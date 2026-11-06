#!/usr/bin/env python3
"""Check the pinned, sanitized Q03 browser response set."""
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent
manifest = json.loads((ROOT / "manifest.json").read_text())
graph = json.loads((ROOT / "graph.json").read_text())["graph"]
nodes = [node for core in graph["cores"].values()
         for layer in core["layers"].values()
         for group in layer["groups"].values()
         for node in group["nodes"].values()]
wires = [source for node in nodes for sources in node.get("inputs", {}).values()
         for source in (sources if isinstance(sources, list) else [sources])]
assert graph["version"] == "0.1.0"
assert len(nodes) == manifest["nodes"] == 19
assert len(wires) == manifest["input_wires"] == 10
assert sum(node["node_type"] == "eos.dispatcher.primitive_base@v0.1" for node in nodes) == 1
assert manifest["stub_delay_ms"] == 5 and manifest["timeout_ms"] == 2000
assert manifest["viewport"] == {"width": 1280, "height": 800}
for name, expected in manifest["responses"].items():
    data = (ROOT / name).read_bytes()
    assert len(data) == expected["bytes"], name
    assert hashlib.sha256(data).hexdigest() == expected["sha256"], name
    assert re.search(rb"/home/[A-Za-z0-9_-]+/", data) is None and b"/tmp/m4" not in data, name
assert json.loads((ROOT / "synthetic-result-in-progress.json").read_text())["result"]["status"] == "InProgress"
assert json.loads((ROOT / "synthetic-result-completed.json").read_text())["result"]["status"] == "Completed"
print("Q03 fixture: 19 nodes, 10 wires, one base binding, hashes verified")
