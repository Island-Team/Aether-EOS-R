"""World resources forwarded into northbound stores and admission."""

from __future__ import annotations

import asyncio
import json

import pytest

from egia.contracts.endpoints import ResourceKind
from egia.contracts.perception.world.detection import Detection, DetectionSnapshot
from egia.contracts.perception.world.state import WorldStateSnapshot
from egia.core.link import EosClient
from egia.core.perception.admission import Admission
from egia.core.perception.body import (
  DETECTION_KIND,
  _admit_detections,
  parts_perception_entry,
)
from egia.io import PartStore
from egia.lib.config import GatewayConfig
from tests.egia.stub.gateway import StubGateway


def _client(stub: StubGateway, *, backend: str = "default") -> EosClient:
  return EosClient(GatewayConfig(host=stub.host, grpc_port=stub.port), backend=backend)


async def _run_parts(
  stub: StubGateway,
  stores: dict[ResourceKind, PartStore],
  *,
  queue: Admission | None = None,
  backend: str = "default",
) -> tuple[asyncio.Event, asyncio.Task[None]]:
  ready = asyncio.Event()
  stop = asyncio.Event()
  client = _client(stub, backend=backend)
  await client.initialize()
  task = asyncio.create_task(
    parts_perception_entry(
      client,
      client.resources,
      stores,
      queue or Admission(4),
      ready,
      stop,
      backend=backend,
    )
  )
  await asyncio.wait_for(ready.wait(), timeout=2)
  return stop, task


@pytest.mark.asyncio
async def test_entry_publishes_world_detection_and_asr(
  stub_gateway: StubGateway,
) -> None:
  stores = {kind: PartStore() for kind in ResourceKind}
  queue = Admission(8)
  stop, task = await _run_parts(stub_gateway, stores, queue=queue)
  try:
    for _ in range(50):
      if all(store.latest() is not None for store in stores.values()):
        break
      await asyncio.sleep(0.05)
    assert isinstance(stores[ResourceKind.WORLD].latest(), WorldStateSnapshot)
    assert isinstance(stores[ResourceKind.DETECTION].latest(), DetectionSnapshot)
    assert queue.latest_by_kind("objects") is not None
    assert queue.latest_by_kind(DETECTION_KIND) is not None
    assert queue.latest_by_kind("hearing") is not None
  finally:
    stop.set()
    await asyncio.wait_for(task, timeout=2)


@pytest.mark.asyncio
async def test_detection_records_preserve_backend_and_source_sequence() -> None:
  queue = Admission(2)
  snapshot = DetectionSnapshot(
    sequence=8,
    timestamp="2026-09-20T09:30:00Z",
    source="camera-0",
    source_sequence=7,
    detections=[
      Detection(
        id="obj:crate-1",
        kind="object",
        label="crate; priority",
        confidence=1.0,
        distance_m=1.2,
        bearing_rad=0.0,
        attributes={"pickable": True},
      )
    ],
  )
  assert await _admit_detections(queue, snapshot, "cell-a", "camera-0") is True
  record = queue.latest_by_kind(DETECTION_KIND)
  assert record is not None
  assert record.source == "detection.cell-a.camera-0"
  assert record.metadata["source_sequence"] == "7"
  assert json.loads(record.text)[0].startswith("obj:crate-1")
