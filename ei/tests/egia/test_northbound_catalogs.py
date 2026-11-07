"""Primitive and native-skill northbound catalog streams."""

from __future__ import annotations

import asyncio
import json

import pytest
import websockets

from egia.api.primitives import PrimitiveDescriptor, PrimitivesSnapshot
from egia.api.skills import SkillDescriptor, SkillsSnapshot
from egia.api.status import HealthState
from egia.contracts.limits import NumericRange
from egia.core.perception.admission import Admission
from egia.core.registry import Primitive, PrimitiveRegistry
from egia.io import PRIMITIVES_PATH, SKILLS_PATH, NorthboundServer
from egia.io.catalog import primitives_snapshot
from egia.io.state import ServerState
from egia.io.store import PrimitivesStore, SkillsStore


async def _server(
  primitives: PrimitivesStore | None = None,
  skills: SkillsStore | None = None,
  *,
  ready: bool = True,
) -> NorthboundServer:
  state = ServerState("test")
  if ready:
    state.status = HealthState.READY
  server = NorthboundServer(
    state,
    Admission(4),
    host="127.0.0.1",
    port=0,
    primitives_store=primitives,
    skills_store=skills,
  )
  await server.start()
  return server


def test_catalog_projection_preserves_binding_metadata() -> None:
  registry = PrimitiveRegistry(
    [
      Primitive(
        kind=None,
        name="custom.inspect",
        uri="custom.inspect",
        backend="robot-a",
        device_id="camera-0",
        eos_robot_id="01a077f7-b5b8-7b4e-8057-ca688b8ff48f",
        version="3.1.4",
        description="Inspect a target",
        device_description="wrist camera",
        scope="device",
        binding_kind="direct",
        health="ready",
        references=("relative",),
        absolute_frame="map",
        dependencies=(("localization", "ready"),),
        accepts=("image/jpeg",),
        limits={"distance_m": NumericRange(minimum=0.1, maximum=2.0)},
      )
    ]
  )
  descriptor = primitives_snapshot(registry).primitives[0]
  assert descriptor.version == "3.1.4"
  assert descriptor.scope == "device"
  assert descriptor.binding_kind == "direct"
  assert descriptor.health == "ready"
  assert descriptor.references == ["relative"]
  assert descriptor.absolute_frame == "map"
  assert descriptor.dependencies[0].model_dump() == {
    "name": "localization",
    "health": "ready",
  }
  assert descriptor.accepted_media_types == ["image/jpeg"]
  assert descriptor.limits["distance_m"].maximum == 2.0


@pytest.mark.asyncio
async def test_primitives_catalog_exposes_typed_argument_schema() -> None:
  store = PrimitivesStore()
  await store.publish(
    PrimitivesSnapshot(
      primitives=[
        PrimitiveDescriptor(
          name="base.velocity",
          description="Move the base",
          device_description="warehouse tug",
          version="1.0.0",
          robot_id="robot-a",
          device_id="base-0",
          eos_robot_id="01a077f7-b5b8-7b4e-8057-ca688b8ff48f",
          scope="device",
          binding_kind="direct",
          health="ready",
          references=["relative", "absolute"],
          absolute_frame="map",
          dependencies=[],
          accepted_media_types=["application/json"],
          arguments_schema={
            "type": "object",
            "properties": {"linear_x_m_s": {"type": "number"}},
          },
        )
      ],
      count=1,
    )
  )
  server = await _server(primitives=store)
  try:
    _, port = server.address
    async with websockets.connect(f"ws://127.0.0.1:{port}{PRIMITIVES_PATH}") as ws:
      frame = json.loads(await asyncio.wait_for(ws.recv(), 2))
      assert frame["count"] == 1
      primitive = frame["primitives"][0]
      assert primitive["name"] == "base.velocity"
      assert primitive["robot_id"] == "robot-a"
      assert primitive["device_id"] == "base-0"
      assert primitive["description"] == "Move the base"
      assert primitive["device_description"] == "warehouse tug"
      assert primitive["scope"] == "device"
      assert primitive["binding_kind"] == "direct"
      assert primitive["health"] == "ready"
      assert primitive["references"] == ["relative", "absolute"]
      assert primitive["absolute_frame"] == "map"
      assert primitive["dependencies"] == []
      assert primitive["accepted_media_types"] == ["application/json"]
      assert primitive["arguments_schema"]["properties"]["linear_x_m_s"] == {
        "type": "number"
      }
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_skills_catalog_exposes_semantics_and_availability() -> None:
  store = SkillsStore()
  await store.publish(
    SkillsSnapshot(
      available=True,
      skills=[
        SkillDescriptor(
          backend="robot-a",
          eos_robot_id="01a077f7-b5b8-7b4e-8057-ca688b8ff48f",
          name="approach",
          version="1.2.0",
          description="Approach a visible target",
          primitives=["base.velocity", "base.posture"],
        )
      ],
      count=1,
    )
  )
  server = await _server(skills=store)
  try:
    _, port = server.address
    async with websockets.connect(f"ws://127.0.0.1:{port}{SKILLS_PATH}") as ws:
      frame = json.loads(await asyncio.wait_for(ws.recv(), 2))
      assert frame["available"] is True
      assert frame["unavailable_backends"] == []
      assert frame["skills"][0]["description"] == "Approach a visible target"
      assert frame["skills"][0]["primitives"] == [
        "base.velocity",
        "base.posture",
      ]
  finally:
    await server.stop()


@pytest.mark.asyncio
@pytest.mark.parametrize("path", [PRIMITIVES_PATH, SKILLS_PATH])
async def test_catalog_routes_are_unavailable_without_a_published_store(
  path: str,
) -> None:
  server = await _server()
  try:
    _, port = server.address
    async with websockets.connect(f"ws://127.0.0.1:{port}{path}") as ws:
      assert json.loads(await asyncio.wait_for(ws.recv(), 2)) == {
        "error": "unavailable"
      }
      with pytest.raises(websockets.exceptions.ConnectionClosed) as closed:
        await asyncio.wait_for(ws.recv(), 2)
      assert closed.value.rcvd is not None
      assert closed.value.rcvd.code == 1013
  finally:
    await server.stop()


@pytest.mark.asyncio
@pytest.mark.parametrize(
  "path",
  [
    "/api/aether/egia/tools",
    "/api/aether/egia/action/invoke",
    "/api/aether/egia/perception/world/raster_map",
  ],
)
async def test_removed_routes_have_no_compatibility_alias(path: str) -> None:
  server = await _server()
  try:
    _, port = server.address
    async with websockets.connect(f"ws://127.0.0.1:{port}{path}") as ws:
      with pytest.raises(websockets.exceptions.ConnectionClosed) as closed:
        await asyncio.wait_for(ws.recv(), 2)
      assert closed.value.rcvd is not None
      assert closed.value.rcvd.code == 1000
  finally:
    await server.stop()
