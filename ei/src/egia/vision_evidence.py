"""Run-bound, read-only EOS gate evidence and owner-local inspection."""

from __future__ import annotations

import asyncio
import contextlib
import copy
import hashlib
import hmac
import json
import os
import re
import socket
import stat
import struct
import uuid
from collections import Counter
from pathlib import Path
from typing import Any

from websockets.asyncio.client import unix_connect
from websockets.asyncio.server import Server, unix_serve
from websockets.exceptions import ConnectionClosed, WebSocketException

_DIGEST = re.compile(r"[0-9a-f]{64}\Z")
_GRANT = re.compile(rb"[0-9a-f]{64}\n\Z")
_RUN = re.compile(r"[A-Za-z0-9_-]{1,64}\Z")
_PATH = re.compile(r"(?:eos/[A-Za-z0-9_-]+|isos/eos_[A-Za-z0-9_-]+)/[A-Za-z0-9_-]+/[A-Za-z0-9_-]+\Z")
_REASONS = (
  "binding_invalid",
  "endpoint_mismatch",
  "grant_denied",
  "run_mismatch",
  "revision_mismatch",
  "source_mismatch",
  "gate_mismatch",
  "stream_mismatch",
  "generation_stale",
  "metadata_invalid",
  "metadata_too_large",
  "jpeg_invalid",
  "jpeg_too_large",
  "digest_mismatch",
  "render_mismatch",
  "stopped",
)
_MAX = (1 << 64) - 1
_FRAME_LIMIT = 4 + 4096 + 8 * 1024 * 1024
_BINDING_KEYS = {
  "schema_version",
  "deployment_run_id",
  "graph_revision",
  "projection_revision",
  "eos_endpoint",
  "source_path",
  "gate_path",
  "ingress_type",
  "render",
  "target_fps",
  "grant_file",
}
_FRAME_KEYS = {
  "gate_path",
  "graph_revision",
  "projection_revision",
  "runtime_stream_id",
  "source_path",
  "source_frame_id",
  "gate_generation",
  "width_px",
  "height_px",
  "source_rate_hz",
  "sequence",
  "timestamp_unix_ms",
  "render",
  "jpeg_sha256",
}
_REPLAY_KEYS = {
  "schema_version",
  "op",
  "deployment_run_id",
  "graph_revision",
  "runtime_stream_id",
  "gate_generation",
  "source_frame_id",
  "jpeg_sha256",
}


def _object(raw: bytes, limit: int) -> dict[str, Any]:
  if not 0 < len(raw) <= limit:
    raise ValueError("metadata_too_large" if len(raw) > limit else "metadata_invalid")
  try:
    value = json.loads(raw.decode("utf-8"))
  except (UnicodeError, json.JSONDecodeError) as exc:
    raise ValueError("metadata_invalid") from exc
  if not isinstance(value, dict):
    raise ValueError("metadata_invalid")  # noqa: TRY004 - stable wire reason
  return value


def _private_file(path: Path, *, limit: int) -> bytes:
  if not path.is_absolute() or path.is_symlink():
    raise ValueError("binding_invalid")
  info = path.stat()
  if (
    not stat.S_ISREG(info.st_mode)
    or info.st_uid != os.getuid()
    or stat.S_IMODE(info.st_mode) != 0o600
    or info.st_size > limit
  ):
    raise ValueError("binding_invalid")
  return path.read_bytes()


def _private_parent(path: Path) -> None:
  _private_dir(path.parent)


def _private_dir(path: Path) -> None:
  info = path.stat()
  if (
    path.is_symlink()
    or not stat.S_ISDIR(info.st_mode)
    or info.st_uid != os.getuid()
    or stat.S_IMODE(info.st_mode) != 0o700
  ):
    raise ValueError("binding_invalid")


def _no_symlink_ancestors(path: Path) -> None:
  if not path.is_absolute() or ".." in path.parts:
    raise ValueError("binding_invalid")
  for ancestor in path.parents:
    if ancestor.is_symlink() or not ancestor.is_dir():
      raise ValueError("binding_invalid")


def _selected_endpoint_dir(grant_path: Path, run_id: str) -> Path:
  run_root = grant_path.parent
  deployment_root = run_root.parent
  control_root = deployment_root.parent
  runtime_root = control_root.parent
  deployment = deployment_root.name
  if (
    control_root.name != "control-grants"
    or run_root.name != run_id
    or not _RUN.fullmatch(deployment)
    or len(deployment) > 48
  ):
    raise ValueError("endpoint_mismatch")
  digest = hashlib.sha256(f"{deployment}\0{run_id}".encode()).hexdigest()[:32]
  endpoint_dir = runtime_root / "v" / digest
  for directory in (
    runtime_root,
    control_root,
    deployment_root,
    run_root,
    endpoint_dir.parent,
    endpoint_dir,
  ):
    _private_dir(directory)
    _no_symlink_ancestors(directory)
  return endpoint_dir


def _digest(value: Any) -> bool:
  return isinstance(value, str) and _DIGEST.fullmatch(value) is not None


def load_binding(
  path: Path, grant_path: Path, run_id: str, stage_revision: str
) -> tuple[dict[str, Any], str]:
  if (
    path.name != "vision-binding.json"
    or path.parent.name != "egia"
    or grant_path.name != "egia-vision.grant"
    or grant_path.parent != path.parent.parent
  ):
    raise ValueError("binding_invalid")
  _no_symlink_ancestors(path)
  _no_symlink_ancestors(grant_path)
  _private_parent(path)
  binding = _object(_private_file(path, limit=8192), 8192)
  if (
    set(binding) != _BINDING_KEYS
    or type(binding.get("schema_version")) is not int
    or binding["schema_version"] != 1
  ):
    raise ValueError("binding_invalid")
  if not isinstance(binding["deployment_run_id"], str) or not _RUN.fullmatch(
    binding["deployment_run_id"]
  ):
    raise ValueError("binding_invalid")
  if binding["deployment_run_id"] != run_id:
    raise ValueError("run_mismatch")
  if not _digest(binding["graph_revision"]) or not _digest(
    binding["projection_revision"]
  ):
    raise ValueError("binding_invalid")
  if binding["graph_revision"] != stage_revision:
    raise ValueError("revision_mismatch")
  endpoint = binding["eos_endpoint"]
  if (
    not isinstance(endpoint, dict)
    or set(endpoint) != {"transport", "path"}
    or endpoint["transport"] != "uds"
  ):
    raise ValueError("endpoint_mismatch")
  if not isinstance(endpoint["path"], str):
    raise ValueError("endpoint_mismatch")  # noqa: TRY004 - stable wire reason
  endpoint_path = Path(endpoint["path"])
  run_root = grant_path.parent
  short_endpoint = endpoint_path.parent != run_root
  if (
    not endpoint_path.is_absolute()
    or endpoint_path.name != "eos-vision-owner.sock"
    or ".." in endpoint_path.parts
  ):
    raise ValueError("endpoint_mismatch")
  if short_endpoint:
    if endpoint_path.parent != _selected_endpoint_dir(grant_path, run_id):
      raise ValueError("endpoint_mismatch")
  else:
    _private_dir(run_root)
    _no_symlink_ancestors(run_root)
  for key in ("source_path", "gate_path"):
    if not isinstance(binding[key], str) or not _PATH.fullmatch(binding[key]):
      raise ValueError("binding_invalid")
  if (
    binding["ingress_type"] != "egia.vision.ingress@v0.1.frame"
    or binding["render"] != {"max_width_px": 1280, "jpeg_quality": 80}
    or type(binding["target_fps"]) is not int
    or binding["target_fps"] != 30
  ):
    raise ValueError("binding_invalid")
  if binding["grant_file"] != str(grant_path):
    raise ValueError("grant_denied")
  grant = _private_file(grant_path, limit=65)
  if not _GRANT.fullmatch(grant):
    raise ValueError("grant_denied")
  if short_endpoint:
    verifier = _object(
      _private_file(endpoint_path.parent / "vision-verifier.json", limit=4096),
      4096,
    )
    if verifier != {
      "schema_version": 1,
      "run_id": run_id,
      "active_revision": binding["graph_revision"],
      "projection_revision": binding["projection_revision"],
      "source_path": binding["source_path"],
      "gate_path": binding["gate_path"],
      "egia_vision_sha256": hashlib.sha256(
        bytes.fromhex(grant.decode("ascii").strip())
      ).hexdigest(),
    }:
      raise ValueError("endpoint_mismatch")
  return binding, grant.decode("ascii").strip()


def _jpeg_dimensions(data: bytes) -> tuple[int, int]:
  if (
    len(data) < 4 or not data.startswith(b"\xff\xd8") or not data.endswith(b"\xff\xd9")
  ):
    raise ValueError("jpeg_invalid")
  cursor = 2
  while cursor + 3 < len(data):
    if data[cursor] != 0xFF:
      break
    marker = data[cursor + 1]
    cursor += 2
    if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
      continue
    segment_len = int.from_bytes(data[cursor : cursor + 2], "big")
    if segment_len < 2 or cursor + segment_len > len(data):
      raise ValueError("jpeg_invalid")
    if marker in (
      0xC0,
      0xC1,
      0xC2,
      0xC3,
      0xC5,
      0xC6,
      0xC7,
      0xC9,
      0xCA,
      0xCB,
      0xCD,
      0xCE,
      0xCF,
    ):
      if segment_len < 7:
        raise ValueError("jpeg_invalid")
      height = int.from_bytes(data[cursor + 3 : cursor + 5], "big")
      width = int.from_bytes(data[cursor + 5 : cursor + 7], "big")
      if not (1 <= width <= 4096 and 1 <= height <= 4096):
        raise ValueError("jpeg_invalid")
      return width, height
    cursor += segment_len
  raise ValueError("jpeg_invalid")


class VisionEvidence:
  """One run's transient frame and bounded metadata-only projection."""

  def __init__(
    self, binding: dict[str, Any], grant: str, *, stale_test: bool = False
  ) -> None:
    self.binding = binding
    self._grant = grant
    self.stream: str | None = None
    self.generation = 0
    self._last: dict[str, Any] | None = None
    self._jpeg: bytes | None = None
    self._replay_metadata: dict[str, Any] | None = None
    self._stale_test = stale_test
    self._test_hook: VisionStaleTest | None = None
    self._test_used = False
    self._grant_revoked = False
    self._admission_lock = asyncio.Lock()
    self._state = "missing"
    self._stopped = False
    self._accepted = 0
    self._rejected = 0
    self._reasons: Counter[str] = Counter()
    self._version = 0
    self.changed = asyncio.Condition()
    self._task: asyncio.Task[None] | None = None

  def snapshot(self) -> dict[str, Any]:
    b = self.binding
    result: dict[str, Any] = {
      "type": "vision_evidence",
      "state": self._state,
      "deployment_run_id": b["deployment_run_id"],
      "graph_revision": b["graph_revision"],
      "projection_revision": b["projection_revision"],
      "source_path": b["source_path"],
      "gate_path": b["gate_path"],
      "render": b["render"],
      "accepted_count": self._accepted,
      "rejected_count": self._rejected,
      "rejection_counts": dict(self._reasons),
      "runtime_stream_id": None,
      "source_frame_id": None,
      "gate_generation": None,
      "sequence": None,
      "timestamp_unix_ms": None,
      "width_px": None,
      "height_px": None,
      "jpeg_sha256": None,
      "jpeg_length_bytes": None,
    }
    if self._state == "available" and self._last is not None:
      result.update(self._last)
    elif self._last is not None:
      result["last_accepted"] = {
        "stale": True,
        "runtime_stream_id": self._last["runtime_stream_id"],
        "gate_path": b["gate_path"],
        "gate_generation": self._last["gate_generation"],
      }
    return result

  async def _notify(self) -> None:
    self._version += 1
    async with self.changed:
      self.changed.notify_all()

  async def reject(self, reason: str) -> None:
    async with self._admission_lock:
      await self._reject_locked(reason)

  async def _reject_locked(self, reason: str) -> None:
    if reason not in _REASONS:
      reason = "metadata_invalid"
    self._rejected = min(_MAX, self._rejected + 1)
    self._reasons[reason] = min(_MAX, self._reasons[reason] + 1)
    await self._notify()

  async def unavailable(self, state: str = "disconnected") -> None:
    async with self._admission_lock:
      await self._unavailable_locked(state)

  async def _unavailable_locked(self, state: str) -> None:
    self._jpeg = None
    self._replay_metadata = None
    self._state = state
    await self._notify()

  async def attest(self, data: dict[str, Any]) -> None:
    async with self._admission_lock:
      await self._attest_locked(data)

  async def _attest_locked(self, data: dict[str, Any]) -> None:
    b = self.binding
    for key, reason in (
      ("graph_revision", "revision_mismatch"),
      ("projection_revision", "revision_mismatch"),
      ("source_path", "source_mismatch"),
      ("gate_path", "gate_mismatch"),
    ):
      if data.get(key) != b[key]:
        raise ValueError(reason)
    stream = data.get("runtime_stream_id")
    if not isinstance(stream, str):
      raise ValueError("stream_mismatch")  # noqa: TRY004 - stable wire reason
    try:
      if str(uuid.UUID(stream)) != stream:
        raise ValueError("stream_mismatch")
    except ValueError as exc:
      raise ValueError("stream_mismatch") from exc
    generation = data.get("gate_generation")
    if type(generation) is not int or not 0 <= generation <= _MAX:
      raise ValueError("metadata_invalid")
    if self.stream != stream:
      self.stream = stream
      self.generation = 0
      await self._unavailable_locked("missing")

  async def accept(self, payload: bytes) -> None:
    async with self._admission_lock:
      await self._accept_locked(payload)

  async def _accept_locked(self, payload: bytes) -> None:
    if self._stopped:
      await self._reject_locked("stopped")
      return
    try:
      if len(payload) < 4:
        raise ValueError("metadata_invalid")
      size = struct.unpack_from("<I", payload)[0]
      if size > 4096:
        raise ValueError("metadata_too_large")
      if size == 0 or len(payload) < 4 + size:
        raise ValueError("metadata_invalid")
      metadata = _object(payload[4 : 4 + size], 4096)
      jpeg = payload[4 + size :]
      if len(jpeg) > 8 * 1024 * 1024:
        raise ValueError("jpeg_too_large")
      if set(metadata) - _FRAME_KEYS:
        raise ValueError("metadata_invalid")
      b = self.binding
      for key, reason in (
        ("graph_revision", "revision_mismatch"),
        ("projection_revision", "revision_mismatch"),
        ("source_path", "source_mismatch"),
        ("gate_path", "gate_mismatch"),
      ):
        if metadata.get(key) != b[key]:
          raise ValueError(reason)
      if (
        self.stream is None
        or not isinstance(metadata.get("runtime_stream_id"), str)
        or metadata["runtime_stream_id"] != self.stream
      ):
        raise ValueError("stream_mismatch")
      generation = metadata.get("gate_generation")
      if type(generation) is not int or not 0 < generation <= _MAX:
        raise ValueError("metadata_invalid")
      if generation <= self.generation:
        raise ValueError("generation_stale")
      if metadata.get("render") != b["render"]:
        raise ValueError("render_mismatch")
      source_frame_id = metadata.get("source_frame_id")
      if (
        not isinstance(source_frame_id, str)
        or not source_frame_id.strip()
        or len(source_frame_id.encode()) > 128
      ):
        raise ValueError("metadata_invalid")
      width, height = _jpeg_dimensions(jpeg)
      declared_width = metadata.get("width_px")
      declared_height = metadata.get("height_px")
      if (
        type(declared_width) is not int
        or type(declared_height) is not int
        or not 1 <= declared_width <= 4096
        or not 1 <= declared_height <= 4096
      ):
        raise ValueError("metadata_invalid")
      if declared_width != width or declared_height != height:
        raise ValueError("jpeg_invalid")
      digest = metadata.get("jpeg_sha256")
      if not _digest(digest) or digest != hashlib.sha256(jpeg).hexdigest():
        raise ValueError("digest_mismatch")
      sequence = metadata.get("sequence")
      if "sequence" in metadata and (
        not isinstance(sequence, int)
        or isinstance(sequence, bool)
        or not 0 <= sequence <= _MAX
      ):
        raise ValueError("metadata_invalid")
      timestamp = metadata.get("timestamp_unix_ms")
      if "timestamp_unix_ms" in metadata and (
        not isinstance(timestamp, int)
        or isinstance(timestamp, bool)
        or not -(1 << 63) <= timestamp < (1 << 63)
      ):
        raise ValueError("metadata_invalid")
      rate = metadata.get("source_rate_hz")
      if (
        not isinstance(rate, (int, float))
        or isinstance(rate, bool)
        or not 0 < rate <= 60
      ):
        raise ValueError("metadata_invalid")
    except ValueError as exc:
      await self._reject_locked(str(exc))
      return
    self.generation = generation
    self._jpeg = jpeg
    if self._stale_test:
      self._replay_metadata = copy.deepcopy(metadata)
    self._last = {
      "runtime_stream_id": self.stream,
      "source_frame_id": source_frame_id,
      "gate_generation": generation,
      "sequence": metadata.get("sequence"),
      "timestamp_unix_ms": metadata.get("timestamp_unix_ms"),
      "width_px": width,
      "height_px": height,
      "jpeg_sha256": digest,
      "jpeg_length_bytes": len(jpeg),
    }
    self._state = "available"
    self._accepted = min(_MAX, self._accepted + 1)
    await self._notify()

  def _check_grant(self) -> None:
    try:
      current = _private_file(Path(self.binding["grant_file"]), limit=65)
    except (OSError, ValueError) as exc:
      raise ValueError("grant_denied") from exc
    if not hmac.compare_digest(current, (self._grant + "\n").encode()):
      raise ValueError("grant_denied")

  async def replay_latest(self, request: dict[str, Any]) -> dict[str, Any]:
    """Compare current identity and replay only the already admitted frame."""
    response: dict[str, Any] = {
      "schema_version": 1,
      "op": "replay_latest",
      "injected": False,
    }
    async with self._admission_lock:
      if self._stopped:
        response["reason_code"] = "stopped"
        return response
      if self._test_used or not self._stale_test:
        response["reason_code"] = "unavailable"
        return response
      b = self.binding
      if (
        request["deployment_run_id"] != b["deployment_run_id"]
        or request["graph_revision"] != b["graph_revision"]
      ):
        response["reason_code"] = "binding_stale"
        return response
      if (
        self._state != "available"
        or self._jpeg is None
        or self._replay_metadata is None
        or self._last is None
      ):
        response["reason_code"] = "unavailable"
        return response
      try:
        self._check_grant()
      except ValueError:
        self._grant_revoked = True
        await self._unavailable_locked("disconnected")
        response["reason_code"] = "unavailable"
        return response
      if any(
        request[key] != self._last[key]
        for key in (
          "runtime_stream_id",
          "gate_generation",
          "source_frame_id",
          "jpeg_sha256",
        )
      ):
        response["reason_code"] = "target_changed"
        return response
      before = self.snapshot()
      metadata = copy.deepcopy(self._replay_metadata)
      jpeg = self._jpeg
      encoded = json.dumps(metadata, separators=(",", ":")).encode()
      payload = struct.pack("<I", len(encoded)) + encoded + jpeg
      await self._accept_locked(payload)
      after = self.snapshot()
      if (
        after["accepted_count"] != before["accepted_count"]
        or after["rejected_count"] != before["rejected_count"] + 1
        or after["rejection_counts"].get("generation_stale", 0)
        != before["rejection_counts"].get("generation_stale", 0) + 1
        or any(
          after[key] != before[key]
          for key in (
            "runtime_stream_id",
            "gate_generation",
            "source_frame_id",
            "jpeg_sha256",
            "jpeg_length_bytes",
          )
        )
      ):
        raise RuntimeError("duplicate replay invariant failed")
      self._test_used = True
      response.update(
        injected=True,
        reason_code="generation_stale",
        before=before,
        after=after,
      )
      return response

  async def subscribe_once(self) -> None:
    b = self.binding
    sock_path = b["eos_endpoint"]["path"]
    _private_parent(Path(sock_path))
    info = Path(sock_path).stat()
    if (
      not stat.S_ISSOCK(info.st_mode)
      or info.st_uid != os.getuid()
      or stat.S_IMODE(info.st_mode) != 0o600
    ):
      raise ValueError("endpoint_mismatch")
    headers = {"Authorization": f"Bearer {self._grant}"}
    async with unix_connect(
      sock_path,
      uri="ws://localhost/api/eos/v2/vision/owner-ws",
      additional_headers=headers,
      open_timeout=5,
      max_size=_FRAME_LIMIT,
    ) as ws:
      peer = ws.transport.get_extra_info("socket")
      try:
        _, peer_uid, _ = struct.unpack(
          "3i", peer.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
        )
      except (AttributeError, OSError) as exc:
        raise ValueError("endpoint_mismatch") from exc
      if peer_uid != os.getuid():
        raise ValueError("endpoint_mismatch")
      request: dict[str, Any] = {
        "op": "subscribe",
        "gate_path": b["gate_path"],
        "revision": b["projection_revision"],
        "target_fps": 30,
        "max_width_px": 1280,
        "jpeg_quality": 80,
      }
      if self.stream is not None and self.generation > 0:
        request["after_generation"] = self.generation
        request["after_runtime_stream_id"] = self.stream
      await ws.send(json.dumps(request, separators=(",", ":")))
      reply = await asyncio.wait_for(ws.recv(), 5)
      if not isinstance(reply, str):
        raise ValueError("metadata_invalid")  # noqa: TRY004 - stable wire reason
      attestation = _object(reply.encode(), 4096)
      if attestation.get("type") != "subscribed":
        if attestation.get("reason_code") == "cursor_stale":
          async with self._admission_lock:
            self.stream = None
            self.generation = 0
            await self._unavailable_locked("missing")
        raise ValueError("metadata_invalid")
      await self.attest(attestation)
      while not self._stopped:
        self._check_grant()
        try:
          message = await asyncio.wait_for(ws.recv(), timeout=0.5)
        except TimeoutError:
          continue
        self._check_grant()
        if not isinstance(message, bytes):
          raise ValueError("metadata_invalid")  # noqa: TRY004 - stable wire reason
        await self.accept(message)

  async def run(self) -> None:
    while not self._stopped:
      try:
        await self.subscribe_once()
      except asyncio.CancelledError:
        raise
      except (OSError, TimeoutError, WebSocketException, ValueError) as exc:
        if str(exc) == "grant_denied" and self._test_hook is not None:
          await self._test_hook.stop()
      if not self._stopped:
        await self.unavailable()
        await asyncio.sleep(0.5)

  async def start(self) -> None:
    self._task = asyncio.create_task(self.run(), name="egia-vision-evidence")

  async def stop(self) -> None:
    async with self._admission_lock:
      self._stopped = True
      await self._unavailable_locked("missing")
    if self._test_hook is not None:
      await self._test_hook.stop()
    if self._task is not None:
      self._task.cancel()
      with contextlib.suppress(asyncio.CancelledError):
        await self._task


class VisionInspection:
  """Separate grant and UDS; sends bounded metadata state only."""

  def __init__(self, evidence: VisionEvidence, path: Path, verifier_path: Path) -> None:
    self.evidence = evidence
    self.path = path
    self.verifier_path = verifier_path
    self.server: Server | None = None
    self._digest: str | None = None

  def _verify(self) -> None:
    _private_parent(self.path)
    _private_parent(self.verifier_path)
    doc = _object(_private_file(self.verifier_path, limit=1024), 1024)
    b = self.evidence.binding
    if (
      set(doc) != {"schema_version", "run_id", "graph_revision", "inspection_sha256"}
      or doc["schema_version"] != 1
      or doc["run_id"] != b["deployment_run_id"]
      or doc["graph_revision"] != b["graph_revision"]
      or not _digest(doc["inspection_sha256"])
    ):
      raise ValueError("binding_invalid")
    self._digest = doc["inspection_sha256"]

  async def _process(self, connection, request):
    if self.evidence._stopped:
      return connection.respond(503, '{"ok":false,"error":"inspection_unavailable"}')
    try:
      self._verify()
    except (OSError, ValueError):
      return connection.respond(409, '{"ok":false,"error":"inspection_run_stale"}')
    sock = connection.transport.get_extra_info("socket")
    try:
      pid, uid, gid = struct.unpack(
        "3i", sock.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
      )
    except (AttributeError, OSError):
      return connection.respond(403, '{"ok":false,"error":"inspection_grant_denied"}')
    del pid, gid
    authorizations = request.headers.get_all("Authorization")
    hosts = request.headers.get_all("Host")
    bearer = (
      authorizations[0].removeprefix("Bearer ") if len(authorizations) == 1 else ""
    )
    valid = bool(_DIGEST.fullmatch(bearer)) and hmac.compare_digest(
      hashlib.sha256(bytes.fromhex(bearer)).hexdigest(), self._digest or ""
    )
    if (
      uid != os.getuid()
      or not valid
      or request.path != "/api/aether/egia/vision/evidence"
      or hosts != ["localhost"]
      or "Origin" in request.headers
      or any(key.lower().startswith("sec-fetch-") for key in request.headers)
    ):
      return connection.respond(403, '{"ok":false,"error":"inspection_grant_denied"}')
    return None

  async def _handle(self, ws) -> None:
    authorized_digest = self._digest

    async def send_snapshot() -> bool:
      try:
        self._verify()
      except (OSError, ValueError):
        await ws.close(1012, "inspection_stale")
        return False
      if self._digest != authorized_digest:
        await ws.close(1012, "inspection_stale")
        return False
      encoded = json.dumps(self.evidence.snapshot(), separators=(",", ":"))
      if len(encoded.encode()) > 4096:
        await ws.close(1011, "snapshot_bound")
        return False
      await ws.send(encoded)
      return True

    if not await send_snapshot():
      return
    while not self.evidence._stopped:
      version = self.evidence._version
      change = asyncio.create_task(self._wait_change(version))
      incoming = asyncio.create_task(ws.recv())
      done, pending = await asyncio.wait(
        {change, incoming}, timeout=0.5, return_when=asyncio.FIRST_COMPLETED
      )
      for task in pending:
        task.cancel()
      if incoming in done:
        with contextlib.suppress(ConnectionClosed):
          incoming.result()
          await ws.close(1008, "read_only")
        return
      if not done:
        try:
          self._verify()
        except (OSError, ValueError):
          await ws.close(1012, "inspection_stale")
          return
        if self._digest != authorized_digest:
          await ws.close(1012, "inspection_stale")
          return
        continue
      if not await send_snapshot():
        return

  async def _wait_change(self, version: int) -> None:
    async with self.evidence.changed:
      await self.evidence.changed.wait_for(lambda: self.evidence._version != version)

  async def start(self) -> None:
    if (
      self.path.name != "egia-vision-inspection.sock"
      or self.verifier_path.name != "egia-vision-inspection-verifier.json"
      or self.path.parent != self.verifier_path.parent
    ):
      raise ValueError("binding_invalid")
    self._verify()
    if self.path.exists():
      raise ValueError("binding_invalid")
    self.server = await unix_serve(
      self._handle,
      str(self.path),
      process_request=self._process,
      origins=[None],
      max_size=4096,
    )
    self.path.chmod(0o600)

  async def stop(self) -> None:
    if self.server is not None:
      self.server.close()
      await self.server.wait_closed()
    self.path.unlink(missing_ok=True)


class VisionStaleTest:
  """One-shot same-UID duplicate replay on a private managed-run socket."""

  def __init__(self, evidence: VisionEvidence, inspection_path: Path) -> None:
    self.evidence = evidence
    self.path = inspection_path.parent / "egia-vision-stale-test.sock"
    self.server: asyncio.AbstractServer | None = None
    self._socket_inode: int | None = None

  @staticmethod
  def _peer_uid(writer: asyncio.StreamWriter) -> int | None:
    sock = writer.get_extra_info("socket")
    try:
      _, uid, _ = struct.unpack(
        "3i", sock.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
      )
    except (AttributeError, OSError):
      return None
    return uid

  @staticmethod
  def _request(raw: bytes) -> dict[str, Any]:
    try:
      request = _object(raw, 1024)
    except ValueError as exc:
      raise ValueError("invalid_request") from exc
    if (
      set(request) != _REPLAY_KEYS
      or type(request["schema_version"]) is not int
      or request["schema_version"] != 1
      or request["op"] != "replay_latest"
      or not isinstance(request["deployment_run_id"], str)
      or not _RUN.fullmatch(request["deployment_run_id"])
      or not _digest(request["graph_revision"])
      or not isinstance(request["runtime_stream_id"], str)
      or not isinstance(request["source_frame_id"], str)
      or not request["source_frame_id"].strip()
      or len(request["source_frame_id"].encode()) > 128
      or type(request["gate_generation"]) is not int
      or not 0 < request["gate_generation"] <= _MAX
      or not _digest(request["jpeg_sha256"])
    ):
      raise ValueError("invalid_request")
    try:
      if str(uuid.UUID(request["runtime_stream_id"])) != request["runtime_stream_id"]:
        raise ValueError("invalid_request")
    except ValueError as exc:
      raise ValueError("invalid_request") from exc
    return request

  async def _handle(
    self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
  ) -> None:
    injected = False
    try:
      if self._peer_uid(writer) != os.geteuid():
        return
      try:
        raw = await asyncio.wait_for(reader.readuntil(b"\n"), 2)
        if len(raw) > 1025:
          raise ValueError("invalid_request")
        request = self._request(raw[:-1])
        response = await self.evidence.replay_latest(request)
        injected = response["injected"]
      except (
        asyncio.IncompleteReadError,
        asyncio.LimitOverrunError,
        TimeoutError,
        ValueError,
      ):
        response = {
          "schema_version": 1,
          "op": "replay_latest",
          "injected": False,
          "reason_code": "invalid_request",
        }
      encoded = json.dumps(response, separators=(",", ":")).encode() + b"\n"
      if len(encoded) > 16384:
        raise RuntimeError("test response exceeds bound")
      with contextlib.suppress(OSError):
        writer.write(encoded)
        await writer.drain()
    finally:
      writer.close()
      with contextlib.suppress(OSError):
        await writer.wait_closed()
      if injected or self.evidence._grant_revoked:
        await self.stop()

  async def start(self) -> None:
    if self.path.name != "egia-vision-stale-test.sock" or not self.path.is_absolute():
      raise ValueError("binding_invalid")
    _private_parent(self.path)
    if self.path.exists() or self.path.is_symlink():
      raise ValueError("binding_invalid")
    self.server = await asyncio.start_unix_server(
      self._handle, path=str(self.path), limit=1025
    )
    try:
      sockets = self.server.sockets
      if not sockets:
        raise ValueError("binding_invalid")
      self._socket_inode = os.lstat(self.path).st_ino
      self.path.chmod(0o600)
      info = self.path.lstat()
      if (
        not stat.S_ISSOCK(info.st_mode)
        or info.st_uid != os.geteuid()
        or info.st_ino != self._socket_inode
        or stat.S_IMODE(info.st_mode) != 0o600
      ):
        raise ValueError("binding_invalid")
    except (Exception, asyncio.CancelledError):
      await self.stop()
      raise

  async def stop(self) -> None:
    if self.server is not None:
      self.server.close()
      await self.server.wait_closed()
      self.server = None
    owned_inode = self._socket_inode
    self._socket_inode = None
    if owned_inode is None:
      # The first stat failed, so pathname ownership was never established.
      # Keep any entry for run-root cleanup rather than unlink a replacement.
      return
    try:
      info = os.lstat(self.path)
    except OSError:
      return
    if owned_inode == info.st_ino and stat.S_ISSOCK(info.st_mode):
      self.path.unlink()


async def start_managed_vision(
  *, run_id: str, etc: Path, stale_test_opt_in: bool = False
) -> tuple[VisionEvidence, VisionInspection] | None:
  binding_env = os.getenv("EGIA_VISION_BINDING_FILE")
  if binding_env is None:
    return None
  binding_path = Path(binding_env)
  grant_path = Path(os.environ["EGIA_EOS_VISION_GRANT_FILE"])
  inspection_path = Path(os.environ["EGIA_VISION_INSPECTION_SOCKET"])
  verifier_path = Path(os.environ["EGIA_VISION_INSPECTION_VERIFIER_FILE"])
  stages = _object((etc / "stages.json").read_bytes(), 1024 * 1024)
  revision = stages.get("revision")
  if not isinstance(revision, str) or not _digest(revision):
    raise ValueError("revision_mismatch")
  binding, grant = load_binding(binding_path, grant_path, run_id, revision)
  selected_endpoint = Path(binding["eos_endpoint"]["path"]).parent
  if (
    inspection_path != selected_endpoint / "egia-vision-inspection.sock"
    or verifier_path != selected_endpoint / "egia-vision-inspection-verifier.json"
  ):
    raise ValueError("endpoint_mismatch")
  _no_symlink_ancestors(inspection_path)
  _no_symlink_ancestors(verifier_path)
  evidence = VisionEvidence(binding, grant, stale_test=stale_test_opt_in)
  inspection = VisionInspection(evidence, inspection_path, verifier_path)
  try:
    await inspection.start()
    if stale_test_opt_in:
      hook = VisionStaleTest(evidence, inspection_path)
      evidence._test_hook = hook
      await hook.start()
    await evidence.start()
  except (Exception, asyncio.CancelledError):
    await evidence.stop()
    await inspection.stop()
    raise
  return evidence, inspection
