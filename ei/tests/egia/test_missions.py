"""Mission queue — admission gate for internal mission proposals."""

from __future__ import annotations

import asyncio
from datetime import UTC, datetime

import pytest
from structlog.testing import capture_logs

from egia.api.mission import MissionPriorityBand, MissionState
from egia.api.plan import PlanRecord, PlanStep, PlanStepStatus, StepOutcome
from egia.core.decision import MissionFailedError, decision_entry
from egia.core.missions import (
  MissionConflictError,
  MissionNotFoundError,
  MissionNotPendingError,
  MissionQueue,
  MissionStartedError,
)
from egia.core.missions.document import MissionDocument, MissionPlan
from egia.internal.backend import BackendSessionHandoff


@pytest.mark.asyncio
async def test_submit_admits_until_capacity_then_rejects() -> None:
  queue = MissionQueue(capacity=2)
  first = await queue.submit("do one", source="test")
  second = await queue.submit("do two", source="test")
  assert first is not None and second is not None
  assert first.state == MissionState.PENDING
  assert await queue.submit("do three", source="test") is None
  assert len(queue.pending()) == 2
  assert len(queue.records()) == 2  # rejected orders leave no record


@pytest.mark.asyncio
async def test_wait_pending_hands_off_serially_in_order() -> None:
  queue = MissionQueue(capacity=4)
  stop = asyncio.Event()
  await queue.submit("first", source="test")
  await queue.submit("second", source="test")
  one = await queue.wait_pending(stop)
  two = await queue.wait_pending(stop)
  assert one is not None and two is not None
  assert (one.order, two.order) == ("first", "second")
  assert not queue.pending()


@pytest.mark.asyncio
async def test_prompt_table_identifies_running_and_scheduled_pending_work() -> None:
  queue = MissionQueue(capacity=3)
  self_mission = await queue.submit("inspect", source="test", origin="self")
  operator = await queue.submit("stop here", source="test", origin="operator")
  assert self_mission is not None and operator is not None
  selected = await queue.wait_pending(asyncio.Event())
  assert selected is not None and selected.id == operator.id
  queue.mark_running(selected)

  rendered = queue.render_for_prompt()

  assert (
    f"MISSION role=running id={operator.id[:8]} state=running origin=operator"
    in rendered
  )
  assert (
    f"MISSION role=pending id={self_mission.id[:8]} state=pending origin=self "
    'band=normal queue=1 order="inspect"'
  ) in rendered


@pytest.mark.asyncio
async def test_debug_logs_trace_mutation_selection_and_suspension_safely() -> None:
  queue = MissionQueue(capacity=1)
  secret_order = "inspect private loading dock"

  with capture_logs() as logs:
    record = await queue.submit(secret_order, source="test")
    assert record is not None
    queue.update_order(record.id, secret_order, expected_revision=1)
    selected = await queue.wait_pending(asyncio.Event())
    assert selected is not None
    queue.mark_running(selected)
    queue.suspend(selected, detail="private shutdown detail")

  events = {entry["event"]: entry for entry in logs}
  revised = events["mission_document_revised"]
  assert revised["mutation"] == "order"
  assert revised["revision"] == 2
  assert events["mission_selected"]["pending_count"] == 0
  assert events["mission_suspended"]["pending_count"] == 1
  assert all(secret_order not in str(entry) for entry in logs)
  assert all("private shutdown detail" not in str(entry) for entry in logs)


@pytest.mark.asyncio
async def test_wait_pending_returns_none_on_stop() -> None:
  queue = MissionQueue(capacity=2)
  stop = asyncio.Event()
  stop.set()
  assert await queue.wait_pending(stop) is None


@pytest.mark.asyncio
async def test_mark_running_and_finish_record_terminal_outcome() -> None:
  queue = MissionQueue(capacity=2)
  record = await queue.submit("order", source="test")
  assert record is not None
  queue.mark_running(record)
  assert record.state == MissionState.RUNNING
  assert record.started_at is not None
  queue.finish(record, state=MissionState.SUCCEEDED, result="done")
  assert record.state == MissionState.SUCCEEDED
  assert record.finished_at is not None
  assert record.result == "done"
  with pytest.raises(ValueError):
    queue.finish(record, state=MissionState.PENDING)

  assert [(event.state, event.detail) for event in queue.history()] == [
    (MissionState.SUCCEEDED, "done"),
  ]


@pytest.mark.asyncio
async def test_info_logs_report_lifecycle_and_changed_plan_progress() -> None:
  queue = MissionQueue(capacity=1)
  record = await queue.submit("inspect", source="test")
  assert record is not None

  with capture_logs() as logs:
    selected = await queue.wait_pending(asyncio.Event())
    assert selected is not None
    queue.mark_running(selected)
    plan = PlanRecord(
      mission_id=record.id,
      priority=record.priority,
      order=record.order,
      source=record.source,
      outcome=StepOutcome.ABORTED,
      started_at=datetime.now(UTC),
      finished_at=datetime.now(UTC),
      plan=[PlanStep(id="inspect", sentence="Inspect target")],
    )
    queue.sync_plan(plan, active=True)
    queue.sync_plan(plan, active=True)
    plan.plan[0].status = PlanStepStatus.RUNNING
    queue.sync_plan(plan, active=True)
    queue.finish(selected, state=MissionState.SUCCEEDED, result="done")

  by_event: dict[str, list[dict[str, object]]] = {}
  for entry in logs:
    by_event.setdefault(str(entry["event"]), []).append(entry)
  assert by_event["mission_started"][0]["state"] == "running"
  assert len(by_event["mission_progress"]) == 2
  assert by_event["mission_progress"][0]["pending_steps"] == 1
  assert by_event["mission_progress"][1]["executing_steps"] == 1
  assert by_event["mission_finished"][0]["state"] == "succeeded"
  assert by_event["mission_finished"][0]["has_result"] is True


@pytest.mark.asyncio
async def test_finish_archives_the_terminal_mission_through_callback() -> None:
  archived = []
  queue = MissionQueue(capacity=2, on_finished=archived.append)
  record = await queue.submit("order", source="test")
  assert record is not None
  queue.mark_running(record)
  queue.finish(record, state=MissionState.SUCCEEDED, result="done")

  assert len(archived) == 1
  assert archived[0].id == record.id
  assert archived[0].state is MissionState.SUCCEEDED


@pytest.mark.asyncio
async def test_history_overwrites_a_mission_regardless_of_intervening_updates() -> None:
  queue = MissionQueue(capacity=1)
  record = await queue.submit("order", source="test")
  assert record is not None

  queue.mark_running(record)
  queue._record_history(record, detail="still running")
  queue.finish(record, state=MissionState.SUCCEEDED, result="done")
  queue._record_history(record, detail="queued again")

  assert len(queue.history()) == 1
  assert queue.history()[0].state == MissionState.SUCCEEDED
  assert queue.history()[0].detail == "queued again"


@pytest.mark.asyncio
async def test_backend_handoff_restores_running_mission_as_pending() -> None:
  original = MissionQueue(capacity=2)
  record = await original.submit("retain me", source="test")
  assert record is not None
  original.mark_running(record)
  plan = PlanRecord(
    mission_id=record.id,
    priority=record.priority,
    order=record.order,
    source=record.source,
    outcome=StepOutcome.ABORTED,
    started_at=datetime.now(UTC),
    finished_at=datetime.now(UTC),
  )
  handoff = BackendSessionHandoff()
  handoff.capture([record], plan)

  records, plans = handoff.consume()
  restored = MissionQueue(capacity=2)
  restored.restore(records)

  assert handoff.revision == 1
  assert [item.id for item in restored.pending()] == [record.id]
  assert restored.pending()[0].state is MissionState.PENDING
  assert restored.pending()[0].started_at is None
  assert plans[record.id].mission_id == record.id


@pytest.mark.asyncio
async def test_shutdown_suspends_running_mission_for_next_launch() -> None:
  class StoppingPlanner:
    async def run(self, _mission, stop):
      stop.set()
      raise MissionFailedError("shutdown during mission")

  missions = MissionQueue(capacity=1)
  record = await missions.submit("resume me", source="test")
  assert record is not None
  ready = asyncio.Event()
  stop = asyncio.Event()

  await decision_entry(
    missions,
    StoppingPlanner(),  # type: ignore[arg-type]
    ready,
    stop,
  )

  assert ready.is_set()
  assert [item.id for item in missions.pending()] == [record.id]
  assert record.state is MissionState.PENDING
  assert record.finished_at is None
  assert missions.history()[0].detail == (
    "shutdown interrupted mission; resume on next launch"
  )


@pytest.mark.asyncio
async def test_exact_get_and_collection_readers_return_isolated_snapshots() -> None:
  queue = MissionQueue(capacity=1)
  record = await queue.submit("original", source="test")
  assert record is not None

  exact = queue.get(record.id)
  pending = queue.pending()[0]
  records = queue.records()[0]
  assert exact is not None
  exact.order = "changed"
  pending.order = "changed"
  records.order = "changed"
  record.order = "changed"

  assert queue.get(record.id) is not exact
  assert queue.get(record.id).order == "original"  # type: ignore[union-attr]
  assert queue.get("missing") is None


@pytest.mark.asyncio
async def test_backend_restore_preserves_priority_band_in_record_and_document() -> None:
  source = MissionQueue(capacity=1)
  record = await source.submit(
    "restore urgent", source="test", priority_band=MissionPriorityBand.URGENT
  )
  assert record is not None
  restored = MissionQueue(capacity=1)

  restored.restore([record])

  restored_record = restored.get(record.id)
  restored_document = restored.document(record.id)
  assert restored_record is not None and restored_document is not None
  assert restored_record.priority_band is MissionPriorityBand.URGENT
  assert restored_document.priority_band is MissionPriorityBand.URGENT


@pytest.mark.asyncio
async def test_update_order_synchronizes_document_plan_and_revision() -> None:
  queue = MissionQueue(capacity=1)
  record = await queue.submit("old order", source="test")
  assert record is not None
  queue.revise(
    MissionDocument(
      mission_id=record.id,
      order=record.order,
      revision=2,
      plan=MissionPlan(plan_id="draft", title=record.order),
    )
  )

  updated = queue.update_order(record.id, "  new order  ", expected_revision=2)
  document = queue.document(record.id)
  assert document is not None and document.plan is not None
  assert updated.order == document.order == document.plan.title == "new order"
  assert document.revision == 3
  assert [revision.document.order for revision in queue.revisions(record.id)] == [
    "old order",
    "old order",
    "new order",
  ]
  with pytest.raises(MissionConflictError) as conflict:
    queue.update_order(record.id, "stale", expected_revision=2)
  assert conflict.value.code == "mission_revision_conflict"


@pytest.mark.asyncio
async def test_mutations_require_physical_pending_and_started_order_is_rejected() -> (
  None
):
  queue = MissionQueue(capacity=1)
  record = await queue.submit("started", source="test")
  assert record is not None
  handed_off = await queue.wait_pending(asyncio.Event())
  assert handed_off is not None
  with pytest.raises(MissionNotPendingError):
    queue.reprioritize(record.id, MissionPriorityBand.HIGH, expected_revision=1)
  queue.mark_running(handed_off)
  assert queue.resubmit(handed_off).value == "admitted"
  with pytest.raises(MissionStartedError):
    queue.update_order(record.id, "replacement", expected_revision=1)
  with pytest.raises(MissionNotFoundError) as missing:
    queue.cancel("missing", expected_revision=1)
  assert missing.value.code == "mission_not_found"


@pytest.mark.asyncio
async def test_reprioritize_and_cancel_preserve_observers_and_archive_callback() -> (
  None
):
  archived = []
  notifications = 0
  queue = MissionQueue(capacity=2, on_finished=archived.append)

  def notified() -> None:
    nonlocal notifications
    notifications += 1

  queue.add_listener(notified)
  low = await queue.submit("low", source="test", priority_band=MissionPriorityBand.LOW)
  normal = await queue.submit("normal", source="test")
  assert low is not None and normal is not None
  baseline = notifications

  raised = queue.reprioritize(low.id, MissionPriorityBand.URGENT, expected_revision=1)
  assert raised.priority_band is MissionPriorityBand.URGENT
  assert queue.document(low.id).priority_band is MissionPriorityBand.URGENT  # type: ignore[union-attr]
  assert queue.best_pending().id == low.id  # type: ignore[union-attr]
  cancelled = queue.cancel(low.id, expected_revision=2, reason="no longer needed")

  assert cancelled.state is MissionState.CANCELLED
  assert cancelled.finished_at is not None
  assert [item.id for item in queue.pending()] == [normal.id]
  assert queue.history()[-1].state is MissionState.CANCELLED
  assert queue.history()[-1].priority_band is MissionPriorityBand.URGENT
  assert archived == [cancelled]
  assert notifications == baseline + 2
