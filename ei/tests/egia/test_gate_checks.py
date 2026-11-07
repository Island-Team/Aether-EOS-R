"""Safety gate — the mission-consistency check pipeline in fixed order."""

from __future__ import annotations

import pytest

from egia.contracts.actions import PrimitiveAck
from egia.core.link import LinkPrimitiveError
from egia.core.missions import MissionQueue
from egia.core.monitor import EstopLatch
from egia.core.perception.admission import Admission
from egia.core.registry import PrimitiveRegistry
from egia.core.safety import SafetyGate
from egia.internal.decision import ActionInvocation, StepContext


class FakeLink:
  """Records invocations and answers with a canned ack or error."""

  def __init__(self, error: Exception | None = None):
    self.ack = PrimitiveAck(
      primitive_version="1.0.0",
      primitive="base.velocity",
      accepted=True,
      command_id="gate-command-1",
      sequence=7,
      result={"status": "succeeded"},
    )
    self.error = error
    self.calls: list[tuple[str, str, dict]] = []

  async def call_primitive(self, name: str, device_id: str, body: dict) -> PrimitiveAck:
    self.calls.append((name, device_id, body))
    if self.error is not None:
      raise self.error
    return self.ack


class FakeRegistry:
  """ClientRegistry stand-in: resolves every backend to one fake link."""

  def __init__(self, link: FakeLink):
    self._link = link

  def client(self, backend_id: str | None = None):
    return self._link


def _gate(
  link: FakeLink,
  *,
  latch: EstopLatch | None = None,
  missions: MissionQueue | None = None,
  registry: PrimitiveRegistry | None = None,
  rate_admit=None,
) -> SafetyGate:
  latch = latch or EstopLatch(link)  # type: ignore[arg-type]
  missions = missions if missions is not None else MissionQueue(4)
  return SafetyGate(
    registry or PrimitiveRegistry.from_contracts(),
    FakeRegistry(link),  # type: ignore[arg-type]
    latch,
    missions,
    Admission(8),
    rate_admit=rate_admit,
  )


async def _run_mission(missions: MissionQueue) -> str:
  record = await missions.submit("move", source="test")
  assert record is not None
  missions.mark_running(record)
  return record.id


def _move() -> ActionInvocation:
  return ActionInvocation(
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


def _planner(mission_id: str = "m-1", step: int = 1) -> StepContext:
  return StepContext(mission_id=mission_id, step_index=step)


@pytest.mark.asyncio
async def test_estop_latch_rejects_before_any_other_check() -> None:
  gate_link = FakeLink()
  latch = EstopLatch(FakeLink())  # type: ignore[arg-type]
  await latch.engage("test")
  gate = _gate(gate_link, latch=latch)
  verdict = await gate.execute(ActionInvocation(action="nope.move"), _planner())
  assert not verdict.admitted
  assert verdict.reason == "estop_engaged"
  assert gate_link.calls == []  # no egress at all through the gate's link


@pytest.mark.asyncio
async def test_unknown_primitive_rejects_without_egress() -> None:
  link = FakeLink()
  gate = _gate(link)
  verdict = await gate.execute(ActionInvocation(action="hyperdrive.engage"))
  assert not verdict.admitted
  assert verdict.reason == "unknown_primitive"
  assert link.calls == []


@pytest.mark.asyncio
async def test_planner_call_with_running_mission_admits() -> None:
  link = FakeLink()
  missions = MissionQueue(4)
  mission_id = await _run_mission(missions)
  gate = _gate(link, missions=missions)
  verdict = await gate.execute(_move(), _planner(mission_id))
  assert verdict.admitted and verdict.ack is not None
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
async def test_planner_mission_mismatch_is_scope_violation() -> None:
  link = FakeLink()
  missions = MissionQueue(4)
  running_id = await _run_mission(missions)
  gate = _gate(link, missions=missions)
  verdict = await gate.execute(_move(), _planner(f"not-{running_id}"))
  assert not verdict.admitted
  assert verdict.reason is not None
  assert verdict.reason.startswith("scope_violation:")
  assert "not the running mission" in verdict.reason
  assert link.calls == []


@pytest.mark.asyncio
async def test_planner_call_with_no_running_mission_is_scope_violation() -> None:
  link = FakeLink()
  gate = _gate(link)
  verdict = await gate.execute(_move(), _planner())
  assert not verdict.admitted
  assert verdict.reason is not None
  assert verdict.reason.startswith("scope_violation: no running mission")
  assert link.calls == []


@pytest.mark.asyncio
async def test_planner_call_on_operator_tier_primitive_is_scope_violation() -> None:
  link = FakeLink()
  missions = MissionQueue(4)
  mission_id = await _run_mission(missions)
  gate = _gate(
    link,
    missions=missions,
    registry=PrimitiveRegistry.from_contracts(expose_operator=True),
  )
  verdict = await gate.execute(
    ActionInvocation(
      action="arm.movej",
      robot_id="default",
      device_id="arm-0",
      arguments={"joint_positions_rad": [0.5, 0.5]},
    ),
    _planner(mission_id),
  )
  assert not verdict.admitted
  assert verdict.reason is not None
  assert verdict.reason.startswith("scope_violation:")
  assert "operator-tier" in verdict.reason
  assert link.calls == []


@pytest.mark.asyncio
async def test_operator_origin_admits_any_resolvable_primitive_without_mission() -> (
  None
):
  link = FakeLink()
  gate = _gate(link, registry=PrimitiveRegistry.from_contracts(expose_operator=True))
  for action, device_id, arguments in [
    ("base.velocity", "base-0", _move().arguments),
    ("arm.movej", "arm-0", {"joint_positions_rad": [0.5, 0.5]}),
  ]:
    verdict = await gate.execute(
      ActionInvocation(
        action=action,
        robot_id="default",
        device_id=device_id,
        arguments=arguments,
      ),
      StepContext(origin="operator"),
    )
    assert verdict.admitted, action
  assert len(link.calls) == 2


@pytest.mark.asyncio
async def test_context_none_admits_like_operator() -> None:
  link = FakeLink()
  gate = _gate(link)
  verdict = await gate.execute(_move())
  assert verdict.admitted
  assert len(link.calls) == 1


@pytest.mark.asyncio
async def test_rate_admit_hook_consulted_last_and_rejects() -> None:
  link = FakeLink()
  budget = {"remaining": 1}

  def rate_admit() -> bool:
    budget["remaining"] -= 1
    return budget["remaining"] >= 0

  gate = _gate(link, rate_admit=rate_admit)
  assert (await gate.execute(_move())).admitted
  verdict = await gate.execute(_move())
  assert not verdict.admitted
  assert verdict.reason == "rate_limited"
  assert len(link.calls) == 1


@pytest.mark.asyncio
async def test_scope_violation_beats_rate_limit() -> None:
  link = FakeLink()
  gate = _gate(link, rate_admit=lambda: False)
  verdict = await gate.execute(_move(), _planner())  # no running mission
  assert not verdict.admitted
  assert verdict.reason is not None
  assert verdict.reason.startswith("scope_violation:")
  assert gate.rejected_count == 1


@pytest.mark.asyncio
async def test_estop_beats_unknown_primitive() -> None:
  link = FakeLink()
  latch = EstopLatch(FakeLink())
  await latch.engage("test")
  gate = _gate(link, latch=latch)
  verdict = await gate.execute(ActionInvocation(action="hyperdrive.engage"))
  assert verdict.reason == "estop_engaged"


@pytest.mark.asyncio
async def test_schema_violation_surfaces_as_admitted_egress_failed() -> None:
  """Contract validation lives at the wire now: the gate admits, the link 400s."""
  link = FakeLink(
    error=LinkPrimitiveError(400, "direction: Input should be 'front'...")
  )
  gate = _gate(link)
  verdict = await gate.execute(
    ActionInvocation(
      action="base.velocity",
      robot_id="default",
      device_id="base-0",
      arguments={"linear_x_m_s": "fast"},
    )
  )
  assert verdict.admitted  # gate policy passed; the wire rejected the body
  assert verdict.ack is None
  assert verdict.reason is not None
  assert verdict.reason.startswith("egress_failed:")
  assert len(link.calls) == 1
