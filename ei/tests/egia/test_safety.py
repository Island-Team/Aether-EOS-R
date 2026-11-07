"""Safety gate — pass-through posture with registry resolution and logging.

The estop/release lifecycle moved to the monitor's estop latch
(``tests/egia/test_monitor_bounds.py``); the rate budget moved to the monitor's
``RateWindow``. What stays here: admitted forwarding to the link contract
URI, unknown-primitive rejection, and the egress-failure verdict shape.
"""

from __future__ import annotations

import asyncio
from types import SimpleNamespace

import pytest

from egia.contracts.actions import PrimitiveAck, RejectReason
from egia.core.link import LinkPrimitiveError
from egia.core.missions import MissionQueue
from egia.core.monitor import EstopLatch
from egia.core.perception.admission import Admission
from egia.core.registry import PrimitiveRegistry
from egia.core.safety import SafetyGate
from egia.internal.decision import ActionInvocation, StepContext
from egia.lib.operation_ledger import OperationLedger


class FakeLink:
  """Records invocations and answers with a canned ack or error."""

  def __init__(self, ack: PrimitiveAck | None = None, error: Exception | None = None):
    self.ack = ack
    self.error = error
    self.calls: list[tuple[str, str, dict]] = []

  async def call_primitive(self, name: str, device_id: str, body: dict) -> PrimitiveAck:
    self.calls.append((name, device_id, body))
    if self.error is not None:
      raise self.error
    assert self.ack is not None
    return self.ack


def _ack(accepted: bool = True, reason: str | None = None) -> PrimitiveAck:
  return PrimitiveAck(
    primitive_version="1.0.0",
    primitive="base.velocity",
    accepted=accepted,
    reason=reason,
    reason_code=None if accepted else RejectReason.INVALID_STATE,
    command_id="safety-command-1",
    sequence=7,
    result={"status": "succeeded" if accepted else "rejected"},
  )


class FakeRegistry:
  """ClientRegistry stand-in: resolves every backend to one fake link."""

  def __init__(self, link: FakeLink):
    self._link = link

  def client(self, backend_id: str | None = None):
    return self._link


def _gate(link: FakeLink) -> SafetyGate:
  return SafetyGate(
    PrimitiveRegistry.from_contracts(),
    FakeRegistry(link),  # type: ignore[arg-type]
    EstopLatch(FakeLink()),  # type: ignore[arg-type]
    MissionQueue(4),
    Admission(8),
  )


@pytest.mark.asyncio
async def test_admitted_call_reaches_link_with_contract_uri() -> None:
  link = FakeLink(ack=_ack())
  gate = _gate(link)
  verdict = await gate.execute(
    ActionInvocation(
      action="base.velocity",
      robot_id="default",
      device_id="base-0",
      arguments={
        "linear_x_m_s": 0.2,
        "linear_y_m_s": 0.0,
        "angular_z_rad_s": 0.0,
        "valid_for_ms": 250,
      },
    )
  )
  assert verdict.admitted and verdict.ack is not None
  assert verdict.ack.accepted and verdict.ack.sequence == 7
  assert link.calls == [
    (
      "base.velocity",
      "base-0",
      {
        "linear_x_m_s": 0.2,
        "linear_y_m_s": 0.0,
        "angular_z_rad_s": 0.0,
        "valid_for_ms": 250,
      },
    )
  ]


@pytest.mark.asyncio
async def test_unknown_primitive_is_rejected_without_egress() -> None:
  link = FakeLink(ack=_ack())
  gate = _gate(link)
  verdict = await gate.execute(ActionInvocation(action="hyperdrive.engage"))
  assert not verdict.admitted
  assert verdict.reason == "unknown_primitive"
  assert verdict.ack is None
  assert link.calls == []


@pytest.mark.asyncio
async def test_egress_failure_is_reported_not_raised() -> None:
  link = FakeLink(error=LinkPrimitiveError(400, "invalid body"))
  gate = _gate(link)
  verdict = await gate.execute(
    ActionInvocation(
      action="base.velocity",
      robot_id="default",
      device_id="base-0",
      arguments={"linear_x_m_s": 0.2},
    )
  )
  assert verdict.admitted  # gate passed it; the wire failed
  assert verdict.ack is None
  assert "invalid body" in (verdict.reason or "")


@pytest.mark.asyncio
async def test_new_planner_barrier_waits_for_backend_availability() -> None:
  link = FakeLink(ack=_ack())
  missions = MissionQueue(4)
  mission = await missions.submit("move", source="test")
  assert mission is not None
  missions.mark_running(mission)
  available = asyncio.Event()
  gate = SafetyGate(
    PrimitiveRegistry.from_contracts(),
    FakeRegistry(link),  # type: ignore[arg-type]
    EstopLatch(FakeLink()),  # type: ignore[arg-type]
    missions,
    Admission(8),
    availability_wait=available.wait,
  )
  invocation = ActionInvocation(
    action="base.velocity",
    robot_id="default",
    device_id="base-0",
    arguments={"linear_x_m_s": 0.2},
  )
  task = asyncio.create_task(
    gate.execute(invocation, StepContext(mission_id=mission.id))
  )
  await asyncio.sleep(0)
  assert link.calls == []
  available.set()
  verdict = await task
  assert verdict.admitted is True
  assert len(link.calls) == 1


@pytest.mark.asyncio
async def test_a03_gate_persists_exact_id_before_link_egress(tmp_path) -> None:
  ledger = OperationLedger(tmp_path)

  class DurableLink(FakeLink):
    discovery = SimpleNamespace(robot_id="eos-robot-a")
    active_revision = "a" * 64

    async def call_primitive(self, name, device_id, body, *, command_id=None):
      assert command_id is not None
      pending = ledger.pending()
      assert len(pending) == 1 and pending[0].operation_id == command_id
      self.calls.append((name, device_id, body))
      return _ack()

  link = DurableLink()
  gate = SafetyGate(
    PrimitiveRegistry.from_contracts(),
    FakeRegistry(link),  # type: ignore[arg-type]
    EstopLatch(FakeLink()),  # type: ignore[arg-type]
    MissionQueue(4),
    Admission(8),
    operation_ledger=ledger,
  )
  verdict = await gate.execute(
    ActionInvocation(
      action="base.velocity",
      robot_id="default",
      device_id="base-0",
      arguments={"linear_x_m_s": 0.2},
      request_id="a03-request",
    )
  )
  assert verdict.admitted
  assert ledger.pending() == []
