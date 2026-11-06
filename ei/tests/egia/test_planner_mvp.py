"""Planner orchestration for the primitive-only MVP reply language."""

from __future__ import annotations

import asyncio
from datetime import UTC, datetime

import pytest

from egia.api.mission import MissionRecord
from egia.api.model_io import ModelActivity, ModelExchange
from egia.api.plan import (
  ActionRecord,
  PlanRecord,
  PlanStep,
  PlanStepStatus,
  StepOutcome,
  StepRecord,
)
from egia.contracts.actions import PrimitiveAck
from egia.core.context import ContextBuilder
from egia.core.decision import DecisionPlanner, MissionFailedError
from egia.core.perception.admission import Admission
from egia.core.registry import PrimitiveRegistry
from egia.internal.decision import ActionInvocation, GateVerdict, StepContext
from egia.lib.config import DecisionConfig, PromptConfig, load_prompt_config
from egia.lib.vlm import ChatMessage, ModelResponse, Usage


class ScriptedModel:
  """Return scripted values while recording the exact provider requests."""

  reasoning_options = ("none", "low", "high")
  reasoning_effort = "none"

  def __init__(self, replies: list[str | Exception]) -> None:
    self.replies = list(replies)
    self.requests: list[list[ChatMessage]] = []
    self.kwargs: list[dict] = []

  async def complete(self, messages: list[ChatMessage], **kwargs) -> ModelResponse:
    self.requests.append(messages)
    self.kwargs.append(kwargs)
    reply = self.replies.pop(0)
    if isinstance(reply, Exception):
      raise reply
    return ModelResponse(
      text=reply,
      model="scripted",
      usage=Usage(prompt_tokens=7, completion_tokens=3),
    )


class BlockingModel:
  reasoning_options: tuple[str, ...] = ("none",)
  reasoning_effort = "none"

  def __init__(self) -> None:
    self.started = asyncio.Event()
    self.cancelled = False

  async def complete(self, _messages, **_kwargs) -> ModelResponse:
    self.started.set()
    try:
      await asyncio.Future()
    except asyncio.CancelledError:
      self.cancelled = True
      raise
    raise AssertionError("blocking model unexpectedly resumed")


class BodyUpdatingModel(ScriptedModel):
  """Publish continuous body telemetry during the first controller request."""

  def __init__(self, replies: list[str], queue: Admission) -> None:
    super().__init__(replies)
    self._queue = queue

  async def complete(self, messages: list[ChatMessage], **kwargs) -> ModelResponse:
    if not self.requests:
      await self._queue.enqueue(
        kind="body",
        source="body.default.arm-0",
        text='health=ok state={"joint_positions":[0.01]}',
      )
    return await super().complete(messages, **kwargs)


class PerceptionUpdatingModel(ScriptedModel):
  """Change action-relevant perception during the first fast request."""

  def __init__(self, replies: list[str], queue: Admission) -> None:
    super().__init__(replies)
    self._queue = queue

  async def complete(self, messages: list[ChatMessage], **kwargs) -> ModelResponse:
    if not self.requests:
      await self._queue.enqueue(
        kind="vision", source="camera", text="obstacle layout changed"
      )
    return await super().complete(messages, **kwargs)


class ReleasableReasoner:
  """Hold a high-reasoning reply until the test permits publication."""

  reasoning_options = ("high",)
  reasoning_effort = "high"

  def __init__(self, reply: str) -> None:
    self.reply = reply
    self.started = asyncio.Event()
    self.release = asyncio.Event()
    self.requests: list[list[ChatMessage]] = []
    self.kwargs: list[dict] = []
    self.cancelled = False

  async def complete(self, messages: list[ChatMessage], **kwargs) -> ModelResponse:
    self.requests.append(messages)
    self.kwargs.append(kwargs)
    self.started.set()
    try:
      await self.release.wait()
    except asyncio.CancelledError:
      self.cancelled = True
      raise
    return ModelResponse(
      text=self.reply,
      model="reasoner",
      usage=Usage(prompt_tokens=11, completion_tokens=5),
    )


class BlockingThirdController(ScriptedModel):
  """Hold the third fast reply while a concurrent reasoner proposal settles."""

  def __init__(self, replies: list[str]) -> None:
    super().__init__(replies)
    self.third_started = asyncio.Event()
    self.release_third = asyncio.Event()
    self.cancelled = False

  async def complete(self, messages: list[ChatMessage], **kwargs) -> ModelResponse:
    if len(self.requests) == 2:
      self.third_started.set()
      try:
        await self.release_third.wait()
      except asyncio.CancelledError:
        self.cancelled = True
        raise
    return await super().complete(messages, **kwargs)


class FakeGate:
  def __init__(self, verdict: GateVerdict | None = None) -> None:
    self.verdict = verdict or GateVerdict(
      admitted=True,
      ack=PrimitiveAck(
        primitive_version="1.0.0",
        primitive="base.velocity",
        accepted=True,
        command_id="cmd-test",
        sequence=3,
        result={"status": "succeeded", "distance_m": 0.1},
      ),
    )
    self.calls: list[tuple[ActionInvocation, StepContext | None]] = []

  async def execute(self, invocation, context=None) -> GateVerdict:
    self.calls.append((invocation, context))
    return self.verdict


def _mission(order: str = "move forward") -> MissionRecord:
  return MissionRecord(
    id="mission-1",
    order=order,
    source="test",
    submitted_at=datetime.now(UTC),
  )


def _planner(
  model,
  queue: Admission,
  prompt_config: PromptConfig,
  *,
  gate: FakeGate | None = None,
  config: DecisionConfig | None = None,
  plan_sink=None,
  exchange_sink=None,
  activity_sink=None,
  resume_plan=None,
  initial_plan=None,
  reasoner_model=None,
) -> tuple[DecisionPlanner, FakeGate]:
  selected_gate = gate or FakeGate()
  planner = DecisionPlanner(
    model,
    ContextBuilder(queue, prompt_config),
    PrimitiveRegistry.from_contracts(),
    selected_gate,  # type: ignore[arg-type]
    queue,
    None,
    None,
    None,
    prompt_config.planner,
    config or DecisionConfig(max_steps=8, min_call_interval_s=0),
    plan_sink=plan_sink,
    exchange_sink=exchange_sink,
    activity_sink=activity_sink,
    resume_plan=resume_plan,
    initial_plan=initial_plan,
    reasoner_model=reasoner_model,
    reasoner_prompts=prompt_config.reasoner if reasoner_model is not None else None,
    summarizer_prompts=prompt_config.summarizer,
  )
  return planner, selected_gate


@pytest.mark.asyncio
async def test_context_pressure_uses_llm_mission_summary_and_hides_settled_detail(
  etc_root,
) -> None:
  prompts = load_prompt_config(etc_root)
  model = ScriptedModel(
    [
      (
        "MISSION: move forward; prior outcomes are settled\n"
        "PLAN 1: outcomes 0 through 78 completed\n"
        "PLAN 2: complete outcome 79 remains active"
      )
    ]
  )
  planner, _ = _planner(model, Admission(4), prompts)
  plan = [
    PlanStep(
      id=f"step-{index}",
      sentence=f"Complete outcome {index}",
      status=PlanStepStatus.RUNNING if index == 79 else PlanStepStatus.DONE,
    )
    for index in range(80)
  ]
  history = [f"result {index} " + "x" * 1200 for index in range(100)]
  mission = _mission()

  assert planner._mission_context_needs_summary(mission, None, history, plan)
  summary = await planner._summarize_mission_context(mission, None, history, plan)
  assert summary is not None

  messages = planner._build_messages(mission, [], None, plan, mission_summary=summary)
  user = messages[1].content

  assert [message.role for message in model.requests[0]] == ["system", "user"]
  assert "Compress one Mission" in model.requests[0][0].content
  assert isinstance(user, str)
  assert len(user) <= prompts.token_budget * 4
  assert "Compressed Mission history:" in user
  assert "outcomes 0 through 78 completed" in user
  assert "CURRENT_STEP step-79" in user
  assert "STEP role=settled id=step-0" not in user


@pytest.mark.asyncio
async def test_continuous_body_update_does_not_block_primitive_dispatch(
  etc_root,
) -> None:
  prompts = load_prompt_config(etc_root)
  queue = Admission(8)
  model = BodyUpdatingModel(
    [
      (
        'plan add greet "Greeting was spoken."\nstep greet\n'
        "speaker.play@default/speaker-0 uri=https://example.test/greeting.wav"
      ),
      "final greeting requested",
    ],
    queue,
  )
  planner, gate = _planner(model, queue, prompts)

  assert await planner.run(_mission("say hello"), asyncio.Event()) == (
    "greeting requested"
  )
  assert len(gate.calls) == 1
  assert gate.calls[0][0].action == "speaker.play"


@pytest.mark.asyncio
async def test_successful_action_replay_is_rejected_before_egress(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  model = ScriptedModel(
    [
      (
        'plan add stand "Base posture command accepted."\n'
        "step stand\nbase.posture@default/base-0 posture=stand"
      ),
      (
        'plan add stand-again "Base posture command accepted."\n'
        "step stand-again\nbase.posture@default/base-0 posture=stand"
      ),
      "final stand command accepted",
    ]
  )
  planner, gate = _planner(model, Admission(8), prompts)

  assert await planner.run(_mission("stand up"), asyncio.Event()) == (
    "stand command accepted"
  )
  assert len(gate.calls) == 1
  assert gate.calls[0][0].action == "base.posture"
  retry_prompt = model.requests[2][-1].content
  assert "equivalent action and outcome already succeeded in this Mission" in (
    retry_prompt
  )
  assert "The Mission has no unfinished steps" in retry_prompt
  assert "Reply with exactly one line: final <supported outcome>" in retry_prompt


@pytest.mark.asyncio
async def test_same_primitive_arguments_can_serve_distinct_outcomes(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  model = ScriptedModel(
    [
      (
        'plan add first "First one-meter segment accepted."\n'
        "step first\n"
        "base.navigate@default/base-0 reference=relative x_m=1 y_m=0 yaw_rad=0"
      ),
      (
        'plan add second "Second one-meter segment accepted."\n'
        "step second\n"
        "base.navigate@default/base-0 reference=relative x_m=1 y_m=0 yaw_rad=0"
      ),
      "final two movement segments accepted",
    ]
  )
  planner, gate = _planner(model, Admission(8), prompts)

  assert await planner.run(_mission("move forward two meters"), asyncio.Event()) == (
    "two movement segments accepted"
  )
  assert len(gate.calls) == 2


@pytest.mark.asyncio
async def test_restored_running_step_is_reported_as_unknown(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  queue = Admission(8)
  mission = _mission("inspect the dock")
  now = datetime.now(UTC)
  resumed = PlanRecord(
    mission_id=mission.id,
    order=mission.order,
    source=mission.source,
    started_at=now,
    finished_at=now,
    outcome=StepOutcome.ABORTED,
    plan=[
      PlanStep(
        id="inspect",
        sentence="Inspect the dock.",
        status=PlanStepStatus.RUNNING,
        commands=["camera.capture@default/camera-0 mode=rgb"],
        required=True,
        terminal=True,
      )
    ],
    steps=[
      StepRecord(
        index=1,
        outcome=StepOutcome.FAILED,
        actions=[
          ActionRecord(
            index=1,
            action="base.navigate",
            arguments={
              "reference": "relative",
              "x_m": 0.2,
              "y_m": 0,
              "yaw_rad": 0,
            },
            outcome=StepOutcome.FAILED,
            reason="relative navigation requires normal localization",
          )
        ],
      )
    ],
  )
  model = ScriptedModel(
    [
      "final bypassed",
      "plan retry inspect",
      (
        "step inspect\n"
        "base.velocity@default/base-0 linear_x_m_s=0.1 "
        "linear_y_m_s=0 angular_z_rad_s=0 valid_for_ms=100"
      ),
      'plan complete inspect "fresh result confirms inspection"',
      "final current state verified",
    ]
  )
  checkpoints: list[tuple[PlanRecord, bool]] = []
  planner, gate = _planner(
    model,
    queue,
    prompts,
    plan_sink=lambda plan, active: checkpoints.append((plan, active)),
    resume_plan=lambda mission_id: resumed if mission_id == mission.id else None,
  )

  assert await planner.run(mission, asyncio.Event()) == "current state verified"
  assert len(gate.calls) == 1
  user_message = str(model.requests[0][-1].content)
  assert "MISSION RESTORED" in user_message
  assert "outcome is unknown" in user_message
  assert "camera.capture@default/camera-0 mode=rgb" in user_message
  assert "relative navigation requires normal localization" in user_message
  assert checkpoints[0][0].plan[0].status is PlanStepStatus.VERIFYING
  assert checkpoints[0][1] is True
  settled = planner.last_plan()
  assert settled is not None
  assert settled.id != resumed.id
  assert any(
    annotation.event == "resumed_from" and annotation.detail == resumed.id
    for annotation in settled.annotations
  )
  assert settled.plan[0].status is PlanStepStatus.DONE


@pytest.mark.asyncio
async def test_plan_step_and_final_run_through_primitive_gate(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  queue = Admission(16)
  await queue.enqueue(kind="environment", source="test", text="clear workshop")
  model = ScriptedModel(
    [
      'plan add move "Robot has moved forward."',
      (
        "step move\n"
        "base.velocity@default/base-0 linear_x_m_s=0.2 "
        "linear_y_m_s=0 angular_z_rad_s=0 valid_for_ms=250"
      ),
      "final movement complete",
    ]
  )
  plans: list[tuple[PlanRecord, bool]] = []
  exchanges: list[ModelExchange] = []
  activities: list[ModelActivity] = []
  planner, gate = _planner(
    model,
    queue,
    prompts,
    plan_sink=lambda plan, live: plans.append((plan, live)),
    exchange_sink=exchanges.append,
    activity_sink=activities.append,
  )

  assert await planner.run(_mission(), asyncio.Event()) == "movement complete"

  assert len(gate.calls) == 1
  invocation, step_context = gate.calls[0]
  assert invocation.action == "base.velocity"
  assert invocation.robot_id == "default"
  assert invocation.device_id == "base-0"
  assert step_context is not None
  assert step_context.mission_id == "mission-1"
  result = queue.latest_by_kind("result")
  assert result is not None and result.source == "decision"
  assert "distance_m" in model.requests[-1][1].content
  assert [message.role for message in model.requests[0]] == ["system", "user"]
  assert "Home Living Competition Reference:" not in model.requests[0][0].content
  assert 'MISSION role=active id=mission- state=pending order="move forward"' in str(
    model.requests[0][1].content
  )
  assert "PLAN\nCURRENT_STEP none" in model.requests[1][1].content
  assert "context.read" not in "\n".join(
    str(message.content) for message in model.requests[0]
  )

  settled = planner.last_plan()
  assert settled is not None
  assert settled.outcome is StepOutcome.SUCCEEDED
  assert settled.result == "movement complete"
  assert settled.plan[0].status is PlanStepStatus.DONE
  assert settled.steps[0].actions[0].command.startswith("base.velocity@")
  assert plans[0][1] is True and plans[-1][1] is False
  assert len([exchange for exchange in exchanges if exchange.status == "complete"]) == 3
  assert [activity.status for activity in activities] == ["running", "complete"]


@pytest.mark.asyncio
@pytest.mark.parametrize(
  ("order", "primitive"),
  [
    (
      ("speaker.play@default/speaker-0 uri=https://example.test/greeting.wav"),
      "speaker.play",
    ),
    ("base.posture@default/base-0 posture=stand", "base.posture"),
  ],
)
async def test_direct_primitive_mission_finishes_after_backend_acceptance(
  etc_root, order: str, primitive: str
) -> None:
  prompts = load_prompt_config(etc_root)
  model = ScriptedModel([])
  gate = FakeGate(
    GateVerdict(
      admitted=True,
      ack=PrimitiveAck(
        primitive_version="2.0.0" if primitive == "speaker.play" else "1.0.0",
        primitive=primitive,
        accepted=True,
        command_id="cmd-direct",
        sequence=9,
        result={"status": "succeeded"},
      ),
    )
  )
  planner, _ = _planner(model, Admission(8), prompts, gate=gate)

  result = await planner.run(_mission(order), asyncio.Event())

  assert result == f'{order} -> succeeded (sequence 9) {{"status": "succeeded"}}'
  assert len(model.requests) == 0
  assert len(gate.calls) == 1
  settled = planner.last_plan()
  assert settled is not None
  assert settled.outcome is StepOutcome.SUCCEEDED
  assert len(settled.steps) == 1


@pytest.mark.asyncio
async def test_direct_primitive_is_preflighted_before_egress(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  model = ScriptedModel([])
  planner, gate = _planner(model, Admission(8), prompts)

  with pytest.raises(MissionFailedError, match="unknown target.*robot-x/base-0"):
    await planner.run(
      _mission("base.posture@robot-x/base-0 posture=stand"), asyncio.Event()
    )

  assert model.requests == []
  assert gate.calls == []


@pytest.mark.asyncio
async def test_direct_mission_does_not_finish_after_mismatched_action(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  order = "speaker.play@default/speaker-0 uri=https://example.test/greeting.wav"
  model = ScriptedModel(
    [
      (
        'plan add speak "A greeting was spoken."\nstep speak\n'
        "speaker.play@default/speaker-0 uri=https://example.test/alternate.wav"
      ),
      "final alternate greeting accepted",
    ]
  )
  planner, gate = _planner(model, Admission(8), prompts)
  mission = _mission(order)
  mission.mission_template_id = "test-template"

  assert await planner.run(mission, asyncio.Event()) == ("alternate greeting accepted")
  assert len(model.requests) == 2
  assert len(gate.calls) == 1


@pytest.mark.asyncio
async def test_executing_primitive_leaves_other_plan_steps_pending(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  queue = Admission(8)
  mission = _mission("stand and wait")
  model = ScriptedModel(
    [
      "step stand\nbase.posture@default/base-0 posture=stand",
      "final standing",
    ]
  )
  initial = [
    PlanStep(
      id="stale",
      sentence="Previously selected work.",
      status=PlanStepStatus.RUNNING,
    ),
    PlanStep(id="stand", sentence="Stand upright."),
  ]
  plans: list[PlanRecord] = []
  planner, _ = _planner(
    model,
    queue,
    prompts,
    plan_sink=lambda plan, active: plans.append(plan),
    initial_plan=lambda mission_id: initial if mission_id == mission.id else [],
  )

  assert await planner.run(mission, asyncio.Event()) == "standing"
  executing = next(
    plan
    for plan in plans
    if any(
      step.status is PlanStepStatus.RUNNING and step.id == "stand" for step in plan.plan
    )
  )
  assert [step.status for step in executing.plan] == [
    PlanStepStatus.PENDING,
    PlanStepStatus.RUNNING,
  ]


@pytest.mark.asyncio
async def test_invalid_target_stays_in_controller_lane_until_corrected(
  etc_root,
) -> None:
  prompts = load_prompt_config(etc_root)
  queue = Admission(8)
  malformed = (
    'plan add move "Base moved."\n'
    "step move\n"
    "base.velocity@base-0 linear_x_m_s=0.1 linear_y_m_s=0 "
    "angular_z_rad_s=0 valid_for_ms=100"
  )
  controller = ScriptedModel(
    [
      malformed,
      malformed,
      (
        'plan add move "Base moved."\n'
        "step move\n"
        "base.velocity@default/base-0 linear_x_m_s=0.1 linear_y_m_s=0 "
        "angular_z_rad_s=0 valid_for_ms=100"
      ),
      "final moved",
    ]
  )
  reasoner = ScriptedModel([])
  planner, gate = _planner(
    controller,
    queue,
    prompts,
    reasoner_model=reasoner,
  )

  assert await planner.run(_mission(), asyncio.Event()) == "moved"
  assert len(gate.calls) == 1
  assert reasoner.requests == []
  assert "explicit target required" in controller.requests[2][-1].content


@pytest.mark.asyncio
async def test_required_initial_plan_waits_for_explicit_verification(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  queue = Admission(16)
  model = ScriptedModel(
    [
      (
        "step enter\n"
        "base.velocity@default/base-0 linear_x_m_s=0.2 "
        "linear_y_m_s=0 angular_z_rad_s=0 valid_for_ms=250"
      ),
      'plan complete enter "fresh result confirms entry"',
      "final competition entry complete",
    ]
  )
  plans: list[tuple[PlanRecord, bool]] = []
  planner, _ = _planner(
    model,
    queue,
    prompts,
    plan_sink=lambda plan, live: plans.append((plan, live)),
    initial_plan=lambda _mission_id: [
      PlanStep(
        id="enter",
        sentence="Enter through the entrance.",
        required=True,
        terminal=True,
      )
    ],
  )

  assert await planner.run(_mission("competition"), asyncio.Event()) == (
    "competition entry complete"
  )
  first_prompt = str(model.requests[0][1].content)
  assert (
    "STEP role=ready id=enter state=pending required=true after=[] terminal=true "
    'text="Enter through the entrance."'
  ) in first_prompt
  assert any(plan.plan[0].status is PlanStepStatus.VERIFYING for plan, _live in plans)
  settled = planner.last_plan()
  assert settled is not None
  assert settled.plan[0].status is PlanStepStatus.DONE
  assert settled.plan[0].evidence == "fresh result confirms entry"


@pytest.mark.asyncio
async def test_required_steps_are_protected_and_terminal_can_follow_failures(
  etc_root,
) -> None:
  prompts = load_prompt_config(etc_root)
  model = ScriptedModel(
    [
      "plan remove prerequisite",
      (
        "step dependent\n"
        "base.velocity@default/base-0 linear_x_m_s=0.1 "
        "linear_y_m_s=0 angular_z_rad_s=0 valid_for_ms=100"
      ),
      'plan fail prerequisite "required capability unavailable"',
      'plan fail dependent "prerequisite failed"',
      (
        "step exit\n"
        "base.velocity@default/base-0 linear_x_m_s=0.1 "
        "linear_y_m_s=0 angular_z_rad_s=0 valid_for_ms=100"
      ),
      'plan complete exit "fresh perception confirms exit"',
      "final partial mission complete",
    ]
  )
  planner, gate = _planner(
    model,
    Admission(16),
    prompts,
    initial_plan=lambda _mission_id: [
      PlanStep(
        id="prerequisite",
        sentence="Complete prerequisite.",
        required=True,
      ),
      PlanStep(
        id="dependent",
        sentence="Complete dependent outcome.",
        required=True,
        after=["prerequisite"],
      ),
      PlanStep(
        id="exit",
        sentence="Leave safely.",
        required=True,
        terminal=True,
        after=["dependent"],
      ),
    ],
  )

  assert await planner.run(_mission("protected"), asyncio.Event()) == (
    "partial mission complete"
  )
  assert len(gate.calls) == 1
  assert "cannot be removed" in str(model.requests[1][1].content)
  assert "blocked by prerequisite" in str(model.requests[2][1].content)
  settled = planner.last_plan()
  assert settled is not None
  statuses = {step.id: step.status for step in settled.plan}
  assert statuses == {
    "prerequisite": PlanStepStatus.FAILED,
    "dependent": PlanStepStatus.FAILED,
    "exit": PlanStepStatus.DONE,
  }


@pytest.mark.asyncio
async def test_invalid_json_and_unknown_primitive_are_returned_for_retry(
  etc_root,
) -> None:
  prompts = load_prompt_config(etc_root)
  model = ScriptedModel(
    [
      '{"final":"obsolete JSON"}',
      'plan add inspect "Inspection complete."\nstep inspect\nlaser.fire@default/x power=9',
      "final safely declined",
    ]
  )
  reasoner = ScriptedModel(['plan add recovery "Inspection is safely declined."'])
  planner, gate = _planner(model, Admission(8), prompts, reasoner_model=reasoner)

  assert await planner.run(_mission("inspect"), asyncio.Event()) == "safely declined"

  assert gate.calls == []
  assert "not a valid command" in model.requests[1][1].content
  assert "unknown primitive" in model.requests[2][1].content
  record = planner.last_plan()
  assert record is not None and record.steps[-1].retries == 2


@pytest.mark.asyncio
async def test_wrong_primitive_target_reports_available_targets(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  model = ScriptedModel(
    [
      (
        'plan add stand "Robot is standing."\nstep stand\n'
        "base.posture@base-0/default posture=stand"
      ),
      "final invalid target was not executed",
    ]
  )
  planner, gate = _planner(model, Admission(8), prompts)

  assert await planner.run(_mission("stand"), asyncio.Event()) == (
    "invalid target was not executed"
  )
  assert gate.calls == []
  assert "unknown target 'base-0/default'" in str(model.requests[1][1].content)
  assert "available targets: ['default/base-0']" in str(model.requests[1][1].content)


@pytest.mark.asyncio
async def test_invalid_controller_replies_are_bounded(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  model = ScriptedModel(['{"invalid":true}'] * 3)
  planner, gate = _planner(
    model,
    Admission(8),
    prompts,
    config=DecisionConfig(
      max_steps=0,
      max_invalid_replies=3,
      min_call_interval_s=0,
    ),
  )

  with pytest.raises(MissionFailedError, match="invalid controller reply limit"):
    await planner.run(_mission("inspect"), asyncio.Event())

  assert len(model.requests) == 3
  assert gate.calls == []


@pytest.mark.asyncio
async def test_terminal_validation_does_not_reset_invalid_reply_limit(
  etc_root,
) -> None:
  prompts = load_prompt_config(etc_root)
  model = ScriptedModel(['{"invalid":true}', "final premature", '{"still":"invalid"}'])
  planner, _ = _planner(
    model,
    Admission(8),
    prompts,
    config=DecisionConfig(
      max_steps=0,
      max_invalid_replies=3,
      min_call_interval_s=0,
    ),
    initial_plan=lambda _mission_id: [
      PlanStep(id="inspect", sentence="Inspect the target.", required=True)
    ],
  )

  with pytest.raises(MissionFailedError, match="invalid controller reply limit"):
    await planner.run(_mission("inspect"), asyncio.Event())

  assert len(model.requests) == 3


@pytest.mark.asyncio
async def test_atomic_failed_plan_final_marks_mission_failed(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  model = ScriptedModel(
    ['plan fail inspect "camera unavailable"\nfinal inspection could not be completed']
  )
  planner, gate = _planner(
    model,
    Admission(8),
    prompts,
    initial_plan=lambda _mission_id: [
      PlanStep(id="inspect", sentence="Inspect the target.")
    ],
  )

  with pytest.raises(MissionFailedError, match="inspection could not be completed"):
    await planner.run(_mission("inspect"), asyncio.Event())

  assert gate.calls == []
  settled = planner.last_plan()
  assert settled is not None
  assert settled.outcome is StepOutcome.FAILED
  assert settled.plan[0].status is PlanStepStatus.FAILED


@pytest.mark.asyncio
async def test_failed_gate_result_is_observed_before_replanning(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  gate = FakeGate(GateVerdict(admitted=False, reason="estop_engaged"))
  model = ScriptedModel(
    [
      (
        'plan add move "Robot has moved."\nstep move\n'
        "base.velocity@default/base-0 linear_x_m_s=0.2 "
        "linear_y_m_s=0 angular_z_rad_s=0 valid_for_ms=250"
      ),
      "final motion was blocked",
    ]
  )
  reasoner = ScriptedModel(['plan fail move "motion blocked by estop"'])
  planner, _ = _planner(
    model,
    Admission(8),
    prompts,
    gate=gate,
    reasoner_model=reasoner,
  )

  assert await planner.run(_mission(), asyncio.Event()) == "motion was blocked"

  assert "rejected by gate (estop_engaged)" in model.requests[1][1].content
  record = planner.last_plan()
  assert record is not None
  assert record.steps[0].outcome is StepOutcome.FAILED
  assert record.plan[0].status is PlanStepStatus.FAILED


@pytest.mark.asyncio
async def test_controller_always_uses_non_reasoning_mode(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  model = ScriptedModel(
    ['plan add inspect "Inspection complete."\neffort low', "final inspected"]
  )
  planner, _ = _planner(
    model,
    Admission(8),
    prompts,
    config=DecisionConfig(
      max_steps=4,
      min_call_interval_s=0,
    ),
  )

  assert await planner.run(_mission("inspect"), asyncio.Event()) == "inspected"
  assert [kwargs["reasoning_effort"] for kwargs in model.kwargs] == ["none", "none"]
  assert [kwargs["max_tokens"] for kwargs in model.kwargs] == [512, 512]


@pytest.mark.asyncio
async def test_model_transport_retries_then_completes(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  model = ScriptedModel([RuntimeError("provider down"), "final recovered"])
  planner, _ = _planner(
    model,
    Admission(8),
    prompts,
    config=DecisionConfig(
      max_steps=2,
      min_call_interval_s=0,
      model_retry_initial_s=0.001,
      model_retry_max_s=0.001,
    ),
  )

  assert await planner.run(_mission(), asyncio.Event()) == "recovered"
  assert len(model.requests) == 2


@pytest.mark.asyncio
async def test_shutdown_cancels_inflight_completion_and_saves_abort(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  model = BlockingModel()
  planner, gate = _planner(model, Admission(8), prompts)
  stop = asyncio.Event()
  task = asyncio.create_task(planner.run(_mission(), stop))
  await asyncio.wait_for(model.started.wait(), timeout=1)

  stop.set()
  with pytest.raises(MissionFailedError, match="shutdown during model call"):
    await asyncio.wait_for(task, timeout=1)

  assert model.cancelled
  assert gate.calls == []
  record = planner.last_plan()
  assert record is not None and record.outcome is StepOutcome.ABORTED
  assert planner.running_mission_id is None


@pytest.mark.asyncio
async def test_controller_runs_first_and_waits_safely_for_initial_reasoning(
  etc_root,
) -> None:
  prompts = load_prompt_config(etc_root)
  controller = ScriptedModel(
    [
      "reason mission needs decomposition",
      (
        "step inspect\n"
        "base.velocity@default/base-0 linear_x_m_s=0.1 "
        "linear_y_m_s=0 angular_z_rad_s=0 valid_for_ms=100"
      ),
      "final inspection complete",
    ]
  )
  reasoner = ReleasableReasoner('plan add inspect "Inspection is complete."')
  planner, gate = _planner(
    controller,
    Admission(8),
    prompts,
    reasoner_model=reasoner,
  )

  task = asyncio.create_task(planner.run(_mission("inspect safely"), asyncio.Event()))
  await asyncio.wait_for(reasoner.started.wait(), timeout=1)
  await asyncio.sleep(0)

  assert len(controller.requests) == 1
  assert controller.kwargs[0]["reasoning_effort"] == "none"
  assert gate.calls == []

  reasoner.release.set()
  assert await asyncio.wait_for(task, timeout=1) == "inspection complete"
  assert reasoner.kwargs[0]["reasoning_effort"] == "high"
  assert len(gate.calls) == 1


@pytest.mark.asyncio
async def test_controller_executes_existing_plan_while_reasoner_runs(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  controller = ScriptedModel(
    [
      'plan add move "Robot has moved."',
      (
        "reason prepare complex follow-up work\nstep move\n"
        "base.velocity@default/base-0 linear_x_m_s=0.1 "
        "linear_y_m_s=0 angular_z_rad_s=0 valid_for_ms=100"
      ),
      "final movement complete",
    ]
  )
  reasoner = ReleasableReasoner('plan add verify "Movement is verified."')
  planner, gate = _planner(
    controller,
    Admission(8),
    prompts,
    reasoner_model=reasoner,
  )

  task = asyncio.create_task(planner.run(_mission(), asyncio.Event()))
  await asyncio.wait_for(reasoner.started.wait(), timeout=1)
  for _ in range(20):
    if gate.calls:
      break
    await asyncio.sleep(0)

  assert len(gate.calls) == 1
  assert not reasoner.release.is_set()

  reasoner.release.set()
  assert await asyncio.wait_for(task, timeout=1) == "movement complete"


@pytest.mark.asyncio
async def test_stale_reasoner_proposal_cannot_overwrite_controller_plan(
  etc_root,
) -> None:
  prompts = load_prompt_config(etc_root)
  controller = ScriptedModel(
    [
      'plan add move "Robot has moved."',
      (
        'plan edit move "Robot has moved to the revised target."\n'
        "reason refine future work"
      ),
      (
        "step move\n"
        "base.velocity@default/base-0 linear_x_m_s=0.1 "
        "linear_y_m_s=0 angular_z_rad_s=0 valid_for_ms=100"
      ),
      "final movement complete",
    ]
  )
  reasoner = ReleasableReasoner('plan add obsolete "Outdated work."')
  planner, gate = _planner(
    controller,
    Admission(8),
    prompts,
    reasoner_model=reasoner,
  )

  task = asyncio.create_task(planner.run(_mission(), asyncio.Event()))
  await asyncio.wait_for(reasoner.started.wait(), timeout=1)
  for _ in range(20):
    if gate.calls:
      break
    await asyncio.sleep(0)
  assert len(gate.calls) == 1

  reasoner.release.set()
  assert await asyncio.wait_for(task, timeout=1) == "movement complete"
  settled = planner.last_plan()
  assert settled is not None
  assert [step.id for step in settled.plan] == ["move"]
  assert settled.plan[0].sentence == "Robot has moved to the revised target."
  assert any(
    annotation.event == "stale_plan_proposal" for annotation in settled.annotations
  )
  assert "Stale reasoner output for reference only" in str(
    controller.requests[-1][1].content
  )
  assert 'plan add obsolete "Outdated work."' in str(controller.requests[-1][1].content)


@pytest.mark.asyncio
async def test_reason_only_reply_waits_for_active_reasoner(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  controller = ScriptedModel(
    ["reason inspect needs comprehensive planning", "final plan updated"]
  )
  reasoner = ReleasableReasoner(
    'plan edit inspect "Inspect the obstacle comprehensively."'
  )
  planner, _ = _planner(
    controller,
    Admission(8),
    prompts,
    initial_plan=lambda _mission_id: [
      PlanStep(id="inspect", sentence="Inspect the obstacle.")
    ],
    reasoner_model=reasoner,
  )

  task = asyncio.create_task(planner.run(_mission("inspect"), asyncio.Event()))
  await asyncio.wait_for(reasoner.started.wait(), timeout=1)
  await asyncio.sleep(0)
  assert len(controller.requests) == 1

  reasoner.release.set()
  assert await asyncio.wait_for(task, timeout=1) == "plan updated"
  assert len(controller.requests) == 2


@pytest.mark.asyncio
async def test_reasoner_commit_invalidates_inflight_fast_reply(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  controller = BlockingThirdController(
    [
      'plan add inspect "Inspect the current obstacle."',
      (
        'plan edit inspect "Inspect the current obstacle carefully."\n'
        "reason complex obstacle layout needs comprehensive planning"
      ),
      'plan edit inspect "Obsolete fast-model revision."',
      "final current plan accepted",
    ]
  )
  reasoner = ReleasableReasoner(
    'plan edit inspect "Comprehensively inspect the obstacle layout."'
  )
  planner, _ = _planner(
    controller,
    Admission(8),
    prompts,
    reasoner_model=reasoner,
  )

  task = asyncio.create_task(planner.run(_mission("inspect safely"), asyncio.Event()))
  await asyncio.wait_for(reasoner.started.wait(), timeout=1)
  await asyncio.wait_for(controller.third_started.wait(), timeout=1)
  reasoner.release.set()
  await asyncio.sleep(0)
  controller.release_third.set()

  assert await asyncio.wait_for(task, timeout=1) == "current plan accepted"
  settled = planner.last_plan()
  assert settled is not None
  assert settled.plan[0].sentence == "Comprehensively inspect the obstacle layout."
  assert "Obsolete fast-model revision." not in settled.plan[0].sentence
  assert "Comprehensively inspect the obstacle layout." in str(
    controller.requests[-1][1].content
  )
  assert any(
    annotation.source == "controller" and annotation.event == "stale_plan_proposal"
    for annotation in settled.annotations
  )


@pytest.mark.asyncio
async def test_perception_stale_fast_output_reaches_next_reasoner(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  queue = Admission(8)
  controller = PerceptionUpdatingModel(
    [
      'plan add obsolete "Inspect the old obstacle layout."',
      "reason changed obstacle layout needs comprehensive planning",
      "final current plan accepted",
    ],
    queue,
  )
  reasoner = ScriptedModel(['plan add inspect "Inspect the current obstacle layout."'])
  planner, _ = _planner(controller, queue, prompts, reasoner_model=reasoner)

  assert await planner.run(_mission("inspect safely"), asyncio.Event()) == (
    "current plan accepted"
  )
  assert len(reasoner.requests) == 1
  reasoner_context = str(reasoner.requests[0][1].content)
  assert "Stale fast-model output for reference only" in reasoner_context
  assert 'plan add obsolete "Inspect the old obstacle layout."' in reasoner_context


@pytest.mark.asyncio
async def test_cancelling_planner_stops_both_model_lanes_and_clears_state(
  etc_root,
) -> None:
  prompts = load_prompt_config(etc_root)
  controller = BlockingThirdController(
    [
      'plan add inspect "Inspect the obstacle."',
      (
        'plan edit inspect "Inspect the obstacle carefully."\n'
        "reason obstacle needs comprehensive planning"
      ),
      'plan edit inspect "Late fast edit."',
    ]
  )
  reasoner = ReleasableReasoner('plan edit inspect "Reasoner edit."')
  planner, _ = _planner(
    controller,
    Admission(8),
    prompts,
    reasoner_model=reasoner,
  )

  task = asyncio.create_task(planner.run(_mission("inspect safely"), asyncio.Event()))
  await asyncio.wait_for(reasoner.started.wait(), timeout=1)
  await asyncio.wait_for(controller.third_started.wait(), timeout=1)
  task.cancel()

  with pytest.raises(asyncio.CancelledError):
    await asyncio.wait_for(task, timeout=1)
  assert controller.cancelled
  assert reasoner.cancelled
  assert planner.running_mission_id is None
  record = planner.last_plan()
  assert record is not None and record.outcome is StepOutcome.ABORTED


@pytest.mark.asyncio
async def test_cancelling_reasoner_only_wait_clears_waiters_and_state(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  controller = ScriptedModel(["reason mission needs comprehensive planning"])
  reasoner = ReleasableReasoner('plan add inspect "Inspect the obstacle."')
  planner, _ = _planner(
    controller,
    Admission(8),
    prompts,
    reasoner_model=reasoner,
  )

  task = asyncio.create_task(planner.run(_mission("inspect safely"), asyncio.Event()))
  await asyncio.wait_for(reasoner.started.wait(), timeout=1)
  task.cancel()

  with pytest.raises(asyncio.CancelledError):
    await asyncio.wait_for(task, timeout=1)
  await asyncio.sleep(0)
  assert reasoner.cancelled
  assert planner.running_mission_id is None


@pytest.mark.asyncio
async def test_execution_failure_returns_to_fast_controller(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  controller = ScriptedModel(
    [
      (
        'plan add move "Robot has moved."\nstep move\n'
        "base.velocity@default/base-0 linear_x_m_s=0.1 "
        "linear_y_m_s=0 angular_z_rad_s=0 valid_for_ms=100"
      ),
      "final motion safely stopped",
    ]
  )
  reasoner = ScriptedModel(["plan retry move"])
  gate = FakeGate(GateVerdict(admitted=False, reason="estop_engaged"))
  planner, _ = _planner(
    controller,
    Admission(8),
    prompts,
    gate=gate,
    reasoner_model=reasoner,
  )

  assert await planner.run(_mission(), asyncio.Event()) == "motion safely stopped"
  assert reasoner.requests == []
  assert len(controller.requests) == 2
  assert "rejected by gate (estop_engaged)" in str(controller.requests[1][1].content)


@pytest.mark.asyncio
async def test_fast_controller_can_delegate_execution_failure_repair(
  etc_root,
) -> None:
  prompts = load_prompt_config(etc_root)
  controller = ScriptedModel(
    [
      (
        'plan add move "Robot has moved."\nstep move\n'
        "base.velocity@default/base-0 linear_x_m_s=0.1 "
        "linear_y_m_s=0 angular_z_rad_s=0 valid_for_ms=100"
      ),
      "reason complex failure needs comprehensive repair",
      "final motion safely stopped",
    ]
  )
  reasoner = ScriptedModel([RuntimeError("reasoner unavailable"), "plan retry move"])
  gate = FakeGate(GateVerdict(admitted=False, reason="estop_engaged"))
  planner, _ = _planner(
    controller,
    Admission(8),
    prompts,
    gate=gate,
    config=DecisionConfig(
      max_steps=4,
      min_call_interval_s=0,
      model_retry_initial_s=0.001,
      model_retry_max_s=0.001,
    ),
    reasoner_model=reasoner,
  )

  assert await planner.run(_mission(), asyncio.Event()) == "motion safely stopped"
  assert len(reasoner.requests) == 2
  assert len(controller.requests) == 3
  assert reasoner.kwargs[-1]["reasoning_effort"] == "high"


@pytest.mark.asyncio
async def test_reasoner_tokens_share_the_mission_budget(etc_root) -> None:
  prompts = load_prompt_config(etc_root)
  controller = ScriptedModel(["reason mission needs a plan"])
  reasoner = ScriptedModel(['plan add inspect "Inspection is complete."'])
  planner, _ = _planner(
    controller,
    Admission(8),
    prompts,
    config=DecisionConfig(
      max_steps=4,
      min_call_interval_s=0,
      mission_token_budget=15,
    ),
    reasoner_model=reasoner,
  )

  with pytest.raises(MissionFailedError, match="mission token budget exhausted"):
    await planner.run(_mission("inspect"), asyncio.Event())

  assert len(controller.requests) == 1
  assert len(reasoner.requests) == 1
