"""Working memory store — the in-flight plan of the running mission.

One SQLite database under ``var/`` (``working.db``), the structural third
write path of the §3.2 three-store split. The planner UPSERTs the running
mission's :class:`egia.api.plan.PlanRecord` after every executed step, so a
crash or kill leaves the partial plan visible on disk; the consolidation
loop re-checks these rows on each wake and recovers ones whose mission is
no longer alive (§3.3).
"""

from __future__ import annotations

import sqlite3
from datetime import UTC, datetime
from pathlib import Path

from egia.api.plan import PlanRecord
from egia.lib.logger import get_logger

_SCHEMA = """
CREATE TABLE IF NOT EXISTS plans (
  mission_id TEXT PRIMARY KEY,
  plan_json TEXT NOT NULL,
  updated_at TEXT NOT NULL
)
"""

__all__ = ["WorkingStore"]


class WorkingStore:
  """One in-flight plan row per running mission, keyed on mission id."""

  def __init__(self, path: Path | str) -> None:
    self._log = get_logger("egia.core.memory")
    db_path = Path(path)
    db_path.parent.mkdir(parents=True, exist_ok=True)
    self._conn = sqlite3.connect(db_path)
    self._conn.execute(_SCHEMA)
    self._closed = False

  def upsert_plan(self, record: PlanRecord) -> None:
    """Insert or replace the mission's in-flight record (crash visibility).

    No-op on a closed store: the supervisor stops subsystems concurrently,
    so a mission aborting during shutdown may write after consolidation has
    closed the file — the row would be dropped at the next wake anyway.
    """
    if self._closed:
      self._log.warning("upsert_on_closed_store", mission_id=record.mission_id)
      return
    self._conn.execute(
      "INSERT OR REPLACE INTO plans VALUES (?, ?, ?)",
      (
        record.mission_id,
        record.model_dump_json(),
        datetime.now(UTC).isoformat(),
      ),
    )
    self._conn.commit()

  def get_plan(self, mission_id: str) -> PlanRecord | None:
    """The in-flight record for ``mission_id``, or ``None`` when absent."""
    row = self._conn.execute(
      "SELECT plan_json FROM plans WHERE mission_id = ?", (mission_id,)
    ).fetchone()
    if row is None:
      return None
    return PlanRecord.model_validate_json(str(row[0]))

  def drop(self, mission_id: str) -> None:
    """Remove the mission's row — the plan concluded and lives in episodic."""
    if self._closed:
      return
    self._conn.execute("DELETE FROM plans WHERE mission_id = ?", (mission_id,))
    self._conn.commit()

  def list_stale(self, active_mission_ids: set[str]) -> list[PlanRecord]:
    """Rows whose mission is not in ``active_mission_ids`` — crashed writes."""
    rows = self._conn.execute("SELECT plan_json FROM plans").fetchall()
    plans = [PlanRecord.model_validate_json(str(row[0])) for row in rows]
    return [p for p in plans if p.mission_id not in active_mission_ids]

  def close(self) -> None:
    self._closed = True
    self._conn.close()
