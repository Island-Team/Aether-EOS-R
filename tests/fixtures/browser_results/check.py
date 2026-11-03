#!/usr/bin/env python3
"""Check the frozen R07 design matrix and mirrored contract markers."""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
fixture = json.loads((ROOT / "tests/fixtures/browser_results/cases.json").read_text())
assert fixture["contract"] == "eos-r07-browser-results"
assert fixture["status"] == "target-design"
cases = fixture["cases"]
assert len(cases) == 32
assert [case["id"] for case in cases] == [f"BR{i:02d}" for i in range(1, 33)]
assert all(case["boundary"] and case["expected"] in {"authorized", "opaque", "rejected", "unavailable", "visible"} for case in cases)
by_id = {case["id"]: case for case in cases}
for case_id in ("BR02", "BR07", "BR23", "BR27", "BR28", "BR29", "BR30"):
    assert isinstance(by_id[case_id].get("response"), dict), case_id
assert by_id["BR02"]["private_op"] == "primitive_result"
assert by_id["BR27"]["private_request"]["op"] == "result"
assert by_id["BR28"]["response"]["result"]["status"] == "Failed"
assert by_id["BR23"]["setup"]["journal_result_reason_utf8_bytes"] > 512
assert by_id["BR30"]["setup"]["private_response_utf8_bytes"] > 4096
assert by_id["BR31"]["public_request"]["header"]["payload_length_bytes"] == 84
assert by_id["BR31"]["private_payload_length_bytes"] == 80
required_typed = {"data_type_id", "data_type_version", "primitive_name", "primitive_version", "source_path", "target_path", "active_revision", "command_id", "sequence", "deadline_monotonic_ns", "payload_length_bytes", "codec"}
assert set(by_id["BR31"]["public_request"]["header"]) == required_typed
assert by_id["BR31"]["setup"]["binding_path"] == by_id["BR31"]["public_request"]["header"]["source_path"]
assert by_id["BR31"]["setup"]["target_path"] == by_id["BR31"]["public_request"]["header"]["target_path"]
assert by_id["BR32"]["public_request"]["header"]["payload_length_bytes"] > 16384
assert by_id["BR32"]["response"] == {"ok": False, "error": "invalid typed request"}
assert by_id["BR32"]["http_status"] == 400
assert by_id["BR32"]["setup"]["bridge_operator_grant"] == "missing"
assert by_id["BR32"]["setup"]["private_ipc_requests"] == 0
for name, markers in {
    "docs/eos/interfaces/control.md": ("R07 authorized browser result", "PrivateBridge"),
    "docs_cn/eos/interfaces/control_cn.md": ("R07 浏览器授权结果后续查询", "PrivateBridge"),
    "docs/eos/design/runtime/browser-results.md": ("R07 EOS and IsosUI scoped implementation is merged and qualified", "InProgress"),
    "docs_cn/eos/design/runtime/browser-results_cn.md": ("R07 的 EOS 与 IsosUI 限定范围实现已合并并通过资格验证", "InProgress"),
}.items():
    text = (ROOT / name).read_text()
    for marker in markers:
        assert marker in text, (name, marker)
print(f"R07 design matrix: {len(cases)} unique cases; EN/CN markers valid")
