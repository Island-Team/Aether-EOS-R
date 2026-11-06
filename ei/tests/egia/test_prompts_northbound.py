"""Northbound ``/prompts`` stream and system-prompt projection."""

from __future__ import annotations

import asyncio
import json

import pytest

from egia.api.prompts import PromptsSnapshot
from egia.core.perception.admission import Admission
from egia.io.northbound import PROMPTS_PATH, NorthboundServer
from egia.io.state import ServerState
from egia.io.store import PromptsStore


@pytest.mark.asyncio
async def test_prompts_ws_stream() -> None:
  import websockets

  from egia.api.status import HealthState

  state = ServerState("test")
  state.status = HealthState.READY
  store = PromptsStore()
  await store.publish(
    PromptsSnapshot(system="You are EGIA.\nUse listed primitives only.")
  )
  server = NorthboundServer(
    state,
    Admission(4),
    host="127.0.0.1",
    port=0,
    prompts_store=store,
  )
  await server.start()
  try:
    _, port = server.address
    async with websockets.connect(f"ws://127.0.0.1:{port}{PROMPTS_PATH}") as ws:
      frame = json.loads(await asyncio.wait_for(ws.recv(), 2.0))
      assert "You are EGIA." in frame["system"]
      assert "primitives" in frame["system"]
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_prompts_ws_gated_before_ready() -> None:
  import websockets

  state = ServerState("test")
  store = PromptsStore()
  await store.publish(PromptsSnapshot(system="ready text"))
  server = NorthboundServer(
    state,
    Admission(4),
    host="127.0.0.1",
    port=0,
    prompts_store=store,
  )
  await server.start()
  try:
    _, port = server.address
    async with websockets.connect(f"ws://127.0.0.1:{port}{PROMPTS_PATH}") as ws:
      frame = json.loads(await asyncio.wait_for(ws.recv(), 2.0))
      assert frame == {"error": "unavailable"}
  finally:
    await server.stop()
