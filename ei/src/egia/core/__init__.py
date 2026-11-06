"""EGIA decision pipeline — the implementation layer launched after ``egia/lib``.

The composition root (:mod:`egia.main`, launched by ``bin/egia``) configures
``egia/lib`` first, then calls :func:`launch` here. This module wires the
pipeline in dependency order — ``link`` (southbound) feeds ``perception``,
which feeds ``queue`` and then ``context``, while ``egia.io`` projects the
result northbound to WebUI. As the wiring layer, this module is the one place
inside the tree that imports ``egia.io`` (io.md's composition-root allowance;
core never imports io otherwise). The supervisor starts the subsystems,
reports readiness, then blocks until SIGINT/SIGTERM and shuts them down in
reverse order.
"""

from __future__ import annotations

import asyncio
import contextlib
import signal
from collections.abc import Mapping
from datetime import UTC, datetime
from importlib.metadata import PackageNotFoundError
from importlib.metadata import version as pkg_version
from pathlib import Path

from egia.api.mission import MissionRecord, MissionState
from egia.api.primitives import PrimitivesSnapshot
from egia.api.prompts import PromptSection, PromptsSnapshot
from egia.api.skills import SkillsSnapshot
from egia.api.status import HealthState, SafetyStatus
from egia.contracts.endpoints import ResourceKind
from egia.core.context import ContextBuilder, ModeState, context_entry, render_hardware
from egia.core.context.mode import BackendMode
from egia.core.decision import DecisionPlanner, decision_entry
from egia.core.decision.model import (
  AvailabilityModel,
  ModelAvailabilityGate,
  VLMModel,
)
from egia.core.decision.model_io import ModelIOStore
from egia.core.decision.proposer import proposer_entry
from egia.core.decision.sentinel import SafetySentinel
from egia.core.link import (
  BackendStatusTracker,
  ClientRegistry,
  EosClient,
  EosDiscovery,
  backend_section_renderers,
)
from egia.core.memory import (
  EpisodicStore,
  LocalMemoryStore,
  RecallService,
  ReferenceStore,
  WorkingStore,
  index_docs_corpus,
  load_docs_corpus,
  memory_consolidation_entry,
)
from egia.core.missions import MissionQueue, MissionStore
from egia.core.missions.document import PlanState
from egia.core.missions.store import MissionCheckpoint
from egia.core.monitor import (
  BoundWatchdog,
  EstopLatch,
  PhysicalBounds,
  RateWindow,
  monitor_entry,
)
from egia.core.perception.admission import Admission
from egia.core.perception.body import parts_perception_entry
from egia.core.perception.distribution import DistributionGate
from egia.core.registry import PrimitiveRegistry
from egia.core.safety import SafetyGate, safety_audit_entry
from egia.core.supervisor import SubsystemSpec, Supervisor
from egia.internal.backend import BackendSessionHandoff
from egia.internal.recall import RecallIndex
from egia.io import (
  ContextStore,
  NorthboundServer,
  PartStore,
  PrimitivesStore,
  PromptsStore,
  ServerState,
  SkillsStore,
)
from egia.io.catalog import (
  primitives_snapshot,
  prompts_snapshot,
  render_skills,
  skills_snapshot,
)
from egia.lib import shared_events
from egia.lib.config import (
  LaunchConfig,
  load_model_config,
  load_model_selection,
  load_safety_config,
  resolve_etc_root,
)
from egia.lib.logger import (
  get_logger,
  log_once,
  restore_console_level,
  show_lifecycle_logs,
)
from egia.lib.safety_state import ResetAuthorization, SafetyState
from egia.stages import StagePayload, StageRuntime, StageValue

_WAITING_FOR_BACKENDS = (
  "No backend session is active. No perception or primitives are bound. "
  "Waiting for backend connections."
)
_EMPTY_LAUNCH = (
  "No backend or sandbox is enabled. Model execution and device routes are disabled."
)


def _version() -> str:
  try:
    return pkg_version("egia")
  except PackageNotFoundError:
    return "0.0.0+local"


async def launch_empty(
  config: LaunchConfig,
  *,
  state_dir: Path | str | None = None,
  external_stop: asyncio.Event | None = None,
  sandbox_media_socket: str | None = None,
) -> int:
  """Serve the intentional ready-empty mode without constructing a model."""
  log = get_logger("egia.core")
  state_root = Path(state_dir or "var/lib/egia")
  state = ServerState(version=_version())
  queue = Admission(
    config.perception.capacity, history_per_kind=config.perception.history_per_kind
  )
  primitives = PrimitivesStore()
  await primitives.publish(PrimitivesSnapshot())
  skills = SkillsStore()
  await skills.publish(SkillsSnapshot())
  prompts = PromptsStore()
  await prompts.publish(
    PromptsSnapshot(
      system=_EMPTY_LAUNCH,
      sections=[
        PromptSection(name="empty_launch", text=_EMPTY_LAUNCH, lines=[_EMPTY_LAUNCH])
      ],
    )
  )
  server = NorthboundServer(
    state,
    queue,
    host=config.server.host,
    port=config.server.port,
    status_frequency_hz=config.server.status_frequency_hz,
    mode="empty",
    backend=list,
    primitives_store=primitives,
    skills_store=skills,
    prompts_store=prompts,
    sandbox_media_socket=sandbox_media_socket,
  )
  stop = external_stop or asyncio.Event()
  loop = asyncio.get_running_loop()
  for sig in (signal.SIGINT, signal.SIGTERM):
    loop.add_signal_handler(sig, stop.set)
  subsystem_specs: list[SubsystemSpec] = []
  reference = None
  if config.launch.memory:
    episodic = EpisodicStore(state_root / "memory" / "episodic.db")
    working = WorkingStore(state_root / "memory" / "working.db")
    reference = ReferenceStore(state_root / "memory" / "reference.db")

    async def _memory(ready: asyncio.Event, subsystem_stop: asyncio.Event) -> None:
      await memory_consolidation_entry(queue, episodic, working, ready, subsystem_stop)

    subsystem_specs.append(SubsystemSpec(name="memory", entry=_memory))
  supervisor = Supervisor(subsystem_specs)
  state.supervisor = supervisor
  await server.start()
  await supervisor.start()
  state.status = (
    HealthState.READY if await supervisor.wait_ready() else HealthState.DEGRADED
  )
  log.info("empty_launch_ready", memory=config.launch.memory)
  await stop.wait()
  state.status = HealthState.STOPPING
  from egia.lib import shared_events

  shared_events.emit("egia_stopping")
  await supervisor.stop()
  if reference is not None:
    reference.close()
  await server.stop()
  log.info("pipeline_stopped", phase="empty")
  return 0


# One-line identity for each subsystem, logged as ``module_start`` when the
# subsystem (re)starts — the human log for a module should open with what
# the module is, not just its state transitions.
_MODULE_ROLES = {
  "parts": "part streams + vision detections into the queue + scene exchange",
  "objects": "semantic world-object snapshots into model context",
  "context": "assemble the model-facing context prompt from queue + memory",
  "decision": "dual-model mission planner: revise, act, and annotate",
  "proposer": "autonomous mission origination from perception (§3.9)",
  "safety": "safety gate + periodic audit with e-stop authority",
  "monitor": "backend liveness probes and failure annotation",
  "memory": "episodic/working memory consolidation between missions",
}


async def launch(
  config: LaunchConfig,
  *,
  stage_runtime: StageRuntime | None = None,
  managed: bool = False,
  reset_authorization: ResetAuthorization | None = None,
  reset_checked: bool = False,
  state_dir: Path | str | None = None,
  var_dir: Path | str | None = None,
  etc: str | None = None,
  defaults_root: Path | str | None = None,
  subsystems: list[SubsystemSpec] | None = None,
  recall_index: RecallIndex | None = None,
  external_stop: asyncio.Event | None = None,
  session_handoff: BackendSessionHandoff | None = None,
  sandbox_media_socket: str | None = None,
) -> int:
  """Start the pipeline supervisor; run until a termination signal arrives.

  ``recall_index`` is the optional §3.2 vector-recall seam: the composition
  root builds it from ``opt/memory`` when configured and available, and
  passes ``None`` otherwise — recall then degrades to keyword search.
  ``state_dir`` is the injected durable EGIA root. ``var_dir`` remains as a
  compatibility alias for programmatic callers that have not migrated yet.
  ``defaults_root`` is the immutable factory configuration root.
  """
  log = get_logger("egia.core")
  if state_dir is not None and var_dir is not None:
    raise ValueError("pass state_dir or legacy var_dir, not both")
  state_root = Path(state_dir or var_dir or "var/lib/egia")
  if not reset_checked:
    reset_authorization = SafetyState(state_root / "safety").consume_reset()
  etc_root = resolve_etc_root(etc)
  state = ServerState(version=_version())
  queue = Admission(
    config.perception.capacity, history_per_kind=config.perception.history_per_kind
  )
  # Multi-backend: one client and tracker per configured endpoint, all ingesting
  # into the one admission queue and
  # scene exchange below. An empty endpoint list synthesizes the default
  # single endpoint from the mode-level tables — today's shape.
  endpoints = config.endpoints()
  backend_clients = ClientRegistry()
  backend_trackers: dict[str, BackendStatusTracker] = {}
  for ep in endpoints:
    backend_clients.register(
      ep.id,
      EosClient(
        ep.gateway,
        backend=ep.id,
        expected_revision=(stage_runtime.source_revision if stage_runtime else None),
      ),
    )
    backend_trackers[ep.id] = BackendStatusTracker(
      ep.gateway.endpoint,
      failure_threshold=config.monitor.failure_threshold,
      backend=ep.id,
      name=ep.name,
      description=ep.intro,
    )

  # Every launch exposes an empty sandbox-facing phase first. Northbound is
  # alive, but perception and primitives remain unbound until every
  # configured EOS endpoint completes mandatory discovery. Connection
  # failures warn once per endpoint while the daemon retries indefinitely.
  waiting_primitives = PrimitivesStore()
  await waiting_primitives.publish(PrimitivesSnapshot())
  waiting_skills = SkillsStore()
  await waiting_skills.publish(SkillsSnapshot())
  waiting_prompts = PromptsStore()
  await waiting_prompts.publish(
    PromptsSnapshot(
      system=_WAITING_FOR_BACKENDS,
      sections=[
        PromptSection(
          name="waiting_for_backends",
          text=_WAITING_FOR_BACKENDS,
          lines=[_WAITING_FOR_BACKENDS],
        )
      ],
    )
  )
  waiting_server = NorthboundServer(
    state,
    queue,
    host=config.server.host,
    port=config.server.port,
    status_frequency_hz=config.server.status_frequency_hz,
    mode="sandbox",
    backend=lambda: [tracker.snapshot() for tracker in backend_trackers.values()],
    primitives_store=waiting_primitives,
    skills_store=waiting_skills,
    prompts_store=waiting_prompts,
    sandbox_media_socket=sandbox_media_socket,
  )
  loop = asyncio.get_running_loop()
  stop = external_stop or asyncio.Event()
  for sig in (signal.SIGINT, signal.SIGTERM):
    loop.add_signal_handler(sig, stop.set)
  state.status = HealthState.DEGRADED
  await waiting_server.start()
  log.info("sandbox_mode_entered", status="waiting_for_backend_connections")

  pending = {ep.id for ep in endpoints}
  while pending and not stop.is_set():
    for ep in endpoints:
      if ep.id not in pending or stop.is_set():
        continue
      client = backend_clients.client(ep.id)
      try:
        discovery = await client.initialize(force=True)
      except Exception as exc:  # noqa: BLE001 - daemon owns connection retries
        backend_trackers[ep.id].record_failure()
        log_once(
          log,
          "warning",
          "backend_connection_waiting",
          key=f"backend_connection_waiting:{ep.id}",
          backend=ep.id,
          endpoint=ep.gateway.endpoint,
          reason=str(exc),
        )
      else:
        backend_trackers[ep.id].update_runtime(discovery)
        backend_trackers[ep.id].record_success()
        pending.discard(ep.id)
        log.info(
          "backend_connected",
          backend=ep.id,
          endpoint=ep.gateway.endpoint,
          eos_robot_id=discovery.robot_id,
        )
    if pending and not stop.is_set():
      try:
        await asyncio.wait_for(stop.wait(), timeout=config.monitor.heartbeat_interval_s)
      except TimeoutError:
        pass

  if stop.is_set():
    state.status = HealthState.STOPPING
    await waiting_server.stop()
    for client in backend_clients.all().values():
      await client.aclose()
    log.info("pipeline_stopped", phase="waiting_for_backend_connections")
    return 0

  state.status = HealthState.STARTING
  await waiting_server.stop()
  log.info("backend_connections_ready", backends=backend_clients.ids())

  endpoint_resources = {}
  pending_hardware: list[tuple[str, EosDiscovery]] = []
  for ep in endpoints:
    client = backend_clients.client(ep.id)
    discovery = client.discovery
    endpoint_resources[ep.id] = client.resources
    if stage_runtime is None:
      await queue.enqueue(
        kind="hardware",
        source=f"eos.{ep.id}",
        text=render_hardware(ep.id, discovery.robot_model, discovery.devices),
        metadata={"robot_id": ep.id, "eos_robot_id": discovery.robot_id},
      )
    else:
      pending_hardware.append((ep.id, discovery))
  context_store = ContextStore()
  # One exchange per standalone robot part configured in any endpoint's
  # [resources.parts]. Parts absent everywhere get no store, so their
  # northbound streams answer ``unavailable`` — a part the pipeline does
  # not serve.
  part_stores: dict[ResourceKind, PartStore] = {
    kind: PartStore()
    for resources in endpoint_resources.values()
    for kind in {item.kind for item in resources}
  }
  # The first distribution gate: typed read ports over the shared exchanges
  # (monitor/safety consumers arrive with phase 7 — the ports exist and are
  # typed now so they attach here without touching the consumers).
  distribution_gate = DistributionGate(queue, part_stores)
  log.debug(
    "distribution_gate_wired",
    parts=sorted(kind.value for kind in distribution_gate.context_port().part_stores),
  )

  # Main chain: registry and gate sit below decision; the planner never
  # touches link or contracts itself. Every remote entry comes from the EOS
  # primitive registry discovered during mandatory endpoint initialization.
  registry = PrimitiveRegistry()
  for ep in endpoints:
    registry.extend(
      PrimitiveRegistry.from_primitives(
        ep.id,
        backend_clients.client(ep.id).primitives,
        expose_operator=config.primitives.expose_operator,
      )
    )

  def _refresh_hardware(refreshed, alias: str) -> None:
    discovery = refreshed.discovery
    if stage_runtime is not None and stage_runtime.active:
      asyncio.create_task(_publish_hardware(alias, discovery))
      return
    asyncio.create_task(
      queue.enqueue(
        kind="hardware",
        source=f"eos.{alias}",
        text=render_hardware(alias, discovery.robot_model, discovery.devices),
        metadata={"robot_id": alias, "eos_robot_id": discovery.robot_id},
      )
    )

  for ep in endpoints:
    client = backend_clients.client(ep.id)
    client.add_refresh_listener(
      lambda refreshed, alias=ep.id: registry.replace_primitives(
        alias,
        refreshed.primitives,
        expose_operator=config.primitives.expose_operator,
      )
    )
    client.add_refresh_listener(
      lambda refreshed, tracker=backend_trackers[ep.id]: tracker.update_runtime(
        refreshed.discovery
      )
    )
    client.add_refresh_listener(
      lambda refreshed, alias=ep.id: _refresh_hardware(refreshed, alias)
    )
  gate_config = load_safety_config(etc_root, defaults=defaults_root)
  local_memory = LocalMemoryStore(state_root / "context_archive.jsonl")
  mission_store = MissionStore(state_root / "missions")
  if session_handoff is None and not managed and not config.decision.mission_preload:
    mission_store.clear()

  def _fail_unrecoverable_coordination(checkpoint: MissionCheckpoint) -> None:
    mission = checkpoint.mission.model_copy(deep=True)
    mission.state = MissionState.FAILED
    mission.finished_at = datetime.now(UTC)
    mission.error = (
      "coordination mission interrupted; execution state cannot be resumed safely"
    )
    document = checkpoint.document.model_copy(deep=True)
    document.state = MissionState.FAILED
    document.reason = mission.error
    if document.plan is not None:
      document.plan.state = PlanState.FAILED
      document.plan.reason = mission.error
    mission_store.save_mission(mission, document, checkpoint.revisions)
    from egia.lib import shared_events

    shared_events.emit(
      "egia_recovery_required",
      fields={"reason_code": "checkpoint_incomplete", "mission_id": mission.id},
    )
    shared_events.emit(
      "egia_mission_restored",
      fields={"mission_id": mission.id, "outcome": "failed_closed"},
      correlation_id=mission.id,
    )
    local_memory.archive_mission(mission)
    log.error(
      "coordination_mission_recovery_rejected",
      mission_id=mission.id,
      reason=mission.error,
    )

  def _archive_finished_mission(record: MissionRecord) -> None:
    local_memory.archive_mission(record)

  missions = MissionQueue(
    config.decision.mission_capacity,
    priority=config.decision.priority,
    on_finished=_archive_finished_mission,
    store=mission_store,
  )
  mission_changed = asyncio.Event()
  missions.add_listener(mission_changed.set)
  durable_checkpoints = mission_store.load_active()
  if config.decision.coordination.enabled:
    for checkpoint in durable_checkpoints:
      if checkpoint.mission.state is MissionState.RUNNING:
        _fail_unrecoverable_coordination(checkpoint)
    durable_checkpoints = [
      checkpoint
      for checkpoint in durable_checkpoints
      if checkpoint.mission.state is MissionState.PENDING
    ]
  resumed_plans = missions.restore_checkpoints(durable_checkpoints)
  for checkpoint in durable_checkpoints:
    shared_events.emit(
      "egia_mission_restored",
      fields={"mission_id": checkpoint.mission.id, "outcome": "pending"},
      correlation_id=checkpoint.mission.id,
    )
  if durable_checkpoints:
    log.info(
      "mission_checkpoints_restored",
      missions=len(durable_checkpoints),
      plans=len(resumed_plans),
    )
  if session_handoff is not None:
    retained_missions, handoff_plans = session_handoff.consume()
    if config.decision.coordination.enabled:
      unsafe_ids = {
        mission.id
        for mission in retained_missions
        if mission.state is MissionState.RUNNING
      }
      for mission_id in unsafe_ids:
        checkpoint = mission_store.load(mission_id)
        if checkpoint is not None:
          _fail_unrecoverable_coordination(checkpoint)
      retained_missions = [
        mission
        for mission in retained_missions
        if mission.state is MissionState.PENDING
      ]
      handoff_plans = {
        mission_id: plan
        for mission_id, plan in handoff_plans.items()
        if mission_id not in unsafe_ids
      }
    resumed_plans.update(handoff_plans)
    retained_observations, retained_history = session_handoff.consume_observations()
    queue.restore(retained_observations, retained_history)
    missions.restore(retained_missions)
    for retained_plan in resumed_plans.values():
      if missions.document(retained_plan.mission_id) is not None:
        missions.sync_plan(retained_plan, active=False)
    if retained_missions:
      log.info(
        "backend_session_restored",
        revision=session_handoff.revision,
        missions=len(retained_missions),
      )
  # Monitor-owned safety state the whole pipeline shares: the estop latch
  # (emergency egress is monitor → link direct, fleet-wide over every
  # registered backend) and the egress rate window.
  from egia.lib.operation_ledger import OperationLedger

  operation_ledger = OperationLedger(state_root)
  unresolved_operations = False
  for pending_operation in operation_ledger.pending():
    try:
      recovered = await backend_clients.client(
        pending_operation.alias
      ).recover_operation(
        pending_operation.operation_id,
        pending_operation.primitive,
        pending_operation.device_id,
        eos_robot_id=pending_operation.eos_robot_id,
        graph_revision=pending_operation.graph_revision,
      )
      if recovered.status.value not in {"succeeded", "failed", "cancelled", "rejected"}:
        raise RuntimeError("EOS operation remains physically uncertain")
      operation_ledger.settle(pending_operation.operation_id, recovered.status.value)
      shared_events.emit(
        "egia_operation_terminal",
        fields={
          "operation_id": pending_operation.operation_id,
          "outcome": recovered.status.value,
        },
        correlation_id=pending_operation.operation_id,
      )
    except Exception as exc:  # noqa: BLE001 - no replay after an uncertain checkpoint
      unresolved_operations = True
      log.error(
        "operation_recovery_unresolved",
        operation_id=pending_operation.operation_id,
        reason=str(exc),
      )
      shared_events.emit(
        "egia_operation_unknown",
        fields={
          "operation_id": pending_operation.operation_id,
          "reason_code": "receipt_missing",
        },
        correlation_id=pending_operation.operation_id,
      )
      shared_events.emit(
        "egia_recovery_required",
        fields={"reason_code": "unknown_outcome"},
        correlation_id=pending_operation.operation_id,
      )
  latch = EstopLatch(
    backend_clients,
    state_root,
    graph_revision=stage_runtime.source_revision if stage_runtime is not None else None,
  )
  if unresolved_operations and not latch.engaged:
    await latch.engage("eos_unknown")
  # Consume one-shot authorization even if the latch file is absent; a stale
  # request must never become usable after a later, unrelated emergency.
  await latch.consume_authorized_reset(
    reset_authorization, unresolved_operations=unresolved_operations
  )
  rate_window = RateWindow(gate_config.rate.max_calls_per_minute)
  model_availability = ModelAvailabilityGate()
  gate = SafetyGate(
    registry,
    backend_clients,
    latch,
    missions,
    queue,
    gate_config,
    rate_admit=rate_window.admit,
    availability_wait=model_availability.wait_available,
    operation_ledger=operation_ledger,
  )
  watchdog = BoundWatchdog(
    PhysicalBounds(
      v_max_m_s=config.monitor.v_max_m_s,
      omega_max_rad_s=config.monitor.omega_max_rad_s,
      escalation_ticks=config.monitor.bound_escalation_ticks,
    ),
    queue,
    latch,
  )
  if config.launch.memory:
    episodic = EpisodicStore(state_root / "memory" / "episodic.db")
    working = WorkingStore(state_root / "memory" / "working.db")
    reference = ReferenceStore(state_root / "memory" / "reference.db")
    docs = load_docs_corpus(config.memory.docs_corpus)
    if recall_index is not None and docs:
      index_docs_corpus(recall_index, docs)
    recall = RecallService(episodic, reference, recall_index, docs)
    for checkpoint in durable_checkpoints:
      # Mission JSON is the recovery authority. A stale working row for the
      # same mission must not be archived as an aborted historical plan while
      # the restored mission waits its turn in the pending table.
      working.drop(checkpoint.mission.id)
  else:
    # [launch] memory = false (§3.2): the master switch is off — no stores
    # opened, no docs corpus, no recall; the pipeline runs without memory.
    log.info("memory_disabled")
    episodic = None
    working = None
    reference = None
    recall = None
  selected_model = load_model_selection(str(etc_root), defaults=defaults_root) or None
  model = AvailabilityModel(
    VLMModel(load_model_config(str(etc_root), selected_model, defaults=defaults_root)),
    model_availability,
  )
  controller_model = AvailabilityModel(
    VLMModel(
      load_model_config(
        str(etc_root), config.decision.controller.model, defaults=defaults_root
      )
    ),
    model_availability,
  )
  reasoner_model = AvailabilityModel(
    VLMModel(
      load_model_config(
        str(etc_root), config.decision.reasoner.model, defaults=defaults_root
      )
    ),
    model_availability,
  )
  model_io_store = ModelIOStore()
  sentinel = None
  if config.decision.sentinel.enabled:
    sentinel_model = AvailabilityModel(
      VLMModel(
        load_model_config(
          str(etc_root),
          config.decision.sentinel.model or selected_model,
          defaults=defaults_root,
        )
      ),
      model_availability,
    )
    sentinel = SafetySentinel(
      sentinel_model,
      queue,
      config.decision.sentinel,
      config.prompts.sentinel,
      token_budget=config.prompts.token_budget,
      motion_stop=latch.motion_stop,
      engage_estop=latch.engage,
      exchange_sink=model_io_store.publish,
    )
  decision_prompt = config.prompts.planner
  decision_instruction = prompts_snapshot(
    decision_prompt.instruction, decision_prompt.sections
  )
  primitives_store = PrimitivesStore()
  await primitives_store.publish(primitives_snapshot(registry))
  skills_store = SkillsStore()
  await skills_store.publish(skills_snapshot(backend_clients.all()))

  def _refresh_catalogs(_refreshed) -> None:
    asyncio.create_task(primitives_store.publish(primitives_snapshot(registry)))
    asyncio.create_task(skills_store.publish(skills_snapshot(backend_clients.all())))

  for ep in endpoints:
    backend_clients.client(ep.id).add_refresh_listener(_refresh_catalogs)
  prompts_store = PromptsStore()
  await prompts_store.publish(decision_instruction)
  backend_mode: BackendMode = "sandbox" if config.mode == "sandbox" else "live"
  mode_state = ModeState(backend_mode)
  context_builder = ContextBuilder(
    queue,
    config.prompts,
    backend_render=backend_section_renderers(backend_trackers),
    mode_state=mode_state,
    dynamic_renderers={
      "missions": missions.render_for_prompt,
      "primitives": registry.render_for_prompt,
      "skills": lambda: render_skills(backend_clients.all(), registry),
    },
    local_memory=local_memory,
    system_prompt=decision_instruction,
    change_frequency_path=state_root / "context_order.json",
  )

  def _sync_plan(record, active: bool) -> None:
    missions.sync_plan(record, active=active)
    if not active:
      local_memory.archive_plan(record)

  planner = DecisionPlanner(
    controller_model,
    context_builder,
    registry,
    gate,
    queue,
    episodic,
    working,
    reference,
    decision_prompt,
    config.decision,
    recall=recall,
    exchange_sink=model_io_store.publish,
    activity_sink=model_io_store.publish_activity,
    should_yield=missions.check_preemption,
    plan_sink=_sync_plan,
    sentinel=sentinel,
    resume_plan=lambda mission_id: resumed_plans.pop(mission_id, None),
    initial_plan=missions.plan_steps,
    reasoner_model=reasoner_model,
    reasoner_prompts=config.prompts.reasoner,
    summarizer_prompts=config.prompts.summarizer,
    skill_clients=backend_clients,
  )
  context_path: str | None = None
  stage_delivery_task: asyncio.Task[None] | None = None
  if stage_runtime is not None:
    owners = {
      "perception": queue,
      "context": context_builder,
      "missions": missions,
      "decision": planner,
      "safety": gate,
      "monitor": latch,
    }
    for path, instance in stage_runtime.composition.instances.items():
      owner = (
        backend_clients.client(instance.settings["backend_alias"])
        if instance.type.group == "link"
        else owners[instance.type.group]
      )
      stage_runtime.bind_owner(path, owner)
    for edge in stage_runtime.composition.edges:
      stage_runtime.bind(
        edge.target,
        edge.input,
        lambda value, node=edge.target, input_name=edge.input: stage_runtime.receive(
          node, input_name, value
        ),
      )
    stage_runtime.start()
    perception_path = next(
      path
      for path, instance in stage_runtime.composition.instances.items()
      if instance.type.group == "perception"
    )
    context_path = next(
      path
      for path, instance in stage_runtime.composition.instances.items()
      if instance.type.group == "context"
    )

    def _publish_observations() -> None:
      if not stage_runtime.active:
        return
      now = datetime.now(UTC)
      stage_runtime.publish(
        "observations",
        StageValue(
          "observations",
          f"observations-{queue.revision}",
          stage_runtime.run_id,
          perception_path,
          now,
          StagePayload(
            "observations",
            {"items": [record.model_dump(mode="json") for record in queue.records()]},
          ),
          received_at=now,
        ),
      )

    queue.add_listener(_publish_observations)
    link_path = next(
      path
      for path, instance in stage_runtime.composition.instances.items()
      if instance.type.group == "link"
    )

    async def _admit_resource(value: StageValue) -> None:
      fields = value.payload.fields
      sample = fields["sample"]
      assert isinstance(sample, Mapping)
      admitted = await queue.enqueue(
        kind="hardware",
        source=f"eos.{fields['backend']}",
        text=str(sample["text"]),
        metadata={
          "robot_id": str(fields["backend"]),
          "eos_robot_id": str(sample["eos_robot_id"]),
        },
      )
      if admitted is None:
        raise RuntimeError("perception rejected a full observation queue")

    stage_runtime.bind_async(perception_path, "resource", _admit_resource)
    stage_runtime.on_overflow = lambda edge, counters: log.warning(
      "stage_edge_overflow",
      source=edge.source,
      output=edge.output,
      target=edge.target,
      input=edge.input,
      delivered=counters.delivered,
      gaps=counters.gaps,
      overflow=counters.overflow,
    )
    stage_delivery_task = asyncio.create_task(stage_runtime.run_scheduler())

    async def _publish_hardware(alias: str, discovery: EosDiscovery) -> None:
      if not stage_runtime.active:
        return
      now = datetime.now(UTC)
      await stage_runtime.publish_async(
        "resource",
        StageValue(
          "resource",
          f"hardware-{alias}-{backend_clients.client(alias).generation}",
          stage_runtime.run_id,
          link_path,
          now,
          StagePayload(
            "resource",
            {
              "backend": alias,
              "reference": f"hardware/{alias}",
              "sample": {
                "text": render_hardware(
                  alias, discovery.robot_model, discovery.devices
                ),
                "eos_robot_id": discovery.robot_id,
              },
            },
          ),
          backend=alias,
          received_at=now,
        ),
      )

    for alias, discovery in pending_hardware:
      await _publish_hardware(alias, discovery)
    log.info(
      "stage_runtime_started",
      stages=sorted(stage_runtime.composition.instances),
      edges=len(stage_runtime.composition.edges),
    )

  def _interrupt_for_preemption() -> None:
    """Wake an in-flight planner call only for a real priority winner."""
    running = missions.running_mission
    if running is None:
      return
    winner = missions.check_preemption(running)
    if winner is not None:
      planner.request_interrupt(
        "scheduler", f"higher-priority mission {winner.id} is pending"
      )

  missions.add_listener(_interrupt_for_preemption)

  pipeline_started = asyncio.Event()

  async def _decision_entry(ready: asyncio.Event, stop: asyncio.Event) -> None:
    await decision_entry(missions, planner, ready, stop, start=pipeline_started)

  coordinator = None
  if config.decision.coordination.enabled:
    # §3.5 mode: the market replaces the serial planner. The planner object
    # stays wired (its plan/annotation hooks serve northbound), but no
    # mission ever enters it; the coordinator is the decision subsystem.
    from egia.core.decision.coordination.coordinator import (
      Coordinator,
      collect_bidders,
      coordinator_entry,
    )
    from egia.core.decision.profile_writer import ProfileWriter
    from egia.core.decision.profiles import ProfileStore

    bidders = await collect_bidders(
      backend_clients, registry, [ep.id for ep in endpoints]
    )
    writer = ProfileWriter(
      reasoner_model,
      registry,
      ProfileStore(state_root / "profiles"),
      prompts=config.prompts.profile_writer,
      token_budget=config.prompts.token_budget,
      reasoning_effort=config.decision.reasoner.reasoning_effort,
    )
    endpoint_ids = [ep.id for ep in endpoints]
    coordinator = Coordinator(
      missions,
      gate,
      writer,
      bidders,
      config=config.decision.coordination,
      step_timeout_s=config.decision.step_timeout_s,
      queue=queue,
      rediscover=lambda: collect_bidders(backend_clients, registry, endpoint_ids),
    )
    gate.set_running_missions(coordinator.running_mission_ids)
    log.info(
      "coordination_wired",
      bidders=sorted(bidder.id for bidder in bidders),
      epsilon=config.decision.coordination.epsilon,
    )

    async def _decision_entry(ready: asyncio.Event, stop: asyncio.Event) -> None:
      await coordinator_entry(coordinator, ready, stop)

  async def _proposer_entry(ready: asyncio.Event, stop: asyncio.Event) -> None:
    await proposer_entry(
      missions,
      model,
      context_builder,
      queue,
      recall,
      config.prompts.proposer,
      config.missions,
      config.decision,
      ready,
      stop,
      active_mission=lambda: planner.running_mission_id,
      exchange_sink=model_io_store.publish,
      start=pipeline_started,
      ensure_hearing_response=backend_mode == "sandbox",
    )

  async def _parts_entry(ready: asyncio.Event, stop: asyncio.Event) -> None:
    # One part-perception task per backend. The scene exchange stays shared
    # and single: the default backend's bundle presents it, as before.
    tasks = [
      asyncio.create_task(
        parts_perception_entry(
          backend_clients.client(ep.id),
          endpoint_resources[ep.id],
          part_stores,
          queue,
          ready,
          stop,
          frame_monitor=watchdog.check_frame,
          backend=ep.id,
        ),
        name=f"egia-parts-{ep.id}",
      )
      for ep in endpoints
    ]
    try:
      await asyncio.gather(*tasks)
    finally:
      for task in tasks:
        task.cancel()
      for task in tasks:
        with contextlib.suppress(asyncio.CancelledError):
          await task

  async def _context_entry(ready: asyncio.Event, stop: asyncio.Event) -> None:
    async def _publish_context(snapshot) -> None:
      await context_store.publish(snapshot)
      if stage_runtime is not None and stage_runtime.active:
        assert context_path is not None
        now = datetime.now(UTC)
        stage_runtime.publish(
          "snapshot",
          StageValue(
            "snapshot",
            f"context-{context_store.version}",
            stage_runtime.run_id,
            context_path,
            snapshot.built_at,
            StagePayload("snapshot", {"text": snapshot.prompt}),
            received_at=now,
          ),
        )

    await context_entry(
      queue,
      config.prompts,
      ready,
      stop,
      publish=_publish_context,
      backend_render=backend_section_renderers(backend_trackers),
      mode_state=mode_state,
      dynamic_renderers={
        "missions": missions.render_for_prompt,
        "primitives": registry.render_for_prompt,
        "skills": lambda: render_skills(backend_clients.all(), registry),
      },
      mission_changed=mission_changed,
      system_prompt=decision_instruction,
      start=pipeline_started,
    )

  async def _safety_entry(ready: asyncio.Event, stop: asyncio.Event) -> None:
    async def _emit_audit(source: str, kind: str, metadata: dict[str, str]) -> None:
      # Invariant 3: the audit loop's own findings (fault-active re-entry,
      # estop verdict) are admitted through the queue, never written around it.
      await queue.enqueue(
        kind="fault",
        source=source,
        text=f"audit {kind}",
        metadata={"reason": kind, **metadata},
      )

    await safety_audit_entry(
      gate,
      queue,
      audit_interval_s=gate_config.audit_interval_s,
      fault_estop_ticks=gate_config.fault_estop_ticks,
      active_mission=lambda: planner.running_mission_id,
      annotate=planner.annotate,
      interrupt=planner.request_interrupt,
      engage_estop=latch.engage,
      emit=_emit_audit,
      ready=ready,
      stop=stop,
    )

  async def _monitor_entry(ready: asyncio.Event, stop: asyncio.Event) -> None:
    availability = True

    def _availability_changed(available: bool) -> None:
      nonlocal availability
      model_availability.set_available(available)
      if available == availability:
        return
      availability = available
      from egia.lib import shared_events

      if available:
        generation = max(
          (client.generation for client in backend_clients.all().values()), default=0
        )
        if generation > 0:
          shared_events.emit(
            "egia_discovery_restored", fields={"generation": generation}
          )
      else:
        shared_events.emit(
          "egia_discovery_lost", fields={"reason_code": "eos_unavailable"}
        )

    await monitor_entry(
      backend_clients.all(),
      queue,
      config.monitor,
      ready,
      stop,
      active_mission=lambda: planner.running_mission_id,
      annotate=planner.annotate,
      interrupt=planner.request_interrupt,
      mission_started_at=lambda: planner.running_mission_started_at,
      backend=backend_trackers,
      latch=latch,
      availability_changed=_availability_changed,
    )

  async def _memory_entry(ready: asyncio.Event, stop: asyncio.Event) -> None:
    # Wired only when config.launch.memory (the spec append below guards it);
    # the asserts narrow the Optional stores for the type checker.
    assert episodic is not None and working is not None
    await memory_consolidation_entry(
      queue,
      episodic,
      working,
      ready,
      stop,
      active_mission=lambda: planner.running_mission_id,
    )

  # Each subsystem logs one module_start line on (re)start so its own
  # var/log/egia/core/<module>.log always begins with what the module is and
  # where it came from — the supervisor's lifecycle events carry state, not
  # identity.
  def _with_module_log(name, entry):
    module_log = get_logger(f"egia.core.{name}")
    role = _MODULE_ROLES[name]

    async def _logged(ready: asyncio.Event, stop: asyncio.Event) -> None:
      module_log.info("module_start", role=role, mode=config.mode)
      await entry(ready, stop)

    return _logged

  pipeline_subsystems = subsystems or [
    SubsystemSpec(name="parts", entry=_with_module_log("parts", _parts_entry)),
    SubsystemSpec(name="context", entry=_with_module_log("context", _context_entry)),
    SubsystemSpec(name="decision", entry=_with_module_log("decision", _decision_entry)),
  ]
  if config.decision.proposer.enabled:
    pipeline_subsystems.append(
      SubsystemSpec(
        name="proposer", entry=_with_module_log("proposer", _proposer_entry)
      )
    )
  pipeline_subsystems.extend(
    [
      SubsystemSpec(name="safety", entry=_with_module_log("safety", _safety_entry)),
      SubsystemSpec(name="monitor", entry=_with_module_log("monitor", _monitor_entry)),
    ]
  )
  if config.launch.memory:
    pipeline_subsystems.append(
      SubsystemSpec(name="memory", entry=_with_module_log("memory", _memory_entry))
    )

  server = NorthboundServer(
    state,
    queue,
    host=config.server.host,
    port=config.server.port,
    status_frequency_hz=config.server.status_frequency_hz,
    mode=config.mode,
    context_store=context_store,
    part_stores=part_stores,
    missions=missions,
    gate=gate,
    step_timeout_s=config.decision.step_timeout_s,
    current_plan=planner.current_plan,
    episodic=episodic,
    reference=reference,
    safety=lambda: SafetyStatus(
      admitted=gate.admitted_count,
      rejected=gate.rejected_count,
      estop_engaged=gate.estop_engaged,
    ),
    backend=lambda: [tracker.snapshot() for tracker in backend_trackers.values()],
    model_io_store=model_io_store,
    primitives_store=primitives_store,
    skills_store=skills_store,
    skill_clients=backend_clients.all(),
    prompts_store=prompts_store,
    sandbox_media_socket=sandbox_media_socket,
  )
  await server.start()

  supervisor = Supervisor(pipeline_subsystems)
  state.supervisor = supervisor
  await supervisor.start()

  ready = await supervisor.wait_ready()
  state.status = HealthState.READY if ready else HealthState.DEGRADED
  log.info("pipeline_ready" if ready else "pipeline_degraded")
  if ready:
    # Context can now build the first complete prompt and the decision loop
    # can issue its first model request.
    pipeline_started.set()
    if config.mode != "sandbox":
      generation = max(
        (client.generation for client in backend_clients.all().values()), default=0
      )
      if generation > 0:
        shared_events.emit("egia_ready", fields={"generation": generation})
  restore_console_level()

  await stop.wait()

  state.status = HealthState.STOPPING
  shared_events.emit("egia_stopping")
  show_lifecycle_logs()
  log.info("pipeline_stopping")
  if stage_runtime is not None:
    await stage_runtime.stop_async()
    for edge, counters in stage_runtime.edge_counters.items():
      log.info(
        "stage_edge_delivery",
        source=edge.source,
        output=edge.output,
        target=edge.target,
        input=edge.input,
        delivered=counters.delivered,
        gaps=counters.gaps,
        overflow=counters.overflow,
      )
  if stage_delivery_task is not None:
    stage_delivery_task.cancel()
    with contextlib.suppress(asyncio.CancelledError):
      await stage_delivery_task
  running_before_stop = missions.running_mission
  await supervisor.stop()
  if session_handoff is not None:
    retained = missions.pending()
    if running_before_stop is not None:
      settled = next(
        (item for item in missions.records() if item.id == running_before_stop.id),
        running_before_stop,
      )
      if settled.state in (MissionState.PENDING, MissionState.RUNNING) and all(
        item.id != settled.id for item in retained
      ):
        retained.append(settled)
    last_plan = planner.last_plan()
    if (
      running_before_stop is None
      or last_plan is None
      or last_plan.mission_id != running_before_stop.id
    ):
      last_plan = None
    session_handoff.capture(
      retained,
      last_plan,
      observations=queue.records(),
      observation_history=queue.history(),
    )
    log.info(
      "backend_session_captured",
      revision=session_handoff.revision,
      missions=len(retained),
      settled_actions=(
        sum(len(step.actions) for step in last_plan.steps)
        if last_plan is not None
        else 0
      ),
    )
  # Episodic and working close inside the memory subsystem's finally; the
  # reference store has no loop of its own, so it closes here, after every
  # subsystem (including the planner that reads it) has stopped. The recall
  # index handed in by the composition root closes here too, after the
  # planner and memory loop can no longer reach it.
  if reference is not None:
    reference.close()
  if recall_index is not None:
    recall_index.close()
  for client in backend_clients.all().values():
    await client.aclose()
  await server.stop()
  log.info("pipeline_stopped")
  return 0
