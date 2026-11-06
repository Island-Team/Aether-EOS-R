"""EOS world and detection ingest through typed v3 resource streams."""

from __future__ import annotations

import asyncio
import json

import pytest

from egia.contracts.endpoints import ResourceKind
from egia.contracts.perception.world.detection import DetectionSnapshot
from egia.contracts.perception.world.state import WorldStateSnapshot
from egia.core.link import EosClient, LinkPrimitiveError
from egia.core.perception.admission import Admission
from egia.core.perception.body import parts_perception_entry
from egia.io import PartStore
from egia.lib.config import GatewayConfig
from tests.egia.stub.gateway import StubGateway, start_stub_gateway, stop_stub_gateway


def _client(stub: StubGateway) -> EosClient:
  return EosClient(GatewayConfig(host=stub.host, grpc_port=stub.port))


@pytest.mark.asyncio
async def test_world_subscription_pushes_valid_snapshots() -> None:
  """Watch notifications trigger a schema-validated world-state reread."""
  stub = await start_stub_gateway()
  client = _client(stub)
  try:
    await client.initialize()
    world = next(item for item in client.resources if item.kind is ResourceKind.WORLD)
    stream = client.stream_resource(world.uri, kind=ResourceKind.WORLD)
    first = await anext(stream)
    assert isinstance(first, WorldStateSnapshot)
    stub.gateway._payloads[world.uri]["sequence"] = first.sequence + 1
    await stub.gateway.notify(world.uri)
    second = await asyncio.wait_for(anext(stream), timeout=2)
    assert second.sequence == first.sequence + 1
    await stream.aclose()
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_resource_notifications_are_silent_without_subscribers() -> None:
  stub = await start_stub_gateway()
  try:
    before = stub.world.sequence
    await asyncio.sleep(0.15)
    assert stub.world.sequence == before
  finally:
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_read_resource_on_undiscovered_route_signals_absent() -> None:
  """An undiscovered resource route fails with a stable link error."""
  stub = await start_stub_gateway()
  client = _client(stub)
  try:
    await client.initialize()
    with pytest.raises(LinkPrimitiveError, match="unknown resource"):
      await client.read_resource("map/occupancy", kind=ResourceKind.WORLD)
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_detection_resource_admits_typed_coalesced_observations(
  stub_gateway: StubGateway,
) -> None:
  """Detection updates admit one coalesced semantic record per model result."""
  stores = {ResourceKind.DETECTION: PartStore()}
  queue = Admission(4)
  ready, stop = asyncio.Event(), asyncio.Event()
  client = _client(stub_gateway)
  await client.initialize()
  task = asyncio.create_task(
    parts_perception_entry(
      client,
      client.resources,
      stores,
      queue,
      ready,
      stop,
    )
  )
  await asyncio.wait_for(ready.wait(), timeout=2)
  try:
    for _ in range(50):
      if queue.latest_by_kind("detection") is not None:
        break
      await asyncio.sleep(0.05)
    record = queue.latest_by_kind("detection")
    assert record is not None
    assert record.source == "detection.default.default"
    count = int(record.metadata["detection_count"])
    rendered = json.loads(record.text)
    assert isinstance(rendered, list)
    assert len(rendered) == count == 1
    assert rendered[0].startswith("obj:box-1 box")
    assert set(record.metadata) >= {
      "detection_count",
      "source_sequence",
      "source_timestamp",
    }
    snapshot = stores[ResourceKind.DETECTION].latest()
    assert isinstance(snapshot, DetectionSnapshot)
    assert snapshot.source_sequence == 1
    assert snapshot.detections[0].id == "obj:box-1"
    assert queue.size == 1
    assert len(queue.history(kinds={"detection"})) == 1
  finally:
    stop.set()
    await asyncio.wait_for(task, timeout=2)
