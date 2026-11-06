"""EOS REPL wrapper resolution, endpoint parsing, and foreground launch."""

from __future__ import annotations

from pathlib import Path
from unittest.mock import ANY

import pytest

from egia_ext.tui.repl import (
  launch_repl,
  parse_repl_endpoint,
  resolve_repl_wrapper,
)


def _executable(path: Path) -> None:
  path.write_text("#!/bin/sh\n", encoding="utf-8")
  path.chmod(path.stat().st_mode | 0o111)


def test_resolve_repl_wrapper_beside_source_launcher(tmp_path: Path) -> None:
  bin_dir = tmp_path / "Island-eos/bin"
  bin_dir.mkdir(parents=True)
  launcher = bin_dir / "eosd"
  wrapper = bin_dir / "eos-repl"
  _executable(launcher)
  _executable(wrapper)

  assert resolve_repl_wrapper(launcher) == wrapper.resolve()


def test_resolve_repl_wrapper_beside_installed_launcher(tmp_path: Path) -> None:
  bin_dir = tmp_path / "usr/bin"
  bin_dir.mkdir(parents=True)
  launcher = bin_dir / "eosd"
  wrapper = bin_dir / "eos-repl"
  _executable(launcher)
  _executable(wrapper)

  assert resolve_repl_wrapper(launcher) == wrapper.resolve()


@pytest.mark.parametrize(
  ("raw", "host", "port"),
  [
    ("127.0.0.1:16003", "127.0.0.1", 16003),
    ("eos.internal:443", "eos.internal", 443),
    ("[2001:db8::8]:16003", "2001:db8::8", 16003),
  ],
)
def test_parse_repl_endpoint(raw: str, host: str, port: int) -> None:
  assert parse_repl_endpoint(raw).host == host
  assert parse_repl_endpoint(raw).port == port


@pytest.mark.parametrize(
  "raw",
  [
    "",
    "localhost",
    "2001:db8::8:16003",
    "[]:16003",
    "host:nope",
    "host:0",
    "host:65536",
  ],
)
def test_parse_repl_endpoint_rejects_malformed_values(raw: str) -> None:
  with pytest.raises(ValueError, match="invalid EOS endpoint"):
    parse_repl_endpoint(raw)


def test_resolve_repl_wrapper_requires_executable(tmp_path: Path) -> None:
  launcher = tmp_path / "eosd"
  wrapper = tmp_path / "eos-repl"
  launcher.touch()
  wrapper.touch()
  wrapper.chmod(0o644)

  with pytest.raises(ValueError, match="not executable"):
    resolve_repl_wrapper(launcher)


@pytest.mark.asyncio
async def test_launch_repl_inherits_stdio_and_reports_exit(
  tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
  bin_dir = tmp_path / "bin"
  bin_dir.mkdir()
  wrapper = bin_dir / "eos-repl"
  _executable(wrapper)
  calls: list[tuple[tuple[str, ...], dict[str, object]]] = []

  class FakeProcess:
    async def wait(self) -> int:
      return 7

  async def fake_create(*argv: str, **kwargs: object) -> FakeProcess:
    calls.append((argv, kwargs))
    return FakeProcess()

  monkeypatch.setattr("egia_ext.tui.repl.asyncio.create_subprocess_exec", fake_create)
  result = await launch_repl(bin_dir / "eosd", "[::1]:16003")

  assert calls == [
    (
      (str(wrapper.resolve()), "--host", "::1", "--port", "16003"),
      {"env": ANY},
    )
  ]
  environment = calls[0][1]["env"]
  assert isinstance(environment, dict)
  assert environment["ISLAND_PREFIX"] == str(tmp_path.resolve())
  assert result.launched is True
  assert result.succeeded is False
  assert result.exit_code == 7
  assert result.error is None


@pytest.mark.asyncio
async def test_launch_repl_returns_resolution_error(tmp_path: Path) -> None:
  result = await launch_repl(tmp_path / "eosd", "127.0.0.1:16003")

  assert result.launched is False
  assert result.argv == ()
  assert "not found" in (result.error or "")


@pytest.mark.asyncio
async def test_launch_repl_returns_process_start_error(
  tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
  wrapper = tmp_path / "eos-repl"
  _executable(wrapper)

  async def fake_create(*argv: str, **kwargs: object) -> None:
    del argv, kwargs
    raise OSError("exec format error")

  monkeypatch.setattr("egia_ext.tui.repl.asyncio.create_subprocess_exec", fake_create)
  result = await launch_repl(tmp_path / "eosd", "127.0.0.1:16003")

  assert result.launched is False
  assert result.argv[0] == str(wrapper.resolve())
  assert result.error == "failed to start EOS REPL: exec format error"


@pytest.mark.asyncio
async def test_launch_repl_replaces_inherited_egia_prefix(
  tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
  eos_prefix = tmp_path / "Island-eos"
  bin_dir = eos_prefix / "bin"
  source_dir = eos_prefix / "adapters/eos-cxx"
  bin_dir.mkdir(parents=True)
  source_dir.mkdir(parents=True)
  wrapper = bin_dir / "eos-repl"
  _executable(wrapper)
  monkeypatch.setenv("ISLAND_PREFIX", str(tmp_path / "Island-egia"))
  captured: dict[str, object] = {}

  class FakeProcess:
    async def wait(self) -> int:
      return 0

  async def fake_create(*argv: str, **kwargs: object) -> FakeProcess:
    del argv
    captured.update(kwargs)
    return FakeProcess()

  monkeypatch.setattr("egia_ext.tui.repl.asyncio.create_subprocess_exec", fake_create)

  result = await launch_repl(bin_dir / "eosd", "127.0.0.1:16003")

  assert result.succeeded is True
  environment = captured["env"]
  assert isinstance(environment, dict)
  assert environment["ISLAND_PREFIX"] == str(eos_prefix.resolve())
  assert environment["ISLAND_MODE"] == "source"
