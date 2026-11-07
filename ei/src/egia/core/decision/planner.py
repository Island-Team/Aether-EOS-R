"""Fast controller and asynchronous semantic reasoner for admitted Missions.

One mission, one loop: every request first reaches a mandatory non-reasoning
controller. It handles small Plan changes and ready execution or asks an
independent slow model for versioned semantic-only Plan edits. Primitive calls
inside one step may run concurrently, each through the safety gate
(``decision → safety → link``, invariant 1), and their acks re-enter as
``kind="result"`` queue observations (invariant 4). The planner never touches
``core/link`` or ``egia/contracts`` itself. Planner replies use only the
line-oriented command language; JSON is reserved for other model lanes and storage.

Plans as memory (``docs/egia/egia-2607.md`` §3.3): every mission end — answer,
failure, or shutdown abort — assembles a :class:`egia.api.plan.PlanRecord`
(one step per executed barrier, its actions, its plan edits, and the mission's
final semantic plan) and persists it through the episodic store. While the
mission runs, the in-flight record is UPSERTed into the working store after
every executed step (crash visibility; consolidation recovers rows orphaned
by a crash). The record is the mission's surviving artifact; ``run``'s return
value remains the decision_entry contract.

Recall before decomposition (§3.3): at mission start, before the first model
call, the planner queries the reference store, then the episodic store, for
plans matching the order and folds a compact digest into the user message —
promoted (reviewed) plans first, failed plans included as counter-examples.
The query runs through :class:`egia.core.memory.RecallService`, which merges
vector recall in when an index is configured and degrades to keyword search
otherwise; a "Relevant docs:" section of corpus excerpts follows when the
docs corpus is indexed. No matches, no section.

Memory-disabled mode (§3.2, ``[launch] memory = false``): when the stores
are passed as ``None`` the planner runs the pipeline by construction without
them — no recalls, no working-store checkpoints, no plan persistence. The
mission still runs to completion; only the memory artifacts are absent (the
failure paths that probe a closed store resolve as no-ops, never a crash).
"""

from __future__ import annotations

import asyncio
import json
import sqlite3
import time
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass
from datetime import UTC, datetime
from typing import Any, Literal
from uuid import NAMESPACE_URL, uuid4, uuid5

from jsonschema import Draft202012Validator
from jsonschema import ValidationError as JsonSchemaValidationError

from egia.api.mission import MissionRecord
from egia.api.model_io import ModelActivity, ModelExchange, ModelMessage
from egia.api.plan import (
  ActionRecord,
  ActiveAction,
  DecisionGraph,
  PlanAnnotation,
  PlanEdit,
  PlanEditKind,
  PlanRecord,
  PlanStep,
  PlanStepStatus,
  StepOutcome,
  StepRecord,
)
from egia.contracts.endpoints import OPERATOR_PRIMITIVE_KINDS
from egia.core.context import (
  ContextBuilder,
  render_dynamic_message,
  render_lane_sections,
)
from egia.core.decision.command import (
  CommandReply,
  parse_command_reply,
  parse_primitive_command,
)
from egia.core.decision.policy import InvocationPolicy, InvocationTrigger
from egia.core.decision.sentinel import SafetySentinel
from egia.core.memory import EpisodicStore, RecallService, ReferenceStore, WorkingStore
from egia.core.perception.admission import Admission
from egia.core.registry import PrimitiveRegistry
from egia.core.safety import SafetyGate
from egia.internal.decision import ActionInvocation, StepContext
from egia.internal.model import DecisionModel
from egia.internal.skills import SkillClients, SkillExecution
from egia.lib.config import AgentPromptConfig, DecisionConfig
from egia.lib.logger import get_logger
from egia.lib.vlm import ChatMessage, ModelStreamDelta, ReasoningEffort

RESULT_OBSERVATION_KIND = "result"
DECISION_SOURCE = "decision"
_VOLATILE_ACTION_METADATA = frozenset({"source_sequence", "source_timestamp"})
_CONTINUOUS_ACTION_CONTEXT_KINDS = frozenset({"body"})
_MODEL_CALL_INTERRUPTED = object()

__all__ = [
  "DecisionPlanner",
  "MissionFailedError",
  "MissionPreemptedError",
  "ReActPlanner",
]


class MissionFailedError(Exception):
  """The mission cannot conclude: step cap, repeated parse failure, shutdown."""


class MissionPreemptedError(Exception):
  """The mission yielded at a step boundary to a higher-priority one (§3.9).

  The plan record is saved ``aborted`` with the preemption annotated; the
  mission returns to the table through ``MissionQueue.resubmit`` — the
  scheduler, not the planner, owns whether it runs again.
  """


@dataclass
class _BufferedStep:
  """One model-accepted barrier waiting for model-free execution.

  Every step carries the completion's token, retry, and planning cost. Its
  primitive lines may run concurrently only after the complete barrier passes
  primitive and safety validation.
  """

  invocations: list[ActionInvocation]
  plan_edits: list[PlanEdit]
  plan_step_id: str | None = None
  commands: list[str] | None = None
  prompt_tokens: int = 0
  completion_tokens: int = 0
  retries: int = 0
  planning_duration_s: float = 0.0


@dataclass
class _ReasoningProposal:
  """One versioned semantic-plan proposal from the slow lane."""

  edits: list[PlanEdit]
  base_generation: int
  context_fingerprint: tuple
  raw_reply: str


@dataclass
class _PeerReference:
  """One stale peer output retained for the opposite lane's next request."""

  source: Literal["controller", "reasoner"]
  base_generation: int
  current_generation: int
  raw_reply: str

  def render(self) -> str:
    """Render obsolete output as bounded advisory context, never executable input."""
    return (
      f"source={self.source} base_generation={self.base_generation} "
      f"current_generation={self.current_generation}\n"
      "This output lost the Plan compare-and-swap race. Treat it only as a "
      "reference and re-evaluate it against the current Mission and Plan:\n"
      + self.raw_reply[:4000]
    )


@dataclass
class _ReasoningSettlement:
  """Validated outcome of one reasoner compare-and-swap attempt."""

  plan_steps: list[PlanStep] | None = None
  stale_reference: _PeerReference | None = None
  error: str | None = None


def _settle_reasoning_proposal(
  proposal: _ReasoningProposal,
  *,
  plan_generation: int,
  context_fingerprint: tuple,
  plan_steps: list[PlanStep],
) -> _ReasoningSettlement:
  """Validate one reasoner result without mutating live planner state."""
  if (
    proposal.base_generation != plan_generation
    or proposal.context_fingerprint != context_fingerprint
  ):
    return _ReasoningSettlement(
      stale_reference=_PeerReference(
        source="reasoner",
        base_generation=proposal.base_generation,
        current_generation=plan_generation,
        raw_reply=proposal.raw_reply,
      )
    )
  try:
    revised = _apply_plan_edits(plan_steps, proposal.edits)
  except ValueError as exc:
    return _ReasoningSettlement(error=str(exc))
  return _ReasoningSettlement(plan_steps=revised)


async def _cancel_tasks(*tasks: asyncio.Task | None) -> None:
  """Cancel and join the supplied model-lane tasks without leaking children."""
  active = [task for task in tasks if task is not None]
  for task in active:
    task.cancel()
  if active:
    await asyncio.gather(*active, return_exceptions=True)


class DecisionPlanner:
  """Drive one Mission through a fast controller and optional slow reasoner."""

  def __init__(
    self,
    model: DecisionModel,
    context: ContextBuilder,
    registry: PrimitiveRegistry,
    gate: SafetyGate,
    queue: Admission,
    episodic: EpisodicStore | None,
    working: WorkingStore | None,
    reference: ReferenceStore | None,
    prompts: AgentPromptConfig,
    config: DecisionConfig,
    recall: RecallService | None = None,
    exchange_sink: Callable[[ModelExchange], None] | None = None,
    activity_sink: Callable[[ModelActivity], None] | None = None,
    should_yield: Callable[[MissionRecord], MissionRecord | None] | None = None,
    plan_sink: Callable[[PlanRecord, bool], None] | None = None,
    sentinel: SafetySentinel | None = None,
    resume_plan: Callable[[str], PlanRecord | None] | None = None,
    initial_plan: Callable[[str], list[PlanStep]] | None = None,
    reasoner_model: DecisionModel | None = None,
    reasoner_prompts: AgentPromptConfig | None = None,
    summarizer_prompts: AgentPromptConfig | None = None,
    skill_clients: SkillClients | None = None,
  ) -> None:
    self._model = model
    self._context = context
    self._registry = registry
    self._gate = gate
    self._queue = queue
    self._episodic = episodic
    self._working = working
    self._reference = reference
    self._recall = recall or (
      RecallService(episodic, reference)
      if episodic is not None and reference is not None
      else None
    )
    self._prompts = prompts
    self._config = config
    self._exchange_sink = exchange_sink
    self._activity_sink = activity_sink
    self._should_yield = should_yield
    self._plan_sink = plan_sink
    self._sentinel = sentinel
    self._resume_plan = resume_plan
    self._initial_plan = initial_plan
    self._reasoner_model = reasoner_model
    self._reasoner_prompts = reasoner_prompts
    self._summarizer_prompts = summarizer_prompts
    self._skill_clients = skill_clients
    self._policy = self._new_policy()
    self._log = get_logger("egia.core.decision.planner")
    self._annotations: list[PlanAnnotation] = []
    self._running_mission_id: str | None = None
    self._running_started_at: datetime | None = None
    self._live_plan: PlanRecord | None = None
    self._last_plan: PlanRecord | None = None
    self._exchange_counter = 0
    self._activity_counter = 0
    self._reasoning_options: tuple[ReasoningEffort, ...] = tuple(
      getattr(model, "reasoning_options", ())
    )
    self._default_reasoning_effort: ReasoningEffort | None = getattr(
      model, "reasoning_effort", None
    )
    self._reasoner_options: tuple[ReasoningEffort, ...] = tuple(
      getattr(reasoner_model, "reasoning_options", ())
    )

  @property
  def running_mission_id(self) -> str | None:
    """The mission currently executing, or ``None`` between missions.

    Side loops (audit, monitor) poll this to decide whether their findings
    concern a live plan; read-only, set only by :meth:`run`.
    """
    return self._running_mission_id

  @property
  def running_mission_started_at(self) -> datetime | None:
    """Wall-clock start of the running mission, or ``None`` between missions.

    The stall detector floors its window here (§3.6): a mission whose first
    model call is still in flight has produced no primitive result by
    definition, and inference latency is not a stall.
    """
    return self._running_started_at

  def current_plan(self) -> PlanRecord | None:
    """A snapshot of the in-flight plan, or ``None`` between missions.

    Read-only view for the operator plan projection (§3.3): a ``model_copy``
    of the record as of the last executed step, so callers never observe the
    planner's mutable lists mid-step.
    """
    if self._live_plan is None:
      return None
    return self._live_plan.model_copy()

  def last_plan(self) -> PlanRecord | None:
    """Return the last settled plan, including a promotion-time abort."""
    if self._last_plan is None:
      return None
    return self._last_plan.model_copy(deep=True)

  def _new_policy(self) -> InvocationPolicy:
    return InvocationPolicy(
      token_budget=self._config.mission_token_budget,
      min_call_interval_s=self._config.min_call_interval_s,
    )

  def request_interrupt(self, source: str, detail: str) -> None:
    """Ask for priority model re-entry at the next step boundary (§3.8).

    No-op when no mission is running — idle model calls are exactly what the
    policy exists to prevent; emergencies below the step granularity belong
    to the safety estop path, not to this one.
    """
    if self._running_mission_id is None:
      return
    self._policy.request_interrupt(source, detail)

  def annotate(self, source: str, event: str, detail: str = "") -> None:
    """Stamp a side-loop verdict onto the in-flight plan (§3.3).

    No-op when no mission is running; annotations arriving after the plan
    is saved are dropped — the record is immutable once persisted.
    """
    if self._running_mission_id is None:
      return
    self._annotations.append(PlanAnnotation(source=source, event=event, detail=detail))

  async def run(  # pyright: ignore[reportGeneralTypeIssues]
    self, mission: MissionRecord, stop: asyncio.Event
  ) -> str:
    """Plan and execute one named primitive barrier at a time.

    Each accepted step crosses a common cancellation and freshness boundary,
    then runs through the safety gate. Its results return to the next model
    request before another step can be accepted. Every exit path persists only
    barriers that actually ran.
    """
    started_at = datetime.now(UTC)
    resumed = self._resume_plan(mission.id) if self._resume_plan is not None else None
    direct_invocation = (
      parse_primitive_command(mission.order)
      if resumed is None and mission.mission_template_id is None
      else None
    )
    steps: list[StepRecord] = (
      [step.model_copy(deep=True) for step in resumed.steps] if resumed else []
    )
    active_actions = (
      [item.model_copy(deep=True) for item in resumed.active_actions]
      if resumed is not None
      else []
    )
    resumed_step_count = len(steps)
    history: list[str] = []
    mission_summary: str | None = None
    summary_attempt_signature: int | None = None
    plan_steps = (
      [step.model_copy(deep=True) for step in resumed.plan]
      if resumed is not None and resumed.plan
      else (
        [step.model_copy(deep=True) for step in self._initial_plan(mission.id)]
        if self._initial_plan is not None
        else []
      )
    )
    if resumed is not None:
      history.append(
        "MISSION RESTORED: the following barriers already settled before "
        "the runtime resumed. Do not repeat any acknowledged action; inspect "
        "fresh state and replan only the remaining mission work."
      )
      interrupted = [
        step for step in plan_steps if step.status is PlanStepStatus.RUNNING
      ]
      if interrupted:
        from egia.lib import shared_events

        shared_events.emit(
          "egia_recovery_required",
          fields={"reason_code": "checkpoint_incomplete", "mission_id": mission.id},
        )
        commands = "; ".join(
          command for step in interrupted for command in step.commands
        )
        history.append(
          "RECOVERY WARNING: the runtime stopped while a barrier may have "
          "been executing. Its outcome is unknown. Inspect current state "
          "before deciding whether any action is still needed."
          + (f" Interrupted commands: {commands}" if commands else "")
        )
        for step in interrupted:
          step.status = PlanStepStatus.VERIFYING
      for step in resumed.steps:
        actions = (
          ", ".join(
            f"{action.action}({action.arguments})={action.outcome.value}"
            + (f" ({action.reason})" if action.reason else "")
            for action in step.actions
          )
          or "no action"
        )
        history.append(f"settled barrier {step.index}: {actions}")
    decision_mode: Literal["plan", "execute"] = "execute"
    decision_graph: DecisionGraph | None = None
    pending: deque[_BufferedStep] = deque()
    reasoning_task: asyncio.Task[_ReasoningProposal] | None = None
    plan_generation = 0
    forced_reason: str | None = None
    reason_only_wait = False
    peer_for_controller: _PeerReference | None = None
    peer_for_reasoner: _PeerReference | None = None
    parse_failures = 0
    answer: str | None = None
    error: str | None = None
    outcome = StepOutcome.FAILED
    self._annotations = (
      [annotation.model_copy(deep=True) for annotation in resumed.annotations]
      if resumed is not None
      else []
    )
    if resumed is not None:
      self._annotations.append(
        PlanAnnotation(
          source="missions",
          event="resumed_from",
          detail=resumed.id,
        )
      )
    self._running_mission_id = mission.id
    self._running_started_at = started_at
    self._live_plan = PlanRecord(
      id=str(uuid4()),
      mission_id=mission.id,
      priority=mission.priority,
      order=mission.order,
      source=mission.source,
      started_at=started_at,
      finished_at=started_at,
      outcome=StepOutcome.ABORTED,
      steps=[step.model_copy(deep=True) for step in steps],
      plan=[step.model_copy(deep=True) for step in plan_steps],
      decision_mode=decision_mode,
      annotations=list(self._annotations),
      revision=(resumed.revision + 1 if resumed is not None else 1),
    )
    if self._plan_sink is not None:
      self._plan_sink(self._live_plan.model_copy(deep=True), True)
    recall = self._recall_prior_plans(mission.order)
    self._policy = self._new_policy()
    trigger = InvocationTrigger.MISSION_START
    preempted_by: MissionRecord | None = None

    def invalidate_pending(source: str, detail: str) -> None:
      """Discard only work that has not crossed a barrier boundary."""
      if not pending:
        return
      discarded = len(pending)
      pending.clear()
      self.annotate(source, "plan_step_invalidated", detail)
      self._log.info(
        "plan_step_invalidated",
        mission_id=mission.id,
        source=source,
        discarded_steps=discarded,
        detail=detail,
      )

    async def execute(buffered: _BufferedStep) -> StepRecord:
      nonlocal plan_generation, plan_steps
      step_index = len(steps) + 1
      selected = next(
        (item for item in plan_steps if item.id == buffered.plan_step_id), None
      )
      if selected is not None:
        for item in plan_steps:
          if item is not selected and item.status is PlanStepStatus.RUNNING:
            item.status = PlanStepStatus.PENDING
            item.commands = []
        selected.status = PlanStepStatus.RUNNING
        selected.commands = list(buffered.commands or [])
      # Write the accepted barrier before egress. If the process dies during
      # the backend call, restart recovery sees RUNNING and treats its outcome
      # as unknown instead of blindly replaying it.
      self._checkpoint_working(
        mission,
        started_at,
        steps,
        plan_steps,
        decision_mode=decision_mode,
        decision_graph=decision_graph,
      )
      record, outcome_parts = await self._execute_buffered_step(
        mission,
        step_index,
        buffered,
        selected,
        active_actions=active_actions,
        checkpoint=lambda: self._checkpoint_working(
          mission,
          started_at,
          steps,
          plan_steps,
          decision_mode=decision_mode,
          decision_graph=decision_graph,
          active_actions=active_actions,
        ),
      )
      if selected is not None:
        selected.status = (
          PlanStepStatus.VERIFYING
          if record.outcome is StepOutcome.SUCCEEDED and selected.required
          else PlanStepStatus.DONE
          if record.outcome is StepOutcome.SUCCEEDED
          else PlanStepStatus.FAILED
        )
        selected.reason = record.reason
      steps.append(record)
      history.append(f"step {step_index}: " + "; ".join(outcome_parts))
      self._checkpoint_working(
        mission,
        started_at,
        steps,
        plan_steps,
        decision_mode=decision_mode,
        decision_graph=decision_graph,
      )
      # Every settled barrier changes the facts a semantic proposal may rely
      # on, including successful verification transitions and fresh results.
      plan_generation += 1
      if record.outcome is not StepOutcome.SUCCEEDED and pending:
        # Later barriers may depend on this one's world transition. Once it
        # fails, their preconditions are no longer established, so return the
        # observed failure to the fast refill lane instead of blindly draining
        # stale work.
        discarded = len(pending)
        detail = (
          f"step {step_index} ended {record.outcome.value}; "
          f"discarded {discarded} dependent barrier(s)"
        )
        history.append(f"PLAN INVALIDATED: {detail}")
        invalidate_pending("executor", detail)
      return record

    if active_actions:
      for active in list(active_actions):
        recovered = await self._resume_skill_execution(
          mission,
          active,
          checkpoint=lambda: self._checkpoint_working(
            mission,
            started_at,
            steps,
            plan_steps,
            decision_mode=decision_mode,
            decision_graph=decision_graph,
            active_actions=active_actions,
          ),
        )
        active_actions.remove(active)
        if recovered is not None:
          steps.append(
            StepRecord(
              index=len(steps) + 1,
              outcome=recovered.outcome,
              reason=recovered.reason,
              actions=[recovered],
            )
          )
        self._checkpoint_working(
          mission,
          started_at,
          steps,
          plan_steps,
          decision_mode=decision_mode,
          decision_graph=decision_graph,
          active_actions=active_actions,
        )

    if direct_invocation is not None and not plan_steps:
      step_id = "direct"
      direct_edit = PlanEdit(
        op=PlanEditKind.ADD,
        step_id=step_id,
        sentence="Submit the admitted primitive command to EOS.",
      )
      plan_steps = _apply_plan_edits(plan_steps, [direct_edit])
      direct_error = _invalid_action_primitive(
        CommandReply(
          plan_edits=[direct_edit],
          actions=[direct_invocation],
          final=None,
          next_reasoning_effort=None,
        ),
        self._registry,
        self._skill_clients,
      )
      if direct_error is not None:
        error = f"invalid direct primitive command: {direct_error}"
        self.annotate("registry", "direct_command_rejected", direct_error)
        self._log.warning(
          "direct_mission_rejected",
          mission_id=mission.id,
          reason=direct_error,
        )
      else:
        pending.append(
          _BufferedStep(
            invocations=[direct_invocation],
            plan_edits=[direct_edit],
            plan_step_id=step_id,
            commands=[mission.order],
          )
        )
        self._log.info(
          "direct_mission_fast_path",
          mission_id=mission.id,
          action=direct_invocation.action,
          robot_id=direct_invocation.robot_id,
          device_id=direct_invocation.device_id,
        )

    def settle_reasoner() -> bool:
      """Commit one completed reasoner proposal if its Plan snapshot still matches."""
      nonlocal decision_mode, error, outcome, peer_for_controller, reason_only_wait
      nonlocal plan_generation, plan_steps
      nonlocal reasoning_task
      assert reasoning_task is not None and reasoning_task.done()
      task = reasoning_task
      reasoning_task = None
      reason_only_wait = False
      try:
        proposal = task.result()
      except Exception as exc:  # noqa: BLE001 - isolated model lane
        history.append(f"REASONER FAILED: {exc}")
        self.annotate("reasoner", "proposal_failed", str(exc))
        if isinstance(exc, MissionFailedError) and "budget exhausted" in str(exc):
          outcome = StepOutcome.FAILED
          error = str(exc)
        decision_mode = "execute"
        return False
      settlement = _settle_reasoning_proposal(
        proposal,
        plan_generation=plan_generation,
        context_fingerprint=self._action_context_fingerprint(),
        plan_steps=plan_steps,
      )
      if settlement.stale_reference is not None:
        peer_for_controller = settlement.stale_reference
        history.append(
          "REASONER STALE: retained outdated proposal as fast-model reference"
        )
        self.annotate("reasoner", "stale_plan_proposal", "plan or perception changed")
        decision_mode = "execute"
        return False
      if settlement.error is not None:
        history.append(f"REASONER INVALID: {settlement.error}")
        self.annotate("reasoner", "proposal_invalid", settlement.error)
        decision_mode = "execute"
        return False
      assert settlement.plan_steps is not None
      plan_steps = settlement.plan_steps
      plan_generation += 1
      decision_mode = "execute"
      history.append(f"reasoner revised plan: {_describe_plan_edits(proposal.edits)}")
      self._checkpoint_working(
        mission,
        started_at,
        steps,
        plan_steps,
        decision_mode=decision_mode,
        decision_graph=decision_graph,
      )
      return True

    cancelled = False
    try:
      while self._config.max_steps == 0 or (
        len(steps) - resumed_step_count < self._config.max_steps
      ):
        if error is not None:
          break
        if stop.is_set():
          outcome = StepOutcome.ABORTED
          error = "shutdown during mission"
          break

        if parse_failures >= self._config.max_invalid_replies:
          outcome = StepOutcome.FAILED
          detail = history[-1] if history else "invalid controller reply"
          error = (
            "invalid controller reply limit reached "
            f"({self._config.max_invalid_replies}): {detail}"
          )
          self.annotate("controller", "invalid_reply_limit", detail)
          self._log.warning(
            "invalid_reply_limit_reached",
            mission_id=mission.id,
            attempts=parse_failures,
            reason=detail,
          )
          break

        if reasoning_task is not None and reasoning_task.done():
          if settle_reasoner():
            invalidate_pending(
              "reasoner", "reasoner committed a newer Plan before egress"
            )
          if error is not None:
            break

        if forced_reason is not None and reasoning_task is None:
          if self._reasoner_model is None or self._reasoner_prompts is None:
            history.append(f"reasoning unavailable: {forced_reason}")
            decision_mode = "execute"
            reason_only_wait = False
          else:
            decision_mode = "plan"
            reasoning_task = asyncio.create_task(
              self._request_reasoning(
                mission,
                list(history),
                recall,
                list(plan_steps),
                forced_reason,
                plan_generation,
                mission_summary=mission_summary,
                peer_reference=(
                  peer_for_reasoner.render() if peer_for_reasoner is not None else None
                ),
              ),
              name=f"egia-reasoner-{mission.id}",
            )
            peer_for_reasoner = None
            self.annotate("controller", "reasoning_requested", forced_reason)
            self._checkpoint_working(
              mission,
              started_at,
              steps,
              plan_steps,
              decision_mode=decision_mode,
              decision_graph=decision_graph,
            )
          forced_reason = None

        if reasoning_task is not None and (
          reason_only_wait or not _has_ready_step(plan_steps)
        ):
          wait_result = await self._wait_for_reasoner(reasoning_task, stop)
          if wait_result == "stop":
            outcome = StepOutcome.ABORTED
            error = "shutdown while waiting for reasoning"
            break
          if wait_result == "interrupt":
            reasoning_task.cancel()
            await asyncio.gather(reasoning_task, return_exceptions=True)
            reasoning_task = None
            reason_only_wait = False
          continue
        if self._should_yield is not None:
          winner = self._should_yield(mission)
          if winner is not None:
            preempted_by = winner
            outcome = StepOutcome.ABORTED
            error = f"preempted by mission {winner.id}"
            self._annotations.append(
              PlanAnnotation(source="scheduler", event="preempted_by", detail=winner.id)
            )
            break

        interrupt = self._policy.pop_interrupt()
        if interrupt is not None:
          source, detail = interrupt
          history.append(
            f"INTERRUPT ({source}): {detail} — re-evaluate your remaining plan"
          )
          self.annotate(source, "model_interrupt", detail)
          trigger = InvocationTrigger.FAULT_INTERRUPT
          invalidate_pending(source, f"interrupt: {detail}")
          continue

        # Accepted work is a model-free execution lane. It drains immediately
        # at barrier boundaries; cooldowns, model retries, and reasoning cost
        # cannot pause a next step that already exists.
        if pending:
          buffered = pending.popleft()
          record = await execute(buffered)
          if _direct_mission_complete(
            direct_invocation, buffered, record, steps, plan_steps, pending
          ):
            action = record.actions[0]
            outcome = StepOutcome.SUCCEEDED
            answer = f"{mission.order} -> {action.result or 'accepted'}"
            break
          trigger = InvocationTrigger.STEP
          continue

        reasoning_effort = self._controller_reasoning_effort()
        decision = self._policy.admit(trigger)
        waited_for_policy = False
        while not decision.call and decision.wait_s > 0:
          # This delays plan production only; an accepted step would have run
          # in the branch above before policy was consulted.
          wait_result = await self._wait_for_policy_delay(decision.wait_s, stop)
          waited_for_policy = True
          if wait_result == "stop":
            outcome = StepOutcome.ABORTED
            error = "shutdown during policy cooldown"
            break
          decision = self._policy.admit(trigger)
        if error is not None:
          break
        if not decision.call:
          outcome = StepOutcome.FAILED
          error = decision.reason
          self.annotate("policy", "invocation_denied", decision.reason)
          break
        if waited_for_policy:
          # The cooldown is also an await boundary. Refresh all mission/world
          # framing before spending a model call.
          if stop.is_set():
            outcome = StepOutcome.ABORTED
            error = "shutdown during mission"
            break
          if self._should_yield is not None:
            winner = self._should_yield(mission)
            if winner is not None:
              preempted_by = winner
              outcome = StepOutcome.ABORTED
              error = f"preempted by mission {winner.id}"
              self._annotations.append(
                PlanAnnotation(
                  source="scheduler", event="preempted_by", detail=winner.id
                )
              )
              break
          late_interrupt = self._policy.pop_interrupt()
          if late_interrupt is not None:
            source, detail = late_interrupt
            history.append(
              f"INTERRUPT ({source}): {detail} — re-evaluate your remaining plan"
            )
            self.annotate(source, "model_interrupt", detail)
            trigger = InvocationTrigger.FAULT_INTERRUPT
            continue

        summary_signature = hash(
          (_render_execution_history(history), _render_plan_block(plan_steps))
        )
        if (
          summary_signature != summary_attempt_signature
          and self._mission_context_needs_summary(
            mission, mission_summary, history, plan_steps
          )
        ):
          summary_attempt_signature = summary_signature
          compressed = await self._summarize_mission_context(
            mission, mission_summary, history, plan_steps
          )
          if compressed is not None:
            mission_summary = compressed
            history.clear()
            summary_attempt_signature = None

        messages = self._build_messages(
          mission,
          list(history),
          recall,
          list(plan_steps),
          mission_summary=mission_summary,
          decision_mode=decision_mode,
          decision_graph=decision_graph,
          reasoning_effort=reasoning_effort or self._default_reasoning_effort,
          peer_reference=(
            peer_for_controller.render() if peer_for_controller is not None else None
          ),
        )
        peer_for_controller = None
        request_generation = plan_generation
        request_fingerprint = self._action_context_fingerprint()

        completion_started = time.monotonic()
        exchange_id = self._exchange_counter
        self._exchange_counter += 1
        stream_text = {"reply": "", "thinking": "", "model": ""}
        stream_active = [True]
        stream_step = len(steps) + 1
        stream_loop = asyncio.get_running_loop()

        def apply_delta(
          delta: ModelStreamDelta,
          *,
          text=stream_text,
          eid=exchange_id,
          call_messages=messages,
          started=completion_started,
          effort=reasoning_effort or self._default_reasoning_effort,
          mission_id=mission.id,
          step=stream_step,
        ) -> None:
          key = "thinking" if delta.kind == "reasoning" else "reply"
          text[key] += delta.text
          if delta.model:
            text["model"] = delta.model
          self._publish_stream_exchange(
            eid,
            mission_id,
            step,
            call_messages,
            started,
            text,
            reasoning_effort=effort,
          )

        def publish_delta(
          delta: ModelStreamDelta,
          *,
          active=stream_active,
          loop=stream_loop,
        ) -> None:
          """Marshal worker-thread SSE chunks onto the planner event loop."""
          if active[0]:
            loop.call_soon_threadsafe(apply_delta, delta)

        self._publish_stream_exchange(
          exchange_id,
          mission.id,
          len(steps) + 1,
          messages,
          completion_started,
          stream_text,
          reasoning_effort=reasoning_effort or self._default_reasoning_effort,
        )
        self._log.info(
          "model_request_started",
          mission_id=mission.id,
          exchange_id=exchange_id,
          step=len(steps) + 1,
          trigger=trigger.value,
          phase=decision_mode,
          prompt_chars=sum(
            len(message.content) if isinstance(message.content, str) else 0
            for message in messages
          ),
          history_items=len(history),
          plan_steps=len(plan_steps),
          reasoning_effort=reasoning_effort or self._default_reasoning_effort,
        )
        controller_reservation = self._config.controller.max_tokens
        self._policy.begin_call(controller_reservation)
        controller_task: asyncio.Task[Any | None] | None = None
        try:
          controller_task = asyncio.create_task(
            self._complete_with_retry(
              messages,
              reasoning_effort,
              stop,
              goal=mission.order,
              on_delta=publish_delta,
            ),
            name=f"egia-controller-{mission.id}",
          )
          if reasoning_task is not None:
            done, _ = await asyncio.wait(
              {controller_task, reasoning_task},
              return_when=asyncio.FIRST_COMPLETED,
            )
            if reasoning_task in done:
              # A valid reasoner commit wins a simultaneous completion. The
              # obsolete fast request is allowed to drain because some model
              # transports cannot cancel their underlying worker thread; its
              # fenced reply can never mutate the Plan or reach egress.
              settle_reasoner()
              if request_generation != plan_generation:
                stream_active[0] = False
          response = await controller_task
        except BaseException:
          stream_active[0] = False
          await _cancel_tasks(controller_task, reasoning_task)
          reasoning_task = None
          self._policy.cancel_call(controller_reservation)
          raise
        # Drain chunks queued just before the worker completed so the final
        # frame cannot race ahead of its last partial update.
        await asyncio.sleep(0)
        stream_active[0] = False
        if reasoning_task is not None and reasoning_task.done():
          settle_reasoner()
        if response is _MODEL_CALL_INTERRUPTED:
          self._policy.cancel_call(controller_reservation)
          self._publish_stream_exchange(
            exchange_id,
            mission.id,
            len(steps) + 1,
            messages,
            completion_started,
            stream_text,
            status="interrupted",
            reasoning_effort=reasoning_effort or self._default_reasoning_effort,
          )
          interrupt = self._policy.pop_interrupt()
          if interrupt is None:
            # The event is level-triggered and this coroutine is the sole
            # consumer, so this is defensive rather than an expected path.
            trigger = InvocationTrigger.FAULT_INTERRUPT
            continue
          source, detail = interrupt
          history.append(f"INTERRUPT ({source}): {detail} — cancelled stale model call")
          self.annotate(source, "model_interrupt", detail)
          trigger = InvocationTrigger.FAULT_INTERRUPT
          continue
        if response is None:
          self._policy.cancel_call(controller_reservation)
          self._publish_stream_exchange(
            exchange_id,
            mission.id,
            len(steps) + 1,
            messages,
            completion_started,
            stream_text,
            status="interrupted",
            reasoning_effort=reasoning_effort or self._default_reasoning_effort,
          )
          outcome = StepOutcome.ABORTED
          error = "shutdown during model call"
          break
        self._policy.settle_call(_usage_total(response.usage), controller_reservation)
        self._publish_exchange(
          mission.id,
          len(steps) + 1,
          messages,
          response,
          completion_started,
          exchange_id=exchange_id,
          reasoning_effort=reasoning_effort or self._default_reasoning_effort,
        )
        reply_preview, reply_truncated = _log_excerpt(response.text)
        self._log.info(
          "model_reply_received",
          mission_id=mission.id,
          exchange_id=exchange_id,
          step=len(steps) + 1,
          duration_s=round(time.monotonic() - completion_started, 3),
          finish_reason=response.finish_reason,
          reply_chars=len(response.text),
          reply_preview=reply_preview,
          truncated=reply_truncated,
        )

        if error is not None:
          break

        # Inference is an await boundary. Never accept a reply against a
        # mission or world that changed while the model was thinking.
        if stop.is_set():
          outcome = StepOutcome.ABORTED
          error = "shutdown during mission"
          break
        if self._should_yield is not None:
          winner = self._should_yield(mission)
          if winner is not None:
            preempted_by = winner
            outcome = StepOutcome.ABORTED
            error = f"preempted by mission {winner.id}"
            self._annotations.append(
              PlanAnnotation(source="scheduler", event="preempted_by", detail=winner.id)
            )
            break
        interrupt = self._policy.pop_interrupt()
        if interrupt is not None:
          source, detail = interrupt
          history.append(
            f"INTERRUPT ({source}): {detail} — discarded stale model reply"
          )
          self.annotate(source, "model_interrupt", detail)
          trigger = InvocationTrigger.FAULT_INTERRUPT
          continue
        if request_generation != plan_generation:
          peer_for_reasoner = _PeerReference(
            source="controller",
            base_generation=request_generation,
            current_generation=plan_generation,
            raw_reply=response.text,
          )
          detail = "reasoner changed the Plan while the fast model was responding"
          history.append(f"FAST MODEL STALE: {detail}; output retained for reasoner")
          self.annotate("controller", "stale_plan_proposal", detail)
          trigger = InvocationTrigger.STEP
          continue
        if request_fingerprint != self._action_context_fingerprint():
          detail = "perception changed while the model was planning"
          peer_for_reasoner = _PeerReference(
            source="controller",
            base_generation=request_generation,
            current_generation=plan_generation,
            raw_reply=response.text,
          )
          history.append(f"PLAN INVALIDATED: {detail}")
          self.annotate("perception", "stale_model_reply", detail)
          self._log.info("stale_model_reply", mission_id=mission.id)
          trigger = InvocationTrigger.FAULT_INTERRUPT
          continue

        parsed = _parse_reply(response.text)
        if parsed is None:
          parse_failures += 1
          self._log.warning(
            "step_parse_failed",
            mission_id=mission.id,
            attempt=parse_failures,
            reply_chars=len(response.text),
            reply_preview=reply_preview,
          )
          history.append(
            f"step {len(steps) + 1}: your reply was not a valid command; "
            "use one documented command per line and no prose"
          )
          trigger = InvocationTrigger.PARSE_RETRY
          continue
        self._log.info(
          "model_reply_adapted",
          mission_id=mission.id,
          exchange_id=exchange_id,
          syntax="command",
          kind=_reply_kind(parsed),
          barriers=int(bool(parsed.actions)),
          actions=len(parsed.actions),
        )
        invalid_action = _invalid_action_primitive(
          parsed, self._registry, self._skill_clients
        )
        if invalid_action is not None:
          parse_failures += 1
          self._log.warning(
            "step_action_rejected",
            mission_id=mission.id,
            attempt=parse_failures,
            reason=invalid_action,
          )
          history.append(
            f"step {len(steps) + 1}: invalid primitive reply ({invalid_action}); "
            "use only listed primitives"
          )
          trigger = InvocationTrigger.PARSE_RETRY
          continue
        repeated_action = _repeated_successful_action(
          parsed, _reply_step_sentence(parsed, plan_steps), steps
        )
        if repeated_action is not None:
          parse_failures += 1
          detail = (
            "equivalent action and outcome already succeeded in this Mission: "
            f"{repeated_action.command}"
          )
          self._log.warning(
            "step_action_replay_rejected",
            mission_id=mission.id,
            attempt=parse_failures,
            reason=detail,
          )
          unfinished = any(
            step.status not in {PlanStepStatus.DONE, PlanStepStatus.FAILED}
            for step in plan_steps
          )
          correction = (
            "The Mission has no unfinished steps. Reply with exactly one line: "
            "final <supported outcome>. Do not emit plan edits, step, or a primitive."
            if not unfinished
            else "Do not repeat this outcome; select only distinct unfinished work."
          )
          history.append(f"REPLAY REJECTED: {detail}. {correction}")
          trigger = InvocationTrigger.PARSE_RETRY
          continue
        try:
          preview_plan = _apply_plan_edits(plan_steps, parsed.plan_edits)
        except ValueError as exc:
          parse_failures += 1
          history.append(f"invalid plan edit: {exc}")
          trigger = InvocationTrigger.PARSE_RETRY
          continue
        if parsed.plan_edits and parsed.final is None and preview_plan == plan_steps:
          parse_failures += 1
          history.append("invalid plan edit: reply does not change the current Plan")
          trigger = InvocationTrigger.PARSE_RETRY
          continue
        if parsed.actions:
          selected = next(
            (item for item in preview_plan if item.id == parsed.plan_step_id), None
          )
          if selected is None:
            parse_failures += 1
            history.append(
              "invalid step: `step <id>` must name a plan step added before "
              "its commands"
            )
            trigger = InvocationTrigger.PARSE_RETRY
            continue
          if selected.status is not PlanStepStatus.PENDING:
            parse_failures += 1
            history.append(
              f"invalid step: plan step {selected.id!r} is "
              f"{selected.status.value}, not pending"
            )
            trigger = InvocationTrigger.PARSE_RETRY
            continue
          by_id = {item.id: item for item in preview_plan}
          unknown_dependencies = [
            dependency for dependency in selected.after if dependency not in by_id
          ]
          if unknown_dependencies:
            parse_failures += 1
            history.append(
              f"invalid step: plan step {selected.id!r} has unknown dependencies "
              + ", ".join(unknown_dependencies)
            )
            trigger = InvocationTrigger.PARSE_RETRY
            continue
          unsettled = [
            dependency
            for dependency in selected.after
            if by_id[dependency].status
            not in {PlanStepStatus.DONE, PlanStepStatus.FAILED}
          ]
          if unsettled:
            parse_failures += 1
            history.append(
              f"invalid step: plan step {selected.id!r} is blocked by "
              + ", ".join(unsettled)
            )
            trigger = InvocationTrigger.PARSE_RETRY
            continue
          failed_dependencies = [
            dependency
            for dependency in selected.after
            if by_id[dependency].status is PlanStepStatus.FAILED
          ]
          if failed_dependencies and not selected.terminal:
            parse_failures += 1
            history.append(
              f"invalid step: plan step {selected.id!r} has failed dependencies "
              + ", ".join(failed_dependencies)
              + "; mark it failed or retry the prerequisite"
            )
            trigger = InvocationTrigger.PARSE_RETRY
            continue
        if parsed.reasoning_request is not None:
          if reasoning_task is None:
            forced_reason = parsed.reasoning_request
          if not parsed.actions and not parsed.plan_edits:
            parse_failures = 0
            reason_only_wait = True
            trigger = InvocationTrigger.STEP
            continue

        retries = parse_failures
        trigger = InvocationTrigger.STEP
        planning_duration = time.monotonic() - completion_started

        if parsed.final is not None:
          decision_mode = "execute"
          unfinished = [
            item.id
            for item in preview_plan
            if item.required
            and item.status not in {PlanStepStatus.DONE, PlanStepStatus.FAILED}
          ]
          incomplete_terminal = [
            item.id
            for item in preview_plan
            if item.terminal and item.status is not PlanStepStatus.DONE
          ]
          if unfinished or incomplete_terminal:
            parse_failures += 1
            details: list[str] = []
            if unfinished:
              details.append("unsettled required steps: " + ", ".join(unfinished))
            if incomplete_terminal:
              details.append(
                "terminal steps not complete: " + ", ".join(incomplete_terminal)
              )
            history.append("invalid final: " + "; ".join(details))
            trigger = InvocationTrigger.PARSE_RETRY
            continue
          parse_failures = 0
          plan_steps = preview_plan
          final_step = StepRecord(
            index=len(steps) + 1,
            outcome=StepOutcome.SUCCEEDED,
            final=parsed.final,
            plan_edits=parsed.plan_edits,
            prompt_tokens=response.usage.prompt_tokens,
            completion_tokens=response.usage.completion_tokens,
            retries=retries,
            duration_s=planning_duration,
          )
          steps.append(final_step)
          if final_step.outcome is not StepOutcome.SUCCEEDED:
            outcome = StepOutcome.FAILED
            error = final_step.reason or "final reply delivery failed"
            self._log.warning(
              "mission_reply_delivery_failed", mission_id=mission.id, reason=error
            )
            break
          terminal_failure = any(
            edit.op is PlanEditKind.FAIL for edit in parsed.plan_edits
          ) and all(item.status is PlanStepStatus.FAILED for item in preview_plan)
          if terminal_failure:
            outcome = StepOutcome.FAILED
            error = parsed.final
            self._log.info(
              "mission_concluded",
              mission_id=mission.id,
              steps=len(steps),
              outcome=outcome.value,
            )
            break
          self._log.info("mission_concluded", mission_id=mission.id, steps=len(steps))
          outcome = StepOutcome.SUCCEEDED
          answer = parsed.final
          break

        if not parsed.actions:
          parse_failures = 0
          plan_steps = preview_plan
          if parsed.plan_edits:
            plan_generation += 1
          self._checkpoint_working(
            mission,
            started_at,
            steps,
            plan_steps,
            decision_mode=decision_mode,
            decision_graph=decision_graph,
          )
          history.append(f"plan revised: {_describe_plan_edits(parsed.plan_edits)}")
          continue

        parse_failures = 0
        plan_steps = preview_plan
        if parsed.plan_edits:
          plan_generation += 1
        self._checkpoint_working(
          mission,
          started_at,
          steps,
          plan_steps,
          decision_mode=decision_mode,
          decision_graph=decision_graph,
        )
        pending.append(
          _BufferedStep(
            invocations=parsed.actions,
            plan_edits=parsed.plan_edits,
            plan_step_id=parsed.plan_step_id,
            commands=parsed.commands,
            prompt_tokens=response.usage.prompt_tokens,
            completion_tokens=response.usage.completion_tokens,
            retries=retries,
            planning_duration_s=planning_duration,
          )
        )
        self._log.info(
          "plan_step_accepted",
          mission_id=mission.id,
          actions=len(parsed.actions),
          reasoning_effort=reasoning_effort,
        )
        # Return through the common boundary before dispatching even the
        # first barrier. Stop, preemption, interrupts, and perception changes
        # that raced acceptance therefore invalidate work before egress.
        continue

      if answer is None and error is None and self._config.max_steps:
        outcome = StepOutcome.FAILED
        error = f"step cap reached ({self._config.max_steps})"
    except asyncio.CancelledError:
      cancelled = True
      outcome = StepOutcome.ABORTED
      error = "planner task cancelled"
    except Exception as exc:  # noqa: BLE001 — persist an unexpected planner failure
      outcome = StepOutcome.FAILED
      error = f"planner failed: {exc}"
    await _cancel_tasks(reasoning_task)
    self._save_plan(
      mission,
      started_at,
      steps,
      plan_steps,
      outcome,
      answer,
      error,
      decision_mode=decision_mode,
      decision_graph=decision_graph,
    )
    if self._working is not None:
      self._working.drop(mission.id)
    self._running_mission_id = None
    self._running_started_at = None
    self._live_plan = None
    if cancelled:
      raise asyncio.CancelledError
    if preempted_by is not None:
      raise MissionPreemptedError(error or "preempted")
    if outcome is not StepOutcome.SUCCEEDED:
      raise MissionFailedError(error or "mission failed")
    assert answer is not None
    return answer

  def _controller_reasoning_effort(self) -> ReasoningEffort:
    """Return the mandatory non-reasoning mode for every controller call."""
    if "none" not in self._reasoning_options:
      raise MissionFailedError("controller model binding does not support 'none'")
    return "none"

  def _action_context_fingerprint(
    self,
  ) -> tuple[tuple[str, str, str, tuple[tuple[str, str], ...]], ...]:
    """Current action-relevant admission state, excluding continuous telemetry.

    IDs and timestamps are intentionally absent: high-rate producers may
    republish an identical snapshot while the model is thinking. A content
    change invalidates stale work; an identical refresh does not. Coalesced
    ``body`` snapshots are prompt context rather than an execution interlock:
    joints and other continuous values can change every poll and would otherwise
    invalidate every model reply before it can reach the safety gate. Decision
    primitive results are already correlated in private mission history and must
    not invalidate the remaining barriers they just enabled.
    """
    return tuple(
      sorted(
        (
          record.kind,
          record.source,
          record.text,
          tuple(
            sorted(
              (str(key), str(value))
              for key, value in record.metadata.items()
              if key not in _VOLATILE_ACTION_METADATA
            )
          ),
        )
        for record in self._queue.records()
        if record.kind not in _CONTINUOUS_ACTION_CONTEXT_KINDS
        and not (
          record.kind == RESULT_OBSERVATION_KIND and record.source == DECISION_SOURCE
        )
      )
    )

  async def _request_reasoning(
    self,
    mission: MissionRecord,
    history: list[str],
    recall: str | None,
    plan_steps: list[PlanStep],
    trigger: str,
    base_generation: int,
    *,
    mission_summary: str | None = None,
    peer_reference: str | None = None,
  ) -> _ReasoningProposal:
    """Request one semantic-only plan proposal from the slow lane."""
    assert self._reasoner_model is not None
    if self._config.reasoner.reasoning_effort not in self._reasoner_options:
      raise MissionFailedError(
        "reasoner model binding does not support "
        f"{self._config.reasoner.reasoning_effort!r}"
      )
    fingerprint = self._action_context_fingerprint()
    messages = self._build_reasoner_messages(
      mission,
      history,
      recall,
      plan_steps,
      trigger=trigger,
      mission_summary=mission_summary,
      peer_reference=peer_reference,
    )
    decision = self._policy.admit(InvocationTrigger.STEP)
    while not decision.call and decision.wait_s > 0:
      await asyncio.sleep(decision.wait_s)
      decision = self._policy.admit(InvocationTrigger.STEP)
    if not decision.call:
      raise MissionFailedError(decision.reason)
    started = time.monotonic()
    reasoner_reservation = self._config.reasoner.max_tokens
    self._policy.begin_call(reasoner_reservation)
    delay_s = min(self._config.model_retry_initial_s, self._config.model_retry_max_s)
    attempt = 0
    try:
      while True:
        try:
          response = await self._reasoner_model.complete(
            messages,
            max_tokens=self._config.reasoner.max_tokens,
            temperature=self._config.temperature,
            reasoning_effort=self._config.reasoner.reasoning_effort,
          )
          break
        except asyncio.CancelledError:
          raise
        except Exception as exc:  # noqa: BLE001 - provider boundary is retryable
          attempt += 1
          self._log.warning(
            "reasoner_request_retrying",
            attempt=attempt,
            delay_s=delay_s,
            error=str(exc),
          )
          await asyncio.sleep(delay_s)
          delay_s = min(delay_s * 2, self._config.model_retry_max_s)
    except BaseException:
      self._policy.cancel_call(reasoner_reservation)
      raise
    self._policy.settle_call(_usage_total(response.usage), reasoner_reservation)
    self._publish_exchange(
      mission.id,
      len(self._live_plan.steps) + 1 if self._live_plan is not None else 1,
      messages,
      response,
      started,
      reasoning_effort=self._config.reasoner.reasoning_effort,
      agent_id="reasoner",
    )
    parsed = _parse_reply(response.text)
    if (
      parsed is None
      or not parsed.plan_edits
      or parsed.actions
      or parsed.final is not None
      or parsed.reasoning_request is not None
    ):
      raise ValueError("reasoner must return semantic plan edits only")
    return _ReasoningProposal(
      parsed.plan_edits, base_generation, fingerprint, response.text
    )

  async def _wait_for_reasoner(
    self,
    task: asyncio.Task[_ReasoningProposal],
    stop: asyncio.Event,
  ) -> Literal["ready", "stop", "interrupt"]:
    """Wait safely when no validated plan step can run."""
    stopped = asyncio.create_task(stop.wait())
    interrupted = asyncio.create_task(self._policy.wait_interrupt())
    try:
      done, pending = await asyncio.wait(
        {task, stopped, interrupted}, return_when=asyncio.FIRST_COMPLETED
      )
    except asyncio.CancelledError:
      stopped.cancel()
      interrupted.cancel()
      await asyncio.gather(stopped, interrupted, return_exceptions=True)
      raise
    for waiter in (stopped, interrupted):
      if waiter in pending:
        waiter.cancel()
    await asyncio.gather(stopped, return_exceptions=True)
    await asyncio.gather(interrupted, return_exceptions=True)
    if stopped in done:
      return "stop"
    if interrupted in done:
      return "interrupt"
    return "ready"

  async def _execute_buffered_step(
    self,
    mission: MissionRecord,
    step_index: int,
    buffered: _BufferedStep,
    plan_step: PlanStep | None,
    *,
    active_actions: list[ActiveAction],
    checkpoint: Callable[[], None],
  ) -> tuple[StepRecord, list[str]]:
    """Execute one accepted barrier without consulting the model."""
    execution_started = time.monotonic()
    self._log.info(
      "step_dispatching",
      mission_id=mission.id,
      step=step_index,
      action_count=len(buffered.invocations),
      actions=[action.action for action in buffered.invocations],
      robots=[action.robot_id for action in buffered.invocations],
    )
    action_starts = [time.monotonic() for _ in buffered.invocations]
    results = await asyncio.gather(
      *[
        (
          self._execute_skill(
            mission,
            action,
            StepContext(mission_id=mission.id, step_index=step_index),
            action_index=position + 1,
            active_actions=active_actions,
            checkpoint=checkpoint,
          )
          if action.action_kind == "skill"
          else self._execute_step(
            mission,
            action,
            StepContext(mission_id=mission.id, step_index=step_index),
          )
        )
        for position, action in enumerate(buffered.invocations)
      ]
    )
    outcome_parts: list[str] = []
    action_records: list[ActionRecord] = []
    for position, (action, (step_text, accepted, timed_out, execution)) in enumerate(
      zip(buffered.invocations, results)
    ):
      action_records.append(
        ActionRecord(
          index=position + 1,
          robot_id=action.robot_id or "default",
          device_id=action.device_id,
          intent=action.intent,
          preconditions=action.preconditions,
          action=action.action,
          action_kind=action.action_kind,
          version=action.version,
          request_id=execution.request_id if execution is not None else None,
          execution_id=execution.execution_id if execution is not None else None,
          execution_status=execution.status if execution is not None else None,
          command=(buffered.commands or [""] * len(buffered.invocations))[position],
          arguments=action.arguments,
          outcome=(
            StepOutcome.ABORTED
            if timed_out
            else StepOutcome.SUCCEEDED
            if accepted
            else StepOutcome.FAILED
          ),
          reason=("step_timeout" if timed_out else None if accepted else step_text),
          result=step_text if accepted else None,
          duration_s=time.monotonic() - action_starts[position],
        )
      )
      target = f"{action.robot_id or 'default'}/{action.device_id or 'default'}"
      outcome_parts.append(
        f"{action.action}@{target}({action.arguments}) -> {step_text}"
      )
    if action_records:
      step_outcome, step_reason = _aggregate_step_outcome(action_records)
    else:
      step_outcome, step_reason = StepOutcome.SUCCEEDED, None
    if not outcome_parts:
      outcome_parts.append(f"plan: {_describe_plan_edits(buffered.plan_edits)}")
    execution_duration = time.monotonic() - execution_started
    self._log.info(
      "step_completed",
      mission_id=mission.id,
      step=step_index,
      outcome=step_outcome.value,
      action_count=len(action_records),
      accepted=sum(
        action.outcome is StepOutcome.SUCCEEDED for action in action_records
      ),
      duration_s=round(execution_duration, 3),
      reason=step_reason,
    )
    return (
      StepRecord(
        index=step_index,
        outcome=step_outcome,
        reason=step_reason,
        plan_step_id=plan_step.id if plan_step is not None else None,
        sentence=plan_step.sentence if plan_step is not None else None,
        plan_edits=buffered.plan_edits,
        prompt_tokens=buffered.prompt_tokens,
        completion_tokens=buffered.completion_tokens,
        retries=buffered.retries,
        duration_s=(buffered.planning_duration_s + execution_duration),
        actions=action_records,
      ),
      outcome_parts,
    )

  def _publish_exchange(
    self,
    mission_id: str,
    step: int,
    messages: list[ChatMessage],
    response: Any,
    started_at: float,
    *,
    exchange_id: int | None = None,
    reasoning_effort: str | None = None,
    agent_id: str = "controller",
  ) -> None:
    """Forward one request/response pair to the model I/O exchange, if wired."""
    if self._exchange_sink is None:
      return
    selected_id = self._exchange_counter if exchange_id is None else exchange_id
    self._exchange_sink(
      ModelExchange(
        exchange_id=selected_id,
        agent_id=agent_id,
        mission_id=mission_id,
        step=step,
        model=response.model,
        messages=[
          ModelMessage(
            role=m.role,
            content=m.content if isinstance(m.content, str) else str(m.content),
          )
          for m in messages
        ],
        reply=response.text,
        thinking=response.thinking,
        reasoning_effort=reasoning_effort,
        finish_reason=response.finish_reason,
        prompt_tokens=response.usage.prompt_tokens,
        completion_tokens=response.usage.completion_tokens,
        duration_s=time.monotonic() - started_at,
        timestamp=datetime.now(UTC),
      )
    )
    if exchange_id is None:
      self._exchange_counter += 1

  def _publish_stream_exchange(
    self,
    exchange_id: int,
    mission_id: str,
    step: int,
    messages: list[ChatMessage],
    started_at: float,
    stream_text: dict[str, str],
    *,
    status: Literal["streaming", "complete", "interrupted", "error"] = "streaming",
    reasoning_effort: str | None = None,
  ) -> None:
    """Publish the current provider-visible text for an in-flight request."""
    if self._exchange_sink is None:
      return
    self._exchange_sink(
      ModelExchange(
        exchange_id=exchange_id,
        agent_id="controller",
        mission_id=mission_id,
        step=step,
        status=status,
        model=stream_text.get("model", ""),
        messages=[
          ModelMessage(
            role=message.role,
            content=(
              message.content
              if isinstance(message.content, str)
              else str(message.content)
            ),
          )
          for message in messages
        ],
        reply=stream_text["reply"],
        thinking=stream_text["thinking"] or None,
        reasoning_effort=reasoning_effort,
        duration_s=time.monotonic() - started_at,
        timestamp=datetime.now(UTC),
      )
    )

  async def _complete_with_retry(
    self,
    messages: list[ChatMessage],
    reasoning_effort: ReasoningEffort | None,
    stop: asyncio.Event,
    *,
    goal: str,
    on_delta: Callable[[ModelStreamDelta], None] | None = None,
  ) -> Any | None:
    """Retry model transport failures until completion or a control event.

    A completion has no hardware side effect, so retrying its failed transport
    is safe. The backoff is capped rather than the number of attempts: an
    unavailable provider should not terminate an otherwise valid mission.
    Shutdown aborts the mission; a priority interrupt exits the backoff so the
    outer loop can rebuild the prompt and replan immediately.
    """
    attempt = 0
    delay_s = min(self._config.model_retry_initial_s, self._config.model_retry_max_s)
    while not stop.is_set():
      try:
        response = await self._complete_once_or_stop(
          messages, reasoning_effort, stop, goal=goal, on_delta=on_delta
        )
        if response is None:
          return None
        return response
      except Exception as exc:  # noqa: BLE001 — provider boundary is retryable
        attempt += 1
        self._log.warning(
          "model_request_retrying",
          attempt=attempt,
          delay_s=delay_s,
          error=str(exc),
        )
        wait_result = await self._wait_for_policy_delay(delay_s, stop)
        if wait_result == "stop":
          return None
        if wait_result == "interrupt":
          return _MODEL_CALL_INTERRUPTED
        delay_s = min(delay_s * 2, self._config.model_retry_max_s)
    return None

  async def _wait_for_policy_delay(self, delay_s: float, stop: asyncio.Event) -> str:
    """Wait for cooldown/backoff expiry, shutdown, or a priority interrupt."""
    elapsed = asyncio.create_task(asyncio.sleep(delay_s))
    stopped = asyncio.create_task(stop.wait())
    interrupted = asyncio.create_task(self._policy.wait_interrupt())
    try:
      done, pending = await asyncio.wait(
        {elapsed, stopped, interrupted}, return_when=asyncio.FIRST_COMPLETED
      )
    except asyncio.CancelledError:
      for task in (elapsed, stopped, interrupted):
        task.cancel()
      await asyncio.gather(elapsed, stopped, interrupted, return_exceptions=True)
      raise
    for task in pending:
      task.cancel()
    await asyncio.gather(elapsed, stopped, interrupted, return_exceptions=True)
    if stopped in done:
      return "stop"
    if interrupted in done:
      return "interrupt"
    return "elapsed"

  async def _complete_once_or_stop(
    self,
    messages: list[ChatMessage],
    reasoning_effort: ReasoningEffort | None,
    stop: asyncio.Event,
    *,
    goal: str,
    on_delta: Callable[[ModelStreamDelta], None] | None = None,
  ) -> Any | None:
    """Race one provider call with shutdown and join both local tasks.

    Cancelling a client implemented with ``asyncio.to_thread`` cannot stop the
    underlying HTTP thread, but it does detach the mission from that late
    response: no parsing or state mutation can occur after shutdown.
    """
    kwargs: dict[str, Any] = {
      "max_tokens": self._config.controller.max_tokens,
      "temperature": self._config.temperature,
    }
    if reasoning_effort is not None:
      kwargs["reasoning_effort"] = reasoning_effort
    if on_delta is not None:
      kwargs["on_delta"] = on_delta
    completion = asyncio.create_task(self._model.complete(messages, **kwargs))
    stopped = asyncio.create_task(stop.wait())
    interrupted = asyncio.create_task(self._policy.wait_interrupt())
    sentinel = (
      asyncio.create_task(self._sentinel.watch(goal, stop))
      if self._sentinel is not None
      else None
    )
    waiters = {completion, stopped, interrupted}
    if sentinel is not None:
      waiters.add(sentinel)
    try:
      done, _pending = await asyncio.wait(waiters, return_when=asyncio.FIRST_COMPLETED)
    except asyncio.CancelledError:
      for task in waiters:
        task.cancel()
      await asyncio.gather(*waiters, return_exceptions=True)
      raise
    if sentinel is not None and sentinel in done and sentinel.result() is not None:
      signal = sentinel.result()
      assert signal is not None
      self._policy.request_interrupt(
        "sentinel", f"{signal.kind.value}: {signal.detail}"
      )
      completion.cancel()
      stopped.cancel()
      interrupted.cancel()
      await asyncio.gather(completion, stopped, interrupted, return_exceptions=True)
      return _MODEL_CALL_INTERRUPTED
    if stopped in done:
      completion.cancel()
      interrupted.cancel()
      if sentinel is not None:
        sentinel.cancel()
      await asyncio.gather(
        completion,
        interrupted,
        *([sentinel] if sentinel is not None else []),
        return_exceptions=True,
      )
      return None
    if interrupted in done:
      completion.cancel()
      stopped.cancel()
      if sentinel is not None:
        sentinel.cancel()
      await asyncio.gather(
        completion,
        stopped,
        *([sentinel] if sentinel is not None else []),
        return_exceptions=True,
      )
      return _MODEL_CALL_INTERRUPTED
    stopped.cancel()
    interrupted.cancel()
    if sentinel is not None:
      sentinel.cancel()
    await asyncio.gather(
      stopped,
      interrupted,
      *([sentinel] if sentinel is not None else []),
      return_exceptions=True,
    )
    return await completion

  async def _execute_step(
    self, mission: MissionRecord, invocation: ActionInvocation, context: StepContext
  ) -> tuple[str, bool, bool, SkillExecution | None]:
    """Pass one step through the gate and enqueue its ack as an observation.

    Returns the outcome text (for the model's history), whether the backend
    accepted the call, and whether the step timed out. The gate call is
    bounded by ``step_timeout_s``: a hung egress aborts the step
    (``reason="step_timeout"``) but the mission continues — the world, not
    the plan, is what stalled.
    """
    activity_id = self._activity_counter
    self._activity_counter += 1
    activity_started_at = datetime.now(UTC)
    if self._activity_sink is not None:
      self._activity_sink(
        ModelActivity(
          activity_id=activity_id,
          agent_id="controller",
          mission_id=mission.id,
          step=context.step_index,
          name=invocation.action,
          arguments=invocation.arguments,
          started_at=activity_started_at,
        )
      )
    timed_out = False
    observation_outcome: str | None = None
    try:
      verdict = await asyncio.wait_for(
        self._gate.execute(invocation, context=context),
        timeout=self._config.step_timeout_s,
      )
    except TimeoutError:
      timed_out = True
      self._log.warning(
        "step_timeout",
        mission_id=mission.id,
        action=invocation.action,
        timeout_s=self._config.step_timeout_s,
      )
      outcome = f"step timed out after {self._config.step_timeout_s}s"
      accepted = False
    except Exception as exc:
      if self._activity_sink is not None:
        self._activity_sink(
          ModelActivity(
            activity_id=activity_id,
            agent_id="controller",
            mission_id=mission.id,
            step=context.step_index,
            name=invocation.action,
            arguments=invocation.arguments,
            status="error",
            error=str(exc),
            started_at=activity_started_at,
            finished_at=datetime.now(UTC),
          )
        )
      raise
    else:
      if not verdict.admitted:
        outcome = f"rejected by gate ({verdict.reason})"
      elif verdict.ack is None:
        outcome = f"egress failed ({verdict.reason})"
      elif verdict.ack.succeeded:
        outcome = f"succeeded (sequence {verdict.ack.sequence})"
      else:
        detail = verdict.ack.reason or verdict.ack.result.get("reason")
        outcome = f"{verdict.ack.status.value} by backend ({detail or 'no detail'})"
      if verdict.ack is not None and verdict.ack.result is not None:
        result = verdict.ack.result
        outcome = f"{outcome} {_render_result(result)}"
      accepted = verdict.ack.succeeded if verdict.ack is not None else False
    observation_outcome = observation_outcome or outcome
    await self._queue.enqueue(
      kind=RESULT_OBSERVATION_KIND,
      source=DECISION_SOURCE,
      text=f"{invocation.action}: {observation_outcome}",
      metadata={
        "mission_id": mission.id,
        "action": invocation.action,
        "accepted": str(accepted).lower(),
      },
    )
    if self._activity_sink is not None:
      self._activity_sink(
        ModelActivity(
          activity_id=activity_id,
          agent_id="controller",
          mission_id=mission.id,
          step=context.step_index,
          name=invocation.action,
          arguments=invocation.arguments,
          status="complete",
          result=outcome,
          started_at=activity_started_at,
          finished_at=datetime.now(UTC),
        )
      )
    return outcome, accepted, timed_out, None

  async def _execute_skill(
    self,
    mission: MissionRecord,
    invocation: ActionInvocation,
    context: StepContext,
    *,
    action_index: int,
    active_actions: list[ActiveAction],
    checkpoint: Callable[[], None],
  ) -> tuple[str, bool, bool, SkillExecution | None]:
    """Start, durably follow, and validate one governed EOS skill."""
    if (
      self._skill_clients is None
      or invocation.robot_id is None
      or invocation.version is None
    ):
      return "skill execution is unavailable", False, False, None
    if self._gate.estop_engaged:
      return "rejected by gate (estop_engaged)", False, False, None
    try:
      client = self._skill_clients.client(invocation.robot_id)
    except LookupError as exc:
      return str(exc), False, False, None
    request_id = invocation.request_id or str(
      uuid5(
        NAMESPACE_URL,
        f"island-egia:{mission.id}:{context.step_index}:{action_index}:{invocation.action}@{invocation.version}",
      )
    )
    timeout_s = min(float(self._config.step_timeout_s), 3600.0)
    execution: SkillExecution | None = None
    active: ActiveAction | None = None
    settled = False
    try:
      execution = await client.start_skill(
        invocation.action,
        invocation.version,
        invocation.arguments,
        request_id=request_id,
        timeout_s=timeout_s,
      )
      active = ActiveAction(
        step_index=context.step_index,
        action_index=action_index,
        robot_id=invocation.robot_id,
        action=invocation.action,
        version=invocation.version,
        arguments=invocation.arguments,
        request_id=request_id,
        execution_id=execution.execution_id,
        sequence=execution.sequence,
        status=execution.status,
        cancellation_requested=execution.cancellation_requested,
      )
      active_actions.append(active)
      checkpoint()
      if not execution.terminal:
        async with asyncio.timeout(timeout_s):
          async for snapshot in client.watch_skill_execution(
            execution.execution_id, after_sequence=execution.sequence
          ):
            execution = snapshot
            active.sequence = snapshot.sequence
            active.status = snapshot.status
            active.cancellation_requested = snapshot.cancellation_requested
            checkpoint()
            if snapshot.terminal:
              break
      if execution is None or not execution.terminal:
        raise OSError("skill watch ended before a terminal snapshot")
      settled = True
      if execution.status == "succeeded":
        binding = client.skill(invocation.action, invocation.version)
        assert binding is not None
        Draft202012Validator(binding.returns or {}).validate(execution.result)
        return (
          f"succeeded (sequence {execution.sequence}) {_render_result(execution.result)}",
          True,
          False,
          execution,
        )
      detail = execution.message or execution.reason_code or "no detail"
      return f"{execution.status} by backend ({detail})", False, False, execution
    except asyncio.CancelledError:
      if execution is not None:
        try:
          cancelled = await asyncio.shield(
            client.cancel_skill_execution(execution.execution_id)
          )
          if active is not None:
            active.sequence = cancelled.sequence
            active.status = cancelled.status
            active.cancellation_requested = cancelled.cancellation_requested
            checkpoint()
        except Exception as exc:  # noqa: BLE001 - cancellation must propagate
          self._log.warning(
            "skill_cancel_failed",
            execution_id=execution.execution_id,
            reason=str(exc),
          )
      raise
    except TimeoutError:
      if execution is not None:
        try:
          execution = await client.cancel_skill_execution(execution.execution_id)
        except Exception as exc:  # noqa: BLE001 - timeout remains primary
          self._log.warning(
            "skill_cancel_failed",
            execution_id=execution.execution_id,
            reason=str(exc),
          )
      settled = execution is not None and execution.terminal
      return (
        f"step timed out after {timeout_s}s; cancellation requested",
        False,
        True,
        execution,
      )
    except Exception as exc:  # noqa: BLE001 - transport failures become action failures
      return f"skill execution failed ({exc})", False, False, execution
    finally:
      if settled and active is not None and active in active_actions:
        active_actions.remove(active)
        checkpoint()

  async def _resume_skill_execution(
    self,
    mission: MissionRecord,
    active: ActiveAction,
    *,
    checkpoint: Callable[[], None],
  ) -> ActionRecord | None:
    """Reconcile a durable EOS execution without ever submitting it again."""
    if self._skill_clients is None:
      return None
    try:
      client = self._skill_clients.client(active.robot_id)
      execution = await client.get_skill_execution(active.execution_id)
      if not execution.terminal:
        async for snapshot in client.watch_skill_execution(
          active.execution_id,
          after_sequence=max(active.sequence, execution.sequence),
        ):
          execution = snapshot
          active.sequence = snapshot.sequence
          active.status = snapshot.status
          active.cancellation_requested = snapshot.cancellation_requested
          checkpoint()
          if snapshot.terminal:
            break
      if not execution.terminal:
        return None
      succeeded = execution.status == "succeeded"
      return ActionRecord(
        index=active.action_index,
        robot_id=active.robot_id,
        action=active.action,
        action_kind="skill",
        version=active.version,
        request_id=active.request_id,
        execution_id=active.execution_id,
        execution_status=execution.status,
        arguments=active.arguments,
        outcome=StepOutcome.SUCCEEDED if succeeded else StepOutcome.FAILED,
        reason=None if succeeded else execution.message or execution.reason_code,
        result=execution.result,
      )
    except Exception as exc:
      if getattr(exc, "status", None) == 404:
        history = (
          f"skill execution {active.execution_id} is missing after restart; "
          "outcome is unknown and requires verification"
        )
        self._log.warning(
          "skill_execution_unknown",
          mission_id=mission.id,
          execution_id=active.execution_id,
        )
        return ActionRecord(
          index=active.action_index,
          robot_id=active.robot_id,
          action=active.action,
          action_kind="skill",
          version=active.version,
          request_id=active.request_id,
          execution_id=active.execution_id,
          execution_status="unknown",
          arguments=active.arguments,
          outcome=StepOutcome.ABORTED,
          reason=history,
        )
      raise

  def _save_plan(
    self,
    mission: MissionRecord,
    started_at: datetime,
    steps: list[StepRecord],
    plan_steps: list[PlanStep],
    outcome: StepOutcome,
    result: str | None,
    error: str | None,
    *,
    decision_mode: Literal["plan", "execute"] = "execute",
    decision_graph: DecisionGraph | None = None,
  ) -> None:
    """Assemble and persist the mission's plan record — its memory artifact."""
    live = self._live_plan
    record = PlanRecord(
      id=live.id if live is not None else str(uuid4()),
      mission_id=mission.id,
      priority=mission.priority,
      order=mission.order,
      source=mission.source,
      started_at=started_at,
      finished_at=datetime.now(UTC),
      outcome=outcome,
      result=result,
      error=error,
      steps=steps,
      plan=list(plan_steps),
      decision_mode=decision_mode,
      decision_graph=decision_graph,
      annotations=self._annotations,
      revision=(live.revision + 1 if live is not None else 1),
    )
    self._last_plan = record.model_copy(deep=True)
    if self._plan_sink is not None:
      self._plan_sink(record.model_copy(deep=True), False)
    if self._episodic is None:
      return
    saved = self._episodic.save_plan(record)
    if saved:
      if self._recall is not None:
        # A store-only configuration (episodic wired, recall not built) must
        # not crash the save path — indexing is advisory like recall itself.
        self._recall.index_plan(record)
      self._log.info(
        "plan_saved",
        mission_id=mission.id,
        plan_id=record.id,
        outcome=outcome.value,
        steps=len(steps),
      )
    else:
      self._log.warning("plan_save_skipped", mission_id=mission.id, plan_id=record.id)

  def _recall_prior_plans(self, order: str) -> str | None:
    """Recall-before-decomposition digest (§3.3), or ``None`` on no matches.

    Reference (reviewed, promoted) plans rank ahead of episodic ones, matched
    by keyword and — when a vector index is configured — by nearest-neighbor
    recall over the same ids, deduped reference-first. Failed plans stay in
    the digest — they are the counter-examples the planner must argue against
    or avoid. A "Relevant docs:" section of corpus excerpts follows when the
    docs corpus answers the order; both sections are omitted when empty.
    ``recall_limit = 0`` disables the query entirely.
    """
    limit = self._config.recall_limit
    if limit <= 0:
      return None
    if self._recall is None:
      # Memory disabled or no stores: recall is not wired, so no digest.
      return None
    try:
      plans = self._recall.search_plans(order, limit)
      docs = self._recall.search_docs(order, limit)
    except sqlite3.ProgrammingError as exc:
      # Recall is advisory — a store closed mid-shutdown must never fail
      # the mission it precedes.
      self._log.warning("plan_recall_failed", error=str(exc))
      return None
    lines: list[str] = []
    if plans:
      lines.append("Prior similar plans:")
      for plan in plans:
        order_text = plan.order if len(plan.order) <= 80 else plan.order[:77] + "..."
        lines.append(f'- "{order_text}" -> {plan.outcome.value}')
        steps: list[str] = []
        for step in plan.steps[:6]:
          names = [action.action for action in step.actions]
          label = "; ".join(names) if names else "final"
          steps.append(f"{label} -> {step.outcome.value}")
        if len(plan.steps) > 6:
          steps.append("...")
        if steps:
          lines.append(f"  steps: {'; '.join(steps)}")
        if plan.review_note:
          lines.append(f"  review note: {plan.review_note}")
    if docs:
      lines.append("Relevant docs:")
      for doc_id, excerpt in docs:
        lines.append(f"- {doc_id}: {excerpt}")
    return "\n".join(lines) if lines else None

  def _checkpoint_working(
    self,
    mission: MissionRecord,
    started_at: datetime,
    steps: list[StepRecord],
    plan_steps: list[PlanStep],
    *,
    decision_mode: Literal["plan", "execute"] = "execute",
    decision_graph: DecisionGraph | None = None,
    active_actions: list[ActiveAction] | None = None,
  ) -> None:
    """Upsert the in-flight plan before egress and after each settled step.

    The record is marked ``aborted`` as a placeholder: a row surviving in
    working.db means the mission never concluded, which consolidation reads
    as a crash. The true outcome is written to episodic at mission end and
    the working row dropped.
    """
    record = PlanRecord(
      id=self._live_plan.id if self._live_plan is not None else str(uuid4()),
      mission_id=mission.id,
      priority=mission.priority,
      order=mission.order,
      source=mission.source,
      started_at=started_at,
      finished_at=datetime.now(UTC),
      outcome=StepOutcome.ABORTED,
      steps=list(steps),
      plan=list(plan_steps),
      decision_mode=decision_mode,
      decision_graph=decision_graph,
      annotations=list(self._annotations),
      active_actions=list(active_actions or []),
      revision=(self._live_plan.revision + 1 if self._live_plan is not None else 1),
    )
    if self._plan_sink is not None:
      self._plan_sink(record.model_copy(deep=True), True)
    if self._working is not None:
      self._working.upsert_plan(record)
    self._live_plan = record

  def _build_messages(
    self,
    mission: MissionRecord,
    history: list[str],
    recall: str | None,
    plan_steps: list[PlanStep],
    *,
    mission_summary: str | None = None,
    decision_mode: Literal["plan", "execute"] = "execute",
    decision_graph: DecisionGraph | None = None,
    reasoning_effort: str | None = None,
    peer_reference: str | None = None,
  ) -> list[ChatMessage]:
    instruction = self._prompts.instruction
    values = {
      "planner_recall": recall,
      "planner_summary": mission_summary,
      "planner_mission": _render_active_mission(mission),
      "planner_plan": _render_plan_block(
        _unsummarized_plan_steps(plan_steps, summarized=mission_summary is not None)
      ),
      "planner_results": _render_execution_history(history),
    }
    peer_parts = (
      [(1150, "Stale reasoner output for reference only:\n" + peer_reference)]
      if peer_reference is not None
      else []
    )
    reserved = "\n\n".join(
      [text for _, text in render_lane_sections(self._prompts, values)]
      + [text for _, text in peer_parts]
    )
    snapshot = self._context.build(reserved_text=reserved)
    frequencies = self._context.observe_dynamic(values)
    return [
      ChatMessage(role="system", content=instruction),
      ChatMessage(
        role="user",
        content=render_dynamic_message(
          self._prompts,
          values,
          context=snapshot,
          extra=peer_parts,
          token_budget=self._context.token_budget,
          change_frequencies=frequencies,
        ),
      ),
    ]

  def _build_reasoner_messages(
    self,
    mission: MissionRecord,
    history: list[str],
    recall: str | None,
    plan_steps: list[PlanStep],
    *,
    trigger: str,
    mission_summary: str | None = None,
    peer_reference: str | None = None,
  ) -> list[ChatMessage]:
    """Build the bounded semantic-planning request for the slow lane."""
    assert self._reasoner_prompts is not None
    values = {
      "reasoner_recall": recall,
      "reasoner_summary": mission_summary,
      "reasoner_mission": _render_active_mission(mission),
      "reasoner_plan": _render_plan_block(
        _unsummarized_plan_steps(plan_steps, summarized=mission_summary is not None)
      ),
      "reasoner_trigger": trigger,
      "reasoner_results": _render_execution_history(history),
    }
    peer_parts = (
      [(1150, "Stale fast-model output for reference only:\n" + peer_reference)]
      if peer_reference is not None
      else []
    )
    reserved = "\n\n".join(
      [text for _, text in render_lane_sections(self._reasoner_prompts, values)]
      + [text for _, text in peer_parts]
    )
    snapshot = self._context.build(reserved_text=reserved)
    frequencies = self._context.observe_dynamic(values)
    return [
      ChatMessage(role="system", content=self._reasoner_prompts.instruction),
      ChatMessage(
        role="user",
        content=render_dynamic_message(
          self._reasoner_prompts,
          values,
          context=snapshot,
          extra=peer_parts,
          token_budget=self._context.token_budget,
          change_frequencies=frequencies,
        ),
      ),
    ]

  def _mission_context_needs_summary(
    self,
    mission: MissionRecord,
    previous: str | None,
    history: list[str],
    plan_steps: list[PlanStep],
  ) -> bool:
    """Return whether Mission-owned detail has reached its budget share."""
    raw = "\n\n".join(
      part
      for part in (
        previous or "",
        _render_active_mission(mission),
        _render_plan_block(
          _unsummarized_plan_steps(plan_steps, summarized=previous is not None)
        ),
        _render_execution_history(history) or "",
      )
      if part
    )
    # Reserve one quarter of the dynamic-message budget for perception,
    # catalogs, recall, and other current state.
    return len(raw) >= self._context.token_budget * 3

  async def _summarize_mission_context(
    self,
    mission: MissionRecord,
    previous: str | None,
    history: list[str],
    plan_steps: list[PlanStep],
  ) -> str | None:
    """Ask the slow lane to compress Mission history without blocking on failure."""
    if self._summarizer_prompts is None:
      return None
    model = self._reasoner_model or self._model
    values = {
      "summarizer_previous": previous,
      "summarizer_mission": _render_active_mission(mission),
      "summarizer_plan": _render_plan_block(
        _unsummarized_plan_steps(plan_steps, summarized=previous is not None)
      ),
      "summarizer_history": _render_execution_history(history),
    }
    messages = [
      ChatMessage(role="system", content=self._summarizer_prompts.instruction),
      ChatMessage(
        role="user",
        content=render_dynamic_message(
          self._summarizer_prompts,
          values,
          token_budget=self._context.token_budget,
          change_frequencies=self._context.observe_dynamic(values),
        ),
      ),
    ]
    started = time.monotonic()
    try:
      response = await model.complete(
        messages,
        max_tokens=min(2048, self._config.reasoner.max_tokens),
        temperature=self._config.temperature,
        reasoning_effort=(
          self._config.reasoner.reasoning_effort
          if self._config.reasoner.reasoning_effort in self._reasoner_options
          else None
        ),
      )
    except Exception as exc:  # noqa: BLE001 - optional compression must degrade
      self._log.warning("mission_context_summary_failed", error=str(exc))
      return None
    summary = response.text.strip()
    if not summary.startswith("MISSION:") or "\nPLAN " not in summary:
      self._log.warning("mission_context_summary_invalid", reply=summary[:512])
      return None
    self._publish_exchange(
      mission.id,
      len(self._live_plan.steps) + 1 if self._live_plan is not None else 1,
      messages,
      response,
      started,
      reasoning_effort=self._config.reasoner.reasoning_effort,
      agent_id="summarizer",
    )
    self._log.info(
      "mission_context_summarized",
      history_events=len(history),
      plan_steps=len(plan_steps),
      summary_chars=len(summary),
    )
    return summary


def _render_result(payload: dict, *, max_chars: int = 512) -> str:
  """Compact and bound one primitive result for the next request."""
  text = json.dumps(payload, ensure_ascii=False, sort_keys=True)
  return text if len(text) <= max_chars else text[: max_chars - 3] + "..."


def _usage_total(usage: Any) -> int:
  """Return provider total usage, deriving it when the field is omitted."""
  total = int(getattr(usage, "total_tokens", 0) or 0)
  if total > 0:
    return total
  return int(getattr(usage, "prompt_tokens", 0) or 0) + int(
    getattr(usage, "completion_tokens", 0) or 0
  )


def _render_context_item(item: Any) -> str:
  """Render one vision, object, or stock record without discarding fields."""
  if not isinstance(item, dict):
    return f"- {_compact_json(item)}"

  consumed: set[str] = set()
  if "object_id" in item:
    identity = f"{item.get('kind', '?')} {item['object_id']}"
    consumed.update({"kind", "object_id"})
  elif "inventory_id" in item:
    identity = f"{item.get('inventory_id', '?')}/{item.get('kind', '?')}"
    consumed.update({"inventory_id", "kind"})
  else:
    return f"- {_compact_json(item)}"

  facts: list[str] = []
  location = item.get("location")
  if isinstance(location, dict):
    place = location.get("place_id", "?")
    slot = location.get("slot_id")
    facts.append(f"@{place}/{slot}" if slot else f"@{place}")
    consumed.add("location")
  elif "location" in item:
    facts.append("@-")
    consumed.add("location")
  states = item.get("states")
  if isinstance(states, list):
    facts.append("states=" + ("|".join(map(str, states)) or "-"))
    consumed.add("states")
  if "variant" in item:
    facts.append(f"variant={item.get('variant') or '-'}")
    consumed.add("variant")
  if "available" in item:
    facts.append(f"available={item['available']}")
    consumed.add("available")

  lines = [f"- {identity}" + (" " + " ".join(facts) if facts else "")]
  relations = item.get("relations")
  if isinstance(relations, list):
    rendered = []
    for relation in relations:
      if not isinstance(relation, dict):
        rendered.append(_compact_json(relation))
        continue
      slot = f"#{relation['slot_id']}" if relation.get("slot_id") else ""
      rendered.append(
        f"{relation.get('kind', '?')}->{relation.get('target_id', '?')}{slot}"
      )
    lines.append("  relations=" + ("; ".join(rendered) or "-"))
    consumed.add("relations")
  slots = item.get("slots")
  if isinstance(slots, list):
    rendered_slots = []
    for slot in slots:
      if not isinstance(slot, dict):
        rendered_slots.append(_compact_json(slot))
        continue
      accepts = "|".join(map(str, slot.get("accepts", []))) or "-"
      occupants = "|".join(map(str, slot.get("occupants", []))) or "-"
      properties = _render_properties(slot.get("properties"))
      rendered_slots.append(
        f"{slot.get('slot_id', '?')}<{accepts}>={occupants}"
        + (f" {{{properties}}}" if properties else "")
      )
    lines.append("  slots=" + ("; ".join(rendered_slots) or "-"))
    consumed.add("slots")
  if "properties" in item:
    properties = _render_properties(item.get("properties"))
    lines.append("  properties=" + (properties or "-"))
    consumed.add("properties")

  extras = {key: item[key] for key in sorted(item) if key not in consumed}
  if extras:
    lines.append(f"  extra={_compact_json(extras)}")
  return "\n".join(lines)


def _render_properties(value: Any) -> str:
  if not isinstance(value, dict):
    return _compact_json(value) if value is not None else ""
  return ",".join(f"{key}={_compact_json(value[key])}" for key in sorted(value))


def _log_excerpt(text: str, limit: int = 768) -> tuple[str, bool]:
  """Bound model text in INFO logs while retaining the failing command."""
  normalized = " ".join(text.split())
  truncated = len(normalized) > limit
  return (normalized[: limit - 3] + "..." if truncated else normalized), truncated


def _reply_kind(reply: _Reply) -> str:
  """Stable log label for one adapted semantic reply."""
  if reply.final is not None:
    return "final"
  if reply.actions:
    return "step"
  return "plan"


def _compact_json(value: Any) -> str:
  return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _aggregate_step_outcome(
  actions: list[ActionRecord],
) -> tuple[StepOutcome, str | None]:
  """Aggregate one reply's action outcomes into the step outcome (§3.3).

  All actions succeeded → ``succeeded``; any failed → ``failed`` (the first
  failure's reason explains the step); otherwise a timed-out action aborts
  the step. ``actions`` is never empty for an executed step — the concluding
  reply is handled as a ``final`` step before this runs.
  """
  if not actions:
    return StepOutcome.ABORTED, None
  if all(action.outcome is StepOutcome.SUCCEEDED for action in actions):
    return StepOutcome.SUCCEEDED, None
  for action in actions:
    if action.outcome is StepOutcome.FAILED:
      return StepOutcome.FAILED, action.reason
  for action in actions:
    if action.outcome is StepOutcome.ABORTED:
      return StepOutcome.ABORTED, action.reason
  return StepOutcome.ABORTED, None


_Reply = CommandReply


def _parse_reply(text: str) -> _Reply | None:
  """Parse the planner's sole supported line-oriented reply format."""
  return parse_command_reply(text)


def _invalid_action_primitive(
  reply: _Reply,
  registry: PrimitiveRegistry,
  skill_clients: SkillClients | None = None,
) -> str | None:
  """Preflight one primitive-or-skill barrier before any side effect."""
  for invocation in reply.actions:
    if invocation.action_kind == "skill":
      if (
        skill_clients is None
        or invocation.robot_id is None
        or invocation.version is None
      ):
        return "skill execution is unavailable"
      try:
        client = skill_clients.client(invocation.robot_id)
      except LookupError as exc:
        return str(exc)
      if not client.skills_available:
        return f"skills are unavailable for robot {invocation.robot_id!r}"
      skill = client.skill(invocation.action, invocation.version)
      if skill is None:
        return f"unknown skill {invocation.action!r}@{invocation.version!r}"
      visible = {
        (item.name, item.version)
        for item in registry.planner_primitives()
        if item.backend == invocation.robot_id
      }
      if any(reference not in visible for reference in skill.primitive_refs):
        return f"skill {invocation.action!r} depends on non-planner-visible primitives"
      try:
        Draft202012Validator(skill.parameters or {}).validate(invocation.arguments)
      except JsonSchemaValidationError as exc:
        return f"invalid arguments for {invocation.action!r}: {exc.message}"
      continue
    try:
      primitive = registry.resolve(
        invocation.action, invocation.robot_id, invocation.device_id
      )
    except LookupError as exc:
      return str(exc)
    if primitive is None:
      targets = sorted(
        f"{item.backend}/{item.device_id}"
        for item in registry.primitives()
        if item.name == invocation.action
      )
      if targets:
        target = f"{invocation.robot_id}/{invocation.device_id}"
        return (
          f"unknown target {target!r} for primitive {invocation.action!r}; "
          f"available targets: {targets}"
        )
      return f"unknown primitive {invocation.action!r}"
    if primitive.kind in OPERATOR_PRIMITIVE_KINDS or primitive.operator:
      return f"operator-tier primitive {invocation.action!r} is not planner-visible"
    if primitive.argument_model is not None:
      try:
        primitive.argument_model.model_validate(invocation.arguments)
      except Exception as exc:  # noqa: BLE001 — validation implementations vary
        return f"invalid arguments for {invocation.action!r}: {exc}"
  return None


def _repeated_successful_action(
  reply: _Reply, sentence: str | None, steps: list[StepRecord]
) -> ActionRecord | None:
  """Return a successful action repeated for the same semantic outcome."""
  if sentence is None:
    return None
  normalized_sentence = " ".join(sentence.lower().split())
  successful = [
    (step, action)
    for step in steps
    for action in step.actions
    if action.outcome is StepOutcome.SUCCEEDED
    and " ".join((step.sentence or "").lower().split()) == normalized_sentence
  ]
  for invocation in reply.actions:
    repeated = next(
      (
        action
        for _, action in successful
        if action.action == invocation.action
        and action.robot_id == invocation.robot_id
        and action.device_id == invocation.device_id
        and action.arguments == invocation.arguments
      ),
      None,
    )
    if repeated is not None:
      return repeated
  return None


def _reply_step_sentence(reply: _Reply, plan: list[PlanStep]) -> str | None:
  """Return the semantic outcome selected by one primitive reply."""
  selected_id = reply.plan_step_id
  if selected_id is None:
    return None
  edited = next(
    (
      edit.sentence
      for edit in reversed(reply.plan_edits)
      if edit.step_id == selected_id and edit.sentence
    ),
    None,
  )
  if edited is not None:
    return edited
  selected = next((step for step in plan if step.id == selected_id), None)
  return selected.sentence if selected is not None else None


def _apply_plan_edits(plan: list[PlanStep], edits: list[PlanEdit]) -> list[PlanStep]:
  """Apply edits without allowing completed or executing work to change."""
  result = [item.model_copy(deep=True) for item in plan]
  for edit in edits:
    existing = next((item for item in result if item.id == edit.step_id), None)
    if edit.op is PlanEditKind.ADD:
      if existing is not None:
        raise ValueError(f"plan step {edit.step_id!r} already exists")
      result.append(PlanStep(id=edit.step_id, sentence=edit.sentence or ""))
    elif edit.op is PlanEditKind.EDIT:
      if existing is None:
        raise ValueError(f"unknown plan step {edit.step_id!r}")
      if existing.required:
        raise ValueError(f"required plan step {edit.step_id!r} cannot be edited")
      if existing.status not in {PlanStepStatus.PENDING, PlanStepStatus.FAILED}:
        raise ValueError(f"plan step {edit.step_id!r} has already started")
      existing.sentence = edit.sentence or ""
      existing.status = PlanStepStatus.PENDING
      existing.commands = []
      existing.reason = None
      existing.evidence = None
    elif edit.op is PlanEditKind.REMOVE:
      if existing is None:
        raise ValueError(f"unknown plan step {edit.step_id!r}")
      if existing.required:
        raise ValueError(f"required plan step {edit.step_id!r} cannot be removed")
      if existing.status not in {PlanStepStatus.PENDING, PlanStepStatus.FAILED}:
        raise ValueError(f"plan step {edit.step_id!r} has already started")
      result = [item for item in result if item.id != edit.step_id]
    elif edit.op is PlanEditKind.COMPLETE:
      if existing is None:
        raise ValueError(f"unknown plan step {edit.step_id!r}")
      if existing.status is not PlanStepStatus.VERIFYING:
        raise ValueError(
          f"plan step {edit.step_id!r} is not awaiting explicit verification"
        )
      if not edit.sentence:
        raise ValueError(f"plan step {edit.step_id!r} completion needs evidence")
      existing.status = PlanStepStatus.DONE
      existing.evidence = edit.sentence
      existing.reason = None
    elif edit.op is PlanEditKind.RETRY:
      if existing is None:
        raise ValueError(f"unknown plan step {edit.step_id!r}")
      if existing.status not in {PlanStepStatus.VERIFYING, PlanStepStatus.FAILED}:
        raise ValueError(f"plan step {edit.step_id!r} cannot be retried yet")
      existing.status = PlanStepStatus.PENDING
      existing.commands = []
      existing.reason = None
      existing.evidence = None
    elif edit.op is PlanEditKind.FAIL:
      if existing is None:
        raise ValueError(f"unknown plan step {edit.step_id!r}")
      if existing.status in {PlanStepStatus.RUNNING, PlanStepStatus.DONE}:
        raise ValueError(f"plan step {edit.step_id!r} cannot be failed now")
      if not edit.sentence:
        raise ValueError(f"plan step {edit.step_id!r} failure needs a reason")
      existing.status = PlanStepStatus.FAILED
      existing.reason = edit.sentence
      existing.evidence = None
  return result


def _describe_plan_edits(edits: list[PlanEdit]) -> str:
  """One compact history phrase for a reply's plan mutations."""
  parts: list[str] = []
  for edit in edits:
    if edit.op is PlanEditKind.ADD:
      parts.append(f"add {edit.step_id} ({edit.sentence})")
    elif edit.op is PlanEditKind.EDIT:
      parts.append(f"edit {edit.step_id}")
    elif edit.op is PlanEditKind.REMOVE:
      parts.append(f"remove {edit.step_id}")
    elif edit.op is PlanEditKind.COMPLETE:
      parts.append(f"complete {edit.step_id}")
    elif edit.op is PlanEditKind.RETRY:
      parts.append(f"retry {edit.step_id}")
    elif edit.op is PlanEditKind.FAIL:
      parts.append(f"fail {edit.step_id}")
  return "; ".join(parts)


def _direct_mission_complete(
  direct: ActionInvocation | None,
  buffered: _BufferedStep,
  record: StepRecord,
  steps: list[StepRecord],
  plan_steps: list[PlanStep],
  pending: deque[_BufferedStep],
) -> bool:
  """Return whether one exact primitive-order mission has fully settled."""
  return bool(
    direct is not None
    and not pending
    and len(buffered.invocations) == 1
    and direct == buffered.invocations[0]
    and len(steps) == 1
    and record.outcome is StepOutcome.SUCCEEDED
    and len(record.actions) == 1
    and len(plan_steps) == 1
    and plan_steps[0].status is PlanStepStatus.DONE
    and not plan_steps[0].required
    and not plan_steps[0].terminal
  )


def _has_ready_step(plan: list[PlanStep]) -> bool:
  """Return whether the controller can execute validated pending work now."""
  return bool(_ready_step_ids(plan))


def _ready_step_ids(plan: list[PlanStep]) -> set[str]:
  """Return pending step IDs whose declared dependencies have settled."""
  by_id = {step.id: step for step in plan}
  ready: set[str] = set()
  for step in plan:
    if step.status is not PlanStepStatus.PENDING:
      continue
    dependencies = [by_id.get(dependency) for dependency in step.after]
    if any(dependency is None for dependency in dependencies):
      continue
    if all(
      dependency.status in {PlanStepStatus.DONE, PlanStepStatus.FAILED}
      for dependency in dependencies
      if dependency is not None
    ) and (
      step.terminal
      or all(
        dependency.status is PlanStepStatus.DONE
        for dependency in dependencies
        if dependency is not None
      )
    ):
      ready.add(step.id)
  return ready


def _render_plan_block(plan: list[PlanStep]) -> str:
  """The mission's semantic plan as the model sees it."""
  running = [item.id for item in plan if item.status is PlanStepStatus.RUNNING]
  ready = _ready_step_ids(plan)
  lines = ["PLAN", "CURRENT_STEP " + (running[0] if running else "none")]
  if not plan:
    lines.append("STEP none")
  else:
    for item in plan:
      role = (
        "current"
        if item.status is PlanStepStatus.RUNNING
        else "verify"
        if item.status is PlanStepStatus.VERIFYING
        else "settled"
        if item.status in {PlanStepStatus.DONE, PlanStepStatus.FAILED}
        else "ready"
        if item.id in ready
        else "future"
      )
      attributes = [
        f"role={role}",
        f"id={item.id}",
        f"state={item.status.value}",
        f"required={str(item.required).lower()}",
        "after=[" + ",".join(item.after) + "]",
        f"terminal={str(item.terminal).lower()}",
        "text=" + json.dumps(item.sentence, ensure_ascii=False),
      ]
      lines.append("STEP " + " ".join(attributes))
      for command in item.commands:
        lines.append(f"  COMMAND {command}")
      if item.reason:
        lines.append("  REASON " + json.dumps(item.reason, ensure_ascii=False))
      if item.evidence:
        lines.append("  EVIDENCE " + json.dumps(item.evidence, ensure_ascii=False))
  return "\n".join(lines)


def _render_execution_history(history: list[str]) -> str | None:
  """Render the chronological event ledger since the latest LLM summary."""
  if not history:
    return None
  return "\n".join(
    f"EVENT {index}: {event}" for index, event in enumerate(history, start=1)
  )


def _unsummarized_plan_steps(
  plan: list[PlanStep], *, summarized: bool
) -> list[PlanStep]:
  """Hide settled step detail after it has entered the Mission summary."""
  if not summarized:
    return plan
  return [
    step
    for step in plan
    if step.status not in {PlanStepStatus.DONE, PlanStepStatus.FAILED}
  ]


def _render_active_mission(mission: MissionRecord) -> str:
  """Render the planner's authoritative active Mission with explicit state."""
  return " ".join(
    (
      "MISSION",
      "role=active",
      f"id={mission.id[:8]}",
      f"state={mission.state.value}",
      "order=" + json.dumps(mission.order, ensure_ascii=False),
    )
  )


def _reasoning_mode_hint(mode: str) -> str:
  """Short, provider-neutral selection guidance embedded in the prompt."""
  return {
    "none": "fastest reply; use for simple acknowledgements and direct actions",
    "low": "light deliberation; use for routine multi-step work",
    "medium": "balanced speed and deliberation; use when state is uncertain",
    "high": "deepest deliberation; reserve for complex, risky, or ambiguous work",
  }.get(mode, "choose only when its extra deliberation is useful")


def _first_json_object(text: str) -> object | None:
  """Parse the first balanced ``{...}`` span in ``text`` as JSON."""
  start = text.find("{")
  while start != -1:
    depth = 0
    in_string = False
    escaped = False
    for index in range(start, len(text)):
      char = text[index]
      if in_string:
        if escaped:
          escaped = False
        elif char == "\\":
          escaped = True
        elif char == '"':
          in_string = False
        continue
      if char == '"':
        in_string = True
      elif char == "{":
        depth += 1
      elif char == "}":
        depth -= 1
        if depth == 0:
          try:
            return json.loads(text[start : index + 1])
          except json.JSONDecodeError:
            break
    start = text.find("{", start + 1)
  return None


# Keep the formerly exported class name available to downstream integrations.
ReActPlanner = DecisionPlanner
