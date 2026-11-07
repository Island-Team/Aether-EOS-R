#!/usr/bin/env python3
"""Regenerate EGIA-owned northbound JSON Schemas from their wire models."""

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

ROOT = Path(__file__).resolve().parents[1]
DRAFT = "https://json-schema.org/draft/2020-12/schema"
CASES = {
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


def main() -> None:
  target = ROOT / "src/egia/contracts/schemas/northbound"
  target.mkdir(parents=True, exist_ok=True)
  obsolete = target / "skills_snapshot.v1.json"
  obsolete.unlink(missing_ok=True)
  for name, (schema_id, model) in CASES.items():
    schema = {"$schema": DRAFT, "$id": schema_id, **model.model_json_schema()}
    (target / name).write_text(
      json.dumps(schema, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
  main()
