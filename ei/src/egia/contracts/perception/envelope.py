"""Shared snapshot envelope for polled resources.

Every replaceable state resource (part snapshots, the scene aggregate,
metadata) carries the same wire frame: a fixed ``type`` discriminator, a
contract ``schema_version``, a producer-side monotonic ``sequence``, a
``timestamp``, and the coordinate ``frame_id`` the payload is expressed in.
This module is that frame as a base model, so the fields are defined once and
each resource adds its own ``type`` literal and payload.

``part_id`` addresses one instance among several of the same part kind
(twin arms, front and rear cameras); it lives on the envelope because a
producer may serve multiple instances of one resource kind, and ``"default"``
keeps single-instance deployments unchanged.
"""

from __future__ import annotations

from datetime import datetime

from pydantic import BaseModel, Field

SCHEMA_VERSION = "1.0"


class SnapshotEnvelope(BaseModel):
  """Wire frame shared by every polled state resource."""

  schema_version: str = SCHEMA_VERSION
  sequence: int = Field(ge=0)
  timestamp: datetime
  frame_id: str = "map"
  part_id: str = Field(
    default="default",
    description="instance address for multi-part systems; 'default' when unique",
  )
  description: str | None = Field(
    default=None,
    description=(
      "human-facing hardware description so an agent can recognize the "
      "instance, e.g. 'left arm' or 'front camera'; null when unnamed"
    ),
  )
  tags: list[str] = Field(
    default_factory=list,
    description=(
      "grouping labels for selection, e.g. ['station1']; tags never target "
      "primitives — callers resolve them to concrete device ids first"
    ),
  )
