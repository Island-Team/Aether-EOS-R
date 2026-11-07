"""Typed speaker-device discovery payload."""

from typing import Literal

from pydantic import BaseModel, ConfigDict, Field


class SpeakerConfiguration(BaseModel):
  """Installed speaker formats and channel configuration."""

  model_config = ConfigDict(extra="forbid")
  supported_media_types: list[str] = Field(min_length=1)
  sample_rates_hz: list[int] = Field(min_length=1)
  channels: int = Field(ge=1)


class SpeakerOperationalState(BaseModel):
  """Low-rate speaker playback state reported during discovery."""

  model_config = ConfigDict(extra="forbid")
  present: bool
  playback_state: Literal["idle", "buffering", "playing", "draining", "fault"]
  buffered_duration_ms: int = Field(ge=0)


class SpeakerDevice(BaseModel):
  """Class-specific content of a discovered speaker descriptor."""

  model_config = ConfigDict(extra="forbid")
  device_class: Literal["speaker"] = "speaker"
  configuration: SpeakerConfiguration
  operational_state: SpeakerOperationalState
