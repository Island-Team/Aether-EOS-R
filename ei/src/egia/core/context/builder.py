"""Build a bounded, dynamic model-context snapshot from admitted state."""

from __future__ import annotations

import json
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from typing import Protocol

from egia.api.context import (
  ContextGroup,
  ContextLine,
  ContextSection,
  ContextSnapshot,
  InnerPerception,
  MemoryContext,
  OuterPerception,
  PerceptionContext,
  UserContext,
)
from egia.api.observation import ObservationRecord
from egia.api.prompts import PromptDocument, PromptsSnapshot
from egia.core.context.mode import ModeState
from egia.core.context.perception_history import (
  HISTORY_LIMIT,
  PERCEPTION_KINDS,
  build_perception_section,
)
from egia.core.perception.admission import Admission
from egia.lib.config import PromptConfig, PromptSectionConfig
from egia.lib.logger import get_logger, log_once


class ContextMemory(Protocol):
  def archive_observation(self, record: ObservationRecord, *, reason: str) -> bool: ...

  def archive_context(self, kind: str, text: str, *, reason: str) -> bool: ...


PERCEPTION_KIND = "perception"

# Heuristic: ~4 characters per token. Renders English prose, JSON-ish
# telemetry, and template scaffolding onto one cheap, deterministic estimate
# with no tokenizer dependency; only rendered characters count (the newlines
# joining sections are ignored).
_CHARS_PER_TOKEN = 4


def _estimate_tokens(text: str) -> int:
  """Estimate the token count of ``text`` as ``len(text) // 4``."""
  return len(text) // _CHARS_PER_TOKEN


def _context_path(kind: str) -> str:
  """Map a legacy prompt kind to its stable structured user path."""
  return {
    "hardware": "user.perception.inner.parts",
    "mode": "user.perception.inner.robot_status",
    "status": "user.perception.inner.robot_status",
    "body": "user.perception.inner.parts",
    "missions": "user.memory.missions",
    "perception": "user.perception.outer",
    "objects": "user.perception.outer.objects",
    "environment": "user.perception.outer.environment",
    "skills": "user.skills",
    "primitives": "user.primitives",
    "backend": "user.status",
  }.get(kind, f"user.status.{kind}")


def _line_kind(section_kind: str, line: str) -> str:
  """Resolve a perception modality from a rendered history line."""
  if "[objects]" in line or section_kind == "objects":
    return "objects"
  if "[detection]" in line or "vision" in line.lower():
    return "vision"
  if "[hearing]" in line or "sound" in line.lower():
    return "sound"
  if section_kind == "environment":
    return "environment"
  if section_kind == "hardware":
    return "parts"
  if section_kind == "body":
    return "parts"
  if section_kind in {"mode", "status", "backend", "gate_report"}:
    return "robot_status"
  return section_kind


def _context_lines(section: _Section) -> list[ContextLine]:
  """Turn rendered content into atomic, UI-friendly lines."""
  if section.context_lines is not None:
    return section.context_lines
  return [
    ContextLine(text=line.strip(), kind=_line_kind(section.kind, line))
    for line in section.rendered.splitlines()
    if line.strip()
  ]


def _build_user_context(sections: list[ContextSection]) -> UserContext:
  """Build the nested user context projection from the prompt sections."""
  perception = PerceptionContext(
    inner=InnerPerception(parts=ContextGroup(), robot_status=ContextGroup()),
    outer=OuterPerception(),
  )
  memory = MemoryContext()
  status = ContextGroup()
  primitives = ContextGroup()
  skills = ContextGroup()

  for section in sections:
    destination = section.path or _context_path(section.kind)
    target = section.lines
    if destination.endswith("inner.parts"):
      perception.inner.parts.lines.extend(target)
    elif destination.endswith("inner.robot_status"):
      perception.inner.robot_status.lines.extend(target)
    elif destination.endswith("outer.vision"):
      perception.outer.vision.lines.extend(target)
    elif destination.endswith("outer.objects"):
      perception.outer.objects.lines.extend(target)
    elif destination.endswith("outer.environment"):
      perception.outer.environment.lines.extend(target)
    elif destination.endswith("outer.sound"):
      perception.outer.sound.lines.extend(target)
    elif destination.endswith("outer"):
      for line in target:
        if line.kind == "parts":
          perception.inner.parts.lines.append(line)
        elif line.kind == "robot_status":
          perception.inner.robot_status.lines.append(line)
        elif line.kind == "vision":
          perception.outer.vision.lines.append(line)
        elif line.kind == "objects":
          perception.outer.objects.lines.append(line)
        elif line.kind == "sound":
          perception.outer.sound.lines.append(line)
        else:
          perception.outer.environment.lines.append(line)
    elif destination.endswith("memory.missions"):
      memory.missions.lines.extend(target)
    elif destination.endswith("primitives"):
      primitives.lines.extend(target)
    elif destination.endswith("skills"):
      skills.lines.extend(target)
    else:
      status.lines.extend(target)

  return UserContext(
    perception=perception,
    memory=memory,
    status=status,
    primitives=primitives,
    skills=skills,
  )


@dataclass
class _Section:
  """Mutable render of one section, before and during the budget pass."""

  kind: str
  rendered: str
  has_data: bool
  image_path: str | None
  observation_id: str | None
  priority: int
  omitted_line: str
  omitted: bool = False
  observation_ids: list[str] = field(default_factory=list)
  context_lines: list[ContextLine] | None = None
  omitted_context_lines: list[ContextLine] | None = None
  change_frequency: float = 0.0


class ContextBuilder:
  """Builds immutable prompt snapshots from the latest observations per kind."""

  def __init__(
    self,
    queue: Admission,
    prompts: PromptConfig,
    *,
    backend_render: Callable[[], str] | None = None,
    mode_state: ModeState | None = None,
    dynamic_renderers: Mapping[str, Callable[[], str]] | None = None,
    local_memory: ContextMemory | None = None,
    system_prompt: PromptsSnapshot | None = None,
    change_frequency_path: Path | None = None,
  ) -> None:
    self._queue = queue
    self._prompts = prompts
    self._backend_render = backend_render
    self._mode_state = mode_state
    self._dynamic_renderers = dict(dynamic_renderers or {})
    self._local_memory = local_memory
    self._system_prompt = system_prompt
    self._change_frequency_path = change_frequency_path
    self._log = get_logger("egia.core.context")
    self._latest: ContextSnapshot | None = None
    self._logged_first_snapshot = False
    self._change_stats = self._load_change_stats()
    self._runtime_values: dict[str, str] = {}

  @property
  def latest(self) -> ContextSnapshot | None:
    return self._latest

  @property
  def token_budget(self) -> int:
    """Complete dynamic-message budget shared by all model lanes."""
    return self._prompts.token_budget

  def switch_mode(self, mode: str) -> ContextSnapshot:
    """Switch the model-visible mode and rebuild all context immediately."""
    if self._mode_state is None:
      raise RuntimeError("mode switching is not configured")
    if mode not in {"sandbox", "live"}:
      raise ValueError(f"unsupported backend mode: {mode!r}")
    self._mode_state.switch(mode)  # type: ignore[arg-type]
    return self.build()

  def observe_dynamic(self, values: Mapping[str, str | None]) -> dict[str, float]:
    """Observe lane-owned dynamic values in the shared lifetime order state."""
    frequencies = {
      kind: self._observe_change_frequency(kind, (value or "").strip())
      for kind, value in values.items()
    }
    self._save_change_stats()
    return frequencies

  def build(
    self, *, reserved_text: str = "", exclude_kinds: set[str] | None = None
  ) -> ContextSnapshot:
    """Render the current prompt snapshot from queue state."""
    preamble = ""
    sections: list[_Section] = []
    excluded = exclude_kinds or set()
    ordered = sorted(
      enumerate(self._prompts.context), key=lambda item: (item[1].priority, item[0])
    )
    for _, cfg in ordered:
      if not cfg.enabled or cfg.kind in excluded:
        continue
      if cfg.kind == "mode" and self._mode_state is not None:
        sections.append(
          _Section(
            cfg.kind,
            self._mode_state.render(),
            True,
            None,
            None,
            cfg.priority,
            cfg.omitted.format(kind=cfg.kind),
          )
        )
      elif cfg.kind == "backend" and self._backend_render is not None:
        sections.append(
          _Section(
            cfg.kind,
            cfg.format.format(data=self._backend_render()),
            True,
            None,
            None,
            cfg.priority,
            cfg.omitted.format(kind=cfg.kind),
          )
        )
      elif cfg.kind in self._dynamic_renderers:
        rendered = self._dynamic_renderers[cfg.kind]().strip()
        sections.append(
          _Section(
            kind=cfg.kind,
            rendered=cfg.format.format(data=rendered) if rendered else cfg.empty,
            has_data=bool(rendered),
            image_path=None,
            observation_id=None,
            priority=cfg.priority,
            omitted_line=cfg.omitted.format(kind=cfg.kind),
          )
        )
      elif cfg.kind == "hardware":
        current = {
          record.source: record
          for record in self._queue.records()
          if record.kind == "hardware"
        }
        records = [current[source] for source in sorted(current)]
        data = "\n".join(record.text for record in records)
        sections.append(
          _Section(
            kind=cfg.kind,
            rendered=cfg.format.format(data=data) if data else cfg.empty,
            has_data=bool(records),
            image_path=None,
            observation_id=None,
            observation_ids=[record.id for record in records],
            priority=cfg.priority,
            omitted_line=cfg.omitted.format(kind=cfg.kind),
          )
        )
      elif cfg.kind == PERCEPTION_KIND:
        records = self._queue.history(kinds=PERCEPTION_KINDS, limit=HISTORY_LIMIT)
        # Build current control evidence from the live admission, not from the
        # bounded history window. A quiet backend must not disappear merely
        # because another source produced more recent events.
        current_records = _current_perception(
          [
            record
            for record in self._queue.records()
            if record.kind in PERCEPTION_KINDS
          ]
        )
        if self._local_memory is not None:
          current_ids = {record.id for record in current_records}
          for record in records:
            # Semantic object snapshots are replaceable current evidence, not
            # episodic perception. Their superseded graphs must not become
            # model-recallable historical facts.
            if record.kind != "objects" and record.id not in current_ids:
              self._local_memory.archive_observation(record, reason="superseded")
        dropped = sum(self._queue.dropped(kind) for kind in PERCEPTION_KINDS)
        rendered, observation_ids, context_lines = build_perception_section(
          current_records, cfg, dropped=dropped
        )
        sections.append(
          _Section(
            kind=cfg.kind,
            rendered=rendered,
            has_data=bool(current_records),
            image_path=None,
            observation_id=None,
            observation_ids=observation_ids,
            context_lines=context_lines,
            priority=cfg.priority,
            omitted_line=cfg.omitted.format(kind=cfg.kind),
          )
        )
      else:
        sections.append(self._render(cfg))

    for section in sections:
      section.change_frequency = self._observe_change_frequency(
        section.kind, section.rendered
      )
    self._save_change_stats()
    self._enforce_budget(f"{reserved_text}\n{preamble}", sections)

    observation_ids: list[str] = []
    lines: list[str] = []
    if preamble:
      lines.append(preamble)
    wire_sections: list[ContextSection] = []
    for section in sections:
      observation_ids.extend(section.observation_ids)
      if section.observation_id is not None:
        observation_ids.append(section.observation_id)
      wire_sections.append(
        ContextSection(
          kind=section.kind,
          rendered=section.rendered,
          has_data=section.has_data,
          path=_context_path(section.kind),
          lines=_context_lines(section),
          image_path=section.image_path,
          omitted=section.omitted,
          priority=section.priority,
          change_frequency=section.change_frequency,
        )
      )
      lines.append(section.rendered)

    prompt = "\n".join(lines)
    snapshot = ContextSnapshot(
      version=self._prompts.version,
      prompt=prompt,
      system=(
        PromptDocument(
          role="system",
          text=self._system_prompt.system,
          sections=self._system_prompt.sections,
        )
        if self._system_prompt is not None
        else None
      ),
      user=_build_user_context(wire_sections),
      sections=wire_sections,
      built_at=datetime.now(UTC),
      observation_ids=observation_ids,
    )
    self._latest = snapshot

    has_any_data = any(section.has_data for section in wire_sections)
    if has_any_data and not self._logged_first_snapshot:
      self._logged_first_snapshot = True
      log_once(
        self._log,
        "info",
        "first_context_snapshot",
        version=snapshot.version,
        observation_count=len(snapshot.observation_ids),
        prompt_chars=len(snapshot.prompt),
      )
    else:
      self._log.debug(
        "context_snapshot_built",
        version=snapshot.version,
        observation_count=len(snapshot.observation_ids),
      )

    return snapshot

  def _observe_change_frequency(self, kind: str, rendered: str) -> float:
    """Return the persisted lifetime change rate for one dynamic section.

    The first sample in each process establishes a fresh baseline, so prompt
    edits made before startup do not count as runtime changes. Only counts are
    persisted; rendered prompt or observation text is never written here.
    """
    samples, changes = self._change_stats.get(kind, (0, 0))
    prior_text = self._runtime_values.get(kind)
    self._runtime_values[kind] = rendered
    if prior_text is None:
      return changes / samples if samples else 0.0
    samples += 1
    changes += int(prior_text != rendered)
    self._change_stats[kind] = (samples, changes)
    return changes / samples

  def _load_change_stats(self) -> dict[str, tuple[int, int]]:
    """Load lifetime ordering counters; malformed state degrades to defaults."""
    path = self._change_frequency_path
    if path is None or not path.exists():
      return {}
    try:
      document = json.loads(path.read_text(encoding="utf-8"))
      raw_sections = document.get("sections", {})
      if document.get("version") != 1 or not isinstance(raw_sections, dict):
        raise ValueError("unsupported context-order state")
      loaded: dict[str, tuple[int, int]] = {}
      for kind, raw in raw_sections.items():
        if not isinstance(kind, str) or not isinstance(raw, dict):
          continue
        samples = int(raw.get("samples", 0))
        changes = int(raw.get("changes", 0))
        if samples >= 0 and 0 <= changes <= samples:
          loaded[kind] = (samples, changes)
      return loaded
    except (OSError, ValueError, TypeError, json.JSONDecodeError) as exc:
      self._log.warning("context_order_state_ignored", path=str(path), reason=str(exc))
      return {}

  def _save_change_stats(self) -> None:
    """Atomically persist lifetime counters below the configured state root."""
    path = self._change_frequency_path
    if path is None:
      return
    document = {
      "version": 1,
      "sections": {
        kind: {"samples": samples, "changes": changes}
        for kind, (samples, changes) in sorted(self._change_stats.items())
      },
    }
    temporary = path.with_suffix(path.suffix + ".tmp")
    try:
      path.parent.mkdir(parents=True, exist_ok=True)
      temporary.write_text(
        json.dumps(document, ensure_ascii=False, sort_keys=True) + "\n",
        encoding="utf-8",
      )
      temporary.replace(path)
    except OSError as exc:
      temporary.unlink(missing_ok=True)
      self._log.warning(
        "context_order_state_write_failed", path=str(path), reason=str(exc)
      )

  def _render(self, cfg: PromptSectionConfig) -> _Section:
    """Render one section config against the latest observation of its kind."""
    record = self._queue.latest_by_kind(cfg.kind)
    image_path: str | None = None
    observation_id: str | None = None
    if record is None:
      rendered = cfg.empty
      has_data = False
      # Once per kind per process — the empty fallback renders in every
      # snapshot, so per-build logging would spam the sinks.
      log_once(self._log, "info", "no_data_received", key=cfg.kind, kind=cfg.kind)
    else:
      rendered = cfg.format.format(data=record.text)
      has_data = True
      observation_id = record.id
    return _Section(
      kind=cfg.kind,
      rendered=rendered,
      has_data=has_data,
      image_path=image_path,
      observation_id=observation_id,
      priority=cfg.priority,
      omitted_line=cfg.omitted.format(kind=cfg.kind),
    )

  def _enforce_budget(self, preamble: str, sections: list[_Section]) -> None:
    """Drop lowest-priority sections until the estimate fits ``token_budget``.

    Mutates over-budget sections in place: ``rendered`` becomes the configured
    ``omitted`` fallback line, and the section contributes ``has_data=False``,
    no observation id, and no image path. Sections without data are not drop
    candidates — their ``empty`` fallback is already minimal, and swapping it
    for the omitted marker would cost tokens rather than save them.

    Drop order is ascending priority with a stable tie-break: among equal
    priorities, the earliest-configured section drops first (Python's stable
    sort over the config-ordered list). The preamble is never dropped; if the
    snapshot still exceeds the budget after every droppable section is gone,
    it is kept and a warning is logged.
    """
    budget = self._prompts.token_budget
    before = _estimate_tokens(preamble) + sum(
      _estimate_tokens(section.rendered) for section in sections
    )
    if before <= budget:
      return

    dropped: list[str] = []
    total = before
    # Ascending priority; stable sort keeps config order within ties.
    candidates = sorted(
      (section for section in sections if section.has_data),
      key=lambda section: section.priority,
    )
    for section in candidates:
      if total <= budget:
        break
      if self._local_memory is not None:
        archived = section.rendered
        if section.kind == PERCEPTION_KIND and "[objects]" in archived:
          # Object graphs are replaceable state, never episodic evidence. Keep
          # other modalities from the mixed perception section while removing
          # every object-state line before durable budget archival.
          archived = "\n".join(
            line for line in archived.splitlines() if "[objects]" not in line
          )
        if archived.strip():
          self._local_memory.archive_context(
            section.kind, archived, reason="context_budget"
          )
      total += _estimate_tokens(section.omitted_line) - _estimate_tokens(
        section.rendered
      )
      section.rendered = section.omitted_line
      section.has_data = False
      section.image_path = None
      section.observation_id = None
      section.context_lines = section.omitted_context_lines
      section.observation_ids = (
        [
          line.observation_id
          for line in section.omitted_context_lines
          if line.observation_id is not None
        ]
        if section.omitted_context_lines is not None
        else []
      )
      section.omitted = True
      dropped.append(section.kind)

    if dropped:
      self._log.info(
        "context_budget_truncated",
        dropped_kinds=dropped,
        estimated_tokens=total,
        estimated_tokens_before=before,
        budget=budget,
      )
    if total > budget:
      self._log.warning(
        "context_budget_exceeded",
        budget=budget,
        estimated_tokens=total,
        dropped_kinds=dropped,
      )


def _current_perception(records: list[ObservationRecord]) -> list[ObservationRecord]:
  """Return the latest record per modality and backend source."""
  latest_by_source = {(record.kind, record.source): record for record in records}
  return sorted(latest_by_source.values(), key=lambda record: record.timestamp)
