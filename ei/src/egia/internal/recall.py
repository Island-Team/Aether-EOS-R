"""Substitution-seam Protocol for vector recall (``docs/egia/egia-2607.md`` §3.2).

``core/memory`` recalls past plans and documentation through this Protocol,
never through a concrete index. The reference implementation lives in
``opt/memory`` (sqlite-vec); a deployment without that package passes
``None`` and the core degrades to keyword recall without breaking an import.

The interface layer is pure: declarations and data only, no I/O, no state.
Implementations own their persistence; callers own the ids they index.
"""

from __future__ import annotations

from typing import Protocol, runtime_checkable

__all__ = ["RecallIndex"]


@runtime_checkable
class RecallIndex(Protocol):
  """Nearest-neighbor index over plan texts and documentation passages.

  Ids are caller-chosen strings (plan ids, corpus-relative doc paths);
  searches return ids ranked best-first. Re-indexing an existing id
  replaces its entry.
  """

  def index_plan(self, plan_id: str, text: str) -> None:
    """Add or replace one plan's searchable text under ``plan_id``."""
    ...

  def search_plans(self, query: str, limit: int) -> list[str]:
    """The ``limit`` plan ids nearest to ``query``, best first."""
    ...

  def index_document(self, doc_id: str, text: str) -> None:
    """Add or replace one documentation file under ``doc_id``."""
    ...

  def search_documents(self, query: str, limit: int) -> list[str]:
    """The ``limit`` doc ids nearest to ``query``, best first."""
    ...

  def close(self) -> None:
    """Release the index's resources; further calls are undefined."""
    ...
