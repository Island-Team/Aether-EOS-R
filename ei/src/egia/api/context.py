"""Context snapshot wire models — assembled prompt-ready state for the decision layer."""

from __future__ import annotations

from datetime import datetime
from typing import Literal

from pydantic import BaseModel, Field

from egia.api.prompts import PromptDocument


class ContextLine(BaseModel):
  """One atomic context fact; one line carries one perception item."""

  text: str = Field(min_length=1)
  kind: str = Field(description="source kind, e.g. vision, sound, or environment")
  source: str | None = None
  timestamp: datetime | None = None
  observation_id: str | None = None


class ContextGroup(BaseModel):
  """Line-oriented context group for UI lists and agent parsers."""

  lines: list[ContextLine] = Field(default_factory=list)


class InnerPerception(BaseModel):
  parts: ContextGroup = Field(default_factory=ContextGroup)
  robot_status: ContextGroup = Field(default_factory=ContextGroup)


class OuterPerception(BaseModel):
  vision: ContextGroup = Field(default_factory=ContextGroup)
  objects: ContextGroup = Field(
    default_factory=ContextGroup,
    description="semantic world-object state and inventory manifests",
  )
  environment: ContextGroup = Field(default_factory=ContextGroup)
  sound: ContextGroup = Field(default_factory=ContextGroup)


class PerceptionContext(BaseModel):
  inner: InnerPerception = Field(default_factory=InnerPerception)
  outer: OuterPerception = Field(default_factory=OuterPerception)


class MemoryContext(BaseModel):
  missions: ContextGroup = Field(default_factory=ContextGroup)


class UserContext(BaseModel):
  """Structured user message context grouped by what the model observes."""

  perception: PerceptionContext = Field(default_factory=PerceptionContext)
  memory: MemoryContext = Field(default_factory=MemoryContext)
  status: ContextGroup = Field(default_factory=ContextGroup)
  primitives: ContextGroup = Field(default_factory=ContextGroup)
  skills: ContextGroup = Field(default_factory=ContextGroup)


class ContextSection(BaseModel):
  """One rendered prompt section derived from observations."""

  kind: str
  rendered: str
  has_data: bool
  audience: Literal["system", "user"] = "user"
  path: str | None = Field(
    default=None,
    description="structured destination, e.g. user.perception.outer.vision",
  )
  lines: list[ContextLine] = Field(
    default_factory=list,
    description="atomic lines represented by this rendered section",
  )
  image_path: str | None = Field(
    default=None,
    description="optional filesystem path to a raster preview for VLM consumers",
  )
  omitted: bool = Field(
    default=False,
    description=(
      "true when token-budget truncation replaced this section's data "
      "with the configured omitted fallback line"
    ),
  )
  priority: int = Field(default=0, description="model-message ordering priority")
  change_frequency: float = Field(
    default=0.0,
    ge=0,
    le=1,
    description="observed content changes per build, used for cache-friendly ordering",
  )


class ContextSnapshot(BaseModel):
  """Immutable prompt bundle built from the latest observations per section."""

  version: str = Field(description="resolved prompt template version")
  prompt: str = Field(description="full assembled prompt text")
  system: PromptDocument | None = Field(
    default=None,
    description="structured system prompt; null when context is built without one",
  )
  user: UserContext = Field(
    default_factory=UserContext,
    description="structured runtime context for the user message",
  )
  sections: list[ContextSection]
  built_at: datetime = Field(description="snapshot assembly time, UTC")
  observation_ids: list[str] = Field(
    default_factory=list,
    description="observation IDs that contributed data to this snapshot",
  )
