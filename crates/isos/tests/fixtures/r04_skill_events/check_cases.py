#!/usr/bin/env python3
"""Check R04 design cases against the frozen owner policy, not IsOS runtime."""

import copy
import json
import re
from pathlib import Path


cases = json.loads((Path(__file__).parent / "cases.json").read_text())
assert cases["schema"] == "isos-r04-skill-events-design-v1"
assert cases["eos_sha"] == "192fbfa6fbed1e9b6a20dd8dc95501625b02d081"
assert len(cases["failed_reasons"]) == len(set(cases["failed_reasons"]))
assert len(cases["child_cases"]) == 31
assert len(cases["cursor_cases"]) == 10

TOP = {"timestamp", "level", "source", "core", "node_path", "event_code", "run_id",
       "correlation_id", "message", "fields"}
EXEC_FIELDS = {"execution_id", "package_id", "package_version", "stage", "reason_code",
               "worker_reaped", "worker_exit_code"}
STOP_FIELDS = {"state", "reason_code"}
ID = re.compile(r"[A-Za-z0-9_-]{1,64}\Z")
ATOM = re.compile(r"[a-z0-9._-]+\Z")
PIN = re.compile(r"[a-z0-9-]+/[a-z0-9-]+\Z")
VERSION = re.compile(r"[0-9][A-Za-z0-9.+-]*\Z")
DEFAULT_PIN = ("island/fixture-skills", "0.1.0+Build.1")


def valid(event, role="eos", selected_pin=DEFAULT_PIN):
    if role != "eos" or set(event) != TOP:
        return False
    if event["source"] != "eos" or event["core"] != "eos" or event["node_path"] is not None:
        return False
    if event["run_id"] != "r04-run" or "pid" in event:
        return False
    if not re.fullmatch(r"2026-11-02T\d\d:\d\d:\d\d\.\d{3}Z", event["timestamp"]):
        return False
    correlation = event["correlation_id"]
    if correlation is not None and not (isinstance(correlation, str) and ID.fullmatch(correlation)):
        return False
    if len((json.dumps(event, ensure_ascii=False, separators=(",", ":")) + "\n").encode()) > 8064:
        return False
    fields = event["fields"]
    if not isinstance(fields, dict):
        return False
    if event["event_code"] == "eos_skill_stop_unconfirmed":
        return (event["level"] == "error" and event["message"] == "EOS native skill stop unconfirmed"
                and set(fields) == STOP_FIELDS and fields == {"state": "failed", "reason_code": "skill_stop_unconfirmed"})
    if event["event_code"] != "eos_skill_execution" or set(fields) != EXEC_FIELDS:
        return False
    if event["message"] != "EOS native skill execution lifecycle":
        return False
    if not isinstance(correlation, str) or not re.fullmatch(r"skill_[0-9a-f]{32}", correlation):
        return False
    execution_id = fields["execution_id"]
    if not isinstance(execution_id, str) or not execution_id.startswith("ex-") or len(execution_id) > 80 or not ATOM.fullmatch(execution_id):
        return False
    package_id = fields["package_id"]
    version = fields["package_version"]
    if not isinstance(package_id, str) or len(package_id) > 96 or not PIN.fullmatch(package_id):
        return False
    if not isinstance(version, str) or len(version) > 64 or not VERSION.fullmatch(version) or len(version.split(".")) < 3:
        return False
    if selected_pin is None or (package_id, version) != selected_pin:
        return False
    stage = fields["stage"]
    reason = fields["reason_code"]
    if not isinstance(reason, str) or len(reason) > 128 or not reason.startswith("worker.") or not ATOM.fullmatch(reason):
        return False
    expected = {"queued": ("info", "worker.queued"), "running": ("info", "worker.running"),
                "completed": ("info", "worker.succeeded")}
    if stage in expected:
        if (event["level"], reason) != expected[stage]:
            return False
    elif stage != "failed" or event["level"] != "error" or reason not in cases["failed_reasons"]:
        return False
    reaped = fields["worker_reaped"]
    exit_code = fields["worker_exit_code"]
    if reaped is not None and not isinstance(reaped, bool):
        return False
    if exit_code is not None and (type(exit_code) is not int or not -(2**31) <= exit_code < 2**31):
        return False
    if stage in {"queued", "running"}:
        return reaped is None and exit_code is None
    if stage == "completed":
        return reaped is True
    return True


seen = set()
for case in cases["child_cases"]:
    assert case["id"] not in seen
    seen.add(case["id"])
    event = copy.deepcopy(cases["base_execution"] if case["base"] == "execution" else cases["base_stop"])
    for key in case.get("remove", []):
        container, field = key.split(".")
        del event[container][field]
    for key, value in case["set"].items():
        if key.startswith("fields."):
            event["fields"][key.removeprefix("fields.")] = value
        else:
            event[key] = value
    pin = case.get("selected_pin")
    selected_pin = (pin["id"], pin["version"]) if pin else DEFAULT_PIN
    if case.get("selected") is False:
        selected_pin = None
    assert valid(event, case.get("role", "eos"), selected_pin) == (case["expect"] == "accept"), case["id"]

cursor_ids = [case["id"] for case in cases["cursor_cases"]]
assert len(cursor_ids) == len(set(cursor_ids))
assert cursor_ids == ["pending_nonready", "first_failure_anchor", "stop_failure_anchor",
                      "filtered_anchor", "rejected_not_anchor", "later_same_code_line",
                      "retained_rotation", "expired_rotation", "write_gap", "stopped_last_run"]
for case in cases["cursor_cases"]:
    assert case["anchor"] in {"pending", "eos_skill_execution", "eos_skill_stop_unconfirmed"}
    assert set(case["diagnostics"]) <= {"log_cursor_expired", "log_collector_gap"}
assert cases["cursor_cases"][6]["rotated"] and not cases["cursor_cases"][6]["diagnostics"]
assert cases["cursor_cases"][7]["rotated"] and cases["cursor_cases"][7]["diagnostics"] == ["log_cursor_expired"]
print(f"R04 design: {len(seen)} child cases and {len(cursor_ids)} cursor scenarios passed")
