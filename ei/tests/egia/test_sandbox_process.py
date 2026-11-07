"""Optional research sandbox process lifecycle."""

from __future__ import annotations

import asyncio
import signal
from argparse import Namespace

import pytest

import egia.main as main_module
from egia.main import start_sandbox, stop_sandbox, validate_managed_config
from island.path import Layout


@pytest.mark.asyncio
async def test_enabled_sandbox_starts_and_stops(stub_launch_config, tmp_path) -> None:
  config = stub_launch_config.model_copy(
    update={"launch": stub_launch_config.launch.model_copy(update={"sandbox": True})}
  )
  log_dir = tmp_path / "var/log/egia"
  process = await start_sandbox(config, log_dir)
  assert process is not None
  sandbox_log = log_dir / "opt/sandbox.log"
  for _ in range(100):
    if sandbox_log.is_file() and "sandbox_ready" in sandbox_log.read_text(
      encoding="utf-8"
    ):
      break
    await asyncio.sleep(0.01)
  else:
    pytest.fail("sandbox did not report readiness to its log")

  await stop_sandbox(process)

  # A shutdown racing the child's signal-handler installation may report the
  # terminating signal; either outcome proves the child was reaped.
  assert process.returncode in {0, -signal.SIGTERM}
  assert sandbox_log.is_file()
  assert "sandbox_ready" in sandbox_log.read_text(encoding="utf-8")


@pytest.mark.asyncio
async def test_disabled_sandbox_launches_no_process(
  stub_launch_config, tmp_path
) -> None:
  config = stub_launch_config.model_copy(
    update={"launch": stub_launch_config.launch.model_copy(update={"sandbox": False})}
  )
  assert await start_sandbox(config, tmp_path / "logs") is None
  await stop_sandbox(None)


def test_managed_mode_rejects_hidden_sandbox(stub_launch_config) -> None:
  configured = stub_launch_config.model_copy(
    update={"launch": stub_launch_config.launch.model_copy(update={"sandbox": True})}
  )

  with pytest.raises(ValueError, match="separate IsOS backend node"):
    validate_managed_config(configured, managed=True)
  validate_managed_config(configured, managed=False)


@pytest.mark.asyncio
async def test_agent_run_wraps_core_with_sandbox_lifecycle(
  stub_launch_config, tmp_path, etc_root, monkeypatch
) -> None:
  marker = object()
  events: list[object] = []

  async def fake_start(config, log_dir, **kwargs):
    events.append(("start", config.launch.sandbox, log_dir))
    return marker

  async def fake_launch(*args, **kwargs):
    events.append("core")
    return 7

  async def fake_stop(process):
    events.append(("stop", process))

  monkeypatch.setattr(main_module, "start_sandbox", fake_start)
  monkeypatch.setattr(main_module, "launch_core", fake_launch)
  monkeypatch.setattr(main_module, "stop_sandbox", fake_stop)
  config = stub_launch_config.model_copy(
    update={
      "launch": stub_launch_config.launch.model_copy(
        update={"memory": False, "sandbox": True}
      )
    }
  )
  layout = Layout.resolve(prefix=tmp_path, mode="source", environ={})

  result = await main_module.run(Namespace(etc=str(etc_root)), config, layout=layout)

  assert result == 7
  assert events == [
    ("start", True, layout.component_log("egia")),
    "core",
    ("stop", marker),
  ]


def test_bootstrap_exception_is_persisted(tmp_path) -> None:
  path = tmp_path / "var/log/egia/main.log"
  try:
    raise RuntimeError("bootstrap exploded")
  except RuntimeError:
    main_module._write_bootstrap_error(path)

  text = path.read_text(encoding="utf-8")
  assert "egia_bootstrap_failed" in text
  assert "RuntimeError: bootstrap exploded" in text
