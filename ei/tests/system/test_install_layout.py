"""CMake installation follows configured shared roots and system paths."""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

from egia.lib.config import ConfigError, load_launch_config
from island.path import Layout, Mode


@pytest.mark.skipif(shutil.which("cmake") is None, reason="cmake is unavailable")
def test_staged_install_materializes_configured_logical_share(
  tmp_path: Path,
) -> None:
  root = Path(__file__).resolve().parents[2]
  build = tmp_path / "build"
  stage = tmp_path / "stage"
  subprocess.run(
    [
      "cmake",
      "-S",
      str(root),
      "-B",
      str(build),
      "-DISLAND_BUILD_EOS=OFF",
      "-DISLAND_BUILD_ECOS=OFF",
    ],
    check=True,
    capture_output=True,
    text=True,
  )
  subprocess.run(
    ["cmake", "--install", str(build), "--prefix", "/usr"],
    env={**os.environ, "DESTDIR": str(stage)},
    check=True,
    capture_output=True,
    text=True,
  )

  installed_config = (stage / "etc/island/island.toml").read_text(encoding="utf-8")
  assert 'share = ["share/island"]' in installed_config
  assert "enabled = false" in installed_config
  assert (stage / "etc/island/egia/missions.toml").is_file()
  assert "home_living_2026" in (stage / "etc/island/egia/missions.toml").read_text(
    encoding="utf-8"
  )
  layout = Layout.resolve(mode=Mode.SYSTEM, prefix=stage / "usr", environ={})
  factory = layout.egia_defaults
  resource_copy = layout.share / "defaults/egia"
  expected = root / "share/defaults/egia"
  for source in expected.rglob("*.toml"):
    relative = source.relative_to(expected)
    installed = factory / relative
    assert installed.is_file() and not installed.is_symlink()
    assert installed.read_bytes() == source.read_bytes()
    assert (resource_copy / relative).read_bytes() == source.read_bytes()
  load_launch_config(etc=str(stage / "etc/island/egia"), defaults=factory)
  shutil.rmtree(factory)
  with pytest.raises(ConfigError, match="factory config root not found"):
    load_launch_config(etc=str(stage / "etc/island/egia"), defaults=factory)
  assert (stage / "usr/share/island/eos/contracts/eos/gateway.proto").is_file()
  assert (stage / "usr/share/island/eos/contracts/eos/capabilities.proto").is_file()
  assert (stage / "usr/share/island/eos/schemas/eos/actions/arm_init.v1.json").is_file()
  assert (stage / "usr/libexec/island/island_admin/schema.py").is_file()
  assert not (stage / "usr/libexec/island/egia_ext").exists()
