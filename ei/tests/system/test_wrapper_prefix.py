"""Public wrappers preserve explicit prefixes and resolve symlink targets."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


def test_islandctl_symlink_resolves_the_real_checkout(tmp_path: Path) -> None:
  root = Path(__file__).resolve().parents[2]
  link = tmp_path / "bin/islandctl"
  link.parent.mkdir()
  link.symlink_to(root / "bin/islandctl")

  result = subprocess.run(
    [str(link), "paths"],
    cwd=tmp_path,
    check=True,
    capture_output=True,
    text=True,
  )

  assert f"prefix={root}" in result.stdout


def test_islandctl_preserves_explicit_prefix(tmp_path: Path) -> None:
  root = Path(__file__).resolve().parents[2]
  prefix = tmp_path / "selected-prefix"
  env = dict(os.environ)
  env["ISLAND_PREFIX"] = str(prefix)
  env["ISLAND_MODE"] = "portable"
  env["ISLAND_PYTHON"] = sys.executable
  env["PYTHONPATH"] = str(root / "src")

  result = subprocess.run(
    [str(root / "bin/islandctl"), "paths"],
    cwd=tmp_path,
    env=env,
    check=True,
    capture_output=True,
    text=True,
  )

  assert f"prefix={prefix}" in result.stdout
  assert f"share={prefix / 'share'}" in result.stdout
