"""Model invocation policy — the explicit answer to "when is the model called".

Every potential model call carries a trigger: ``mission_start`` (first
decomposition after recall), ``step`` (continuation after a primitive result),
``parse_retry`` (malformed reply, corrective history appended), or
``fault_interrupt`` (priority re-entry requested by a side loop). The policy
is a pure decision function over those triggers
(``docs/egia/egia-2607.md`` §3.8):

- a pending fault interrupt promotes any trigger to ``fault_interrupt``;
- the mission token budget denies every trigger equally — exhaustion fails
  the mission rather than the endpoint bill;
- the cooldown interval delays ordinary triggers but never an interrupt —
  urgency is the point of the interrupt path;
- between missions the model is never called: idle capacity belongs to
  memory consolidation, and emergencies belong to the safety estop path.

The admission decision holds no external I/O and takes an injected clock, so
every branch is unit-testable as a pure function. A level-triggered asyncio
event only lets the planner cancel a cooldown or model call promptly when an
interrupt arrives. Enforcement lives in the planner, which consults
:meth:`InvocationPolicy.admit` before each ``complete`` call, reserves in-flight
capacity with :meth:`InvocationPolicy.begin_call`, and settles actual usage after
completion.
"""

from __future__ import annotations

import asyncio
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from enum import Enum

__all__ = ["InvocationDecision", "InvocationPolicy", "InvocationTrigger"]


class InvocationTrigger(Enum):
  """Why the planner is about to call the model."""

  MISSION_START = "mission_start"
  STEP = "step"
  PARSE_RETRY = "parse_retry"
  FAULT_INTERRUPT = "fault_interrupt"
  PROPOSAL_EVENT = "proposal_event"  # perception woke the proposer (§3.9)
  PROPOSAL_IDLE = "proposal_idle"  # idle reflection timer fired (§3.9)


@dataclass
class InvocationDecision:
  """The policy verdict for one candidate call.

  ``call`` is ``True`` for an immediate call. A wait carries the remaining
  cooldown in seconds (the caller awaits, then re-admits); a denial carries
  the machine-readable reason for the plan record.
  """

  call: bool
  wait_s: float = 0.0
  reason: str = ""


@dataclass
class _Interrupt:
  source: str
  detail: str


@dataclass
class InvocationPolicy:
  """Per-mission gate over model calls; reset by the planner between missions."""

  token_budget: int = 0  # 0 = unlimited
  min_call_interval_s: float = 0.0  # 0 = no cooldown
  clock: Callable[[], float] = field(default_factory=lambda: time.monotonic)

  def __post_init__(self) -> None:
    self._tokens_spent = 0
    self._last_call: float | None = None
    self._interrupt: _Interrupt | None = None
    self._interrupt_changed = asyncio.Event()

  def admit(self, trigger: InvocationTrigger) -> InvocationDecision:
    """Decide one candidate call; interrupts pending promote the trigger."""
    if self._interrupt is not None:
      trigger = InvocationTrigger.FAULT_INTERRUPT
    if self.token_budget > 0 and self._tokens_spent >= self.token_budget:
      return InvocationDecision(
        call=False,
        reason=f"mission token budget exhausted ({self._tokens_spent} tokens)",
      )
    if trigger is not InvocationTrigger.FAULT_INTERRUPT and self._last_call is not None:
      elapsed = self.clock() - self._last_call
      remaining = self.min_call_interval_s - elapsed
      if remaining > 0:
        return InvocationDecision(call=False, wait_s=remaining)
    return InvocationDecision(call=True)

  def record(self, total_tokens: int) -> None:
    """Report one completed call's usage back into the budget accounting."""
    self._tokens_spent += max(total_tokens, 0)
    self._last_call = self.clock()

  def begin_call(self, reserved_tokens: int = 0) -> None:
    """Reserve an admitted in-flight call before another lane can admit."""
    self._tokens_spent += max(reserved_tokens, 0)
    self._last_call = self.clock()

  def settle_call(self, total_tokens: int, reserved_tokens: int = 0) -> None:
    """Replace one in-flight reservation with the provider's actual usage."""
    self._tokens_spent = max(
      self._tokens_spent - max(reserved_tokens, 0) + max(total_tokens, 0), 0
    )
    self._last_call = self.clock()

  def cancel_call(self, reserved_tokens: int = 0) -> None:
    """Release a call reservation that ended without token usage."""
    self._tokens_spent = max(self._tokens_spent - max(reserved_tokens, 0), 0)

  def request_interrupt(self, source: str, detail: str) -> None:
    """Ask the planner for priority re-entry; the newest interrupt wins."""
    self._interrupt = _Interrupt(source=source, detail=detail)
    self._interrupt_changed.set()

  def pop_interrupt(self) -> tuple[str, str] | None:
    """Take the pending interrupt, if any; the planner folds it into the prompt."""
    if self._interrupt is None:
      return None
    interrupt = self._interrupt
    self._interrupt = None
    self._interrupt_changed.clear()
    return interrupt.source, interrupt.detail

  async def wait_interrupt(self) -> None:
    """Wait until an interrupt is pending without consuming it."""
    await self._interrupt_changed.wait()

  @property
  def tokens_spent(self) -> int:
    return self._tokens_spent
