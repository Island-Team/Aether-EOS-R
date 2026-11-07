#!/usr/bin/env python3
"""Static design-matrix checks for R07 UI client and state boundaries."""
import json
from pathlib import Path

root = Path(__file__).resolve().parents[4]
fixture = json.loads(Path(__file__).with_name("cases.json").read_text())
cases = fixture["cases"]
ids = [case["id"] for case in cases]
assert len(ids) == len(set(ids)), "duplicate case ID"
assert len(cases) >= 46, "missing sender and result boundary cases"
assert {case["category"] for case in cases} >= {"send", "send_transition", "log_link", "request", "owner_result", "transition", "protocol", "fence", "owner_error", "transport", "poll", "stop"}
assert {case["response"]["result"]["status"] for case in cases if case["category"] == "owner_result"} == {"Completed", "Failed", "Cancelled", "Rejected", "InProgress", "OutcomeUnknown"}
send = next(case for case in cases if case["id"] == "public_send_exact")
assert set(send["request"]) == {"op", "header", "payload"}
assert send["request"]["op"] == "primitive_typed"
assert send["request"]["header"]["source_path"] == send["given"]["binding_path"]
assert send["request"]["header"]["target_path"] == send["given"]["edge_target"]
assert send["request"]["header"]["active_revision"] == fixture["revision_a"]
assert send["given"]["node_type"] == "eos.dispatcher.primitive_base@v0.1"
assert send["request"]["header"]["payload_length_bytes"] == len(json.dumps(send["request"]["payload"], separators=(",", ":")).encode())
assert "authority_grant" not in send["request"] and "authority_grant" not in send["request"]["header"]
assert next(case for case in cases if case["id"] == "mock_in_progress_then_result")["follow_request"]["op"] == "result"
exact = next(case for case in cases if case["id"] == "public_read_exact")
assert exact["request"] == {"op": "result", "command_id": fixture["command_id"], "active_revision": fixture["revision_a"]}
assert "authority_grant" not in exact["request"] and "target_path" not in exact["request"]
assert next(case for case in cases if case["id"] == "stop_during_outage")["expect"] == "isos_stop_visible_enabled"
assert next(case for case in cases if case["id"] == "log_no_owner_correlation")["expect"] == "show_command_id_open_unfiltered_current_run"
for path in [
    root / "docs/isosui/interfaces/browser-results.md",
    root / "docs_cn/isosui/interfaces/browser-results_cn.md",
    root / "docs/isosui/design-browser-results.md",
    root / "docs_cn/isosui/design-browser-results_cn.md",
]:
    assert path.is_file(), path
    source = path.read_text()
    for status in ["Completed", "Failed", "Cancelled", "Rejected", "InProgress", "OutcomeUnknown"]:
        assert status in source, f"{path}: missing {status}"
    assert "Stop" in source and "active_revision" in source, path
for path in [root / "docs/isosui/usage.md", root / "docs_cn/isosui/usage_cn.md"]:
    source = path.read_text()
    assert "Failed" in source and "InProgress" in source, path
    if "usage" in path.name:
        assert "correlation" in source or "关联" in source, path
print(f"R07 UI design matrix: {len(cases)} unique cases, typed send and guarded read, six owner statuses, EN/CN contracts and designs")
