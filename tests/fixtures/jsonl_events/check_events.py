#!/usr/bin/env python3
"""Check P06 design examples, bilingual parity and frozen cases without runtime code."""

import copy
import json
import re
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = Path(__file__).parent
TOP = {
    "timestamp", "level", "source", "core", "node_path", "event_code",
    "run_id", "correlation_id", "message", "fields",
}
FIELD_KEYS = {
    "state", "stage", "reason_code", "package_id", "package_version",
    "release_sha256", "payload_sha256", "node_count", "suppressed_count",
}
ID = re.compile(r"[A-Za-z0-9_-]{1,64}\Z")
CODE = re.compile(r"[a-z][a-z0-9_]{0,63}\Z")
KEY = re.compile(r"[A-Za-z0-9_-]{1,48}\Z")
DIGEST = re.compile(r"[0-9a-f]{64}\Z")
PATH = re.compile(r"/cores/eos/layers/(?:[^~/\x00-\x1f]|~[01])+/groups/(?:[^~/\x00-\x1f]|~[01])+/nodes/(?:[^~/\x00-\x1f]|~[01])+\Z")
TIME = re.compile(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z\Z")


def valid(event: object) -> bool:
    if not isinstance(event, dict) or set(event) != TOP:
        return False
    if len(json.dumps(event, ensure_ascii=False, separators=(",", ":")).encode()) + 1 > 8064:
        return False
    if not isinstance(event["timestamp"], str) or not TIME.fullmatch(event["timestamp"]):
        return False
    try:
        stamp = datetime.fromisoformat(event["timestamp"].replace("Z", "+00:00"))
    except ValueError:
        return False
    if stamp.tzinfo != timezone.utc:
        return False
    if event["level"] not in ("trace", "debug", "info", "warn", "error"):
        return False
    if event["source"] not in ("eos", "bridge", "config") or event["core"] != "eos":
        return False
    node = event["node_path"]
    if node is not None and (not isinstance(node, str) or len(node.encode()) > 512 or not PATH.fullmatch(node)):
        return False
    if not isinstance(event["event_code"], str) or not CODE.fullmatch(event["event_code"]):
        return False
    for prefix, source in (("eos_", "eos"), ("bridge_", "bridge"), ("config_", "config")):
        if event["event_code"].startswith(prefix) and event["source"] != source:
            return False
    if not isinstance(event["run_id"], str) or not ID.fullmatch(event["run_id"]):
        return False
    correlation = event["correlation_id"]
    if correlation is not None and (not isinstance(correlation, str) or not ID.fullmatch(correlation)):
        return False
    message = event["message"]
    if not isinstance(message, str) or not message or len(message.encode()) > 8192 or any(ord(ch) < 32 for ch in message):
        return False
    fields = event["fields"]
    if not isinstance(fields, dict) or len(fields) > 12 or not set(fields) <= FIELD_KEYS:
        return False
    if len(json.dumps(fields, ensure_ascii=False, separators=(",", ":")).encode()) > 4096:
        return False
    for key, value in fields.items():
        if not KEY.fullmatch(key):
            return False
        if isinstance(value, str):
            if len(value.encode()) > 256 or any(ord(ch) < 32 for ch in value):
                return False
        elif isinstance(value, bool) or value is None:
            pass
        elif isinstance(value, int):
            if not (0 <= value <= 2**32 - 1):
                return False
        else:
            return False
        if key in ("node_count", "suppressed_count") and (type(value) is not int):
            return False
        if key not in ("node_count", "suppressed_count") and not isinstance(value, str):
            return False
    enums = {
        "state": {"starting", "ready", "failed", "stopping", "stopped"},
        "stage": {"options", "profile", "registry", "adapter", "journal", "backend", "listener", "stop", "sink"},
        "reason_code": {"invalid_run_id", "profile_invalid", "registry_invalid", "adapter_unavailable", "journal_unavailable", "backend_start_failed", "child_exited", "listener_bind_failed", "stop_unconfirmed", "event_sink_failed", "other_failure"},
    }
    if any(key in fields and fields[key] not in values for key, values in enums.items()):
        return False
    if "package_id" in fields and (not re.fullmatch(r"[a-z0-9][a-z0-9._/-]{0,95}", fields["package_id"]) or ".." in fields["package_id"].split("/")):
        return False
    if "package_version" in fields and not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", fields["package_version"]):
        return False
    if ("package_id" in fields) != ("package_version" in fields):
        return False
    if ("release_sha256" in fields) != ("payload_sha256" in fields):
        return False
    for key in ("release_sha256", "payload_sha256"):
        if key in fields and not DIGEST.fullmatch(str(fields[key])):
            return False
    if event["event_code"] == "eos_package_admitted":
        if not {"package_id", "package_version", "release_sha256", "payload_sha256"} <= set(fields):
            return False
    return True


def markdown_examples(path: Path) -> list[str]:
    return re.findall(r"```json\n([^`]+)\n```", path.read_text())


def check_links(path: Path) -> None:
    for target in re.findall(r"\[[^]]+\]\(([^)]+)\)", path.read_text()):
        if target.startswith(("http://", "https://", "#")):
            continue
        assert (path.parent / target.split("#", 1)[0]).exists(), (path, target)


def main() -> None:
    cases = json.loads((FIXTURE / "cases.json").read_text())
    assert cases["schema_version"] == 1 and cases["slice"] == "P06-eos"
    example_lines = (ROOT / cases["examples"]).read_text().splitlines()
    examples = [json.loads(line) for line in example_lines]
    assert all(len(line.encode()) + 1 <= 8064 for line in example_lines)
    assert len(examples) == 3 and all(valid(event) for event in examples)
    assert [event["event_code"] for event in examples] == [
        "eos_package_admitted", "eos_ready", "eos_backend_lost",
    ]
    assert [event["timestamp"] for event in examples] == sorted(event["timestamp"] for event in examples)
    assert examples[2]["fields"]["reason_code"] == "child_exited"
    english = ROOT / "docs/eos/interfaces/events.md"
    chinese = ROOT / "docs_cn/eos/interfaces/events_cn.md"
    assert markdown_examples(english) == example_lines == markdown_examples(chinese)
    for path in (
        english, chinese,
        ROOT / "docs/eos/design/runtime/events.md",
        ROOT / "docs_cn/eos/design/runtime/events_cn.md",
        ROOT / "docs/eos/usage/README.md",
        ROOT / "docs_cn/eos/usage/README_cn.md",
        ROOT / "docs/eos/interfaces/README.md",
        ROOT / "docs_cn/eos/interfaces/README_cn.md",
        ROOT / "docs/eos/design/runtime/README.md",
        ROOT / "docs_cn/eos/design/runtime/README_cn.md",
        FIXTURE / "README.md", FIXTURE / "README_cn.md",
    ):
        check_links(path)
    for token in ("run_id", "correlation_id", "eos_package_admitted", "eos_backend_lost", "eos_events_suppressed", "--backend-release-sha256", "--backend-payload-sha256"):
        assert token in english.read_text() and token in chinese.read_text(), token
    case_list = cases["cases"]
    ids = [case["id"] for case in case_list]
    assert len(ids) == 30 and len(ids) == len(set(ids))
    assert {case["boundary"] for case in case_list} == {"handoff", "lifecycle", "package", "stop", "privacy", "suppression", "sink"}
    by_id = {case["id"]: case for case in case_list}
    assert by_id["managed_package_provenance"]["expected"]["release_sha256"] == examples[0]["fields"]["release_sha256"]
    assert by_id["managed_package_provenance"]["expected"]["payload_sha256"] == examples[0]["fields"]["payload_sha256"]
    assert by_id["prelaunch_receipt_change"]["expected"]["eos_event_count"] == 0
    assert by_id["forged_child_identity"]["expected"]["no_forged_aggregation"] is True
    assert by_id["correlated_operation"]["expected"]["raw_arguments_absent"] is True
    assert by_id["terminal_bypasses_suppression"]["expected"]["emissions"] == 1
    assert by_id["safe_stop_before_reap"]["expected"]["stop_before_backend_reap"] is True
    assert by_id["redact_untrusted_inputs"]["expected"]["forbidden_tokens"]
    assert by_id["saved_graph_pointer"]["expected"]["node_path"] == examples[0]["node_path"]
    assert by_id["invalid_graph_segment"]["expected"] == {
        "result": "reject_before_node_admission",
        "node_specific_event_count": 0, "reason_code": "profile_invalid",
    }
    assert by_id["forged_raw_pid"]["expected"]["observed_pid_only"] is True
    assert by_id["normalized_line_reserve"]["expected"] == {
        "result": "reject_or_bound_before_emit", "max_raw_line_bytes": 8064,
        "max_stored_line_bytes": 8192, "pid_reserved_bytes": 128,
    }
    assert by_id["private_mujoco_capture"]["expected"]["shared_event"] is False
    assert by_id["private_mujoco_capture"]["expected"]["log_dir"].endswith("/owner/eos/")
    mutations = [
        ("run_id", "bad/id"), ("timestamp", "2026-11-01T08:00:00Z"),
        ("level", "verbose"), ("core", "egia"), ("source", "bridge"),
        ("node_path", "/home/private"),
        ("node_path", "eos/process/mujoco/sim"),
        ("node_path", "/cores/eos/layers/process/groups/mujoco/nodes/bad~2escape"),
        ("message", "bad\nline"), ("fields", {"raw_prompt": "private"}),
        ("fields", {"nested": {"secret": "x"}}),
    ]
    for key, value in mutations:
        bad = copy.deepcopy(examples[0])
        bad[key] = value
        assert not valid(bad), (key, value)
    bad = copy.deepcopy(examples[0])
    bad["unknown"] = "x"
    assert not valid(bad)
    bad = copy.deepcopy(examples[0])
    bad["pid"] = 1234
    assert not valid(bad)
    print(f"P06 EOS fixtures valid: {len(examples)} bilingual examples, {len(case_list)} unique cases, {len(mutations)+2} malformed envelopes")


if __name__ == "__main__":
    main()
