"""Live process monitor for the console — /proc sampling, no dependencies.

Wrap mode owns the pipeline child; the child (or its launcher) spawns the
backend extension as its own subprocess. Every ~2s this loop samples the
whole tree (CPU% from utime+stime deltas, RSS from /proc/<pid>/status) into
``state.processes`` for the status pane. Linux-only by design: /proc is the
interface; other platforms get an absent list and the pane says so.
"""

from __future__ import annotations

import asyncio
import time

from egia_ext.tui.feeds import ConsoleState

SAMPLE_INTERVAL_S = 2.0
_CLK_TZ = 100.0  # /proc ticks per second; good enough for a pane readout


def _proc_stat(pid: int) -> tuple[str, int, float] | None:
  """(comm, ppid, cpu_ticks) from /proc/<pid>/stat, or None when gone."""
  try:
    with open(f"/proc/{pid}/stat", encoding="utf-8") as fh:
      text = fh.read()
  except OSError:
    return None
  # comm may contain spaces/parens; split around the last ')'.
  head, _, tail = text.rpartition(")")
  comm = head[head.rindex("(") + 1 :]
  fields = tail.split()
  ppid = int(fields[1])
  utime, stime = int(fields[11]), int(fields[12])
  return comm, ppid, (utime + stime) / _CLK_TZ


def _proc_rss_mb(pid: int) -> float:
  try:
    with open(f"/proc/{pid}/status", encoding="utf-8") as fh:
      for line in fh:
        if line.startswith("VmRSS:"):
          return int(line.split()[1]) / 1024.0
  except (OSError, ValueError, IndexError):
    pass
  return 0.0


def _descendants(root: int) -> list[int]:
  """PIDs in root's tree (root included), one /proc scan."""
  children: dict[int, list[int]] = {}
  try:
    import os

    for entry in os.scandir("/proc"):
      if not entry.name.isdigit():
        continue
      stat = _proc_stat(int(entry.name))
      if stat is not None:
        children.setdefault(stat[1], []).append(int(entry.name))
  except OSError:
    return [root]
  seen: list[int] = []
  frontier = [root]
  while frontier:
    pid = frontier.pop()
    if pid in seen:
      continue
    seen.append(pid)
    frontier.extend(children.get(pid, []))
  return seen


def sample_tree(
  root: int, previous: dict[int, float], elapsed_s: float
) -> tuple[list[dict], dict[int, float]]:
  """One sampling pass; ``previous`` maps pid → cpu seconds at the last pass."""
  samples: list[dict] = []
  now: dict[int, float] = {}
  for pid in _descendants(root):
    stat = _proc_stat(pid)
    if stat is None:
      continue
    comm, _, cpu_s = stat
    now[pid] = cpu_s
    delta = cpu_s - previous.get(pid, cpu_s)
    pct = 100.0 * delta / elapsed_s if elapsed_s > 0 else 0.0
    samples.append(
      {
        "pid": pid,
        "name": comm if pid != root else f"{comm} (pipeline)",
        "cpu_pct": max(pct, 0.0),
        "rss_mb": _proc_rss_mb(pid),
      }
    )
  samples.sort(key=lambda s: s["pid"] == root, reverse=True)
  return samples, now


async def process_monitor(state: ConsoleState, root_pid: int | None) -> None:
  """Periodically refresh ``state.processes`` from the child's process tree."""
  if root_pid is None:  # attach mode: no owned tree to watch
    state.processes = []
    return
  previous: dict[int, float] = {}
  last = time.monotonic()
  while True:
    await asyncio.sleep(SAMPLE_INTERVAL_S)
    now = time.monotonic()
    samples, previous = sample_tree(root_pid, previous, now - last)  # type: ignore[misc]
    last = now
    state.processes = samples
    state.dirty.set()
