"""Plan record models — the versioned, outcome-judged plans-as-memory artifact.

``docs/egia/egia-2607.md`` §3.3: a mission must not end as result/error strings
alone — it ends as a structured record persisted in episodic memory, so later
decompositions can recall what was attempted and how it ended. These models
are that record's shape. Pure data: the interface layer performs no I/O and
holds no state; persistence belongs to ``core/memory`` and outcome judging to
the planner plus (later) the operator review path.

Shape history: schema v1 recorded each executed motion as its own flat
``StepRecord``. Schema v2 grouped concurrent actions but exposed the editable
plan as a generic checklist. Schema v3 names the hierarchy directly: one Mission
goal, one editable Plan, semantic Plan steps, and concurrent commands inside an
executed step. Schema v4 adds protected-step, dependency, terminal, evidence,
and explicit verification state. Old rows migrate on read through
schema v5 adds typed action identity plus durable in-flight skill watch cursors.
Old rows migrate on read through :meth:`PlanRecord._migrate`.
"""

from __future__ import annotations

from datetime import UTC, datetime
from enum import StrEnum
from typing import Any, Literal
from uuid import uuid4

from pydantic import BaseModel, ConfigDict, Field, model_validator

__all__ = [
  "ActionProjection",
  "ActionRecord",
  "ActiveAction",
  "DecisionGraph",
  "DecisionNode",
  "DecisionNodeKind",
  "PlanAnnotation",
  "PlanEdit",
  "PlanEditKind",
  "PlanProjection",
  "PlanRecord",
  "PlanStep",
  "PlanStepProjection",
  "PlanStepStatus",
  "PlannedStepProjection",
  "StepOutcome",
  "StepRecord",
]


class StepOutcome(StrEnum):
  """Terminal outcome of one step; reused as the plan-level outcome.

  The planner only ever sets ``succeeded``, ``failed``, and ``aborted``.
  ``effective`` and ``ineffective`` are reserved for the operator review
  path (a later milestone): judging whether a *succeeded* step actually
  moved the mission toward its goal is a human decision recorded on the
  plan, never inferred from the ack alone.
  """

  SUCCEEDED = "succeeded"
  FAILED = "failed"
  ABORTED = "aborted"
  EFFECTIVE = "effective"
  INEFFECTIVE = "ineffective"


class PlanStepStatus(StrEnum):
  """Runtime-owned lifecycle of one semantic plan step."""

  PENDING = "pending"
  RUNNING = "running"
  VERIFYING = "verifying"
  DONE = "done"
  FAILED = "failed"


class PlanEditKind(StrEnum):
  """Edits the model may make to work that is not currently executing."""

  ADD = "add"
  EDIT = "edit"
  REMOVE = "remove"
  COMPLETE = "complete"
  RETRY = "retry"
  FAIL = "fail"


class DecisionNodeKind(StrEnum):
  """Semantic role of one node in the model-authored decision graph."""

  OBSERVE = "observe"
  ACTION = "action"
  VERIFY = "verify"
  DECISION = "decision"
  RECOVERY = "recovery"
  FINISH = "finish"


class DecisionNode(BaseModel):
  """One operator-visible unit in a validated mission decision graph."""

  id: str = Field(min_length=1, description="stable node identifier")
  title: str = Field(min_length=1, description="short operator-facing purpose")
  kind: DecisionNodeKind
  after: list[str] = Field(
    default_factory=list, description="node IDs that must complete first"
  )
  requires: list[str] = Field(
    default_factory=list, description="observable facts required before this node"
  )
  verify: str = Field(
    min_length=1, description="observable condition proving this node complete"
  )
  on_success: list[str] = Field(
    default_factory=list, description="next node IDs after verification succeeds"
  )
  on_failure: list[str] = Field(
    default_factory=list, description="recovery or alternative node IDs"
  )
  parallel_group: str | None = Field(
    default=None, description="same label identifies independent parallel-ready work"
  )


class DecisionGraph(BaseModel):
  """Validated plan produced before EGIA enters execution mode."""

  goal: str = Field(min_length=1, description="observable mission postcondition")
  revision: int = Field(default=1, ge=1)
  nodes: list[DecisionNode] = Field(min_length=1)

  @model_validator(mode="after")
  def _validate_graph(self) -> DecisionGraph:
    by_id = {node.id: node for node in self.nodes}
    if len(by_id) != len(self.nodes):
      raise ValueError("decision graph node IDs must be unique")
    for node in self.nodes:
      references = [*node.after, *node.on_success, *node.on_failure]
      unknown = [target for target in references if target not in by_id]
      if unknown:
        raise ValueError(
          f"decision graph node {node.id!r} references unknown nodes: {unknown}"
        )
      if node.id in references:
        raise ValueError(f"decision graph node {node.id!r} references itself")

    # ``after`` is the execution dependency relation. Branch edges describe
    # routing and may intentionally return to a recovery decision, but hard
    # dependencies must remain a DAG so a ready layer always exists.
    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(node_id: str) -> None:
      if node_id in visiting:
        raise ValueError("decision graph contains a dependency cycle")
      if node_id in visited:
        return
      visiting.add(node_id)
      for dependency in by_id[node_id].after:
        visit(dependency)
      visiting.remove(node_id)
      visited.add(node_id)

    for node_id in by_id:
      visit(node_id)
    return self


class ActionRecord(BaseModel):
  """One executed motion: what was issued, what happened, what it cost.

  ``index`` is the 1-based position within its step. ``action`` names the
  invoked primitive or skill; skill records retain exact request and execution
  identity so recovery never has to submit the operation again.
  ``duration_s`` is wall-clock seconds from the action's gate call to its
  ack enqueued.
  """

  index: int = Field(ge=1, description="1-based position within the step")
  robot_id: str = Field(default="default", min_length=1)
  device_id: str | None = None
  intent: str = Field(default="", description="why the model says this action exists")
  preconditions: list[str] = Field(
    default_factory=list, description="state the model says this action assumes"
  )
  action: str = Field(min_length=1, description="invoked primitive name")
  action_kind: Literal["primitive", "skill"] = "primitive"
  version: str | None = None
  request_id: str | None = None
  execution_id: str | None = None
  execution_status: str | None = None
  command: str = Field(default="", description="semantic command issued by the model")
  arguments: dict = Field(
    default_factory=dict, description="unvalidated invocation body"
  )
  outcome: StepOutcome
  reason: str | None = Field(default=None, description="failure explanation, else None")
  result: Any | None = Field(default=None, description="backend primitive result")
  duration_s: float = Field(default=0.0, ge=0.0)


class ActiveAction(BaseModel):
  """Durable identity and cursor for one non-terminal EOS skill execution."""

  step_index: int = Field(ge=1)
  action_index: int = Field(ge=1)
  robot_id: str = Field(min_length=1)
  action: str = Field(min_length=1)
  version: str = Field(min_length=1)
  arguments: dict[str, Any] = Field(default_factory=dict)
  request_id: str = Field(min_length=1)
  execution_id: str = Field(min_length=1)
  sequence: int = Field(default=0, ge=0)
  status: str = Field(min_length=1)
  cancellation_requested: bool = False


class PlanStep(BaseModel):
  """One semantic sentence in the mission's runtime-editable plan."""

  id: str = Field(min_length=1, description="model-supplied short identifier")
  sentence: str = Field(min_length=1, description="one semantic sentence")
  status: PlanStepStatus = PlanStepStatus.PENDING
  commands: list[str] = Field(
    default_factory=list,
    description="independent commands used on the latest execution attempt",
  )
  reason: str | None = None
  required: bool = Field(
    default=False, description="template outcome that cannot be edited or removed"
  )
  after: list[str] = Field(
    default_factory=list, description="step IDs that must settle before execution"
  )
  terminal: bool = Field(
    default=False, description="step that must succeed before the Mission may finish"
  )
  evidence: str | None = Field(
    default=None, description="fresh evidence accepted for explicit completion"
  )


class PlanEdit(BaseModel):
  """One add, edit, or remove mutation of the semantic plan."""

  model_config = ConfigDict(str_strip_whitespace=True)

  op: PlanEditKind
  step_id: str = Field(min_length=1)
  sentence: str | None = Field(default=None, description="add/edit payload")


class StepRecord(BaseModel):
  """One model reply as executed: its motions and plan edits.

  ``index`` is 1-based execution order over replies — a reply that issued
  several motions records them as one step with multiple actions. ``final``
  is the concluding answer; a final step issues no actions. ``plan_edits``
  log the semantic-plan mutations the reply carried. Cost accounting (tokens,
  retries, duration) belongs to the reply, so it lives on the step, not per
  action. ``outcome`` aggregates the actions: all succeeded → ``succeeded``;
  any failed → ``failed``; a timed-out action aborts the step.
  """

  index: int = Field(ge=1, description="1-based position in execution order")
  outcome: StepOutcome
  reason: str | None = Field(default=None, description="failure explanation, else None")
  prompt_tokens: int = Field(default=0, ge=0)
  completion_tokens: int = Field(default=0, ge=0)
  retries: int = Field(default=0, ge=0)
  duration_s: float = Field(default=0.0, ge=0.0)
  final: str | None = Field(default=None, description="concluding answer, else None")
  plan_step_id: str | None = Field(
    default=None, description="semantic plan step executed by this barrier"
  )
  sentence: str | None = Field(default=None, description="plan sentence at execution")
  actions: list[ActionRecord] = Field(default_factory=list)
  plan_edits: list[PlanEdit] = Field(default_factory=list)


class PlanAnnotation(BaseModel):
  """A side-loop verdict stamped onto a live plan (§3.3).

  Audit and monitor findings land on the plan while it executes, so the
  recorded outcome explains not only what failed but when the world moved
  out from under the plan. ``source`` is the emitting loop (``"audit"`` or
  ``"monitor"``); ``event`` is the machine-parsable verdict name (e.g.
  ``"fault_active"``, ``"estop"``, ``"step_stall"``); ``detail`` is the
  optional human-readable explanation.
  """

  timestamp: datetime = Field(
    default_factory=lambda: datetime.now(UTC),
    description="annotation time, UTC",
  )
  source: str = Field(description="emitting side loop: audit or monitor")
  event: str = Field(description="machine-parsable verdict name")
  detail: str = Field(default="", description="optional explanation")


class PlanRecord(BaseModel):
  """A finished mission as one remembered, outcome-judged plan (§3.3).

  ``outcome`` is the plan-level judgment: ``succeeded`` on a final answer,
  ``failed`` on step cap / parse-retry exhaustion / model transport failure,
  ``aborted`` when shutdown fired mid-mission. Steps left unexecuted at cap
  or abort are simply absent — the record covers what ran, never a
  speculative remainder. ``plan`` is the final forward-looking semantic plan
  the model edited as it worked. ``schema_version``
  gates future shape changes: recall code migrates by version, never in
  place (see :meth:`_migrate_v1`).
  """

  schema_version: Literal["5"] = "5"
  id: str = Field(default_factory=lambda: str(uuid4()), description="uuid string")
  mission_id: str
  priority: float = 0.0
  revision: int = Field(default=1, ge=1)
  order: str
  source: str
  started_at: datetime
  finished_at: datetime
  outcome: StepOutcome
  result: str | None = None
  error: str | None = None
  steps: list[StepRecord] = Field(default_factory=list)
  plan: list[PlanStep] = Field(default_factory=list)
  decision_mode: Literal["plan", "execute"] = Field(
    default="plan", description="last plan/execute phase reached by the mission"
  )
  decision_graph: DecisionGraph | None = Field(
    default=None, description="latest runtime-validated model-authored plan"
  )
  annotations: list[PlanAnnotation] = Field(
    default_factory=list,
    description="side-loop verdicts stamped while the plan executed",
  )
  active_actions: list[ActiveAction] = Field(
    default_factory=list,
    description="non-terminal skill executions to reconcile after restart",
  )
  reviewed_at: datetime | None = Field(
    default=None,
    description="operator review time, UTC; None until the plan is promoted",
  )
  review_note: str | None = Field(
    default=None,
    description="operator note recorded at promotion; None until reviewed",
  )

  @model_validator(mode="before")
  @classmethod
  def _migrate(cls, data: Any) -> Any:
    """Migrate schema-v1/v2/v3 rows to the current semantic-plan schema.

    Every memory store loads plans through ``PlanRecord.model_validate_json``,
    so one before-validator is the single choke point. A v1 row's steps —
    one step per motion — become v2 steps with a single action each; the v1
    concluding step (``tool=None``) becomes a step with no actions and no
    ``final`` (v1 never recorded the answer on the step). Storage is never
    rewritten in place; the migration applies per read (§3.3).
    """
    if not isinstance(data, dict) or data.get("schema_version") not in {
      "1",
      "2",
      "3",
      "4",
    }:
      return data
    if data.get("schema_version") == "4":
      return {**data, "schema_version": "5", "active_actions": []}
    if data.get("schema_version") == "3":
      return {**data, "schema_version": "5", "active_actions": []}
    if data.get("schema_version") == "2":
      migrated = dict(data)
      migrated["schema_version"] = "5"
      migrated["plan"] = [
        {
          "id": item.get("id", "step"),
          "sentence": item.get("title", "planned work"),
          "status": (
            "failed"
            if item.get("status") == "removed"
            else item.get("status", "pending")
          ),
        }
        for item in data.get("todos", [])
        if isinstance(item, dict)
      ]
      migrated.pop("todos", None)
      for step in migrated.get("steps", []):
        if isinstance(step, dict):
          step["plan_edits"] = []
          step.pop("todo_ops", None)
      return migrated
    steps: list[dict[str, Any]] = []
    for step in data.get("steps") or []:
      if not isinstance(step, dict):
        steps.append(step)
        continue
      actions: list[dict[str, Any]] = []
      if step.get("tool") is not None:
        actions.append(
          {
            "index": 1,
            "intent": step.get("intent", ""),
            "preconditions": step.get("preconditions", []),
            "action": step["tool"],
            "arguments": step.get("arguments", {}),
            "outcome": step.get("outcome"),
            "reason": step.get("reason"),
            "duration_s": step.get("duration_s", 0.0),
          }
        )
      steps.append(
        {
          "index": step.get("index"),
          "outcome": step.get("outcome"),
          "reason": step.get("reason"),
          "prompt_tokens": step.get("prompt_tokens", 0),
          "completion_tokens": step.get("completion_tokens", 0),
          "retries": step.get("retries", 0),
          "duration_s": step.get("duration_s", 0.0),
          "final": None,
          "actions": actions,
          "plan_edits": [],
        }
      )
    return {
      **data,
      "schema_version": "5",
      "steps": steps,
      "plan": [],
      "active_actions": [],
    }


class ActionProjection(BaseModel):
  """One motion of the operator plan view — a projection, never a copy."""

  index: int
  robot_id: str = "default"
  device_id: str | None = None
  intent: str = ""
  action: str | None = None
  action_kind: Literal["primitive", "skill"] = "primitive"
  version: str | None = None
  request_id: str | None = None
  execution_id: str | None = None
  execution_status: str | None = None
  command: str = ""
  arguments: dict[str, Any] = Field(
    default_factory=dict,
    description="primitive parameters sent with this action",
  )
  outcome: StepOutcome
  state: str
  result: Any | None = None
  reason: str | None = None


class PlanStepProjection(BaseModel):
  """One executed concurrent step in the operator plan view.

  Carries what the operator needs to follow execution: the step's position,
  its aggregate outcome, its motions with their outcomes, and the concluding
  answer when the step ended the mission. Cost accounting (tokens, retries,
  duration) stays on the record itself.
  """

  index: int
  outcome: StepOutcome
  state: str
  result: Any | None = None
  reason: str | None = None
  final: str | None = None
  actions: list[ActionProjection] = Field(default_factory=list)


class PlannedStepProjection(BaseModel):
  """One semantic plan step as the operator sees it."""

  id: str
  sentence: str
  status: PlanStepStatus
  commands: list[str] = Field(default_factory=list)
  reason: str | None = None
  required: bool = False
  after: list[str] = Field(default_factory=list)
  terminal: bool = False
  evidence: str | None = None


class PlanProjection(BaseModel):
  """The working view of a live mission plan (§3.3).

  A projection of the in-flight :class:`PlanRecord` — the steps executed so
  far, the mission's current semantic steps, and the count of side-loop
  annotations — never a separate list that dies with the run. Emitted on
  northbound frames while a mission runs.
  """

  plan_id: str
  mission_id: str
  priority: float = 0.0
  revision: int = Field(default=1, ge=1)
  completeness: float = Field(default=0.0, ge=0.0, le=1.0)
  steps: list[PlanStepProjection] = Field(default_factory=list)
  plan: list[PlannedStepProjection] = Field(default_factory=list)
  decision_mode: Literal["plan", "execute"] = "plan"
  decision_graph: DecisionGraph | None = None
  annotations: int = Field(ge=0, description="side-loop verdicts stamped so far")
