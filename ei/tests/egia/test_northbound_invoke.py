"""Operator primitive invocation through the northbound safety gate."""

from __future__ import annotations

import asyncio
import json

import pytest
import websockets

from egia.api.status import HealthState
from egia.contracts.actions import PrimitiveAck
from egia.contracts.actions.ack import TerminalStatus
from egia.core.missions import MissionQueue
from egia.core.monitor import EstopLatch
from egia.core.perception.admission import Admission
from egia.core.registry import PrimitiveRegistry
from egia.core.safety import SafetyGate
from egia.io import PRIMITIVE_INVOKE_PATH, NorthboundServer
from egia.io.state import ServerState


class FakeLink:
  def __init__(
    self,
    *,
    error: Exception | None = None,
    status: TerminalStatus = TerminalStatus.SUCCEEDED,
  ) -> None:
    self.error = error
    self.status = status
    self.calls: list[tuple[str, str, dict]] = []

  async def call_primitive(
    self, name: str, device_id: str, arguments: dict
  ) -> PrimitiveAck:
    self.calls.append((name, device_id, arguments))
    if self.error is not None:
      raise self.error
    return PrimitiveAck(
      primitive_version="1.0.0",
      primitive=name,
      accepted=True,
      command_id=f"command-{len(self.calls)}",
      sequence=len(self.calls),
      result={"status": self.status.value, "target": device_id},
    )


class FakeLinks:
  def __init__(self, link: FakeLink) -> None:
    self.link = link

  def client(self, backend: str | None = None) -> FakeLink:
    assert backend == "robot-a"
    return self.link


def _gate(link: FakeLink) -> SafetyGate:
  registry = PrimitiveRegistry.from_contracts(["robot-a"])
  return SafetyGate(
    registry,
    FakeLinks(link),  # type: ignore[arg-type]
    EstopLatch(link),  # type: ignore[arg-type]
    MissionQueue(4),
    Admission(8),
  )


async def _start_server(
  *,
  ready: bool = True,
  terminal_status: TerminalStatus = TerminalStatus.SUCCEEDED,
) -> tuple[NorthboundServer, FakeLink]:
  state = ServerState(version="test")
  if ready:
    state.status = HealthState.READY
  link = FakeLink(status=terminal_status)
  server = NorthboundServer(
    state,
    Admission(8),
    host="127.0.0.1",
    port=0,
    status_frequency_hz=10,
    gate=_gate(link),
  )
  await server.start()
  return server, link


async def _invoke(ws, payload: dict) -> dict:
  await ws.send(json.dumps(payload))
  return json.loads(await asyncio.wait_for(ws.recv(), timeout=2))


@pytest.mark.asyncio
async def test_invoke_executes_explicit_target_through_gate() -> None:
  server, link = await _start_server()
  _, port = server.address
  try:
    async with websockets.connect(
      f"ws://127.0.0.1:{port}{PRIMITIVE_INVOKE_PATH}"
    ) as ws:
      ack = await _invoke(
        ws,
        {
          "primitive": "base.velocity",
          "robot_id": "robot-a",
          "device_id": "base-0",
          "arguments": {
            "linear_x_m_s": 0.1,
            "linear_y_m_s": 0.0,
            "angular_z_rad_s": 0.0,
            "valid_for_ms": 250,
          },
          "correlation_id": "c-1",
        },
      )
      assert ack == {
        "ok": True,
        "accepted": True,
        "command_id": "command-1",
        "status": "succeeded",
        "sequence": 1,
        "result": {"status": "succeeded", "target": "base-0"},
        "reason": None,
        "reason_code": None,
        "correlation_id": "c-1",
      }
      assert link.calls[0][:2] == ("base.velocity", "base-0")
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_invoke_rejects_unknown_primitive_without_egress() -> None:
  server, link = await _start_server()
  _, port = server.address
  try:
    async with websockets.connect(
      f"ws://127.0.0.1:{port}{PRIMITIVE_INVOKE_PATH}"
    ) as ws:
      error = await _invoke(
        ws,
        {
          "primitive": "nope.move",
          "robot_id": "robot-a",
          "device_id": "nope-0",
        },
      )
      assert error["ok"] is False
      assert error["error"] == "rejected_by_gate"
      assert error["reason"] == "unknown_primitive"
      assert link.calls == []
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_invoke_exposes_non_success_terminal_status() -> None:
  server, _link = await _start_server(terminal_status=TerminalStatus.FAILED)
  _, port = server.address
  try:
    async with websockets.connect(
      f"ws://127.0.0.1:{port}{PRIMITIVE_INVOKE_PATH}"
    ) as ws:
      ack = await _invoke(
        ws,
        {
          "primitive": "base.velocity",
          "robot_id": "robot-a",
          "device_id": "base-0",
          "arguments": {
            "linear_x_m_s": 0.1,
            "linear_y_m_s": 0.0,
            "angular_z_rad_s": 0.0,
            "valid_for_ms": 250,
          },
        },
      )
      assert ack["accepted"] is True
      assert ack["status"] == "failed"
      assert ack["result"]["status"] == "failed"
      assert ack["command_id"] == "command-1"
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_invoke_validates_request_and_json() -> None:
  server, _link = await _start_server()
  _, port = server.address
  try:
    async with websockets.connect(
      f"ws://127.0.0.1:{port}{PRIMITIVE_INVOKE_PATH}"
    ) as ws:
      await ws.send("not json")
      error = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      assert error["error"] == "invalid_json"
      missing_target = await _invoke(
        ws,
        {"primitive": "base.velocity", "robot_id": "robot-a"},
      )
      assert missing_target["error"].startswith("invalid_request:")
      assert "device_id" in missing_target["error"]
      reserved_command = await _invoke(
        ws,
        {
          "primitive": "base.velocity",
          "robot_id": "robot-a",
          "device_id": "base-0",
          "arguments": {"command_id": "caller-owned"},
        },
      )
      assert reserved_command["error"].startswith("invalid_request:")
      assert "arguments.command_id" in reserved_command["error"]
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_invoke_unavailable_before_readiness() -> None:
  server, _link = await _start_server(ready=False)
  _, port = server.address
  try:
    async with websockets.connect(
      f"ws://127.0.0.1:{port}{PRIMITIVE_INVOKE_PATH}"
    ) as ws:
      error = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      assert error == {
        "ok": False,
        "error": "unavailable",
        "reason": None,
        "correlation_id": None,
      }
      with pytest.raises(websockets.exceptions.ConnectionClosed) as closed:
        await asyncio.wait_for(ws.recv(), timeout=2)
      assert closed.value.rcvd is not None
      assert closed.value.rcvd.code == 1013
  finally:
    await server.stop()
