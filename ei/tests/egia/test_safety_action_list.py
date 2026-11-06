"""Action-list safety seam — the stub assessment returns no findings (§3.3)."""

from __future__ import annotations

from egia.api.plan import ActionRecord, PlanStep, PlanStepStatus, StepOutcome
from egia.core.safety.action_list import SafetyFinding, assess_action_list


def test_assess_action_list_stub_returns_no_findings() -> None:
  plan = [
    PlanStep(id="t1", sentence="Survey the north field.", status=PlanStepStatus.DONE),
    PlanStep(id="t2", sentence="Report findings.", status=PlanStepStatus.PENDING),
  ]
  actions = [
    ActionRecord(index=1, action="base.velocity", outcome=StepOutcome.SUCCEEDED),
    ActionRecord(
      index=2,
      action="speaker.say",
      outcome=StepOutcome.FAILED,
      reason="rejected by backend (speaker disabled)",
    ),
  ]
  assert assess_action_list(plan, actions) == []
  assert assess_action_list([], []) == []


def test_safety_finding_carries_severity_and_event() -> None:
  finding = SafetyFinding(
    severity="critical", event="conflicting_actions", detail="two base moves"
  )
  assert finding.severity == "critical"
  assert finding.item is None
