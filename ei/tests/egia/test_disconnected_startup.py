"""Disconnected startup enters an empty sandbox phase and keeps retrying EOS."""

from __future__ import annotations

import asyncio
import json
import os
import signal
import socket
import time

import pytest
import websockets

from egia.core import launch
from egia.lib.config import GatewayConfig
from tests.egia.stub.gateway import start_stub_gateway, stop_stub_gateway


def _unused_port() -> int:
  with socket.socket() as probe:
    probe.bind(("127.0.0.1", 0))
    return int(probe.getsockname()[1])


async def _read_json(url: str, *, timeout: float = 5.0) -> dict:
  deadline = time.monotonic() + timeout
  while time.monotonic() < deadline:
    try:
      async with websockets.connect(url, open_timeout=0.5) as stream:
        return json.loads(await asyncio.wait_for(stream.recv(), timeout=1.0))
    except (
      OSError,
      TimeoutError,
      websockets.ConnectionClosed,
      websockets.InvalidStatus,
    ):
      await asyncio.sleep(0.02)
  raise AssertionError(f"no frame from {url} within {timeout}s")


async def _wait_ready(url: str, *, timeout: float = 10.0) -> dict:
  deadline = time.monotonic() + timeout
  while time.monotonic() < deadline:
    payload = await _read_json(url, timeout=1.0)
    if payload.get("status") == "ready":
      return payload
    await asyncio.sleep(0.02)
  raise AssertionError("pipeline did not become ready after EOS connected")


@pytest.mark.asyncio
async def test_disconnected_agent_serves_placeholder_then_connects(
  stub_launch_config, tmp_path, etc_root
) -> None:
  gateway_port = _unused_port()
  server_port = _unused_port()
  config = stub_launch_config.model_copy(
    update={
      "gateway": GatewayConfig(host="127.0.0.1", grpc_port=gateway_port),
      "server": stub_launch_config.server.model_copy(update={"port": server_port}),
      "monitor": stub_launch_config.monitor.model_copy(
        update={"heartbeat_interval_s": 0.05}
      ),
      "launch": stub_launch_config.launch.model_copy(update={"memory": False}),
      "decision": stub_launch_config.decision.model_copy(
        update={
          "proposer": stub_launch_config.decision.proposer.model_copy(
            update={"enabled": False}
          )
        }
      ),
    }
  )
  task = asyncio.create_task(
    launch(config, state_dir=tmp_path / "state", etc=str(etc_root))
  )
  stub = None
  base = f"ws://127.0.0.1:{server_port}/api/aether/egia"
  try:
    health = await _read_json(f"{base}/health")
    assert health["status"] == "degraded"
    prompts = await _read_json(f"{base}/prompts")
    assert "Waiting for backend connections" in prompts["system"]
    primitives = await _read_json(f"{base}/primitives")
    assert primitives == {"primitives": [], "count": 0}
    skills = await _read_json(f"{base}/skills")
    assert skills["available"] is False
    assert not task.done()

    stub = await start_stub_gateway(port=gateway_port)
    assert stub.port == gateway_port
    await _wait_ready(f"{base}/health")
    connected_primitives = await _read_json(f"{base}/primitives")
    assert connected_primitives["count"] > 0
  finally:
    if not task.done():
      os.kill(os.getpid(), signal.SIGINT)
    await asyncio.wait_for(task, timeout=15)
    if stub is not None:
      await stop_stub_gateway(stub)
