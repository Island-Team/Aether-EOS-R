#!/usr/bin/env python3
"""Real v0.2 IsOS→EOS supervisor replay with the published MuJoCo package.

Usage: python3 crates/isos/tests/unified_mujoco_supervisor_replay.py OUT RELEASE BUNDLE MUJOCO_ROOT
The ignored Rust snapshot bridge stages a verified candidate without opening public Apply.
"""

import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from generated_config_preflight_cli import CleanHome, ISOS, digest
from real_mujoco_first_use import ASSET_SHA256, MANIFEST_SHA256, digest as file_digest
from unified_owner_replay_cli import EI, BIN, ROOT, unified_graph


def children(pid):
    path = Path(f"/proc/{pid}/task/{pid}/children")
    return [int(value) for value in path.read_text().split()] if path.exists() else []


def bridge_request(route, payload=None):
    request = Request(f"http://127.0.0.1:16020/api/eos/v2/{route}",
                      data=None if payload is None else json.dumps(payload).encode(),
                      headers={"Content-Type": "application/json"})
    try:
        response = urlopen(request, timeout=15)
    except HTTPError as error:
        response = error
    with response:
        return response.status, json.loads(response.read())


def main():
    out, manifest, bundle, runtime = map(lambda value: Path(value).resolve(), sys.argv[1:5])
    assert not out.exists(), "use a fresh evidence directory"
    out.mkdir(parents=True)
    assert file_digest(manifest) == MANIFEST_SHA256
    assert file_digest(bundle.parent / "mujoco-sim-v0.1.1-linux-x86_64.tar.gz") == ASSET_SHA256
    assert (runtime / "lib/libmujoco.so.3.16.0").is_file()
    short_root = Path(tempfile.mkdtemp(prefix="isos-v2-", dir="/tmp"))
    (short_root / "xdg").mkdir()
    case = CleanHome(str(short_root))
    case.env.update({
        "XDG_CONFIG_HOME": str(short_root / "xdg"),
        "ISOS_AETHER_BIN_DIR": str(BIN),
        "ISOS_EGIA_BIN_DIR": str(EI / "bin"),
        "ISOS_EI_BIN_DIR": str(EI / "bin"),
        "ISOS_EI_DEFAULTS_DIR": str(EI / "etc/egia"),
        "ISOS_EI_FACTORY_DEFAULTS_DIR": str(EI / "share/defaults/egia"),
        "EI_PYTHON": str(EI / ".venv/bin/python"),
        "ISLAND_PREFIX": str(EI),
        "MUJOCO_ROOT": str(runtime),
        "LD_LIBRARY_PATH": str(runtime / "lib"),
        "MUJOCO_GL": "egl",
    })
    try:
        case.init()
        case.command("pkg", "link", "island/mujoco-sim", "0.1.1", bundle, "--manifest", manifest)
        case.command("pkg", "verify", manifest,
                     bundle.parent / "mujoco-sim-v0.1.1-linux-x86_64.tar.gz")
        template = Path(__file__).parent / "fixtures/unified_mujoco/legacy_draft.json"
        graph = json.loads(template.read_text().replace("{ISOS_HOME}", str(case.home)))
        graph = unified_graph(graph)
        candidate = out / "candidate.nodes.json"
        candidate.write_text(json.dumps(graph, indent=2, sort_keys=True) + "\n")
        case.command("config", "validate", candidate)
        saved = case.graph_path.read_bytes()
        case.graph_path.unlink()
        staged_env = dict(case.env, ISOS_TEST_UNIFIED_HOME=str(case.home),
                          ISOS_TEST_UNIFIED_GRAPH_PATH=str(candidate))
        stage = subprocess.run([str(Path.home() / ".cargo/bin/cargo"), "test", "-q", "-p", "isos",
                                "--bin", "isos", "unified_snapshot_fixture_for_supervisor_replay",
                                "--", "--ignored", "--nocapture"], cwd=ROOT, env=staged_env,
                               capture_output=True, text=True, timeout=180)
        (out / "stage.log").write_text(stage.stdout + stage.stderr)
        assert stage.returncode == 0, (stage.stdout + stage.stderr)[-3000:]
        revision = digest(case.graph_path.read_bytes())
        assert revision == hashlib.sha256(candidate.read_bytes()).hexdigest()
        case.output = (out / "serve.log").open("w+")
        case.server = subprocess.Popen([str(ISOS), "serve"], env=case.env,
                                       stdout=case.output, stderr=subprocess.STDOUT, text=True)
        for _ in range(200):
            case.output.flush()
            log = (out / "serve.log").read_text()
            match = re.search(r"IsOS API: http://127\.0\.0\.1:(\d+)", log)
            ui = re.search(r"http://127\.0\.0\.1:(\d+)/en/isos", log)
            if match and ui:
                case.port = int(match.group(1))
                def api(route, data=None, method=None, timeout=30):
                    route = {"node-graph": "graph", "process-status": "runtime/process-status",
                             "process-control": "runtime/control"}.get(route, route)
                    payload = None if data is None else json.dumps(data).encode()
                    request = Request(f"http://127.0.0.1:{case.port}/api/isos/v1/{route}",
                                      data=payload, method=method or ("POST" if payload else "GET"),
                                      headers={"Origin": f"http://127.0.0.1:{ui.group(1)}",
                                               "Content-Type": "application/json"})
                    try:
                        response = urlopen(request, timeout=timeout)
                    except HTTPError as error:
                        response = error
                    with response:
                        body = response.read()
                        try:
                            return response.status, json.loads(body)
                        except ValueError as error:
                            raise AssertionError((response.status, body[:300])) from error
                case.api = api
                case.control = lambda operation, timeout=30: case.api(
                    "runtime/control", {"schema_version": "0.1.0", "operation": operation,
                                        "revision": digest(case.graph_path.read_bytes())}, timeout=timeout)
                try:
                    case.api("node-graph")
                    break
                except OSError:
                    pass
            if case.server.poll() is not None:
                raise AssertionError("IsOS server exited before readiness")
            time.sleep(.05)
        else:
            raise AssertionError("IsOS server did not become ready")
        status, reply = case.control("run", timeout=90)
        (out / "run.json").write_text(json.dumps(reply, indent=2))
        assert status == 200, reply
        status, state = case.api("process-status")
        assert status == 200, state
        (out / "running.json").write_text(json.dumps(state, indent=2))
        processes = {entry["id"]: entry for entry in state["data"]["processes"]}
        assert "eos" in processes, processes
        eos_pid = processes["eos"]["pid"]
        simulator_children = children(eos_pid)
        assert simulator_children, "EOS did not directly supervise MuJoCo"
        status, gates = bridge_request("vision/gates")
        (out / "camera-gates.json").write_text(json.dumps(gates, indent=2))
        assert status == 200 and gates["gates"], gates
        camera = "isos/eos_hardware/camera/front"
        status, captured = bridge_request("control", {"op": "capture", "node_path": camera})
        assert status == 200 and captured["ok"] and captured["frame"]["rgb8_base64"], captured
        frame = captured["frame"]
        pixels = base64.b64decode(frame["rgb8_base64"], validate=True)
        (out / "camera-capture.json").write_text(json.dumps({
            "node_path": camera, "width_px": frame["width_px"],
            "height_px": frame["height_px"], "rgb8_sha256": digest(pixels),
            "rgb8_bytes": len(pixels)}, indent=2))
        status, clock = bridge_request("control", {"op": "time"})
        assert status == 200 and clock["ok"], clock
        payload = {"type": "body_velocity", "x_m_s": 0.1, "y_m_s": 0,
                   "yaw_rad_s": 0, "valid_for_ms": 100}
        control = {"op": "primitive_typed", "header": {
            "data_type_id": "base_movement", "data_type_version": "1.0",
            "primitive_name": "base.velocity", "primitive_version": "1.0.0",
            "source_path": "isos/eos_dispatcher/base/base-velocity",
            "target_path": "isos/eos_hardware/base/drive", "active_revision": revision,
            "command_id": "supervisor-first-use", "sequence": 1,
            "deadline_monotonic_ns": clock["monotonic_ns"] + 2_000_000_000,
            "payload_length_bytes": len(json.dumps(payload, separators=(",", ":")).encode()),
            "codec": "json_v1"}, "payload": payload}
        status, admitted = bridge_request("control", control)
        (out / "admitted-control.json").write_text(json.dumps(admitted, indent=2))
        assert status == 200 and admitted["ok"], admitted
        deadline = time.monotonic() + 5
        while True:
            status, result = bridge_request("control", {
                "op": "result", "command_id": "supervisor-first-use",
                "active_revision": revision})
            assert status == 200 and result["ok"], result
            if result["result"]["status"] != "InProgress":
                break
            assert time.monotonic() < deadline, result
            time.sleep(.05)
        (out / "completed-control.json").write_text(json.dumps(result, indent=2))
        assert result["result"]["status"] == "Completed", result
        (out / "first-simulator-pid.txt").write_text(str(simulator_children[0]) + "\n")
        status, restarted = case.control("restart", timeout=90)
        (out / "restart.json").write_text(json.dumps(restarted, indent=2))
        assert status == 200, restarted
        assert not Path(f"/proc/{simulator_children[0]}").exists(), "old simulator survived Restart"
        status, state = case.api("process-status")
        assert status == 200, state
        eos_pid = {entry["id"]: entry for entry in state["data"]["processes"]}["eos"]["pid"]
        simulator_children = children(eos_pid)
        assert simulator_children, "restarted EOS did not supervise MuJoCo"
        status, stopped = case.control("stop", timeout=90)
        (out / "stop.json").write_text(json.dumps(stopped, indent=2))
        assert status == 200, stopped
        assert not Path(f"/proc/{simulator_children[0]}").exists(), "simulator child survived Stop"
        status, reply = case.control("run", timeout=90)
        (out / "failure-run.json").write_text(json.dumps(reply, indent=2))
        assert status == 200, reply
        status, state = case.api("process-status")
        assert status == 200, state
        eos_pid = {entry["id"]: entry for entry in state["data"]["processes"]}["eos"]["pid"]
        simulator_children = children(eos_pid)
        assert simulator_children, "failure run did not supervise MuJoCo"
        (out / "failed-simulator-pid.txt").write_text(str(simulator_children[0]) + "\n")
        os.kill(simulator_children[0], signal.SIGKILL)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and Path(f"/proc/{simulator_children[0]}").exists():
            time.sleep(.05)
        status, failed = case.api("process-status")
        (out / "after-child-failure.json").write_text(json.dumps(failed, indent=2))
        assert status == 200
        assert failed["data"]["state"] == "degraded", failed
        assert failed["data"]["terminal"]["outcome"] == "unknown", failed
        status, rejected = case.control("restart", timeout=90)
        (out / "rejected-restart.json").write_text(json.dumps(rejected, indent=2))
        assert status != 200 and rejected["error"]["code"] == "deployment_stop_unconfirmed", rejected
        assert not Path(f"/proc/{simulator_children[0]}").exists(), "failed child remains"
        assert case.graph_path.read_bytes() == candidate.read_bytes()
        print("v0.2 real owner Run → Restart → Stop; failure rejects unsafe Restart")
    finally:
        case.close()
        shutil.copytree(case.home, out / "home", dirs_exist_ok=True, symlinks=True)
        shutil.rmtree(short_root)


if __name__ == "__main__":
    main()
