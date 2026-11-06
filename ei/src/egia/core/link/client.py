"""Async gRPC client for the EOS ``GatewayService`` northbound interface."""

from __future__ import annotations

import asyncio
import os
import re
import stat
from collections.abc import AsyncIterator, Callable
from dataclasses import dataclass
from typing import Any, TypeVar, cast
from uuid import uuid4

import grpc
from google.protobuf import duration_pb2, empty_pb2, json_format, struct_pb2
from grpc_health.v1 import health_pb2, health_pb2_grpc
from jsonschema import Draft202012Validator
from jsonschema.exceptions import SchemaError
from pydantic import BaseModel, ValidationError

from egia.contracts.actions import PrimitiveAck
from egia.contracts.endpoints import (
  DeviceKind,
  PrimitiveKind,
  ResourceKind,
  resource_kind,
)
from egia.contracts.limits import (
  NumericRange,
  parse_numeric_limits,
  validate_numeric_arguments,
)
from egia.contracts.registry import resource_contract
from egia.contracts.validate import (
  validate_primitive,
  validate_primitive_ack,
  validate_resource,
)
from egia.internal.skills import SkillExecution
from egia.lib import shared_events
from egia.lib.config import GatewayConfig
from egia.lib.logger import bind_log_context, get_logger
from island.generated.eos import (
  capabilities_pb2,
  capabilities_pb2_grpc,
  gateway_pb2_grpc,
  skills_pb2_grpc,
)
from island.generated.eos.perception import camera_pb2
from island.generated.eos.robot import descriptor_pb2
from island.generated.eos.skills import catalog_pb2, execution_pb2
from island.generated.eos.system import resource_pb2

T = TypeVar("T", bound=BaseModel)

__all__ = [
  "CameraFrame",
  "DeviceBinding",
  "EosClient",
  "EosDiscovery",
  "LinkPrimitiveError",
  "PrimitiveBinding",
  "ResourceBinding",
  "SkillBinding",
  "SkillExecution",
  "SkillExecutionSnapshot",
]

INGRESS_EXCERPT_CAP = 1024
# EOS enables gRPC's default aggregate health entry (the empty service name).
HEALTH_SERVICE = ""
OPERATION_SUBMIT_TIMEOUT_S = 5.0
OPERATION_WATCH_TIMEOUT_S = 30.0
_SHA256_HEX = re.compile(r"[0-9a-f]{64}\Z")
_GRANT_ENV = "EGIA_EOS_CONTROL_GRANT_FILE"


@dataclass(frozen=True)
class DeviceBinding:
  """One compact EOS body descriptor with inline primitive bindings."""

  id: str
  device_class: str
  part_id: str
  description: str
  state: str
  health: str
  primitives: tuple[PrimitiveBinding, ...]

  @property
  def supported_primitives(self) -> tuple[str, ...]:
    """Return names of ready bindings for coordination consumers."""
    return tuple(item.name for item in self.primitives if item.health == "ready")

  @property
  def alive(self) -> bool:
    """Compatibility projection for health-aware coordination."""
    return self.health != "unavailable"

  @property
  def admitted(self) -> bool:
    """Whether this device is currently usable for new work."""
    return self.health == "ready"


@dataclass(frozen=True)
class EosDiscovery:
  """One complete atomic EOS body and executable-surface generation."""

  robot_id: str
  robot_name: str
  robot_model: str
  robot_description: str
  state: str
  health: str
  estop: bool
  power_pct: float | None
  version: str
  started_at: str
  robot_primitives: tuple[PrimitiveBinding, ...]
  devices: tuple[DeviceBinding, ...]


@dataclass(frozen=True)
class ResourceBinding:
  kind: ResourceKind
  uri: str
  device_id: str | None = None


@dataclass(frozen=True)
class PrimitiveBinding:
  """One deployment-specific primitive binding at robot or device scope."""

  name: str
  eos_robot_id: str
  device_id: str | None
  version: str
  description: str
  device_description: str
  scope: str
  kind: str
  health: str
  references: tuple[str, ...]
  absolute_frame: str | None
  dependencies: tuple[tuple[str, str], ...]
  accepts: tuple[str, ...]
  limits: dict[str, NumericRange]
  capability_name: str | None = None


@dataclass(frozen=True)
class SkillBinding:
  """Semantic metadata for one deployment-admitted EOS skill."""

  backend: str
  eos_robot_id: str
  name: str
  version: str
  description: str
  primitives: tuple[str, ...]
  primitive_refs: tuple[tuple[str, str], ...] = ()
  parameters: dict[str, Any] | None = None
  returns: dict[str, Any] | None = None
  resources: tuple[tuple[str, str], ...] = ()


SkillExecutionSnapshot = SkillExecution


@dataclass(frozen=True)
class CameraFrame:
  """One validated JPEG frame captured from an EOS camera cache."""

  jpeg: bytes
  width_px: int
  height_px: int
  captured_at: str


class LinkPrimitiveError(Exception):
  """EOS transport, lookup, or argument validation failure."""

  def __init__(self, status: int | None, reason: str) -> None:
    super().__init__(reason)
    self.status = status
    self.reason = reason


_GRPC_STATUS = {
  grpc.StatusCode.INVALID_ARGUMENT: 400,
  grpc.StatusCode.UNAUTHENTICATED: 401,
  grpc.StatusCode.PERMISSION_DENIED: 403,
  grpc.StatusCode.NOT_FOUND: 404,
  grpc.StatusCode.ALREADY_EXISTS: 409,
  grpc.StatusCode.FAILED_PRECONDITION: 412,
  grpc.StatusCode.RESOURCE_EXHAUSTED: 429,
  grpc.StatusCode.INTERNAL: 500,
  grpc.StatusCode.DATA_LOSS: 502,
  grpc.StatusCode.UNAVAILABLE: 503,
  grpc.StatusCode.DEADLINE_EXCEEDED: 504,
  grpc.StatusCode.CANCELLED: 499,
}


def _rpc_error(exc: grpc.aio.AioRpcError) -> LinkPrimitiveError:
  return LinkPrimitiveError(
    _GRPC_STATUS.get(exc.code()), exc.details() or exc.code().name
  )


def _struct_dict(message: struct_pb2.Struct) -> dict[str, Any]:
  value = json_format.MessageToDict(message)
  if not isinstance(value, dict):
    raise TypeError("EOS Struct response is not an object")
  return value


class EosClient:
  """One discovered EOS process, addressed externally by a stable alias."""

  def __init__(
    self,
    gateway: GatewayConfig,
    *,
    backend: str = "default",
    expected_revision: str | None = None,
  ) -> None:
    if (
      expected_revision is not None and _SHA256_HEX.fullmatch(expected_revision) is None
    ):
      raise ValueError("selected IsOS revision must be a lowercase SHA-256")
    self._gateway = gateway
    self._backend = backend
    self._expected_revision = expected_revision
    self._target = gateway.endpoint
    self._channel: grpc.aio.Channel | None = None
    self._stub: gateway_pb2_grpc.GatewayServiceStub | None = None
    self._skill_stub: skills_pb2_grpc.SkillServiceStub | None = None
    self._capability_stub: capabilities_pb2_grpc.CapabilityServiceStub | None = None
    self._health: Any | None = None
    self._init_lock = asyncio.Lock()
    self._discovery: EosDiscovery | None = None
    self._resources: tuple[ResourceBinding, ...] = ()
    self._primitives: tuple[PrimitiveBinding, ...] = ()
    self._skills: tuple[SkillBinding, ...] = ()
    self._skills_available = False
    self._status: dict[str, Any] = {}
    self._generation = 0
    self._ready = False
    self._active_revision: str | None = None
    self._projection_sha256: str | None = None
    self._refresh_listeners: list[Callable[[EosClient], None]] = []
    self._log = get_logger("egia.core.link")

  @property
  def endpoint(self) -> str:
    return self._target

  @property
  def backend(self) -> str:
    """Stable EGIA robot alias retained for existing internal callers."""
    return self._backend

  @property
  def robot_alias(self) -> str:
    return self._backend

  @property
  def discovery(self) -> EosDiscovery:
    if self._discovery is None or not self._ready:
      raise RuntimeError("EOS endpoint has not been initialized")
    return self._discovery

  @property
  def resources(self) -> tuple[ResourceBinding, ...]:
    return self._resources

  @property
  def primitives(self) -> tuple[PrimitiveBinding, ...]:
    return self._primitives

  @property
  def skills(self) -> tuple[SkillBinding, ...]:
    """Return the latest validated native-skill metadata."""
    return self._skills

  @property
  def skills_available(self) -> bool:
    """Whether EOS answered native-skill discovery for this generation."""
    return self._skills_available

  @property
  def generation(self) -> int:
    return self._generation

  @property
  def active_revision(self) -> str | None:
    """IsOS saved graph digest advertised by the complete EOS generation."""
    return self._active_revision if self._ready else None

  @property
  def projection_sha256(self) -> str | None:
    """EOS projection digest from the same complete ready generation."""
    return self._projection_sha256 if self._ready else None

  def _invalidate(self) -> None:
    """Fence writes immediately when a generation becomes uncertain."""
    self._ready = False
    self._discovery = None
    self._resources = ()
    self._primitives = ()
    self._skills = ()
    self._skills_available = False
    self._active_revision = None
    self._projection_sha256 = None

  @staticmethod
  def _grant() -> str | None:
    path = os.environ.get(_GRANT_ENV)
    if path is None:
      return None
    if not path or not os.path.isabs(path):
      raise LinkPrimitiveError(503, "EGIA control grant path is invalid")
    try:
      info = os.lstat(path)
      if (
        not stat.S_ISREG(info.st_mode)
        or info.st_uid != os.getuid()
        or stat.S_IMODE(info.st_mode) != 0o600
      ):
        raise LinkPrimitiveError(503, "EGIA control grant file is unsafe")
      with open(path, "rb") as handle:
        value = handle.read(66)
        opened = os.fstat(handle.fileno())
      if (opened.st_dev, opened.st_ino) != (info.st_dev, info.st_ino):
        raise LinkPrimitiveError(503, "EGIA control grant file changed")
    except OSError as exc:
      raise LinkPrimitiveError(503, "EGIA control grant file is unavailable") from exc
    if (
      len(value) != 65
      or value[-1:] != b"\n"
      or _SHA256_HEX.fullmatch(value[:64].decode("ascii", errors="ignore")) is None
    ):
      raise LinkPrimitiveError(503, "EGIA control grant file is malformed")
    return value[:64].decode("ascii")

  @staticmethod
  def _generation_ids(metadata: Any) -> tuple[str | None, str | None]:
    entries = cast(list[tuple[str, str]], list(metadata or ()))
    for name in ("eos-active-revision", "eos-projection-sha256"):
      if sum(key == name for key, _ in entries) > 1:
        raise LinkPrimitiveError(503, "EOS returned duplicate generation metadata")
    values = dict(entries)
    revision = values.get("eos-active-revision")
    projection = values.get("eos-projection-sha256")
    for value in (revision, projection):
      if value is not None and (
        not isinstance(value, str) or _SHA256_HEX.fullmatch(value) is None
      ):
        raise LinkPrimitiveError(503, "EOS returned malformed generation metadata")
    return revision, projection

  def _operation_metadata(
    self, *, emergency: bool = False, lookup: bool = False
  ) -> tuple[tuple[str, str], ...]:
    grant = self._grant()
    if grant is None:
      return ()
    if not lookup and (
      not self._ready or (not emergency and self._active_revision is None)
    ):
      raise LinkPrimitiveError(503, "EOS managed generation is unavailable")
    result = [("authorization", f"Bearer {grant}")]
    if not emergency and not lookup and self._active_revision is not None:
      result.append(("x-eos-active-revision", self._active_revision))
    return tuple(result)

  def add_refresh_listener(self, listener: Callable[[EosClient], None]) -> None:
    """Run ``listener`` after every successful atomic registry refresh."""
    self._refresh_listeners.append(listener)

  async def get_skill(self, name: str, version: str) -> SkillBinding:
    """Resolve and validate one exact skill descriptor through EOS."""
    await self.initialize()
    assert self._skill_stub is not None
    try:
      descriptor = await self._skill_stub.GetSkill(
        catalog_pb2.GetSkillRequest(
          reference=catalog_pb2.SkillReference(name=name, version=version)
        ),
        timeout=5.0,
      )
      return self._parse_skill(descriptor, self.discovery, self.primitives)
    except grpc.aio.AioRpcError as exc:
      raise _rpc_error(exc) from exc

  async def start_skill_execution(
    self,
    name: str,
    version: str,
    arguments: dict[str, Any],
    request_id: str,
    timeout_s: float,
  ) -> SkillExecutionSnapshot:
    """Northbound spelling for :meth:`start_skill`."""
    return await self.start_skill(
      name,
      version,
      arguments,
      request_id=request_id,
      timeout_s=timeout_s,
    )

  def motion_device_ids(self) -> tuple[str, ...]:
    return tuple(
      device.id
      for device in self.discovery.devices
      if device.device_class in {"base", "manipulator", "gimbal"}
    )

  async def initialize(self, *, force: bool = False) -> EosDiscovery:
    """Discover the runtime and atomically refresh resources and primitives."""
    async with self._init_lock:
      if self._discovery is not None and self._ready and not force:
        return self._discovery
      prior = self._discovery.started_at if self._discovery is not None else None
      self._invalidate()
      grant = self._grant()
      if grant is not None and self._expected_revision is None:
        raise LinkPrimitiveError(
          503, "managed EOS requires selected IsOS graph revision"
        )
      if self._expected_revision is not None and grant is None:
        raise LinkPrimitiveError(
          503, "selected IsOS graph revision requires EGIA control grant"
        )
      self._ensure_channel()
      assert self._stub is not None
      assert self._health is not None
      try:
        health = await self._health.Check(
          health_pb2.HealthCheckRequest(service=HEALTH_SERVICE), timeout=5.0
        )
        if health.status != health_pb2.HealthCheckResponse.SERVING:
          raise LinkPrimitiveError(503, "EOS GatewayService health is not SERVING")
        discover_call = self._stub.Discover(empty_pb2.Empty(), timeout=5.0)
        response = await discover_call
        discover_ids = self._generation_ids(await discover_call.initial_metadata())
        resources = await self._stub.ListResources(empty_pb2.Empty(), timeout=5.0)
        status_call = self._stub.GetStatus(empty_pb2.Empty(), timeout=5.0)
        status = _struct_dict(await status_call)
        status_ids = self._generation_ids(await status_call.initial_metadata())
        assert self._capability_stub is not None
        capabilities_call = self._capability_stub.ListCapabilities(
          capabilities_pb2.ListCapabilitiesRequest(), timeout=5.0
        )
        capability_catalog = await capabilities_call
        capability_ids = self._generation_ids(
          await capabilities_call.initial_metadata()
        )
      except grpc.aio.AioRpcError as exc:
        raise _rpc_error(exc) from exc
      if discover_ids != status_ids or discover_ids != capability_ids:
        raise LinkPrimitiveError(503, "EOS discovery generation changed during refresh")
      revision, projection = discover_ids
      if grant is not None and (revision is None or projection is None):
        raise LinkPrimitiveError(503, "EOS managed generation metadata is incomplete")
      if revision is not None and grant is None:
        raise LinkPrimitiveError(
          503, "EOS managed generation requires EGIA control grant"
        )
      if self._expected_revision is not None and revision != self._expected_revision:
        raise LinkPrimitiveError(
          412, "EOS active revision differs from selected IsOS graph"
        )
      discovery, primitive_bindings = self._parse_discovery(
        response, status, capability_catalog
      )
      resource_bindings = self._parse_resources(resources, discovery)
      skills, skills_available = await self._discover_skills(
        discovery, primitive_bindings
      )
      self._discovery = discovery
      self._resources = resource_bindings
      self._primitives = primitive_bindings
      self._skills = skills
      self._skills_available = skills_available
      self._status = status
      self._active_revision = revision
      self._projection_sha256 = projection
      self._ready = True
      self._generation += 1
      for listener in self._refresh_listeners:
        listener(self)
      self._log.info(
        "eos_discovered",
        robot=self._backend,
        eos_robot_id=discovery.robot_id,
        devices=len(discovery.devices),
        resources=len(resource_bindings),
        primitives=len(primitive_bindings),
        skills=len(skills),
        skills_available=skills_available,
        restarted=prior is not None and prior != discovery.started_at,
      )
      return discovery

  async def _discover_skills(
    self,
    discovery: EosDiscovery,
    primitives: tuple[PrimitiveBinding, ...],
  ) -> tuple[tuple[SkillBinding, ...], bool]:
    """Read the dedicated skill catalog, degrading only while it is unavailable."""
    assert self._skill_stub is not None
    try:
      registry = await self._skill_stub.ListSkills(
        catalog_pb2.ListSkillsRequest(), timeout=5.0
      )
      descriptors = sorted(
        registry.skills,
        key=lambda item: (item.reference.name, item.reference.version),
      )
      skills: list[SkillBinding] = []
      seen: set[tuple[str, str]] = set()
      for descriptor in descriptors:
        key = (descriptor.reference.name, descriptor.reference.version)
        if not all(key) or key in seen:
          raise ValueError("ListSkills returned malformed or duplicate descriptors")
        seen.add(key)
        skills.append(self._parse_skill(descriptor, discovery, primitives))
      return tuple(skills), True
    except grpc.aio.AioRpcError as exc:
      if exc.code() is grpc.StatusCode.UNAVAILABLE:
        self._log.info(
          "skills_unavailable",
          robot=self._backend,
          reason=exc.details() or exc.code().name,
        )
        return (), False
      raise _rpc_error(exc) from exc
    except (SchemaError, TypeError, ValueError) as exc:
      raise LinkPrimitiveError(400, f"invalid SkillService catalog: {exc}") from exc
    except asyncio.CancelledError:
      raise

  def _parse_skill(
    self,
    descriptor: catalog_pb2.SkillDescriptor,
    discovery: EosDiscovery,
    primitives: tuple[PrimitiveBinding, ...],
  ) -> SkillBinding:
    """Validate one EOS native-skill definition for prompt projection."""
    parameters = _struct_dict(descriptor.parameters)
    returns = _struct_dict(descriptor.returns)
    primitive_refs = tuple((item.name, item.version) for item in descriptor.primitives)
    resources = tuple(
      (item.uri_template, item.description) for item in descriptor.resources
    )
    Draft202012Validator.check_schema(parameters)
    Draft202012Validator.check_schema(returns)
    reference = descriptor.reference
    if (
      not reference.name
      or not reference.version
      or not descriptor.description
      or any(not name or not version for name, version in primitive_refs)
      or len(primitive_refs) != len(set(primitive_refs))
      or any(not uri or not description for uri, description in resources)
    ):
      raise ValueError("ListSkills returned a malformed descriptor")
    known = {(primitive.name, primitive.version) for primitive in primitives}
    if any(item not in known for item in primitive_refs):
      raise ValueError("ListSkills declared a primitive absent from Discover bindings")
    return SkillBinding(
      backend=self._backend,
      eos_robot_id=discovery.robot_id,
      name=reference.name,
      version=reference.version,
      description=descriptor.description,
      primitives=tuple(name for name, _ in primitive_refs),
      primitive_refs=primitive_refs,
      parameters=parameters,
      returns=returns,
      resources=resources,
    )

  def _parse_discovery(
    self,
    response: descriptor_pb2.DiscoverResponse,
    status: dict[str, Any],
    capability_catalog: capabilities_pb2.ListCapabilitiesResponse,
  ) -> tuple[EosDiscovery, tuple[PrimitiveBinding, ...]]:
    """Parse one compact body response and all inline primitive bindings."""
    robot = response.robot
    version = status.get("version")
    started_at = status.get("started_at")
    if (
      not robot.id
      or not robot.name
      or not robot.model
      or not robot.description
      or not robot.state
      or robot.health not in {"ready", "degraded", "unavailable"}
      or not isinstance(version, str)
      or not version
      or not isinstance(started_at, str)
      or not started_at
    ):
      raise LinkPrimitiveError(400, "Discover returned incomplete robot metadata")

    capabilities = self._parse_capabilities(capability_catalog, robot.id)
    bindings: list[PrimitiveBinding] = []
    robot_bindings = self._parse_inline_bindings(
      robot.primitives,
      robot_id=robot.id,
      device_id=None,
      device_description=robot.description,
      scope="robot",
      capabilities=capabilities,
    )
    bindings.extend(robot_bindings)
    devices: list[DeviceBinding] = []
    seen_devices: set[str] = set()
    for item in response.devices:
      device_class = getattr(item, "class")
      try:
        DeviceKind(device_class)
      except ValueError as exc:
        raise LinkPrimitiveError(
          400, f"Discover returned unknown device class {device_class!r}"
        ) from exc
      if (
        not item.id
        or item.id in seen_devices
        or not item.part
        or not item.description
        or not item.state
        or item.health not in {"ready", "degraded", "unavailable"}
      ):
        raise LinkPrimitiveError(
          400, "Discover returned malformed or duplicate devices"
        )
      seen_devices.add(item.id)
      device_primitives = self._parse_inline_bindings(
        item.primitives,
        robot_id=robot.id,
        device_id=item.id,
        device_description=item.description,
        scope="device",
        capabilities=capabilities,
      )
      bindings.extend(device_primitives)
      devices.append(
        DeviceBinding(
          id=item.id,
          device_class=device_class,
          part_id=item.part,
          description=item.description,
          state=item.state,
          health=item.health,
          primitives=device_primitives,
        )
      )
    identities = [
      (item.scope, item.device_id, item.name, item.version) for item in bindings
    ]
    if len(identities) != len(set(identities)):
      raise LinkPrimitiveError(400, "Discover returned duplicate primitive bindings")
    discovery = EosDiscovery(
      robot_id=robot.id,
      robot_name=robot.name,
      robot_model=robot.model,
      robot_description=robot.description,
      state=robot.state,
      health=robot.health,
      estop=robot.estop,
      power_pct=robot.power_pct if robot.HasField("power_pct") else None,
      version=version,
      started_at=started_at,
      robot_primitives=robot_bindings,
      devices=tuple(devices),
    )
    return discovery, tuple(bindings)

  def _parse_inline_bindings(
    self,
    descriptors,
    *,
    robot_id: str,
    device_id: str | None,
    device_description: str,
    scope: str,
    capabilities: dict[tuple[str, str, str, str], tuple[str, bool]],
  ) -> tuple[PrimitiveBinding, ...]:
    """Validate primitive metadata without admitting unhealthy bindings."""
    parsed: list[PrimitiveBinding] = []
    for item in descriptors:
      if (
        not item.name
        or not item.version
        or not item.description
        or item.kind not in {"direct", "advanced"}
        or item.health not in {"ready", "degraded", "unavailable"}
        or len(item.references) != len(set(item.references))
        or any(value not in {"relative", "absolute"} for value in item.references)
      ):
        raise LinkPrimitiveError(400, "Discover returned malformed primitive binding")
      try:
        primitive_kind = PrimitiveKind(item.name)
      except ValueError:
        # Retain deployment-specific metadata for diagnostics, but it has no
        # local contract and cannot enter the executable registry.
        primitive_kind = None
      limits_payload = _struct_dict(item.limits)
      limits: dict[str, NumericRange] = {}
      if primitive_kind is not None:
        from egia.contracts.registry import action_contract

        contract = action_contract(primitive_kind)
        if item.version != contract.version:
          raise LinkPrimitiveError(
            400,
            f"Discover returned unsupported {item.name!r} version {item.version!r}",
          )
        try:
          limits = parse_numeric_limits(primitive_kind, limits_payload)
        except (TypeError, ValueError) as exc:
          raise LinkPrimitiveError(
            400, f"Discover returned invalid limits for {item.name!r}: {exc}"
          ) from exc
      dependencies = tuple((entry.name, entry.health) for entry in item.dependencies)
      if any(
        not name or health not in {"ready", "degraded", "unavailable"}
        for name, health in dependencies
      ):
        raise LinkPrimitiveError(400, "Discover returned malformed dependencies")
      advertised = capabilities.get(
        (robot_id, item.name, item.version, device_id or "")
      )
      parsed.append(
        PrimitiveBinding(
          name=item.name,
          capability_name=advertised[0] if advertised and advertised[1] else None,
          eos_robot_id=robot_id,
          device_id=device_id,
          version=item.version,
          description=item.description,
          device_description=device_description,
          scope=scope,
          kind=item.kind,
          health=(
            item.health if advertised is not None and advertised[1] else "unavailable"
          ),
          references=tuple(item.references),
          absolute_frame=item.absolute_frame or None,
          dependencies=dependencies,
          accepts=tuple(item.accepts),
          limits=limits,
        )
      )
    return tuple(parsed)

  @staticmethod
  def _parse_capabilities(
    response: capabilities_pb2.ListCapabilitiesResponse,
    robot_id: str,
  ) -> dict[tuple[str, str, str, str], tuple[str, bool]]:
    """Resolve one canonical advertised capability for each exact action target."""
    parsed: dict[tuple[str, str, str, str], tuple[str, bool]] = {}
    seen_names: set[str] = set()
    for item in response.capabilities:
      if (
        not item.name
        or not item.primitive
        or not item.version
        or item.name in seen_names
      ):
        raise LinkPrimitiveError(400, "ListCapabilities returned malformed descriptors")
      segments = item.name.split("/")
      valid_hardware_name = (
        len(segments) == 5
        and segments[0] == "hardware"
        and segments[1] == robot_id
        and bool(segments[2])
        and bool(segments[3])
        and bool(item.device_id)
        and segments[4] == f"{item.primitive}@{item.version}"
      )
      valid_runtime_name = (
        len(segments) == 3
        and segments[0] == "runtime"
        and segments[1] == robot_id
        and not item.device_id
        and segments[2] == f"{item.primitive}@{item.version}"
      )
      if not (valid_hardware_name or valid_runtime_name):
        raise LinkPrimitiveError(
          400,
          "ListCapabilities returned a malformed name or capability for another robot",
        )
      seen_names.add(item.name)
      key = (robot_id, item.primitive, item.version, item.device_id)
      previous = parsed.get(key)
      if previous is None:
        parsed[key] = (item.name, item.available)
        continue
      # Multiple aliases to the same exact target are safe only when their
      # availability agrees. Retain the lexically first name deterministically.
      if previous[1] != item.available:
        raise LinkPrimitiveError(
          400, "ListCapabilities returned conflicting availability for one target"
        )
      parsed[key] = (min(previous[0], item.name), item.available)
    return parsed

  def _parse_resources(
    self, response: struct_pb2.ListValue, discovery: EosDiscovery
  ) -> tuple[ResourceBinding, ...]:
    payload = json_format.MessageToDict(response)
    if not isinstance(payload, list):
      raise LinkPrimitiveError(400, "ListResources returned a non-list payload")
    bindings: list[ResourceBinding] = []
    seen: set[str] = set()
    for item in payload:
      if (
        not isinstance(item, dict)
        or not isinstance(item.get("uri"), str)
        or "class" not in item
        or not isinstance(item.get("class"), (str, type(None)))
      ):
        raise LinkPrimitiveError(400, "ListResources returned a malformed entry")
      uri = cast(str, item.get("uri"))
      if not uri or uri in seen:
        raise LinkPrimitiveError(
          400, "ListResources returned an empty or duplicate URI"
        )
      seen.add(uri)
      if not isinstance(item.get("class"), str):
        raise LinkPrimitiveError(
          400, "ListResources returned a classless runtime route"
        )
      if not uri.startswith(f"robots/{discovery.robot_name}/"):
        raise LinkPrimitiveError(
          400, "ListResources returned a URI for another runtime"
        )
      kind = resource_kind(item.get("class"), uri)
      if kind is None:
        continue
      bindings.append(ResourceBinding(kind=kind, uri=uri))
    return tuple(bindings)

  def _ensure_channel(self) -> None:
    if self._channel is not None:
      return
    self._channel = grpc.aio.insecure_channel(self._target)
    self._stub = gateway_pb2_grpc.GatewayServiceStub(self._channel)
    self._skill_stub = skills_pb2_grpc.SkillServiceStub(self._channel)
    self._capability_stub = capabilities_pb2_grpc.CapabilityServiceStub(self._channel)
    self._health = health_pb2_grpc.HealthStub(self._channel)

  async def health_check(self) -> bool:
    self._ensure_channel()
    assert self._health is not None
    try:
      response = await self._health.Check(
        health_pb2.HealthCheckRequest(service=HEALTH_SERVICE),
        timeout=5.0,
      )
      return response.status == health_pb2.HealthCheckResponse.SERVING
    except grpc.aio.AioRpcError:
      return False

  async def get_status(self) -> dict[str, Any]:
    await self.initialize()
    assert self._stub is not None
    try:
      return _struct_dict(await self._stub.GetStatus(empty_pb2.Empty()))
    except grpc.aio.AioRpcError as exc:
      raise _rpc_error(exc) from exc

  async def refresh_status(self) -> dict[str, Any]:
    """Refresh the complete discovery generation on each heartbeat cycle."""
    await self.initialize(force=True)
    return dict(self._status)

  def _status_requires_rediscovery(self, status: dict[str, Any]) -> bool:
    """Compare the current EOS GetStatus aggregate with typed discovery."""
    robot_id = status.get("robot_id")
    robot_name = status.get("robot_name")
    started_at = status.get("started_at")
    state = status.get("status")
    if (
      not isinstance(robot_id, str)
      or not isinstance(robot_name, str)
      or not isinstance(started_at, str)
      or state not in {"ready", "degraded"}
    ):
      raise LinkPrimitiveError(400, "GetStatus returned malformed runtime metadata")
    return (
      robot_id != self.discovery.robot_id
      or robot_name != self.discovery.robot_name
      or started_at != self.discovery.started_at
    )

  async def list_resources(self) -> tuple[ResourceBinding, ...]:
    await self.initialize()
    return self.resources

  async def list_primitives(self) -> tuple[PrimitiveBinding, ...]:
    await self.initialize()
    return self.primitives

  def skill(self, name: str, version: str) -> SkillBinding | None:
    """Resolve one exact skill from the current discovery generation."""
    return next(
      (item for item in self._skills if item.name == name and item.version == version),
      None,
    )

  async def start_skill(
    self,
    name: str,
    version: str,
    arguments: dict[str, Any],
    *,
    request_id: str,
    timeout_s: float,
  ) -> SkillExecution:
    """Start or idempotently recover one exact skill execution."""
    await self.initialize()
    if not 0 < timeout_s <= 3600:
      raise LinkPrimitiveError(400, "skill timeout must be in (0, 3600] seconds")
    binding = self.skill(name, version)
    if binding is None:
      raise LinkPrimitiveError(404, f"unknown skill {name}@{version}")
    try:
      Draft202012Validator(binding.parameters or {}).validate(arguments)
      payload = struct_pb2.Struct()
      payload.update(arguments)
      timeout = duration_pb2.Duration()
      timeout.FromNanoseconds(int(timeout_s * 1_000_000_000))
      assert self._skill_stub is not None
      response = await self._skill_stub.StartExecution(
        execution_pb2.StartExecutionRequest(
          reference=catalog_pb2.SkillReference(name=name, version=version),
          arguments=payload,
          request_id=request_id,
          timeout=timeout,
        )
      )
      return self._parse_skill_execution(response, expected=(name, version, request_id))
    except grpc.aio.AioRpcError as exc:
      raise _rpc_error(exc) from exc
    except (SchemaError, ValueError) as exc:
      raise LinkPrimitiveError(400, f"invalid skill request: {exc}") from exc

  async def get_skill_execution(self, execution_id: str) -> SkillExecution:
    """Read one execution snapshot by EOS identity."""
    self._ensure_channel()
    assert self._skill_stub is not None
    try:
      response = await self._skill_stub.GetExecution(
        execution_pb2.GetExecutionRequest(execution_id=execution_id)
      )
      return self._parse_skill_execution(response, execution_id=execution_id)
    except grpc.aio.AioRpcError as exc:
      raise _rpc_error(exc) from exc

  async def watch_skill_execution(
    self, execution_id: str, *, after_sequence: int = 0
  ) -> AsyncIterator[SkillExecution]:
    """Yield strictly advancing snapshots after the supplied durable cursor."""
    self._ensure_channel()
    assert self._skill_stub is not None
    try:
      stream = self._skill_stub.WatchExecution(
        execution_pb2.WatchExecutionRequest(
          execution_id=execution_id, after_sequence=after_sequence
        )
      )
      cursor = after_sequence
      async for response in stream:
        snapshot = self._parse_skill_execution(response, execution_id=execution_id)
        if snapshot.sequence <= cursor:
          raise LinkPrimitiveError(502, "skill watch sequence did not advance")
        cursor = snapshot.sequence
        yield snapshot
    except grpc.aio.AioRpcError as exc:
      raise _rpc_error(exc) from exc

  async def cancel_skill_execution(self, execution_id: str) -> SkillExecution:
    """Request cooperative cancellation and return EOS's latest snapshot."""
    self._ensure_channel()
    assert self._skill_stub is not None
    try:
      response = await self._skill_stub.TerminateExecution(
        execution_pb2.TerminateExecutionRequest(execution_id=execution_id)
      )
      return self._parse_skill_execution(response, execution_id=execution_id)
    except grpc.aio.AioRpcError as exc:
      raise _rpc_error(exc) from exc

  def _parse_skill_execution(
    self,
    response: execution_pb2.SkillExecution,
    *,
    expected: tuple[str, str, str] | None = None,
    execution_id: str | None = None,
  ) -> SkillExecution:
    status = (
      execution_pb2.ExecutionStatus.Name(response.status)
      .removeprefix("EXECUTION_STATUS_")
      .lower()
    )
    identity = (
      response.reference.name,
      response.reference.version,
      response.request_id,
    )
    if (
      not response.execution_id
      or not all(identity)
      or status == "unspecified"
      or response.sequence < 1
      or (expected is not None and identity != expected)
      or (execution_id is not None and response.execution_id != execution_id)
    ):
      raise LinkPrimitiveError(502, "SkillService returned a malformed execution")
    return SkillExecution(
      execution_id=response.execution_id,
      request_id=response.request_id,
      name=response.reference.name,
      version=response.reference.version,
      status=status,
      sequence=response.sequence,
      cancellation_requested=response.cancellation_requested,
      reason_code=response.reason_code or None,
      message=response.message or None,
      result=_struct_dict(response.result),
    )

  async def read_resource(
    self,
    uri: str,
    model: type[T] | None = None,
    *,
    kind: ResourceKind | None = None,
  ) -> T:
    await self.initialize()
    if kind is not None:
      model = cast("type[T]", resource_contract(kind).model)
    if model is None:
      raise ValueError("read_resource requires a ResourceKind or model")
    assert self._stub is not None
    try:
      response = await self._stub.ReadResource(resource_pb2.ResourceRequest(uri=uri))
    except grpc.aio.AioRpcError as exc:
      raise _rpc_error(exc) from exc
    raw = _struct_dict(response)
    try:
      discovered_kind = kind or next(
        (binding.kind for binding in self._resources if binding.uri == uri), None
      )
      if discovered_kind is not None:
        parsed = cast("T", validate_resource(discovered_kind, raw))
      else:
        parsed = model.model_validate(raw)
    except (ValidationError, ValueError) as exc:
      self._log.warning(
        "ingress_invalid", robot=self._backend, uri=uri, kind=kind, error=str(exc)
      )
      raise ValueError(f"invalid EOS resource {uri}: {exc}") from exc
    self._log.info(
      "resource_read", robot=self._backend, uri=uri, kind=kind.value if kind else None
    )
    return parsed

  async def stream_resource(
    self,
    uri: str,
    model: type[T] | None = None,
    *,
    kind: ResourceKind | None = None,
    poll_interval_s: float = 1.0,
  ) -> AsyncIterator[T]:
    """Yield changed snapshots, using periodic reads to close watch setup gaps."""
    await self.initialize()
    if poll_interval_s <= 0:
      raise ValueError("poll_interval_s must be positive")
    if kind is not None:
      model = cast("type[T]", resource_contract(kind).model)
    if model is None:
      raise ValueError("stream_resource requires a ResourceKind or model")
    binding = next((item for item in self._resources if item.uri == uri), None)
    selector_kind = kind or (binding.kind if binding is not None else None)
    selector_device = binding.device_id if binding is not None else None
    current_uri = uri
    while True:
      assert self._stub is not None
      pending: asyncio.Task[resource_pb2.ResourceChanged] | None = None
      try:
        watch = self._stub.WatchResource(resource_pb2.ResourceRequest(uri=current_uri))
        notifications = watch.__aiter__()
        pending = asyncio.create_task(anext(notifications))
        latest = await self.read_resource(current_uri, model, kind=selector_kind)
        yield latest
        while True:
          done, _ = await asyncio.wait({pending}, timeout=poll_interval_s)
          if not done:
            refreshed = await self.read_resource(current_uri, model, kind=selector_kind)
            if refreshed != latest:
              latest = refreshed
              yield refreshed
            continue
          try:
            changed = pending.result()
          except StopAsyncIteration:
            return
          pending = asyncio.create_task(anext(notifications))
          if changed.uri != current_uri:
            raise LinkPrimitiveError(
              None, "WatchResource returned a notification for another URI"
            )
          refreshed = await self.read_resource(current_uri, model, kind=selector_kind)
          if refreshed != latest:
            latest = refreshed
            yield refreshed
      except asyncio.CancelledError:
        raise
      except (grpc.aio.AioRpcError, LinkPrimitiveError) as exc:
        status = (
          exc.status
          if isinstance(exc, LinkPrimitiveError)
          else _GRPC_STATUS.get(exc.code())
        )
        if status not in {404, 503} or selector_kind is None:
          raise OSError(f"EOS resource watch failed for {current_uri}: {exc}") from exc
        await self.initialize(force=True)
        refreshed = next(
          (
            item
            for item in self._resources
            if item.kind is selector_kind and item.device_id == selector_device
          ),
          None,
        )
        if refreshed is None:
          raise LinkPrimitiveError(
            404, "resource target vanished after rediscovery"
          ) from exc
        current_uri = refreshed.uri
      finally:
        if pending is not None:
          if not pending.done():
            pending.cancel()
          await asyncio.gather(pending, return_exceptions=True)

  async def call_primitive(
    self,
    name: str,
    device_id: str,
    arguments: dict[str, Any],
    *,
    command_id: str | None = None,
  ) -> PrimitiveAck:
    """Send one uniquely identified primitive and await its terminal result."""
    if command_id is not None and not shared_events.valid_opaque_id(command_id):
      raise LinkPrimitiveError(400, "invalid operation ID")
    await self.initialize()
    if "command_id" in arguments:
      raise LinkPrimitiveError(
        400, "arguments.command_id is forbidden; command identity is an envelope field"
      )
    command_id = command_id if command_id is not None else str(uuid4())
    try:
      kind = PrimitiveKind(name)
      validated = validate_primitive(kind, arguments)
    except (ValueError, ValidationError) as exc:
      raise LinkPrimitiveError(400, f"invalid primitive request: {exc}") from exc
    target_required = kind not in {PrimitiveKind.ROBOT_STOP, PrimitiveKind.SAFETY_ESTOP}
    if target_required != bool(device_id):
      requirement = "non-empty" if target_required else "empty"
      raise LinkPrimitiveError(400, f"{name} requires a {requirement} device_id")
    primitive = next(
      (
        item
        for item in self._primitives
        if item.name == name and item.device_id == (device_id or None)
      ),
      None,
    )
    if primitive is None:
      raise LinkPrimitiveError(
        404, f"primitive {name!r} is not discovered for {device_id!r}"
      )
    if primitive.health != "ready":
      raise LinkPrimitiveError(
        503, f"primitive {name!r} is {primitive.health} for {device_id!r}"
      )
    if primitive is not None:
      try:
        validate_numeric_arguments(kind, primitive.limits, arguments)
      except (TypeError, ValueError) as exc:
        self._log.warning(
          "primitive_egress_rejected",
          robot=self._backend,
          endpoint=self._target,
          primitive=name,
          device_id=device_id,
          category="numeric_limits",
          reason=str(exc),
        )
        raise LinkPrimitiveError(400, f"invalid primitive request: {exc}") from exc
    payload = validated.model_dump(mode="json", exclude={"type"}, exclude_unset=True)
    struct = struct_pb2.Struct()
    struct.update(payload)
    assert self._capability_stub is not None
    started_at = asyncio.get_running_loop().time()
    submitted = False
    submission_attempted = False

    def _unknown(reason_code: str) -> None:
      shared_events.emit(
        "egia_operation_unknown",
        fields={"operation_id": command_id, "reason_code": reason_code},
        correlation_id=command_id,
      )
      shared_events.emit(
        "egia_recovery_required",
        fields={"reason_code": "unknown_outcome"},
        correlation_id=command_id,
      )

    with bind_log_context(backend=self._backend, command_id=command_id):
      self._log.info(
        "primitive_command_reserved",
        primitive=name,
        primitive_version=primitive.version,
        device_id=device_id,
        argument_keys=sorted(arguments),
      )
      try:
        if primitive.capability_name is None:
          raise LinkPrimitiveError(
            503, f"primitive {name!r} has no available EOS capability"
          )
        operation_metadata = self._operation_metadata(
          emergency=kind is PrimitiveKind.SAFETY_ESTOP
        )
        submission_attempted = True
        response = await self._capability_stub.SubmitOperation(
          capabilities_pb2.SubmitOperationRequest(
            capability_name=primitive.capability_name,
            arguments=struct,
            operation_id=command_id,
          ),
          timeout=OPERATION_SUBMIT_TIMEOUT_S,
          metadata=operation_metadata,
        )
        if (
          response.operation_id == command_id
          and response.HasField("accepted")
          and response.accepted
        ):
          submitted = True
          shared_events.emit(
            "egia_operation_submitted",
            fields={"operation_id": command_id},
            correlation_id=command_id,
          )
        response = await self._wait_operation(response, command_id)
      except asyncio.CancelledError:
        self._log.warning("primitive_command_interrupted", primitive=name)
        confirmed = False
        if self._is_cancellable(kind):
          confirmed = await self._cancel_after_interruption(command_id)
        if submission_attempted and not confirmed:
          _unknown("receipt_missing")
          self._invalidate()
        raise
      except LinkPrimitiveError as exc:
        if not submission_attempted:
          raise
        try:
          response = await self._lookup_operation(command_id)
          submitted = True
          self._invalidate()
        except LinkPrimitiveError:
          confirmed = self._is_cancellable(
            kind
          ) and await self._cancel_after_interruption(command_id)
          if not confirmed:
            _unknown("receipt_missing")
          self._invalidate()
          raise exc
      except grpc.aio.AioRpcError as exc:
        if not submission_attempted:
          raise _rpc_error(exc) from exc
        try:
          response = await self._lookup_operation(command_id)
          submitted = True
          self._invalidate()
        except LinkPrimitiveError:
          confirmed = (
            exc.code() is grpc.StatusCode.DEADLINE_EXCEEDED
            and self._is_cancellable(kind)
            and await self._cancel_after_interruption(command_id)
          )
          if not confirmed:
            _unknown(
              "timeout"
              if exc.code() is grpc.StatusCode.DEADLINE_EXCEEDED
              else "disconnect"
            )
          self._invalidate()
          raise _rpc_error(exc) from exc
    try:
      ack = self._validated_operation_ack(response, command_id, primitive=primitive)
    except LinkPrimitiveError:
      if submitted:
        _unknown("receipt_missing")
      raise
    outcome = ack.status.value
    if outcome in {"timed_out", "outcome_unknown"}:
      _unknown("timeout" if outcome == "timed_out" else "receipt_missing")
    else:
      shared_events.emit(
        "egia_operation_terminal",
        fields={"operation_id": command_id, "outcome": outcome},
        correlation_id=command_id,
      )
    with bind_log_context(backend=self._backend, command_id=command_id):
      self._log.info(
        "primitive_terminal",
        device_id=device_id,
        primitive=ack.primitive,
        primitive_version=ack.primitive_version,
        accepted=ack.accepted,
        status=ack.status.value,
        reason_code=ack.reason_code,
        sequence=ack.sequence,
        duration_ms=round((asyncio.get_running_loop().time() - started_at) * 1000, 3),
      )
    return ack

  @staticmethod
  def _is_cancellable(kind: PrimitiveKind) -> bool:
    """Return whether EOS defines command-targeted cancellation for ``kind``."""
    return kind in {
      PrimitiveKind.BASE_VELOCITY,
      PrimitiveKind.BASE_NAVIGATE,
      PrimitiveKind.BASE_POSTURE,
      PrimitiveKind.GIMBAL_MOTION,
      PrimitiveKind.ARM_INIT,
      PrimitiveKind.ARM_MOVEJ,
      PrimitiveKind.ARM_MOVEL,
      PrimitiveKind.ARM_PICK,
      PrimitiveKind.GRIPPER_MOTION,
    }

  async def _cancel_after_interruption(self, command_id: str) -> bool:
    """Give EOS five seconds to confirm command-targeted cancellation."""
    try:
      self._log.info("primitive_cancel_requested", command_id=command_id)
      async with asyncio.timeout(5.0):
        ack = await asyncio.shield(self.cancel_primitive(command_id))
      self._log.info(
        "primitive_cancel_terminal",
        command_id=command_id,
        status=ack.status.value,
        sequence=ack.sequence,
      )
      if ack.status.value in {"succeeded", "failed", "cancelled", "rejected"}:
        shared_events.emit(
          "egia_operation_terminal",
          fields={"operation_id": command_id, "outcome": ack.status.value},
        )
        return True
    except (TimeoutError, LinkPrimitiveError):
      self._log.warning(
        "primitive_outcome_unknown", robot=self._backend, command_id=command_id
      )
    return False

  async def cancel_primitive(self, command_id: str) -> PrimitiveAck:
    """Cancel one cancellable operation and validate its terminal acknowledgement."""
    if not command_id:
      raise LinkPrimitiveError(400, "command_id must be non-empty")
    await self.initialize()
    assert self._capability_stub is not None
    try:
      async with asyncio.timeout(5.0):
        response = await self._capability_stub.CancelOperation(
          capabilities_pb2.OperationRequest(operation_id=command_id),
          timeout=5.0,
          metadata=self._operation_metadata(lookup=True),
        )
        response = await self._wait_operation(response, command_id, timeout_s=5.0)
    except TimeoutError as exc:
      raise LinkPrimitiveError(
        None, "EOS cancellation did not reach a terminal result within five seconds"
      ) from exc
    except grpc.aio.AioRpcError as exc:
      raise _rpc_error(exc) from exc
    return self._validated_operation_ack(response, command_id)

  async def _lookup_operation(self, operation_id: str) -> capabilities_pb2.Operation:
    """Resolve an uncertain operation by ID without resubmitting it."""
    assert self._capability_stub is not None
    try:
      operation = await self._capability_stub.GetOperation(
        capabilities_pb2.OperationRequest(operation_id=operation_id),
        timeout=OPERATION_SUBMIT_TIMEOUT_S,
        metadata=self._operation_metadata(lookup=True),
      )
      return await self._wait_operation(operation, operation_id)
    except grpc.aio.AioRpcError as exc:
      raise _rpc_error(exc) from exc

  async def recover_operation(
    self,
    operation_id: str,
    primitive_name: str,
    device_id: str,
    *,
    eos_robot_id: str | None = None,
    graph_revision: str | None = None,
  ) -> PrimitiveAck:
    """Query the same durable ID; never call SubmitOperation on recovery."""
    if not shared_events.valid_opaque_id(operation_id):
      raise LinkPrimitiveError(400, "invalid operation ID")
    await self.initialize(force=True)
    if eos_robot_id is not None and self.discovery.robot_id != eos_robot_id:
      raise LinkPrimitiveError(
        412, "EOS robot identity changed before operation lookup"
      )
    if (
      graph_revision is not None
      and (self.active_revision or "standalone") != graph_revision
    ):
      raise LinkPrimitiveError(
        412, "EOS graph revision changed before operation lookup"
      )
    binding = next(
      (
        item
        for item in self._primitives
        if item.name == primitive_name and item.device_id == (device_id or None)
      ),
      None,
    )
    if binding is None:
      raise LinkPrimitiveError(
        404, "operation primitive is absent from current EOS generation"
      )
    response = await self._lookup_operation(operation_id)
    return self._validated_operation_ack(response, operation_id, primitive=binding)

  async def _wait_operation(
    self,
    operation: capabilities_pb2.Operation,
    operation_id: str,
    *,
    timeout_s: float = OPERATION_WATCH_TIMEOUT_S,
  ) -> capabilities_pb2.Operation:
    """Follow a pending operation until EOS reports an explicit terminal state."""
    if operation.operation_id != operation_id:
      raise LinkPrimitiveError(None, "EOS operation identity was not echoed")
    if operation.status != capabilities_pb2.OPERATION_STATUS_PENDING:
      return operation
    assert self._capability_stub is not None
    stream = self._capability_stub.WatchOperation(
      capabilities_pb2.OperationRequest(operation_id=operation_id),
      timeout=timeout_s,
      metadata=self._operation_metadata(lookup=True),
    )
    terminal: capabilities_pb2.Operation | None = None
    try:
      async for update in stream:
        if update.operation_id != operation_id:
          raise LinkPrimitiveError(None, "EOS operation stream changed identity")
        if update.status != capabilities_pb2.OPERATION_STATUS_PENDING:
          terminal = update
          break
    except asyncio.CancelledError:
      if terminal is None:
        await stream.cancel()
      raise
    except grpc.aio.AioRpcError as exc:
      raise _rpc_error(exc) from exc
    if terminal is None:
      raise LinkPrimitiveError(
        None, "EOS operation stream ended before a terminal result"
      )
    return terminal

  @staticmethod
  def _validated_operation_ack(
    operation: capabilities_pb2.Operation,
    operation_id: str,
    *,
    primitive: PrimitiveBinding | None = None,
  ) -> PrimitiveAck:
    """Validate EOS's operation result and its canonical acknowledgement together."""
    if operation.operation_id != operation_id:
      raise LinkPrimitiveError(None, "EOS operation identity was not echoed")
    if operation.status == capabilities_pb2.OPERATION_STATUS_PENDING:
      raise LinkPrimitiveError(None, "EOS operation has no terminal result")
    if not operation.HasField("accepted"):
      raise LinkPrimitiveError(None, "terminal EOS operation omitted admission result")
    try:
      ack = validate_primitive_ack(_struct_dict(operation.acknowledgement))
    except (ValueError, TypeError) as exc:
      raise LinkPrimitiveError(
        None, f"malformed operation acknowledgement: {exc}"
      ) from exc
    terminal_status = {
      capabilities_pb2.OPERATION_STATUS_SUCCEEDED: "succeeded",
      capabilities_pb2.OPERATION_STATUS_REJECTED: "rejected",
      capabilities_pb2.OPERATION_STATUS_FAILED: "failed",
      capabilities_pb2.OPERATION_STATUS_CANCELLED: "cancelled",
      capabilities_pb2.OPERATION_STATUS_TIMED_OUT: "timed_out",
      capabilities_pb2.OPERATION_STATUS_OUTCOME_UNKNOWN: "outcome_unknown",
    }.get(operation.status)
    if terminal_status is None or ack.status.value != terminal_status:
      raise LinkPrimitiveError(
        None, "operation status disagrees with its acknowledgement"
      )
    if (
      operation.status == capabilities_pb2.OPERATION_STATUS_SUCCEEDED
      and not operation.accepted
    ):
      raise LinkPrimitiveError(None, "successful operation was not admitted")
    if (
      operation.status == capabilities_pb2.OPERATION_STATUS_REJECTED
      and operation.accepted
    ):
      raise LinkPrimitiveError(None, "rejected operation reports successful admission")
    if ack.command_id != operation_id or ack.accepted != operation.accepted:
      raise LinkPrimitiveError(
        None, "operation acknowledgement identity or admission mismatch"
      )
    if primitive is not None and (
      ack.primitive != primitive.name or ack.primitive_version != primitive.version
    ):
      raise LinkPrimitiveError(None, "operation acknowledgement action mismatch")
    return ack

  async def capture_frames(
    self, device_id: str, frame_count: int = 1
  ) -> tuple[CameraFrame, ...]:
    """Capture one or two distinct cached JPEG frames from a camera device."""
    await self.initialize()
    if frame_count not in {1, 2}:
      raise LinkPrimitiveError(400, "frame_count must be one or two")
    device = next(
      (item for item in self.discovery.devices if item.id == device_id), None
    )
    if device is None or device.device_class not in {"camera", "cam"}:
      raise LinkPrimitiveError(404, f"camera device {device_id!r} is not discovered")
    if not device.alive or not device.admitted:
      raise LinkPrimitiveError(503, f"camera device {device_id!r} is unavailable")
    assert self._stub is not None
    try:
      response = await self._stub.CaptureFrames(
        camera_pb2.CaptureFramesRequest(
          robot_name=self.discovery.robot_name,
          device_id=device_id,
          frame_count=frame_count,
        )
      )
    except grpc.aio.AioRpcError as exc:
      raise _rpc_error(exc) from exc
    frames = tuple(
      CameraFrame(
        jpeg=bytes(item.jpeg),
        width_px=item.width_px,
        height_px=item.height_px,
        captured_at=item.captured_at,
      )
      for item in response.frames
    )
    if (
      len(frames) != frame_count
      or any(
        not item.jpeg
        or not item.jpeg.startswith(b"\xff\xd8")
        or not item.jpeg.endswith(b"\xff\xd9")
        or item.width_px <= 0
        or item.height_px <= 0
        or not item.captured_at
        for item in frames
      )
      or len({(item.captured_at, item.jpeg) for item in frames}) != len(frames)
    ):
      raise LinkPrimitiveError(None, "CaptureFrames returned malformed frames")
    return frames

  async def aclose(self) -> None:
    if self._channel is not None:
      await self._channel.close()
    self._channel = None
    self._stub = None
    self._skill_stub = None
    self._capability_stub = None
    self._health = None
