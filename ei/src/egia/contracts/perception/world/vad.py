"""Typed normalized voice-activity snapshot."""

from typing import Literal, Self

from pydantic import BaseModel, ConfigDict, Field, model_validator

from egia.contracts.perception.world.snapshot import WorldSnapshot


class VadSegment(BaseModel):
  """One detected speech interval in seconds."""

  model_config = ConfigDict(extra="forbid")
  start_s: float = Field(ge=0)
  end_s: float = Field(gt=0)
  confidence: float | None = Field(ge=0, le=1)

  @model_validator(mode="after")
  def _positive_duration(self) -> Self:
    if self.end_s <= self.start_s:
      raise ValueError("VAD segment end_s must be greater than start_s")
    return self


class VadSnapshot(WorldSnapshot):
  """One complete normalized voice-activity result."""

  type: Literal["vad.snapshot"] = "vad.snapshot"
  model_id: str = Field(min_length=1)
  speech: bool
  probability: float | None = Field(ge=0, le=1)
  segments: list[VadSegment]

  @model_validator(mode="after")
  def _segments_match_speech(self) -> Self:
    if self.speech != bool(self.segments):
      raise ValueError("speech must be true exactly when VAD segments are present")
    if any(
      current.start_s < previous.end_s
      for previous, current in zip(self.segments, self.segments[1:])
    ):
      raise ValueError("VAD segments must be ordered and non-overlapping")
    return self
