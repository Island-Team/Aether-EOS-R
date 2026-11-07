"""Northbound WebSocket endpoints for WebUI.

Serves the wire spec in ``docs/egia/northbound.md``: the EGIA-server
liveness stream, translated-hearing perception ingress, the bundled scene
telemetry stream, and one replaceable-snapshot stream
per standalone robot part. Every part stream is the same shape — a producer
publishes a ``egia.contracts.perception`` snapshot to a :class:`PartStore` and the
handler emits the latest on connect plus each newer sequence — so a new part
only adds a path/``ResourceKind`` binding here, not a new handler.
"""

from __future__ import annotations

import asyncio
import contextlib
import errno
import json
from collections.abc import AsyncIterator, Callable, Mapping
from datetime import UTC, datetime
from typing import Protocol, cast

from pydantic import ValidationError
from websockets.asyncio.client import unix_connect
from websockets.asyncio.server import Server, serve
from websockets.exceptions import ConnectionClosed
from websockets.typing import Origin

from egia.api.chat import ChatAck, ChatError, ChatRequest
from egia.api.invoke import (
  PrimitiveInvokeAck,
  PrimitiveInvokeError,
  PrimitiveInvokeRequest,
)
from egia.api.mission import (
  MissionCommandAck,
  MissionCommandError,
  MissionCommandErrorCode,
  MissionCommandMission,
  MissionCommandRequest,
  MissionCreateRequest,
  MissionDocumentProjection,
  MissionHistoryAction,
  MissionHistoryEntry,
  MissionHistoryStep,
  MissionPlanProjection,
  MissionPriorityRequest,
  MissionState,
  MissionTableEntry,
  MissionTableSnapshot,
  MissionUpdateRequest,
)
from egia.api.perceptions import PerceptionResource, PerceptionsSnapshot
from egia.api.plan import (
  ActionProjection,
  PlannedStepProjection,
  PlanProjection,
  PlanRecord,
  PlanStepProjection,
  PlanStepStatus,
  StepOutcome,
)
from egia.api.review import ReviewAck, ReviewError, ReviewRequest
from egia.api.skills import (
  SkillCancelRequest,
  SkillCommandAck,
  SkillCommandError,
  SkillCommandErrorCode,
  SkillCommandRequest,
  SkillDescriptor,
  SkillExecutionSnapshot,
  SkillGetExecutionRequest,
  SkillLookupRequest,
  SkillPrimitiveReference,
  SkillResource,
  SkillStartRequest,
  SkillWatchRequest,
)
from egia.api.status import (
  BackendStatus,
  BackendStatusFrame,
  HealthState,
  SafetyStatus,
  ServerStatus,
)
from egia.contracts.endpoints import ResourceKind
from egia.core.decision.model_io import ModelIOStore
from egia.core.link import LinkPrimitiveError, SkillBinding
from egia.core.link.client import SkillExecutionSnapshot as LinkSkillExecution
from egia.core.memory.reference import ReferenceStore
from egia.core.memory.store import EpisodicStore
from egia.core.missions import MissionMutationError, MissionQueue
from egia.core.missions.store import MissionStoreError
from egia.core.perception.admission import Admission
from egia.core.safety.gate import SafetyGate
from egia.internal.decision import ActionInvocation, StepContext
from egia.io.state import ServerState
from egia.io.store import (
  ContextStore,
  PartStore,
  PrimitivesStore,
  PromptsStore,
  SkillsStore,
)
from egia.lib.logger import bind_correlation_id, get_logger, log_once

HEALTH_PATH = "/api/aether/egia/health"
BACKEND_PATH = "/api/aether/egia/backend"
CHAT_PATH = "/api/aether/egia/chat"
MISSIONS_STREAM_PATH = "/api/aether/egia/missions/stream"
MISSIONS_COMMAND_PATH = "/api/aether/egia/missions/command"
REVIEW_PATH = "/api/aether/egia/review"
CONTEXT_PATH = "/api/aether/egia/context"
MODEL_IO_PATH = "/api/aether/egia/model_io"
PRIMITIVES_PATH = "/api/aether/egia/primitives"
SKILLS_PATH = "/api/aether/egia/skills"
SKILLS_COMMAND_PATH = "/api/aether/egia/skills/command"
SKILLS_EXECUTION_PATH = "/api/aether/egia/skills/execution"


class SkillExecutionClient(Protocol):
  """Minimal EOS skill surface injected into the northbound adapter."""

  async def get_skill(self, name: str, version: str) -> SkillBinding: ...

  async def start_skill_execution(
    self,
    name: str,
    version: str,
    arguments: dict[str, object],
    request_id: str,
    timeout_s: float,
  ) -> LinkSkillExecution: ...

  async def get_skill_execution(self, execution_id: str) -> LinkSkillExecution: ...

  async def cancel_skill_execution(self, execution_id: str) -> LinkSkillExecution: ...

  def watch_skill_execution(
    self, execution_id: str, *, after_sequence: int
  ) -> AsyncIterator[LinkSkillExecution]: ...


class NorthboundBindError(RuntimeError):
  """Raised when EGIA's configured WebSocket listener is already occupied."""

  def __init__(self, host: str, port: int) -> None:
    super().__init__(f"northbound listener {host}:{port} is already in use")
    self.host = host
    self.port = port


PROMPTS_PATH = "/api/aether/egia/prompts"

# Runtime-facing routes follow the contracts taxonomy: ``perception/world``
# and ``perception/body`` mirror ``egia.contracts.perception.{world,body}``
# and ``primitive`` is the operator ingress into ``egia.contracts.actions``.
PERCEPTION_PREFIX = "/api/aether/egia/perception"
PERCEPTIONS_PATH = "/api/aether/egia/perceptions"
WORLD_PREFIX = f"{PERCEPTION_PREFIX}/world"
PRIMITIVE_PREFIX = "/api/aether/egia/primitive"
PRIMITIVE_INVOKE_PATH = f"{PRIMITIVE_PREFIX}/invoke"
SANDBOX_MEDIA_PATH = "/v1/sandbox"

# One northbound replaceable-snapshot stream per readable EOS resource. Status
# remains under the body namespace; semantic products use the world namespace.
# Resources absent from a deployment's stores answer ``unavailable``.
PART_PATHS: Mapping[ResourceKind, str] = {
  ResourceKind.WORLD: f"{WORLD_PREFIX}/state",
  ResourceKind.DETECTION: f"{WORLD_PREFIX}/detection",
  ResourceKind.ASR: f"{WORLD_PREFIX}/asr",
}

ALLOWED_ORIGINS = frozenset({"https://www.os-island.com"})

HEARING_KIND = "hearing"
CHAT_SOURCE = "webui.chat"
RESULT_KIND = "result"
INVOKE_SOURCE = "webui.invoke"
MISSION_COMMAND_SOURCE = "webui.missions.command"


def _effective_status(state: ServerState, log) -> HealthState:
  supervisor = state.supervisor
  status = state.status
  if (
    supervisor is not None
    and status == HealthState.READY
    and not supervisor.is_healthy()
  ):
    log_once(
      log,
      "warning",
      "health_degraded",
      reported=state.status.value,
    )
    return HealthState.DEGRADED
  return status


def _build_status(
  state: ServerState,
  log,
  correlation_id: str | None = None,
  safety: Callable[[], SafetyStatus] | None = None,
) -> ServerStatus:
  uptime = (datetime.now(UTC) - state.started_at).total_seconds()
  supervisor = state.supervisor
  status = _effective_status(state, log)
  return ServerStatus(
    status=status,
    version=state.version,
    started_at=state.started_at,
    uptime_seconds=uptime,
    correlation_id=correlation_id,
    subsystems=supervisor.statuses() if supervisor else [],
    safety=safety() if safety is not None else None,
  )


def _plan_projection(record: PlanRecord) -> PlanProjection:
  """Project a live plan record into the operator plan view (§3.3)."""
  semantic_complete = sum(
    step.status in (PlanStepStatus.DONE, PlanStepStatus.FAILED) for step in record.plan
  )
  action_count = sum(len(step.actions) for step in record.steps)
  return PlanProjection(
    plan_id=record.id,
    mission_id=record.mission_id,
    priority=record.priority,
    revision=record.revision,
    completeness=(
      semantic_complete / len(record.plan)
      if record.plan
      else sum(
        action.outcome
        in (StepOutcome.SUCCEEDED, StepOutcome.FAILED, StepOutcome.ABORTED)
        for step in record.steps
        for action in step.actions
      )
      / action_count
      if action_count
      else 0.0
    ),
    steps=[
      PlanStepProjection(
        index=step.index,
        outcome=step.outcome,
        state=("done" if step.outcome is StepOutcome.SUCCEEDED else "failed"),
        result=step.final,
        reason=step.reason,
        final=step.final,
        actions=[
          ActionProjection(
            index=action.index,
            robot_id=action.robot_id,
            device_id=action.device_id,
            intent=action.intent,
            action=action.action,
            action_kind=action.action_kind,
            version=action.version,
            request_id=action.request_id,
            execution_id=action.execution_id,
            execution_status=action.execution_status,
            command=action.command,
            arguments=action.arguments,
            outcome=action.outcome,
            state=("done" if action.outcome is StepOutcome.SUCCEEDED else "failed"),
            result=action.result,
            reason=action.reason,
          )
          for action in step.actions
        ],
      )
      for step in record.steps
    ],
    plan=[
      PlannedStepProjection(
        id=item.id,
        sentence=item.sentence,
        status=item.status,
        commands=item.commands,
        reason=item.reason,
        required=item.required,
        after=list(item.after),
        terminal=item.terminal,
        evidence=item.evidence,
      )
      for item in record.plan
    ],
    decision_mode=record.decision_mode,
    decision_graph=record.decision_graph,
    annotations=len(record.annotations),
  )


def _mission_history_entry(entry: MissionHistoryEntry, document) -> MissionHistoryEntry:
  """Attach a mission document's current steps to its history summary."""
  if document is None:
    return entry.model_copy(deep=True)
  return MissionHistoryEntry(
    mission_id=entry.mission_id,
    order=entry.order,
    state=entry.state,
    priority_band=entry.priority_band,
    timestamp=entry.timestamp,
    detail=entry.detail,
    revision=document.revision,
    completeness=document.completeness,
    document=_document_projection(document),
    steps=[
      MissionHistoryStep(
        index=step.index,
        id=step.id,
        sentence=step.sentence,
        state=step.state.value,
        result=step.result,
        reason=step.reason,
        required=step.required,
        after=list(step.after),
        terminal=step.terminal,
        evidence=step.evidence,
        actions=[
          MissionHistoryAction(
            index=action.index,
            robot_id=action.robot_id,
            device_id=action.device_id,
            intent=action.intent,
            action=action.action,
            command=action.command,
            arguments=action.arguments,
            state=action.state.value,
            result=action.result,
            reason=action.reason,
          )
          for action in step.actions
        ],
      )
      # Planner-synchronized records live under the primary plan; manually
      # authored legacy documents retain their compatible root steps.
      for step in (document.plan.steps if document.plan is not None else document.steps)
    ],
  )


def _document_projection(document) -> MissionDocumentProjection | None:
  if document is None:
    return None

  def project_plan(plan) -> MissionPlanProjection:
    completed = sum(step.state.value in ("done", "failed") for step in plan.steps)
    return MissionPlanProjection(
      plan_id=plan.plan_id,
      title=plan.title,
      revision=plan.revision,
      state=plan.state.value,
      result=plan.result,
      reason=plan.reason,
      completeness=completed / len(plan.steps) if plan.steps else 0.0,
      steps=[
        MissionHistoryStep(
          index=step.index,
          id=step.id,
          sentence=step.sentence,
          state=step.state.value,
          result=step.result,
          reason=step.reason,
          required=step.required,
          after=list(step.after),
          terminal=step.terminal,
          evidence=step.evidence,
          actions=[
            MissionHistoryAction(
              index=action.index,
              robot_id=action.robot_id,
              device_id=action.device_id,
              intent=action.intent,
              action=action.action,
              command=action.command,
              arguments=action.arguments,
              state=action.state.value,
              result=action.result,
              reason=action.reason,
            )
            for action in step.actions
          ],
        )
        for step in plan.steps
      ],
      subplans=[project_plan(child) for child in plan.subplans],
    )

  return MissionDocumentProjection(
    revision=document.revision,
    completeness=document.completeness,
    plan=project_plan(document.plan) if document.plan is not None else None,
  )


class NorthboundServer:
  """Asyncio WebSocket server for WebUI.

  One server, many routes under ``/api/aether/egia/*``: status, hearing,
  primitive and skill catalogs, primitive invocation, and replaceable world,
  detection, and ASR snapshot streams. Resource
  streams read from ``part_stores``; a part absent from the mapping answers
  ``unavailable``.
  """

  def __init__(
    self,
    state: ServerState,
    queue: Admission,
    host: str = "127.0.0.1",
    port: int = 16060,
    *,
    status_frequency_hz: float = 0.5,
    mode: str = "sim",
    context_store: ContextStore | None = None,
    part_stores: Mapping[ResourceKind, PartStore] | None = None,
    missions: MissionQueue | None = None,
    gate: SafetyGate | None = None,
    step_timeout_s: float = 30.0,
    current_plan: Callable[[], PlanRecord | None] | None = None,
    episodic: EpisodicStore | None = None,
    reference: ReferenceStore | None = None,
    safety: Callable[[], SafetyStatus] | None = None,
    backend: Callable[[], list[BackendStatus]] | None = None,
    model_io_store: ModelIOStore | None = None,
    primitives_store: PrimitivesStore | None = None,
    skills_store: SkillsStore | None = None,
    skill_clients: Mapping[str, SkillExecutionClient] | None = None,
    prompts_store: PromptsStore | None = None,
    sandbox_media_socket: str | None = None,
  ) -> None:
    self._log = get_logger("egia.io")
    self._state = state
    self._queue = queue
    self._host = host
    self._port = port
    self._status_interval = 1.0 / status_frequency_hz
    self._mode = mode
    self._context_store = context_store
    self._part_stores = dict(part_stores) if part_stores else {}
    self._missions = missions
    self._gate = gate
    self._step_timeout_s = step_timeout_s
    self._current_plan = current_plan
    self._episodic = episodic
    self._reference = reference
    self._safety = safety
    self._backend = backend
    self._model_io_store = model_io_store
    self._primitives_store = primitives_store
    self._skills_store = skills_store
    self._skill_clients = dict(skill_clients or {})
    self._prompts_store = prompts_store
    self._sandbox_media_socket = sandbox_media_socket
    self._server: Server | None = None

  @property
  def address(self) -> tuple[str, int]:
    if self._server is None or self._server.sockets is None:
      return self._host, self._port
    sock = self._server.sockets[0].getsockname()
    host, port = sock[:2]
    return str(host), int(port)

  async def start(self) -> None:
    try:
      self._server = await serve(
        self._handle,
        self._host,
        self._port,
        origins=[*(Origin(origin) for origin in ALLOWED_ORIGINS), None],
      )
    except OSError as exc:
      if exc.errno == errno.EADDRINUSE:
        raise NorthboundBindError(self._host, self._port) from exc
      raise
    host, port = self.address
    self._log.info("io_started", host=host, port=port)

  async def stop(self) -> None:
    if self._server is None:
      return
    self._log.info("io_stopping")
    self._server.close()
    await self._server.wait_closed()
    self._server = None
    self._log.info("io_stopped")

  async def _handle(self, websocket) -> None:
    path = websocket.request.path
    if path == SANDBOX_MEDIA_PATH:
      await self._handle_sandbox_media(websocket)
      return
    if path == HEALTH_PATH:
      await self._handle_health(websocket)
      return
    if path == BACKEND_PATH:
      await self._handle_backend(websocket)
      return
    if path == CHAT_PATH:
      await self._handle_chat(websocket)
      return
    if path == MISSIONS_STREAM_PATH:
      await self._handle_missions_stream(websocket)
      return
    if path == MISSIONS_COMMAND_PATH:
      await self._handle_missions_command(websocket)
      return
    if path == PRIMITIVE_INVOKE_PATH:
      await self._handle_invoke(websocket)
      return
    if path == REVIEW_PATH:
      await self._handle_review(websocket)
      return
    if path == PERCEPTIONS_PATH:
      await self._handle_perceptions(websocket)
      return
    if path == CONTEXT_PATH:
      await self._handle_context(websocket)
      return
    if path == MODEL_IO_PATH:
      await self._handle_model_io(websocket)
      return
    if path == PRIMITIVES_PATH:
      await self._handle_catalog(websocket, self._primitives_store)
      return
    if path == SKILLS_PATH:
      await self._handle_catalog(websocket, self._skills_store)
      return
    if path == SKILLS_COMMAND_PATH:
      await self._handle_skill_command(websocket)
      return
    if path == SKILLS_EXECUTION_PATH:
      await self._handle_skill_execution(websocket)
      return
    if path == PROMPTS_PATH:
      await self._handle_prompts(websocket)
      return
    for kind, part_path in PART_PATHS.items():
      if path == part_path:
        await self._handle_part(websocket, kind)
        return
    log_once(self._log, "debug", "route_not_found", key=path, path=path)
    return

  def _skill_client(self, robot_id: str) -> SkillExecutionClient:
    try:
      return self._skill_clients[robot_id]
    except KeyError as exc:
      raise LookupError(robot_id) from exc

  @staticmethod
  def _skill_descriptor(binding: SkillBinding) -> SkillDescriptor:
    return SkillDescriptor(
      backend=binding.backend,
      eos_robot_id=binding.eos_robot_id,
      name=binding.name,
      version=binding.version,
      description=binding.description,
      primitives=list(binding.primitives),
      primitive_refs=[
        SkillPrimitiveReference(name=name, version=version)
        for name, version in binding.primitive_refs
      ],
      parameters_schema=binding.parameters or {},
      returns_schema=binding.returns or {},
      resources=[
        SkillResource(uri_template=uri, description=description)
        for uri, description in binding.resources
      ],
    )

  @staticmethod
  def _execution(robot_id: str, item: LinkSkillExecution) -> SkillExecutionSnapshot:
    return SkillExecutionSnapshot(robot_id=robot_id, **item.__dict__)

  @staticmethod
  def _skill_error(exc: LinkPrimitiveError, operation: str | None) -> SkillCommandError:
    status = exc.status
    error = cast(
      SkillCommandErrorCode,
      {
        400: "invalid_request",
        404: "execution_not_found" if operation != "get_skill" else "skill_not_found",
        409: "request_conflict",
        429: "capacity_exhausted",
        503: "unavailable",
        504: "timeout",
      }.get(status, "internal_failure")
      if status is not None
      else "internal_failure",
    )
    return SkillCommandError(operation=operation, error=error, reason=exc.reason)

  async def _handle_skill_command(self, websocket) -> None:
    """Accept exact lookup, start, read, and cancellation requests."""
    if not self._skill_clients or self._state.status not in {
      HealthState.READY,
      HealthState.DEGRADED,
    }:
      await websocket.send(SkillCommandError(error="unavailable").model_dump_json())
      await websocket.close(1013, "unavailable")
      return
    try:
      async for message in websocket:
        await websocket.send(await self._process_skill_command(message))
    except ConnectionClosed:
      return

  async def _process_skill_command(self, message: str | bytes) -> str:
    if isinstance(message, bytes):
      try:
        message = message.decode()
      except UnicodeDecodeError:
        return SkillCommandError(error="invalid_json").model_dump_json()
    try:
      data = json.loads(message)
    except json.JSONDecodeError:
      return SkillCommandError(error="invalid_json").model_dump_json()
    operation = data.get("operation") if isinstance(data, dict) else None
    try:
      request = SkillCommandRequest.model_validate(data).root
    except ValidationError as exc:
      return SkillCommandError(
        operation=operation if isinstance(operation, str) else None,
        error="invalid_request",
        reason=exc.errors()[0]["msg"],
      ).model_dump_json()
    try:
      client = self._skill_client(request.robot_id)
    except LookupError:
      return SkillCommandError(
        operation=request.operation, error="robot_not_found"
      ).model_dump_json()
    try:
      if isinstance(request, SkillLookupRequest):
        descriptor = self._skill_descriptor(
          await client.get_skill(request.name, request.version)
        )
        response = SkillCommandAck(operation=request.operation, descriptor=descriptor)
      elif isinstance(request, SkillStartRequest):
        execution = await client.start_skill_execution(
          request.name,
          request.version,
          request.arguments,
          request.request_id,
          request.timeout_s,
        )
        response = SkillCommandAck(
          operation=request.operation,
          execution=self._execution(request.robot_id, execution),
        )
      elif isinstance(request, SkillGetExecutionRequest):
        execution = await client.get_skill_execution(request.execution_id)
        response = SkillCommandAck(
          operation=request.operation,
          execution=self._execution(request.robot_id, execution),
        )
      else:
        assert isinstance(request, SkillCancelRequest)
        execution = await client.cancel_skill_execution(request.execution_id)
        response = SkillCommandAck(
          operation=request.operation,
          execution=self._execution(request.robot_id, execution),
        )
      return response.model_dump_json()
    except LinkPrimitiveError as exc:
      return self._skill_error(exc, request.operation).model_dump_json()
    except Exception:  # noqa: BLE001 - keep internal details off the wire
      self._log.exception("skill_command_failed", operation=request.operation)
      return SkillCommandError(
        operation=request.operation, error="internal_failure"
      ).model_dump_json()

  async def _handle_skill_execution(self, websocket) -> None:
    """Validate one watch request, then proxy ordered EOS snapshots."""
    if not self._skill_clients or self._state.status not in {
      HealthState.READY,
      HealthState.DEGRADED,
    }:
      await websocket.send(SkillCommandError(error="unavailable").model_dump_json())
      await websocket.close(1013, "unavailable")
      return
    try:
      message = await websocket.recv()
      if isinstance(message, bytes):
        message = message.decode()
      request = SkillWatchRequest.model_validate(json.loads(message))
    except (UnicodeDecodeError, json.JSONDecodeError, ValidationError) as exc:
      await websocket.send(
        SkillCommandError(error="invalid_request", reason=str(exc)).model_dump_json()
      )
      return
    try:
      client = self._skill_client(request.robot_id)
    except LookupError:
      await websocket.send(SkillCommandError(error="robot_not_found").model_dump_json())
      return
    try:
      async for item in client.watch_skill_execution(
        request.execution_id, after_sequence=request.after_sequence
      ):
        await websocket.send(self._execution(request.robot_id, item).model_dump_json())
    except LinkPrimitiveError as exc:
      await websocket.send(self._skill_error(exc, "get_execution").model_dump_json())
    except ConnectionClosed:
      return

  async def _handle_sandbox_media(self, websocket) -> None:
    """Proxy optional sandbox media without importing the sandbox package."""
    if self._sandbox_media_socket is None:
      await websocket.close(1013, "sandbox unavailable")
      return
    try:
      async with unix_connect(
        self._sandbox_media_socket, uri=f"ws://localhost{SANDBOX_MEDIA_PATH}"
      ) as sandbox:

        async def relay(source, destination) -> None:
          async for message in source:
            await destination.send(message)

        client_to_sandbox = asyncio.create_task(relay(websocket, sandbox))
        sandbox_to_client = asyncio.create_task(relay(sandbox, websocket))
        done, pending = await asyncio.wait(
          {client_to_sandbox, sandbox_to_client},
          return_when=asyncio.FIRST_COMPLETED,
        )
        for task in pending:
          task.cancel()
        await asyncio.gather(*done, *pending, return_exceptions=True)
    except (OSError, ConnectionClosed):
      with contextlib.suppress(ConnectionClosed):
        await websocket.close(1013, "sandbox unavailable")

  async def _reject_unavailable(self, websocket) -> None:
    """Send the standard ``unavailable`` error and close with ``1013``."""
    await websocket.send(json.dumps({"error": "unavailable"}))
    await websocket.close(1013, "unavailable")

  async def _wait_next(self, websocket, store, after):
    """Race the store's next snapshot against the peer closing the socket.

    The websockets library surfaces peer closure only when a handler reads or
    writes; a handler parked on a long store wait would otherwise hold the
    connection open forever and stall ``stop()``'s ``wait_closed``.
    """
    wait_task = asyncio.create_task(store.wait_next(after))
    close_task = asyncio.create_task(websocket.wait_closed())
    try:
      done, _pending = await asyncio.wait(
        {wait_task, close_task}, return_when=asyncio.FIRST_COMPLETED
      )
      if close_task in done:
        raise ConnectionClosed(None, None)
      return wait_task.result()
    finally:
      for task in (wait_task, close_task):
        if not task.done():
          task.cancel()

  def _perceptions_snapshot(self) -> PerceptionsSnapshot:
    """Project latest world and body resources into one grouped frame."""
    resources: list[PerceptionResource] = []
    for kind, store in self._part_stores.items():
      snapshots = store.all_latest() or (
        {"default": store.latest()} if store.latest() is not None else {}
      )
      for target, snapshot in snapshots.items():
        assert snapshot is not None
        data = snapshot.model_dump(mode="json")
        resources.append(
          PerceptionResource(
            kind=kind.value,
            part_id=str(data.get("part_id") or target),
            sequence=int(data["sequence"]),
            timestamp=data["timestamp"],
            snapshot=data,
          )
        )
    sequence = max((resource.sequence for resource in resources), default=0)
    timestamp = max(
      (resource.timestamp for resource in resources), default=datetime.now(UTC)
    )
    return PerceptionsSnapshot(
      sequence=sequence, timestamp=timestamp, resources=resources
    )

  async def _handle_perceptions(self, websocket) -> None:
    """Stream one replaceable aggregate of the latest perception resources."""
    if self._state.status != HealthState.READY:
      await self._reject_unavailable(websocket)
      return
    if not self._part_stores:
      await self._reject_unavailable(websocket)
      return
    previous = ""
    try:
      while True:
        frame = self._perceptions_snapshot()
        encoded = frame.model_dump_json()
        if encoded != previous:
          await websocket.send(encoded)
          previous = encoded
        sleep_task = asyncio.create_task(asyncio.sleep(self._status_interval))
        close_task = asyncio.create_task(websocket.wait_closed())
        done, _pending = await asyncio.wait(
          {sleep_task, close_task}, return_when=asyncio.FIRST_COMPLETED
        )
        for task in (sleep_task, close_task):
          if not task.done():
            task.cancel()
        if close_task in done:
          return
    except ConnectionClosed:
      return

  async def _handle_context(self, websocket) -> None:
    """Stream the latest prompt snapshot: on connect, then per rebuild."""
    store = self._context_store
    if store is None or self._state.status != HealthState.READY:
      await self._reject_unavailable(websocket)
      return
    latest = store.latest()
    if latest is None:
      await self._reject_unavailable(websocket)
      return
    await websocket.send(latest.model_dump_json())
    after = store.version
    try:
      while True:
        state = await self._wait_next(websocket, store, after)
        if state is None:
          return
        after = store.version
        await websocket.send(state.model_dump_json())
    except ConnectionClosed:
      return

  async def _handle_model_io(self, websocket) -> None:
    """Stream model I/O, including any separate provider thinking block.

    Readiness-gated like ``/context``; sends an initial frame with
    ``exchange=null`` and the running request count when no request has run.
    """
    store = self._model_io_store
    if store is None or self._state.status != HealthState.READY:
      await self._reject_unavailable(websocket)
      return
    frame = store.frame()
    await websocket.send(frame.model_dump_json())
    after = store.version
    try:
      while True:
        frame = await self._wait_next(websocket, store, after)
        if frame is None:
          return
        after = store.version
        await websocket.send(frame.model_dump_json())
    except ConnectionClosed:
      return

  async def _handle_catalog(self, websocket, store) -> None:
    """Stream the latest replaceable catalog snapshot."""
    if store is None or self._state.status not in {
      HealthState.READY,
      HealthState.DEGRADED,
    }:
      await self._reject_unavailable(websocket)
      return
    latest = store.latest()
    if latest is None:
      await self._reject_unavailable(websocket)
      return
    await websocket.send(latest.model_dump_json())
    after = store.version
    try:
      while True:
        state = await self._wait_next(websocket, store, after)
        if state is None:
          return
        after = store.version
        await websocket.send(state.model_dump_json())
    except ConnectionClosed:
      return

  async def _handle_prompts(self, websocket) -> None:
    """Stream the raw planner system prompt: latest snapshot on connect."""
    store = self._prompts_store
    if store is None or self._state.status not in {
      HealthState.READY,
      HealthState.DEGRADED,
    }:
      await self._reject_unavailable(websocket)
      return
    latest = store.latest()
    if latest is None:
      await self._reject_unavailable(websocket)
      return
    await websocket.send(latest.model_dump_json())
    after = store.version
    try:
      while True:
        state = await self._wait_next(websocket, store, after)
        if state is None:
          return
        after = store.version
        await websocket.send(state.model_dump_json())
    except ConnectionClosed:
      return

  async def _handle_part(self, websocket, kind: ResourceKind) -> None:
    store = self._part_stores.get(kind)
    if store is None or self._state.status != HealthState.READY:
      await self._reject_unavailable(websocket)
      return
    latest = store.latest()
    if latest is not None:
      await websocket.send(latest.model_dump_json())
    # ``-1`` so the very first published snapshot (sequence 0) still streams
    # to a client that connected before any data existed.
    after = latest.sequence if latest is not None else -1
    try:
      while True:
        state = await self._wait_next(websocket, store, after)
        if state is None:
          return
        after = state.sequence
        await websocket.send(state.model_dump_json())
    except ConnectionClosed:
      return

  async def _handle_backend(self, websocket) -> None:
    """Stream the per-backend link snapshots at the health push cadence.

    Served even while the pipeline is starting — backend connectivity is
    exactly what an operator wants to see during that window. One frame
    carries one :class:`BackendStatus` per wired endpoint; answers the
    standard ``unavailable`` envelope only when no tracker is wired.
    """
    if self._backend is None:
      await self._reject_unavailable(websocket)
      return
    backend_snapshot = self._backend

    def _frame() -> str:
      return BackendStatusFrame(backends=backend_snapshot()).model_dump_json()

    await websocket.send(_frame())
    try:
      while True:
        await asyncio.sleep(self._status_interval)
        await websocket.send(_frame())
    except ConnectionClosed:
      return

  async def _handle_health(self, websocket) -> None:
    if self._state.supervisor is None and self._state.status == HealthState.STARTING:
      await websocket.send(json.dumps({"error": "unavailable"}))
      await websocket.close(1013, "unavailable")
      return

    await self._send_status(websocket)

    push_task = asyncio.create_task(self._periodic_push(websocket))
    try:
      async for message in websocket:
        correlation_id = _parse_correlation_id(message)
        await self._send_status(websocket, correlation_id)
    except ConnectionClosed:
      return
    finally:
      push_task.cancel()
      with contextlib.suppress(asyncio.CancelledError):
        await push_task

  async def _handle_chat(self, websocket) -> None:
    if self._state.status != HealthState.READY:
      await websocket.send(
        ChatError(error="unavailable", correlation_id=None).model_dump_json()
      )
      await websocket.close(1013, "unavailable")
      return

    log_once(self._log, "info", "chat_connected")
    try:
      async for message in websocket:
        response = await self._process_chat_message(message)
        await websocket.send(response)
    except ConnectionClosed:
      return

  async def _process_chat_message(self, message: str | bytes) -> str:
    if isinstance(message, bytes):
      message = message.decode()
    try:
      data = json.loads(message)
    except json.JSONDecodeError:
      return ChatError(error="invalid_json").model_dump_json()

    try:
      request = ChatRequest.model_validate(data)
    except ValidationError as exc:
      return ChatError(
        error=f"invalid_request: {exc.errors()[0]['msg']}"
      ).model_dump_json()

    with bind_correlation_id(request.correlation_id):
      record = await self._queue.enqueue(
        kind=HEARING_KIND,
        source=CHAT_SOURCE,
        text=request.sentence.strip(),
      )
      if record is None:
        self._log.info("chat_rejected", reason="queue_full")
        return ChatError(
          error="queue_full",
          correlation_id=request.correlation_id,
        ).model_dump_json()

      self._log.info(
        "chat_accepted",
        observation_id=record.id,
        sentence_length=len(record.text),
      )
      return ChatAck(
        observation_id=record.id,
        timestamp=record.timestamp,
        correlation_id=request.correlation_id,
      ).model_dump_json()

  def _mission_snapshot(self) -> MissionTableSnapshot:
    """Project the mission table: running, pending by rank, recent finished."""
    assert self._missions is not None
    records = self._missions.records()
    running = [r for r in records if r.state == MissionState.RUNNING]
    pending = sorted(
      self._missions.pending(),
      key=lambda r: (
        0 if r.origin == "operator" else 1,
        -r.priority,
        r.submitted_at.timestamp(),
      ),
    )
    finished = [
      r
      for r in records
      if r.state
      in (MissionState.SUCCEEDED, MissionState.FAILED, MissionState.CANCELLED)
    ][-20:]
    plan: PlanProjection | None = None
    live = self._current_plan() if self._current_plan is not None else None
    if running and live is not None and live.mission_id == running[0].id:
      plan = _plan_projection(live)
    return MissionTableSnapshot(
      missions=[
        MissionTableEntry(
          mission_id=r.id,
          order=r.order,
          mission_template_id=r.mission_template_id,
          mission_template_version=r.mission_template_version,
          origin=r.origin,
          class_rank=0 if r.origin == "operator" else 1,
          state=r.state,
          priority_band=r.priority_band,
          priority=r.priority,
          urgency=r.urgency,
          value=r.value,
          est_cost_s=r.est_cost_s,
          preempted_count=r.preempted_count,
          submitted_at=r.submitted_at,
          started_at=r.started_at,
          finished_at=r.finished_at,
          result=r.result,
          error=r.error,
          document=_document_projection(self._missions.document(r.id)),
        )
        for r in [*running, *pending, *finished]
      ],
      history=[
        _mission_history_entry(entry, self._missions.document(entry.mission_id))
        for entry in self._missions.history()
      ],
      plan=plan,
    )

  async def _handle_missions_command(self, websocket) -> None:
    """Accept typed operator create and pending-mission mutation commands."""
    if self._missions is None or self._state.status != HealthState.READY:
      await websocket.send(MissionCommandError(error="unavailable").model_dump_json())
      await websocket.close(1013, "unavailable")
      return
    log_once(self._log, "info", "missions_command_connected")
    try:
      async for message in websocket:
        await websocket.send(await self._process_missions_command(message))
    except ConnectionClosed:
      return

  async def _process_missions_command(self, message: str | bytes) -> str:
    if isinstance(message, bytes):
      try:
        message = message.decode()
      except UnicodeDecodeError:
        return MissionCommandError(error="invalid_json").model_dump_json()
    try:
      data = json.loads(message)
    except json.JSONDecodeError:
      return MissionCommandError(error="invalid_json").model_dump_json()

    operation = (
      data.get("operation")
      if isinstance(data, dict)
      and data.get("operation") in {"create", "update", "priority", "cancel"}
      else None
    )
    correlation_id = (
      data.get("correlation_id")
      if isinstance(data, dict) and isinstance(data.get("correlation_id"), str)
      else None
    )
    try:
      request = MissionCommandRequest.model_validate(data).root
    except ValidationError as exc:
      return MissionCommandError(
        operation=operation,
        correlation_id=correlation_id,
        error="invalid_request",
        reason=exc.errors()[0]["msg"],
      ).model_dump_json()

    assert self._missions is not None
    with bind_correlation_id(request.correlation_id):
      try:
        if isinstance(request, MissionCreateRequest):
          record = await self._missions.submit(
            request.order,
            source=MISSION_COMMAND_SOURCE,
            correlation_id=request.correlation_id,
            origin="operator",
            priority_band=request.priority_band,
          )
          if record is None:
            return MissionCommandError(
              operation=request.operation,
              correlation_id=request.correlation_id,
              error="queue_full",
            ).model_dump_json()
        elif isinstance(request, MissionUpdateRequest):
          record = self._missions.update_order(
            request.mission_id,
            request.order,
            expected_revision=request.expected_revision,
          )
        elif isinstance(request, MissionPriorityRequest):
          record = self._missions.reprioritize(
            request.mission_id,
            request.priority_band,
            expected_revision=request.expected_revision,
          )
        else:
          record = self._missions.cancel(
            request.mission_id,
            expected_revision=request.expected_revision,
            reason="cancelled by operator",
          )
      except MissionMutationError as exc:
        return MissionCommandError(
          operation=request.operation,
          correlation_id=request.correlation_id,
          error=cast(MissionCommandErrorCode, exc.code),
          reason=str(exc),
        ).model_dump_json()
      except MissionStoreError:
        self._log.exception(
          "mission_command_storage_failed", operation=request.operation
        )
        return MissionCommandError(
          operation=request.operation,
          correlation_id=request.correlation_id,
          error="storage_failure",
        ).model_dump_json()
      except Exception:  # noqa: BLE001 - keep internal failures off the wire
        self._log.exception("mission_command_failed", operation=request.operation)
        return MissionCommandError(
          operation=request.operation,
          correlation_id=request.correlation_id,
          error="internal_failure",
        ).model_dump_json()

      document = self._missions.document(record.id)
      if document is None:
        self._log.error("mission_command_document_missing", mission_id=record.id)
        return MissionCommandError(
          operation=request.operation,
          correlation_id=request.correlation_id,
          error="internal_failure",
        ).model_dump_json()
      return MissionCommandAck(
        operation=request.operation,
        correlation_id=request.correlation_id,
        mission=MissionCommandMission(
          mission_id=record.id,
          order=record.order,
          state=record.state,
          priority_band=record.priority_band,
          revision=document.revision,
        ),
      ).model_dump_json()

  async def _handle_missions_stream(self, websocket) -> None:
    """Stream live mission-table snapshots: on connect, then per mutation."""
    missions = self._missions
    if missions is None or self._state.status != HealthState.READY:
      await self._reject_unavailable(websocket)
      return
    log_once(self._log, "info", "missions_stream_connected")
    changed = asyncio.Event()
    changed.set()  # the first frame is the on-connect snapshot
    unsubscribe = missions.add_listener(changed.set)
    try:
      while True:
        wait_change = asyncio.create_task(changed.wait())
        close_task = asyncio.create_task(websocket.wait_closed())
        done, _pending = await asyncio.wait(
          {wait_change, close_task}, return_when=asyncio.FIRST_COMPLETED
        )
        for task in (wait_change, close_task):
          if not task.done():
            task.cancel()
        if close_task in done:
          return
        changed.clear()
        await websocket.send(self._mission_snapshot().model_dump_json())
    except ConnectionClosed:
      return
    finally:
      unsubscribe()

  async def _handle_invoke(self, websocket) -> None:
    """Operator direct primitive call — same gate as every planner step."""
    if self._gate is None or self._state.status != HealthState.READY:
      await websocket.send(
        PrimitiveInvokeError(error="unavailable", correlation_id=None).model_dump_json()
      )
      await websocket.close(1013, "unavailable")
      return

    log_once(self._log, "info", "invoke_connected")
    try:
      async for message in websocket:
        response = await self._process_invoke_message(message)
        await websocket.send(response)
    except ConnectionClosed:
      return

  async def _process_invoke_message(self, message: str | bytes) -> str:
    if isinstance(message, bytes):
      message = message.decode()
    try:
      data = json.loads(message)
    except json.JSONDecodeError:
      return PrimitiveInvokeError(error="invalid_json").model_dump_json()

    try:
      request = PrimitiveInvokeRequest.model_validate(data)
    except ValidationError as exc:
      return PrimitiveInvokeError(
        error=f"invalid_request: {exc.errors()[0]['msg']}",
        correlation_id=data.get("correlation_id") if isinstance(data, dict) else None,
      ).model_dump_json()

    assert self._gate is not None
    with bind_correlation_id(request.correlation_id):
      invocation = ActionInvocation(
        action=request.primitive,
        robot_id=request.robot_id,
        device_id=request.device_id,
        arguments=request.arguments,
        intent="operator invoke",
      )
      try:
        verdict = await asyncio.wait_for(
          self._gate.execute(invocation, StepContext(origin="operator")),
          timeout=self._step_timeout_s,
        )
      except TimeoutError:
        self._log.warning(
          "invoke_timeout", primitive=request.primitive, timeout_s=self._step_timeout_s
        )
        return PrimitiveInvokeError(
          error="timeout", correlation_id=request.correlation_id
        ).model_dump_json()
      if not verdict.admitted:
        self._log.info(
          "invoke_rejected", primitive=request.primitive, reason=verdict.reason
        )
        return PrimitiveInvokeError(
          error="rejected_by_gate",
          reason=verdict.reason,
          correlation_id=request.correlation_id,
        ).model_dump_json()
      if verdict.ack is None:
        return PrimitiveInvokeError(
          error="egress_failed",
          reason=verdict.reason,
          correlation_id=request.correlation_id,
        ).model_dump_json()
      outcome = (
        f"accepted (sequence {verdict.ack.sequence})"
        if verdict.ack.accepted
        else f"rejected by backend ({verdict.ack.reason})"
      )
      # Admitted or not, the operator's call is world history like any step.
      await self._queue.enqueue(
        kind=RESULT_KIND,
        source=INVOKE_SOURCE,
        text=f"{request.primitive}: {outcome}",
        metadata={
          "primitive": request.primitive,
          "robot_id": request.robot_id,
          "device_id": request.device_id or "",
          "accepted": str(verdict.ack.accepted).lower(),
        },
      )
      self._log.info("invoke_executed", primitive=request.primitive, outcome=outcome)
      if verdict.ack.command_id is None:
        return PrimitiveInvokeError(
          error="egress_failed",
          reason="backend acknowledgement omitted command_id",
          correlation_id=request.correlation_id,
        ).model_dump_json()
      return PrimitiveInvokeAck(
        accepted=verdict.ack.accepted,
        command_id=verdict.ack.command_id,
        status=verdict.ack.status,
        sequence=verdict.ack.sequence,
        result=verdict.ack.result,
        reason=verdict.ack.reason if not verdict.ack.accepted else None,
        reason_code=verdict.ack.reason_code,
        correlation_id=request.correlation_id,
      ).model_dump_json()

  async def _handle_review(self, websocket) -> None:
    """Operator review route — promote a concluded plan into reference memory."""
    if (
      self._episodic is None
      or self._reference is None
      or self._state.status != HealthState.READY
    ):
      await websocket.send(
        ReviewError(error="unavailable", correlation_id=None).model_dump_json()
      )
      await websocket.close(1013, "unavailable")
      return

    log_once(self._log, "info", "review_connected")
    try:
      async for message in websocket:
        response = await self._process_review_message(message)
        await websocket.send(response)
    except ConnectionClosed:
      return

  async def _process_review_message(self, message: str | bytes) -> str:
    if isinstance(message, bytes):
      message = message.decode()
    try:
      data = json.loads(message)
    except json.JSONDecodeError:
      return ReviewError(error="invalid_json").model_dump_json()

    try:
      request = ReviewRequest.model_validate(data)
    except ValidationError as exc:
      return ReviewError(
        error=f"invalid_request: {exc.errors()[0]['msg']}",
        correlation_id=data.get("correlation_id") if isinstance(data, dict) else None,
      ).model_dump_json()

    assert self._episodic is not None
    assert self._reference is not None
    with bind_correlation_id(request.correlation_id):
      plan = self._episodic.get_plan(request.plan_id)
      if plan is None:
        self._log.info(
          "review_rejected", reason="plan_not_found", plan_id=request.plan_id
        )
        return ReviewError(
          error="plan_not_found",
          correlation_id=request.correlation_id,
        ).model_dump_json()
      record = self._reference.promote(plan, outcome=request.outcome, note=request.note)
      assert record.reviewed_at is not None
      self._log.info(
        "plan_reviewed",
        plan_id=record.id,
        outcome=record.outcome.value,
      )
      return ReviewAck(
        plan_id=record.id,
        outcome=record.outcome.value,
        reviewed_at=record.reviewed_at,
        correlation_id=request.correlation_id,
      ).model_dump_json()

  async def _send_status(self, websocket, correlation_id: str | None = None) -> None:
    payload = _build_status(self._state, self._log, correlation_id, self._safety)
    await websocket.send(payload.model_dump_json())
    log_once(
      self._log,
      "debug",
      "health_served",
      status=payload.status.value,
      uptime_seconds=payload.uptime_seconds,
    )

  async def _periodic_push(self, websocket) -> None:
    try:
      while True:
        await asyncio.sleep(self._status_interval)
        await self._send_status(websocket)
    except ConnectionClosed:
      return


HealthServer = NorthboundServer


def _parse_correlation_id(message: str | bytes) -> str | None:
  if isinstance(message, bytes):
    message = message.decode()
  message = message.strip()
  if not message:
    return None
  try:
    data = json.loads(message)
  except json.JSONDecodeError:
    return None
  if not isinstance(data, dict):
    return None
  value = data.get("correlation_id")
  return value if isinstance(value, str) else None
