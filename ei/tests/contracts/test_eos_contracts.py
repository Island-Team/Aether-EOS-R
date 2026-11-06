"""Pins for the current EOS northbound contract snapshot."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from egia.contracts.actions import TerminalStatus
from egia.contracts.endpoints import PrimitiveKind, ResourceKind
from egia.contracts.registry import ACTION_CONTRACTS, RESOURCE_CONTRACTS
from egia.contracts.validate import validate_primitive, validate_primitive_ack
from island.generated.eos import capabilities_pb2, gateway_pb2, skills_pb2
from island.generated.eos.robot import descriptor_pb2
from island.share import repository_resources

ROOT = Path(__file__).resolve().parents[2]
RESOURCES = repository_resources(ROOT)
PRIMITIVES = RESOURCES.directory("eos/schemas/eos/actions")


def test_gateway_capability_and_skill_services_have_canonical_rpcs() -> None:
  service = gateway_pb2.DESCRIPTOR.services_by_name["GatewayService"]
  names = {method.name for method in service.methods}
  assert {
    "Discover",
    "GetStatus",
    "ListResources",
    "ReadResource",
    "WatchResource",
    "CaptureFrames",
  } <= names
  assert "primitives" in descriptor_pb2.RobotDescriptor.DESCRIPTOR.fields_by_name
  assert "CallPrimitive" not in service.methods_by_name
  assert "CancelPrimitive" not in service.methods_by_name
  capability_service = capabilities_pb2.DESCRIPTOR.services_by_name["CapabilityService"]
  assert [method.name for method in capability_service.methods] == [
    "ListCapabilities",
    "SubmitOperation",
    "GetOperation",
    "WatchOperation",
    "CancelOperation",
  ]
  skill_service = skills_pb2.DESCRIPTOR.services_by_name["SkillService"]
  assert {
    "ListSkills",
    "GetSkill",
    "StartExecution",
    "GetExecution",
    "WatchExecution",
    "TerminateExecution",
  } <= {method.name for method in skill_service.methods}


def test_contract_inventory_matches_typed_registries() -> None:
  files = {path.name for path in PRIMITIVES.glob("*.json")}
  expected = {
    Path(contract.schema_path).name for contract in ACTION_CONTRACTS.values()
  } | {"ack.v1.json"}
  assert expected <= files
  assert set(ACTION_CONTRACTS) == set(PrimitiveKind)
  assert set(RESOURCE_CONTRACTS) == set(ResourceKind)
  assert {kind.value for kind in PrimitiveKind} == {
    "arm.init",
    "arm.movej",
    "arm.movel",
    "arm.pick",
    "base.navigate",
    "base.posture",
    "base.velocity",
    "face.register",
    "gimbal.motion",
    "gripper.motion",
    "motion.stop",
    "robot.stop",
    "safety.estop",
    "speaker.play",
    "speaker.speak",
  }


def test_action_models_publish_exact_canonical_schemas_and_versions() -> None:
  for contract in ACTION_CONTRACTS.values():
    schema = json.loads(
      RESOURCES.require(contract.schema_path).read_text(encoding="utf-8")
    )
    assert contract.model.model_json_schema() == schema
    assert schema["x-island-primitive-version"] == contract.version


@pytest.mark.parametrize(
  ("kind", "arguments"),
  [
    (PrimitiveKind.ARM_INIT, {}),
    (
      PrimitiveKind.ARM_MOVEJ,
      {"reference": "absolute", "joint_positions_rad": [0.0]},
    ),
    (
      PrimitiveKind.ARM_MOVEL,
      {
        "reference": "relative",
        "position_m": [0.1, 0.0, 0.0],
        "orientation_quat_xyzw": [0.0, 0.0, 0.0, 1.0],
      },
    ),
    (PrimitiveKind.ARM_PICK, {"object_id": "obj:part-7"}),
    (
      PrimitiveKind.BASE_NAVIGATE,
      {"reference": "relative", "x_m": 1.0, "y_m": 0.0, "yaw_rad": 0.0},
    ),
    (PrimitiveKind.BASE_POSTURE, {"posture": "stand"}),
    (
      PrimitiveKind.BASE_VELOCITY,
      {
        "linear_x_m_s": 0.1,
        "linear_y_m_s": 0.0,
        "angular_z_rad_s": 0.0,
        "valid_for_ms": 100,
      },
    ),
    (PrimitiveKind.FACE_REGISTER, {"name": "Person A"}),
    (
      PrimitiveKind.GIMBAL_MOTION,
      {"reference": "relative", "pan_rad": 0.1, "tilt_rad": 0.0},
    ),
    (PrimitiveKind.GRIPPER_MOTION, {"action": "open"}),
    (PrimitiveKind.SPEAKER_PLAY, {"uri": "asset://alerts/ready.wav"}),
    (PrimitiveKind.SPEAKER_SPEAK, {"text": "Ready."}),
    (PrimitiveKind.MOTION_STOP, {}),
    (PrimitiveKind.ROBOT_STOP, {}),
    (PrimitiveKind.SAFETY_ESTOP, {}),
  ],
)
def test_current_primitive_payloads_are_accepted(kind, arguments) -> None:
  validate_primitive(kind, arguments)


@pytest.mark.parametrize(
  ("kind", "arguments"),
  [
    (PrimitiveKind.BASE_NAVIGATE, {"x_m": 1, "y_m": 0, "yaw_rad": 0}),
    (
      PrimitiveKind.BASE_NAVIGATE,
      {
        "reference": "absolute",
        "x_m": 1,
        "y_m": 0,
        "yaw_rad": 0,
      },
    ),
    (PrimitiveKind.GIMBAL_MOTION, {"pan": 0.5, "tilt": 0.5}),
    (PrimitiveKind.GRIPPER_MOTION, {"action": "close", "force": 1}),
    (PrimitiveKind.ARM_PICK, {"object_id": "human:7"}),
    (PrimitiveKind.SPEAKER_PLAY, {"text": "legacy speech"}),
    (PrimitiveKind.ARM_INIT, {"command_id": "caller-owned"}),
  ],
)
def test_invalid_and_legacy_payloads_are_rejected(kind, arguments) -> None:
  with pytest.raises(ValueError):
    validate_primitive(kind, arguments)


def test_terminal_ack_requires_identity_result_and_rejection_taxonomy() -> None:
  accepted = {
    "type": "primitive.ack",
    "primitive_version": "1.0.0",
    "primitive": "base.velocity",
    "accepted": True,
    "reason": None,
    "reason_code": None,
    "command_id": "command-1",
    "sequence": 1,
    "result": {"status": "succeeded"},
  }
  assert validate_primitive_ack(accepted).status is TerminalStatus.SUCCEEDED
  failed = {**accepted, "result": {"status": "failed", "reason": "driver"}}
  assert validate_primitive_ack(failed).status is TerminalStatus.FAILED
  with pytest.raises(ValueError):
    validate_primitive_ack({**accepted, "accepted": False, "reason": "busy"})
