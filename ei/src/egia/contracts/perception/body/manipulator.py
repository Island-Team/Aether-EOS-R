"""Typed manipulator-device discovery payload."""

from typing import Literal, Self

from pydantic import BaseModel, ConfigDict, Field, model_validator


class ManipulatorConfiguration(BaseModel):
  """Installed manipulator joints, tool, and mounting frame."""

  model_config = ConfigDict(extra="forbid")
  joint_names: list[str] = Field(min_length=1)
  joint_count: int = Field(ge=1)
  end_effector_type: str | None
  mount_frame: str = Field(min_length=1)

  @model_validator(mode="after")
  def _joint_count_matches_names(self) -> Self:
    if self.joint_count != len(self.joint_names):
      raise ValueError("joint_count must equal the number of joint_names")
    if len(set(self.joint_names)) != len(self.joint_names):
      raise ValueError("joint_names must be unique")
    return self


class ManipulatorOperationalState(BaseModel):
  """Low-rate manipulator controller state reported during discovery."""

  model_config = ConfigDict(extra="forbid")
  controller_state: Literal["disabled", "idle", "active", "fault"]
  calibrated: bool


class ManipulatorDevice(BaseModel):
  """Class-specific content of a discovered manipulator descriptor."""

  model_config = ConfigDict(extra="forbid")
  device_class: Literal["manipulator"] = "manipulator"
  configuration: ManipulatorConfiguration
  operational_state: ManipulatorOperationalState
