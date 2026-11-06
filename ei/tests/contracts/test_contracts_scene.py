"""World and detection contract validation against the canonical EOS schemas."""

from __future__ import annotations

import pytest

from egia.contracts.endpoints import ResourceKind
from egia.contracts.perception.world.detection import DetectionSnapshot
from egia.contracts.perception.world.state import WorldStateSnapshot
from egia.contracts.validate import load_resource_schema, validate_resource


def test_world_and_detection_contracts_select_canonical_schema_ids() -> None:
  assert load_resource_schema(ResourceKind.WORLD)["$id"].endswith("world.v1")
  assert load_resource_schema(ResourceKind.DETECTION)["$id"].endswith("detection.v1")


def test_world_round_trip_preserves_typed_entities() -> None:
  parsed = validate_resource(
    ResourceKind.WORLD,
    {
      "type": "world.snapshot",
      "schema_version": "1.0",
      "sequence": 7,
      "timestamp": "2026-09-20T09:30:00Z",
      "frame_id": "map",
      "entities": [{"id": "obj:part-7", "label": "bearing", "position_m": [1, 2, 0.1]}],
      "relations": [],
    },
  )
  assert isinstance(parsed, WorldStateSnapshot)
  assert parsed.entities[0].id == "obj:part-7"


def test_detection_rejects_identity_kind_mismatch() -> None:
  with pytest.raises(ValueError, match="human detection id"):
    DetectionSnapshot.model_validate(
      {
        "type": "detection.snapshot",
        "schema_version": "1.0",
        "sequence": 1,
        "timestamp": "2026-09-20T09:30:00Z",
        "source": "camera-0",
        "detections": [
          {
            "id": "obj:7",
            "kind": "human",
            "label": "person",
            "confidence": 0.9,
          }
        ],
      }
    )
