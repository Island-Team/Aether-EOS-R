"""Structural layering invariant: egia.io may import core; core never imports io.

import-linter's layers contract only covers the listed layer packages, so this
suite scans every module under ``egia/core`` by hand — the composition root
(``egia/core/__init__.py``, the wiring layer io.md allows) is the sole
exception. See ``docs/egia/io.md``.
"""

from __future__ import annotations

import ast
from pathlib import Path

import egia.core
import egia.io

CORE_ROOT = Path(egia.core.__file__).parent
COMPOSITION_ROOT = CORE_ROOT / "__init__.py"


def _imported_modules(tree: ast.Module) -> set[str]:
  """Return every module name imported anywhere in ``tree``."""
  names: set[str] = set()
  for node in ast.walk(tree):
    if isinstance(node, ast.Import):
      names.update(alias.name for alias in node.names)
    elif isinstance(node, ast.ImportFrom) and node.module and node.level == 0:
      names.add(node.module)
  return names


def test_io_imports_standalone() -> None:
  """``egia.io`` imports without dragging the pipeline in behind it."""
  assert egia.io.NorthboundServer.__module__ == "egia.io.northbound"


def test_core_never_imports_io() -> None:
  """No module under egia/core except the composition root imports egia.io."""
  offenders: list[str] = []
  for path in sorted(CORE_ROOT.rglob("*.py")):
    if path == COMPOSITION_ROOT:
      continue
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    if any(
      name == "egia.io" or name.startswith("egia.io.")
      for name in _imported_modules(tree)
    ):
      offenders.append(str(path.relative_to(CORE_ROOT)))
  assert not offenders, f"core modules importing egia.io: {offenders}"


def test_composition_root_is_the_sole_importer() -> None:
  """The composition root does import egia.io — the documented allowance."""
  tree = ast.parse(COMPOSITION_ROOT.read_text(encoding="utf-8"))
  assert any(
    name == "egia.io" or name.startswith("egia.io.") for name in _imported_modules(tree)
  )
