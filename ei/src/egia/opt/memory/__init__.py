"""Optional memory subsystems — vector recall behind ``internal/recall``.

Imports are lazy: importing this package never requires sqlite-vec. Accessing
:class:`SqliteVecIndex` raises :class:`ImportError` when the extension is
absent, so the composition root degrades to keyword recall (§3.2).
"""

from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
  from egia.opt.memory.vec import SqliteVecIndex

__all__ = ["SqliteVecIndex"]


def __getattr__(name: str) -> object:
  if name == "SqliteVecIndex":
    from egia.opt.memory.vec import SqliteVecIndex

    return SqliteVecIndex
  raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
