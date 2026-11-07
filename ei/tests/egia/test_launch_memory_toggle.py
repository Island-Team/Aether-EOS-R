"""Launch-level ``[launch] memory = false`` master toggle (§3.2).

The first real ``core.launch`` test: with the master switch off — and the
proposer off too, so no model call can ever fire — the pipeline still comes
up READY. The health stream lists no ``memory`` subsystem (and no
``proposer``) and the durable state's ``memory/`` is never created: no SQLite files. The
stub gateway (``stub_gateway`` fixture) serves the backend side, so every
other subsystem runs exactly as it does in production; shutdown goes through
the real SIGINT handler the launcher installs.
"""

from __future__ import annotations

import asyncio
import json
import os
import signal
import time

import pytest
import websockets

from egia.core import launch


async def _wait_ready(host: str, port: int, *, timeout: float = 15.0) -> dict:
  """Poll the northbound health stream until the pipeline reports READY.

  Reconnects on startup rejects (the health route answers ``unavailable``
  and closes with 1013 before the supervisor is wired) and collects every
  observed status, so a timeout fails with the evidence rather than a bare
  assertion.
  """
  url = f"ws://{host}:{port}/api/aether/egia/health"
  deadline = time.monotonic() + timeout
  observed: list[str] = []
  while time.monotonic() < deadline:
    try:
      async with websockets.connect(url, open_timeout=2) as ws:
        while time.monotonic() < deadline:
          payload = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
          observed.append(str(payload.get("status") or payload.get("error")))
          if payload.get("status") == "ready":
            return payload
    except (OSError, TimeoutError, websockets.ConnectionClosed):
      await asyncio.sleep(0.05)
  raise AssertionError(
    f"pipeline never became ready within {timeout}s; observed: {observed}"
  )


@pytest.mark.asyncio
async def test_launch_memory_disabled_runs_without_memory(
  stub_launch_config, tmp_path, etc_root
) -> None:
  """``[launch] memory = false``: no memory subsystem, no store files.

  The health payload's subsystem list must omit both ``memory`` and
  ``proposer``, and ``state/memory`` must never exist on disk. SIGINT (the
  handler the launcher installs) stops the pipeline gracefully.
  """
  config = stub_launch_config.model_copy(
    update={
      "launch": stub_launch_config.launch.model_copy(update={"memory": False}),
      "decision": stub_launch_config.decision.model_copy(
        update={
          "proposer": stub_launch_config.decision.proposer.model_copy(
            update={"enabled": False}
          )
        }
      ),
    }
  )
  state_dir = tmp_path / "var/lib/egia"
  task = asyncio.create_task(launch(config, state_dir=state_dir, etc=str(etc_root)))
  try:
    payload = await _wait_ready(config.server.host, config.server.port)
    names = [item["name"] for item in payload["subsystems"]]
    assert "memory" not in names
    assert "proposer" not in names
    assert not (state_dir / "memory").exists()
  finally:
    if not task.done():
      os.kill(os.getpid(), signal.SIGINT)
    await asyncio.wait_for(task, timeout=15)
