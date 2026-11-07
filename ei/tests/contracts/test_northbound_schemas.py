"""Generated JSON Schemas for northbound primitive and skill interfaces."""

from __future__ import annotations

import json
from pathlib import Path

from egia.api.invoke import (
  PrimitiveInvokeAck,
  PrimitiveInvokeError,
  PrimitiveInvokeRequest,
)
from egia.api.mission import (
  MissionCommandAck,
  MissionCommandError,
  MissionCommandRequest,
)
from egia.api.primitives import PrimitivesSnapshot
from egia.api.skills import (
  SkillCommandAck,
  SkillCommandError,
  SkillCommandRequest,
  SkillExecutionSnapshot,
  SkillsSnapshot,
  SkillWatchRequest,
)

SCHEMA_DRAFT = "https://json-schema.org/draft/2020-12/schema"
SCHEMA_ROOT = (
  Path(__file__).resolve().parents[2] / "src/egia/contracts/schemas/northbound"
)


def test_northbound_schemas_match_their_wire_models() -> None:
  cases = {
    "mission_command_request.v1.json": (
      "egia.contracts.northbound.mission_command_request.v1",
      MissionCommandRequest,
    ),
    "mission_command_ack.v1.json": (
      "egia.contracts.northbound.mission_command_ack.v1",
      MissionCommandAck,
    ),
    "mission_command_error.v1.json": (
      "egia.contracts.northbound.mission_command_error.v1",
      MissionCommandError,
    ),
    "primitive_invoke_request.v1.json": (
      "egia.contracts.northbound.primitive_invoke_request.v1",
      PrimitiveInvokeRequest,
    ),
    "primitive_invoke_ack.v1.json": (
      "egia.contracts.northbound.primitive_invoke_ack.v1",
      PrimitiveInvokeAck,
    ),
    "primitive_invoke_error.v1.json": (
      "egia.contracts.northbound.primitive_invoke_error.v1",
      PrimitiveInvokeError,
    ),
    "primitives_snapshot.v1.json": (
      "egia.contracts.northbound.primitives_snapshot.v1",
      PrimitivesSnapshot,
    ),
    "skills_snapshot.v2.json": (
      "egia.contracts.northbound.skills_snapshot.v2",
      SkillsSnapshot,
    ),
    "skill_command_request.v1.json": (
      "egia.contracts.northbound.skill_command_request.v1",
      SkillCommandRequest,
    ),
    "skill_command_ack.v1.json": (
      "egia.contracts.northbound.skill_command_ack.v1",
      SkillCommandAck,
    ),
    "skill_command_error.v1.json": (
      "egia.contracts.northbound.skill_command_error.v1",
      SkillCommandError,
    ),
    "skill_execution_snapshot.v1.json": (
      "egia.contracts.northbound.skill_execution_snapshot.v1",
      SkillExecutionSnapshot,
    ),
    "skill_watch_request.v1.json": (
      "egia.contracts.northbound.skill_watch_request.v1",
      SkillWatchRequest,
    ),
  }
  for name, (schema_id, model) in cases.items():
    actual = json.loads((SCHEMA_ROOT / name).read_text(encoding="utf-8"))
    expected = {"$schema": SCHEMA_DRAFT, "$id": schema_id, **model.model_json_schema()}
    assert actual == expected
