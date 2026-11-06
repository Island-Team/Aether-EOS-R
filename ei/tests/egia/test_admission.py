"""Admission store behavior (the former observation queue)."""

from __future__ import annotations

import asyncio

import pytest
from structlog.testing import capture_logs

from egia.core.perception.admission import Admission


@pytest.mark.asyncio
async def test_enqueue_assigns_ids_and_preserves_fifo_order() -> None:
  queue = Admission(capacity=3)
  first = await queue.enqueue(kind="hearing", source="test", text="hello")
  second = await queue.enqueue(kind="hearing", source="test", text="world")
  assert first is not None and second is not None
  assert first.id != second.id
  assert [record.text for record in queue.records()] == ["hello", "world"]


@pytest.mark.asyncio
async def test_full_queue_rejects_new_records() -> None:
  queue = Admission(capacity=2)
  assert await queue.enqueue(kind="hearing", source="test", text="one") is not None
  assert await queue.enqueue(kind="hearing", source="test", text="two") is not None
  rejected = await queue.enqueue(kind="hearing", source="test", text="three")
  assert rejected is None
  assert queue.size == 2
  assert [record.text for record in queue.records()] == ["one", "two"]


@pytest.mark.asyncio
async def test_latest_by_kind() -> None:
  queue = Admission(capacity=4)
  await queue.enqueue(kind="hearing", source="test", text="heard")
  await queue.enqueue(kind="vision", source="test", text="seen")
  await queue.enqueue(kind="hearing", source="test", text="heard again")
  assert queue.latest_by_kind("hearing").text == "heard again"
  assert queue.latest_by_kind("vision").text == "seen"
  assert queue.latest_by_kind("touch") is None


@pytest.mark.asyncio
async def test_history_keeps_bounded_per_kind_admissions() -> None:
  queue = Admission(capacity=16, history_per_kind=3)
  for text in ("one", "two", "three", "four"):
    await queue.enqueue(kind="hearing", source="test", text=text)
  assert [record.text for record in queue.history()] == ["two", "three", "four"]
  assert queue.history(kinds={"touch"}) == []
  assert [record.text for record in queue.history(limit=2)] == ["three", "four"]


@pytest.mark.asyncio
async def test_coalescing_admissions_still_append_to_history() -> None:
  queue = Admission(capacity=8, history_per_kind=4)
  await queue.enqueue_latest(kind="scene", source="test", text="scene 0")
  await queue.enqueue_latest(kind="scene", source="test", text="scene 1")
  # The live FIFO holds only the newest; history keeps every admission.
  assert queue.latest_by_kind("scene").text == "scene 1"
  assert [record.text for record in queue.history(kinds={"scene"})] == [
    "scene 0",
    "scene 1",
  ]


@pytest.mark.asyncio
async def test_coalesced_replacements_are_debug_after_first_snapshot() -> None:
  queue = Admission(capacity=4)

  with capture_logs() as logs:
    await queue.enqueue_latest(kind="body", source="arm-0", text="first")
    await queue.enqueue_latest(kind="body", source="arm-0", text="replacement")
    await queue.enqueue_latest(kind="body", source="gripper-0", text="first")

  admissions = [entry for entry in logs if entry["event"] == "observation_enqueued"]
  assert [entry["log_level"] for entry in admissions] == ["info", "debug", "info"]
  assert [entry["coalesced"] for entry in admissions] == [False, True, False]


@pytest.mark.asyncio
async def test_latest_admission_coalesces_per_source_without_losing_backends() -> None:
  queue = Admission(capacity=4)
  await queue.enqueue_latest(kind="objects", source="objects.a", text="a0")
  await queue.enqueue_latest(kind="objects", source="objects.b", text="b0")
  await queue.enqueue_latest(kind="objects", source="objects.a", text="a1")

  assert [
    (record.source, record.text) for record in queue.records_by_kind("objects")
  ] == [
    ("objects.b", "b0"),
    ("objects.a", "a1"),
  ]
  assert queue.latest_by_kind("objects", source="objects.a").text == "a1"
  assert queue.latest_by_kind("objects", source="objects.b").text == "b0"
  assert queue.coalesced("objects") == 1


@pytest.mark.asyncio
async def test_wait_changed_returns_false_when_stopped() -> None:
  queue = Admission(capacity=2)
  stop = asyncio.Event()
  stop.set()
  assert await queue.wait_changed(stop) is False


@pytest.mark.asyncio
async def test_change_revisions_do_not_allow_one_subscriber_to_consume_another() -> (
  None
):
  queue = Admission(capacity=2)
  revision = queue.revision
  context_wait = asyncio.create_task(queue.wait_for_change(revision))
  proposer_wait = asyncio.create_task(queue.wait_for_change(revision))

  await queue.enqueue(kind="hearing", source="test", text="hello")

  assert await context_wait == queue.revision
  assert await proposer_wait == queue.revision


@pytest.mark.asyncio
async def test_change_revision_survives_legacy_notification_consumption() -> None:
  queue = Admission(capacity=2)
  revision = queue.revision
  await queue.enqueue(kind="hearing", source="test", text="hello")

  assert await queue.wait_changed() is True
  assert await queue.wait_for_change(revision) == queue.revision


@pytest.mark.asyncio
async def test_coalesced_counter_enqueue_latest() -> None:
  queue = Admission(capacity=4)
  assert queue.coalesced("detection") == 0
  await queue.enqueue_latest(kind="detection", source="s", text="a")
  assert queue.coalesced("detection") == 0
  await queue.enqueue_latest(kind="detection", source="s", text="b")
  assert queue.coalesced("detection") == 1
  await queue.enqueue_latest(kind="detection", source="s", text="c")
  assert queue.coalesced("detection") == 2
  assert queue.coalesced("hearing") == 0


@pytest.mark.asyncio
async def test_backend_rebind_restores_fifo_and_bounded_history() -> None:
  source = Admission(capacity=4, history_per_kind=2)
  await source.enqueue_latest(kind="camera", source="sandbox", text="frame 1")
  await source.enqueue_latest(kind="camera", source="sandbox", text="frame 2")

  restored = Admission(capacity=4, history_per_kind=2)
  revision = restored.revision
  restored.restore(source.records(), source.history())

  assert restored.revision == revision + 1
  assert [item.text for item in restored.records()] == ["frame 2"]
  assert [item.text for item in restored.history()] == ["frame 1", "frame 2"]
