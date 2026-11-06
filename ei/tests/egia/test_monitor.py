"""Monitor loop — heartbeat escalation, recovery observation, plan stalls."""

from __future__ import annotations

import asyncio
from datetime import UTC, datetime, timedelta

import pytest

from egia.core.monitor import monitor_entry
from egia.core.perception.admission import Admission
from egia.lib.config import MonitorConfig


class FlakyLink:
  """Answers health checks from a mutable boolean."""

  def __init__(self, healthy: bool):
    self.healthy = healthy

  async def health_check(self) -> bool:
    return self.healthy


class RediscoveryLink(FlakyLink):
  def __init__(self) -> None:
    super().__init__(healthy=False)
    self.rediscovery_ready = False

  async def initialize(self, *, force: bool):
    assert force is True
    if not self.rediscovery_ready:
      raise OSError("resources not ready")
    return object()


class RecordingLogger:
  """Captures structured monitor event names without depending on log sinks."""

  def __init__(self) -> None:
    self.events: list[str] = []

  def info(self, event: str, **_fields: object) -> None:
    self.events.append(event)

  def warning(self, event: str, **_fields: object) -> None:
    self.events.append(event)

  def error(self, event: str, **_fields: object) -> None:
    self.events.append(event)


async def _run_loop(link: FlakyLink, queue: Admission, ticks: float) -> None:
  ready = asyncio.Event()
  stop = asyncio.Event()
  config = MonitorConfig(heartbeat_interval_s=0.01, failure_threshold=2)
  task = asyncio.create_task(monitor_entry(link, queue, config, ready, stop))
  await ready.wait()
  await asyncio.sleep(ticks)
  stop.set()
  await task


@pytest.mark.asyncio
async def test_heartbeat_loss_enqueues_fault_observation() -> None:
  queue = Admission(capacity=8)
  await _run_loop(FlakyLink(healthy=False), queue, ticks=0.06)
  fault = queue.latest_by_kind("fault")
  assert fault is not None
  assert fault.source == "monitor"
  assert fault.metadata["reason"] == "heartbeat_lost"


@pytest.mark.asyncio
async def test_heartbeat_outage_logs_only_its_first_failed_probe(monkeypatch) -> None:
  queue = Admission(capacity=8)
  logger = RecordingLogger()
  monkeypatch.setattr("egia.core.monitor.loop.get_logger", lambda _name: logger)
  await _run_loop(FlakyLink(healthy=False), queue, ticks=0.06)

  assert logger.events.count("heartbeat_failed") == 1


@pytest.mark.asyncio
async def test_healthy_backend_produces_no_fault() -> None:
  queue = Admission(capacity=8)
  await _run_loop(FlakyLink(healthy=True), queue, ticks=0.06)
  assert queue.latest_by_kind("fault") is None


@pytest.mark.asyncio
async def test_heartbeat_recovery_enqueues_recovered_fault() -> None:
  queue = Admission(capacity=8)
  link = FlakyLink(healthy=False)
  ready = asyncio.Event()
  stop = asyncio.Event()
  config = MonitorConfig(heartbeat_interval_s=0.01, failure_threshold=2)
  task = asyncio.create_task(monitor_entry(link, queue, config, ready, stop))
  await ready.wait()
  await asyncio.sleep(0.05)  # escalate
  link.healthy = True
  await asyncio.sleep(0.04)
  stop.set()
  await task
  latest = queue.latest_by_kind("fault")
  assert latest is not None
  assert latest.metadata["reason"] == "heartbeat_lost"
  assert latest.metadata["recovered"] == "true"


@pytest.mark.asyncio
async def test_availability_reopens_only_after_successful_rediscovery() -> None:
  queue = Admission(capacity=8)
  link = RediscoveryLink()
  ready = asyncio.Event()
  stop = asyncio.Event()
  availability: list[bool] = []
  task = asyncio.create_task(
    monitor_entry(
      link,
      queue,
      MonitorConfig(heartbeat_interval_s=0.01, failure_threshold=2),
      ready,
      stop,
      availability_changed=availability.append,
    )
  )
  await ready.wait()
  await asyncio.sleep(0.025)
  assert availability and availability[-1] is False
  link.healthy = True
  await asyncio.sleep(0.025)
  assert availability[-1] is False
  link.rediscovery_ready = True
  await asyncio.sleep(0.025)
  assert availability[-1] is True
  stop.set()
  await task


async def _run_stall_loop(
  queue: Admission,
  *,
  mission_id: str | None = "m-1",
  stall_timeout_s: float = 0.03,
  annotations: list[tuple[str, str, str]] | None = None,
  ticks: float = 0.1,
  started_at: datetime | None = None,
) -> None:
  ready = asyncio.Event()
  stop = asyncio.Event()
  seen = annotations if annotations is not None else []
  config = MonitorConfig(
    heartbeat_interval_s=0.01, failure_threshold=2, stall_timeout_s=stall_timeout_s
  )
  task = asyncio.create_task(
    monitor_entry(
      FlakyLink(healthy=True),
      queue,
      config,
      ready,
      stop,
      active_mission=lambda: mission_id,
      annotate=lambda s, e, d: seen.append((s, e, d)),
      mission_started_at=lambda: started_at,
    )
  )
  await ready.wait()
  await asyncio.sleep(ticks)
  stop.set()
  await task


def _stall_faults(queue: Admission):
  return [
    r
    for r in queue.records()
    if r.kind == "fault" and r.metadata.get("reason") == "step_stall"
  ]


@pytest.mark.asyncio
async def test_stall_detected_with_running_mission_and_no_primitive_result() -> None:
  queue = Admission(capacity=16)
  annotations: list[tuple[str, str, str]] = []
  await _run_stall_loop(queue, annotations=annotations)
  stalls = _stall_faults(queue)
  assert len(stalls) == 1  # once per stall episode
  assert stalls[0].source == "monitor"
  assert stalls[0].metadata["mission_id"] == "m-1"
  assert len(annotations) == 1
  source, event, detail = annotations[0]
  assert source == "monitor"
  assert event == "step_stall"
  assert "no primitive result" in detail


@pytest.mark.asyncio
async def test_no_running_mission_means_no_stall() -> None:
  queue = Admission(capacity=16)
  await _run_stall_loop(queue, mission_id=None)
  assert _stall_faults(queue) == []


@pytest.mark.asyncio
async def test_recently_started_mission_does_not_stall() -> None:
  """First-step inference is not a stall: the window floors at mission start."""
  queue = Admission(capacity=16)
  # No primitive results at all — the pre-fix false positive — but the
  # mission started 0.1s ago against a 0.03s... no: started within the
  # window, so no stall despite zero primitive activity.
  await _run_stall_loop(queue, started_at=datetime.now(UTC), stall_timeout_s=1.0)
  assert _stall_faults(queue) == []


@pytest.mark.asyncio
async def test_stalled_mission_past_start_window_still_fires() -> None:
  queue = Admission(capacity=16)
  started = datetime.now(UTC) - timedelta(seconds=10)
  await _run_stall_loop(queue, started_at=started, stall_timeout_s=0.03)
  assert len(_stall_faults(queue)) == 1


@pytest.mark.asyncio
async def test_fresh_primitive_observation_rearms_stall_detection() -> None:
  queue = Admission(capacity=16)
  ready = asyncio.Event()
  stop = asyncio.Event()
  config = MonitorConfig(
    heartbeat_interval_s=0.01, failure_threshold=2, stall_timeout_s=0.03
  )
  task = asyncio.create_task(
    monitor_entry(
      FlakyLink(healthy=True),
      queue,
      config,
      ready,
      stop,
      active_mission=lambda: "m-1",
    )
  )
  await ready.wait()
  await asyncio.sleep(0.03)  # first stall fires
  assert len(_stall_faults(queue)) == 1
  await queue.enqueue(
    kind="result", source="decision", text="base.velocity: accepted", metadata={}
  )
  await asyncio.sleep(0.06)  # fresh activity re-arms, then stalls again
  stop.set()
  await task
  assert len(_stall_faults(queue)) == 2
