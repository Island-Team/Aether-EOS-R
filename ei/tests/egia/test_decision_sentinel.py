"""Fast safety-sentinel behavior and planner interruption integration."""

from __future__ import annotations

import asyncio
from datetime import UTC, datetime

import pytest

from egia.api.mission import MissionRecord
from egia.api.model_io import ModelExchange
from egia.core.context import ContextBuilder
from egia.core.decision.planner import DecisionPlanner
from egia.core.decision.sentinel import (
  SafetySentinel,
  SentinelSignal,
  SentinelSignalKind,
  _parse_signal,
)
from egia.core.perception.admission import Admission
from egia.core.registry import PrimitiveRegistry
from egia.lib.config import DecisionConfig, DecisionSentinelConfig, load_prompt_config
from egia.lib.vlm import ModelResponse


class ScriptedSentinelModel:
  reasoning_options = ("none", "high")

  def __init__(self, replies: list[str | Exception]) -> None:
    self.replies = list(replies)
    self.calls: list[tuple[list, dict]] = []

  async def complete(self, messages, **kwargs) -> ModelResponse:
    self.calls.append((messages, kwargs))
    reply = self.replies.pop(0)
    if isinstance(reply, Exception):
      raise reply
    return ModelResponse(text=reply, model="sentinel-test")


async def _append(values: list, value) -> None:
  values.append(value)


async def _trigger(
  sentinel: SafetySentinel,
  queue: Admission,
  goal: str,
  *,
  kind: str = "fault",
  text: str = "new safety state",
):
  task = asyncio.create_task(sentinel.watch(goal, asyncio.Event()))
  await asyncio.sleep(0)
  await queue.enqueue(kind=kind, source="test", text=text)
  return await asyncio.wait_for(task, timeout=1)


@pytest.mark.asyncio
async def test_estop_uses_no_reasoning_and_unified_prompt(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  queue = Admission(8)
  model = ScriptedSentinelModel(["ESTOP person inside robot envelope"])
  estops: list[str] = []
  stops: list[bool] = []
  exchanges: list[ModelExchange] = []
  sentinel = SafetySentinel(
    model,
    queue,
    DecisionSentinelConfig(),
    prompts.sentinel,
    motion_stop=lambda: _append(stops, True),
    engage_estop=lambda reason: _append(estops, reason),
    exchange_sink=exchanges.append,
  )

  signal = await _trigger(sentinel, queue, "assemble panel", text="person in envelope")

  assert signal == SentinelSignal(
    SentinelSignalKind.ESTOP, "person inside robot envelope"
  )
  assert estops == ["model sentinel: person inside robot envelope"]
  assert stops == []
  messages, kwargs = model.calls[0]
  assert kwargs["reasoning_effort"] == "none"
  assert [message.role for message in messages] == ["system", "user"]
  assert "fast safety sentinel" in messages[0].content
  assert "Mission goal: assemble panel" in messages[1].content
  assert "person in envelope" in messages[1].content
  assert [exchange.status for exchange in exchanges] == ["streaming", "complete"]


@pytest.mark.asyncio
async def test_continue_rechecks_after_perception_and_refreshes(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  queue = Admission(8)
  model = ScriptedSentinelModel(["CONTINUE", "REFRESH object position changed"])
  sentinel = SafetySentinel(
    model,
    queue,
    DecisionSentinelConfig(),
    prompts.sentinel,
    motion_stop=lambda: _append([], True),
    engage_estop=lambda reason: _append([], reason),
  )
  task = asyncio.create_task(sentinel.watch("pick sheet", asyncio.Event()))
  await asyncio.sleep(0)
  await queue.enqueue(kind="vision", source="camera", text="sheet visible")
  while len(model.calls) < 1:
    await asyncio.sleep(0)
  await queue.enqueue(kind="vision", source="camera", text="sheet moved")

  signal = await asyncio.wait_for(task, timeout=1)

  assert signal is not None and signal.kind is SentinelSignalKind.REFRESH
  assert len(model.calls) == 2
  assert "sheet moved" in model.calls[1][0][1].content


@pytest.mark.asyncio
async def test_change_during_completion_is_not_missed(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  queue = Admission(8)

  class RacingModel(ScriptedSentinelModel):
    async def complete(self, messages, **kwargs) -> ModelResponse:
      response = await super().complete(messages, **kwargs)
      if len(self.calls) == 1:
        await queue.enqueue(kind="vision", source="camera", text="person entered")
      return response

  model = RacingModel(["CONTINUE", "REFRESH person entered"])
  sentinel = SafetySentinel(
    model,
    queue,
    DecisionSentinelConfig(),
    prompts.sentinel,
    motion_stop=lambda: _append([], True),
    engage_estop=lambda reason: _append([], reason),
  )

  signal = await _trigger(
    sentinel, queue, "move fixture", kind="vision", text="fixture visible"
  )

  assert signal is not None and signal.kind is SentinelSignalKind.REFRESH
  assert len(model.calls) == 2


@pytest.mark.asyncio
async def test_stop_calls_motion_stop_and_safety_failure_still_returns_signal(
  etc_root,
) -> None:
  prompts = load_prompt_config(etc_root)
  model = ScriptedSentinelModel(["STOP obstacle too close"])

  async def failed_stop() -> None:
    raise RuntimeError("link unavailable")

  queue = Admission(4)
  sentinel = SafetySentinel(
    model,
    queue,
    DecisionSentinelConfig(),
    prompts.sentinel,
    motion_stop=failed_stop,
    engage_estop=lambda reason: _append([], reason),
  )

  signal = await _trigger(
    sentinel, queue, "move", kind="vision", text="obstacle detected"
  )
  assert signal == SentinelSignal(SentinelSignalKind.STOP, "obstacle too close")


@pytest.mark.asyncio
async def test_model_failure_retries_without_blocking_primary_safety(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  model = ScriptedSentinelModel([RuntimeError("provider down"), "ESTOP danger"])
  estops: list[str] = []
  exchanges: list[ModelExchange] = []
  queue = Admission(4)
  sentinel = SafetySentinel(
    model,
    queue,
    DecisionSentinelConfig(retry_s=0.001),
    prompts.sentinel,
    motion_stop=lambda: _append([], True),
    engage_estop=lambda reason: _append(estops, reason),
    exchange_sink=exchanges.append,
  )

  signal = await _trigger(sentinel, queue, "hold", text="danger detected")

  assert signal is not None and signal.kind is SentinelSignalKind.ESTOP
  assert len(model.calls) == 2
  assert estops == ["model sentinel: danger"]
  assert [item.status for item in exchanges] == [
    "streaming",
    "error",
    "streaming",
    "complete",
  ]


@pytest.mark.asyncio
async def test_preexisting_and_unrelated_state_cannot_trigger_stop(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  queue = Admission(4)
  await queue.enqueue(kind="fault", source="monitor", text="old stall")
  model = ScriptedSentinelModel(["STOP stale fault"])
  stops: list[bool] = []
  sentinel = SafetySentinel(
    model,
    queue,
    DecisionSentinelConfig(),
    prompts.sentinel,
    motion_stop=lambda: _append(stops, True),
    engage_estop=lambda reason: _append([], reason),
  )
  stop = asyncio.Event()
  task = asyncio.create_task(sentinel.watch("move", stop))
  await asyncio.sleep(0)

  await queue.enqueue(kind="hearing", source="operator", text="hello")
  await asyncio.sleep(0)
  assert model.calls == []
  assert stops == []

  stop.set()
  assert await asyncio.wait_for(task, timeout=1) is None


@pytest.mark.parametrize(
  ("reply", "expected"),
  [
    ("CONTINUE", None),
    ("refresh changed\nignored", SentinelSignal(SentinelSignalKind.REFRESH, "changed")),
    ("STOP", SentinelSignal(SentinelSignalKind.STOP, "stop")),
    ("ESTOP immediate", SentinelSignal(SentinelSignalKind.ESTOP, "immediate")),
    ("final safe", None),
    ("", None),
  ],
)
def test_signal_parser_accepts_only_sentinel_verdicts(reply, expected) -> None:
  assert _parse_signal(reply) == expected


@pytest.mark.asyncio
async def test_sentinel_refresh_cancels_stale_planner_reply(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  queue = Admission(8)

  class PrimaryModel:
    reasoning_options = ("none",)
    reasoning_effort = "none"

    def __init__(self) -> None:
      self.calls: list[list] = []

    async def complete(self, messages, **_kwargs) -> ModelResponse:
      self.calls.append(messages)
      if len(self.calls) == 1:
        await asyncio.Future()
      return ModelResponse(text="final recovered")

  class SentinelStub:
    def __init__(self) -> None:
      self.calls = 0

    async def watch(self, _goal, stop):
      self.calls += 1
      if self.calls == 1:
        await queue.enqueue(kind="vision", source="camera", text="object moved")
        return SentinelSignal(SentinelSignalKind.REFRESH, "object moved")
      await stop.wait()
      return None

  class ReasonerStub:
    reasoning_options = ("high",)
    reasoning_effort = "high"

    async def complete(self, _messages, **_kwargs) -> ModelResponse:
      return ModelResponse(text='plan add recover "State is refreshed."')

  primary = PrimaryModel()
  planner = DecisionPlanner(
    primary,
    ContextBuilder(queue, prompts),
    PrimitiveRegistry.from_contracts(),
    object(),  # type: ignore[arg-type]  # no primitive is executed in this test
    queue,
    None,
    None,
    None,
    prompts.planner,
    DecisionConfig(
      max_steps=2,
      min_call_interval_s=0,
    ),
    sentinel=SentinelStub(),  # type: ignore[arg-type]
    reasoner_model=ReasonerStub(),
    reasoner_prompts=prompts.reasoner,
  )
  mission = MissionRecord(
    id="mission-1",
    order="place object",
    source="test",
    submitted_at=datetime.now(UTC),
  )

  assert await planner.run(mission, asyncio.Event()) == "recovered"
  assert len(primary.calls) == 2
  dynamic = primary.calls[1][1].content
  assert "object moved" in dynamic
  assert "INTERRUPT (sentinel)" in dynamic
