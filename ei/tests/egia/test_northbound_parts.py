"""Northbound WebSocket streams for v3 world resources."""

from __future__ import annotations

import asyncio
import errno
import json
from datetime import UTC, datetime
from unittest.mock import AsyncMock, patch

import pytest
import websockets

from egia.api.status import HealthState
from egia.contracts.endpoints import ResourceKind
from egia.contracts.perception.world.asr import AsrSnapshot
from egia.contracts.perception.world.detection import Detection, DetectionSnapshot
from egia.contracts.perception.world.state import WorldEntity, WorldStateSnapshot
from egia.core.perception.admission import Admission
from egia.io import PART_PATHS, NorthboundServer, PartStore
from egia.io.northbound import NorthboundBindError
from egia.io.state import ServerState


def _snapshot(kind: ResourceKind, sequence: int):
  now = datetime.now(UTC)
  if kind is ResourceKind.WORLD:
    return WorldStateSnapshot(
      sequence=sequence,
      timestamp=now,
      frame_id="map",
      entities=[
        WorldEntity(id="obj:crate-1", label="crate", position_m=(1.0, 2.0, 0.0))
      ],
      relations=[],
    )
  if kind is ResourceKind.DETECTION:
    return DetectionSnapshot(
      sequence=sequence,
      timestamp=now,
      source="camera-0",
      source_sequence=sequence,
      detections=[
        Detection(
          id="human:operator-1",
          kind="human",
          label="operator",
          confidence=0.95,
        )
      ],
    )
  return AsrSnapshot(
    sequence=sequence,
    timestamp=now,
    text="hello robot",
    language="en",
    confidence=0.9,
  )


async def _start_part_server(kind: ResourceKind, store: PartStore):
  state = ServerState(version="test")
  state.status = HealthState.READY
  server = NorthboundServer(
    state,
    Admission(4),
    port=0,
    part_stores={kind: store},
  )
  await server.start()
  return server


@pytest.mark.asyncio
async def test_listener_collision_has_an_actionable_error() -> None:
  server = NorthboundServer(ServerState(version="test"), Admission(4), port=16060)
  busy = OSError(errno.EADDRINUSE, "address already in use")
  with (
    patch("egia.io.northbound.serve", AsyncMock(side_effect=busy)),
    pytest.raises(NorthboundBindError, match="127.0.0.1:16060 is already in use"),
  ):
    await server.start()


@pytest.mark.asyncio
@pytest.mark.parametrize("kind", list(ResourceKind))
async def test_world_stream_emits_latest_then_newer_sequence(
  kind: ResourceKind,
) -> None:
  store = PartStore()
  await store.publish(_snapshot(kind, 3))
  server = await _start_part_server(kind, store)
  _, port = server.address
  try:
    async with websockets.connect(f"ws://127.0.0.1:{port}{PART_PATHS[kind]}") as ws:
      first = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      assert first["sequence"] == 3
      assert (
        first["type"]
        == {
          ResourceKind.WORLD: "world.snapshot",
          ResourceKind.DETECTION: "detection.snapshot",
          ResourceKind.ASR: "asr.snapshot",
        }[kind]
      )

      await store.publish(_snapshot(kind, 4))
      second = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      assert second["sequence"] == 4
  finally:
    await server.stop()


@pytest.mark.asyncio
@pytest.mark.parametrize("kind", list(ResourceKind))
async def test_world_stream_unavailable_when_no_store_wired(kind: ResourceKind) -> None:
  state = ServerState(version="test")
  state.status = HealthState.READY
  server = NorthboundServer(state, Admission(4), port=0)
  await server.start()
  _, port = server.address
  try:
    with pytest.raises(websockets.exceptions.ConnectionClosed) as exc:
      async with websockets.connect(f"ws://127.0.0.1:{port}{PART_PATHS[kind]}") as ws:
        assert json.loads(await asyncio.wait_for(ws.recv(), timeout=2)) == {
          "error": "unavailable"
        }
        await ws.recv()
    assert exc.value.rcvd is not None
    assert exc.value.rcvd.code == 1013
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_world_stream_unavailable_before_readiness() -> None:
  store = PartStore()
  state = ServerState(version="test")
  state.status = HealthState.STARTING
  server = NorthboundServer(
    state,
    Admission(4),
    port=0,
    part_stores={ResourceKind.WORLD: store},
  )
  await server.start()
  _, port = server.address
  try:
    with pytest.raises(websockets.exceptions.ConnectionClosed) as exc:
      async with websockets.connect(
        f"ws://127.0.0.1:{port}{PART_PATHS[ResourceKind.WORLD]}"
      ) as ws:
        assert json.loads(await asyncio.wait_for(ws.recv(), timeout=2)) == {
          "error": "unavailable"
        }
        await ws.recv()
    assert exc.value.rcvd is not None
    assert exc.value.rcvd.code == 1013
  finally:
    await server.stop()
