"""Decision layer — fast controller, semantic reasoner, and Mission execution.

The model seam lives in :mod:`egia.internal.model`; :mod:`egia.core.decision.model`
binds it to the configured VLM endpoint. The mission table lives in
:mod:`egia.core.missions` (the sole ingress gate for internal proposals);
``planner`` drives each admitted Mission through the governed dual-lane loop.
"""

from __future__ import annotations

import asyncio
import contextlib

from egia.api.mission import MissionState
from egia.core.decision.planner import (
  DecisionPlanner,
  MissionFailedError,
  MissionPreemptedError,
  ReActPlanner,
)
from egia.core.missions import MissionQueue, ResubmitOutcome
from egia.lib.logger import get_logger


async def decision_entry(
  missions: MissionQueue,
  planner: DecisionPlanner,
  ready: asyncio.Event,
  stop: asyncio.Event,
  *,
  start: asyncio.Event | None = None,
) -> None:
  """Supervisor entry: run table-selected missions one at a time until shutdown.

  Serial execution is the mission table's contract — the planner never sees
  two missions concurrently. A mission that fails is recorded with its error
  and the loop moves to the next pending order; a preempted one returns to
  the table (§3.9) unless the re-entry is rejected — past the preemption
  cap or into a full table — in which case it fails with the cause-specific
  error rather than ping-ponging. The subsystem itself stays up.
  """
  log = get_logger("egia.core.decision")
  ready.set()
  log.info("decision_ready")
  if start is not None:
    # Do not request the model until every supervised module has completed
    # initialization.  A mission may already be queued during startup.
    start_task = asyncio.create_task(start.wait())
    stop_task = asyncio.create_task(stop.wait())
    done, pending = await asyncio.wait(
      {start_task, stop_task}, return_when=asyncio.FIRST_COMPLETED
    )
    for task in pending:
      task.cancel()
    for task in pending:
      with contextlib.suppress(asyncio.CancelledError):
        await task
    if stop_task in done:
      log.info("decision_stopped")
      return
  while not stop.is_set():
    mission = await missions.wait_pending(stop)
    if mission is None:
      break
    missions.mark_running(mission)
    try:
      result = await planner.run(mission, stop)
    except MissionPreemptedError as exc:
      outcome = missions.resubmit(mission)
      if outcome is ResubmitOutcome.ADMITTED:
        log.info(
          "mission_preempted",
          mission_id=mission.id,
          count=mission.preempted_count,
          error=str(exc),
        )
      else:
        error = (
          "preempted too often"
          if outcome is ResubmitOutcome.PREEMPTION_CAP
          else "preempted: mission table full"
        )
        missions.finish(mission, state=MissionState.FAILED, error=error)
    except MissionFailedError as exc:
      if stop.is_set():
        missions.suspend(
          mission,
          detail="shutdown interrupted mission; resume on next launch",
        )
      else:
        missions.finish(mission, state=MissionState.FAILED, error=str(exc))
    else:
      missions.finish(mission, state=MissionState.SUCCEEDED, result=result)
  log.info("decision_stopped")


__all__ = [
  "DecisionPlanner",
  "MissionFailedError",
  "MissionPreemptedError",
  "MissionQueue",
  "ReActPlanner",
  "decision_entry",
]
