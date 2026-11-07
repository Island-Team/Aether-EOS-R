"""Durable mission checkpoints and restart restoration."""

from __future__ import annotations

import asyncio
import json
from datetime import UTC, datetime

import pytest

from egia.api.mission import MissionPriorityBand, MissionState
from egia.api.plan import PlanRecord, PlanStep, PlanStepStatus, StepOutcome
from egia.core.missions import MissionQueue, MissionStore
from egia.core.missions.document import MissionAction, MissionDocument, MissionStep
from egia.core.missions.store import MissionCheckpointPhase, MissionStoreError


def _plan(mission_id: str, order: str) -> PlanRecord:
  now = datetime.now(UTC)
  return PlanRecord(
    mission_id=mission_id,
    order=order,
    source="test",
    started_at=now,
    finished_at=now,
    outcome=StepOutcome.ABORTED,
    plan=[
      PlanStep(
        id="inspect",
        sentence="Inspect the target.",
        status=PlanStepStatus.RUNNING,
        commands=["camera.capture@default/camera-0 mode=rgb"],
      )
    ],
  )


class FailingMissionStore(MissionStore):
  """Inject a durable-write failure after a configured number of saves."""

  def __init__(self, root, *, fail_after: int) -> None:
    super().__init__(root)
    self._remaining = fail_after

  def save_mission(self, mission, document, revisions):
    if self._remaining == 0:
      raise MissionStoreError("injected write failure")
    self._remaining -= 1
    return super().save_mission(mission, document, revisions)


@pytest.mark.asyncio
async def test_checkpoint_round_trip_and_restore_running_as_pending(tmp_path) -> None:
  store = MissionStore(tmp_path / "missions")
  queue = MissionQueue(2, store=store)
  record = await queue.submit("检查仓库门", source="test")
  assert record is not None
  pending = store.load(record.id)
  assert pending is not None
  assert pending.phase is MissionCheckpointPhase.PENDING

  queue.mark_running(record)
  plan = _plan(record.id, record.order)
  queue.sync_plan(plan, active=True)

  executing = store.load(record.id)
  assert executing is not None
  assert executing.phase is MissionCheckpointPhase.EXECUTING
  assert executing.mission.state is MissionState.RUNNING
  assert executing.mission.order == "检查仓库门"
  assert executing.plan == plan
  assert list(store.root.glob(".*.tmp")) == []

  checkpoint_path = store.root / f"{record.id}.mission.json"
  old_payload = json.loads(checkpoint_path.read_text(encoding="utf-8"))
  assert old_payload["schema_version"] == 3
  assert old_payload["plan"]["schema_version"] == "5"
  old_payload["schema_version"] = 2
  old_payload["plan"]["schema_version"] = "3"
  checkpoint_path.write_text(json.dumps(old_payload), encoding="utf-8")
  executing = store.load(record.id)
  assert executing is not None
  assert executing.schema_version == 3
  assert executing.plan is not None and executing.plan.schema_version == "5"

  restored = MissionQueue(2, store=store)
  resumed = restored.restore_checkpoints(store.load_active())

  assert [item.id for item in restored.pending()] == [record.id]
  assert restored.pending()[0].state is MissionState.PENDING
  assert restored.pending()[0].started_at is None
  assert resumed[record.id] == plan
  restored_document = restored.document(record.id)
  assert restored_document is not None
  assert restored_document.state is MissionState.PENDING
  assert restored_document.revision == executing.document.revision + 1
  persisted = store.load(record.id)
  assert persisted is not None
  assert persisted.phase is MissionCheckpointPhase.PENDING


@pytest.mark.asyncio
async def test_terminal_checkpoint_is_not_restored(tmp_path) -> None:
  store = MissionStore(tmp_path / "missions")
  queue = MissionQueue(1, store=store)
  record = await queue.submit("finish me", source="test")
  assert record is not None
  queue.mark_running(record)
  queue.finish(record, state=MissionState.SUCCEEDED, result="done")

  checkpoint = store.load(record.id)
  assert checkpoint is not None
  assert checkpoint.phase is MissionCheckpointPhase.TERMINAL
  assert checkpoint.mission.result == "done"
  assert store.load_active() == []


@pytest.mark.asyncio
async def test_clear_removes_active_and_terminal_checkpoints(tmp_path) -> None:
  store = MissionStore(tmp_path / "missions")
  queue = MissionQueue(2, store=store)
  active = await queue.submit("active", source="test")
  terminal = await queue.submit("terminal", source="test")
  assert active is not None and terminal is not None
  queue.mark_running(terminal)
  queue.finish(terminal, state=MissionState.SUCCEEDED, result="done")

  store.clear()

  assert list(store.root.glob("*.mission.json")) == []
  assert store.load_active() == []


@pytest.mark.asyncio
async def test_cancelled_checkpoint_is_terminal_and_never_restored(tmp_path) -> None:
  store = MissionStore(tmp_path / "missions")
  queue = MissionQueue(1, store=store)
  record = await queue.submit("cancel me", source="test")
  assert record is not None

  cancelled = queue.cancel(record.id, expected_revision=1, reason="operator request")
  checkpoint = store.load(record.id)

  assert cancelled.state is MissionState.CANCELLED
  assert checkpoint is not None
  assert checkpoint.schema_version == 3
  assert checkpoint.phase is MissionCheckpointPhase.TERMINAL
  assert checkpoint.mission.state is MissionState.CANCELLED
  assert checkpoint.document.state is MissionState.CANCELLED
  assert store.load_active() == []
  assert MissionQueue(1, store=store).restore_checkpoints(store.load_active()) == {}


@pytest.mark.parametrize("old_version", [1, 2])
@pytest.mark.asyncio
async def test_v1_and_v2_checkpoints_migrate_priority_band(
  tmp_path, old_version: int
) -> None:
  store = MissionStore(tmp_path / str(old_version))
  queue = MissionQueue(1, store=store)
  record = await queue.submit("legacy", source="test")
  assert record is not None
  path = store.root / f"{record.id}.mission.json"
  payload = json.loads(path.read_text(encoding="utf-8"))
  payload["schema_version"] = old_version
  payload["mission"].pop("priority_band")
  payload["document"].pop("priority_band")
  for revision in payload["revisions"]:
    revision["document"].pop("priority_band")
  path.write_text(json.dumps(payload), encoding="utf-8")

  migrated = store.load(record.id)
  assert migrated is not None
  assert migrated.schema_version == 3
  assert migrated.mission.priority_band.value == "normal"


def test_invalid_checkpoint_fails_closed(tmp_path) -> None:
  root = tmp_path / "missions"
  store = MissionStore(root)
  (root / "broken.mission.json").write_text(
    '{"schema_version": 99, "mission": {}}', encoding="utf-8"
  )

  with pytest.raises(MissionStoreError, match="invalid mission checkpoint"):
    store.load_active()


@pytest.mark.asyncio
async def test_restore_rejects_backlog_larger_than_capacity(tmp_path) -> None:
  store = MissionStore(tmp_path / "missions")
  source = MissionQueue(2, store=store)
  assert await source.submit("one", source="test") is not None
  assert await source.submit("two", source="test") is not None

  target = MissionQueue(1)
  with pytest.raises(ValueError, match="backlog exceeds mission capacity"):
    target.restore_checkpoints(store.load_active())


@pytest.mark.asyncio
async def test_write_failure_does_not_admit_or_start_ghost_mission(tmp_path) -> None:
  rejected_store = FailingMissionStore(tmp_path / "rejected", fail_after=0)
  rejected = MissionQueue(1, store=rejected_store)
  with pytest.raises(MissionStoreError, match="injected write failure"):
    await rejected.submit("not admitted", source="test")
  assert rejected.pending() == []
  assert rejected.records() == []

  start_store = FailingMissionStore(tmp_path / "start", fail_after=1)
  queue = MissionQueue(1, store=start_store)
  record = await queue.submit("stay pending", source="test")
  assert record is not None
  handed_off = await queue.wait_pending(asyncio.Event())
  assert handed_off is not None and handed_off.id == record.id
  with pytest.raises(MissionStoreError, match="injected write failure"):
    queue.mark_running(record)
  assert record.state is MissionState.PENDING
  assert record.started_at is None
  pending = queue.pending()
  assert len(pending) == 1 and pending[0].id == record.id
  assert pending[0].state is MissionState.PENDING


@pytest.mark.asyncio
async def test_mutation_write_failure_leaves_memory_and_observers_unchanged(
  tmp_path,
) -> None:
  store = FailingMissionStore(tmp_path / "mutation", fail_after=1)
  queue = MissionQueue(1, store=store)
  notifications = 0

  def notified() -> None:
    nonlocal notifications
    notifications += 1

  queue.add_listener(notified)
  record = await queue.submit("unchanged", source="test")
  assert record is not None
  baseline = notifications

  with pytest.raises(MissionStoreError, match="injected write failure"):
    queue.reprioritize(record.id, MissionPriorityBand.URGENT, expected_revision=1)

  current = queue.get(record.id)
  document = queue.document(record.id)
  assert current is not None and document is not None
  assert current.priority_band is MissionPriorityBand.NORMAL
  assert document.priority_band is MissionPriorityBand.NORMAL
  assert document.revision == 1
  assert notifications == baseline


@pytest.mark.asyncio
async def test_step_write_failure_does_not_publish_started_work(tmp_path) -> None:
  store = FailingMissionStore(tmp_path / "step", fail_after=2)
  queue = MissionQueue(1, store=store)
  record = await queue.submit("execute", source="test")
  assert record is not None
  queue.revise(
    MissionDocument(
      mission_id=record.id,
      order=record.order,
      revision=2,
      steps=[
        MissionStep(
          index=1,
          actions=[MissionAction(index=1, robot_id="robot", action="inspect")],
        )
      ],
    )
  )

  with pytest.raises(MissionStoreError, match="injected write failure"):
    queue.begin_step(record.id, 1)

  document = queue.document(record.id)
  assert document is not None
  assert document.state is MissionState.PENDING
  assert document.steps[0].state.value == "pending"
