"""Log dedup behavior for libexec/egia_ext/tui.feeds — no curses, no real streams."""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
from typing import Self

import pytest

from egia_ext.tui.config import TuiConfig
from egia_ext.tui.feeds import (
  ConsoleState,
  _append_log,
  _apply_model_io_frame,
  _seen_recently,
  log_tail,
  replay_tail_offset,
  send_call,
  send_mission_command,
  send_skill_command,
  session_replay_path,
  watch_skill_execution,
)


class _FakeWS:
  """Records sent frames; answers ``recv`` with a canned verdict."""

  def __init__(self, replies: list[str]) -> None:
    self.replies = replies
    self.sent: list[str] = []

  async def send(self, message: str) -> None:
    self.sent.append(message)

  async def recv(self) -> str:
    return self.replies.pop(0)

  async def __aenter__(self) -> Self:
    return self

  async def __aexit__(self, *exc) -> None:
    return None

  def __aiter__(self):
    return self

  async def __anext__(self) -> str:
    if not self.replies:
      raise StopAsyncIteration
    return self.replies.pop(0)


def test_model_io_frame_keeps_all_agents_and_selects_planner_for_relative_panes() -> (
  None
):
  state = ConsoleState()
  sentinel = {
    "exchange_id": 4,
    "agent_id": "sentinel",
    "status": "streaming",
  }
  planner = {
    "exchange_id": 2,
    "agent_id": "planner",
    "status": "complete",
  }

  _apply_model_io_frame(
    state,
    {
      "exchange": sentinel,
      "total": 5,
      "agents": [
        {"agent_id": "planner", "thinking": False, "exchange": planner},
        {"agent_id": "sentinel", "thinking": True, "exchange": sentinel},
      ],
    },
  )

  assert state.model_io_latest == sentinel
  assert state.model_io == planner
  assert len(state.model_agents) == 2
  assert state.model_requests == 5


def test_model_io_frame_accepts_legacy_top_level_exchange() -> None:
  state = ConsoleState()
  exchange = {"exchange_id": 1, "status": "complete"}

  _apply_model_io_frame(state, {"exchange": exchange, "total": 1})

  assert state.model_io_latest == exchange
  assert state.model_io == exchange
  assert state.model_agents == []


def test_send_call_payload_carries_eos_target(monkeypatch) -> None:
  state = ConsoleState()
  config = TuiConfig()
  fake = _FakeWS([json.dumps({"ok": True, "accepted": True, "result": None})])
  urls: list[str] = []

  def fake_connect(url: str, open_timeout: float = 0):
    urls.append(url)
    return fake

  monkeypatch.setattr("egia_ext.tui.feeds.websockets.connect", fake_connect)
  asyncio.run(send_call(state, config, "base.velocity", {"x": 1}, "sim", "base-0"))
  assert urls[0].endswith("/api/aether/egia/primitive/invoke")
  assert json.loads(fake.sent[0]) == {
    "primitive": "base.velocity",
    "arguments": {"x": 1},
    "robot_id": "sim",
    "device_id": "base-0",
  }
  assert any(
    "base.velocity@sim/base-0 accepted" in str(entry.event.get("event"))
    for entry in state.logs
  )


def test_send_process_call_omits_device(monkeypatch) -> None:
  state = ConsoleState()
  config = TuiConfig()
  fake = _FakeWS(
    [
      json.dumps(
        {"ok": False, "error": "rejected_by_gate", "reason": "fleet estop latched"}
      )
    ]
  )

  def fake_connect(url: str, open_timeout: float = 0):
    return fake

  monkeypatch.setattr("egia_ext.tui.feeds.websockets.connect", fake_connect)
  asyncio.run(send_call(state, config, "safety.estop", {}, "sim", None))
  assert json.loads(fake.sent[0]) == {
    "primitive": "safety.estop",
    "robot_id": "sim",
    "arguments": {},
  }
  assert any(
    "rejected: rejected_by_gate (fleet estop latched)" in str(entry.event.get("event"))
    for entry in state.logs
  )


def test_send_mission_command_payload_ack_and_error(monkeypatch) -> None:
  state = ConsoleState()
  config = TuiConfig()
  mission_id = "12345678-1234-1234-1234-123456789abc"
  fake = _FakeWS(
    [
      json.dumps(
        {
          "ok": True,
          "operation": "cancel",
          "mission": {
            "mission_id": mission_id,
            "state": "cancelled",
            "revision": 4,
          },
        }
      ),
      json.dumps(
        {
          "ok": False,
          "operation": "cancel",
          "error": "mission_revision_conflict",
          "reason": "expected revision 3, current revision 4",
        }
      ),
    ]
  )

  def fake_connect(url: str, open_timeout: float = 0):
    assert url.endswith("/api/aether/egia/missions/command")
    return fake

  monkeypatch.setattr("egia_ext.tui.feeds.websockets.connect", fake_connect)
  payload = {
    "operation": "cancel",
    "mission_id": mission_id,
    "expected_revision": 3,
  }
  asyncio.run(send_mission_command(state, config, payload))
  asyncio.run(send_mission_command(state, config, payload))

  assert json.loads(fake.sent[0]) == payload
  messages = [str(entry.event.get("event")) for entry in state.logs]
  assert any(
    "mission cancel acknowledged" in message and "r4" in message for message in messages
  )
  assert any("mission_revision_conflict" in message for message in messages)


def test_send_mission_command_logs_transport_failure(monkeypatch) -> None:
  state = ConsoleState()

  def fail_connect(url: str, open_timeout: float = 0):
    raise OSError("offline")

  monkeypatch.setattr("egia_ext.tui.feeds.websockets.connect", fail_connect)
  asyncio.run(
    send_mission_command(state, TuiConfig(), {"operation": "create", "order": "x"})
  )
  assert any(
    "mission create transport failed: offline" in str(entry.event.get("event"))
    for entry in state.logs
  )


def test_send_skill_start_mints_uuid_and_retains_execution(monkeypatch) -> None:
  state = ConsoleState()
  execution = {
    "robot_id": "robot-a",
    "execution_id": "exec-1",
    "request_id": "request-1",
    "name": "scan",
    "version": "1.0.0",
    "status": "queued",
    "sequence": 1,
    "cancellation_requested": False,
  }
  fake = _FakeWS(
    [json.dumps({"ok": True, "operation": "start", "execution": execution})]
  )
  monkeypatch.setattr("egia_ext.tui.feeds.websockets.connect", lambda *a, **k: fake)
  monkeypatch.setattr("egia_ext.tui.feeds.uuid.uuid4", lambda: "request-1")

  asyncio.run(
    send_skill_command(
      state,
      TuiConfig(),
      {
        "operation": "start",
        "robot_id": "robot-a",
        "name": "scan",
        "version": "1.0.0",
        "arguments": {},
        "timeout_s": 30.0,
      },
    )
  )

  assert json.loads(fake.sent[0])["request_id"] == "request-1"
  assert state.skill_executions["exec-1"] == execution


def test_skill_watch_resumes_after_sequence_and_stops_at_terminal(monkeypatch) -> None:
  state = ConsoleState(
    skill_executions={"exec-1": {"execution_id": "exec-1", "sequence": 4}}
  )
  terminal = {
    "robot_id": "robot-a",
    "execution_id": "exec-1",
    "request_id": "request-1",
    "name": "scan",
    "version": "1.0.0",
    "status": "succeeded",
    "sequence": 5,
    "cancellation_requested": False,
    "result": {"found": 2},
  }
  fake = _FakeWS([json.dumps(terminal)])
  monkeypatch.setattr("egia_ext.tui.feeds.websockets.connect", lambda *a, **k: fake)

  asyncio.run(watch_skill_execution(state, TuiConfig(), "robot-a", "exec-1"))

  assert json.loads(fake.sent[0]) == {
    "robot_id": "robot-a",
    "execution_id": "exec-1",
    "after_sequence": 4,
  }
  assert state.skill_executions["exec-1"]["status"] == "succeeded"


def _event(ts: str, name: str = "queue_full", level: str = "warning") -> dict:
  return {
    "level": level,
    "logger": "egia.core",
    "timestamp": ts,
    "event": name,
  }


def test_consecutive_repeats_collapse_into_counted_entry() -> None:
  state = ConsoleState()
  for ts in ("12:00:01Z", "12:00:02Z", "12:00:03Z"):
    _append_log(state, _event(ts))
  assert len(state.logs) == 1
  assert state.logs[0].count == 3
  assert state.logs[0].event["timestamp"] == "12:00:03Z"


def test_distinct_levels_or_loggers_stay_separate() -> None:
  state = ConsoleState()
  _append_log(state, _event("12:00:01Z", name="x"))
  _append_log(state, _event("12:00:02Z", name="x", level="error"))
  _append_log(state, _event("12:00:03Z", name="x", level="error"))
  assert len(state.logs) == 2
  assert [e.count for e in state.logs] == [1, 2]


def test_repeats_anywhere_collapse_not_just_consecutive() -> None:
  state = ConsoleState()
  _append_log(state, _event("12:00:01Z", name="a"))
  _append_log(state, _event("12:00:02Z", name="a"))
  _append_log(state, _event("12:00:03Z", name="b"))
  _append_log(state, _event("12:00:04Z", name="a"))
  assert [e.event["event"] for e in state.logs] == ["b", "a"]
  assert [e.count for e in state.logs] == [1, 3]  # "a" fired at 01, 02, 04
  assert state.logs[-1].event["timestamp"] == "12:00:04Z"


def test_repeat_moves_entry_to_tail() -> None:
  state = ConsoleState()
  _append_log(state, _event("12:00:01Z", name="old"))
  _append_log(state, _event("12:00:02Z", name="mid"))
  _append_log(state, _event("12:00:03Z", name="old"))
  assert [e.event["event"] for e in state.logs] == ["mid", "old"]
  assert state.logs[-1].count == 2


def test_seen_recently_matches_event_and_timestamp() -> None:
  state = ConsoleState()
  _append_log(state, _event("12:00:01Z"))
  assert _seen_recently(state, _event("12:00:01Z"))
  assert not _seen_recently(state, _event("12:00:99Z"))


class _Lines:
  """Minimal async readline stub for child_stderr_feed."""

  def __init__(self, lines: list[str]) -> None:
    self._lines = lines

  async def readline(self) -> bytes:
    if not self._lines:
      return b""
    line = self._lines.pop(0)
    return (line + "\n").encode()


def test_child_stderr_traceback_lines_are_child_errors() -> None:
  import asyncio

  from egia_ext.tui.feeds import child_stderr_feed

  state = ConsoleState()
  stream = _Lines(
    [
      "\x1b[31mTraceback (most recent call last):\x1b[0m",
      '  File "g.py", line 200, in <module>',
      "ValueError: boom",
      "",
      "some other passthrough",
    ]
  )
  asyncio.run(child_stderr_feed(state, TuiConfig(), stream))
  events = [e.event for e in state.logs]
  assert [(e["level"], e["logger"]) for e in events[:3]] == [
    ("error", "child.stderr")
  ] * 3
  assert events[0]["event"].startswith("Traceback")
  assert events[2]["event"] == "ValueError: boom"
  assert events[3]["level"] == "warning"
  assert events[3]["logger"] == "egia.main"
  assert not _seen_recently(state, _event("12:00:01Z", name="other"))


def test_child_stderr_single_lined_console_event_mints_no_fake_warning() -> None:
  import asyncio

  from egia_ext.tui.feeds import child_stderr_feed

  # The child's renderers escape newlines in extras (egia.lib.logger), so a
  # multi-line prompt arrives as one parseable line — no continuation lines
  # that the fallback would tag WARNING egia.main.
  state = ConsoleState()
  stream = _Lines(
    [
      (
        "[aether-egia] INFO @ egia.core.context [2026-08-18T10:00:00Z]: "
        "first_context_snapshot prompt=Preamble.\\nno data received"
        "\\nEnvironment: unavailable observation_ids=[]"
      )
    ]
  )
  asyncio.run(child_stderr_feed(state, TuiConfig(), stream))
  events = [e.event for e in state.logs]
  assert len(events) == 1
  assert events[0]["level"] == "info"
  assert events[0]["logger"] == "egia.core.context"
  assert not any(e["logger"] == "egia.main" for e in events)


def test_child_stderr_skips_structured_events_forwarded_to_session() -> None:
  from egia_ext.tui.feeds import child_stderr_feed

  state = ConsoleState()
  stream = _Lines(
    [
      "[aether-egia] INFO @ egia.main [2026-08-18T10:00:00Z]: ready",
      "unstructured crash detail",
    ]
  )
  asyncio.run(child_stderr_feed(state, TuiConfig(), stream, structured_in_session=True))

  assert [entry.event["event"] for entry in state.logs] == ["unstructured crash detail"]


def test_raw_warning_lines_dedup_too() -> None:
  state = ConsoleState()
  raw = {"level": "warning", "logger": "egia.main", "timestamp": "-", "event": "tb"}
  _append_log(state, dict(raw))
  _append_log(state, dict(raw))
  assert len(state.logs) == 1
  assert state.logs[0].count == 2


def test_level_threshold_config_unchanged() -> None:
  cfg = TuiConfig()
  assert cfg.log_level == "INFO"


def test_log_tail_reads_replay_from_state_not_diagnostic_log(
  tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
  state_dir = tmp_path / "state"
  log_dir = tmp_path / "log"
  state_dir.mkdir()
  log_dir.mkdir()
  (state_dir / "egia.jsonl").write_text(
    json.dumps(_event("12:00:01Z", name="durable_event")) + "\n",
    encoding="utf-8",
  )
  (log_dir / "egia.jsonl").write_text(
    json.dumps(_event("12:00:02Z", name="wrong_root")) + "\n",
    encoding="utf-8",
  )
  state = ConsoleState()

  class TailRead(Exception):
    """Stop the perpetual tail loop after its first read."""

  async def stop_after_read(_delay: float) -> None:
    raise TailRead

  monkeypatch.setattr("egia_ext.tui.feeds.asyncio.sleep", stop_after_read)
  with pytest.raises(TailRead):
    asyncio.run(
      log_tail(
        state,
        TuiConfig(state_dir=state_dir, log_dir=log_dir),
      )
    )

  assert [entry.event["event"] for entry in state.logs] == ["durable_event"]


def test_log_tail_skips_events_before_the_tui_session_boundary(
  tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
  state_dir = tmp_path / "state"
  state_dir.mkdir()
  replay = state_dir / "egia.jsonl"
  replay.write_text(
    json.dumps(_event("12:00:01Z", name="old_error", level="error")) + "\n",
    encoding="utf-8",
  )
  offset = replay_tail_offset(replay)
  with replay.open("a", encoding="utf-8") as fh:
    fh.write(json.dumps(_event("12:00:02Z", name="current_event")) + "\n")
  state = ConsoleState()

  class TailRead(Exception):
    """Stop the perpetual tail loop after its first read."""

  async def stop_after_read(_delay: float) -> None:
    raise TailRead

  monkeypatch.setattr("egia_ext.tui.feeds.asyncio.sleep", stop_after_read)
  with pytest.raises(TailRead):
    asyncio.run(
      log_tail(
        state,
        TuiConfig(state_dir=state_dir),
        initial_offset=offset,
      )
    )

  assert [entry.event["event"] for entry in state.logs] == ["current_event"]


def test_log_tail_detects_same_size_inode_rotation(
  tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
  state_dir = tmp_path / "state"
  state_dir.mkdir()
  replay = state_dir / "egia.jsonl"
  old_line = json.dumps(_event("12:00:01Z", name="old_segment")) + "\n"
  replay.write_text(old_line, encoding="utf-8")
  state = ConsoleState()

  class TailRead(Exception):
    """Stop after the replacement inode has been consumed."""

  calls = 0

  async def rotate_then_stop(_delay: float) -> None:
    nonlocal calls
    calls += 1
    if calls == 1:
      replay.rename(state_dir / "egia.jsonl.1")
      new_line = json.dumps(_event("12:00:02Z", name="new_segment")) + "\n"
      replay.write_text(new_line.ljust(len(old_line)), encoding="utf-8")
      return
    raise TailRead

  monkeypatch.setattr("egia_ext.tui.feeds.asyncio.sleep", rotate_then_stop)
  with pytest.raises(TailRead):
    asyncio.run(log_tail(state, TuiConfig(state_dir=state_dir)))

  assert [entry.event["event"] for entry in state.logs] == ["new_segment"]


def _write_session_metadata(
  config: TuiConfig, session_id: str, socket_path: Path
) -> Path:
  session_dir = config.run_dir / "island/sessions"
  session_dir.mkdir(parents=True, exist_ok=True)
  log_path = config.log_dir.parent / "island/sessions" / f"{session_id}.jsonl"
  log_path.parent.mkdir(parents=True, exist_ok=True)
  metadata = {
    "schema_version": 1,
    "session_id": session_id,
    "log_path": str(log_path),
    "socket_path": str(socket_path),
  }
  (session_dir / f"{session_id}.json").write_text(json.dumps(metadata))
  (session_dir / "current.json").write_text(json.dumps(metadata))
  return log_path


def test_session_replay_requires_live_collector_for_attach(tmp_path: Path) -> None:
  config = TuiConfig(log_dir=tmp_path / "log/egia", run_dir=tmp_path / "run")
  socket_path = tmp_path / "run/session.sock"
  log_path = _write_session_metadata(config, "session-a", socket_path)

  assert session_replay_path(config) is None
  assert session_replay_path(config, "session-a") == log_path
  socket_path.touch()
  assert session_replay_path(config) == log_path


def test_log_tail_reads_all_local_island_components_and_skips_malformed(
  tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
  config = TuiConfig(
    log_dir=tmp_path / "log/egia", run_dir=tmp_path / "run", log_level="INFO"
  )
  socket_path = tmp_path / "run/session.sock"
  socket_path.parent.mkdir(parents=True)
  socket_path.touch()
  log_path = _write_session_metadata(config, "session-a", socket_path)
  records = [
    {
      "level": "info",
      "component": component,
      "event": f"{component}_event",
      "collected_at": "2026-09-24T01:00:00Z",
    }
    for component in ("island", "eos", "egia", "tui")
  ]
  log_path.write_text(
    "\n".join(
      [
        *(json.dumps(record) for record in records),
        "not-json",
        json.dumps({"level": "error", "component": "remote", "event": "no"}),
      ]
    )
    + "\n",
    encoding="utf-8",
  )
  state = ConsoleState()

  class TailRead(Exception):
    pass

  async def stop_after_read(_delay: float) -> None:
    raise TailRead

  monkeypatch.setattr("egia_ext.tui.feeds.asyncio.sleep", stop_after_read)
  with pytest.raises(TailRead):
    asyncio.run(log_tail(state, config))

  assert [entry.event["event"] for entry in state.logs] == [
    "island_event",
    "eos_event",
    "egia_event",
    "tui_event",
  ]
  assert all(entry.event["timestamp"] == "2026-09-24T01:00:00Z" for entry in state.logs)


def test_attach_log_tail_switches_when_current_session_is_replaced(
  tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
  config = TuiConfig(log_dir=tmp_path / "log/egia", run_dir=tmp_path / "run")
  socket_a = tmp_path / "run/a.sock"
  socket_a.parent.mkdir(parents=True)
  socket_a.touch()
  first = _write_session_metadata(config, "session-a", socket_a)
  first.write_text(
    json.dumps({"level": "info", "component": "island", "event": "old"}) + "\n"
  )
  state = ConsoleState()
  calls = 0

  class TailRead(Exception):
    pass

  async def replace_then_stop(_delay: float) -> None:
    nonlocal calls
    calls += 1
    if calls == 1:
      socket_b = tmp_path / "run/b.sock"
      socket_b.touch()
      second = _write_session_metadata(config, "session-b", socket_b)
      second.write_text(
        json.dumps({"level": "warning", "component": "eos", "event": "new"}) + "\n"
      )
      return
    raise TailRead

  monkeypatch.setattr("egia_ext.tui.feeds.asyncio.sleep", replace_then_stop)
  with pytest.raises(TailRead):
    asyncio.run(log_tail(state, config))

  assert [entry.event["event"] for entry in state.logs] == ["new"]
