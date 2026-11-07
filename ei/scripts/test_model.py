#!/usr/bin/env python3
"""Send one small completion to the decision model selected by egia.toml."""

from __future__ import annotations

import argparse
import asyncio
import importlib.util
import json
import os
import shutil
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[1]
if sys.version_info < (3, 12) or importlib.util.find_spec("pydantic") is None:
  uv = shutil.which("uv")
  if uv is None:
    raise SystemExit(
      "model test requires uv; install it or run inside the project venv"
    )
  os.execv(
    uv,
    [
      uv,
      "run",
      "--project",
      str(REPOSITORY),
      "python",
      str(Path(__file__).resolve()),
      *sys.argv[1:],
    ],
  )

for candidate in (REPOSITORY / "src", REPOSITORY):
  if str(candidate) not in sys.path:
    sys.path.insert(0, str(candidate))

from egia.api.plan import PlanStep
from egia.core.context import render_dynamic_message
from egia.core.decision.planner import _render_plan_block
from egia.lib.config import (
  ConfigError,
  load_mission_templates,
  load_model_config,
  load_model_selection,
  load_prompt_config,
)
from egia.lib.vlm import ChatMessage, ModelClientError, VLMClient


def _messages(
  etc: Path,
  prompt: str | None,
  planner_mission: str | None,
  mission_template: str | None,
) -> list[ChatMessage]:
  """Build either a connectivity probe or the real planner prompt shape."""
  if planner_mission is None and mission_template is None:
    return [
      ChatMessage(role="user", content=prompt or "Reply with exactly: EGIA model OK")
    ]
  prompts = load_prompt_config(etc)
  plan = None
  if mission_template is not None:
    template = load_mission_templates(etc).resolve(mission_template)
    if template is None:
      raise ConfigError(f"unknown Mission template: {mission_template}")
    planner_mission = template.mission
    plan = _render_plan_block(
      [
        PlanStep(
          id=step.id,
          sentence=step.outcome,
          required=step.required,
          after=list(step.after),
          terminal=step.terminal,
        )
        for step in template.steps
      ]
    )
  values = {
    "planner_recall": None,
    "planner_mission": (
      "MISSION role=active state=running order="
      + json.dumps(planner_mission, ensure_ascii=False)
      if planner_mission is not None
      else None
    ),
    "planner_plan": plan,
    "planner_results": None,
  }
  return [
    ChatMessage(role="system", content=prompts.planner.instruction),
    ChatMessage(
      role="user",
      content=render_dynamic_message(
        prompts.planner,
        values,
        token_budget=prompts.token_budget,
      ),
    ),
  ]


async def _test_model(
  etc: Path, messages: list[ChatMessage], max_tokens: int
) -> dict[str, str]:
  selected = load_model_selection(str(etc))
  if selected is None:
    raise ConfigError(f"no [model].name configured in {etc / 'egia.toml'}")
  binding = load_model_config(str(etc), selected)
  response = await VLMClient.from_config(binding).complete(
    messages,
    max_tokens=max_tokens,
  )
  if not response.text.strip():
    raise ModelClientError("configured model returned an empty completion")
  return {
    "binding": selected,
    "model": response.model or binding.model,
    "endpoint": binding.endpoint,
    "reply": response.text,
  }


def main() -> None:
  """Test the configured decision model and print its reply."""
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--etc", type=Path, default=REPOSITORY / "etc/egia")
  mode = parser.add_mutually_exclusive_group()
  mode.add_argument(
    "--prompt",
    help="small user prompt sent without planner instructions",
  )
  mode.add_argument(
    "--planner-mission",
    help="mission sent with the configured planner system and dynamic prompts",
  )
  mode.add_argument(
    "--mission-template",
    help="configured Mission template sent with its protected initial Plan",
  )
  parser.add_argument(
    "--max-tokens",
    type=int,
    help="completion bound (default: 32, or 4096 for --planner-mission)",
  )
  parser.add_argument("--json", action="store_true")
  args = parser.parse_args()
  planner_mode = args.planner_mission is not None or args.mission_template is not None
  max_tokens = (
    args.max_tokens if args.max_tokens is not None else (4096 if planner_mode else 32)
  )
  if max_tokens < 1:
    parser.error("--max-tokens must be positive")
  try:
    result = asyncio.run(
      _test_model(
        args.etc.resolve(),
        _messages(
          args.etc.resolve(),
          args.prompt,
          args.planner_mission,
          args.mission_template,
        ),
        max_tokens,
      )
    )
  except (ConfigError, ModelClientError, ValueError) as exc:
    parser.exit(1, f"model test failed: {exc}\n")
  if args.json:
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return
  print(f"binding: {result['binding']}")
  print(f"model: {result['model']}")
  print(f"endpoint: {result['endpoint']}")
  print(f"reply: {result['reply']}")


if __name__ == "__main__":
  main()
