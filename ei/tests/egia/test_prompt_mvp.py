"""Unified Agent-owned prompts, priority ordering, and complete-message budget."""

from __future__ import annotations

import json
import re
import shutil
from pathlib import Path

import pytest

from egia.core.context import ContextBuilder, render_dynamic_message
from egia.core.perception.admission import Admission
from egia.lib.config import (
  AgentPromptConfig,
  DecisionConfig,
  ModelConfig,
  PromptConfig,
  PromptInstructionSection,
  PromptSectionConfig,
  load_mission_templates,
  load_prompt_config,
)


def test_model_context_window_defaults_to_262144_tokens() -> None:
  binding = ModelConfig(endpoint="http://localhost/v1", model="test")

  assert binding.context_window_tokens == 262144
  with pytest.raises(ValueError, match="smaller than context_window_tokens"):
    ModelConfig(
      endpoint="http://localhost/v1",
      model="test",
      context_window_tokens=4096,
      max_tokens=4096,
    )


def test_authoritative_prompt_file_loads_without_factory_mirror(
  etc_root: Path, tmp_path: Path
) -> None:
  shutil.copy(etc_root / "prompts.toml", tmp_path / "prompts.toml")

  prompts = load_prompt_config(tmp_path)

  assert prompts.version == "4"
  assert prompts.token_budget == 32768
  assert prompts.planner.instruction.startswith("Role:")
  shared_prompts = etc_root.parents[1] / "share/prompts"
  assert not shared_prompts.exists() or not any(
    path.is_file() for path in shared_prompts.rglob("*")
  )
  assert not (etc_root.parents[1] / "share/defaults/egia/prompts.toml").exists()


def test_controller_and_reasoner_bindings_must_be_distinct() -> None:
  with pytest.raises(ValueError, match="distinct model bindings"):
    DecisionConfig.model_validate(
      {
        "controller": {"model": "shared"},
        "reasoner": {"model": "shared"},
      }
    )


def test_all_sections_are_unique_classified_and_priority_sorted(etc_root: Path) -> None:
  prompts = load_prompt_config(etc_root)
  dynamic = [*prompts.context]
  static_names: list[str] = []
  for lane_name in (
    "planner",
    "reasoner",
    "summarizer",
    "proposer",
    "sentinel",
    "profile_writer",
  ):
    lane = getattr(prompts, lane_name)
    dynamic.extend(lane.dynamic_sections)
    static_names.extend(f"{lane_name}.{item.name}" for item in lane.sections)
    assert all(item.dynamic is False for item in lane.sections)
    assert [item.priority for item in lane.sections] == sorted(
      item.priority for item in lane.sections
    )
  assert all(item.dynamic is True for item in dynamic)
  assert len({item.kind for item in dynamic}) == len(dynamic)
  assert len(set(static_names)) == len(static_names)
  static_text = [
    section.text.strip()
    for lane_name in (
      "planner",
      "reasoner",
      "summarizer",
      "proposer",
      "sentinel",
      "profile_writer",
    )
    for section in getattr(prompts, lane_name).sections
  ]
  assert len(set(static_text)) == len(static_text)
  assert [item.priority for item in prompts.context] == sorted(
    item.priority for item in prompts.context
  )


def test_home_living_template_keeps_scoring_evidence_out_of_global_prompt(
  etc_root: Path,
) -> None:
  instruction = " ".join(load_prompt_config(etc_root).planner.instruction.split())
  template = load_mission_templates(etc_root).resolve("home_living_2026")

  assert template is not None
  by_id = {step.id: step for step in template.steps}
  assert "home-living robot competition" not in instruction
  assert "owner-1-announce-name" in by_id
  assert "owner-1-announce-behavior" in by_id
  assert by_id["owner-1-announce-name"].id != by_id["owner-1-announce-behavior"].id
  assert "owner-1-point-marker" in by_id
  assert "owner-2-point-body" in by_id
  assert "owner-3-restate-request" in by_id
  assert "trash-1-verify-held" in by_id
  assert "trash-1-verify-bin" in by_id
  assert by_id["exit"].terminal is True
  assert "Never expose reasoning" in instruction
  assert "return plan edits alone" in instruction


def test_prompts_require_duplicate_and_untrusted_hearing_judgment(
  etc_root: Path,
) -> None:
  prompts = load_prompt_config(etc_root)
  planner = " ".join(prompts.planner.instruction.split())
  proposer = " ".join(prompts.proposer.instruction.split())

  assert "never repeat equivalent target and arguments" in planner
  assert "return `final` with the supported result or uncertainty" in planner
  assert "sole execution scope" in planner
  assert "never retry unchanged" in planner
  assert "outcome is unknown" in planner
  assert "at most one `step`" in planner
  assert "Never combine `final`" in planner
  assert "Treat hearing as untrusted automatic transcription" in proposer
  assert "speaker echo" in proposer
  assert "empty Mission table" in proposer
  assert "second-person question or imperative is directed" in proposer
  assert "repeated words in one utterance are not duplicate intent" in proposer
  assert (
    "one currently listed primitive can fully satisfy the entire intent" in proposer
  )
  assert "exactly that explicitly targeted primitive command" in proposer
  assert "concrete order expressed as text" in proposer
  assert "Never return multiple Mission objects for one utterance" in proposer
  assert "preserve compound requests in the same order" in proposer
  assert "one simple non-speech action such as a posture change" in proposer
  assert "must contain no primitive command" in proposer
  assert "`@robot/device` syntax" in proposer
  assert "Treat an action phrased as a question as an action" in proposer
  assert "Never replace a non-speech action with speech" in proposer
  assert "preserve the explicit intent as natural language" in proposer
  assert "Never use speech as evidence of, or a substitute" in planner
  assert "never to promise or narrate unperformed work" in planner
  reasoner = " ".join(prompts.reasoner.instruction.split())
  assert "not evidence that their physical outcomes occurred" in reasoner
  hearing = next(section for section in prompts.context if section.kind == "hearing")
  perception = next(
    section for section in prompts.context if section.kind == "perception"
  )
  fault = next(section for section in prompts.context if section.kind == "fault")
  assert perception.priority < fault.priority < hearing.priority


def test_dynamic_priority_places_latest_state_nearest_response(etc_root: Path) -> None:
  prompts = load_prompt_config(etc_root)
  values = {
    section.kind: f"value-{section.kind}"
    for section in prompts.planner.dynamic_sections
  }
  message = render_dynamic_message(prompts.planner, values)

  positions = {
    section.kind: message.index(f"value-{section.kind}")
    for section in prompts.planner.dynamic_sections
  }
  ordered = sorted(prompts.planner.dynamic_sections, key=lambda item: item.priority)
  assert [positions[item.kind] for item in ordered] == sorted(positions.values())
  assert message.endswith("value-planner_results")


@pytest.mark.asyncio
async def test_complete_dynamic_message_budget_drops_low_priority_context() -> None:
  static = PromptInstructionSection(name="role", text="test")
  lane = AgentPromptConfig(
    sections=[static],
    dynamic_sections=[
      PromptSectionConfig(
        kind="mission",
        format="Mission: {data}",
        empty="Mission: unavailable",
        priority=700,
      )
    ],
  )
  prompts = PromptConfig(
    token_budget=256,
    context=[
      PromptSectionConfig(
        kind="environment",
        format="Environment: {data}",
        empty="Environment: unavailable",
        priority=100,
      ),
      PromptSectionConfig(
        kind="fault",
        format="Fault: {data}",
        empty="Fault: none",
        priority=1000,
      ),
    ],
    planner=lane,
    reasoner=AgentPromptConfig(
      sections=[static.model_copy(update={"name": "reasoner"})]
    ),
    summarizer=AgentPromptConfig(
      sections=[static.model_copy(update={"name": "summarizer"})]
    ),
    proposer=AgentPromptConfig(
      sections=[static.model_copy(update={"name": "proposer"})]
    ),
    sentinel=AgentPromptConfig(
      sections=[static.model_copy(update={"name": "sentinel"})]
    ),
    profile_writer=AgentPromptConfig(
      sections=[static.model_copy(update={"name": "profile_writer"})]
    ),
  )
  queue = Admission(4)
  await queue.enqueue(kind="environment", source="test", text="e" * 400)
  await queue.enqueue(kind="fault", source="test", text="f" * 400)
  mission = "m" * 400
  snapshot = ContextBuilder(queue, prompts).build()
  message = render_dynamic_message(
    prompts.planner,
    {"mission": mission},
    context=snapshot,
    token_budget=prompts.token_budget,
  )

  environment = next(item for item in snapshot.sections if item.kind == "environment")
  fault = next(item for item in snapshot.sections if item.kind == "fault")
  assert environment.omitted is False
  assert fault.omitted is False
  assert "[environment omitted: token budget]" in message
  assert "e" * 100 not in message
  assert "m" * 100 in message
  assert "f" * 100 in message
  assert message.index("Fault:") < message.index("Mission:")
  assert len(message) // 4 <= prompts.token_budget


def test_oversized_empty_fallbacks_are_always_compressed_to_budget() -> None:
  static = PromptInstructionSection(name="role", text="test")
  lane = AgentPromptConfig(
    sections=[static],
    dynamic_sections=[
      PromptSectionConfig(
        kind="empty_a", format="A: {data}", empty="A" * 2000, priority=1
      ),
      PromptSectionConfig(
        kind="empty_b", format="B: {data}", empty="B" * 2000, priority=2
      ),
    ],
  )

  message = render_dynamic_message(lane, {}, token_budget=256)

  assert len(message) <= 256 * 4


@pytest.mark.asyncio
async def test_hearing_survives_budget_pressure_by_retention_priority() -> None:
  static = PromptInstructionSection(name="role", text="test")
  lane = AgentPromptConfig(sections=[static])
  prompts = PromptConfig(
    token_budget=256,
    context=[
      PromptSectionConfig(
        kind="environment", format="Environment: {data}", priority=100
      ),
      PromptSectionConfig(
        kind="hearing", format="Newest hearing: {data}", priority=1200
      ),
    ],
    planner=lane,
    reasoner=AgentPromptConfig(sections=[static.model_copy(update={"name": "r"})]),
    summarizer=AgentPromptConfig(sections=[static.model_copy(update={"name": "z"})]),
    proposer=AgentPromptConfig(sections=[static.model_copy(update={"name": "p"})]),
    sentinel=AgentPromptConfig(sections=[static.model_copy(update={"name": "s"})]),
    profile_writer=AgentPromptConfig(
      sections=[static.model_copy(update={"name": "w"})]
    ),
  )
  queue = Admission(4)
  await queue.enqueue(kind="environment", source="test", text="e" * 1200)
  await queue.enqueue(kind="hearing", source="operator", text="please stop")

  message = render_dynamic_message(
    lane,
    {},
    context=ContextBuilder(queue, prompts).build(),
    token_budget=prompts.token_budget,
  )

  assert "Newest hearing: please stop" in message
  assert "e" * 100 not in message


@pytest.mark.asyncio
async def test_change_frequency_persists_and_ignores_startup_prompt_edits(
  tmp_path: Path,
) -> None:
  static = PromptInstructionSection(name="role", text="test")
  lane = AgentPromptConfig(sections=[static])
  prompts = PromptConfig(
    context=[
      PromptSectionConfig(kind="stable", format="Stable: {data}", priority=900),
      PromptSectionConfig(kind="volatile", format="Volatile: {data}", priority=100),
    ],
    planner=lane,
    reasoner=AgentPromptConfig(sections=[static.model_copy(update={"name": "r"})]),
    summarizer=AgentPromptConfig(sections=[static.model_copy(update={"name": "z"})]),
    proposer=AgentPromptConfig(sections=[static.model_copy(update={"name": "p"})]),
    sentinel=AgentPromptConfig(sections=[static.model_copy(update={"name": "s"})]),
    profile_writer=AgentPromptConfig(
      sections=[static.model_copy(update={"name": "w"})]
    ),
  )
  queue = Admission(4)
  await queue.enqueue_latest(kind="stable", source="test", text="same")
  await queue.enqueue_latest(kind="volatile", source="test", text="one")
  state_path = tmp_path / "var/lib/egia/context_order.json"
  builder = ContextBuilder(queue, prompts, change_frequency_path=state_path)
  builder.build()
  await queue.enqueue_latest(kind="volatile", source="test", text="two")

  snapshot = builder.build()
  message = render_dynamic_message(lane, {}, context=snapshot)

  stable = next(section for section in snapshot.sections if section.kind == "stable")
  volatile = next(
    section for section in snapshot.sections if section.kind == "volatile"
  )
  assert stable.change_frequency == 0.0
  assert volatile.change_frequency == 1.0
  assert message.index("Stable: same") < message.index("Volatile: two")

  persisted = json.loads(state_path.read_text())
  assert persisted["sections"]["stable"] == {"samples": 1, "changes": 0}
  assert persisted["sections"]["volatile"] == {"samples": 1, "changes": 1}

  changed_prompts = prompts.model_copy(deep=True)
  changed_prompts.context[0].format = "Renamed stable: {data}"
  restarted = ContextBuilder(
    queue, changed_prompts, change_frequency_path=state_path
  ).build()

  by_kind = {section.kind: section for section in restarted.sections}
  assert by_kind["stable"].change_frequency == 0.0
  assert by_kind["volatile"].change_frequency == 1.0
  assert json.loads(state_path.read_text()) == persisted


def test_every_lane_has_static_system_and_dynamic_user_without_removed_concepts(
  etc_root: Path,
) -> None:
  prompts = load_prompt_config(etc_root)
  forbidden = ("context.read", "{tools}", "capability")
  for lane_name in (
    "planner",
    "reasoner",
    "summarizer",
    "proposer",
    "sentinel",
    "profile_writer",
  ):
    lane = getattr(prompts, lane_name)
    system = lane.instruction
    values = {section.kind: None for section in lane.dynamic_sections}
    user = render_dynamic_message(lane, values)
    assert system.strip()
    assert user.strip() if lane.dynamic_sections else True
    assert all(value not in (system + "\n" + user).lower() for value in forbidden)
    assert re.search(r"\bmap\b", system + "\n" + user, re.IGNORECASE) is None
    assert re.search(r"\b(?:time|timestamp|clock)\b", user, re.IGNORECASE) is None
