"""Typed normalized object-detection snapshot."""

from typing import Literal

from pydantic import BaseModel, ConfigDict, Field

from egia.contracts.perception.world.snapshot import WorldSnapshot


class VisionDetection(BaseModel):
  """One normalized object detection in source-image coordinates."""

  model_config = ConfigDict(extra="forbid")
  object_id: str | None
  label: str = Field(min_length=1)
  bbox: tuple[float, float, float, float] | None
  distance_m: float | None = Field(ge=0)
  bearing_rad: float | None
  confidence: float = Field(ge=0, le=1)
  pickable: bool | None
  unpickable_reason: str | None


class VisionSnapshot(WorldSnapshot):
  """One complete normalized object-detection result."""

  type: Literal["vision.snapshot"] = "vision.snapshot"
  frame_id: str = Field(min_length=1)
  model_id: str = Field(min_length=1)
  detections: list[VisionDetection]
