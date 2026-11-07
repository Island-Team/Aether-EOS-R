"""Backend-driven model request cancellation and pause behavior."""

from __future__ import annotations

import asyncio

import pytest

from egia.core.decision.model import ModelAvailabilityGate
from egia.lib.vlm import ModelResponse


@pytest.mark.asyncio
async def test_gate_cancels_active_completion_and_retries_after_recovery() -> None:
  gate = ModelAvailabilityGate()
  started = asyncio.Event()
  cancelled = asyncio.Event()
  attempts = 0

  async def completion():
    nonlocal attempts
    attempts += 1
    if attempts == 1:
      started.set()
      try:
        await asyncio.Future()
      finally:
        cancelled.set()
    return ModelResponse(text="recovered")

  task = asyncio.create_task(gate.run(completion))
  await started.wait()
  gate.set_available(False)
  await asyncio.wait_for(cancelled.wait(), timeout=1)
  await asyncio.sleep(0)
  assert not task.done()
  assert attempts == 1
  gate.set_available(True)
  assert (await asyncio.wait_for(task, timeout=1)).text == "recovered"
  assert attempts == 2
