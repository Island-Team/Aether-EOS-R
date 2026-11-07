"""Structured mission document contract from northbound.md."""

from __future__ import annotations

import asyncio
from datetime import UTC, datetime

import pytest
from pydantic import ValidationError

from egia.api.plan import ActionRecord, PlanRecord, StepOutcome, StepRecord
from egia.core.missions import MissionProtectedError, MissionQueue
from egia.core.missions.document import (
  ActionState,
  MissionAction,
  MissionDocument,
  MissionStep,
  StepState,
)
from egia.lib.config import MissionTemplateConfig


def _document(*, revision: int = 1) -> MissionDocument:
  return MissionDocument(
    mission_id="m1",
    order="inspect the bay",
    priority=0.8,
    revision=revision,
    steps=[
      MissionStep(
        index=1,
        actions=[
          MissionAction(index=1, robot_id="robot-a", action="base.velocity"),
          MissionAction(index=2, robot_id="robot-b", action="vision.detect"),
        ],
      )
    ],
  )


def test_rejects_duplicate_robot_binding_in_one_step() -> None:
  with pytest.raises(ValidationError, match="robot_id may appear only once"):
    MissionStep(
      index=1,
      actions=[
        MissionAction(index=1, robot_id="robot-a", action="base.velocity"),
        MissionAction(index=2, robot_id="robot-a", action="vision.detect"),
      ],
    )


def test_completeness_is_terminal_action_fraction() -> None:
  document = _document()
  document.steps[0].actions[0].state = ActionState.DONE
  assert document.completeness == 0.5


@pytest.mark.asyncio
async def test_queue_records_revisions_and_protects_started_steps() -> None:
  queue = MissionQueue(2)
  record = await queue.submit("inspect the bay", source="test")
  assert record is not None
  initial = queue.document(record.id)
  assert initial is not None and initial.revision == 1

  proposed = MissionDocument(
    mission_id=record.id,
    order=record.order,
    priority=record.priority,
    revision=2,
    steps=[
      MissionStep(
        index=1,
        actions=[MissionAction(index=1, robot_id="robot-a", action="base.velocity")],
      )
    ],
  )
  queue.revise(proposed)
  assert [item.revision for item in queue.revisions(record.id)] == [1, 2]

  changed_order = proposed.model_copy(update={"revision": 3, "order": "replacement"})
  with pytest.raises(ValueError, match="order must be changed"):
    queue.revise(changed_order)

  started = queue._documents[record.id]
  started.steps[0].state = StepState.EXECUTING
  changed = proposed.model_copy(deep=True)
  changed.revision = 3
  changed.steps[0].actions[0].action = "speaker.play"
  with pytest.raises(ValueError, match="has started"):
    queue.revise(changed)


@pytest.mark.asyncio
async def test_queue_protects_required_plan_steps_before_they_start() -> None:
  queue = MissionQueue(2)
  record = await queue.submit(
    "model paraphrase",
    source="test",
    template=MissionTemplateConfig.model_validate(
      {
        "id": "fixed_outcomes",
        "version": 1,
        "description": "fixed outcomes",
        "mission": "complete fixed outcomes",
        "steps": [{"id": "fixed-outcome", "outcome": "Complete the outcome."}],
      }
    ),
  )
  assert record is not None
  current = queue.document(record.id)
  assert current is not None and current.plan is not None

  changed = current.model_copy(deep=True)
  changed.revision += 1
  assert changed.plan is not None
  changed.plan.steps[0].sentence = "Replace the outcome."
  with pytest.raises(ValueError, match="required plan step.*cannot be edited"):
    queue.revise(changed)
  with pytest.raises(MissionProtectedError) as protected:
    queue.update_order(record.id, "replace template", expected_revision=1)
  assert protected.value.code == "mission_protected"


@pytest.mark.asyncio
async def test_barriers_and_action_results_drive_completeness() -> None:
  queue = MissionQueue(2)
  record = await queue.submit("inspect the bay", source="test")
  assert record is not None
  document = MissionDocument(
    mission_id=record.id,
    order=record.order,
    revision=2,
    steps=[
      MissionStep(
        index=1,
        actions=[MissionAction(index=1, robot_id="robot-a", action="base.velocity")],
      ),
      MissionStep(
        index=2,
        actions=[MissionAction(index=1, robot_id="robot-a", action="vision.detect")],
      ),
    ],
  )
  queue.revise(document)
  with pytest.raises(ValueError, match="blocked"):
    queue.begin_step(record.id, 2)
  queue.begin_step(record.id, 1)
  current = queue.complete_action(record.id, 1, 1, result={"ok": True})
  assert current.steps[0].state is StepState.DONE
  assert current.completeness == 0.5
  assert queue.begin_step(record.id, 2).steps[1].state is StepState.EXECUTING


@pytest.mark.asyncio
async def test_execute_next_launches_barrier_actions_concurrently() -> None:
  queue = MissionQueue(2)
  record = await queue.submit("inspect the bay", source="test")
  assert record is not None
  queue.revise(
    MissionDocument(
      mission_id=record.id,
      order=record.order,
      revision=2,
      steps=[
        MissionStep(
          index=1,
          actions=[
            MissionAction(index=1, robot_id="robot-a", action="base.velocity"),
            MissionAction(index=2, robot_id="robot-b", action="vision.detect"),
          ],
        )
      ],
    )
  )
  active = 0
  maximum = 0

  async def execute(action):
    nonlocal active, maximum
    active += 1
    maximum = max(maximum, active)
    await asyncio.sleep(0.01)
    active -= 1
    return {"action": action.action}

  current = await queue.execute_next(record.id, execute)
  assert current is not None
  assert maximum == 2
  assert current.state.value == "succeeded"
  assert current.completeness == 1.0


@pytest.mark.asyncio
async def test_planner_record_syncs_into_revisioned_plan_tree() -> None:
  queue = MissionQueue(2)
  mission = await queue.submit("inspect the bay", source="test")
  assert mission is not None
  now = datetime.now(UTC)
  record = PlanRecord(
    id="plan-1",
    mission_id=mission.id,
    order=mission.order,
    source=mission.source,
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
            robot_id="camera",
            action="vision.detect",
            outcome=StepOutcome.SUCCEEDED,
          )
        ],
      )
    ],
  )
  document = queue.sync_plan(record, active=True)
  assert document.plan is not None
  assert document.plan.plan_id == "plan-1"
  assert document.plan.steps[0].actions[0].robot_id == "camera"
  assert document.plan.steps[0].id == "step-1"
  assert document.revision == 2
