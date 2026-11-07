"""Reference memory store — reviewed, promoted plans only.

One SQLite database under ``var/`` (``reference.db``). Promotion is the
*separate write path* of §3.3: an operator reviews a concluded episodic plan
and promotes it here through :meth:`ReferenceStore.promote` — a human
decision recorded with the plan, never an automatic consequence of success.
Only this path may set the ``effective`` / ``ineffective`` outcomes. The
planner reads this store first during recall-before-decomposition.
"""

from __future__ import annotations

import sqlite3
from datetime import UTC, datetime
from pathlib import Path

from egia.api.plan import PlanRecord, StepOutcome
from egia.lib.logger import get_logger

_SCHEMA = """
CREATE TABLE IF NOT EXISTS plans (
  plan_id TEXT PRIMARY KEY,
  order_text TEXT NOT NULL,
  outcome TEXT NOT NULL,
  reviewed_at TEXT NOT NULL,
  review_note TEXT,
  record_json TEXT NOT NULL
)
"""

_REVIEW_OUTCOMES = (StepOutcome.EFFECTIVE.value, StepOutcome.INEFFECTIVE.value)

__all__ = ["ReferenceStore"]


class ReferenceStore:
  """Promoted plan records, recalled ahead of episodic memory."""

  def __init__(self, path: Path | str) -> None:
    self._log = get_logger("egia.core.memory")
    db_path = Path(path)
    db_path.parent.mkdir(parents=True, exist_ok=True)
    self._conn = sqlite3.connect(db_path)
    self._conn.execute(_SCHEMA)

  def promote(
    self, plan: PlanRecord, *, outcome: str | None = None, note: str | None = None
  ) -> PlanRecord:
    """Record the operator's review and persist the plan as reference memory.

    Stamps ``reviewed_at`` (now, UTC) and ``review_note`` on the stored
    record. ``outcome`` optionally overrides the plan outcome and must be
    ``"effective"`` or ``"ineffective"`` — the review path is the only writer
    of those judgments. Re-promoting the same plan id replaces the row.
    Returns the record as stored.
    """
    if outcome is not None and outcome not in _REVIEW_OUTCOMES:
      raise ValueError(
        f"review outcome must be one of {_REVIEW_OUTCOMES}, got {outcome!r}"
      )
    update: dict = {"reviewed_at": datetime.now(UTC), "review_note": note}
    if outcome is not None:
      update["outcome"] = StepOutcome(outcome)
    record = plan.model_copy(update=update)
    assert record.reviewed_at is not None
    self._conn.execute(
      "INSERT OR REPLACE INTO plans VALUES (?, ?, ?, ?, ?, ?)",
      (
        record.id,
        record.order,
        record.outcome.value,
        record.reviewed_at.isoformat(),
        record.review_note,
        record.model_dump_json(),
      ),
    )
    self._conn.commit()
    self._log.info(
      "plan_promoted",
      plan_id=record.id,
      mission_id=record.mission_id,
      outcome=record.outcome.value,
    )
    return record

  def get_plan(self, plan_id: str) -> PlanRecord | None:
    """Recall one promoted record by id, or ``None`` when absent."""
    row = self._conn.execute(
      "SELECT record_json FROM plans WHERE plan_id = ?", (plan_id,)
    ).fetchone()
    if row is None:
      return None
    return PlanRecord.model_validate_json(str(row[0]))

  def search_plans(self, query: str, limit: int) -> list[PlanRecord]:
    """Keyword recall: case-insensitive substring match on the order text.

    Same LIKE semantics as the episodic store; vector recall over both
    stores arrives with sqlite-vec in ``opt/memory`` and degrades to this.
    """
    rows = self._conn.execute(
      "SELECT record_json FROM plans WHERE order_text LIKE ? COLLATE NOCASE "
      "ORDER BY rowid DESC LIMIT ?",
      (f"%{query}%", limit),
    ).fetchall()
    return [PlanRecord.model_validate_json(str(row[0])) for row in rows]

  def list_plans(self, limit: int) -> list[PlanRecord]:
    """The newest ``limit`` promoted records, most recent first."""
    rows = self._conn.execute(
      "SELECT record_json FROM plans ORDER BY rowid DESC LIMIT ?", (limit,)
    ).fetchall()
    return [PlanRecord.model_validate_json(str(row[0])) for row in rows]

  def close(self) -> None:
    self._conn.close()
