"""Wrap-mode shutdown must clean up the entire child process group."""

from __future__ import annotations

import asyncio
import os
from pathlib import Path

import pytest

from egia_ext.tui.spawn import spawn_pipeline, stop_pipeline


def _is_running(pid: int) -> bool:
  try:
    os.kill(pid, 0)
  except ProcessLookupError:
    return False
  try:
    # ``kill(pid, 0)`` succeeds for a zombie until its new parent reaps it;
    # a zombie has no live gateway or listener, so treat it as stopped.
    state = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8").split(") ", 1)[1][0]
  except (FileNotFoundError, IndexError):
    return False
  return state != "Z"


@pytest.mark.asyncio
async def test_stop_pipeline_stops_grandchild_after_leader_exits(
  tmp_path: Path,
) -> None:
  """A stale extension cannot outlive a pipeline leader that has already died."""
  pid_file = tmp_path / "extension.pid"
  child_code = "import time; time.sleep(60)"
  leader_code = (
    "import pathlib, subprocess, sys; "
    f"child = subprocess.Popen([sys.executable, '-c', {child_code!r}]); "
    f"pathlib.Path({str(pid_file)!r}).write_text(str(child.pid)); "
    "raise SystemExit(0)"
  )
  pipeline = await spawn_pipeline(["-c", leader_code], cwd=tmp_path)
  try:
    for _ in range(100):
      if pid_file.is_file():
        break
      await asyncio.sleep(0.01)
    assert pid_file.is_file()
    extension_pid = int(pid_file.read_text(encoding="utf-8"))
    # The grandchild inherits the pipeline's stderr pipe. ``wait()`` waits
    # for that pipe to close as well, so observe the leader's reaped exit
    # code directly before asking the console shutdown path to kill the group.
    for _ in range(100):
      if pipeline.process.returncode is not None:
        break
      await asyncio.sleep(0.01)
    assert pipeline.process.returncode == 0
    assert _is_running(extension_pid)

    await stop_pipeline(pipeline)

    for _ in range(100):
      if not _is_running(extension_pid):
        break
      await asyncio.sleep(0.01)
    assert not _is_running(extension_pid)
  finally:
    await stop_pipeline(pipeline)
