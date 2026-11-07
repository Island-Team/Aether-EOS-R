"""Collect one supervised Island launch into an ordered JSONL session log."""

from __future__ import annotations

import hashlib
import json
import logging
import logging.handlers
import os
import selectors
import socket
import struct
import threading
import uuid
from collections.abc import Mapping
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, Self

from island.cfg import LoggingConfig
from island.path import Layout

_MAX_DATAGRAM_BYTES = 60 * 1024
_RESERVED_FIELDS = frozenset(
  {
    "schema_version",
    "session_id",
    "collector_sequence",
    "collected_at",
    "component",
    "pid",
  }
)
_SECRET_PARTS = frozenset(
  {"authorization", "cookie", "credential", "password", "secret", "token", "api_key"}
)


def _timestamp() -> str:
  """Return a UTC RFC3339 timestamp suitable for JSON records."""
  return datetime.now(UTC).isoformat().replace("+00:00", "Z")


def _safe(value: object, *, key: str = "", depth: int = 0) -> object:
  """Bound and redact untrusted component fields before persistence."""
  parts = set(key.lower().replace("-", "_").split("_"))
  if parts & _SECRET_PARTS:
    return "<redacted>"
  if depth >= 6:
    return "<max-depth>"
  if isinstance(value, str):
    if value.lower().startswith(("http://", "https://", "ws://", "wss://", "data:")):
      return "<redacted-url>"
    return value[:1024] + ("…<truncated>" if len(value) > 1024 else "")
  if isinstance(value, dict):
    items = list(value.items())
    result = {
      str(item_key): _safe(item, key=str(item_key), depth=depth + 1)
      for item_key, item in items[:32]
    }
    if len(items) > 32:
      result["_truncated_fields"] = len(items) - 32
    return result
  if isinstance(value, list | tuple):
    items = list(value)
    result = [_safe(item, depth=depth + 1) for item in items[:32]]
    if len(items) > 32:
      result.append(f"<truncated:{len(items) - 32}>")
    return result
  if value is None or isinstance(value, bool | int | float):
    return value
  return _safe(str(value), key=key, depth=depth)


class _PlainRotatingHandler(logging.handlers.RotatingFileHandler):
  """Rotating handler that writes an already serialized JSON line."""

  def emit_line(self, line: str) -> None:
    """Write one line while retaining the handler's rotation behavior."""
    self.emit(logging.makeLogRecord({"msg": line, "levelno": logging.INFO}))


class _EosTail:
  """Incrementally follow EOS JSONL across truncation and replacement."""

  def __init__(self, path: Path) -> None:
    self.path = path
    self._stream: Any | None = None
    self._inode: tuple[int, int] | None = None
    self._offset = 0
    self._partial = b""
    self._guard_length = 0
    self._guard_digest = ""
    self._baseline = self._snapshot()

  def _snapshot(self) -> tuple[int, int, int, str] | None:
    try:
      stat = self.path.stat()
      with self.path.open("rb") as stream:
        prefix = stream.read(min(stat.st_size, 4096))
      return stat.st_dev, stat.st_ino, stat.st_size, hashlib.sha256(prefix).hexdigest()
    except OSError:
      return None

  def _open_current(self) -> None:
    try:
      stream = self.path.open("rb")
      stat = os.fstat(stream.fileno())
    except OSError:
      return
    offset = 0
    if self._baseline is not None:
      baseline_dev, baseline_inode, baseline_size, baseline_digest = self._baseline
      same_file = (stat.st_dev, stat.st_ino) == (baseline_dev, baseline_inode)
      stream.seek(0)
      current_prefix = stream.read(min(baseline_size, 4096))
      unchanged_prefix = hashlib.sha256(current_prefix).hexdigest() == baseline_digest
      if same_file and unchanged_prefix and stat.st_size >= baseline_size:
        offset = baseline_size
    stream.seek(offset)
    guard_length = min(stat.st_size, 4096)
    stream.seek(0)
    guard = stream.read(guard_length)
    self._stream = stream
    self._inode = (stat.st_dev, stat.st_ino)
    self._offset = offset
    self._guard_length = guard_length
    self._guard_digest = hashlib.sha256(guard).hexdigest()
    self._baseline = None

  def read(self) -> list[dict[str, object]]:
    """Return all newly completed EOS JSON records."""
    if self._stream is None:
      self._open_current()
    if self._stream is None:
      return []
    records: list[dict[str, object]] = []
    if self._guard_length:
      self._stream.seek(0)
      guard = self._stream.read(self._guard_length)
      if hashlib.sha256(guard).hexdigest() != self._guard_digest:
        self._offset = 0
        self._partial = b""
        self._guard_length = len(guard)
        self._guard_digest = hashlib.sha256(guard).hexdigest()
    self._stream.seek(self._offset)
    chunk = self._stream.read()
    self._offset = self._stream.tell()
    if chunk:
      data = self._partial + chunk
      lines = data.split(b"\n")
      self._partial = lines.pop()
      for raw in lines:
        try:
          value = json.loads(raw)
        except (UnicodeDecodeError, json.JSONDecodeError):
          records.append({"event": "eos_log_malformed", "level": "warning"})
          continue
        records.append(
          value if isinstance(value, dict) else {"event": "eos_log_malformed"}
        )

    try:
      current = self.path.stat()
    except OSError:
      current = None
    if current is not None and (current.st_dev, current.st_ino) != self._inode:
      self._stream.close()
      self._stream = None
      self._inode = None
      self._offset = 0
      self._partial = b""
      self._guard_length = 0
      self._guard_digest = ""
      records.extend(self.read())
    elif current is not None and current.st_size < self._offset:
      self._stream.seek(0)
      self._offset = 0
      self._partial = b""
      self._guard_length = 0
      self._guard_digest = ""
      records.extend(self.read())
    return records

  def close(self) -> None:
    """Close the followed inode after a final read."""
    if self._stream is not None:
      self._stream.close()
      self._stream = None


class SessionLogCollector:
  """Own a protected datagram endpoint and one normalized session replay."""

  def __init__(self, layout: Layout, config: LoggingConfig) -> None:
    self.layout = layout
    self.config = config
    self.session_id = str(uuid.uuid4())
    self.sessions_dir = layout.log / "island/sessions"
    self.runtime_dir = layout.run / "island/sessions"
    # Linux limits AF_UNIX paths to 107 bytes. Keep the endpoint directly
    # below the run root and use metadata for the full session identity.
    self.socket_path = layout.run / f"i-{self.session_id[:8]}.sock"
    self.log_path = self.sessions_dir / f"{self.session_id}.jsonl"
    self._socket: socket.socket | None = None
    self._handler: _PlainRotatingHandler | None = None
    self._thread: threading.Thread | None = None
    self._stop = threading.Event()
    self._lock = threading.Lock()
    self._sequence = 0
    self._components: dict[int, str] = {os.getpid(): "island"}
    self._tail = _EosTail(layout.component_log("eos") / "eos.jsonl")

  def start(self) -> None:
    """Create storage, socket, metadata, and the collector worker."""
    try:
      self.sessions_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
      self.runtime_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
      self._handler = _PlainRotatingHandler(
        self.log_path,
        encoding="utf-8",
        maxBytes=self.config.max_bytes,
        backupCount=self.config.backup_count,
      )
      self._handler.setFormatter(logging.Formatter("%(message)s"))
      sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
      sock.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
      sock.bind(str(self.socket_path))
      os.chmod(self.socket_path, 0o600)
      sock.setblocking(False)
      self._socket = sock
      self._write_metadata()
      self._prune_sessions()
      self._thread = threading.Thread(
        target=self._run, name="island-session-log", daemon=True
      )
      self._thread.start()
    except OSError as exc:
      self.close()
      raise RuntimeError(f"cannot initialize session logging: {exc}") from exc

  def register(self, pid: int, component: str) -> None:
    """Associate an owned child PID with its trusted component name."""
    self._components[pid] = component

  def environment(self, component: str) -> tuple[tuple[str, str], ...]:
    """Return session variables for one child process."""
    return (
      ("ISLAND_SESSION_ID", self.session_id),
      ("ISLAND_COMPONENT", component),
      ("ISLAND_SESSION_LOG_SOCKET", str(self.socket_path)),
    )

  def event(self, event: str, *, level: str = "info", **fields: object) -> None:
    """Persist one launcher-owned lifecycle event synchronously."""
    self._write(
      {"event": event, "level": level, **fields}, component="island", pid=os.getpid()
    )

  def _write(self, source: Mapping[str, object], *, component: str, pid: int) -> None:
    sanitized = {str(key): _safe(value, key=str(key)) for key, value in source.items()}
    for key in _RESERVED_FIELDS:
      sanitized.pop(key, None)
    source_timestamp = sanitized.pop(
      "timestamp", sanitized.pop("source_timestamp", None)
    )
    logger_name = str(sanitized.get("logger", ""))
    if logger_name.startswith("egia_ext.tui"):
      component = "tui"
    elif logger_name.startswith("egia.") and component == "unknown":
      component = "egia"
    with self._lock:
      self._sequence += 1
      record = {
        **sanitized,
        "schema_version": 1,
        "session_id": self.session_id,
        "collector_sequence": self._sequence,
        "collected_at": _timestamp(),
        "component": component,
        "pid": pid,
      }
      if source_timestamp is not None:
        record["source_timestamp"] = source_timestamp
      if self._handler is not None:
        self._handler.emit_line(
          json.dumps(record, ensure_ascii=False, separators=(",", ":"))
        )

  def _run(self) -> None:
    selector = selectors.DefaultSelector()
    if self._socket is not None:
      selector.register(self._socket, selectors.EVENT_READ)
    while not self._stop.is_set():
      for _key, _mask in selector.select(timeout=0.1):
        self._receive()
      for record in self._tail.read():
        self._write(record, component="eos", pid=0)
    self._receive()
    for record in self._tail.read():
      self._write(record, component="eos", pid=0)
    selector.close()

  def _receive(self) -> None:
    if self._socket is None:
      return
    while True:
      try:
        data, ancillary, _flags, _address = self._socket.recvmsg(
          _MAX_DATAGRAM_BYTES, socket.CMSG_SPACE(struct.calcsize("3i"))
        )
      except BlockingIOError:
        return
      except OSError:
        return
      pid = 0
      for level, kind, value in ancillary:
        if level == socket.SOL_SOCKET and kind == socket.SCM_CREDENTIALS:
          pid, _uid, _gid = struct.unpack("3i", value[: struct.calcsize("3i")])
      try:
        payload = json.loads(data)
      except (UnicodeDecodeError, json.JSONDecodeError):
        payload = {"event": "session_datagram_malformed", "level": "warning"}
      if not isinstance(payload, dict):
        payload = {"event": "session_datagram_malformed", "level": "warning"}
      self._write(payload, component=self._components.get(pid, "unknown"), pid=pid)

  def _write_metadata(self) -> None:
    metadata = {
      "schema_version": 1,
      "session_id": self.session_id,
      "log_path": str(self.log_path),
      "socket_path": str(self.socket_path),
      "started_at": _timestamp(),
    }
    session_meta = self.runtime_dir / f"{self.session_id}.json"
    self._atomic_json(session_meta, metadata)
    self._atomic_json(self.runtime_dir / "current.json", metadata)

  @staticmethod
  def _atomic_json(path: Path, value: dict[str, object]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, sort_keys=True) + "\n", encoding="utf-8")
    os.chmod(temporary, 0o600)
    temporary.replace(path)

  def _prune_sessions(self) -> None:
    paths = sorted(
      self.sessions_dir.glob("*.jsonl"), key=lambda item: item.stat().st_mtime
    )
    stale = paths[: max(0, len(paths) - self.config.retained_sessions)]
    for path in stale:
      path.unlink(missing_ok=True)
      for index in range(1, self.config.backup_count + 1):
        Path(f"{path}.{index}").unlink(missing_ok=True)

  def close(self) -> None:
    """Drain pending events and remove the process-lifetime socket."""
    self._stop.set()
    if self._thread is not None:
      self._thread.join(timeout=2)
      self._thread = None
    self._tail.close()
    if self._socket is not None:
      self._socket.close()
      self._socket = None
    self.socket_path.unlink(missing_ok=True)
    if self._handler is not None:
      self._handler.close()
      self._handler = None

  def __enter__(self) -> Self:
    self.start()
    return self

  def __exit__(self, _type: object, _value: object, _traceback: object) -> None:
    self.close()
