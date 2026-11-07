#!/usr/bin/env python3
"""First-use replay for the exact published MuJoCo 0.1.1 asset.

Usage: python3 tests/real_mujoco_first_use.py RELEASE_JSON ASSET_TAR_GZ MUJOCO_ROOT
The test needs the official MuJoCo 3.16.0 Linux runtime under MUJOCO_ROOT.
"""

import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tarfile
import tempfile
import time


MANIFEST_SHA256 = "c421039753c24ffc74658d6b1dfe5c9de6a1ed77b64df8fbaee77063da837096"
ASSET_SHA256 = "bb2862b01c3ccab8b68570e766a0ad26c8c5783b3c87cf99225eb0edb7a260f4"


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def exchange(path: Path, request: dict) -> dict:
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(8)
        client.connect(str(path))
        body = json.dumps(request, separators=(",", ":")).encode()
        client.sendall(struct.pack("<II", len(body), 0) + body)

        def read_exact(count: int) -> bytes:
            data = bytearray()
            while len(data) < count:
                part = client.recv(count - len(data))
                assert part, "simulator closed the response"
                data.extend(part)
            return bytes(data)

        size, payload = struct.unpack("<II", read_exact(8))
        assert 0 < size <= 65536 and payload == 0
        return json.loads(read_exact(size))


def request(path: Path, op: str, **fields: str) -> dict:
    return exchange(path, {"version": "2.0.0", "id": op, "node": "base-0", "op": op, **fields})


def run_child(binary: Path, scene: Path, runtime: Path, directory: Path) -> str:
    main_socket = directory / "sim.sock"
    stop_socket = Path(f"{main_socket}.stop")
    environment = dict(os.environ)
    environment["MUJOCO_ROOT"] = str(runtime)
    environment["LD_LIBRARY_PATH"] = str(runtime / "lib")
    environment.setdefault("MUJOCO_GL", "egl")
    child = subprocess.Popen(
        [str(binary), "--managed-strict-socket", "--v2-socket", str(main_socket), "--scene", str(scene)],
        env=environment,
        cwd=directory,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    try:
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if main_socket.is_socket() and stop_socket.is_socket():
                break
            if child.poll() is not None:
                raise AssertionError(f"simulator exited before readiness: {child.stderr.read().decode()}")
            time.sleep(0.05)
        else:
            raise AssertionError("simulator did not create both managed sockets")
        health = request(stop_socket, "health")
        generation = health["generation_id"]
        assert health["ok"] and "strict-stop-v1" in health["features"]
        movement = request(
            main_socket, "base_move", command_id="first-use-motion", generation_id=generation,
            stop_epoch="0", linear_x_m_s=0.1, linear_y_m_s=0, angular_z_rad_s=0,
            valid_for_ms=200,
        )
        assert movement["ok"] and movement["accepted"]
        stopped = request(stop_socket, "stop", command_id="first-use-stop", generation_id=generation)
        assert stopped["ok"] and stopped["accepted"]
        assert stopped["base_stop"] == "stopped" and stopped["arm_stop"] == "stopped"
        assert int(stopped["stop_epoch"]) >= 1
        late = request(
            main_socket, "base_move", command_id="late-motion", generation_id=generation,
            stop_epoch="0", linear_x_m_s=0.1, linear_y_m_s=0, angular_z_rad_s=0,
            valid_for_ms=200,
        )
        assert not late["ok"] and late["error"]["code"] == "stop_fenced"
        return generation
    finally:
        if child.poll() is None:
            os.killpg(child.pid, signal.SIGTERM)
        try:
            _, stderr = child.communicate(timeout=12)
        except subprocess.TimeoutExpired:
            os.killpg(child.pid, signal.SIGKILL)
            child.communicate(timeout=5)
            raise AssertionError("simulator group did not stop within deadline")
        assert child.returncode == 0, stderr.decode()
        assert not main_socket.exists() and not stop_socket.exists()


def main() -> None:
    manifest, archive, runtime = map(Path, sys.argv[1:4])
    assert digest(manifest) == MANIFEST_SHA256, "unexpected release manifest"
    assert digest(archive) == ASSET_SHA256, "unexpected published asset"
    assert (runtime / "lib/libmujoco.so.3.16.0").is_file(), "MuJoCo 3.16.0 runtime missing"
    with tempfile.TemporaryDirectory(prefix="isos-mujoco-first-use-") as temporary:
        root = Path(temporary)
        bundle = root / "bundle"
        bundle.mkdir()
        with tarfile.open(archive, "r:gz") as source:
            for member in source.getmembers():
                target = bundle / member.name
                assert target.resolve().is_relative_to(bundle), "unsafe archive member"
                assert member.isdir() or member.isfile(), "unsupported archive member"
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with source.extractfile(member) as stream, target.open("wb") as output:
                        while chunk := stream.read(1024 * 1024):
                            output.write(chunk)
                    target.chmod(member.mode & 0o777)
        binary = bundle / "bin/eos-mujoco-sim"
        scene = bundle / "assets/scene.xml"
        assert binary.is_file() and os.access(binary, os.X_OK) and scene.is_file()
        first = run_child(binary, scene, runtime, root)
        second = run_child(binary, scene, runtime, root)
        assert first != second, "restart reused the previous simulator generation"
    print("published MuJoCo 0.1.1: verified asset, readiness, safe Stop, child reap, restart passed")


if __name__ == "__main__":
    main()
