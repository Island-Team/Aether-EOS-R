#!/usr/bin/env python3
"""P06 bounded process stderr replay for the three persistent Rust EOS binaries."""

import json
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "target/debug"
PROFILE = ROOT / "etc/eos/backends/mock-cxx.toml"
BACKEND_PROFILE = ROOT / "examples/v2/mujoco-isos.toml"


def backend_fixture(root, duration=0.8, confirm_stop=True):
    simulator_socket = root / "sim.sock"
    profile = root / "graph.toml"
    source = BACKEND_PROFILE.read_text().replace(
        "/tmp/mujoco-v2.sock", str(simulator_socket)).replace(
        "groups.backend", "groups.mujoco").replace(
        "/groups/backend/", "/groups/mujoco/").replace(
        "eos.process.backend@v1.0", "eos.process.mujoco@v1.0")
    for node in ("camera.nodes.front", "base.nodes.drive", "manipulator.nodes.arm"):
        source += (f"\n[cores.eos.layers.hardware.groups.{node}.adapter.package]\n"
                   'id = "island/mujoco-sim"\nversion = "0.1.1"\n')
    source += ('\n[cores.eos.layers.process.groups.mujoco.nodes.simulator.package]\n'
               'id = "island/mujoco-sim"\nversion = "0.1.1"\n')
    profile.write_text(source)
    catalog = json.loads(subprocess.check_output([str(BIN / "aether-profile"), "catalog"]))
    process = next(item for item in catalog["types"] if item["id"] == "eos.process.backend@v1.0")
    process["id"] = "eos.process.mujoco@v1.0"
    process["group"] = "mujoco"
    sidecar = root / "node-types.json"
    sidecar.write_text(json.dumps({
        "package": {"id": "island/mujoco-sim", "version": "0.1.1"},
        "node_types": [process], "safety_adapters": [], "data_types": [],
        "adapter_families": [{"id": "mujoco-v2", "device_kinds":
                              ["camera", "base", "manipulator"],
                              "transport": "unix_socket", "description": "Verified MuJoCo device socket",
                              "settings_schema": None,
                              "supported_primitives": ["base.velocity@1.0.0", "arm.movej@1.0.0"]}]}))
    scene = root / "scene.xml"
    scene.write_text("<mujoco/>")
    stub = root / "sim.py"
    stub.write_text("""#!/usr/bin/python3
import json, socket, struct, sys, time
from pathlib import Path
path = sys.argv[sys.argv.index('--v2-socket') + 1]
server = socket.socket(socket.AF_UNIX)
server.bind(path)
server.listen(1)
server.settimeout(0.1)
Path(%r).write_text(str(__import__('os').getpid()))
print('raw-simulator-stderr', file=sys.stderr, flush=True)
deadline = time.monotonic() + %s
while time.monotonic() < deadline:
    try:
        client, _ = server.accept()
    except socket.timeout:
        continue
    with client:
        header = client.recv(8)
        if len(header) != 8:
            continue
        length, payload = struct.unpack('<II', header)
        request = bytearray()
        while len(request) < length:
            request.extend(client.recv(length - len(request)))
        metadata = json.loads(request)
        if metadata.get('op') == 'stop':
            Path(%r).write_text('stop received')
        accepted = metadata.get('op') != 'stop' or %r
        reply = json.dumps({'version':'2.0.0','id':metadata['id'],'ok':accepted,
                            'error':{'message':'TOP_SECRET_9F'}}).encode()
        client.sendall(struct.pack('<II', len(reply), 0) + reply)
""" % (str(root / "sim.pid"), duration, str(root / "stop.marker"), confirm_stop))
    stub.chmod(0o700)
    return profile, sidecar, scene, stub


def lines(raw):
    result = []
    for line in raw.splitlines(keepends=True):
        assert line.endswith(b"\n"), line
        assert len(line) <= 8064, len(line)
        event = json.loads(line)
        assert set(event) == {"timestamp", "level", "source", "core", "node_path",
                              "event_code", "run_id", "correlation_id", "message", "fields"}
        assert "pid" not in event
        result.append(event)
    return result


def first_two(process):
    output = bytearray()
    while output.count(b"\n") < 2:
        readable, _, _ = select.select([process.stderr], [], [], 5)
        assert readable, "event startup timed out"
        output.extend(os.read(process.stderr.fileno(), 4096))
    return bytes(output)


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


class ProcessEvents(unittest.TestCase):
    def test_bridge_and_config_are_locally_ready_then_stop(self):
        with tempfile.TemporaryDirectory(prefix="eos-p06-") as directory:
            for name, source, args in [
                ("aether-http-bridge", "bridge", ["--socket", str(Path(directory) / "missing.sock")]),
                ("aether-config-server", "config", ["--profile", str(PROFILE)]),
            ]:
                with self.subTest(name=name):
                    process = subprocess.Popen(
                        [str(BIN / name), *args, "--listen", f"127.0.0.1:{free_port()}",
                         "--run-id", "run_7"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    initial = first_two(process)
                    events = lines(initial)
                    self.assertEqual([event["event_code"] for event in events],
                                     [f"{source}_starting", f"{source}_ready"])
                    self.assertTrue(all(event["source"] == source and event["core"] == "eos"
                                        and event["run_id"] == "run_7" for event in events))
                    process.send_signal(signal.SIGTERM)
                    stdout, remaining = process.communicate(timeout=5)
                    self.assertEqual(process.returncode, 0)
                    self.assertEqual(stdout, b"")
                    self.assertEqual([event["event_code"] for event in lines(remaining)],
                                     [f"{source}_stopping", f"{source}_stopped"])

    def test_invalid_run_ids_and_digest_pairs_emit_no_event(self):
        cases = [
            ("aether-runtime", ["--run-id", "bad/path"]),
            ("aether-runtime", ["--run-id", "run_1", "--run-id", "run_2"]),
            ("aether-runtime", ["--backend-release-sha256", "a" * 64]),
            ("aether-runtime", ["--backend-payload-sha256", "A" * 64]),
            ("aether-http-bridge", ["--run-id", "bad/path"]),
            ("aether-config-server", ["--run-id", "bad/path"]),
        ]
        for name, args in cases:
            with self.subTest(name=name, args=args):
                result = subprocess.run([str(BIN / name), *args], capture_output=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn(b"event_code", result.stderr)

    def test_config_failure_is_terminal_and_probes_preserve_stdout(self):
        result = subprocess.run(
            [str(BIN / "aether-config-server"), "--profile", "/private/TOP_SECRET_9F.toml",
             "--listen", f"127.0.0.1:{free_port()}", "--run-id", "run_7"],
            capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual([event["event_code"] for event in lines(result.stderr)],
                         ["config_starting", "config_start_failed"])
        self.assertNotIn(b"TOP_SECRET_9F", result.stderr)
        for name in ("aether-runtime", "aether-http-bridge", "aether-config-server"):
            with self.subTest(name=name):
                probe = subprocess.run([str(BIN / name), "--capabilities"], capture_output=True,
                                       timeout=5, check=True)
                self.assertEqual(probe.stderr, b"")
                self.assertEqual(json.loads(probe.stdout)["binary"], name)

    def test_runtime_backend_provenance_loss_and_private_capture(self):
        with tempfile.TemporaryDirectory(prefix="eos-p06-backend-") as directory:
            root = Path(directory)
            profile, sidecar, scene, stub = backend_fixture(root)
            eos_socket = root / "eos.sock"
            raw = root / "owner/eos"
            release_digest = "a" * 64
            payload_digest = "b" * 64
            process = subprocess.Popen([
                str(BIN / "aether-runtime"), "--profile", str(profile), "--journal-dir",
                str(root / "journal"), "--socket", str(eos_socket), "--run-id", "run_7",
                "--node-types", str(sidecar),
                "--backend-node", "cores/eos/layers/process/groups/mujoco/nodes/simulator",
                "--backend-executable", str(stub), "--backend-scene", str(scene),
                "--backend-release-sha256", release_digest,
                "--backend-payload-sha256", payload_digest,
                "--log-dir", str(raw),
            ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                stdout, stderr = process.communicate(timeout=8)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
            events = lines(stderr)
            codes = [event["event_code"] for event in events]
            self.assertEqual(codes[:4], ["eos_starting", "eos_package_admitted",
                                         "eos_backend_ready", "eos_ready"], events)
            self.assertEqual(codes.count("eos_backend_lost"), 1)
            self.assertNotIn("eos_stopped", codes)
            package = events[1]
            self.assertEqual(package["node_path"],
                             "/cores/eos/layers/process/groups/mujoco/nodes/simulator")
            self.assertEqual(package["fields"]["release_sha256"], release_digest)
            self.assertEqual(package["fields"]["payload_sha256"], payload_digest)
            self.assertTrue(all(event["run_id"] == "run_7" for event in events))
            self.assertNotIn(b"raw-simulator-stderr", stderr)
            self.assertIn(b"raw-simulator-stderr", (raw / "mujoco.log").read_bytes())
            self.assertIn(b'"status":"ready"', stdout)

    def test_standalone_backend_warns_without_digest_claim(self):
        with tempfile.TemporaryDirectory(prefix="eos-p06-standalone-") as directory:
            root = Path(directory)
            profile, sidecar, scene, stub = backend_fixture(root)
            process = subprocess.Popen([
                str(BIN / "aether-runtime"), "--profile", str(profile), "--journal-dir",
                str(root / "journal"), "--socket", str(root / "eos.sock"),
                "--node-types", str(sidecar),
                "--backend-node", "cores/eos/layers/process/groups/mujoco/nodes/simulator",
                "--backend-executable", str(stub), "--backend-scene", str(scene),
            ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                _, stderr = process.communicate(timeout=8)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
            events = lines(stderr)
            provenance = next(event for event in events if event["event_code"] ==
                              "eos_package_provenance_unavailable")
            self.assertNotIn("release_sha256", provenance["fields"])
            self.assertNotIn("payload_sha256", provenance["fields"])
            self.assertNotIn(b"raw-simulator-stderr", stderr)

    def test_graceful_stop_reaps_backend_after_stop_events(self):
        with tempfile.TemporaryDirectory(prefix="eos-p06-stop-") as directory:
            root = Path(directory)
            profile, sidecar, scene, stub = backend_fixture(root, duration=20,
                                                            confirm_stop=False)
            eos_socket = root / "eos.sock"
            process = subprocess.Popen([
                str(BIN / "aether-runtime"), "--profile", str(profile), "--journal-dir",
                str(root / "journal"), "--socket", str(eos_socket), "--node-types", str(sidecar),
                "--backend-node", "cores/eos/layers/process/groups/mujoco/nodes/simulator",
                "--backend-executable", str(stub), "--backend-scene", str(scene),
                "--log-dir", str(root / "owner/eos"), "--run-id", "run_7",
            ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                readable, _, _ = select.select([process.stdout], [], [], 5)
                self.assertTrue(readable)
                ready = process.stdout.readline()
                self.assertEqual(json.loads(ready)["status"], "ready")
                with socket.socket(socket.AF_UNIX) as client:
                    client.connect(str(eos_socket))
                    client.sendall(b'{"op":"shutdown","correlation_id":"op_42"}\n')
                    client.recv(4096)
                _, stderr = process.communicate(timeout=30)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
            self.assertEqual(process.returncode, 0)
            events = lines(stderr)
            codes = [event["event_code"] for event in events]
            self.assertEqual(codes[:4], ["eos_starting", "eos_package_provenance_unavailable",
                                         "eos_backend_ready", "eos_ready"])
            self.assertEqual(codes[-1], "eos_stopped")
            self.assertLess(codes.index("eos_stopping"), codes.index("eos_stopped"))
            self.assertIn("eos_stop_unconfirmed", codes)
            self.assertTrue(all(event["correlation_id"] == "op_42" for event in events
                                if event["event_code"] in ("eos_stopping", "eos_stop_unconfirmed",
                                                            "eos_stopped")))
            self.assertNotIn(b"TOP_SECRET_9F", stderr)
            self.assertTrue((root / "stop.marker").exists())
            pid = int((root / "sim.pid").read_text())
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)

    def test_full_stderr_pipe_does_not_block_emergency_stop_or_reap(self):
        with tempfile.TemporaryDirectory(prefix="eos-p06-full-pipe-") as directory:
            root = Path(directory)
            profile, sidecar, scene, stub = backend_fixture(root, duration=20,
                                                            confirm_stop=False)
            read_fd, write_fd = os.pipe()
            os.set_blocking(write_fd, False)
            try:
                while True:
                    os.write(write_fd, b"x" * 4096)
            except BlockingIOError:
                pass
            eos_socket = root / "eos.sock"
            process = subprocess.Popen([
                str(BIN / "aether-runtime"), "--profile", str(profile), "--journal-dir",
                str(root / "journal"), "--socket", str(eos_socket), "--node-types", str(sidecar),
                "--backend-node", "cores/eos/layers/process/groups/mujoco/nodes/simulator",
                "--backend-executable", str(stub), "--backend-scene", str(scene),
                "--log-dir", str(root / "owner/eos"), "--run-id", "run_7",
            ], stdout=subprocess.PIPE, stderr=write_fd)
            os.close(write_fd)
            try:
                readable, _, _ = select.select([process.stdout], [], [], 5)
                self.assertTrue(readable)
                ready = process.stdout.readline()
                self.assertEqual(json.loads(ready)["status"], "ready")
                with socket.socket(socket.AF_UNIX) as client:
                    client.connect(str(eos_socket))
                    client.sendall(b'{"op":"shutdown","correlation_id":"op_42"}\n')
                    client.recv(4096)
                self.assertEqual(process.wait(timeout=5), 0)
                self.assertTrue((root / "stop.marker").exists())
                pid = int((root / "sim.pid").read_text())
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                os.close(read_fd)


if __name__ == "__main__":
    unittest.main(verbosity=2)
