#!/usr/bin/env python3
"""P04 clean-home generated configuration and launch-preflight replay.

Build IsOS and EOS first, then set ISOS_BIN and ISOS_AETHER_BIN_DIR if their
debug binaries are outside the sibling checkouts.
"""

import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
ISOS = Path(os.environ.get("ISOS_BIN", ROOT / "target/debug/isos"))
EOS_BIN_DIR = Path(os.environ.get(
    "ISOS_AETHER_BIN_DIR", ROOT.parent / "Island-eos/target/debug"
))
CASES = json.loads((ROOT / "tests/fixtures/generated_config_preflight/cases.json").read_text())


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def digest(data):
    return hashlib.sha256(data).hexdigest()


class CleanHome:
    def __init__(self, root):
        self.root = Path(root)
        self.home = self.root / "home"
        self.graph_path = self.home / "config/deployments/default.nodes.json"
        self.generated = self.home / "data/generated/default"
        self.port = free_port()
        self.ui_port = free_port()
        self.server = None
        self.output = None
        self.env = dict(os.environ, ISOS_HOME=str(self.home),
                        ISOS_AETHER_BIN_DIR=str(EOS_BIN_DIR),
                        ISOS_HTTP_PORT=str(self.port), ISOSUI_PORT=str(self.ui_port))
        self.env.pop("P04_MISSING_SECRET", None)

    def command(self, *args, success=True):
        result = subprocess.run([str(ISOS), *map(str, args)], env=self.env,
                                text=True, capture_output=True, timeout=45)
        print("$ isos", *args, "=>", result.returncode, flush=True)
        if result.stdout:
            print(result.stdout.rstrip(), flush=True)
        if result.stderr:
            print(result.stderr.rstrip(), flush=True)
        if success and result.returncode:
            raise AssertionError(f"isos {' '.join(map(str, args))} failed")
        return result

    def init(self):
        self.command("init", "--mode", "dev")
        graph = json.loads(self.graph_path.read_text())
        assert graph["version"] == "0.1.0", graph["version"]
        return graph

    def apply(self, graph=None):
        if graph is None:
            graph = json.loads(self.graph_path.read_text())
        candidate = self.root / "candidate.nodes.json"
        candidate.write_text(json.dumps(graph, indent=2))
        saved_revision = digest(self.graph_path.read_bytes())
        self.command("config", "validate", candidate)
        self.command("config", "apply", candidate, "--revision", saved_revision)
        return digest(self.graph_path.read_bytes())

    def selected(self):
        revision = digest(self.graph_path.read_bytes())
        selection = json.loads((self.generated / "selection.json").read_text())
        assert selection["schema_version"] == 2
        entry = selection["entries"][revision]
        assert entry["kind"] == "v2"
        generation = entry["generation"]
        leaf = self.generated / "v2" / revision / generation
        return revision, generation, leaf

    def seed_egia_distribution(self):
        prefix = self.root / "egia-install"
        binary_dir = prefix / "bin"
        component = prefix / "etc/egia"
        factory = prefix / "share/defaults/egia"
        binary_dir.mkdir(parents=True)
        component.mkdir(parents=True)
        factory.mkdir(parents=True)
        (prefix / "pyproject.toml").write_text("[project]\nname='egia-fixture'\nversion='0.1.0'\n")
        module = prefix / "src/egia"
        module.mkdir(parents=True)
        (module / "__init__.py").write_text("# verified fixture module\n")
        for relative in (
            "egia.toml", "modes.toml", "profiles/isos.toml", "models.toml",
            "prompts.toml", "missions.toml", "safety/gate.toml", "opt/sandbox.toml",
            "extra/nested.toml",
        ):
            path = component / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("enabled = true\n")
        (factory / "models.toml").write_text("enabled = true\n")
        adapter = binary_dir / "egia-config"
        adapter.write_text("""#!/usr/bin/env python3
import json
import os
import sys
request = json.load(sys.stdin)
reply = {"schema_version": 2, "ok": True, "diagnostics": [], "files": []}
if request["op"] == "identity":
    reply["identity"] = {
        "core": "egia", "graph_version": "1.0.0", "node_type_version": "1.0",
        "product_version": "0.1.0", "setup_schema_version": 2,
        "supported_graph_versions": ["1.0.0", "0.1.0"],
        "supported_node_type_versions": ["1.0"],
        "supported_node_types": ["egia.common.context@v1.0", "egia.common.guard@v1.0",
                                 "egia.common.planner@v1.0", "egia.common.source@v1.0"],
        "features": ["setup-v2", "catalog", "graph-0.1-reader"],
    }
elif request["op"] == "render":
    reply["files"] = [
        {"path": "egia/egia.toml", "content": "mode = 'isos'\\n"},
        {"path": "egia/modes.toml", "content": "enabled = true\\n"},
        {"path": "egia/profiles/isos.toml", "content": "managed = true\\n"},
    ]
    if os.getenv("P04_REJECT_RENDER") == "1":
        reply["ok"] = False
        reply["diagnostics"] = [{"path": "/cores/egia", "code": "stub_rejected",
                                 "message": "rejected"}]
print(json.dumps(reply))
""")
        adapter.chmod(0o700)
        executable = binary_dir / "egia"
        executable.write_text("#!/bin/sh\nif [ \"$1\" = --capabilities ]; then printf '%s\\n' '{\"binary\":\"egia\",\"product_version\":\"0.1.0\",\"features\":[\"managed-config-v1\",\"eos-gateway-client-v2\"]}'; exit 0; fi\nexit 0\n")
        executable.chmod(0o700)
        self.env["ISOS_EGIA_BIN_DIR"] = str(binary_dir)
        self.env["EGIA_PYTHON"] = str(Path(sys.executable).resolve())
        self.env["ISOS_EGIA_DEFAULTS_DIR"] = str(component)
        self.env["ISOS_EGIA_FACTORY_DEFAULTS_DIR"] = str(factory)
        self.env.pop("ISLAND_PREFIX", None)
        return component, factory

    def start_server(self):
        self.output = (self.root / "serve.log").open("w+")
        self.server = subprocess.Popen([str(ISOS), "serve"], env=self.env,
                                       stdout=self.output, stderr=subprocess.STDOUT, text=True)
        for _ in range(160):
            if self.server.poll() is not None:
                raise AssertionError(f"IsOS serve exited {self.server.returncode}")
            try:
                self.api("node-graph")
                return
            except (OSError, ValueError):
                time.sleep(0.05)
        raise AssertionError("IsOS API did not become ready")

    def api(self, route, data=None, method=None, timeout=30):
        payload = None if data is None else json.dumps(data).encode()
        request = Request(f"http://127.0.0.1:{self.port}/api/isos/v1/{route}",
                          data=payload, method=method or ("POST" if data is not None else "GET"),
                          headers={"Origin": f"http://127.0.0.1:{self.port}",
                                   "Content-Type": "application/json"})
        try:
            with urlopen(request, timeout=timeout) as response:
                return response.status, json.load(response)
        except HTTPError as error:
            return error.code, json.load(error)

    def control(self, operation, timeout=30):
        return self.api("process-control", {
            "schema_version": 1, "operation": operation,
            "revision": digest(self.graph_path.read_bytes()),
        }, timeout=timeout)

    def http_apply(self, graph):
        return self.api("node-graph", {
            "schema_version": 1,
            "revision": digest(self.graph_path.read_bytes()),
            "graph": graph,
        }, method="PUT")

    def close(self):
        if self.server is not None:
            self.server.terminate()
            try:
                self.server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.server.kill()
                self.server.wait(timeout=5)
        if self.output is not None:
            self.output.close()


class GeneratedConfigReplay(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for binary in (ISOS, EOS_BIN_DIR / "aether-profile", EOS_BIN_DIR / "aether-runtime"):
            if not binary.is_file():
                raise AssertionError(f"missing test binary: {binary}")
        assert CASES["slice"] == "P04"

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="isos-p04-")
        self.case = CleanHome(self.temp.name)
        self.case.init()

    def tearDown(self):
        self.case.close()
        self.temp.cleanup()

    def assert_failure(self, expected_case):
        expected = next(case for case in CASES["cases"] if case["id"] == expected_case)
        status, reply = self.case.control("run")
        self.assertEqual(status, 422, reply)
        self.assertFalse(reply["ok"])
        self.assertIsInstance(reply.get("data"), dict, reply)
        self.assertIn("diagnostics", reply["data"])
        first = reply["data"]["diagnostics"][0]
        self.assertEqual(
            {key: first[key] for key in ("code", "path", "stage")},
            {"code": expected["expected_code"], "path": expected["expected_path"],
             "stage": expected["expected_stage"]},
        )
        self.assertEqual(set(first), {"code", "path", "message", "stage"})

    def assert_apply_failure(self, candidate, revision, expected_case):
        expected = next(case for case in CASES["cases"] if case["id"] == expected_case)
        graph_before = self.case.graph_path.read_bytes()
        selection_before = (self.case.generated / "selection.json").read_bytes()
        result = self.case.command("config", "apply", candidate,
                                   "--revision", revision, success=False)
        self.assertNotEqual(result.returncode, 0)
        output = result.stdout + result.stderr
        self.assertIn('"code":"' + expected["expected_code"] + '"', output)
        self.assertIn('"path":"' + expected["expected_path"] + '"', output)
        self.assertIn('"stage":"' + expected["expected_stage"] + '"', output)
        self.assertEqual(self.case.graph_path.read_bytes(), graph_before)
        self.assertEqual((self.case.generated / "selection.json").read_bytes(),
                         selection_before)

    def test_immutable_selection_and_repeat_apply(self):
        graph_before = self.case.graph_path.read_bytes()
        self.case.apply()
        revision, generation, leaf = self.case.selected()
        self.assertEqual(revision, digest(self.case.graph_path.read_bytes()))
        marker = self.case.generated / "v2/required" / f"{revision}.json"
        self.assertEqual(json.loads(marker.read_text()),
                         {"schema_version": 1, "revision": revision})
        manifest = json.loads((leaf / "manifest.json").read_text())
        self.assertEqual(manifest["schema_version"], 2)
        self.assertEqual((manifest["revision"], manifest["generation"]),
                         (revision, generation))
        inventory = {key: value for key, value in manifest.items() if key != "generation"}
        canonical = json.dumps(inventory, sort_keys=True, separators=(",", ":"),
                               ensure_ascii=False).encode()
        self.assertEqual(digest(canonical), generation)
        self.assertEqual([item["path"] for item in manifest["files"]],
                         sorted(item["path"] for item in manifest["files"]))
        listed = {item["path"] for item in manifest["files"]}
        present = {str(path.relative_to(leaf)) for path in leaf.rglob("*") if path.is_file()}
        self.assertEqual(present, listed | {"manifest.json"})
        for item in manifest["files"]:
            path = leaf / item["path"]
            self.assertEqual(digest(path.read_bytes()), item["sha256"])
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(leaf.stat().st_mode & 0o777, 0o700)
        self.assertEqual(marker.stat().st_mode & 0o777, 0o600)
        self.assertEqual((leaf / "manifest.json").stat().st_mode & 0o777, 0o600)
        manifest_before = (leaf / "manifest.json").read_bytes()
        self.case.apply()
        self.assertEqual(self.case.selected()[:2], (revision, generation))
        self.assertEqual((leaf / "manifest.json").read_bytes(), manifest_before)
        self.assertEqual(self.case.graph_path.read_bytes(), graph_before)

    def test_deleted_selection_cannot_downgrade_to_g03(self):
        self.case.apply()
        before = self.case.graph_path.read_bytes()
        (self.case.generated / "selection.json").unlink()
        self.case.start_server()
        self.assert_failure("deleted_selection_after_v2_apply")
        self.assertEqual(self.case.graph_path.read_bytes(), before)

    def test_changed_graph_selection_loss_keeps_marked_revision_inert(self):
        self.case.apply()
        old_revision, old_generation, old_leaf = self.case.selected()
        graph = json.loads(self.case.graph_path.read_text())
        graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["eos"][
            "description"] = "Changed EOS process"
        self.case.apply(graph)
        new_revision, new_generation, new_leaf = self.case.selected()
        self.assertNotEqual(new_revision, old_revision)
        self.assertTrue((old_leaf / "manifest.json").is_file())
        self.assertTrue((new_leaf / "manifest.json").is_file())
        selection = json.loads((self.case.generated / "selection.json").read_text())
        self.assertEqual(selection["entries"][old_revision]["generation"], old_generation)
        self.assertEqual(selection["entries"][new_revision]["generation"], new_generation)
        (self.case.generated / "selection.json").unlink()
        before = self.case.graph_path.read_bytes()
        self.case.start_server()
        self.assert_failure("deleted_selection_after_changed_graph_apply")
        self.assertEqual(self.case.graph_path.read_bytes(), before)

    def test_changed_defaults_and_binary_require_explicit_generation_selection(self):
        component, _ = self.case.seed_egia_distribution()
        graph = json.loads(self.case.graph_path.read_text())
        nodes = graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]
        nodes["egia"] = {
            "id": "egia", "node_type": "isos.process.component@v0.1",
            "label": "EGIA agent", "description": "Managed agent for generation selection",
            "capabilities": ["process"],
            "settings": {"role": "egia", "args": [],
                         "depends_on": ["cores/isos/layers/process/groups/component/nodes/eos"]},
        }
        self.case.apply(graph)
        revision, first_generation, first_leaf = self.case.selected()
        first_manifest = (first_leaf / "manifest.json").read_bytes()
        graph_bytes = self.case.graph_path.read_bytes()
        self.case.start_server()

        models = component / "models.toml"
        models.write_text("enabled = false\n")
        status, reply = self.case.http_apply(json.loads(graph_bytes))
        self.assertEqual(status, 200, reply)
        default_revision, default_generation, default_leaf = self.case.selected()
        self.assertEqual(default_revision, revision)
        self.assertNotEqual(default_generation, first_generation)
        self.assertEqual((first_leaf / "manifest.json").read_bytes(), first_manifest)
        default_manifest = (default_leaf / "manifest.json").read_bytes()

        executable = self.case.root / "egia-install/bin/egia"
        executable.write_text("#!/bin/sh\n# changed verified executable\nif [ \"$1\" = --capabilities ]; then printf '%s\\n' '{\"binary\":\"egia\",\"product_version\":\"0.1.0\",\"features\":[\"managed-config-v1\",\"eos-gateway-client-v2\"]}'; exit 0; fi\nexit 0\n")
        status, reply = self.case.http_apply(json.loads(graph_bytes))
        self.assertEqual(status, 200, reply)
        binary_revision, binary_generation, binary_leaf = self.case.selected()
        self.assertEqual(binary_revision, revision)
        self.assertNotIn(binary_generation, (first_generation, default_generation))
        self.assertEqual((default_leaf / "manifest.json").read_bytes(), default_manifest)
        self.assertTrue((binary_leaf / "manifest.json").is_file())
        self.assertEqual(self.case.graph_path.read_bytes(), graph_bytes)

    def assert_uncertain_publication_boundary(self, boundary):
        if boundary == "graph":
            self.case.apply()
        before = self.case.graph_path.read_bytes()
        old_revision = digest(before)
        graph = json.loads(before)
        if boundary == "graph":
            graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["eos"][
                "description"] = "Changed EOS process for publish boundary"
        self.case.env["ISOS_P04_TEST_FAIL_SYNC"] = boundary
        self.case.env["ISOS_P04_TEST_FAIL_SYNC_DEPLOYMENT"] = "default"
        self.case.start_server()
        status, reply = self.case.http_apply(graph)
        self.assertEqual(status, 500, reply)
        first = reply["data"]["diagnostics"][0]
        self.assertEqual((first["code"], first["path"], first["stage"]),
                         ("generated_commit_uncertain", "", "publish"))
        observed_revision = digest(self.case.graph_path.read_bytes())
        if boundary == "graph":
            self.assertTrue(observed_revision == old_revision
                            or json.loads(self.case.graph_path.read_bytes()) == graph)
        else:
            self.assertEqual(observed_revision, old_revision)
        self.case.close()
        self.case.server = None
        self.case.output = None
        self.case.env.pop("ISOS_P04_TEST_FAIL_SYNC")
        self.case.env.pop("ISOS_P04_TEST_FAIL_SYNC_DEPLOYMENT")
        self.case.start_server()
        status, reply = self.case.control("run")
        self.assertEqual(status, 200, reply)
        revision, _, leaf = self.case.selected()
        self.assertEqual(revision, digest(self.case.graph_path.read_bytes()))
        self.assertTrue((leaf / "manifest.json").is_file())

    def test_selection_parent_sync_uncertain_recovers(self):
        self.assert_uncertain_publication_boundary("selection")

    def test_marker_parent_sync_uncertain_recovers(self):
        self.assert_uncertain_publication_boundary("marker")

    def test_graph_parent_sync_uncertain_recovers(self):
        self.assert_uncertain_publication_boundary("graph")

    def test_owner_render_rejection_after_apply_stays_typed_over_http(self):
        self.case.seed_egia_distribution()
        graph = json.loads(self.case.graph_path.read_text())
        nodes = graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]
        nodes["egia"] = {
            "id": "egia", "node_type": "isos.process.component@v0.1",
            "label": "EGIA agent", "description": "Managed agent for typed rejection",
            "capabilities": ["process"],
            "settings": {"role": "egia", "args": [],
                         "depends_on": ["cores/isos/layers/process/groups/component/nodes/eos"]},
        }
        self.case.apply(graph)
        graph_before = self.case.graph_path.read_bytes()
        self.case.env["P04_REJECT_RENDER"] = "1"
        self.case.start_server()
        status, reply = self.case.control("run")
        self.assertEqual(status, 422, reply)
        first = reply["data"]["diagnostics"][0]
        self.assertEqual((first["code"], first["path"], first["stage"]),
                         ("generated_owner_rejected", "/cores/egia", "render"))
        self.assertEqual(set(first), {"code", "path", "message", "stage"})
        self.assertEqual(self.case.graph_path.read_bytes(), graph_before)

    def test_generated_file_tamper_rejected_before_run(self):
        self.case.apply()
        before = self.case.graph_path.read_bytes()
        _, _, leaf = self.case.selected()
        with (leaf / "eos/graph.json").open("ab") as file:
            file.write(b"\n")
        self.case.start_server()
        self.assert_failure("manifest_file_tamper")
        self.assertEqual(self.case.graph_path.read_bytes(), before)

    def test_symlinked_revision_parent_cannot_replace_active_child(self):
        self.case.apply()
        self.case.start_server()
        status, reply = self.case.control("run")
        self.assertEqual(status, 200, reply)
        status, reply = self.case.api("process-status")
        self.assertEqual(status, 200, reply)
        active = reply["data"]
        pids = {process["id"]: process["pid"] for process in active["processes"]}
        self.assertTrue(pids)
        _, _, leaf = self.case.selected()
        revision_dir = leaf.parent
        backup = revision_dir.with_name(revision_dir.name + ".original")
        revision_dir.rename(backup)
        revision_dir.symlink_to(backup.name, target_is_directory=True)
        status, rejected = self.case.control("restart")
        self.assertEqual(status, 422, rejected)
        first = rejected["data"]["diagnostics"][0]
        self.assertEqual((first["code"], first["path"], first["stage"]),
                         ("generated_unsafe_path", "", "snapshot"))
        status, reply = self.case.api("process-status")
        self.assertEqual(status, 200, reply)
        self.assertEqual({process["id"]: process["pid"] for process in reply["data"]["processes"]},
                         pids)

    def test_restart_tamper_keeps_active_children(self):
        self.case.apply()
        self.case.start_server()
        status, reply = self.case.control("run")
        self.assertEqual(status, 200, reply)
        status, reply = self.case.api("process-status")
        self.assertEqual(status, 200, reply)
        active = reply["data"]
        self.assertEqual(active["state"], "running")
        pids = {process["id"]: process["pid"] for process in active["processes"]}
        self.assertTrue(pids)
        _, _, leaf = self.case.selected()
        with (leaf / "eos/graph.json").open("ab") as file:
            file.write(b"\n")
        status, rejected = self.case.control("restart")
        self.assertEqual(status, 422, rejected)
        first = rejected["data"]["diagnostics"][0]
        self.assertEqual((first["code"], first["path"], first["stage"]),
                         ("generated_manifest_mismatch", "", "snapshot"))
        status, reply = self.case.api("process-status")
        self.assertEqual(status, 200, reply)
        after = reply["data"]
        self.assertEqual(after["active_revision"], active["active_revision"])
        self.assertEqual({process["id"]: process["pid"] for process in after["processes"]}, pids)

    def test_missing_secret_has_exact_pointer_and_no_value_leak(self):
        graph = json.loads(self.case.graph_path.read_text())
        eos = graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["eos"]
        eos["settings"]["env_from"] = {"P04_TARGET": "P04_MISSING_SECRET"}
        self.case.apply(graph)
        _, _, leaf = self.case.selected()
        self.assertNotIn(b"secret-value", (leaf / "manifest.json").read_bytes())
        self.case.start_server()
        status, reply = self.case.control("run")
        self.assertEqual(status, 422, reply)
        first = reply["data"]["diagnostics"][0]
        self.assertEqual(
            (first["code"], first["path"], first["stage"]),
            ("generated_missing_secret",
             "/cores/isos/layers/process/groups/component/nodes/eos/settings/env_from/P04_TARGET",
             "spawn"),
        )
        self.assertNotIn("P04_MISSING_SECRET", json.dumps(reply))

    def test_egia_defaults_are_selected_and_missing_file_keeps_generation(self):
        component, factory = self.case.seed_egia_distribution()
        graph = json.loads(self.case.graph_path.read_text())
        nodes = graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]
        nodes["egia"] = {
            "id": "egia", "node_type": "isos.process.component@v0.1",
            "label": "EGIA agent", "description": "Managed agent for config preflight",
            "capabilities": ["process"],
            "settings": {"role": "egia", "args": [],
                         "depends_on": ["cores/isos/layers/process/groups/component/nodes/eos"]},
        }
        self.case.apply(graph)
        revision, generation, leaf = self.case.selected()
        manifest = json.loads((leaf / "manifest.json").read_text())
        self.assertEqual(manifest["default_roots"],
                         {"component": str(component), "factory": str(factory)})
        for relative in CASES["required_egia_files"]:
            self.assertTrue((leaf / relative).is_file(), relative)
        self.assertTrue((leaf / "egia/extra/nested.toml").is_file())
        self.assertIn(b"mode = 'isos'", (leaf / "egia/egia.toml").read_bytes())
        graph_before = self.case.graph_path.read_bytes()
        candidate = self.case.root / "candidate.nodes.json"
        for relative, fixture in (("prompts.toml", "missing_prompts"),
                                  ("missions.toml", "missing_missions"),
                                  ("safety/gate.toml", "missing_safety_gate")):
            path = component / relative
            original = path.read_bytes()
            path.unlink()
            self.assert_apply_failure(candidate, revision, fixture)
            path.write_bytes(original)

        models = component / "models.toml"
        original_models = models.read_bytes()
        models.unlink()
        models.symlink_to(self.case.root / "outside-models.toml")
        self.assert_apply_failure(candidate, revision, "default_symlink")
        models.unlink()
        models.write_bytes(original_models)

        adapter = self.case.root / "egia-install/bin/egia-config"
        original_adapter = adapter.read_text()
        adapter.write_text(original_adapter.replace(
            'print(json.dumps(reply))',
            'if request["op"] == "render":\n    reply["ok"] = False\n'
            '    reply["diagnostics"] = [{"path": "/cores/egia", '
            '"code": "stub_rejected", "message": "rejected"}]\n'
            'print(json.dumps(reply))'))
        self.assert_apply_failure(candidate, revision, "owner_rejection")
        adapter.write_text(original_adapter.replace(
            'print(json.dumps(reply))',
            'if request["op"] == "render":\n'
            '    reply["files"] = [{"path": "egia/../escape.toml", '
            '"content": "bad = true\\n"}]\n'
            'print(json.dumps(reply))'))
        self.assert_apply_failure(candidate, revision, "overlay_escape")
        adapter.write_text(original_adapter)

        self.assertEqual(self.case.graph_path.read_bytes(), graph_before)
        self.assertEqual(self.case.selected()[:2], (revision, generation))
        (factory / "models.toml").write_text("enabled = false\n")
        self.case.start_server()
        status, reply = self.case.control("run")
        self.assertEqual(status, 422, reply)
        self.assertEqual(
            {key: reply["data"]["diagnostics"][0][key] for key in ("code", "path", "stage")},
            {"code": "generated_manifest_mismatch", "path": "", "stage": "snapshot"},
        )
        self.assertEqual(self.case.graph_path.read_bytes(), graph_before)

    def test_later_child_missing_secret_keeps_active_pids(self):
        self.case.seed_egia_distribution()
        executable = self.case.root / "egia-install/bin/egia"
        executable.write_text("#!/bin/sh\nif [ \"$1\" = --capabilities ]; then printf '%s\\n' '{\"binary\":\"egia\",\"product_version\":\"0.1.0\",\"features\":[\"managed-config-v1\",\"eos-gateway-client-v2\"]}'; exit 0; fi\nsleep 60\n")
        self.case.apply()
        self.case.start_server()
        status, reply = self.case.control("run")
        self.assertEqual(status, 200, reply)
        status, reply = self.case.api("process-status")
        self.assertEqual(status, 200, reply)
        active = reply["data"]
        pids = {process["id"]: process["pid"] for process in active["processes"]}
        self.assertTrue(pids)
        graph = json.loads(self.case.graph_path.read_text())
        nodes = graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]
        nodes["egia"] = {
            "id": "egia", "node_type": "isos.process.component@v0.1",
            "label": "EGIA agent", "description": "Managed agent for secret preflight",
            "capabilities": ["process"],
            "settings": {"role": "egia", "args": [],
                         "depends_on": ["cores/isos/layers/process/groups/component/nodes/eos"],
                         "env_from": {"EGIA_API_KEY": "P04_EGIA_SECRET"}},
        }
        self.case.apply(graph)
        status, rejected = self.case.control("restart")
        self.assertEqual(status, 422, rejected)
        first = rejected["data"]["diagnostics"][0]
        self.assertEqual((first["code"], first["path"], first["stage"]),
                         ("generated_missing_secret",
                          "/cores/isos/layers/process/groups/component/nodes/egia/settings/env_from/EGIA_API_KEY",
                          "spawn"))
        status, reply = self.case.api("process-status")
        self.assertEqual(status, 200, reply)
        self.assertEqual({process["id"]: process["pid"] for process in reply["data"]["processes"]},
                         pids)


if __name__ == "__main__":
    unittest.main(verbosity=2)
