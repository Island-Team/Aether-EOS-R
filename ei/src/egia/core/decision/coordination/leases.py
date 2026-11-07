"""Zone leases — lock contention as the second auction (§3.5 step 4).

A shared-workspace zone is an exclusive lease: one running step occupies it
at a time, and a contender resolves against the holder by the doc's rule —
the lease goes to ``argmax(pi_i * c_switch_i)`` over holder and claimant
(priority class times re-sequencing cost), so a rush order with cheap
re-sequencing preempts an idle holder, while an expensive-to-move holder
keeps the lock against a low-priority claimant. The loser is deferred and
locally re-sequenced — its step returns to READY on the board; nothing
globally re-plans.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum

__all__ = ["LeaseClaim", "LeaseOutcome", "ZoneLeases"]


@dataclass(frozen=True)
class LeaseClaim:
  """One step's claim on a zone, priced for the contention rule.

  ``priority`` is the mission's class-weighted priority (operator missions
  outrank self-proposed, §3.9); ``switch_cost`` is what re-sequencing the
  claimant away from this zone would cost — the ``c_switch`` term of its
  bid.
  """

  step_id: str
  priority: float
  switch_cost: float


class LeaseOutcome(StrEnum):
  """The contention verdict for one acquire attempt."""

  ACQUIRED = "acquired"
  ACQUIRED_PREEMPTED = "acquired_preempted"  # holder lost, returns to READY
  DEFERRED = "deferred"  # claimant lost, stays READY for a later tick


class ZoneLeases:
  """Exclusive zone locks with §3.5-step-4 contention resolution."""

  def __init__(self) -> None:
    self._holders: dict[str, LeaseClaim] = {}

  def holder(self, zone: str) -> LeaseClaim | None:
    """The zone's current lease holder, if leased."""
    return self._holders.get(zone)

  def leased_zones(self) -> dict[str, LeaseClaim]:
    """A copy of every currently held lease."""
    return dict(self._holders)

  def acquire(self, zone: str, claim: LeaseClaim) -> LeaseOutcome:
    """Claim one zone; contention resolves on priority times switch cost.

    Raises:
      ValueError: When the claimant already holds the zone (a running step
        re-acquiring — a wiring bug, not contention).
    """
    holder = self._holders.get(zone)
    if holder is not None and holder.step_id == claim.step_id:
      raise ValueError(f"step {claim.step_id!r} already holds zone {zone!r}")
    if holder is None:
      self._holders[zone] = claim
      return LeaseOutcome.ACQUIRED
    if self._contention_score(claim) > self._contention_score(holder):
      self._holders[zone] = claim
      return LeaseOutcome.ACQUIRED_PREEMPTED
    return LeaseOutcome.DEFERRED

  def release(self, zone: str, step_id: str) -> LeaseClaim | None:
    """Release one zone; returns the released holder, or ``None``.

    Releasing a zone the step does not hold is a no-op — the preempted
    holder releases after it has already lost the lease, and that late
    release must not evict the new holder.
    """
    holder = self._holders.get(zone)
    if holder is not None and holder.step_id == step_id:
      del self._holders[zone]
      return holder
    return None

  @staticmethod
  def _contention_score(claim: LeaseClaim) -> float:
    # pi_i * c_switch_i (§3.5 step 4); a zero switch cost always loses —
    # re-sequencing the claimant is free, so the holder keeps the lock.
    return claim.priority * claim.switch_cost
