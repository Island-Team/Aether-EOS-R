"""Asynchronous safety audit — the safety chain's background loop (§3.3).

Runs off the same inputs as the main chain but needs no model output: every
``audit_interval_s`` it re-evaluates the queued state against the gate. An
unrecovered fault — the latest ``kind="fault"`` observation with reason
``heartbeat_lost`` that carries no ``recovered`` flag — counts up consecutive
ticks; while it holds and a mission is running (reported through the
injected ``active_mission`` provider) the verdict is annotated onto the live
plan through the injected ``annotate`` callable.

At ``fault_estop_ticks`` consecutive faulted ticks the audit escalates
through the injected ``engage_estop`` hook — the monitor's estop latch, which
egresses ``safety.estop`` directly (the invariant-1 exception, because an
emergency cannot wait for model output). The composition root wires the hook
to the latch, never to egress. The estop fires once per fault episode and is
never released by the loop — release is an operator decision after the
triggering condition is cleared.
"""

from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Callable

from egia.core.perception.admission import Admission
from egia.core.safety.gate import SafetyGate
from egia.lib.logger import get_logger

__all__ = ["safety_audit_entry"]


def _unrecovered_fault(queue: Admission) -> str | None:
  """The active heartbeat fault's detail text, or ``None`` when clear.

  The latest ``heartbeat_lost`` fault observation decides: a ``recovered``
  flag on it means the monitor saw the backend return; anything older is
  superseded.
  """
  for record in reversed(queue.records()):
    if record.kind == "fault" and record.metadata.get("reason") == "heartbeat_lost":
      if "recovered" in record.metadata:
        return None
      return record.text
  return None


def _drop_interrupt(source: str, detail: str) -> None:
  pass


async def safety_audit_entry(
  gate: SafetyGate,
  queue: Admission,
  *,
  audit_interval_s: float,
  fault_estop_ticks: int,
  active_mission: Callable[[], str | None],
  annotate: Callable[[str, str, str], None],
  ready: asyncio.Event,
  stop: asyncio.Event,
  interrupt: Callable[[str, str], None] = _drop_interrupt,
  engage_estop: Callable[[str], Awaitable[None]] | None = None,
  emit: Callable[[str, str, dict[str, str]], Awaitable[None] | None] | None = None,
) -> None:
  """Supervisor entry: re-evaluate queued state each tick until shutdown.

  ``emit`` is the optional queue-admission hook (per invariant 3, observations
  enter only through the queue): when provided, the loop feeds its own
  findings — the fault-active re-entry and the estop verdict — back through it
  so the queued state records what the audit loop decided, not just the plan
  annotation. It defaults to ``None`` (no-op) so callers that only annotate
  keep working. ``engage_estop`` is the monitor's latch hook the escalation
  goes through; when ``None`` the escalation is logged and annotated but no
  egress is requested.
  """
  log = get_logger("egia.core.safety.audit")
  fault_ticks = 0
  estop_requested = False
  ready.set()
  log.info("audit_ready", interval_s=audit_interval_s)
  while not stop.is_set():
    try:
      await asyncio.wait_for(stop.wait(), timeout=audit_interval_s)
      break
    except TimeoutError:
      pass
    fault = _unrecovered_fault(queue)
    log.info(
      "audit_tick",
      queue_size=queue.size,
      estop_engaged=gate.estop_engaged,
      admitted=gate.admitted_count,
      rejected=gate.rejected_count,
    )
    if fault is None:
      fault_ticks = 0
      estop_requested = False
      continue
    fault_ticks += 1
    log.warning("audit_verdict", verdict="fault_active", fault=fault, ticks=fault_ticks)
    mission_id = active_mission()
    if mission_id is not None:
      annotate("audit", "fault_active", fault)
      if emit is not None:
        result = emit("audit", "fault_active", {"fault": fault})
        if result is not None:
          await result
      # Priority re-entry (§3.8): the next model call must see the fault now,
      # not whenever the current step happens to conclude.
      interrupt("audit", f"fault active: {fault}")
    if fault_ticks >= fault_estop_ticks and not estop_requested:
      estop_requested = True
      log.warning("audit_estop", fault=fault, ticks=fault_ticks)
      if engage_estop is not None:
        await engage_estop(f"audit: unrecovered fault for {fault_ticks} ticks: {fault}")
      annotate("audit", "estop", f"unrecovered fault for {fault_ticks} ticks: {fault}")
      if emit is not None:
        result = emit(
          "audit",
          "estop",
          {"fault": fault, "tick_count": str(fault_ticks)},
        )
        if result is not None:
          await result
  log.info("audit_stopped")
