"""Human-sink renderers in egia.lib.logger — one event is always one line."""

from __future__ import annotations

import logging
from pathlib import Path

from egia.lib import logger as logger_mod


def _event_dict() -> dict:
  return {
    "event": "first_context_snapshot",
    "level": "info",
    "logger": "egia.core.context",
    "timestamp": "2026-08-18T10:00:00Z",
    "prompt": "Preamble.\nno data received\nno map data received",
    "observation_ids": [],
  }


def test_render_file_preserves_multiline_prompt() -> None:
  rendered = logger_mod._render_file(None, None, _event_dict())
  lines = rendered.splitlines()
  assert len(lines) == 3
  assert "prompt=Preamble.\nno data received" in rendered
  assert rendered.startswith(
    "[aether-egia]INFO@egia.core.context[2026-08-18T10:00:00Z]:"
  )


def test_render_console_preserves_multiline_prompt() -> None:
  rendered = logger_mod._render_console(None, None, _event_dict())
  assert len(rendered.splitlines()) == 3
  assert "\nno data received" in rendered


def test_render_console_colors_warning_and_error(monkeypatch) -> None:
  monkeypatch.setattr(logger_mod, "_CONSOLE_COLOR", True)
  warning = logger_mod._render_console(
    None, None, {"event": "warn", "level": "warning", "logger": "x", "timestamp": "-"}
  )
  error = logger_mod._render_console(
    None, None, {"event": "err", "level": "error", "logger": "x", "timestamp": "-"}
  )
  assert "\x1b[33mWARNING\x1b[0m" in warning
  assert "\x1b[31mERROR\x1b[0m" in error
  assert "\x1b[33mWARNING\x1b[0m\x1b[2m@" in warning


def test_render_file_normalizes_crlf_and_cr() -> None:
  event = {"event": "e", "level": "info", "logger": "egia.core", "timestamp": "-"}
  event["text"] = "a\r\nb\rc"
  rendered = logger_mod._render_file(None, None, event)
  assert "\r" not in rendered
  assert "a\nb\nc" in rendered


def test_optional_extension_logs_have_their_own_file() -> None:
  root = Path("/tmp/logs")
  assert logger_mod._human_log_path(root, "egia_ext.tui") == root / "ext" / "tui.log"


def test_optional_inner_logs_have_their_own_file() -> None:
  root = Path("/tmp/logs")
  assert logger_mod._human_log_path(root, "egia.opt.sandbox") == (
    root / "opt" / "sandbox.log"
  )


def _record(message: dict) -> logging.LogRecord:
  return logging.LogRecord(
    "egia.core.decision.model", logging.INFO, __file__, 1, message, (), None
  )


def test_repeat_suppressor_keeps_distinct_event_payloads() -> None:
  suppressor = logger_mod._RepeatSuppressor(60.0)
  first = _record(
    {"event": "model_completion", "prompt_tokens": 100, "timestamp": "t1"}
  )
  same = _record({"event": "model_completion", "prompt_tokens": 100, "timestamp": "t2"})
  changed = _record(
    {"event": "model_completion", "prompt_tokens": 101, "timestamp": "t3"}
  )

  assert suppressor.filter(first) is True
  assert suppressor.filter(same) is False
  assert suppressor.filter(changed) is True


def test_repeat_suppressor_ignores_poll_sequence_and_raw_json() -> None:
  suppressor = logger_mod._RepeatSuppressor(60.0)
  first = _record(
    {
      "event": "stream_frame",
      "backend": "sandbox",
      "uri": "scene/state",
      "kind": "scene",
      "bytes": 1000,
      "payload_sequence": 7,
      "sequence": 1,
      "json": '{"timestamp":"t1"}',
    }
  )
  unchanged_poll = _record(
    {
      **first.msg,
      "sequence": 2,
      "json": '{"timestamp":"t2"}',
    }
  )
  changed_payload = _record({**first.msg, "payload_sequence": 8, "sequence": 3})

  assert suppressor.filter(first) is True
  assert suppressor.filter(unchanged_poll) is False
  assert suppressor.filter(changed_payload) is True
