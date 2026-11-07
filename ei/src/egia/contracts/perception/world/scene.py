"""Typed fused world-scene snapshot."""

from __future__ import annotations

from typing import Literal, Self

from pydantic import BaseModel, ConfigDict, Field, model_validator

from egia.contracts.perception.pose import PlanarPose, PlanarVelocity
from egia.contracts.perception.world.snapshot import WorldSnapshot
from egia.contracts.source import load_json


class RobotKinematics(BaseModel):
  """Any currently observed subset of robot planar kinematics."""

  model_config = ConfigDict(extra="forbid")
  pose: PlanarPose | None = None
  velocity: PlanarVelocity | None = None
  posture: str | None = None


class EndEffectorState(BaseModel):
  """Any currently observed subset of one end effector's state."""

  model_config = ConfigDict(extra="forbid")
  frame_id: str | None = Field(default=None, min_length=1)
  position_m: tuple[float, float, float] | None = None
  orientation_quat_xyzw: tuple[float, float, float, float] | None = None
  jaw_state: str | None = None
  held_object_id: str | None = None


class SceneObject(BaseModel):
  """One spatial object fused into the scene."""

  model_config = ConfigDict(extra="forbid")
  id: str = Field(min_length=1)
  label: str = Field(min_length=1)
  position_m: tuple[float, float, float]
  orientation_quat_xyzw: tuple[float, float, float, float] | None = None
  radius_m: float | None = Field(default=None, gt=0)
  confidence: float | None = Field(default=None, ge=0, le=1)


class SceneSnapshot(WorldSnapshot):
  """One complete, replaceable fused world-scene snapshot."""

  type: Literal["scene.snapshot"] = "scene.snapshot"
  frame_id: str = Field(min_length=1)
  robot: RobotKinematics | None = None
  end_effectors: list[EndEffectorState] | None = None
  objects: list[SceneObject] | None = None

  @model_validator(mode="after")
  def _contains_observation(self) -> Self:
    if self.robot is None and self.end_effectors is None and self.objects is None:
      raise ValueError("scene requires robot, end_effectors, or objects")
    if self.objects is not None:
      ids = [item.id for item in self.objects]
      if len(ids) != len(set(ids)):
        raise ValueError("scene object ids must be unique")
    return self

  @classmethod
  def model_json_schema(cls, *args, **kwargs) -> dict:
    """Return EOS's canonical scene schema."""
    del args, kwargs
    return load_json("eos/schemas/eos/world/world.v1.json")


SceneState = SceneSnapshot

__all__ = [
  "EndEffectorState",
  "PlanarPose",
  "PlanarVelocity",
  "RobotKinematics",
  "SceneObject",
  "SceneSnapshot",
  "SceneState",
]
