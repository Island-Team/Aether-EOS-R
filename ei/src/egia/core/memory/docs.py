"""Local docs corpus — plain files indexed for RAG retrieval (§3.2).

The committed reference notes and deployment manuals under
``docs/egia`` (or any directory named by ``[memory] docs_corpus``) are
loaded as ``{relative_path: text}`` and indexed into the same vector index
as plan recall, so "no sqlite-vec" degrades both uniformly. No watcher:
re-indexing happens at launch.
"""

from __future__ import annotations

from pathlib import Path

from egia.internal.recall import RecallIndex
from egia.lib.logger import get_logger

_CORPUS_SUFFIXES = (".md", ".txt")

__all__ = ["index_docs_corpus", "load_docs_corpus"]


def load_docs_corpus(corpus_dir: Path | str) -> dict[str, str]:
  """All ``*.md``/``*.txt`` files under ``corpus_dir``, recursively.

  Keys are paths relative to the corpus root (forward slashes), values the
  file texts. An empty or missing directory yields an empty mapping — docs
  RAG is off, never a launch error.
  """
  log = get_logger("egia.core.memory")
  root = Path(corpus_dir)
  if not root.is_dir():
    if str(corpus_dir):
      log.warning("docs_corpus_missing", corpus=str(corpus_dir))
    return {}
  docs: dict[str, str] = {}
  for path in sorted(root.rglob("*")):
    if path.is_file() and path.suffix.lower() in _CORPUS_SUFFIXES:
      docs[path.relative_to(root).as_posix()] = path.read_text(encoding="utf-8")
  return docs


def index_docs_corpus(index: RecallIndex, docs: dict[str, str]) -> int:
  """Index every corpus document; returns the number indexed."""
  for doc_id, text in docs.items():
    index.index_document(doc_id, text)
  if docs:
    get_logger("egia.core.memory").info("docs_corpus_indexed", count=len(docs))
  return len(docs)
