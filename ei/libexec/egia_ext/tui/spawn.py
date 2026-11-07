"""Child process management for wrap mode.

The TUI is the foreground process; EGIA runs as its child with stderr piped
into the log pane so colorized console output never corrupts curses. On quit
the child gets SIGINT (graceful pipeline shutdown), then SIGTERM after a
grace window, then SIGKILL.
"""

from __future__ import annotations

import asyncio
import contextlib
import os
import signal
import sys
from dataclasses import dataclass
from pathlib import Path

GRACE_S = 8.0
_GROUP_POLL_S = 0.05


@dataclass
class ChildProcess:
  """Running pipeline child owned by the console."""

  process: asyncio.subprocess.Process
  argv: list[str]

  @property
  def stderr(self):
    """Child stderr stream; ``None`` only in attach mode."""
    return self.process.stderr


async def spawn_pipeline(child_argv: list[str], *, cwd: Path) -> ChildProcess:
  """Launch the EGIA composition root with stderr captured to a pipe."""
  argv = [sys.executable, *child_argv]
  process = await asyncio.create_subprocess_exec(
    *argv,
    cwd=cwd,
    stdout=asyncio.subprocess.DEVNULL,
    stderr=asyncio.subprocess.PIPE,
    start_new_session=True,
  )
  return ChildProcess(process=process, argv=argv)


async def stop_pipeline(child: ChildProcess) -> int:
  """Stop every process launched by the console, with signal escalation.

  Signals go to the child's whole process group: the child is spawned with
  ``start_new_session=True`` (pgid == child pid) and its own extension
  subprocess shares that group. Signalling only the child pid leaves the
  extension orphaned — still bound to its gateway port — when the child
  dies without stopping it. The group is signalled even when the pipeline
  leader already exited, because its extension may still be alive in the
  group at that point.
  """
  process = child.process
  pgid = process.pid

  def _signal_group(sig: int) -> None:
    with contextlib.suppress(ProcessLookupError, PermissionError):
      os.killpg(pgid, sig)

  def _group_alive() -> bool:
    # On Linux, ``killpg(pgid, 0)`` treats a zombie as alive until its parent
    # reaps it. Inspect the group directly so an already-dead extension does
    # not make TUI shutdown wait through every escalation window.
    try:
      for entry in os.scandir("/proc"):
        if not entry.name.isdigit():
          continue
        try:
          text = Path(entry.path, "stat").read_text(encoding="utf-8")
          _, _, tail = text.rpartition(")")
          fields = tail.split()
          state, group = fields[0], int(fields[2])
        except (FileNotFoundError, IndexError, ValueError):
          continue
        if group == pgid and state != "Z":
          return True
      return False
    except OSError:
      # ``/proc`` is absent on non-Linux platforms. Fall back to the portable
      # process-group liveness probe there.
      pass
    try:
      os.killpg(pgid, 0)
    except ProcessLookupError:
      return False
    except PermissionError:
      # The console owns this group. Treat an unexpected permission failure
      # as alive so we never claim that the child tree was cleaned up.
      return True
    return True

  async def _wait_for_group_exit(timeout_s: float) -> bool:
    deadline = asyncio.get_running_loop().time() + timeout_s
    while _group_alive():
      if asyncio.get_running_loop().time() >= deadline:
        return False
      await asyncio.sleep(_GROUP_POLL_S)
    return True

  _signal_group(signal.SIGINT)
  if not await _wait_for_group_exit(GRACE_S):
    _signal_group(signal.SIGTERM)
    if not await _wait_for_group_exit(2.0):
      _signal_group(signal.SIGKILL)
      await _wait_for_group_exit(2.0)
  return await process.wait()
