"""Wire models for direct operator primitive invocation.

``/api/aether/egia/primitive/invoke`` accepts a discovered primitive and its
arguments, then runs the call through the same safety
gate as every planner step — the route adds a caller, never a bypass.
Pure data: the models below are the request/ack/error frames; the handler
lives in ``egia.io`` and the gate in ``core/safety``.
"""

from __future__ import annotations

from typing import Any, Literal

from pydantic import BaseModel, Field, model_validator

from egia.contracts.actions.ack import RejectReason, TerminalStatus
from egia.contracts.endpoints import PrimitiveKind


class PrimitiveInvokeRequest(BaseModel):
  """One operator primitive call with an explicit EOS target.

  ``arguments`` is the unvalidated body, exactly like a planner step —
  schema validation belongs to the safety gate, not the route.
  """

  primitive: str = Field(min_length=1, description="primitive name from the catalog")
  robot_id: str = Field(min_length=1, description="stable configured robot alias")
  device_id: str | None = Field(
    default=None,
    description=("generated EOS device id; omitted only for process-wide safety.estop"),
  )
  arguments: dict[str, Any] = Field(
    default_factory=dict, description="raw arguments; the gate validates them"
  )
  correlation_id: str | None = Field(
    default=None, description="optional client ID for log cross-reference"
  )

  @model_validator(mode="after")
  def _target_matches_primitive(self) -> PrimitiveInvokeRequest:
    if "command_id" in self.arguments:
      raise ValueError("arguments.command_id is reserved for EGIA")
    try:
      kind = PrimitiveKind(self.primitive)
    except ValueError:
      kind = None
    if kind in {PrimitiveKind.ROBOT_STOP, PrimitiveKind.SAFETY_ESTOP}:
      if self.device_id not in {None, ""}:
        raise ValueError(f"{self.primitive} requires device_id to be omitted")
      self.device_id = None
    elif kind is not None and not self.device_id:
      raise ValueError(f"{self.primitive} requires device_id")
    return self


class PrimitiveInvokeAck(BaseModel):
  """Executed call — the gate admitted it and the backend answered."""

  ok: Literal[True] = True
  accepted: bool = Field(description="backend-side admission of the call")
  command_id: str = Field(description="EGIA-minted durable EOS command identity")
  status: TerminalStatus = Field(description="terminal EOS command outcome")
  sequence: int = Field(ge=0, description="terminal acknowledgement sequence")
  result: dict[str, Any] = Field(
    description="semantic terminal payload returned by the primitive"
  )
  reason: str | None = Field(default=None, description="backend reason on refusal")
  reason_code: RejectReason | None = Field(
    default=None, description="machine-readable backend rejection reason"
  )
  correlation_id: str | None = None


class PrimitiveInvokeError(BaseModel):
  """Rejected call — unknown primitive, gate refusal, or egress failure."""

  ok: Literal[False] = False
  error: str
  reason: str | None = None
  correlation_id: str | None = None


__all__ = ["PrimitiveInvokeAck", "PrimitiveInvokeError", "PrimitiveInvokeRequest"]
