"""sqlite-vec recall index — the §3.2 vector-recall implementation.

One SQLite file under the EGIA cache root holds two ``vec0`` virtual tables
(plans, documents) plus id-mapping tables, since ``vec0`` keys on integer
rowids while callers index by string ids. The same index serves both plan
recall and docs-corpus RAG, so "no sqlite-vec" degrades both uniformly.

Embeddings come from a deterministic local hashing embedder: lowercase
tokenize, signed-hash each token into a fixed-dim vector, L2-normalize. No
network, no model download, fully reproducible — the offline MVP. Its honest
limitation: it matches shared vocabulary, not true semantics — texts that
share rare tokens rank close regardless of meaning, and synonymy is missed.
A real embedding model is a later drop-in behind the same Protocol.

Imports only ``egia.internal.recall`` and ``egia.lib`` (plus stdlib and
sqlite-vec), per the ``opt/`` boundary. Importing this module raises
:class:`ImportError` when sqlite-vec is absent; the package's lazy export
keeps ``import egia.opt.memory`` itself safe.
"""

from __future__ import annotations

import hashlib
import math
import re
import sqlite3
from pathlib import Path

try:
  import sqlite_vec
except ImportError as exc:  # pragma: no cover - exercised via monkeypatch
  raise ImportError(
    "egia.opt.memory requires the sqlite-vec package "
    "(install the 'vec' extra); without it vector recall degrades to "
    "keyword search and this module must not be imported"
  ) from exc

from egia.internal.recall import RecallIndex
from egia.lib.logger import get_logger

EMBEDDING_DIM = 256

_TOKENS = re.compile(r"[a-z0-9]+")

__all__ = ["EMBEDDING_DIM", "SqliteVecIndex", "embed_text"]


def embed_text(text: str, dim: int = EMBEDDING_DIM) -> list[float]:
  """Deterministic signed-hashing embedding of ``text`` into ``dim`` floats.

  Each lowercase alphanumeric token contributes +/-1 at one hashed slot;
  the result is L2-normalized (the zero vector for tokenless text). Pure
  stdlib, stable across processes — the offline MVP embedder documented in
  the module header.
  """
  vector = [0.0] * dim
  for token in _TOKENS.findall(text.lower()):
    digest = hashlib.sha256(token.encode()).digest()
    slot = int.from_bytes(digest[:4], "big") % dim
    vector[slot] += 1.0 if digest[4] % 2 == 0 else -1.0
  norm = math.sqrt(sum(value * value for value in vector))
  if norm > 0:
    vector = [value / norm for value in vector]
  return vector


class SqliteVecIndex(RecallIndex):
  """Vector recall over plans and documents in one sqlite-vec database."""

  def __init__(self, path: Path | str, *, dim: int = EMBEDDING_DIM) -> None:
    self._log = get_logger("egia.opt.memory")
    self._dim = dim
    db_path = Path(path)
    db_path.parent.mkdir(parents=True, exist_ok=True)
    self._conn = sqlite3.connect(db_path)
    self._conn.enable_load_extension(True)
    sqlite_vec.load(self._conn)
    self._conn.enable_load_extension(False)
    for name in ("plans", "documents"):
      self._conn.execute(f"CREATE TABLE IF NOT EXISTS {name}_ids (id TEXT PRIMARY KEY)")
      self._conn.execute(
        f"CREATE VIRTUAL TABLE IF NOT EXISTS {name}_vec "
        f"USING vec0(embedding float[{dim}])"
      )
    self._conn.commit()

  def index_plan(self, plan_id: str, text: str) -> None:
    self._index("plans", plan_id, text)

  def index_document(self, doc_id: str, text: str) -> None:
    self._index("documents", doc_id, text)

  def search_plans(self, query: str, limit: int) -> list[str]:
    return self._search("plans", query, limit)

  def search_documents(self, query: str, limit: int) -> list[str]:
    return self._search("documents", query, limit)

  def close(self) -> None:
    self._conn.close()

  def _index(self, table: str, entry_id: str, text: str) -> None:
    """Replace ``entry_id``'s vector; the id-mapping rowid is the vec rowid."""
    row = self._conn.execute(
      f"SELECT rowid FROM {table}_ids WHERE id = ?", (entry_id,)
    ).fetchone()
    if row is not None:
      self._conn.execute(f"DELETE FROM {table}_vec WHERE rowid = ?", (row[0],))
      self._conn.execute(f"DELETE FROM {table}_ids WHERE id = ?", (entry_id,))
    cursor = self._conn.execute(f"INSERT INTO {table}_ids (id) VALUES (?)", (entry_id,))
    self._conn.execute(
      f"INSERT INTO {table}_vec (rowid, embedding) VALUES (?, ?)",
      (cursor.lastrowid, sqlite_vec.serialize_float32(embed_text(text, self._dim))),
    )
    self._conn.commit()

  def _search(self, table: str, query: str, limit: int) -> list[str]:
    """Cosine-equivalent nearest neighbors (L2 on normalized vectors)."""
    if limit <= 0:
      return []
    rows = self._conn.execute(
      f"SELECT v.rowid, i.id FROM {table}_vec v "
      f"JOIN {table}_ids i ON i.rowid = v.rowid "
      "WHERE v.embedding MATCH ? AND k = ? ORDER BY v.distance",
      (sqlite_vec.serialize_float32(embed_text(query, self._dim)), limit),
    ).fetchall()
    return [str(entry_id) for _, entry_id in rows]
