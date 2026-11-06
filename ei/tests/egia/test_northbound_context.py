"""Northbound context stream: the live prompt snapshot for WebUI and the TUI."""

from __future__ import annotations

import asyncio
import json
from datetime import UTC, datetime

import pytest
import websockets

from egia.api.context import ContextSection, ContextSnapshot
from egia.api.status import HealthState
from egia.core.perception.admission import Admission
from egia.io import CONTEXT_PATH, ContextStore, NorthboundServer
from egia.io.state import ServerState


def _snapshot(version: str = "1") -> ContextSnapshot:
  return ContextSnapshot(
    version=version,
    prompt="You are a robot assistant.\nYou hear: hello",
    sections=[
      ContextSection(kind="hearing", rendered="You hear: hello", has_data=True),
    ],
    built_at=datetime.now(UTC),
    observation_ids=["obs-1"],
  )


async def _start_server(store: ContextStore | None, *, ready: bool = True):
  state = ServerState(version="test")
  state.status = HealthState.READY if ready else HealthState.STARTING
  server = NorthboundServer(state, Admission(4), port=0, context_store=store)
  await server.start()
  return server


@pytest.mark.asyncio
async def test_context_stream_emits_latest_then_newer_version() -> None:
  store = ContextStore()
  await store.publish(_snapshot(version="1"))
  server = await _start_server(store)
  _, port = server.address
  try:
    async with websockets.connect(f"ws://127.0.0.1:{port}{CONTEXT_PATH}") as ws:
      first = json.loads(await asyncio.wait_for(ws.recv(), 2))
      assert first["version"] == "1"
      assert first["sections"][0]["kind"] == "hearing"

      await store.publish(_snapshot(version="2"))
      second = json.loads(await asyncio.wait_for(ws.recv(), 2))
      assert second["version"] == "2"
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_context_stream_unavailable_before_readiness() -> None:
  store = ContextStore()
  await store.publish(_snapshot())
  server = await _start_server(store, ready=False)
  _, port = server.address
  try:
    with pytest.raises(websockets.exceptions.ConnectionClosed) as exc:
      async with websockets.connect(f"ws://127.0.0.1:{port}{CONTEXT_PATH}") as ws:
        assert (
          json.loads(await asyncio.wait_for(ws.recv(), 2))["error"] == "unavailable"
        )
        await ws.recv()
    assert exc.value.rcvd.code == 1013
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_context_stream_unavailable_without_snapshot() -> None:
  server = await _start_server(ContextStore())
  _, port = server.address
  try:
    with pytest.raises(websockets.exceptions.ConnectionClosed) as exc:
      async with websockets.connect(f"ws://127.0.0.1:{port}{CONTEXT_PATH}") as ws:
        assert (
          json.loads(await asyncio.wait_for(ws.recv(), 2))["error"] == "unavailable"
        )
        await ws.recv()
    assert exc.value.rcvd.code == 1013
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_context_entry_publishes_each_rebuild(etc_root) -> None:
  from egia.core.context import context_entry
  from egia.lib.config import load_prompt_config

  prompts = load_prompt_config(etc_root)
  queue = Admission(4)
  store = ContextStore()
  ready, stop = asyncio.Event(), asyncio.Event()
  task = asyncio.create_task(
    context_entry(queue, prompts, ready, stop, publish=store.publish)
  )
  await asyncio.wait_for(ready.wait(), 1)
  assert store.version == 1
  assert store.latest() is not None

  await queue.enqueue(kind="hearing", source="test", text="hi")
  for _ in range(50):
    if store.version >= 2:
      break
    await asyncio.sleep(0.05)
  assert store.version == 2
  latest = store.latest()
  assert latest is not None
  assert "hi" in latest.prompt  # type: ignore[union-attr]
  stop.set()
  await asyncio.wait_for(task, 1)
