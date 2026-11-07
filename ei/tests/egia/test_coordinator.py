"""Coordinator — intake → profile → auction → per-executor fan-out, end to end."""

from __future__ import annotations

import asyncio
from pathlib import Path

from egia.api.mission import MissionState
from egia.contracts.actions import PrimitiveAck
from egia.core.decision.coordination.bidders import BidderProfile
from egia.core.decision.coordination.coordinator import Coordinator
from egia.core.decision.profile_writer import ProfileWriter
from egia.core.decision.profiles import ProfileStore
from egia.core.missions import MissionQueue
from egia.core.registry import PrimitiveRegistry
from egia.internal.decision import ActionInvocation, GateVerdict, StepContext
from egia.lib.config import (
  AgentPromptConfig,
  CoordinationConfig,
  PromptInstructionSection,
  PromptSectionConfig,
)
from egia.lib.vlm import ChatMessage, ModelResponse

_PROFILE_TOML = """name = "job-x"
version = 1
order = "pick and drop"
intro = "Two-step pick and drop."

[[steps]]
id = "pick"
primitive = "gripper.motion"
arguments = { action = "close" }

[[steps]]
id = "drop"
primitive = "gripper.motion"
arguments = { action = "open" }
after = ["pick"]
zone = "cell-a"
"""


def _writer_prompts() -> AgentPromptConfig:
  return AgentPromptConfig(
    sections=[PromptInstructionSection(name="instruction", text="Write TOML.")],
    dynamic_sections=[
      PromptSectionConfig(
        kind="profile_primitives",
        format="Primitives:\n{data}",
        empty="Primitives: unavailable",
        priority=400,
      ),
      PromptSectionConfig(
        kind="profile_existing",
        format="Existing profiles:\n{data}",
        empty="Existing profiles: unavailable",
        priority=600,
      ),
      PromptSectionConfig(
        kind="profile_mission",
        format="Mission: {data}",
        empty="Mission: unavailable",
        priority=700,
      ),
      PromptSectionConfig(
        kind="profile_feedback",
        format="Previous validation: {data}",
        empty="Previous validation: none",
        priority=1000,
      ),
    ],
  )


class ScriptedModel:
  def __init__(self, replies: list[str]) -> None:
    self._replies = list(replies)

  async def complete(self, messages: list[ChatMessage], **_kwargs) -> ModelResponse:
    return ModelResponse(text=self._replies.pop(0))


class RecordingGate:
  """Safety-gate fake: accepts everything, or rejects named primitives."""

  def __init__(
    self, reject_primitives: set[str] | None = None, *, delay_s: float = 0.0
  ) -> None:
    self.reject_primitives = reject_primitives or set()
    self.delay_s = delay_s
    self.calls: list[tuple[ActionInvocation, StepContext]] = []
    self.running: set[str] = set()

  async def execute(
    self, invocation: ActionInvocation, context: StepContext | None = None
  ) -> GateVerdict:
    self.calls.append((invocation, context))  # type: ignore[arg-type]
    if self.delay_s:
      await asyncio.sleep(self.delay_s)
    if invocation.action in self.reject_primitives:
      return GateVerdict(
        admitted=True,
        ack=PrimitiveAck(
          primitive_version="1.0.0",
          primitive=invocation.action,
          accepted=False,
          reason="gripper controller is not calibrated",
          reason_code="rejected.invalid_state",
          command_id=f"coordinator-command-{len(self.calls)}",
          sequence=len(self.calls),
          result={"status": "rejected"},
        ),
      )
    return GateVerdict(
      admitted=True,
      ack=PrimitiveAck(
        primitive_version="1.0.0",
        primitive=invocation.action,
        accepted=True,
        command_id=f"coordinator-command-{len(self.calls)}",
        sequence=len(self.calls),
        result={"status": "succeeded"},
      ),
    )


def _bidders(count: int = 2) -> list[BidderProfile]:
  return [
    BidderProfile(
      backend="line",
      part_id=f"arm-{index + 1}",
      primitives={"gripper.motion"},
    )
    for index in range(count)
  ]


def _coordinator(
  tmp_path: Path,
  gate: RecordingGate,
  bidders: list[BidderProfile],
  *,
  profile_toml: str = _PROFILE_TOML,
) -> tuple[Coordinator, MissionQueue]:
  missions = MissionQueue(4)
  writer = ProfileWriter(
    ScriptedModel([profile_toml]),
    PrimitiveRegistry.from_contracts(["line"]),
    ProfileStore(tmp_path),
    prompts=_writer_prompts(),
    max_attempts=1,
  )
  coordinator = Coordinator(
    missions,
    gate,  # type: ignore[arg-type]
    writer,
    bidders,
    config=CoordinationConfig(),
    step_timeout_s=2.0,
  )
  return coordinator, missions


async def _run_until_finished(
  coordinator: Coordinator, missions: MissionQueue, mission_id: str, stop: asyncio.Event
) -> MissionState:
  ready = asyncio.Event()
  task = asyncio.create_task(coordinator.run(ready, stop))
  await asyncio.wait_for(ready.wait(), timeout=2.0)
  try:
    for _ in range(200):
      record = next((r for r in missions.records() if r.id == mission_id), None)
      if record is not None and record.state in (
        MissionState.SUCCEEDED,
        MissionState.FAILED,
      ):
        return record.state
      await asyncio.sleep(0.02)
    raise AssertionError("mission did not settle within the polling budget")
  finally:
    stop.set()
    await asyncio.wait_for(task, timeout=2.0)


async def test_mission_runs_through_market_to_success(tmp_path: Path) -> None:
  gate = RecordingGate()
  coordinator, missions = _coordinator(tmp_path, gate, _bidders())
  stop = asyncio.Event()
  mission = await missions.submit("pick and drop", source="test")
  assert mission is not None
  state = await _run_until_finished(coordinator, missions, mission.id, stop)

  assert state is MissionState.SUCCEEDED
  # Two steps, both through the gate with the winning bidder's targeting.
  actions = [invocation.action for invocation, _ in gate.calls]
  assert actions == ["gripper.motion", "gripper.motion"]
  pick_call = gate.calls[0][0]
  assert pick_call.arguments == {"action": "close", "part_id": "arm-1"}
  assert pick_call.robot_id == "line"
  # Every planner-origin call carried the job's mission id (gate scope).
  assert all(context.mission_id == mission.id for _, context in gate.calls)
  # The board drained and the mission scope emptied.
  assert coordinator.running_mission_ids() == set()


async def test_backend_rejection_fails_the_mission(tmp_path: Path) -> None:
  gate = RecordingGate(reject_primitives={"gripper.motion"})
  coordinator, missions = _coordinator(tmp_path, gate, _bidders())
  stop = asyncio.Event()
  mission = await missions.submit("pick and drop", source="test")
  assert mission is not None
  state = await _run_until_finished(coordinator, missions, mission.id, stop)
  assert state is MissionState.FAILED
  record = next(r for r in missions.records() if r.id == mission.id)
  assert "pick" in (record.error or "")
  assert "gripper controller is not calibrated" in (record.error or "")


async def test_parallel_steps_share_zone_exclusively(tmp_path: Path) -> None:
  profile_toml = """name = "parallel-x"
version = 1
order = "two picks in one cell"

[[steps]]
id = "p1"
primitive = "gripper.motion"
zone = "cell-a"
arguments = { object_id = "o1" }

[[steps]]
id = "p2"
primitive = "gripper.motion"
zone = "cell-a"
arguments = { object_id = "o2" }
"""
  gate = RecordingGate()
  coordinator, missions = _coordinator(
    tmp_path, gate, _bidders(), profile_toml=profile_toml
  )
  stop = asyncio.Event()
  mission = await missions.submit("two picks in one cell", source="test")
  assert mission is not None
  state = await _run_until_finished(coordinator, missions, mission.id, stop)

  assert state is MissionState.SUCCEEDED
  # Both steps ran, but never concurrently: the shared zone serialized them
  # across the two executors (busy-set re-opens only after the lease frees).
  assert len(gate.calls) == 2


async def test_running_mission_scope_tracks_live_jobs(tmp_path: Path) -> None:
  # A slow gate keeps the job observably mid-flight, proving the scope set
  # tracks live board jobs (the gate's coordination seam).
  gate = RecordingGate(delay_s=0.2)
  coordinator, missions = _coordinator(tmp_path, gate, _bidders())
  assert coordinator.running_mission_ids() == set()
  stop = asyncio.Event()
  mission = await missions.submit("pick and drop", source="test")
  assert mission is not None
  ready = asyncio.Event()
  task = asyncio.create_task(coordinator.run(ready, stop))
  await asyncio.wait_for(ready.wait(), timeout=2.0)
  try:
    for _ in range(100):
      if mission.id in coordinator.running_mission_ids():
        break
      await asyncio.sleep(0.02)
    assert mission.id in coordinator.running_mission_ids()
    for _ in range(200):
      record = next((r for r in missions.records() if r.id == mission.id), None)
      if record is not None and record.state is MissionState.SUCCEEDED:
        break
      await asyncio.sleep(0.02)
    assert coordinator.running_mission_ids() == set()
  finally:
    stop.set()
    await asyncio.wait_for(task, timeout=2.0)


# ------------------------------------------------------------ re-discovery


def _fallback_bidder(backend: str) -> BidderProfile:
  return BidderProfile(
    backend=backend, part_id="default", primitives={"gripper.motion"}
  )


async def _apply_and_drain(coordinator: Coordinator, fresh, stop) -> None:
  await coordinator._apply_discovery(fresh, stop)
  await asyncio.sleep(0)  # let freshly started executors reach their queues


async def test_rediscovery_replaces_fallback_with_real_bidders(tmp_path: Path) -> None:
  """A backend down at launch degrades to one coarse bidder; recovery
  folds the real per-arm bidders in and retires the fallback."""
  coordinator, _missions = _coordinator(
    tmp_path, RecordingGate(), [_fallback_bidder("line")]
  )
  stop = asyncio.Event()
  try:
    fresh = [
      BidderProfile(
        backend="line",
        part_id="arm-1",
        primitives={"gripper.motion"},
        stations={"station-1"},
      ),
      BidderProfile(
        backend="line",
        part_id="arm-2",
        primitives={"gripper.motion"},
        stations={"station-2"},
      ),
    ]
    await _apply_and_drain(coordinator, fresh, stop)
    by_id = coordinator._by_id
    assert "line:default" in by_id and not by_id["line:default"].healthy
    assert by_id["line:arm-1"].healthy and by_id["line:arm-2"].healthy
    # The new bidders got their own serial-drain executors.
    assert {"line:arm-1", "line:arm-2"} <= set(coordinator._executor_tasks)
  finally:
    stop.set()
    for task in coordinator._executor_tasks.values():
      task.cancel()
    await asyncio.gather(*coordinator._executor_tasks.values(), return_exceptions=True)


async def test_rediscovery_marks_vanished_bidders_unhealthy(tmp_path: Path) -> None:
  """A robot absent from the fresh advertisement stops bidding — its
  object stays (never killed mid-queue) and recovers if it returns."""
  coordinator, _missions = _coordinator(tmp_path, RecordingGate(), _bidders(2))
  stop = asyncio.Event()
  try:
    arm1 = coordinator._by_id["line:arm-1"]
    arm1.load_s = 4.0  # committed load that must survive the refresh
    fresh_arm1 = BidderProfile(
      backend="line", part_id="arm-1", primitives={"gripper.motion"}
    )
    await _apply_and_drain(coordinator, [fresh_arm1], stop)
    assert coordinator._by_id["line:arm-1"].healthy
    assert not coordinator._by_id["line:arm-2"].healthy
    # Returning bidder keeps load state and refreshes primitives.
    assert coordinator._by_id["line:arm-1"].load_s == 4.0
  finally:
    stop.set()
    for task in coordinator._executor_tasks.values():
      task.cancel()
    await asyncio.gather(*coordinator._executor_tasks.values(), return_exceptions=True)


async def test_rediscovery_loop_disabled_without_callback_or_interval(
  tmp_path: Path,
) -> None:
  """Launch-time discovery only: the loop exits immediately unless both a
  callback and a positive interval are wired."""
  coordinator, _missions = _coordinator(
    tmp_path,
    RecordingGate(),
    _bidders(1),
  )
  stop = asyncio.Event()
  # No callback: returns without waiting on the interval.
  await asyncio.wait_for(coordinator._rediscovery_loop(stop), timeout=0.1)
  coordinator2, _ = _coordinator(tmp_path, RecordingGate(), _bidders(1))
  coordinator2._rediscover = lambda: _never()  # type: ignore[assignment]
  coordinator2._config = coordinator2._config.model_copy(
    update={"rediscovery_interval_s": 0.0}
  )
  await asyncio.wait_for(coordinator2._rediscovery_loop(stop), timeout=0.1)


async def _never() -> list[BidderProfile]:
  raise AssertionError("rediscover must not be called when the interval is 0")


async def test_rediscovery_loop_folds_fresh_bidders(tmp_path: Path) -> None:
  """The loop drives the fold: callback result becomes live bidders."""
  coordinator, _missions = _coordinator(
    tmp_path, RecordingGate(), [_fallback_bidder("line")]
  )
  stop = asyncio.Event()
  coordinator._rediscover = lambda: _returns(
    [BidderProfile(backend="line", part_id="arm-1", primitives={"gripper.motion"})]
  )
  coordinator._config = coordinator._config.model_copy(
    update={"rediscovery_interval_s": 0.05}
  )
  task = asyncio.create_task(coordinator._rediscovery_loop(stop))
  try:
    for _ in range(100):
      if "line:arm-1" in coordinator._by_id:
        break
      await asyncio.sleep(0.02)
    assert coordinator._by_id["line:arm-1"].healthy
  finally:
    stop.set()
    await asyncio.wait_for(task, timeout=2.0)
    for executor_task in coordinator._executor_tasks.values():
      executor_task.cancel()
    await asyncio.gather(*coordinator._executor_tasks.values(), return_exceptions=True)


async def _returns(bidders: list[BidderProfile]) -> list[BidderProfile]:
  return bidders
