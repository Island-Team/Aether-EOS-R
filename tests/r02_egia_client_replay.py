#!/usr/bin/env python3
"""Replay the selected EGIA typed client against the Rust EOS gRPC listener.

Run with EGIA's dependency environment. EOS/EGIA paths may be supplied with
EGIA_SRC and AETHER_RUNTIME_BIN; defaults match the sibling checkouts.
"""

from __future__ import annotations

import asyncio
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
EGIA_SRC = Path(os.environ.get("EGIA_SRC", ROOT.parent / "Island-egia" / "src"))
EOS_BIN = Path(os.environ.get("AETHER_RUNTIME_BIN", ROOT / "target/debug/aether-runtime"))
sys.path.insert(0, str(EGIA_SRC))

from egia.core.link.client import EosClient  # noqa: E402
from egia.lib.config import GatewayConfig  # noqa: E402


async def check(port: int) -> None:
  client = EosClient(GatewayConfig(host="127.0.0.1", grpc_port=port))
  try:
    discovery = await client.initialize()
    assert discovery.robot_id == "demo-0"
    assert len(discovery.devices) == 3
    assert len(client.resources) == 0
    assert {item.name for item in client.primitives} == {
      "base.velocity", "arm.movej", "arm.movel", "safety.estop"
    }
    primitive = next(item for item in client.primitives if item.name == "base.velocity")
    assert primitive.name == "base.velocity"
    assert primitive.capability_name is not None
    assert primitive.health == "ready"
    assert not client.skills_available
    ack = await client.call_primitive(
      "base.velocity", "drive",
      {"linear_x_m_s": 0.1, "linear_y_m_s": 0.0,
       "angular_z_rad_s": 0.0, "valid_for_ms": 100},
      command_id="egia-r02-velocity-1",
    )
    assert ack.command_id == "egia-r02-velocity-1"
    assert ack.primitive == "base.velocity"
    assert ack.status.value == "outcome_unknown"
    await client.initialize(force=True)
    assert next(item for item in client.primitives if item.name == "base.velocity").health == "unavailable"
    assert next(item for item in client.primitives if item.name == "safety.estop").health == "ready"
    estop = await client.call_primitive(
      "safety.estop", "", {}, command_id="egia-r02-estop-1",
    )
    assert estop.command_id == "egia-r02-estop-1"
    assert estop.primitive == "safety.estop"
    assert estop.status.value in {"succeeded", "outcome_unknown"}
    print(json.dumps({"robot_id": discovery.robot_id, "devices": len(discovery.devices),
                      "primitives": len(client.primitives), "skills_available": client.skills_available,
                      "operation_status": ack.status.value,
                      "estop_status": estop.status.value}))
  finally:
    await client.aclose()


def main() -> None:
  with tempfile.TemporaryDirectory(prefix="r02-egia-") as directory:
    directory = Path(directory)
    child = subprocess.Popen(
      [str(EOS_BIN), "--profile", str(ROOT / "etc/eos/backends/mock-cxx.toml"),
       "--journal-dir", str(directory / "journal"), "--socket", str(directory / "control.sock"),
       "--grpc-port", "0"],
      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    try:
      assert child.stdout is not None
      ready_line = child.stdout.readline()
      assert ready_line, child.stderr.read() if child.stderr else "no ready line"
      ready = json.loads(ready_line)
      assert ready["status"] == "ready"
      port = int(ready["grpc_address"].rsplit(":", 1)[1])
      asyncio.run(check(port))
      with socket.socket(socket.AF_UNIX) as control:
        control.connect(str(directory / "control.sock"))
        control.sendall(b'{"op":"shutdown"}\n')
        assert json.loads(control.makefile("rb").readline())["ok"]
      assert child.wait(timeout=5) == 0
    finally:
      if child.poll() is None:
        child.kill()
        child.wait()


if __name__ == "__main__":
  main()
