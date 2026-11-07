"""Northbound review route — operator promotion of concluded plans (§3.3)."""

from __future__ import annotations

import asyncio
import json
from datetime import UTC, datetime

import pytest
import websockets

from egia.api.plan import ActionRecord, PlanRecord, StepOutcome, StepRecord
from egia.api.status import HealthState
from egia.core.memory import EpisodicStore, ReferenceStore
from egia.core.perception.admission import Admission
from egia.io import NorthboundServer
from egia.io.northbound import REVIEW_PATH
from egia.io.state import ServerState


def _concluded_plan(
  plan_id: str = "plan-1",
  order: str = "dock the robot",
  outcome: StepOutcome = StepOutcome.SUCCEEDED,
) -> PlanRecord:
  now = datetime.now(UTC)
  return PlanRecord(
    id=plan_id,
    mission_id="m-1",
    order=order,
    source="decision.proposer",
    started_at=now,
    finished_at=now,
    outcome=outcome,
    result="docked",
    steps=[
      StepRecord(
        index=1,
        outcome=StepOutcome.SUCCEEDED,
        actions=[
          ActionRecord(
            index=1,
            intent="drive to the dock",
            action="base.velocity",
            outcome=StepOutcome.SUCCEEDED,
          )
        ],
      ),
    ],
  )


async def _start_server(
  tmp_path,
  *,
  episodic: EpisodicStore | None = None,
  reference: ReferenceStore | None = None,
) -> tuple[NorthboundServer, EpisodicStore, ReferenceStore]:
  state = ServerState(version="test")
  state.status = HealthState.READY
  episodic = episodic or EpisodicStore(tmp_path / "episodic.db")
  reference = reference or ReferenceStore(tmp_path / "reference.db")
  server = NorthboundServer(
    state,
    Admission(8),
    host="127.0.0.1",
    port=0,
    status_frequency_hz=10,
    episodic=episodic,
    reference=reference,
  )
  await server.start()
  return server, episodic, reference


async def _review(ws, payload: dict) -> dict:
  await ws.send(json.dumps(payload))
  return json.loads(await asyncio.wait_for(ws.recv(), timeout=2))


@pytest.mark.asyncio
async def test_review_promotes_concluded_plan(tmp_path) -> None:
  server, episodic, reference = await _start_server(tmp_path)
  episodic.save_plan(_concluded_plan())
  _, port = server.address
  try:
    async with websockets.connect(f"ws://127.0.0.1:{port}{REVIEW_PATH}") as ws:
      ack = await _review(ws, {"plan_id": "plan-1", "correlation_id": "r-1"})
      assert ack["ok"] is True
      assert ack["plan_id"] == "plan-1"
      assert ack["outcome"] == "succeeded"
      assert ack["correlation_id"] == "r-1"
      assert ack["reviewed_at"]
      promoted = reference.get_plan("plan-1")
      assert promoted is not None
      assert promoted.reviewed_at is not None
  finally:
    await server.stop()
    episodic.close()
    reference.close()


@pytest.mark.asyncio
async def test_review_unknown_plan_errors(tmp_path) -> None:
  server, episodic, reference = await _start_server(tmp_path)
  _, port = server.address
  try:
    async with websockets.connect(f"ws://127.0.0.1:{port}{REVIEW_PATH}") as ws:
      err = await _review(ws, {"plan_id": "missing"})
      assert err["ok"] is False
      assert err["error"] == "plan_not_found"
      assert reference.get_plan("missing") is None
  finally:
    await server.stop()
    episodic.close()
    reference.close()


@pytest.mark.asyncio
async def test_review_invalid_outcome_errors(tmp_path) -> None:
  server, episodic, reference = await _start_server(tmp_path)
  episodic.save_plan(_concluded_plan())
  _, port = server.address
  try:
    async with websockets.connect(f"ws://127.0.0.1:{port}{REVIEW_PATH}") as ws:
      err = await _review(ws, {"plan_id": "plan-1", "outcome": "brilliant"})
      assert err["ok"] is False
      assert "invalid_request" in err["error"]
      assert reference.get_plan("plan-1") is None
  finally:
    await server.stop()
    episodic.close()
    reference.close()


@pytest.mark.asyncio
async def test_review_records_note(tmp_path) -> None:
  server, episodic, reference = await _start_server(tmp_path)
  episodic.save_plan(_concluded_plan())
  _, port = server.address
  try:
    async with websockets.connect(f"ws://127.0.0.1:{port}{REVIEW_PATH}") as ws:
      ack = await _review(
        ws, {"plan_id": "plan-1", "note": "docked cleanly on first try"}
      )
      assert ack["ok"] is True
      promoted = reference.get_plan("plan-1")
      assert promoted is not None
      assert promoted.review_note == "docked cleanly on first try"
  finally:
    await server.stop()
    episodic.close()
    reference.close()


@pytest.mark.asyncio
async def test_review_outcome_override_to_effective(tmp_path) -> None:
  server, episodic, reference = await _start_server(tmp_path)
  episodic.save_plan(_concluded_plan())
  _, port = server.address
  try:
    async with websockets.connect(f"ws://127.0.0.1:{port}{REVIEW_PATH}") as ws:
      ack = await _review(ws, {"plan_id": "plan-1", "outcome": "effective"})
      assert ack["ok"] is True
      assert ack["outcome"] == "effective"
      promoted = reference.get_plan("plan-1")
      assert promoted is not None
      assert promoted.outcome is StepOutcome.EFFECTIVE
  finally:
    await server.stop()
    episodic.close()
    reference.close()


@pytest.mark.asyncio
async def test_review_unavailable_before_readiness(tmp_path) -> None:
  state = ServerState(version="test")  # status stays STARTING
  episodic = EpisodicStore(tmp_path / "episodic.db")
  reference = ReferenceStore(tmp_path / "reference.db")
  server = NorthboundServer(
    state,
    Admission(8),
    host="127.0.0.1",
    port=0,
    episodic=episodic,
    reference=reference,
  )
  await server.start()
  _, port = server.address
  try:
    async with websockets.connect(f"ws://127.0.0.1:{port}{REVIEW_PATH}") as ws:
      err = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      assert err["ok"] is False
      assert err["error"] == "unavailable"
      with pytest.raises(websockets.exceptions.ConnectionClosed) as closed:
        await asyncio.wait_for(ws.recv(), timeout=2)
      assert closed.value.rcvd.code == 1013
  finally:
    await server.stop()
    episodic.close()
    reference.close()
