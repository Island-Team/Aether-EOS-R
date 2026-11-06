"""Minimal ``.env`` loading — file as fallback, environment as authority.

Deployment secrets (model API keys) live in the environment by design. The
composition root checks ``.env`` below the resolved Island prefix and then
below the resolved EGIA config root; this module only loads the injected path.
Variables already present in the environment are never overridden — a file
cannot shadow a real export. Only ``KEY=VALUE`` lines are honored; comments
(``#``) and blank lines are skipped; ``export`` prefixes are tolerated;
values may be bare, single-quoted, or double-quoted.
"""

from __future__ import annotations

import os
from pathlib import Path

__all__ = ["load_env_file"]


def load_env_file(path: Path) -> list[str]:
  """Populate ``os.environ`` from ``path``; returns the variable names loaded.

  Names already in the environment are skipped (and not reported). A missing
  file is not an error — it loads nothing.
  """
  if not path.is_file():
    return []
  loaded: list[str] = []
  for line in path.read_text(encoding="utf-8").splitlines():
    entry = line.strip()
    if not entry or entry.startswith("#"):
      continue
    if entry.startswith("export "):
      entry = entry[len("export ") :].strip()
    if "=" not in entry:
      continue
    key, _, value = entry.partition("=")
    key = key.strip()
    if not key or not key.isidentifier():
      continue
    if os.environ.get(key):
      continue
    os.environ[key] = _unquote(value.strip())
    loaded.append(key)
  return loaded


def _unquote(value: str) -> str:
  if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
    return value[1:-1]
  return value
