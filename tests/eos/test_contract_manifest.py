#!/usr/bin/env python3
"""Build-time contract manifest tests using isolated schema trees."""

import json
import subprocess
import sys
import tempfile
from pathlib import Path


repo = Path(__file__).resolve().parents[2]
tool = repo / "scripts/contract_manifest.py"


def run(*arguments: str) -> subprocess.CompletedProcess[str]:
  return subprocess.run(
      [sys.executable, str(tool), *arguments], capture_output=True, text=True, check=False
  )


with tempfile.TemporaryDirectory() as directory:
  root = Path(directory)
  actions = root / "actions"
  perception = root / "world"
  actions.mkdir(parents=True)
  perception.mkdir(parents=True)
  ack = {
      "$schema": "https://json-schema.org/draft/2020-12/schema",
      "$id": "test.ack.v1",
      "type": "object",
      "properties": {"type": {"const": "primitive.ack"}},
  }
  primitive = {
      "$schema": "https://json-schema.org/draft/2020-12/schema",
      "$id": "test.primitive.v1",
      "x-island-primitive-version": "1.0.0",
      "type": "object",
      "properties": {"type": {"const": "test.action"}},
  }
  resource = {
      "$schema": "https://json-schema.org/draft/2020-12/schema",
      "$id": "test.resource.v1",
      "$defs": {"State": {"type": "object"}},
      "properties": {
          "type": {"const": "test.snapshot"},
          "state": {"$ref": "test.resource.v1#/$defs/State"},
      },
  }
  (actions / "ack.v1.json").write_text(json.dumps(ack), encoding="utf-8")
  primitive_path = actions / "primitive.v1.json"
  primitive_path.write_text(json.dumps(primitive), encoding="utf-8")
  resource_path = perception / "resource.v1.json"
  resource_path.write_text(json.dumps(resource), encoding="utf-8")
  other_resource = dict(resource)
  other_resource["$id"] = "test.other-resource.v1"
  other_resource["properties"] = dict(resource["properties"])
  other_resource["properties"]["type"] = {"const": "test-other.snapshot"}
  other_resource_path = perception / "other-resource.v1.json"
  other_resource_path.write_text(json.dumps(other_resource), encoding="utf-8")
  output = root / "contracts.manifest.json"

  generated = run("--root", str(root), "--output", str(output))
  assert generated.returncode == 0, generated.stderr
  first = output.read_text(encoding="utf-8")
  manifest = json.loads(first)
  assert manifest["schema_count"] == 4
  assert manifest["format_version"] == 3
  assert [item["path"] for item in manifest["schemas"]] == [
      "actions/ack.v1.json",
      "actions/primitive.v1.json",
      "world/other-resource.v1.json",
      "world/resource.v1.json",
  ]
  assert all(str(root) not in item["path"] for item in manifest["schemas"])
  assert [item["role"] for item in manifest["schemas"]] == [
      "acknowledgement", "primitive", "world-resource", "world-resource"
  ]
  primitive_entry = manifest["schemas"][1]
  assert primitive_entry["name"] == "test.action"
  assert primitive_entry["version"] == "1.0.0"
  assert run("--root", str(root), "--output", str(output), "--check").returncode == 0

  duplicate = dict(primitive)
  duplicate["$id"] = "test.duplicate.v1"
  duplicate_path = actions / "duplicate.v1.json"
  duplicate_path.write_text(json.dumps(duplicate), encoding="utf-8")
  duplicate_identity = run("--root", str(root), "--output", str(output))
  assert duplicate_identity.returncode != 0
  assert "duplicate primitive identity" in duplicate_identity.stderr
  duplicate["properties"] = {"type": {"const": "test.other-action"}}
  duplicate_path.write_text(json.dumps(duplicate), encoding="utf-8")
  distinct_action = run("--root", str(root), "--output", str(output))
  assert distinct_action.returncode == 0, distinct_action.stderr
  duplicate_path.unlink()

  del primitive["x-island-primitive-version"]
  primitive_path.write_text(json.dumps(primitive), encoding="utf-8")
  missing_version = run("--root", str(root), "--output", str(output))
  assert missing_version.returncode != 0
  assert "semantic x-island-primitive-version" in missing_version.stderr
  primitive["x-island-primitive-version"] = "1.0"
  primitive_path.write_text(json.dumps(primitive), encoding="utf-8")
  malformed_version = run("--root", str(root), "--output", str(output))
  assert malformed_version.returncode != 0
  assert "semantic x-island-primitive-version" in malformed_version.stderr
  primitive["x-island-primitive-version"] = "1.0.0"
  primitive_path.write_text(json.dumps(primitive), encoding="utf-8")

  resource["description"] = "changed"
  resource_path.write_text(json.dumps(resource), encoding="utf-8")
  stale = run("--root", str(root), "--output", str(output), "--check")
  assert stale.returncode != 0
  assert "stale" in stale.stderr

  resource["properties"]["state"]["$ref"] = "test.missing.v1#/$defs/State"
  resource_path.write_text(json.dumps(resource), encoding="utf-8")
  broken = run("--root", str(root), "--output", str(output))
  assert broken.returncode != 0
  assert "unresolved external $ref" in broken.stderr

  resource["properties"]["state"]["$ref"] = "#/$defs/State"
  misplaced = root / "body/misplaced.v1.json"
  misplaced.parent.mkdir(parents=True, exist_ok=True)
  misplaced.write_text(json.dumps(resource), encoding="utf-8")
  invalid_role = run("--root", str(root), "--output", str(output))
  assert invalid_role.returncode != 0
  assert "body-device device_class" in invalid_role.stderr
