"""Mission priority scoring — the math behind the mission table (§3.9).

Weighted additive score in the style of the §3.4 route score: each mission
carries 0–1 ``urgency`` and ``value`` hints (the proposer supplies them for
self-originated missions; operator submissions get fixed defaults), an
estimated cost in seconds, and an age. The score is

    score = w_urgency·urgency + w_value·value − w_cost·est_cost
            + w_age·min(age/τ, 1)

Origin is a *class*, not a term: operators always outrank autonomy — the
table orders by ``(class_rank, −band, −score, submitted_at)`` and no score a
self-proposed mission can reach crosses the class boundary. The age term
only prevents starvation *within* a class.

Cost estimates come from plans-as-memory (§3.3): the measured duration of
the most similar recalled plan, falling back to a configured default. The
function is pure — injected clock, no I/O — so every property above is a
table test.
"""

from __future__ import annotations

import time
from collections.abc import Callable
from dataclasses import dataclass

from egia.api.mission import MissionPriorityBand

__all__ = ["MissionPriority", "PriorityWeights"]


@dataclass(frozen=True)
class PriorityWeights:
  """Weights and preemption thresholds (``[decision.priority]`` in egia.toml)."""

  w_urgency: float = 1.0
  w_value: float = 1.0
  w_cost: float = 0.001  # per estimated second
  w_age: float = 0.25
  age_tau_s: float = 300.0
  default_cost_s: float = 60.0
  operator_urgency: float = 0.8
  operator_value: float = 0.8
  preempt_margin: float = 0.5
  max_preemptions: int = 2


@dataclass(frozen=True)
class MissionPriority:
  """One mission's computed table entry."""

  score: float
  class_rank: int  # 0 = operator, 1 = self — lower rank wins first
  est_cost_s: float
  submitted_at_s: float = 0.0
  band: MissionPriorityBand = MissionPriorityBand.NORMAL

  @property
  def order_key(self) -> tuple[int, int, float, float]:
    """Ascending sort key over the pending table (smaller = taken first).

    Class dominates first, then descending band, descending score, and age.
    """
    return (
      self.class_rank,
      -_BAND_RANK[self.band],
      -self.score,
      self.submitted_at_s,
    )


_BAND_RANK = {
  MissionPriorityBand.LOW: 0,
  MissionPriorityBand.NORMAL: 1,
  MissionPriorityBand.HIGH: 2,
  MissionPriorityBand.URGENT: 3,
}


def class_rank(origin: str) -> int:
  """Operator missions are class 0, everything autonomy proposes class 1."""
  return 0 if origin == "operator" else 1


def score_mission(
  *,
  origin: str,
  band: MissionPriorityBand = MissionPriorityBand.NORMAL,
  urgency: float,
  value: float,
  est_cost_s: float | None,
  submitted_at: float,
  config: PriorityWeights,
  now: float | None = None,
  clock: Callable[[], float] | None = None,
) -> MissionPriority:
  """Compute one mission's table entry.

  ``submitted_at`` and ``now`` are ``time.time`` epoch seconds (age is wall
  time — a mission proposed before a pause must not lose its place to one
  proposed after); tests inject ``now`` directly.
  """
  rank = class_rank(origin)
  if rank == 0:
    urgency, value = config.operator_urgency, config.operator_value
  cost = config.default_cost_s if est_cost_s is None else max(est_cost_s, 0.0)
  if now is None:
    now = time.time() if clock is None else clock()
  age = max(now - submitted_at, 0.0)
  score = (
    config.w_urgency * _clamp01(urgency)
    + config.w_value * _clamp01(value)
    - config.w_cost * cost
    + config.w_age * min(age / config.age_tau_s, 1.0)
  )
  return MissionPriority(
    score=score,
    class_rank=rank,
    est_cost_s=cost,
    submitted_at_s=submitted_at,
    band=band,
  )


def should_preempt(
  running: MissionPriority, pending: MissionPriority, config: PriorityWeights
) -> bool:
  """Step-boundary yield rule: lower class always wins; same class needs margin.

  The margin is hysteresis — a pending mission must beat the running one by
  ``preempt_margin`` score points, so noise around a tie cannot oscillate the
  planner between missions.
  """
  if pending.class_rank < running.class_rank:
    return True
  if pending.class_rank > running.class_rank:
    return False
  pending_band = _BAND_RANK[pending.band]
  running_band = _BAND_RANK[running.band]
  if pending_band > running_band:
    return True
  if pending_band < running_band:
    return False
  return pending.score - running.score >= config.preempt_margin


def _clamp01(value: float) -> float:
  return max(0.0, min(1.0, value))
