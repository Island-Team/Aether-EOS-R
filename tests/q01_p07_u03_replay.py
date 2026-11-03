#!/usr/bin/env python3
"""Selected-pin Q01 P07 live rotation and U03 mounted-owner test replay."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import tempfile
import time
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

from q01_selection import load_selection, verify_checkout

ISOS_PIN = "00c1d7aa184de8b969e6e40472bf9b1fa55032ff"
UI_PIN = "920995788d8e29dd6ab594d31278033bb791cfeb"
SENTINEL = "synthetic_grant_prompt_q01_keep_private"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path: Path, obj: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2, sort_keys=True) + "\n")


def git(root: Path, *args: str) -> str:
    return subprocess.check_output(["git", *args], cwd=root, text=True).strip()


def source_archive(root: Path, target: Path) -> None:
    with target.open("wb") as output:
        result = subprocess.run(["git", "archive", "--format=tar", "HEAD"], cwd=root,
                                stdout=output, stderr=subprocess.PIPE)
    assert result.returncode == 0, result.stderr.decode()


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def api(port: int, route: str, method: str = "GET", body: object = None) -> dict:
    url = f"http://127.0.0.1:{port}/api/isos/v1/{route}"
    req = Request(url, method=method,
                  data=None if body is None else json.dumps(body).encode(),
                  headers={"Origin": f"http://127.0.0.1:{port}", "Content-Type": "application/json"})
    try:
        resp = urlopen(req, timeout=15)
    except HTTPError as error:
        resp = error
    with resp:
        return {"status": resp.status, "body": json.load(resp)}


def append(sock_path: Path) -> dict:
    request = {"schema_version": 1, "op": "log_append", "deployment": "default",
               "log": {"source": "legacy_external", "level": "info",
                       "event_code": "legacy_line", "message": SENTINEL}}
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(15)
        sock.connect(str(sock_path))
        sock.sendall(json.dumps(request).encode() + b"\n")
        reader = sock.makefile("rb")
        result = json.loads(reader.readline())
    assert result["ok"] and result["data"]["accepted"] is True, result
    return result


def tamper_mac(token: str) -> str:
    prefix, body, mac = token.split(".")
    assert prefix == "lc1"
    raw = bytearray(base64.urlsafe_b64decode(mac + "=" * (-len(mac) % 4)))
    assert len(raw) == 32
    raw[0] ^= 1
    changed = base64.urlsafe_b64encode(raw).rstrip(b"=").decode()
    return f"lc1.{body}.{changed}"


def decode_cursor_for_qualification(token: str) -> dict:
    body = token.split(".")[1]
    return json.loads(base64.urlsafe_b64decode(body + "=" * (-len(body) % 4)))


def snapshot(log_root: Path, out: Path, index: int) -> dict:
    record = {}
    for name in ("events.log.1", "events.log"):
        source = log_root / name
        if not source.exists():
            continue
        stat = source.stat()
        identity_raw = subprocess.check_output([str(out / "bin/segment-id"), str(source)], text=True).strip().split()
        assert len(identity_raw) == 4
        device, inode, birth_ns, segment_id = identity_raw
        assert int(device) == stat.st_dev and int(inode) == stat.st_ino
        target = out / "segments" / f"snapshot-{index}" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        record[name] = {"inode": stat.st_ino, "device": stat.st_dev,
                        "birth_ns": int(birth_ns), "segment_id": segment_id,
                        "size": stat.st_size, "sha256": sha(target),
                        "file": target.relative_to(out).as_posix()}
    return record


def run_logged(command: list[str], cwd: Path, out: Path, timeout: int = 600) -> None:
    result = subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=timeout)
    out.write_text("$ " + " ".join(command) + "\n" + result.stdout + f"\nexit={result.returncode}\n")
    assert result.returncode == 0, (out, result.stdout[-3000:])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--isos-root", type=Path, required=True)
    parser.add_argument("--ui-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--selection", type=Path,
                        help="externally frozen Q03 owner selection")
    parser.add_argument("--expect-selection-sha256",
                        help="reviewer-supplied digest of the selection file")
    args = parser.parse_args()
    assert bool(args.selection) == bool(args.expect_selection_sha256), \
        "selection and its external SHA-256 must be supplied together"
    selection = (load_selection(args.selection, args.expect_selection_sha256)
                 if args.selection else None)
    isos, ui, out = args.isos_root.resolve(), args.ui_root.resolve(), args.output.resolve()
    isos_pin = selection["owners"]["isos"]["commit"] if selection else ISOS_PIN
    ui_pin = selection["owners"]["ui"]["commit"] if selection else UI_PIN
    assert git(isos, "rev-parse", "HEAD") == isos_pin
    assert git(ui, "rev-parse", "HEAD") == ui_pin
    assert not git(isos, "status", "--porcelain", "--untracked-files=no")
    assert not git(ui, "status", "--porcelain", "--untracked-files=no")
    if selection is not None:
        verify_checkout(selection, "isos", isos)
        verify_checkout(selection, "ui", ui)
        for name in ("q01_p07_u03_replay.py", "q01_p07_segment_id.rs",
                     "q01_p07_u03_mounted.test.tsx",
                     "q01_p07_workspace_stop.test.tsx"):
            source = Path(__file__).with_name(name)
            assert sha(source) == selection["harness"][f"tests/{name}"], name
    out.mkdir(parents=True, exist_ok=False)
    source_archive(isos, out / "isos-selected-source.tar")
    source_archive(ui, out / "ui-selected-source.tar")
    cargo = str(Path.home() / ".cargo/bin/cargo")
    run_logged([cargo, "build", "--locked", "--bin", "isos"], isos, out / "isos-build.log")
    isos_bin = isos / "target/debug/isos"
    (out / "bin").mkdir()
    helper = Path(__file__).with_name("q01_p07_segment_id.rs")
    shutil.copy2(helper, out / "source-segment-id.rs")
    run_logged([str(Path.home() / ".cargo/bin/rustc"), str(helper), "-o", str(out / "bin/segment-id")],
               Path(__file__).parent, out / "segment-id-build.log")
    shutil.copy2(isos_bin, out / "bin/isos")
    shutil.copy2(isos / "Cargo.lock", out / "isos-Cargo.lock")
    shutil.copy2(ui / "package-lock.json", out / "ui-package-lock.json")
    write(out / "isos-build-receipt.json", {
        "source_commit": isos_pin,
        "source_tree": git(isos, "rev-parse", "HEAD^{tree}"),
        "source_archive_sha256": sha(out / "isos-selected-source.tar"),
        "cargo_lock_sha256": sha(out / "isos-Cargo.lock"),
        "working_directory": str(isos),
        "command": [cargo, "build", "--locked", "--bin", "isos"],
        "cargo_version": subprocess.check_output([cargo, "--version"], text=True).strip(),
        "rustc_version": subprocess.check_output([str(Path.home() / ".cargo/bin/rustc"), "--version"], text=True).strip(),
        "output_binary": "bin/isos", "output_binary_sha256": sha(out / "bin/isos"),
        "exit": 0,
    })
    ui_tests = [
        "src/components/isos/IsosLogsPanel.integration.test.tsx",
        "src/components/isos/WorkspacePanel.test.tsx",
        "src/components/isos/IsosHome.selected-catalog.test.tsx",
        "src/components/isos/NodeGraphManager.selected-catalog.test.tsx",
        "src/components/isos/log-cursor-state.test.ts",
        "src/components/isos/workspace-layout.test.ts",
    ]
    run_logged(["npm", "test", "--", "--reporter", "verbose", *ui_tests], ui, out / "ui-mounted-tests.log")
    overlays = [
        ("q01_p07_u03_mounted.test.tsx", "ui-q01-mounted-overlay.log"),
        ("q01_p07_workspace_stop.test.tsx", "ui-q01-workspace-stop.log"),
    ]
    for overlay_name, log_name in overlays:
        overlay = Path(__file__).with_name(overlay_name)
        overlay_target = ui / "src/components/isos" / overlay_name
        assert not overlay_target.exists(), "qualification overlay would replace owner source"
        shutil.copy2(overlay, overlay_target)
        try:
            run_logged(["npm", "test", "--", "--reporter", "verbose", f"src/components/isos/{overlay_name}"],
                       ui, out / log_name)
        finally:
            overlay_target.unlink()
        shutil.copy2(overlay, out / f"ui-{overlay_name}")
    run_logged(["npm", "run", "build"], ui, out / "ui-build.log")
    write(out / "ui-build-receipt.json", {
        "source_commit": ui_pin, "source_tree": git(ui, "rev-parse", "HEAD^{tree}"),
        "source_archive_sha256": sha(out / "ui-selected-source.tar"),
        "package_lock_sha256": sha(out / "ui-package-lock.json"),
        "working_directory": str(ui), "command": ["npm", "run", "build"],
        "node_version": subprocess.check_output(["node", "--version"], text=True).strip(),
        "npm_version": subprocess.check_output(["npm", "--version"], text=True).strip(),
        "bundles": {p.relative_to(ui).as_posix(): sha(p) for p in (ui / "dist").rglob("*") if p.is_file()},
        "exit": 0,
    })
    ui_sources = {name: sha(ui / name) for name in ui_tests}
    ui_bundles = {p.relative_to(ui).as_posix(): sha(p) for p in (ui / "dist").rglob("*") if p.is_file()}
    pins = {
        "isos_source": isos_pin, "isos_tree": git(isos, "rev-parse", "HEAD^{tree}"),
        "isos_lock_sha256": sha(out / "isos-Cargo.lock"), "isos_binary_sha256": sha(out / "bin/isos"),
        "isos_archive_sha256": sha(out / "isos-selected-source.tar"),
        "ui_source": ui_pin, "ui_tree": git(ui, "rev-parse", "HEAD^{tree}"),
        "ui_working_directory": str(ui),
        "ui_lock_sha256": sha(out / "ui-package-lock.json"),
        "ui_test_sources": ui_sources, "ui_bundles": ui_bundles,
        "ui_overlays_sha256": {name: sha(out / f"ui-{name}") for name, _ in overlays},
        "ui_archive_sha256": sha(out / "ui-selected-source.tar"),
        "segment_helper_source_sha256": sha(out / "source-segment-id.rs"),
        "segment_helper_binary_sha256": sha(out / "bin/segment-id"),
        "environment": {"os": os.uname().sysname, "arch": os.uname().machine,
                        "node": subprocess.check_output(["node", "--version"], text=True).strip(),
                        "npm": subprocess.check_output(["npm", "--version"], text=True).strip()},
    }
    if selection is not None:
        pins["selection_sha256"] = args.expect_selection_sha256
        for name in ui_tests:
            assert pins["ui_test_sources"][name] == \
                selection["owners"]["ui"]["sources"][name], name
    write(out / "selected-pins.json", pins)
    for name in ui_tests:
        target = out / "ui-source" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ui / name, target)
    for name in ui_bundles:
        target = out / "ui-dist" / name.removeprefix("dist/")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ui / name, target)

    with tempfile.TemporaryDirectory(prefix="q01-p07-u03-") as temp:
        home = Path(temp) / "home"
        env = dict(os.environ, ISOS_HOME=str(home), ISOS_HTTP_PORT=str(free_port()),
                   ISOSUI_PORT=str(free_port()), ISOS_LOG_MAX_BYTES="65536",
                   ISOS_LOG_BACKUPS="1", ISOSUI_DIST_DIR=str(ui / "dist/local"))
        port = int(env["ISOS_HTTP_PORT"])
        init = subprocess.run([str(out / "bin/isos"), "init"], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (out / "isos-init.log").write_text(init.stdout + f"\nexit={init.returncode}\n")
        assert init.returncode == 0
        graph_path = home / "config/deployments/default.nodes.json"
        before = graph_path.read_bytes()
        (out / "saved-before.raw.json").write_bytes(before)
        log_root = home / "data/logs/default"
        server = None
        try:
            for session in (1, 2):
                log_file = (out / f"isos-serve-{session}.log").open("w")
                server = subprocess.Popen([str(out / "bin/isos"), "serve"], env=env,
                                          stdout=log_file, stderr=subprocess.STDOUT)
                for _ in range(100):
                    if server.poll() is not None:
                        raise RuntimeError(f"IsOS serve {session} exited")
                    try:
                        api(port, "node-graph")
                        break
                    except OSError:
                        time.sleep(.05)
                else:
                    raise RuntimeError("IsOS serve did not become ready")
                if session == 1:
                    write(out / "status-before.json", api(port, "process-status"))
                    pending = api(port, "logs?" + urlencode({"view": "current", "source": "all"}))
                    write(out / "pending.json", pending)
                    graph = json.loads(before)
                    graph["version"] = "2.0.0"
                    invalid = api(port, "node-graph", "PUT", {"schema_version": 1,
                                  "revision": hashlib.sha256(before).hexdigest(), "graph": graph})
                    write(out / "invalid-apply.json", invalid)
                    write(out / "invalid-candidate.json", graph)
                    found = api(port, "logs?" + urlencode({"view": "current", "source": "all"}))
                    write(out / "found.json", found)
                    shutil.copy2(log_root / "cursor-state.json", out / "cursor-state-found.raw.json")
                    token = found["body"]["data"]["next_cursor"]
                    write(out / "decoded-cursor.json", decode_cursor_for_qualification(token))
                    write(out / "filtered.json", api(port, "logs?" + urlencode({"view": "current", "source": "eos"})))
                    write(out / "mismatch.json", api(port, "logs?" + urlencode({"view": "current", "source": "eos", "cursor": token})))
                    changed = tamper_mac(token)
                    write(out / "tampered-token.json", {"original": token, "tampered": changed})
                    write(out / "tampered-reply.json", api(port, "logs?" + urlencode({"view": "current", "source": "all", "cursor": changed})))
                    snapshots = [snapshot(log_root, out, 0)]
                    previous_inode = snapshots[0]["events.log"]["inode"]
                    rotations = 0
                    socket_path = home / "run/manager.sock"
                    for number in range(1100):
                        append(socket_path)
                        current_inode = (log_root / "events.log").stat().st_ino
                        if current_inode != previous_inode:
                            rotations += 1
                            previous_inode = current_inode
                            snapshots.append(snapshot(log_root, out, rotations))
                        if rotations >= 3:
                            break
                    assert rotations >= 3, rotations
                    write(out / "rotation-snapshots.json", snapshots)
                    shutil.copy2(log_root / "cursor-state.json", out / "cursor-state-pruned.raw.json")
                    write(out / "expired.json", api(port, "logs?" + urlencode({"view": "current", "source": "all", "cursor": token})))
                    (out / "saved-after.raw.json").write_bytes(graph_path.read_bytes())
                    write(out / "status-after.json", api(port, "process-status"))
                else:
                    write(out / "restart-status.json", api(port, "process-status"))
                    write(out / "restart-fresh.json", api(port, "logs?" + urlencode({"view": "current", "source": "all"})))
                    write(out / "restart-old-cursor.json", api(port, "logs?" + urlencode({"view": "current", "source": "all", "cursor": token})))
                    shutil.copy2(log_root / "cursor-state.json", out / "cursor-state-restarted.raw.json")
                server.send_signal(signal.SIGINT)
                server.wait(timeout=15)
                write(out / f"manager-exit-{session}.json", {"exit": server.returncode, "pid": server.pid})
                log_file.close()
                server = None
        finally:
            if server is not None and server.poll() is None:
                server.send_signal(signal.SIGINT)
                server.wait(timeout=15)
        write(out / "home-path.json", {"disposable": True, "manager_sessions": 2})
    manifest = {p.relative_to(out).as_posix(): sha(p) for p in out.rglob("*")
                if p.is_file() and p.name != "evidence.sha256.json"}
    write(out / "evidence.sha256.json", manifest)
    print("Q01_P07_U03_REPLAY_CREATED", out)


if __name__ == "__main__":
    main()
