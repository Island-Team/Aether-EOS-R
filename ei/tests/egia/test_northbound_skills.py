"""Northbound native-skill command and execution-watch routes."""

from __future__ import annotations

import asyncio
import json
from collections.abc import AsyncIterator

import pytest
import websockets

from egia.api.status import HealthState
from egia.core.link import LinkPrimitiveError, SkillBinding
from egia.core.link.client import SkillExecutionSnapshot
from egia.core.perception.admission import Admission
from egia.io import SKILLS_COMMAND_PATH, SKILLS_EXECUTION_PATH, NorthboundServer
from egia.io.state import ServerState


def _execution(sequence: int, status: str = "running") -> SkillExecutionSnapshot:
  return SkillExecutionSnapshot(
    execution_id="exec-1",
    request_id="request-1",
    name="approach",
    version="1.2.0",
    status=status,
    sequence=sequence,
    cancellation_requested=False,
    reason_code=None,
    message=None,
    result={"distance_m": 0.1} if status == "succeeded" else None,
  )


class FakeSkillClient:
  """Deterministic SkillService seam for WebSocket contract tests."""

  async def get_skill(self, name: str, version: str) -> SkillBinding:
    if name == "missing":
      raise LinkPrimitiveError(404, "not found")
    return SkillBinding(
      backend="robot-a",
      eos_robot_id="eos-1",
      name=name,
      version=version,
      description="Approach a visible target",
      primitives=("base.velocity",),
      parameters={"type": "object"},
      returns={"type": "object"},
    )

  async def start_skill_execution(
    self,
    name: str,
    version: str,
    arguments: dict[str, object],
    request_id: str,
    timeout_s: float,
  ) -> SkillExecutionSnapshot:
    assert (name, version, arguments, request_id, timeout_s) == (
      "approach",
      "1.2.0",
      {"distance_m": 1.5},
      "request-1",
      30.0,
    )
    return _execution(1, "queued")

  async def get_skill_execution(self, execution_id: str) -> SkillExecutionSnapshot:
    assert execution_id == "exec-1"
    return _execution(2)

  async def cancel_skill_execution(self, execution_id: str) -> SkillExecutionSnapshot:
    assert execution_id == "exec-1"
    return _execution(3, "cancelled")

  async def watch_skill_execution(
    self, execution_id: str, after_sequence: int
  ) -> AsyncIterator[SkillExecutionSnapshot]:
    assert execution_id == "exec-1"
    assert after_sequence == 1
    yield _execution(2)
    yield _execution(3, "succeeded")


async def _server() -> NorthboundServer:
  state = ServerState(version="test")
  state.status = HealthState.READY
  server = NorthboundServer(
    state,
    Admission(4),
    port=0,
    skill_clients={"robot-a": FakeSkillClient()},
  )
  await server.start()
  return server


@pytest.mark.asyncio
async def test_skill_command_start_requires_exact_bounded_request() -> None:
  server = await _server()
  try:
    _, port = server.address
    async with websockets.connect(
      f"ws://127.0.0.1:{port}{SKILLS_COMMAND_PATH}"
    ) as websocket:
      await websocket.send(
        json.dumps(
          {
            "operation": "start",
            "robot_id": "robot-a",
            "name": "approach",
            "version": "1.2.0",
            "request_id": "request-1",
            "arguments": {"distance_m": 1.5},
            "timeout_s": 30.0,
          }
        )
      )
      response = json.loads(await asyncio.wait_for(websocket.recv(), 2))
      assert response["ok"] is True
      assert response["execution"]["status"] == "queued"
      assert response["execution"]["robot_id"] == "robot-a"

      invalid = {
        "operation": "start",
        "robot_id": "robot-a",
        "name": "approach",
        "version": "1.2.0",
        "request_id": "request-2",
        "arguments": {},
        "timeout_s": 3601,
      }
      await websocket.send(json.dumps(invalid))
      assert json.loads(await websocket.recv())["error"] == "invalid_request"
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_skill_command_maps_lookup_and_robot_errors() -> None:
  server = await _server()
  try:
    _, port = server.address
    async with websockets.connect(
      f"ws://127.0.0.1:{port}{SKILLS_COMMAND_PATH}"
    ) as websocket:
      await websocket.send(
        json.dumps(
          {
            "operation": "get_skill",
            "robot_id": "robot-a",
            "name": "missing",
            "version": "1.0.0",
          }
        )
      )
      assert json.loads(await websocket.recv())["error"] == "skill_not_found"
      await websocket.send(
        json.dumps(
          {
            "operation": "get_execution",
            "robot_id": "robot-x",
            "execution_id": "exec-1",
          }
        )
      )
      assert json.loads(await websocket.recv())["error"] == "robot_not_found"
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_skill_execution_watch_resumes_after_sequence() -> None:
  server = await _server()
  try:
    _, port = server.address
    async with websockets.connect(
      f"ws://127.0.0.1:{port}{SKILLS_EXECUTION_PATH}"
    ) as websocket:
      await websocket.send(
        json.dumps(
          {
            "robot_id": "robot-a",
            "execution_id": "exec-1",
            "after_sequence": 1,
          }
        )
      )
      first = json.loads(await asyncio.wait_for(websocket.recv(), 2))
      terminal = json.loads(await asyncio.wait_for(websocket.recv(), 2))
      assert [first["sequence"], terminal["sequence"]] == [2, 3]
      assert terminal["status"] == "succeeded"
      assert terminal["result"] == {"distance_m": 0.1}
  finally:
    await server.stop()
