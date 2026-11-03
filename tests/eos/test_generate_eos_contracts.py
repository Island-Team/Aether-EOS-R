#!/usr/bin/env python3
"""Descriptor generation is deterministic and rejects stale runtime JSON."""

import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "scripts/generate_eos_contracts.py"


def run(output: Path, check: bool = False) -> subprocess.CompletedProcess[str]:
  command = [sys.executable, str(TOOL), "--output", str(output)]
  if check:
    command.append("--check")
  return subprocess.run(command, capture_output=True, text=True, check=False)


with tempfile.TemporaryDirectory(prefix="eos-contract-generation-test-") as directory:
  output = Path(directory)
  first = run(output)
  assert first.returncode == 0, first.stderr
  manifest_path = output / "contracts.manifest.json"
  manifest = json.loads(manifest_path.read_text())
  assert manifest["schema_count"] == 40
  assert {entry["path"] for entry in manifest["schemas"] if entry["path"].endswith(".v1_1.json")} == {
      f"actions/{name}.v1_1.json" for name in
      ("arm_movej", "arm_movel", "base_navigate", "gripper_motion", "speaker_play")
  }
  assert {entry["path"].split("/")[0] for entry in manifest["schemas"]} == {
      "actions", "body", "world"
  }
  assert run(output, check=True).returncode == 0

  schema = output / "world/detection.v1.json"
  schema.write_text(schema.read_text() + "\n")
  stale = run(output, check=True)
  assert stale.returncode != 0
  assert "world/detection.v1.json" in stale.stderr

  assert run(output).returncode == 0
  assert run(output, check=True).returncode == 0
  extra = output / "actions/obsolete.v1.json"
  extra.write_text("{}")
  assert run(output, check=True).returncode != 0
  assert run(output).returncode == 0
  assert not extra.exists()
