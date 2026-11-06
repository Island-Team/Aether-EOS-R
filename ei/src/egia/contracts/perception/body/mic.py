"""Typed microphone-device discovery payload."""

from typing import Literal

from pydantic import BaseModel, ConfigDict, Field


class MicrophoneConfiguration(BaseModel):
  """Installed microphone sampling configuration."""

  model_config = ConfigDict(extra="forbid")
  sample_rate_hz: int = Field(ge=1)
  channels: int = Field(ge=1)
  sample_format: str = Field(min_length=1)
  window_duration_ms: int = Field(ge=1)


class MicrophoneOperationalState(BaseModel):
  """Low-rate microphone availability reported during discovery."""

  model_config = ConfigDict(extra="forbid")
  present: bool
  capturing: bool


class MicrophoneDevice(BaseModel):
  """Class-specific content of a discovered microphone descriptor."""

  model_config = ConfigDict(extra="forbid")
  device_class: Literal["microphone"] = "microphone"
  configuration: MicrophoneConfiguration
  operational_state: MicrophoneOperationalState
