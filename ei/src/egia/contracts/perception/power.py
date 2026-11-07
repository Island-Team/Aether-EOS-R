"""Power / battery contract — composable struct only (v1).

Owns the power subsystem as a high-level state of charge and source: the agent
reads ``charge_pct``, ``is_charging``, and ``remaining_s`` to decide whether it
can move or must dock — it does not read cell voltages or pack balancing. Raw
battery-management data is semanticalized by the southbound adapter.

There is **no standalone power resource**: ``PowerState`` is served only as
the ``power`` field of the general status snapshot
(:class:`egia.contracts.perception.body.status.RobotStatus`), so the platform has one
general-status frame instead of a second power stream.

``remaining_s`` is ``None`` when the adapter cannot estimate it (e.g. charging
or on external power); ``health_pct`` is the pack's state of health relative
to new capacity, for long-horizon planning.
"""

from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, Field

SCHEMA_VERSION = "1.0"

__all__ = ["PowerState"]


class PowerState(BaseModel):
  """Semantic battery / power state after adapter-level semanticalization."""

  charge_pct: float = Field(ge=0, le=100)
  health_pct: float = Field(ge=0, le=100)
  voltage_v: float = Field(gt=0)
  current_a: float = Field(description="pack current; negative when charging")
  is_charging: bool = False
  power_source: Literal["battery", "docked", "external"] = "battery"
  remaining_s: float | None = Field(
    default=None, description="estimated seconds of charge left at the current draw"
  )
