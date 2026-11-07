"""Typed fused external-world snapshot from EOS."""

from __future__ import annotations

from datetime import datetime
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field


class WorldEntity(BaseModel):
  """One typed physical entity in the fused world state."""

  model_config = ConfigDict(extra="forbid")
  id: str = Field(pattern=r"^(?:human|obj):[^\s:]+$")
  label: str = Field(min_length=1, max_length=128)
  position_m: tuple[float, float, float]
  velocity_m_s: tuple[float, float, float] | None = None
  state: str | None = Field(default=None, min_length=1, max_length=64)


class WorldRelation(BaseModel):
  """One directed semantic relation between two world entities."""

  model_config = ConfigDict(extra="forbid")
  subject: str = Field(pattern=r"^(?:human|obj):[^\s:]+$")
  relation: str = Field(min_length=1, max_length=64)
  object: str = Field(pattern=r"^(?:human|obj):[^\s:]+$")


class WorldStateSnapshot(BaseModel):
  """One complete fused external-world state."""

  model_config = ConfigDict(extra="forbid")
  type: Literal["world.snapshot"] = "world.snapshot"
  schema_version: Literal["1.0"] = "1.0"
  sequence: int = Field(ge=0)
  timestamp: datetime
  frame_id: str = Field(min_length=1)
  entities: list[WorldEntity]
  relations: list[WorldRelation]
