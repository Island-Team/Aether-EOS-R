"""Typed normalized human-pose snapshot."""

from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field

from egia.contracts.perception.world.snapshot import WorldSnapshot

Keypoint = tuple[float, float]
Keypoints = Annotated[list[Keypoint], Field(min_length=17, max_length=17)]
KeypointScores = Annotated[
  list[Annotated[float, Field(ge=0, le=1)]], Field(min_length=17, max_length=17)
]


class PosePerson(BaseModel):
  """One tracked or untracked person with COCO-17 keypoints."""

  model_config = ConfigDict(extra="forbid")
  track_id: int | None = Field(ge=1)
  bbox: tuple[float, float, float, float]
  confidence: float = Field(ge=0, le=1)
  distance_m: float | None = Field(ge=0)
  keypoints: Keypoints
  keypoint_scores: KeypointScores


class PoseSnapshot(WorldSnapshot):
  """One complete normalized human-pose result."""

  type: Literal["pose.snapshot"] = "pose.snapshot"
  frame_id: str = Field(min_length=1)
  model_id: str = Field(min_length=1)
  persons: list[PosePerson]
