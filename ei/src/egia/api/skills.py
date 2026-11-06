"""Northbound wire models for descriptive native EOS skills."""

from __future__ import annotations

from typing import Annotated, Any, Literal

from pydantic import BaseModel, ConfigDict, Field, RootModel


class _StrictModel(BaseModel):
  model_config = ConfigDict(extra="forbid", strict=True)


class SkillPrimitiveReference(_StrictModel):
  """One exact primitive identity declared by a native EOS skill."""

  name: str
  version: str


class SkillResource(_StrictModel):
  """One resource template advertised as planning input for a skill."""

  uri_template: str
  description: str


class SkillDescriptor(_StrictModel):
  """One deployment-admitted EOS skill descriptor."""

  backend: str
  eos_robot_id: str
  name: str
  version: str
  description: str
  primitives: list[str] = Field(default_factory=list)
  primitive_refs: list[SkillPrimitiveReference] = Field(default_factory=list)
  parameters_schema: dict[str, Any] = Field(default_factory=dict)
  returns_schema: dict[str, Any] = Field(default_factory=dict)
  resources: list[SkillResource] = Field(default_factory=list)


class SkillsSnapshot(_StrictModel):
  """Version-two skill catalog with explicit service availability."""

  schema_version: Literal[2] = 2
  available: bool = False
  skills: list[SkillDescriptor] = Field(default_factory=list)
  unavailable_backends: list[str] = Field(default_factory=list)
  count: int = 0


class SkillIdentity(_StrictModel):
  """Exact native-skill identity on one configured robot alias."""

  robot_id: str = Field(min_length=1, max_length=128)
  name: str = Field(min_length=1, max_length=256)
  version: str = Field(min_length=1, max_length=64)


class SkillLookupRequest(SkillIdentity):
  """Resolve one exact skill descriptor from EOS."""

  operation: Literal["get_skill"]


class SkillStartRequest(SkillIdentity):
  """Start one idempotent EOS skill execution."""

  operation: Literal["start"]
  request_id: str = Field(min_length=1, max_length=256)
  arguments: dict[str, Any] = Field(default_factory=dict)
  timeout_s: float = Field(gt=0, le=3600)


class SkillGetExecutionRequest(_StrictModel):
  """Read the current complete execution snapshot."""

  operation: Literal["get_execution"]
  robot_id: str = Field(min_length=1, max_length=128)
  execution_id: str = Field(min_length=1, max_length=256)


class SkillCancelRequest(_StrictModel):
  """Request cooperative cancellation of one retained execution."""

  operation: Literal["cancel"]
  robot_id: str = Field(min_length=1, max_length=128)
  execution_id: str = Field(min_length=1, max_length=256)


SkillCommand = Annotated[
  SkillLookupRequest
  | SkillStartRequest
  | SkillGetExecutionRequest
  | SkillCancelRequest,
  Field(discriminator="operation"),
]


class SkillCommandRequest(RootModel[SkillCommand]):
  """Strict tagged request accepted by the skill command route."""


SkillExecutionStatus = Literal[
  "queued", "running", "succeeded", "rejected", "cancelled", "timed_out", "failed"
]


class SkillExecutionSnapshot(_StrictModel):
  """One complete ordered EOS execution snapshot."""

  robot_id: str
  execution_id: str
  request_id: str
  name: str
  version: str
  status: SkillExecutionStatus
  sequence: int = Field(ge=0)
  cancellation_requested: bool
  reason_code: str | None = None
  message: str | None = None
  result: dict[str, Any] | None = None


class SkillWatchRequest(_StrictModel):
  """Open an execution watch after the last processed EOS sequence."""

  robot_id: str = Field(min_length=1, max_length=128)
  execution_id: str = Field(min_length=1, max_length=256)
  after_sequence: int = Field(default=0, ge=0)


class SkillCommandAck(_StrictModel):
  """Successful response from a skill command."""

  ok: Literal[True] = True
  operation: Literal["get_skill", "start", "get_execution", "cancel"]
  descriptor: SkillDescriptor | None = None
  execution: SkillExecutionSnapshot | None = None


SkillCommandErrorCode = Literal[
  "invalid_json",
  "invalid_request",
  "robot_not_found",
  "skill_not_found",
  "execution_not_found",
  "request_conflict",
  "capacity_exhausted",
  "unavailable",
  "timeout",
  "internal_failure",
]


class SkillCommandError(_StrictModel):
  """Stable skill route failure envelope."""

  ok: Literal[False] = False
  operation: str | None = None
  error: SkillCommandErrorCode
  reason: str | None = None


__all__ = [
  "SkillCancelRequest",
  "SkillCommandAck",
  "SkillCommandError",
  "SkillCommandErrorCode",
  "SkillCommandRequest",
  "SkillDescriptor",
  "SkillExecutionSnapshot",
  "SkillGetExecutionRequest",
  "SkillLookupRequest",
  "SkillPrimitiveReference",
  "SkillResource",
  "SkillStartRequest",
  "SkillWatchRequest",
  "SkillsSnapshot",
]
