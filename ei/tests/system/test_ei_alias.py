"""EI public entry points and EGIA compatibility commands."""

from __future__ import annotations

import subprocess
from pathlib import Path


def test_ei_commands_use_new_public_name_and_legacy_commands_remain() -> None:
  """Both command families resolve the same installed implementation."""
  root = Path(__file__).resolve().parents[2]
  for command, expected in (
    ("ei", "EI decision runtime"),
    ("egia", "EGIA decision layer"),
    ("ei-config", "usage: ei-config"),
    ("egia-config", "usage: egia-config"),
  ):
    result = subprocess.run(
      [str(root / "bin" / command), "--help"],
      check=True,
      capture_output=True,
      text=True,
    )
    assert expected in result.stdout
