"""Typed gripper-device discovery payload."""

from typing import Literal

from pydantic import BaseModel, ConfigDict, Field


class GripperConfiguration(BaseModel):
  """Installed gripper controls and supported actions."""

  model_config = ConfigDict(extra="forbid")
  supported_actions: list[Literal["open", "close"]] = Field(min_length=1)
  supports_force_control: bool
  supports_position_control: bool


class GripperOperationalState(BaseModel):
  """Low-rate jaw state reported during discovery."""

  model_config = ConfigDict(extra="forbid")
  jaw_state: Literal["unknown", "open", "closed", "moving", "fault"]


class GripperDevice(BaseModel):
  """Class-specific content of a discovered gripper descriptor."""

  model_config = ConfigDict(extra="forbid")
  device_class: Literal["gripper"] = "gripper"
  configuration: GripperConfiguration
  operational_state: GripperOperationalState
