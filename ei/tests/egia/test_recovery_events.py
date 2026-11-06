"""A03 crash-durable safety and operation boundaries."""

from __future__ import annotations

import io
import json
import os
from pathlib import Path
from types import SimpleNamespace

import pytest

from egia.core.monitor import EstopLatch
from egia.lib import shared_events
from egia.lib.operation_ledger import OperationLedger, PendingOperation
from egia.lib.safety_state import SafetyState, SafetyStateError, runtime_lock
from egia.main import main as egia_main
from egia.safety_cli import main as safety_cli_main

REVISION = "a" * 64


class ResetClient:
  backend = "robot-a"
  active_revision = REVISION
  projection_sha256 = "b" * 64

  def __init__(self) -> None:
    self.estop = False
    self.health = "ready"
    self.calls: list[str] = []

  @property
  def discovery(self):
    return SimpleNamespace(
      estop=self.estop,
      health=self.health,
      state="idle",
      robot_id="eos-robot-a",
      started_at="t1",
    )

  async def initialize(self, *, force: bool = False):
    self.calls.append("discover" if force else "initialize")
    return self.discovery

  async def refresh_status(self):
    self.calls.append("status")
    return {"status": self.health}

  async def call_primitive(self, name: str, device_id: str, body: dict):
    self.calls.append(name)
    return SimpleNamespace(succeeded=True)


def test_safety_state_atomic_roundtrip_and_one_shot(tmp_path: Path) -> None:
  state = SafetyState(tmp_path / "safety")
  with runtime_lock(state.root):
    state.engage({"robot-a": "eos-robot-a"}, REVISION)
    assert state.aliases() == {"robot-a"}
    assert (state.root / "safety-latch.json").stat().st_mode & 0o777 == 0o600
    state.authorize_reset(frozenset({"robot-a"}), *state.episode())
    authorization = state.consume_reset()
    assert authorization is not None and set(authorization.identities) == {"robot-a"}
    assert state.consume_reset() is None
    state.clear(authorization)
    assert state.aliases() == frozenset()
    assert (state.root / "safety-history.json").is_file()


def test_safety_state_lock_corruption_and_alias_mismatch_fail_closed(
  tmp_path: Path,
) -> None:
  state = SafetyState(tmp_path / "safety")
  state.engage({"robot-a": "eos-robot-a"}, REVISION)
  with runtime_lock(state.root):
    with (
      pytest.raises(SafetyStateError, match="runtime is active"),
      runtime_lock(state.root),
    ):
      pass
    with pytest.raises(SafetyStateError, match="aliases"):
      state.authorize_reset(frozenset({"robot-b"}), *state.episode())
  (state.root / "safety-latch.json").write_text("{")
  with pytest.raises(SafetyStateError):
    state.aliases()


def test_missing_latch_after_engagement_is_not_a_clean_home(tmp_path: Path) -> None:
  state = SafetyState(tmp_path / "safety")
  assert state.aliases() == frozenset()
  state.engage({"robot-a": "eos-robot-a"}, REVISION)
  (state.root / "safety-latch.json").unlink()
  with pytest.raises(SafetyStateError, match="missing"):
    state.aliases()


def test_crash_after_engaged_marker_before_latch_fails_closed(
  tmp_path: Path, monkeypatch
) -> None:
  state = SafetyState(tmp_path / "safety")
  replace = os.replace

  def crash_on_latch(source, target):
    if str(target).endswith("safety-latch.json"):
      raise OSError("simulated pre-latch crash")
    return replace(source, target)

  monkeypatch.setattr(os, "replace", crash_on_latch)
  with pytest.raises(OSError, match="pre-latch crash"):
    state.engage({"robot-a": "eos-robot-a"}, REVISION)
  assert (state.root / "safety-history.json").is_file()
  with pytest.raises(SafetyStateError, match="missing"):
    SafetyState(state.root).aliases()


def test_reset_authorization_is_bound_to_episode_and_revision(tmp_path: Path) -> None:
  state = SafetyState(tmp_path / "safety")
  state.engage({"robot-a": "eos-robot-a"}, REVISION)
  episode, revision = state.episode()
  with pytest.raises(SafetyStateError):
    state.authorize_reset(frozenset({"robot-a"}), "f" * 32, revision)
  with pytest.raises(SafetyStateError):
    state.authorize_reset(frozenset({"robot-a"}), episode, "c" * 64)
  state.authorize_reset(frozenset({"robot-a"}), episode, revision)
  authorization = state.consume_reset()
  assert authorization is not None
  assert (
    authorization.episode_id == episode and authorization.graph_revision == revision
  )
  state.clear(authorization)
  state.engage({"robot-a": "eos-robot-a"}, REVISION)
  assert state.episode()[0] != episode
  with pytest.raises(SafetyStateError):
    state.clear(authorization)


def test_failed_atomic_replace_preserves_prior_latch(
  tmp_path: Path, monkeypatch
) -> None:
  state = SafetyState(tmp_path / "safety")
  state.engage({"robot-a": "eos-robot-a"}, REVISION)

  def interrupted_replace(source, target):
    raise OSError("simulated crash before replace")

  monkeypatch.setattr(os, "replace", interrupted_replace)
  with pytest.raises(OSError, match="simulated crash"):
    state.engage({"robot-b": "eos-robot-b"}, REVISION)
  assert state.identities() == {"robot-a": "eos-robot-a"}
  assert not [path for path in state.root.iterdir() if path.name.endswith(".tmp")]


@pytest.mark.asyncio
async def test_latch_persists_before_egress_and_reset_rechecks_twice(
  tmp_path: Path,
) -> None:
  client = ResetClient()
  stream = io.StringIO()
  old = shared_events._sink
  shared_events.configure("r-a03", stream)
  try:
    latch = EstopLatch(client, tmp_path, graph_revision=REVISION)  # type: ignore[arg-type]
    await latch.engage("operator: test")
    state = SafetyState(tmp_path / "safety")
    assert state.aliases() == {"robot-a"}
    assert client.calls[-1] == "safety.estop"
    assert EstopLatch(client, tmp_path, graph_revision=REVISION).engaged  # type: ignore[arg-type]
    state.authorize_reset(frozenset({"robot-a"}), *state.episode())
    client.estop = True
    assert not await latch.consume_authorized_reset(state.consume_reset())
    assert latch.engaged and state.aliases() == {"robot-a"}
    assert state.consume_reset() is None
    client.estop = False
    state.authorize_reset(frozenset({"robot-a"}), *state.episode())
    assert await latch.consume_authorized_reset(state.consume_reset())
    assert not latch.engaged and not state.aliases()
    assert client.calls[-4:] == ["discover", "status", "discover", "status"]
    codes = [json.loads(line)["event_code"] for line in stream.getvalue().splitlines()]
    assert codes[-2:] == ["egia_safety_reset_requested", "egia_safety_reset_completed"]
  finally:
    shared_events._sink = old


@pytest.mark.asyncio
async def test_wrong_alias_and_unresolved_operation_refuse_reset(
  tmp_path: Path,
) -> None:
  client = ResetClient()
  latch = EstopLatch(client, tmp_path, graph_revision=REVISION)  # type: ignore[arg-type]
  await latch.engage("operator: test")
  state = SafetyState(tmp_path / "safety")
  state.authorize_reset(frozenset({"robot-a"}), *state.episode())
  assert not await latch.consume_authorized_reset(
    state.consume_reset(), unresolved_operations=True
  )
  assert latch.engaged
  state.authorize_reset(frozenset({"robot-a"}), *state.episode())
  # An alias removed from the selected backend set cannot be reset.
  other = ResetClient()
  other.backend = "robot-b"
  changed_latch = EstopLatch(other, tmp_path, graph_revision=REVISION)  # type: ignore[arg-type]
  assert not await changed_latch.consume_authorized_reset(state.consume_reset())
  assert state.aliases() == {"robot-a"}


@pytest.mark.asyncio
async def test_stale_selected_graph_revision_refuses_reset(tmp_path: Path) -> None:
  client = ResetClient()
  latch = EstopLatch(client, tmp_path, graph_revision=REVISION)  # type: ignore[arg-type]
  await latch.engage("operator: test")
  state = SafetyState(tmp_path / "safety")
  state.authorize_reset(frozenset({"robot-a"}), *state.episode())
  changed = EstopLatch(client, tmp_path, graph_revision="c" * 64)  # type: ignore[arg-type]
  assert not await changed.consume_authorized_reset(state.consume_reset())
  assert state.aliases() == {"robot-a"}


@pytest.mark.asyncio
async def test_changed_revision_between_reset_checks_retains_latch(
  tmp_path: Path,
) -> None:
  class DriftingClient(ResetClient):
    async def refresh_status(self):
      result = await super().refresh_status()
      if self.calls.count("status") == 2:
        self.projection_sha256 = "c" * 64
      return result

  client = DriftingClient()
  latch = EstopLatch(client, tmp_path, graph_revision=REVISION)  # type: ignore[arg-type]
  await latch.engage("operator: test")
  state = SafetyState(tmp_path / "safety")
  state.authorize_reset(frozenset({"robot-a"}), *state.episode())
  assert not await latch.consume_authorized_reset(state.consume_reset())
  assert state.aliases() == {"robot-a"}


def test_operation_identity_is_durable_and_corruption_fails_closed(
  tmp_path: Path,
) -> None:
  ledger = OperationLedger(tmp_path)
  item = PendingOperation(
    "op-a", "robot-a", "base.velocity", "base-0", "mission-a", "eos-robot-a", REVISION
  )
  ledger.reserve(item)
  assert ledger.pending() == [item]
  assert OperationLedger(tmp_path).pending() == [item]
  ledger.settle("op-a", "succeeded")
  assert ledger.pending() == []
  with pytest.raises(SafetyStateError, match="cannot be settled"):
    ledger.settle("op-a", "failed")
  with pytest.raises(SafetyStateError, match="reserved"):
    ledger.reserve(item)
  bad = ledger.root / "bad.json"
  bad.write_text("{")
  with pytest.raises(SafetyStateError):
    ledger.pending()


def test_operation_index_detects_missing_payload_and_directory(tmp_path: Path) -> None:
  ledger = OperationLedger(tmp_path)
  item = PendingOperation(
    "op-a", "robot-a", "base.velocity", "base-0", None, "eos-robot-a", REVISION
  )
  ledger.reserve(item)
  (ledger.root / "op-a.json").unlink()
  with pytest.raises(SafetyStateError, match="index"):
    ledger.pending()
  (ledger.root / "op-a.index").unlink()
  with pytest.raises(SafetyStateError, match="history"):
    ledger.pending()
  ledger.root.rmdir()
  with pytest.raises(SafetyStateError, match="directory is missing"):
    OperationLedger(tmp_path)


def test_operation_history_detects_one_missing_id_among_other_operations(
  tmp_path: Path,
) -> None:
  ledger = OperationLedger(tmp_path)
  first = PendingOperation(
    "op-a", "robot-a", "base.velocity", "base-0", None, "eos-robot-a", REVISION
  )
  second = PendingOperation(
    "op-b", "robot-a", "base.velocity", "base-0", None, "eos-robot-a", REVISION
  )
  ledger.reserve(first)
  ledger.reserve(second)
  ledger.settle("op-b", "succeeded")
  (ledger.root / "op-a.index").unlink()
  (ledger.root / "op-a.json").unlink()
  with pytest.raises(SafetyStateError, match="history and index set differ"):
    OperationLedger(tmp_path).pending()


def test_crash_after_operation_history_before_index_fails_closed(
  tmp_path: Path, monkeypatch
) -> None:
  ledger = OperationLedger(tmp_path)
  item = PendingOperation(
    "op-a", "robot-a", "base.velocity", "base-0", None, "eos-robot-a", REVISION
  )
  replace = os.replace

  def crash_on_index(source, target):
    if str(target).endswith("op-a.index"):
      raise OSError("simulated pre-index crash")
    return replace(source, target)

  monkeypatch.setattr(os, "replace", crash_on_index)
  with pytest.raises(OSError, match="pre-index crash"):
    ledger.reserve(item)
  with pytest.raises(SafetyStateError, match="history and index set differ"):
    OperationLedger(tmp_path).pending()


def test_crash_between_operation_index_and_payload_fails_closed(
  tmp_path: Path, monkeypatch
) -> None:
  ledger = OperationLedger(tmp_path)
  item = PendingOperation(
    "op-a", "robot-a", "base.velocity", "base-0", None, "eos-robot-a", REVISION
  )
  replace = os.replace

  def crash_on_record(source, target):
    if str(target).endswith("op-a.json"):
      raise OSError("simulated pre-record crash")
    return replace(source, target)

  monkeypatch.setattr(os, "replace", crash_on_record)
  with pytest.raises(OSError, match="pre-record crash"):
    ledger.reserve(item)
  assert (ledger.root / "op-a.index").is_file()
  with pytest.raises(SafetyStateError, match="set differ"):
    OperationLedger(tmp_path).pending()


def test_reset_auth_consumed_before_invalid_managed_bootstrap(
  tmp_path: Path, monkeypatch
) -> None:
  state = SafetyState(tmp_path / "egia/safety")
  state.engage({"robot-a": "eos-robot-a"}, REVISION)
  state.authorize_reset(frozenset({"robot-a"}), *state.episode())
  monkeypatch.delenv("ISOS_RUN_ID", raising=False)
  assert egia_main(["--managed", "--state-root", str(tmp_path)]) == 2
  assert state.consume_reset() is None


def test_reset_cli_requires_tty_confirmation_and_stopped_runtime(
  tmp_path: Path, monkeypatch
) -> None:
  state = SafetyState(tmp_path / "egia/safety")
  state.engage({"robot-a": "eos-robot-a"}, REVISION)
  assert (
    safety_cli_main(
      [
        "reset",
        "--state-root",
        str(tmp_path),
        "--episode",
        state.episode()[0],
        "--graph-revision",
        REVISION,
      ]
    )
    == 2
  )

  class Terminal(io.StringIO):
    def isatty(self) -> bool:
      return True

  monkeypatch.setattr("sys.stdin", Terminal("RESET EGIA SAFETY\n"))
  monkeypatch.setattr("sys.stdout", Terminal())
  with runtime_lock(state.root):
    assert (
      safety_cli_main(
        [
          "reset",
          "--state-root",
          str(tmp_path),
          "--episode",
          state.episode()[0],
          "--graph-revision",
          REVISION,
        ]
      )
      == 1
    )
  assert state.consume_reset() is None
  monkeypatch.setattr("sys.stdin", Terminal("wrong\n"))
  assert (
    safety_cli_main(
      [
        "reset",
        "--state-root",
        str(tmp_path),
        "--episode",
        state.episode()[0],
        "--graph-revision",
        REVISION,
      ]
    )
    == 1
  )
  assert state.consume_reset() is None
  monkeypatch.setattr("sys.stdin", Terminal("RESET EGIA SAFETY\n"))
  assert (
    safety_cli_main(
      [
        "reset",
        "--state-root",
        str(tmp_path),
        "--episode",
        state.episode()[0],
        "--graph-revision",
        REVISION,
      ]
    )
    == 0
  )
  assert state.consume_reset() is not None
