"""Recall merging and planner prompt integration for the prompt-optimized MVP."""

from __future__ import annotations

from datetime import UTC, datetime
from pathlib import Path

from egia.api.mission import MissionRecord
from egia.api.plan import ActionRecord, PlanRecord, PlanStep, StepOutcome, StepRecord
from egia.core.context import ContextBuilder
from egia.core.decision import DecisionPlanner
from egia.core.memory import EpisodicStore, RecallService, ReferenceStore
from egia.core.perception.admission import Admission
from egia.core.registry import PrimitiveRegistry
from egia.lib.config import DecisionConfig, load_prompt_config


class UnusedModel:
  reasoning_options: tuple[str, ...] = ()
  reasoning_effort = None


class UnusedGate:
  pass


class FakeRecallIndex:
  def __init__(
    self,
    *,
    plans: list[str] | None = None,
    documents: list[str] | None = None,
  ) -> None:
    self._plans = list(plans or [])
    self._documents = list(documents or [])
    self.indexed: list[tuple[str, str]] = []

  def search_plans(self, _query: str, limit: int) -> list[str]:
    return self._plans[:limit]

  def search_documents(self, _query: str, limit: int) -> list[str]:
    return self._documents[:limit]

  def index_plan(self, plan_id: str, text: str) -> None:
    self.indexed.append((plan_id, text))


def _plan(
  order: str,
  outcome: StepOutcome,
  *,
  plan_id: str,
  action: str = "base.velocity",
) -> PlanRecord:
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
        actions=[ActionRecord(index=1, action=action, outcome=outcome)],
      )
    ],
  )


def _stores(tmp_path: Path) -> tuple[EpisodicStore, ReferenceStore]:
  return (
    EpisodicStore(tmp_path / "episodic.db"),
    ReferenceStore(tmp_path / "reference.db"),
  )


def _planner(
  etc_root: Path,
  queue: Admission,
  recall: RecallService,
  *,
  recall_limit: int = 3,
) -> DecisionPlanner:
  prompts = load_prompt_config(etc_root)
  return DecisionPlanner(
    UnusedModel(),  # type: ignore[arg-type]
    ContextBuilder(queue, prompts),
    PrimitiveRegistry.from_contracts(["robot-a"]),
    UnusedGate(),  # type: ignore[arg-type]
    queue,
    None,
    None,
    None,
    prompts.planner,
    DecisionConfig(recall_limit=recall_limit),
    recall=recall,
  )


def test_recall_merges_reference_first_and_deduplicates_vector_hits(
  tmp_path: Path,
) -> None:
  episodic, reference = _stores(tmp_path)
  try:
    shared = _plan("dock the robot", StepOutcome.SUCCEEDED, plan_id="shared")
    episodic.save_plan(shared)
    reference.promote(shared, outcome="effective", note="stable in low light")
    episodic_only = _plan(
      "dock the robot at night", StepOutcome.FAILED, plan_id="episodic"
    )
    episodic.save_plan(episodic_only)
    index = FakeRecallIndex(plans=["shared", "episodic"])
    recall = RecallService(episodic, reference, index=index)  # type: ignore[arg-type]

    found = recall.search_plans("dock the robot", 5)

    assert [plan.id for plan in found] == ["shared", "episodic"]
    assert found[0].outcome is StepOutcome.EFFECTIVE
    assert found[0].review_note == "stable in low light"
  finally:
    episodic.close()
    reference.close()


def test_recall_degrades_without_vector_index_and_skips_unknown_docs(
  tmp_path: Path,
) -> None:
  episodic, reference = _stores(tmp_path)
  try:
    keyword = _plan("dock the robot", StepOutcome.SUCCEEDED, plan_id="keyword")
    episodic.save_plan(keyword)
    degraded = RecallService(
      episodic, reference, docs={"guide.md": "Dock using the alignment marks."}
    )
    assert [plan.id for plan in degraded.search_plans("dock the robot", 2)] == [
      "keyword"
    ]
    assert degraded.search_docs("alignment", 2) == []

    index = FakeRecallIndex(documents=["missing.md", "guide.md"])
    enabled = RecallService(
      episodic,
      reference,
      index=index,  # type: ignore[arg-type]
      docs={"guide.md": "Dock using the alignment marks and verify contact."},
    )
    assert enabled.search_docs("alignment", 2) == [
      ("guide.md", "Dock using the alignment marks and verify contact.")
    ]
  finally:
    episodic.close()
    reference.close()


def test_recalled_state_is_only_in_final_dynamic_user_message(
  tmp_path: Path, etc_root: Path
) -> None:
  episodic, reference = _stores(tmp_path)
  try:
    failed = _plan("dock the robot at night", StepOutcome.FAILED, plan_id="failed")
    episodic.save_plan(failed)
    reviewed = reference.promote(
      _plan(
        "dock the robot at the station",
        StepOutcome.SUCCEEDED,
        plan_id="reviewed",
      ),
      outcome="effective",
      note="worked in low light",
    )
    queue = Admission(capacity=16)
    planner = _planner(etc_root, queue, RecallService(episodic, reference))
    digest = planner._recall_prior_plans("dock the robot")
    assert digest is not None
    mission = MissionRecord(
      id="m-current",
      order="dock the robot",
      source="test",
      submitted_at=datetime.now(UTC),
    )

    messages = planner._build_messages(
      mission,
      ["step 1: base.velocity=succeeded"],
      digest,
      [PlanStep(id="approach", sentence="Approach the dock")],
    )

    assert [message.role for message in messages] == ["system", "user"]
    system = messages[0].content
    user = messages[1].content
    assert isinstance(system, str)
    assert isinstance(user, str)
    assert system == load_prompt_config(etc_root).planner.instruction
    assert "dock the robot at the station" not in system
    assert "worked in low light" not in system
    assert user.count("dock the robot at the station") == 1
    assert user.index("dock the robot at the station") < user.index(
      "dock the robot at night"
    )
    assert "-> effective" in user
    assert "-> failed" in user
    assert "review note: worked in low light" in user
    assert reviewed.reviewed_at is not None
    assert user.index("Recall and profile context:") < user.index(
      'MISSION role=active id=m-curren state=pending order="dock the robot"'
    )
    assert "Execution history (oldest to newest):" in user
    assert "EVENT 1: step 1: base.velocity=succeeded" in user
    assert user.endswith("Newest hearing: none")
  finally:
    episodic.close()
    reference.close()


def test_recall_unavailable_and_closed_store_are_nonfatal(
  tmp_path: Path, etc_root: Path
) -> None:
  episodic, reference = _stores(tmp_path)
  queue = Admission(capacity=4)
  planner = _planner(etc_root, queue, RecallService(episodic, reference))
  mission = MissionRecord(
    id="m-empty",
    order="inspect the panel",
    source="test",
    submitted_at=datetime.now(UTC),
  )

  assert planner._recall_prior_plans(mission.order) is None
  messages = planner._build_messages(mission, [], None, [])
  user = messages[1].content
  assert isinstance(user, str)
  assert "Recall and profile context: unavailable" in user
  assert "Execution history: none" in user
  assert user.endswith("Newest hearing: none")

  episodic.close()
  reference.close()
  assert planner._recall_prior_plans(mission.order) is None


def test_recall_limit_zero_avoids_store_access(tmp_path: Path, etc_root: Path) -> None:
  episodic, reference = _stores(tmp_path)
  recall = RecallService(episodic, reference)
  planner = _planner(etc_root, Admission(capacity=4), recall, recall_limit=0)
  episodic.close()
  reference.close()

  assert planner._recall_prior_plans("anything") is None


def test_index_plan_summarizes_concurrent_primitives(tmp_path: Path) -> None:
  episodic, reference = _stores(tmp_path)
  try:
    index = FakeRecallIndex()
    recall = RecallService(episodic, reference, index=index)  # type: ignore[arg-type]
    record = _plan("pick the cube", StepOutcome.SUCCEEDED, plan_id="indexed")
    record.steps[0].actions.append(
      ActionRecord(index=2, action="gripper.motion", outcome=StepOutcome.SUCCEEDED)
    )
    record.steps.append(StepRecord(index=2, outcome=StepOutcome.SUCCEEDED))

    recall.index_plan(record)

    assert index.indexed == [
      (
        "indexed",
        (
          "pick the cube\n"
          "base.velocity; gripper.motion -> succeeded\n"
          "final -> succeeded"
        ),
      )
    ]
  finally:
    episodic.close()
    reference.close()
