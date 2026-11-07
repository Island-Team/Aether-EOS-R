"""Editable mission documents owned by :mod:`egia.core.missions`.

The northbound contract describes the planner's durable unit as an ordered
list of barrier steps.  This module keeps that representation independent of
the model loop: it validates revisions, protects work that has already
started, and derives operator-facing state and completeness.
"""

from __future__ import annotations

from datetime import UTC, datetime
from enum import StrEnum
from typing import Any

from pydantic import BaseModel, Field, model_validator

from egia.api.mission import MissionPriorityBand, MissionState

__all__ = [
  "ActionState",
  "MissionAction",
  "MissionDocument",
  "MissionPlan",
  "MissionRevision",
  "MissionStep",
  "PlanState",
  "StepState",
  "validate_edit",
]


class ActionState(StrEnum):
  PENDING = "pending"
  EXECUTING = "executing"
  DONE = "done"
  FAILED = "failed"


class StepState(StrEnum):
  PENDING = "pending"
  EXECUTING = "executing"
  VERIFYING = "verifying"
  DONE = "done"
  FAILED = "failed"


class PlanState(StrEnum):
  DRAFT = "draft"
  RUNNING = "running"
  SUCCEEDED = "succeeded"
  FAILED = "failed"
  STOPPED = "stopped"


class MissionAction(BaseModel):
  index: int = Field(ge=1)
  robot_id: str = Field(min_length=1)
  device_id: str | None = None
  action: str = Field(min_length=1)
  command: str = ""
  arguments: dict[str, Any] = Field(default_factory=dict)
  intent: str = ""
  state: ActionState = ActionState.PENDING
  result: Any | None = None
  reason: str | None = None


class MissionStep(BaseModel):
  index: int = Field(ge=1)
  id: str | None = None
  sentence: str = ""
  state: StepState = StepState.PENDING
  result: Any | None = None
  reason: str | None = None
  required: bool = False
  after: list[str] = Field(default_factory=list)
  terminal: bool = False
  evidence: str | None = None
  actions: list[MissionAction] = Field(default_factory=list)

  @model_validator(mode="after")
  def _unique_bindings(self) -> MissionStep:
    action_indexes = [action.index for action in self.actions]
    if len(action_indexes) != len(set(action_indexes)):
      raise ValueError("action indexes must be unique within a step")
    robot_ids = [action.robot_id for action in self.actions]
    if len(robot_ids) != len(set(robot_ids)):
      raise ValueError("a robot_id may appear only once within a step")
    if action_indexes != sorted(action_indexes):
      raise ValueError("actions must be ordered by index")
    if self.id is not None and self.id in self.after:
      raise ValueError("a step cannot depend on itself")
    if len(self.after) != len(set(self.after)):
      raise ValueError("step dependencies must be unique")
    return self


class MissionPlan(BaseModel):
  """One editable plan; subplans form a recursively inspectable plan tree."""

  plan_id: str = Field(min_length=1)
  title: str = Field(min_length=1)
  revision: int = Field(default=1, ge=1)
  state: PlanState = PlanState.DRAFT
  result: Any | None = None
  reason: str | None = None
  steps: list[MissionStep] = Field(default_factory=list)
  subplans: list[MissionPlan] = Field(default_factory=list)

  @model_validator(mode="after")
  def _validate_tree(self) -> MissionPlan:
    indexes = [step.index for step in self.steps]
    if len(indexes) != len(set(indexes)) or indexes != sorted(indexes):
      raise ValueError("plan steps must have unique ascending indexes")
    ids = [plan.plan_id for plan in self.subplans]
    if len(ids) != len(set(ids)):
      raise ValueError("subplan ids must be unique within a plan")
    return self


class MissionDocument(BaseModel):
  mission_id: str = Field(min_length=1)
  order: str = Field(min_length=1)
  priority: float = 0.0
  priority_band: MissionPriorityBand = MissionPriorityBand.NORMAL
  revision: int = Field(default=1, ge=1)
  state: MissionState = MissionState.PENDING
  result: Any | None = None
  reason: str | None = None
  plan: MissionPlan | None = None
  # Compatibility with the original single-plan document. New model edits
  # write ``plan``; existing callers may continue to use root ``steps``.
  steps: list[MissionStep] = Field(default_factory=list)

  @model_validator(mode="after")
  def _ordered_steps(self) -> MissionDocument:
    indexes = [step.index for step in self.steps]
    if len(indexes) != len(set(indexes)):
      raise ValueError("step indexes must be unique")
    if indexes != sorted(indexes):
      raise ValueError("steps must be ordered by index")
    return self

  @property
  def completeness(self) -> float:
    if self.plan is not None and self.plan.steps:
      completed = sum(
        step.state in (StepState.DONE, StepState.FAILED) for step in self.plan.steps
      )
      return completed / len(self.plan.steps)
    actions = [action for step in self.steps for action in step.actions]
    if not actions:
      return 0.0
    completed = sum(
      action.state in (ActionState.DONE, ActionState.FAILED) for action in actions
    )
    return completed / len(actions)


class MissionRevision(BaseModel):
  revision: int = Field(ge=1)
  accepted_at: datetime = Field(default_factory=lambda: datetime.now(UTC))
  document: MissionDocument


def validate_edit(current: MissionDocument, proposed: MissionDocument) -> None:
  """Reject identity changes and edits to protected or started work."""
  if proposed.mission_id != current.mission_id:
    raise ValueError("mission_id cannot change")
  if proposed.revision != current.revision + 1:
    raise ValueError("revision must increment by one")
  if proposed.order != current.order:
    raise ValueError("order must be changed through the mission mutation API")
  if proposed.priority_band is not current.priority_band:
    raise ValueError("priority band must be changed through the mission mutation API")

  proposed_steps = {step.index: step for step in proposed.steps}
  for step in current.steps:
    if step.state is StepState.PENDING:
      continue
    replacement = proposed_steps.get(step.index)
    if replacement is None or replacement != step:
      raise ValueError(f"step {step.index} has started and cannot be edited")

  def plan_steps(
    plan: MissionPlan | None,
  ) -> dict[tuple[tuple[str, ...], int], MissionStep]:
    indexed: dict[tuple[tuple[str, ...], int], MissionStep] = {}

    def visit(node: MissionPlan, parents: tuple[str, ...]) -> None:
      path = (*parents, node.plan_id)
      for step in node.steps:
        indexed[(path, step.index)] = step
      for subplan in node.subplans:
        visit(subplan, path)

    if plan is not None:
      visit(plan, ())
    return indexed

  proposed_plan_steps = plan_steps(proposed.plan)
  for location, step in plan_steps(current.plan).items():
    if not step.required and step.state is StepState.PENDING:
      continue
    replacement = proposed_plan_steps.get(location)
    if replacement is not None and replacement == step:
      continue
    label = repr(step.id) if step.id is not None else str(step.index)
    if step.required:
      raise ValueError(f"required plan step {label} cannot be edited")
    raise ValueError(f"plan step {label} has started and cannot be edited")
