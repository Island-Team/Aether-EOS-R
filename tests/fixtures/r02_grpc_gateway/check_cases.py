#!/usr/bin/env python3
"""Check the frozen R02 design fixture and published method names."""

from __future__ import annotations

import json
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
GATEWAY = (ROOT / "share/eos/contracts/eos/gateway.proto").read_text()
CAPABILITIES = (ROOT / "share/eos/contracts/eos/capabilities.proto").read_text()
SKILLS = (ROOT / "share/eos/contracts/eos/skills.proto").read_text()
REQUIRED_GATEWAY = {
  "Discover", "ListResources", "GetStatus", "ReadResource", "WatchResource", "CaptureFrames"
}
REQUIRED_CAPABILITY = {
  "ListCapabilities", "SubmitOperation", "WatchOperation", "GetOperation", "CancelOperation"
}


def check() -> None:
  data = json.loads((HERE / "cases.json").read_text())
  assert data["schema_version"] == 1 and data["gate"] == "R02"
  assert data["status"] == "design_review_pending"
  assert data["wire_packages"] == ["island.eos.v1", "island.eos.v2"]
  cases = data["cases"]
  assert len(cases) == 35
  assert [case["id"] for case in cases] == [f"R02-{i:02d}" for i in range(1, 36)]
  assert all(set(case) == {"id", "method", "stimulus", "expect", "evidence"} for case in cases)
  assert all(all(isinstance(value, str) and value for value in case.values()) for case in cases)
  methods = {case["method"] for case in cases}
  assert REQUIRED_GATEWAY | REQUIRED_CAPABILITY | {"health", "ListSkills"} <= methods
  assert all(re.search(rf"\brpc\s+{name}\s*\(", GATEWAY) for name in REQUIRED_GATEWAY)
  assert all(re.search(rf"\brpc\s+{name}\s*\(", CAPABILITIES) for name in REQUIRED_CAPABILITY)
  assert re.search(r"\brpc\s+ListSkills\s*\(", SKILLS)
  by_id = {case["id"]: case for case in cases}
  assert "robot_name" in by_id["R02-05"]["expect"]
  assert "RESOURCE_EXHAUSTED" in by_id["R02-12"]["expect"]
  assert "surfaces to caller" in by_id["R02-12"]["expect"]
  for case_id, kind in (("R02-26", "MotionStop"), ("R02-27", "RobotStop"), ("R02-28", "SafetyEstop")):
    assert kind in by_id[case_id]["expect"]
    assert "journaled" in by_id[case_id]["expect"]
  assert "Robot{robot_id}" in by_id["R02-29"]["expect"]
  assert "no fake hardware path" in by_id["R02-29"]["expect"]
  assert "never calls either again" in by_id["R02-30"]["expect"]
  assert ">64 parts prevents SERVING" in by_id["R02-30"]["expect"]
  assert "before adapter release" in by_id["R02-31"]["expect"]
  assert "zero stop adapter calls" in by_id["R02-32"]["expect"]
  assert "same ID never replays" in by_id["R02-33"]["expect"]
  assert "old/current target" in by_id["R02-34"]["expect"]
  assert "release barrier" in by_id["R02-35"]["expect"]
  assert "before its adapter call" in by_id["R02-35"]["stimulus"]
  assert "adapter fences old physical generation" in by_id["R02-35"]["expect"]
  assert {case["evidence"] for case in cases} == {
    "startup", "discovery", "compatibility", "resource", "camera", "operation", "shutdown", "negative"
  }


if __name__ == "__main__":
  check()
  print("R02 design fixture: 35 cases, published methods and categories valid")
