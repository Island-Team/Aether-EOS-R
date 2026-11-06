"""Intentional empty launch remains ready and never binds a model."""

from __future__ import annotations

import asyncio
import json

import pytest
import websockets

import egia.core as core_module
from egia.io.northbound import (
  HEALTH_PATH,
  PRIMITIVES_PATH,
  PROMPTS_PATH,
  SKILLS_PATH,
)


async def _frame(port: int, path: str) -> dict:
  async with websockets.connect(f"ws://127.0.0.1:{port}{path}") as websocket:
    return json.loads(await asyncio.wait_for(websocket.recv(), timeout=2))


@pytest.mark.asyncio
async def test_empty_launch_is_ready_with_no_primitives_skills_or_model_requests(
  launch_config, tmp_path, unused_tcp_port, monkeypatch
) -> None:
  config = launch_config.model_copy(
    update={
      "launch": launch_config.launch.model_copy(
        update={"backend": False, "sandbox": False, "memory": False}
      ),
      "server": launch_config.server.model_copy(update={"port": unused_tcp_port}),
    }
  )

  def model_must_not_load(*_args, **_kwargs):
    raise AssertionError("empty launch loaded a model binding")

  monkeypatch.setattr(core_module, "VLMModel", model_must_not_load)
  stop = asyncio.Event()
  task = asyncio.create_task(
    core_module.launch_empty(config, state_dir=tmp_path, external_stop=stop)
  )
  try:
    for _ in range(50):
      try:
        health = await _frame(unused_tcp_port, HEALTH_PATH)
      except OSError:
        await asyncio.sleep(0.01)
        continue
      if health.get("status") == "ready":
        break
    else:
      raise AssertionError("empty launch did not become ready")
    assert await _frame(unused_tcp_port, PRIMITIVES_PATH) == {
      "primitives": [],
      "count": 0,
    }
    assert await _frame(unused_tcp_port, SKILLS_PATH) == {
      "schema_version": 2,
      "available": False,
      "skills": [],
      "unavailable_backends": [],
      "count": 0,
    }
    prompts = await _frame(unused_tcp_port, PROMPTS_PATH)
    assert "No backend or sandbox is enabled" in prompts["system"]
  finally:
    stop.set()
    await task
