#!/usr/bin/env python3
"""Compare selected EOS binaries and setup reader identity with the P05 fixture."""

import json
from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "target/debug"
FIXTURE = json.loads((ROOT / "tests/fixtures/reader_features/advertised.json").read_text())
CASES = json.loads((ROOT / "tests/fixtures/reader_features/cases.json").read_text())


def call(binary, *args, request=None):
    result = subprocess.run(
        [str(BIN / binary), *args],
        input=None if request is None else json.dumps(request),
        text=True, capture_output=True, timeout=5, check=True,
    )
    assert len(result.stdout) <= 65536, "probe output exceeds 64 KiB"
    assert len(result.stdout.splitlines()) == 1, "probe must write one JSON object"
    return json.loads(result.stdout)


class ReaderFeaturesReplay(unittest.TestCase):
    def test_exact_selected_binary_probes(self):
        for name, expected in FIXTURE["probes"].items():
            with self.subTest(name=name):
                actual = call(name, "--capabilities")
                self.assertEqual(actual, expected)
                self.assertEqual(actual["binary"], name)
                self.assertEqual(len(actual["features"]), len(set(actual["features"])))
                if name != "aether-http-bridge":
                    self.assertEqual(len(actual["supported_node_types"]), 18)
                    self.assertEqual(actual["supported_node_types"],
                                     sorted(actual["supported_node_types"]))
                else:
                    self.assertNotIn("supported_node_types", actual)

    def test_setup_identity_matches_profile_probe_and_preserves_failure(self):
        probe = call("aether-profile", "--capabilities")
        reply = call("aether-profile", "setup", "--json", request={
            "schema_version": 1, "op": "identity", "core": "eos"})
        self.assertEqual(reply["schema_version"], 1)
        self.assertTrue(reply["ok"])
        self.assertEqual(reply["diagnostics"], [])
        self.assertEqual(reply["files"], [])
        identity = reply["identity"]
        self.assertEqual(identity["core"], "eos")
        self.assertEqual(identity["setup_schema_version"], 1)
        for field in FIXTURE["setup_identity"]["same_reader_fields"]:
            self.assertEqual(identity[field], probe[field], field)
        bad = call("aether-profile", "setup", "--json", request={
            "schema_version": 2, "op": "identity", "core": "eos"})
        self.assertFalse(bad["ok"])
        self.assertEqual(bad["files"], [])
        self.assertEqual(bad["diagnostics"][0]["code"], "unsupported_version")
        self.assertEqual(bad["diagnostics"][0]["path"], "schema_version")

    def test_direct_child_and_bridge_feature_claims_cover_frozen_forms(self):
        probes = {name: call(name, "--capabilities")
                  for name in FIXTURE["probes"]}
        ids = {case["id"]: case for case in CASES["cases"]}
        for name in ("target_mujoco_verified", "legacy_contributed_mujoco_verified",
                     "legacy_builtin_backend_pinned_mujoco"):
            case = ids[name]
            self.assertEqual(case["required"]["aether-runtime"], ["mujoco-child-v2"])
            self.assertIn("mujoco-child-v2", probes["aether-runtime"]["features"])
        bridge = ids["enabled_bridge_without_camera_gate"]
        self.assertFalse(bridge["camera_gate_present"])
        self.assertIn("vision-ws", probes["aether-http-bridge"]["features"])
        self.assertNotIn("graph-0.1-reader", probes["aether-http-bridge"]["features"])

    def test_setup_validates_safe_legacy_and_target_graphs(self):
        for version in ("1.0.0", "0.1.0"):
            with self.subTest(version=version):
                graph = {"version": version,
                         "robot": {"id": "demo", "name": "Demo"},
                         "cores": {"eos": {"layers": {"dispatcher": {
                             "groups": {"control": {"nodes": {"command": {
                                 "id": "command",
                                 "node_type": "eos.dispatcher.control@v0.1"
                                 if version == "0.1.0" else "eos.dispatcher.control@v1.0",
                                 "label": "Dispatcher", "description": "Command admission",
                                 "observation": None, "movement": None,
                             }}}}
                         }}}}}
                reply = call("aether-profile", "setup", "--json", request={
                    "schema_version": 1, "op": "validate", "core": "eos",
                    "revision": "fixture", "graph": graph,
                    "endpoints": {}, "packages": []})
                self.assertTrue(reply["ok"], reply)
                self.assertEqual(reply["files"], [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
