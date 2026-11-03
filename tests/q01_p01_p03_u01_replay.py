#!/usr/bin/env python3
"""Live selected-pin P01/P03/U01 negative replay in disposable IsOS homes."""

from __future__ import annotations

import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import mimetypes
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from q01_selection import load_selection, verify_checkout

PINS = {"isos": "00c1d7aa184de8b969e6e40472bf9b1fa55032ff",
        "mujoco": "4e6b5449ccd25be53ad95b13baa2c89f7ed91e10",
        "ui": "920995788d8e29dd6ab594d31278033bb791cfeb"}
PROCESS = "/cores/eos/layers/process/groups/mujoco/nodes/simulator"
CAMERA = "/cores/eos/layers/hardware/groups/camera/nodes/front"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def git(root: Path, spec: str) -> str:
    return subprocess.check_output(["git", "rev-parse", spec], cwd=root, text=True).strip()


def archive_source(root: Path, output: Path, owner: str) -> None:
    source = output / "sources" / owner
    source.mkdir(parents=True)
    (source / "tracked.tar").write_bytes(subprocess.check_output(
        ["git", "archive", "--format=tar", "HEAD"], cwd=root))
    (source / "commit.raw").write_bytes(subprocess.check_output(
        ["git", "cat-file", "-p", "HEAD"], cwd=root))
    save(source / "identity.json", {"commit": git(root, "HEAD"),
         "tree": git(root, "HEAD^{tree}"), "archive_sha256": sha(source / "tracked.tar"),
         "commit_sha256": sha(source / "commit.raw")})


def port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def api(number: int, route: str, method: str = "GET", value: object = None) -> dict:
    request = Request(f"http://127.0.0.1:{number}/api/isos/v1/{route}", method=method,
                      data=None if value is None else json.dumps(value).encode(),
                      headers={"Content-Type": "application/json",
                               "Origin": f"http://127.0.0.1:{number}"})
    try:
        response = urlopen(request, timeout=30)
    except HTTPError as error:
        response = error
    with response:
        return {"status": response.status, "body": json.loads(response.read())}


def command(binary: Path, env: dict, output: Path, *args: str) -> None:
    run = subprocess.run([str(binary), *args], env=env, text=True, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, timeout=120)
    output.write_text("$ " + " ".join((str(binary), *args)) + "\n" + run.stdout)
    assert run.returncode == 0, (args, run.returncode, run.stdout[-3000:])


def serve(binary: Path, env: dict, log: Path):
    handle = log.open("w")
    server = subprocess.Popen([str(binary), "serve"], env=env, stdout=handle,
                              stderr=subprocess.STDOUT)
    try:
        number = int(env["ISOS_HTTP_PORT"])
        for _ in range(150):
            if server.poll() is not None:
                raise RuntimeError("IsOS exited before API readiness")
            try:
                response = api(number, "node-graph")
                assert response["status"] == 200
                return server, handle
            except OSError:
                time.sleep(.05)
        raise RuntimeError("IsOS API did not become ready")
    except Exception:
        server.send_signal(signal.SIGINT)
        server.wait(timeout=15)
        handle.close()
        raise


def stop(server, handle):
    server.send_signal(signal.SIGINT)
    server.wait(timeout=15)
    handle.close()


def status(number: int) -> dict:
    response = api(number, "process-status")
    assert response["status"] == 200 and response["body"]["ok"]
    return response["body"]["data"]


def graph_state(home: Path, number: int) -> dict:
    response = api(number, "node-graph")
    assert response["status"] == 200 and response["body"]["ok"]
    data = response["body"]["data"]
    assert data["revision"] == sha(home / "config/deployments/default.nodes.json")
    return data


def run_case(label: str, home: Path, binary: Path, base_env: dict, graph: dict,
             output: Path, mutation: str) -> None:
    case = output / label
    case.mkdir()
    env = dict(base_env, ISOS_HOME=str(home), ISOS_HTTP_PORT=str(port()), ISOSUI_PORT=str(port()))
    number = int(env["ISOS_HTTP_PORT"])
    package = home / "data/packages/island/mujoco-sim/0.1.1"
    shutil.copytree(package, case / "installed-before", symlinks=True)
    baseline_receipt = (package / "receipt.json").read_bytes()
    baseline_payload = (package / "LICENSE").read_bytes()
    if mutation == "payload":
        changed = bytearray(baseline_payload)
        assert changed
        changed[0] ^= 1
        (package / "LICENSE").write_bytes(changed)
    elif mutation == "receipt":
        receipt = json.loads(baseline_receipt)
        original = receipt["payload_sha256"]
        receipt["payload_sha256"] = ("0" if original[0] != "0" else "1") + original[1:]
        save(package / "receipt.json", receipt)
    elif mutation == "scene":
        graph["cores"]["eos"]["layers"]["process"]["groups"]["mujoco"]["nodes"]["simulator"]["settings"]["scene"] = "ghost-scene"
    elif mutation == "device":
        graph["cores"]["eos"]["layers"]["hardware"]["groups"]["camera"]["nodes"]["front"]["adapter"]["settings"]["device_id"] = "ghost-device"
    elif mutation != "valid":
        raise AssertionError(mutation)
    (case / "receipt-before.raw.json").write_bytes(baseline_receipt)
    (case / "receipt-after.raw.json").write_bytes((package / "receipt.json").read_bytes())
    (case / "payload-before.raw").write_bytes(baseline_payload)
    (case / "payload-after.raw").write_bytes((package / "LICENSE").read_bytes())
    shutil.copytree(package, case / "installed-after", symlinks=True)
    save(case / "candidate.json", graph)
    server, handle = serve(binary, env, case / "serve.log")
    try:
        before = graph_state(home, number)
        before_status = status(number)
        (case / "saved-before.raw.json").write_bytes((home / "config/deployments/default.nodes.json").read_bytes())
        save(case / "graph-before.json", before)
        save(case / "status-before.json", before_status)
        option = api(number, "node-options", "POST", {"schema_version": 1, "graph": graph})
        save(case / "options.json", option)
        validation = api(number, "node-graph/validate", "POST", {"schema_version": 1, "graph": graph})
        save(case / "validate.json", validation)
        applied = api(number, "node-graph", "PUT", {"schema_version": 1,
                        "revision": before["revision"], "graph": graph})
        save(case / "apply.json", applied)
        after = graph_state(home, number)
        after_status = status(number)
        (case / "saved-after.raw.json").write_bytes((home / "config/deployments/default.nodes.json").read_bytes())
        save(case / "graph-after.json", after)
        save(case / "status-after.json", after_status)
        if mutation == "valid":
            assert validation["body"]["data"]["valid"], validation
            assert applied["status"] == 200, applied
            assert after["revision"] != before["revision"]
            stale = api(number, "node-graph", "PUT", {"schema_version": 1,
                        "revision": before["revision"], "graph": graph})
            save(case / "stale-apply.json", stale)
            assert stale["status"] == 409
            assert graph_state(home, number) == after and status(number) == after_status
            return
        expected = ("package_changed", PROCESS + "/package", "payload") if mutation in ("payload", "receipt") else (
            "mujoco_scene_unavailable", PROCESS + "/settings/scene", "admission") if mutation == "scene" else (
            "mujoco_device_unavailable", CAMERA + "/adapter/settings/device_id", "admission")
        assert validation["status"] == 200 and not validation["body"]["data"]["valid"], validation
        assert applied["status"] == 422, applied
        for response in (validation, applied):
            diagnostics = response["body"]["data"]["diagnostics"]
            assert any((item["code"], item["path"], item["stage"]) == expected for item in diagnostics), diagnostics
        assert before == after and before_status == after_status
        assert (case / "saved-before.raw.json").read_bytes() == (case / "saved-after.raw.json").read_bytes()
        if mutation in ("scene", "device"):
            assert option["status"] == 200, option
            node_path, setting, selected = (
                ("eos/process/mujoco/simulator", "scene", "ghost-scene") if mutation == "scene" else
                ("eos/hardware/camera/front", "adapter.settings.device_id", "ghost-device"))
            groups = [row for row in option["body"]["data"]["setting_options"]
                      if (row["node_path"], row["setting"]) == (node_path, setting)]
            assert len(groups) == 1 and groups[0]["selected"] == selected, groups
            rows = [row for row in groups[0]["options"] if row["value"] == selected]
            assert len(rows) == 1 and rows[0]["available"] is False and rows[0]["reason"] and rows[0]["reason_code"]
    finally:
        stop(server, handle)


def ui_stage(args, output: Path, roots: dict[str, Path], pins: dict[str, str]) -> None:
    """Hold a selected UI/owner server for an external visible browser observation."""
    home = Path(tempfile.mkdtemp(prefix="q01-p01-p03-u01-ui-"))
    number, ui_number, isos_static_number = port(), port(), port()
    env = dict(os.environ, ISOS_HOME=str(home), ISOS_HTTP_PORT=str(number),
               ISOSUI_PORT=str(isos_static_number), ISOS_AETHER_BIN_DIR=str(args.eos_bin_dir.resolve()),
               ISOSUI_DIST_DIR=str((output / "ui-dist").resolve()),
               MUJOCO_ROOT=str(args.mujoco_library_root.resolve()))
    binary = output / "bin/isos"
    (output / "ui-post-options").mkdir()
    command(binary, env, output / "ui-init.log", "init", "--mode", "dev")
    installed = home / "data/packages/island/mujoco-sim/0.1.1"
    installed.parent.mkdir(parents=True)
    shutil.copytree(output / "package-installed", installed)
    draft = obj_from(output / "ghost-device/candidate.json")
    old_home = draft["cores"]["eos"]["layers"]["process"]["groups"]["mujoco"]["nodes"]["simulator"]["settings"]["socket"].removesuffix("/run/mujoco.sock")
    draft = json.loads(json.dumps(draft).replace(old_home, str(home)))
    graph_path = home / "config/deployments/default.nodes.json"
    graph_path.write_text(json.dumps(draft, indent=2) + "\n")
    (output / "ui-home-graph.raw.json").write_bytes(graph_path.read_bytes())
    ui_env = dict(os.environ, ISOSUI_API_ORIGIN=f"http://127.0.0.1:{ui_number}",
                  ISOSUI_EOS_API_ORIGIN="http://127.0.0.1:16020")
    build_command = ["npm", "run", "build:local"]
    build = subprocess.run(build_command, cwd=roots["ui"], env=ui_env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, timeout=300)
    (output / "ui-build.log").write_text("$ " + " ".join(build_command) + "\n" + build.stdout)
    assert build.returncode == 0, build.stdout[-3000:]
    shutil.copytree(roots["ui"] / "dist/local", output / "ui-dist")
    ui_files = {path.relative_to(output / "ui-dist").as_posix(): sha(path)
                for path in (output / "ui-dist").rglob("*") if path.is_file()}
    assert any(ui_env["ISOSUI_API_ORIGIN"].encode() in path.read_bytes()
               for path in (output / "ui-dist/assets").glob("*.js"))
    shutil.copy2(Path(__file__), output / "ui-proxy-harness.py")
    save(output / "ui-build-receipt.json", {"source_commit": pins["ui"],
         "source_tree": git(roots["ui"], "HEAD^{tree}"), "command": build_command,
         "isos_api_origin": ui_env["ISOSUI_API_ORIGIN"], "files_sha256": ui_files,
         "proxy_harness_sha256": sha(output / "ui-proxy-harness.py")})
    server, handle = serve(binary, env, output / "ui-serve.log")
    dist = output / "ui-dist"

    class SameOriginHandler(BaseHTTPRequestHandler):
        post_lock = threading.Lock()
        post_number = 0

        def log_message(self, format_string, *items):
            with (output / "ui-proxy.log").open("a") as log:
                log.write(format_string % items + "\n")

        def do_GET(self):
            self.route()

        def do_POST(self):
            self.route()

        def do_PUT(self):
            self.route()

        def route(self):
            path = self.path.split("?", 1)[0]
            if path.startswith("/api/isos/v1/"):
                size = int(self.headers.get("Content-Length", "0"))
                assert 0 <= size <= 8 * 1024 * 1024
                body = self.rfile.read(size) if size else None
                request = Request(f"http://127.0.0.1:{number}{self.path}",
                                  method=self.command, data=body,
                                  headers={"Content-Type": self.headers.get("Content-Type", "application/json"),
                                           "Origin": f"http://127.0.0.1:{number}"})
                try:
                    response = urlopen(request, timeout=30)
                except HTTPError as error:
                    response = error
                with response:
                    content = response.read()
                    if path == "/api/isos/v1/node-options" and self.command == "POST":
                        with self.post_lock:
                            type(self).post_number += 1
                            index = type(self).post_number
                        assert index <= 128, "UI option requests exceeded evidence bound"
                        prefix = output / "ui-post-options" / f"{index:06d}"
                        (prefix.with_name(prefix.name + "-request.raw.json")).write_bytes(body or b"")
                        (prefix.with_name(prefix.name + "-response.raw.json")).write_bytes(content)
                        save(prefix.with_name(prefix.name + "-meta.json"), {
                            "index": index, "method": self.command, "path": path,
                            "status": response.status,
                            "request_sha256": digest_bytes(body or b""),
                            "response_sha256": digest_bytes(content)})
                    self.send_response(response.status)
                    self.send_header("Content-Type", response.headers.get("Content-Type", "application/json"))
                    self.send_header("Content-Length", str(len(content)))
                    self.end_headers()
                    self.wfile.write(content)
                return
            relative = path.lstrip("/")
            if relative in ("", "en/isos", "cn/isos"):
                relative = "index.html"
            target = dist / relative
            if ".." in Path(relative).parts or not target.is_file() or target.is_symlink():
                self.send_error(404)
                return
            content = target.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", mimetypes.guess_type(target.name)[0] or "application/octet-stream")
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)

    proxy = ThreadingHTTPServer(("127.0.0.1", ui_number), SameOriginHandler)
    thread = threading.Thread(target=proxy.serve_forever, daemon=True)
    thread.start()
    try:
        owner_options = api(number, "node-options")
        save(output / "ui-owner-options.json", owner_options)
        assert owner_options["status"] == 200 and owner_options["body"]["ok"]
        save(output / "ui-owner-graph.json", graph_state(home, number))
        served_files = {}
        for relative, expected in ui_files.items():
            path = "en/isos" if relative == "index.html" else relative
            with urlopen(f"http://127.0.0.1:{ui_number}/{path}", timeout=15) as response:
                served = response.read()
            assert digest_bytes(served) == expected, relative
            served_files[relative] = digest_bytes(served)
        proxied = api(ui_number, "node-options")
        assert proxied == owner_options
        save(output / "ui-proxy-owner-options.json", proxied)
        save(output / "ui-served.json", {"url": f"http://127.0.0.1:{ui_number}/en/isos",
             "index_sha256": served_files["index.html"], "api_origin": ui_env["ISOSUI_API_ORIGIN"],
             "served_sha256": served_files,
             "proxy_api_options_sha256": sha(output / "ui-proxy-owner-options.json")})
        save(output / "browser-ready.json", {"url": f"http://127.0.0.1:{ui_number}/en/isos",
             "release_file": str(output / "browser-release")})
        for _ in range(1800):
            if (output / "browser-release").exists():
                break
            time.sleep(1)
        else:
            raise RuntimeError("visible browser observation timed out")
        assert (output / "visible-editor.json").is_file(), "visible editor evidence absent"
        assert graph_state(home, number)["revision"] == sha(output / "ui-home-graph.raw.json")
        selected = []
        for meta_path in sorted((output / "ui-post-options").glob("*-meta.json")):
            meta = obj_from(meta_path)
            prefix = f"{meta['index']:06d}"
            request_body = obj_from(output / "ui-post-options" / f"{prefix}-request.raw.json")
            response_body = obj_from(output / "ui-post-options" / f"{prefix}-response.raw.json")
            if request_body.get("graph") == draft and response_body.get("ok"):
                groups = response_body["data"]["setting_options"]
                matching = [row for row in groups if row["node_path"] == "eos/hardware/camera/front"
                            and row["setting"] == "adapter.settings.device_id" and row["selected"] == "ghost-device"]
                if len(matching) == 1:
                    selected.append(meta)
        assert selected, "editor made no matching POST node-options request"
        chosen = selected[-1]
        save(output / "ui-consumed-options.json", {"post_index": chosen["index"],
             "request_sha256": chosen["request_sha256"],
             "response_sha256": chosen["response_sha256"],
             "graph_sha256": sha(output / "ui-home-graph.raw.json")})
    finally:
        proxy.shutdown()
        proxy.server_close()
        thread.join(timeout=10)
        stop(server, handle)
        shutil.rmtree(home)
    save(output / "case-result.json", {"status": "passed", "scope": "selected-pin package, options, stale Apply, visible editor"})
    save(output / "evidence.sha256.json", {path.relative_to(output).as_posix(): sha(path)
         for path in sorted(output.rglob("*")) if path.is_file() and path.name != "evidence.sha256.json"})
    print("Q01_P01_P03_U01_REPLAY_PASS", output)


def obj_from(path: Path):
    return json.loads(path.read_text())


def digest_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("isos-root", "mujoco-root", "ui-root", "eos-bin-dir", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--asset", type=Path, required=True)
    parser.add_argument("--release", type=Path, required=True)
    parser.add_argument("--mujoco-library-root", type=Path, required=True,
                        help="installed MuJoCo runtime directory for the selected package")
    parser.add_argument("--ui-stage", action="store_true")
    parser.add_argument("--selection", type=Path,
                        help="externally frozen Q03 owner selection")
    parser.add_argument("--expect-selection-sha256",
                        help="reviewer-supplied digest of the selection file")
    args = parser.parse_args()
    if not args.mujoco_library_root.is_dir():
        parser.error("--mujoco-library-root must name an existing directory")
    assert bool(args.selection) == bool(args.expect_selection_sha256), \
        "selection and its external SHA-256 must be supplied together"
    selection = (load_selection(args.selection, args.expect_selection_sha256)
                 if args.selection else None)
    pins = ({owner: selection["owners"][owner]["commit"] for owner in PINS}
            if selection else PINS)
    roots = {"isos": args.isos_root.resolve(), "mujoco": args.mujoco_root.resolve(), "ui": args.ui_root.resolve()}
    output = args.output.resolve()
    if not args.ui_stage:
        output.mkdir(parents=True, exist_ok=False)
    for owner, root in roots.items():
        if selection is not None:
            verify_checkout(selection, owner, root)
            if not args.ui_stage:
                selected_owner = selection["owners"][owner]
                remote = subprocess.check_output(
                    ["git", "ls-remote", selected_owner["repository"],
                     selected_owner["ref"]], text=True, timeout=60)
                assert remote.strip() == (selected_owner["commit"] + "\t" +
                                          selected_owner["ref"])
                (output / f"{owner}-remote-fetch.log").write_text(
                    "$ git ls-remote " + selected_owner["repository"] + " " +
                    selected_owner["ref"] + "\n" + remote)
        elif not args.ui_stage:
            fetch_command = ["git", "-c", "url.https://github.com/.insteadOf=git@github.com:",
                             "fetch", "origin", "master"]
            fetched = subprocess.run(fetch_command, cwd=root,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, timeout=60)
            (output / f"{owner}-remote-fetch.log").write_text("$ " + " ".join(fetch_command) + "\n" + fetched.stdout)
            assert fetched.returncode == 0, (owner, fetched.stdout)
        assert git(root, "HEAD") == pins[owner], (owner, git(root, "HEAD"))
        if selection is None:
            assert git(root, "origin/master") == pins[owner]
        assert not subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"], cwd=root)
    if args.ui_stage:
        assert output.is_dir()
        if selection is not None:
            assert obj_from(output / "selected-pins.json")["selection_sha256"] == \
                args.expect_selection_sha256
            assert sha(Path(__file__)) == \
                selection["harness"]["tests/q01_p01_p03_u01_replay.py"]
        ui_stage(args, output, roots, pins)
        return
    if selection is not None:
        for name in ("q01_p01_p03_u01_replay.py",):
            assert sha(Path(__file__).with_name(name)) == \
                selection["harness"][f"tests/{name}"]
    for owner, root in roots.items():
        archive_source(root, output, owner)
    release = json.loads(args.release.read_text())
    assert release["source_commit"] == pins["mujoco"] and release["version"] == "0.1.1"
    assert any(row["sha256"] == sha(args.asset) for row in release["assets"])
    if selection is not None:
        assert sha(args.release) == selection["package"]["release_sha256"]
        assert sha(args.asset) == selection["package"]["asset_sha256"]
    (output / "package-input").mkdir()
    shutil.copy2(args.release, output / "package-input/release.json")
    shutil.copy2(args.asset, output / "package-input/asset.tar.gz")
    cargo = "cargo"
    clean_command = [cargo, "clean", "-p", "isos"]
    cleaned = subprocess.run(clean_command, cwd=roots["isos"], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, timeout=300)
    (output / "isos-clean.log").write_text("$ " + " ".join(clean_command) + "\n" + cleaned.stdout)
    assert cleaned.returncode == 0
    build_command = [cargo, "build", "--locked", "--bin", "isos"]
    build = subprocess.run(build_command, cwd=roots["isos"], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=600)
    (output / "isos-build.log").write_text("$ " + " ".join(build_command) + "\n" + build.stdout)
    assert build.returncode == 0 and "Finished" in build.stdout
    binary = output / "bin/isos"
    binary.parent.mkdir()
    shutil.copy2(roots["isos"] / "target/debug/isos", binary)
    shutil.copy2(roots["isos"] / "Cargo.lock", output / "isos-Cargo.lock")
    shutil.copy2(roots["ui"] / "package-lock.json", output / "ui-package-lock.json")
    ui_port = port()
    # One selected build binds the owner API used for every browser observation.
    # The visible editor case is served from the device-home API below.
    with tempfile.TemporaryDirectory(prefix="q01-p01-p03-u01-") as temp_name:
        temp = Path(temp_name)
        clean = temp / "clean"
        env = dict(os.environ, ISOS_HOME=str(clean), ISOS_HTTP_PORT=str(port()),
                   ISOSUI_PORT=str(ui_port), ISOS_AETHER_BIN_DIR=str(args.eos_bin_dir.resolve()),
                   ISOSUI_DIST_DIR=str((roots["ui"] / "dist/local").resolve()),
                   MUJOCO_ROOT=str(args.mujoco_library_root.resolve()))
        command(binary, env, output / "init.log", "init", "--mode", "dev")
        command(binary, env, output / "install.log", "pkg", "install", "--manifest",
                str(args.release.resolve()), "--asset", str(args.asset.resolve()), "--allow-unapproved")
        installed = clean / "data/packages/island/mujoco-sim/0.1.1"
        shutil.copytree(installed, output / "package-installed")
        first, handle = serve(binary, env, output / "initial-serve.log")
        try:
            first_port = int(env["ISOS_HTTP_PORT"])
            initial = graph_state(clean, first_port)
            save(output / "initial-graph.json", initial)
            preview = api(first_port, "node-sets/preview", "POST", {"package": {"id": "island/mujoco-sim", "version": "0.1.1"},
                          "set_id": "sim-hardware", "graph": initial["graph"], "values": {
                              "scene": "pick-place", "socket": str(clean / "run/mujoco.sock"),
                              "camera_device": "cam-0", "base_device": "base-0", "manipulator_device": "arm-0"}})
            save(output / "preview.json", preview)
            assert preview["status"] == 200 and preview["body"]["ok"], preview
            graph = preview["body"]["data"]["graph"]
        finally:
            stop(first, handle)
        for label, mutation in (("payload-tamper", "payload"), ("receipt-tamper", "receipt"),
                                ("ghost-scene", "scene"), ("ghost-device", "device"),
                                ("stale-revision", "valid")):
            home = temp / label
            shutil.copytree(clean, home)
            candidate = json.loads(json.dumps(graph).replace(str(clean), str(home)))
            run_case(label, home, binary, env, candidate, output, mutation)
    selected = {"pins": pins,
        "source_trees": {owner: git(root, "HEAD^{tree}") for owner, root in roots.items()},
        "isos_lock_sha256": sha(output / "isos-Cargo.lock"), "isos_binary_sha256": sha(binary),
        "ui_lock_sha256": sha(output / "ui-package-lock.json"),
        "release_sha256": sha(output / "package-input/release.json"),
        "asset_sha256": sha(output / "package-input/asset.tar.gz"),
        "installed_receipt_sha256": sha(output / "package-installed/receipt.json"),
        "ui_source_commit": pins["ui"]}
    if selection is not None:
        selected["selection_sha256"] = args.expect_selection_sha256
    save(output / "selected-pins.json", selected)
    save(output / "case-result.json", {"status": "pending_visible_editor", "scope": "selected-pin package and selector admission"})
    print("Q01_P01_P03_OWNER_REPLAY_PASS", output)


if __name__ == "__main__":
    main()
