#!/usr/bin/env python3
"""Archive-only semantic gate for Q01 selected R06/A03 negative paths."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import re
import shutil
import stat
import subprocess
import tarfile
import tempfile
from pathlib import Path

from q01_selection import canonical_bytes, load_selection, selected_pins as frozen_pins, sha256

REASONS = {"child_exit_unexpected", "stop_signal_failed", "stop_deadline_exceeded",
           "stop_forced_kill", "stop_group_unreaped", "eos_stop_unconfirmed",
           "eos_receipt_missing", "eos_receipt_invalid", "eos_receipt_late",
           "control_grant_retire_failed", "stop_result_persist_failed",
           "stop_interrupted"}
SELECTED_PINS = {
    "eos": "642166576a2cddfd97cd703e5bd9e53567745515",
    "isos": "00c1d7aa184de8b969e6e40472bf9b1fa55032ff",
    "egia": "d3868d2763c35d9f6bfed80d75edb7a1844ef54e",
    "mujoco": "4e6b5449ccd25be53ad95b13baa2c89f7ed91e10",
    "ui": "920995788d8e29dd6ab594d31278033bb791cfeb",
}
SELECTED_TREES = {
    "eos": "dc912ae2320938305aa5f5d0cdf7dee7e88af549",
    "isos": "90d7a87c5c98dacbdcca0c2eacbdcead3fb4774b",
    "egia": "d3c662c06470323553e0e827cd430b129ef6622b",
    "mujoco": "0df44f54ffdbb8fac8e4b694b23cb19333f76241",
    "ui": "b93db23ec5ad24453417dc3a83d03f93dd069deb",
}


def read(root: Path, name: str):
    return json.loads((root / name).read_text())


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def hex64(value: str) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def git_sha(value: str) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{40,64}", value) is not None


def fnv(body: dict) -> int:
    encoded = json.dumps(body, separators=(",", ":"), ensure_ascii=False).encode()
    value = 0xcbf29ce484222325
    for byte in encoded:
        value = ((value ^ byte) * 0x100000001b3) & ((1 << 64) - 1)
    return value


def archive_tree(archive: Path) -> str:
    """Recompute the Git tree represented by a selected git archive."""
    def oid(kind: str, data: bytes) -> bytes:
        return hashlib.sha1(kind.encode() + b" " + str(len(data)).encode() + b"\0" + data).digest()

    tree: dict = {}
    with tarfile.open(archive) as tar:
        for member in tar:
            if member.isdir():
                continue
            parts = member.name.split("/")
            assert parts and all(part not in ("", ".", "..") for part in parts)
            assert not member.name.startswith("/")
            if member.isfile():
                content = tar.extractfile(member)
                assert content is not None
                raw = content.read()
                mode = b"100755" if member.mode & 0o111 else b"100644"
            elif member.issym():
                raw = member.linkname.encode()
                mode = b"120000"
            else:
                raise AssertionError(("unsupported tar entry", member.name))
            node = tree
            for part in parts[:-1]:
                node = node.setdefault(part, {})
                assert isinstance(node, dict), member.name
            assert parts[-1] not in node, ("duplicate tar entry", member.name)
            node[parts[-1]] = (mode, oid("blob", raw))

    def hash_tree(node: dict) -> bytes:
        entries = []
        for name, value in node.items():
            is_dir = isinstance(value, dict)
            mode, value_oid = (b"40000", hash_tree(value)) if is_dir else value
            entries.append((name.encode() + (b"/" if is_dir else b""), mode, name.encode(), value_oid))
        raw = b"".join(mode + b" " + name + b"\0" + value_oid
                       for _, mode, name, value_oid in sorted(entries))
        return oid("tree", raw)

    return hash_tree(tree).hex()


def source(root: Path, selected: dict, pins: dict | None,
           selection: dict | None = None) -> None:
    for owner in ("eos", "isos", "egia", "mujoco", "isosui"):
        receipt = selected[owner]
        assert git_sha(receipt["commit"]) and git_sha(receipt["tree"])
        archive = root / "source-snapshot" / f"{owner}.tar"
        assert sha(archive) == receipt["source_sha256"]
        embedded_commit = subprocess.check_output(
            ["git", "get-tar-commit-id"], input=archive.read_bytes(), text=False
        ).decode().strip()
        assert embedded_commit == receipt["commit"], owner
        pin_key = "ui" if owner == "isosui" else owner
        if selection:
            assert receipt['commit'] == selection['owners'][pin_key]['commit'], owner
        expected_tree = (selection['owners'][pin_key]['tree'] if selection else
                         SELECTED_TREES[pin_key])
        assert archive_tree(archive) == receipt["tree"] == expected_tree, owner
        with tarfile.open(archive) as tar:
            names = set(tar.getnames())
            assert "AGENTS.md" in names or "README.md" in names
            if selection:
                for path, expected_hash in selection['owners'][pin_key]['sources'].items():
                    assert path in names, (owner, path)
                    member = tar.extractfile(path)
                    assert member is not None and hashlib.sha256(member.read()).hexdigest() == \
                        expected_hash, (owner, path)
            lock = {"egia": "uv.lock", "isosui": "package-lock.json"}.get(
                owner, "Cargo.lock")
            if "lock_sha256" in receipt:
                assert lock in names
                content = tar.extractfile(lock)
                assert content is not None
                raw = content.read()
                assert hashlib.sha256(raw).hexdigest() == receipt["lock_sha256"]
                assert raw == (root / "source-snapshot" / f"{owner}-{lock}").read_bytes()
                if selection:
                    assert receipt['lock_sha256'] == selection['owners'][pin_key]['locks'][lock]
        if pins is not None:
            assert receipt["commit"] == pins[pin_key], owner
    binaries = root / "selected-binaries"
    for name in ("isos", "egia", "egia-python", "aether-runtime", "aether-profile",
                 "aether-http-bridge", "aether-config-server"):
        assert sha(binaries / name) == selected["binary_sha256"][name]
    assert selected["egia"]["selected_interpreter_sha256"] == \
        selected["binary_sha256"]["egia-python"]
    assert selected["isosui"]["build_command"] == ["npm", "run", "build:local"]
    assert sha(root / "isosui-selected-build.log") == selected["isosui"]["build_log_sha256"]
    assert "compiled successfully" in (root / "isosui-selected-build.log").read_text()
    ui_dist = root / "selected-ui-dist"
    assert {path.relative_to(ui_dist).as_posix(): sha(path) for path in ui_dist.rglob("*")
            if path.is_file()} == selected["isosui"]["dist_sha256"]
    assert "index.html" in selected["isosui"]["dist_sha256"]
    assert "Finished" in (root / "eos-selected-build.log").read_text()
    assert "Finished" in (root / "isos-selected-build.log").read_text()
    for owner in ("eos", "isos"):
        receipt = selected[owner]
        assert receipt["build_command"][1:3] == ["build", "--locked"]
        assert sha(root / f"{owner}-selected-build.log") == receipt["build_log_sha256"]
    package = selected["package"]
    release = read(root, "package-release.json")
    assert release["id"] == "island/mujoco-sim" and release["version"] == "0.1.1"
    assert release["source_commit"] == selected["mujoco"]["commit"] == package["source_commit"]
    assert sha(root / "package-release.json") == package["release_sha256"]
    assert sha(root / "package-asset.tar.gz") == package["asset_sha256"]
    if selection:
        assert package['release_sha256'] == selection['package']['release_sha256']
        assert package['asset_sha256'] == selection['package']['asset_sha256']
    assert any(item["sha256"] == package["asset_sha256"] for item in release["assets"])
    with tarfile.open(root / "package-asset.tar.gz") as tar:
        payload = [item for item in tar.getmembers()
                   if item.isfile() and item.name.endswith("bin/eos-mujoco-sim")]
        assert len(payload) == 1
        stream = tar.extractfile(payload[0])
        assert stream is not None
        assert hashlib.sha256(stream.read()).hexdigest() == package["executable_sha256"]


def operation_journal(case: Path, revision: str, run_id: str) -> None:
    admitted = read(case, "admitted-command.json")
    assert admitted["accepted"] is True and admitted["status"] == "succeeded"
    raw = read(case, "admitted-command.rec")
    body = raw["body"]
    assert raw["checksum"] == fnv(body)
    assert body["command_id"] == admitted["command_id"]
    assert body["node_path"] == "eos/hardware/base/drive"
    assert body["phase"] == "terminal"
    assert body["result"]["status"] == "Completed"
    identity = body["operation"]["identity"]
    assert identity["primitive"] == "base.velocity"
    assert identity["admission"]["run_id"] == run_id
    assert identity["admission"]["active_revision"] == revision
    assert identity["admission"]["authority_role"] == "egia_control"
    digest = sha(case / "admitted-command.rec")
    before = read(case, "pre-fault-state.json")
    after = read(case, "post-fault-state.json")
    assert before["journal_record_sha256"] == after["journal_record_sha256"] == digest
    journal = case / "journal"
    manifest = read(case, "journal-sha256.json")
    assert {path.name for path in journal.iterdir() if path.is_file()} == set(manifest)
    for name, value in manifest.items():
        assert sha(journal / name) == value
    encoded = admitted["command_id"].encode().hex() + ".rec"
    assert encoded in manifest and manifest[encoded] == digest


def identity(value: dict) -> None:
    assert type(value["pid"]) is int and value["pid"] > 1
    assert type(value["ppid"]) is int and value["ppid"] > 0
    assert type(value["pgrp"]) is int and value["pgrp"] > 1
    assert type(value["session"]) is int and value["session"] > 1
    assert type(value["start_ticks"]) is int and value["start_ticks"] > 0


def blocked(case: Path, name: str) -> None:
    value = read(case, name)
    assert value["http_status"] == 422
    reply = value["reply"]
    assert reply["ok"] is False
    assert reply["data"]["diagnostics"][0]["code"] == "deployment_stop_unconfirmed"


def recorded_case_root(argv: list[str], runtime_env: dict) -> Path:
    """Check the launch location independently of the archive's current path."""
    output = Path(argv[argv.index("--output") + 1])
    assert output.is_absolute()
    expected_root = output.resolve()
    binaries = Path(runtime_env["ISOS_AETHER_BIN_DIR"])
    ui_dist = Path(runtime_env["ISOSUI_DIST_DIR"])
    assert binaries.is_absolute() and ui_dist.is_absolute()
    recorded_root = binaries.parent
    assert recorded_root == expected_root
    assert binaries == recorded_root / "selected-binaries"
    assert ui_dist == recorded_root / "selected-ui-dist"
    return recorded_root


def release_views_equal(selected_release: dict, installed_release: dict) -> bool:
    """Compare release views after only the owner-defined empty-field omissions."""
    selected = copy.deepcopy(selected_release)
    installed = copy.deepcopy(installed_release)
    selected_contributions = selected.get("node_contributions", {})
    installed_contributions = installed.get("node_contributions", {})
    if "data_types" not in selected_contributions and \
            installed_contributions.get("data_types") == []:
        installed_contributions.pop("data_types")
    selected_assets = selected.get("assets", [])
    installed_assets = installed.get("assets", [])
    if len(selected_assets) == len(installed_assets):
        for source_asset, installed_asset in zip(selected_assets, installed_assets):
            for key in ("required_libraries", "tested_ubuntu"):
                if source_asset.get(key) == [] and key not in installed_asset:
                    source_asset.pop(key)
    return selected == installed


def live_case(root: Path, label: str, selected: dict) -> None:
    case = root / label
    runtime_env = read(case, "runtime-environment.json")
    # The matrix copies sealed case bytes under its own archive. Validate the
    # original run location recorded by the launcher, then inspect the copied
    # binaries and UI bytes through this archive's sealed manifest.
    argv = read(root, "command-argv.json")
    recorded_case_root(argv, runtime_env)
    assert runtime_env["ISOS_HOME"] == read(case, "home-path.json")["path"]
    before = read(case, "status-running.json")
    snapshot = read(case, "process-before.json")
    revision = before["active_revision"]
    assert hex64(revision) and revision == before["saved_revision"]
    assert sha(case / "saved-graph.raw.json") == revision
    graph = json.loads((case / "saved-graph.raw.json").read_bytes())
    assert graph["version"] == "0.1.0"
    receipt = read(case, "package-receipt.raw.json")
    assert receipt["id"] == "island/mujoco-sim" and receipt["version"] == "0.1.1"
    assert receipt["source_commit"] == selected["mujoco"]["commit"]
    assert receipt["source_kind"] == "local_link"
    assert receipt["release_sha256"] == sha(case / "installed-release.raw.json")
    installed_release = read(case, "installed-release.raw.json")
    selected_release = read(root, "package-release.json")
    assert release_views_equal(selected_release, installed_release)
    assert before["state"] == "running" and before["run_id"]
    run_id = before["run_id"]
    direct = snapshot["direct_children"]
    assert set(direct) == {row["id"] for row in before["processes"]}
    assert {name: row["pid"] for name, row in direct.items()} == {
        row["id"]: row["pid"] for row in before["processes"]}
    for row in direct.values():
        identity(row)
    engine = snapshot["eos_child"]
    identity(engine)
    assert engine["ppid"] == direct["eos"]["pid"]
    assert engine["pid"] not in {row["pid"] for row in direct.values()}
    assert snapshot["socket_exists"] is True
    assert snapshot["eos_child_exe_sha256"] == selected["package"]["executable_sha256"]
    assert snapshot["eos_child_cmdline"].endswith("eos-mujoco-sim")
    operation_journal(case, revision, run_id)
    fault = read(case, "fault-signal.json")
    assert fault["pid_only"] is True and fault["sent_unix_ns"] > 0
    target = engine if label == "selected-child-fault" else direct["eos"]
    assert all(fault["identity"][field] == target[field] for field in
               ("pid", "ppid", "pgrp", "session", "start_ticks"))
    assert fault["identity"]["state"] not in {"Z", "X"}
    assert fault["signal"] == ("SIGKILL" if label == "selected-child-fault" else "SIGSTOP")
    if label == "receipt-loss":
        barrier = read(case, "stop-barrier.json")
        assert barrier["record"]["run_id"] == run_id
        assert barrier["record"]["phase"] == "stopping"
        assert barrier["record"]["outcome"] == "unknown"
        assert barrier["stopping_event"]["run_id"] == run_id
        kill = read(case, "kill-signal.json")
        assert all(kill["identity"][field] == direct["eos"][field] for field in
                   ("pid", "ppid", "pgrp", "session", "start_ticks"))
        assert kill["identity"]["state"] not in {"Z", "X"}
        assert kill["signal"] == "SIGKILL" and kill["pid_only"] is True
        assert kill["sent_unix_ns"] >= fault["sent_unix_ns"]
        response = read(case, "stop-response.json")
        assert response["http_status"] == 200 and response["reply"]["ok"] is True
        terminal = response["reply"]["data"]["terminal"]
    else:
        spontaneous = read(case, "spontaneous-status.json")
        assert spontaneous["state"] == "degraded"
        terminal = spontaneous["terminal"]
        assert not (case / "stop-response.json").exists()
    assert terminal["run_id"] == run_id and terminal["outcome"] == "unknown"
    assert terminal["reason_code"] in REASONS
    status = read(case, "status-degraded.json")
    assert status["state"] == "degraded" and status["run_id"] == run_id
    assert status["active_revision"] == revision and status["terminal"] == terminal
    expected_error = ("managed child exited unexpectedly; dependent stop is unconfirmed"
        if label == "selected-child-fault" else
        "dependent stop unconfirmed; owner recovery required")
    assert status["last_error"] == expected_error
    record = read(case, "stop-record.raw.json")
    assert {key: record[key] for key in ("schema_version", "deployment", "run_id",
            "phase", "outcome")} == {"schema_version": 1, "deployment": "default",
            "run_id": run_id, "phase": "stopping", "outcome": "unknown"}
    selected_ids = record["selected_processes"]
    assert set(selected_ids) == set(direct) and len(selected_ids) == len(direct)
    assert [row["id"] for row in terminal["processes"]] == selected_ids
    components = graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]
    for consumer in selected_ids:
        for dependency in components[consumer]["settings"].get("depends_on", []):
            provider = dependency.rsplit("/", 1)[-1]
            if provider in selected_ids:
                assert selected_ids.index(consumer) < selected_ids.index(provider)
    assert all(row["outcome"] in {"unknown", "confirmed"} and
               (row["reason_code"] is not None if row["outcome"] == "unknown" else
                row["reason_code"] is None) for row in terminal["processes"])
    event_lines = [json.loads(line) for line in (case / "events.raw.jsonl").read_text().splitlines()]
    same = [row for row in event_lines if row.get("run_id") == run_id]
    stops = [row for row in same if row["event_code"] == "deployment_stopping"]
    failures = [row for row in same if row["event_code"] == "deployment_stop_unconfirmed"]
    assert len(stops) == len(failures) == 1
    assert stops[0]["correlation_id"] == failures[0]["correlation_id"]
    assert read(case, "stop-events.json") == {"stopping": stops[0],
                                              "unconfirmed": failures[0]}
    owner_logs = case / "owner-logs"
    assert {f"{name}.log" for name in ("eos", "isos", "mujoco", "egia")} <= {
        path.name for path in owner_logs.iterdir() if path.is_file()}
    isos_lines = [json.loads(line) for line in (owner_logs / "isos.log").read_text().splitlines()
                  if line.startswith("{")]
    assert any(row.get("run_id") == run_id and row.get("event_code") ==
               "deployment_stop_unconfirmed" for row in isos_lines)
    if label == "selected-child-fault":
        eos_lines = [json.loads(line) for line in (owner_logs / "eos.log").read_text().splitlines()
                     if line.startswith("{")]
        assert any(row.get("run_id") == run_id and row.get("event_code") ==
                   "eos_backend_lost" for row in eos_lines)
        assert any(row.get("run_id") == run_id and row.get("event_code") ==
                   "eos_stop_unconfirmed" for row in eos_lines)
    assert not any(row["event_code"] == "deployment_ready"
                   for row in same[same.index(failures[0]) + 1:])
    reap = read(case, "reap.json")
    assert reap["identities"] == {**direct, "mujoco": engine}
    assert reap["alive"] == {name: False for name in reap["identities"]}
    assert reap["mujoco_socket_exists"] is False
    for operation in ("run", "restart"):
        blocked(case, f"blocked-{operation}.json")
        blocked(case, f"manager-restarted-blocked-{operation}.json")
    repeated = read(case, "repeated-stop.json")
    assert repeated["http_status"] == 200 and repeated["reply"]["data"]["terminal"] == terminal
    shutdown = json.loads((case / "manager-shutdown.stdout").read_text())
    assert shutdown["manager"] == "stopping" and shutdown["terminal"] == [terminal]
    assert (case / "manager-shutdown.stderr").read_text().strip() == \
        "isos: dependent stop unconfirmed"
    assert read(case, "manager-serve-exit.json")["exit_code"] == 1
    assert "isos: dependent stop unconfirmed during manager shutdown" in \
        (case / "serve.log").read_text()
    recovered = read(case, "status-manager-restarted.json")
    assert recovered["state"] == "degraded" and recovered["run_id"] == run_id
    assert recovered["terminal"]["outcome"] == "unknown"
    assert recovered["terminal"]["reason_code"] in {"stop_interrupted", "stop_result_persist_failed"}
    assert not recovered["processes"]
    after = read(case, "post-fault-state.json")
    assert after["graph_sha256"] == sha(case / "saved-graph.raw.json")
    assert after["run_id"] == run_id and after["new_child_alive"] is False
    assert read(case, "pre-fault-state.json")["graph_sha256"] == after["graph_sha256"]


def owner_snapshot(root: Path, label: str, operation_id: str,
                   *, safety: bool) -> dict[str, bytes]:
    snapshot = root / label
    assert snapshot.is_dir() and not snapshot.is_symlink()
    assert stat.S_IMODE(snapshot.stat().st_mode) == 0o700
    expected = ["operations-history.json", f"operations/{operation_id}.index",
                f"operations/{operation_id}.json"]
    if safety:
        expected += ["safety/safety-history.json", "safety/safety-latch.json"]
    manifest = read(snapshot, "manifest.json")
    assert set(manifest) == {"label", "operation_id", "files"}
    assert manifest["label"] == label and manifest["operation_id"] == operation_id
    assert [item["path"] for item in manifest["files"]] == expected
    files = {}
    inventory = set()
    for path in snapshot.rglob("*"):
        assert not path.is_symlink(), path
        if path.is_dir():
            assert stat.S_IMODE(path.stat().st_mode) == 0o700, path
        else:
            assert path.is_file(), path
            inventory.add(path.relative_to(snapshot).as_posix())
            assert stat.S_IMODE(path.stat().st_mode) == 0o600, path
    assert inventory == set(expected) | {"manifest.json"}
    for item in manifest["files"]:
        assert set(item) == {"path", "sha256", "bytes", "mode"}
        assert item["mode"] == "0600" and hex64(item["sha256"])
        raw = (snapshot / item["path"]).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == item["sha256"]
        assert len(raw) == item["bytes"]
        files[item["path"]] = raw
    history = json.loads(files["operations-history.json"])
    index = json.loads(files[f"operations/{operation_id}.index"])
    checkpoint = json.loads(files[f"operations/{operation_id}.json"])
    assert history == {"schema": "egia-operations-history-v1", "reserved_count": 1}
    assert index == {"schema": "egia-operation-index-v1", "operation_id": operation_id,
                     "sequence": 1}
    assert checkpoint["schema"] == "egia-operation-v1"
    assert checkpoint["operation_id"] == operation_id
    assert checkpoint["graph_revision"] == "a" * 64
    assert checkpoint["alias"] == "robot-a" and checkpoint["primitive"] == "base.velocity"
    assert checkpoint["device_id"] == "base-0"
    if safety:
        safety_history = json.loads(files["safety/safety-history.json"])
        latch = json.loads(files["safety/safety-latch.json"])
        assert safety_history["schema"] == "egia-safety-history-v1"
        assert safety_history["state"] == "engaged"
        assert re.fullmatch(r"[0-9a-f]{32}", safety_history["episode_id"])
        assert safety_history["graph_revision"] == "a" * 64
        assert latch["schema"] == "egia-safety-latch-v1"
        assert latch["episode_id"] == safety_history["episode_id"]
        assert latch["graph_revision"] == safety_history["graph_revision"]
        assert latch["identities"]["robot-a"] == checkpoint["eos_robot_id"]
    return files


def egia_case(root: Path, selected: dict) -> None:
    case = root / "egia-process-restart"
    command = read(case, "test-command.json")
    assert command["exit_code"] == 0
    assert sha(case / "pytest.log") == command["log_sha256"]
    assert "7 passed" in (case / "pytest.log").read_text()
    with tarfile.open(root / "source-snapshot/egia.tar") as tar:
        source = tar.extractfile("tests/egia/test_a03_process_restart.py")
        worker = tar.extractfile("tests/egia/a03_restart_worker.py")
        assert source is not None and worker is not None
        assert b"A03_EVIDENCE_DIR" in source.read()
        assert b"os._exit(71 if submit else 72)" in worker.read()
    snapshots = case / "worker-evidence"
    positive_before = owner_snapshot(snapshots, "positive-before",
                                     "a03-process-terminal", safety=False)
    positive_after = owner_snapshot(snapshots, "positive-after",
                                    "a03-process-terminal", safety=False)
    negative_before = owner_snapshot(snapshots, "negative-before",
                                     "a03-process-unknown", safety=False)
    negative_after = owner_snapshot(snapshots, "negative-after",
                                    "a03-process-unknown", safety=True)
    negative_refused = owner_snapshot(snapshots, "negative-refused",
                                      "a03-process-unknown", safety=True)
    for path in ("operations-history.json", "operations/a03-process-terminal.index"):
        assert positive_before[path] == positive_after[path]
    assert json.loads(positive_before["operations/a03-process-terminal.json"])["state"] == \
        "pending"
    assert json.loads(positive_after["operations/a03-process-terminal.json"])["state"] == \
        "terminal"
    for path in negative_before:
        assert negative_before[path] == negative_after[path] == negative_refused[path]
    for path in ("safety/safety-history.json", "safety/safety-latch.json"):
        assert negative_after[path] == negative_refused[path]
    for branch, operation_id, expected in (
        ("positive", "a03-process-terminal", "recovered"),
        ("negative", "a03-process-unknown", "missing"),
    ):
        first = read(case, f"worker-evidence/{branch}-worker-a.rpc.json")
        second = read(case, f"worker-evidence/{branch}-worker-b.rpc.json")
        assert first["run_id"] == second["run_id"] == "ra03_owner_restart"
        assert first["revision"] == second["revision"] == "a" * 64
        assert ("GetOperation", operation_id) in second["operation_requests"] or \
            ["GetOperation", operation_id] in second["operation_requests"]
        first_methods = first["rpc_methods"]
        second_methods = second["rpc_methods"]
        assert second_methods[:len(first_methods)] == first_methods
        restart = second_methods[len(first_methods):]
        lookup = restart.index("GetOperation")
        assert {"Discover", "GetStatus", "ListCapabilities"} <= set(restart[:lookup])
        assert "SubmitOperation" not in restart[:lookup]
        if branch == "positive":
            assert "SubmitOperation" not in restart
        else:
            assert restart.count("SubmitOperation") == 1
        state = case / "owner-state" / expected
        assert stat.S_IMODE(state.parent.stat().st_mode) == 0o700
        assert stat.S_IMODE(state.stat().st_mode) == 0o700
        allowed = {"operations-history.json", f"operations/{operation_id}.index",
                   f"operations/{operation_id}.json", "modes.json"}
        if branch == "negative":
            allowed |= {"safety/safety-history.json", "safety/safety-latch.json"}
        inventory = set()
        for path in state.rglob("*"):
            assert not path.is_symlink(), path
            assert path.is_dir() or path.is_file(), path
            if path.is_file():
                inventory.add(path.relative_to(state).as_posix())
        assert inventory == allowed
        history = read(state, "operations-history.json")
        index = read(state, f"operations/{operation_id}.index")
        checkpoint = read(state, f"operations/{operation_id}.json")
        assert history == {"schema": "egia-operations-history-v1", "reserved_count": 1}
        assert index == {"schema": "egia-operation-index-v1",
                         "operation_id": operation_id, "sequence": 1}
        assert checkpoint["schema"] == "egia-operation-v1"
        assert checkpoint["operation_id"] == operation_id
        assert checkpoint["graph_revision"] == "a" * 64
        assert checkpoint["alias"] == "robot-a" and checkpoint["primitive"] == "base.velocity"
        assert checkpoint["device_id"] == "base-0"
        modes = read(state, "modes.json")
        assert set(modes) == allowed - {"modes.json"}
        for path, mode in modes.items():
            assert mode == 0o600 and (state / path).is_file()
            assert (state / path).stat().st_mode & 0o777 == 0o600
        final_snapshot = positive_after if branch == "positive" else negative_refused
        for path, raw in final_snapshot.items():
            assert (state / path).read_bytes() == raw
        if branch == "positive":
            assert checkpoint["state"] == "terminal" and checkpoint["outcome"] == "succeeded"
            assert second["operation_requests"].count(["SubmitOperation", operation_id]) == 1
            assert second["operation_requests"].count(["GetOperation", operation_id]) >= 1
            assert len(second["physical_calls"]) == 1
            assert second["physical_calls"][0][0] == "base.velocity"
        else:
            assert checkpoint["state"] == "pending" and "outcome" not in checkpoint
            assert second["operation_requests"].count(["SubmitOperation", operation_id]) == 0
            assert second["operation_requests"].count(["GetOperation", operation_id]) >= 1
            assert second["physical_calls"] == [["safety.estop", "", {}]]
            assert all(request[1] != operation_id for request in second["operation_requests"]
                       if request[0] == "SubmitOperation")
            history = read(state, "safety/safety-history.json")
            latch = read(state, "safety/safety-latch.json")
            assert history["schema"] == "egia-safety-history-v1"
            assert history["state"] == "engaged"
            assert re.fullmatch(r"[0-9a-f]{32}", history["episode_id"])
            assert history["graph_revision"] == "a" * 64
            assert latch["schema"] == "egia-safety-latch-v1"
            assert latch["episode_id"] == history["episode_id"]
            assert latch["graph_revision"] == history["graph_revision"]
            assert latch["identities"]["robot-a"] == checkpoint["eos_robot_id"]
            third = read(case, "worker-evidence/negative-worker-c.rpc.json")
            assert third["operation_requests"].count(["SubmitOperation", operation_id]) == 0
            assert third["physical_calls"] == second["physical_calls"]
            stderr = (case / "worker-evidence/negative-worker-c.stderr.log").read_text()
            assert '"event_code": "egia_safety_reset_refused"' in stderr or \
                '"event_code":"egia_safety_reset_refused"' in stderr


def verify(root: Path, selected_pins: dict | None = None,
           selection: dict | None = None) -> None:
    root = root.resolve()
    manifest = read(root, "evidence.sha256.json")
    actual = {}
    for path in root.rglob("*"):
        mode = path.lstat().st_mode
        if stat.S_ISDIR(mode):
            continue
        assert stat.S_ISREG(mode), f"nonregular archive member: {path}"
        if path != root / "evidence.sha256.json":
            actual[path.relative_to(root).as_posix()] = sha(path)
    assert actual == manifest
    if selection:
        assert read(root,'selection-sha256.json') == \
            {'profile':'selected-q03','sha256':sha256(canonical_bytes(selection))}
        if selected_pins is not None:
            assert all(selected_pins[name] == pin for name,pin in
                       frozen_pins(selection).items())
    selected = read(root, "selected-inputs.json")
    source(root, selected, selected_pins, selection=selection)
    for label in ("selected-child-fault", "receipt-loss"):
        live_case(root, label, selected)
    egia_case(root, selected)


def source_tamper_checks(root: Path, selection=None) -> None:
    """Reseal selected-source forgeries and require semantic rejection."""
    selected = read(root, "selected-inputs.json")
    pins = frozen_pins(selection) if selection else SELECTED_PINS
    wrong_ui = dict(pins, ui="0" * 40)
    try:
        source(root, selected, wrong_ui, selection=selection)
    except AssertionError:
        print("ui-external-pin REJECTED")
    else:
        raise AssertionError("UI external selected pin was ignored")

    with tempfile.TemporaryDirectory(prefix="q01-r06-source-tamper-",
                                     dir=root.parent) as temporary:
        for case in ("ui-embedded-commit", "mujoco-archive-tree"):
            copy = Path(temporary) / case
            shutil.copytree(root, copy, copy_function=shutil.copy2)
            inputs = read(copy, "selected-inputs.json")
            if case == "ui-embedded-commit":
                archive = copy / "source-snapshot/isosui.tar"
                raw = archive.read_bytes()
                embedded = pins["ui"].encode()
                assert raw.count(embedded) == 1
                forged = raw.replace(embedded, b"0" * 40)
                replacement = archive.with_suffix(".changed")
                replacement.write_bytes(forged)
                replacement.replace(archive)
                inputs["isosui"]["source_sha256"] = sha(archive)
            else:
                archive = copy / "source-snapshot/mujoco.tar"
                raw = bytearray(archive.read_bytes())
                with tarfile.open(archive) as tar:
                    member = next(item for item in tar if item.isfile()
                                  and item.name in ("README.md", "AGENTS.md")
                                  and item.size > 0)
                offset = member.offset_data
                raw[offset] = ord("X") if raw[offset] != ord("X") else ord("Y")
                replacement = archive.with_suffix(".changed")
                replacement.write_bytes(raw)
                replacement.replace(archive)
                inputs["mujoco"]["source_sha256"] = sha(archive)
                inputs["mujoco"]["tree"] = archive_tree(archive)
                assert inputs["mujoco"]["tree"] != (selection['owners']['mujoco']['tree']
                    if selection else SELECTED_TREES["mujoco"])
            inputs_replacement = copy / "selected-inputs.changed"
            inputs_replacement.write_text(json.dumps(inputs, sort_keys=True))
            inputs_replacement.replace(copy / "selected-inputs.json")
            manifest = {path.relative_to(copy).as_posix(): sha(path)
                        for path in copy.rglob("*") if path.is_file()
                        and path != copy / "evidence.sha256.json"}
            seal_replacement = copy / "evidence.sha256.changed"
            seal_replacement.write_text(json.dumps(manifest, sort_keys=True))
            seal_replacement.replace(copy / "evidence.sha256.json")
            try:
                verify(copy, pins, selection=selection)
            except AssertionError:
                print(case, "REJECTED after manifest reseal")
            else:
                raise AssertionError((case, "forged selected source accepted"))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--source-tamper-checks", action="store_true")
    parser.add_argument('--expect-selection',type=Path)
    parser.add_argument('--expect-selection-sha256')
    args = parser.parse_args()
    assert bool(args.expect_selection) == bool(args.expect_selection_sha256)
    selection = (load_selection(args.expect_selection,args.expect_selection_sha256)
                 if args.expect_selection else None)
    pins = frozen_pins(selection) if selection else SELECTED_PINS
    verify(args.archive, pins, selection=selection)
    print("Q01_R06_A03_ARCHIVE_PASS")
    if args.source_tamper_checks:
        source_tamper_checks(args.archive.resolve(), selection=selection)


if __name__ == "__main__":
    main()
