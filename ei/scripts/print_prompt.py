#!/usr/bin/env python3
"""Render every production prompt lane offline with deterministic mock state."""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[1]
for candidate in (REPOSITORY / "src", REPOSITORY / "libexec", REPOSITORY):
  if str(candidate) not in sys.path:
    sys.path.insert(0, str(candidate))

from egia.core.context import (
  ContextBuilder,
  ModeState,
  render_dynamic_message,
  render_hardware,
)
from egia.core.link import DeviceBinding
from egia.core.perception.admission import Admission
from egia.core.registry import PrimitiveRegistry
from egia.lib.config import AgentPromptConfig, load_launch_config


def _static(lane: AgentPromptConfig) -> dict[str, object]:
  return {
    "message": lane.instruction,
    "sections": [section.model_dump() for section in lane.sections],
  }


async def _preview(etc: Path, mode: str | None) -> dict[str, object]:
  launch = load_launch_config(str(etc), mode)
  registry = PrimitiveRegistry.from_contracts(["mock-robot"])
  devices = (
    DeviceBinding("camera-1", "camera", "vision", "", "idle", "ready", ()),
    DeviceBinding("base-0", "base", "drive", "", "idle", "ready", ()),
    DeviceBinding("camera-0", "camera", "vision", "", "idle", "ready", ()),
  )
  queue = Admission(launch.perception.capacity)
  await queue.enqueue(
    kind="hardware",
    source="mock",
    text=render_hardware("mock-robot", "mock/reference", devices),
  )
  context_builder = ContextBuilder(
    queue,
    launch.prompts,
    mode_state=ModeState("sandbox" if launch.mode == "sandbox" else "live"),
    backend_render=lambda: "Backend: unavailable",
    dynamic_renderers={
      "missions": lambda: "MISSION_TABLE empty",
      "primitives": registry.render_for_prompt,
      "skills": lambda: "unavailable",
    },
  )
  context = context_builder.build()
  proposer_context = context_builder.build(exclude_kinds={"missions"})
  lanes = {
    "planner": {
      "static": _static(launch.prompts.planner),
      "dynamic": render_dynamic_message(
        launch.prompts.planner,
        {
          "planner_recall": None,
          "planner_summary": None,
          "planner_mission": None,
          "planner_plan": None,
          "planner_results": None,
        },
        context=context,
        token_budget=launch.prompts.token_budget,
      ),
    },
    "reasoner": {
      "static": _static(launch.prompts.reasoner),
      "dynamic": render_dynamic_message(
        launch.prompts.reasoner,
        {
          "reasoner_recall": None,
          "reasoner_summary": None,
          "reasoner_mission": None,
          "reasoner_plan": None,
          "reasoner_trigger": None,
          "reasoner_results": None,
        },
        context=context,
        token_budget=launch.prompts.token_budget,
      ),
    },
    "summarizer": {
      "static": _static(launch.prompts.summarizer),
      "dynamic": render_dynamic_message(
        launch.prompts.summarizer,
        {
          "summarizer_previous": None,
          "summarizer_mission": None,
          "summarizer_plan": None,
          "summarizer_history": None,
        },
        token_budget=launch.prompts.token_budget,
      ),
    },
    "proposer": {
      "static": _static(launch.prompts.proposer),
      "dynamic": render_dynamic_message(
        launch.prompts.proposer,
        {
          "proposer_templates": "\n".join(
            (
              f"- {template.id}@{template.version}: {template.description}; "
              f"Mission={json.dumps(template.mission, ensure_ascii=False)}"
            )
            for template in launch.missions.templates
          )
          or "- none",
          "proposer_missions": "MISSION_TABLE empty",
        },
        context=proposer_context,
        token_budget=launch.prompts.token_budget,
      ),
    },
    "sentinel": {
      "static": _static(launch.prompts.sentinel),
      "dynamic": render_dynamic_message(
        launch.prompts.sentinel,
        {"sentinel_goal": None, "sentinel_state": None},
        token_budget=launch.prompts.token_budget,
      ),
    },
    "profile_writer": {
      "static": _static(launch.prompts.profile_writer),
      "dynamic": render_dynamic_message(
        launch.prompts.profile_writer,
        {
          "profile_primitives": registry.render_for_prompt(),
          "profile_existing": None,
          "profile_mission": None,
          "profile_feedback": None,
        },
        token_budget=launch.prompts.token_budget,
      ),
    },
  }
  return {
    "version": launch.prompts.version,
    "token_budget": launch.prompts.token_budget,
    "context_sections": [section.model_dump() for section in launch.prompts.context],
    "lanes": lanes,
  }


def main() -> None:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--etc", type=Path, default=REPOSITORY / "etc/egia")
  parser.add_argument("--mode")
  parser.add_argument("--json", action="store_true")
  args = parser.parse_args()
  with redirect_stdout(StringIO()):
    preview = asyncio.run(_preview(args.etc.resolve(), args.mode))
  if args.json:
    print(json.dumps(preview, indent=2, ensure_ascii=False))
    return
  for name, lane in preview["lanes"].items():
    print(f"=== {name}: static ===")
    print(lane["static"]["message"])
    print(f"\n=== {name}: final dynamic preview ===")
    print(lane["dynamic"])
    print()


if __name__ == "__main__":
  main()
