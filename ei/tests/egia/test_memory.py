"""Memory consolidation — queued observations persist into the episodic store."""

from __future__ import annotations

import asyncio
from datetime import UTC, datetime

import pytest

from egia.api.plan import ActionRecord, PlanRecord, StepOutcome, StepRecord
from egia.core.memory import (
  EpisodicStore,
  ReferenceStore,
  WorkingStore,
  memory_consolidation_entry,
)
from egia.core.perception.admission import Admission


@pytest.mark.asyncio
async def test_consolidation_drains_queue_into_episodic_store(tmp_path) -> None:
  queue = Admission(capacity=8)
  store = EpisodicStore(tmp_path / "episodic.db")
  working = WorkingStore(tmp_path / "working.db")
  ready = asyncio.Event()
  stop = asyncio.Event()
  task = asyncio.create_task(
    memory_consolidation_entry(queue, store, working, ready, stop)
  )
  await ready.wait()
  try:
    await queue.enqueue(kind="scene", source="test", text="snapshot one")
    await queue.enqueue(
      kind="result", source="decision", text="base.velocity: accepted"
    )
    for _ in range(50):
      if store.count() >= 2:
        break
      await asyncio.sleep(0.01)
    assert store.count() == 2
    rows = store.recent(10)
    assert [kind for _, kind, _ in rows] == ["result", "scene"]
  finally:
    stop.set()
    await task
  # Reopening the same file keeps the records: consolidation is durable.
  assert EpisodicStore(tmp_path / "episodic.db").count() == 2


@pytest.mark.asyncio
async def test_append_is_idempotent(tmp_path) -> None:
  queue = Admission(capacity=1)
  await queue.enqueue(kind="scene", source="test", text="snapshot")
  record = queue.records()[0]
  store = EpisodicStore(tmp_path / "episodic.db")
  assert store.append(record)
  assert not store.append(record)
  assert store.count() == 1


def _plan(mission_id: str = "m-1", order: str = "dock the robot") -> PlanRecord:
  now = datetime.now(UTC)
  return PlanRecord(
    mission_id=mission_id,
    order=order,
    source="test",
    started_at=now,
    finished_at=now,
    outcome=StepOutcome.ABORTED,
    steps=[
      StepRecord(
        index=1,
        outcome=StepOutcome.SUCCEEDED,
        actions=[
          ActionRecord(index=1, action="base.velocity", outcome=StepOutcome.SUCCEEDED)
        ],
      )
    ],
  )


def test_working_store_round_trip(tmp_path) -> None:
  store = WorkingStore(tmp_path / "working.db")
  plan = _plan()
  assert store.get_plan("m-1") is None
  store.upsert_plan(plan)
  assert store.get_plan("m-1") == plan
  # Upsert replaces the row for the same mission.
  updated = plan.model_copy(update={"order": "dock the robot, revised"})
  store.upsert_plan(updated)
  assert store.get_plan("m-1") == updated
  store.drop("m-1")
  assert store.get_plan("m-1") is None
  # Stale listing: only missions absent from the active set come back.
  store.upsert_plan(_plan("m-2"))
  store.upsert_plan(_plan("m-3"))
  stale = store.list_stale({"m-2"})
  assert [p.mission_id for p in stale] == ["m-3"]
  assert store.list_stale({"m-2", "m-3"}) == []
  store.close()


def test_reference_store_promote_and_recall(tmp_path) -> None:
  store = ReferenceStore(tmp_path / "reference.db")
  plan = _plan()
  # Promotion without an outcome override keeps the plan's outcome.
  promoted = store.promote(plan, note="operator approved")
  assert promoted.outcome is StepOutcome.ABORTED
  assert promoted.reviewed_at is not None
  assert promoted.review_note == "operator approved"
  assert store.get_plan(plan.id) == promoted
  # With an override: only effective/ineffective are accepted, and review is
  # the only path that may set them.
  judged = store.promote(plan, outcome="effective")
  assert judged.outcome is StepOutcome.EFFECTIVE
  assert store.get_plan(plan.id).outcome is StepOutcome.EFFECTIVE
  with pytest.raises(ValueError, match="effective"):
    store.promote(plan, outcome="succeeded")
  # Keyword recall and listing, same semantics as episodic.
  assert [p.id for p in store.search_plans("DOCK", 10)] == [plan.id]
  assert store.search_plans("patrol", 10) == []
  assert [p.id for p in store.list_plans(10)] == [plan.id]
  assert store.get_plan("missing") is None
  store.close()


@pytest.mark.asyncio
async def test_stale_working_row_is_recovered_as_aborted(tmp_path) -> None:
  queue = Admission(capacity=8)
  store = EpisodicStore(tmp_path / "episodic.db")
  working = WorkingStore(tmp_path / "working.db")
  working.upsert_plan(_plan("m-dead"))
  ready = asyncio.Event()
  stop = asyncio.Event()
  task = asyncio.create_task(
    memory_consolidation_entry(
      queue, store, working, ready, stop, active_mission=lambda: None
    )
  )
  await ready.wait()
  try:
    # Any queue admission wakes the loop, which then re-checks working.db.
    await queue.enqueue(kind="scene", source="test", text="wake")
    for _ in range(50):
      if store.list_plans(1):
        break
      await asyncio.sleep(0.01)
    plans = store.list_plans(1)
    assert len(plans) == 1
    recovered = plans[0]
    assert recovered.mission_id == "m-dead"
    assert recovered.outcome is StepOutcome.ABORTED
    assert [(a.source, a.event) for a in recovered.annotations] == [
      ("memory", "crash_recovery")
    ]
    # The stale row is dropped once recovered.
    assert working.list_stale(set()) == []
  finally:
    stop.set()
    await task


@pytest.mark.asyncio
async def test_live_working_row_is_not_recovered(tmp_path) -> None:
  queue = Admission(capacity=8)
  store = EpisodicStore(tmp_path / "episodic.db")
  working = WorkingStore(tmp_path / "working.db")
  working.upsert_plan(_plan("m-live"))
  ready = asyncio.Event()
  stop = asyncio.Event()
  task = asyncio.create_task(
    memory_consolidation_entry(
      queue, store, working, ready, stop, active_mission=lambda: "m-live"
    )
  )
  await ready.wait()
  try:
    await queue.enqueue(kind="scene", source="test", text="wake")
    for _ in range(50):
      if store.count() >= 1:
        break
      await asyncio.sleep(0.01)
    assert store.list_plans(1) == []
    assert working.get_plan("m-live") is not None
  finally:
    stop.set()
    await task
