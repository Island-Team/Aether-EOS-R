#!/usr/bin/env python3
"""Replay unified graph owner validation with the real EI and EOS adapters."""

import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from generated_config_preflight_cli import CleanHome


ROOT = Path(__file__).resolve().parents[3]
EI = ROOT / "ei"
BIN = ROOT / "target/debug"


def unified_graph(graph):
    """Map the starter and published EI stage fixture into the 0.2 namespace."""
    fixture = json.loads((Path(__file__).parent / "fixtures/egia_stage_reader/valid_egia.json").read_text())
    graph["cores"]["egia"] = fixture["cores"]["egia"]
    graph["cores"]["egia"]["layers"]["io"]["groups"]["link"]["nodes"]["gateway"]["settings"]["gateway"]["port"] = 16200
    merged = {}
    for core, body in graph["cores"].items():
        for layer, content in body["layers"].items():
            new_layer = layer if core == "isos" else f"{'ei' if core == 'egia' else core}_{layer}"
            for group in content["groups"].values():
                for node in group["nodes"].values():
                    old_type = node["node_type"].split("@")[0]
                    node["node_type"] = f"isos.{new_layer}.{old_type.split('.')[-1]}@v0.2"
            merged[new_layer] = content
    graph["version"] = "0.2.0"
    graph["cores"] = {"isos": {"layers": merged}}
    merged["process"]["groups"]["component"]["nodes"]["egia"] = {
        "id": "egia", "node_type": "isos.process.component@v0.2",
        "label": "EI agent", "description": "Selected EI stage runtime",
        "capabilities": ["process"],
        "settings": {"role": "egia", "args": [], "depends_on": [
            "cores/isos/layers/process/groups/component/nodes/bridge"]},
    }

    def rewrite(value):
        if isinstance(value, str):
            parts = value.split("/")
            if len(parts) >= 4 and parts[0] == "cores" and parts[2] == "layers":
                layer = parts[3] if parts[1] == "isos" else f"{'ei' if parts[1] == 'egia' else parts[1]}_{parts[3]}"
                return "/".join(["cores", "isos", "layers", layer, *parts[4:]])
            if len(parts) == 4 and parts[0] in ("isos", "eos", "egia"):
                layer = parts[1] if parts[0] == "isos" else f"{'ei' if parts[0] == 'egia' else parts[0]}_{parts[1]}"
                return "/".join(["isos", layer, *parts[2:]])
            return value
        if isinstance(value, list):
            return [rewrite(item) for item in value]
        if isinstance(value, dict):
            return {rewrite(key): rewrite(item) for key, item in value.items()}
        return value

    return rewrite(graph)


class UnifiedOwnerReplay(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="isos-unified-owner-")
        self.case = CleanHome(self.temp.name)
        self.case.env.update({
            "ISOS_AETHER_BIN_DIR": str(BIN),
            "ISOS_EGIA_BIN_DIR": str(EI / "bin"),
            "ISOS_EI_BIN_DIR": str(EI / "bin"),
            "ISOS_EGIA_DEFAULTS_DIR": str(EI / "etc/egia"),
            "ISOS_EI_DEFAULTS_DIR": str(EI / "etc/egia"),
            "ISOS_EGIA_FACTORY_DEFAULTS_DIR": str(EI / "share/defaults/egia"),
            "ISOS_EI_FACTORY_DEFAULTS_DIR": str(EI / "share/defaults/egia"),
            "EI_PYTHON": str(EI / ".venv/bin/python"),
            "ISLAND_PREFIX": str(EI),
        })
        self.graph = unified_graph(self.case.init())
        self.saved = self.case.graph_path.read_bytes()

    def tearDown(self):
        self.case.close()
        self.temp.cleanup()

    def test_real_owner_validation_and_revision_bound_render(self):
        candidate = self.case.root / "unified.nodes.json"
        candidate.write_text(json.dumps(self.graph))
        revision = hashlib.sha256(candidate.read_bytes()).hexdigest()
        for role, executable, layer in (("eos", BIN / "aether-profile", "eos_dispatcher"),
                                        ("egia", EI / "bin/ei-config", "ei_io")):
            projection = {"version": "0.2.0", "robot": self.graph["robot"],
                          "cores": {"isos": {"layers": {name: content for name, content in
                          self.graph["cores"]["isos"]["layers"].items()
                          if name.startswith("eos_" if role == "eos" else "ei_")}}}}
            request = {"schema_version": 2, "op": "render", "core": role,
                       "revision": revision, "graph": projection, "packages": [],
                       "endpoints": {"eos_gateway": {"transport": "tcp", "host": "127.0.0.1", "port": 16200},
                                     "egia_northbound": {"host": "127.0.0.1", "port": 16060}}}
            env = self.case.env.copy()
            validate = dict(request, op="validate")
            checked = subprocess.run([str(executable), "setup", "--json"], input=json.dumps(validate),
                                     env=env, text=True, capture_output=True, timeout=15)
            self.assertEqual(checked.returncode, 0, checked.stderr)
            self.assertTrue(json.loads(checked.stdout)["ok"], checked.stdout)
            output = subprocess.run([str(executable), "setup", "--json"], input=json.dumps(request),
                                    env=env, text=True, capture_output=True, timeout=15)
            self.assertEqual(output.returncode, 0, output.stderr)
            reply = json.loads(output.stdout)
            self.assertTrue(reply["ok"], reply)
            self.assertTrue(projection["cores"]["isos"]["layers"][layer])
            if role == "egia":
                stage = next(item for item in reply["files"] if item["path"] == "ei/stages.json")
                rendered = json.loads(stage["content"])
                self.assertEqual(rendered["revision"], revision)
                self.assertEqual(rendered["cores"], projection["cores"])
        self.case.command("config", "validate", candidate)
        self.assertEqual(self.case.graph_path.read_bytes(), self.saved)

    def test_rejects_unknown_ei_type_with_precise_path_and_preserves_saved_graph(self):
        original = self.case.graph_path.read_bytes()
        node = self.graph["cores"]["isos"]["layers"]["ei_io"]["groups"]["link"]["nodes"]["gateway"]
        node["node_type"] = "isos.ei_io.future@v0.2"
        candidate = self.case.root / "invalid.nodes.json"
        candidate.write_text(json.dumps(self.graph))
        result = self.case.command("config", "validate", candidate, success=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("/cores/isos/layers/ei_io/groups/link/nodes/gateway/node_type", result.stderr)
        self.assertEqual(self.case.graph_path.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
