"""P06 closed shared-event projection, independent of the private replay."""

from __future__ import annotations

import io
import json
import os
import subprocess
import sys
from pathlib import Path

import grpc
import pytest

from egia.core.link import EosClient, LinkPrimitiveError
from egia.lib import shared_event_policy, shared_events
from egia.lib.config import GatewayConfig
from tests.egia.stub.gateway import start_stub_gateway, stop_stub_gateway


def _lines(stream: io.StringIO) -> list[dict]:
  return [json.loads(line) for line in stream.getvalue().splitlines()]


def test_policy_matches_reviewed_fixture() -> None:
  fixture = json.loads(
    (
      Path(__file__).parents[1] / "fixtures/jsonl_events/egia-role-policy.json"
    ).read_text()
  )
  assert shared_event_policy.CODES == fixture["codes"]
  assert {
    key: list(value) if isinstance(value, tuple) else value
    for key, value in shared_event_policy.TYPES.items()
  } == fixture["types"]


def test_managed_binding_and_standalone_identity() -> None:
  assert shared_events.select_run_id(True, "r7-cde") == "r7-cde"
  local = shared_events.select_run_id(False, None)
  assert len(local) == 33 and local.startswith("l")
  for binding in (None, "l" + "a" * 32, "r" + "a" * 64, "r/bad"):
    try:
      shared_events.select_run_id(True, binding)
    except ValueError:
      pass
    else:
      raise AssertionError(f"accepted malformed managed binding: {binding!r}")
  try:
    shared_events.select_run_id(False, "r7-cde")
  except ValueError:
    pass
  else:
    raise AssertionError("standalone claimed a manager run")


def test_invalid_managed_process_exits_before_shared_event() -> None:
  env = os.environ.copy()
  env.pop("ISOS_RUN_ID", None)
  process = subprocess.run(
    [sys.executable, "-m", "egia.main", "--managed"],
    env=env,
    capture_output=True,
    text=True,
    timeout=5,
    check=False,
  )
  assert process.returncode == 2
  assert "EGIA run binding invalid" in process.stderr
  assert '"source":"egia"' not in process.stderr


def test_managed_logger_keeps_private_replay_and_projects_only_closed_events(
  tmp_path: Path,
) -> None:
  script = """
from pathlib import Path
from egia.lib.config import LoggingConfig
from egia.lib.logger import configure_logging, get_logger
from egia.lib import shared_events
import sys
import os
root = Path(sys.argv[1])
configure_logging(log_dir=root/'log', replay_dir=root/'state', config=LoggingConfig(),
                  run_id='r7-cde', managed=True)
get_logger('egia.main').error('provider_failure', prompt='SECRET_PROMPT',
                              media='SECRET_MEDIA', token='SECRET_TOKEN')
print('UNTRUSTED_RAW_STDERR', file=sys.stderr)
os.write(2, b'NATIVE_SECRET_STDERR\\n')
shared_events.emit('egia_starting')
shared_events.emit('egia_start_failed', fields={'reason_code':'model_unavailable'})
"""
  process = subprocess.run(
    [sys.executable, "-c", script, str(tmp_path)],
    capture_output=True,
    text=True,
    timeout=5,
    check=False,
  )
  assert process.returncode == 0, process.stderr
  events = [json.loads(line) for line in process.stderr.splitlines()]
  assert [event["event_code"] for event in events] == [
    "egia_starting",
    "egia_start_failed",
  ]
  assert all(event["run_id"] == "r7-cde" and "pid" not in event for event in events)
  assert all(len(line.encode()) + 1 <= 8064 for line in process.stderr.splitlines())
  assert "SECRET_" not in process.stderr
  assert "UNTRUSTED_RAW_STDERR" not in process.stderr
  assert "NATIVE_SECRET_STDERR" not in process.stderr
  assert not (tmp_path / "log/raw-stderr.log").exists()
  private = tmp_path / "state/egia.jsonl"
  assert private.is_file()
  replay = private.read_text()
  assert "provider_failure" in replay and "SECRET_" not in replay
  assert "UNTRUSTED_RAW_STDERR" not in replay
  assert "NATIVE_SECRET_STDERR" not in replay
  for path in (tmp_path / "log").rglob("*"):
    if path.is_file():
      assert "NATIVE_SECRET_STDERR" not in path.read_text()


def test_closed_policy_stop_severity_and_nonleak() -> None:
  stream = io.StringIO()
  sink = shared_events.EventSink("r7-cde", stream)
  assert sink.emit("egia_starting")
  assert sink.emit("egia_ready", fields={"generation": 3})
  assert not sink.emit("egia_ready", fields={"generation": 3})
  assert sink.ready_emitted
  assert sink.emit("egia_stopped", fields={"state": "stop_unconfirmed"})
  assert not sink.emit("egia_stopped", fields={"state": "stopped"})
  assert not sink.emit("egia_unlisted", fields={})
  assert not sink.emit(
    "egia_mission_started", fields={"mission_id": "secret prompt media"}
  )
  assert not sink.emit(
    "egia_mission_started", fields={"mission_id": "m1", "prompt": "secret"}
  )
  assert not sink.emit(
    "egia_operation_unknown",
    fields={"operation_id": "op1", "reason_code": "url=https://secret"},
  )
  assert not sink.emit(
    "egia_safety_rejected",
    fields={"reason_code": "policy_denied"},
    node_path="/cores/eos/layers/x/groups/y/nodes/z",
  )
  lines = _lines(stream)
  assert [item["event_code"] for item in lines] == [
    "egia_starting",
    "egia_ready",
    "egia_stopped",
  ]
  assert lines[-1]["level"] == "error"
  confirmed = io.StringIO()
  assert shared_events.EventSink("r7-cde", confirmed).emit(
    "egia_stopped", fields={"state": "stopped"}
  )
  assert _lines(confirmed)[0]["level"] == "info"
  assert all(len(line.encode()) + 1 <= 8064 for line in stream.getvalue().splitlines())
  assert "secret" not in stream.getvalue()
  assert all("pid" not in item for item in lines)


def test_first_refusal_preserved_and_repetitions_counted() -> None:
  stream = io.StringIO()
  sink = shared_events.EventSink("r7-cde", stream)
  for second in range(1000):
    assert sink.emit(
      "egia_safety_rejected",
      fields={"reason_code": "policy_denied"},
      now=second / 1000,
    )
  assert len(_lines(stream)) == 1
  sink.flush()
  lines = _lines(stream)
  assert [item["event_code"] for item in lines] == [
    "egia_safety_rejected",
    "egia_events_suppressed",
  ]
  assert lines[1]["fields"]["suppressed_count"] == 999


def test_distinct_correlations_keep_each_first_safety_refusal() -> None:
  stream = io.StringIO()
  sink = shared_events.EventSink("r7-cde", stream)
  for operation_id in ("op-a", "op-b", "op-a"):
    assert sink.emit(
      "egia_safety_rejected",
      fields={"reason_code": "policy_denied", "operation_id": operation_id},
      correlation_id=operation_id,
      now=1.0,
    )
  assert [event["correlation_id"] for event in _lines(stream)] == ["op-a", "op-b"]
  sink.flush()
  assert _lines(stream)[-1]["fields"] == {"suppressed_count": 1}


def test_collector_outage_cannot_block_terminal_or_private_work() -> None:
  class Broken(io.StringIO):
    def write(self, value: str) -> int:
      raise BrokenPipeError("collector closed")

  sink = shared_events.EventSink("r7-cde", Broken())
  assert not sink.emit("egia_ready", fields={"generation": 1})
  assert not sink.ready_emitted
  assert not sink.emit("egia_stopped", fields={"state": "stop_unconfirmed"})
  assert sink.write_failures == 2


def test_failed_first_refusal_is_retried_instead_of_suppressed() -> None:
  class Flaky(io.StringIO):
    failed = False

    def write(self, value: str) -> int:
      if not self.failed:
        self.failed = True
        raise BrokenPipeError("first append failed")
      return super().write(value)

  stream = Flaky()
  sink = shared_events.EventSink("r7-cde", stream)
  fields = {"reason_code": "policy_denied"}
  assert not sink.emit("egia_safety_rejected", fields=fields, now=1.0)
  assert sink.emit("egia_safety_rejected", fields=fields, now=1.1)
  assert [item["event_code"] for item in _lines(stream)] == ["egia_safety_rejected"]


@pytest.mark.asyncio
async def test_stub_eos_operation_receipt_and_unknown_are_projected(
  monkeypatch,
) -> None:
  stream = io.StringIO()
  monkeypatch.setattr(shared_events, "_sink", shared_events.EventSink("r7-cde", stream))
  stub = await start_stub_gateway()
  client = EosClient(
    GatewayConfig(host=stub.host, grpc_port=stub.port), backend="robot-a"
  )
  try:
    await client.initialize()
    await client.call_primitive(
      "speaker.play",
      "speaker-0",
      {"uri": "asset://alerts/ready.wav"},
      command_id="op-success",
    )
    stub.gateway.outcomes["speaker.play"] = "outcome_unknown"
    await client.call_primitive(
      "speaker.play",
      "speaker-0",
      {"uri": "asset://alerts/private.wav"},
      command_id="op-unknown",
    )
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)
  events = _lines(stream)
  assert [event["event_code"] for event in events] == [
    "egia_operation_submitted",
    "egia_operation_terminal",
    "egia_operation_submitted",
    "egia_operation_unknown",
    "egia_recovery_required",
  ]
  assert events[1]["fields"] == {"operation_id": "op-success", "outcome": "succeeded"}
  assert events[3]["fields"] == {
    "operation_id": "op-unknown",
    "reason_code": "receipt_missing",
  }
  assert shared_events.stop_unconfirmed()
  assert "private.wav" not in stream.getvalue()


@pytest.mark.asyncio
async def test_submit_transport_loss_is_unknown_before_any_response(
  monkeypatch,
) -> None:
  stream = io.StringIO()
  sink = shared_events.EventSink("r7-cde", stream)
  monkeypatch.setattr(shared_events, "_sink", sink)
  stub = await start_stub_gateway()
  stub.gateway.errors["SubmitOperation"] = (
    grpc.StatusCode.UNAVAILABLE,
    "submission response lost",
  )
  client = EosClient(
    GatewayConfig(host=stub.host, grpc_port=stub.port), backend="robot-a"
  )
  try:
    with pytest.raises(LinkPrimitiveError) as error:
      await client.call_primitive(
        "speaker.play",
        "speaker-0",
        {"uri": "asset://alerts/ready.wav"},
        command_id="op-lost",
      )
    assert error.value.status == 503
    assert stub.gateway.calls == []
    assert [event["event_code"] for event in _lines(stream)] == [
      "egia_operation_unknown",
      "egia_recovery_required",
    ]
    assert _lines(stream)[0]["fields"] == {
      "operation_id": "op-lost",
      "reason_code": "disconnect",
    }
    assert sink.stop_unconfirmed
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
@pytest.mark.parametrize("bad_id", ["", "bad/id", "x" * 65, "contains space"])
async def test_invalid_caller_operation_id_never_reaches_eos(bad_id: str) -> None:
  stub = await start_stub_gateway()
  client = EosClient(
    GatewayConfig(host=stub.host, grpc_port=stub.port), backend="robot-a"
  )
  try:
    with pytest.raises(LinkPrimitiveError) as error:
      await client.call_primitive(
        "speaker.play",
        "speaker-0",
        {"uri": "asset://alerts/ready.wav"},
        command_id=bad_id,
      )
    assert error.value.status == 400
    assert error.value.reason == "invalid operation ID"
    assert stub.gateway.calls == []
    assert client.generation == 0
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)
