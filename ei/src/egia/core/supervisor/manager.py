"""Supervised asyncio lifecycle for pipeline subsystems."""

from __future__ import annotations

import asyncio
from datetime import UTC, datetime

from egia.api.status import HealthState, SubsystemStatus
from egia.core.supervisor.spec import (
  READY_TIMEOUT_S,
  RESTART_BACKOFF_S,
  SubsystemSpec,
  _Runtime,
)
from egia.lib.logger import get_logger


class Supervisor:
  """Starts, readiness-gates, health-checks, and restarts subsystems."""

  def __init__(self, specs: list[SubsystemSpec]) -> None:
    self._log = get_logger("egia.core.supervisor")
    self._stop = asyncio.Event()
    self._runtimes = {spec.name: _Runtime(spec) for spec in specs}
    self._watchdog: asyncio.Task | None = None

  async def start(self) -> None:
    """Launch every subsystem; no loop runs un-gated (see :meth:`wait_ready`)."""
    self._log.info("supervisor_starting", subsystem_count=len(self._runtimes))
    for rt in self._runtimes.values():
      self._spawn(rt)
    self._watchdog = asyncio.create_task(self._watch(), name="egia-watchdog")

  async def wait_ready(self, timeout: float = READY_TIMEOUT_S) -> bool:
    """Block until all subsystems signal ready. Returns False on timeout."""
    pending = [rt.ready.wait() for rt in self._runtimes.values()]
    if not pending:
      return True
    await asyncio.wait((asyncio.create_task(p) for p in pending), timeout=timeout)
    missing = [n for n, rt in self._runtimes.items() if not rt.ready.is_set()]
    if missing:
      self._log.error("subsystems_not_ready", missing=missing)
      return False
    for rt in self._runtimes.values():
      if rt.ready_at is None:
        rt.ready_at = datetime.now(UTC)
        rt.state = HealthState.READY
    self._log.info("subsystems_ready", subsystem_count=len(self._runtimes))
    return True

  def statuses(self) -> list[SubsystemStatus]:
    """Snapshot of per-subsystem health for the northbound interface."""
    out = []
    for rt in self._runtimes.values():
      uptime = (datetime.now(UTC) - rt.ready_at).total_seconds() if rt.ready_at else 0.0
      out.append(
        SubsystemStatus(
          name=rt.spec.name,
          state=rt.state,
          uptime_seconds=max(uptime, 0.0),
          restarts=rt.restarts,
          last_error=rt.last_error,
        )
      )
    return out

  def is_healthy(self) -> bool:
    """True while every subsystem is ready and its task is alive."""
    return all(
      rt.state == HealthState.READY and rt.task is not None and not rt.task.done()
      for rt in self._runtimes.values()
    )

  async def stop(self) -> None:
    """Signal all subsystems to stop and await their tasks."""
    self._log.info("supervisor_stopping", subsystem_count=len(self._runtimes))
    for rt in self._runtimes.values():
      rt.state = HealthState.STOPPING
    self._stop.set()
    if self._watchdog:
      self._watchdog.cancel()
    tasks = [rt.task for rt in self._runtimes.values() if rt.task]
    if tasks:
      await asyncio.gather(*tasks, return_exceptions=True)
    self._log.info("supervisor_stopped")

  def _spawn(self, rt: _Runtime) -> None:
    rt.ready = asyncio.Event()
    rt.ready_at = None
    rt.state = HealthState.STARTING
    rt.task = asyncio.create_task(
      rt.spec.entry(rt.ready, self._stop), name=f"egia-{rt.spec.name}"
    )
    self._log.info("subsystem_started", name=rt.spec.name, restarts=rt.restarts)

  async def _watch(self) -> None:
    """Health loop: detect dead subsystem tasks and restart with backoff."""
    self._log.debug("watchdog_started")
    while not self._stop.is_set():
      await asyncio.sleep(0.5)
      if self._stop.is_set():
        return
      for rt in self._runtimes.values():
        if rt.ready_at is None and rt.ready.is_set():
          rt.ready_at = datetime.now(UTC)
          rt.state = HealthState.READY
          self._log.info("subsystem_ready", name=rt.spec.name)
        if rt.task is None or not rt.task.done():
          continue
        exc = rt.task.exception() if not rt.task.cancelled() else None
        rt.failures += 1
        rt.last_error = repr(exc) if exc else "task exited without error"
        rt.state = HealthState.DEGRADED
        self._log.error(
          "subsystem_failed",
          name=rt.spec.name,
          error=rt.last_error,
          failures=rt.failures,
        )
        backoff = RESTART_BACKOFF_S[min(rt.failures - 1, len(RESTART_BACKOFF_S) - 1)]
        self._log.warning(
          "subsystem_restarting",
          name=rt.spec.name,
          backoff_seconds=backoff,
          restarts=rt.restarts + 1,
        )
        await asyncio.sleep(backoff)
        if self._stop.is_set():
          return
        rt.restarts += 1
        self._spawn(rt)
