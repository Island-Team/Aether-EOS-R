"""Native EOS v3 transport, discovery, resources, skills, and commands."""

from __future__ import annotations

import asyncio

import grpc
import pytest
from grpc_health.v1 import health_pb2

from egia.contracts.actions import TerminalStatus
from egia.contracts.endpoints import ResourceKind
from egia.core.link import EosClient, LinkPrimitiveError
from egia.core.registry import PrimitiveRegistry
from egia.lib.config import GatewayConfig
from tests.egia.stub.gateway import start_stub_gateway, stop_stub_gateway


def _client(stub, alias: str = "robot-a") -> EosClient:
  return EosClient(GatewayConfig(host=stub.host, grpc_port=stub.port), backend=alias)


def _speaker_arguments() -> dict[str, object]:
  return {"uri": "asset://alerts/ready.wav"}


@pytest.mark.asyncio
async def test_initialization_uses_compact_body_and_inline_bindings() -> None:
  stub = await start_stub_gateway()
  client = _client(stub)
  try:
    discovery = await client.initialize()
    assert client.robot_alias == "robot-a"
    assert discovery.robot_id == stub.gateway.robot_id
    assert discovery.robot_name == "demo-0"
    assert (await client.get_status())["robot_id"] == discovery.robot_id
    assert discovery.robot_model == "stub"
    assert discovery.robot_description == "Test robot"
    assert discovery.started_at == stub.gateway.started_at
    assert {item.name for item in discovery.robot_primitives} == {
      "robot.stop",
      "safety.estop",
    }
    assert {item.kind for item in client.resources} == {
      ResourceKind.WORLD,
      ResourceKind.DETECTION,
      ResourceKind.ASR,
    }
    navigation = next(
      item for item in client.primitives if item.name == "base.navigate"
    )
    assert navigation.scope == "device"
    assert navigation.kind == "direct"
    assert navigation.health == "ready"
    assert navigation.references == ("relative", "absolute")
    assert navigation.absolute_frame == "map"
    assert navigation.limits["x_m"].maximum == 2.0
    assert all(discovery.robot_name in item.uri for item in client.resources)
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_skill_execution_is_idempotent_and_watchable() -> None:
  stub = await start_stub_gateway()
  stub.gateway.skill_definitions[("inspect.area", "2.1.0")] = {
    "description": "Inspect an area",
    "parameters": {
      "type": "object",
      "properties": {"radius_m": {"type": "number"}},
      "required": ["radius_m"],
      "additionalProperties": False,
    },
    "returns": {"type": "object"},
    "primitives": [("speaker.play", "1.0.0")],
  }
  client = _client(stub)
  try:
    await client.initialize()
    started = await client.start_skill(
      "inspect.area", "2.1.0", {"radius_m": 2}, request_id="request-1", timeout_s=10
    )
    duplicate = await client.start_skill(
      "inspect.area", "2.1.0", {"radius_m": 2}, request_id="request-1", timeout_s=10
    )
    assert duplicate.execution_id == started.execution_id
    snapshots = [
      item
      async for item in client.watch_skill_execution(
        started.execution_id, after_sequence=started.sequence
      )
    ]
    assert snapshots[-1].status == "succeeded"
    assert snapshots[-1].terminal
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_non_serving_and_malformed_mandatory_calls_fail_atomically() -> None:
  unhealthy = await start_stub_gateway(
    config={"health_status": health_pb2.HealthCheckResponse.NOT_SERVING}
  )
  malformed = await start_stub_gateway(config={"malformed": {"ListResources"}})
  try:
    with pytest.raises(LinkPrimitiveError, match="not SERVING"):
      await _client(unhealthy).initialize()
    with pytest.raises(LinkPrimitiveError, match="malformed"):
      await _client(malformed).initialize()
  finally:
    await stop_stub_gateway(unhealthy)
    await stop_stub_gateway(malformed)


@pytest.mark.asyncio
@pytest.mark.parametrize("malformed", ["Discover", "PrimitiveDescription", "GetStatus"])
async def test_malformed_body_generation_is_not_published(malformed: str) -> None:
  stub = await start_stub_gateway(config={"malformed": {malformed}})
  client = _client(stub)
  try:
    with pytest.raises(LinkPrimitiveError):
      await client.initialize()
    with pytest.raises(RuntimeError, match="not been initialized"):
      _ = client.discovery
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_unavailable_bindings_remain_diagnostic_but_not_executable() -> None:
  stub = await start_stub_gateway()
  client = _client(stub)
  try:
    stub.gateway.device_health["base-0"] = (False, False)
    await client.initialize()
    base = [item for item in client.primitives if item.device_id == "base-0"]
    assert base and all(item.health == "unavailable" for item in base)
    registry = PrimitiveRegistry.from_primitives("robot-a", client.primitives)
    assert registry.resolve("base.navigate", "robot-a", "base-0") is None
    with pytest.raises(LinkPrimitiveError, match="unavailable"):
      await client.call_primitive(
        "base.velocity",
        "base-0",
        {
          "linear_x_m_s": 0.1,
          "linear_y_m_s": 0.0,
          "angular_z_rad_s": 0.0,
          "valid_for_ms": 250,
        },
      )
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
@pytest.mark.parametrize("field", ["x_m", "y_m", "yaw_rad"])
async def test_navigation_requires_every_advertised_numeric_limit(field: str) -> None:
  stub = await start_stub_gateway()
  client = _client(stub)
  stub.gateway.limit_omissions["base.navigate"] = {field}
  try:
    with pytest.raises(LinkPrimitiveError, match=rf"missing \['{field}'\]"):
      await client.initialize()
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_resource_read_is_schema_validated() -> None:
  stub = await start_stub_gateway()
  client = _client(stub)
  try:
    await client.initialize()
    detection = next(
      item for item in client.resources if item.kind is ResourceKind.DETECTION
    )
    snapshot = await client.read_resource(detection.uri, kind=ResourceKind.DETECTION)
    assert snapshot.type == "detection.snapshot"
    del stub.gateway._payloads[detection.uri]["timestamp"]
    with pytest.raises(ValueError, match="timestamp"):
      await client.read_resource(detection.uri, kind=ResourceKind.DETECTION)
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_watch_rereads_notifications_and_periodic_changes() -> None:
  stub = await start_stub_gateway()
  client = _client(stub)
  try:
    await client.initialize()
    world = next(item for item in client.resources if item.kind is ResourceKind.WORLD)
    stream = client.stream_resource(
      world.uri, kind=ResourceKind.WORLD, poll_interval_s=0.01
    )
    first = await anext(stream)
    stub.gateway._payloads[world.uri]["sequence"] = first.sequence + 1
    await stub.gateway.notify(world.uri)
    assert (await asyncio.wait_for(anext(stream), 2)).sequence == first.sequence + 1
    stub.gateway._payloads[world.uri]["sequence"] = first.sequence + 2
    assert (await asyncio.wait_for(anext(stream), 1)).sequence == first.sequence + 2
    await stream.aclose()
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_rediscovery_tracks_started_at_not_stable_robot_id() -> None:
  stub = await start_stub_gateway()
  client = _client(stub)
  try:
    first = await client.initialize()
    old_routes = {item.uri for item in client.resources}
    stub.gateway.restart(first.robot_id)
    second = await client.initialize(force=True)
    assert second.robot_id == first.robot_id
    assert second.started_at != first.started_at
    assert old_routes == {item.uri for item in client.resources}
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
@pytest.mark.parametrize(
  ("code", "status"),
  [
    (grpc.StatusCode.INVALID_ARGUMENT, 400),
    (grpc.StatusCode.NOT_FOUND, 404),
    (grpc.StatusCode.UNAVAILABLE, 503),
  ],
)
async def test_primitive_grpc_errors_map_to_domain_errors(code, status) -> None:
  stub = await start_stub_gateway()
  client = _client(stub)
  try:
    await client.initialize()
    stub.gateway.errors["SubmitOperation"] = (code, "configured failure")
    with pytest.raises(LinkPrimitiveError) as caught:
      await client.call_primitive(
        "base.velocity",
        "base-0",
        {
          "linear_x_m_s": 0.1,
          "linear_y_m_s": 0.0,
          "angular_z_rad_s": 0.0,
          "valid_for_ms": 250,
        },
      )
    assert caught.value.status == status
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_terminal_command_identity_and_targeting() -> None:
  stub = await start_stub_gateway()
  client = _client(stub)
  try:
    await client.initialize()
    ack = await client.call_primitive("speaker.play", "speaker-0", _speaker_arguments())
    assert ack.command_id
    assert ack.primitive_version == "1.0.0"
    assert ack.status is TerminalStatus.SUCCEEDED
    assert stub.gateway.calls[-1] == (
      "speaker.play",
      "speaker-0",
      _speaker_arguments(),
    )
    with pytest.raises(LinkPrimitiveError, match="arguments.command_id"):
      await client.call_primitive(
        "speaker.play",
        "speaker-0",
        {**_speaker_arguments(), "command_id": "caller-owned"},
      )
    with pytest.raises(LinkPrimitiveError, match="non-empty device_id"):
      await client.call_primitive("speaker.play", "", _speaker_arguments())
    with pytest.raises(LinkPrimitiveError, match="empty device_id"):
      await client.call_primitive("safety.estop", "base-0", {})
    with pytest.raises(LinkPrimitiveError, match="not discovered"):
      await client.call_primitive("motion.stop", "speaker-0", {})

    robot_stop = await client.call_primitive("robot.stop", "", {})
    assert robot_stop.status is TerminalStatus.SUCCEEDED
    assert robot_stop.command_id
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_rejected_primitive_has_terminal_status_and_rejection_reason() -> None:
  stub = await start_stub_gateway()
  stub.gateway.rejections.add("speaker.play")
  client = _client(stub)
  try:
    await client.initialize()
    ack = await client.call_primitive("speaker.play", "speaker-0", _speaker_arguments())
    assert not ack.accepted
    assert ack.status is TerminalStatus.REJECTED
    assert ack.reason == "test rejection"
    assert ack.reason_code == "rejected.invalid_state"
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_outcome_unknown_is_terminal_and_never_retried() -> None:
  stub = await start_stub_gateway()
  stub.gateway.outcomes["speaker.play"] = "outcome_unknown"
  client = _client(stub)
  try:
    await client.initialize()
    ack = await client.call_primitive("speaker.play", "speaker-0", _speaker_arguments())
    assert ack.status is TerminalStatus.OUTCOME_UNKNOWN
    assert len(stub.gateway.calls) == 1
    assert len(stub.gateway.operations) == 1
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_pending_operation_is_watched_with_the_same_identity() -> None:
  stub = await start_stub_gateway()
  stub.gateway.pending_operations.add("speaker.play")
  client = _client(stub)
  try:
    await client.initialize()
    ack = await client.call_primitive("speaker.play", "speaker-0", _speaker_arguments())
    assert ack.status is TerminalStatus.SUCCEEDED
    assert len(stub.gateway.calls) == 1
    assert len(stub.gateway.operations) == 1
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_lost_submit_response_is_not_retried() -> None:
  stub = await start_stub_gateway()
  stub.gateway.errors["SubmitOperation"] = (
    grpc.StatusCode.UNAVAILABLE,
    "submit response lost",
  )
  client = _client(stub)
  operation_id = "75a852c0-894a-4d93-b1b9-31fd642621cd"
  try:
    await client.initialize()
    with pytest.raises(LinkPrimitiveError):
      await client.call_primitive(
        "speaker.play",
        "speaker-0",
        _speaker_arguments(),
        command_id=operation_id,
      )
    assert stub.gateway.calls == []
    assert stub.gateway.operations == {}
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_numeric_limit_rejection_occurs_before_egress() -> None:
  stub = await start_stub_gateway()
  client = _client(stub)
  try:
    await client.initialize()
    with pytest.raises(LinkPrimitiveError, match="admitted device range"):
      await client.call_primitive(
        "base.navigate",
        "base-0",
        {
          "reference": "relative",
          "x_m": 2.1,
          "y_m": 0.0,
          "yaw_rad": 0.0,
        },
      )
    assert stub.gateway.calls == []
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_cancel_primitive_returns_command_correlated_terminal_ack() -> None:
  stub = await start_stub_gateway()
  client = _client(stub)
  try:
    await client.initialize()
    cancelled = await client.cancel_primitive("command-7")
    assert cancelled.command_id == "command-7"
    assert cancelled.status is TerminalStatus.CANCELLED
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_capture_frames_returns_validated_v2_frames() -> None:
  stub = await start_stub_gateway()
  client = _client(stub)
  try:
    await client.initialize()
    frames = await client.capture_frames("camera-0", 2)
    assert len(frames) == 2
    assert frames[0].jpeg.startswith(b"\xff\xd8")
    assert frames[0].captured_at == "2026-09-20T09:30:00Z"
    with pytest.raises(LinkPrimitiveError, match="one or two"):
      await client.capture_frames("camera-0", 3)
    with pytest.raises(LinkPrimitiveError, match="not discovered"):
      await client.capture_frames("base-0")
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_malformed_capture_frames_is_rejected() -> None:
  stub = await start_stub_gateway(config={"malformed": {"CaptureFrames"}})
  client = _client(stub)
  try:
    await client.initialize()
    with pytest.raises(LinkPrimitiveError, match="malformed frames"):
      await client.capture_frames("camera-0")
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)
