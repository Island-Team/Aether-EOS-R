"""Validate the frozen R04 design cases without claiming implementation evidence."""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
selection = json.loads((ROOT / "selection.json").read_text())
cases = json.loads((ROOT / "cases.json").read_text())
preflight = json.loads((ROOT / "preflight_cases.json").read_text())

assert selection["schema_version"] == cases["schema_version"] == 1
assert len(selection["active_revision"]) == 64
assert len(selection["libraries"]) == 1
assert set(selection["libraries"][0]) == {
    "package_id", "package_version", "package_root", "payload_root",
    "receipt_sha256", "release_sha256", "payload_sha256", "library",
    "library_sha256",
}
assert len(cases["cases"]) == 27
assert [entry["id"] for entry in cases["cases"]] == [
    f"R04-{index:02}" for index in range(1, 28)
]
assert {entry["boundary"] for entry in cases["cases"]} == {
    "selection", "package", "worker", "abi", "service", "broker",
    "lifecycle", "compatibility", "observability", "limits",
}
assert all(entry["input"] and entry["expect"] for entry in cases["cases"])
assert preflight["schema_version"] == 1
assert set(preflight) == {"schema_version", "cases"}
assert len(preflight["cases"]) == 8
assert [entry["id"] for entry in preflight["cases"]] == [
    f"R04-PF{index:02}" for index in range(1, 9)
]
assert {entry["boundary"] for entry in preflight["cases"]} == {
    "candidate", "isolation", "cleanup", "output", "handoff", "restart",
}
allowed_isos_codes = {
    "skill_catalog_preflight_rejected",
    "skill_catalog_preflight_invalid_output",
    "skill_catalog_preflight_timeout",
    "skill_catalog_preflight_cleanup_unconfirmed",
}
for entry in preflight["cases"]:
    assert set(entry) == {
        "id", "boundary", "input", "expect", "expected_isos_codes",
    }
    assert isinstance(entry["input"], str) and entry["input"].strip()
    assert isinstance(entry["expect"], str) and entry["expect"].strip()
    assert isinstance(entry["expected_isos_codes"], list)
    assert len(entry["expected_isos_codes"]) == len(set(entry["expected_isos_codes"]))
    assert set(entry["expected_isos_codes"]) <= allowed_isos_codes
assert set(preflight["cases"][5]["expected_isos_codes"]) == allowed_isos_codes
print("R04 design fixtures: 27 host cases, 8 preflight cases and selected shape valid")
