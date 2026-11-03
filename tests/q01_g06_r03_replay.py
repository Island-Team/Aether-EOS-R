#!/usr/bin/env python3
"""Selected-pin IsOS edge and EOS controlled-dispatcher negative replay.

This case uses a disposable IsOS home and the exact owner source checkouts. It
retains raw HTTP responses and raw Cargo test output for offline inspection.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import shutil
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from q01_selection import check_harness, load_selection, verify_checkout

EOS_PIN = "642166576a2cddfd97cd703e5bd9e53567745515"
ISOS_PIN = "00c1d7aa184de8b969e6e40472bf9b1fa55032ff"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def head(path: Path) -> str:
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=path, text=True).strip()


def oid(path: Path, revision: str) -> str:
    return subprocess.check_output(["git", "rev-parse", revision], cwd=path, text=True).strip()


def build(command: list[str], root: Path, log: Path) -> None:
    run = subprocess.run(command, cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
    log.write_text("$ " + " ".join(command) + "\n" + run.stdout)
    assert run.returncode == 0 and "Finished" in run.stdout, (command, run.returncode, run.stdout[-3000:])


def save(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def api(port_number: int, route: str, method: str = "GET", value: object = None) -> dict:
    request = Request(
        f"http://127.0.0.1:{port_number}/api/isos/v1/{route}",
        method=method,
        data=None if value is None else json.dumps(value).encode(),
        headers={"Content-Type": "application/json", "Origin": f"http://127.0.0.1:{port_number}"},
    )
    try:
        response = urlopen(request, timeout=30)
    except HTTPError as error:
        response = error
    with response:
        return {"status": response.status, "body": json.loads(response.read())}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--eos-root", type=Path, required=True)
    parser.add_argument("--isos-root", type=Path, required=True)
    parser.add_argument("--eos-bin-dir", type=Path, required=True)
    parser.add_argument("--isos-bin", type=Path, required=True)
    parser.add_argument("--ui-dist", type=Path, required=True)
    parser.add_argument("--cargo", type=Path, default=Path.home() / ".cargo/bin/cargo")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--selection", type=Path)
    parser.add_argument("--expect-selection-sha256")
    args = parser.parse_args()
    eos, isos = args.eos_root.resolve(), args.isos_root.resolve()
    assert bool(args.selection) == bool(args.expect_selection_sha256)
    selection = (load_selection(args.selection, args.expect_selection_sha256)
                 if args.selection else None)
    eos_pin = selection['owners']['eos']['commit'] if selection else EOS_PIN
    isos_pin = selection['owners']['isos']['commit'] if selection else ISOS_PIN
    assert head(eos) == eos_pin and head(isos) == isos_pin
    if selection:
        verify_checkout(selection, 'eos', eos)
        verify_checkout(selection, 'isos', isos)
        check_harness(selection, Path(__file__).parent)
    assert not subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"], cwd=eos)
    assert not subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"], cwd=isos)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / "bin").mkdir()
    shutil.copy2(isos / "Cargo.lock", out / "isos-Cargo.lock")
    shutil.copy2(eos / "Cargo.lock", out / "eos-Cargo.lock")
    isos_build = [str(args.cargo), "build", "--locked", "--bin", "isos"]
    eos_build = [str(args.cargo), "build", "--locked", "--bin", "aether-profile"]
    build(isos_build, isos, out / "isos-selected-build.log")
    build(eos_build, eos, out / "eos-selected-build.log")
    built_isos = isos / "target/debug/isos"
    built_profile = eos / "target/debug/aether-profile"
    assert built_isos.samefile(args.isos_bin) and built_profile.samefile(args.eos_bin_dir / "aether-profile")
    shutil.copy2(built_isos, out / "bin/isos")
    shutil.copy2(built_profile, out / "bin/aether-profile")
    save(out / "owner-builds.json", {
        "isos": {"source_commit": isos_pin, "tree_oid": oid(isos, "HEAD^{tree}"),
                 "lock_sha256": sha(out / "isos-Cargo.lock"), "command": isos_build,
                 "binary_sha256": sha(out / "bin/isos")},
        "eos": {"source_commit": eos_pin, "crates_tree_oid": oid(eos, "HEAD:crates"),
                "lock_sha256": sha(out / "eos-Cargo.lock"), "command": eos_build,
                "profile_sha256": sha(out / "bin/aether-profile"),
                "test_overlay_sha256": sha(Path(__file__).with_name("q01_g06_r03_journal_probe.rs"))},
    })
    fixture = eos / "tests/fixtures/edge_contract/valid_evidence.json"
    test_sources = {
        "gateway_stop": eos / "crates/aether-dispatcher/tests/gateway_stop.rs",
        "grpc_journal_fault": eos / "crates/aether-runtime/tests/grpc_journal_fault.rs",
        "grpc_submit_validation": eos / "crates/aether-runtime/tests/grpc_submit_validation.rs",
    }
    save(out / "selected-pins.json", {
        "eos_source": eos_pin, "isos_source": isos_pin,
        "eos_profile_sha256": sha(out / "bin/aether-profile"),
        "isos_binary_sha256": sha(out / "bin/isos"),
        "fixture_sha256": sha(fixture),
        "test_sources_sha256": {key: sha(value) for key, value in test_sources.items()},
        "environment": {"kind": "disposable IsOS API and controlled EOS Rust stub tests", "os": os.uname().sysname, "arch": os.uname().machine},
    })
    if selection:
        assert sha(fixture) == selection['owners']['eos']['sources'][
            'tests/fixtures/edge_contract/valid_evidence.json']
        assert sha(Path(__file__).with_name('q01_g06_r03_journal_probe.rs')) == \
            selection['harness']['tests/q01_g06_r03_journal_probe.rs']
        for source_path in test_sources.values():
            relative = source_path.relative_to(eos).as_posix()
            assert sha(source_path) == selection['owners']['eos']['sources'][relative]
        save(out / 'selection-sha256.json',
             {'profile': 'selected-q03', 'sha256': args.expect_selection_sha256})
    shutil.copy2(fixture, out / "source-edge-fixture.json")
    for key, path in test_sources.items():
        shutil.copy2(path, out / f"source-{key}.rs")
    overlay = Path(__file__).with_name("q01_g06_r03_journal_probe.rs")
    shutil.copy2(overlay, out / "source-journal-probe.rs")
    source = json.loads(fixture.read_text())
    graph = {key: value for key, value in source.items() if key in ("version", "robot", "cores")}
    graph["cores"] = {"eos": graph["cores"]["eos"]}
    # The fixture has a valid EOS base binding. Its separate test mutations
    # target one port each, so diagnostics cannot be attributed to another edge.
    with tempfile.TemporaryDirectory(prefix="q01-g06-r03-") as root_name:
        root = Path(root_name)
        home = root / "home"
        graph_path = home / "config/deployments/default.nodes.json"
        graph_path.parent.mkdir(parents=True)
        graph_path.write_text(json.dumps(graph, indent=2) + "\n")
        (out / "saved-before.raw.json").write_bytes(graph_path.read_bytes())
        listen = port()
        env = dict(os.environ, ISOS_HOME=str(home), ISOS_HTTP_PORT=str(listen),
                   ISOS_AETHER_BIN_DIR=str(out / "bin"), ISOSUI_DIST_DIR=str(args.ui_dist))
        with (out / "isos-serve.log").open("w") as log:
            server = subprocess.Popen([str(out / "bin/isos"), "serve"], env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                for _ in range(100):
                    if server.poll() is not None:
                        raise RuntimeError("IsOS serve exited before readiness")
                    try:
                        initial = api(listen, "node-graph")
                        break
                    except OSError:
                        time.sleep(.05)
                else:
                    raise RuntimeError("IsOS serve did not become ready")
                save(out / "initial-get.json", initial)
                assert initial["status"] == 200 and initial["body"]["ok"]
                revision = sha(graph_path)
                assert initial["body"]["data"]["revision"] == revision
                initial_status = api(listen, "process-status")
                save(out / "initial-status.json", initial_status)
                cases = []
                for name, pointer, code, mutate in [
                    ("fixed-safety-qos", "/cores/eos/layers/hardware/groups/base/nodes/drive/inputs/command/0/priority", "edge_qos_forbidden", "qos"),
                    ("actuator-bypass", "/cores/eos/layers/hardware/groups/base/nodes/drive/inputs/command/0", "edge_actuator_bypass", "bypass"),
                ]:
                    candidate = copy.deepcopy(graph)
                    edge = candidate["cores"]["eos"]["layers"]["hardware"]["groups"]["base"]["nodes"]["drive"]["inputs"]["command"][0]
                    if mutate == "qos":
                        edge["priority"] = 0
                    else:
                        edge["node"] = "eos/dispatcher/control/command"
                        edge["output"] = "base_command"
                    save(out / f"{name}-candidate.json", candidate)
                    validate = api(listen, "node-graph/validate", "POST", {"schema_version": 1, "graph": candidate})
                    apply = api(listen, "node-graph", "PUT", {"schema_version": 1, "revision": revision, "graph": candidate})
                    save(out / f"{name}-validate.json", validate)
                    save(out / f"{name}-apply.json", apply)
                    save(out / f"{name}-status.json", api(listen, "process-status"))
                    diagnostics = validate["body"]["data"]["diagnostics"]
                    assert validate["status"] == 200 and validate["body"]["data"]["valid"] is False
                    assert (diagnostics[0]["path"], diagnostics[0]["code"]) == (pointer, code), diagnostics
                    assert apply["status"] == 422 and not apply["body"]["ok"]
                    assert (apply["body"]["data"]["diagnostics"][0]["path"], apply["body"]["data"]["diagnostics"][0]["code"]) == (pointer, code)
                    assert sha(graph_path) == revision
                    cases.append({"name": name, "path": pointer, "code": code, "revision": revision})
                save(out / "edge-cases.json", cases)
                save(out / "final-get.json", api(listen, "node-graph"))
                save(out / "final-status.json", api(listen, "process-status"))
                (out / "saved-after.raw.json").write_bytes(graph_path.read_bytes())
                assert (out / "saved-before.raw.json").read_bytes() == (out / "saved-after.raw.json").read_bytes()
            finally:
                server.send_signal(signal.SIGINT)
                server.wait(timeout=15)

    # These selected-pin owner tests use a counted adapter, fake clock or
    # journal write fault, and assert the release count and restart behavior.
    tests = [
        ("identity", "aether-dispatcher", "gateway_stop", "managed_movement_identity_and_owner_survive_restart_without_replay"),
        ("deadline", "aether-runtime", "grpc_submit_validation", "expired_deadline_and_transport_request_id_cannot_replace_operation_id"),
        ("uncertain", "aether-runtime", "grpc_journal_fault", "post_release_terminal_write_fault_is_unknown_and_never_replays_after_restart"),
        ("stop-fence", "aether-dispatcher", "gateway_stop", "managed_whole_robot_stop_preempts_blocked_release_and_fences_late_completion"),
    ]
    for label, package, target, name in tests:
        command = [str(args.cargo), "test", "--locked", "-p", package, "--test", target, name, "--", "--exact", "--nocapture", "--test-threads=1"]
        run = subprocess.run(command, cwd=eos, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
        (out / f"{label}-cargo.log").write_text("$ " + " ".join(command) + "\n" + run.stdout)
        assert run.returncode == 0 and f"test {name} ... ok" in run.stdout, (label, run.returncode, run.stdout[-3000:])
    probe_target = eos / "crates/aether-dispatcher/tests/q01_g06_r03_journal_probe.rs"
    assert not probe_target.exists(), "selected EOS checkout already contains Q01 overlay"
    shutil.copy2(overlay, probe_target)
    try:
        command = [str(args.cargo), "test", "--locked", "-p", "aether-dispatcher", "--test", "q01_g06_r03_journal_probe", "q01_archived_journal_identity_and_stop_fence", "--", "--exact", "--nocapture", "--test-threads=1"]
        probe_env = dict(os.environ, Q01_G06_R03_PROBE_OUTPUT=str(out / "journal-probe"))
        run = subprocess.run(command, cwd=eos, env=probe_env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
        (out / "journal-probe-cargo.log").write_text("$ " + " ".join(command) + "\n" + run.stdout)
        assert run.returncode == 0 and "test q01_archived_journal_identity_and_stop_fence ... ok" in run.stdout, run.stdout[-3000:]
    finally:
        probe_target.unlink()
    save(out / "case-result.json", {"status": "passed", "scope": "selected-pin IsOS API plus EOS controlled-stub tests", "edge_cases": [case["name"] for case in cases], "dispatcher_tests": [item[0] for item in tests]})
    save(out / "evidence.sha256.json", {path.relative_to(out).as_posix(): sha(path) for path in sorted(out.rglob("*")) if path.is_file() and path.name != "evidence.sha256.json"})
    print("Q01_G06_R03_CASE_PASS", out)


if __name__ == "__main__":
    main()
