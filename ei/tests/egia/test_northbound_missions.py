"""Northbound mission-state projection and operator mission commands."""

from __future__ import annotations

import asyncio
import json
from datetime import UTC, datetime

import pytest
import websockets

from egia.api.mission import MissionPriorityBand, MissionState
from egia.api.plan import (
  ActionRecord,
  PlanAnnotation,
  PlanRecord,
  PlanStep,
  PlanStepStatus,
  StepOutcome,
  StepRecord,
)
from egia.api.status import HealthState, SafetyStatus
from egia.core.missions import MissionQueue
from egia.core.missions.document import MissionAction, MissionDocument, MissionStep
from egia.core.missions.store import MissionStore, MissionStoreError
from egia.core.perception.admission import Admission
from egia.io import NorthboundServer
from egia.io.northbound import (
  HEALTH_PATH,
  MISSIONS_COMMAND_PATH,
  MISSIONS_STREAM_PATH,
)
from egia.io.state import ServerState
from egia.lib.config import MissionTemplateConfig, MissionTemplateStepConfig


async def _start_server(
  *, mission_capacity: int = 2, current_plan=None, missions: MissionQueue | None = None
) -> tuple[NorthboundServer, MissionQueue]:
  state = ServerState(version="test")
  state.status = HealthState.READY
  missions = missions or MissionQueue(mission_capacity)
  server = NorthboundServer(
    state,
    Admission(8),
    host="127.0.0.1",
    port=0,
    status_frequency_hz=10,
    missions=missions,
    current_plan=current_plan,
  )
  await server.start()
  return server, missions


async def _command(ws, payload: dict) -> dict:
  await ws.send(json.dumps(payload))
  return json.loads(await asyncio.wait_for(ws.recv(), timeout=2))


class _FailingMissionStore(MissionStore):
  """Inject a durable write failure after initial mission admission."""

  fail = False

  def save_mission(self, mission, document, revisions):
    if self.fail:
      raise MissionStoreError("injected write failure")
    return super().save_mission(mission, document, revisions)


def _template() -> MissionTemplateConfig:
  return MissionTemplateConfig(
    id="home_living_2026",
    version=1,
    description="Test protected mission",
    mission="patrol the bay",
    steps=[MissionTemplateStepConfig(id="inspect", outcome="Verify the bay is clear.")],
  )


def _running_plan() -> PlanRecord:
  now = datetime.now(UTC)
  return PlanRecord(
    id="plan-live",
    mission_id="m-running",
    order="patrol the bay",
    source="decision.proposer",
    started_at=now,
    finished_at=now,
    outcome=StepOutcome.ABORTED,
    steps=[
      StepRecord(
        index=1,
        outcome=StepOutcome.SUCCEEDED,
        actions=[
          ActionRecord(
            index=1,
            robot_id="robot-a",
            intent="drive to the bay entrance",
            action="base.velocity",
            arguments={"target": "bay entrance"},
            outcome=StepOutcome.SUCCEEDED,
          )
        ],
      ),
      StepRecord(
        index=2,
        outcome=StepOutcome.FAILED,
        reason="rejected by gate (rate_limited)",
        actions=[
          ActionRecord(
            index=1,
            intent="scan the bay",
            action="vision.detect",
            outcome=StepOutcome.FAILED,
            reason="rejected by gate (rate_limited)",
          )
        ],
      ),
    ],
    annotations=[PlanAnnotation(source="monitor", event="step_stall")],
  )


@pytest.mark.asyncio
async def test_status_includes_safety_counters() -> None:
  state = ServerState(version="test")
  state.status = HealthState.READY
  server = NorthboundServer(
    state,
    Admission(8),
    host="127.0.0.1",
    port=0,
    status_frequency_hz=10,
    safety=lambda: SafetyStatus(admitted=7, rejected=2, estop_engaged=True),
  )
  await server.start()
  _, port = server.address
  try:
    async with websockets.connect(f"ws://127.0.0.1:{port}{HEALTH_PATH}") as ws:
      frame = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      assert frame["safety"] == {
        "admitted": 7,
        "rejected": 2,
        "estop_engaged": True,
      }
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_status_safety_absent_without_gate() -> None:
  state = ServerState(version="test")
  state.status = HealthState.READY
  server = NorthboundServer(
    state,
    Admission(8),
    host="127.0.0.1",
    port=0,
    status_frequency_hz=10,
  )
  await server.start()
  _, port = server.address
  try:
    async with websockets.connect(f"ws://127.0.0.1:{port}{HEALTH_PATH}") as ws:
      frame = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      assert frame["safety"] is None
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_missions_stream_snapshots_and_updates() -> None:
  """The live table stream: snapshot on connect, a frame per mutation."""
  server, missions = await _start_server()
  _, port = server.address
  try:
    async with websockets.connect(f"ws://127.0.0.1:{port}{MISSIONS_STREAM_PATH}") as ws:
      first = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      assert first["missions"] == []
      assert first["history"] == []
      assert first["plan"] is None

      record = await missions.submit("patrol the bay", source="decision.proposer")
      frame = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      assert [m["mission_id"] for m in frame["missions"]] == [record.id]
      entry = frame["missions"][0]
      assert entry["order"] == "patrol the bay"
      assert entry["state"] == "pending"
      assert entry["origin"] == "operator"
      assert entry["class_rank"] == 0
      assert [(event["state"], event["order"]) for event in frame["history"]] == [
        ("pending", "patrol the bay")
      ]

      missions.mark_running(record)
      frame = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      assert frame["missions"][0]["state"] == "running"
      assert [event["state"] for event in frame["history"]] == ["running"]

      missions.finish(record, state=MissionState.SUCCEEDED, result="done")
      frame = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      assert frame["missions"][0]["state"] == "succeeded"
      assert frame["missions"][0]["result"] == "done"
      assert [event["state"] for event in frame["history"]] == ["succeeded"]
      assert frame["history"][-1]["detail"] == "done"
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_mission_history_projects_document_steps_and_actions() -> None:
  server, missions = await _start_server()
  try:
    record = await missions.submit("patrol the bay", source="decision.proposer")
    assert record is not None
    missions.revise(
      MissionDocument(
        mission_id=record.id,
        order=record.order,
        priority=record.priority,
        revision=2,
        steps=[
          MissionStep(
            index=1,
            actions=[
              MissionAction(
                index=1,
                robot_id="robot-a",
                action="base.velocity",
                intent="reach the bay",
              )
            ],
          )
        ],
      )
    )
    missions.begin_step(record.id, 1)
    missions.complete_action(record.id, 1, 1, result="arrived")
    missions.finish(record, state=MissionState.SUCCEEDED, result="done")

    history = server._mission_snapshot().history
    assert len(history) == 1
    assert history[0].state is MissionState.SUCCEEDED
    assert history[0].revision == 2
    assert history[0].completeness == 1.0
    assert history[0].steps[0].state == "done"
    assert history[0].steps[0].actions[0].action == "base.velocity"
    assert history[0].steps[0].actions[0].arguments == {}
    assert history[0].steps[0].actions[0].result == "arrived"
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_mission_commands_create_mutate_and_stream_updates() -> None:
  server, missions = await _start_server()
  _, port = server.address
  try:
    async with (
      websockets.connect(f"ws://127.0.0.1:{port}{MISSIONS_COMMAND_PATH}") as command,
      websockets.connect(f"ws://127.0.0.1:{port}{MISSIONS_STREAM_PATH}") as stream,
    ):
      await asyncio.wait_for(stream.recv(), timeout=2)
      created = await _command(
        command,
        {
          "operation": "create",
          "order": "  inspect the bay  ",
          "priority_band": "high",
          "correlation_id": "ui-42",
        },
      )
      mission_id = created["mission"]["mission_id"]
      assert created == {
        "ok": True,
        "operation": "create",
        "correlation_id": "ui-42",
        "mission": {
          "mission_id": mission_id,
          "order": "inspect the bay",
          "state": "pending",
          "priority_band": "high",
          "revision": 1,
        },
      }
      admitted = missions.get(mission_id)
      assert admitted is not None
      assert admitted.source == "webui.missions.command"
      assert admitted.origin == "operator"
      frame = json.loads(await asyncio.wait_for(stream.recv(), timeout=2))
      assert frame["missions"][0]["priority_band"] == "high"

      updated = await _command(
        command,
        {
          "operation": "update",
          "mission_id": mission_id,
          "expected_revision": 1,
          "order": "inspect the loading bay",
        },
      )
      assert updated["mission"]["order"] == "inspect the loading bay"
      assert updated["mission"]["revision"] == 2
      await asyncio.wait_for(stream.recv(), timeout=2)

      prioritized = await _command(
        command,
        {
          "operation": "priority",
          "mission_id": mission_id,
          "expected_revision": 2,
          "priority_band": "urgent",
        },
      )
      assert prioritized["mission"]["priority_band"] == "urgent"
      assert prioritized["mission"]["revision"] == 3
      priority_frame = json.loads(await asyncio.wait_for(stream.recv(), timeout=2))
      assert priority_frame["history"][0]["priority_band"] == "urgent"

      cancelled = await _command(
        command,
        {
          "operation": "cancel",
          "mission_id": mission_id,
          "expected_revision": 3,
        },
      )
      assert cancelled["mission"]["state"] == "cancelled"
      assert cancelled["mission"]["revision"] == 4
      cancelled_frame = json.loads(await asyncio.wait_for(stream.recv(), timeout=2))
      assert cancelled_frame["missions"][0]["state"] == "cancelled"
      assert cancelled_frame["missions"][0]["priority_band"] == "urgent"
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_mission_command_maps_validation_capacity_and_mutation_errors() -> None:
  server, missions = await _start_server(mission_capacity=1)
  _, port = server.address
  try:
    async with websockets.connect(
      f"ws://127.0.0.1:{port}{MISSIONS_COMMAND_PATH}"
    ) as ws:
      await ws.send("{")
      assert json.loads(await ws.recv())["error"] == "invalid_json"

      invalid = await _command(
        ws,
        {
          "operation": "create",
          "order": "inspect",
          "source": "untrusted",
          "correlation_id": "bad-1",
        },
      )
      assert invalid["error"] == "invalid_request"
      assert invalid["operation"] == "create"
      assert invalid["correlation_id"] == "bad-1"

      created = await _command(ws, {"operation": "create", "order": "first"})
      mission_id = created["mission"]["mission_id"]
      full = await _command(ws, {"operation": "create", "order": "second"})
      assert full["error"] == "queue_full"

      stale = await _command(
        ws,
        {
          "operation": "priority",
          "mission_id": mission_id,
          "expected_revision": 99,
          "priority_band": "low",
          "correlation_id": "stale-1",
        },
      )
      assert stale["error"] == "mission_revision_conflict"
      assert stale["correlation_id"] == "stale-1"

      missing = await _command(
        ws,
        {
          "operation": "cancel",
          "mission_id": "00000000-0000-0000-0000-000000000000",
          "expected_revision": 1,
        },
      )
      assert missing["error"] == "mission_not_found"

      record = await missions.wait_pending(asyncio.Event())
      assert record is not None
      missions.mark_running(record)
      not_pending = await _command(
        ws,
        {
          "operation": "cancel",
          "mission_id": mission_id,
          "expected_revision": 1,
        },
      )
      assert not_pending["error"] == "mission_not_pending"
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_mission_command_rejects_protected_order_update() -> None:
  missions = MissionQueue(1)
  record = await missions.submit("ignored", source="config", template=_template())
  assert record is not None
  server, _ = await _start_server(missions=missions)
  _, port = server.address
  try:
    async with websockets.connect(
      f"ws://127.0.0.1:{port}{MISSIONS_COMMAND_PATH}"
    ) as ws:
      protected = await _command(
        ws,
        {
          "operation": "update",
          "mission_id": record.id,
          "expected_revision": 1,
          "order": "replace protected work",
        },
      )
      assert protected["error"] == "mission_protected"
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_mission_command_store_failure_does_not_publish_mutation(
  tmp_path,
) -> None:
  store = _FailingMissionStore(tmp_path / "missions")
  missions = MissionQueue(1, store=store)
  record = await missions.submit("unchanged", source="test")
  assert record is not None
  store.fail = True
  server, _ = await _start_server(missions=missions)
  _, port = server.address
  try:
    async with websockets.connect(
      f"ws://127.0.0.1:{port}{MISSIONS_COMMAND_PATH}"
    ) as ws:
      failed = await _command(
        ws,
        {
          "operation": "priority",
          "mission_id": record.id,
          "expected_revision": 1,
          "priority_band": "urgent",
        },
      )
      assert failed["error"] == "storage_failure"
      current = missions.get(record.id)
      document = missions.document(record.id)
      assert current is not None and document is not None
      assert current.priority_band is MissionPriorityBand.NORMAL
      assert document.revision == 1
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_missions_stream_embeds_running_action_list() -> None:
  """The running mission's plan streams as the operator action list."""
  box: dict[str, str] = {}

  def current_plan():
    plan = _running_plan()
    plan.mission_id = box.get("running_id", "m-running")
    plan.plan = [
      PlanStep(
        id="step-1",
        sentence="Verify the bay is clear.",
        status=PlanStepStatus.VERIFYING,
        required=True,
      )
    ]
    return plan

  server, missions = await _start_server(current_plan=current_plan)
  _, port = server.address
  try:
    async with websockets.connect(f"ws://127.0.0.1:{port}{MISSIONS_STREAM_PATH}") as ws:
      await asyncio.wait_for(ws.recv(), timeout=2)  # initial snapshot
      record = await missions.submit(
        "patrol the bay", source="decision.proposer", template=_template()
      )
      assert record is not None
      await asyncio.wait_for(ws.recv(), timeout=2)  # pending frame
      box["running_id"] = record.id
      missions.mark_running(record)
      frame = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      plan = frame["plan"]
      assert plan is not None
      assert plan["plan_id"] == "plan-live"
      assert plan["mission_id"] == record.id
      assert plan["completeness"] == 0.0
      assert plan["plan"][0]["status"] == "verifying"
      assert frame["missions"][0]["mission_template_id"] == "home_living_2026"
      assert frame["missions"][0]["mission_template_version"] == 1
      assert plan["steps"][0]["actions"][0]["action"] == "base.velocity"
      assert plan["steps"][0]["actions"][0]["arguments"] == {"target": "bay entrance"}
      assert plan["steps"][1]["outcome"] == "failed"
      assert plan["annotations"] == 1
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_mission_history_uses_planner_synced_steps_and_parameters() -> None:
  server, missions = await _start_server()
  try:
    record = await missions.submit("patrol the bay", source="decision.proposer")
    assert record is not None
    missions.mark_running(record)
    plan = _running_plan()
    plan.mission_id = record.id
    plan.plan = [
      PlanStep(
        id="step-1",
        sentence="Verify the bay is clear.",
        status=PlanStepStatus.VERIFYING,
        required=True,
      )
    ]
    missions.sync_plan(plan, active=True)
    missions.finish(record, state=MissionState.SUCCEEDED, result="done")

    history = server._mission_snapshot().history
    assert history[0].steps[0].actions[0].action == "base.velocity"
    assert history[0].steps[0].actions[0].arguments == {"target": "bay entrance"}
    assert history[0].document is not None
    assert history[0].document.plan is not None
    assert history[0].document.plan.completeness == 0.0
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_missions_stream_gates_on_readiness() -> None:
  state = ServerState(version="test")  # status stays STARTING
  server = NorthboundServer(
    state,
    Admission(8),
    host="127.0.0.1",
    port=0,
    missions=MissionQueue(2),
  )
  await server.start()
  _, port = server.address
  try:
    async with websockets.connect(f"ws://127.0.0.1:{port}{MISSIONS_STREAM_PATH}") as ws:
      err = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      assert err["error"] == "unavailable"
      with pytest.raises(websockets.exceptions.ConnectionClosed) as closed:
        await asyncio.wait_for(ws.recv(), timeout=2)
      assert closed.value.rcvd.code == 1013
  finally:
    await server.stop()


@pytest.mark.asyncio
async def test_missions_command_gates_on_readiness() -> None:
  state = ServerState(version="test")
  server = NorthboundServer(
    state,
    Admission(8),
    host="127.0.0.1",
    port=0,
    missions=MissionQueue(2),
  )
  await server.start()
  _, port = server.address
  try:
    async with websockets.connect(
      f"ws://127.0.0.1:{port}{MISSIONS_COMMAND_PATH}"
    ) as ws:
      error = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
      assert error == {
        "ok": False,
        "operation": None,
        "correlation_id": None,
        "error": "unavailable",
        "reason": None,
      }
      with pytest.raises(websockets.exceptions.ConnectionClosed) as closed:
        await asyncio.wait_for(ws.recv(), timeout=2)
      assert closed.value.rcvd.code == 1013
  finally:
    await server.stop()
