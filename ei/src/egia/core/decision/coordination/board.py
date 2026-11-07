"""Task board — live execution state over stored profile steps.

The board is where a profile stops being a note and becomes work: each
admitted mission's profile is one job, its steps move READY → RUNNING →
DONE/FAILED as the market awards and the executors settle, and the ready
set is exactly the auction's input. Jobs execute in admission order and a
step's dependencies may span only its own job — two orders never share
steps, keeping the "one order's calls never interleave on one robot"
invariant auditable per job.
"""

from __future__ import annotations

import itertools
import uuid
from dataclasses import dataclass, field
from enum import StrEnum

from egia.core.decision.profiles import Profile, ProfileStep, ready_steps

__all__ = ["BoardJob", "BoardStep", "StepState", "TaskBoard"]


class StepState(StrEnum):
  WAITING = "waiting"  # dependencies not yet settled
  READY = "ready"
  RUNNING = "running"
  DONE = "done"
  FAILED = "failed"


@dataclass
class BoardStep:
  """One profile step with its live board state."""

  step: ProfileStep
  job_id: str
  state: StepState = StepState.READY
  assignee: str | None = None  # bidder id (backend:part)


@dataclass
class BoardJob:
  """One admitted mission's profile instantiated on the board."""

  job_id: str
  profile: Profile
  mission_id: str
  steps: dict[str, BoardStep] = field(default_factory=dict)

  @property
  def done(self) -> set[str]:
    """Step ids finished successfully — the ready-set input."""
    return {
      step_id
      for step_id, board_step in self.steps.items()
      if board_step.state is StepState.DONE
    }

  def finished(self) -> bool:
    """True when every step is DONE (the job's success condition)."""
    return bool(self.steps) and all(
      board_step.state is StepState.DONE for board_step in self.steps.values()
    )

  def failed(self) -> bool:
    return any(
      board_step.state is StepState.FAILED for board_step in self.steps.values()
    )


class TaskBoard:
  """Bounded job table: admission, ready sets, award and completion bookkeeping."""

  def __init__(self, capacity: int = 8) -> None:
    if capacity < 1:
      raise ValueError("task board capacity must be at least 1")
    self._capacity = capacity
    self._jobs: dict[str, BoardJob] = {}
    self._order: list[str] = []
    self._ids = itertools.count(1)

  @property
  def capacity(self) -> int:
    return self._capacity

  def add_job(self, profile: Profile, mission_id: str) -> BoardJob | None:
    """Admit one job; ``None`` when the table is full (backpressure).

    Admission is the precondition filter: only steps with no dependencies
    start READY, exactly ``ready_steps`` says, so the board never holds a
    step whose dependencies have not settled.
    """
    if len(self._jobs) >= self._capacity:
      return None
    job_id = f"job-{next(self._ids)}-{uuid.uuid4().hex[:6]}"
    job = BoardJob(job_id=job_id, profile=profile, mission_id=mission_id)
    ready = {step.id for step in ready_steps(profile, set())}
    for step in profile.steps:
      job.steps[step.id] = BoardStep(
        step=step,
        job_id=job_id,
        state=StepState.READY if step.id in ready else StepState.WAITING,
      )
    self._jobs[job_id] = job
    self._order.append(job_id)
    return job

  def jobs(self) -> list[BoardJob]:
    """All jobs in admission order."""
    return [self._jobs[job_id] for job_id in self._order if job_id in self._jobs]

  def job(self, job_id: str) -> BoardJob | None:
    return self._jobs.get(job_id)

  def ready(self) -> list[BoardStep]:
    """Every READY step, jobs in admission order, steps in profile order.

    This is the auction's input; capped per job by nothing but the ready
    set itself — the precondition filter is what keeps it small (§3.5).
    """
    ready: list[BoardStep] = []
    for job in self.jobs():
      for step in job.profile.steps:
        board_step = job.steps[step.id]
        if board_step.state is StepState.READY:
          ready.append(board_step)
      if job.failed():
        break  # a failed job's downstream steps never run; stop at it
    return ready

  def award(self, job_id: str, step_id: str, assignee: str) -> None:
    """Mark one step RUNNING under its winning bidder."""
    board_step = self._require(job_id, step_id)
    if board_step.state is not StepState.READY:
      raise ValueError(f"step {step_id!r} is {board_step.state.value}, not awardable")
    board_step.state = StepState.RUNNING
    board_step.assignee = assignee

  def complete(self, job_id: str, step_id: str) -> list[BoardStep]:
    """Settle one step; returns the steps this completion made ready."""
    board_step = self._require(job_id, step_id)
    if board_step.state is not StepState.RUNNING:
      raise ValueError(f"step {step_id!r} is {board_step.state.value}, not settleable")
    board_step.state = StepState.DONE
    job = self._jobs[job_id]
    newly: list[BoardStep] = []
    for step in job.profile.steps:
      dependent = job.steps[step.id]
      if dependent.state is StepState.WAITING and all(
        dep in job.done for dep in step.after
      ):
        dependent.state = StepState.READY
        newly.append(dependent)
    return newly

  def fail(self, job_id: str, step_id: str) -> None:
    """Fail one step; downstream steps of its job never become ready."""
    board_step = self._require(job_id, step_id)
    board_step.state = StepState.FAILED

  def retire(self, job_id: str) -> None:
    """Drop a finished (done or failed) job from the table."""
    job = self._jobs.get(job_id)
    if job is None:
      return
    if not (job.finished() or job.failed()):
      raise ValueError(f"job {job_id!r} is still running; cannot retire")
    del self._jobs[job_id]
    self._order.remove(job_id)

  def _require(self, job_id: str, step_id: str) -> BoardStep:
    job = self._jobs.get(job_id)
    if job is None:
      raise KeyError(f"unknown job {job_id!r}")
    board_step = job.steps.get(step_id)
    if board_step is None:
      raise KeyError(f"unknown step {step_id!r} on job {job_id!r}")
    return board_step
