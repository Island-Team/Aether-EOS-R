"""Northbound wire models for discovered EOS primitives."""

from __future__ import annotations

from typing import Any

from pydantic import BaseModel, ConfigDict, Field

from egia.contracts.limits import NumericRange


class PrimitiveDependency(BaseModel):
  """One health-bearing dependency advertised for a primitive binding."""

  model_config = ConfigDict(extra="forbid")

  name: str
  health: str


class PrimitiveDescriptor(BaseModel):
  """One exact EOS primitive binding projected to an operator client."""

  model_config = ConfigDict(extra="forbid")

  name: str
  description: str
  device_description: str
  version: str = Field(pattern=r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
  robot_id: str
  device_id: str | None = None
  eos_robot_id: str
  operator: bool = False
  scope: str
  binding_kind: str
  health: str
  references: list[str] = Field(default_factory=list)
  absolute_frame: str | None = None
  dependencies: list[PrimitiveDependency] = Field(default_factory=list)
  accepted_media_types: list[str] = Field(default_factory=list)
  limits: dict[str, NumericRange] = Field(default_factory=dict)
  arguments_schema: dict[str, Any] = Field(default_factory=dict)


class PrimitivesSnapshot(BaseModel):
  primitives: list[PrimitiveDescriptor] = Field(default_factory=list)
  count: int = 0


__all__ = ["PrimitiveDependency", "PrimitiveDescriptor", "PrimitivesSnapshot"]
