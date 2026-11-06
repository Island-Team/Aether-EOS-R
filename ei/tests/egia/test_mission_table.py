"""Mission table — priority scoring and table ordering (§3.9)."""

from __future__ import annotations

import asyncio
from datetime import datetime

import pytest

from egia.api.mission import MissionPriorityBand, MissionState
from egia.core.missions import MissionQueue, ResubmitOutcome
from egia.core.missions.priority import (
  MissionPriority,
  PriorityWeights,
  score_mission,
  should_preempt,
)
from egia.lib.config import PriorityConfig

WEIGHTS = PriorityWeights()


def _score(*, origin="self", urgency=0.5, value=0.5, cost=60.0, age=0.0):
  return score_mission(
    origin=origin,
    urgency=urgency,
    value=value,
    est_cost_s=cost,
    submitted_at=1000.0,
    config=WEIGHTS,
    now=1000.0 + age,
  )


def test_score_terms_add_and_subtract() -> None:
  high = _score(urgency=1.0, value=1.0, cost=0.0)
  low = _score(urgency=0.0, value=0.0, cost=100.0)
  assert high.score > low.score
  assert high.score == pytest.approx(2.0)


def test_operator_class_dominates_regardless_of_score() -> None:
  operator = _score(origin="operator", urgency=0.0, value=0.0, cost=10_000.0)
  autonomous = _score(origin="self", urgency=1.0, value=1.0, cost=0.0)
  assert operator.class_rank == 0
  assert autonomous.class_rank == 1
  assert operator.order_key < autonomous.order_key


def test_order_key_includes_submitted_at() -> None:
  """The documented ordering key is (class_rank, -score, submitted_at)."""
  older = score_mission(
    origin="self",
    urgency=0.5,
    value=0.5,
    est_cost_s=60.0,
    submitted_at=1000.0,
    config=WEIGHTS,
    now=2000.0,
  )
  newer = score_mission(
    origin="self",
    urgency=0.5,
    value=0.5,
    est_cost_s=60.0,
    submitted_at=1500.0,
    config=WEIGHTS,
    now=2000.0,
  )
  # Same class and (before the age term saturates) a near-equal score — the
  # older submission must still take first, resolved by submitted_at, not by
  # an equal-tuple comparison that would collapse on (class_rank, -score) alone.
  assert older.score == newer.score
  assert older.order_key < newer.order_key


def test_operator_submissions_take_configured_defaults() -> None:
  entry = _score(origin="operator", urgency=0.0, value=0.0)
  expected = WEIGHTS.operator_urgency + WEIGHTS.operator_value - WEIGHTS.w_cost * 60.0
  assert entry.score == pytest.approx(expected)


def test_age_term_grows_and_saturates() -> None:
  fresh = _score(age=0.0)
  waited = _score(age=WEIGHTS.age_tau_s)
  saturated = _score(age=10 * WEIGHTS.age_tau_s)
  assert fresh.score < waited.score
  assert waited.score == pytest.approx(saturated.score)


def test_missing_cost_falls_back_to_default() -> None:
  entry = score_mission(
    origin="self",
    urgency=0.5,
    value=0.5,
    est_cost_s=None,
    submitted_at=1000.0,
    config=WEIGHTS,
    now=1000.0,
  )
  assert entry.est_cost_s == WEIGHTS.default_cost_s


def test_should_preempt_class_and_margin() -> None:
  operator_pending = MissionPriority(score=0.0, class_rank=0, est_cost_s=60.0)
  self_running = MissionPriority(score=2.0, class_rank=1, est_cost_s=0.0)
  assert should_preempt(self_running, operator_pending, WEIGHTS)
  self_pending = MissionPriority(score=2.0, class_rank=1, est_cost_s=0.0)
  assert not should_preempt(self_running, self_pending, WEIGHTS)
  # Same class: margin decides, hysteresis around a tie.
  close = MissionPriority(
    score=self_running.score + WEIGHTS.preempt_margin / 2, class_rank=1, est_cost_s=0.0
  )
  clear = MissionPriority(
    score=self_running.score + WEIGHTS.preempt_margin, class_rank=1, est_cost_s=0.0
  )
  assert not should_preempt(self_running, close, WEIGHTS)
  assert should_preempt(self_running, clear, WEIGHTS)


def test_should_preempt_uses_band_before_margin_within_class() -> None:
  running = MissionPriority(
    score=2.0,
    class_rank=1,
    est_cost_s=0.0,
    band=MissionPriorityBand.HIGH,
  )
  lower = MissionPriority(
    score=20.0,
    class_rank=1,
    est_cost_s=0.0,
    band=MissionPriorityBand.NORMAL,
  )
  higher = MissionPriority(
    score=0.0,
    class_rank=1,
    est_cost_s=0.0,
    band=MissionPriorityBand.URGENT,
  )
  assert not should_preempt(running, lower, WEIGHTS)
  assert should_preempt(running, higher, WEIGHTS)


def _table() -> MissionQueue:
  return MissionQueue(capacity=8, priority=PriorityConfig())


@pytest.mark.asyncio
async def test_table_orders_operator_first_then_score() -> None:
  table = _table()
  await table.submit("operator order", source="webui")
  await table.submit(
    "weak self", source="self.proposer", origin="self", urgency=0.1, value=0.1
  )
  await table.submit(
    "strong self", source="self.proposer", origin="self", urgency=1.0, value=1.0
  )
  head = table.best_pending()
  assert head is not None and head.origin == "operator"
  first = await table.wait_pending(asyncio.Event())
  assert first is not None and first.origin == "operator"
  second = await table.wait_pending(asyncio.Event())
  assert second is not None and second.order == "strong self"


@pytest.mark.asyncio
async def test_table_orders_band_before_dynamic_score_within_origin_class() -> None:
  table = _table()
  await table.submit(
    "strong normal",
    source="self.proposer",
    origin="self",
    priority_band=MissionPriorityBand.NORMAL,
    urgency=1.0,
    value=1.0,
  )
  await table.submit(
    "weak high",
    source="self.proposer",
    origin="self",
    priority_band=MissionPriorityBand.HIGH,
    urgency=0.0,
    value=0.0,
  )
  operator = await table.submit(
    "operator low",
    source="operator",
    priority_band=MissionPriorityBand.LOW,
  )

  selected = await table.wait_pending(asyncio.Event())
  assert selected is not None and operator is not None and selected.id == operator.id
  high = await table.wait_pending(asyncio.Event())
  assert high is not None and high.order == "weak high"


@pytest.mark.asyncio
async def test_capacity_still_rejects() -> None:
  table = MissionQueue(capacity=1, priority=PriorityConfig())
  assert await table.submit("one", source="webui") is not None
  assert await table.submit("two", source="webui") is None


@pytest.mark.asyncio
async def test_check_preemption_operator_beats_running_self() -> None:
  table = _table()
  running = await table.submit("running self", source="self.proposer", origin="self")
  assert running is not None
  table.mark_running(running)
  await table.submit("operator rush", source="webui")
  winner = table.check_preemption(running)
  assert winner is not None and winner.origin == "operator"


@pytest.mark.asyncio
async def test_check_preemption_requires_margin_within_class() -> None:
  table = _table()
  running = await table.submit(
    "running", source="self.proposer", origin="self", urgency=0.5, value=0.5
  )
  assert running is not None
  table.mark_running(running)
  # Same hints, submitted moments later: score lead far below the margin.
  await table.submit(
    "similar", source="self.proposer", origin="self", urgency=0.5, value=0.5
  )
  assert table.check_preemption(running) is None
  await table.submit(
    "urgent", source="self.proposer", origin="self", urgency=1.0, value=1.0
  )
  assert table.check_preemption(running) is not None


@pytest.mark.asyncio
async def test_resubmit_counts_and_caps() -> None:
  table = _table()
  record = await table.submit("preempted", source="self.proposer", origin="self")
  assert record is not None
  stop = asyncio.Event()
  selected = await table.wait_pending(stop)
  assert selected is not None and selected.id == record.id  # the scheduler's pop
  table.mark_running(record)
  config = table.priority_config
  while record.preempted_count < config.max_preemptions:
    assert table.resubmit(record) is ResubmitOutcome.ADMITTED
    assert record.state == MissionState.PENDING
    selected = await table.wait_pending(stop)
    assert selected is not None and selected.id == record.id  # picked again
    table.mark_running(record)
  assert table.resubmit(record) is ResubmitOutcome.PREEMPTION_CAP
  # the cap rejection mutates nothing — the caller finishes the record
  assert record.state == MissionState.RUNNING
  assert record.started_at is not None
  assert record not in table.pending()


@pytest.mark.asyncio
async def test_resubmit_full_table_rejects_without_mutating() -> None:
  table = MissionQueue(capacity=1, priority=PriorityConfig())
  stop = asyncio.Event()
  record = await table.submit("preempted", source="self.proposer", origin="self")
  assert record is not None
  selected = await table.wait_pending(stop)
  assert selected is not None and selected.id == record.id  # popped; the slot frees
  table.mark_running(record)
  filler = await table.submit("filler", source="webui")
  assert filler is not None  # occupies the table's only slot

  assert table.resubmit(record) is ResubmitOutcome.TABLE_FULL
  # no mutation on the rejected path: count, state, and started_at stay
  assert record.preempted_count == 0
  assert record.state == MissionState.RUNNING
  assert record.started_at is not None
  assert [mission.id for mission in table.pending()] == [filler.id]


@pytest.mark.asyncio
async def test_self_pending_count_and_est_cost_pass_through() -> None:
  table = _table()
  await table.submit("self one", source="self.proposer", origin="self", est_cost_s=12.0)
  await table.submit("operator", source="webui")
  assert table.self_pending_count() == 1
  head = table.best_pending()
  assert head is None or head.origin == "operator"
  self_record = next(r for r in table.pending() if r.origin == "self")
  assert self_record.est_cost_s == 12.0
  assert self_record.submitted_at.tzinfo is not None
  assert isinstance(self_record.submitted_at, datetime)


@pytest.mark.asyncio
async def test_running_mission_returns_the_one_running_record_or_none() -> None:
  table = _table()
  assert table.running_mission is None
  stop = asyncio.Event()
  record = await table.submit("move", source="webui")
  assert record is not None
  selected = await table.wait_pending(stop)
  assert selected is not None and selected.id == record.id
  assert table.running_mission is None  # popped but not yet running
  table.mark_running(record)
  assert table.running_mission == record
  assert table.running_mission is not record
  table.finish(record, state=MissionState.SUCCEEDED, result="done")
  assert table.running_mission is None
