"""Mission table and northbound mission-command wire models."""

from __future__ import annotations

from datetime import datetime
from enum import StrEnum
from typing import Annotated, Any, Literal

from pydantic import BaseModel, ConfigDict, Field, RootModel, StrictInt

from egia.api.plan import PlanProjection


class MissionState(StrEnum):
  """Lifecycle of one admitted order."""

  PENDING = "pending"
  RUNNING = "running"
  SUCCEEDED = "succeeded"
  FAILED = "failed"
  CANCELLED = "cancelled"


class MissionPriorityBand(StrEnum):
  """Operator-selected priority band, independent of the dynamic score."""

  LOW = "low"
  NORMAL = "normal"
  HIGH = "high"
  URGENT = "urgent"


class _MissionCommandRequest(BaseModel):
  """Shared strict fields for one mission command."""

  model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)

  correlation_id: str | None = Field(
    default=None, description="optional client ID for log cross-reference"
  )


class MissionCreateRequest(_MissionCommandRequest):
  """Admit one deterministic operator mission."""

  operation: Literal["create"]
  order: str = Field(min_length=1)
  priority_band: MissionPriorityBand = MissionPriorityBand.NORMAL


class _MissionMutationRequest(_MissionCommandRequest):
  """Shared compare-and-swap fields for a pending-mission mutation."""

  mission_id: str = Field(
    pattern=(
      r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-"
      r"[0-9a-f]{4}-[0-9a-f]{12}$"
    )
  )
  expected_revision: StrictInt = Field(ge=1)


class MissionUpdateRequest(_MissionMutationRequest):
  """Replace the order of one never-started pending mission."""

  operation: Literal["update"]
  order: str = Field(min_length=1)


class MissionPriorityRequest(_MissionMutationRequest):
  """Change the priority band of one pending mission."""

  operation: Literal["priority"]
  priority_band: MissionPriorityBand


class MissionCancelRequest(_MissionMutationRequest):
  """Cancel one pending mission."""

  operation: Literal["cancel"]


MissionCommand = Annotated[
  MissionCreateRequest
  | MissionUpdateRequest
  | MissionPriorityRequest
  | MissionCancelRequest,
  Field(discriminator="operation"),
]


class MissionCommandRequest(RootModel[MissionCommand]):
  """Strict operation-specific request accepted by the mission command route."""


class MissionCommandMission(BaseModel):
  """Accepted mission state sufficient for optimistic client updates."""

  mission_id: str
  order: str
  state: MissionState
  priority_band: MissionPriorityBand
  revision: int = Field(ge=1)


class MissionCommandAck(BaseModel):
  """Acknowledgement for one accepted mission command."""

  ok: Literal[True] = True
  operation: Literal["create", "update", "priority", "cancel"]
  correlation_id: str | None = None
  mission: MissionCommandMission


MissionCommandErrorCode = Literal[
  "invalid_json",
  "invalid_request",
  "queue_full",
  "mission_not_found",
  "mission_revision_conflict",
  "mission_not_pending",
  "mission_protected",
  "mission_started",
  "mission_mutation_failed",
  "storage_failure",
  "internal_failure",
  "unavailable",
]


class MissionCommandError(BaseModel):
  """Stable rejection envelope for one mission command."""

  ok: Literal[False] = False
  operation: Literal["create", "update", "priority", "cancel"] | None = None
  correlation_id: str | None = None
  error: MissionCommandErrorCode
  reason: str | None = None


class MissionRecord(BaseModel):
  """One admitted mission and its terminal outcome.

  The record is the audit trail of the main chain: what was ordered, when it
  ran, and how it ended. ``result`` carries the planner's final answer on
  success; ``error`` the failure reason otherwise.
  """

  id: str
  order: str
  source: str
  state: MissionState = MissionState.PENDING
  submitted_at: datetime
  started_at: datetime | None = None
  finished_at: datetime | None = None
  result: str | None = None
  error: str | None = None
  correlation_id: str | None = None
  origin: Literal["operator", "self"] = "operator"
  priority_band: MissionPriorityBand = MissionPriorityBand.NORMAL
  priority: float = Field(default=0.0, description="mission-table score (§3.9)")
  urgency: float = Field(default=0.5, ge=0, le=1, description="proposer urgency hint")
  value: float = Field(default=0.5, ge=0, le=1, description="proposer value hint")
  preempted_count: int = Field(
    default=0, ge=0, description="step-boundary preemptions so far"
  )
  est_cost_s: float | None = Field(
    default=None,
    description="estimated duration from recalled plans; None = default (§3.9)",
  )
  mission_template_id: str | None = None
  mission_template_version: int | None = Field(default=None, ge=1)


class MissionTableEntry(BaseModel):
  """One mission as seen by mission-table observers (the TUI console)."""

  mission_id: str
  order: str
  mission_template_id: str | None = None
  mission_template_version: int | None = Field(default=None, ge=1)
  origin: Literal["operator", "self"]
  priority_band: MissionPriorityBand = MissionPriorityBand.NORMAL
  class_rank: int = Field(description="0 = operator, 1 = self-proposed (§3.9)")
  state: MissionState
  priority: float
  urgency: float
  value: float
  est_cost_s: float | None = None
  preempted_count: int = 0
  submitted_at: datetime
  started_at: datetime | None = None
  finished_at: datetime | None = None
  result: str | None = None
  error: str | None = None
  document: MissionDocumentProjection | None = None


class MissionHistoryEntry(BaseModel):
  """One live-session mission summary for operators.

  A mission owns exactly one entry.  The queue refreshes its state and
  timestamp as the mission progresses instead of appending lifecycle events.
  ``steps`` is the mission document's current executable work, so completed
  missions retain their action outcomes in the northbound history view.
  """

  mission_id: str
  order: str
  state: MissionState
  priority_band: MissionPriorityBand = MissionPriorityBand.NORMAL
  timestamp: datetime
  detail: str | None = None
  revision: int = Field(default=1, ge=1)
  completeness: float = Field(default=0.0, ge=0.0, le=1.0)
  steps: list[MissionHistoryStep] = Field(default_factory=list)
  document: MissionDocumentProjection | None = None


class MissionHistoryAction(BaseModel):
  """One action in a northbound mission-history record."""

  index: int = Field(ge=1)
  robot_id: str
  device_id: str | None = None
  intent: str = ""
  action: str
  command: str = ""
  arguments: dict[str, Any] = Field(default_factory=dict)
  state: str
  result: Any | None = None
  reason: str | None = None


class MissionHistoryStep(BaseModel):
  """One ordered step in a northbound mission-history record."""

  index: int = Field(ge=1)
  id: str | None = None
  sentence: str = ""
  state: str
  result: Any | None = None
  reason: str | None = None
  required: bool = False
  after: list[str] = Field(default_factory=list)
  terminal: bool = False
  evidence: str | None = None
  actions: list[MissionHistoryAction] = Field(default_factory=list)


class MissionPlanProjection(BaseModel):
  """One plan node; nested subplans retain their own steps and status."""

  plan_id: str
  title: str
  revision: int
  state: str
  result: Any | None = None
  reason: str | None = None
  completeness: float = Field(ge=0.0, le=1.0)
  steps: list[MissionHistoryStep] = Field(default_factory=list)
  subplans: list[MissionPlanProjection] = Field(default_factory=list)


class MissionDocumentProjection(BaseModel):
  """Authoritative model-owned mission and its complete plan tree."""

  revision: int
  completeness: float = Field(ge=0.0, le=1.0)
  plan: MissionPlanProjection | None = None


class MissionTableSnapshot(BaseModel):
  """Live view of the mission table — the chain's mission-list stage.

  Ordered: the running mission first, then pending by table rank
  ``(class_rank, −priority_band, −score, submitted_at)``, then the most recent finished
  missions. ``history`` is the bounded, chronological live-session transition
  log. ``plan`` is the running mission's action list — the operator view of
  its in-flight plan (steps with per-action outcomes and semantic work) —
  whenever one is running, else ``None``.
  """

  missions: list[MissionTableEntry]
  history: list[MissionHistoryEntry] = Field(default_factory=list)
  plan: PlanProjection | None = None
