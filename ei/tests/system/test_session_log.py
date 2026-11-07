"""Island-wide session log collection, normalization, and EOS following."""

from __future__ import annotations

import json
import os
import socket
import time
from pathlib import Path

from island.cfg import LoggingConfig
from island.path import Layout, Mode
from island.session_log import SessionLogCollector


def _layout(tmp_path: Path) -> Layout:
  """Return one isolated source-style layout."""
  return Layout.resolve(mode=Mode.SOURCE, prefix=tmp_path, environ={})


def _wait_for(path: Path, event: str) -> None:
  """Wait briefly for one asynchronous event to reach the session file."""
  deadline = time.monotonic() + 2
  while time.monotonic() < deadline:
    if path.is_file() and f'"event":"{event}"' in path.read_text(encoding="utf-8"):
      return
    time.sleep(0.02)
  raise AssertionError(f"event {event!r} was not collected")


def test_collector_normalizes_datagrams_and_overrides_reserved_fields(
  tmp_path: Path,
) -> None:
  layout = _layout(tmp_path)
  with SessionLogCollector(layout, LoggingConfig()) as collector:
    collector.register(os.getpid(), "egia")
    sender = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    sender.sendto(
      json.dumps(
        {
          "event": "egia_ready",
          "timestamp": "2026-09-24T01:02:03Z",
          "session_id": "forged",
          "component": "forged",
          "password": "do-not-store",
        }
      ).encode(),
      str(collector.socket_path),
    )
    sender.close()
    _wait_for(collector.log_path, "egia_ready")

  record = json.loads(collector.log_path.read_text(encoding="utf-8").splitlines()[0])
  assert record["session_id"] == collector.session_id
  assert record["component"] == "egia"
  assert record["pid"] == os.getpid()
  assert record["source_timestamp"] == "2026-09-24T01:02:03Z"
  assert record["password"] == "<redacted>"
  assert record["collector_sequence"] == 1
  assert not collector.socket_path.exists()


def test_collector_tails_only_new_eos_records_and_survives_replacement(
  tmp_path: Path,
) -> None:
  layout = _layout(tmp_path)
  eos_log = layout.component_log("eos") / "eos.jsonl"
  eos_log.parent.mkdir(parents=True)
  eos_log.write_text('{"event":"old"}\n', encoding="utf-8")
  with SessionLogCollector(layout, LoggingConfig()) as collector:
    with eos_log.open("a", encoding="utf-8") as stream:
      stream.write('{"event":"new"}\n')
    _wait_for(collector.log_path, "new")
    rotated = eos_log.with_suffix(".jsonl.1")
    eos_log.replace(rotated)
    eos_log.write_text('{"event":"replacement"}\n', encoding="utf-8")
    _wait_for(collector.log_path, "replacement")
    eos_log.write_text(
      '{"event":"rapid-regrowth","detail":"xxxxxxxxxxxxxxxxxxxxxxxx"}\n',
      encoding="utf-8",
    )
    _wait_for(collector.log_path, "rapid-regrowth")

  events = [
    json.loads(line)["event"]
    for line in collector.log_path.read_text(encoding="utf-8").splitlines()
  ]
  assert events == ["new", "replacement", "rapid-regrowth"]


def test_collector_publishes_metadata_and_prunes_old_sessions(tmp_path: Path) -> None:
  layout = _layout(tmp_path)
  sessions = layout.log / "island/sessions"
  sessions.mkdir(parents=True)
  for index in range(3):
    stale = sessions / f"old-{index}.jsonl"
    stale.write_text("{}\n", encoding="utf-8")
    os.utime(stale, (index + 1, index + 1))

  with SessionLogCollector(
    layout, LoggingConfig(max_bytes=1024, backup_count=2, retained_sessions=2)
  ) as collector:
    current = json.loads(
      (layout.run / "island/sessions/current.json").read_text(encoding="utf-8")
    )
    assert current["session_id"] == collector.session_id
    assert current["log_path"] == str(collector.log_path)
    assert oct(collector.socket_path.stat().st_mode & 0o777) == "0o600"

  assert {path.name for path in sessions.glob("*.jsonl")} == {
    "old-2.jsonl",
    f"{collector.session_id}.jsonl",
  }
