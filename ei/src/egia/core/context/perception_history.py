"""Current-perception rendering — source-attributed sensory state.

Replaces per-kind snapshot sections with one block containing the latest live
record per modality and source, timestamp-sorted and kind-tagged. Primitive
results stay out because planner history already carries them.

Detection records render a bounded digest while ingest stays raw.
"""

from __future__ import annotations

from egia.api.context import ContextLine
from egia.api.observation import ObservationRecord
from egia.core.context.detection_digest import (
  build_detection_digest,
  build_detection_lines,
)
from egia.core.context.object_digest import build_object_digest, build_object_lines
from egia.lib.config import PromptSectionConfig

__all__ = ["PERCEPTION_KINDS", "build_perception_section"]

DETECTION_KIND = "detection"
OBJECTS_KIND = "objects"
BODY_KIND = "body"

# The sensory kinds that make up the perception history.
PERCEPTION_KINDS = frozenset({BODY_KIND, DETECTION_KIND, OBJECTS_KIND})

# Bound for recent diagnostic/archive lookup. Current rendering comes from the
# live admission and does not drop a quiet source merely because this window
# filled with another source's events.
HISTORY_LIMIT = 8


def _render_record(record: ObservationRecord) -> str:
  """One history line: raw text, except detection records render the digest."""
  if record.kind == DETECTION_KIND:
    return build_detection_digest(record) or "vision count=0"
  if record.kind == OBJECTS_KIND:
    return build_object_digest(record)
  return record.text


def _render_record_lines(record: ObservationRecord) -> list[str]:
  if record.kind == DETECTION_KIND:
    return build_detection_lines(record)
  if record.kind == OBJECTS_KIND:
    return build_object_lines(record)
  if record.kind == "hearing":
    return [f"hear: {record.text}"]
  if record.kind == BODY_KIND:
    return [record.text]
  return [record.text]


def build_perception_section(
  records: list[ObservationRecord],
  cfg: PromptSectionConfig,
  *,
  dropped: int = 0,
) -> tuple[str, list[str], list[ContextLine]]:
  """Render current source-attributed sensory records as one block.

  Returns rendered text, shown record ids, and structured lines carrying the
  same source/timestamp/id provenance as their admitted record.
  """
  if not records:
    return cfg.empty, [], []
  lines = [cfg.format.format(data=f"{len(records)} sources")]
  context_lines = [ContextLine(text=lines[0], kind="perception")]
  for record in records:
    stamp = record.timestamp.strftime("%H:%M:%S")
    kind = {
      BODY_KIND: "parts",
      DETECTION_KIND: "vision",
      OBJECTS_KIND: "objects",
      "hearing": "sound",
    }.get(record.kind, record.kind)
    for item in _render_record_lines(record):
      text = f"  {stamp} [{record.kind}] source={record.source} {item}"
      lines.append(text)
      context_lines.append(
        ContextLine(
          text=text.strip(),
          kind=kind,
          source=record.source,
          timestamp=record.timestamp,
          observation_id=record.id,
        )
      )
  if dropped:
    omitted = f"  observations dropped={dropped}"
    lines.append(omitted)
    context_lines.append(ContextLine(text=omitted.strip(), kind="perception"))
  return "\n".join(lines), [record.id for record in records], context_lines
