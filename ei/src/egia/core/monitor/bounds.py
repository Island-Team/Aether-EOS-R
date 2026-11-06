"""Hardcoded physical bounds and the egress rate window (monitor.md).

Two low-level, high-frequency checks with no model involvement: per-frame
velocity bounds over fused scene snapshots (a violation is a fault
observation immediately, a persistent streak escalates to the estop latch),
and the sliding 60 s egress-rate budget the mission-level gate consults.
"""

from __future__ import annotations

import math
import time
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass

from pydantic import BaseModel

from egia.contracts.perception.pose import PlanarVelocity
from egia.contracts.perception.world.scene import SceneSnapshot
from egia.core.monitor.latch import EstopLatch
from egia.core.perception.admission import Admission
from egia.lib.logger import get_logger

BOUND_VIOLATION_REASON = "bound_violation"
MONITOR_SOURCE = "monitor"

__all__ = [
  "BOUND_VIOLATION_REASON",
  "BoundViolation",
  "BoundWatchdog",
  "PhysicalBounds",
  "RateWindow",
]


@dataclass(frozen=True)
class BoundViolation:
  """One measured quantity over its hardcoded maximum."""

  quantity: str
  value: float
  limit: float


@dataclass(frozen=True)
class PhysicalBounds:
  """The deployment constant table for the per-frame base checks."""

  v_max_m_s: float = 2.0
  omega_max_rad_s: float = 3.14159
  escalation_ticks: int = 5

  def check_velocity(self, velocity: PlanarVelocity) -> BoundViolation | None:
    """Violation of the planar velocity or yaw-rate bound, or ``None``."""
    speed = math.hypot(velocity.x_m_s, velocity.y_m_s)
    yaw_rate = abs(velocity.yaw_rad_s)
    if speed > self.v_max_m_s:
      return BoundViolation("velocity", speed, self.v_max_m_s)
    if yaw_rate > self.omega_max_rad_s:
      return BoundViolation("yaw_rate", yaw_rate, self.omega_max_rad_s)
    return None


class RateWindow:
  """The sliding 60 s egress budget: one timestamp per admitted call.

  Only fully admitted calls append — an admitted call whose egress then
  fails still consumed its slot: the budget bounds egress attempts, not
  successes. Owned by the monitor; the mission-level gate consults
  ``admit`` as part of its admission.
  """

  def __init__(
    self,
    max_calls_per_minute: int = 60,
    clock: Callable[[], float] = time.monotonic,
  ) -> None:
    self._max = max_calls_per_minute
    self._clock = clock
    self._admitted: deque[float] = deque()

  def admit(self) -> bool:
    """Prune the expired window, then admit iff a slot remains."""
    now = self._clock()
    while self._admitted and now - self._admitted[0] >= 60.0:
      self._admitted.popleft()
    if len(self._admitted) >= self._max:
      return False
    self._admitted.append(now)
    return True


class BoundWatchdog:
  """Per-frame bound checks inline in the parts publish path.

  Each scene snapshot with observed base velocity over a bound enqueues a
  ``bound_violation`` fault
  immediately (invariant 4 — findings enter through admission); a violation
  streak persisting to ``escalation_ticks`` escalates ONCE via the estop
  latch. A healthy frame resets the streak and re-arms escalation.
  """

  def __init__(
    self, bounds: PhysicalBounds, queue: Admission, latch: EstopLatch
  ) -> None:
    self._bounds = bounds
    self._queue = queue
    self._latch = latch
    self._streak = 0
    self._escalated = False
    self._log = get_logger("egia.core.monitor")

  async def check_frame(self, _kind: object, snapshot: BaseModel) -> None:
    """Check observed planar velocity in one fused scene snapshot."""
    if (
      not isinstance(snapshot, SceneSnapshot)
      or snapshot.robot is None
      or snapshot.robot.velocity is None
    ):
      return
    violation = self._bounds.check_velocity(snapshot.robot.velocity)
    if violation is None:
      self._streak = 0
      self._escalated = False
      return
    self._streak += 1
    self._log.warning(
      "bound_violation",
      quantity=violation.quantity,
      value=round(violation.value, 3),
      limit=violation.limit,
      frame=snapshot.frame_id,
    )
    await self._queue.enqueue(
      kind="fault",
      source=MONITOR_SOURCE,
      text=f"base {violation.quantity} {violation.value:.3f} exceeds {violation.limit}",
      metadata={
        "reason": BOUND_VIOLATION_REASON,
        "quantity": violation.quantity,
        "value": f"{violation.value:.3f}",
        "limit": str(violation.limit),
      },
    )
    if self._streak >= self._bounds.escalation_ticks and not self._escalated:
      self._escalated = True
      await self._latch.engage(
        f"bound_violation: {violation.quantity} {violation.value:.3f} > "
        f"{violation.limit} for {self._streak} consecutive frames"
      )
