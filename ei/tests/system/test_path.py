"""Product-level checks for the Island filesystem layout contract."""

from __future__ import annotations

from pathlib import Path

import pytest

from island.path import Layout, Mode


def test_source_layout_is_checkout_relative_not_cwd(
  monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
  monkeypatch.chdir(tmp_path)

  layout = Layout.resolve(environ={})

  root = Path(__file__).resolve().parents[2]
  assert layout.mode is Mode.SOURCE
  assert layout.prefix == root
  assert layout.etc == root / "etc"
  assert layout.contracts == root / "share/contracts"
  assert layout.doc == root / "docs"
  assert layout.component_state("egia") == root / "var/lib/egia"
  assert layout.component_log("eos") == root / "var/log/eos"


def test_system_layout_maps_each_unix_role() -> None:
  layout = Layout.resolve(mode=Mode.SYSTEM, environ={})

  assert layout.prefix == Path("/usr")
  assert layout.etc == Path("/etc/island")
  assert layout.share == Path("/usr/share/island")
  assert layout.egia_defaults == Path("/usr/share/defaults/egia")
  assert layout.doc == Path("/usr/share/doc/island")
  assert layout.libexec == Path("/usr/libexec/island")
  assert layout.state == Path("/var/lib/island")
  assert layout.cache == Path("/var/cache/island")
  assert layout.log == Path("/var/log/island")
  assert layout.run == Path("/run/island")
  assert layout.tmp == Path("/var/tmp/island")
  assert layout.src is None


def test_portable_layout_keeps_every_role_below_prefix(tmp_path: Path) -> None:
  layout = Layout.resolve(mode=Mode.PORTABLE, prefix=tmp_path, environ={})

  assert layout.prefix == tmp_path
  assert layout.etc == tmp_path / "etc"
  assert layout.share == tmp_path / "share"
  assert layout.egia_defaults == tmp_path / "share/defaults/egia"
  assert layout.doc == tmp_path / "share/doc"
  assert layout.state == tmp_path / "var/lib"
  assert layout.src is None


def test_system_documentation_tracks_custom_install_prefix(tmp_path: Path) -> None:
  layout = Layout.resolve(mode=Mode.SYSTEM, prefix=tmp_path, environ={})

  assert layout.share == tmp_path / "share/island"
  assert layout.egia_defaults == tmp_path / "share/defaults/egia"
  assert layout.doc == tmp_path / "share/doc/island"


def test_explicit_root_wins_over_environment(tmp_path: Path) -> None:
  explicit = tmp_path / "explicit"
  inherited = tmp_path / "inherited"

  layout = Layout.resolve(
    etc=explicit,
    environ={"ISLAND_ETC": str(inherited)},
  )

  assert layout.etc == explicit


def test_legacy_var_splits_mutable_roles_below_one_root(tmp_path: Path) -> None:
  layout = Layout.resolve(
    var=tmp_path / "legacy",
    cache=tmp_path / "explicit-cache",
    environ={"ISLAND_STATE": str(tmp_path / "environment-state")},
  )

  assert layout.component_state("egia") == tmp_path / "legacy/lib/egia"
  assert layout.component_cache("egia") == tmp_path / "explicit-cache/egia"
  assert layout.component_log("egia") == tmp_path / "legacy/log/egia"
  assert layout.run == tmp_path / "legacy/run"
  assert layout.tmp == tmp_path / "legacy/tmp"


def test_relative_legacy_var_is_checkout_relative(
  monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
  monkeypatch.chdir(tmp_path)

  layout = Layout.resolve(var="mutable", environ={})

  root = Path(__file__).resolve().parents[2]
  assert layout.state == root / "mutable/lib"


def test_relative_override_is_rejected() -> None:
  with pytest.raises(ValueError, match="etc root must be absolute"):
    Layout.resolve(etc="etc", environ={})


def test_unknown_component_is_rejected() -> None:
  layout = Layout.resolve(environ={})

  with pytest.raises(ValueError, match="unknown Island component"):
    layout.component_cache("planner")
