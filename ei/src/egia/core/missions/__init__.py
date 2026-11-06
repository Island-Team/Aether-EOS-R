"""Mission table — the sole admission gate for missions, operator or autonomy.

Every order enters the decision layer through here (the web chain, §3 of
``docs/egia/egia-2607.md``; autonomy's proposer, §3.9): a bounded table of pending
missions, executed strictly one at a time. Serial execution is the safety
property — two orders never interleave primitive calls on one robot — and the
bound is the backpressure: a full table rejects new submissions instead of
growing unbounded latency.

Pending order is by ``(class_rank, −band, −score, submitted_at)`` (§3.9): operator
missions are class 0 and always outrank self-proposed ones regardless of
score; within a class the §3.9 priority score decides and submission time
breaks ties. Scores are recomputed at selection time — the age term grows
while a mission waits — so ``best_pending()`` and the next ``wait_pending``
hand-off always see current priorities.
"""

from __future__ import annotations

import asyncio
import json
import time
import uuid
from collections.abc import Awaitable, Callable
from datetime import UTC, datetime
from enum import Enum
from typing import Literal

from egia.api.mission import (
  MissionHistoryEntry,
  MissionPriorityBand,
  MissionRecord,
  MissionState,
)
from egia.api.plan import PlanRecord, PlanStep, PlanStepStatus, StepOutcome
from egia.core.missions.document import (
  ActionState,
  MissionAction,
  MissionDocument,
  MissionPlan,
  MissionRevision,
  MissionStep,
  PlanState,
  StepState,
  validate_edit,
)
from egia.core.missions.priority import (
  MissionPriority,
  PriorityWeights,
  score_mission,
  should_preempt,
)
from egia.core.missions.store import MissionCheckpoint, MissionStore
from egia.lib import shared_events
from egia.lib.config import MissionTemplateConfig, PriorityConfig
from egia.lib.logger import get_logger

__all__ = [
  "ActionState",
  "MissionConflictError",
  "MissionDocument",
  "MissionMutationError",
  "MissionNotFoundError",
  "MissionNotPendingError",
  "MissionPlan",
  "MissionProtectedError",
  "MissionQueue",
  "MissionRevision",
  "MissionStartedError",
  "MissionStore",
  "ResubmitOutcome",
  "StepState",
]


class ResubmitOutcome(Enum):
  """Why :meth:`MissionQueue.resubmit` did — or did not — re-admit."""

  ADMITTED = "admitted"
  PREEMPTION_CAP = "preemption_cap"
  TABLE_FULL = "table_full"


class MissionMutationError(ValueError):
  """Base error for stable mapping of mission mutation failures."""

  code = "mission_mutation_failed"


class MissionNotFoundError(MissionMutationError):
  """The requested mission does not exist."""

  code = "mission_not_found"


class MissionConflictError(MissionMutationError):
  """The expected document revision is stale."""

  code = "mission_revision_conflict"


class MissionNotPendingError(MissionMutationError):
  """The mission is not physically present in the pending table."""

  code = "mission_not_pending"


class MissionProtectedError(MissionMutationError):
  """The requested edit would alter protected template work."""

  code = "mission_protected"


class MissionStartedError(MissionMutationError):
  """The requested edit requires work that has never started."""

  code = "mission_started"


def _weights(config: PriorityConfig) -> PriorityWeights:
  return PriorityWeights(
    w_urgency=config.w_urgency,
    w_value=config.w_value,
    w_cost=config.w_cost,
    w_age=config.w_age,
    age_tau_s=config.age_tau_s,
    default_cost_s=config.default_cost_s,
    operator_urgency=config.operator_urgency,
    operator_value=config.operator_value,
    preempt_margin=config.preempt_margin,
    max_preemptions=config.max_preemptions,
  )


class MissionQueue:
  """Bounded mission table with priority hand-off to the planner."""

  def __init__(
    self,
    capacity: int,
    priority: PriorityConfig | None = None,
    clock: Callable[[], float] | None = None,
    on_finished: Callable[[MissionRecord], None] | None = None,
    store: MissionStore | None = None,
  ) -> None:
    if capacity < 1:
      raise ValueError("mission queue capacity must be at least 1")
    self._capacity = capacity
    self._priority = priority or PriorityConfig()
    self._weights = _weights(self._priority)
    self._clock = clock or time.time
    self._pending: list[MissionRecord] = []
    self._records: list[MissionRecord] = []
    self._history: list[MissionHistoryEntry] = []
    self._documents: dict[str, MissionDocument] = {}
    self._revisions: dict[str, list[MissionRevision]] = {}
    self._progress_signatures: dict[str, tuple[object, ...]] = {}
    self._changed = asyncio.Event()
    self._listeners: list[Callable[[], None]] = []
    self._on_finished = on_finished
    self._store = store
    self._log = get_logger("egia.core.missions")

  def add_listener(self, callback: Callable[[], None]) -> Callable[[], None]:
    """Observe every table mutation (submit, resubmit, start, finish).

    Returns the unsubscribe callable. Listeners are plain sync callbacks so
    northbound streams can raise their own per-connection events; they must
    not touch the table.
    """
    self._listeners.append(callback)

    def _remove() -> None:
      try:
        self._listeners.remove(callback)
      except ValueError:
        pass

    return _remove

  def _notify(self) -> None:
    for callback in list(self._listeners):
      callback()

  @property
  def capacity(self) -> int:
    return self._capacity

  @property
  def priority_config(self) -> PriorityConfig:
    return self._priority

  def pending(self) -> list[MissionRecord]:
    """Return isolated snapshots of physically pending missions."""
    return [record.model_copy(deep=True) for record in self._pending]

  def restore(self, records: list[MissionRecord]) -> None:
    """Re-admit missions retained across an atomic backend-session change.

    Running work becomes pending because the old backend's unstarted action
    horizon is intentionally discarded.  Mission identity, priority inputs,
    submission order, and preemption history remain unchanged.
    """
    for source in records:
      if any(record.id == source.id for record in self._records):
        continue
      record = source.model_copy(deep=True)
      record.state = MissionState.PENDING
      record.started_at = None
      record.finished_at = None
      record.result = None
      record.error = None
      self._rescore(record)
      document = MissionDocument(
        mission_id=record.id,
        order=record.order,
        priority=record.priority,
        priority_band=record.priority_band,
      )
      revisions = [
        MissionRevision(
          revision=document.revision, document=document.model_copy(deep=True)
        )
      ]
      if self._store is not None:
        self._store.save_mission(record, document, revisions)
      self._pending.append(record)
      self._records.append(record)
      self._documents[record.id] = document
      self._revisions[record.id] = revisions
      self._record_history(record, detail="restored after backend promotion")
      self._log.info(
        "mission_restored",
        mission_id=record.id,
        restore_source="backend_handoff",
        previous_state=source.state.value,
        state=record.state.value,
        revision=document.revision,
        has_plan=False,
      )
    if records:
      self._changed.set()
      self._notify()

  def restore_checkpoints(
    self, checkpoints: list[MissionCheckpoint]
  ) -> dict[str, PlanRecord]:
    """Restore durable non-terminal missions as pending planner work.

    A mission that was running cannot safely continue at an old primitive
    cursor. Its identity and completed plan history survive, but it re-enters
    the table as pending so the planner can inspect fresh state and replan.

    Returns:
        Latest plan records keyed by mission id for the planner resume seam.

    Raises:
        ValueError: If the durable backlog exceeds this table's capacity.
    """
    active = [
      checkpoint
      for checkpoint in checkpoints
      if checkpoint.mission.state in (MissionState.PENDING, MissionState.RUNNING)
    ]
    if len(active) > self._capacity:
      raise ValueError(
        "durable mission backlog exceeds mission capacity: "
        f"{len(active)} > {self._capacity}"
      )
    plans: dict[str, PlanRecord] = {}
    for checkpoint in active:
      if any(record.id == checkpoint.mission.id for record in self._records):
        continue
      record = checkpoint.mission.model_copy(deep=True)
      record.state = MissionState.PENDING
      record.started_at = None
      record.finished_at = None
      record.result = None
      record.error = None
      self._rescore(record)
      document = checkpoint.document.model_copy(deep=True)
      document.state = MissionState.PENDING
      document.result = None
      document.reason = None
      if document.plan is not None:
        document.plan.state = PlanState.STOPPED
      document.revision += 1
      revisions = [revision.model_copy(deep=True) for revision in checkpoint.revisions]
      revisions.append(
        MissionRevision(
          revision=document.revision,
          document=document.model_copy(deep=True),
        )
      )
      if checkpoint.plan is not None:
        plans[record.id] = checkpoint.plan.model_copy(deep=True)
      if self._store is not None:
        self._store.save_mission(record, document, revisions)
      self._pending.append(record)
      self._records.append(record)
      self._documents[record.id] = document
      self._revisions[record.id] = revisions
      self._record_history(record, detail="restored from durable checkpoint")
      self._log.info(
        "mission_restored",
        mission_id=record.id,
        restore_source="checkpoint",
        previous_state=checkpoint.mission.state.value,
        state=record.state.value,
        revision=document.revision,
        has_plan=checkpoint.plan is not None,
      )
    if active:
      self._changed.set()
      self._notify()
    return plans

  @property
  def running_mission(self) -> MissionRecord | None:
    """The one RUNNING mission, if any — the safety gate's scope anchor."""
    record = next(
      (record for record in self._records if record.state == MissionState.RUNNING),
      None,
    )
    return record.model_copy(deep=True) if record is not None else None

  def records(self) -> list[MissionRecord]:
    """All admitted missions, oldest first — including finished ones."""
    return [record.model_copy(deep=True) for record in self._records]

  def render_for_prompt(self) -> str:
    """Render running and pending work as a compact model-facing table."""
    active = [
      record for record in self._records if record.state is MissionState.RUNNING
    ]
    pending = sorted(self._pending, key=lambda record: self._entry(record).order_key)
    if not active and not pending:
      return "MISSION_TABLE empty"
    lines = ["MISSION_TABLE"]
    for record in active:
      lines.append(self._render_prompt_record(record, role="running"))
    for position, record in enumerate(pending, start=1):
      lines.append(self._render_prompt_record(record, role="pending", queue=position))
    return "\n".join(lines)

  @staticmethod
  def _render_prompt_record(
    record: MissionRecord, *, role: str, queue: int | None = None
  ) -> str:
    fields = [
      f"role={role}",
      f"id={record.id[:8]}",
      f"state={record.state.value}",
      f"origin={record.origin}",
      f"band={record.priority_band.value}",
    ]
    if queue is not None:
      fields.append(f"queue={queue}")
    fields.append("order=" + json.dumps(record.order, ensure_ascii=False))
    return "MISSION " + " ".join(fields)

  def get(self, mission_id: str) -> MissionRecord | None:
    """Return an exact isolated record snapshot, or ``None`` when unknown."""
    record = next((item for item in self._records if item.id == mission_id), None)
    return record.model_copy(deep=True) if record is not None else None

  def history(self) -> list[MissionHistoryEntry]:
    """Bounded live-session lifecycle transitions, oldest first."""
    return [entry.model_copy(deep=True) for entry in self._history]

  def document(self, mission_id: str) -> MissionDocument | None:
    """Return an isolated snapshot of the mission's current revision."""
    document = self._documents.get(mission_id)
    return document.model_copy(deep=True) if document is not None else None

  def revisions(self, mission_id: str) -> list[MissionRevision]:
    """Every accepted document revision, oldest first."""
    return [
      revision.model_copy(deep=True) for revision in self._revisions.get(mission_id, [])
    ]

  def plan_steps(self, mission_id: str) -> list[PlanStep]:
    """Return the current semantic plan as isolated planner steps."""
    document = self._documents.get(mission_id)
    if document is None or document.plan is None:
      return []
    statuses = {
      StepState.PENDING: PlanStepStatus.PENDING,
      StepState.EXECUTING: PlanStepStatus.RUNNING,
      StepState.VERIFYING: PlanStepStatus.VERIFYING,
      StepState.DONE: PlanStepStatus.DONE,
      StepState.FAILED: PlanStepStatus.FAILED,
    }
    return [
      PlanStep(
        id=step.id or f"step-{step.index}",
        sentence=step.sentence,
        status=statuses[step.state],
        commands=[action.command for action in step.actions if action.command],
        reason=step.reason,
        required=step.required,
        after=list(step.after),
        terminal=step.terminal,
        evidence=step.evidence,
      )
      for step in document.plan.steps
    ]

  def revise(self, document: MissionDocument) -> MissionDocument:
    """Accept a validated edit of not-yet-started mission work."""
    current = self._documents.get(document.mission_id)
    if current is None:
      raise KeyError(document.mission_id)
    validate_edit(current, document)
    accepted = document.model_copy(deep=True)
    revisions = [
      revision.model_copy(deep=True)
      for revision in self._revisions[document.mission_id]
    ]
    revisions.append(
      MissionRevision(
        revision=accepted.revision, document=accepted.model_copy(deep=True)
      )
    )
    if self._store is not None:
      self._store.save_mission(self._record(document.mission_id), accepted, revisions)
    self._documents[document.mission_id] = accepted
    self._revisions[document.mission_id] = revisions
    self._log.debug(
      "mission_document_revised",
      mission_id=document.mission_id,
      mutation="edit",
      revision=accepted.revision,
    )
    self._notify()
    return accepted.model_copy(deep=True)

  def update_order(
    self, mission_id: str, order: str, *, expected_revision: int
  ) -> MissionRecord:
    """Replace an ordinary never-started pending order using revision CAS."""
    record, document = self._pending_mutation(mission_id, expected_revision)
    if record.mission_template_id is not None:
      raise MissionProtectedError("template missions cannot change their order")
    if (
      record.started_at is not None
      or record.preempted_count
      or self._work_started(document)
    ):
      raise MissionStartedError("mission work has begun and its order cannot change")
    normalized = order.strip()
    if not normalized:
      raise MissionMutationError("mission order must not be blank")

    updated = record.model_copy(deep=True)
    updated.order = normalized
    revised = document.model_copy(deep=True)
    revised.order = normalized
    revised.revision += 1
    if revised.plan is not None:
      revised.plan.title = normalized
    revisions = self._next_revision(mission_id, revised)
    self._persist_staged(updated, revised, revisions)
    self._publish_revision(
      record,
      updated,
      revised,
      revisions,
      mutation="order",
      detail="order updated",
    )
    return updated.model_copy(deep=True)

  def reprioritize(
    self,
    mission_id: str,
    priority_band: MissionPriorityBand,
    *,
    expected_revision: int,
  ) -> MissionRecord:
    """Change the band of any physically pending mission using revision CAS."""
    record, document = self._pending_mutation(mission_id, expected_revision)
    priority_band = MissionPriorityBand(priority_band)
    updated = record.model_copy(deep=True)
    updated.priority_band = priority_band
    revised = document.model_copy(deep=True)
    revised.priority_band = priority_band
    revised.revision += 1
    revisions = self._next_revision(mission_id, revised)
    self._persist_staged(updated, revised, revisions)
    self._publish_revision(
      record,
      updated,
      revised,
      revisions,
      mutation="priority",
      detail=f"reprioritized {priority_band.value}",
    )
    return updated.model_copy(deep=True)

  def cancel(
    self,
    mission_id: str,
    *,
    expected_revision: int,
    reason: str = "cancelled",
  ) -> MissionRecord:
    """Cancel a physically pending mission and retain its terminal checkpoint."""
    record, document = self._pending_mutation(mission_id, expected_revision)
    cancelled = record.model_copy(deep=True)
    cancelled.state = MissionState.CANCELLED
    cancelled.finished_at = datetime.now(UTC)
    cancelled.result = None
    cancelled.error = reason
    revised = document.model_copy(deep=True)
    revised.state = MissionState.CANCELLED
    revised.result = None
    revised.reason = reason
    revised.revision += 1
    if revised.plan is not None:
      revised.plan.state = PlanState.STOPPED
      revised.plan.reason = reason
    revisions = self._next_revision(mission_id, revised)
    self._persist_staged(cancelled, revised, revisions)

    self._pending.remove(record)
    self._copy_record(record, cancelled)
    self._documents[mission_id] = revised
    self._revisions[mission_id] = revisions
    self._record_history(record, detail=reason)
    if self._on_finished is not None:
      self._on_finished(record.model_copy(deep=True))
    self._changed.set()
    self._notify()
    self._log.info("mission_cancelled", mission_id=mission_id, reason=reason)
    shared_events.emit(
      "egia_mission_terminal", fields={"mission_id": mission_id, "outcome": "cancelled"}
    )
    return cancelled.model_copy(deep=True)

  def _pending_mutation(
    self, mission_id: str, expected_revision: int
  ) -> tuple[MissionRecord, MissionDocument]:
    record = next((item for item in self._records if item.id == mission_id), None)
    if record is None:
      raise MissionNotFoundError(f"unknown mission {mission_id!r}")
    if record not in self._pending:
      raise MissionNotPendingError(f"mission {mission_id!r} is not physically pending")
    document = self._documents[mission_id].model_copy(deep=True)
    if document.revision != expected_revision:
      raise MissionConflictError(
        f"mission revision is {document.revision}, expected {expected_revision}"
      )
    return record, document

  @staticmethod
  def _work_started(document: MissionDocument) -> bool:
    if document.state is not MissionState.PENDING:
      return True
    if any(step.state is not StepState.PENDING for step in document.steps):
      return True
    if document.plan is None:
      return False
    return document.plan.state is not PlanState.DRAFT or any(
      step.state is not StepState.PENDING for step in document.plan.steps
    )

  def _next_revision(
    self, mission_id: str, document: MissionDocument
  ) -> list[MissionRevision]:
    revisions = [item.model_copy(deep=True) for item in self._revisions[mission_id]]
    revisions.append(
      MissionRevision(
        revision=document.revision, document=document.model_copy(deep=True)
      )
    )
    return revisions

  def _persist_staged(
    self,
    record: MissionRecord,
    document: MissionDocument,
    revisions: list[MissionRevision],
  ) -> None:
    if self._store is not None:
      self._store.save_mission(record, document, revisions)

  @staticmethod
  def _copy_record(target: MissionRecord, source: MissionRecord) -> None:
    for name in MissionRecord.model_fields:
      setattr(target, name, getattr(source, name))

  def _publish_revision(
    self,
    record: MissionRecord,
    updated: MissionRecord,
    document: MissionDocument,
    revisions: list[MissionRevision],
    *,
    mutation: str,
    detail: str,
  ) -> None:
    self._copy_record(record, updated)
    self._documents[record.id] = document
    self._revisions[record.id] = revisions
    self._record_history(record, detail=detail)
    self._changed.set()
    self._log.debug(
      "mission_document_revised",
      mission_id=record.id,
      mutation=mutation,
      revision=document.revision,
      priority_band=document.priority_band.value,
    )
    self._notify()

  def sync_plan(self, record: PlanRecord, *, active: bool) -> MissionDocument:
    """Synchronize the planner's real execution record into its mission graph."""
    current_document = self._documents[record.mission_id]
    semantic_plan = list(record.plan)
    if not semantic_plan and record.steps:
      # Schema-v1/v2 records and legacy JSON replies have executed barriers
      # but no authored semantic plan. Preserve those actions in the mission
      # tree instead of presenting an empty plan to operators.
      semantic_plan = [
        PlanStep(
          id=step.plan_step_id or f"step-{step.index}",
          sentence=step.sentence or f"Execute recorded step {step.index}.",
          status=(
            PlanStepStatus.DONE
            if step.outcome is StepOutcome.SUCCEEDED
            else PlanStepStatus.FAILED
          ),
          commands=[action.command for action in step.actions if action.command],
          reason=step.reason,
        )
        for step in record.steps
      ]
    executed = {
      (step.plan_step_id or f"step-{step.index}"): step for step in record.steps
    }
    state = (
      PlanState.RUNNING
      if active
      else PlanState.SUCCEEDED
      if record.outcome is StepOutcome.SUCCEEDED
      else PlanState.STOPPED
      if record.outcome is StepOutcome.ABORTED
      else PlanState.FAILED
    )
    plan = MissionPlan(
      plan_id=record.id,
      title=record.order,
      revision=record.revision,
      state=state,
      result=record.result,
      reason=record.error,
      steps=[
        MissionStep(
          index=index,
          id=planned.id,
          sentence=planned.sentence,
          state=(
            StepState.PENDING
            if planned.status is PlanStepStatus.PENDING
            else StepState.EXECUTING
            if planned.status is PlanStepStatus.RUNNING
            else StepState.VERIFYING
            if planned.status is PlanStepStatus.VERIFYING
            else StepState.DONE
            if planned.status is PlanStepStatus.DONE
            else StepState.FAILED
          ),
          result=(executed[planned.id].final if planned.id in executed else None),
          reason=planned.reason,
          required=planned.required,
          after=list(planned.after),
          terminal=planned.terminal,
          evidence=planned.evidence,
          actions=[
            MissionAction(
              index=action.index,
              robot_id=action.robot_id,
              device_id=action.device_id,
              action=action.action,
              command=action.command,
              arguments=action.arguments,
              intent=action.intent,
              state=(
                ActionState.DONE
                if action.outcome is StepOutcome.SUCCEEDED
                else ActionState.FAILED
              ),
              result=action.result,
              reason=action.reason,
            )
            for action in (
              executed[planned.id].actions if planned.id in executed else []
            )
          ],
        )
        for index, planned in enumerate(semantic_plan, start=1)
      ],
      subplans=(
        current_document.plan.subplans if current_document.plan is not None else []
      ),
    )
    document = current_document.model_copy(deep=True)
    document.plan = plan
    document.revision += 1
    document.state = MissionState.RUNNING if active else document.state
    revisions = [
      revision.model_copy(deep=True) for revision in self._revisions[record.mission_id]
    ]
    revisions.append(
      MissionRevision(
        revision=document.revision, document=document.model_copy(deep=True)
      )
    )
    if self._store is not None:
      mission = self._record(record.mission_id)
      self._store.save_plan(
        mission,
        document,
        revisions,
        record,
        active=active,
      )
    self._documents[record.mission_id] = document
    self._revisions[record.mission_id] = revisions
    self._log.debug(
      "mission_plan_synced",
      mission_id=record.mission_id,
      plan_id=record.id,
      revision=document.revision,
      plan_state=plan.state.value,
      active=active,
      step_count=len(plan.steps),
    )
    statuses = tuple(step.state.value for step in plan.steps)
    current_step = next(
      (
        step
        for step in plan.steps
        if step.state in (StepState.EXECUTING, StepState.VERIFYING)
      ),
      None,
    )
    signature = (
      plan.plan_id,
      plan.revision,
      plan.state.value,
      active,
      record.decision_mode,
      statuses,
      tuple(step.outcome.value for step in record.steps),
    )
    if self._progress_signatures.get(record.mission_id) != signature:
      self._progress_signatures[record.mission_id] = signature
      self._log.info(
        "mission_progress",
        mission_id=record.mission_id,
        plan_id=record.id,
        plan_revision=record.revision,
        document_revision=document.revision,
        plan_state=plan.state.value,
        active=active,
        decision_mode=record.decision_mode,
        total_steps=len(plan.steps),
        pending_steps=sum(step.state is StepState.PENDING for step in plan.steps),
        executing_steps=sum(step.state is StepState.EXECUTING for step in plan.steps),
        verifying_steps=sum(step.state is StepState.VERIFYING for step in plan.steps),
        done_steps=sum(step.state is StepState.DONE for step in plan.steps),
        failed_steps=sum(step.state is StepState.FAILED for step in plan.steps),
        current_step_id=current_step.id if current_step is not None else None,
        settled_barriers=len(record.steps),
        latest_outcome=(record.steps[-1].outcome.value if record.steps else None),
      )
    self._notify()
    return document.model_copy(deep=True)

  def begin_step(self, mission_id: str, step_index: int) -> MissionDocument:
    """Make one pending barrier eligible and mark all of its actions executing."""
    document = self._documents[mission_id].model_copy(deep=True)
    step = next((item for item in document.steps if item.index == step_index), None)
    if step is None:
      raise KeyError(f"step {step_index}")
    if step.state is not StepState.PENDING:
      raise ValueError(f"step {step_index} is not pending")
    previous = [item for item in document.steps if item.index < step_index]
    if any(item.state not in (StepState.DONE, StepState.FAILED) for item in previous):
      raise ValueError(f"step {step_index} is blocked by an earlier step")
    step.state = StepState.EXECUTING
    document.state = MissionState.RUNNING
    for action in step.actions:
      action.state = ActionState.EXECUTING
    self._persist_document(mission_id, document)
    self._documents[mission_id] = document
    self._log.debug(
      "mission_step_started",
      mission_id=mission_id,
      step_index=step_index,
      action_count=len(step.actions),
    )
    self._notify()
    return document.model_copy(deep=True)

  def complete_action(
    self,
    mission_id: str,
    step_index: int,
    action_index: int,
    *,
    result: object | None = None,
    reason: str | None = None,
  ) -> MissionDocument:
    """Record one terminal action result and close its barrier when complete."""
    document = self._documents[mission_id].model_copy(deep=True)
    step = next((item for item in document.steps if item.index == step_index), None)
    if step is None:
      raise KeyError(f"step {step_index}")
    action = next((item for item in step.actions if item.index == action_index), None)
    if action is None:
      raise KeyError(f"action {action_index}")
    if action.state is not ActionState.EXECUTING:
      raise ValueError(f"action {action_index} is not executing")
    action.result = result
    action.reason = reason
    action.state = ActionState.FAILED if reason is not None else ActionState.DONE
    if all(
      item.state in (ActionState.DONE, ActionState.FAILED) for item in step.actions
    ):
      failed = next(
        (item for item in step.actions if item.state is ActionState.FAILED), None
      )
      step.state = StepState.FAILED if failed is not None else StepState.DONE
      step.reason = failed.reason if failed is not None else None
      step.result = [item.result for item in step.actions]
      if all(
        item.state in (StepState.DONE, StepState.FAILED) for item in document.steps
      ):
        failed_step = next(
          (item for item in document.steps if item.state is StepState.FAILED), None
        )
        document.state = (
          MissionState.FAILED if failed_step is not None else MissionState.SUCCEEDED
        )
        document.reason = failed_step.reason if failed_step is not None else None
        document.result = [item.result for item in document.steps]
    self._persist_document(mission_id, document)
    self._documents[mission_id] = document
    self._log.debug(
      "mission_action_completed",
      mission_id=mission_id,
      step_index=step_index,
      action_index=action_index,
      action_state=action.state.value,
      step_state=step.state.value,
      mission_state=document.state.value,
    )
    self._notify()
    return document.model_copy(deep=True)

  async def execute_next(
    self,
    mission_id: str,
    executor: Callable[[object], Awaitable[object]],
  ) -> MissionDocument | None:
    """Execute the next eligible barrier, launching its actions concurrently.

    The executor is the safety/primitive boundary. It receives an isolated
    ``MissionAction`` snapshot; a returned value is the action result and an
    exception is recorded as its failure reason. Every invocation is made
    anew, so accepting a revision never bypasses safety admission.
    """
    document = self._documents[mission_id]
    step = next(
      (item for item in document.steps if item.state is StepState.PENDING), None
    )
    if step is None:
      return None
    self.begin_step(mission_id, step.index)

    async def _run(action_index: int) -> tuple[int, object | None, str | None]:
      action = next(item for item in step.actions if item.index == action_index)
      try:
        result = await executor(action.model_copy(deep=True))
      except Exception as exc:  # noqa: BLE001 — executor boundary becomes result state
        return action_index, None, str(exc)
      return action_index, result, None

    outcomes = await asyncio.gather(*[_run(action.index) for action in step.actions])
    for action_index, result, reason in outcomes:
      self.complete_action(
        mission_id,
        step.index,
        action_index,
        result=result,
        reason=reason,
      )
    return self.document(mission_id)

  def _record_history(
    self, record: MissionRecord, *, detail: str | None = None
  ) -> None:
    timestamp = datetime.now(UTC)
    existing_index = next(
      (
        index
        for index, entry in enumerate(self._history)
        if entry.mission_id == record.id
      ),
      None,
    )
    if existing_index is not None:
      entry = self._history.pop(existing_index)
      entry.order = record.order
      entry.state = record.state
      entry.priority_band = record.priority_band
      entry.timestamp = timestamp
      entry.detail = detail
      # Keep the list chronological by most recent mission update while
      # retaining exactly one entry for this mission.
      self._history.append(entry)
      return
    self._history.append(
      MissionHistoryEntry(
        mission_id=record.id,
        order=record.order,
        state=record.state,
        priority_band=record.priority_band,
        timestamp=timestamp,
        detail=detail,
      )
    )
    if len(self._history) > 100:
      del self._history[:-100]

  def _record(self, mission_id: str) -> MissionRecord:
    return next(record for record in self._records if record.id == mission_id)

  def _persist_document(self, mission_id: str, document: MissionDocument) -> None:
    if self._store is not None:
      self._store.save_mission(
        self._record(mission_id), document, self._revisions[mission_id]
      )

  def self_pending_count(self) -> int:
    return sum(1 for record in self._pending if record.origin == "self")

  def _rescore(self, record: MissionRecord) -> float:
    entry = score_mission(
      origin=record.origin,
      band=record.priority_band,
      urgency=record.urgency,
      value=record.value,
      est_cost_s=record.est_cost_s,
      submitted_at=record.submitted_at.timestamp(),
      config=self._weights,
      now=self._clock(),
    )
    record.priority = entry.score
    return entry.score

  def _sort_key(self, record: MissionRecord) -> tuple[int, int, float, float]:
    entry = self._entry(record)
    record.priority = entry.score
    return entry.order_key

  async def submit(
    self,
    order: str,
    *,
    source: str,
    correlation_id: str | None = None,
    origin: Literal["operator", "self"] = "operator",
    priority_band: MissionPriorityBand = MissionPriorityBand.NORMAL,
    urgency: float = 0.5,
    value: float = 0.5,
    est_cost_s: float | None = None,
    template: MissionTemplateConfig | None = None,
  ) -> MissionRecord | None:
    """Admit one mission. Returns ``None`` when the pending table is full.

    ``origin="self"`` admits autonomy-proposed missions (§3.9); they enter
    the same table and the same gate — origin changes priority class, never
    the execution path.
    """
    if len(self._pending) >= self._capacity:
      self._log.warning("mission_rejected", reason="queue_full", source=source)
      return None
    record = MissionRecord(
      id=str(uuid.uuid4()),
      order=(template.mission if template is not None else order).strip(),
      source=source,
      submitted_at=datetime.now(UTC),
      correlation_id=correlation_id,
      origin=origin,
      priority_band=priority_band,
      urgency=urgency,
      value=value,
      est_cost_s=est_cost_s,
      mission_template_id=template.id if template is not None else None,
      mission_template_version=template.version if template is not None else None,
    )
    self._rescore(record)
    document = MissionDocument(
      mission_id=record.id,
      order=record.order,
      priority=record.priority,
      priority_band=record.priority_band,
      plan=(
        MissionPlan(
          plan_id=f"{record.id}:template:{template.id}@{template.version}",
          title=record.order,
          steps=[
            MissionStep(
              index=index,
              id=step.id,
              sentence=step.outcome,
              required=step.required,
              after=list(step.after),
              terminal=step.terminal,
            )
            for index, step in enumerate(template.steps, start=1)
          ],
        )
        if template is not None
        else None
      ),
    )
    revisions = [
      MissionRevision(
        revision=document.revision, document=document.model_copy(deep=True)
      )
    ]
    if self._store is not None:
      self._store.save_mission(record, document, revisions)
    self._pending.append(record)
    self._records.append(record)
    self._documents[record.id] = document
    self._revisions[record.id] = revisions
    self._record_history(record)
    self._log.info(
      "mission_submitted",
      mission_id=record.id,
      state=record.state.value,
      source=source,
      origin=origin,
      priority_band=record.priority_band.value,
      priority=round(record.priority, 3),
      mission_template=record.mission_template_id,
      document_revision=document.revision,
      pending_count=len(self._pending),
    )
    self._changed.set()
    self._notify()
    return record.model_copy(deep=True)

  def best_pending(self) -> MissionRecord | None:
    """The current head of the table, scores refreshed — for preemption checks."""
    if not self._pending:
      return None
    return min(self._pending, key=self._sort_key).model_copy(deep=True)

  def _entry(self, record: MissionRecord) -> MissionPriority:
    return score_mission(
      origin=record.origin,
      band=record.priority_band,
      urgency=record.urgency,
      value=record.value,
      est_cost_s=record.est_cost_s,
      submitted_at=record.submitted_at.timestamp(),
      config=self._weights,
      now=self._clock(),
    )

  def check_preemption(self, running: MissionRecord) -> MissionRecord | None:
    """The pending mission the running one should yield to, if any (§3.9).

    Class dominance plus the same-class score margin — the planner consults
    this at each step boundary and never mid-primitive call.
    """
    best = self.best_pending()
    if best is None or best.id == running.id:
      return None
    return (
      best
      if should_preempt(self._entry(running), self._entry(best), self._weights)
      else None
    )

  def _pop_best(self) -> MissionRecord:
    best = min(self._pending, key=self._sort_key)
    self._pending.remove(best)
    self._log.info(
      "mission_selected",
      mission_id=best.id,
      state=best.state.value,
      origin=best.origin,
      priority_band=best.priority_band.value,
      priority=round(best.priority, 3),
      pending_count=len(self._pending),
    )
    return best.model_copy(deep=True)

  async def wait_pending(self, stop: asyncio.Event) -> MissionRecord | None:
    """Block until a mission is pending or ``stop`` is set; hands off the head."""
    while not stop.is_set():
      if self._pending:
        return self._pop_best()
      self._changed.clear()
      wait_changed = asyncio.create_task(self._changed.wait())
      wait_stop = asyncio.create_task(stop.wait())
      done, pending = await asyncio.wait(
        {wait_changed, wait_stop}, return_when=asyncio.FIRST_COMPLETED
      )
      for task in pending:
        task.cancel()
      if wait_changed not in done:
        return None
    return None

  def resubmit(self, record: MissionRecord) -> ResubmitOutcome:
    """Return a preempted mission to the table (§3.9 step-boundary yield).

    ``PREEMPTION_CAP`` means the mission is past ``max_preemptions`` and
    ``TABLE_FULL`` that the pending table has no room; the caller finishes
    the mission as ``FAILED`` on either, with the outcome carrying the real
    cause. Neither rejection mutates the record — it keeps its ``running``
    state so nothing re-selects it in between.
    """
    current = self._record(record.id)
    if current.preempted_count >= self._priority.max_preemptions:
      self._log.warning(
        "mission_preemption_cap", mission_id=record.id, count=current.preempted_count
      )
      return ResubmitOutcome.PREEMPTION_CAP
    if len(self._pending) >= self._capacity:
      self._log.warning(
        "mission_resubmit_rejected", mission_id=record.id, reason="queue_full"
      )
      return ResubmitOutcome.TABLE_FULL
    resumed = current.model_copy(deep=True)
    resumed.preempted_count += 1
    resumed.state = MissionState.PENDING
    resumed.started_at = None
    self._rescore(resumed)
    document = self._documents[record.id].model_copy(deep=True)
    document.state = MissionState.PENDING
    if document.plan is not None:
      document.plan.state = PlanState.STOPPED
    if self._store is not None:
      self._store.save_mission(resumed, document, self._revisions[record.id])
    self._copy_record(current, resumed)
    self._copy_record(record, resumed)
    self._documents[record.id] = document
    self._pending.append(current)
    self._record_history(current, detail=f"preempted {current.preempted_count} time(s)")
    self._changed.set()
    self._notify()
    self._log.info(
      "mission_resubmitted",
      mission_id=record.id,
      from_state=MissionState.RUNNING.value,
      state=current.state.value,
      count=current.preempted_count,
      pending_count=len(self._pending),
    )
    return ResubmitOutcome.ADMITTED

  def mark_running(self, record: MissionRecord) -> None:
    current = self._record(record.id)
    running = current.model_copy(deep=True)
    running.state = MissionState.RUNNING
    running.started_at = datetime.now(UTC)
    document = self._documents[record.id].model_copy(deep=True)
    document.state = MissionState.RUNNING
    if self._store is not None:
      try:
        self._store.save_mission(running, document, self._revisions[record.id])
      except Exception:
        # wait_pending removes the record before this transition. Keep the
        # durable pending mission selectable if storage is temporarily down.
        if current not in self._pending:
          self._pending.append(current)
          self._changed.set()
        raise
    self._copy_record(current, running)
    self._copy_record(record, running)
    self._documents[record.id] = document
    self._record_history(current)
    self._log.info(
      "mission_started",
      mission_id=record.id,
      from_state=MissionState.PENDING.value,
      state=running.state.value,
      started_at=running.started_at,
      priority_band=running.priority_band.value,
      priority=round(running.priority, 3),
      preempted_count=running.preempted_count,
      pending_count=len(self._pending),
    )
    shared_events.emit("egia_mission_started", fields={"mission_id": record.id})
    self._notify()

  def finish(
    self,
    record: MissionRecord,
    *,
    state: MissionState,
    result: str | None = None,
    error: str | None = None,
  ) -> None:
    """Close a mission with a terminal outcome (``succeeded`` or ``failed``)."""
    if state not in (MissionState.SUCCEEDED, MissionState.FAILED):
      raise ValueError(f"terminal state must be succeeded or failed, got {state}")
    current = self._record(record.id)
    finished = current.model_copy(deep=True)
    finished.state = state
    finished.finished_at = datetime.now(UTC)
    finished.result = result
    finished.error = error
    document = self._documents[record.id].model_copy(deep=True)
    document.state = state
    document.result = result
    document.reason = error
    if self._store is not None:
      self._store.save_mission(finished, document, self._revisions[record.id])
    self._copy_record(current, finished)
    self._copy_record(record, finished)
    self._documents[record.id] = document
    self._record_history(
      current, detail=result if state == MissionState.SUCCEEDED else error
    )
    if self._on_finished is not None:
      self._on_finished(current.model_copy(deep=True))
    self._log.info(
      "mission_finished",
      mission_id=record.id,
      from_state=MissionState.RUNNING.value,
      state=state.value,
      duration_s=round(
        (
          finished.finished_at - (finished.started_at or finished.submitted_at)
        ).total_seconds(),
        3,
      ),
      has_result=result is not None,
      error=error,
    )
    shared_events.emit(
      "egia_mission_terminal",
      fields={"mission_id": record.id, "outcome": state.value},
    )
    self._notify()

  def suspend(self, record: MissionRecord, *, detail: str) -> None:
    """Return interrupted running work to the durable pending table."""
    current = self._record(record.id)
    suspended = current.model_copy(deep=True)
    suspended.state = MissionState.PENDING
    suspended.started_at = None
    self._rescore(suspended)
    document = self._documents[record.id].model_copy(deep=True)
    document.state = MissionState.PENDING
    if document.plan is not None:
      document.plan.state = PlanState.STOPPED
    if self._store is not None:
      self._store.save_mission(suspended, document, self._revisions[record.id])
    self._copy_record(current, suspended)
    self._copy_record(record, suspended)
    self._documents[record.id] = document
    self._pending.append(current)
    self._record_history(current, detail=detail)
    self._changed.set()
    self._log.info(
      "mission_suspended",
      mission_id=record.id,
      from_state=MissionState.RUNNING.value,
      state=suspended.state.value,
      reason_code="interrupted",
      pending_count=len(self._pending),
    )
    shared_events.emit(
      "egia_mission_terminal", fields={"mission_id": record.id, "outcome": "suspended"}
    )
    self._notify()
