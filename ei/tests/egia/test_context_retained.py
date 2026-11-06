"""Focused coverage for the retained bounded, inline model context."""

from __future__ import annotations

import json
from datetime import UTC, datetime

import pytest

from egia.api.observation import ObservationRecord
from egia.core.context import ContextBuilder
from egia.core.context.detection_digest import (
  build_detection_digest,
  build_detection_lines,
)
from egia.core.context.object_digest import build_object_digest, build_object_lines
from egia.core.perception.admission import Admission
from egia.lib.config import (
  AgentPromptConfig,
  PromptConfig,
  PromptInstructionSection,
  PromptSectionConfig,
)


def _record(
  kind: str,
  text: str,
  *,
  record_id: str = "obs-1",
  source: str = "robot-a",
  metadata: dict[str, str] | None = None,
) -> ObservationRecord:
  return ObservationRecord(
    id=record_id,
    kind=kind,
    source=source,
    timestamp=datetime(2026, 1, 2, 3, 4, 5, tzinfo=UTC),
    text=text,
    metadata=metadata or {},
  )


def _lane(name: str) -> AgentPromptConfig:
  return AgentPromptConfig(
    sections=[PromptInstructionSection(name=f"{name}_instruction", text="test")]
  )


def _prompts(*sections: PromptSectionConfig, token_budget: int = 4096) -> PromptConfig:
  return PromptConfig(
    version="test",
    token_budget=token_budget,
    context=list(sections),
    planner=_lane("planner"),
    reasoner=_lane("reasoner"),
    summarizer=_lane("summarizer"),
    proposer=_lane("proposer"),
    sentinel=_lane("sentinel"),
    profile_writer=_lane("profile_writer"),
  )


def test_detection_digest_reports_inline_omissions_without_a_read_handle() -> None:
  renderings = [f"part-{index:02d} block confidence=0.9" for index in range(35)]
  record = _record(
    "detection",
    json.dumps(renderings),
    metadata={"detection_count": "40", "held_object_id": "part-03"},
  )

  lines = build_detection_lines(record)
  digest = build_detection_digest(record)

  assert lines[0] == "vision count=40"
  assert renderings[31] in lines
  assert renderings[32] not in lines
  assert "vision omitted=8" in lines
  assert lines[-1] == "holding part-03"
  assert digest is not None
  assert "omitted=8" in digest
  assert "holding part-03" in digest
  assert "context.read" not in digest
  assert "snapshot_id" not in digest


def test_detection_digest_marks_malformed_payload_unavailable() -> None:
  record = _record("detection", '{"detections": []}', record_id="bad-vision")

  assert build_detection_lines(record) == [
    "vision unavailable reason=invalid_payload source=robot-a snapshot_id=bad-vision"
  ]
  assert build_detection_digest(record) == build_detection_lines(record)[0]


def test_detection_digest_bounds_oversized_renderings_by_characters() -> None:
  first = "part-a " + "x" * 2000
  second = "part-b " + "y" * 2000

  lines = build_detection_lines(_record("detection", json.dumps([first, second])))

  assert first in lines
  assert second not in lines
  assert lines[-1] == "vision omitted=1"


def test_detection_digest_drops_an_oversized_item_with_an_exact_count() -> None:
  record = _record("detection", json.dumps(["x" * 4000, "small detection"]))

  lines = build_detection_lines(record)

  assert lines == ["vision count=2", "vision omitted=2"]
  assert "x" * 100 not in (build_detection_digest(record) or "")


def test_object_digest_reports_bounded_inline_objects_and_omitted_count() -> None:
  objects = [
    {
      "object_id": f"item-{index:02d}",
      "kind": "block",
      "location": {"place_id": "table", "slot_id": index},
      "states": ["visible"],
    }
    for index in range(15)
  ]
  record = _record(
    "objects",
    json.dumps(
      {
        "objects": objects,
        "stock": [{"inventory_id": "bin-a", "kind": "block", "available": 6}],
      }
    ),
    metadata={"source_sequence": "12"},
  )

  lines = build_object_lines(record)
  digest = build_object_digest(record)

  assert lines[0] == "objects count=15 source_sequence=12"
  assert lines[1] == "stock bin-a:block=6"
  assert any(line.startswith("item-11 block") for line in lines)
  assert not any(line.startswith("item-12 block") for line in lines)
  assert lines[-1] == "objects omitted=3"
  assert "objects omitted=3" in digest
  assert "context.read" not in digest
  assert "snapshot_id" not in digest


def test_object_digest_marks_malformed_payload_unavailable() -> None:
  record = _record("objects", "[]", record_id="bad-objects")

  assert build_object_lines(record) == [
    "objects unavailable invalid_payload source=robot-a snapshot_id=bad-objects"
  ]


def test_object_digest_bounds_oversized_summaries_by_characters() -> None:
  objects = [
    {"object_id": "item-a", "properties": {"payload": "x" * 2000}},
    {"object_id": "item-b", "properties": {"payload": "y" * 2000}},
  ]

  lines = build_object_lines(
    _record("objects", json.dumps({"objects": objects, "stock": []}))
  )

  assert any(line.startswith("item-a object") for line in lines)
  assert not any(line.startswith("item-b object") for line in lines)
  assert lines[-1] == "objects omitted=1"


def test_object_digest_bounds_oversized_stock_and_reports_omissions() -> None:
  stock = [
    {"inventory_id": f"bin-{index}-" + character * 600, "available": index}
    for index, character in enumerate(("a", "b", "c"))
  ]

  lines = build_object_lines(
    _record("objects", json.dumps({"objects": [], "stock": stock}))
  )

  assert lines[1].startswith("stock bin-0-")
  assert "bin-1-" not in lines[1]
  assert lines[1].endswith("omitted=2")


def test_object_digest_drops_oversized_state_with_an_exact_count() -> None:
  record = _record(
    "objects",
    json.dumps(
      {
        "objects": [
          {"object_id": "huge", "kind": "part", "properties": {"raw": "x" * 4000}}
        ],
        "stock": [{"inventory_id": "x" * 1200, "kind": "part", "available": 1}],
      }
    ),
  )

  lines = build_object_lines(record)

  assert lines == ["objects count=1", "stock omitted=1", "objects omitted=1"]


@pytest.mark.asyncio
async def test_budget_drops_low_priority_data_and_removes_its_observation_id() -> None:
  queue = Admission(4)
  low = await queue.enqueue(kind="environment", source="sensor", text="L" * 1200)
  high = await queue.enqueue(kind="faults", source="monitor", text="keep this fault")
  assert low is not None and high is not None
  prompts = _prompts(
    PromptSectionConfig(
      kind="environment",
      format="Environment: {data}",
      omitted="[{kind} unavailable within budget]",
      priority=10,
    ),
    PromptSectionConfig(
      kind="faults", format="Faults: {data}", empty="no faults", priority=90
    ),
    token_budget=256,
  )

  snapshot = ContextBuilder(queue, prompts).build()
  environment, faults = snapshot.sections

  assert environment.rendered == "[environment unavailable within budget]"
  assert environment.omitted is True
  assert faults.rendered == "Faults: keep this fault"
  assert faults.omitted is False
  assert low.id not in snapshot.observation_ids
  assert high.id in snapshot.observation_ids


@pytest.mark.asyncio
async def test_equal_priority_budget_drop_is_stable_in_config_order() -> None:
  queue = Admission(4)
  first = await queue.enqueue(kind="alpha", source="test", text="A" * 600)
  second = await queue.enqueue(kind="beta", source="test", text="B" * 600)
  assert first is not None and second is not None
  prompts = _prompts(
    PromptSectionConfig(kind="alpha", format="Alpha: {data}", priority=50),
    PromptSectionConfig(kind="beta", format="Beta: {data}", priority=50),
    token_budget=256,
  )

  snapshot = ContextBuilder(queue, prompts).build()
  alpha, beta = snapshot.sections

  assert alpha.omitted is True
  assert beta.omitted is False
  assert snapshot.observation_ids == [second.id]


def test_empty_fallbacks_are_not_relabelled_as_budget_omissions() -> None:
  prompts = _prompts(
    PromptSectionConfig(kind="alpha", format="Alpha: {data}", empty="A" * 600),
    PromptSectionConfig(kind="beta", format="Beta: {data}", empty="B" * 600),
    token_budget=256,
  )

  snapshot = ContextBuilder(Admission(2), prompts).build()

  assert [section.omitted for section in snapshot.sections] == [False, False]
  assert snapshot.observation_ids == []
