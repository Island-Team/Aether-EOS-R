"""Memory consolidation — the memory chain's background loop.

Wakes on queue admissions and drains every queued observation into the
episodic store. Records persist by id, so re-draining a queue that has not
turned over is a no-op. Each wake also re-checks the working store (the MVP
form of "re-checking stored plans", ``docs/egia/egia-2607.md`` §3.3): a row whose
mission is no longer running means the mission died mid-write (crash/kill),
so the partial plan is persisted to episodic as ``aborted`` with a
``crash_recovery`` annotation and the stale row is dropped.
"""

from __future__ import annotations

import asyncio
from collections.abc import Callable
from datetime import UTC, datetime

from egia.api.plan import PlanAnnotation, StepOutcome
from egia.core.memory.store import EpisodicStore
from egia.core.memory.working import WorkingStore
from egia.core.perception.admission import Admission
from egia.lib.logger import get_logger

__all__ = ["memory_consolidation_entry"]


async def memory_consolidation_entry(
  queue: Admission,
  store: EpisodicStore,
  working: WorkingStore,
  ready: asyncio.Event,
  stop: asyncio.Event,
  *,
  active_mission: Callable[[], str | None] = lambda: None,
) -> None:
  """Supervisor entry: consolidate queued observations until shutdown.

  ``active_mission`` reports the currently running mission id (the planner's
  provider), so the working-store re-check can tell a live in-flight row
  from one orphaned by a crash.
  """
  log = get_logger("egia.core.memory.consolidate")
  ready.set()
  log.info("consolidation_ready")
  admission_revision = queue.revision
  try:
    while not stop.is_set():
      changed = await queue.wait_for_change(admission_revision, stop)
      if changed is None:
        break
      admission_revision = changed
      if stop.is_set():
        break
      persisted = sum(1 for record in queue.records() if store.append(record))
      if persisted:
        log.info("consolidation_drained", persisted=persisted, total=store.count())
      _recover_stale_plans(store, working, active_mission, log)
  finally:
    store.close()
    working.close()
    log.info("consolidation_stopped")


def _recover_stale_plans(
  store: EpisodicStore,
  working: WorkingStore,
  active_mission: Callable[[], str | None],
  log,
) -> None:
  """Persist working rows whose mission died mid-write, then drop them."""
  running = active_mission()
  active = {running} if running is not None else set()
  for plan in working.list_stale(active):
    if store.get_plan(plan.id) is None:
      recovered = plan.model_copy(
        update={"outcome": StepOutcome.ABORTED, "finished_at": datetime.now(UTC)}
      )
      recovered.annotations.append(
        PlanAnnotation(
          source="memory",
          event="crash_recovery",
          detail="in-flight plan recovered; the mission died mid-write",
        )
      )
      store.save_plan(recovered)
      log.info(
        "plan_crash_recovered",
        mission_id=plan.mission_id,
        plan_id=plan.id,
        steps=len(plan.steps),
      )
    working.drop(plan.mission_id)
