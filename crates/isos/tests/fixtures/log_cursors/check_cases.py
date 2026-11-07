#!/usr/bin/env python3
"""Executable design fixtures for P07 wire and durable scope transitions."""
import json
from pathlib import Path

HERE = Path(__file__).parent
wire = json.loads((HERE / "cases.json").read_text())
assert wire["contract"] == "isos-log-cursors-p07-v1"
assert wire["endpoint"] == "/api/isos/v1/logs" and wire["view"] == "current"
assert wire["response_keys"] == ["scope", "anchor", "lines", "next_cursor", "rotated", "diagnostics"]
assert wire["scope_keys"] == ["kind", "run_id", "active_revision"]
assert wire["anchor_keys"] == ["kind", "state", "event"]
assert wire["filter_order"] == ["scope", "anchor", "run_id", "source", "level", "node_path", "correlation_id"]
assert wire["diagnostic_order"] == ["log_run_changed", "log_cursor_expired", "log_collector_gap", "log_anchor_unavailable"]
assert wire["bounds"] == {"default_limit": 100, "max_limit": 200, "max_scan_bytes": 65536, "max_cursor_ascii_bytes": 512}
expected_names = {
    "manager_package_failure_filter_bypass", "manager_apply_failure", "manager_validate_only_not_anchor",
    "run_pending_empty", "run_ready_first", "run_failure_before_ready", "run_filtered_anchor_and_progress",
    "rotation_retained", "rotation_expired", "new_admitted_run_rebase", "stop_retains_last_run",
    "failed_retains_last_run", "collector_outage_recovery", "anchor_write_failure",
    "legacy_socket_cannot_anchor", "cursor_filter_mismatch", "cursor_tampered", "legacy_cursor_rejected",
    "node_path_invalid", "correlation_id_invalid", "scope_unrecoverable", "cursor_key_rotated",
}
cases = wire["cases"]
assert len(cases) == len(expected_names)
assert {c["name"] for c in cases} == expected_names
for c in cases:
    assert set(c) in ({"name", "phase", "query", "expected"}, {"name", "phase", "query", "expected_error"})
    if "expected_error" in c:
        error = c["expected_error"]
        if error["code"] == "log_scope_unavailable":
            assert (error["http"], error["path"]) == (503, "/scope")
        else:
            assert error["http"] == 400
            assert (error["code"], error["path"]) in {
                ("log_cursor_invalid", "/query/cursor"),
                ("log_cursor_filter_mismatch", "/query/cursor"),
                ("log_node_path_invalid", "/query/node_path"),
                ("log_correlation_id_invalid", "/query/correlation_id"),
            }
        continue
    e = c["expected"]
    assert e["scope_kind"] in {"manager_session", "deployment_run"}
    assert e["anchor_state"] in {"pending", "found", "unavailable"}
    if e["anchor_state"] == "pending":
        assert e.get("lines") == [] and e.get("anchor_code") is None
    if e["anchor_state"] == "unavailable":
        assert any(x["code"] == "log_anchor_unavailable" for x in e["diagnostics"])
    codes = [x["code"] for x in e["diagnostics"]]
    assert codes == sorted(codes, key=wire["diagnostic_order"].index)
    assert all(x["count"] is None or (isinstance(x["count"], int) and x["count"] >= 0) for x in e["diagnostics"])
assert next(c for c in cases if c["name"] == "manager_package_failure_filter_bypass")["expected"]["anchor_source"] == "packages"
for name in ("stop_retains_last_run", "failed_retains_last_run"):
    assert next(c for c in cases if c["name"] == name)["expected"]["process_status_run_id"] == "r7"


def resolve(state, retained):
    """Reference scope selector. The durable last-run ID survives log rotation."""
    last = state.get("last_run_id") if state["valid"] else None
    if last is None and (not state["valid"] or not state.get("never_run", False)):
        return {"http": 503, "code": "log_scope_unavailable", "path": "/scope",
                "run_id_state": "unavailable", "scope_kind": None, "scope_run_id": None}
    if last is None:
        return {"http": 200, "run_id_state": "none", "scope_kind": "manager_session",
                "scope_run_id": state["manager_session"]}
    out = {"http": 200, "run_id_state": "known", "scope_kind": "deployment_run", "scope_run_id": last}
    anchor = state.get("anchor") if state["valid"] else None
    if anchor and anchor["run_id"] == last and not state.get("anchor_proven", True):
        out["anchor_state"] = "unavailable"
        out["diagnostics"] = ["log_collector_gap", "log_anchor_unavailable"]
    elif anchor and anchor["run_id"] == last:
        out["anchor_code"] = anchor["event_code"]
        out["lines"] = [e["event_code"] for e in retained if e["run_id"] == last and e["accepted_position"] > anchor["accepted_position"]]
    elif not state["valid"] or state.get("admission_interrupted"):
        out["anchor_state"] = "unavailable"
        out["diagnostic"] = "log_anchor_unavailable"
    return out


def admit(state, candidate, publication):
    """Reference admission barrier: no spawn until durable publication succeeds."""
    result = {"http": 500, "code": "log_scope_publish_failed", "path": "/run_id", "spawned": False}
    durable = dict(state)
    if publication == "write_failed":
        result["durable_last_run_id"] = state["last_run_id"]
    elif publication == "parent_fsync_uncertain_survives":
        durable["last_run_id"] = candidate
        durable["anchor"] = None
        durable["admission_interrupted"] = True
        result["durable_last_run_id"] = candidate
        recovered = resolve(durable, [])
        result["recovered_scope_kind"] = recovered["scope_kind"]
        result["recovered_scope_run_id"] = recovered["scope_run_id"]
        result["recovered_anchor_state"] = recovered["anchor_state"]
        result["recovered_diagnostic"] = recovered["diagnostic"]
    else:
        raise AssertionError(publication)
    return result


state_cases = json.loads((HERE / "state-scenarios.json").read_text())
assert state_cases["contract"] == "isos-log-cursors-p07-state-v1"
assert len(state_cases["cases"]) == 12
for scenario in state_cases["cases"]:
    retained = scenario.get("retained_events", [])
    positions = [e["accepted_position"] for e in retained]
    assert positions == sorted(set(positions)), scenario["name"]
    if "publication" in scenario:
        actual = admit(scenario["state"], scenario["candidate_run_id"], scenario["publication"])
    else:
        actual = resolve(scenario["state"], scenario["retained_events"])
    assert actual == scenario["expected"], (scenario["name"], actual, scenario["expected"])

print(f"P07 contract fixtures OK: {len(cases)} wire cases, {len(state_cases['cases'])} state scenarios")
