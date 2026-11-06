"""Coordination — bidders, epsilon-raising auction, task board, zone leases."""

from __future__ import annotations

from egia.core.decision.coordination.auction import award
from egia.core.decision.coordination.bidders import BidderProfile, bidder_profiles
from egia.core.decision.coordination.board import StepState, TaskBoard
from egia.core.decision.coordination.leases import (
  LeaseClaim,
  LeaseOutcome,
  ZoneLeases,
)
from egia.core.decision.profiles import Profile, ProfileStep
from egia.core.link import DeviceBinding, EosDiscovery, PrimitiveBinding


def _step(step_id: str, primitive: str, **kwargs) -> ProfileStep:
  return ProfileStep.model_validate({"id": step_id, "primitive": primitive, **kwargs})


def _profile(steps: list[dict]) -> Profile:
  return Profile.model_validate({"name": "job-x", "order": "order x", "steps": steps})


# ----------------------------------------------------------------- bidders


def _bidder(part_id: str = "arm-1", **overrides) -> BidderProfile:
  fields = {
    "backend": "line",
    "part_id": part_id,
    "primitives": {"gripper.motion", "process.assemble"},
  }
  fields.update(overrides)
  return BidderProfile(**fields)


def test_bid_price_sums_terms_and_abstains() -> None:
  bidder = _bidder(load_s=10.0, zone="cell-a", last_primitive="gripper.motion")
  fresh = _step("s1", "gripper.motion", est_duration_s=2.0)
  assert bidder.bid(fresh, congestion=0.1) == 2.0 + 0.0 + 0.0 + 1.0

  moved = _step("s2", "process.assemble", est_duration_s=2.0, zone="cell-b")
  assert bidder.bid(moved, congestion=0.1) == 2.0 + 1.0 + 0.5 + 1.0

  assert _bidder(healthy=False).bid(fresh) is None
  assert _bidder(primitives={"speaker.play"}).bid(fresh) is None


def test_bidder_commit_and_settle_load() -> None:
  bidder = _bidder()
  step = _step("s1", "gripper.motion", est_duration_s=3.0, zone="cell-a")
  bidder.commit(step)
  assert (
    bidder.load_s == 3.0
    and bidder.zone == "cell-a"
    and bidder.last_primitive == "gripper.motion"
  )
  bidder.settle(step)
  assert bidder.load_s == 0.0


def _discovery(*devices: DeviceBinding) -> EosDiscovery:
  return EosDiscovery(
    robot_id="01a077f7-b5b8-7b4e-8057-ca688b8ff48f",
    robot_name="test-robot",
    robot_model="test-model",
    robot_description="coordination fixture",
    state="idle",
    health="ready",
    estop=False,
    power_pct=None,
    version="0.1.0",
    started_at="2026-09-07T00:00:00Z",
    robot_primitives=(),
    devices=tuple(devices),
  )


def _device(device_id: str, *primitives: str, alive: bool = True) -> DeviceBinding:
  health = "ready" if alive else "unavailable"
  return DeviceBinding(
    id=device_id,
    device_class="arm",
    part_id="default",
    description="test arm",
    state="idle" if alive else "fault",
    health=health,
    primitives=tuple(
      PrimitiveBinding(
        name=name,
        eos_robot_id="01a077f7-b5b8-7b4e-8057-ca688b8ff48f",
        device_id=device_id,
        version="2.0.0" if name in {"arm.movej", "arm.movel"} else "1.0.0",
        description=f"fixture binding for {name}",
        device_description="test arm",
        scope="device",
        kind="direct",
        health=health,
        references=("relative", "absolute")
        if name in {"arm.movej", "arm.movel"}
        else (),
        absolute_frame="base_link" if name == "arm.movel" else None,
        dependencies=(),
        accepts=(),
        limits={},
      )
      for name in primitives
    ),
  )


def test_bidder_profiles_compile_from_eos_devices() -> None:
  profiles = bidder_profiles(
    "line",
    _discovery(
      _device("arm-0", "arm.movej", "arm.movel"),
      _device("arm-1", "arm.movel"),
    ),
  )
  assert [profile.id for profile in profiles] == ["line:arm-0", "line:arm-1"]
  assert profiles[0].primitives == {"arm.movej", "arm.movel"}
  assert profiles[1].primitives == {"arm.movel"}
  assert profiles[0].tags == set()


def test_each_same_class_device_keeps_its_own_primitives() -> None:
  profiles = {
    profile.part_id: profile
    for profile in bidder_profiles(
      "line",
      _discovery(
        _device("arm-0", "arm.movej"),
        _device("arm-1", "arm.movel"),
      ),
    )
  }
  assert profiles["arm-0"].primitives == {"arm.movej"}
  assert profiles["arm-1"].primitives == {"arm.movel"}


def test_unhealthy_device_is_excluded_from_bidding() -> None:
  assert (
    bidder_profiles("sim", _discovery(_device("arm-0", "arm.movel", alive=False))) == []
  )


def test_eos_discovery_does_not_infer_station_reach() -> None:
  (profile,) = bidder_profiles("line", _discovery(_device("arm-0", "arm.movel")))
  assert profile.stations == set()
  assert profile.bid(_step("s1", "arm.movel", station="station-3")) is not None


# ----------------------------------------------------------------- auction


def test_auction_assigns_each_step_a_distinct_bidder() -> None:
  steps = [
    _step("s1", "gripper.motion", est_duration_s=2.0),
    _step("s2", "gripper.motion", est_duration_s=2.0),
  ]
  bidders = [_bidder("arm-1"), _bidder("arm-2")]
  result = award(steps, bidders)
  assert set(result.awards) == {"s1", "s2"}
  assert len(set(result.awards.values())) == 2  # distinct bidders
  assert result.unassigned == []


def test_auction_is_deterministic() -> None:
  steps = [_step("s1", "gripper.motion"), _step("s2", "gripper.motion")]
  bidders = [_bidder("arm-1"), _bidder("arm-2")]
  first = award(steps, bidders)
  second = award(steps, bidders)
  assert first.awards == second.awards and first.prices == second.prices


def test_auction_prefers_cheaper_bidder() -> None:
  step = _step("s1", "gripper.motion", est_duration_s=2.0, zone="cell-b")
  near = _bidder("arm-1", zone="cell-b")
  far = _bidder("arm-2", zone="cell-a")
  result = award([step], [far, near])
  assert result.awards["s1"] == "line:arm-1"


def test_auction_congestion_prices_loaded_bidder_out() -> None:
  step = _step("s1", "gripper.motion", est_duration_s=2.0)
  loaded = _bidder("arm-1", load_s=100.0)
  idle = _bidder("arm-2")
  result = award([step], [loaded, idle])
  assert result.awards["s1"] == "line:arm-2"


def test_auction_unassigns_steps_without_feasible_bidders() -> None:
  step = _step("s1", "magic.primitive")
  result = award([step], [_bidder("arm-1")])
  assert result.awards == {} and result.unassigned == ["s1"]


def test_auction_more_steps_than_bidders_leaves_remainder() -> None:
  steps = [
    _step("s1", "gripper.motion"),
    _step("s2", "gripper.motion"),
    _step("s3", "gripper.motion"),
  ]
  result = award(steps, [_bidder("arm-1")])
  assert len(result.awards) == 1
  assert len(result.unassigned) == 2


# ------------------------------------------------------------------- board


def _board_with_job():
  board = TaskBoard()
  profile = _profile(
    [
      {"id": "a", "primitive": "gripper.motion"},
      {"id": "b", "primitive": "gripper.motion", "after": ["a"]},
      {"id": "c", "primitive": "gripper.motion", "after": ["b"]},
    ]
  )
  job = board.add_job(profile, mission_id="m-1")
  return board, job


def test_board_states_follow_award_and_completion() -> None:
  board, job = _board_with_job()
  assert [s.step.id for s in board.ready()] == ["a"]
  assert job.steps["b"].state is StepState.WAITING

  board.award(job.job_id, "a", "line:arm-1")
  assert job.steps["a"].state is StepState.RUNNING
  assert [s.step.id for s in board.ready()] == []

  newly = board.complete(job.job_id, "a")
  assert [s.step.id for s in newly] == ["b"]
  assert [s.step.id for s in board.ready()] == ["b"]

  board.award(job.job_id, "b", "line:arm-2")
  board.complete(job.job_id, "b")
  board.award(job.job_id, "c", "line:arm-1")
  board.complete(job.job_id, "c")
  assert job.finished()


def test_board_fail_blocks_downstream() -> None:
  board, job = _board_with_job()
  board.award(job.job_id, "a", "line:arm-1")
  board.fail(job.job_id, "a")
  assert job.failed() and not job.finished()
  assert board.ready() == []  # b, c never run


def test_board_retire_enforces_finished() -> None:
  board, job = _board_with_job()
  import pytest

  with pytest.raises(ValueError, match="still running"):
    board.retire(job.job_id)
  board.fail(job.job_id, "a")
  board.retire(job.job_id)
  assert board.job(job.job_id) is None


def test_board_capacity_is_backpressure() -> None:
  board = TaskBoard(capacity=1)
  assert (
    board.add_job(
      _profile(
        [{"id": "a", "primitive": "gripper.motion"}],
      ),
      "m-1",
    )
    is not None
  )
  assert (
    board.add_job(_profile([{"id": "a", "primitive": "gripper.motion"}]), "m-2") is None
  )


def test_board_rejects_award_on_running_step() -> None:
  import pytest

  board, job = _board_with_job()
  board.award(job.job_id, "a", "line:arm-1")
  with pytest.raises(ValueError, match="not awardable"):
    board.award(job.job_id, "a", "line:arm-2")


# ------------------------------------------------------------------ leases


def test_lease_acquire_release_cycle() -> None:
  leases = ZoneLeases()
  claim = LeaseClaim(step_id="s1", priority=1.0, switch_cost=2.0)
  assert leases.acquire("cell-a", claim) is LeaseOutcome.ACQUIRED
  assert leases.holder("cell-a") is claim
  assert leases.release("cell-a", "s1") is claim
  assert leases.holder("cell-a") is None


def test_lease_contention_defers_weaker_claimant() -> None:
  leases = ZoneLeases()
  leases.acquire("cell-a", LeaseClaim(step_id="holder", priority=0.8, switch_cost=3.0))
  weak = leases.acquire(
    "cell-a", LeaseClaim(step_id="claimant", priority=0.5, switch_cost=3.0)
  )
  assert weak is LeaseOutcome.DEFERRED
  assert leases.holder("cell-a").step_id == "holder"


def test_lease_contention_preempts_for_rush_order() -> None:
  leases = ZoneLeases()
  leases.acquire("cell-a", LeaseClaim(step_id="holder", priority=0.4, switch_cost=2.0))
  rush = leases.acquire(
    "cell-a", LeaseClaim(step_id="claimant", priority=1.0, switch_cost=2.0)
  )
  assert rush is LeaseOutcome.ACQUIRED_PREEMPTED
  assert leases.holder("cell-a").step_id == "claimant"
  # The preempted holder's late release must not evict the new holder.
  assert leases.release("cell-a", "holder") is None
  assert leases.holder("cell-a").step_id == "claimant"


def test_lease_self_reacquire_is_a_wiring_error() -> None:
  import pytest

  leases = ZoneLeases()
  claim = LeaseClaim(step_id="s1", priority=1.0, switch_cost=1.0)
  leases.acquire("cell-a", claim)
  with pytest.raises(ValueError, match="already holds"):
    leases.acquire("cell-a", claim)
