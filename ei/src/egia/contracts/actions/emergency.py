"""EOS privileged primitive contracts."""

from __future__ import annotations

from typing import Literal

from egia.contracts.actions.commands import PrimitiveCommand


class EstopEngageCommand(PrimitiveCommand):
  """Engage the robot-wide emergency stop."""

  schema_path = "eos/schemas/eos/actions/safety_estop.v1.json"
  type: Literal["safety.estop"] = "safety.estop"


class MotionStopCommand(PrimitiveCommand):
  """Bring one actuator's current motion to a controlled stop."""

  schema_path = "eos/schemas/eos/actions/motion_stop.v1.json"
  type: Literal["motion.stop"] = "motion.stop"


class RobotStopCommand(PrimitiveCommand):
  """Bring all active robot parts to a controlled stop."""

  schema_path = "eos/schemas/eos/actions/robot_stop.v1.json"
  type: Literal["robot.stop"] = "robot.stop"
