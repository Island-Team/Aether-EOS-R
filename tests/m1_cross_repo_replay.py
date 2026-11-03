#!/usr/bin/env python3
"""Replay M1 first use across pinned EOS, IsOS, EGIA, and IsosUI checkouts.

Run ``python3 tests/m1_cross_repo_replay.py --help`` for path and revision
arguments. The output directory defaults to ignored ``logs/test`` artifacts.
"""

from __future__ import annotations

import argparse
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
from urllib.request import Request, urlopen


EOS = Path(__file__).resolve().parents[1]
ISOS = EOS.parent / "isos"
EGIA = EOS.parent / "Island-egia"
UI = EOS.parent / "IsosUI"
ARTIFACTS = EOS / "logs/test" / f"{time.strftime('%Y%m%d-%H%M%S')}-m1-cross-repo"
FIXTURES = ISOS / "tests/fixtures/graph_compat"
EOS_EDGES = EOS / "tests/fixtures/edge_contract/valid_evidence.json"
ISOS_EDGE_VALID = ISOS / "tests/fixtures/edge_admission/valid_0_1_0.json"
ISOS_EDGE_CROSS_CORE = ISOS / "tests/fixtures/edge_admission/cross_core_guard_request.json"
EXPECTED = {
    "legacy_1_0_0.json": "7a8daee6b7db9105f5483506ffc1743625b63089710dfbf936622329c471016b",
    "legacy_1_0_0_whitespace.json": "27e137a1387393d7fb2e1abc1b735315cbdefd47a02ae2b265fa3596720158e9",
    "future_2_0_0.json": "f4552929021608b7bb8dc4b141e4a93982636cb8010bc61c88598dc3589e4b29",
    "unknown_type_1_0_0.json": "087ce65c200debb12631d0fee566de8031dfcf1f68c76bddee46e23d1f8ec802",
    "unsafe_component_1_0_0.json": "09a8af69de54448e30d4bbfb7ed027e7b58ffba009578848458a5147e9cd7273",
    "legacy_optional_defaults_1_0_0.json": "44bc481fddc7cb910b7fab49d0e7dc417f91e845bcbb576cf8ec21c3231aac5f",
    "legacy_unmapped_1_0_0.json": "bd0213124fe81b54497f00a8ad034798ebccf5eee8312d0be88a261eeb1c584f",
}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def at_pointer(value: object, pointer: str) -> object:
    for segment in pointer.removeprefix("/").split("/"):
        value = value[segment.replace("~1", "/").replace("~0", "~")]
    return value


def free_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def git_head(repo: Path) -> str:
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()


class Case:
    def __init__(self, name: str, isos_bin: Path, eos_bin: Path, ui_dist: Path):
        self.name = name
        self.directory = ARTIFACTS / name
        self.directory.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(prefix=f"m1-{name[:9]}-"))
        self.home = self.root / "home"
        self.graph = self.home / "config/deployments/default.nodes.json"
        self.graph.parent.mkdir(parents=True)
        self.isos_bin = isos_bin
        self.eos_bin = self.root / "aether-bin"
        self.eos_bin.mkdir()
        for name in ("aether-profile", "aether-runtime", "aether-http-bridge", "aether-config-server"):
            shutil.copy2(eos_bin / name, self.eos_bin / name)
        self.api_port, self.ui_port = free_port(), free_port()
        self.env = dict(
            os.environ,
            ISOS_HOME=str(self.home),
            ISOS_HTTP_PORT=str(self.api_port),
            ISOSUI_PORT=str(self.ui_port),
            ISOS_AETHER_BIN_DIR=str(self.eos_bin),
            ISOS_EGIA_BIN_DIR=str(EGIA / "bin"),
            ISOSUI_DIST_DIR=str(ui_dist),
        )
        self.server: subprocess.Popen[str] | None = None
        self.runner: subprocess.Popen[str] | None = None
        self.serve_count = 0
        self._logs = []
        print(f"CASE {name}: isolated home {self.home}", flush=True)

    def log(self, label: str, value: object) -> None:
        path = self.directory / f"{label}.json"
        path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
        print(f"{label}: {json.dumps(value, sort_keys=True)}", flush=True)

    def command(self, *args: str, expect: int | None = 0) -> subprocess.CompletedProcess[str]:
        command = [str(self.isos_bin), *args]
        result = subprocess.run(command, env=self.env, text=True, capture_output=True, timeout=35)
        print(f"$ {' '.join(command)}\nexit={result.returncode}\n{result.stdout}{result.stderr}", flush=True)
        with (self.directory / "commands.log").open("a") as output:
            output.write(f"$ {' '.join(command)}\nexit={result.returncode}\n{result.stdout}{result.stderr}\n")
        if expect is not None and result.returncode != expect:
            raise AssertionError(f"{self.name}: expected exit {expect}, got {result.returncode}")
        return result

    def api(self, path: str, method: str = "GET", data: object | None = None) -> tuple[int, dict]:
        request = Request(
            f"http://127.0.0.1:{self.api_port}/api/isos/v1/{path}",
            method=method,
            data=None if data is None else json.dumps(data).encode(),
            headers={
                "Origin": f"http://127.0.0.1:{self.api_port}",
                "Content-Type": "application/json",
            },
        )
        try:
            response = urlopen(request, timeout=12)
        except HTTPError as error:
            response = error
        with response:
            status = response.status
            body = json.load(response)
        self.log(f"api-{path.replace('/', '-')}-{method.lower()}-{int(time.time() * 1000)}", {"status": status, "body": body})
        return status, body

    def serve(self) -> None:
        if self.server is not None:
            # A foreground Run interrupt closes the manager socket but can
            # leave the old HTTP listener alive. Reap it before readiness.
            if self.server.poll() is None:
                self.server.send_signal(signal.SIGINT)
            self.server.wait(timeout=12)
        self.serve_count += 1
        log_name = "serve.log" if self.serve_count == 1 else f"serve-{self.serve_count}.log"
        output = (self.directory / log_name).open("w")
        self._logs.append(output)
        self.server = subprocess.Popen([str(self.isos_bin), "serve"], env=self.env, stdout=output, stderr=subprocess.STDOUT, text=True)
        for _ in range(160):
            if self.server.poll() is not None:
                raise RuntimeError(f"IsOS serve exited {self.server.returncode}: {(self.directory / 'serve.log').read_text()}")
            try:
                self.api("node-graph")
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("IsOS API did not become ready")

    def start(self) -> None:
        output = (self.directory / "run.log").open("w")
        self._logs.append(output)
        self.runner = subprocess.Popen([str(self.isos_bin), "run"], env=self.env, stdout=output, stderr=subprocess.STDOUT, text=True)
        for _ in range(220):
            if self.runner.poll() is not None:
                raise RuntimeError(f"IsOS run exited {self.runner.returncode}: {(self.directory / 'run.log').read_text()}")
            status, body = self.api("process-status")
            if status == 200 and body.get("data", {}).get("state") == "running":
                return
            time.sleep(0.05)
        raise RuntimeError("IsOS deployment did not become ready")

    def status(self) -> dict:
        status, body = self.api("process-status")
        assert status == 200 and body["ok"], body
        return body["data"]

    def snapshot(self, label: str) -> str:
        data = self.graph.read_bytes()
        revision = digest(data)
        (self.directory / f"{label}.graph.json").write_bytes(data)
        self.log(f"{label}-revision", {"sha256": revision, "bytes": len(data)})
        return revision

    def close(self) -> None:
        for child in [self.runner, self.server]:
            if child is not None and child.poll() is None:
                child.send_signal(signal.SIGINT)
                try:
                    child.wait(timeout=12)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
        if self.home.exists():
            for path in (self.home / "data/generated").rglob("manifest.json"):
                target = self.directory / "manifests" / path.relative_to(self.home)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)
        for output in self._logs:
            output.close()
        print(f"CASE {self.name}: home retained at {self.home}", flush=True)


def legacy_and_negative(isos_bin: Path, eos_bin: Path, ui_dist: Path) -> None:
    cases = {
        "legacy_1_0_0.json": None,
        "legacy_1_0_0_whitespace.json": None,
        "future_2_0_0.json": ("/version", "unsupported_graph_version"),
        "unknown_type_1_0_0.json": ("/cores/eos/layers/dispatcher/groups/control/nodes/command/node_type", "unsupported_node_type"),
        "unsafe_component_1_0_0.json": ("/cores/isos/layers/process/groups/component/nodes/eos/node_type", "unsafe_process_node"),
        "legacy_optional_defaults_1_0_0.json": None,
        "legacy_unmapped_1_0_0.json": ("/cores/eos/layers/control/groups/manual/nodes/manual_0/node_type", "unmapped_node_type"),
    }
    baseline_projection = None
    approved_map = json.loads((FIXTURES / "migration_mapping.json").read_text())["type_map"]
    for filename, expected_diagnostic in cases.items():
        case = Case(filename.removesuffix(".json"), isos_bin, eos_bin, ui_dist)
        try:
            source = (FIXTURES / filename).read_bytes()
            assert digest(source) == EXPECTED[filename], f"frozen fixture drift: {filename}"
            case.graph.write_bytes(source)
            case.serve()
            before = case.snapshot("before")
            status, get = case.api("node-graph")
            assert status == 200 and get["data"]["revision"] == before, get
            assert get["data"]["graph"] == json.loads(source), get
            assert case.graph.read_bytes() == source
            if expected_diagnostic:
                pointer, code = expected_diagnostic
                assert get["data"]["read_only"] is True, get
                assert "legacy_projection" not in get["data"], get
                assert (get["data"]["diagnostics"][0]["path"], get["data"]["diagnostics"][0]["code"]) == (pointer, code), get
                status, validation = case.api("node-graph/validate", "POST", {"schema_version": 1, "graph": get["data"]["graph"]})
                assert status == 200 and validation["data"]["valid"] is False, validation
                issue = validation["data"]["diagnostics"][0]
                assert (issue["path"], issue["code"]) == (pointer, code), issue
                status, applied = case.api("node-graph", "PUT", {"schema_version": 1, "revision": before, "graph": get["data"]["graph"]})
                assert status == 422, applied
                assert applied["data"]["diagnostics"][0]["path"] == pointer, applied
                result = case.command("run", expect=None)
                assert result.returncode != 0, result
            else:
                projection = get["data"]["legacy_projection"]
                assert projection["read_only"] is True and projection["source_revision"] == before, get
                assert projection["graph"]["version"] == "1.0.0", projection
                for mapping in projection["mappings"]:
                    assert at_pointer(projection["graph"], mapping["path"]) == mapping["source_type"], mapping
                    assert approved_map[mapping["source_type"]] == mapping["candidate_type"], mapping
                    assert at_pointer(get["data"]["graph"], mapping["path"]) == mapping["source_type"], mapping
                if filename in ("legacy_1_0_0.json", "legacy_1_0_0_whitespace.json"):
                    comparable = {key: value for key, value in projection.items() if key != "source_revision"}
                    if baseline_projection is None:
                        baseline_projection = comparable
                    else:
                        assert comparable == baseline_projection, projection
                if filename == "legacy_optional_defaults_1_0_0.json":
                    defaults = {entry["path"]: entry["value"] for entry in projection["defaults"]}
                    assert defaults["/cores/eos/layers/dispatcher/groups/control/nodes/command/capabilities"] == [], defaults
                    assert defaults["/cores/isos/layers/process/groups/component/nodes/eos/settings/max_restarts"] == 0, defaults
                    status, validation = case.api("node-graph/validate", "POST", {"schema_version": 1, "graph": get["data"]["graph"]})
                    assert status == 200 and validation["data"]["valid"] is True, validation
                    status, noop = case.api("node-graph", "PUT", {"schema_version": 1, "revision": before, "graph": get["data"]["graph"]})
                    assert status == 200 and noop["data"]["revision"] == before, noop
            if filename == "legacy_1_0_0.json":
                status, preview = case.api("node-graph/migration/preview", "POST", {"schema_version": 1, "revision": before})
                assert status == 200 and preview["data"]["revision"] == before, preview
                stale = "0" * 64 if before != "0" * 64 else "1" * 64
                status, _ = case.api("node-graph/migration/apply", "POST", {"schema_version": 1, "revision": stale, "proposed_revision": preview["data"]["proposed_revision"]})
                assert status == 409
            assert case.snapshot("after") == before
            assert case.status().get("active_revision") in (None, ""), case.status()
        finally:
            case.close()


def adjusted_legacy(case: Case) -> bytes:
    graph = json.loads((FIXTURES / "legacy_1_0_0.json").read_text())
    nodes = graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]
    sock = str(case.home / "run/eos.sock")
    bridge_port, config_port = free_port(), free_port()
    eos = nodes["eos"]["settings"]
    eos["args"] = ["--profile", str(case.graph), "--journal-dir", str(case.home / "data/journal"), "--socket", sock]
    eos["ready_unix"] = sock
    bridge = nodes["bridge"]["settings"]
    bridge["args"] = ["--socket", sock, "--listen", f"127.0.0.1:{bridge_port}"]
    bridge["ready_tcp"] = f"127.0.0.1:{bridge_port}"
    config = nodes["config"]["settings"]
    config["args"] = ["--profile", str(case.graph), "--listen", f"127.0.0.1:{config_port}"]
    config["ready_tcp"] = f"127.0.0.1:{config_port}"
    return (json.dumps(graph, indent=2) + "\n").encode()


def invalid_edge_variants(graph: dict) -> list[tuple[str, dict, str, str]]:
    result = json.loads(json.dumps(graph))
    eos = result["cores"]["eos"]["layers"]
    fixture = json.loads(EOS_EDGES.read_text())["cores"]["eos"]["layers"]
    eos["hardware"] = fixture["hardware"]
    eos["interface"] = fixture["interface"]
    eos["dispatcher"]["groups"].update(fixture["dispatcher"]["groups"])
    source = eos["interface"]["groups"]["gate"]["nodes"]["front"]["inputs"]["frame"]
    source["output"] = "health"
    pointer = "/cores/eos/layers/interface/groups/gate/nodes/front/inputs/frame"
    wrong_type = ("wrong-type", result, pointer, "edge_type_mismatch")
    wrong_priority = json.loads(json.dumps(result))
    wrong_priority["cores"]["eos"]["layers"]["interface"]["groups"]["gate"]["nodes"]["front"]["inputs"]["frame"]["output"] = "frame"
    wrong_priority["cores"]["eos"]["layers"]["interface"]["groups"]["gate"]["nodes"]["front"]["inputs"]["frame"]["priority"] = 20
    priority = ("priority-bound", wrong_priority, f"{pointer}/priority", "edge_priority")
    fixed_qos = json.loads(json.dumps(wrong_priority))
    fixed_qos["cores"]["eos"]["layers"]["interface"]["groups"]["gate"]["nodes"]["front"]["inputs"]["frame"]["priority"] = 0
    fixed_qos["cores"]["eos"]["layers"]["hardware"]["groups"]["base"]["nodes"]["drive"]["inputs"]["command"][0]["priority"] = 0
    fixed_pointer = "/cores/eos/layers/hardware/groups/base/nodes/drive/inputs/command/0/priority"
    fixed = ("fixed-command-qos", fixed_qos, fixed_pointer, "edge_qos_forbidden")
    cross_core = json.loads(json.dumps(graph))
    cross_fixture = json.loads(ISOS_EDGE_CROSS_CORE.read_text())
    cross_core["cores"]["egia"] = cross_fixture["cores"]["egia"]
    request = cross_fixture["cores"]["eos"]["layers"]["dispatcher"]["groups"]["control"]["nodes"]["command"]["inputs"]
    cross_core["cores"]["eos"]["layers"]["dispatcher"]["groups"]["control"]["nodes"]["command"]["inputs"] = request
    cross_pointer = "/cores/eos/layers/dispatcher/groups/control/nodes/command/inputs/request"
    cross = ("cross-core-guard-request", cross_core, cross_pointer, "edge_bridge_undeclared")
    return [wrong_type, priority, fixed, cross]


def migration_and_projected_start(isos_bin: Path, eos_bin: Path, ui_dist: Path, include_edges: bool) -> None:
    case = Case("migration-start", isos_bin, eos_bin, ui_dist)
    try:
        case.graph.write_bytes(adjusted_legacy(case))
        case.serve()
        initial = case.snapshot("adapted-source")
        case.command("config", "apply", str(case.graph))
        old = case.snapshot("legacy-applied")
        case.start()
        assert case.status().get("active_revision") == old, case.status()
        status, preview = case.api("node-graph/migration/preview", "POST", {"schema_version": 1, "revision": old})
        assert status == 200 and preview["data"]["can_apply"], preview
        proposed = preview["data"]["proposed_revision"]
        assert case.snapshot("after-preview") == old
        cli_preview = json.loads(case.command("config", "migrate", "--preview", "--revision", old).stdout)
        assert cli_preview["proposed_revision"] == proposed, cli_preview
        status, committed = case.api("node-graph/migration/apply", "POST", {"schema_version": 1, "revision": old, "proposed_revision": proposed})
        assert status == 200 and committed["data"]["revision"] == proposed, committed
        assert case.snapshot("migrated") == proposed
        assert case.status().get("active_revision") == old, case.status()
        migrated = json.loads(case.graph.read_text())
        assert migrated["version"] == "0.1.0"
        assert migrated["cores"]["eos"]["layers"]["dispatcher"]["groups"]["control"]["nodes"]["command"]["node_type"] == "eos.dispatcher.control@v0.1"
        assert all(node["node_type"] == "isos.process.component@v0.1" for node in migrated["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"].values())
        assert migrated["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["eos"]["settings"]["args"][1] == str(case.graph)
        generated = case.home / "data/generated/default" / proposed
        projection = generated / "eos/graph.json"
        assert projection.is_file() and json.loads(projection.read_text())["version"] == "0.1.0"
        assert (generated / "manifest.json").is_file()
        case.log("projection", {"path": str(projection), "sha256": digest(projection.read_bytes()), "source_sha256": initial})
        if include_edges:
            eos_with_edges = json.loads(json.dumps(migrated))
            eos_fixture = json.loads(EOS_EDGES.read_text())
            eos_with_edges["cores"]["eos"]["layers"] = eos_fixture["cores"]["eos"]["layers"]
            (case.directory / "positive-eos-edges.json").write_text(json.dumps(eos_with_edges, indent=2) + "\n")
            status, validated = case.api("node-graph/validate", "POST", {"schema_version": 1, "graph": eos_with_edges})
            assert status == 200 and validated["data"]["valid"] is True and validated["data"]["diagnostics"] == [], validated
            # EGIA's current owner contract gates validate/render on a verified
            # gRPC endpoint, which IsOS cannot provide until M3.
            with_egia = json.loads(json.dumps(eos_with_edges))
            with_egia["cores"]["egia"] = json.loads(ISOS_EDGE_VALID.read_text())["cores"]["egia"]
            (case.directory / "gated-eos-egia-edges.json").write_text(json.dumps(with_egia, indent=2) + "\n")
            status, gated = case.api("node-graph/validate", "POST", {"schema_version": 1, "graph": with_egia})
            assert status == 200 and gated["data"]["valid"] is False, gated
            assert (gated["data"]["diagnostics"][0]["path"], gated["data"]["diagnostics"][0]["code"]) == ("/cores/egia", "owner_validation_failed"), gated
            bad_variants = invalid_edge_variants(migrated)
            for label, bad, pointer, code in bad_variants:
                status, validation = case.api("node-graph/validate", "POST", {"schema_version": 1, "graph": bad})
                assert status == 200 and validation["data"]["valid"] is False, validation
                issue = validation["data"]["diagnostics"][0]
                assert (issue["path"], issue["code"]) == (pointer, code), issue
                status, applied = case.api("node-graph", "PUT", {"schema_version": 1, "revision": proposed, "graph": bad})
                assert status == 422 and (applied["data"]["diagnostics"][0]["path"], applied["data"]["diagnostics"][0]["code"]) == (pointer, code), applied
                bad_file = case.directory / f"{label}.json"
                bad_file.write_text(json.dumps(bad))
                cli = case.command("config", "validate", str(bad_file), expect=None)
                assert cli.returncode != 0 and pointer in (cli.stdout + cli.stderr) and code in (cli.stdout + cli.stderr), cli
                cli = case.command("config", "apply", str(bad_file), "--revision", proposed, expect=None)
                assert cli.returncode != 0 and pointer in (cli.stdout + cli.stderr) and code in (cli.stdout + cli.stderr), cli
                assert case.snapshot(f"after-{label}") == proposed
                assert case.status().get("active_revision") == old, case.status()
            before_tamper = case.status()
            before_pids = {entry["id"]: entry["pid"] for entry in before_tamper["processes"]}
            saved = case.graph.read_bytes()
            case.graph.write_text(json.dumps(bad_variants[0][1]))
            try:
                tampered = case.command("run", expect=None)
                assert tampered.returncode != 0 and bad_variants[0][2] in (tampered.stdout + tampered.stderr) and bad_variants[0][3] in (tampered.stdout + tampered.stderr), tampered
                after_tamper = case.status()
                assert after_tamper["active_revision"] == old, after_tamper
                assert {entry["id"]: entry["pid"] for entry in after_tamper["processes"]} == before_pids, after_tamper
            finally:
                case.graph.write_bytes(saved)
            assert case.snapshot("restored-after-tamper") == proposed
        status, restarted = case.api("process-control", "POST", {"schema_version": 1, "operation": "restart"})
        assert status == 200 and restarted["ok"], restarted
        for _ in range(100):
            if case.status().get("active_revision") == proposed:
                break
            time.sleep(0.05)
        assert case.status().get("active_revision") == proposed, case.status()
        process_cmdlines = []
        for process in case.status().get("processes", []):
            try:
                pid = int(process["pid"])
                process_cmdlines.append(Path(f"/proc/{pid}/cmdline").read_bytes().replace(b"\0", b" ").decode())
            except (KeyError, ValueError, OSError):
                pass
        case.log("child-cmdlines", process_cmdlines)
        assert any(str(projection) in line and "aether-runtime" in line for line in process_cmdlines), process_cmdlines
        assert any(str(projection) in line and "aether-config-server" in line for line in process_cmdlines), process_cmdlines
        assert case.snapshot("after-restart") == proposed
        assert case.runner is not None
        case.runner.send_signal(signal.SIGINT)
        assert case.runner.wait(timeout=12) == 0
        case.runner = None
        runtime = case.eos_bin / "aether-runtime"
        original_runtime = runtime.read_bytes()
        try:
            runtime.write_bytes(original_runtime + b"\nM1 changed binary check\n")
            changed = case.command("run", expect=None)
            assert changed.returncode != 0 and case.snapshot("after-changed-binary") == proposed, changed
            case.log("changed-binary-rejection", {"exit": changed.returncode, "runtime_sha256": digest(runtime.read_bytes())})
        finally:
            runtime.write_bytes(original_runtime)
    finally:
        case.close()


def main() -> None:
    global EOS, ISOS, EGIA, UI, ARTIFACTS, FIXTURES, EOS_EDGES, ISOS_EDGE_VALID, ISOS_EDGE_CROSS_CORE
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", action="store_true", help="run the replay against pinned revisions")
    parser.add_argument("--stage", choices=("g03", "all"), default="all", help="G03 subset or full replay after G06")
    parser.add_argument("--eos-root", type=Path, default=EOS, help="EOS checkout containing this script by default")
    parser.add_argument("--isos-root", type=Path, default=ISOS, help="IsOS integration checkout or G06 worktree")
    parser.add_argument("--egia-root", type=Path, default=EGIA, help="EGIA sibling checkout by default")
    parser.add_argument("--isosui-root", type=Path, default=UI, help="IsosUI sibling checkout by default")
    parser.add_argument("--artifacts", type=Path, default=ARTIFACTS, help="output directory, default ignored EOS logs/test/<run-id>")
    parser.add_argument("--eos-bin-dir", type=Path, help="directory of built EOS executables, default EOS target/debug")
    parser.add_argument("--isos-bin", type=Path, help="built IsOS executable, default IsOS target/debug/isos")
    for owner in ("eos", "isos", "egia", "isosui"):
        parser.add_argument(f"--expect-{owner}", required=False, help=f"required exact {owner} HEAD SHA for --run")
    args = parser.parse_args()
    EOS = args.eos_root.resolve()
    ISOS = args.isos_root.resolve()
    EGIA = args.egia_root.resolve()
    UI = args.isosui_root.resolve()
    ARTIFACTS = args.artifacts.resolve()
    FIXTURES = ISOS / "tests/fixtures/graph_compat"
    EOS_EDGES = EOS / "tests/fixtures/edge_contract/valid_evidence.json"
    ISOS_EDGE_VALID = ISOS / "tests/fixtures/edge_admission/valid_0_1_0.json"
    ISOS_EDGE_CROSS_CORE = ISOS / "tests/fixtures/edge_admission/cross_core_guard_request.json"
    roots = {"eos": EOS, "isos": ISOS, "egia": EGIA, "isosui": UI}
    if not args.run:
        print(json.dumps({"mode": "dry-plan", "roots": {owner: str(root) for owner, root in roots.items()},
            "artifacts": str(ARTIFACTS), "fixture_sha256": EXPECTED}, indent=2), flush=True)
        print("Pass --run with --eos-root, --isos-root, --egia-root, and --isosui-root for your checkouts,")
        print("plus exact --expect-eos, --expect-isos, --expect-egia, and --expect-isosui HEADs.")
        print("Use --eos-bin-dir and --isos-bin when built executables live outside those checkouts.")
        return
    expected = {owner: getattr(args, f"expect_{owner}") for owner in roots}
    if any(not revision for revision in expected.values()):
        parser.error("--run requires --expect-eos, --expect-isos, --expect-egia, and --expect-isosui")
    heads = {owner: git_head(root) for owner, root in roots.items()}
    if heads != expected:
        parser.error(f"HEAD pin mismatch: actual {heads}, expected {expected}")
    print(json.dumps({"heads": heads, "fixture_sha256": EXPECTED, "artifacts": str(ARTIFACTS)}, indent=2), flush=True)
    isos_bin = (args.isos_bin or ISOS / "target/debug/isos").resolve()
    eos_bin = (args.eos_bin_dir or EOS / "target/debug").resolve()
    ui_dist = UI / "dist/local"
    required = [isos_bin, *(eos_bin / name for name in ("aether-profile", "aether-runtime", "aether-http-bridge", "aether-config-server")), ui_dist / "index.html", EGIA / "bin/egia-config", EGIA / "bin/egia", EOS_EDGES, ISOS_EDGE_VALID, ISOS_EDGE_CROSS_CORE, *(FIXTURES / name for name in EXPECTED)]
    for item in required:
        if not item.is_file():
            raise FileNotFoundError(f"build or restore prerequisite: {item}")
    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    print(json.dumps({"isos_binary": str(isos_bin), "isos_binary_sha256": digest(isos_bin.read_bytes()),
        "eos_bin_dir": str(eos_bin), "egia_bin_dir": str(EGIA / "bin")}, sort_keys=True), flush=True)
    legacy_and_negative(isos_bin, eos_bin, ui_dist)
    migration_and_projected_start(isos_bin, eos_bin, ui_dist, include_edges=args.stage == "all")
    print(f"M1 cross-repository replay ({args.stage}): PASS", flush=True)


if __name__ == "__main__":
    main()
