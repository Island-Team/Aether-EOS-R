"""Typed base-device discovery payload."""

from typing import Literal

from pydantic import BaseModel, ConfigDict, Field


class BaseConfiguration(BaseModel):
  """Installed mobile-base geometry and supported drive modes."""

  model_config = ConfigDict(extra="forbid")
  footprint_length_m: float = Field(gt=0)
  footprint_width_m: float = Field(gt=0)
  supported_modes: list[Literal["differential", "omnidirectional", "legged"]] = Field(
    min_length=1
  )


class BaseOperationalState(BaseModel):
  """Low-rate base controller state reported during discovery."""

  model_config = ConfigDict(extra="forbid")
  controller_state: Literal["disabled", "idle", "active", "fault"]
  motors_enabled: bool


class BaseDevice(BaseModel):
  """Class-specific content of a discovered base descriptor."""

  model_config = ConfigDict(extra="forbid")
  device_class: Literal["base"] = "base"
  configuration: BaseConfiguration
  operational_state: BaseOperationalState
