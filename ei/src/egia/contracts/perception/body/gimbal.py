"""Typed gimbal-device discovery payload."""

from typing import Literal, Self

from pydantic import BaseModel, ConfigDict, Field, model_validator


class GimbalConfiguration(BaseModel):
  """Installed gimbal travel and optical limits."""

  model_config = ConfigDict(extra="forbid")
  pan_min_rad: float
  pan_max_rad: float
  tilt_min_rad: float
  tilt_max_rad: float
  fov_rad: float = Field(gt=0, le=6.283185307179586)
  range_m: float | None = Field(gt=0)

  @model_validator(mode="after")
  def _ordered_ranges(self) -> Self:
    if self.pan_min_rad > self.pan_max_rad or self.tilt_min_rad > self.tilt_max_rad:
      raise ValueError("gimbal minimum angle must not exceed maximum angle")
    return self


class GimbalOperationalState(BaseModel):
  """Low-rate gimbal controller state reported during discovery."""

  model_config = ConfigDict(extra="forbid")
  enabled: bool
  controller_state: Literal["disabled", "idle", "active", "fault"]


class GimbalDevice(BaseModel):
  """Class-specific content of a discovered gimbal descriptor."""

  model_config = ConfigDict(extra="forbid")
  device_class: Literal["gimbal"] = "gimbal"
  configuration: GimbalConfiguration
  operational_state: GimbalOperationalState
