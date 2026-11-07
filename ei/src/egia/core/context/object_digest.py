"""Compact bounded semantic-object context."""

from __future__ import annotations

import json
from typing import Any

from egia.api.observation import ObservationRecord

__all__ = [
  "build_object_digest",
  "build_object_lines",
  "object_payload",
]

_MAX_INLINE_OBJECTS = 12
_MAX_INLINE_CHARS = 3072
_MAX_INLINE_STOCK = 12
_MAX_INLINE_STOCK_CHARS = 1024


def object_payload(
  record: ObservationRecord | None,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
  """Decode the typed object and stock arrays stored in one admission."""
  if record is None:
    return [], []
  try:
    payload = json.loads(record.text)
  except (json.JSONDecodeError, TypeError) as exc:
    raise ValueError("invalid objects JSON") from exc
  if not isinstance(payload, dict):
    raise TypeError("objects payload must be an object")
  objects = payload.get("objects", [])
  stock = payload.get("stock", [])
  if not isinstance(objects, list) or not all(
    isinstance(item, dict) for item in objects
  ):
    raise ValueError("objects must be an array of objects")
  if not isinstance(stock, list) or not all(isinstance(item, dict) for item in stock):
    raise ValueError("stock must be an array of objects")
  return objects, stock


def _compact(value: Any) -> str:
  return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def _object_summary(item: dict[str, Any]) -> str:
  """Render one action-addressable object without repeating empty fields."""
  object_id = str(item.get("object_id") or item.get("id") or "<unknown>")
  kind = str(item.get("kind") or item.get("label") or "object")
  parts = [object_id, kind]
  location = item.get("location")
  if isinstance(location, dict):
    place = location.get("place_id")
    slot = location.get("slot_id")
    if place is not None:
      parts.append(f"at={place}" + (f"/{slot}" if slot is not None else ""))
  states = item.get("states")
  if isinstance(states, list) and states:
    parts.append("states=" + ",".join(str(value) for value in states))
  relations = item.get("relations")
  if isinstance(relations, list) and relations:
    rendered_relations = []
    for relation in relations:
      if not isinstance(relation, dict):
        continue
      relation_kind = relation.get("kind", "related")
      target = relation.get("target_id", "?")
      slot = relation.get("slot_id")
      rendered_relations.append(
        f"{relation_kind}:{target}" + (f"@{slot}" if slot is not None else "")
      )
    if rendered_relations:
      parts.append("relations=" + ",".join(rendered_relations))
  slots = item.get("slots")
  if isinstance(slots, list) and slots:
    parts.append(f"slots={len(slots)}")
  properties = item.get("properties")
  if isinstance(properties, dict) and properties:
    parts.append(f"properties={_compact(properties)}")
  return " ".join(parts)


def _stock_summary(stock: list[dict[str, Any]]) -> str | None:
  if not stock:
    return None
  parts = []
  for item in stock:
    inventory = item.get("inventory_id", "inventory")
    kind = item.get("kind", "object")
    variant = item.get("variant")
    available = item.get("available", "?")
    label = f"{inventory}:{kind}" + (f"/{variant}" if variant else "")
    parts.append(f"{label}={available}")
  kept: list[str] = []
  chars = len("stock ")
  for part in parts:
    separator = 2 if kept else 0
    if (
      len(kept) >= _MAX_INLINE_STOCK
      or chars + separator + len(part) > _MAX_INLINE_STOCK_CHARS
    ):
      break
    kept.append(part)
    chars += separator + len(part)
  omitted = len(parts) - len(kept)
  summary = "stock " + ", ".join(kept)
  if omitted:
    summary += (", " if kept else "") + f"omitted={omitted}"
  return summary


def build_object_lines(record: ObservationRecord) -> list[str]:
  """Render current stock and a bounded inline object list."""
  try:
    objects, stock = object_payload(record)
  except (TypeError, ValueError):
    return [
      (
        "objects unavailable invalid_payload "
        f"source={record.source} snapshot_id={record.id}"
      )
    ]
  summaries = [_object_summary(item) for item in objects]
  sequence = record.metadata.get("source_sequence")
  sequence_text = f" source_sequence={sequence}" if sequence is not None else ""
  lines = [f"objects count={len(objects)}{sequence_text}"]
  stock_line = _stock_summary(stock)
  if stock_line is not None:
    lines.append(stock_line)
  kept: list[str] = []
  chars = 0
  for summary in summaries:
    separator = 1 if kept else 0
    if (
      len(kept) >= _MAX_INLINE_OBJECTS
      or chars + separator + len(summary) > _MAX_INLINE_CHARS
    ):
      break
    kept.append(summary)
    chars += separator + len(summary)
  lines.extend(kept)
  if len(kept) < len(summaries):
    lines.append(f"objects omitted={len(summaries) - len(kept)}")
  return lines


def build_object_digest(record: ObservationRecord) -> str:
  """Render one compact, current object-state digest for prompt history."""
  return "; ".join(build_object_lines(record))
