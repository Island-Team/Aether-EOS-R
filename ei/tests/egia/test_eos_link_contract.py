"""Focused coverage for the current EOS northbound client contract."""

from __future__ import annotations

import re
from pathlib import Path

import pytest
from google.protobuf import json_format, struct_pb2

from egia.contracts.actions import TerminalStatus
from egia.contracts.endpoints import ResourceKind
from egia.contracts.limits import NumericRange
from egia.contracts.registry import ACTION_CONTRACTS, RESOURCE_CONTRACTS
from egia.core.link import EosClient, LinkPrimitiveError
from egia.core.registry import PrimitiveRegistry
from egia.lib.config import GatewayConfig
from island.generated.eos import capabilities_pb2, gateway_pb2, skills_pb2
from tests.egia.stub.gateway import start_stub_gateway, stop_stub_gateway


def _client(stub) -> EosClient:
  return EosClient(
    GatewayConfig(host=stub.host, grpc_port=stub.port), backend="robot-a"
  )


def test_contract_registry_uses_explicit_paths_and_exact_versions() -> None:
  """Every admitted EOS contract selects its canonical file explicitly."""
  assert RESOURCE_CONTRACTS[ResourceKind.WORLD].schema_path.endswith("world.v1.json")
  assert RESOURCE_CONTRACTS[ResourceKind.ASR].version == "1.0"
  assert (
    ACTION_CONTRACTS[
      next(kind for kind in ACTION_CONTRACTS if kind.value == "arm.movej")
    ].version
    == "1.0.0"
  )
  assert ACTION_CONTRACTS[
    next(kind for kind in ACTION_CONTRACTS if kind.value == "speaker.play")
  ].schema_path.endswith("speaker_play.v1.json")
  methods = gateway_pb2.DESCRIPTOR.services_by_name["GatewayService"].methods_by_name
  assert "ListPrimitives" not in methods
  assert "CancelPrimitive" not in methods
  capability_methods = capabilities_pb2.DESCRIPTOR.services_by_name[
    "CapabilityService"
  ].methods_by_name
  assert {
    "ListCapabilities",
    "SubmitOperation",
    "WatchOperation",
    "CancelOperation",
  } <= set(capability_methods)
  skill_methods = skills_pb2.DESCRIPTOR.services_by_name["SkillService"].methods_by_name
  assert {"ListSkills", "StartExecution", "WatchExecution"} <= set(skill_methods)


def test_discovery_limits_accept_protojson_exclusive_field_names() -> None:
  interval = NumericRange.model_validate(
    {"minimum": 0.0, "minimumExclusive": True, "maximum": 1.0}
  )

  assert interval.minimum_exclusive is True
  assert interval.contains(0.0) is False


def test_capability_binding_keeps_robot_identity_and_rejects_other_robots() -> None:
  """Catalog aliases cannot deterministically select another robot's target."""
  response = capabilities_pb2.ListCapabilitiesResponse()
  response.capabilities.add(
    name="runtime/robot-a/robot.stop@1.0.0",
    primitive="robot.stop",
    version="1.0.0",
  )
  response.capabilities.add(
    name="runtime/robot-b/robot.stop@1.0.0",
    primitive="robot.stop",
    version="1.0.0",
  )

  with pytest.raises(LinkPrimitiveError, match="another robot"):
    EosClient._parse_capabilities(response, "robot-a")

  response.capabilities.pop()
  assert EosClient._parse_capabilities(response, "robot-a") == {
    ("robot-a", "robot.stop", "1.0.0", ""): (
      "runtime/robot-a/robot.stop@1.0.0",
      False,
    )
  }


@pytest.mark.parametrize(
  ("name", "device_id"),
  [
    ("hardware/robot-a/arm/arm-1/arm.movej@1.0.0", ""),
    ("runtime/robot-a/robot.stop@1.0.0", "unexpected-device"),
  ],
)
def test_capability_name_scope_must_match_device_identity(
  name: str, device_id: str
) -> None:
  response = capabilities_pb2.ListCapabilitiesResponse()
  response.capabilities.add(
    name=name,
    primitive=name.rsplit("/", 1)[-1].split("@", 1)[0],
    version="1.0.0",
    device_id=device_id,
  )

  with pytest.raises(LinkPrimitiveError, match="malformed name"):
    EosClient._parse_capabilities(response, "robot-a")


def test_eos_operation_wire_examples_match_in_english_and_chinese() -> None:
  """Both operator guides must show the same request and operation payloads."""
  root = Path(__file__).resolve().parents[2]
  english = (root / "../docs/ei/interfaces/eos-v2.md").read_text()
  chinese = (root / "../docs_cn/ei/interfaces/eos-v2.md").read_text()
  extract = lambda document: re.findall(
    r"\x60\x60\x60json\n(.*?)\n\x60\x60\x60", document, flags=re.DOTALL
  )

  assert len(extract(english)) == 4
  assert extract(english) == extract(chinese)


def test_successful_operation_cannot_report_admission_false() -> None:
  operation = capabilities_pb2.Operation(
    operation_id="operation-1",
    status=capabilities_pb2.OPERATION_STATUS_SUCCEEDED,
    accepted=False,
  )
  acknowledgement = struct_pb2.Struct()
  json_format.ParseDict(
    {
      "type": "primitive.ack",
      "primitive": "speaker.play",
      "primitive_version": "1.0.0",
      "accepted": False,
      "reason": "test rejection",
      "reason_code": "rejected.invalid_state",
      "command_id": "operation-1",
      "sequence": 1,
      "result": {"status": "succeeded"},
    },
    acknowledgement,
  )
  operation.acknowledgement.CopyFrom(acknowledgement)

  with pytest.raises(LinkPrimitiveError, match="successful operation was not admitted"):
    EosClient._validated_operation_ack(operation, "operation-1")


@pytest.mark.asyncio
async def test_discover_is_atomic_and_inline_bindings_gate_execution() -> None:
  """Discover plus ListResources forms one health-filtered generation."""
  stub = await start_stub_gateway()
  client = _client(stub)
  try:
    discovery = await client.initialize()
    assert discovery.started_at == stub.gateway.started_at
    assert {item.kind for item in client.resources} == {
      ResourceKind.WORLD,
      ResourceKind.DETECTION,
      ResourceKind.ASR,
    }
    assert {item.name for item in discovery.robot_primitives} == {
      "robot.stop",
      "safety.estop",
    }
    assert any(item.name == "motion.stop" for item in client.primitives)

    registry = PrimitiveRegistry.from_primitives("robot-a", client.primitives)
    assert registry.resolve("base.navigate", "robot-a", "base-0") is not None

    stub.gateway.device_health["base-0"] = (False, False)
    await client.initialize(force=True)
    registry.replace_primitives("robot-a", client.primitives)
    assert registry.resolve("base.navigate", "robot-a", "base-0") is None
    assert any(
      item.name == "base.navigate" and item.health == "unavailable"
      for item in client.primitives
    )
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_terminal_command_identity_cancellation_and_capture_v2() -> None:
  """Command IDs stay in the envelope and terminal/capture data is validated."""
  stub = await start_stub_gateway()
  client = _client(stub)
  try:
    await client.initialize()
    with pytest.raises(LinkPrimitiveError, match="arguments.command_id"):
      await client.call_primitive(
        "base.velocity",
        "base-0",
        {
          "command_id": "caller-owned",
          "linear_x_m_s": 0.1,
          "linear_y_m_s": 0.0,
          "angular_z_rad_s": 0.0,
          "valid_for_ms": 100,
        },
      )

    ack = await client.call_primitive(
      "base.velocity",
      "base-0",
      {
        "linear_x_m_s": 0.1,
        "linear_y_m_s": 0.0,
        "angular_z_rad_s": 0.0,
        "valid_for_ms": 100,
      },
    )
    assert ack.command_id
    assert ack.status is TerminalStatus.SUCCEEDED
    assert stub.gateway.calls[-1][2].get("command_id") is None

    cancelled = await client.cancel_primitive(ack.command_id)
    assert cancelled.status is TerminalStatus.CANCELLED
    frames = await client.capture_frames("camera-0", 2)
    assert [item.captured_at for item in frames] == [
      "2026-09-20T09:30:00Z",
      "2026-09-20T09:30:01Z",
    ]
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)
