#!/usr/bin/env python3
"""Check R01 target fixtures without claiming runtime implementation."""

import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parent
spec = json.loads((ROOT / "cases.json").read_text())
assert spec["schema_version"] == 1 and spec["owner"] == "eos" and spec["target"] == "R01"
assert spec["live_route"] == {
    "source_type": "eos.hardware.camera@v0.1",
    "source_port": "frame",
    "target_type": "eos.interface.camera_gate@v0.1",
    "target_port": "frame",
    "data_type": "camera_frame",
    "data_class": "image",
    "transport": "in_process",
}
assert spec["limits"] == {
    "route_count": 256,
    "global_queued_bytes": 64 * 1024 * 1024,
    "fifo_records_per_route": 64,
    "fifo_bytes_per_route": 1024 * 1024,
    "deficit_credit_max": 1024,
    "cost_unit_bytes": 64 * 1024,
    "attempts_per_slice": 64,
    "route_visits_per_slice": 64,
}
cases = spec["cases"]
by_id = {case["id"]: case for case in cases}
assert len(by_id) == len(cases) == 28
assert {case["kind"] for case in cases} == {"admission", "mapping", "clock", "queue", "fairness", "failure", "stop", "status", "delivery"}
for case in cases:
    assert set(case) == {"id", "kind", "input", "expect"}
    assert case["input"] and case["expect"]
for priority, weight in [(-20, 40), (0, 20), (19, 1)]:
    assert 20 - priority == weight
assert by_id["exact_camera_gate"]["expect"]["routed"] is True
assert all(by_id[case_id]["expect"]["routed"] is False for case_id in ["similar_legacy_pair", "contributed_port_no_implicit_route", "fixed_actuator_unthrottled", "fixed_result_health_stop_unthrottled"])
assert by_id["latest_replacement"]["expect"]["gap_count"] == 2
assert by_id["fifo_saturation"]["expect"]["retained_count"] == spec["limits"]["fifo_records_per_route"]
assert by_id["fifo_oversized"]["input"]["record_bytes"] > spec["limits"]["fifo_bytes_per_route"]
assert by_id["large_low_weight_progress"]["input"]["visits"] == spec["limits"]["deficit_credit_max"]
assert by_id["shared_camera_independent_gates"]["expect"]["capped_gate_does_not_see_undelivered_frame"] is True
assert by_id["legacy_preview_is_not_r01"]["expect"]["r01_route"] is False
assert by_id["stop_with_backlog"]["expect"]["bounded_join"] is True
assert by_id["status_has_no_payload_or_health"]["expect"]["implies_health"] is False
assert (ROOT / by_id["exact_camera_gate"]["input"]["fixture"]).is_file()


def capped_token_ready(rate_hz: float, elapsed_ns: int) -> bool:
    """Model only the frozen arithmetic, not the runtime scheduler."""
    assert math.isfinite(rate_hz) and rate_hz > 0
    if elapsed_ns <= 0:
        return False
    if rate_hz > 1e9:
        return True
    product = float(elapsed_ns) * rate_hz
    assert math.isfinite(product)
    return product >= 1e9


for case_id in ["max_finite_frequency", "min_subnormal_frequency"]:
    case = by_id[case_id]
    rate = case["input"]["frequency_hz"]
    observed = [capped_token_ready(rate, tick) for tick in case["input"]["elapsed_ns"]]
    assert observed == case["expect"]["token_ready"]
assert capped_token_ready(1e9, 1)
assert not capped_token_ready(1e9, 0)
assert not capped_token_ready(1, -1)

def model_first_visits(priorities: list[int]) -> tuple[dict[int, int], list[int]]:
    """Reference cursor model for the frozen slice budgets and always-ready queues."""
    count = len(priorities)
    credit = [0] * count
    delivered = [0] * count
    first_seen = {}
    cursor = 0
    for slice_number in range(1, count + 1):
        visits = attempts = 0
        while visits < 64 and attempts < 64 and len(first_seen) < count:
            route = cursor
            cursor = (cursor + 1) % count
            visits += 1
            first_seen.setdefault(route, slice_number)
            credit[route] = min(1024, credit[route] + 20 - priorities[route])
            served = min(credit[route], 64 - attempts)
            delivered[route] += served
            credit[route] -= served
            attempts += served
        if len(first_seen) == count:
            return first_seen, delivered
    raise AssertionError("persistent cursor starved a ready route")


for case_id in ["ready_65_cursor", "ready_256_cursor"]:
    case = by_id[case_id]
    count = case["input"]["ready_routes"]
    visit_budget = case["input"]["route_visits_per_slice"]
    attempt_budget = case["input"]["attempts_per_slice"]
    assert visit_budget == attempt_budget == spec["limits"]["attempts_per_slice"]
    assert case["input"]["priority"] == 19
    assert case["input"]["payload_bytes"] == spec["limits"]["cost_unit_bytes"]
    visits, deliveries = model_first_visits([case["input"]["priority"]] * count)
    slices = max(visits.values())
    assert slices == case["expect"]["all_visited_within_slices"]
    assert visits[count - 1] == case["expect"]["last_route_first_visited_slice"]
    assert all(delivered > 0 for delivered in deliveries)
    assert case["expect"]["cursor_persists"] is True

mixed = by_id["ready_256_mixed_weights"]
assert mixed["input"]["ready_routes"] == spec["limits"]["route_count"]
assert mixed["input"]["priorities"] == [-20, 19]
assert mixed["input"]["priority_pattern"] == "alternating"
mixed_visits, mixed_deliveries = model_first_visits([mixed["input"]["priorities"][index % 2] for index in range(256)])
assert max(mixed_visits.values()) <= mixed["expect"]["all_visited_within_slices_at_most"]
assert all(delivered > 0 for delivered in mixed_deliveries)
assert mixed["expect"]["all_visited_within_slices_at_most"] == spec["limits"]["route_count"]
assert mixed["expect"]["low_priority_progress"] is True

cross_core = by_id["cross_core_designonly_not_in_eos_status"]
assert cross_core["input"]["source_type"] == "egia.common.guard@v1.0"
assert cross_core["input"]["target_type"] == "eos.dispatcher.control@v1.0"
assert cross_core["input"]["source_output"] == cross_core["input"]["target_input"] == "request"
assert cross_core["input"]["source_node"].startswith("egia/")
assert cross_core["input"]["target_node"].startswith("eos/")
assert cross_core["expect"]["validated_by_profile"] is True
assert cross_core["expect"]["eos_route_status_row"] is False
assert by_id["route_status_after_stop"]["expect"]["stopped_row"] is False

accepted = by_id["inflight_accept_before_cancel"]
assert accepted["input"]["accept_after_ms"] < accepted["input"]["cancel_after_ms"]
assert accepted["expect"]["delivered_count_delta"] == 1
assert accepted["expect"]["cancelled_count_delta"] == 0
cancelled = by_id["inflight_deadline_cancel"]
assert cancelled["input"]["acceptance_deadline_ms"] == 100
assert cancelled["input"]["join_deadline_ms"] == 1000
assert cancelled["expect"]["cancelled_count_delta"] == cancelled["input"]["queued"] + cancelled["input"]["in_flight"]
assert cancelled["expect"]["gap_count_delta"] == cancelled["expect"]["cancelled_count_delta"]
assert cancelled["expect"]["late_publish"] is False
assert all(
    by_id[case_id]["expect"]["emergency_stop_before_evidence_join"]
    and by_id[case_id]["expect"]["emergency_stop_before_queue_drain"]
    for case_id in ["stop_with_backlog", "inflight_accept_before_cancel", "inflight_deadline_cancel"]
)
print(f"R01 design fixtures: {len(cases)} cases valid")
