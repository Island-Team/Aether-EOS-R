"""sqlite-vec recall remains an optional, offline memory implementation."""

from __future__ import annotations

import asyncio
import math
import sys
import types
from datetime import UTC, datetime
from pathlib import Path

import pytest

from egia.api.mission import MissionRecord
from egia.api.plan import ActionRecord, PlanRecord, StepOutcome, StepRecord
from egia.core.context import ContextBuilder
from egia.core.decision import DecisionPlanner
from egia.core.memory import EpisodicStore, RecallService, ReferenceStore
from egia.core.perception.admission import Admission
from egia.core.registry import PrimitiveRegistry
from egia.lib.config import DecisionConfig, LaunchConfig, load_prompt_config
from egia.lib.vlm import ChatMessage, ModelResponse


def _vec_index(tmp_path: Path):
  """Build the real optional index or skip when the extra is not installed."""
  pytest.importorskip("sqlite_vec")
  from egia.opt.memory.vec import SqliteVecIndex

  return SqliteVecIndex(tmp_path / "vec.db")


def _plan(order: str, outcome: StepOutcome, *, plan_id: str) -> PlanRecord:
  now = datetime.now(UTC)
  return PlanRecord(
    id=plan_id,
    mission_id=f"mission-{plan_id}",
    order=order,
    source="test",
    started_at=now,
    finished_at=now,
    outcome=outcome,
    steps=[
      StepRecord(
        index=1,
        outcome=outcome,
        actions=[
          ActionRecord(
            index=1,
            action="base.velocity",
            command="base.velocity@robot-a/base-0 linear_x_m_s=0.1",
            outcome=outcome,
          )
        ],
      ),
      StepRecord(index=2, outcome=outcome, final="done"),
    ],
  )


def test_embedder_is_deterministic_case_insensitive_and_normalized() -> None:
  pytest.importorskip("sqlite_vec")
  from egia.opt.memory.vec import EMBEDDING_DIM, embed_text

  vector = embed_text("Dock the robot at the station")

  assert len(vector) == EMBEDDING_DIM
  assert vector == embed_text("dock the ROBOT at the station")
  assert math.isclose(math.sqrt(sum(value * value for value in vector)), 1.0)
  assert embed_text("!!!") == [0.0] * EMBEDDING_DIM


def test_real_index_keeps_plan_and_document_namespaces_separate(tmp_path: Path) -> None:
  index = _vec_index(tmp_path)
  try:
    index.index_plan("shared-id", "inspect rooftop antenna array")
    index.index_plan("dock", "recharge battery at dock station")
    index.index_document("shared-id", "antenna maintenance manual")

    assert index.search_plans("rooftop antenna", 1) == ["shared-id"]
    assert index.search_documents("antenna manual", 1) == ["shared-id"]
    assert index.search_plans("anything", 0) == []
    assert index.search_documents("anything", -1) == []
  finally:
    index.close()


def test_reindex_replaces_vector_without_duplicate_ids(tmp_path: Path) -> None:
  index = _vec_index(tmp_path)
  try:
    index.index_plan("plan-1", "inspect rooftop antenna")
    index.index_plan("plan-1", "recharge battery at dock")
    index.index_plan("plan-2", "inspect rooftop antenna")

    assert index.search_plans("rooftop antenna", 2)[0] == "plan-2"
    assert index.search_plans("recharge battery dock", 2)[0] == "plan-1"
    assert index.search_plans("recharge battery dock", 2).count("plan-1") == 1
  finally:
    index.close()


def test_recall_service_merges_real_vector_hits_reference_first(
  tmp_path: Path,
) -> None:
  index = _vec_index(tmp_path)
  episodic = EpisodicStore(tmp_path / "episodic.db")
  reference = ReferenceStore(tmp_path / "reference.db")
  try:
    service = RecallService(episodic, reference, index)
    keyword = _plan("dock the robot now", StepOutcome.SUCCEEDED, plan_id="keyword")
    vector = _plan("inspect the rooftop antenna", StepOutcome.FAILED, plan_id="vector")
    promoted = reference.promote(
      _plan(
        "survey the rooftop antenna mast",
        StepOutcome.SUCCEEDED,
        plan_id="promoted",
      ),
      outcome="effective",
    )
    episodic.save_plan(keyword)
    episodic.save_plan(vector)
    service.index_plan(vector)
    service.index_plan(promoted)

    recalled = service.search_plans("dock the robot now", 3)

    assert recalled[0].id == promoted.id
    assert keyword.id in {record.id for record in recalled}
    assert len({record.id for record in recalled}) == len(recalled)
  finally:
    index.close()
    episodic.close()
    reference.close()


def test_real_document_index_returns_loaded_excerpt_only(tmp_path: Path) -> None:
  index = _vec_index(tmp_path)
  episodic = EpisodicStore(tmp_path / "episodic.db")
  reference = ReferenceStore(tmp_path / "reference.db")
  try:
    index.index_document("missing.md", "antenna safety")
    index.index_document("safety.md", "Estop after consecutive unrecovered faults.")
    service = RecallService(
      episodic,
      reference,
      index,
      {"safety.md": "Estop after consecutive unrecovered faults."},
    )

    hits = service.search_docs("unrecovered faults estop", 2)

    assert hits == [("safety.md", "Estop after consecutive unrecovered faults.")]
  finally:
    index.close()
    episodic.close()
    reference.close()


def test_missing_or_disabled_vector_runtime_degrades_to_none(
  tmp_path: Path,
  launch_config: LaunchConfig,
  monkeypatch: pytest.MonkeyPatch,
) -> None:
  from egia.main import build_recall_index

  disabled = launch_config.model_copy(
    update={"memory": launch_config.memory.model_copy(update={"vector_recall": False})}
  )
  assert build_recall_index(disabled, tmp_path) is None

  monkeypatch.setitem(sys.modules, "egia.opt.memory.vec", None)
  assert build_recall_index(launch_config, tmp_path) is None


def test_vector_database_is_created_below_cache_root(
  tmp_path: Path,
  launch_config: LaunchConfig,
  monkeypatch: pytest.MonkeyPatch,
) -> None:
  from egia.main import build_recall_index

  paths: list[Path] = []
  memory = types.ModuleType("egia.opt.memory")
  memory.SqliteVecIndex = lambda path: paths.append(path) or object()
  monkeypatch.setitem(sys.modules, "egia.opt.memory", memory)
  enabled = launch_config.model_copy(
    update={"launch": launch_config.launch.model_copy(update={"memory": True})}
  )

  assert build_recall_index(enabled, tmp_path) is not None
  assert paths == [tmp_path / "memory/vec.db"]


class ScriptedModel:
  reasoning_options: tuple[str, ...] = ("none",)
  reasoning_effort = "none"

  def __init__(self) -> None:
    self.requests: list[list[ChatMessage]] = []

  async def complete(self, messages: list[ChatMessage], **_kwargs) -> ModelResponse:
    self.requests.append(messages)
    return ModelResponse(text="final done")


@pytest.mark.asyncio
async def test_planner_recall_uses_vector_docs_and_indexes_final_plan(
  tmp_path: Path, etc_root: Path
) -> None:
  index = _vec_index(tmp_path)
  episodic = EpisodicStore(tmp_path / "episodic.db")
  reference = ReferenceStore(tmp_path / "reference.db")
  docs = {
    "egia/core/safety.md": (
      "The safety gate admits or rejects primitive egress; estop engages after "
      "consecutive unrecovered faults."
    )
  }
  try:
    service = RecallService(episodic, reference, index, docs)
    for doc_id, text in docs.items():
      index.index_document(doc_id, text)
    prompts = load_prompt_config(etc_root)
    model = ScriptedModel()
    planner = DecisionPlanner(
      model,
      ContextBuilder(Admission(8), prompts),
      PrimitiveRegistry.from_contracts(),
      object(),  # type: ignore[arg-type]  # final replies do not execute primitives
      Admission(8),
      episodic,
      None,
      reference,
      prompts.planner,
      DecisionConfig(max_steps=2, min_call_interval_s=0),
      recall=service,
    )
    mission = MissionRecord(
      id="mission-docs",
      order="check safety gate estop behavior",
      source="test",
      submitted_at=datetime.now(UTC),
    )

    assert await planner.run(mission, asyncio.Event()) == "done"

    user = model.requests[0][1].content
    assert "Recall and profile context:" in user
    assert "Relevant docs:" in user
    assert "egia/core/safety.md" in user
    assert "estop" in user.lower()
    assert index.search_plans("safety gate estop", 1) != []
  finally:
    index.close()
    episodic.close()
    reference.close()
