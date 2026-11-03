#!/usr/bin/env python3
"""Validate the EOS R05 contract matrix without asserting runtime behavior."""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
data = json.loads(Path(__file__).with_name("cases.json").read_text(encoding="utf-8"))
cases = data["cases"]
ids = [case["id"] for case in cases]
assert len(cases) == 31 and len(ids) == len(set(ids))
assert data["feature"] == "strict-socket-v1"
assert data["child_flag"] == "--managed-strict-socket"
assert {case["stage"] for case in cases} == {
    "child_launch", "profile", "startup", "runtime_health",
    "movement", "restart", "integration", "scope",
}
for case in cases:
    assert isinstance(case["expect"], dict) and case["expect"], case["id"]

local_launch = next(case for case in cases if case["id"] == "local-eos-backend-also-strict")
assert local_launch["expect"]["receipt_verified"] is False
assert local_launch["expect"]["sha_args_prove_provenance"] is False
legacy_case = next(case for case in cases if case["id"] == "legacy-example-read-only")
assert legacy_case["expect"]["migration_required"] is True

legacy_example = (ROOT / "examples/v2/mujoco-isos.toml").read_text(encoding="utf-8")
for token in ('Historical read-only 1.0.0 graph fixture', 'version = "1.0.0"', 'scene = "default"'):
    assert token in legacy_example, token

for relative in (
    "docs/eos/interfaces/mujoco.md",
    "docs_cn/eos/interfaces/mujoco_cn.md",
    "docs/eos/design/runtime/hardware.md",
    "docs_cn/eos/design/runtime/hardware_cn.md",
    "docs/eos/design/runtime/foundation.md",
    "docs_cn/eos/design/runtime/foundation_cn.md",
    "docs/eos/usage/mujoco.md",
    "docs_cn/eos/usage/mujoco_cn.md",
):
    text = (ROOT / relative).read_text(encoding="utf-8")
    for token in ("strict-socket-v1", "--managed-strict-socket", "R05", "R06"):
        assert token in text, (relative, token)

print(f"EOS R05 design matrix: {len(cases)} unique cases; JSON and EN/CN markers valid")
print("Behavioral evidence is in focused Rust/real-child tests; cross-owner qualification and independent review remain separate")
