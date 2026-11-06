"""Grouped northbound perception projection."""

from __future__ import annotations

from datetime import datetime
from typing import Any

from pydantic import BaseModel, Field


class PerceptionResource(BaseModel):
  """One latest perception resource in the grouped stream."""

  kind: str = Field(description="resource kind, such as scene, vision, or status")
  part_id: str = "default"
  sequence: int = Field(ge=0)
  timestamp: datetime
  snapshot: dict[str, Any] | None = None


class PerceptionsSnapshot(BaseModel):
  """Replaceable aggregate of the latest world and body perceptions."""

  schema_version: str = "1"
  sequence: int = Field(ge=0)
  timestamp: datetime
  resources: list[PerceptionResource] = Field(default_factory=list)
