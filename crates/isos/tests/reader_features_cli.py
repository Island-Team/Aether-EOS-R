#!/usr/bin/env python3
"""P05 selected-reader replay against the real EOS build and isolated EGIA fixture."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from generated_config_preflight_cli import CleanHome, EOS_BIN_DIR, digest


class ReaderFeaturesReplay(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="isos-p05-")
        self.case = CleanHome(self.temp.name)
        self.graph = self.case.init()

    def tearDown(self):
        self.case.close()
        self.temp.cleanup()

    def test_real_selected_eos_readers_validate_and_apply(self):
        self.case.apply(self.graph)
        revision, _, leaf = self.case.selected()
        self.assertEqual(json.loads((leaf / "manifest.json").read_text())["revision"], revision)
        check = self.case.command("version", "check", "--json")
        eos = next(core for core in json.loads(check.stdout)["cores"] if core["core"] == "eos")
        self.assertTrue(eos["ready"])
        self.assertEqual(eos["diagnostics"], [])

    def test_selected_runtime_missing_claim_fails_before_apply(self):
        selected = self.case.root / "selected-eos"
        selected.mkdir()
        for name in ("aether-profile", "aether-runtime", "aether-http-bridge",
                     "aether-config-server"):
            shutil.copy2(EOS_BIN_DIR / name, selected / name)
        runtime = selected / "aether-runtime"
        original = EOS_BIN_DIR / "aether-runtime"
        capabilities = json.loads(subprocess.check_output([str(original), "--capabilities"]))
        capabilities["features"] = [feature for feature in capabilities["features"]
                                    if feature != "node-types"]
        runtime.write_text("#!/usr/bin/env python3\nimport os,sys\n"
                           "if sys.argv[1:] == ['--capabilities']:\n"
                           f" print({json.dumps(json.dumps(capabilities))}); sys.exit(0)\n"
                           f"os.execv({str(original)!r}, [{str(original)!r}, *sys.argv[1:]])\n")
        runtime.chmod(0o700)
        self.case.env["ISOS_AETHER_BIN_DIR"] = str(selected)
        before = self.case.graph_path.read_bytes()
        self.case.start_server()
        status, reply = self.case.http_apply(self.graph)
        self.assertEqual(status, 422, reply)
        diagnostic = reply["data"]["diagnostics"][0]
        self.assertEqual((diagnostic["code"], diagnostic["path"], diagnostic["stage"]),
                         ("reader_feature_unsupported", "/version", "feature"))
        self.assertEqual(diagnostic["reader"]["binary"], "aether-runtime")
        self.assertEqual(diagnostic["reader"]["requested"]["feature"], "node-types")
        self.assertEqual(self.case.graph_path.read_bytes(), before)

    def test_bridge_requires_transport_feature_without_graph_arrays(self):
        selected = self.case.root / "selected-eos"
        selected.mkdir()
        for name in ("aether-profile", "aether-runtime", "aether-http-bridge",
                     "aether-config-server"):
            shutil.copy2(EOS_BIN_DIR / name, selected / name)
        bridge = selected / "aether-http-bridge"
        original = EOS_BIN_DIR / "aether-http-bridge"
        capabilities = json.loads(subprocess.check_output([str(original), "--capabilities"]))
        capabilities["features"] = [feature for feature in capabilities["features"]
                                    if feature != "vision-ws"]
        bridge.write_text("#!/usr/bin/env python3\nimport os,sys\n"
                          "if sys.argv[1:] == ['--capabilities']:\n"
                          f" print({json.dumps(json.dumps(capabilities))}); sys.exit(0)\n"
                          f"os.execv({str(original)!r}, [{str(original)!r}, *sys.argv[1:]])\n")
        bridge.chmod(0o700)
        self.case.env["ISOS_AETHER_BIN_DIR"] = str(selected)
        self.case.start_server()
        status, reply = self.case.http_apply(self.graph)
        self.assertEqual(status, 422, reply)
        diagnostic = reply["data"]["diagnostics"][0]
        self.assertEqual(diagnostic["code"], "reader_feature_unsupported")
        self.assertEqual(diagnostic["reader"]["binary"], "aether-http-bridge")
        self.assertEqual(diagnostic["reader"]["requested"]["feature"], "vision-ws")
        self.assertEqual(diagnostic["reader"]["supported"]["graph_versions"], [])
        self.assertEqual(diagnostic["path"],
                         "/cores/isos/layers/process/groups/component/nodes/bridge/node_type")

    def test_runtime_claim_change_rejects_restart_without_stopping_children(self):
        selected = self.case.root / "selected-eos"
        selected.mkdir()
        for name in ("aether-profile", "aether-runtime", "aether-http-bridge",
                     "aether-config-server"):
            shutil.copy2(EOS_BIN_DIR / name, selected / name)
        runtime = selected / "aether-runtime"
        original = EOS_BIN_DIR / "aether-runtime"
        capabilities = json.loads(subprocess.check_output([str(original), "--capabilities"]))
        weaker = dict(capabilities, features=[feature for feature in capabilities["features"]
                                             if feature != "node-types"])
        toggle = self.case.root / "drop-feature"
        runtime.write_text("#!/usr/bin/env python3\nimport os,sys\n"
                           "if sys.argv[1:] == ['--capabilities']:\n"
                           f" print({json.dumps(json.dumps(weaker))} if os.path.exists({str(toggle)!r}) "
                           f"else {json.dumps(json.dumps(capabilities))}); sys.exit(0)\n"
                           f"os.execv({str(original)!r}, [{str(original)!r}, *sys.argv[1:]])\n")
        runtime.chmod(0o700)
        self.case.env["ISOS_AETHER_BIN_DIR"] = str(selected)
        self.case.apply(self.graph)
        self.case.start_server()
        status, reply = self.case.control("run")
        self.assertEqual(status, 200, reply)
        status, reply = self.case.api("process-status")
        self.assertEqual(status, 200, reply)
        pids = {item["id"]: item["pid"] for item in reply["data"]["processes"]}
        self.assertTrue(pids)
        toggle.write_text("omit node-types\n")
        status, reply = self.case.control("restart")
        self.assertEqual(status, 422, reply)
        diagnostic = reply["data"]["diagnostics"][0]
        self.assertEqual((diagnostic["code"], diagnostic["path"], diagnostic["stage"]),
                         ("reader_feature_unsupported", "/version", "feature"))
        status, after = self.case.api("process-status")
        self.assertEqual(status, 200, after)
        self.assertEqual({item["id"]: item["pid"] for item in after["data"]["processes"]}, pids)

    def test_old_eos_without_managed_grant_feature_keeps_active_run(self):
        selected = self.case.root / "selected-eos"
        selected.mkdir()
        for name in ("aether-profile", "aether-runtime", "aether-http-bridge",
                     "aether-config-server"):
            shutil.copy2(EOS_BIN_DIR / name, selected / name)
        runtime = selected / "aether-runtime"
        original = EOS_BIN_DIR / "aether-runtime"
        capabilities = json.loads(subprocess.check_output([str(original), "--capabilities"]))
        self.assertIn("managed-control-grants", capabilities["features"])
        weaker = dict(capabilities, features=[feature for feature in capabilities["features"]
                                             if feature != "managed-control-grants"])
        toggle = self.case.root / "drop-managed-grants"
        runtime.write_text("#!/usr/bin/env python3\nimport os,sys\n"
                           "if sys.argv[1:] == ['--capabilities']:\n"
                           f" print({json.dumps(json.dumps(weaker))} if os.path.exists({str(toggle)!r}) "
                           f"else {json.dumps(json.dumps(capabilities))}); sys.exit(0)\n"
                           f"os.execv({str(original)!r}, [{str(original)!r}, *sys.argv[1:]])\n")
        runtime.chmod(0o700)
        self.case.env["ISOS_AETHER_BIN_DIR"] = str(selected)
        self.case.apply(self.graph)
        self.case.start_server()
        status, reply = self.case.control("run")
        self.assertEqual(status, 200, reply)
        status, before = self.case.api("process-status")
        self.assertEqual(status, 200, before)
        pids = {item["id"]: item["pid"] for item in before["data"]["processes"]}
        run_id = before["data"]["run_id"]
        bundles = self.case.home / "run" / "control-grants" / "default"
        self.assertEqual(sorted(path.name for path in bundles.iterdir()), [run_id])
        toggle.write_text("omit managed-control-grants\n")
        status, reply = self.case.control("restart")
        self.assertEqual(status, 422, reply)
        diagnostic = reply["data"]["diagnostics"][0]
        self.assertEqual((diagnostic["code"], diagnostic["path"], diagnostic["stage"]),
                         ("reader_feature_unsupported",
                          "/cores/isos/layers/process/groups/component/nodes/eos/node_type",
                          "feature"))
        self.assertEqual(diagnostic["reader"]["requested"]["feature"],
                         "managed-control-grants")
        status, after = self.case.api("process-status")
        self.assertEqual(status, 200, after)
        self.assertEqual({item["id"]: item["pid"] for item in after["data"]["processes"]}, pids)
        self.assertEqual(after["data"]["run_id"], run_id)
        self.assertEqual(sorted(path.name for path in bundles.iterdir()), [run_id])

    def test_egia_module_change_invalidates_selected_generation(self):
        self.case.seed_egia_distribution()
        nodes = self.graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]
        nodes["egia"] = {
            "id": "egia", "node_type": "isos.process.component@v0.1",
            "label": "EGIA agent", "description": "P05 EGIA chain fixture",
            "capabilities": ["process"],
            "settings": {"role": "egia", "args": [], "depends_on": [
                "cores/isos/layers/process/groups/component/nodes/eos"]},
        }
        self.case.apply(self.graph)
        _, _, leaf = self.case.selected()
        manifest = json.loads((leaf / "manifest.json").read_text())
        self.assertEqual(manifest["egia_chain"]["mode"], "source")
        self.assertEqual(manifest["egia_chain"]["binding"]["PYTHONPATH"], "")
        inventory = {key: value for key, value in manifest.items() if key != "generation"}
        canonical = json.dumps(inventory, sort_keys=True, separators=(",", ":"),
                               ensure_ascii=False).encode()
        self.assertEqual(digest(canonical), manifest["generation"])
        module = self.case.root / "egia-install/src/egia/__init__.py"
        module.write_text("# changed selected module\n")
        self.case.start_server()
        status, reply = self.case.control("run")
        self.assertEqual(status, 422, reply)
        self.assertEqual(reply["data"]["diagnostics"][0]["code"],
                         "generated_manifest_mismatch")

    def test_egia_setup_must_claim_selected_legacy_type(self):
        self.case.seed_egia_distribution()
        nodes = self.graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]
        nodes["egia"] = {
            "id": "egia", "node_type": "isos.process.component@v0.1",
            "label": "EGIA agent", "description": "P05 EGIA type fixture",
            "capabilities": ["process"],
            "settings": {"role": "egia", "args": [], "depends_on": [
                "cores/isos/layers/process/groups/component/nodes/eos"]},
        }
        self.graph["cores"]["egia"] = {"layers": {"common": {"groups": {"data": {"nodes": {
            "source": {"id": "source", "node_type": "egia.common.source@v1.0",
                       "label": "source", "description": "legacy source"}
        }}}}}}
        adapter = self.case.root / "egia-install/bin/egia-config"
        source = adapter.read_text()
        adapter.write_text(source.replace(', "egia.common.source@v1.0"', ''))
        self.case.start_server()
        status, reply = self.case.http_apply(self.graph)
        self.assertEqual(status, 422, reply)
        diagnostic = reply["data"]["diagnostics"][0]
        self.assertEqual((diagnostic["code"], diagnostic["path"], diagnostic["stage"]),
                         ("reader_type_unsupported",
                          "/cores/egia/layers/common/groups/data/nodes/source/node_type",
                          "reader"))
        self.assertEqual(diagnostic["reader"]["binary"], "egia-config")

    def test_target_stage_missing_from_selected_setup_rejects_validate_and_apply(self):
        self.case.seed_egia_distribution()
        self.graph["cores"]["egia"] = {"layers": {"io": {"groups": {"link": {"nodes": {
            "gateway": {"id": "gateway", "node_type": "egia.link.gateway@v0.1",
                        "label": "Gateway", "description": "Selected link stage",
                        "settings": {"backend_alias": "robot-a"}}
        }}}}}}
        candidate = self.case.root / "candidate.nodes.json"
        candidate.write_text(json.dumps(self.graph))
        before = self.case.graph_path.read_bytes()
        result = self.case.command("config", "validate", candidate, success=False)
        self.assertIn('"code":"reader_type_unsupported"', result.stderr)
        self.assertIn('/cores/egia/layers/io/groups/link/nodes/gateway/node_type',
                      result.stderr)
        self.case.start_server()
        status, reply = self.case.http_apply(self.graph)
        self.assertEqual(status, 422, reply)
        diagnostic = reply["data"]["diagnostics"][0]
        self.assertEqual((diagnostic["code"], diagnostic["path"]),
                         ("reader_type_unsupported",
                          "/cores/egia/layers/io/groups/link/nodes/gateway/node_type"))
        self.assertEqual(self.case.graph_path.read_bytes(), before)

    def test_real_egia_readers_reach_owner_render(self):
        root = Path(os.environ.get("ISOS_REAL_EGIA_ROOT", ""))
        if not root.is_absolute() or not (root / "bin/egia-config").is_file():
            self.skipTest("set ISOS_REAL_EGIA_ROOT to a built EGIA checkout")
        self.case.env.update({
            "ISOS_EGIA_BIN_DIR": str(root / "bin"),
            "ISOS_EGIA_DEFAULTS_DIR": str(root / "etc/egia"),
            "ISOS_EGIA_FACTORY_DEFAULTS_DIR": str(root / "share/defaults/egia"),
            "EGIA_PYTHON": os.environ.get("ISOS_REAL_EGIA_PYTHON", str(root / ".venv/bin/python")),
            "ISLAND_PREFIX": str(root),
        })
        nodes = self.graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]
        nodes["egia"] = {
            "id": "egia", "node_type": "isos.process.component@v0.1",
            "label": "EGIA agent", "description": "Real EGIA reader fixture",
            "capabilities": ["process"],
            "settings": {"role": "egia", "args": [], "depends_on": [
                "cores/isos/layers/process/groups/component/nodes/eos"]},
        }
        candidate = self.case.root / "real-egia.nodes.json"
        candidate.write_text(json.dumps(self.graph))
        result = self.case.command("config", "validate", candidate, success=False)
        self.assertIn('"code":"generated_owner_rejected"', result.stderr)
        self.assertNotIn('"code":"reader_', result.stderr)

    def test_real_egia_stage_catalog_routes_only_evidence_and_denies_fixed_edges(self):
        root = Path(os.environ.get("ISOS_REAL_EGIA_ROOT", ""))
        if not root.is_absolute() or not (root / "bin/egia-config").is_file():
            self.skipTest("set ISOS_REAL_EGIA_ROOT to the A01 EGIA checkout")
        self.case.env.update({
            "ISOS_EGIA_BIN_DIR": str(root / "bin"),
            "ISOS_EGIA_DEFAULTS_DIR": str(root / "etc/egia"),
            "ISOS_EGIA_FACTORY_DEFAULTS_DIR": str(root / "share/defaults/egia"),
            "EGIA_PYTHON": os.environ.get("ISOS_REAL_EGIA_PYTHON", str(root / ".venv/bin/python")),
            "ISLAND_PREFIX": str(root),
        })
        fixture = json.loads((Path(__file__).parent / "fixtures/egia_stage_reader/valid_egia.json").read_text())
        self.graph["cores"]["egia"] = fixture["cores"]["egia"]
        self.graph["cores"]["egia"]["layers"]["io"]["groups"]["link"]["nodes"]["gateway"]["settings"]["gateway"]["port"] = 16200
        self.graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["egia"] = {
            "id": "egia", "node_type": "isos.process.component@v0.1", "label": "EGIA agent",
            "description": "A04 selected stage runtime", "capabilities": ["process"],
            "settings": {"role": "egia", "args": [], "depends_on": [
                "cores/isos/layers/process/groups/component/nodes/eos"]},
        }
        candidate = self.case.root / "stage.nodes.json"
        pointer = self.graph["cores"]["egia"]["layers"]["evidence"]["groups"]["perception"]["nodes"]["admission"]["inputs"]["resource"]
        pointer["priority"] = 1
        candidate.write_text(json.dumps(self.graph))
        self.case.command("config", "validate", candidate)
        pointer.pop("priority")
        snapshot = self.graph["cores"]["egia"]["layers"]["cognition"]["groups"]["decision"]["nodes"]["planner"]["inputs"]["snapshot"]
        snapshot["priority"] = 1
        candidate.write_text(json.dumps(self.graph))
        result = self.case.command("config", "validate", candidate, success=False)
        self.assertIn('"code":"edge_qos_forbidden"', result.stderr)
        self.assertIn('/cores/egia/layers/cognition/groups/decision/nodes/planner/inputs/snapshot/priority', result.stderr)
        snapshot.pop("priority")
        fixed = self.graph["cores"]["egia"]["layers"]["io"]["groups"]["link"]["nodes"]["gateway"]["inputs"]
        fixed["admitted"] = {"node": "egia/governance/safety/gate", "output": "admission"}
        candidate.write_text(json.dumps(self.graph))
        result = self.case.command("config", "validate", candidate, success=False)
        self.assertIn('"code":"edge_fixed_route"', result.stderr)
        self.assertIn('/cores/egia/layers/io/groups/link/nodes/gateway/inputs/admitted',
                      result.stderr)

    def test_real_egia_stage_apply_pins_exact_owner_overlay(self):
        root = Path(os.environ.get("ISOS_REAL_EGIA_ROOT", ""))
        if not root.is_absolute() or not (root / "bin/egia-config").is_file():
            self.skipTest("set ISOS_REAL_EGIA_ROOT to the A01 EGIA checkout")
        self.case.env.update({
            "ISOS_EGIA_BIN_DIR": str(root / "bin"),
            "ISOS_EGIA_DEFAULTS_DIR": str(root / "etc/egia"),
            "ISOS_EGIA_FACTORY_DEFAULTS_DIR": str(root / "share/defaults/egia"),
            "EGIA_PYTHON": os.environ.get("ISOS_REAL_EGIA_PYTHON", str(root / ".venv/bin/python")),
            "ISLAND_PREFIX": str(root),
        })
        fixture = json.loads((Path(__file__).parent / "fixtures/egia_stage_reader/valid_egia.json").read_text())
        self.graph["cores"]["egia"] = fixture["cores"]["egia"]
        self.graph["cores"]["egia"]["layers"]["io"]["groups"]["link"]["nodes"]["gateway"]["settings"]["gateway"]["port"] = 16200
        nodes = self.graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]
        nodes["egia"] = {
            "id": "egia", "node_type": "isos.process.component@v0.1",
            "label": "EGIA agent", "description": "A01 selected stage runtime",
            "capabilities": ["process"],
            "settings": {"role": "egia", "args": [], "depends_on": [
                "cores/isos/layers/process/groups/component/nodes/eos"]},
        }
        link_settings = self.graph["cores"]["egia"]["layers"]["io"]["groups"]["link"]["nodes"]["gateway"]["settings"]
        link_settings["gateway"]["port"] = 16022
        candidate = self.case.root / "wrong-gateway.nodes.json"
        candidate.write_text(json.dumps(self.graph))
        result = self.case.command("config", "validate", candidate, success=False)
        self.assertIn('"code":"generated_owner_rejected"', result.stderr)
        link_settings["gateway"]["port"] = 16200
        revision = self.case.apply(self.graph)
        self.case.start_server()
        status, selected = self.case.api("node-types/selected")
        self.assertEqual(status, 200, selected)
        self.assertEqual(selected["data"]["graph_revision"], revision)
        stages = {item["id"]: item for item in selected["data"]["catalog"]["types"]
                  if item["id"].startswith("egia.") and item["id"].endswith("@v0.1")}
        self.assertEqual(len(stages), 7)
        self.assertEqual(len(selected["data"]["catalog"]["data_types"]), 23 +
                         len([item for item in selected["data"]["catalog"]["data_types"]
                              if not item["id"].startswith("egia.")]))
        self.assertTrue(all(all("data_type" in field for field in stage["settings"])
                            for stage in stages.values()))
        def setting(stage_id, name):
            return next(field for field in stages[stage_id]["settings"]
                        if field["name"] == name)
        self.assertEqual(setting("egia.link.gateway@v0.1", "gateway")["data_type"], "endpoint")
        self.assertEqual(setting("egia.missions.queue@v0.1", "mission_capacity")["data_type"],
                         "positive_integer")
        self.assertEqual(setting("egia.monitor.health@v0.1", "stall_timeout_s")["data_type"],
                         "positive_number")
        self.assertEqual(setting("egia.link.gateway@v0.1", "gateway")["type"], "endpoint")
        self.assertEqual(stages["egia.link.gateway@v0.1"]["settings_fields"],
                         [{key: value for key, value in field.items() if key != "data_type"}
                          for field in stages["egia.link.gateway@v0.1"]["settings"]])
        status, graph_get = self.case.api("node-graph")
        self.assertEqual(status, 200, graph_get)
        self.assertEqual(graph_get["data"]["revision"], revision)
        self.assertFalse(graph_get["data"]["read_only"])
        self.assertEqual(graph_get["data"]["graph"], self.graph)
        _, _, leaf = self.case.selected()
        overlay = (leaf / "egia/stages.json").read_bytes()
        expected = json.dumps({"revision": revision, "version": "0.1.0",
                               "cores": {"egia": self.graph["cores"]["egia"]}},
                              sort_keys=True, separators=(",", ":")).encode() + b"\n"
        self.assertEqual(overlay, expected)
        manifest = json.loads((leaf / "manifest.json").read_text())
        self.assertIn("egia/stages.json", [item["path"] for item in manifest["files"]])
        self.case.start_server()
        status, options = self.case.api("node-options", {"schema_version": 1, "graph": self.graph})
        self.assertEqual(status, 200, options)
        self.assertEqual(options["data"]["setting_options"], [])
        (leaf / "egia/stages.json").write_bytes(overlay.replace(b'"revision"', b'"revision_changed"', 1))
        for operation in ("run", "restart"):
            status, reply = self.case.control(operation)
            self.assertEqual(status, 422, reply)
            self.assertEqual(reply["data"]["diagnostics"][0]["code"],
                             "generated_manifest_mismatch")
            status, state = self.case.api("process-status")
            self.assertEqual(status, 200, state)
            self.assertEqual(state["data"]["processes"], [])

    def test_real_a01_owner_stays_design_only_and_can_run(self):
        root = Path(os.environ.get("ISOS_REAL_EGIA_A01_ROOT", ""))
        if not root.is_absolute() or not (root / "bin/egia-config").is_file():
            self.skipTest("set ISOS_REAL_EGIA_A01_ROOT to the merged A01 EGIA checkout")
        self.case.env.update({
            "ISOS_EGIA_BIN_DIR": str(root / "bin"),
            "ISOS_EGIA_DEFAULTS_DIR": str(root / "etc/egia"),
            "ISOS_EGIA_FACTORY_DEFAULTS_DIR": str(root / "share/defaults/egia"),
            "EGIA_PYTHON": os.environ.get("ISOS_REAL_EGIA_PYTHON", str(root / ".venv/bin/python")),
            "ISLAND_PREFIX": str(root),
        })
        fixture = json.loads((Path(__file__).parent / "fixtures/egia_stage_reader/valid_egia.json").read_text())
        self.graph["cores"]["egia"] = fixture["cores"]["egia"]
        self.graph["cores"]["egia"]["layers"]["io"]["groups"]["link"]["nodes"]["gateway"]["settings"]["gateway"]["port"] = 16200
        source = self.graph["cores"]["egia"]["layers"]["evidence"]["groups"]["perception"]["nodes"]["admission"]["inputs"]["resource"]
        candidate = self.case.root / "a01-stage.nodes.json"
        candidate.write_text(json.dumps(self.graph))
        self.case.command("config", "validate", candidate)
        before = self.case.graph_path.read_bytes()
        source["priority"] = 0
        candidate.write_text(json.dumps(self.graph))
        rejected = self.case.command("config", "validate", candidate, success=False)
        self.assertIn('"code":"edge_qos_forbidden"', rejected.stderr)
        self.assertIn('/cores/egia/layers/evidence/groups/perception/nodes/admission/inputs/resource/priority', rejected.stderr)
        self.assertEqual(self.case.graph_path.read_bytes(), before)
        source.pop("priority")
        self.case.apply(self.graph)
        self.case.start_server()
        status, reply = self.case.control("run", timeout=90)
        if status != 200:
            for path in sorted((self.case.home / "data/logs").rglob("*.log")):
                print(f"{path}: {path.read_text(errors='replace')[-3000:]}")
            print((self.case.root / "serve.log").read_text(errors="replace")[-3000:])
        self.assertEqual(status, 200, reply)
        status, state = self.case.api("process-status")
        self.assertEqual(status, 200, state)
        self.assertNotIn("egia", {item["id"] for item in state["data"]["processes"]})

    def test_real_egia_scheduler_loss_rejects_restart_before_old_child_stop(self):
        root = Path(os.environ.get("ISOS_REAL_EGIA_ROOT", ""))
        if not root.is_absolute() or not (root / "bin/egia-config").is_file():
            self.skipTest("set ISOS_REAL_EGIA_ROOT to the A04 EGIA checkout")
        selected = self.case.root / "selected-egia/bin"
        selected.mkdir(parents=True)
        shutil.copy2(root / "bin/egia-config", selected / "egia-config")
        original = root / "bin/egia"
        env = dict(os.environ, EGIA_PYTHON=os.environ.get("ISOS_REAL_EGIA_PYTHON", str(root / ".venv/bin/python")), ISLAND_PREFIX=str(root))
        capabilities = json.loads(subprocess.check_output([str(original), "--capabilities"], env=env))
        weaker = dict(capabilities, features=[feature for feature in capabilities["features"]
                                             if feature != "egia-evidence-scheduler-v1"])
        toggle = self.case.root / "drop-egia-scheduler"
        runtime = selected / "egia"
        runtime.write_text("#!/usr/bin/env python3\nimport os,sys\n"
                           "if sys.argv[1:] == ['--capabilities']:\n"
                           f" print({json.dumps(json.dumps(weaker))} if os.path.exists({str(toggle)!r}) "
                           f"else {json.dumps(json.dumps(capabilities))}); sys.exit(0)\n"
                           f"os.execv({str(original)!r}, [{str(original)!r}, *sys.argv[1:]])\n")
        runtime.chmod(0o700)
        self.case.env.update({"ISOS_EGIA_BIN_DIR": str(selected),
                              "ISOS_EGIA_DEFAULTS_DIR": str(root / "etc/egia"),
                              "ISOS_EGIA_FACTORY_DEFAULTS_DIR": str(root / "share/defaults/egia"),
                              "EGIA_PYTHON": env["EGIA_PYTHON"], "ISLAND_PREFIX": str(root)})
        self.case.apply(self.graph)
        self.case.start_server()
        status, reply = self.case.control("run", timeout=90)
        self.assertEqual(status, 200, reply)
        status, before = self.case.api("process-status")
        self.assertEqual(status, 200, before)
        pids = {item["id"]: item["pid"] for item in before["data"]["processes"]}
        fixture = json.loads((Path(__file__).parent / "fixtures/egia_stage_reader/valid_egia.json").read_text())
        self.graph["cores"]["egia"] = fixture["cores"]["egia"]
        self.graph["cores"]["egia"]["layers"]["io"]["groups"]["link"]["nodes"]["gateway"]["settings"]["gateway"]["port"] = 16200
        self.graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["egia"] = {
            "id": "egia", "node_type": "isos.process.component@v0.1", "label": "EGIA agent",
            "description": "A04 selected stage runtime", "capabilities": ["process"],
            "settings": {"role": "egia", "args": [], "depends_on": [
                "cores/isos/layers/process/groups/component/nodes/eos"]},
        }
        original_saved = self.case.graph_path.read_bytes()
        toggle.write_text("omit scheduler before Apply\n")
        status, reply = self.case.http_apply(self.graph)
        self.assertEqual(status, 422, reply)
        self.assertEqual(reply["data"]["diagnostics"][0]["reader"]["requested"]["feature"],
                         "egia-evidence-scheduler-v1")
        self.assertEqual(self.case.graph_path.read_bytes(), original_saved)
        toggle.unlink()
        status, reply = self.case.http_apply(self.graph)
        self.assertEqual(status, 200, reply)
        saved = self.case.graph_path.read_bytes()
        toggle.write_text("omit scheduler\n")
        for operation in ("run", "restart"):
            status, reply = self.case.control(operation)
            self.assertEqual(status, 422, reply)
            diagnostic = reply["data"]["diagnostics"][0]
            self.assertEqual((diagnostic["code"], diagnostic["reader"]["requested"]["feature"]),
                             ("reader_feature_unsupported", "egia-evidence-scheduler-v1"))
            self.assertIn("/inputs/", diagnostic["path"])
            status, after = self.case.api("process-status")
            self.assertEqual(status, 200, after)
            self.assertEqual({item["id"]: item["pid"] for item in after["data"]["processes"]}, pids)
            self.assertEqual(self.case.graph_path.read_bytes(), saved)


if __name__ == "__main__":
    unittest.main(verbosity=2)
