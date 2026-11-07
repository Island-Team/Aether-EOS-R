"""Small durable archive for context that no longer belongs in the live prompt.

This is deliberately separate from the optional vector-memory subsystem.  It
is an append-only JSONL file under ``var/`` that preserves superseded
perception, budget-truncated context, and completed missions for a future
model-driven compressor to read and summarize.
"""

from __future__ import annotations

import hashlib
import json
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from egia.api.mission import MissionRecord
from egia.api.observation import ObservationRecord
from egia.api.plan import PlanRecord

__all__ = ["LocalMemoryStore"]


class LocalMemoryStore:
  """Append-only, de-duplicated context archive stored as JSONL."""

  def __init__(self, path: Path | str) -> None:
    self.path = Path(path)
    self.path.parent.mkdir(parents=True, exist_ok=True)
    self._keys = self._load_keys()

  def archive_observation(self, record: ObservationRecord, *, reason: str) -> bool:
    return self.append(
      "perception",
      record.model_dump(mode="json"),
      reason=reason,
      key=f"observation:{record.id}",
    )

  def archive_mission(self, record: MissionRecord) -> bool:
    return self.append(
      "mission",
      record.model_dump(mode="json"),
      reason="mission_finished",
      key=f"mission:{record.id}:{record.state.value}",
    )

  def archive_plan(self, record: PlanRecord) -> bool:
    """Retain a completed mission's full executed step/action history."""
    return self.append(
      "plan",
      record.model_dump(mode="json"),
      reason="mission_finished",
      key=f"plan:{record.id}",
    )

  def archive_context(self, kind: str, text: str, *, reason: str) -> bool:
    return self.append(
      "context",
      {"kind": kind, "text": text},
      reason=reason,
    )

  def append(
    self, kind: str, payload: dict[str, Any], *, reason: str, key: str | None = None
  ) -> bool:
    """Persist one archive entry; return False when its stable key exists."""
    stable_key = key or self._payload_key(kind, payload, reason)
    if stable_key in self._keys:
      return False
    entry = {
      "key": stable_key,
      "archived_at": datetime.now(UTC).isoformat(),
      "kind": kind,
      "reason": reason,
      "payload": payload,
    }
    with self.path.open("a", encoding="utf-8") as stream:
      stream.write(json.dumps(entry, ensure_ascii=False, sort_keys=True) + "\n")
    self._keys.add(stable_key)
    return True

  def _load_keys(self) -> set[str]:
    if not self.path.exists():
      return set()
    keys: set[str] = set()
    for line in self.path.read_text(encoding="utf-8").splitlines():
      try:
        entry = json.loads(line)
      except json.JSONDecodeError:
        continue
      if isinstance(entry, dict) and isinstance(entry.get("key"), str):
        keys.add(entry["key"])
    return keys

  @staticmethod
  def _payload_key(kind: str, payload: dict[str, Any], reason: str) -> str:
    body = json.dumps(payload, ensure_ascii=False, sort_keys=True, default=str)
    digest = hashlib.sha256(f"{kind}:{reason}:{body}".encode()).hexdigest()
    return f"{kind}:{digest}"
