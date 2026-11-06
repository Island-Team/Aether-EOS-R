"""EOS primitive acknowledgement contract."""

from __future__ import annotations

from enum import StrEnum
from typing import Any, Literal, Self

from pydantic import BaseModel, ConfigDict, Field, model_validator

from egia.contracts.source import load_json


class RejectReason(StrEnum):
  """Machine-readable EOS primitive rejection taxonomy."""

  ESTOP = "rejected.estop"
  MODE = "rejected.mode"
  POSTURE = "rejected.posture"
  INVALID_STATE = "rejected.invalid_state"
  BUSY = "rejected.busy"
  UNSUPPORTED = "rejected.unsupported"
  INVALID_ARGUMENTS = "rejected.invalid_arguments"
  INVALID_TARGET = "rejected.invalid_target"
  UNSUPPORTED_REFERENCE = "rejected.unsupported_reference"
  UNAVAILABLE_DEPENDENCY = "rejected.unavailable_dependency"
  UNAUTHORIZED = "rejected.unauthorized"
  COMMAND_CONFLICT = "rejected.command_conflict"
  INTERNAL = "rejected.internal"


class TerminalStatus(StrEnum):
  """Terminal outcome of an EOS primitive command."""

  SUCCEEDED = "succeeded"
  REJECTED = "rejected"
  FAILED = "failed"
  CANCELLED = "cancelled"
  TIMED_OUT = "timed_out"
  OUTCOME_UNKNOWN = "outcome_unknown"


class PrimitiveAck(BaseModel):
  """Result envelope for one versioned primitive invocation."""

  model_config = ConfigDict(extra="forbid")

  type: Literal["primitive.ack"] = "primitive.ack"
  primitive_version: str = Field(
    pattern=r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$"
  )
  primitive: str
  accepted: bool
  reason: str | None = None
  reason_code: RejectReason | None = None
  command_id: str | None
  sequence: int = Field(ge=0)
  result: dict[str, Any]

  @model_validator(mode="after")
  def _reason_matches_acceptance(self) -> Self:
    if self.accepted:
      if self.reason is not None or self.reason_code is not None:
        raise ValueError("accepted acknowledgement requires null rejection reason")
    elif not self.reason or self.reason_code is None:
      raise ValueError("rejected acknowledgement requires reason and reason_code")
    try:
      TerminalStatus(self.result.get("status"))
    except (TypeError, ValueError) as exc:
      raise ValueError("result.status must be a terminal status") from exc
    return self

  @property
  def status(self) -> TerminalStatus:
    """Return the validated terminal status from the semantic result."""
    return TerminalStatus(self.result["status"])

  @property
  def succeeded(self) -> bool:
    """Whether EOS confirmed the primitive's terminal success boundary."""
    return self.status is TerminalStatus.SUCCEEDED

  @classmethod
  def model_json_schema(cls, *args, **kwargs) -> dict:
    del args, kwargs
    return load_json("eos/schemas/eos/actions/ack.v1.json")
