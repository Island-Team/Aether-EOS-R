"""Local context archive: durable JSONL, independent of optional recall."""

from __future__ import annotations

import json
from datetime import UTC, datetime

from egia.api.plan import PlanRecord, StepOutcome
from egia.core.memory import LocalMemoryStore


def test_local_memory_store_is_append_only_and_deduplicated(tmp_path) -> None:
  store = LocalMemoryStore(tmp_path / "var" / "context_archive.jsonl")

  assert store.archive_context("perception", "old view", reason="context_budget")
  assert not store.archive_context("perception", "old view", reason="context_budget")

  entries = [json.loads(line) for line in store.path.read_text().splitlines()]
  assert len(entries) == 1
  assert entries[0]["kind"] == "context"
  assert entries[0]["reason"] == "context_budget"
  assert entries[0]["payload"] == {"kind": "perception", "text": "old view"}


def test_local_memory_archives_completed_plan_once(tmp_path) -> None:
  now = datetime.now(UTC)
  plan = PlanRecord(
    id="plan-1",
    mission_id="mission-1",
    order="inspect the dock",
    source="test",
    started_at=now,
    finished_at=now,
    outcome=StepOutcome.SUCCEEDED,
    result="done",
  )
  store = LocalMemoryStore(tmp_path / "context_archive.jsonl")

  assert store.archive_plan(plan)
  assert not store.archive_plan(plan)
  assert '"kind": "plan"' in store.path.read_text(encoding="utf-8")
