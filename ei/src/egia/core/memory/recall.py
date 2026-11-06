"""Recall service — keyword recall merged with optional vector recall (§3.2–3.3).

The planner's single entry point for recall-before-decomposition. Keyword
hits from the episodic and reference stores are the baseline; when a
:class:`egia.internal.recall.RecallIndex` is present (``opt/memory``'s
sqlite-vec index), vector hits are merged in — reference-first ordering
preserved, ids deduped. Without an index the service is keyword-only and
``search_docs`` answers empty: the documented uniform degradation.
"""

from __future__ import annotations

from collections.abc import Mapping

from egia.api.plan import PlanRecord
from egia.core.memory.reference import ReferenceStore
from egia.core.memory.store import EpisodicStore
from egia.internal.recall import RecallIndex
from egia.lib.logger import get_logger

__all__ = ["RecallService"]


class RecallService:
  """Merges keyword and vector recall over both plan stores and docs.

  ``docs`` maps doc ids (corpus-relative paths) to their text so
  ``search_docs`` can excerpt hits; it is independent of the index so the
  service stays usable when only one of the two is configured.
  """

  def __init__(
    self,
    episodic: EpisodicStore,
    reference: ReferenceStore,
    index: RecallIndex | None = None,
    docs: Mapping[str, str] | None = None,
  ) -> None:
    self._episodic = episodic
    self._reference = reference
    self._index = index
    self._docs = dict(docs or {})
    self._log = get_logger("egia.core.memory")

  @property
  def index(self) -> RecallIndex | None:
    """The vector index in use, or ``None`` — the degraded configuration."""
    return self._index

  def search_plans(self, order: str, limit: int) -> list[PlanRecord]:
    """Keyword plus vector recall, reference store first, deduped by id.

    Keyword hits keep their existing rank; vector hits (rehydrated through
    the stores' ``get_plan``) follow within each store's group, so reviewed
    plans always rank ahead of episodic ones. Without an index this is
    exactly the pre-vector keyword behavior.
    """
    reference_hits = self._reference.search_plans(order, limit)
    episodic_hits = self._episodic.search_plans(order, limit)
    vector_reference: list[PlanRecord] = []
    vector_episodic: list[PlanRecord] = []
    if self._index is not None:
      for plan_id in self._index.search_plans(order, limit):
        record = self._reference.get_plan(plan_id)
        if record is not None:
          vector_reference.append(record)
          continue
        record = self._episodic.get_plan(plan_id)
        if record is not None:
          vector_episodic.append(record)
    merged = reference_hits + vector_reference + episodic_hits + vector_episodic
    seen: set[str] = set()
    plans: list[PlanRecord] = []
    for plan in merged:
      if plan.id not in seen:
        seen.add(plan.id)
        plans.append(plan)
    self._log.info(
      "plan_recall",
      reference_hits=len(reference_hits) + len(vector_reference),
      episodic_hits=len(episodic_hits) + len(vector_episodic),
      used=len(plans[:limit]),
    )
    return plans[:limit]

  def search_docs(self, query: str, limit: int) -> list[tuple[str, str]]:
    """``(doc_id, excerpt)`` pairs for corpus passages nearest the query.

    Vector-only: keyword recall over plan stores never covers docs, and
    without an index the docs search degrades to empty (§3.2). Docs
    unknown to the loaded corpus mapping are skipped.
    """
    if self._index is None or not self._docs:
      return []
    hits: list[tuple[str, str]] = []
    for doc_id in self._index.search_documents(query, limit):
      text = self._docs.get(doc_id)
      if text is not None:
        hits.append((doc_id, _excerpt(text, query)))
    return hits

  def index_plan(self, record: PlanRecord) -> None:
    """Index a concluded plan's text for future vector recall.

    No-op without an index — the degradation is symmetric: what was never
    indexed is simply absent from vector hits, keyword recall unaffected.
    The indexed text is the order plus a per-step action/outcome summary,
    rendered exactly as the recall digest renders it.
    """
    if self._index is None:
      return
    lines = [record.order]
    for step in record.steps:
      names = [action.action for action in step.actions]
      label = "; ".join(names) if names else "final"
      lines.append(f"{label} -> {step.outcome.value}")
    self._index.index_plan(record.id, "\n".join(lines))


def _excerpt(text: str, query: str, width: int = 200) -> str:
  """~``width`` chars around the earliest query-token match, else the head."""
  lowered = text.lower()
  positions = [
    pos for token in query.lower().split() if (pos := lowered.find(token)) >= 0
  ]
  if not positions:
    return text[:width].strip()
  start = max(0, min(positions) - width // 4)
  return text[start : start + width].strip()
