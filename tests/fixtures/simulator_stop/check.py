#!/usr/bin/env python3
"""Check the frozen R06 EOS design inventory, without claiming runtime safety."""

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
data = json.loads((Path(__file__).parent / "cases.json").read_text())
assert data["schema_version"] == 1
assert (ROOT / data["contract"]).is_file()
cases = data["cases"]
assert len(cases) == 59
ids = [case["id"] for case in cases]
assert len(ids) == len(set(ids))
assert {case["boundary"] for case in cases} == {
    "readiness", "live_health", "dispatcher", "journal", "stop_lane", "child", "receipt", "lifecycle", "integration"
}
assert all(set(case) == {"id", "boundary", "expected"} for case in cases)
assert {
    "blocked_move_permit_preempted", "single_shared_robot_estop_shutdown",
    "one_frame_maps_all_journal_parts", "device_motion_stop_unavailable",
    "leader_exits_descendant_live", "descendant_escapes_group",
    "receipt_wrong_backend_identity", "receipt_duplicate_json_key",
    "receipt_missing_part", "receipt_group_not_drained",
    "short_eos_grace_late_receipt", "render_blocked_after_ready",
} <= set(ids)

receipt_fields = {
    "schema_version", "type", "run_id", "eos_pid", "active_revision",
    "backend_node", "backend_pid", "backend_pgid", "generation_id",
    "stop_epoch", "admission_closed", "parts", "backend_group_drained",
    "backend_reaped", "sockets_removed", "outcome", "reason_code",
}
for document in [
    ROOT / "docs/eos/interfaces/simulator-stop.md",
    ROOT / "docs_cn/eos/interfaces/simulator-stop_cn.md",
]:
    examples = re.findall(r"```json\n(.*?)\n```", document.read_text(), re.S)
    assert len(examples) == 2
    for example in examples:
        def unique_object(pairs):
            result = {}
            for key, value in pairs:
                assert key not in result
                result[key] = value
            return result

        receipt = json.loads(example, object_pairs_hook=unique_object)
        assert set(receipt) == receipt_fields
        assert receipt["backend_node"] == "eos/process/mujoco/sim"
        assert receipt["backend_pgid"] == receipt["backend_pid"]
        assert receipt["parts"] == sorted(receipt["parts"], key=lambda part: part["node_path"])
        assert all(set(part) == {"node_path", "kind", "outcome"} for part in receipt["parts"])
print(f"R06 EOS design inventory: {len(cases)} unique cases")
