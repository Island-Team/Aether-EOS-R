#!/usr/bin/env python3
"""Verify a retained Q01 G06/R03 case without accessing owner worktrees."""

from __future__ import annotations

import argparse
import hashlib
import json
import copy
from pathlib import Path

from q01_selection import canonical_bytes, sha256

EOS_PIN = "642166576a2cddfd97cd703e5bd9e53567745515"
ISOS_SOURCES = {
    "ac984bcf998e9252cc4e5c222620dca3ae6d3c37": "8304ab894ed97f92c5fd201ec233c9455bcf98fb",
    "00c1d7aa184de8b969e6e40472bf9b1fa55032ff": "90d7a87c5c98dacbdcca0c2eacbdcead3fb4774b",
}
EDGE_CASES = {
    "fixed-safety-qos": ("/cores/eos/layers/hardware/groups/base/nodes/drive/inputs/command/0/priority", "edge_qos_forbidden"),
    "actuator-bypass": ("/cores/eos/layers/hardware/groups/base/nodes/drive/inputs/command/0", "edge_actuator_bypass"),
}
OWNER_TESTS = {
    "identity": ("gateway_stop", "managed_movement_identity_and_owner_survive_restart_without_replay"),
    "deadline": ("grpc_submit_validation", "expired_deadline_and_transport_request_id_cannot_replace_operation_id"),
    "uncertain": ("grpc_journal_fault", "post_release_terminal_write_fault_is_unknown_and_never_replays_after_restart"),
    "stop-fence": ("gateway_stop", "managed_whole_robot_stop_preempts_blocked_release_and_fences_late_completion"),
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(directory: Path, filename: str):
    return json.loads((directory / filename).read_text())


def journal_body(directory: Path, command_id: str) -> dict:
    path = directory / "journal-probe" / (command_id.encode().hex() + ".rec")
    record = json.loads(path.read_text())
    body = record["body"]
    wire = json.dumps(body, separators=(",", ":"), ensure_ascii=False).encode()
    checksum = 0xcbf29ce484222325
    for byte in wire:
        checksum = ((checksum ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    assert record["checksum"] == checksum, (command_id, "journal checksum")
    assert body["command_id"] == body["result"]["command_id"] == command_id
    return body


def verify(directory: Path, selected_pins: dict | None = None,
           selection: dict | None = None) -> None:
    manifest = read(directory, "evidence.sha256.json")
    actual_files = {p.relative_to(directory).as_posix() for p in directory.rglob("*") if p.is_file() and p != directory / "evidence.sha256.json"}
    assert set(manifest) == actual_files, ("archive coverage", set(manifest) ^ actual_files)
    for name, expected in manifest.items():
        assert digest(directory / name) == expected, (name, "digest mismatch")
    pins = read(directory, "selected-pins.json")
    isos_pin = pins["isos_source"]
    eos_pin = selection['owners']['eos']['commit'] if selection else EOS_PIN
    isos_tree = (selection['owners']['isos']['tree'] if selection
                 else ISOS_SOURCES[isos_pin])
    isos_lock = (selection['owners']['isos']['locks']['Cargo.lock'] if selection
                 else '915c47050861921da13290aa1f574c3de54d70f97b02603d2536c88264d3eb5d')
    eos_lock = (selection['owners']['eos']['locks']['Cargo.lock'] if selection
                else 'f0ba1b13107592d0213919144198c83a0c3bc2cb40cd22c5679db0ab8ac4537f')
    crates_tree = (selection['owners']['eos']['subtrees']['crates'] if selection
                   else 'c22d50d18edb4b555f28d48773e1f21b10414be6')
    assert (selection is not None or isos_pin in ISOS_SOURCES)
    assert pins["eos_source"] == eos_pin
    if selection:
        assert isos_pin == selection['owners']['isos']['commit']
        assert read(directory, 'selection-sha256.json') == \
            {'profile':'selected-q03','sha256':sha256(canonical_bytes(selection))}
        assert digest(directory/'source-journal-probe.rs') == \
            selection['harness']['tests/q01_g06_r03_journal_probe.rs']
        assert digest(directory/'source-edge-fixture.json') == \
            selection['owners']['eos']['sources'][
                'tests/fixtures/edge_contract/valid_evidence.json']
    if selected_pins is not None:
        assert selected_pins["eos"] == eos_pin and selected_pins["isos"] == isos_pin
    builds = read(directory, "owner-builds.json")
    assert builds["isos"]["source_commit"] == isos_pin
    assert builds["isos"]["tree_oid"] == isos_tree
    assert builds["isos"]["lock_sha256"] == digest(directory / "isos-Cargo.lock") == isos_lock
    assert builds["isos"]["command"][1:] == ["build", "--locked", "--bin", "isos"]
    assert builds["isos"]["binary_sha256"] == pins["isos_binary_sha256"] == digest(directory / "bin/isos")
    assert builds["eos"]["source_commit"] == eos_pin
    assert builds["eos"]["crates_tree_oid"] == crates_tree
    assert builds["eos"]["lock_sha256"] == digest(directory / "eos-Cargo.lock") == eos_lock
    assert builds["eos"]["command"][1:] == ["build", "--locked", "--bin", "aether-profile"]
    assert builds["eos"]["profile_sha256"] == pins["eos_profile_sha256"] == digest(directory / "bin/aether-profile")
    assert builds["eos"]["test_overlay_sha256"] == digest(directory / "source-journal-probe.rs")
    assert "Finished" in (directory / "isos-selected-build.log").read_text()
    assert "Finished" in (directory / "eos-selected-build.log").read_text()
    for key in ("eos_profile_sha256", "isos_binary_sha256"):
        assert len(pins[key]) == 64 and all(ch in "0123456789abcdef" for ch in pins[key])
    assert pins["fixture_sha256"] == digest(directory / "source-edge-fixture.json")
    for key, expected in pins["test_sources_sha256"].items():
        assert expected == digest(directory / f"source-{key}.rs")
        if selection:
            source_path = {'gateway_stop':'crates/aether-dispatcher/tests/gateway_stop.rs',
                           'grpc_journal_fault':'crates/aether-runtime/tests/grpc_journal_fault.rs',
                           'grpc_submit_validation':'crates/aether-runtime/tests/grpc_submit_validation.rs'}[key]
            assert expected == selection['owners']['eos']['sources'][source_path]

    before = (directory / "saved-before.raw.json").read_bytes()
    after = (directory / "saved-after.raw.json").read_bytes()
    assert before == after, "rejected Apply changed saved bytes"
    revision = hashlib.sha256(before).hexdigest()
    fixture = read(directory, "source-edge-fixture.json")
    expected_graph = {key: value for key, value in fixture.items() if key in ("version", "robot", "cores")}
    expected_graph["cores"] = {"eos": expected_graph["cores"]["eos"]}
    baseline = json.loads(before)
    assert baseline == expected_graph
    assert read(directory, "initial-get.json")["body"]["data"]["revision"] == revision
    final = read(directory, "final-get.json")
    assert final["status"] == 200 and final["body"]["data"]["revision"] == revision
    assert final["body"]["data"]["graph"] == json.loads(before)
    status = read(directory, "final-status.json")
    assert status["status"] == 200 and status["body"]["ok"]
    assert status["body"]["data"]["state"] == "stopped"
    assert not status["body"]["data"].get("processes")
    initial_status = read(directory, "initial-status.json")
    assert initial_status["status"] == 200 and initial_status["body"]["ok"]
    stable = ("active_revision", "run_id", "run_id_state", "saved_revision", "state", "processes")
    baseline_status = initial_status["body"]["data"]
    assert all(baseline_status[key] == status["body"]["data"][key] for key in stable)
    cases = read(directory, "edge-cases.json")
    assert {case["name"] for case in cases} == set(EDGE_CASES)
    for case in cases:
        name = case["name"]
        pointer, code = EDGE_CASES[name]
        assert (case["path"], case["code"], case["revision"]) == (pointer, code, revision)
        candidate = read(directory, f"{name}-candidate.json")
        expected_candidate = copy.deepcopy(baseline)
        expected_edge = expected_candidate["cores"]["eos"]["layers"]["hardware"]["groups"]["base"]["nodes"]["drive"]["inputs"]["command"][0]
        if name == "fixed-safety-qos":
            expected_edge["priority"] = 0
        else:
            expected_edge["node"] = "eos/dispatcher/control/command"
            expected_edge["output"] = "base_command"
        assert candidate == expected_candidate, (name, "unrelated graph edit")
        edge = candidate["cores"]["eos"]["layers"]["hardware"]["groups"]["base"]["nodes"]["drive"]["inputs"]["command"][0]
        if name == "fixed-safety-qos":
            assert edge["priority"] == 0
        else:
            assert edge["node"] == "eos/dispatcher/control/command" and edge["output"] == "base_command"
        for operation, expected_status in (("validate", 200), ("apply", 422)):
            reply = read(directory, f"{name}-{operation}.json")
            assert reply["status"] == expected_status
            if operation == "validate":
                assert reply["body"]["data"]["valid"] is False
            else:
                assert reply["body"]["ok"] is False
            diagnostic = reply["body"]["data"]["diagnostics"][0]
            assert (diagnostic["path"], diagnostic["code"]) == (pointer, code)
        after_status = read(directory, f"{name}-status.json")
        assert after_status["status"] == 200 and after_status["body"]["ok"]
        assert all(after_status["body"]["data"][key] == baseline_status[key] for key in stable)

    for label, (source_key, test_name) in OWNER_TESTS.items():
        source = (directory / f"source-{source_key}.rs").read_text()
        log = (directory / f"{label}-cargo.log").read_text()
        assert f"fn {test_name}()" in source
        assert f"test {test_name} ... ok" in log and "test result: ok. 1 passed" in log
        assert "--exact" in log and "--test-threads=1" in log
    identity = (directory / "source-gateway_stop.rs").read_text()
    uncertain = (directory / "source-grpc_journal_fault.rs").read_text()
    assert "managed-one" in identity and "gateway_operation_authorized" in identity
    assert "assert_eq!(a.movement_calls(), 1)" in identity
    assert "post_release_terminal_write_fault_is_unknown_and_never_replays_after_restart" in uncertain
    assert "assert_eq!(base.movement_calls.load(Ordering::SeqCst), 1)" in uncertain
    overlay = (directory / "source-journal-probe.rs").read_text()
    assert "include!(\"gateway_stop.rs\")" in overlay
    assert "q01_archived_journal_identity_and_stop_fence" in overlay
    probe_log = (directory / "journal-probe-cargo.log").read_text()
    assert "test q01_archived_journal_identity_and_stop_fence ... ok" in probe_log
    assert "test result: ok. 1 passed" in probe_log
    summary = dict(line.split("=", 1) for line in (directory / "journal-probe/probe-result.txt").read_text().splitlines())
    assert summary == {"movement_calls": "1", "emergency_stop_calls": "1", "movement_inhibited": "true", "stop_status": "Completed"}
    recs = list((directory / "journal-probe").glob("*.rec"))
    assert {p.name for p in recs} == {name.encode().hex() + ".rec" for name in ("q01-uncertain", "q01-stop")}
    movement = journal_body(directory, "q01-uncertain")
    assert movement["phase"] == "terminal" and movement["result"]["status"] == "OutcomeUnknown"
    assert movement["operation"]["accepted"] is True
    assert movement["operation"]["identity"] == {
        "primitive": "base.velocity", "version": "1.0.0", "target": {"hardware": {"node_path": "eos/hardware/base/a"}},
        "kind": "movement", "admission": {"run_id": "run-a", "active_revision": "a" * 64,
            "authority_role": "egia_control", "ingress": "grpc",
            "binding_path": "cores/eos/layers/dispatcher/groups/primitive_base/nodes/velocity_a",
            "primitive": "base.velocity", "primitive_version": "1.0.0"},
    }
    stopped = journal_body(directory, "q01-stop")
    assert stopped["phase"] == "terminal" and stopped["result"]["status"] == "Completed"
    assert stopped["operation"]["accepted"] is True
    assert stopped["operation"]["identity"]["kind"] == "safety_estop"
    assert stopped["operation"]["identity"]["admission"] == {
        "run_id": "run-a", "active_revision": None, "authority_role": "egia_control", "ingress": "grpc",
        "binding_path": "", "primitive": "safety.estop", "primitive_version": "1.0.0"}
    assert stopped["operation"]["identity"]["target"] == {"robot": {"robot_id": "r"}}
    result = read(directory, "case-result.json")
    assert result == {"status": "passed", "scope": "selected-pin IsOS API plus EOS controlled-stub tests", "edge_cases": list(EDGE_CASES), "dispatcher_tests": list(OWNER_TESTS)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    args = parser.parse_args()
    verify(args.archive)
    print("Q01_G06_R03_ARCHIVE_PASS", args.archive)


if __name__ == "__main__":
    main()
