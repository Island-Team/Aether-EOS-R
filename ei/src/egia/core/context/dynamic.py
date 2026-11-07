"""Priority-ordered assembly of the final dynamic user message."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass

from egia.api.context import ContextSnapshot
from egia.lib.config import AgentPromptConfig, PromptSectionConfig

__all__ = ["render_dynamic_message", "render_lane_sections"]


# Stable bootstrap order used until lifetime runtime observations distinguish
# section volatility. It is intentionally code-owned: prompt wording or config
# reorderings at deployment time must not train the runtime frequency model.
_DEFAULT_DYNAMIC_ORDER = {
  kind: index
  for index, kind in enumerate(
    (
      "environment",
      "hardware",
      "skills",
      "primitives",
      "mode",
      "backend",
      "planner_recall",
      "reasoner_recall",
      "summarizer_previous",
      "planner_summary",
      "reasoner_summary",
      "proposer_templates",
      "profile_primitives",
      "profile_existing",
      "planner_mission",
      "reasoner_mission",
      "summarizer_mission",
      "proposer_missions",
      "sentinel_goal",
      "profile_mission",
      "planner_plan",
      "reasoner_plan",
      "summarizer_plan",
      "reasoner_trigger",
      "perception",
      "missions",
      "fault",
      "planner_results",
      "reasoner_results",
      "summarizer_history",
      "sentinel_state",
      "profile_feedback",
      "hearing",
    )
  )
}


@dataclass
class _DynamicPart:
  kind: str
  priority: int
  text: str
  omitted: str
  has_data: bool
  index: int
  change_frequency: float = 1.0


def _render(section: PromptSectionConfig, value: str | None) -> str:
  data = (value or "").strip()
  return section.format.format(data=data) if data else section.empty


def render_lane_sections(
  lane: AgentPromptConfig, values: Mapping[str, str | None]
) -> list[tuple[int, str]]:
  """Render configured lane sections without adding static instructions."""
  return [
    (section.priority, _render(section, values.get(section.kind)))
    for section in lane.dynamic_sections
    if section.enabled
  ]


def render_dynamic_message(
  lane: AgentPromptConfig,
  values: Mapping[str, str | None],
  *,
  context: ContextSnapshot | None = None,
  extra: Sequence[tuple[int, str]] = (),
  token_budget: int | None = None,
  change_frequencies: Mapping[str, float] | None = None,
) -> str:
  """Render one user message, with the most important state emitted last."""
  parts = [
    _DynamicPart(
      kind=section.kind,
      priority=section.priority,
      text=_render(section, values.get(section.kind)),
      omitted=section.omitted.format(kind=section.kind),
      has_data=bool((values.get(section.kind) or "").strip()),
      index=index,
      change_frequency=(change_frequencies or {}).get(section.kind, 1.0),
    )
    for index, section in enumerate(lane.dynamic_sections)
    if section.enabled
  ]
  if context is not None:
    offset = len(parts)
    parts.extend(
      _DynamicPart(
        kind=section.kind,
        priority=section.priority,
        text=section.rendered,
        omitted=f"[{section.kind} omitted: token budget]",
        has_data=section.has_data and not section.omitted,
        index=offset + index,
        change_frequency=section.change_frequency,
      )
      for index, section in enumerate(context.sections)
    )
  offset = len(parts)
  parts.extend(
    _DynamicPart(
      f"extra_{index}",
      priority,
      text.strip(),
      "[state omitted: token budget]",
      True,
      offset + index,
    )
    for index, (priority, text) in enumerate(extra)
    if text.strip()
  )
  # Stable dynamic state forms a reusable prefix and volatile state follows it.
  # Retention priority is independent and no section is pinned in position.
  ordered = sorted(
    parts,
    key=lambda item: (
      round(item.change_frequency, 2),
      _DEFAULT_DYNAMIC_ORDER.get(item.kind, len(_DEFAULT_DYNAMIC_ORDER)),
      item.index,
    ),
  )

  def message() -> str:
    return "\n\n".join(part.text for part in ordered if part.text)

  if token_budget is not None:
    for part in sorted(
      (item for item in ordered if item.has_data),
      key=lambda item: (item.priority, item.index),
    ):
      if len(message()) // 4 <= token_budget:
        break
      part.text = part.omitted
      part.has_data = False
    # Empty fallbacks and omission markers can themselves exceed the budget.
    # Remove least-important whole parts, then retain the tail of the final
    # highest-value part. This deterministic fallback always returns a bounded
    # message and never depends on another model call succeeding.
    for part in sorted(
      ordered,
      key=lambda item: (item.priority, item.index),
    ):
      if len(message()) // 4 <= token_budget:
        break
      part.text = ""
    rendered = message()
    max_chars = token_budget * 4
    if len(rendered) > max_chars:
      marker = "\n[dynamic context compressed]\n"
      keep = max(0, max_chars - len(marker))
      head = keep // 2
      tail = keep - head
      rendered = (
        rendered[:head] + marker + rendered[-tail:] if tail else marker[:max_chars]
      )
    return rendered
  return message()
