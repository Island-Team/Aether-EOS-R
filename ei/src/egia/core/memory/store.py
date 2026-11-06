"""Episodic memory store — one SQLite database under ``var/``.

MVP: an append-only ``records`` table holding every observation the
consolidation loop drains from the state queue, so nothing admitted to the
pipeline is lost to queue eviction, plus a ``plans`` table holding the
versioned, outcome-judged plan records the planner persists at every mission
end (``docs/egia/egia-2607.md`` §3.3). The working and reference stores are their
own modules (``working.py``, ``reference.py``); the sqlite-vec index (§3.2)
arrives in ``opt/memory``.
"""

from __future__ import annotations

import json
import sqlite3
from pathlib import Path

from egia.api.observation import ObservationRecord
from egia.api.plan import PlanRecord
from egia.lib.logger import get_logger

_SCHEMA = """
CREATE TABLE IF NOT EXISTS records (
  id TEXT PRIMARY KEY,
  ts TEXT NOT NULL,
  kind TEXT NOT NULL,
  source TEXT NOT NULL,
  text TEXT NOT NULL,
  metadata TEXT NOT NULL
)
"""

_PLANS_SCHEMA = """
CREATE TABLE IF NOT EXISTS plans (
  id TEXT PRIMARY KEY,
  mission_id TEXT NOT NULL,
  order_text TEXT NOT NULL,
  outcome TEXT NOT NULL,
  started_at TEXT NOT NULL,
  finished_at TEXT NOT NULL,
  record_json TEXT NOT NULL
)
"""

__all__ = ["EpisodicStore"]


class EpisodicStore:
  """Append-only episodic record log backed by one SQLite file."""

  def __init__(self, path: Path | str) -> None:
    self._log = get_logger("egia.core.memory")
    db_path = Path(path)
    db_path.parent.mkdir(parents=True, exist_ok=True)
    self._conn = sqlite3.connect(db_path)
    self._conn.execute(_SCHEMA)
    self._conn.execute(_PLANS_SCHEMA)
    self._closed = False

  def append(self, record: ObservationRecord) -> bool:
    """Persist one observation. Returns False when the id already exists."""
    cursor = self._conn.execute(
      "INSERT OR IGNORE INTO records VALUES (?, ?, ?, ?, ?, ?)",
      (
        record.id,
        record.timestamp.isoformat(),
        record.kind,
        record.source,
        record.text,
        json.dumps(record.metadata),
      ),
    )
    self._conn.commit()
    return cursor.rowcount > 0

  def count(self) -> int:
    """Total persisted records."""
    row = self._conn.execute("SELECT COUNT(*) FROM records").fetchone()
    return int(row[0])

  def recent(self, limit: int) -> list[tuple[str, str, str]]:
    """The newest ``limit`` records as ``(ts, kind, text)`` rows."""
    rows = self._conn.execute(
      "SELECT ts, kind, text FROM records ORDER BY rowid DESC LIMIT ?", (limit,)
    ).fetchall()
    return [(str(ts), str(kind), str(text)) for ts, kind, text in rows]

  def save_plan(self, record: PlanRecord) -> bool:
    """Persist one plan record. Returns False when the id already exists.

    Also returns False on a closed store: the supervisor stops subsystems
    concurrently, so a mission aborting during shutdown may save after
    consolidation has closed the file — the wave-1 shutdown race. The plan
    is recoverable from the working store on the next launch instead.
    """
    if self._closed:
      self._log.warning("save_plan_on_closed_store", plan_id=record.id)
      return False
    cursor = self._conn.execute(
      "INSERT OR IGNORE INTO plans VALUES (?, ?, ?, ?, ?, ?, ?)",
      (
        record.id,
        record.mission_id,
        record.order,
        record.outcome.value,
        record.started_at.isoformat(),
        record.finished_at.isoformat(),
        record.model_dump_json(),
      ),
    )
    self._conn.commit()
    return cursor.rowcount > 0

  def get_plan(self, plan_id: str) -> PlanRecord | None:
    """Recall one plan record by id, or ``None`` when absent."""
    row = self._conn.execute(
      "SELECT record_json FROM plans WHERE id = ?", (plan_id,)
    ).fetchone()
    if row is None:
      return None
    return PlanRecord.model_validate_json(str(row[0]))

  def list_plans(self, limit: int) -> list[PlanRecord]:
    """The newest ``limit`` plan records, most recent first."""
    rows = self._conn.execute(
      "SELECT record_json FROM plans ORDER BY rowid DESC LIMIT ?", (limit,)
    ).fetchall()
    return [PlanRecord.model_validate_json(str(row[0])) for row in rows]

  def search_plans(self, query: str, limit: int) -> list[PlanRecord]:
    """Keyword recall: case-insensitive substring match on the order text.

    The §3.3 recall loop's baseline; vector recall over the same table
    arrives with sqlite-vec in ``opt/memory`` and degrades to this when
    absent.
    """
    rows = self._conn.execute(
      "SELECT record_json FROM plans WHERE order_text LIKE ? COLLATE NOCASE "
      "ORDER BY rowid DESC LIMIT ?",
      (f"%{query}%", limit),
    ).fetchall()
    return [PlanRecord.model_validate_json(str(row[0])) for row in rows]

  def close(self) -> None:
    self._closed = True
    self._conn.close()
