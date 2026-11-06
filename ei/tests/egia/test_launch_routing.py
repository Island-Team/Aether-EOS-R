"""Composition-root routing across empty, playground, and real backends."""

from __future__ import annotations

from types import SimpleNamespace

import pytest

import egia.main as main_module
from island.path import Layout


def _switches(config, *, backend: bool, sandbox: bool):
  return config.model_copy(
    update={
      "launch": config.launch.model_copy(
        update={"backend": backend, "sandbox": sandbox, "memory": False}
      )
    }
  )


async def _run(monkeypatch, tmp_path, etc_root, config, launch_core, launch_empty):
  marker = object()

  async def start(selected, *_args, **_kwargs):
    return marker if selected.launch.sandbox else None

  async def stop(_process):
    return None

  monkeypatch.setattr(main_module, "start_sandbox", start)
  monkeypatch.setattr(main_module, "stop_sandbox", stop)
  monkeypatch.setattr(main_module, "launch_core", launch_core)
  monkeypatch.setattr(main_module, "launch_empty", launch_empty)
  return await main_module.run(
    SimpleNamespace(etc=str(etc_root)),
    config,
    layout=Layout.resolve(prefix=tmp_path, mode="source", environ={}),
  )


@pytest.mark.asyncio
async def test_neither_route_starts_ready_empty(
  monkeypatch, tmp_path, etc_root, launch_config
) -> None:
  async def core(*_args, **_kwargs):
    raise AssertionError("empty mode must not construct the core model pipeline")

  async def empty(config, **_kwargs):
    assert config.launch.backend is False
    return 11

  result = await _run(
    monkeypatch,
    tmp_path,
    etc_root,
    _switches(launch_config, backend=False, sandbox=False),
    core,
    empty,
  )
  assert result == 11


@pytest.mark.asyncio
@pytest.mark.parametrize(
  ("backend", "sandbox", "expected_alias"),
  [(False, True, "sandbox"), (True, False, "default")],
)
async def test_single_route_launches_directly(
  monkeypatch, tmp_path, etc_root, launch_config, backend, sandbox, expected_alias
) -> None:
  selected_aliases = []

  async def core(config, **_kwargs):
    selected_aliases.append(config.endpoints()[0].id)
    return 12

  async def empty(*_args, **_kwargs):
    raise AssertionError("a selected route is not empty mode")

  result = await _run(
    monkeypatch,
    tmp_path,
    etc_root,
    _switches(launch_config, backend=backend, sandbox=sandbox),
    core,
    empty,
  )
  assert result == 12
  assert selected_aliases == [expected_alias]


@pytest.mark.asyncio
async def test_real_fleet_winning_grace_period_skips_playground(
  monkeypatch, tmp_path, etc_root, launch_config
) -> None:
  monkeypatch.setattr(main_module, "_wait_for_real_backends", _always_ready)
  aliases = []

  async def core(config, **_kwargs):
    aliases.append(config.endpoints()[0].id)
    return 13

  result = await _run(
    monkeypatch,
    tmp_path,
    etc_root,
    _switches(launch_config, backend=True, sandbox=True),
    core,
    _unexpected_empty,
  )
  assert result == 13
  assert aliases == ["default"]


@pytest.mark.asyncio
async def test_fallback_promotes_once_and_carries_session_handoff(
  monkeypatch, tmp_path, etc_root, launch_config
) -> None:
  probe_results = iter((False, True))

  async def probe(*_args, **_kwargs):
    return next(probe_results)

  monkeypatch.setattr(main_module, "_wait_for_real_backends", probe)
  aliases = []
  revisions = []

  async def core(config, **kwargs):
    aliases.append(config.endpoints()[0].id)
    handoff = kwargs.get("session_handoff")
    if config.endpoints()[0].id == "sandbox":
      await kwargs["external_stop"].wait()
      handoff.capture([])
      return 0
    revisions.append(handoff.revision)
    return 14

  result = await _run(
    monkeypatch,
    tmp_path,
    etc_root,
    _switches(launch_config, backend=True, sandbox=True),
    core,
    _unexpected_empty,
  )
  assert result == 14
  assert aliases == ["sandbox", "default"]
  assert revisions == [1]


async def _always_ready(*_args, **_kwargs):
  return True


async def _unexpected_empty(*_args, **_kwargs):
  raise AssertionError("a selected route is not empty mode")
