"""Synchronous mission-consistency gate on the decision → safety → link chain.

The gate rules on exactly one question: *may this mission, and this call
inside it, happen at all?* Against two inputs — the mission table and the
estop latch (monitor-owned). Emergency egress itself does NOT pass through
here: the monitor owns the estop latch and the direct ``monitor → link``
emergency route (invariant-1 exception).

Admitted EOS primitives forward to ``core/link``. Schema validation, entity caps,
plan-step limits, loop detection, and the rate window are no longer gate
checks: contract-shaped validation lives at the wire boundary, plan bounds
live in decision, and the rate window is owned by the monitor (consulted
here through the injected ``rate_admit`` hook).
"""

from __future__ import annotations

from collections.abc import Awaitable, Callable
from uuid import uuid4

from egia.contracts.endpoints import OPERATOR_PRIMITIVE_KINDS
from egia.core.link import ClientRegistry, LinkPrimitiveError
from egia.core.missions import MissionQueue
from egia.core.monitor import EstopLatch
from egia.core.perception.admission import Admission
from egia.core.registry import PrimitiveRegistry
from egia.internal.decision import ActionInvocation, GateVerdict, StepContext
from egia.lib import shared_events
from egia.lib.config import SafetyConfig
from egia.lib.logger import get_logger
from egia.lib.operation_ledger import OperationLedger, PendingOperation

__all__ = ["SafetyGate"]


class SafetyGate:
  """Validate and forward every workload primitive; nothing bypasses it."""

  def __init__(
    self,
    registry: PrimitiveRegistry,
    links: ClientRegistry,
    monitor_latch: EstopLatch,
    missions: MissionQueue,
    perceptions: Admission | None = None,
    config: SafetyConfig | None = None,
    rate_admit: Callable[[], bool] | None = None,
    running_missions: Callable[[], set[str]] | None = None,
    availability_wait: Callable[[], Awaitable[None]] | None = None,
    operation_ledger: OperationLedger | None = None,
  ) -> None:
    self._registry = registry
    self._links = links
    self._latch = monitor_latch
    self._missions = missions
    # Perception state seam: the phase-6 safety port arrives later; the
    # admission store is held now so the wiring does not change again.
    self._perceptions = perceptions
    self._config = config or SafetyConfig()
    self._rate_admit = rate_admit
    # Coordination seam (§3.5): with several executors running in parallel,
    # mission scope widens from "the one RUNNING mission" to "any mission
    # the set returns". None keeps the serial-planner contract unchanged.
    self._running_missions = running_missions
    self._availability_wait = availability_wait
    self._operation_ledger = operation_ledger
    self._log = get_logger("egia.core.safety")
    self.admitted_count = 0
    self.rejected_count = 0

  def set_running_missions(self, hook: Callable[[], set[str]] | None) -> None:
    """Swap the mission-scope hook (composition-root wiring for coordination)."""
    self._running_missions = hook

  @property
  def estop_engaged(self) -> bool:
    """A read of the monitor's estop latch — the northbound status surface."""
    return self._latch.engaged

  async def execute(
    self, invocation: ActionInvocation, context: StepContext | None = None
  ) -> GateVerdict:
    """Admit one invocation and execute it, or reject it without egress.

    The scope
    check trusts origin: an operator-typed call (context ``None`` or
    ``origin="operator"``) needs no mission; a planner call must match the
    mission table's RUNNING mission and target a mission-tier primitive.
    """
    mission_id = context.mission_id if context is not None else ""
    operation_id = invocation.request_id or str(uuid4())
    if not shared_events.valid_opaque_id(operation_id):
      return GateVerdict(admitted=False, reason="invalid operation ID")
    if self._operation_ledger is not None:
      shared_events.emit(
        "egia_proposal_created",
        fields={
          "operation_id": operation_id,
          **(
            {"mission_id": mission_id}
            if shared_events.valid_opaque_id(mission_id)
            else {}
          ),
        },
        correlation_id=operation_id,
      )
    if (
      context is not None
      and context.origin == "planner"
      and self._availability_wait is not None
    ):
      # A barrier that entered before link loss is already governed by its
      # existing timeout. New planner barriers wait until the complete fleet
      # has rediscovered resources and primitives.
      await self._availability_wait()
    if self._latch.engaged:
      self.rejected_count += 1
      self._log.warning(
        "gate_rejected", action=invocation.action, reason="estop_engaged"
      )
      shared_events.emit(
        "egia_safety_rejected",
        fields={"reason_code": "estop_latched", "operation_id": operation_id},
        correlation_id=operation_id,
      )
      return GateVerdict(admitted=False, reason="estop_engaged")
    primitive = None
    try:
      primitive = self._registry.resolve(
        invocation.action, invocation.robot_id, invocation.device_id
      )
    except LookupError:
      # Multi-backend ambiguity: the same primitive on several endpoints —
      # the planner must say which backend it means.
      self.rejected_count += 1
      self._log.warning(
        "gate_rejected",
        action=invocation.action,
        reason="ambiguous_primitive: specify robot_id and device_id",
      )
      shared_events.emit(
        "egia_safety_rejected",
        fields={"reason_code": "capability_missing", "operation_id": operation_id},
        correlation_id=operation_id,
      )
      return GateVerdict(
        admitted=False, reason="ambiguous_primitive: specify robot_id and device_id"
      )
    if primitive is None:
      self.rejected_count += 1
      self._log.warning(
        "gate_rejected", action=invocation.action, reason="unknown_primitive"
      )
      shared_events.emit(
        "egia_safety_rejected",
        fields={"reason_code": "capability_missing", "operation_id": operation_id},
        correlation_id=operation_id,
      )
      return GateVerdict(admitted=False, reason="unknown_primitive")
    if context is not None and context.origin == "planner":
      if self._running_missions is not None:
        if context.mission_id not in self._running_missions():
          self._rejected_scope(
            invocation,
            f"mission {context.mission_id or '<empty>'} is not a running mission",
            operation_id,
          )
          return GateVerdict(
            admitted=False,
            reason=f"scope_violation: mission {context.mission_id or '<empty>'} "
            "is not a running mission",
          )
      else:
        running = self._missions.running_mission
        if running is None:
          self._rejected_scope(invocation, "no running mission", operation_id)
          return GateVerdict(
            admitted=False, reason="scope_violation: no running mission"
          )
        if running.id != context.mission_id:
          self._rejected_scope(
            invocation,
            f"mission {context.mission_id or '<empty>'} is not the running mission",
            operation_id,
          )
          return GateVerdict(
            admitted=False,
            reason=f"scope_violation: mission {context.mission_id or '<empty>'} "
            "is not the running mission",
          )
      if primitive.kind in OPERATOR_PRIMITIVE_KINDS or primitive.operator:
        self._rejected_scope(
          invocation, f"{invocation.action} is operator-tier", operation_id
        )
        return GateVerdict(
          admitted=False,
          reason=f"scope_violation: {invocation.action} is operator-tier",
        )
    if self._rate_admit is not None and not self._rate_admit():
      self.rejected_count += 1
      self._log.warning(
        "gate_rejected", action=invocation.action, reason="rate_limited"
      )
      shared_events.emit(
        "egia_safety_rejected",
        fields={"reason_code": "policy_denied", "operation_id": operation_id},
        correlation_id=operation_id,
      )
      return GateVerdict(admitted=False, reason="rate_limited")
    self.admitted_count += 1
    if self._operation_ledger is not None:
      try:
        link = self._links.client(primitive.backend)
        self._operation_ledger.reserve(
          PendingOperation(
            operation_id,
            primitive.backend,
            primitive.name,
            primitive.device_id or "",
            mission_id or None,
            link.discovery.robot_id,
            link.active_revision or "standalone",
          )
        )
      except Exception as exc:  # noqa: BLE001 - no durable identity, no egress
        self.rejected_count += 1
        self.admitted_count -= 1
        self._log.error("operation_reservation_failed", reason=str(exc))
        return GateVerdict(admitted=False, reason="operation reservation failed")
      shared_events.emit(
        "egia_admission_decided",
        fields={
          "outcome": "admitted",
          "operation_id": operation_id,
          **(
            {"mission_id": mission_id}
            if shared_events.valid_opaque_id(mission_id)
            else {}
          ),
        },
        correlation_id=operation_id,
      )
    self._log.info(
      "gate_admitted",
      action=primitive.name,
      uri=primitive.uri,
      mission_id=mission_id,
      argument_keys=sorted(invocation.arguments),
    )
    try:
      link = self._links.client(primitive.backend)
      assert primitive.kind is not None
      assert primitive.device_id is not None
      if self._operation_ledger is None:
        ack = await link.call_primitive(
          primitive.name, primitive.device_id, invocation.arguments
        )
      else:
        ack = await link.call_primitive(
          primitive.name,
          primitive.device_id,
          invocation.arguments,
          command_id=operation_id,
        )
        if ack.status.value in {"succeeded", "failed", "cancelled", "rejected"}:
          self._operation_ledger.settle(operation_id, ack.status.value)
    except LinkPrimitiveError as exc:
      self._log.warning(
        "gate_egress_failed",
        action=primitive.name,
        status=exc.status,
        reason=exc.reason,
      )
      return GateVerdict(admitted=True, reason=f"egress_failed: {exc.reason}")
    except Exception as exc:  # noqa: BLE001 — transport failures become verdicts
      self._log.warning("gate_egress_failed", action=primitive.name, reason=str(exc))
      return GateVerdict(admitted=True, reason=f"egress_failed: {exc}")
    return GateVerdict(admitted=True, ack=ack)

  def _rejected_scope(
    self, invocation: ActionInvocation, detail: str, operation_id: str
  ) -> None:
    self.rejected_count += 1
    self._log.warning(
      "gate_rejected",
      action=invocation.action,
      reason=f"scope_violation: {detail}",
    )
    shared_events.emit(
      "egia_safety_rejected",
      fields={"reason_code": "policy_denied", "operation_id": operation_id},
      correlation_id=operation_id,
    )
