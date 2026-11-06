"""File-sink dedup in egia.lib.logger — repeats collapse into counted lines."""

from __future__ import annotations

import json
import logging
from pathlib import Path

import pytest

from egia.lib import logger as logger_mod
from egia.lib.config import LoggingConfig
from egia.lib.logger import configure_logging, get_logger

WINDOW_S = 0.05


@pytest.fixture()
def configured(tmp_path: Path):
  """Configure distinct state and diagnostic roots with a tiny window."""
  saved_handlers = logging.getLogger().handlers[:]
  saved_level = logging.getLogger().level
  logger_mod._configured = False
  logger_mod._console_handler = None
  cfg = LoggingConfig()
  cfg = cfg.model_copy(
    update={
      "dedup": True,
      "dedup_window_s": WINDOW_S,
      "console": cfg.console.model_copy(update={"enabled": False}),
    }
  )
  replay_dir = tmp_path / "state"
  log_dir = tmp_path / "log"
  configure_logging(log_dir, cfg, replay_dir=replay_dir)
  yield replay_dir, log_dir
  for h in logging.getLogger().handlers[:]:
    h.close()
    logging.getLogger().removeHandler(h)
  logging.getLogger().handlers = saved_handlers
  logging.getLogger().setLevel(saved_level)
  logger_mod._configured = False


def _jsonl(replay_dir: Path) -> list[dict]:
  lines = (replay_dir / "egia.jsonl").read_text().splitlines()
  return [json.loads(line) for line in lines]


def test_repeats_inside_window_collapse_to_one_line(
  configured: tuple[Path, Path],
) -> None:
  replay_dir, _ = configured
  log = get_logger("egia.core.test")
  for _ in range(5):
    log.warning("queue_full", kind="hearing")
  events = [e for e in _jsonl(replay_dir) if e["event"] == "queue_full"]
  assert len(events) == 1
  assert "repeats" not in events[0]


def test_first_occurrence_after_window_carries_repeats(
  configured: tuple[Path, Path],
) -> None:
  replay_dir, _ = configured
  log = get_logger("egia.core.test")
  for _ in range(5):
    log.warning("queue_full")
  import time

  time.sleep(WINDOW_S + 0.02)
  log.warning("queue_full")
  events = [e for e in _jsonl(replay_dir) if e["event"] == "queue_full"]
  assert len(events) == 2
  assert events[1]["repeats"] == 5  # 4 suppressed + this one


def test_distinct_events_are_not_suppressed(
  configured: tuple[Path, Path],
) -> None:
  replay_dir, _ = configured
  log = get_logger("egia.core.test")
  log.warning("queue_full")
  log.warning("link_down")
  log.error("queue_full")  # different level = different event
  events = [e["event"] for e in _jsonl(replay_dir)]
  assert sorted(events) == ["link_down", "queue_full", "queue_full"]


def test_human_files_dedup_too(configured: tuple[Path, Path]) -> None:
  replay_dir, log_dir = configured
  log = get_logger("egia.core.test")
  for _ in range(3):
    log.warning("queue_full")
  text = (log_dir / "core" / "test.log").read_text()
  assert text.count("queue_full") == 1
  assert (replay_dir / "egia.jsonl").is_file()
  assert not (log_dir / "egia.jsonl").exists()


@pytest.mark.parametrize(
  ("replay_enabled", "files_enabled"),
  ((True, False), (False, True)),
)
def test_sink_roots_are_created_only_for_their_lifecycle(
  tmp_path: Path,
  replay_enabled: bool,
  files_enabled: bool,
) -> None:
  saved_handlers = logging.getLogger().handlers[:]
  saved_level = logging.getLogger().level
  logger_mod._configured = False
  logger_mod._console_handler = None
  cfg = LoggingConfig()
  cfg = cfg.model_copy(
    update={
      "console": cfg.console.model_copy(update={"enabled": False}),
      "replay": cfg.replay.model_copy(update={"enabled": replay_enabled}),
      "files": cfg.files.model_copy(update={"enabled": files_enabled}),
    }
  )
  replay_dir = tmp_path / "state"
  log_dir = tmp_path / "log"
  configure_logging(log_dir, cfg, replay_dir=replay_dir)
  try:
    get_logger("egia.core.test").warning("lifecycle_probe")
  finally:
    for handler in logging.getLogger().handlers[:]:
      handler.close()
      logging.getLogger().removeHandler(handler)
    logging.getLogger().handlers = saved_handlers
    logging.getLogger().setLevel(saved_level)
    logger_mod._configured = False

  assert replay_dir.exists() is replay_enabled
  # Human diagnostics are mandatory even for programmatically constructed
  # configs that bypass Pydantic validation via model_copy.
  assert log_dir.exists()


def test_replay_append_preserves_existing_events(tmp_path: Path) -> None:
  saved_handlers = logging.getLogger().handlers[:]
  saved_level = logging.getLogger().level
  logger_mod._configured = False
  logger_mod._console_handler = None
  replay_dir = tmp_path / "state"
  replay_dir.mkdir()
  (replay_dir / "egia.jsonl").write_text(
    '{"event": "previous_run", "level": "info"}\n',
    encoding="utf-8",
  )
  cfg = LoggingConfig()
  cfg = cfg.model_copy(
    update={
      "console": cfg.console.model_copy(update={"enabled": False}),
      "files": cfg.files.model_copy(update={"enabled": False}),
      "replay": cfg.replay.model_copy(update={"overwrite": False}),
    }
  )
  configure_logging(tmp_path / "log", cfg, replay_dir=replay_dir)
  try:
    get_logger("egia.core.test").warning("current_run")
  finally:
    for handler in logging.getLogger().handlers[:]:
      handler.close()
      logging.getLogger().removeHandler(handler)
    logging.getLogger().handlers = saved_handlers
    logging.getLogger().setLevel(saved_level)
    logger_mod._configured = False

  assert [event["event"] for event in _jsonl(replay_dir)] == [
    "previous_run",
    "current_run",
  ]


def test_dedup_disabled_keeps_every_line(tmp_path: Path) -> None:
  saved_handlers = logging.getLogger().handlers[:]
  logger_mod._configured = False
  logger_mod._console_handler = None
  cfg = LoggingConfig()
  cfg = cfg.model_copy(
    update={
      "dedup": False,
      "console": cfg.console.model_copy(update={"enabled": False}),
    }
  )
  replay_dir = tmp_path / "state"
  configure_logging(tmp_path / "log", cfg, replay_dir=replay_dir)
  try:
    log = get_logger("egia.core.test")
    for _ in range(5):
      log.warning("queue_full")
  finally:
    for h in logging.getLogger().handlers[:]:
      h.close()
      logging.getLogger().removeHandler(h)
    logging.getLogger().handlers = saved_handlers
    logger_mod._configured = False
  events = [e for e in _jsonl(replay_dir) if e["event"] == "queue_full"]
  assert len(events) == 5
