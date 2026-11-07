"""Safe structured logging, bounded retention, and strict configuration."""

from __future__ import annotations

import json
import logging
import os
import time
from pathlib import Path

import pytest
from pydantic import ValidationError

from egia.lib import logger as logger_mod
from egia.lib.config import LoggingConfig
from egia.lib.logger import bind_log_context, configure_logging, get_logger
from island.cfg import LoggingConfig as IslandLoggingConfig
from island.path import Layout, Mode
from island.session_log import SessionLogCollector


@pytest.fixture()
def logging_state(tmp_path: Path):
  """Restore global logging state after one configured-logger test."""
  saved_handlers = logging.getLogger().handlers[:]
  saved_level = logging.getLogger().level
  logger_mod._configured = False
  logger_mod._console_handler = None
  yield tmp_path
  for handler in logging.getLogger().handlers[:]:
    handler.close()
    logging.getLogger().removeHandler(handler)
  logging.getLogger().handlers = saved_handlers
  logging.getLogger().setLevel(saved_level)
  logger_mod._configured = False
  logger_mod._log_context.set(None)


def _config(**updates: object) -> LoggingConfig:
  config = LoggingConfig()
  return config.model_copy(
    update={
      "console": config.console.model_copy(update={"enabled": False}),
      **updates,
    }
  )


def _events(root: Path) -> list[dict[str, object]]:
  return [json.loads(line) for line in (root / "egia.jsonl").read_text().splitlines()]


def test_logging_config_rejects_unknown_root_and_sink_fields() -> None:
  with pytest.raises(ValidationError, match="unexpected"):
    LoggingConfig.model_validate({"unexpected": True})
  with pytest.raises(ValidationError, match="unexpected"):
    LoggingConfig.model_validate({"replay": {"unexpected": True}})


def test_sanitizer_recurses_redacts_and_bounds_values() -> None:
  safe = logger_mod._sanitize_event(
    None,
    "info",
    {
      "event": "probe",
      "prompt": "private prompt",
      "nested": {
        "api_key": "sk-private",
        "target": "https://user:pass@example.test/path?token=private",
        "data": b"private bytes",
      },
      "long": "x" * 2000,
      "many": list(range(40)),
    },
  )
  assert safe["prompt"] == "<excluded>"
  assert safe["nested"] == {
    "api_key": "<redacted>",
    "target": "<redacted-url>",
    "data": {"type": "bytes", "bytes": 13},
  }
  assert str(safe["long"]).endswith("…<truncated>")
  assert len(safe["many"]) == 33
  assert safe["many"][-1] == "<truncated:8>"


def test_run_and_lifecycle_context_are_written_and_nested(logging_state: Path) -> None:
  replay = logging_state / "state"
  configure_logging(logging_state / "log", _config(), replay_dir=replay)
  with bind_log_context(
    mission_id="mission-1",
    step_id="step-2",
    backend="robot-a",
    command_id="command-3",
    execution_id="execution-4",
  ):
    get_logger("egia.core.test").warning("context_probe")
  event = _events(replay)[0]
  assert isinstance(event["run_id"], str) and event["run_id"]
  assert event["mission_id"] == "mission-1"
  assert event["step_id"] == "step-2"
  assert event["backend"] == "robot-a"
  assert event["command_id"] == "command-3"
  assert event["execution_id"] == "execution-4"
  with (
    pytest.raises(ValueError, match="unknown logging context fields"),
    bind_log_context(unsafe="value"),
  ):
    pass


def test_foreign_websockets_log_keeps_logger_name_without_formatter_error(
  logging_state: Path, capsys: pytest.CaptureFixture[str]
) -> None:
  replay = logging_state / "state"
  configure_logging(logging_state / "log", _config(), replay_dir=replay)
  logging.getLogger("websockets.server").info(
    "server listening on %s", "unix:///tmp/eos.sock"
  )
  assert "Logging error" not in capsys.readouterr().err
  event = _events(replay)[0]
  assert event["logger"] == "websockets.server"
  assert event["event"] == "server listening on unix:///tmp/eos.sock"
  assert "_record" not in event
  assert "_from_structlog" not in event


def test_dedup_index_is_bounded_and_terminal_events_bypass_it() -> None:
  suppressor = logger_mod._RepeatSuppressor(60.0, capacity=2)
  assert suppressor.filter(_record("one"))
  assert suppressor.filter(_record("two"))
  assert suppressor.filter(_record("three"))
  assert len(suppressor._states) == 2
  terminal = _record("command_terminal", status="failed")
  assert suppressor.filter(terminal)
  assert suppressor.filter(_record("command_terminal", status="failed"))


def _record(event: str, **fields: object) -> logging.LogRecord:
  return logging.LogRecord(
    "egia.core.test",
    logging.WARNING,
    __file__,
    1,
    {"event": event, **fields},
    (),
    None,
  )


def test_handler_close_writes_final_repeat_summary(logging_state: Path) -> None:
  replay = logging_state / "state"
  configure_logging(logging_state / "log", _config(), replay_dir=replay)
  log = get_logger("egia.core.test")
  for _ in range(4):
    log.warning("repeat_at_shutdown")
  handlers = logging.getLogger().handlers[:]
  for handler in handlers:
    handler.close()
    logging.getLogger().removeHandler(handler)
  events = [item for item in _events(replay) if item["event"] == "repeat_at_shutdown"]
  assert len(events) == 2
  assert events[-1]["repeats"] == 4
  assert events[-1]["dedup_summary"] is True


def test_replay_rotates_and_stale_human_logs_are_removed(logging_state: Path) -> None:
  stale = logging_state / "log" / "core" / "retired.log"
  stale.parent.mkdir(parents=True)
  stale.write_text("stale", encoding="utf-8")
  replay = logging_state / "state"
  configure_logging(
    logging_state / "log",
    _config(
      replay_max_bytes=1024,
      replay_backup_count=5,
      dedup=False,
      files=LoggingConfig().files.model_copy(update={"overwrite": True}),
    ),
    replay_dir=replay,
  )
  log = get_logger("egia.core.test")
  for index in range(12):
    log.warning("rotation_probe", index=index, detail="x" * 700)
  assert not stale.exists()
  assert (replay / "egia.jsonl.1").is_file()
  assert len(list(replay.glob("egia.jsonl.*"))) <= 5


def test_replay_disabled_child_does_not_remove_parent_logs(logging_state: Path) -> None:
  parent_log = logging_state / "log" / "main.log"
  parent_log.parent.mkdir(parents=True)
  parent_log.write_text("parent is live", encoding="utf-8")
  defaults = LoggingConfig()
  configure_logging(
    logging_state / "log",
    _config(
      replay=defaults.replay.model_copy(update={"enabled": False}),
      files=defaults.files.model_copy(update={"overwrite": True}),
    ),
    replay_dir=logging_state / "state",
  )
  assert parent_log.read_text(encoding="utf-8") == "parent is live"


def test_logging_forwards_sanitized_events_to_island_session(
  logging_state: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
  layout = Layout.resolve(mode=Mode.SOURCE, prefix=logging_state, environ={})
  with SessionLogCollector(layout, IslandLoggingConfig()) as collector:
    collector.register(os.getpid(), "egia")
    monkeypatch.setenv("ISLAND_SESSION_LOG_SOCKET", str(collector.socket_path))
    configure_logging(
      logging_state / "log", _config(), replay_dir=logging_state / "state"
    )
    get_logger("egia.core.test").warning(
      "forwarded_probe", password="private", mission_id="mission-1"
    )
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
      if (
        collector.log_path.is_file()
        and '"event":"forwarded_probe"'
        in collector.log_path.read_text(encoding="utf-8")
      ):
        break
      time.sleep(0.02)
    else:
      raise AssertionError("structured event was not forwarded")

  event = json.loads(collector.log_path.read_text(encoding="utf-8").splitlines()[0])
  assert event["component"] == "egia"
  assert event["event"] == "forwarded_probe"
  assert event["password"] == "<redacted>"
  assert event["mission_id"] == "mission-1"
  assert event["run_id"]
