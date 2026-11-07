"""Typed semantic detection snapshot from EOS."""

from __future__ import annotations

from datetime import datetime
from typing import Literal, Self

from pydantic import BaseModel, ConfigDict, Field, model_validator


class Detection(BaseModel):
  """One typed human or object observation."""

  model_config = ConfigDict(extra="forbid")
  id: str = Field(pattern=r"^(?:human|obj):[^\s:]+$")
  kind: Literal["human", "object"]
  label: str = Field(min_length=1, max_length=128)
  confidence: float = Field(ge=0, le=1)
  distance_m: float | None = Field(default=None, ge=0)
  bearing_rad: float | None = None
  attributes: dict[str, str | float | bool | None] | None = None

  @model_validator(mode="after")
  def _typed_identity(self) -> Self:
    prefix = "human:" if self.kind == "human" else "obj:"
    if not self.id.startswith(prefix):
      raise ValueError(f"{self.kind} detection id must start with {prefix!r}")
    if self.attributes is not None and len(self.attributes) > 16:
      raise ValueError("detection attributes may contain at most 16 values")
    return self


class DetectionSnapshot(BaseModel):
  """One complete semantic model observation."""

  model_config = ConfigDict(extra="forbid")
  type: Literal["detection.snapshot"] = "detection.snapshot"
  schema_version: Literal["1.0"] = "1.0"
  sequence: int = Field(ge=0)
  timestamp: datetime
  source: str = Field(min_length=1)
  source_sequence: int | None = Field(default=None, ge=1)
  detections: list[Detection]
