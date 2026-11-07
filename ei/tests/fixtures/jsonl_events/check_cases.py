#!/usr/bin/env python3
"""Validate the frozen EGIA A03 role policy and representative boundary cases.

Contract evidence only: this does not exercise the future producer or IsOS collector.
"""

import json
import re
from pathlib import Path

ROOT = Path(__file__).parent
POLICY = json.loads((ROOT / "egia-role-policy.json").read_text())
CASES = json.loads((ROOT / "cases.json").read_text())
EXPECTED_CODES = {
  "egia_starting",
  "egia_ready",
  "egia_start_failed",
  "egia_stopping",
  "egia_stopped",
  "egia_discovery_lost",
  "egia_discovery_restored",
  "egia_mission_started",
  "egia_mission_terminal",
  "egia_safety_rejected",
  "egia_safety_latched",
  "egia_operation_submitted",
  "egia_operation_terminal",
  "egia_operation_unknown",
  "egia_recovery_required",
  "egia_events_suppressed",
  "egia_stage_transition",
  "egia_proposal_created",
  "egia_admission_decided",
  "egia_mission_restored",
  "egia_monitor_episode",
  "egia_safety_reset_requested",
  "egia_safety_reset_completed",
  "egia_safety_reset_refused",
}
EXPECTED_CASES = {
  "managed_run_valid",
  "managed_run_missing",
  "managed_run_wrong_prefix",
  "managed_run_overlong",
  "standalone_minted_run",
  "standalone_env_spoof",
  "starting_once",
  "ready_after_complete_discovery",
  "listener_only_not_ready",
  "startup_failure_terminal",
  "stop_confirmed_info",
  "stop_unconfirmed_error",
  "stop_confirmed_error_rejected",
  "stop_unconfirmed_info_rejected",
  "private_replay_continues_on_collector_loss",
  "managed_stderr_only_jsonl",
  "source_role_spoof",
  "run_mismatch",
  "raw_pid",
  "unknown_code",
  "wrong_level",
  "wrong_message",
  "cross_core_node",
  "node_member_pointer",
  "invalid_node_escape",
  "wrong_field",
  "wrong_field_type",
  "unsafe_reason",
  "raw_prompt",
  "raw_frame",
  "primitive_arguments",
  "secret_or_env",
  "raw_8064",
  "raw_8065",
  "stored_8192",
  "stored_8193",
  "mission_after_checkpoint",
  "safety_first_refusal",
  "latch_never_suppressed",
  "operation_unknown_no_replay",
  "terminal_requires_receipt",
  "recovery_needs_rediscovery",
  "high_rate_counted",
  "first_failure_not_suppressed",
  "stop_on_collector_outage",
  "stage_transition_correlated",
  "proposal_precedes_gate",
  "admission_after_durable_id",
  "mission_restored_pending",
  "monitor_fault_transition",
  "reset_one_shot_requested",
  "reset_refused_closed_reason",
  "distinct_safety_correlation_first",
}
assert POLICY["schema"] == "egia-jsonl-role-policy-a03-v2"
assert POLICY["source"] == POLICY["core"] == "egia"
assert set(POLICY["codes"]) == EXPECTED_CODES
assert CASES["schema"] == "egia-jsonl-events-a03-fixtures-v2"
assert {row["id"] for row in CASES["cases"]} == EXPECTED_CASES
assert len(CASES["cases"]) == len(EXPECTED_CASES)
assert all(set(row) == {"id", "expected"} for row in CASES["cases"])


def valid_run(value, *, managed):
  if not isinstance(value, str):
    return False
  if managed:
    return re.fullmatch(r"r[A-Za-z0-9_-]{1,63}", value) is not None
  return re.fullmatch(r"l[0-9a-f]{32}", value) is not None


assert valid_run("r7-cde", managed=True)
assert not valid_run(None, managed=True)
assert not valid_run("l" + "a" * 32, managed=True)
assert not valid_run("r" + "a" * 64, managed=True)
assert valid_run("l" + "a" * 32, managed=False)
assert not valid_run("r7-cde", managed=False)


def valid_typed(value, type_name):
  spec = POLICY["types"][type_name]
  if isinstance(spec, list):
    return type(value) is str and value in spec
  if spec["json"] == "integer":
    return type(value) is int and spec["min"] <= value <= spec["max"]
  return type(value) is str and re.fullmatch(spec["pattern"], value) is not None


def wire(event):
  return (json.dumps(event, ensure_ascii=False, separators=(",", ":")) + "\n").encode()


ADMITTED = {"/cores/egia/layers/common/groups/agent/nodes/main"}


def valid_event(event, *, expected_run="r7-cde"):
  required = {"timestamp", "level", "source", "core", "event_code", "run_id", "message"}
  allowed = required | {"node_path", "correlation_id", "fields"}
  if (
    not isinstance(event, dict)
    or not required <= event.keys()
    or not event.keys() <= allowed
  ):
    return False
  if event["source"] != "egia" or event["core"] != "egia":
    return False
  if not valid_run(event["run_id"], managed=True) or event["run_id"] != expected_run:
    return False
  if (
    not isinstance(event["timestamp"], str)
    or re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z", event["timestamp"])
    is None
  ):
    return False
  if event.get("correlation_id") is not None and not valid_typed(
    event["correlation_id"], "opaque_id"
  ):
    return False
  code = event["event_code"]
  if not isinstance(code, str) or code not in POLICY["codes"]:
    return False
  rule = POLICY["codes"][code]
  fields = event.get("fields", {})
  if not isinstance(fields, dict):
    return False
  expected_level = rule.get("level")
  if "level_by_outcome" in rule:
    expected_level = rule["level_by_outcome"].get(fields.get("outcome"))
  if "level_by_state" in rule:
    expected_level = rule["level_by_state"].get(fields.get("state"))
  if event["level"] != expected_level or event["message"] != rule["message"]:
    return False
  pointer = event.get("node_path")
  if pointer is not None and (rule["node_path"] == "null" or pointer not in ADMITTED):
    return False
  if len(fields) > 12 or len(wire(fields)) > 4096:
    return False
  required_fields = {k for k in rule["fields"] if not k.endswith("?")}
  allowed_fields = {k.rstrip("?") for k in rule["fields"]}
  if not required_fields <= fields.keys() or not fields.keys() <= allowed_fields:
    return False
  if any(
    not valid_typed(v, rule["fields"].get(k, rule["fields"].get(k + "?")))
    for k, v in fields.items()
  ):
    return False
  return len(wire(event)) <= 8064


ready = {
  "timestamp": "2026-11-01T10:00:02.000Z",
  "level": "info",
  "source": "egia",
  "core": "egia",
  "node_path": None,
  "event_code": "egia_ready",
  "run_id": "r7-cde",
  "correlation_id": None,
  "message": "EGIA ready",
  "fields": {"generation": 3},
}
assert valid_event(ready)
stopped = {
  **ready,
  "event_code": "egia_stopped",
  "message": "EGIA stopped",
  "fields": {"state": "stopped"},
}
assert valid_event(stopped)
assert valid_event(
  {**stopped, "level": "error", "fields": {"state": "stop_unconfirmed"}}
)
assert not valid_event({**stopped, "level": "error"})
assert not valid_event({**stopped, "fields": {"state": "stop_unconfirmed"}})
assert not valid_event({**ready, "source": "eos"})
assert not valid_event({**ready, "run_id": "r-other"})
assert not valid_event({**ready, "pid": 42})
assert not valid_event({**ready, "event_code": "egia_unlisted"})
assert not valid_event({**ready, "level": "warn"})
assert not valid_event({**ready, "message": "raw model prompt"})
assert not valid_event(
  {**ready, "node_path": "/cores/eos/layers/process/groups/mujoco/nodes/sim"}
)
assert not valid_event(
  {**ready, "node_path": "/cores/egia/layers/common/groups/agent/nodes/main/settings"}
)
assert not valid_event(
  {**ready, "node_path": "/cores/egia/layers/common/groups/agent/nodes/~2"}
)
assert not valid_event({**ready, "fields": {"generation": 3, "prompt": "secret"}})
assert not valid_event({**ready, "fields": {"generation": "3"}})
assert not valid_event({**ready, "fields": {"generation": 0}})
mission = {
  **ready,
  "event_code": "egia_mission_started",
  "message": "Mission started",
  "fields": {"mission_id": "550e8400-e29b-41d4-a716-446655440000"},
  "node_path": "/cores/egia/layers/common/groups/agent/nodes/main",
}
assert valid_event(mission)
assert not valid_event({**mission, "fields": {"mission_id": "user typed a prompt"}})
assert not valid_event({**mission, "fields": {"mission_id": "x", "arguments": "move"}})
unknown = {
  **ready,
  "event_code": "egia_operation_unknown",
  "level": "error",
  "message": "EOS operation outcome unknown",
  "fields": {"operation_id": "op-1", "reason_code": "timeout"},
}
assert valid_event(unknown)
assert not valid_event(
  {
    **unknown,
    "fields": {"operation_id": "op-1", "reason_code": "provider said key=abc"},
  }
)
assert valid_event(
  {
    **ready,
    "event_code": "egia_events_suppressed",
    "level": "warn",
    "message": "Repeated EGIA diagnostics suppressed",
    "fields": {"suppressed_count": 1},
  }
)

# The transport bound is independent of this closed catalog's short templates.
padding = {**ready, "message": ""}
base = len(wire(padding))
assert len(wire({**padding, "message": "x" * (8064 - base)})) == 8064
assert len(wire({**padding, "message": "x" * (8065 - base)})) == 8065
stored = {**ready, "pid": 4242, "message": ""}
stored_base = len(wire(stored))
assert len(wire({**stored, "message": "x" * (8192 - stored_base)})) == 8192
assert len(wire({**stored, "message": "x" * (8193 - stored_base)})) == 8193
print(
  f"EGIA A03 contract: {len(EXPECTED_CODES)} codes, {len(EXPECTED_CASES)} cases; byte bounds checked"
)

# A03 policy additions retain exact closed shapes.
for code, fields, level in [
  ("egia_stage_transition", {"kind": "resource"}, "info"),
  ("egia_proposal_created", {"operation_id": "op-a"}, "info"),
  ("egia_admission_decided", {"operation_id": "op-a", "outcome": "admitted"}, "info"),
  ("egia_mission_restored", {"mission_id": "mission-a", "outcome": "pending"}, "warn"),
  ("egia_monitor_episode", {"outcome": "fault"}, "warn"),
  ("egia_safety_reset_requested", {}, "warn"),
  ("egia_safety_reset_completed", {}, "info"),
  ("egia_safety_reset_refused", {"reason_code": "alias_coverage"}, "error"),
]:
  rule = POLICY["codes"][code]
  event = {
    **ready,
    "event_code": code,
    "message": rule["message"],
    "level": level,
    "fields": fields,
  }
  assert valid_event(event), code
  assert not valid_event({**event, "fields": {**fields, "prompt": "secret"}}), code
