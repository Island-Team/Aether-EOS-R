"""Safety audit loop — fault re-evaluation, estop escalation, annotations."""

from __future__ import annotations

import asyncio
from collections.abc import Awaitable

import pytest

from egia.core.perception.admission import Admission
from egia.core.safety import safety_audit_entry


class FakeGate:
  """Exposes the counters the audit tick logs; no behavior of its own."""

  def __init__(self) -> None:
    self.estop_engaged = False
    self.admitted_count = 0
    self.rejected_count = 0


class FakeLatch:
  """Records engage calls — the escalation must go through the hook only."""

  def __init__(self) -> None:
    self.reasons: list[str] = []

  async def engage(self, reason: str) -> None:
    self.reasons.append(reason)


async def _enqueue_fault(queue: Admission, *, recovered: bool = False) -> None:
  metadata = {"reason": "heartbeat_lost"}
  if recovered:
    metadata["recovered"] = "true"
  await queue.enqueue(
    kind="fault",
    source="monitor",
    text="backend heartbeat lost (2 consecutive failures)",
    metadata=metadata,
  )


async def _run_audit(
  gate: FakeGate,
  queue: Admission,
  ticks: float,
  *,
  fault_estop_ticks: int = 3,
  active_mission=lambda: "m-1",
  annotations: list[tuple[str, str, str]] | None = None,
  engage_estop: Awaitable | None = None,
  emit=None,
) -> None:
  ready = asyncio.Event()
  stop = asyncio.Event()
  seen = annotations if annotations is not None else []
  task = asyncio.create_task(
    safety_audit_entry(
      gate,
      queue,
      audit_interval_s=0.01,
      fault_estop_ticks=fault_estop_ticks,
      active_mission=active_mission,
      annotate=lambda s, e, d: seen.append((s, e, d)),
      ready=ready,
      stop=stop,
      engage_estop=engage_estop,
      emit=emit,
    )
  )
  await ready.wait()
  await asyncio.sleep(ticks)
  stop.set()
  await task


@pytest.mark.asyncio
async def test_unrecovered_fault_engages_latch_once_per_episode() -> None:
  queue = Admission(capacity=16)
  gate = FakeGate()
  latch = FakeLatch()
  await _enqueue_fault(queue)
  await _run_audit(
    gate, queue, ticks=0.1, fault_estop_ticks=3, engage_estop=latch.engage
  )
  assert len(latch.reasons) == 1
  assert "unrecovered fault" in latch.reasons[0]


@pytest.mark.asyncio
async def test_no_fault_means_no_engage() -> None:
  queue = Admission(capacity=16)
  gate = FakeGate()
  latch = FakeLatch()
  await _run_audit(gate, queue, ticks=0.05, engage_estop=latch.engage)
  assert latch.reasons == []


@pytest.mark.asyncio
async def test_recovery_observation_resets_the_fault_episode() -> None:
  queue = Admission(capacity=16)
  gate = FakeGate()
  latch = FakeLatch()
  await _enqueue_fault(queue)
  ready = asyncio.Event()
  stop = asyncio.Event()
  task = asyncio.create_task(
    safety_audit_entry(
      gate,
      queue,
      audit_interval_s=0.01,
      fault_estop_ticks=4,
      active_mission=lambda: None,
      annotate=lambda s, e, d: None,
      ready=ready,
      stop=stop,
      engage_estop=latch.engage,
    )
  )
  await ready.wait()
  await asyncio.sleep(0.025)  # two faulted ticks, below the estop threshold
  await _enqueue_fault(queue, recovered=True)
  await asyncio.sleep(0.05)
  stop.set()
  await task
  assert latch.reasons == []


@pytest.mark.asyncio
async def test_fault_active_annotates_the_live_plan() -> None:
  queue = Admission(capacity=16)
  gate = FakeGate()
  annotations: list[tuple[str, str, str]] = []
  await _enqueue_fault(queue)
  await _run_audit(
    gate,
    queue,
    ticks=0.05,
    fault_estop_ticks=10,
    annotations=annotations,
    engage_estop=FakeLatch().engage,
  )
  assert annotations
  assert all(source == "audit" for source, _, _ in annotations)
  assert all(event == "fault_active" for _, event, _ in annotations)


@pytest.mark.asyncio
async def test_estop_is_annotated() -> None:
  queue = Admission(capacity=16)
  gate = FakeGate()
  annotations: list[tuple[str, str, str]] = []
  await _enqueue_fault(queue)
  await _run_audit(
    gate,
    queue,
    ticks=0.1,
    fault_estop_ticks=2,
    annotations=annotations,
    engage_estop=FakeLatch().engage,
  )
  estops = [a for a in annotations if a[1] == "estop"]
  assert len(estops) == 1


@pytest.mark.asyncio
async def test_no_running_mission_skips_fault_annotations_but_still_escalates() -> None:
  queue = Admission(capacity=16)
  gate = FakeGate()
  latch = FakeLatch()
  annotations: list[tuple[str, str, str]] = []
  await _enqueue_fault(queue)
  await _run_audit(
    gate,
    queue,
    ticks=0.1,
    fault_estop_ticks=2,
    active_mission=lambda: None,
    annotations=annotations,
    engage_estop=latch.engage,
  )
  assert len(latch.reasons) == 1
  assert [a for a in annotations if a[1] == "fault_active"] == []
  # The estop verdict itself is always annotated for the record attempt;
  # with no live plan the planner drops it, but the loop still emits it.
  assert len([a for a in annotations if a[1] == "estop"]) == 1


@pytest.mark.asyncio
async def test_audit_findings_are_emitted_through_the_queue() -> None:
  """Per invariant 3 the loop feeds its own findings back through the queue.

  The estop verdict (and the fault-active re-entry) is admitted back into the
  queue via the injected ``emit`` hook, not just annotated on the plan.
  """
  queue = Admission(capacity=16)
  gate = FakeGate()
  emitted: list[tuple[str, str, dict]] = []
  await _enqueue_fault(queue)
  await _run_audit(
    gate,
    queue,
    ticks=0.1,
    fault_estop_ticks=2,
    engage_estop=FakeLatch().engage,
    emit=lambda source, reason, metadata: emitted.append((source, reason, metadata)),
  )
  # The estop verdict and (with a running mission) the fault-active re-entry
  # were both admitted back into the queue by the loop.
  reasons = [reason for _, reason, _ in emitted]
  assert "estop" in reasons
  assert "fault_active" in reasons
  assert all(source == "audit" for source, _, _ in emitted)
