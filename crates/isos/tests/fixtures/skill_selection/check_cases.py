#!/usr/bin/env python3
"""Validate the frozen R04 IsOS design fixtures without executing skill code."""

import json
from pathlib import Path

root = Path(__file__).resolve().parent
cases = json.loads((root / "cases.json").read_text())
assert len(cases) == 20
assert [case["id"] for case in cases] == [f"R04-I{i:02}" for i in range(1, 21)]
assert all({"id", "operation", "mutation", "expected"} <= set(case) for case in cases)
for case in cases:
    for diagnostic in ([case["diagnostic"]] if "diagnostic" in case else case.get("diagnostics", [])):
        assert set(diagnostic) == {"code", "stage", "path"}
        assert diagnostic["path"] == "" or diagnostic["path"].startswith("/cores/isos/")
assert all("diagnostic" in case or "diagnostics" in case for case in cases[2:12])
sample = json.loads((root / "selection.example.json").read_text())
assert set(sample) == {"schema_version", "active_revision", "libraries"}
assert sample["schema_version"] == 1
assert len(sample["active_revision"]) == 64
assert len(sample["libraries"]) == 1
assert set(sample["libraries"][0]) == {
    "package_id", "package_version", "package_root", "payload_root",
    "receipt_sha256", "release_sha256", "payload_sha256", "library", "library_sha256",
}
for digest in ("receipt_sha256", "release_sha256", "payload_sha256", "library_sha256"):
    assert len(sample["libraries"][0][digest]) == 64
catalog = json.loads((root / "catalog-setting.example.json").read_text())
assert catalog["data_type"] == "object_array"
assert catalog["min_items"] == 0 and catalog["max_items"] == 16
assert catalog["items_schema"]["additionalProperties"] is False
assert catalog["items_schema"]["properties"]["package"]["additionalProperties"] is False
for name in ("README.md", "README_cn.md"):
    assert (root / name).is_file()
print("R04 IsOS design fixtures: 20 cases, selection and catalog schemas pass")
