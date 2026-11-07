"""The coordinator — mission intake, auction ticks, per-executor fan-out.

The §3.5 subsystem that replaces the serial decision entry when
``[decision.coordination] enabled``: missions enter through the same table,
decompose into stored profiles via the writer seam, and their ready steps
are auctioned to (backend, part) bidders. Each executor drains its award
queue serially — one executing step per executor, many executors in
parallel — and every call still travels decision → safety gate → link.
Completion events re-open bidder capacity and re-tick the market: an
incremental event costs one auction over the affected ready set, never a
global re-solve.
"""

from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Callable

from egia.api.mission import MissionRecord, MissionState
from egia.contracts.actions import PrimitiveAck
from egia.core.decision.coordination.auction import award
from egia.core.decision.coordination.bidders import BidderProfile, bidder_profiles
from egia.core.decision.coordination.board import BoardStep, TaskBoard
from egia.core.decision.coordination.leases import LeaseClaim, LeaseOutcome, ZoneLeases
from egia.core.decision.planner import DECISION_SOURCE, RESULT_OBSERVATION_KIND
from egia.core.decision.profile_writer import ProfileWriter, ProfileWriterError
from egia.core.link import ClientRegistry
from egia.core.missions import MissionQueue
from egia.core.perception.admission import Admission
from egia.core.registry import PrimitiveRegistry
from egia.core.safety import SafetyGate
from egia.internal.decision import ActionInvocation, StepContext
from egia.lib.config import CoordinationConfig
from egia.lib.logger import get_logger

__all__ = ["Coordinator", "collect_bidders", "coordinator_entry"]


class Coordinator:
  """Fan-out mission execution: profile decomposition + primitive market."""

  def __init__(
    self,
    missions: MissionQueue,
    gate: SafetyGate,
    writer: ProfileWriter,
    bidders: list[BidderProfile],
    *,
    config: CoordinationConfig,
    step_timeout_s: float = 30.0,
    queue: Admission | None = None,
    rediscover: Callable[[], Awaitable[list[BidderProfile]]] | None = None,
  ) -> None:
    self._missions = missions
    self._gate = gate
    self._writer = writer
    self._bidders = bidders
    self._config = config
    self._step_timeout_s = step_timeout_s
    self._queue = queue
    self._rediscover = rediscover
    self._board = TaskBoard(config.board_capacity)
    self._leases = ZoneLeases()
    self._queues: dict[str, asyncio.Queue[BoardStep]] = {
      bidder.id: asyncio.Queue() for bidder in bidders
    }
    self._busy: set[str] = set()
    self._by_id = {bidder.id: bidder for bidder in bidders}
    self._executor_tasks: dict[str, asyncio.Task] = {}
    self._mission_tasks: set[asyncio.Task] = set()
    self._mission_by_job: dict[str, MissionRecord] = {}
    self._tick = asyncio.Event()
    self._log = get_logger("egia.core.coordination")

  # ------------------------------------------------------------- surfaces

  def running_mission_ids(self) -> set[str]:
    """Mission ids with live board jobs — the gate's coordination scope."""
    return {
      record.id
      for job_id, record in self._mission_by_job.items()
      if self._board.job(job_id) is not None
    }

  async def run(self, ready: asyncio.Event, stop: asyncio.Event) -> None:
    """Supervisor entry: intake, tick loop, discovery, one executor/bidder."""
    ready.set()
    self._log.info("coordination_ready", bidders=sorted(self._by_id))
    for bidder in self._bidders:
      self._start_executor(bidder, stop)
    tasks = [
      asyncio.create_task(self._intake(stop), name="egia-coord-intake"),
      asyncio.create_task(self._tick_loop(stop), name="egia-coord-tick"),
      asyncio.create_task(self._rediscovery_loop(stop), name="egia-coord-discovery"),
    ]
    try:
      await asyncio.gather(*tasks)
    finally:
      shutdown_tasks = [
        *tasks,
        *self._executor_tasks.values(),
        *self._mission_tasks,
      ]
      for task in shutdown_tasks:
        task.cancel()
      for task in shutdown_tasks:
        try:
          await task
        except BaseException as exc:  # noqa: BLE001 — shutdown drain
          if not isinstance(exc, asyncio.CancelledError):
            self._log.debug("executor_drained", error=str(exc))
    self._log.info("coordination_stopped")

  def _start_executor(self, bidder: BidderProfile, stop: asyncio.Event) -> None:
    """One serial-drain executor per bidder id; re-discovery adds more."""
    if bidder.id in self._executor_tasks:
      return
    task = asyncio.create_task(
      self._executor(bidder, stop), name=f"egia-coord-{bidder.id}"
    )
    task.add_done_callback(_log_executor_exit)
    self._executor_tasks[bidder.id] = task

  # --------------------------------------------------------------- intake

  async def _intake(self, stop: asyncio.Event) -> None:
    while not stop.is_set():
      mission = await self._missions.wait_pending(stop)
      if mission is None:
        return
      self._missions.mark_running(mission)
      task = asyncio.create_task(
        self._run_mission(mission), name=f"egia-coord-mission-{mission.id}"
      )
      self._mission_tasks.add(task)
      task.add_done_callback(self._mission_tasks.discard)

  async def _run_mission(self, mission: MissionRecord) -> None:
    try:
      profile = await self._writer.write(mission.order)
    except ProfileWriterError as exc:
      self._missions.finish(mission, state=MissionState.FAILED, error=str(exc))
      return
    job = self._board.add_job(profile, mission.id)
    if job is None:
      self._missions.finish(
        mission, state=MissionState.FAILED, error="coordination board full"
      )
      return
    self._mission_by_job[job.job_id] = mission
    self._log.info(
      "job_admitted",
      mission_id=mission.id,
      job_id=job.job_id,
      profile=profile.name,
      steps=len(profile.steps),
    )
    self._tick.set()

  # ---------------------------------------------------------------- ticks

  async def _tick_loop(self, stop: asyncio.Event) -> None:
    while not stop.is_set():
      self._tick.clear()
      wait_tick = asyncio.create_task(self._tick.wait())
      wait_stop = asyncio.create_task(stop.wait())
      done, pending = await asyncio.wait(
        {wait_tick, wait_stop}, return_when=asyncio.FIRST_COMPLETED
      )
      for task in pending:
        task.cancel()
      if wait_stop in done:
        return
      await self._auction_tick()

  # ------------------------------------------------------------ re-discovery

  async def _rediscovery_loop(self, stop: asyncio.Event) -> None:
    """Re-read the fleet's discovery documents on an interval.

    Bidders are compiled once at launch; a fleet is a live set — a robot
    that was down at launch (coarse fallback bidder), one that reboots
    with new parts, or one that disappears should be reflected without a
    EGIA restart. ``rediscovery_interval_s = 0`` keeps launch-time
    discovery only.
    """
    interval = self._config.rediscovery_interval_s
    if self._rediscover is None or interval <= 0:
      return
    while not stop.is_set():
      wait_stop = asyncio.create_task(stop.wait())
      wait_interval = asyncio.create_task(asyncio.sleep(interval))
      done, pending = await asyncio.wait(
        {wait_stop, wait_interval}, return_when=asyncio.FIRST_COMPLETED
      )
      for task in pending:
        task.cancel()
      if wait_stop in done:
        return
      try:
        await self._apply_discovery(await self._rediscover(), stop)
      except Exception as exc:  # noqa: BLE001 — discovery stays best-effort
        self._log.warning("rediscovery_failed", error=str(exc))

  async def _apply_discovery(
    self, fresh: list[BidderProfile], stop: asyncio.Event
  ) -> None:
    """Fold one fresh discovery pass into the live market.

    New bidders join with their own executor; bidders absent from the
    fresh advertisement stop bidding (marked unhealthy — never killed
    mid-queue, their queued steps still drain); returning bidders keep
    their committed load and zone state but refresh primitives. A
    backend whose discovery recovers implicitly retires its coarse
    ``("backend", "default")`` fallback: the fallback is absent from the
    fresh set, and the fallback returns the same way when discovery
    fails again — degradation and recovery are the same mechanism.
    """
    fresh_by_id = {bidder.id: bidder for bidder in fresh}
    changed = False
    for bidder_id, bidder in fresh_by_id.items():
      existing = self._by_id.get(bidder_id)
      if existing is None:
        self._bidders.append(bidder)
        self._by_id[bidder_id] = bidder
        self._queues[bidder_id] = asyncio.Queue()
        self._start_executor(bidder, stop)
        changed = True
      else:
        existing.healthy = True
        existing.primitives = bidder.primitives
        existing.tags = bidder.tags
        existing.stations = bidder.stations
    for bidder in self._bidders:
      if bidder.id not in fresh_by_id and bidder.healthy:
        bidder.healthy = False
        changed = True
    if changed:
      self._log.info(
        "bidders_refreshed",
        healthy=sorted(b.id for b in self._bidders if b.healthy),
      )
      self._tick.set()

  async def _auction_tick(self) -> None:
    ready = self._board.ready()
    if not ready:
      return
    available = [bidder for bidder in self._bidders if bidder.id not in self._busy]
    if not available:
      return
    result = award(
      [board_step.step for board_step in ready],
      available,
      epsilon=self._config.epsilon,
      congestion=self._config.congestion,
    )
    for step_id, bidder_id in result.awards.items():
      board_step = next(bs for bs in ready if bs.step.id == step_id)
      if not self._acquire_zone(board_step, bidder_id):
        continue  # deferred to a later tick; step stays READY
      self._board.award(board_step.job_id, step_id, bidder_id)
      bidder = self._by_id[bidder_id]
      bidder.commit(board_step.step)
      self._busy.add(bidder_id)
      self._queues[bidder_id].put_nowait(board_step)
      self._log.info(
        "step_awarded",
        job_id=board_step.job_id,
        step_id=step_id,
        bidder=bidder_id,
        price=next((bid.price for bid in result.bids if bid.step_id == step_id), None),
      )
    if result.unassigned:
      self._log.debug("steps_unassigned", count=len(result.unassigned))

  def _acquire_zone(self, board_step: BoardStep, bidder_id: str) -> bool:
    """Lease the step's zone, deferring when any holder keeps it.

    A held zone defers the claimant — preempting a *running* step's lease is
    the §3.6 anticipatory extension, not this milestone's policy.
    """
    zone = board_step.step.zone
    if zone is None:
      return True
    claim = LeaseClaim(
      step_id=board_step.step.id,
      priority=1.0,  # operator missions outrank self-proposed (§3.9) at wiring
      switch_cost=bidder_cost(self._by_id[bidder_id], board_step),
    )
    outcome = self._leases.acquire(zone, claim)
    return outcome is LeaseOutcome.ACQUIRED

  # ------------------------------------------------------------- executors

  async def _executor(self, bidder: BidderProfile, stop: asyncio.Event) -> None:
    queue = self._queues[bidder.id]
    while not stop.is_set():
      try:
        board_step = await asyncio.wait_for(queue.get(), timeout=0.5)
      except TimeoutError:
        continue
      try:
        await self._execute(bidder, board_step)
      finally:
        if queue.empty():
          self._busy.discard(bidder.id)
        self._tick.set()

  async def _execute(self, bidder: BidderProfile, board_step: BoardStep) -> None:
    job = self._board.job(board_step.job_id)
    if job is None:
      return
    arguments = dict(board_step.step.arguments)
    if bidder.part_id != "default":
      arguments.setdefault("part_id", bidder.part_id)
    invocation = ActionInvocation(
      action=board_step.step.primitive,
      arguments=arguments,
      robot_id=bidder.backend,
      device_id=bidder.part_id,
      intent=f"profile {job.profile.name} step {board_step.step.id}",
    )
    context = StepContext(mission_id=job.mission_id, origin="planner")
    try:
      verdict = await asyncio.wait_for(
        self._gate.execute(invocation, context), timeout=self._step_timeout_s
      )
    except TimeoutError:
      verdict = None
    accepted = (
      verdict is not None
      and verdict.admitted
      and verdict.ack is not None
      and verdict.ack.succeeded
    )
    ack: PrimitiveAck | None = verdict.ack if verdict is not None else None
    await self._observe(board_step, accepted, ack)
    if board_step.step.zone is not None:
      self._leases.release(board_step.step.zone, board_step.step.id)
    if accepted:
      self._board.complete(board_step.job_id, board_step.step.id)
      bidder.settle(board_step.step)
      if job.finished():
        self._settle_job(board_step.job_id, MissionState.SUCCEEDED, None)
    else:
      if ack is not None and not ack.succeeded:
        reason = ack.reason or str(
          ack.result.get("reason") or ack.reason_code or ack.status.value
        )
      elif verdict is not None:
        reason = verdict.reason or "safety gate rejected the primitive"
      else:
        reason = f"step timed out after {self._step_timeout_s}s"
      self._board.fail(board_step.job_id, board_step.step.id)
      self._settle_job(
        board_step.job_id, MissionState.FAILED, f"step {board_step.step.id}: {reason}"
      )

  def _settle_job(self, job_id: str, state: MissionState, detail: str | None) -> None:
    mission = self._mission_by_job.pop(job_id, None)
    self._board.retire(job_id)
    if mission is None:
      return
    if state is MissionState.SUCCEEDED:
      self._missions.finish(mission, state=state, result=detail)
    else:
      self._missions.finish(mission, state=state, error=detail)

  async def _observe(
    self,
    board_step: BoardStep,
    accepted: bool,
    ack: PrimitiveAck | None,
  ) -> None:
    """Admit the step outcome through perception — same ingress as planner steps."""
    if self._queue is None:
      return
    outcome = (
      f"accepted (sequence {ack.sequence})"
      if accepted and ack is not None
      else (f"rejected ({ack.reason})" if ack is not None else "rejected by gate")
    )
    await self._queue.enqueue(
      kind=RESULT_OBSERVATION_KIND,
      source=DECISION_SOURCE,
      text=f"{board_step.step.primitive}: {outcome}",
      metadata={
        "job_id": board_step.job_id,
        "step_id": board_step.step.id,
        "accepted": str(accepted).lower(),
      },
    )


def _log_executor_exit(task: asyncio.Task) -> None:
  """Surface an executor task's unexpected exit (shutdown cancels stay quiet)."""
  if task.cancelled():
    return
  exc = task.exception()
  if exc is not None:
    get_logger("egia.core.coordination").warning("executor_exited", error=str(exc))


def bidder_cost(bidder: BidderProfile, board_step: BoardStep) -> float:
  """The claim's switch-cost term for lease contention (§3.5 step 4)."""
  step = board_step.step
  switch = (
    0.0
    if bidder.last_primitive is None or bidder.last_primitive == step.primitive
    else bidder.primitive_switch_cost_s
  )
  return step.est_duration_s + switch


async def collect_bidders(
  links: ClientRegistry, registry: PrimitiveRegistry, backend_ids: list[str]
) -> list[BidderProfile]:
  """Compile bidders from the devices returned by EOS ``Discover``."""
  del registry
  bidders: list[BidderProfile] = []
  for backend in backend_ids:
    try:
      client = links.client(backend)
      discovery = await client.initialize(force=True)
      bidders.extend(bidder_profiles(backend, discovery))
    except Exception as exc:  # noqa: BLE001 — continue across endpoints
      get_logger("egia.core.coordination").warning(
        "bidder_rediscovery_failed", robot_id=backend, error=str(exc)
      )
      continue
  return bidders


async def coordinator_entry(
  coordinator: Coordinator, ready: asyncio.Event, stop: asyncio.Event
) -> None:
  """Supervisor-entry adapter."""
  await coordinator.run(ready, stop)
