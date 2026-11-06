"""Relocatable process invocation for the private EGIA console."""

from __future__ import annotations

import asyncio
from types import SimpleNamespace

import egia_ext.tui.__main__ as tui_main
from egia_ext.tui.__main__ import _pipeline_argv, _selected_product_config
from egia_ext.tui.config import TuiConfig
from island.path import Layout


def test_pipeline_argv_uses_installed_module_not_source_path(tmp_path) -> None:
  layout = Layout.resolve(prefix=tmp_path, mode="system", environ={})

  argv = _pipeline_argv(layout.component_etc("egia"), layout, ["--mode", "sandbox"])

  assert argv[:2] == ["-m", "egia.main"]
  assert not any("/src/" in value for value in argv)
  assert argv[-2:] == ["--mode", "sandbox"]
  assert str(layout.state) in argv
  assert str(layout.log) in argv


def test_product_config_uses_environment_fallback(tmp_path) -> None:
  layout = Layout.resolve(prefix=tmp_path, mode="source", environ={})

  selected = _selected_product_config(
    layout,
    None,
    {"ISLAND_CONFIG": "deploy/custom.toml"},
  )

  assert selected == (tmp_path / "deploy/custom.toml").resolve()


def test_explicit_product_config_wins_over_environment(tmp_path) -> None:
  layout = Layout.resolve(prefix=tmp_path, mode="source", environ={})

  selected = _selected_product_config(
    layout,
    "deploy/explicit.toml",
    {"ISLAND_CONFIG": "deploy/environment.toml"},
  )

  assert selected == (tmp_path / "deploy/explicit.toml").resolve()


def test_run_wires_mission_command_callback(tmp_path, monkeypatch) -> None:
  layout = Layout.resolve(prefix=tmp_path, mode="source", environ={})
  seen: list[dict] = []

  async def idle(*args, **kwargs) -> None:
    await asyncio.Event().wait()

  async def fake_send(state, config, payload: dict) -> None:
    seen.append(payload)

  async def fake_curses(app) -> None:
    assert app.on_mission is not None
    await app.on_mission({"operation": "create", "order": "inspect bay"})

  monkeypatch.setattr(tui_main, "load_tui_config", lambda **kwargs: TuiConfig())
  monkeypatch.setattr(
    tui_main,
    "load_product_config",
    lambda *args, **kwargs: SimpleNamespace(
      components=SimpleNamespace(eos=SimpleNamespace(launch="bin/eos"))
    ),
  )
  monkeypatch.setattr(tui_main, "send_mission_command", fake_send)
  monkeypatch.setattr(tui_main, "run_curses", fake_curses)
  monkeypatch.setattr(tui_main, "replay_tail_offset", lambda path: 0)
  for name in (
    "health_feed",
    "context_feed",
    "backend_feed",
    "model_io_feed",
    "missions_feed",
    "primitives_feed",
    "skills_feed",
    "prompts_feed",
    "log_tail",
  ):
    monkeypatch.setattr(tui_main, name, idle)
  args = SimpleNamespace(
    etc=None,
    log_level=None,
    attach=True,
    child_args=[],
    island_config=None,
  )

  assert asyncio.run(tui_main._run(args, layout)) == 0
  assert seen == [{"operation": "create", "order": "inspect bay"}]


def test_tui_unhandled_error_is_persisted_without_terminal_traceback(
  tmp_path, monkeypatch, capsys
) -> None:
  async def fail(*args, **kwargs):
    raise RuntimeError("tui exploded")

  monkeypatch.setattr(tui_main, "_run", fail)
  monkeypatch.setattr(tui_main.sys, "stdout", SimpleNamespace(isatty=lambda: True))
  log_root = tmp_path / "var/log"

  result = tui_main.main(
    [
      "--attach",
      "--log-root",
      str(log_root),
      "--state-root",
      str(tmp_path / "var/lib"),
      "--cache-root",
      str(tmp_path / "var/cache"),
      "--run-root",
      str(tmp_path / "var/run"),
      "--tmp-root",
      str(tmp_path / "var/tmp"),
    ]
  )

  assert result == 1
  assert capsys.readouterr().err == ""
  text = (log_root / "egia/ext/tui.log").read_text(encoding="utf-8")
  assert "tui_unhandled_error" in text
  assert "RuntimeError: tui exploded" in text
