"""Closed, bounded IsOS stderr projection of EGIA state transitions.

The private structlog replay remains independent. Callers provide only a reviewed
code and its typed scalar fields; arbitrary log records never enter this sink.
"""

from __future__ import annotations

import json
import re
import sys
import threading
import time
import uuid
from collections import OrderedDict
from datetime import UTC, datetime
from typing import TextIO

from egia.lib import shared_event_policy

_ID = re.compile(r"[A-Za-z0-9_-]{1,64}\Z")
_MANAGED = re.compile(r"r[A-Za-z0-9_-]{1,63}\Z")
_NODE = re.compile(
  r"/cores/egia/layers/[A-Za-z0-9_-]+/groups/[A-Za-z0-9_-]+/nodes/[A-Za-z0-9_-]+\Z"
)
_MAX_RAW = 8064
_MAX_FIELDS = 4096
_MAX_SUPPRESSED = 4_294_967_295
_WINDOW = 60.0
_CAPACITY = 256
_ONCE = frozenset(
  {"egia_starting", "egia_ready", "egia_start_failed", "egia_stopping", "egia_stopped"}
)


def valid_opaque_id(value: object) -> bool:
  """Check the exact shared-event identity grammar before EOS egress."""
  return type(value) is str and _ID.fullmatch(value) is not None


def select_run_id(managed: bool, binding: str | None) -> str:
  """Validate the supervisor binding before any shared event can be emitted."""
  if managed:
    if binding is None or _MANAGED.fullmatch(binding) is None:
      raise ValueError("invalid managed run binding")
    return binding
  if binding is not None:
    raise ValueError("unexpected manager run binding")
  return "l" + uuid.uuid4().hex


def _typed(value: object, kind: str) -> bool:
  spec = shared_event_policy.TYPES[kind]
  if isinstance(spec, tuple):
    return type(value) is str and value in spec
  if kind == "positive_integer":
    return type(value) is int and 1 <= value <= _MAX_SUPPRESSED
  return type(value) is str and _ID.fullmatch(value) is not None


class EventSink:
  """One synchronized producer with a bounded noncritical repetition table."""

  def __init__(self, run_id: str, stream: TextIO | None = None) -> None:
    if _ID.fullmatch(run_id) is None:
      raise ValueError("invalid run ID")
    self.run_id = run_id
    self.stream = stream if stream is not None else sys.stderr
    self._lock = threading.RLock()
    self._repeats: OrderedDict[
      tuple[str, str | None, str | None, str | None], tuple[float, int]
    ] = OrderedDict()
    self.write_failures = 0
    self.ready_emitted = False
    self._emitted_once: set[str] = set()
    self._uncertain_operations: set[str] = set()
    self._recovery_unconfirmed = False

  def emit(
    self,
    code: str,
    *,
    fields: dict[str, object] | None = None,
    node_path: str | None = None,
    correlation_id: str | None = None,
    admitted_nodes: frozenset[str] | None = None,
    now: float | None = None,
  ) -> bool:
    """Emit a reviewed event. Invalid candidates fail closed without writing."""
    if not isinstance(code, str):
      return False
    rule = shared_event_policy.CODES.get(code)
    if rule is None:
      return False
    if fields is not None and not isinstance(fields, dict):
      return False
    supplied = fields or {}
    allowed = rule["fields"]
    required = {key for key in allowed if not key.endswith("?")}
    normalized = {key.removesuffix("?"): kind for key, kind in allowed.items()}
    if (
      not required.issubset(supplied)
      or not supplied.keys() <= normalized.keys()
      or len(supplied) > 12
      or any(not _typed(value, normalized[key]) for key, value in supplied.items())
    ):
      return False
    if node_path is not None and (
      not isinstance(node_path, str)
      or rule["node_path"] == "null"
      or _NODE.fullmatch(node_path) is None
      or len(node_path.encode()) > 512
      or admitted_nodes is None
      or node_path not in admitted_nodes
    ):
      return False
    if correlation_id is not None and (
      not isinstance(correlation_id, str) or _ID.fullmatch(correlation_id) is None
    ):
      return False
    level = rule.get("level")
    if "level_by_outcome" in rule:
      level = rule["level_by_outcome"].get(supplied.get("outcome"))
    if "level_by_state" in rule:
      level = rule["level_by_state"].get(supplied.get("state"))
    if level is None:
      return False
    body = {
      "timestamp": datetime.now(UTC)
      .isoformat(timespec="milliseconds")
      .replace("+00:00", "Z"),
      "level": level,
      "source": "egia",
      "core": "egia",
      "node_path": node_path,
      "event_code": code,
      "run_id": self.run_id,
      "correlation_id": correlation_id,
      "message": rule["message"],
      "fields": supplied,
    }
    if (
      len(json.dumps(supplied, ensure_ascii=False, separators=(",", ":")).encode())
      > _MAX_FIELDS
    ):
      return False
    line = json.dumps(body, ensure_ascii=False, separators=(",", ":")) + "\n"
    if len(line.encode()) > _MAX_RAW:
      return False
    with self._lock:
      if code in _ONCE and code in self._emitted_once:
        return False
      if code == "egia_operation_unknown":
        self._uncertain_operations.add(str(supplied["operation_id"]))
      elif code == "egia_operation_terminal":
        self._uncertain_operations.discard(str(supplied["operation_id"]))
      elif code == "egia_recovery_required" and supplied["reason_code"] in {
        "unknown_outcome",
        "checkpoint_incomplete",
      }:
        self._recovery_unconfirmed = True
      current = now if now is not None else time.monotonic()
      repeat_key: tuple[str, str | None, str | None, str | None] | None = None
      if code == "egia_safety_rejected":
        # The first refusal for each admitted request must survive even when
        # another request fails for the same reason in this window.
        repeat_key = (code, node_path, str(supplied.get("reason_code")), correlation_id)
        previous = self._repeats.get(repeat_key)
        if previous is not None and current - previous[0] < _WINDOW:
          self._repeats[repeat_key] = (
            previous[0],
            min(previous[1] + 1, _MAX_SUPPRESSED),
          )
          return True
        if previous is not None and previous[1]:
          self._summary(previous[1])
        elif len(self._repeats) >= _CAPACITY:
          _, (_, count) = self._repeats.popitem(last=False)
          if count:
            self._summary(count)
        self._repeats[repeat_key] = (current, 0)
      written = self._write(line)
      if written:
        if code in _ONCE:
          self._emitted_once.add(code)
        if code == "egia_ready":
          self.ready_emitted = True
      elif repeat_key is not None:
        self._repeats.pop(repeat_key, None)
      return written

  def _summary(self, count: int) -> None:
    if count:
      self.emit("egia_events_suppressed", fields={"suppressed_count": count})

  def flush(self) -> None:
    with self._lock:
      pending = [count for _, count in self._repeats.values() if count]
      self._repeats.clear()
      for count in pending:
        self._summary(count)

  def _write(self, line: str) -> bool:
    try:
      self.stream.write(line)
      self.stream.flush()
    except (OSError, ValueError):
      self.write_failures = min(self.write_failures + 1, _MAX_SUPPRESSED)
      return False
    return True

  @property
  def stop_unconfirmed(self) -> bool:
    return self._recovery_unconfirmed or bool(self._uncertain_operations)


_sink: EventSink | None = None


def configure(run_id: str, stream: TextIO | None = None) -> EventSink:
  global _sink
  _sink = EventSink(run_id, stream)
  return _sink


def emit(code: str, **kwargs: object) -> bool:
  if _sink is None:
    return False
  return _sink.emit(code, **kwargs)  # type: ignore[arg-type]


def flush() -> None:
  if _sink is not None:
    _sink.flush()


def configured() -> bool:
  return _sink is not None


def ready_emitted() -> bool:
  return _sink is not None and _sink.ready_emitted


def stop_unconfirmed() -> bool:
  return _sink is not None and _sink.stop_unconfirmed
