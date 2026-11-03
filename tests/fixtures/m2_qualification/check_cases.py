#!/usr/bin/env python3
"""Validate the frozen M2 cross-repository qualification design."""

from __future__ import annotations

import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
IDS = {f"M2-{i:02d}" for i in range(1, 29)}
PHASES = {"install", "selection", "apply", "preflight", "launch", "events", "cursor", "negative", "terminal"}
REQUIRED = {f"P0{i}" for i in range(1, 8)}


def check() -> None:
    data = json.loads((HERE / "cases.json").read_text())
    assert data["schema_version"] == 1 and data["gate"] == "M2"
    assert data["status"] == "replay_pending_reattest"
    assert set(data["baseline"]) == {"eos", "isos", "egia", "mujoco", "isosui"}
    assert all(re.fullmatch(r"[0-9a-f]{40}", value) for value in data["baseline"].values())
    cases = data["cases"]
    assert len(cases) == len(IDS) and {case["id"] for case in cases} == IDS
    assert {case["phase"] for case in cases} == PHASES
    assert all(set(case) == {"id", "phase", "owner", "input", "expect", "requires", "assertions"} for case in cases)
    assert all(all(isinstance(case[key], str) and case[key] for key in ("owner", "input", "expect", "requires")) for case in cases)
    assert all(isinstance(case["assertions"], dict) and case["assertions"] for case in cases)
    requirements = {part for case in cases for part in case["requires"].split(",")}
    assert REQUIRED <= requirements and {"R02-later", "R03-later", "R05-later"} <= requirements
    assert all(case["requires"].split(",") == [p.strip() for p in case["requires"].split(",")] for case in cases)
    assert all("P07" in case["requires"] for case in cases if case["phase"] == "cursor")
    by_id = {case["id"]: case["assertions"] for case in cases}
    cases_by_id = {case["id"]: case for case in cases}
    assert by_id["M2-01"]["pin"] == "island/mujoco-sim@0.1.1"
    assert by_id["M2-03"]["receipt_published"] is False
    assert by_id["M2-23"]["link_result"] == "success" and by_id["M2-23"]["receipt_schema"] == 1
    assert by_id["M2-23"]["listing_status"] == "unverified_legacy_link"
    assert by_id["M2-23"]["apply_run_code"] == "package_invalid_manifest"
    assert by_id["M2-23"]["apply_run_path"] == "/cores/eos/layers/process/groups/mujoco/nodes/sim/package"
    assert by_id["M2-10"]["code"] == "reader_feature_unsupported" and by_id["M2-10"]["stage"] == "feature"
    assert by_id["M2-10"]["path"] == "/cores/eos/layers/process/groups/mujoco/nodes/sim/node_type"
    assert by_id["M2-24"]["code"] == "reader_type_unsupported" and by_id["M2-24"]["stage"] == "reader"
    assert by_id["M2-24"]["path"] == "/cores/eos/layers/hardware/groups/camera/nodes/front/node_type"
    assert by_id["M2-25"]["code"] == "unsupported_node_type" and by_id["M2-25"]["reader_absent"] is True
    assert by_id["M2-26"]["code"] == "package_invalid_sidecar" and by_id["M2-26"]["stage"] == "sidecar"
    assert by_id["M2-27"]["code"] == "package_missing_file" and by_id["M2-27"]["stage"] == "manifest"
    assert by_id["M2-27"]["path"] == "/cores/eos/layers/process/groups/mujoco/nodes/sim/package"
    assert by_id["M2-27"]["reader_binary"] is None and by_id["M2-27"]["child_spawned"] is False
    assert cases_by_id["M2-27"]["requires"] == "P01"
    assert by_id["M2-28"]["boundary"] == "owner_unit_predicate_only"
    assert by_id["M2-28"]["test"] == "contributed_type_needs_exact_catalog_pin"
    assert by_id["M2-28"]["code"] == "reader_package_pin_mismatch" and by_id["M2-28"]["stage"] == "package"
    assert by_id["M2-28"]["public_replay_expected"] is False
    assert cases_by_id["M2-28"]["requires"] == "P05"
    assert by_id["M2-13"]["reader_preflight"] == "admit_reader_only" and by_id["M2-13"]["egia_ready_claim"] is False
    assert by_id["M2-15"]["scope_kind"] == "manager_session" and by_id["M2-15"]["validate_only_anchor"] is False
    assert by_id["M2-16"]["scope_kind"] == "deployment_run" and by_id["M2-16"]["empty_filtered_page_is_gap"] is False
    assert by_id["M2-17"]["expired_code"] == "log_cursor_expired" and by_id["M2-17"]["new_run_code"] == "log_run_changed"
    assert by_id["M2-19"]["scope_error_code"] == "log_scope_unavailable" and by_id["M2-19"]["scope_error_path"] == "/scope"
    assert by_id["M2-21"]["public_mutation"] == "cursor_state_directory_obstruction"
    assert by_id["M2-21"]["public_http"] == 500 and by_id["M2-21"]["public_code"] == "log_scope_publish_failed"
    assert by_id["M2-21"]["public_path"] == "/run_id" and by_id["M2-21"]["public_child_spawned"] is False
    assert by_id["M2-21"]["owner_unit_tests"] == ["scope_publication_failure_blocks_child_spawn", "uncertain_publication_keeps_surviving_new_run_and_unavailable_anchor"]
    assert by_id["M2-21"]["owner_uncertain_new_run_survives"] is True
    assert by_id["M2-21"]["owner_uncertain_anchor_state"] == "unavailable"
    assert by_id["M2-21"]["actual_parent_fsync_failure_injected"] is False
    negative = " ".join(case["expect"] for case in cases)
    for token in ("log_cursor_expired", "log_run_changed", "log_collector_gap", "log_scope_unavailable", "log_scope_publish_failed", "first_prelaunch_failure", "first_ready_or_failure", "before spawn", "saved bytes"):
        assert token in negative, token
    for lang in ("docs/island/m2-qualification.md", "docs_cn/island/m2-qualification_cn.md"):
        doc = (ROOT / lang).read_text()
        assert "P07" in doc and "R02" in doc and "logs/test/<run-id>/" in doc
        for code in ("unverified_legacy_link", "package_invalid_manifest", "reader_feature_unsupported", "reader_type_unsupported", "unsupported_node_type", "package_invalid_sidecar", "package_missing_file", "reader_package_pin_mismatch"):
            assert code in doc, (lang, code)
    for lang in ("docs/island/implementation-todo.md", "docs_cn/island/implementation-todo_cn.md"):
        todo = (ROOT / lang).read_text()
        for req in sorted(REQUIRED - {"P07"}):
            assert f"| [x] **{req}** |" in todo
        assert "| [x] **P07** |" in todo
    print(f"M2 qualification fixture: {len(cases)} cases, {len(REQUIRED)} owner dependencies, bilingual docs OK")


if __name__ == "__main__":
    check()
