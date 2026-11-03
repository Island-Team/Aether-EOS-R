#!/usr/bin/env python3
"""Execute pinned M2 first-use boundaries in disposable IsOS homes.

Every frozen case receives a machine result. A case that cannot be exercised is
reported as blocked, never passed by reading its fixture or an owner unit test.
The complete command and HTTP transcripts stay in ignored ``logs/test``.
"""

from __future__ import annotations

import argparse
import copy
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import socket
import subprocess
import tempfile
import time
from urllib.error import HTTPError
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
CASES = json.loads((ROOT / "tests/fixtures/m2_qualification/cases.json").read_text())["cases"]
HARNESS = ROOT / "tests/m2_cross_repo_replay.py"
FIXTURE = ROOT / "tests/fixtures/m2_qualification/cases.json"
WRAPPER = ROOT / "tests/run_m2_qualification.sh"


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def tree_manifest(root: Path, *, ignore_python_cache: bool = False) -> dict[str, dict[str, str | int]]:
    """Bind every file and symlink in a selected input or transcript tree."""
    assert root.is_dir() and not root.is_symlink(), root
    entries: dict[str, dict[str, str | int]] = {}
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        if ignore_python_cache and ("__pycache__" in path.parts or path.suffix == ".pyc"):
            continue
        if path.is_symlink():
            entries[relative] = {"kind": "symlink", "target": os.readlink(path)}
        elif path.is_file():
            entries[relative] = {"kind": "file", "sha256": sha256(path), "size": path.stat().st_size}
        elif path.is_dir():
            entries[relative] = {"kind": "directory"}
        else:
            raise ValueError(f"unsupported selected input: {path}")
    return entries


def digest_json(value: object) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def selected_inputs(args: argparse.Namespace) -> dict[str, object]:
    egia = args.egia_root
    ui = args.isosui_root
    python = egia / ".venv/bin/python"
    assert python.is_file() and (ui / "dist/local").is_dir()
    return {
        "egia": {
            "launcher": sha256(egia / "bin/egia"),
            "setup": sha256(egia / "bin/egia-config"),
            "python_path": str(python),
            "python_target": str(python.resolve()),
            "python_sha256": sha256(python.resolve()),
            "module_root": digest_json(tree_manifest(egia / "src/egia", ignore_python_cache=True)),
            "libexec": digest_json(tree_manifest(egia / "libexec", ignore_python_cache=True)),
            "component_defaults": digest_json(tree_manifest(egia / "etc/egia")),
            "factory_defaults": digest_json(tree_manifest(egia / "share/defaults/egia")),
        },
        "isosui": {"dist_local": digest_json(tree_manifest(ui / "dist/local"))},
    }


def head(path: Path) -> str:
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=path, text=True).strip()


def port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Replay:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.output = args.artifacts.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.private = Path(tempfile.mkdtemp(prefix="m2-replay-"))
        self.bundle = self.private / "bundle"
        self.archive = self.private / f"mujoco-sim-v0.1.1-linux-{platform.machine()}.tar.gz"
        self.results: dict[str, dict[str, str]] = {}
        self._template_graph: dict | None = None
        self._template_home: str | None = None
        self.record("environment.json", {
            "heads": {name: head(path) for name, path in self.roots.items()},
            "binaries": {name: sha256(path) for name, path in self.binaries.items()},
            "harness_sha256": sha256(HARNESS),
            "fixture_sha256": sha256(FIXTURE),
            "wrapper_sha256": sha256(WRAPPER),
            "selected_inputs": selected_inputs(args),
            "created_at_utc": datetime.now(timezone.utc).isoformat(),
            "platform": platform.platform(),
            "architecture": platform.machine(),
            "mujoco_library_root": str(args.mujoco_library_root),
            "mujoco_library_sha256": sha256(args.mujoco_library_root / "lib/libmujoco.so"),
            "bundle_root": str(self.bundle),
            "private_root": str(self.private),
        })

    @property
    def roots(self) -> dict[str, Path]:
        return {name: getattr(self.args, f"{name}_root") for name in ("eos", "isos", "egia", "mujoco", "isosui")}

    @property
    def binaries(self) -> dict[str, Path]:
        return {
            "isos": self.args.isos_root / "target/debug/isos",
            **{name: self.args.eos_root / "target/debug" / name for name in
               ("aether-profile", "aether-runtime", "aether-http-bridge", "aether-config-server")},
            "mujoco": self.args.mujoco_root / "build/eos-mujoco-sim",
        }

    def record(self, name: str, data: object) -> None:
        (self.output / name).write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")

    def build_bundle(self) -> None:
        command = ["python3", str(self.args.mujoco_root / "tools/build-backend-bundle.py"),
                   "--build-dir", str(self.args.mujoco_root / "build"), "--output", str(self.bundle)]
        self.run_command(command, self.output / "bundle.log", env=os.environ.copy())
        release = json.loads((self.bundle / "release.json").read_text())
        assert release["schema_version"] == 4 and release["id"] == "island/mujoco-sim"
        assert release["version"] == "0.1.1" and release["source_commit"] == head(self.args.mujoco_root)
        assert release["assets"][0]["sha256"] == sha256(self.archive)
        shutil.copyfile(self.bundle / "release.json", self.output / "bundle-release.json")
        self.record("bundle-file-hashes.json", tree_manifest(self.bundle))
        self.record("bundle-digests.json", {"release": sha256(self.bundle / "release.json"),
                                            "archive": sha256(self.archive),
                                            "descriptor": sha256(self.bundle / "backend-v2.json"),
                                            "payload_tree": digest_json(tree_manifest(self.bundle))})

    @staticmethod
    def run_command(command: list[str], logfile: Path, *, env: dict[str, str], timeout: int = 90,
                    cwd: Path | None = None, stdin: str | None = None) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(command, cwd=cwd, env=env, input=stdin, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=timeout)
        with logfile.open("a") as log:
            log.write(f"$ {json.dumps(command)}\nstdin={stdin or ''}\nexit={result.returncode}\n{result.stdout}\n")
        return result

    def case(self, case_id: str) -> "Home":
        return Home(self, case_id)

    def graph_for(self, home: "Home") -> dict:
        if self._template_graph is None:
            with self.case("template-graph") as template:
                template.init()
                template.link()
                template.serve()
                self._template_graph = template.preview()
                self._template_home = str(template.home)
        raw = json.dumps(self._template_graph)
        raw = raw.replace(self._template_home or "", str(home.home))
        raw = raw.replace("eos/process/mujoco/simulator", "eos/process/mujoco/sim")
        graph = json.loads(raw)
        nodes = graph["cores"]["eos"]["layers"]["process"]["groups"]["mujoco"]["nodes"]
        simulator = nodes.pop("simulator")
        simulator["id"] = "sim"
        nodes["sim"] = simulator
        return graph

    def egia_graph_for(self, home: "Home") -> dict:
        graph = self.graph_for(home)
        graph["cores"]["egia"] = {"layers": {}}
        nodes = graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]
        nodes["egia"] = {
            "id": "egia", "node_type": "isos.process.component@v0.1",
            "label": "EGIA agent", "description": "Managed agent reader qualification",
            "capabilities": ["process"],
            "settings": {"role": "egia", "args": [],
                         "depends_on": ["isos/process/component/eos"]},
        }
        return graph

    def result(self, case_id: str, status: str, detail: str) -> None:
        self.results[case_id] = {"status": status, "detail": detail}
        self.record("results.json", self.results)
        print(f"{case_id} {status}: {detail}", flush=True)

    def attest(self, selected: set[str]) -> None:
        assert set(self.results) == selected and all(
            result["status"] == "pass" for result in self.results.values())
        transcripts = {case_id: tree_manifest(self.output / case_id) for case_id in sorted(selected)}
        assert all(transcripts.values()), "a passed case has no transcript"
        support = {path.name: tree_manifest(path) for path in self.output.iterdir()
                   if path.is_dir() and path.name not in selected}
        assert all(not name.startswith("M2-") for name in support), "unexpected unreported case directory"
        self.record("batch-attestation.json", {
            "selected_cases": sorted(selected),
            "harness_sha256": sha256(HARNESS),
            "fixture_sha256": sha256(FIXTURE),
            "wrapper_sha256": sha256(WRAPPER),
            "environment_sha256": sha256(self.output / "environment.json"),
            "bundle_digests_sha256": sha256(self.output / "bundle-digests.json"),
            "bundle_file_hashes_sha256": sha256(self.output / "bundle-file-hashes.json"),
            "bundle_release_sha256": sha256(self.output / "bundle-release.json"),
            "results_sha256": sha256(self.output / "results.json"),
            "bundle_log_sha256": sha256(self.output / "bundle.log"),
            "transcripts": transcripts,
            "support_transcripts": support,
        })


class Home:
    def __init__(self, replay: Replay, case_id: str) -> None:
        self.replay = replay
        self.case_id = case_id
        self.directory = replay.output / case_id
        self.directory.mkdir()
        self.root = replay.private / case_id
        self.home = self.root / "home"
        self.home.mkdir(parents=True)
        self.graph = self.home / "config/deployments/default.nodes.json"
        self.api_port, self.ui_port = port(), port()
        self.env = dict(os.environ, ISOS_HOME=str(self.home), ISOS_HTTP_PORT=str(self.api_port),
                        ISOSUI_PORT=str(self.ui_port), MUJOCO_ROOT=str(replay.args.mujoco_library_root),
                        ISOS_AETHER_BIN_DIR=str(replay.args.eos_root / "target/debug"),
                        ISOS_EGIA_BIN_DIR=str(replay.args.egia_root / "bin"),
                        ISOSUI_DIST_DIR=str(replay.args.isosui_root / "dist/local"))
        self.env["LD_LIBRARY_PATH"] = str(replay.args.mujoco_library_root / "lib") + ":" + self.env.get("LD_LIBRARY_PATH", "")
        self.env["ISOS_EGIA_DEFAULTS_DIR"] = str(replay.args.egia_root / "etc/egia")
        self.env["ISOS_EGIA_FACTORY_DEFAULTS_DIR"] = str(replay.args.egia_root / "share/defaults/egia")
        self.env["EGIA_PYTHON"] = str(replay.args.egia_root / ".venv/bin/python")
        self.env["ISLAND_PREFIX"] = str(replay.args.egia_root)
        self.server: subprocess.Popen[bytes] | None = None
        self.server_log = None

    def cmd(self, *args: str, success: bool | None = True) -> subprocess.CompletedProcess[str]:
        result = self.replay.run_command([str(self.replay.binaries["isos"]), *args],
                                         self.directory / "commands.log", env=self.env)
        if success is not None:
            assert (result.returncode == 0) == success, (args, result.returncode, result.stdout)
        return result

    def init(self) -> None:
        self.cmd("init")
        assert self.graph.is_file()

    def saved(self) -> bytes:
        return self.graph.read_bytes()

    def status(self) -> dict:
        code, body = self.api("process-status")
        assert code == 200 and body["ok"], body
        return body["data"]

    def link(self, *, manifest: bool = True) -> dict:
        args = ["pkg", "link", "island/mujoco-sim", "0.1.1", str(self.replay.bundle)]
        if manifest:
            args += ["--manifest", str(self.replay.bundle / "release.json")]
        self.cmd(*args)
        listing = json.loads(self.cmd("pkg", "list", "--json").stdout)
        return next(item for item in listing if item["id"] == "island/mujoco-sim" and item["version"] == "0.1.1")

    def modified_capability(self, binary: str, *, remove_feature: str | None = None,
                            remove_type: str | None = None) -> None:
        selected = self.root / "aether-bin"
        selected.mkdir(exist_ok=True)
        for name in ("aether-profile", "aether-runtime", "aether-http-bridge", "aether-config-server"):
            shutil.copy2(self.replay.binaries[name], selected / name)
        actual = self.replay.binaries[binary]
        probe = json.loads(subprocess.check_output([str(actual), "--capabilities"], text=True))
        if remove_feature:
            probe["features"].remove(remove_feature)
        if remove_type:
            probe["supported_node_types"].remove(remove_type)
        script = ("#!/usr/bin/env python3\nimport json, os, subprocess, sys\n"
                  f"REAL = {json.dumps(str(actual))}\n"
                  f"if sys.argv[1:] == ['--capabilities']:\n    print({json.dumps(json.dumps(probe))})\n"
                  f"elif sys.argv[1:] == ['setup', '--json'] and {json.dumps(remove_type)} is not None:\n"
                  "    request = sys.stdin.buffer.read()\n"
                  "    result = subprocess.run([REAL, *sys.argv[1:]], input=request, capture_output=True)\n"
                  "    if result.returncode == 0 and json.loads(request).get('op') == 'identity':\n"
                  "        reply = json.loads(result.stdout)\n"
                  f"        reply['identity']['supported_node_types'].remove({json.dumps(remove_type)})\n"
                  "        print(json.dumps(reply))\n"
                  "    else:\n        sys.stdout.buffer.write(result.stdout)\n"
                  "    sys.stderr.buffer.write(result.stderr)\n    sys.exit(result.returncode)\n"
                  "else:\n    os.execv(REAL, [REAL, *sys.argv[1:]])\n")
        target = selected / binary
        target.write_text(script)
        target.chmod(0o755)
        self.env["ISOS_AETHER_BIN_DIR"] = str(selected)

    def stub_egia_owner(self) -> Path:
        prefix = self.root / "egia-owner-stub"
        binary = prefix / "bin"
        component = prefix / "etc/egia"
        factory = prefix / "share/defaults/egia"
        binary.mkdir(parents=True)
        component.mkdir(parents=True)
        factory.mkdir(parents=True)
        (prefix / "pyproject.toml").write_text("[project]\nname='egia-m2-fixture'\nversion='0.1.0'\n")
        module = prefix / "src/egia"
        module.mkdir(parents=True)
        (module / "__init__.py").write_text("# selected module inventory fixture\n")
        for name in ("egia.toml", "modes.toml", "profiles/isos.toml", "models.toml",
                     "prompts.toml", "missions.toml", "safety/gate.toml", "opt/sandbox.toml"):
            target = component / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("enabled = true\n")
        (factory / "models.toml").write_text("enabled = true\n")
        (binary / "egia-config").write_text('''#!/usr/bin/env python3
import json, sys
request = json.load(sys.stdin)
reply = {"schema_version": 1, "ok": True, "diagnostics": [], "files": []}
if request["op"] == "identity":
    reply["identity"] = {"core": "egia", "graph_version": "1.0.0", "node_type_version": "1.0",
        "product_version": "0.1.0", "setup_schema_version": 1,
        "supported_graph_versions": ["0.1.0", "1.0.0"],
        "supported_node_type_versions": ["1.0"],
        "supported_node_types": ["egia.common.context@v1.0", "egia.common.guard@v1.0",
                                 "egia.common.planner@v1.0", "egia.common.source@v1.0"],
        "features": ["setup-v1", "catalog", "graph-0.1-reader"]}
elif request["op"] == "render":
    reply["files"] = [{"path": "egia/egia.toml", "content": "mode = 'isos'\\n"},
                      {"path": "egia/modes.toml", "content": "enabled = true\\n"},
                      {"path": "egia/profiles/isos.toml", "content": "managed = true\\n"}]
print(json.dumps(reply))
''')
        (binary / "egia-config").chmod(0o700)
        launcher = binary / "egia"
        launcher.write_text('''#!/bin/sh
if [ "$1" = --capabilities ]; then
  printf '%s\\n' '{"binary":"egia","product_version":"0.1.0","features":["managed-config-v1","eos-gateway-client-v2"]}'
  exit 0
fi
exit 0
''')
        launcher.chmod(0o700)
        self.env.update({"ISOS_EGIA_BIN_DIR": str(binary), "ISOS_EGIA_DEFAULTS_DIR": str(component),
                         "ISOS_EGIA_FACTORY_DEFAULTS_DIR": str(factory),
                         "EGIA_PYTHON": shutil.which("python3") or "/usr/bin/python3"})
        self.env.pop("ISLAND_PREFIX", None)
        return launcher

    def preview(self, scene: str = "pick-place") -> dict:
        values = {"scene": scene, "socket": str(self.home / "run/sim.sock"),
                  "camera_device": "cam-0", "base_device": "base-0", "manipulator_device": "arm-0"}
        code, body = self.api("node-sets/preview", "POST", {
            "package": {"id": "island/mujoco-sim", "version": "0.1.1"},
            "set_id": "sim-hardware", "graph": json.loads(self.saved()), "values": values,
        })
        assert code == 200 and body["ok"], body
        return body["data"]["graph"]

    def validate(self, graph: dict) -> tuple[int, dict]:
        return self.api("node-graph/validate", "POST", {"schema_version": 1, "graph": graph})

    def apply(self, graph: dict) -> tuple[int, dict]:
        code, original = self.api("node-graph")
        assert code == 200 and original["ok"], original
        return self.api("node-graph", "PUT", {"schema_version": 1,
                                               "revision": original["data"]["revision"], "graph": graph})

    def run(self, revision: str) -> tuple[int, dict]:
        return self.api("process-control", "POST", {"schema_version": 1,
                                                     "operation": "run", "revision": revision})

    def current(self, **filters: str) -> tuple[int, dict]:
        from urllib.parse import urlencode
        return self.api("logs?" + urlencode({"view": "current", "source": "all", "level": "info", **filters}))

    def manager(self, request: dict) -> dict:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.connect(str(self.home / "run/manager.sock"))
            connection.sendall(json.dumps(request).encode() + b"\n")
            connection.shutdown(socket.SHUT_WR)
            chunks = []
            while part := connection.recv(65536):
                chunks.append(part)
        response = json.loads(b"".join(chunks))
        with (self.directory / "manager.jsonl").open("a") as log:
            log.write(json.dumps({"request": request, "response": response}, sort_keys=True) + "\n")
        return response

    def api(self, path: str, method: str = "GET", data: object | None = None) -> tuple[int, dict]:
        request = Request(f"http://127.0.0.1:{self.api_port}/api/isos/v1/{path}", method=method,
                          data=None if data is None else json.dumps(data).encode(),
                          headers={"Origin": f"http://127.0.0.1:{self.api_port}", "Content-Type": "application/json"})
        try:
            response = urlopen(request, timeout=120)
        except HTTPError as error:
            response = error
        with response:
            code, body = response.status, json.load(response)
        with (self.directory / "http.jsonl").open("a") as log:
            log.write(json.dumps({"method": method, "path": path, "request": data,
                                  "status": code, "response": body}, sort_keys=True) + "\n")
        return code, body

    def serve(self) -> None:
        self.server_log = (self.directory / "serve.log").open("wb")
        self.server = subprocess.Popen([str(self.replay.binaries["isos"]), "serve"], env=self.env,
                                       stdout=self.server_log, stderr=subprocess.STDOUT)
        for _ in range(150):
            if self.server.poll() is not None:
                raise RuntimeError(f"serve exited {self.server.returncode}")
            try:
                self.api("process-status")
                return
            except OSError:
                time.sleep(0.05)
        raise TimeoutError("IsOS API did not become ready")

    def close(self) -> None:
        if self.server is not None and self.server.poll() is None:
            self.server.send_signal(signal.SIGINT)
            try:
                self.server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.server.kill()
                self.server.wait(timeout=5)
        if self.server_log is not None:
            self.server_log.close()
            self.server_log = None
        self.server = None

    def restart_serve(self) -> None:
        self.close()
        self.serve()

    def __enter__(self) -> "Home":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()
        if self.graph.is_file():
            (self.directory / "saved-graph.sha256").write_text(sha256(self.graph) + "\n")
        generated = self.home / "data/generated"
        if generated.is_dir():
            (self.directory / "generated-file-hashes.json").write_text(
                json.dumps(tree_manifest(generated), indent=2, sort_keys=True) + "\n")
        shared_logs = self.home / "data/logs/default"
        if shared_logs.is_dir():
            for path in shared_logs.glob("events.log*"):
                if path.is_file():
                    shutil.copyfile(path, self.directory / path.name)


def check_01(replay: Replay) -> None:
    with replay.case("M2-01") as home:
        home.init()
        saved = home.saved()
        status = json.loads(home.cmd("status", "--json").stdout)
        home.cmd("pkg", "install", "--manifest", str(replay.bundle / "release.json"),
                 "--asset", str(replay.archive), "--allow-unapproved")
        listing = json.loads(home.cmd("pkg", "list", "--json").stdout)
        receipt = next(item for item in listing if item["id"] == "island/mujoco-sim")
        assert receipt["schema_version"] == 2 and receipt["version"] == "0.1.1"
        assert receipt["source_kind"] == "local_candidate" and len(receipt["release_sha256"]) == 64
        assert receipt["asset_sha256"] == sha256(replay.archive)
        stored = home.home / "data/packages/island/mujoco-sim/0.1.1"
        assert receipt["release_sha256"] == sha256(stored / "release.json")
        assert receipt["sidecar_sha256"] == sha256(stored / "node-types.json")
        assert receipt["payload_sha256"] and len(receipt["payload_sha256"]) == 64
        after = json.loads(home.cmd("status", "--json").stdout)
        assert home.saved() == saved
        assert all(after[key] == status[key] for key in ("active_revision", "state", "processes", "run_id", "run_id_state"))


def check_02(replay: Replay) -> None:
    with replay.case("M2-02") as home:
        home.init()
        saved = home.saved()
        receipt = home.link()
        assert receipt["schema_version"] == 2 and receipt["source_kind"] == "local_link"
        assert receipt["local_link"] == str(replay.bundle) and receipt["available"]
        stored = home.home / "data/packages/island/mujoco-sim/0.1.1"
        assert receipt["release_sha256"] == sha256(stored / "release.json")
        assert receipt["sidecar_sha256"] == sha256(stored / "node-types.json")
        assert home.saved() == saved


def check_03(replay: Replay) -> None:
    with replay.case("M2-03") as home:
        home.init()
        saved = home.saved()
        bad = copy.deepcopy(json.loads((replay.bundle / "release.json").read_text()))
        bad["assets"][0]["sha256"] = "0" * 64
        bad_manifest = home.root / "bad-release.json"
        bad_manifest.write_text(json.dumps(bad))
        home.cmd("pkg", "install", "--manifest", str(bad_manifest), "--asset",
                 str(replay.archive), "--allow-unapproved", success=False)
        assert not json.loads(home.cmd("pkg", "list", "--json").stdout)
        assert home.saved() == saved
        escaped_bundle = home.root / "escaped-bundle"
        shutil.copytree(replay.bundle, escaped_bundle)
        escaped_scene = escaped_bundle / "assets/scene.xml"
        escaped_scene.unlink()
        escaped_scene.symlink_to("/etc/passwd")
        home.serve()
        code, scope = home.current()
        assert code == 200 and scope["data"]["scope"]["kind"] == "manager_session", scope
        manager_run = scope["data"]["scope"]["run_id"]
        events = home.home / "data/logs/default/events.log"
        before_events = len(events.read_text().splitlines()) if events.exists() else 0
        code, response = home.api("packages/link", "POST", {"id": "island/mujoco-sim",
                                                              "version": "0.1.1", "path": str(escaped_bundle),
                                                              "manifest": str(escaped_bundle / "release.json")})
        assert code == 422 and not response["ok"], response
        assert response["data"]["diagnostics"] == [{
            "code": "package_unsafe_path", "id": "island/mujoco-sim", "version": "0.1.1",
            "path": "", "stage": "descriptor",
            "message": "Reinstall or relink island/mujoco-sim@0.1.1"}], response
        accepted = [json.loads(line) for line in events.read_text().splitlines()[before_events:]]
        matching = [item for item in accepted if item["event_code"] == "package_unsafe_path"]
        assert len(matching) == 1, accepted
        event = matching[0]
        assert event["source"] == "packages" and event["core"] == "isos" and event["level"] == "error"
        assert event["run_id"] == manager_run and event["correlation_id"]
        assert event["fields"]["package_id"] == "island/mujoco-sim"
        assert event["fields"]["package_version"] == "0.1.1"
        code, current = home.current()
        assert code == 200 and current["data"]["anchor"]["event"]["correlation_id"] == event["correlation_id"], current
        assert not json.loads(home.cmd("pkg", "list", "--json").stdout)
        assert home.saved() == saved and home.status()["processes"] == []


def check_04(replay: Replay) -> None:
    with replay.case("M2-04") as home:
        home.init()
        saved = home.saved()
        home.link()
        descriptor = json.loads((replay.bundle / "backend-v2.json").read_text())
        assert descriptor["catalog_version"] == "0.1.0"
        scenes = {item["id"] for item in descriptor["scenes"]}
        assert scenes == {"navigation", "pick-place"}
        graph = replay.graph_for(home)
        home.env["MUJOCO_ROOT"] = str(home.root / "missing-mujoco-library")
        home.serve()
        code, rejected = home.apply(graph)
        assert code == 422 and not rejected["ok"], rejected
        assert any(item["code"] == "mujoco_library_unavailable"
                   and item["path"] == "/cores/eos/layers/process/groups/mujoco/nodes/sim/package"
                   for item in rejected["data"]["diagnostics"]), rejected
        assert home.saved() == saved


def check_05(replay: Replay) -> None:
    with replay.case("M2-05") as home:
        home.init()
        home.link()
        home.serve()
        graph = home.preview()
        code, result = home.api("node-options", "POST", {"schema_version": 1, "graph": graph})
        assert code == 200 and result["ok"], result
        options = result["data"]["setting_options"]
        assert options and all("node_path" in entry and "setting" in entry for entry in options)
        keys = [(entry["node_path"], entry["setting"]) for entry in options]
        assert keys == sorted(set(keys))
        for node in ("eos/hardware/camera/front", "eos/hardware/base/drive",
                     "eos/hardware/manipulator/arm"):
            by_setting = {entry["setting"]: entry for entry in options if entry["node_path"] == node}
            assert by_setting["adapter.settings.device_id"]["selected"] == by_setting["adapter.settings.simulator_node"]["selected"]
        changed = home.preview("navigation")
        code, second = home.api("node-options", "POST", {"schema_version": 1, "graph": changed})
        assert code == 200 and second["ok"], second
        assert second["data"]["setting_options"] != options


def check_06(replay: Replay) -> None:
    with replay.case("M2-06") as home:
        home.init()
        home.link()
        home.serve()
        graph = home.preview()
        saved = home.saved()
        camera = graph["cores"]["eos"]["layers"]["hardware"]["groups"]["camera"]["nodes"]["front"]
        camera["adapter"]["settings"]["device_id"] = "../private"
        code, result = home.apply(graph)
        assert code == 422 and not result["ok"], result
        diagnostics = result["data"]["diagnostics"]
        assert any(issue["code"] == "node_option_invalid_id"
                   and issue["path"].endswith("/adapter/settings/device_id") for issue in diagnostics), diagnostics
        assert home.saved() == saved
        camera["adapter"]["settings"]["device_id"] = "ghost-device"
        code, options = home.api("node-options", "POST", {"schema_version": 1, "graph": graph})
        assert code == 200 and options["ok"], options
        groups = options["data"]["setting_options"]
        missing = next(entry for entry in groups if entry["node_path"] == "eos/hardware/camera/front"
                       and entry["setting"] == "adapter.settings.device_id")
        assert missing["selected"] == "ghost-device"
        assert any(option["value"] == "ghost-device" and not option["available"]
                   for option in missing["options"]), missing
        code, result = home.apply(graph)
        assert code == 422 and not result["ok"], result
        assert home.saved() == saved


def check_07(replay: Replay) -> None:
    with replay.case("M2-07") as home:
        home.init()
        home.link()
        home.serve()
        graph = home.preview()
        code, validation = home.validate(graph)
        assert code == 200 and validation["data"]["valid"], validation
        code, applied = home.apply(graph)
        assert code == 200 and applied["ok"], applied
        changed = home.saved()
        assert json.loads(changed)["version"] == "0.1.0"
        generated = home.home / "data/generated"
        manifests = list(generated.rglob("manifest.json"))
        assert manifests and any(json.loads(path.read_text())["schema_version"] == 2 for path in manifests)
        before_manifest_bytes = {path.relative_to(generated).as_posix(): path.read_bytes() for path in manifests}
        before_snapshot = tree_manifest(generated)
        code, again = home.apply(graph)
        assert code == 200 and again["ok"] and home.saved() == changed, again
        after_manifest_bytes = {path.relative_to(generated).as_posix(): path.read_bytes()
                                for path in generated.rglob("manifest.json")}
        assert after_manifest_bytes == before_manifest_bytes
        assert tree_manifest(generated) == before_snapshot


def check_09(replay: Replay) -> None:
    with replay.case("M2-09") as home:
        home.init()
        home.link()
        graph = replay.graph_for(home)
        home.serve()
        code, result = home.apply(graph)
        assert code == 200 and result["ok"], result
        for name in ("aether-profile", "aether-runtime", "aether-http-bridge", "aether-config-server"):
            binary = replay.binaries[name]
            capability = json.loads(subprocess.check_output([str(binary), "--capabilities"], text=True))
            assert capability["binary"] == name
            if name in ("aether-profile", "aether-runtime"):
                assert "0.1.0" in capability["supported_graph_versions"]
        manifest = next((home.home / "data/generated").rglob("manifest.json"))
        generated = json.loads(manifest.read_text())
        assert generated["schema_version"] == 2
        inputs = {item["binary"]: item["sha256"] for item in generated["launch_inputs"]}
        assert all(inputs[name] == sha256(replay.binaries[name]) for name in
                   ("aether-profile", "aether-runtime", "aether-http-bridge", "aether-config-server")), inputs
        assert generated["packages"] and generated["packages"][0]["id"] == "island/mujoco-sim"
        assert generated["packages"][0]["version"] == "0.1.1"
        assert graph["cores"]["eos"]["layers"]["process"]["groups"]["mujoco"]["nodes"]["sim"]["node_type"] == "eos.process.mujoco@v0.1"


def check_08(replay: Replay) -> None:
    with replay.case("M2-08") as home:
        home.init()
        home.link()
        graph = replay.graph_for(home)
        home.serve()
        code, applied = home.apply(graph)
        assert code == 200 and applied["ok"], applied
        revision, saved = sha256(home.graph), home.saved()
        manifest = next((home.home / "data/generated").rglob("manifest.json"))
        original = manifest.read_bytes()
        changed = json.loads(original)
        changed["files"][0]["sha256"] = "0" * 64
        manifest.write_text(json.dumps(changed) + "\n")
        code, rejected = home.run(revision)
        assert code == 422 and not rejected["ok"], rejected
        assert any(item["code"] == "generated_manifest_mismatch" for item in rejected["data"]["diagnostics"]), rejected
        assert home.saved() == saved and home.status()["processes"] == []
        assert json.loads(manifest.read_text())["files"][0]["sha256"] == "0" * 64
        manifest.write_bytes(original)
        code, started = home.run(revision)
        assert code == 200 and started["ok"], started
        before = {item["id"]: item["pid"] for item in home.status()["processes"]}
        assert before
        manifest.write_text(json.dumps(changed) + "\n")
        code, rejected = home.api("process-control", "POST", {"schema_version": 1,
                                                               "operation": "restart", "revision": revision})
        assert code == 422 and any(item["code"] == "generated_manifest_mismatch"
                                   for item in rejected["data"]["diagnostics"]), rejected
        assert {item["id"]: item["pid"] for item in home.status()["processes"]} == before


def check_20(replay: Replay) -> None:
    with replay.case("M2-20") as home:
        home.init()
        mutable = home.root / "mutable-bundle"
        shutil.copytree(replay.bundle, mutable)
        home.cmd("pkg", "link", "island/mujoco-sim", "0.1.1", str(mutable),
                 "--manifest", str(mutable / "release.json"))
        graph = replay.graph_for(home)
        original = home.saved()
        home.serve()
        future = copy.deepcopy(graph)
        future["version"] = "99.0.0"
        code, rejected = home.apply(future)
        assert code == 422 and not rejected["ok"], rejected
        assert any(item["path"] == "/version" for item in rejected["data"]["diagnostics"]), rejected
        no_scene = copy.deepcopy(graph)
        no_scene["cores"]["eos"]["layers"]["process"]["groups"]["mujoco"]["nodes"]["sim"]["settings"]["scene"] = "missing-scene"
        code, rejected = home.apply(no_scene)
        assert code == 422 and any(item["code"] == "mujoco_scene_unavailable"
                                   for item in rejected["data"]["diagnostics"]), rejected
        no_device = copy.deepcopy(graph)
        no_device["cores"]["eos"]["layers"]["hardware"]["groups"]["manipulator"]["nodes"]["arm"]["adapter"]["settings"]["device_id"] = "missing-arm"
        code, rejected = home.apply(no_device)
        assert code == 422 and any(item["code"] == "mujoco_device_unavailable"
                                   for item in rejected["data"]["diagnostics"]), rejected
        descriptor = mutable / "backend-v2.json"
        descriptor_original = descriptor.read_bytes()
        descriptor.write_bytes(descriptor_original + b"\n")
        code, rejected = home.apply(graph)
        assert code == 422 and not rejected["ok"], rejected
        assert any(item["code"] == "package_changed" and item["stage"] == "payload"
                   for item in rejected["data"]["diagnostics"]), rejected
        assert home.saved() == original and home.status()["active_revision"] is None
        assert home.status()["processes"] == []
        assert home.saved() == original
        descriptor.write_bytes(descriptor_original)
        code, applied = home.apply(graph)
        assert code == 200 and applied["ok"], applied
        saved_applied = home.saved()
        revision = sha256(home.graph)
        code, started = home.run(revision)
        assert code == 200 and started["ok"], started
        before = home.status()
        pids = {item["id"]: item["pid"] for item in before["processes"]}
        assert pids
        executable = mutable / "bin/eos-mujoco-sim"
        replacement = executable.with_suffix(".tampered")
        replacement.write_bytes(executable.read_bytes() + b"\n")
        replacement.chmod(0o755)
        replacement.replace(executable)
        code, rejected = home.api("process-control", "POST", {"schema_version": 1,
                                                               "operation": "restart", "revision": revision})
        assert code == 422 and not rejected["ok"], rejected
        assert any(item["code"] == "generated_missing_input" and item["stage"] == "package"
                   for item in rejected["data"]["diagnostics"]), rejected
        after = home.status()
        assert after["active_revision"] == before["active_revision"]
        assert {item["id"]: item["pid"] for item in after["processes"]} == pids
        assert home.saved() == saved_applied


def check_21(replay: Replay) -> None:
    with replay.case("M2-21") as home:
        home.init()
        home.link()
        graph = replay.graph_for(home)
        home.serve()
        code, applied = home.apply(graph)
        assert code == 200 and applied["ok"], applied
        state_file = home.home / "data/logs/default/cursor-state.json"
        assert state_file.is_file()
        prior = state_file.read_bytes()
        state_file.unlink()
        state_file.mkdir()
        code, rejected = home.run(sha256(home.graph))
        assert code == 500 and not rejected["ok"], rejected
        assert any(issue["code"] == "log_scope_publish_failed" and issue["path"] == "/run_id"
                   for issue in rejected["data"]["diagnostics"]), rejected
        assert home.status()["processes"] == []
        state_file.rmdir()
        state_file.write_bytes(prior)
        for test in ("uncertain_publication_keeps_surviving_new_run_and_unavailable_anchor",
                     "scope_publication_failure_blocks_child_spawn"):
            result = replay.run_command(["cargo", "test", "--no-default-features",
                                         "--bin", "isos", test], home.directory / "owner-publish.log",
                                        env=os.environ.copy(), timeout=180, cwd=replay.args.isos_root)
            assert result.returncode == 0 and "1 passed" in result.stdout, (test, result.stdout)


def check_10(replay: Replay) -> None:
    with replay.case("M2-10") as home:
        home.init()
        home.link()
        graph = replay.graph_for(home)
        home.modified_capability("aether-runtime", remove_feature="mujoco-child-v2")
        saved = home.saved()
        home.serve()
        code, result = home.apply(graph)
        assert code == 422 and not result["ok"], result
        assert any(item["code"] == "reader_feature_unsupported" and item["stage"] == "feature"
                   and item["path"] == "/cores/eos/layers/process/groups/mujoco/nodes/sim/node_type"
                   and item["reader"]["binary"] == "aether-runtime" for item in result["data"]["diagnostics"]), result
        assert home.saved() == saved and home.status()["processes"] == []


def check_11(replay: Replay) -> None:
    with replay.case("M2-11") as home:
        home.init()
        home.link()
        graph = replay.egia_graph_for(home)
        launcher = home.stub_egia_owner()
        home.serve()
        code, applied = home.apply(graph)
        assert code == 200 and applied["ok"], applied
        saved = home.saved()
        launcher.write_bytes(launcher.read_bytes() + b"\n# changed after Apply\n")
        code, rejected = home.run(sha256(home.graph))
        assert code == 422 and not rejected["ok"], rejected
        assert any(item["code"] == "generated_manifest_mismatch" for item in rejected["data"]["diagnostics"]), rejected
        assert home.saved() == saved and home.status()["processes"] == []


def check_13(replay: Replay) -> None:
    with replay.case("M2-13") as home:
        home.init()
        home.link()
        graph = replay.egia_graph_for(home)
        home.serve()
        code, validation = home.validate(graph)
        assert code == 200 and not validation["data"]["valid"], validation
        issues = validation["data"]["diagnostics"]
        assert any(item["code"] == "generated_owner_rejected" and item["stage"] == "render"
                   and item["path"] == "/cores/egia" for item in issues), validation
        assert not any(item["code"].startswith("reader_") for item in issues), validation
        code, applied = home.apply(graph)
        assert code == 422 and not applied["ok"], applied
        assert home.status()["processes"] == []
        events_path = home.home / "data/logs/default/events.log"
        if events_path.exists():
            assert not any(json.loads(line).get("event_code") == "egia_ready"
                           for line in events_path.read_text().splitlines())
        runtime = replay.run_command([str(replay.args.egia_root / "bin/egia"), "--capabilities"],
                                     home.directory / "owner-reader.log", env=home.env, timeout=30,
                                     cwd=replay.args.egia_root)
        setup = replay.run_command([str(replay.args.egia_root / "bin/egia-config"), "setup", "--json"],
                                   home.directory / "owner-reader.log", env=home.env, timeout=30,
                                   cwd=replay.args.egia_root,
                                   stdin='{"schema_version":1,"op":"identity","core":"egia"}')
        assert runtime.returncode == setup.returncode == 0, (runtime.stdout, setup.stdout)
        runtime_claim = json.loads(runtime.stdout)
        setup_claim = json.loads(setup.stdout)["identity"]
        assert runtime_claim["binary"] == "egia" and "eos-gateway-client-v2" in runtime_claim["features"]
        assert "0.1.0" in setup_claim["supported_graph_versions"]
        assert setup_claim["supported_node_types"] == [
            "egia.common.context@v1.0", "egia.common.guard@v1.0",
            "egia.common.planner@v1.0", "egia.common.source@v1.0"]


def check_12(replay: Replay) -> None:
    with replay.case("M2-12") as home:
        home.init()
        home.link()
        graph = replay.graph_for(home)
        home.serve()
        code, applied = home.apply(graph)
        assert code == 200 and applied["ok"], applied
        revision = sha256(home.graph)
        code, started = home.run(revision)
        assert code == 200 and started["ok"], started
        for _ in range(90):
            state = home.status()
            if state["state"] == "running":
                break
            time.sleep(0.2)
        assert state["state"] == "running" and state["active_revision"] == revision, state
        assert state["run_id_state"] == "known" and state["run_id"].startswith("r")
        code, logs = home.current()
        assert code == 200 and logs["ok"], logs
        page = logs["data"]
        assert page["scope"]["kind"] == "deployment_run" and page["scope"]["run_id"] == state["run_id"]
        events = [json.loads(line) for line in (home.home / "data/logs/default/events.log").read_text().splitlines()]
        codes = [item["event_code"] for item in events if item["run_id"] == state["run_id"]]
        assert "deployment_starting" in codes and "eos_ready" in codes and "deployment_ready" in codes, codes
        assert all(codes.count(code) == 1 for code in ("deployment_starting", "eos_ready", "deployment_ready")), codes
        assert page["anchor"]["state"] == "found"


def running_home(replay: Replay, case_id: str, env: dict[str, str] | None = None) -> Home:
    home = replay.case(case_id)
    try:
        home.env.update(env or {})
        home.init()
        home.link()
        graph = replay.graph_for(home)
        home.serve()
        code, applied = home.apply(graph)
        assert code == 200 and applied["ok"], applied
        code, started = home.run(sha256(home.graph))
        assert code == 200 and started["ok"], started
        assert home.status()["run_id_state"] == "known"
        return home
    except BaseException:
        home.close()
        raise


def kill_backend_child(home: Home) -> int:
    eos = next(item for item in home.status()["processes"] if item["id"] == "eos")
    eos_pid = eos["pid"]
    children_file = Path(f"/proc/{eos_pid}/task/{eos_pid}/children")
    assert children_file.is_file(), "Linux /proc child inventory is required for the injected failure"
    for child in children_file.read_text().split():
        child_pid = int(child)
        executable = Path(f"/proc/{child_pid}/exe")
        try:
            selected = executable.resolve(strict=True)
        except (FileNotFoundError, ProcessLookupError):
            continue
        if selected.name == "eos-mujoco-sim":
            with (home.directory / "faults.jsonl").open("a") as log:
                log.write(json.dumps({"fault": "kill_backend_child", "eos_pid": eos_pid,
                                      "backend_pid": child_pid, "signal": "SIGKILL"}) + "\n")
            os.kill(child_pid, signal.SIGKILL)
            return child_pid
    raise AssertionError(f"EOS {eos_pid} has no selected MuJoCo child")


def await_event(home: Home, code: str, *, timeout: float = 15.0) -> dict:
    path = home.home / "data/logs/default/events.log"
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file():
            events = [json.loads(line) for line in path.read_text().splitlines()]
            matching = [event for event in events if event["event_code"] == code]
            if matching:
                return matching[-1]
        time.sleep(0.1)
    raise AssertionError(f"event {code} did not arrive")


def check_16(replay: Replay) -> None:
    with running_home(replay, "M2-16") as home:
        run_id = home.status()["run_id"]
        node_path = "/cores/eos/layers/process/groups/mujoco/nodes/sim"
        kill_backend_child(home)
        lost = await_event(home, "eos_backend_lost")
        assert lost["run_id"] == run_id and lost["node_path"] == node_path
        code, matching = home.current(source="eos", level="error", node_path=node_path)
        assert code == 200 and matching["ok"], matching
        assert matching["data"]["scope"]["run_id"] == run_id
        assert matching["data"]["anchor"]["kind"] == "first_ready_or_failure"
        assert matching["data"]["anchor"]["event"]["node_path"] is None
        assert any(item["event_code"] == "eos_backend_lost" and item["node_path"] == node_path
                   for item in matching["data"]["lines"]), matching
        assert all(item["source"] == "eos" and item["node_path"] == node_path
                   for item in matching["data"]["lines"]), matching
        code, page = home.current(source="egia", level="error", correlation_id="m2-no-match")
        assert code == 200 and page["ok"], page
        data = page["data"]
        assert data["scope"]["kind"] == "deployment_run"
        assert data["anchor"]["kind"] == "first_ready_or_failure"
        assert data["anchor"]["state"] == "found" and data["anchor"]["event"]["source"] != "egia"
        assert data["anchor"]["event"] not in data["lines"]
        assert data["lines"] == [] and data["diagnostics"] == []
        assert data["next_cursor"].startswith("lc1.")


def check_18(replay: Replay) -> None:
    with running_home(replay, "M2-18") as home:
        before = home.status()
        run_id = before["run_id"]
        code, stopped = home.api("process-control", "POST", {"schema_version": 1, "operation": "stop"})
        assert code == 200 and stopped["ok"], stopped
        state = home.status()
        assert state["run_id_state"] == "known" and state["run_id"] == run_id
        assert state["state"] == "stopped" and not state["processes"]
        code, started = home.run(sha256(home.graph))
        assert code == 200 and started["ok"], started
        active = home.status()
        failure_run = active["run_id"]
        assert failure_run != run_id and active["run_id_state"] == "known"
        eos_pid = next(item["pid"] for item in active["processes"] if item["id"] == "eos")
        killed_eos: set[int] = set()
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            failed = home.status()
            if failed["state"] in ("degraded", "failed"):
                break
            current_eos = next((item for item in failed["processes"] if item["id"] == "eos"), None)
            if current_eos is not None and current_eos["pid"] not in killed_eos:
                kill_backend_child(home)
                killed_eos.add(current_eos["pid"])
            time.sleep(0.1)
        assert failed["state"] == "degraded" and failed["last_error"] == "a managed process exited", failed
        assert len(killed_eos) >= 2 and eos_pid in killed_eos, killed_eos
        events = [json.loads(line) for line in (home.home / "data/logs/default/events.log").read_text().splitlines()]
        lost = [item for item in events if item["event_code"] == "eos_backend_lost"
                and item["run_id"] == failure_run]
        assert len(lost) >= 2 and eos_pid in {item["pid"] for item in lost}, lost
        assert failed["run_id_state"] == "known" and failed["run_id"] == failure_run
        assert failed["processes"] == []
        assert all(item["pid"] != eos_pid for item in failed["processes"])
        code, failed_page = home.current()
        assert code == 200 and failed_page["data"]["scope"]["run_id"] == failure_run
        home.restart_serve()
        recovered = home.status()
        assert recovered["run_id_state"] == "known" and recovered["run_id"] == failure_run
        code, page = home.current()
        assert code == 200 and page["data"]["scope"]["run_id"] == failure_run


def check_17(replay: Replay) -> None:
    with running_home(replay, "M2-17", {"ISOS_LOG_MAX_BYTES": "65536", "ISOS_LOG_BACKUPS": "1"}) as home:
        code, first = home.current()
        assert code == 200 and first["data"]["anchor"]["state"] == "found", first
        old_run = first["data"]["scope"]["run_id"]
        cursor = first["data"]["next_cursor"]
        logs = home.home / "data/logs/default"
        events = logs / "events.log"
        backup = logs / "events.log.1"
        assert events.is_file() and not backup.exists()
        def flood_until(rotated: object) -> None:
            for index in range(1000):
                reply = home.manager({"schema_version": 1, "op": "log_append", "deployment": "default",
                                      "log": {"source": "m2-flood", "level": "info",
                                              "event_code": "m2_rotation", "message": f"bounded {index}"}})
                assert reply["ok"] and reply["data"]["accepted"], reply
                if rotated():
                    return
            raise AssertionError("normal bounded log rotation did not occur")
        flood_until(lambda: backup.is_file())
        first_backup_inode = backup.stat().st_ino
        code, stopped = home.api("process-control", "POST", {"schema_version": 1, "operation": "stop"})
        assert code == 200 and stopped["ok"], stopped
        code, retained = home.current(cursor=cursor)
        assert code == 200 and retained["ok"], retained
        assert retained["data"]["scope"]["run_id"] == old_run
        assert retained["data"]["rotated"] and not retained["data"]["diagnostics"], retained
        assert any(item["event_code"] == "deployment_stopped" for item in retained["data"]["lines"]), retained
        flood_until(lambda: backup.is_file() and backup.stat().st_ino != first_backup_inode)
        code, expired = home.current(cursor=cursor)
        assert code == 200 and expired["ok"], expired
        assert any(item["code"] == "log_cursor_expired" for item in expired["data"]["diagnostics"]), expired
        code, restarted = home.run(sha256(home.graph))
        assert code == 200 and restarted["ok"], restarted
        code, changed = home.current(cursor=cursor)
        assert code == 200 and changed["ok"], changed
        assert changed["data"]["scope"]["run_id"] != old_run
        assert any(item["code"] == "log_run_changed" for item in changed["data"]["diagnostics"]), changed


def check_19(replay: Replay) -> None:
    with running_home(replay, "M2-19") as home:
        code, before = home.current()
        assert code == 200 and before["data"]["anchor"]["state"] == "found", before
        run_id = before["data"]["scope"]["run_id"]
        log_root = home.home / "data/logs/default"
        key = log_root / "cursor-key"
        assert key.is_file()
        key.unlink()
        code, lost = home.current()
        assert code == 200 and lost["ok"], lost
        assert lost["data"]["scope"]["run_id"] == run_id
        assert lost["data"]["anchor"]["state"] == "unavailable", lost
        codes = {item["code"] for item in lost["data"]["diagnostics"]}
        assert {"log_collector_gap", "log_anchor_unavailable"} <= codes, lost
        state_file = log_root / "cursor-state.json"
        prior_state = state_file.read_bytes()
        state_file.write_text("{corrupt}")
        code, unavailable = home.current()
        assert code == 503 and not unavailable["ok"], unavailable
        assert any(item["code"] == "log_scope_unavailable" and item["path"] == "/scope"
                   for item in unavailable["data"]["diagnostics"]), unavailable
        state_file.write_bytes(prior_state)
        code, recovered = home.current()
        assert code == 200 and recovered["ok"], recovered
        assert recovered["data"]["scope"]["run_id"] == run_id
        assert recovered["data"]["anchor"]["state"] == "unavailable"


def check_22(replay: Replay) -> None:
    with running_home(replay, "M2-22") as home:
        run_id = home.status()["run_id"]
        code, stopped = home.api("process-control", "POST", {"schema_version": 1, "operation": "stop"})
        assert code == 200 and stopped["ok"], stopped
        assert home.status()["state"] == "stopped"
        events = [json.loads(line) for line in (home.home / "data/logs/default/events.log").read_text().splitlines()]
        assert any(item["run_id"] == run_id and item["event_code"] == "deployment_stopped" for item in events)
        code, page = home.current()
        assert code == 200 and page["data"]["scope"]["run_id"] == run_id


def check_15(replay: Replay) -> None:
    with replay.case("M2-15") as home:
        home.init()
        home.serve()
        code, before = home.api("logs?view=current&source=eos&level=error")
        assert code == 200 and before["data"]["scope"]["kind"] == "manager_session", before
        assert before["data"]["anchor"]["kind"] == "first_prelaunch_failure"
        assert before["data"]["anchor"]["state"] == "pending"
        invalid = copy.deepcopy(json.loads(home.saved()))
        invalid["version"] = "99.0.0"
        code, validation = home.validate(invalid)
        assert code == 200 and not validation["data"]["valid"], validation
        code, still_pending = home.current(source="eos", level="error")
        assert code == 200 and still_pending["data"]["anchor"]["state"] == "pending", still_pending
        # Invalid package request must produce a typed manager event before returning.
        code, failed = home.api("packages/link", "POST", {"id": "island/mujoco-sim",
                                                         "version": "0.1.1", "path": "/missing"})
        assert code >= 400 and not failed["ok"], failed
        code, after = home.api("logs?view=current&source=eos&level=error")
        assert code == 200 and after["data"]["anchor"]["state"] == "found", after
        assert after["data"]["anchor"]["event"]["source"] == "packages"
        assert after["data"]["anchor"]["event"]["run_id"] == before["data"]["scope"]["run_id"]


def check_14(replay: Replay) -> None:
    with running_home(replay, "M2-14") as home:
        run_id = home.status()["run_id"]
        secret = "M2_RAW_FRAME_PROMPT_SECRET_4931"
        reply = home.manager({"schema_version": 1, "op": "log_append", "deployment": "default",
                              "log": {"source": "eos", "level": "warn", "event_code": "eos_ready",
                                      "message": secret}})
        assert reply["ok"] and reply["data"]["accepted"], reply
        events_path = home.home / "data/logs/default/events.log"
        lines = events_path.read_text().splitlines()
        assert secret not in "\n".join(lines)
        events = [json.loads(line) for line in lines]
        assert any(item["event_code"] == "legacy_external_event" and item["source"] == "legacy_external"
                   for item in events)
        assert any(item["event_code"] == "eos_ready" and item["source"] == "eos"
                   and item["run_id"] == run_id for item in events)
        assert any(item["event_code"] == "deployment_ready" and item["source"] == "isos"
                   and item["run_id"] == run_id and item["correlation_id"] for item in events)
        for test in ("repeated_noncritical_child_event_has_bounded_terminal_summary",
                     "selected_envelope_rejects_spoofing_and_unsafe_shapes"):
            result = replay.run_command(["cargo", "test", "--no-default-features",
                                         "--bin", "isos", test],
                                        home.directory / "owner-boundary.log", env=os.environ.copy(),
                                        timeout=180, cwd=replay.args.isos_root)
            assert result.returncode == 0 and "1 passed" in result.stdout, (test, result.stdout)


def check_23(replay: Replay) -> None:
    with replay.case("M2-23") as home:
        home.init()
        legacy_bundle = home.root / "legacy-bundle"
        shutil.copytree(replay.bundle, legacy_bundle)
        (legacy_bundle / "release.json").unlink()
        home.cmd("pkg", "link", "island/mujoco-sim", "0.1.1", str(legacy_bundle))
        listing = json.loads(home.cmd("pkg", "list", "--json").stdout)
        receipt = next(item for item in listing if item["id"] == "island/mujoco-sim")
        assert receipt["schema_version"] == 1 and receipt["status"] == "unverified_legacy_link"
        graph = replay.graph_for(home)
        saved = home.saved()
        home.serve()
        code, applied = home.apply(graph)
        assert code == 422 and not applied["ok"], applied
        expected_path = "/cores/eos/layers/process/groups/mujoco/nodes/sim/package"
        assert any(item["code"] == "package_invalid_manifest" and item["stage"] == "manifest"
                   and item["path"] == expected_path for item in applied["data"]["diagnostics"]), applied
        assert home.saved() == saved
        # Seed a saved graph as setup only: Run must still reverify the legacy link.
        home.graph.write_text(json.dumps(graph) + "\n")
        seeded = home.saved()
        code, rejected = home.api("process-control", "POST", {"schema_version": 1, "operation": "run",
                                                             "revision": sha256(home.graph)})
        assert code == 422 and not rejected["ok"], rejected
        assert any(item["code"] == "package_invalid_manifest" and item["stage"] == "manifest"
                   and item["path"] == expected_path for item in rejected["data"]["diagnostics"]), rejected
        assert home.saved() == seeded and home.status()["processes"] == []


def check_24(replay: Replay) -> None:
    with replay.case("M2-24") as home:
        home.init()
        home.link()
        graph = replay.graph_for(home)
        home.modified_capability("aether-profile", remove_type="eos.hardware.camera@v0.1")
        saved = home.saved()
        home.serve()
        code, result = home.apply(graph)
        assert code == 422 and not result["ok"], result
        assert any(item["code"] == "reader_type_unsupported" and item["stage"] == "reader"
                   and item["path"] == "/cores/eos/layers/hardware/groups/camera/nodes/front/node_type"
                   and item["reader"]["binary"] == "aether-profile" for item in result["data"]["diagnostics"]), result
        assert home.saved() == saved and home.status()["processes"] == []


def check_25(replay: Replay) -> None:
    with replay.case("M2-25") as home:
        home.init()
        home.link()
        graph = replay.graph_for(home)
        wheel = copy.deepcopy(graph["cores"]["eos"]["layers"]["hardware"]["groups"]["base"]["nodes"]["drive"])
        wheel["id"] = "wheel"
        wheel["node_type"] = "eos.hardware.wheel@v0.1"
        graph["cores"]["eos"]["layers"]["hardware"]["groups"]["base"]["nodes"]["wheel"] = wheel
        saved = home.saved()
        home.serve()
        code, result = home.apply(graph)
        assert code == 422 and not result["ok"], result
        assert any(item["code"] == "unsupported_node_type"
                   and item["path"] == "/cores/eos/layers/hardware/groups/base/nodes/wheel/node_type"
                   for item in result["data"]["diagnostics"]), result
        assert home.saved() == saved and home.status()["processes"] == []


def check_26(replay: Replay) -> None:
    with replay.case("M2-26") as home:
        home.init()
        copy_bundle = home.root / "mutable-bundle"
        shutil.copytree(replay.bundle, copy_bundle)
        home.cmd("pkg", "link", "island/mujoco-sim", "0.1.1", str(copy_bundle),
                 "--manifest", str(copy_bundle / "release.json"))
        graph = replay.graph_for(home)
        sidecar = home.home / "data/packages/island/mujoco-sim/0.1.1/node-types.json"
        assert sidecar.is_file(), sidecar
        sidecar.write_bytes(sidecar.read_bytes() + b"\n")
        saved = home.saved()
        home.serve()
        code, result = home.apply(graph)
        assert code == 422 and not result["ok"], result
        assert any(item["code"] == "package_invalid_sidecar" and item["stage"] == "sidecar"
                   and item["path"] == "/cores/eos/layers/process/groups/mujoco/nodes/sim/package"
                   for item in result["data"]["diagnostics"]), result
        assert home.saved() == saved and home.status()["processes"] == []


def check_27(replay: Replay) -> None:
    with replay.case("M2-27") as home:
        home.init()
        home.link()
        graph = replay.graph_for(home)
        graph["cores"]["eos"]["layers"]["process"]["groups"]["mujoco"]["nodes"]["sim"]["package"]["version"] = "0.1.0"
        original = home.saved()
        home.serve()
        code, rejected = home.apply(graph)
        assert code == 422 and not rejected["ok"], rejected
        path = "/cores/eos/layers/process/groups/mujoco/nodes/sim/package"
        assert any(item["code"] == "package_missing_file" and item["stage"] == "manifest"
                   and item["path"] == path for item in rejected["data"]["diagnostics"]), rejected
        assert home.saved() == original and home.status()["processes"] == []
        home.graph.write_text(json.dumps(graph) + "\n")
        seeded = home.saved()
        code, rejected = home.run(sha256(home.graph))
        assert code == 422 and not rejected["ok"], rejected
        assert any(item["code"] == "package_missing_file" and item["stage"] == "manifest"
                   and item["path"] == path for item in rejected["data"]["diagnostics"]), rejected
        assert home.saved() == seeded and home.status()["processes"] == []


def check_28(replay: Replay) -> None:
    with replay.case("M2-28") as home:
        command = ["cargo", "test", "--no-default-features", "--bin", "isos",
                   "contributed_type_needs_exact_catalog_pin"]
        result = replay.run_command(command, home.directory / "owner-predicate.log", env=os.environ.copy(),
                                    timeout=180, cwd=replay.args.isos_root)
        assert result.returncode == 0 and "1 passed" in result.stdout, result.stdout


def aggregate(args: argparse.Namespace) -> None:
    expected = {item["id"] for item in CASES}
    current_code = {"harness_sha256": sha256(HARNESS), "fixture_sha256": sha256(FIXTURE),
                    "wrapper_sha256": sha256(WRAPPER)}
    current_heads = {name: head(getattr(args, f"{name}_root"))
                     for name in ("eos", "isos", "egia", "mujoco", "isosui")}
    binaries = {"isos": args.isos_root / "target/debug/isos",
                **{name: args.eos_root / "target/debug" / name for name in
                   ("aether-profile", "aether-runtime", "aether-http-bridge", "aether-config-server")},
                "mujoco": args.mujoco_root / "build/eos-mujoco-sim"}
    current_binaries = {name: sha256(path) for name, path in binaries.items()}
    current_selected_inputs = selected_inputs(args)
    combined: dict[str, dict] = {}
    evidence = []
    for directory in args.aggregate_batches:
        directory = directory.resolve()
        environment = json.loads((directory / "environment.json").read_text())
        bundle = json.loads((directory / "bundle-digests.json").read_text())
        bundle_files = json.loads((directory / "bundle-file-hashes.json").read_text())
        release = json.loads((directory / "bundle-release.json").read_text())
        results = json.loads((directory / "results.json").read_text())
        attestation = json.loads((directory / "batch-attestation.json").read_text())
        assert all(environment[key] == value == attestation[key]
                   for key, value in current_code.items()), (directory, "harness or fixture mismatch")
        assert environment["heads"] == current_heads, (directory, "HEAD mismatch")
        assert environment["binaries"] == current_binaries, (directory, "binary mismatch")
        assert environment["selected_inputs"] == current_selected_inputs, (directory, "EGIA/UI input mismatch")
        assert environment["mujoco_library_sha256"] == sha256(args.mujoco_library_root / "lib/libmujoco.so"), directory
        assert set(bundle) == {"release", "archive", "descriptor", "payload_tree"}
        assert all(len(value) == 64 and all(char in "0123456789abcdef" for char in value)
                   for value in bundle.values()), (directory, "invalid package digest")
        assert bundle["payload_tree"] == digest_json(bundle_files), (directory, "package tree mismatch")
        assert bundle_files["release.json"]["sha256"] == bundle["release"] == sha256(
            directory / "bundle-release.json"), (directory, "release mismatch")
        assert bundle_files["backend-v2.json"]["sha256"] == bundle["descriptor"], (
            directory, "descriptor mismatch")
        assert release["schema_version"] == 4 and release["id"] == "island/mujoco-sim"
        assert release["version"] == "0.1.1" and release["source_commit"] == current_heads["mujoco"]
        assert release["assets"][0]["sha256"] == bundle["archive"], (directory, "archive mismatch")
        assert attestation["environment_sha256"] == sha256(directory / "environment.json"), directory
        assert attestation["bundle_digests_sha256"] == sha256(directory / "bundle-digests.json"), directory
        assert attestation["bundle_file_hashes_sha256"] == sha256(directory / "bundle-file-hashes.json"), directory
        assert attestation["bundle_release_sha256"] == sha256(directory / "bundle-release.json"), directory
        assert attestation["results_sha256"] == sha256(directory / "results.json"), directory
        assert attestation["bundle_log_sha256"] == sha256(directory / "bundle.log"), directory
        assert attestation["selected_cases"] == sorted(results), directory
        assert set(attestation["transcripts"]) == set(results), directory
        support = {path.name: tree_manifest(path) for path in directory.iterdir()
                   if path.is_dir() and path.name not in results}
        assert all(not name.startswith("M2-") for name in support), directory
        assert support == attestation["support_transcripts"], (directory, "support transcript mismatch")
        for case_id, result in results.items():
            assert case_id in expected and case_id not in combined, (directory, case_id)
            assert result["status"] == "pass", (directory, case_id, result)
            case_dir = directory / case_id
            actual_transcript = tree_manifest(case_dir)
            assert actual_transcript and actual_transcript == attestation["transcripts"][case_id], (
                directory, case_id, "transcript mismatch")
            combined[case_id] = result
        evidence.append({"directory": str(directory), "attestation_sha256": sha256(directory / "batch-attestation.json"),
                         "results_sha256": sha256(directory / "results.json"),
                         "package_digests": bundle, "cases": sorted(results)})
    assert set(combined) == expected, {"missing": sorted(expected - set(combined)),
                                       "extra": sorted(set(combined) - expected)}
    args.artifacts.mkdir(parents=True, exist_ok=True)
    (args.artifacts / "aggregate.json").write_text(json.dumps({"status": "pass", "cases": len(combined),
        "code": current_code, "heads": current_heads, "binaries": current_binaries,
        "selected_inputs": current_selected_inputs, "batches": evidence}, indent=2, sort_keys=True) + "\n")
    print(f"M2 pinned merged replay: PASS ({len(combined)} cases across {len(evidence)} bounded batches)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true", help="execute pinned replay (default: list inputs)")
    parser.add_argument("--only", help="comma-separated case IDs for a focused development run")
    parser.add_argument("--aggregate-batches", nargs="+", type=Path,
                        help="verify a complete exact-pin replay from bounded batch transcripts")
    for name, default in (("eos", ROOT.parent / "Island-eos"), ("isos", ROOT.parent / "isos"),
                          ("egia", ROOT.parent / "Island-egia"), ("mujoco", ROOT.parent / "isos-mujoco-sim"),
                          ("isosui", ROOT.parent / "IsosUI")):
        parser.add_argument(f"--{name}-root", type=Path, default=default)
        parser.add_argument(f"--expect-{name}", help=f"exact required {name} HEAD")
    parser.add_argument("--mujoco-library-root", type=Path, required=True, help="MuJoCo 3.16.0 runtime root")
    parser.add_argument("--artifacts", type=Path, default=ROOT / "logs/test" / f"{time.strftime('%Y%m%d-%H%M%S')}-m2-replay")
    args = parser.parse_args()
    args.mujoco_library_root = args.mujoco_library_root.resolve()
    for name in ("eos", "isos", "egia", "mujoco", "isosui"):
        setattr(args, f"{name}_root", getattr(args, f"{name}_root").resolve())
    if not __debug__:
        parser.error("assertions must be enabled; do not use python -O")
    if not args.run and not args.aggregate_batches:
        print(json.dumps({"cases": [item["id"] for item in CASES],
                          "roots": {name: str(getattr(args, f"{name}_root")) for name in
                                    ("eos", "isos", "egia", "mujoco", "isosui")}}, indent=2))
        return
    for name in ("eos", "isos", "egia", "mujoco", "isosui"):
        expected = getattr(args, f"expect_{name}")
        if not expected or head(getattr(args, f"{name}_root")) != expected:
            parser.error(f"{name} checkout does not match required exact HEAD {expected}")
    if args.aggregate_batches:
        if args.run or args.only:
            parser.error("--aggregate-batches cannot be combined with --run or --only")
        aggregate(args)
        return
    replay = Replay(args)
    for path in replay.binaries.values():
        if not path.is_file():
            raise FileNotFoundError(path)
    replay.build_bundle()
    checks = {"M2-01": check_01, "M2-02": check_02, "M2-03": check_03,
              "M2-04": check_04, "M2-05": check_05, "M2-06": check_06,
              "M2-07": check_07, "M2-08": check_08, "M2-09": check_09, "M2-10": check_10,
              "M2-11": check_11, "M2-12": check_12, "M2-13": check_13, "M2-14": check_14,
              "M2-15": check_15, "M2-16": check_16, "M2-17": check_17, "M2-18": check_18,
              "M2-19": check_19, "M2-20": check_20, "M2-21": check_21, "M2-22": check_22,
              "M2-23": check_23, "M2-24": check_24,
              "M2-25": check_25, "M2-26": check_26, "M2-27": check_27, "M2-28": check_28}
    selected = set(args.only.split(",")) if args.only else {item["id"] for item in CASES}
    if not selected <= {item["id"] for item in CASES}:
        parser.error("--only contains an unknown frozen case ID")
    for item in CASES:
        case_id = item["id"]
        if case_id not in selected:
            continue
        if case_id not in checks:
            replay.result(case_id, "blocked", "executor for this frozen boundary is not yet implemented")
            continue
        try:
            checks[case_id](replay)
            replay.result(case_id, "pass", "assertions and private transcript saved")
        except Exception as error:  # Continue so every frozen case has an explicit result.
            replay.result(case_id, "fail", f"{type(error).__name__}: {error}")
    if any(result["status"] != "pass" for result in replay.results.values()):
        raise SystemExit("M2 gate incomplete; inspect results.json")
    replay.attest(selected)
    if len(selected) == len(CASES):
        print("M2 clean-home replay: PASS")
    else:
        print(f"M2 focused development replay: {len(selected)} case(s) passed; full gate not evaluated")


if __name__ == "__main__":
    main()
