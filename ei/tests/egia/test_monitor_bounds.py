"""Monitor low-level safety — estop latch, rate window, physical bounds."""

from __future__ import annotations

from datetime import UTC, datetime
from types import SimpleNamespace

import pytest
from structlog.testing import capture_logs

from egia.contracts.actions import PrimitiveAck, RejectReason
from egia.contracts.perception.pose import PlanarPose, PlanarVelocity
from egia.contracts.perception.world.scene import RobotKinematics, SceneSnapshot
from egia.core.monitor import (
  BoundWatchdog,
  EstopLatch,
  PhysicalBounds,
  RateWindow,
)
from egia.core.perception.admission import Admission
from egia.lib.config import MonitorConfig


class FakeClient:
  """Records direct emergency egress; optionally fails every call."""

  def __init__(
    self, error: Exception | None = None, rejected_device: str | None = None
  ):
    self.backend = "default"
    self.error = error
    self.rejected_device = rejected_device
    self.robot_id = "01a077f7-b5b8-7b4e-8057-ca688b8ff48f"
    self.started_at = "2026-09-23T00:00:00Z"
    self.calls: list[tuple[str, str, dict]] = []

  @property
  def discovery(self):
    return SimpleNamespace(robot_id=self.robot_id, started_at=self.started_at)

  async def initialize(self, *, force: bool = False):
    del force
    return self.discovery

  def motion_device_ids(self) -> tuple[str, ...]:
    return ("base-0", "arm-0", "gimbal-0")

  async def call_primitive(self, name: str, device_id: str, body: dict) -> PrimitiveAck:
    self.calls.append((name, device_id, body))
    if self.error is not None:
      raise self.error
    if device_id == self.rejected_device:
      return PrimitiveAck(
        primitive_version="1.0.0",
        primitive=name,
        accepted=False,
        reason="motion stop was not released",
        reason_code=RejectReason.INVALID_STATE,
        command_id=f"stop-{device_id}",
        sequence=0,
        result={"status": "rejected"},
      )
    return PrimitiveAck(
      primitive_version="1.0.0",
      primitive=name,
      accepted=True,
      command_id=f"stop-{device_id or 'robot'}",
      sequence=0,
      result={"status": "succeeded"},
    )


def _scene(x_m_s: float = 0.0, yaw_rad_s: float = 0.0) -> SceneSnapshot:
  return SceneSnapshot(
    sequence=0,
    timestamp=datetime.now(UTC),
    source_timestamp_ns=None,
    frame_id="map",
    robot=RobotKinematics(
      pose=PlanarPose(x_m=0.0, y_m=0.0, yaw_rad=0.0),
      velocity=PlanarVelocity(x_m_s=x_m_s, y_m_s=0.0, yaw_rad_s=yaw_rad_s),
    ),
  )


# --- EstopLatch -----------------------------------------------------------


@pytest.mark.asyncio
async def test_engage_sets_latch_then_egresses_safety_estop_direct() -> None:
  client = FakeClient()
  latch = EstopLatch(client)  # type: ignore[arg-type]
  await latch.engage("audit: unrecovered fault")
  assert latch.engaged
  assert client.calls == [("safety.estop", "", {})]


@pytest.mark.asyncio
async def test_engage_egress_failure_leaves_latch_engaged_and_never_raises() -> None:
  client = FakeClient(error=OSError("gRPC connect failed"))
  latch = EstopLatch(client)  # type: ignore[arg-type]
  await latch.engage("bound_violation")  # must not raise
  assert latch.engaged
  assert client.calls == [("safety.estop", "", {})]


@pytest.mark.asyncio
async def test_latch_does_not_clear_without_a_runtime_restart() -> None:
  client = FakeClient()
  latch = EstopLatch(client)  # type: ignore[arg-type]
  await latch.engage("test")
  assert latch.reconcile_restarts() is False
  assert latch.engaged


@pytest.mark.asyncio
async def test_new_runtime_start_time_does_not_clear_the_latch() -> None:
  client = FakeClient()
  latch = EstopLatch(client)  # type: ignore[arg-type]
  await latch.engage("test")
  client.started_at = "2026-09-23T00:01:00Z"
  assert latch.reconcile_restarts() is False
  assert latch.engaged


@pytest.mark.asyncio
async def test_motion_stop_egresses_direct() -> None:
  client = FakeClient()
  latch = EstopLatch(client)  # type: ignore[arg-type]
  await latch.motion_stop()
  assert client.calls == [
    ("motion.stop", "base-0", {}),
    ("motion.stop", "arm-0", {}),
    ("motion.stop", "gimbal-0", {}),
  ]
  assert not latch.engaged  # below estop: no latch involvement


@pytest.mark.asyncio
async def test_motion_stop_logs_rejected_ack_and_continues_fanout() -> None:
  client = FakeClient(rejected_device="arm-0")
  latch = EstopLatch(client)  # type: ignore[arg-type]

  with capture_logs() as logs:
    await latch.motion_stop()

  failures = [entry for entry in logs if entry["event"] == "emergency_egress_failed"]
  assert len(failures) == 1
  assert failures[0]["device_id"] == "arm-0"
  assert failures[0]["primitive"] == "motion.stop"
  assert "rejected.invalid_state" in failures[0]["reason"]
  assert "motion stop was not released" in failures[0]["reason"]
  assert client.calls[-1] == ("motion.stop", "gimbal-0", {})


# --- RateWindow -----------------------------------------------------------


def test_rate_window_admits_under_budget_and_exhausts() -> None:
  now = {"t": 100.0}
  window = RateWindow(max_calls_per_minute=2, clock=lambda: now["t"])
  assert window.admit()
  assert window.admit()
  assert not window.admit()


def test_rate_window_prunes_expired_timestamps() -> None:
  now = {"t": 100.0}
  window = RateWindow(max_calls_per_minute=1, clock=lambda: now["t"])
  assert window.admit()
  assert not window.admit()
  now["t"] += 60.0  # exactly 60s later: the slot expired
  assert window.admit()


def test_rate_window_appends_only_on_admit() -> None:
  now = {"t": 100.0}
  window = RateWindow(max_calls_per_minute=1, clock=lambda: now["t"])
  assert window.admit()
  assert not window.admit()  # rejected call consumed nothing
  now["t"] += 60.0
  assert window.admit()  # still exactly one slot per window


# --- Physical bounds ------------------------------------------------------


def test_check_velocity_within_bounds_returns_none() -> None:
  bounds = PhysicalBounds()
  assert bounds.check_velocity(_scene(x_m_s=1.9).robot.velocity) is None
  assert bounds.check_velocity(_scene(yaw_rad_s=3.0).robot.velocity) is None


def test_check_velocity_over_speed_reports_violation() -> None:
  bounds = PhysicalBounds()
  over = bounds.check_velocity(_scene(x_m_s=2.5).robot.velocity)
  assert over is not None and over.quantity == "velocity"
  assert over.value > 2.0 and over.limit == 2.0


def test_check_velocity_over_yaw_rate_reports_violation() -> None:
  bounds = PhysicalBounds()
  over = bounds.check_velocity(_scene(yaw_rad_s=3.5).robot.velocity)
  assert over is not None and over.quantity == "yaw_rate"
  assert over.limit == bounds.omega_max_rad_s


# --- BoundWatchdog --------------------------------------------------------


def _watchdog(ticks: int = 3) -> tuple[BoundWatchdog, Admission, FakeClient]:
  queue = Admission(capacity=16)
  client = FakeClient()
  latch = EstopLatch(client)  # type: ignore[arg-type]
  return (
    BoundWatchdog(
      PhysicalBounds(v_max_m_s=2.0, omega_max_rad_s=3.14159, escalation_ticks=ticks),
      queue,
      latch,
    ),
    queue,
    client,
  )


@pytest.mark.asyncio
async def test_violating_frame_enqueues_fault_immediately_with_metadata() -> None:
  watchdog, queue, _ = _watchdog()
  await watchdog.check_frame(None, _scene(x_m_s=3.0))
  fault = queue.latest_by_kind("fault")
  assert fault is not None
  assert fault.source == "monitor"
  assert fault.metadata["reason"] == "bound_violation"
  assert fault.metadata["quantity"] == "velocity"
  assert float(fault.metadata["limit"]) == 2.0


@pytest.mark.asyncio
async def test_healthy_frame_enqueues_nothing() -> None:
  watchdog, queue, _ = _watchdog()
  await watchdog.check_frame(None, _scene(x_m_s=1.0))
  assert queue.latest_by_kind("fault") is None


@pytest.mark.asyncio
async def test_streak_below_threshold_does_not_engage_latch() -> None:
  watchdog, _, client = _watchdog(ticks=3)
  await watchdog.check_frame(None, _scene(x_m_s=3.0))
  await watchdog.check_frame(None, _scene(x_m_s=3.0))
  assert client.calls == []


@pytest.mark.asyncio
async def test_streak_reaching_threshold_engages_latch_once() -> None:
  watchdog, _, client = _watchdog(ticks=3)
  for _ in range(6):
    await watchdog.check_frame(None, _scene(x_m_s=3.0))
  assert client.calls == [("safety.estop", "", {})]  # escalated exactly once


@pytest.mark.asyncio
async def test_healthy_frame_resets_streak_and_rearms_escalation() -> None:
  watchdog, _, client = _watchdog(ticks=3)
  await watchdog.check_frame(None, _scene(x_m_s=3.0))
  await watchdog.check_frame(None, _scene(x_m_s=3.0))
  await watchdog.check_frame(None, _scene(x_m_s=1.0))  # healthy: streak reset
  await watchdog.check_frame(None, _scene(x_m_s=3.0))
  await watchdog.check_frame(None, _scene(x_m_s=3.0))
  assert client.calls == []  # never reached 3 consecutive violations


@pytest.mark.asyncio
async def test_non_base_snapshots_are_ignored() -> None:
  from pydantic import BaseModel

  class OtherSnapshot(BaseModel):
    pass

  watchdog, queue, _ = _watchdog()
  await watchdog.check_frame(None, OtherSnapshot())  # type: ignore[arg-type]
  await watchdog.check_frame(
    None, _scene()
  )  # healthy scene frame enqueues nothing either
  assert queue.latest_by_kind("fault") is None


def test_monitor_config_carries_bound_constants() -> None:
  config = MonitorConfig()
  assert config.v_max_m_s == 2.0
  assert config.omega_max_rad_s == 3.14159
  assert config.bound_escalation_ticks == 5
