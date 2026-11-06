#!/usr/bin/env python3
"""Check the IsOS owner-side R04 preflight design table and closed wire examples."""
import json
from pathlib import Path

root = Path(__file__).resolve().parent
raw = (root / "preflight_cases.json").read_text()

def no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


data = json.loads(raw, object_pairs_hook=no_duplicates)
assert set(data) == {"schema_version", "eos_design_commit", "success_result", "rejection_result", "cases"}
assert data["schema_version"] == 1
assert len(data["eos_design_commit"]) == 40
success = data["success_result"]
assert set(success) == {"schema_version", "ok", "active_revision", "generation", "run_id", "selection_sha256", "libraries"}
assert success["schema_version"] == 1 and success["ok"] is True
for name in ("active_revision", "generation", "selection_sha256"):
    assert len(success[name]) == 64 and all(c in "0123456789abcdef" for c in success[name])
assert len(success["libraries"]) == 1
assert set(success["libraries"][0]) == {"package_id", "package_version", "library", "library_sha256", "abi_version", "skill_count"}
assert success["libraries"][0]["abi_version"] == 2
assert 1 <= success["libraries"][0]["skill_count"] <= 256
rejection = data["rejection_result"]
assert rejection == {"schema_version": 1, "ok": False, "stage": "abi", "code": "abi_descriptor_invalid"}
cases = data["cases"]
assert [case["id"] for case in cases] == [f"R04-IPF{i:02}" for i in range(1, 19)]
allowed = {None, "skill_selection_invalid", "skill_catalog_preflight_rejected", "skill_catalog_preflight_invalid_output", "skill_catalog_preflight_timeout", "skill_catalog_preflight_cleanup_unconfirmed"}
assert all({"id", "mutation", "expected", "isos_code"} <= set(case) and case["isos_code"] in allowed for case in cases)
assert cases[6]["isos_code"] == "skill_selection_invalid" and "eos_stage" not in cases[6] and "eos_code" not in cases[6]
closed = {
    "input": {"preflight_arguments_invalid", "preflight_grants_invalid", "preflight_profile_invalid"},
    "selection": {"selection_invalid", "selection_revision_mismatch", "selection_manifest_invalid", "selection_digest_mismatch", "selection_handoff_incomplete"},
    "package": {"package_invalid", "package_changed", "package_unsafe", "library_changed"},
    "elf": {"library_not_elf", "library_dependency_unsafe"},
    "worker": {"worker_spawn_failed", "worker_sandbox_failed", "worker_inspection_failed", "worker_timeout", "worker_reap_failed"},
    "abi": {"abi_library_invalid", "abi_descriptor_invalid", "abi_schema_invalid", "abi_capability_unowned"},
    "catalog": {"catalog_duplicate_identity", "catalog_capacity", "catalog_broker_unavailable", "catalog_binding_unavailable", "catalog_list_oversize"},
}
for case in cases:
    if "eos_stage" in case or "eos_code" in case:
        assert case["eos_code"] in closed[case["eos_stage"]]
        assert case["isos_code"] == "skill_catalog_preflight_rejected"
assert len(json.dumps(success, separators=(",", ":")).encode()) < 16384
assert len(json.dumps(rejection, separators=(",", ":")).encode()) < 16384
retirement = json.loads((root / "retirement_cases.json").read_text(), object_pairs_hook=no_duplicates)
assert set(retirement) == {"schema_version", "owner", "boundary", "correlation_contract", "cases"}
assert retirement["schema_version"] == 1 and retirement["owner"] == "isos"
correlation = retirement["correlation_contract"]
assert correlation["request_correlation_id"] == "restart-request-42"
assert correlation["stop_event"] == {"source": "isos", "event_code": "deployment_stop_unconfirmed", "run_scope": "old-admitted-run", "correlation_id": "restart-request-42"}
assert correlation["candidate_cleanup_event"] == {"source": "isos", "event_code": "deployment_preflight_failed", "run_scope": "manager-session", "correlation_id": "restart-request-42", "only_when": "candidate-retirement-unconfirmed"}
retirement_cases = retirement["cases"]
assert [case["id"] for case in retirement_cases] == [f"R04-IRT{i:02}" for i in range(1, 7)]
assert all({"id", "fault", "expected", "restart_code", "old_terminal_reason", "candidate_scope_published"} <= set(case) for case in retirement_cases)
assert retirement_cases[0]["candidate_scope_published"] is True
assert all(case["candidate_scope_published"] is False for case in retirement_cases[1:])
assert all(case["restart_code"] in {None, "deployment_stop_unconfirmed", "skill_catalog_preflight_cleanup_unconfirmed"} for case in retirement_cases)
assert all(case.get("restart_stage") == "preflight" and case.get("restart_path") == "" for case in retirement_cases if case["restart_code"] == "skill_catalog_preflight_cleanup_unconfirmed")
assert retirement_cases[5]["old_terminal_reason"] == "eos_stop_unconfirmed"
assert retirement_cases[5]["manager_restart_reasons"] == ["stop_interrupted", "stop_result_persist_failed"]
print("R04 IsOS candidate preflight fixtures: 18 probe and 6 retirement cases pass")
