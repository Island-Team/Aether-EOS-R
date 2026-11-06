"""Retained multi-backend routing and fleet emergency-egress behavior."""

from __future__ import annotations

from types import SimpleNamespace

import pytest

from egia.contracts.actions import PrimitiveAck
from egia.core.link import ClientRegistry
from egia.core.missions import MissionQueue
from egia.core.monitor import EstopLatch
from egia.core.registry import PrimitiveRegistry
from egia.core.safety import SafetyGate
from egia.internal.decision import ActionInvocation


class FakeLink:
  """Record primitive calls and optionally fail after receiving one."""

  def __init__(self, backend: str, error: Exception | None = None) -> None:
    self.backend = backend
    self.error = error
    self.robot_id = f"runtime-{backend}-old"
    self.started_at = f"2026-09-23T00:00:0{0 if backend == 'robot-a' else 1}Z"
    self.calls: list[tuple[str, str, dict]] = []

  @property
  def discovery(self) -> SimpleNamespace:
    return SimpleNamespace(robot_id=self.robot_id, started_at=self.started_at)

  async def initialize(self, *, force: bool = False) -> SimpleNamespace:
    del force
    return self.discovery

  def motion_device_ids(self) -> tuple[str, ...]:
    return ("base-0",)

  async def call_primitive(self, name: str, device_id: str, body: dict) -> PrimitiveAck:
    self.calls.append((name, device_id, body))
    if self.error is not None:
      raise self.error
    return PrimitiveAck(
      primitive_version="1.0.0",
      primitive=name,
      accepted=True,
      command_id=f"{self.backend}-command-{len(self.calls)}",
      sequence=len(self.calls),
      result={"status": "succeeded"},
    )


def _links(
  *, second_error: Exception | None = None
) -> tuple[ClientRegistry, FakeLink, FakeLink]:
  first = FakeLink("robot-a")
  second = FakeLink("robot-b", second_error)
  links = ClientRegistry()
  links.register("robot-a", first)  # type: ignore[arg-type]
  links.register("robot-b", second)  # type: ignore[arg-type]
  return links, first, second


def _gate() -> tuple[SafetyGate, FakeLink, FakeLink]:
  links, first, second = _links()
  gate = SafetyGate(
    PrimitiveRegistry.from_contracts(["robot-a", "robot-b"]),
    links,
    EstopLatch(FakeLink("monitor")),  # type: ignore[arg-type]
    MissionQueue(4),
  )
  return gate, first, second


@pytest.mark.asyncio
async def test_addressed_primitive_egresses_only_to_selected_robot() -> None:
  gate, first, second = _gate()

  verdict = await gate.execute(
    ActionInvocation(
      action="base.velocity",
      robot_id="robot-b",
      device_id="base-0",
      arguments={"linear_x_m_s": 0.1},
    )
  )

  assert verdict.admitted is True
  assert verdict.ack is not None
  assert first.calls == []
  assert second.calls == [("base.velocity", "base-0", {"linear_x_m_s": 0.1})]


@pytest.mark.asyncio
async def test_unaddressed_primitive_is_rejected_before_egress() -> None:
  gate, first, second = _gate()

  verdict = await gate.execute(ActionInvocation(action="base.velocity"))

  assert verdict.admitted is False
  assert verdict.reason == "ambiguous_primitive: specify robot_id and device_id"
  assert first.calls == []
  assert second.calls == []


@pytest.mark.asyncio
async def test_unknown_explicit_target_is_rejected_before_egress() -> None:
  gate, first, second = _gate()

  verdict = await gate.execute(
    ActionInvocation(action="base.velocity", robot_id="missing", device_id="base-0")
  )

  assert verdict.admitted is False
  assert verdict.reason == "unknown_primitive"
  assert first.calls == []
  assert second.calls == []


@pytest.mark.asyncio
async def test_estop_egresses_to_every_robot_in_the_fleet() -> None:
  links, first, second = _links()
  latch = EstopLatch(links)

  await latch.engage("bound violation")

  assert latch.engaged is True
  assert first.calls == [("safety.estop", "", {})]
  assert second.calls == [("safety.estop", "", {})]


@pytest.mark.asyncio
async def test_estop_continues_after_one_robot_egress_fails() -> None:
  links, first, second = _links(second_error=RuntimeError("gateway down"))
  latch = EstopLatch(links)

  await latch.engage("audit escalation")

  assert latch.engaged is True
  assert first.calls == [("safety.estop", "", {})]
  assert second.calls == [("safety.estop", "", {})]


@pytest.mark.asyncio
async def test_latch_stays_engaged_after_every_runtime_start_time_changes() -> None:
  links, first, second = _links()
  latch = EstopLatch(links)
  await latch.engage("test")

  first.started_at = "2026-09-23T00:01:00Z"
  assert latch.reconcile_restarts() is False
  assert latch.engaged is True

  second.started_at = "2026-09-23T00:01:01Z"
  assert latch.reconcile_restarts() is False
  assert latch.engaged is True
