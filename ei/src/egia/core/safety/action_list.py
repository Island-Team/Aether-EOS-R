"""Plan safety assessment seam for concurrent semantic steps (§3.3).

The safety audit loop continuously re-evaluates the mission table against
live state, but it does not yet inspect a mission's semantic plan and
executed actions. This module is that seam. For
now :func:`assess_action_list` is a stub: it always returns no findings.

The deferred checks are the ones a parallel-action plan makes dangerous:
conflicting actions in one step (two moves of the same body part) and an action
whose preconditions contradict the current plan state. The seam is deliberately not wired into the audit
loop yet; the signature is the contract the loop will call once the checks
land.
"""

from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, Field

from egia.api.plan import ActionRecord, PlanStep

__all__ = ["SafetyFinding", "assess_action_list"]


class SafetyFinding(BaseModel):
  """One verdict about the action list: what, why, and how severe.

  ``severity`` mirrors the audit loop's vocabulary (``info`` | ``warning`` |
  ``critical``) so a wired loop can escalate without remapping; ``item``
  names the plan step or action the finding concerns, when one applies.
  """

  severity: Literal["info", "warning", "critical"]
  event: str = Field(description="machine-parsable verdict name")
  detail: str = Field(default="", description="human-readable explanation")
  item: str | None = Field(default=None, description="plan-step id or action name")


def assess_action_list(
  plan: list[PlanStep],
  actions: list[ActionRecord],
) -> list[SafetyFinding]:
  """Assess the mission's semantic plan and recorded actions for safety.

  Stub: always returns ``[]``. Concurrent-conflict detection (two moves of
  the same body part in one step) and plan-vs-action consistency checks land
  here; nothing calls this yet (§3.3 seam).
  """
  return []
