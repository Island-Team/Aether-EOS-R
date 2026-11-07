"""Execution monitoring — heartbeat escalation plus plan stall detection.

Every ``heartbeat_interval_s`` the monitor probes the gateway through
``core/link``. ``failure_threshold`` consecutive failures escalate: a
``kind="fault"`` observation enters through the state queue (invariant 4 —
monitor findings reach the pipeline like any other state) and an
``escalation`` event is logged. The first success after an escalation logs
recovery and enqueues a matching fault observation carrying
``recovered="true"`` so downstream consumers (the audit loop) can see the
fault cleared.

While a mission runs (reported through the injected ``active_mission``
provider) the monitor also watches for plan stalls: no fresh ``kind="result"``
observation within ``stall_timeout_s`` is a deviation (``docs/egia/egia-2607.md``
§3.6), enqueued as a ``step_stall`` fault and annotated onto the live plan
through the injected ``annotate`` callable. A stall fires once per episode
and re-arms when a fresh primitive result appears.
"""

from __future__ import annotations

import asyncio
from collections.abc import Callable, Mapping
from datetime import UTC, datetime

from egia.core.link import BackendStatusTracker, EosClient
from egia.core.monitor.latch import EstopLatch
from egia.core.perception.admission import Admission
from egia.lib import shared_events
from egia.lib.config import MonitorConfig
from egia.lib.logger import get_logger

FAULT_KIND = "fault"
MONITOR_SOURCE = "monitor"

__all__ = ["FAULT_KIND", "monitor_entry"]


def _no_active_mission() -> str | None:
  return None


def _no_started_at() -> datetime | None:
  return None


def _drop_annotation(source: str, event: str, detail: str = "") -> None:
  pass


def _drop_interrupt(source: str, detail: str) -> None:
  pass


async def monitor_entry(
  client: EosClient | Mapping[str, EosClient],
  queue: Admission,
  config: MonitorConfig,
  ready: asyncio.Event,
  stop: asyncio.Event,
  *,
  active_mission: Callable[[], str | None] = _no_active_mission,
  annotate: Callable[[str, str, str], None] = _drop_annotation,
  interrupt: Callable[[str, str], None] = _drop_interrupt,
  mission_started_at: Callable[[], datetime | None] = _no_started_at,
  backend: BackendStatusTracker | Mapping[str, BackendStatusTracker] | None = None,
  latch: EstopLatch | None = None,
  availability_changed: Callable[[bool], None] | None = None,
) -> None:
  """Supervisor entry: heartbeat every backend and watch for plan stalls.

  ``client`` is one client or a per-backend mapping; every heartbeat tick
  probes EVERY client, each into its own tracker (``backend`` — one tracker
  or a matching mapping) so the northbound ``/backend`` stream and the
  context ``backend`` section see per-backend link state. Escalation and
  recovery are tracked per backend id. ``latch`` is the estop latch this
  loop owns alongside the composition root (built from ``client`` when not
  injected); the safety audit loop and the bound watchdog escalate through
  it, never through direct egress of their own.
  """
  log = get_logger("egia.core.monitor")
  if isinstance(client, Mapping):
    clients = dict(client)
  else:
    # A single client (or test fake): key it by its backend id, "default"
    # for anything that does not carry one.
    clients = {getattr(client, "backend", None) or "default": client}
  trackers: dict[str, BackendStatusTracker] | None
  if backend is None:
    trackers = None
  elif isinstance(backend, Mapping):
    trackers = dict(backend)
  else:
    trackers = {next(iter(clients), "default"): backend}
  if latch is None:
    latch = EstopLatch(next(iter(clients.values())))
  failures = {backend_id: 0 for backend_id in clients}
  escalated: set[str] = set()
  stalled = False
  ready.set()
  log.info(
    "monitor_ready",
    interval_s=config.heartbeat_interval_s,
    failure_threshold=config.failure_threshold,
    backends=sorted(clients),
  )
  while not stop.is_set():
    try:
      await asyncio.wait_for(stop.wait(), timeout=config.heartbeat_interval_s)
      break
    except TimeoutError:
      pass
    for backend_id, link in clients.items():
      tracker = trackers.get(backend_id) if trackers is not None else None
      if await link.health_check():
        rediscovery_failed = False
        if latch.engaged or backend_id in escalated or failures[backend_id] > 0:
          initialize = getattr(link, "initialize", None)
          if initialize is not None:
            try:
              discovery = await initialize(force=True)
            except Exception as exc:  # noqa: BLE001 - heartbeat state handles retry
              log.warning("eos_rediscovery_failed", backend=backend_id, reason=str(exc))
              rediscovery_failed = True
            else:
              if tracker is not None:
                tracker.update_runtime(discovery)
        get_status = getattr(link, "refresh_status", None) or getattr(
          link, "get_status", None
        )
        if not rediscovery_failed and get_status is not None:
          try:
            runtime_status = await get_status()
          except Exception as exc:  # noqa: BLE001 - status is part of heartbeat
            log.warning("eos_status_failed", backend=backend_id, reason=str(exc))
            rediscovery_failed = True
          else:
            if tracker is not None:
              tracker.update_status(runtime_status)
        if rediscovery_failed:
          failures[backend_id] += 1
          if tracker is not None:
            tracker.record_failure()
          continue
        if tracker is not None:
          tracker.record_success()
        if backend_id in escalated:
          escalated.discard(backend_id)
          log.info(
            "heartbeat_recovered", backend=backend_id, failures=failures[backend_id]
          )
          shared_events.emit("egia_monitor_episode", fields={"outcome": "recovered"})
          await queue.enqueue(
            kind=FAULT_KIND,
            source=MONITOR_SOURCE,
            text="backend heartbeat recovered",
            metadata={
              "reason": "heartbeat_lost",
              "recovered": "true",
              "backend": backend_id,
            },
          )
        failures[backend_id] = 0
      else:
        failures[backend_id] += 1
        if tracker is not None:
          tracker.record_failure()
        if failures[backend_id] == 1:
          log.warning("heartbeat_failed", backend=backend_id, consecutive_failures=1)
        if (
          failures[backend_id] >= config.failure_threshold
          and backend_id not in escalated
        ):
          escalated.add(backend_id)
          log.error(
            "escalation",
            reason="heartbeat_lost",
            backend=backend_id,
            consecutive_failures=failures[backend_id],
          )
          shared_events.emit("egia_monitor_episode", fields={"outcome": "fault"})
          await queue.enqueue(
            kind=FAULT_KIND,
            source=MONITOR_SOURCE,
            text=(
              f"backend {backend_id} heartbeat lost "
              f"({failures[backend_id]} consecutive failures)"
            ),
            metadata={
              "reason": "heartbeat_lost",
              "backend": backend_id,
              "consecutive_failures": str(failures[backend_id]),
            },
          )
    if availability_changed is not None:
      availability_changed(all(value == 0 for value in failures.values()))
    stalled = await _check_stall(
      queue,
      config,
      active_mission,
      annotate,
      interrupt,
      mission_started_at,
      stalled,
      log,
    )
  log.info("monitor_stopped")


async def _check_stall(
  queue: Admission,
  config: MonitorConfig,
  active_mission: Callable[[], str | None],
  annotate: Callable[[str, str, str], None],
  interrupt: Callable[[str, str], None],
  mission_started_at: Callable[[], datetime | None],
  stalled: bool,
  log,
) -> bool:
  """One stall-detection tick; returns the episode state for the next tick.

  Fires at most one ``step_stall`` fault per stall episode; a fresh result
  observation re-arms the detector. The window is floored at the mission's
  start time: a mission whose first model call is still in flight has no
  result observation by definition, and inference latency is not a stall.
  """
  mission_id = active_mission()
  if mission_id is None:
    return False
  now = datetime.now(UTC)
  latest = queue.latest_by_kind("result")
  since_activity = (
    config.stall_timeout_s + 1.0
    if latest is None
    else (now - latest.timestamp).total_seconds()
  )
  started = mission_started_at()
  since_start = (
    config.stall_timeout_s + 1.0 if started is None else (now - started).total_seconds()
  )
  if min(since_activity, since_start) < config.stall_timeout_s:
    return False
  if stalled:
    return True
  log.warning(
    "step_stall", mission_id=mission_id, stall_timeout_s=config.stall_timeout_s
  )
  await queue.enqueue(
    kind=FAULT_KIND,
    source=MONITOR_SOURCE,
    text=f"mission {mission_id} stalled: no primitive result for "
    f"{config.stall_timeout_s}s",
    metadata={"reason": "step_stall", "mission_id": mission_id},
  )
  annotate(
    MONITOR_SOURCE,
    "step_stall",
    f"no primitive result for {config.stall_timeout_s}s",
  )
  # Priority re-entry (§3.8): the deviation must reach the model at the next
  # step boundary, cooldown or not.
  interrupt(
    MONITOR_SOURCE, f"step stall: no primitive result for {config.stall_timeout_s}s"
  )
  return True
