"""Declarations shared across the main chain: decision → safety → link.

``ActionInvocation`` is what the planner emits per step; ``GateVerdict`` is
what the safety gate answers. They live in the interface layer so neither
``core/decision`` nor ``core/safety`` imports the other for data. Pure data —
no I/O, no state.
"""

from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, Field

from egia.contracts.actions import PrimitiveAck

__all__ = ["ActionInvocation", "GateVerdict", "StepContext"]


class ActionInvocation(BaseModel):
  """One planned primitive plus raw arguments.

  ``action`` is the primitive name (e.g. ``base.velocity``); ``arguments`` is
  the unvalidated body — contract validation belongs to the safety gate and
  the backend, not to the planner. ``intent`` and ``preconditions`` are the
  optional §3.3 plan metadata the model may attach to an action object: why
  the action exists and what it assumes. Both default empty, so callers that
  construct invocations without them (and the gate) are unchanged.
  """

  action: str = Field(min_length=1)
  action_kind: Literal["primitive", "skill"] = "primitive"
  version: str | None = Field(default=None, min_length=1)
  request_id: str | None = Field(default=None, min_length=1)
  robot_id: str | None = Field(
    default=None,
    min_length=1,
    description="explicit robot binding; never inferred from the action name",
  )
  device_id: str | None = Field(
    default=None,
    description="explicit EOS device target",
  )
  arguments: dict = Field(default_factory=dict)
  intent: str = ""
  preconditions: list[str] = Field(default_factory=list)


class StepContext(BaseModel):
  """Where one invocation sits inside its mission — the gate's call framing.

  ``origin`` is who typed the call: ``"planner"`` (inside a mission, gated
  against the mission table) or ``"operator"`` (typed by a human — origin
  trust, no mission context required; ``mission_id`` stays empty).
  ``step_index`` is 1-based within the mission. Optional on ``execute``:
  context-less calls are treated as operator-trust admissions.
  """

  mission_id: str = ""
  step_index: int = Field(default=1, ge=1)
  origin: Literal["planner", "operator"] = "planner"


class GateVerdict(BaseModel):
  """Safety gate's answer to one invocation.

  ``admitted=False`` means the gate refused egress (no backend round-trip
  happened) and ``ack`` is null. ``admitted=True`` carries the backend's
  ``PrimitiveAck``, whose own ``accepted`` field reports backend-side admission —
  the two levels are distinct: the gate rules on policy, the backend on
  execution.
  """

  admitted: bool
  reason: str | None = None
  ack: PrimitiveAck | None = None
