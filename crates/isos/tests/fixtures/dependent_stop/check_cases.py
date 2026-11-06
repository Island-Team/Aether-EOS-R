#!/usr/bin/env python3
"""Validate the design-only R06 IsOS stop inventory."""

import json
from pathlib import Path


root = Path(__file__).resolve().parent
data = json.loads((root / "cases.json").read_text())
assert data["schema_version"] == 1
cases = data["cases"]
assert len(cases) >= 56
ids = [case["id"] for case in cases]
assert len(ids) == len(set(ids))
reasons = {
    "child_exit_unexpected", "stop_signal_failed", "stop_deadline_exceeded",
    "stop_forced_kill", "stop_group_unreaped", "eos_stop_unconfirmed",
    "eos_receipt_missing", "eos_receipt_invalid", "eos_receipt_late",
    "control_grant_retire_failed", "stop_result_persist_failed", "stop_interrupted",
}
for case in cases:
    assert case["expected_outcome"] in {"confirmed", "unknown", "no_stop"}, case
    assert case["expected_stop_order"] == list(dict.fromkeys(case["expected_stop_order"])), case
    assert set(case["expected_stop_order"]).issubset(set(case["selected"])), case
    if case["expected_outcome"] == "unknown":
        assert case["expected_reason"] in reasons, case
        assert case["expected_event"] in {"deployment_stop_unconfirmed", "unavailable"}, case
    else:
        assert "expected_reason" not in case, case
    if case.get("automatic_restart_blocked"):
        assert case["expected_outcome"] == "unknown", case
    if case.get("status_client_required") is False:
        assert case["trigger"] in {"provider_loss", "daemon_poll"}, case
        assert case["expected_outcome"] == "unknown", case
        assert case["expected_reason"] == "child_exit_unexpected", case
        assert case["dependent_reaped"], case
    if case.get("candidate_spawned") is False:
        assert case["expected_outcome"] in {"unknown", "no_stop"}, case
    if case.get("old_run_unchanged"):
        assert case["expected_stop_order"] == [], case
    if case.get("must_not_signal_reused_pgid") or case.get("must_not_signal_saved_pgid"):
        assert case["expected_outcome"] == "unknown", case
    if case.get("signal_pinned_original_group"):
        assert case["id"] == "leader_exited_unreaped_before_stop", case
        assert case["expected_outcome"] == "unknown", case
    if case.get("new_run_record_before_spawn"):
        assert case["saved_phase"] == "running" and case["expected_outcome"] == "no_stop", case
    if "cleanup_at_ms" in case:
        assert case["cleanup_at_ms"] == case["stop_timeout_ms"] - 2000, case
        assert case["max_kill_reap_ms"] + case["min_finalize_ms"] == 2000, case
    if case.get("late_worker_cannot_publish_confirmed"):
        assert case["expected_outcome"] == "unknown", case
        assert case["late_worker_durable_outcome"] == "unknown", case
        assert case["emergency_signal_attempted"], case
    if case.get("prior_live_outcome") == "confirmed":
        assert case["trigger"] == "manager_recovery", case
        assert case["expected_outcome"] == "unknown", case
        assert case["recovered_process_outcomes"] == ["unknown"] * len(case["selected"]), case
        assert case["historical_event_is_not_authority"], case
    if "receipt_mutation" in case:
        assert case["receipt_fixture"] == "eos_receipt_confirmed.json", case
        assert case["expected_outcome"] == "unknown", case
        assert case["expected_reason"] in {
            "eos_receipt_missing", "eos_receipt_invalid", "eos_receipt_late",
            "eos_stop_unconfirmed", "stop_deadline_exceeded",
        }, case
    if case.get("support_token_not_stop_proof"):
        assert case["expected_reason"] == "eos_receipt_missing", case
assert {"clean_reverse_dependency", "eos_dual_ack_unknown", "provider_crash", "restart_old_stop_unknown", "log_collector_outage", "simulator_crash", "socket_cleanup_failure"}.issubset(ids)
assert "daemon_short_snapshot" in ids
assert {"leader_exited_unreaped_before_stop", "leader_reaped_before_group_signal", "cleanup_window_starts_before_deadline", "crash_after_durable_stop_intent", "crash_during_stop", "new_run_stop_state_reset", "crash_between_p07_publish_and_state_reset", "stop_intent_fsync_failure", "manager_crash_with_running_record"}.issubset(ids)
assert {"stalled_intent_rename_after_deadline", "stalled_intent_parent_fsync_after_deadline", "live_confirmed_then_manager_restart", "deadline_equality_fence"}.issubset(ids)
assert {"receipt_duplicate_json_key", "receipt_unknown_json_key", "receipt_missing_required_key", "receipt_oversized_line", "receipt_partial_line", "receipt_second_line", "receipt_wrong_pipe", "receipt_wrong_run", "receipt_wrong_pid", "receipt_wrong_revision", "receipt_wrong_backend", "receipt_wrong_parts", "receipt_group_not_drained", "receipt_late", "receipt_after_global_deadline", "receipt_nonzero_eos_exit", "receipt_short_eos_grace", "managed_stop_token_only"}.issubset(ids)
assert {"receipt_stderr_log_only", "receipt_descendant_holds_stdout", "managed_stop_token_absent_preflight"}.issubset(ids)
by_id = {case["id"]: case for case in cases}
assert by_id["stale_eos_receipt"]["receipt_source"] == "selected_eos_stdout"
assert by_id["stale_eos_receipt"]["receipt_arrival"] == "before_eos_grace"
assert by_id["stale_eos_receipt"]["expected_reason"] == "eos_receipt_invalid"
assert by_id["receipt_wrong_pipe"]["selected_pipe_has_receipt"] is False
assert by_id["receipt_wrong_pipe"]["selected_pipe_empty_through_eos_grace"] is True
assert by_id["receipt_wrong_pipe"]["expected_reason"] == "eos_receipt_missing"
assert by_id["receipt_after_global_deadline"]["terminal_pending_at_deadline"] is True
assert by_id["receipt_after_global_deadline"]["eos_group_reaped_before_cleanup_at"] is True
assert by_id["receipt_after_global_deadline"]["expected_reason"] == "stop_deadline_exceeded"
assert by_id["receipt_late"]["expected_reason"] == "eos_receipt_late"
assert by_id["receipt_short_eos_grace"]["expected_reason"] == "eos_receipt_late"

receipt_keys = {
    "schema_version", "type", "run_id", "eos_pid", "active_revision", "backend_node",
    "backend_pid", "backend_pgid", "generation_id", "stop_epoch", "admission_closed",
    "parts", "backend_group_drained", "backend_reaped", "sockets_removed", "outcome", "reason_code",
}
for name, outcome in (("eos_receipt_confirmed.json", "confirmed"), ("eos_receipt_unknown.json", "unknown")):
    raw = (root / name).read_bytes()
    assert raw.endswith(b"\n") and len(raw) <= 4096, name
    receipt = json.loads(raw)
    assert set(receipt) == receipt_keys, name
    assert receipt["schema_version"] == 1 and receipt["type"] == "eos_terminal_stop", name
    assert receipt["backend_node"] == "eos/process/worker/sim", name
    assert receipt["backend_pid"] == receipt["backend_pgid"] > 0, name
    assert receipt["outcome"] == outcome, name
    assert receipt["parts"] == sorted(receipt["parts"], key=lambda part: part["node_path"]), name
    assert {part["kind"] for part in receipt["parts"]} == {"base", "arm"}, name
    assert all(set(part) == {"node_path", "kind", "outcome"} for part in receipt["parts"]), name
    if outcome == "confirmed":
        assert receipt["reason_code"] is None, name
        assert all(part["outcome"] == "stopped" for part in receipt["parts"]), name
        assert all(receipt[key] for key in ("admission_closed", "backend_group_drained", "backend_reaped", "sockets_removed")), name
print(f"R06 IsOS stop fixture: {len(cases)} cases passed")
