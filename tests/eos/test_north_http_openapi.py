#!/usr/bin/env python3
"""Check generated north HTTP API routes and public schema inventory."""

import json
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DOCUMENT = ROOT / "share/ext/north_http/openapi.json"
SERVER = ROOT / "src/eos/ext/north_http/server/server.cpp"
GENERATOR = ROOT / "scripts/generate_north_http_openapi.py"


checked = subprocess.run(
    [sys.executable, str(GENERATOR), "--check"],
    capture_output=True, text=True, check=False,
)
assert checked.returncode == 0, checked.stderr


def unique_object(pairs):
  result = {}
  for key, value in pairs:
    assert key not in result, f"duplicate OpenAPI JSON key: {key}"
    result[key] = value
  return result


document = json.loads(DOCUMENT.read_text(encoding="utf-8"), object_pairs_hook=unique_object)
assert document["openapi"] == "3.1.0"
paths = document["paths"]
routes = {(method.upper(), path) for path, methods in paths.items() for method in methods}
assert len(routes) == sum(len(methods) for methods in paths.values())

# These HTTP-only routes must remain present in the adapter and its proto
# annotations, including the transport used by long-lived connections.
source = SERVER.read_text(encoding="utf-8")
for method, path in (
    ("GET", "/health/live"),
    ("GET", "/health/ready"),
    ("GET", "/v1/eos/audit/status"),
    ("GET", "/v1/eos/audit/action-lists"),
):
  assert (method, path) in routes
  pattern = rf'method == http::verb::{method.lower()} && target\.path == "{re.escape(path)}"'
  assert re.search(pattern, source), f"missing adapter route: {method} {path}"

for path in (
    "/v1/eos/resource:watch",
    "/v1/eos/workflows:watch",
    "/v1/eos/executions:monitor",
):
  operation = paths[path]["get"]
  assert operation["x-eos-transport"] == "sse"
  assert "text/event-stream" in operation["responses"]["200"]["content"]
  assert f'target.path == "{path}"' in source

websocket_path = "/v1/eos/monitor/ws"
assert paths[websocket_path]["get"]["x-eos-transport"] == "websocket"
assert f'target.path == "{websocket_path}"' in source
assert "websocket::is_upgrade(request)" in source

for method, path in (
    ("GET", "/v1/eos/discover"),
    ("GET", "/v1/eos/status"),
    ("POST", "/v1/eos/primitives:call"),
    ("POST", "/v1/eos/workflows"),
):
  assert (method, path) in routes
  assert paths[path][method.lower()]["x-eos-grpc-method"].startswith("/island.eos.v1.")

manifest = json.loads((ROOT / "share/ext/eos/contracts.manifest.json").read_text())
components = document["components"]["schemas"]
for entry in manifest["schemas"]:
  section, filename = entry["path"].split("/")
  key = "EOS_" + section + "_" + filename.removesuffix(".json").replace(".", "_")
  assert components[key]["$id"] == entry["id"]
