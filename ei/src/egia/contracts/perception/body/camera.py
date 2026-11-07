"""Typed camera-device discovery payload."""

from typing import Literal

from pydantic import BaseModel, ConfigDict, Field


class CameraConfiguration(BaseModel):
  """Installed camera format and optical configuration."""

  model_config = ConfigDict(extra="forbid")
  width_px: int = Field(ge=1)
  height_px: int = Field(ge=1)
  frame_rate_hz: float = Field(gt=0)
  pixel_format: str = Field(min_length=1)
  fov_rad: float | None = Field(gt=0, le=6.283185307179586)


class CameraOperationalState(BaseModel):
  """Low-rate camera availability reported during discovery."""

  model_config = ConfigDict(extra="forbid")
  present: bool
  capturing: bool


class CameraDevice(BaseModel):
  """Class-specific content of a discovered camera descriptor."""

  model_config = ConfigDict(extra="forbid")
  device_class: Literal["camera"] = "camera"
  configuration: CameraConfiguration
  operational_state: CameraOperationalState
