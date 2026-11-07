"""In-process EOS gRPC test double."""

from __future__ import annotations

import asyncio
from dataclasses import dataclass, field
from typing import Any, cast

import grpc
from google.protobuf import json_format, struct_pb2
from grpc_health.v1 import health_pb2, health_pb2_grpc

from island.generated.eos import (
  capabilities_pb2,
  capabilities_pb2_grpc,
  gateway_pb2_grpc,
  skills_pb2_grpc,
)
from island.generated.eos.perception import camera_pb2
from island.generated.eos.robot import descriptor_pb2, primitive_pb2
from island.generated.eos.skills import catalog_pb2, execution_pb2
from island.generated.eos.system import resource_pb2

ROBOT_ID = "01a077f7-b5b8-7b4e-8057-ca688b8ff48f"
ROBOT_NAME = "demo-0"

PRIMITIVE_LIMITS: dict[str, dict[str, dict[str, float | bool]]] = {
  "base.velocity": {
    "linear_x_m_s": {"minimum": -1.0, "maximum": 1.0},
    "linear_y_m_s": {"minimum": -1.0, "maximum": 1.0},
    "angular_z_rad_s": {"minimum": -1.0, "maximum": 1.0},
    "valid_for_ms": {"minimum": 1.0, "maximum": 1000.0},
  },
  "base.navigate": {
    "x_m": {"minimum": -2.0, "maximum": 2.0},
    "y_m": {"minimum": -1.0, "maximum": 1.0},
    "yaw_rad": {"minimum": -3.14, "maximum": 3.14},
  },
  "gimbal.motion": {
    "pan_rad": {"minimum": -3.14, "maximum": 3.14},
    "tilt_rad": {"minimum": -1.57, "maximum": 1.57},
    "speed_rad_s": {"minimum": 0.0, "minimum_exclusive": True},
  },
  "arm.init": {},
  "arm.movej": {
    "joint_positions_rad": {"minimum": -3.14, "maximum": 3.14},
    "speed_rad_s": {"minimum": 0.0, "minimum_exclusive": True},
  },
  "arm.movel": {
    "speed_m_s": {"minimum": 0.0, "minimum_exclusive": True},
  },
  "gripper.motion": {
    "force_n": {"minimum": 0.0, "maximum": 10.0, "minimum_exclusive": True},
  },
  "base.posture": {},
  "speaker.play": {},
  "motion.stop": {},
  "robot.stop": {},
  "safety.estop": {},
}

DEVICE_METADATA: dict[str, tuple[dict[str, Any], dict[str, Any]]] = {
  "base": (
    {
      "footprint_length_m": 0.7,
      "footprint_width_m": 0.48,
      "supported_modes": ["differential"],
    },
    {"controller_state": "idle", "motors_enabled": True},
  ),
  "manipulator": (
    {
      "joint_names": ["joint_1"],
      "joint_count": 1,
      "end_effector_type": "parallel_gripper",
      "mount_frame": "base_link",
    },
    {"controller_state": "idle", "calibrated": True},
  ),
  "gimbal": (
    {
      "pan_min_rad": -3.14,
      "pan_max_rad": 3.14,
      "tilt_min_rad": -1.57,
      "tilt_max_rad": 1.57,
      "fov_rad": 1.2,
      "range_m": 5.0,
    },
    {"enabled": True, "controller_state": "idle"},
  ),
  "gripper": (
    {
      "supported_actions": ["open", "close"],
      "supports_force_control": True,
      "supports_position_control": False,
    },
    {"jaw_state": "open"},
  ),
  "camera": (
    {
      "width_px": 640,
      "height_px": 480,
      "frame_rate_hz": 30.0,
      "pixel_format": "jpeg",
      "fov_rad": 1.2,
    },
    {"present": True, "capturing": True},
  ),
  "microphone": (
    {
      "sample_rate_hz": 16000,
      "channels": 1,
      "sample_format": "s16le",
      "window_duration_ms": 1000,
    },
    {"present": True, "capturing": True},
  ),
  "speaker": (
    {"supported_media_types": ["audio/wav"], "sample_rates_hz": [16000], "channels": 1},
    {"present": True, "playback_state": "idle", "buffered_duration_ms": 0},
  ),
}


def _struct(value: dict[str, Any]) -> struct_pb2.Struct:
  message = struct_pb2.Struct()
  message.update(value)
  return message


class _Health(health_pb2_grpc.HealthServicer):
  def __init__(self, status: int = health_pb2.HealthCheckResponse.SERVING) -> None:
    self.status = status

  async def Check(self, request, context):
    if request.service:
      await context.abort(grpc.StatusCode.NOT_FOUND, "service name unknown")
    return health_pb2.HealthCheckResponse(status=self.status)


@dataclass
class StubWorld:
  sequence: int = 1
  materials: dict[str, Any] = field(default_factory=dict)


class StubGatewayService(
  gateway_pb2_grpc.GatewayServiceServicer,
  capabilities_pb2_grpc.CapabilityServiceServicer,
  skills_pb2_grpc.SkillServiceServicer,
):
  def __init__(self, robot_id: str = ROBOT_ID) -> None:
    self.robot_id = robot_id
    self.world = StubWorld()
    self.calls: list[tuple[str, str, dict[str, Any]]] = []
    self.errors: dict[str, tuple[grpc.StatusCode, str]] = {}
    self.malformed: set[str] = set()
    self.limit_omissions: dict[str, set[str]] = {}
    self.rejections: set[str] = set()
    self.outcomes: dict[str, str] = {}
    self.skill_definitions: dict[tuple[str, str], dict[str, Any]] = {}
    self.skill_executions: dict[str, execution_pb2.SkillExecution] = {}
    self.operations: dict[str, capabilities_pb2.Operation] = {}
    self.pending_operations: set[str] = set()
    self.generation_metadata: dict[str, tuple[tuple[str, str], ...]] = {}
    self.expected_grant: str | None = None
    self.received_metadata: list[tuple[str, dict[str, str]]] = []
    self.operation_requests: list[tuple[str, str]] = []
    self.rpc_methods: list[str] = []
    self.lost_submit_once = False
    self.lost_watch_once = False
    self.device_health: dict[str, tuple[bool, bool]] = {}
    self.frames: list[tuple[bytes, int, int, str]] = [
      (b"\xff\xd8frame-one\xff\xd9", 640, 480, "2026-09-20T09:30:00Z"),
      (b"\xff\xd8frame-two\xff\xd9", 640, 480, "2026-09-20T09:30:01Z"),
    ]
    self.started_at = "2026-09-07T00:00:00Z"
    self._changes: dict[str, asyncio.Queue[str]] = {}
    self._payloads = self._make_payloads()

  async def _maybe_abort(self, method: str, context) -> None:
    error = self.errors.get(method)
    if error is not None:
      await context.abort(*error)

  async def _send_generation(self, method: str, context) -> None:
    self.rpc_methods.append(method)
    metadata = self.generation_metadata.get(method)
    if metadata is not None:
      await context.send_initial_metadata(metadata)

  async def _check_operation_auth(
    self, method: str, context, *, revision: bool = False
  ) -> None:
    metadata = dict(context.invocation_metadata())
    self.received_metadata.append((method, metadata))
    if self.expected_grant is None:
      return
    if metadata.get("authorization") != f"Bearer {self.expected_grant}":
      await context.abort(grpc.StatusCode.UNAUTHENTICATED, "missing EGIA grant")
    if revision and metadata.get("x-eos-active-revision") != dict(
      self.generation_metadata.get("Discover", ())
    ).get("eos-active-revision"):
      await context.abort(grpc.StatusCode.FAILED_PRECONDITION, "stale revision")

  def _make_payloads(self) -> dict[str, dict[str, Any]]:
    prefix = f"robots/{ROBOT_NAME}"
    return {
      f"{prefix}/world/state": {
        "type": "world.snapshot",
        "schema_version": "1.0",
        "sequence": 1,
        "timestamp": "2026-09-20T09:30:00Z",
        "frame_id": "map",
        "entities": [
          {"id": "obj:box-1", "label": "box", "position_m": [1.0, 0.0, 0.0]}
        ],
        "relations": [],
      },
      f"{prefix}/detection/state": {
        "type": "detection.snapshot",
        "schema_version": "1.0",
        "sequence": 1,
        "timestamp": "2026-09-20T09:30:00Z",
        "source": "camera-0",
        "source_sequence": 1,
        "detections": [
          {
            "id": "obj:box-1",
            "kind": "object",
            "label": "box",
            "distance_m": 1.0,
            "bearing_rad": 0.0,
            "confidence": 0.99,
            "attributes": {"pickable": True},
          }
        ],
      },
      f"{prefix}/world/asr/state": {
        "type": "asr.snapshot",
        "schema_version": "1.0",
        "sequence": 1,
        "timestamp": "2026-09-20T09:30:00Z",
        "text": "hello",
        "language": "en",
        "confidence": 0.98,
      },
    }

  async def Discover(self, request, context):
    del request
    await self._maybe_abort("Discover", context)
    await self._send_generation("Discover", context)
    devices = [
      (
        "base-0",
        "base",
        ["base.velocity", "base.navigate", "base.posture", "motion.stop"],
      ),
      ("gimbal-0", "gimbal", ["gimbal.motion", "motion.stop"]),
      ("arm-0", "manipulator", ["arm.init", "arm.movej", "arm.movel", "motion.stop"]),
      ("mic-0", "microphone", []),
      ("camera-0", "camera", []),
      ("gripper-0", "gripper", ["gripper.motion"]),
      ("speaker-0", "speaker", ["speaker.play"]),
    ]

    def binding(name: str, *, health: str = "ready"):
      version = {
        "base.navigate": "1.0.0",
        "arm.movej": "1.0.0",
        "arm.movel": "1.0.0",
        "gripper.motion": "1.0.0",
        "speaker.play": "1.0.0",
      }.get(name, "1.0.0")
      references = (
        ["relative", "absolute"]
        if name in {"base.navigate", "gimbal.motion", "arm.movej", "arm.movel"}
        else []
      )
      limits = {
        field: interval
        for field, interval in PRIMITIVE_LIMITS.get(name, {}).items()
        if field not in self.limit_omissions.get(name, set())
      }
      if "PrimitiveLimits" in self.malformed and name == "base.velocity":
        limits = {}
      return primitive_pb2.PrimitiveBinding(
        name=name,
        version=version,
        description="" if "PrimitiveDescription" in self.malformed else f"test {name}",
        kind="direct",
        health=health,
        references=references,
        absolute_frame=(
          "map"
          if name == "base.navigate"
          else "base_link"
          if name in {"gimbal.motion", "arm.movel"}
          else ""
        ),
        limits=_struct(limits),
        accepts=["audio/wav"] if name == "speaker.play" else [],
      )

    return descriptor_pb2.DiscoverResponse(
      robot=descriptor_pb2.RobotDescriptor(
        id="" if "Discover" in self.malformed else self.robot_id,
        name=ROBOT_NAME,
        model="stub",
        description="Test robot",
        state="idle",
        health="ready",
        estop=False,
        power_pct=100.0,
        primitives=[binding("robot.stop"), binding("safety.estop")],
      ),
      devices=[
        descriptor_pb2.DeviceDescriptor(
          id=device_id,
          part="default",
          state="idle",
          health=(
            "ready"
            if all(self.device_health.get(device_id, (True, True)))
            else "unavailable"
          ),
          description=f"Test {device_class}",
          primitives=[
            binding(
              name,
              health=(
                "ready"
                if all(self.device_health.get(device_id, (True, True)))
                else "unavailable"
              ),
            )
            for name in primitives
          ],
          **{"class": device_class},
        )
        for device_id, device_class, primitives in devices
      ],
    )

  async def GetStatus(self, request, context):
    del request
    await self._maybe_abort("GetStatus", context)
    await self._send_generation("GetStatus", context)
    if "GetStatus" in self.malformed:
      return _struct({"robot_id": self.robot_id, "status": "ready"})
    total = 7
    alive = sum(
      self.device_health.get(device_id, (True, True))[0]
      for device_id in (
        "base-0",
        "gimbal-0",
        "arm-0",
        "mic-0",
        "camera-0",
        "gripper-0",
        "speaker-0",
      )
    )
    state = "ready" if alive == total else "degraded"
    return _struct(
      {
        "robot_id": self.robot_id,
        "robot_name": ROBOT_NAME,
        "status": state,
        "version": "0.1.0",
        "started_at": self.started_at,
        "uptime_seconds": 1.0,
        "correlation_id": "test-correlation",
        "subsystems": [],
        "devices_total": total,
        "devices_alive": alive,
        "robots": [
          {
            "id": self.robot_id,
            "status": state,
            "devices_total": total,
            "devices_alive": alive,
          }
        ],
      }
    )

  async def ListResources(self, request, context):
    del request
    await self._maybe_abort("ListResources", context)
    if "ListResources" in self.malformed:
      values = struct_pb2.ListValue()
      values.extend(cast(Any, [{"uri": 42}]))
      return values
    values = struct_pb2.ListValue()
    values.extend(
      cast(
        Any,
        [
          {
            "class": (
              "detection"
              if "/detection/" in uri
              else "asr"
              if "/asr/" in uri
              else "world"
            ),
            "uri": uri,
          }
          for uri in self._payloads
        ],
      )
    )
    return values

  async def ReadResource(self, request, context):
    await self._maybe_abort("ReadResource", context)
    payload = self._payloads.get(request.uri)
    if payload is None:
      await context.abort(grpc.StatusCode.NOT_FOUND, "unknown resource")
    assert payload is not None
    return _struct(payload)

  async def WatchResource(self, request, context):
    await self._maybe_abort("WatchResource", context)
    if request.uri not in self._payloads:
      await context.abort(grpc.StatusCode.NOT_FOUND, "unknown resource")
    queue = self._changes.setdefault(request.uri, asyncio.Queue())
    while True:
      uri = await queue.get()
      yield resource_pb2.ResourceChanged(uri=uri)

  async def ListSkills(self, request, context):
    del request
    await self._maybe_abort("ListSkills", context)
    definitions = self.skill_definitions
    if "ListSkills" in self.malformed:
      definitions = {("", "1.0.0"): {}}
    skills = []
    for (name, version), definition in definitions.items():
      if "SkillDescriptor" in self.malformed:
        name = ""
      skills.append(
        catalog_pb2.SkillDescriptor(
          reference=catalog_pb2.SkillReference(name=name, version=version),
          description=definition.get("description", ""),
          parameters=_struct(definition.get("parameters", {})),
          returns=_struct(definition.get("returns", {})),
          primitives=[
            catalog_pb2.SkillPrimitiveReference(name=item[0], version=item[1])
            for item in definition.get("primitives", [])
          ],
          resources=[
            catalog_pb2.SkillResource(uri_template=item[0], description=item[1])
            for item in definition.get("resources", [])
          ],
        )
      )
    return catalog_pb2.ListSkillsResponse(skills=skills)

  async def StartExecution(self, request, context):
    await self._maybe_abort("StartExecution", context)
    existing = next(
      (
        item
        for item in self.skill_executions.values()
        if item.request_id == request.request_id
      ),
      None,
    )
    if existing is not None:
      return existing
    if (
      request.reference.name,
      request.reference.version,
    ) not in self.skill_definitions:
      await context.abort(grpc.StatusCode.NOT_FOUND, "unknown skill")
    execution_id = f"execution-{len(self.skill_executions) + 1}"
    execution = execution_pb2.SkillExecution(
      execution_id=execution_id,
      request_id=request.request_id,
      reference=request.reference,
      status=execution_pb2.EXECUTION_STATUS_RUNNING,
      sequence=1,
    )
    self.skill_executions[execution_id] = execution
    return execution

  async def GetExecution(self, request, context):
    await self._maybe_abort("GetExecution", context)
    execution = self.skill_executions.get(request.execution_id)
    if execution is None:
      await context.abort(grpc.StatusCode.NOT_FOUND, "unknown execution")
    return execution

  async def WatchExecution(self, request, context):
    await self._maybe_abort("WatchExecution", context)
    execution = self.skill_executions.get(request.execution_id)
    if execution is None:
      await context.abort(grpc.StatusCode.NOT_FOUND, "unknown execution")
    if (
      execution.sequence > request.after_sequence
      and execution.status != execution_pb2.EXECUTION_STATUS_RUNNING
    ):
      yield execution
      return
    terminal = execution_pb2.SkillExecution(
      execution_id=execution.execution_id,
      request_id=execution.request_id,
      reference=execution.reference,
      status=execution_pb2.EXECUTION_STATUS_SUCCEEDED,
      sequence=execution.sequence + 1,
      result=_struct({"completed": True}),
    )
    self.skill_executions[request.execution_id] = terminal
    yield terminal

  async def TerminateExecution(self, request, context):
    await self._maybe_abort("TerminateExecution", context)
    execution = self.skill_executions.get(request.execution_id)
    if execution is None:
      await context.abort(grpc.StatusCode.NOT_FOUND, "unknown execution")
    cancelled = execution_pb2.SkillExecution(
      execution_id=execution.execution_id,
      request_id=execution.request_id,
      reference=execution.reference,
      status=execution_pb2.EXECUTION_STATUS_CANCELLED,
      sequence=execution.sequence + 1,
      cancellation_requested=True,
    )
    self.skill_executions[request.execution_id] = cancelled
    return cancelled

  async def CallPrimitive(self, request, context):
    await self._maybe_abort("CallPrimitive", context)
    arguments = json_format.MessageToDict(request.arguments)
    self.calls.append((request.name, request.device_id, arguments))
    command_id = request.command_id
    rejected = request.name in self.rejections
    outcome = self.outcomes.get(request.name, "rejected" if rejected else "succeeded")
    return _struct(
      {
        "type": "primitive.ack",
        "primitive": (
          "base.posture" if "PrimitiveAckIdentity" in self.malformed else request.name
        ),
        "primitive_version": request.version,
        "accepted": not rejected,
        "reason": "test rejection" if rejected else None,
        "reason_code": ("rejected.invalid_state" if rejected else None),
        "command_id": command_id,
        "sequence": len(self.calls),
        "result": {"status": outcome},
      }
    )

  async def CancelPrimitive(self, request, context):
    await self._maybe_abort("CancelPrimitive", context)
    return _struct(
      {
        "type": "primitive.ack",
        "primitive": "base.velocity",
        "primitive_version": "1.0.0",
        "accepted": True,
        "reason": None,
        "reason_code": None,
        "command_id": request.command_id,
        "sequence": len(self.calls) + 1,
        "result": {"status": "cancelled"},
      }
    )

  async def ListCapabilities(self, request, context):
    del request
    await self._maybe_abort("ListCapabilities", context)
    await self._send_generation("ListCapabilities", context)
    response = capabilities_pb2.ListCapabilitiesResponse()
    devices = {
      "base-0": (
        "base",
        ["base.velocity", "base.navigate", "base.posture", "motion.stop"],
      ),
      "gimbal-0": ("gimbal", ["gimbal.motion", "motion.stop"]),
      "arm-0": ("manipulator", ["arm.init", "arm.movej", "arm.movel", "motion.stop"]),
      "gripper-0": ("gripper", ["gripper.motion"]),
      "speaker-0": ("speaker", ["speaker.play"]),
    }
    for device_id, (device_class, names) in devices.items():
      ready = all(self.device_health.get(device_id, (True, True)))
      for name in names:
        version = {
          "base.navigate": "1.0.0",
          "arm.movej": "1.0.0",
          "arm.movel": "1.0.0",
          "gripper.motion": "1.0.0",
          "speaker.play": "1.0.0",
        }.get(name, "1.0.0")
        response.capabilities.add(
          name=f"hardware/{self.robot_id}/{device_class}/default/{name}@{version}",
          primitive=name,
          version=version,
          device_id=device_id,
          available=ready,
        )
    for name in ("robot.stop", "safety.estop"):
      response.capabilities.add(
        name=f"runtime/{self.robot_id}/{name}@1.0.0",
        primitive=name,
        version="1.0.0",
        available=True,
      )
    return response

  async def SubmitOperation(self, request, context):
    self.rpc_methods.append("SubmitOperation")
    self.operation_requests.append(("SubmitOperation", request.operation_id))
    await self._maybe_abort("SubmitOperation", context)
    await self._check_operation_auth(
      "SubmitOperation",
      context,
      revision=not request.capability_name.endswith("/safety.estop@1.0.0"),
    )
    descriptor = next(
      (
        item
        for item in (await self.ListCapabilities(None, context)).capabilities
        if item.name == request.capability_name
      ),
      None,
    )
    if descriptor is None:
      await context.abort(grpc.StatusCode.NOT_FOUND, "unknown capability")
    ack = await self.CallPrimitive(
      primitive_pb2.CallPrimitiveRequest(
        name=descriptor.primitive,
        version=descriptor.version,
        device_id=descriptor.device_id,
        arguments=request.arguments,
        command_id=request.operation_id,
      ),
      context,
    )
    payload = json_format.MessageToDict(ack)
    result = payload["result"]["status"]
    status = {
      "succeeded": capabilities_pb2.OPERATION_STATUS_SUCCEEDED,
      "rejected": capabilities_pb2.OPERATION_STATUS_REJECTED,
      "failed": capabilities_pb2.OPERATION_STATUS_FAILED,
      "cancelled": capabilities_pb2.OPERATION_STATUS_CANCELLED,
      "timed_out": capabilities_pb2.OPERATION_STATUS_TIMED_OUT,
      "outcome_unknown": capabilities_pb2.OPERATION_STATUS_OUTCOME_UNKNOWN,
    }[result]
    operation = capabilities_pb2.Operation(
      operation_id=request.operation_id,
      status=status,
      accepted=payload["accepted"],
      acknowledgement=ack,
    )
    self.operations[request.operation_id] = operation
    if self.lost_submit_once:
      self.lost_submit_once = False
      await context.abort(grpc.StatusCode.UNAVAILABLE, "reply lost after reservation")
    if descriptor.primitive in self.pending_operations:
      self.pending_operations.remove(descriptor.primitive)
      return capabilities_pb2.Operation(
        operation_id=request.operation_id,
        status=capabilities_pb2.OPERATION_STATUS_PENDING,
      )
    return operation

  async def GetOperation(self, request, context):
    self.rpc_methods.append("GetOperation")
    self.operation_requests.append(("GetOperation", request.operation_id))
    await self._maybe_abort("GetOperation", context)
    await self._check_operation_auth("GetOperation", context)
    operation = self.operations.get(request.operation_id)
    if operation is None:
      await context.abort(grpc.StatusCode.NOT_FOUND, "unknown operation")
    return operation

  async def WatchOperation(self, request, context):
    await self._check_operation_auth("WatchOperation", context)
    if self.lost_watch_once:
      self.lost_watch_once = False
      await context.abort(grpc.StatusCode.UNAVAILABLE, "watch disconnected")
    yield await self.GetOperation(request, context)

  async def CancelOperation(self, request, context):
    await self._maybe_abort("CancelOperation", context)
    await self._check_operation_auth("CancelOperation", context)
    if self.expected_grant is not None and request.operation_id not in self.operations:
      await context.abort(grpc.StatusCode.NOT_FOUND, "unknown operation")
    ack = await self.CancelPrimitive(
      primitive_pb2.PrimitiveCommandRequest(command_id=request.operation_id), context
    )
    operation = capabilities_pb2.Operation(
      operation_id=request.operation_id,
      status=capabilities_pb2.OPERATION_STATUS_CANCELLED,
      accepted=True,
      acknowledgement=ack,
    )
    self.operations[request.operation_id] = operation
    return operation

  async def CaptureFrames(self, request, context):
    await self._maybe_abort("CaptureFrames", context)
    if request.robot_name != ROBOT_NAME or request.device_id != "camera-0":
      await context.abort(grpc.StatusCode.NOT_FOUND, "camera not found")
    if request.frame_count not in {1, 2}:
      await context.abort(grpc.StatusCode.INVALID_ARGUMENT, "invalid frame count")
    frames = []
    for jpeg, width, height, captured_at in self.frames[: request.frame_count]:
      frame = camera_pb2.CameraFrame(
        jpeg=jpeg, width_px=width, height_px=height, captured_at=captured_at
      )
      frames.append(frame)
    if "CaptureFrames" in self.malformed and frames:
      frames[0].jpeg = b""
    return camera_pb2.CaptureFramesResponse(frames=frames)

  async def notify(self, uri: str) -> None:
    await self._changes.setdefault(uri, asyncio.Queue()).put(uri)

  def restart(self, robot_id: str) -> None:
    self.robot_id = robot_id
    self.started_at = "2026-09-23T00:00:00Z"
    self._payloads = self._make_payloads()


@dataclass
class StubGateway:
  host: str
  port: int
  gateway: StubGatewayService
  world: StubWorld
  _server: grpc.aio.Server

  @property
  def endpoint(self) -> str:
    return f"{self.host}:{self.port}"


async def start_stub_gateway(
  *, host: str = "127.0.0.1", port: int = 0, config: dict | None = None
) -> StubGateway:
  service = StubGatewayService()
  config = config or {}
  service.errors.update(config.get("errors", {}))
  service.malformed.update(config.get("malformed", set()))
  server = grpc.aio.server()
  gateway_pb2_grpc.add_GatewayServiceServicer_to_server(service, server)
  capabilities_pb2_grpc.add_CapabilityServiceServicer_to_server(service, server)
  skills_pb2_grpc.add_SkillServiceServicer_to_server(service, server)
  health_pb2_grpc.add_HealthServicer_to_server(
    _Health(config.get("health_status", health_pb2.HealthCheckResponse.SERVING)),
    server,
  )
  bound_port = server.add_insecure_port(f"{host}:{port}")
  await server.start()
  return StubGateway(host, bound_port, service, service.world, server)


async def stop_stub_gateway(stub: StubGateway) -> None:
  await stub._server.stop(grace=0)
