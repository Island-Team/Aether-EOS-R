"""Operational world-object state resource -- v1.

Unlike vision detections (sensor evidence) and the planar scene aggregate
(renderer geometry), this resource carries the backend's authoritative,
replaceable state for mission-addressable world objects.  The vocabulary is
deliberately generic: deployments choose object kinds, state labels, relation
kinds, slot kinds, and JSON properties without changing the wire contract.
"""

from __future__ import annotations

from typing import Literal, Self

from pydantic import BaseModel, Field, JsonValue, model_validator

from egia.contracts.perception.envelope import SnapshotEnvelope

OBJECTS_SNAPSHOT_TYPE = "objects.snapshot"

__all__ = [
  "OBJECTS_SNAPSHOT_TYPE",
  "ObjectLocation",
  "ObjectRelation",
  "ObjectSlot",
  "ObjectStateSnapshot",
  "StockLevel",
  "WorldObjectState",
]


class ObjectLocation(BaseModel):
  """A named place and optional slot within that place."""

  place_id: str = Field(min_length=1, max_length=256)
  slot_id: str | None = Field(default=None, min_length=1, max_length=256)


class ObjectRelation(BaseModel):
  """One directed relation from the containing object to another entity."""

  kind: str = Field(min_length=1, max_length=128)
  target_id: str = Field(min_length=1, max_length=256)
  slot_id: str | None = Field(default=None, min_length=1, max_length=256)


class ObjectSlot(BaseModel):
  """A child-facing slot, its accepted kinds, occupants, and metadata."""

  slot_id: str = Field(min_length=1, max_length=256)
  accepts: list[str] = Field(default_factory=list)
  occupants: list[str] = Field(default_factory=list)
  properties: dict[str, JsonValue] = Field(default_factory=dict)

  @model_validator(mode="after")
  def _unique_values(self) -> Self:
    if len(set(self.accepts)) != len(self.accepts):
      raise ValueError("slot accepts must be unique")
    if len(set(self.occupants)) != len(self.occupants):
      raise ValueError("slot occupants must be unique")
    return self


class WorldObjectState(BaseModel):
  """Current operational state for one backend-owned world object."""

  object_id: str = Field(min_length=1, max_length=256)
  kind: str = Field(min_length=1, max_length=128)
  location: ObjectLocation | None = None
  states: list[str] = Field(default_factory=list)
  relations: list[ObjectRelation] = Field(default_factory=list)
  slots: list[ObjectSlot] = Field(default_factory=list)
  properties: dict[str, JsonValue] = Field(default_factory=dict)

  @model_validator(mode="after")
  def _unique_members(self) -> Self:
    if len(set(self.states)) != len(self.states):
      raise ValueError("object states must be unique")
    slot_ids = [slot.slot_id for slot in self.slots]
    if len(set(slot_ids)) != len(slot_ids):
      raise ValueError("object slot ids must be unique")
    relations = [
      (relation.kind, relation.target_id, relation.slot_id)
      for relation in self.relations
    ]
    if len(set(relations)) != len(relations):
      raise ValueError("object relations must be unique")
    return self


class StockLevel(BaseModel):
  """Aggregate uninstantiated stock available from one inventory."""

  inventory_id: str = Field(min_length=1, max_length=256)
  kind: str = Field(min_length=1, max_length=128)
  variant: str | None = Field(default=None, min_length=1, max_length=256)
  available: int = Field(ge=0)
  properties: dict[str, JsonValue] = Field(default_factory=dict)


class ObjectStateSnapshot(SnapshotEnvelope):
  """One complete, replaceable operational object-state snapshot."""

  type: Literal["objects.snapshot"] = OBJECTS_SNAPSHOT_TYPE
  frame_id: str = "world"
  objects: list[WorldObjectState] = Field(default_factory=list)
  stock: list[StockLevel] = Field(default_factory=list)

  @model_validator(mode="after")
  def _unique_records(self) -> Self:
    object_ids = [item.object_id for item in self.objects]
    if len(set(object_ids)) != len(object_ids):
      raise ValueError("snapshot object ids must be unique")
    stock_keys = [(item.inventory_id, item.kind, item.variant) for item in self.stock]
    if len(set(stock_keys)) != len(stock_keys):
      raise ValueError("snapshot stock records must be unique")
    return self
