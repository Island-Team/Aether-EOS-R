# egia/opt/memory — sqlite-vec vector recall index

## Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [API](#api)
- [Design](#design)
- [Testing](#testing)

## Overview

`egia/opt/memory/` implements optional sqlite-vec recall through the `RecallIndex` Protocol from `src/egia/internal/recall.py`, using one database at `var/cache/egia/memory/vec.db`. The same index serves plan recall and docs-corpus RAG, so an unavailable sqlite-vec dependency degrades both to keyword search uniformly. Core memory never imports this package; [`egia/core/memory`](../core/memory.md) owns the Protocol and merge logic.

The package is behind an import-extra: `vec` in `pyproject.toml` (`sqlite-vec>=0.1`). Per the `opt/` boundary, `vec.py` imports only `egia.internal.recall` and `egia.lib` plus stdlib and sqlite-vec; the package `__init__` exports `SqliteVecIndex` lazily via module `__getattr__`, so `import egia.opt.memory` itself never fails without the extra — only reaching for the class raises `ImportError`.

## Architecture

```mermaid
flowchart LR
  cfg["etc/egia/egia.toml<br/>[launch] memory = true (default)<br/>[memory] vector_recall = true (default)"] --> root["main.py<br/>build_recall_index"]
  root -->|"SqliteVecIndex(var/cache/egia/memory/vec.db)"| idx["vec.py<br/>SqliteVecIndex"]
  root -->|"disabled or import failed"| none["None<br/>keyword-only recall, docs off"]
  idx --> svc["core/memory<br/>RecallService"]
  corpus["[memory] docs_corpus<br/>load_docs_corpus"] --> launch["core.launch<br/>index_docs_corpus"]
  launch --> idx
  svc --> planner["decision/planner.py<br/>digest: prior plans + docs excerpts"]
```

Activation is a composition-root decision (`build_recall_index` in `src/egia/main.py`, the only layer permitted to import `opt/` by name): the index is built only when `[launch] memory` (default true) and `[memory] vector_recall` are true **and** the import succeeds. The launch toggle is the master switch for the whole memory chain — off, neither the stores nor the index are wired; `vector_recall` off skips just the index. A failed import — or the flags off — logs `vector_recall_unavailable` (INFO) and yields `None`; startup never depends on the opt package being present. `core.launch` then hands the index to `RecallService`, indexes the docs corpus when configured, and closes the index after the supervisor stops.

One SQLite file holds four tables, created idempotently per `dim` (default 256, `EMBEDDING_DIM` in `src/egia/opt/memory/vec.py`):

- `plans_ids` / `documents_ids`: `id TEXT PRIMARY KEY` — the id-mapping tables bridging caller-chosen string ids to integer rowids.
- `plans_vec` / `documents_vec`: `CREATE VIRTUAL TABLE ... USING vec0(embedding float[<dim>])` — the two `vec0` virtual tables.

## API

`SqliteVecIndex(path, *, dim=256)` opens (creating if needed) the database and loads the sqlite-vec extension. The five Protocol methods:

- `index_plan(plan_id, text)` / `index_document(doc_id, text)` — add or replace the entry's embedding of `text` under the string id.
- `search_plans(query, limit)` / `search_documents(query, limit)` — the `limit` ids nearest to `query`, best first; `[]` for `limit <= 0`.
- `close()` — release the connection (part of the `RecallIndex` Protocol).

`embed_text(text, dim=256)` is the deterministic embedder (exported for tests and reuse); `EMBEDDING_DIM = 256` the default dimensionality. The module raises `ImportError` on import when sqlite-vec is absent — the message names the `vec` extra and the degradation.

Minimal example — indexing and searching one plan row:

```python
from egia.opt.memory import SqliteVecIndex

index = SqliteVecIndex("var/cache/egia/memory/vec.db")
index.index_plan("p-1", "dock the robot at the station")
# plans_ids: ("p-1") -> rowid N;  plans_vec: (rowid N, embedding float[256])
index.search_plans("dock at the station", 2)  # -> ["p-1", ...] best-first
index.close()
```

## Design

### Embedding input construction

The index is text-agnostic: callers provide a string. Documents are indexed whole. Plan text is assembled from the order plus one `primitive action(s) -> outcome` line per step; a final-only step renders `final -> outcome`. Document retrieval is independent of the persisted plan shape.

### The hashing embedder

`embed_text` is a deterministic local signed-hashing embedder — the offline MVP. Tokens are lowercase alphanumeric runs (`[a-z0-9]+`); each token contributes $\pm 1$ at one hashed slot, then the vector is L2-normalized:

$$ \text{slot}(t) = \operatorname{int}(\text{sha256}(t)[:4]) \bmod d,\qquad v_{\text{slot}(t)} \mathrel{+}= \begin{cases}+1 & \text{sha256}(t)[4]\ \text{even}\\ -1 & \text{odd}\end{cases},\qquad \hat{v} = \frac{v}{\lVert v\rVert_2} $$

A tokenless text stays the zero vector (the norm guard), not an error. No network, no model download, reproducible across processes. Honest limitation: it matches **shared vocabulary, not true semantics** — texts sharing rare tokens rank close regardless of meaning, and synonyms and paraphrase are missed entirely. `tests/egia/test_vec_recall.py` demonstrates both sides: a query whose full order string misses under keyword `LIKE` still ranks the right plan, precisely because the tokens overlap.

### Query path

`_search` embeds the query with the same embedder and runs one KNN query per table:

```sql
SELECT v.rowid, i.id FROM {table}_vec v
JOIN {table}_ids i ON i.rowid = v.rowid
WHERE v.embedding MATCH ? AND k = ?
ORDER BY v.distance
```

with the query vector serialized via `sqlite_vec.serialize_float32`. Because indexed and query vectors are L2-normalized, ordering by L2 distance is cosine-equivalent ($\lVert u-v\rVert^2 = 2 - 2\cos(u,v)$ for unit vectors). `limit <= 0` short-circuits to `[]`.

### Replace semantics

`_index` keeps the id-mapping rowid and the `vec0` rowid paired: an existing id's vec row and mapping row are deleted, a fresh mapping row is inserted, and the embedding is written under its `cursor.lastrowid`. Re-indexing an id therefore replaces its entry — the Protocol contract — without accumulating stale vectors.

### Degradation without the extra

Two paths reach the same shape: `[launch] memory = false` skips the whole memory chain, `[memory] vector_recall = false` (default true) returns `None` before any import attempt, and a failed import (sqlite-vec not installed) is caught by `build_recall_index`, which logs `vector_recall_unavailable` and launches with no index. Downstream, `RecallService` becomes keyword-only (`LIKE` on order text, reference store first) and `search_docs` answers `[]` — plan recall degrades, docs retrieval turns off, and no `egia.core` or `egia.opt.memory` import breaks. The `vec.db` file is simply absent in that deployment.

## Testing

Method: unit tests over a real sqlite-vec database in `tmp_path`, plus planner-level stubbed integration (fake model + admitting gate, real stores and index) and composition-root tests that monkeypatch `sys.modules` to simulate a broken opt import.

`tests/egia/test_vec_recall.py` is guarded by `pytest.importorskip("sqlite_vec")` (per-test and in the shared `_vec_index` helper), so the suite passes without the extra — the §5.1 optionality invariant. Covered:

- embedder determinism (case-insensitive tokens), L2 normalization, and the zero vector for tokenless text;
- vector ranking where keyword `LIKE` on the full order string misses both candidates;
- `RecallService` merging vector hits reference-first and deduping across stores and channels;
- degradation with no index (keyword-only, docs off, `index_plan` no-op);
- composition-root degradation: a present-but-broken `egia.opt.memory.vec` import yields `None` (never a launch failure), and `vector_recall = false` skips the import entirely;
- the planner's "Relevant docs:" prompt section, and the concluded plan being indexed for the next mission's vector recall.

Run (the extra is required for anything to execute rather than skip):

```bash
uv sync --extra vec        # or: pip install 'egia[vec]'
uv run pytest tests/egia/test_vec_recall.py -q
```

Pass criteria: with the extra, all tests pass; without it, the vec-dependent tests skip and the two `build_recall_index` degradation tests still run and pass. The skip semantics are guardrails, not masking: `RecallService.index_plan` behavior on stepped plans is pinned by `test_index_plan_renders_v2_steps` with a stub index (it runs in the default environment too), while the sqlite-vec-backed query paths (`test_vec_recall.py`'s KNN and replace tests) skip until the extra is installed.

Related: [`egia/core/memory`](../core/memory.md) (the seam's core side), [`decision.md`](../core/decision.md) (the digest consumer), [`modules.md`](../core/modules.md) (layering).
