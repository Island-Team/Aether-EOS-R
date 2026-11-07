"""Model invocation policy — pure admission logic (docs/egia/egia-2607.md §3.8)."""

from __future__ import annotations

import pytest

from egia.core.decision.policy import InvocationPolicy, InvocationTrigger


class FakeClock:
  def __init__(self) -> None:
    self.now = 100.0

  def __call__(self) -> float:
    return self.now

  def advance(self, seconds: float) -> None:
    self.now += seconds


def test_first_call_admits_any_trigger() -> None:
  policy = InvocationPolicy(token_budget=10, min_call_interval_s=5.0, clock=FakeClock())
  for trigger in InvocationTrigger:
    assert policy.admit(trigger).call


def test_budget_denies_every_trigger() -> None:
  clock = FakeClock()
  policy = InvocationPolicy(token_budget=10, clock=clock)
  policy.record(10)
  for trigger in InvocationTrigger:
    decision = policy.admit(trigger)
    assert not decision.call
    assert "budget" in decision.reason


def test_budget_zero_means_unlimited() -> None:
  policy = InvocationPolicy(token_budget=0, clock=FakeClock())
  policy.record(1_000_000)
  assert policy.admit(InvocationTrigger.STEP).call


def test_cooldown_waits_then_admits() -> None:
  clock = FakeClock()
  policy = InvocationPolicy(min_call_interval_s=2.0, clock=clock)
  policy.record(0)
  clock.advance(0.5)
  decision = policy.admit(InvocationTrigger.STEP)
  assert not decision.call
  assert decision.wait_s == pytest.approx(1.5)
  clock.advance(1.5)
  assert policy.admit(InvocationTrigger.STEP).call


def test_fault_interrupt_bypasses_cooldown() -> None:
  clock = FakeClock()
  policy = InvocationPolicy(min_call_interval_s=60.0, clock=clock)
  policy.record(0)
  assert policy.admit(InvocationTrigger.FAULT_INTERRUPT).call


def test_pending_interrupt_promotes_ordinary_trigger() -> None:
  clock = FakeClock()
  policy = InvocationPolicy(min_call_interval_s=60.0, clock=clock)
  policy.record(0)
  policy.request_interrupt("monitor", "step stall")
  # An ordinary trigger is promoted, so the cooldown does not apply.
  assert policy.admit(InvocationTrigger.STEP).call
  # The promotion consumed nothing; pop still returns the interrupt.
  assert policy.pop_interrupt() == ("monitor", "step stall")


def test_pop_interrupt_returns_none_when_empty_and_newest_wins() -> None:
  policy = InvocationPolicy(clock=FakeClock())
  assert policy.pop_interrupt() is None
  policy.request_interrupt("audit", "fault one")
  policy.request_interrupt("monitor", "fault two")
  assert policy.pop_interrupt() == ("monitor", "fault two")
  assert policy.pop_interrupt() is None


def test_record_accumulates_tokens() -> None:
  policy = InvocationPolicy(token_budget=100, clock=FakeClock())
  policy.record(30)
  policy.record(45)
  assert policy.tokens_spent == 75
  assert policy.admit(InvocationTrigger.STEP).call
  policy.record(25)
  assert policy.tokens_spent == 100
  assert not policy.admit(InvocationTrigger.STEP).call


def test_inflight_reservation_blocks_concurrent_budget_and_cooldown() -> None:
  clock = FakeClock()
  policy = InvocationPolicy(token_budget=100, min_call_interval_s=5.0, clock=clock)

  assert policy.admit(InvocationTrigger.STEP).call
  policy.begin_call(100)

  assert not policy.admit(InvocationTrigger.STEP).call
  policy.settle_call(20, 100)
  assert policy.tokens_spent == 20
  clock.advance(5.0)
  assert policy.admit(InvocationTrigger.STEP).call
