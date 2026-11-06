"""Robot operational status contract — standalone part resource (v1).

Owns the system-level operational state the decision layer needs to gate
actions: autonomy/teleop mode, emergency stop, motor and autonomy enables,
active fault labels, and uptime. This is command-and-safety telemetry, not
kinematics — distinct from the base, gimbal, and manipulator part resources.

The resource served at ``GET /resources/{status_uri}`` is a ``RobotStatus``
snapshot. ``mode`` is the single high-level dispatch state; ``faults`` carries
zero or more active fault labels (empty when healthy) and ``estop`` is the
hard stop that must override every other field when engaged.
"""

from __future__ import annotations

from typing import Literal

from pydantic import Field

from egia.contracts.perception.envelope import SnapshotEnvelope
from egia.contracts.perception.power import PowerState

ROBOT_STATUS_TYPE = "robot.status"


class RobotStatus(SnapshotEnvelope):
  """One replaceable robot operational-status snapshot (standalone resource).

  The robot's **general status section**: system-level operational state plus
  the power subsystem, in one frame. Distinct from the part snapshots: this
  carries no spatial ``state`` struct because it describes the platform
  rather than a kinematic joint. ``estop`` engaged means motion commands must
  be rejected regardless of ``mode``; ``power`` is the composed
  :class:`PowerState` struct — there is no standalone power resource.
  """

  type: Literal["robot.status"] = ROBOT_STATUS_TYPE
  frame_id: str = "robot"
  mode: Literal["auto", "teleop", "idle", "charging", "error"] = "idle"
  estop: bool = False
  motors_enabled: bool = True
  autonomy_enabled: bool = False
  uptime_s: float = Field(ge=0)
  faults: list[str] = Field(default_factory=list)
  power: PowerState = Field(
    default_factory=lambda: PowerState(
      charge_pct=100.0, health_pct=100.0, voltage_v=14.8, current_a=0.0
    )
  )
