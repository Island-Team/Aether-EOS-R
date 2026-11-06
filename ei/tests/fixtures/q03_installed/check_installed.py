"""Installed EGIA setup probe against the frozen selected Q01 stage overlay."""

from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path

import egia
import island
from island.path import Layout
from island.share import configured_resources

_FIXTURE_SHA256 = "1e239085ec48ac988cdc30732e7843141a184ecfd60ca6e3b49459a971a2a702"
_TYPE_IDS = {
  "egia.link.gateway@v0.1",
  "egia.perception.admission@v0.1",
  "egia.context.builder@v0.1",
  "egia.missions.queue@v0.1",
  "egia.decision.planner@v0.1",
  "egia.safety.gate@v0.1",
  "egia.monitor.health@v0.1",
}


def main() -> None:
  prefix = Path.cwd().resolve()
  fixture_path = prefix / "q03-fixture" / "stages.json"
  fixture = fixture_path.read_bytes()
  assert hashlib.sha256(fixture).hexdigest() == _FIXTURE_SHA256
  overlay = json.loads(fixture)
  graph = {"version": overlay["version"], "robot": {}, "cores": overlay["cores"]}
  link = graph["cores"]["egia"]["layers"]["io"]["groups"]["link"]["nodes"]["gateway"]
  endpoint = link["settings"]["gateway"]
  layout = Layout.resolve()
  resources = configured_resources(layout)
  paths = [
    Path(egia.__file__),
    Path(island.__file__),
    layout.etc / "island.toml",
    layout.share,
    *resources.roots,
    *resources.files.values(),
  ]
  assert all(path.resolve().is_relative_to(prefix) for path in paths), paths
  replies = {}
  for op in ("catalog", "validate", "render"):
    request = {
      "schema_version": 1,
      "op": op,
      "core": "egia",
      "revision": overlay["revision"],
      "graph": graph,
      "endpoints": {"eos_gateway": endpoint},
      "packages": [],
    }
    result = subprocess.run(
      [str(prefix / "bin" / "egia-config"), "setup", "--json"],
      input=json.dumps(request),
      text=True,
      capture_output=True,
      check=True,
      cwd=prefix,
    )
    replies[op] = json.loads(result.stdout)
    assert replies[op]["ok"] and not replies[op]["diagnostics"], replies[op]
  catalog = replies["catalog"]["catalog"]
  assert {item["id"] for item in catalog["stage_types"]} == _TYPE_IDS
  assert len(catalog["routed_edges"]) == 2
  files = {item["path"]: item["content"] for item in replies["render"]["files"]}
  actual = files["egia/stages.json"].encode()
  assert actual == fixture, "installed stage overlay differs byte-for-byte"
  print(
    json.dumps(
      {
        "revision": overlay["revision"],
        "stages_sha256": hashlib.sha256(actual).hexdigest(),
        "stage_types": sorted(_TYPE_IDS),
        "installed_prefix": str(prefix),
      },
      sort_keys=True,
    )
  )


if __name__ == "__main__":
  main()
