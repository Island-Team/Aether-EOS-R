"""Bounded inline detection summaries for model context."""

from __future__ import annotations

import json

from egia.api.observation import ObservationRecord

__all__ = ["build_detection_digest", "build_detection_lines", "detection_renderings"]

# Normal sandbox frames include arms, cameras, and a handful of materials.
# Above this size, the inline summary reports an explicit omitted count.
_MAX_INLINE_DETECTIONS = 32
_MAX_INLINE_CHARS = 3072


def _bounded_renderings(renderings: list[str]) -> list[str]:
  kept: list[str] = []
  chars = 0
  for rendering in renderings:
    separator = 2 if kept else 0
    if (
      len(kept) >= _MAX_INLINE_DETECTIONS
      or chars + separator + len(rendering) > _MAX_INLINE_CHARS
    ):
      break
    kept.append(rendering)
    chars += separator + len(rendering)
  return kept


def detection_renderings(record: ObservationRecord | None) -> list[str]:
  """Return each atomic rendering, accepting the legacy delimiter framing."""
  if record is None:
    return []
  try:
    payload = json.loads(record.text)
  except (json.JSONDecodeError, TypeError):
    # Records admitted before the JSON-array framing used ``;`` as a
    # delimiter. Retain bounded-history and upgrade compatibility for them.
    return [part.strip() for part in record.text.split(";") if part.strip()]
  if not isinstance(payload, list) or not all(
    isinstance(item, str) for item in payload
  ):
    raise ValueError("vision payload must be a JSON string array")
  return payload


def _invalid_payload(record: ObservationRecord) -> str:
  return (
    f"vision unavailable reason=invalid_payload source={record.source} "
    f"snapshot_id={record.id}"
  )


def build_detection_lines(record: ObservationRecord) -> list[str]:
  """Render a detection frame as atomic, one-information-per-line facts."""
  try:
    renderings = detection_renderings(record)
  except ValueError:
    return [_invalid_payload(record)]
  total = _detection_count(record) or len(renderings)
  if not renderings:
    return ["vision count=0"]
  kept = _bounded_renderings(renderings)
  lines = [f"vision count={total}"]
  lines.extend(kept)
  omitted = max(total, len(renderings)) - len(kept)
  if omitted:
    lines.append(f"vision omitted={omitted}")
  held = record.metadata.get("held_object_id", "")
  if held:
    lines.append(f"holding {held}")
  return lines


def build_detection_digest(record: ObservationRecord) -> str | None:
  """Render one detection record as the mission-addressable digest line.

  Returns ``None`` when ``record`` carries no renderings (or is empty), so a
  detection-free frame does not render a bare header.
  """
  try:
    renderings = detection_renderings(record)
  except ValueError:
    return _invalid_payload(record)
  if not renderings:
    return None
  kept = _bounded_renderings(renderings)
  total = _detection_count(record) or len(renderings)
  parts: list[str] = []
  if kept:
    parts.append("; ".join(kept))
  omitted = max(total, len(renderings)) - len(kept)
  if omitted:
    parts.append(f"omitted={omitted}")
  held = record.metadata.get("held_object_id", "")
  if held:
    parts.append(f"holding {held}")
  return f"vision count={total}: {'; '.join(parts)}"


def _detection_count(record: ObservationRecord) -> int | None:
  """Read the ingest-side count metadata, when it parses."""
  raw = record.metadata.get("detection_count")
  try:
    return int(raw) if raw is not None else None
  except ValueError:
    return None
