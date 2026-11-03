#!/usr/bin/env python3
"""Q01 R06/A03 fault replay against clean, selected owner checkouts.

Each live fault gets a fresh home.  A failed or ambiguous trial is retained,
but cannot be promoted to a passing archive by the verifier.
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from m4_affected_replay import api, save, sha
from q01_selection import check_harness, load_selection, replay_home, verify_checkout

BINARIES = ("aether-runtime", "aether-profile", "aether-http-bridge",
            "aether-config-server")


def git(root: Path, *argv: str) -> str:
    return subprocess.check_output(["git", *argv], cwd=root, text=True).strip()


def port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def raw_api(port_number: int, route: str, operation: str,
            body: dict | None = None, *, timeout: float = 180) -> dict:
    request = Request(f"http://127.0.0.1:{port_number}/api/isos/v1/{route}",
        method=operation, data=None if body is None else json.dumps(body).encode(),
        headers={"Content-Type": "application/json",
                 "Origin": f"http://127.0.0.1:{port_number}"})
    try:
        response = urlopen(request, timeout=timeout)
    except HTTPError as error:
        response = error
    with response:
        return {"http_status": response.status, "reply": json.load(response)}


def wait_manager_restart_http(api_port: int, server: subprocess.Popen,
                              out: Path, *, seconds: float = 25) -> dict:
    """Wait for the restarted manager's HTTP bind, then require one good reply."""
    deadline = time.monotonic() + seconds
    start = time.monotonic()
    attempts = 0
    refused = 0
    with (out / "manager-restart-http-probes.jsonl").open("w") as probes:
        try:
            while time.monotonic() < deadline:
                exit_code = server.poll()
                assert exit_code is None, f"isos serve exited before HTTP bind: {exit_code}"
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                attempts += 1
                observed_ns = time.time_ns()
                try:
                    observed = raw_api(api_port, "process-status", "GET",
                                       timeout=min(3, remaining))
                except URLError as error:
                    if not isinstance(error.reason, ConnectionRefusedError):
                        raise
                    refused += 1
                    probes.write(json.dumps({"attempt": attempts,
                        "observed_unix_ns": observed_ns,
                        "elapsed_seconds": time.monotonic() - start,
                        "connection_refused": True}) + "\n")
                    probes.flush()
                    time.sleep(min(.05, max(0, deadline - time.monotonic())))
                    continue
                probes.write(json.dumps({"attempt": attempts,
                    "observed_unix_ns": observed_ns,
                    "elapsed_seconds": time.monotonic() - start,
                    **observed}) + "\n")
                probes.flush()
                assert observed["http_status"] == 200, observed
                reply = observed["reply"]
                assert isinstance(reply, dict) and reply.get("ok") is True, observed
                assert isinstance(reply.get("data"), dict), observed
                return reply["data"]
            raise AssertionError(f"manager HTTP did not bind within {seconds}s")
        finally:
            save(out, "manager-restart-http-probes-summary.json", {
                "attempts": attempts, "connection_refused": refused,
                "elapsed_seconds": time.monotonic() - start,
                "deadline_seconds": seconds})


def wait_initial_ready(api_port: int, revision: str, run: subprocess.Popen,
                       out: Path, *, seconds: float = 360) -> dict:
    """Allow only IsOS's documented busy status before the first ready state."""
    deadline = time.monotonic() + seconds
    counts = {"transient_503": 0, "valid_not_ready": 0, "ready": 0}
    last = None
    attempts = 0
    try:
        with (out / "readiness-probes.jsonl").open("w") as probes:
            while time.monotonic() < deadline:
                exit_code = run.poll()
                assert exit_code is None, f"isos run exited before readiness: {exit_code}; see run.log"
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                # The owner spends up to two seconds on its manager socket.
                # Give the HTTP reply time to arrive without extending the
                # overall readiness deadline.
                observed = raw_api(api_port, "process-status", "GET",
                                   timeout=min(3, remaining))
                attempts += 1
                last = observed
                probes.write(json.dumps({"attempt": attempts,
                    "observed_unix_ns": time.time_ns(), **observed}) + "\n")
                probes.flush()
                status, reply = observed["http_status"], observed["reply"]
                if status == 503 and reply == {"schema_version": 1,
                        "ok": False, "data": None,
                        "error": "process status is unavailable"}:
                    counts["transient_503"] += 1
                else:
                    assert status == 200 and reply.get("ok") is True, observed
                    value = reply.get("data")
                    assert isinstance(value, dict), observed
                    assert value.get("schema_version") == "1.0.0", observed
                    assert value.get("state") in {"stopped", "running"}, observed
                    assert "active_revision" in value, observed
                    active_revision = value["active_revision"]
                    assert (active_revision is None or
                        isinstance(active_revision, str) and
                        re.fullmatch(r"[0-9a-f]{64}", active_revision)), observed
                    if value["state"] == "running":
                        assert active_revision == revision, observed
                    processes = value.get("processes")
                    assert isinstance(processes, list) and all(
                        isinstance(process, dict) and
                        isinstance(process.get("id"), str) and process["id"] and
                        type(process.get("pid")) is int and process["pid"] > 0 and
                        process.get("state") in {"running", "exited"}
                        for process in processes), observed
                    if (value.get("state") == "running"
                            and {"eos", "bridge", "egia"} <= {
                                process["id"] for process in processes}):
                        counts["ready"] += 1
                        return value
                    counts["valid_not_ready"] += 1
                time.sleep(min(.05, max(0, deadline - time.monotonic())))
        raise AssertionError(f"initial readiness did not converge in {seconds}s: {last!r}")
    finally:
        save(out, "readiness-probes-summary.json", {"attempts": attempts,
            "counts": counts, "last": last, "deadline_seconds": seconds})


def proc_identity(pid: int) -> dict:
    raw = Path(f"/proc/{pid}/stat").read_text()
    fields = raw[raw.rfind(")") + 2:].split()
    return {"pid": pid, "state": fields[0], "ppid": int(fields[1]),
            "pgrp": int(fields[2]), "session": int(fields[3]),
            "start_ticks": int(fields[19])}


def live(identity: dict) -> bool:
    try:
        actual = proc_identity(identity["pid"])
    except (FileNotFoundError, ProcessLookupError):
        return False
    return actual["start_ticks"] == identity["start_ticks"] and actual["state"] != "Z"


def signal_checked(identity: dict, sig: signal.Signals) -> dict:
    before = proc_identity(identity["pid"])
    assert before["start_ticks"] == identity["start_ticks"]
    assert before["ppid"] == identity["ppid"]
    assert before["pgrp"] == identity["pgrp"]
    assert before["session"] == identity["session"]
    assert before["state"] not in {"Z", "X"}
    at_ns = time.time_ns()
    os.kill(identity["pid"], sig)
    return {"identity": before, "signal": sig.name, "sent_unix_ns": at_ns,
            "pid_only": True}


def children(pid: int) -> list[dict]:
    path = Path(f"/proc/{pid}/task/{pid}/children")
    return [proc_identity(int(value)) for value in path.read_text().split()]


def until(predicate, *, seconds: float, interval: float = .05):
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        try:
            last = predicate()
            if last:
                return last
        except (OSError, ValueError, KeyError):
            pass
        time.sleep(interval)
    raise AssertionError(f"condition did not converge in {seconds}s: {last!r}")


def source_and_binary_receipts(a, out: Path) -> dict:
    receipts = {}
    selected = {"isos": a.isos_root, "eos": a.eos_root,
                "egia": a.egia_root, "mujoco": a.mujoco_source,
                "isosui": a.ui_root}
    snapshots = out / "source-snapshot"
    snapshots.mkdir()
    for owner, root in selected.items():
        assert git(root, "status", "--porcelain", "--untracked-files=no") == "", owner
        source = snapshots / f"{owner}.tar"
        with source.open("wb") as file:
            subprocess.run(["git", "archive", "--format=tar", "HEAD"], cwd=root,
                           stdout=file, check=True)
        receipts[owner] = {"commit": git(root, "rev-parse", "HEAD"),
                           "tree": git(root, "rev-parse", "HEAD^{tree}"),
                           "source_sha256": sha(source)}
        lock = root / ({"egia": "uv.lock", "isosui": "package-lock.json"}
                       .get(owner, "Cargo.lock"))
        if lock.is_file():
            shutil.copy2(lock, snapshots / f"{owner}-{lock.name}")
            receipts[owner]["lock_sha256"] = sha(lock)
    commands = {"isos": [str(a.cargo), "build", "--locked", "--bin", "isos"],
                "eos": [str(a.cargo), "build", "--locked", "--bins"]}
    for owner, argv in commands.items():
        log = out / f"{owner}-selected-build.log"
        with log.open("wb") as stream:
            result = subprocess.run(argv, cwd=selected[owner], stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=900,
                                    check=False)
        assert result.returncode == 0, (owner, result.returncode, log)
        receipts[owner]["build_command"] = argv
        receipts[owner]["build_log_sha256"] = sha(log)
    binaries = out / "selected-binaries"
    binaries.mkdir()
    assert a.isos_bin.samefile(a.isos_root / "target/debug/isos")
    assert a.eos_bin_dir == a.eos_root / "target/debug"
    for name, path in {"isos": a.isos_bin,
                       **{name: a.eos_bin_dir / name for name in BINARIES},
                       "egia": a.egia_root / "bin/egia",
                       "egia-python": a.egia_python}.items():
        shutil.copy2(path, binaries / name)
    receipts["binary_sha256"] = {path.name: sha(path) for path in binaries.iterdir()}
    receipts["egia"]["selected_interpreter"] = str(a.egia_python)
    receipts["egia"]["selected_interpreter_sha256"] = sha(a.egia_python)
    assert a.ui_dist == a.ui_root / "dist/local"
    ui_build = ["npm", "run", "build:local"]
    ui_log = out / "isosui-selected-build.log"
    with ui_log.open("wb") as stream:
        result = subprocess.run(ui_build, cwd=a.ui_root, stdout=stream,
                                stderr=subprocess.STDOUT, timeout=300, check=False)
    assert result.returncode == 0, (result.returncode, ui_log)
    receipts["isosui"]["build_command"] = ui_build
    receipts["isosui"]["build_log_sha256"] = sha(ui_log)
    ui_archive = out / "selected-ui-dist"
    shutil.copytree(a.ui_dist, ui_archive, symlinks=False)
    receipts["isosui"]["dist_sha256"] = {
        path.relative_to(ui_archive).as_posix(): sha(path)
        for path in ui_archive.rglob("*") if path.is_file()}
    shutil.copy2(a.bundle / "release.json", out / "package-release.json")
    shutil.copy2(a.asset, out / "package-asset.tar.gz")
    release = json.loads((a.bundle / "release.json").read_text())
    assert release["source_commit"] == receipts["mujoco"]["commit"]
    assert any(item["sha256"] == sha(out / "package-asset.tar.gz")
               for item in release["assets"])
    receipts["package"] = {"release_sha256": sha(out / "package-release.json"),
                            "asset_sha256": sha(out / "package-asset.tar.gz"),
                            "source_commit": release["source_commit"],
                            "executable_sha256": sha(a.bundle / "bin/eos-mujoco-sim")}
    save(out, "selected-inputs.json", receipts)
    return receipts


def environment(a, home: Path) -> tuple[dict, int, int]:
    api_port, grpc_port = port(), port()
    env = dict(os.environ, ISOS_HOME=str(home), ISOS_HTTP_PORT=str(api_port),
               ISOSUI_PORT=str(port()), ISOS_AETHER_BIN_DIR=str(a.eos_bin_dir),
               ISOSUI_DIST_DIR=str(a.output / "selected-ui-dist"),
               MUJOCO_ROOT=str(a.mujoco_runtime),
               ISOS_EGIA_BIN_DIR=str(a.egia_root / "bin"),
               ISOS_EGIA_DEFAULTS_DIR=str(a.egia_root / "etc/egia"),
               ISOS_EGIA_FACTORY_DEFAULTS_DIR=str(a.egia_root / "share/defaults/egia"),
               EGIA_PYTHON=str(a.egia_python), ISLAND_PREFIX=str(a.egia_root))
    return env, api_port, grpc_port


def prepare_graph(a, home: Path, env: dict, api_port: int, grpc_port: int,
                  out: Path) -> tuple[str, dict]:
    def command(label: str, *argv: str):
        result = subprocess.run([str(a.isos_bin), *argv], env=env, text=True,
                                capture_output=True, timeout=90, check=False)
        (out / f"{label}.stdout").write_text(result.stdout)
        (out / f"{label}.stderr").write_text(result.stderr)
        assert result.returncode == 0, (label, result.stderr)
    command("init", "init", "--mode", "dev")
    command("pkg-link", "pkg", "link", "island/mujoco-sim", "0.1.1",
            str(a.bundle), "--manifest", str(a.bundle / "release.json"))
    command("pkg-list", "pkg", "list", "--json")
    receipt = home / "data/packages/island/mujoco-sim/0.1.1/receipt.json"
    shutil.copy2(receipt, out / "package-receipt.raw.json")
    shutil.copy2(receipt.parent / "release.json", out / "installed-release.raw.json")
    config = home / "config/selected-island.toml"
    source = (a.egia_root / "etc/island.toml").read_text()
    relative = os.path.relpath(a.eos_root / "share", a.egia_root)
    assert source.count('"../Island-eos/share"') == 1
    config.write_text(source.replace('"../Island-eos/share"', f'"{relative}"'))
    env["ISLAND_CONFIG"] = str(config)
    shutil.copy2(config, out / "selected-island.toml")
    server_log = (out / "serve.log").open("wb")
    server = subprocess.Popen([str(a.isos_bin), "serve"], env=env,
                              stdout=server_log, stderr=subprocess.STDOUT)
    try:
        until(lambda: (home / "run/manager.sock").exists(), seconds=25)
        graph = api(api_port, "node-graph")["graph"]
        graph = api(api_port, "node-sets/preview", "POST", {
            "package": {"id": "island/mujoco-sim", "version": "0.1.1"},
            "set_id": "sim-hardware", "graph": graph,
            "values": {"scene": "pick-place", "socket": str(home / "run/mujoco.sock"),
                       "camera_device": "cam-0", "base_device": "base-0",
                       "manipulator_device": "arm-0"}})["graph"]
        graph["cores"]["eos"]["layers"]["process"]["groups"]["mujoco"]["nodes"]["simulator"]["settings"]["env"] = {"MUJOCO_GUI": "0"}
        components = graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]
        components["eos"]["settings"]["args"] += ["--grpc-port", str(grpc_port)]
        for name in ("bridge", "config"):
            value = port()
            components[name]["settings"]["args"][-1] = f"127.0.0.1:{value}"
            components[name]["settings"]["ready_tcp"] = f"127.0.0.1:{value}"
        fixture = json.loads((a.isos_root / "tests/fixtures/egia_stage_reader/valid_egia.json").read_text())
        graph["cores"]["egia"] = fixture["cores"]["egia"]
        graph["cores"]["egia"]["layers"]["io"]["groups"]["link"]["nodes"]["gateway"]["settings"]["gateway"]["port"] = grpc_port
        components["egia"] = {"id": "egia", "node_type": "isos.process.component@v0.1",
            "label": "EGIA agent", "description": "Q01 R06 dependent fault",
            "capabilities": ["process"], "settings": {"role": "egia", "args": [],
            "depends_on": ["cores/isos/layers/process/groups/component/nodes/bridge"]}}
        saved = api(api_port, "node-graph")
        validated = api(api_port, "node-graph/validate", "POST",
                        {"schema_version": 1, "graph": graph})
        save(out, "validation.json", validated)
        save(out, "candidate-graph.json", graph)
        if not validated["valid"]:
            probe = subprocess.run([str(a.egia_root / "bin/egia-config"), "setup", "--json"],
                input=json.dumps({"schema_version": 1, "op": "identity", "core": "egia"}),
                env=env, text=True, capture_output=True, timeout=10, check=False)
            save(out, "egia-identity-direct.json", {"exit": probe.returncode,
                "stdout": probe.stdout, "stderr": probe.stderr,
                "environment": {key: env.get(key) for key in (
                    "ISLAND_PREFIX", "EGIA_PYTHON", "ISOS_EGIA_BIN_DIR")}})
        assert validated["valid"], validated
        applied = api(api_port, "node-graph", "PUT", {"schema_version": 1,
                      "revision": saved["revision"], "graph": graph})
        revision = applied["revision"]
        graph_path = home / "config/deployments/default.nodes.json"
        (out / "saved-graph.raw.json").write_bytes(graph_path.read_bytes())
        assert sha(graph_path) == revision
        return revision, graph, server, server_log
    except BaseException:
        server.terminate()
        server.wait(timeout=10)
        server_log.close()
        raise


async def admit_once(grpc_port: int, grant: Path, revision: str, label: str) -> dict:
    from egia.core.link.client import EosClient
    from egia.lib.config import GatewayConfig
    os.environ["EGIA_EOS_CONTROL_GRANT_FILE"] = str(grant)
    client = EosClient(GatewayConfig(host="127.0.0.1", grpc_port=grpc_port),
                       backend="default", expected_revision=revision)
    await client.initialize(force=True)
    command_id = f"q01-r06-{label}-{time.time_ns()}"
    try:
        ack = await client.call_primitive("base.velocity", "drive", {
            "linear_x_m_s": .05, "linear_y_m_s": 0., "angular_z_rad_s": 0.,
            "valid_for_ms": 100}, command_id=command_id)
        assert ack.accepted and ack.status.value == "succeeded", ack
        return {"command_id": command_id, "accepted": ack.accepted,
                "status": ack.status.value}
    finally:
        await client.aclose()


def process_snapshot(home: Path, status: dict) -> dict:
    rows = {item["id"]: proc_identity(item["pid"]) for item in status["processes"]}
    eos = rows["eos"]
    engine = [item for item in children(eos["pid"])
              if item["ppid"] == eos["pid"]]
    assert len(engine) == 1, engine
    return {"direct_children": rows, "eos_child": engine[0],
            "socket_exists": (home / "run/mujoco.sock").exists(),
            "eos_child_exe_sha256": hashlib.sha256(
                Path(f"/proc/{engine[0]['pid']}/exe").read_bytes()).hexdigest(),
            "eos_child_cmdline": Path(f"/proc/{engine[0]['pid']}/cmdline").read_bytes().split(b"\0")[0].decode()}


def events(home: Path, run_id: str) -> list[dict]:
    source = home / "data/logs/default/events.log"
    if not source.is_file():
        return []
    return [event for line in source.read_text().splitlines()
            if (event := json.loads(line)).get("run_id") == run_id]


def reap_observation(snapshot: dict, home: Path) -> dict:
    selected = {**snapshot["direct_children"], "mujoco": snapshot["eos_child"]}
    until(lambda: all(not live(identity) for identity in selected.values()),
          seconds=20)
    return {"identities": selected,
            "alive": {name: live(identity) for name, identity in selected.items()},
            "mujoco_socket_exists": (home / "run/mujoco.sock").exists()}


def copy_owner_state(home: Path, out: Path, run_id: str) -> None:
    for source, target in (
        (home / "data/stops/default" / f"{run_id}.json", "stop-record.raw.json"),
        (home / "data/logs/default/events.log", "events.raw.jsonl"),
        (home / "data/journal/default/.control", "journal-control.raw.json"),
    ):
        if source.is_file():
            shutil.copy2(source, out / target)
    journal = home / "data/journal/default"
    records = out / "journal"
    records.mkdir(exist_ok=True)
    for source in journal.glob("*.rec"):
        shutil.copy2(source, records / source.name)
    save(out, "journal-sha256.json", {path.name: sha(path)
        for path in records.iterdir() if path.is_file()})
    owner_logs = out / "owner-logs"
    owner_logs.mkdir(exist_ok=True)
    for name in ("eos", "isos", "mujoco", "egia"):
        source = home / "data/logs/default" / f"{name}.log"
        if source.is_file():
            shutil.copy2(source, owner_logs / f"{name}.log")


def assert_unknown(api_port: int, revision: str, home: Path, out: Path,
                   run_id: str, snapshot: dict, terminal: dict,
                   response: dict | None) -> None:
    if response is not None:
        assert response["http_status"] == 200 and response["reply"]["ok"] is True, response
        assert response["reply"]["data"]["terminal"] == terminal
    assert terminal["run_id"] == run_id and terminal["outcome"] == "unknown"
    assert terminal["reason_code"] is not None
    status = api(api_port, "process-status")
    assert status["state"] == "degraded" and status["terminal"] == terminal
    expected_error = ("managed child exited unexpectedly; dependent stop is unconfirmed"
        if out.name == "selected-child-fault" else
        "dependent stop unconfirmed; owner recovery required")
    assert status["last_error"] == expected_error, status
    record_path = home / "data/stops/default" / f"{run_id}.json"
    record = json.loads(record_path.read_text())
    assert record["phase"] == "stopping" and record["outcome"] == "unknown"
    assert record["run_id"] == run_id
    selected = record["selected_processes"]
    assert [item["id"] for item in terminal["processes"]] == selected
    assert all(item["outcome"] in {"confirmed", "unknown"} for item in terminal["processes"])
    same_run = until(lambda: (items if any(item["event_code"] ==
        "deployment_stop_unconfirmed" for item in items) else None)
        if (items := events(home, run_id)) else None, seconds=10)
    stopping = [item for item in same_run if item["event_code"] == "deployment_stopping"]
    unconfirmed = [item for item in same_run if item["event_code"] == "deployment_stop_unconfirmed"]
    assert len(stopping) == len(unconfirmed) == 1, (stopping, unconfirmed)
    assert stopping[0]["correlation_id"] == unconfirmed[0]["correlation_id"]
    save(out, "status-degraded.json", status)
    if response is not None:
        save(out, "stop-response.json", response)
    save(out, "stop-events.json", {"stopping": stopping[0],
                                   "unconfirmed": unconfirmed[0]})
    save(out, "reap.json", reap_observation(snapshot, home))
    for operation in ("run", "restart"):
        blocked = raw_api(api_port, "process-control", "POST", {"schema_version": 1,
            "operation": operation, "revision": revision}, timeout=30)
        save(out, f"blocked-{operation}.json", blocked)
        assert blocked["http_status"] == 422 and blocked["reply"]["data"]["diagnostics"][0]["code"] == "deployment_stop_unconfirmed"
    repeated = raw_api(api_port, "process-control", "POST", {"schema_version": 1,
        "operation": "stop", "revision": revision})
    assert repeated["http_status"] == 200 and repeated["reply"]["data"]["terminal"] == terminal
    save(out, "repeated-stop.json", repeated)
    assert api(api_port, "process-status")["terminal"] == terminal


def live_case(a, root: Path, label: str) -> None:
    root.mkdir()
    home = replay_home(f"q01r06-{os.getpid()}-{label[:3]}")
    assert not home.exists(), home
    save(root, "home-path.json", {"path": str(home)})
    env, api_port, grpc_port = environment(a, home)
    revision, _graph, server, server_log = prepare_graph(a, home, env, api_port,
                                                         grpc_port, root)
    environment_keys = ("ISOS_HOME", "ISOS_HTTP_PORT", "ISOSUI_PORT",
                        "ISOS_AETHER_BIN_DIR", "ISOSUI_DIST_DIR", "MUJOCO_ROOT",
                        "ISOS_EGIA_BIN_DIR", "ISOS_EGIA_DEFAULTS_DIR",
                        "ISOS_EGIA_FACTORY_DEFAULTS_DIR", "EGIA_PYTHON",
                        "ISLAND_PREFIX", "ISLAND_CONFIG")
    save(root, "runtime-environment.json", {key: env[key] for key in environment_keys})
    run_log = (root / "run.log").open("wb")
    run = subprocess.Popen([str(a.isos_bin), "run"], env=env, stdout=run_log,
                           stderr=subprocess.STDOUT)
    try:
        status = wait_initial_ready(api_port, revision, run, root)
        save(root, "status-running.json", status)
        snapshot = process_snapshot(home, status)
        assert snapshot["socket_exists"]
        assert snapshot["eos_child_exe_sha256"] == sha(a.bundle / "bin/eos-mujoco-sim")
        save(root, "process-before.json", snapshot)
        grant = home / "run/control-grants/default" / status["run_id"] / "egia-control.grant"
        os.environ["ISLAND_CONFIG"] = env["ISLAND_CONFIG"]
        os.environ["ISLAND_PREFIX"] = str(a.egia_root)
        admitted = asyncio.run(admit_once(grpc_port, grant, revision, label))
        save(root, "admitted-command.json", admitted)
        command_record = home / "data/journal/default" / (admitted["command_id"].encode().hex() + ".rec")
        assert command_record.is_file(), command_record
        shutil.copy2(command_record, root / "admitted-command.rec")
        graph_digest = sha(home / "config/deployments/default.nodes.json")
        grant_digest = sha(grant)
        save(root, "pre-fault-state.json", {"revision": revision,
            "graph_sha256": graph_digest, "grant_sha256": grant_digest,
            "run_id": status["run_id"], "journal_record_sha256": sha(command_record)})
        if label == "selected-child-fault":
            fault = signal_checked(snapshot["eos_child"], signal.SIGKILL)
            save(root, "fault-signal.json", fault)
            response = until(lambda: (reply if reply["state"] == "degraded" and
                reply["terminal"]["outcome"] == "unknown" else None)
                if (reply := api(api_port, "process-status")) else None,
                seconds=90)
            terminal = response["terminal"]
            # The daemon's dependent-stop path is spontaneous; retain the
            # actual status and never claim an HTTP Stop that was not sent.
            save(root, "spontaneous-status.json", response)
            response = None
        else:
            fault = signal_checked(snapshot["direct_children"]["eos"], signal.SIGSTOP)
            save(root, "fault-signal.json", fault)
            box: dict = {}
            def issue_stop():
                try:
                    box["response"] = raw_api(api_port, "process-control", "POST", {
                        "schema_version": 1, "operation": "stop",
                        "revision": revision}, timeout=180)
                except Exception as exc:  # noqa: BLE001 - capture thread failure for the barrier
                    box["error"] = f"{type(exc).__name__}: {exc}"
            worker = threading.Thread(target=issue_stop, daemon=True)
            worker.start()
            record_path = home / "data/stops/default" / f"{status['run_id']}.json"
            until(lambda: record_path.exists() and
                json.loads(record_path.read_text()).get("phase") == "stopping"
                and any(item["event_code"] == "deployment_stopping"
                        for item in events(home, status["run_id"])), seconds=30)
            save(root, "stop-barrier.json", {"record": json.loads(record_path.read_text()),
                 "stopping_event": next(item for item in events(home, status["run_id"])
                     if item["event_code"] == "deployment_stopping")})
            kill = signal_checked(snapshot["direct_children"]["eos"], signal.SIGKILL)
            save(root, "kill-signal.json", kill)
            worker.join(timeout=180)
            assert not worker.is_alive() and "response" in box, box
            response = box["response"]
            terminal = response["reply"]["data"]["terminal"]
        assert_unknown(api_port, revision, home, root, status["run_id"], snapshot,
                       terminal, response)
        assert sha(home / "config/deployments/default.nodes.json") == graph_digest
        assert sha(command_record) == sha(root / "admitted-command.rec")
        # A revoked grant may be absent after stop; its pre-fault digest is
        # retained without copying the bearer into the evidence archive.
        save(root, "post-fault-state.json", {"graph_sha256": graph_digest,
            "run_id": api(api_port, "process-status")["run_id"],
            "new_child_alive": any(live(item) for item in snapshot["direct_children"].values()),
            "journal_record_sha256": sha(command_record)})
        copy_owner_state(home, root, status["run_id"])
        shutdown = subprocess.run([str(a.isos_bin), "stop"], env=env,
                                  text=True, capture_output=True, timeout=30,
                                  check=False)
        (root / "manager-shutdown.stdout").write_text(shutdown.stdout)
        (root / "manager-shutdown.stderr").write_text(shutdown.stderr)
        # The owner CLI returns an error when a known unknown terminal remains.
        assert shutdown.returncode == 1, shutdown.stderr
        assert shutdown.stderr.strip() == "isos: dependent stop unconfirmed"
        shutdown_body = json.loads(shutdown.stdout)
        assert shutdown_body["manager"] == "stopping"
        assert shutdown_body["terminal"] == [terminal]
        serve_exit = server.wait(timeout=15)
        save(root, "manager-serve-exit.json", {"exit_code": serve_exit})
        assert serve_exit == 1
        assert "isos: dependent stop unconfirmed during manager shutdown" in \
            (root / "serve.log").read_text()
        server_log.close()
        server_log = (root / "serve-restarted.log").open("wb")
        server = subprocess.Popen([str(a.isos_bin), "serve"], env=env,
                                  stdout=server_log, stderr=subprocess.STDOUT)
        until(lambda: server.poll() is None and
              (home / "run/manager.sock").exists(), seconds=25)
        recovered = wait_manager_restart_http(api_port, server, root)
        assert recovered["state"] == "degraded"
        assert recovered["run_id"] == status["run_id"]
        assert recovered["terminal"]["outcome"] == "unknown"
        assert recovered["terminal"]["reason_code"] in {
            "stop_interrupted", "stop_result_persist_failed"}
        assert not recovered["processes"]
        save(root, "status-manager-restarted.json", recovered)
        for operation in ("run", "restart"):
            blocked = raw_api(api_port, "process-control", "POST", {
                "schema_version": 1, "operation": operation,
                "revision": revision}, timeout=30)
            assert blocked["http_status"] == 422
            assert blocked["reply"]["data"]["diagnostics"][0]["code"] == "deployment_stop_unconfirmed"
            save(root, f"manager-restarted-blocked-{operation}.json", blocked)
        assert sha(home / "config/deployments/default.nodes.json") == graph_digest
        copy_owner_state(home, root, status["run_id"])
    finally:
        if server.poll() is None:
            try:
                api(api_port, "process-control", "POST", {"schema_version": 1,
                    "operation": "stop", "revision": revision}, checked=False,
                    timeout=15)
            except Exception as exc:  # noqa: BLE001 - best effort after failed trials
                (root / "cleanup-stop-error.txt").write_text(
                    f"{type(exc).__name__}: {exc}\n")
            server.terminate()
            server.wait(timeout=15)
        if run.poll() is None:
            run.terminate()
            run.wait(timeout=15)
        run_log.close()
        server_log.close()


def egia_case(a, root: Path) -> None:
    root.mkdir()
    command = [str(a.egia_test_python), "-m", "pytest", "-q", "-s",
        "tests/egia/test_a03_process_restart.py", "--basetemp", str(root / "pytest-temp")]
    env = dict(os.environ, A03_EVIDENCE_DIR=str(root / "worker-evidence"),
               PYTHONPATH=f"{a.egia_root / 'src'}:{a.egia_root}",
               ISLAND_CONFIG=str(root.parent / "selected-child-fault/selected-island.toml"),
               ISLAND_PREFIX=str(a.egia_root))
    log = root / "pytest.log"
    with log.open("wb") as output:
        result = subprocess.run(command, cwd=a.egia_root, env=env,
                                stdout=output, stderr=subprocess.STDOUT,
                                timeout=180, check=False)
    save(root, "test-command.json", {"argv": command,
         "exit_code": result.returncode, "log_sha256": sha(log)})
    assert result.returncode == 0, log
    for label in ("positive-before", "positive-after", "negative-before",
                  "negative-after", "negative-refused"):
        assert (root / "worker-evidence" / label / "manifest.json").is_file(), label
    cases = {"recovered": ("test_pending_receipt_settles_a0", "owner-state"),
             "missing": ("test_missing_receipt_stays_pen0", "unresolved-state")}
    (root / "owner-state").mkdir(mode=0o700)
    for label, (fixture, state_name) in cases.items():
        state = root / "pytest-temp" / fixture / state_name
        assert state.is_dir(), state
        target = root / "owner-state" / label
        target.mkdir(parents=True, mode=0o700)
        modes = {}
        operation_id = "a03-process-terminal" if label == "recovered" else "a03-process-unknown"
        allowed = [Path("operations-history.json"),
                   Path("operations") / f"{operation_id}.index",
                   Path("operations") / f"{operation_id}.json"]
        if label == "missing":
            allowed += [Path("safety/safety-history.json"),
                        Path("safety/safety-latch.json")]
        for relative in allowed:
            source = state / relative
            assert source.is_file() and not source.is_symlink(), source
            assert source.stat().st_uid == os.getuid(), source
            assert source.stat().st_mode & 0o777 == 0o600, source
            destination = target / relative
            destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            shutil.copy2(source, destination)
            modes[str(relative)] = source.stat().st_mode & 0o777
        save(target, "modes.json", modes)
    # Pytest's current aliases are absolute symlinks into this disposable
    # basetemp.  The selected owner bytes above are the retained evidence.
    shutil.rmtree(root / "pytest-temp")


def tamper_checks(out: Path, selection=None) -> None:
    """Reseal changed archives; each semantic mismatch must still be denied."""
    from verify_q01_r06_a03 import verify

    mutations = {
        "receipt-outcome": ("receipt-loss/stop-response.json",
            lambda value: value["reply"]["data"]["terminal"].update(outcome="confirmed")),
        "terminal-reason": ("receipt-loss/status-degraded.json",
            lambda value: value["terminal"].update(reason_code=None)),
        "child-survivor": ("selected-child-fault/reap.json",
            lambda value: value["alive"].update(mujoco=True)),
        "restart-allowed": ("receipt-loss/blocked-restart.json",
            lambda value: value.update(http_status=200)),
        "checkpoint-id": ("egia-process-restart/owner-state/recovered/operations/a03-process-terminal.json",
            lambda value: value.update(operation_id="a03-forged-id")),
        "latch-episode": ("egia-process-restart/owner-state/missing/safety/safety-latch.json",
            lambda value: value.update(episode_id="f" * 32)),
        "fault-pid-start": ("selected-child-fault/fault-signal.json",
            lambda value: value["identity"].update(start_ticks=1)),
        "manager-restart-allowed": ("receipt-loss/manager-restarted-blocked-run.json",
            lambda value: value.update(http_status=200)),
        "snapshot-checkpoint-id": (
            "egia-process-restart/worker-evidence/positive-before/operations/a03-process-terminal.json",
            lambda value: value.update(operation_id="a03-forged-id")),
        "snapshot-index-sequence": (
            "egia-process-restart/worker-evidence/negative-after/operations/a03-process-unknown.index",
            lambda value: value.update(sequence=2)),
        "snapshot-latch-episode": (
            "egia-process-restart/worker-evidence/negative-refused/safety/safety-latch.json",
            lambda value: value.update(episode_id="f" * 32)),
    }
    results = {}
    for name, (relative, mutate) in mutations.items():
        copy = out.parent / f"{out.name}-tamper-{name}"
        assert not copy.exists(), copy
        shutil.copytree(out, copy, copy_function=os.link)
        target = copy / relative
        value = json.loads(target.read_text())
        mutate(value)
        target.unlink()
        save(target.parent, target.name, value)
        if "worker-evidence" in target.parts:
            target.chmod(0o600)
            snapshot = target.parents[1]
            manifest_path = snapshot / "manifest.json"
            snapshot_manifest = json.loads(manifest_path.read_text())
            relative_file = target.relative_to(snapshot).as_posix()
            for entry in snapshot_manifest["files"]:
                if entry["path"] == relative_file:
                    entry["sha256"] = sha(target)
                    entry["bytes"] = target.stat().st_size
            manifest_path.unlink()
            save(snapshot, "manifest.json", snapshot_manifest)
            manifest_path.chmod(0o600)
        (copy / "evidence.sha256.json").unlink()
        save(copy, "evidence.sha256.json", {path.relative_to(copy).as_posix(): sha(path)
            for path in copy.rglob("*") if path.is_file() and path.name != "evidence.sha256.json"})
        try:
            verify(copy, selection=selection)
        except (AssertionError, KeyError, ValueError) as error:
            results[name] = {"rejected": True, "reason": f"{type(error).__name__}: {error}"}
        else:
            raise AssertionError(f"resealed semantic tamper was accepted: {name}")
    save(out.parent, f"{out.name}-tamper-results.json", results)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("isos-root", "eos-root", "egia-root", "mujoco-source", "ui-root",
                 "mujoco-runtime",
                 "isos-bin", "eos-bin-dir", "egia-python", "egia-test-python", "bundle",
                 "ui-dist", "asset", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--cargo", type=Path,
                        default=Path.home() / ".cargo/bin/cargo")
    parser.add_argument('--selection',type=Path)
    parser.add_argument('--expect-selection-sha256')
    a = parser.parse_args()
    assert bool(a.selection) == bool(a.expect_selection_sha256)
    selection = (load_selection(a.selection,a.expect_selection_sha256)
                 if a.selection else None)
    for field in ("isos_root", "eos_root", "egia_root", "mujoco_source", "ui_root",
                  "mujoco_runtime",
                  "isos_bin", "eos_bin_dir", "bundle",
                  "ui_dist", "asset", "output"):
        setattr(a, field, getattr(a, field).resolve())
    # The venv entry is a symlink by design.  Resolving it selects the base
    # interpreter and loses the selected installed dependencies.
    for field in ("egia_python", "egia_test_python"):
        setattr(a, field, getattr(a, field).absolute())
    out = a.output
    out.mkdir(parents=True, exist_ok=False)
    if selection:
        for name, root in (('isos',a.isos_root),('eos',a.eos_root),
                           ('egia',a.egia_root),('ui',a.ui_root),
                           ('mujoco',a.mujoco_source)):
            verify_checkout(selection,name,root)
        check_harness(selection,Path(__file__).parent)
        assert sha(a.bundle/'release.json')==selection['package']['release_sha256']
        assert sha(a.asset)==selection['package']['asset_sha256']
        save(out,'selection-sha256.json',
             {'profile':'selected-q03','sha256':a.expect_selection_sha256})
    save(out, "command-argv.json", sys.argv)
    source_and_binary_receipts(a, out)
    # Launch the archived selected bytes so later source builds cannot replace
    # a binary between the receipt and either fault trial.
    a.isos_bin = out / "selected-binaries/isos"
    a.eos_bin_dir = out / "selected-binaries"
    for case in ("selected-child-fault", "receipt-loss"):
        live_case(a, out / case, case)
    egia_case(a, out / "egia-process-restart")
    save(out, "evidence.sha256.json", {path.relative_to(out).as_posix(): sha(path)
        for path in out.rglob("*") if path.is_file() and path.name != "evidence.sha256.json"})
    from verify_q01_r06_a03 import verify
    verify(out,selection=selection)
    tamper_checks(out,selection=selection)
    print("Q01_R06_A03_SELECTED_PIN_PASS")


if __name__ == "__main__":
    main()
