#!/usr/bin/env python3
"""Public EOS interface catalog and legacy wire-compatibility checks."""

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from wire_surface import wire_surface


ROOT = Path(__file__).resolve().parents[2]
PROTO_ROOT = ROOT / "share/contracts"
GENERATED = ROOT / "share/ext/eos"
TOOL = ROOT / "scripts/print_contract_interfaces.py"
GOLDEN = Path(__file__).parent / "fixtures/public_wire_surface.json"

for name in ("gateway", "skills", "workflow"):
  assert (PROTO_ROOT / f"eos/{name}.proto").is_file()
assert {path.name for path in (PROTO_ROOT / "eos").glob("*.proto")} == {
  "gateway.proto", "skills.proto", "workflow.proto"
}
assert {path.name for path in (PROTO_ROOT / "eos").iterdir() if path.is_dir()} == {
  "actions", "body", "world", "system", "robot", "perception", "skills",
  "workflow", "options", "external", "southbound"
}
assert not (PROTO_ROOT / "eos/northbound").exists()
assert not any((PROTO_ROOT / f"island/{name}.proto").exists()
               for name in ("gateway", "skills", "workflow"))

# This baseline was compiled once from the original master service protos.
# Test execution needs only the checked-in fixture and current proto source.
original = json.loads(GOLDEN.read_text(encoding="utf-8"))
current = wire_surface(PROTO_ROOT)
for kind, declarations in original.items():
  assert {name: current[kind].get(name) for name in declarations} == declarations, kind

manifest = json.loads((GENERATED / "contracts.manifest.json").read_text(encoding="utf-8"))
assert manifest["schema_count"] == 32
assert {entry["path"].split("/")[0] for entry in manifest["schemas"]} == {
  "actions", "body", "world"
}
schemas = {}
for entry in manifest["schemas"]:
  schema = json.loads((GENERATED / entry["path"]).read_text(encoding="utf-8"))
  assert schema["$id"] == entry["id"]
  schemas[entry["path"]] = schema
assert len(schemas) == 32
assert all(path.endswith(".v1.json") for path in schemas)
assert all(entry.get("version", "1.0.0") == "1.0.0" for entry in manifest["schemas"])
assert all(schema["$id"].startswith("egia.contracts.") for schema in schemas.values())
assert "opening_pct" in schemas["actions/gripper_motion.v1.json"]["properties"]
assert "speed_pct" in schemas["actions/gripper_motion.v1.json"]["properties"]
for resource in ("asr", "vision"):
  assert schemas[f"world/{resource}.v1.json"]["properties"]["schema_version"] == {
    "const": "1.0"
  }
assert not (ROOT / "share/contracts/perception/world/world.v1.json").exists()


def run(*args: str) -> subprocess.CompletedProcess[str]:
  return subprocess.run([sys.executable, str(TOOL), *args],
                        capture_output=True, text=True, check=False)


explicit = run("--contracts", str(ROOT / "share"), "--compact")
assert explicit.returncode == 0, explicit.stderr
repeat = run("--contracts", str(ROOT / "share"), "--compact")
assert repeat.stdout == explicit.stdout
from_generated = run("--contracts", str(GENERATED), "--compact")
assert from_generated.returncode == 0, from_generated.stderr
assert json.loads(from_generated.stdout) == json.loads(explicit.stdout)
default = run("--compact")
assert default.returncode == 0, default.stderr
assert json.loads(default.stdout) == json.loads(explicit.stdout)
payload = json.loads(explicit.stdout)
assert payload["schemaVersion"] == "5.0"
assert [service["fullName"] for service in payload["services"]] == [
  "island.eos.v1.GatewayService",
  "island.eos.v1.SkillService",
  "island.eos.v1.WorkflowService",
]
assert {key: len(payload["contracts"][key]) for key in
        ("primitives", "bodyDevices", "resources")} == {
  "primitives": 15, "bodyDevices": 7, "resources": 9
}
assert payload["contracts"]["acknowledgement"]["contract"] == "actions/ack.v1.json"
assert all(item["contract"].startswith("actions/")
           for item in payload["contracts"]["primitives"])
assert all(item["contract"].startswith("body/")
           for item in payload["contracts"]["bodyDevices"])
assert all(item["contract"].startswith("world/")
           for item in payload["contracts"]["resources"])

services = {service["fullName"].split(".")[-1]: service for service in payload["services"]}
expected_methods = {
  "GatewayService": [
    "Discover", "GetStatus", "GetModelStatus", "WatchModelStatus", "ListResources",
    "ReadResource", "ListNavigationPoints", "GetNavigationPoint", "PutNavigationPoint",
    "DeleteNavigationPoint", "ReloadNavigationPoints", "QueryResourceHistory",
    "WatchResource", "CallPrimitive", "CancelPrimitive", "CaptureFrames",
    "ObserveObjects", "InferVad", "DetectHumans", "EstimateHumanPose", "DetectFaces",
    "GetPerceptionRoles", "WatchMonitor",
  ],
  "SkillService": [
    "ListSkills", "GetSkill", "StartExecution", "GetExecution", "WatchExecution",
    "GetExecutionState", "WatchExecutionState", "TerminateExecution",
    "TerminateExecutions", "ListExecutions", "SignalExecution",
    "FollowExecutionJournal", "MonitorExecutions",
  ],
  "WorkflowService": [
    "ListWorkflowTemplates", "GetWorkflowTemplate", "RunWorkflowTemplate",
    "ApplyActionList", "GetActionList", "ListActionLists", "GetRuntimeSnapshot",
    "WatchActionListEvents",
  ],
}
for service_name, names in expected_methods.items():
  assert [method["name"] for method in services[service_name]["methods"]] == names

methods = {method["name"]: method for service in payload["services"]
           for method in service["methods"]}
assert len(methods) == sum(map(len, expected_methods.values()))
assert methods["Discover"]["requestType"] == "Empty"
assert methods["Discover"]["responseType"] == "DiscoverResponse"
assert methods["Discover"]["http"] == {"verb": "GET", "path": "/v1/eos/discover"}
assert methods["PutNavigationPoint"]["http"] == {
  "verb": "PUT", "path": "/v1/eos/robots/{robot_name}/navigation/waypoints/{name}", "body": "*"
}
assert methods["ObserveObjects"]["http"] == {
  "verb": "POST", "path": "/v1/eos/perception/objects:observe", "body": "*"
}
assert methods["StartExecution"]["http"] == {
  "verb": "POST", "path": "/v1/eos/skills/{reference.name}/versions/{reference.version}/executions",
  "body": "*"
}
assert methods["RunWorkflowTemplate"]["http"] == {
  "verb": "POST", "path": "/v1/eos/workflow-templates/{name}:run", "body": "*"
}
assert methods["WatchMonitor"]["http"] is None
assert methods["WatchActionListEvents"]["http"] is None
assert {name for name, method in methods.items() if method["serverStreaming"]} == {
  "WatchModelStatus", "WatchResource", "WatchMonitor", "WatchExecution",
  "WatchExecutionState", "FollowExecutionJournal", "MonitorExecutions",
  "WatchActionListEvents",
}

# The catalog uses descriptor-derived references. Every public RPC message
# and every type reference must resolve inside the emitted type table.
types = payload["types"]
for method in methods.values():
  for key in ("requestSchema", "responseSchema"):
    pending = [method[key]]
    while pending:
      value = pending.pop()
      if isinstance(value, dict):
        reference = value.get("$ref")
        if reference:
          assert reference.startswith("#/components/schemas/")
          assert reference.rsplit("/", 1)[1] in types, reference
        pending.extend(value.values())
      elif isinstance(value, list):
        pending.extend(value)
for type_schema in types.values():
  pending = [type_schema]
  while pending:
    value = pending.pop()
    if isinstance(value, dict):
      reference = value.get("$ref")
      if reference:
        assert reference.rsplit("/", 1)[1] in types, reference
      pending.extend(value.values())
    elif isinstance(value, list):
      pending.extend(value)

assert types["island_eos_v1_CaptureFramesResponse"]["properties"]["schema_version"] == {
  "type": "string"
}
assert types["island_eos_v1_ObserveObjectsRequest"]["properties"]["sourceTimestampNs"] == {
  "type": "string", "pattern": "^-?[0-9]+$"
}
assert types["island_eos_v1_WorkflowAction"]["properties"]["skill"] == {
  "$ref": "#/components/schemas/island_eos_v1_SkillReference"
}

with tempfile.TemporaryDirectory() as directory:
  empty = run("--contracts", directory)
  assert empty.returncode != 0
  assert "no generated EOS contracts" in empty.stderr
  missing_environment = dict(os.environ)
  missing_environment.pop("EOS_CONTRACTS", None)
  missing_environment.pop("ISLAND_SHARE", None)
  missing = subprocess.run(
    [sys.executable, str(TOOL), "--contracts", "/definitely/missing/eos/contracts"],
    capture_output=True, text=True, env=missing_environment,
  )
  assert missing.returncode != 0
  assert "cannot resolve contracts root" in missing.stderr
