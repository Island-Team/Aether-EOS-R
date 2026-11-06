"""Northbound WebSocket chat and health regression."""

from __future__ import annotations

import asyncio
import json

import pytest
import websockets

from egia.api.status import HealthState
from egia.core.context import context_entry
from egia.core.missions import MissionQueue
from egia.core.perception.admission import Admission
from egia.core.supervisor import SubsystemSpec, Supervisor
from egia.io import CHAT_PATH, HEALTH_PATH, NorthboundServer
from egia.io.state import ServerState
from egia.lib.config import load_prompt_config


async def _start_pipeline(
  *,
  queue_capacity: int = 8,
  port: int = 0,
) -> tuple[NorthboundServer, Admission, Supervisor, asyncio.Event]:
  prompts = load_prompt_config("etc/egia")
  queue = Admission(queue_capacity)
  state = ServerState(version="test")
  server = NorthboundServer(
    state, queue, host="127.0.0.1", port=port, status_frequency_hz=10
  )

  async def _context_entry(ready: asyncio.Event, stop: asyncio.Event) -> None:
    await context_entry(queue, prompts, ready, stop)

  supervisor = Supervisor([SubsystemSpec(name="context", entry=_context_entry)])
  shutdown = asyncio.Event()

  await server.start()
  await supervisor.start()
  ready = await supervisor.wait_ready()
  assert ready is True
  state.status = HealthState.READY
  state.supervisor = supervisor
  return server, queue, supervisor, shutdown


async def _stop_pipeline(
  server: NorthboundServer,
  supervisor: Supervisor,
  shutdown: asyncio.Event,
) -> None:
  shutdown.set()
  await supervisor.stop()
  await server.stop()


@pytest.mark.asyncio
async def test_health_endpoint_still_works() -> None:
  server, _queue, supervisor, shutdown = await _start_pipeline()
  _, port = server.address
  try:
    async with websockets.connect(f"ws://127.0.0.1:{port}{HEALTH_PATH}") as ws:
      payload = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      assert payload["status"] == "ready"
      assert any(item["name"] == "context" for item in payload["subsystems"])
  finally:
    await _stop_pipeline(server, supervisor, shutdown)


@pytest.mark.asyncio
async def test_chat_accepts_sentence_and_enqueues() -> None:
  server, queue, supervisor, shutdown = await _start_pipeline()
  _, port = server.address
  try:
    async with websockets.connect(f"ws://127.0.0.1:{port}{CHAT_PATH}") as ws:
      await ws.send(
        json.dumps({"sentence": "Where is the cup?", "correlation_id": "c-1"})
      )
      ack = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      assert ack["ok"] is True
      assert ack["correlation_id"] == "c-1"
      assert queue.size == 1
      assert queue.records()[0].text == "Where is the cup?"
      assert queue.records()[0].kind == "hearing"
  finally:
    await _stop_pipeline(server, supervisor, shutdown)


@pytest.mark.asyncio
async def test_chat_is_not_mission_ingress() -> None:
  """Hearing is perception input and never creates a mission."""
  server, queue, supervisor, shutdown = await _start_pipeline()
  missions = MissionQueue(4)
  server._missions = missions
  _, port = server.address
  try:
    async with websockets.connect(f"ws://127.0.0.1:{port}{CHAT_PATH}") as ws:
      await ws.send(json.dumps({"sentence": "bring the cup here"}))
      ack = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      assert ack["ok"] is True
      assert queue.size == 1
      pending = missions.pending()
      assert pending == []
  finally:
    await _stop_pipeline(server, supervisor, shutdown)


@pytest.mark.asyncio
async def test_chat_rejects_invalid_request() -> None:
  server, queue, supervisor, shutdown = await _start_pipeline()
  _, port = server.address
  try:
    async with websockets.connect(f"ws://127.0.0.1:{port}{CHAT_PATH}") as ws:
      await ws.send(json.dumps({"sentence": ""}))
      err = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      assert err["ok"] is False
      assert "invalid_request" in err["error"]
      assert queue.size == 0
  finally:
    await _stop_pipeline(server, supervisor, shutdown)


@pytest.mark.asyncio
async def test_chat_rejects_when_queue_full() -> None:
  server, queue, supervisor, shutdown = await _start_pipeline(queue_capacity=1)
  _, port = server.address
  try:
    async with websockets.connect(f"ws://127.0.0.1:{port}{CHAT_PATH}") as ws:
      await ws.send(json.dumps({"sentence": "first"}))
      ack = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      assert ack["ok"] is True
      await ws.send(json.dumps({"sentence": "second"}))
      err = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      assert err["ok"] is False
      assert err["error"] == "queue_full"
      assert queue.size == 1
  finally:
    await _stop_pipeline(server, supervisor, shutdown)
