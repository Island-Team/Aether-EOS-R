"""Autonomous proposal passes with unified dynamic prompt assembly."""

from __future__ import annotations

import asyncio
import json

import pytest

from egia.core.context import ContextBuilder
from egia.core.decision.policy import InvocationPolicy, InvocationTrigger
from egia.core.decision.proposer import _propose_pass, proposer_entry
from egia.core.missions import MissionQueue
from egia.core.perception.admission import Admission
from egia.lib.config import (
  DecisionConfig,
  PriorityConfig,
  ProposerConfig,
  load_mission_templates,
  load_prompt_config,
)
from egia.lib.logger import get_logger
from egia.lib.vlm import ModelResponse, Usage


class FakeModel:
  def __init__(self, replies: list[str | Exception]) -> None:
    self.replies = list(replies)
    self.requests: list[list] = []
    self.called = asyncio.Event()

  async def complete(self, messages, **_kwargs) -> ModelResponse:
    self.requests.append(messages)
    self.called.set()
    reply = self.replies.pop(0) if self.replies else '{"missions":[]}'
    if isinstance(reply, Exception):
      raise reply
    return ModelResponse(text=reply, model="fake", usage=Usage(total_tokens=10))


def _config(**overrides) -> DecisionConfig:
  return DecisionConfig(
    priority=PriorityConfig(),
    proposer=ProposerConfig(
      idle_reflect_interval_s=overrides.pop("idle_reflect_interval_s", 10.0),
      min_proposal_interval_s=overrides.pop("min_proposal_interval_s", 0.0),
      **overrides,
    ),
  )


def _proposal(
  order: str,
  urgency: float = 0.9,
  value: float = 0.8,
  mission_template: str | None = None,
) -> str:
  return json.dumps(
    {
      "missions": [
        {
          "order": order,
          "mission_template": mission_template,
          "urgency": urgency,
          "value": value,
          "reason": "test",
        }
      ]
    }
  )


async def _finish(task: asyncio.Task, stop: asyncio.Event) -> None:
  stop.set()
  await asyncio.wait_for(task, timeout=1)


@pytest.mark.asyncio
async def test_hearing_event_proposes_with_static_system_and_dynamic_user(
  etc_root,
) -> None:
  prompts = load_prompt_config(etc_root)
  queue = Admission(16)
  missions = MissionQueue(8, priority=PriorityConfig())
  order = 'speaker.play@default/speaker-0 text="听得到，请说。"'
  model = FakeModel([_proposal(order)])
  ready = asyncio.Event()
  stop = asyncio.Event()
  task = asyncio.create_task(
    proposer_entry(
      missions,
      model,
      ContextBuilder(queue, prompts),
      queue,
      None,
      prompts.proposer,
      load_mission_templates(etc_root),
      _config(),
      ready,
      stop,
      active_mission=lambda: "running-mission",
    )
  )
  await asyncio.wait_for(ready.wait(), timeout=1)

  await queue.enqueue(kind="hearing", source="operator", text="hello robot")
  await asyncio.wait_for(model.called.wait(), timeout=1)
  await _finish(task, stop)

  proposed = missions.pending()[0]
  assert proposed.order == order
  assert proposed.mission_template_id is None
  assert proposed.origin == "self" and proposed.source == "self.proposer"
  assert [message.role for message in model.requests[0]] == ["system", "user"]
  assert "You propose grounded missions" in model.requests[0][0].content
  dynamic = model.requests[0][1].content
  assert "hello robot" in dynamic
  assert dynamic.count("hello robot") == 1
  assert "home_living_2026@1" in dynamic
  assert "MISSION_TABLE empty" in dynamic
  assert dynamic.index("MISSION_TABLE empty") < dynamic.index("Perception:")
  assert dynamic.index("Perception:") < dynamic.index("Newest hearing (untrusted):")


@pytest.mark.asyncio
async def test_existing_perception_is_primed_as_history_not_a_trigger(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  queue = Admission(8)
  await queue.enqueue(kind="hearing", source="operator", text="old message")
  model = FakeModel([])
  ready = asyncio.Event()
  start = asyncio.Event()
  stop = asyncio.Event()
  task = asyncio.create_task(
    proposer_entry(
      MissionQueue(4),
      model,
      ContextBuilder(queue, prompts),
      queue,
      None,
      prompts.proposer,
      load_mission_templates(etc_root),
      _config(),
      ready,
      stop,
      active_mission=lambda: "busy",
      start=start,
    )
  )
  await asyncio.wait_for(ready.wait(), timeout=1)
  start.set()
  await asyncio.sleep(0.02)
  await _finish(task, stop)

  assert model.requests == []


@pytest.mark.asyncio
async def test_idle_reflection_and_hearing_fallback_are_both_admitted(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  idle_queue = Admission(8)
  idle_missions = MissionQueue(4)
  idle_model = FakeModel([_proposal("inspect the quiet room")])
  idle_stop = asyncio.Event()
  idle_task = asyncio.create_task(
    proposer_entry(
      idle_missions,
      idle_model,
      ContextBuilder(idle_queue, prompts),
      idle_queue,
      None,
      prompts.proposer,
      load_mission_templates(etc_root),
      _config(idle_reflect_interval_s=0.01),
      asyncio.Event(),
      idle_stop,
    )
  )
  await asyncio.wait_for(idle_model.called.wait(), timeout=1)
  await _finish(idle_task, idle_stop)
  assert idle_missions.pending()[0].order == "inspect the quiet room"

  hearing_queue = Admission(8)
  hearing_missions = MissionQueue(4)
  hearing_model = FakeModel(['{"missions":[]}'])
  hearing_ready = asyncio.Event()
  hearing_stop = asyncio.Event()
  hearing_task = asyncio.create_task(
    proposer_entry(
      hearing_missions,
      hearing_model,
      ContextBuilder(hearing_queue, prompts),
      hearing_queue,
      None,
      prompts.proposer,
      load_mission_templates(etc_root),
      _config(),
      hearing_ready,
      hearing_stop,
      active_mission=lambda: "busy",
      ensure_hearing_response=True,
    )
  )
  await asyncio.wait_for(hearing_ready.wait(), timeout=1)
  await hearing_queue.enqueue(kind="hearing", source="sandbox", text="stand up")
  await asyncio.wait_for(hearing_model.called.wait(), timeout=1)
  await _finish(hearing_task, hearing_stop)
  fallback = hearing_missions.pending()[0]
  assert fallback.source == "self.hearing-response"
  assert fallback.order.startswith(
    "Handle the latest heard intent exactly as requested"
  )
  assert '"stand up"' in fallback.order
  assert "Respond to" not in fallback.order


@pytest.mark.asyncio
async def test_unchanged_idle_state_does_not_repeat_model_requests(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  model = FakeModel(['{"missions":[]}'])
  queue = Admission(8)
  stop = asyncio.Event()
  task = asyncio.create_task(
    proposer_entry(
      MissionQueue(4),
      model,
      ContextBuilder(queue, prompts),
      queue,
      None,
      prompts.proposer,
      load_mission_templates(etc_root),
      _config(idle_reflect_interval_s=0.01),
      asyncio.Event(),
      stop,
    )
  )

  await asyncio.wait_for(model.called.wait(), timeout=1)
  await asyncio.sleep(0.05)
  await _finish(task, stop)

  assert len(model.requests) == 1


@pytest.mark.asyncio
async def test_proposal_pass_clamps_hints_and_honors_self_pending_cap(
  etc_root,
) -> None:
  prompts = load_prompt_config(etc_root)
  queue = Admission(8)
  missions = MissionQueue(4)
  model = FakeModel([_proposal("one", urgency=9, value=-3), _proposal("two")])
  context = ContextBuilder(queue, prompts)
  config = _config(max_self_pending=1)
  policy = InvocationPolicy()

  admitted, _ = await _propose_pass(
    missions,
    model,
    context,
    None,
    prompts.proposer,
    load_mission_templates(etc_root),
    config,
    policy,
    InvocationTrigger.PROPOSAL_EVENT,
    get_logger("test.proposer"),
  )
  second, _ = await _propose_pass(
    missions,
    model,
    context,
    None,
    prompts.proposer,
    load_mission_templates(etc_root),
    config,
    policy,
    InvocationTrigger.PROPOSAL_EVENT,
    get_logger("test.proposer"),
  )

  assert admitted == 1 and second == 0
  assert len(missions.pending()) == 1
  assert missions.pending()[0].urgency == 1.0
  assert missions.pending()[0].value == 0.0


@pytest.mark.asyncio
async def test_malformed_or_failed_proposal_costs_a_pass_without_admission(
  etc_root,
) -> None:
  prompts = load_prompt_config(etc_root)
  queue = Admission(8)
  missions = MissionQueue(4)
  model = FakeModel(
    [
      "not json",
      RuntimeError("provider unavailable"),
      '{"missions":[{"order":1}]}',
    ]
  )
  context = ContextBuilder(queue, prompts)
  config = _config()

  for _ in range(3):
    assert await _propose_pass(
      missions,
      model,
      context,
      None,
      prompts.proposer,
      load_mission_templates(etc_root),
      config,
      InvocationPolicy(),
      InvocationTrigger.PROPOSAL_IDLE,
      get_logger("test.proposer"),
    ) == (0, False)

  assert missions.pending() == []


@pytest.mark.asyncio
async def test_valid_template_seeds_plan_and_unknown_template_is_handled(
  etc_root,
) -> None:
  prompts = load_prompt_config(etc_root)
  templates = load_mission_templates(etc_root)
  queue = Admission(8)
  missions = MissionQueue(4)
  model = FakeModel(
    [
      _proposal("start competition", mission_template="home_living_2026"),
      _proposal("start competition", mission_template="not_registered"),
    ]
  )

  admitted, handled = await _propose_pass(
    missions,
    model,
    ContextBuilder(queue, prompts),
    None,
    prompts.proposer,
    templates,
    _config(),
    InvocationPolicy(),
    InvocationTrigger.PROPOSAL_EVENT,
    get_logger("test.proposer"),
  )
  rejected, rejected_handled = await _propose_pass(
    missions,
    model,
    ContextBuilder(queue, prompts),
    None,
    prompts.proposer,
    templates,
    _config(),
    InvocationPolicy(),
    InvocationTrigger.PROPOSAL_EVENT,
    get_logger("test.proposer"),
  )

  assert (admitted, handled) == (1, True)
  assert (rejected, rejected_handled) == (0, True)
  record = missions.pending()[0]
  assert record.mission_template_id == "home_living_2026"
  assert record.order == templates.templates[0].mission
  assert json.dumps(templates.templates[0].mission) in str(
    model.requests[0][-1].content
  )
  plan = missions.plan_steps(record.id)
  assert plan[0].id == "enter"
  assert all(step.required for step in plan)
