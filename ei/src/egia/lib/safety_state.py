"""Owner-only, crash-durable EGIA emergency latch and reset authorization."""

from __future__ import annotations

import fcntl
import json
import os
import re
import stat
import uuid
from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path


class SafetyStateError(RuntimeError):
  """Unsafe or malformed durable safety state; callers must fail closed."""


_ALIAS = re.compile(r"[A-Za-z0-9_-]{1,64}\Z")
_NONCE = re.compile(r"[0-9a-f]{32}\Z")
_REVISION = re.compile(r"[0-9a-f]{64}\Z")


@dataclass(frozen=True)
class ResetAuthorization:
  """One consumed operator intent, bound to one latch episode and graph."""

  episode_id: str
  graph_revision: str
  identities: dict[str, str]
  nonce: str


def _directory(root: Path) -> None:
  root.mkdir(mode=0o700, parents=True, exist_ok=True)
  info = root.lstat()
  if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
    raise SafetyStateError("EGIA safety state directory is unsafe")
  if stat.S_IMODE(info.st_mode) & 0o077:
    raise SafetyStateError("EGIA safety state directory is not owner-only")


def _read(path: Path) -> dict[str, object] | None:
  try:
    info = path.lstat()
  except FileNotFoundError:
    return None
  if (
    not stat.S_ISREG(info.st_mode)
    or info.st_uid != os.getuid()
    or stat.S_IMODE(info.st_mode) != 0o600
  ):
    raise SafetyStateError("EGIA safety state file is unsafe")
  fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
  try:
    opened = os.fstat(fd)
    if (opened.st_dev, opened.st_ino) != (info.st_dev, info.st_ino):
      raise SafetyStateError("EGIA safety state file changed during read")
    raw = os.read(fd, 8193)
  finally:
    os.close(fd)
  if len(raw) > 8192:
    raise SafetyStateError("EGIA safety state file is oversized")
  try:
    value = json.loads(raw)
  except (UnicodeError, json.JSONDecodeError) as exc:
    raise SafetyStateError("EGIA safety state file is malformed") from exc
  if type(value) is not dict:
    raise SafetyStateError("EGIA safety state file is malformed")
  return value


def _write(path: Path, value: dict[str, object]) -> None:
  _directory(path.parent)
  raw = (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()
  if len(raw) > 8192:
    raise SafetyStateError("EGIA safety state file is oversized")
  temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
  fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
  try:
    with os.fdopen(fd, "wb") as handle:
      handle.write(raw)
      handle.flush()
      os.fsync(handle.fileno())
    os.replace(temporary, path)
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
      os.fsync(directory)
    finally:
      os.close(directory)
  finally:
    temporary.unlink(missing_ok=True)


def _remove(path: Path) -> None:
  path.unlink(missing_ok=True)
  directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
  try:
    os.fsync(directory)
  finally:
    os.close(directory)


@contextmanager
def runtime_lock(root: Path) -> Iterator[None]:
  """Exclude a reset CLI while any EGIA runtime owns this state root."""
  _directory(root)
  path = root / "safety.lock"
  fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
  try:
    info = os.fstat(fd)
    if (
      not stat.S_ISREG(info.st_mode)
      or info.st_uid != os.getuid()
      or stat.S_IMODE(info.st_mode) != 0o600
    ):
      raise SafetyStateError("EGIA safety lock is unsafe")
    try:
      fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as exc:
      raise SafetyStateError("EGIA runtime is active") from exc
    yield
  finally:
    os.close(fd)


class SafetyState:
  """A single owner-authoritative latch and one-shot reset request."""

  def __init__(self, root: Path) -> None:
    self.root = root
    _directory(root)

  def aliases(self) -> frozenset[str]:
    return frozenset(self.identities())

  def _marker(self) -> dict[str, object] | None:
    value = _read(self.root / "safety-history.json")
    if value is None:
      return None
    episode_id = value.get("episode_id")
    graph_revision = value.get("graph_revision")
    if (
      value.get("schema") != "egia-safety-history-v1"
      or value.get("state") not in {"engaged", "cleared"}
      or type(episode_id) is not str
      or _NONCE.fullmatch(episode_id) is None
      or type(graph_revision) is not str
      or _REVISION.fullmatch(graph_revision) is None
    ):
      raise SafetyStateError("EGIA safety history is malformed")
    return value

  def episode(self) -> tuple[str, str] | None:
    marker = self._marker()
    latch = _read(self.root / "safety-latch.json")
    if marker is None:
      if latch is not None:
        raise SafetyStateError("EGIA safety latch has no history marker")
      return None
    if marker["state"] == "engaged":
      if latch is None:
        raise SafetyStateError("engaged EGIA safety latch is missing")
      if (
        latch.get("episode_id") != marker["episode_id"]
        or latch.get("graph_revision") != marker["graph_revision"]
      ):
        raise SafetyStateError("EGIA safety episode differs from history")
      return str(marker["episode_id"]), str(marker["graph_revision"])
    if latch is not None:
      raise SafetyStateError("cleared EGIA safety episode retains a latch")
    return None

  def identities(self) -> dict[str, str]:
    episode = self.episode()
    value = _read(self.root / "safety-latch.json")
    if value is None:
      return {}
    identities = value.get("identities")
    if (
      episode is None
      or value.get("schema") != "egia-safety-latch-v1"
      or type(identities) is not dict
      or not identities
      or any(
        type(alias) is not str
        or _ALIAS.fullmatch(alias) is None
        or type(robot_id) is not str
        or len(robot_id.encode()) > 128
        for alias, robot_id in identities.items()
      )
    ):
      raise SafetyStateError("EGIA safety latch is malformed")
    return identities

  def engage(self, identities: dict[str, str], graph_revision: str) -> None:
    if not identities or _REVISION.fullmatch(graph_revision) is None:
      raise SafetyStateError("EGIA safety latch needs affected aliases")
    current = self.identities()
    existing = self.episode()
    if existing is not None and existing[1] != graph_revision:
      raise SafetyStateError("selected graph changed during latch")
    for alias, robot_id in identities.items():
      if alias in current and current[alias] != robot_id:
        raise SafetyStateError("EOS robot identity changed during latch")
      current[alias] = robot_id
    episode_id = existing[0] if existing is not None else uuid.uuid4().hex
    # Marker first: a crash before latch replace leaves a detectable engaged
    # episode, never a clean home. A prior cleared marker is overwritten only
    # after a new emergency request has crossed this owner boundary.
    _write(
      self.root / "safety-history.json",
      {
        "schema": "egia-safety-history-v1",
        "state": "engaged",
        "episode_id": episode_id,
        "graph_revision": graph_revision,
      },
    )
    _write(
      self.root / "safety-latch.json",
      {
        "schema": "egia-safety-latch-v1",
        "identities": current,
        "episode_id": episode_id,
        "graph_revision": graph_revision,
      },
    )

  def authorize_reset(
    self, aliases: frozenset[str], episode_id: str, graph_revision: str
  ) -> None:
    if (
      aliases != self.aliases()
      or not aliases
      or self.episode() != (episode_id, graph_revision)
    ):
      raise SafetyStateError("reset aliases do not match the durable latch")
    _write(
      self.root / "safety-reset.json",
      {
        "schema": "egia-safety-reset-v1",
        "identities": self.identities(),
        "episode_id": episode_id,
        "graph_revision": graph_revision,
        "nonce": uuid.uuid4().hex,
      },
    )

  def consume_reset(self) -> ResetAuthorization | None:
    path = self.root / "safety-reset.json"
    value = _read(path)
    if value is None:
      return None
    _remove(path)
    identities = value.get("identities")
    nonce = value.get("nonce")
    episode_id = value.get("episode_id")
    graph_revision = value.get("graph_revision")
    if (
      value.get("schema") != "egia-safety-reset-v1"
      or type(identities) is not dict
      or not identities
      or any(
        type(alias) is not str
        or _ALIAS.fullmatch(alias) is None
        or type(robot_id) is not str
        or len(robot_id.encode()) > 128
        for alias, robot_id in identities.items()
      )
      or type(nonce) is not str
      or _NONCE.fullmatch(nonce) is None
      or type(episode_id) is not str
      or _NONCE.fullmatch(episode_id) is None
      or type(graph_revision) is not str
      or _REVISION.fullmatch(graph_revision) is None
    ):
      raise SafetyStateError("EGIA safety reset request is malformed")
    return ResetAuthorization(episode_id, graph_revision, identities, nonce)

  def clear(self, authorization: ResetAuthorization) -> None:
    if authorization.identities != self.identities() or self.episode() != (
      authorization.episode_id,
      authorization.graph_revision,
    ):
      raise SafetyStateError("reset aliases changed before unlatch")
    _remove(self.root / "safety-latch.json")
    _write(
      self.root / "safety-history.json",
      {
        "schema": "egia-safety-history-v1",
        "state": "cleared",
        "episode_id": authorization.episode_id,
        "graph_revision": authorization.graph_revision,
      },
    )
