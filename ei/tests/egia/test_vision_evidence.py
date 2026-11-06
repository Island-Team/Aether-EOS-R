"""Owner Vision evidence admission, transport and inspection boundaries."""

from __future__ import annotations

import asyncio
import contextlib
import hashlib
import json
import os
import socket
import struct
import tempfile
import uuid
from pathlib import Path

import pytest
from websockets.asyncio.client import unix_connect
from websockets.asyncio.server import unix_serve
from websockets.exceptions import InvalidStatus

from egia.vision_evidence import (
  VisionEvidence,
  VisionInspection,
  VisionStaleTest,
  load_binding,
  start_managed_vision,
)

_GRAPH = "0" * 64
_PROJECTION = "a" * 64
_STREAM = str(uuid.uuid4())
_JPEG = (
  b"\xff\xd8\xff\xc0\x00\x0b\x08\x00\x01\x00\x01\x01\x01\x11\x00"
  b"\xff\xda\x00\x08\x01\x01\x00\x00\x3f\x00\x00\xff\xd9"
)


def _replay_request(snapshot: dict) -> dict:
  return {
    "schema_version": 1,
    "op": "replay_latest",
    "deployment_run_id": snapshot["deployment_run_id"],
    "graph_revision": snapshot["graph_revision"],
    "runtime_stream_id": snapshot["runtime_stream_id"],
    "gate_generation": snapshot["gate_generation"],
    "source_frame_id": snapshot["source_frame_id"],
    "jpeg_sha256": snapshot["jpeg_sha256"],
  }


async def _replay(path: Path, request: dict | bytes) -> dict | None:
  reader, writer = await asyncio.open_unix_connection(str(path))
  raw = request if isinstance(request, bytes) else json.dumps(request).encode()
  writer.write(raw + b"\n")
  await writer.drain()
  with contextlib.suppress(ConnectionResetError):
    response = await reader.readline()
    writer.close()
    with contextlib.suppress(ConnectionResetError, BrokenPipeError):
      await writer.wait_closed()
    return json.loads(response) if response else None
  writer.close()
  with contextlib.suppress(ConnectionResetError, BrokenPipeError):
    await writer.wait_closed()
  return None


def _setup(tmp_path: Path) -> tuple[dict, str, Path, Path]:
  run = tmp_path / "run"
  run.mkdir(mode=0o700)
  egia = run / "egia"
  egia.mkdir(mode=0o700)
  grant_path = run / "egia-vision.grant"
  grant = "1" * 64
  grant_path.write_text(grant + "\n")
  grant_path.chmod(0o600)
  binding = {
    "schema_version": 1,
    "deployment_run_id": "run-one",
    "graph_revision": _GRAPH,
    "projection_revision": _PROJECTION,
    "eos_endpoint": {"transport": "uds", "path": str(run / "eos-vision-owner.sock")},
    "source_path": "eos/hardware/camera/front",
    "gate_path": "eos/interface/gate/front",
    "ingress_type": "egia.vision.ingress@v0.1.frame",
    "render": {"max_width_px": 1280, "jpeg_quality": 80},
    "target_fps": 30,
    "grant_file": str(grant_path),
  }
  binding_path = egia / "vision-binding.json"
  binding_path.write_text(json.dumps(binding))
  binding_path.chmod(0o600)
  return binding, grant, binding_path, grant_path


def _short_setup(
  tmp_path: Path, *, long_home: bool = True
) -> tuple[dict, str, Path, Path, Path]:
  runtime = (
    tmp_path / "home-with-a-long-private-runtime-path" / "run"
    if long_home
    else tmp_path / "r"
  )
  runtime.mkdir(mode=0o700, parents=True)
  control = runtime / "control-grants"
  control.mkdir(mode=0o700)
  deployment = control / "default"
  deployment.mkdir(mode=0o700)
  run = deployment / "run-one"
  run.mkdir(mode=0o700)
  egia = run / "egia"
  egia.mkdir(mode=0o700)
  grant_path = run / "egia-vision.grant"
  grant = "1" * 64
  grant_path.write_text(grant + "\n")
  grant_path.chmod(0o600)
  endpoint_key = hashlib.sha256(b"default\0run-one").hexdigest()[:32]
  endpoint_parent = runtime / "v"
  endpoint_parent.mkdir(mode=0o700)
  endpoint = endpoint_parent / endpoint_key
  endpoint.mkdir(mode=0o700)
  binding = {
    "schema_version": 1,
    "deployment_run_id": "run-one",
    "graph_revision": _GRAPH,
    "projection_revision": _PROJECTION,
    "eos_endpoint": {
      "transport": "uds",
      "path": str(endpoint / "eos-vision-owner.sock"),
    },
    "source_path": "eos/hardware/camera/front",
    "gate_path": "eos/interface/gate/front",
    "ingress_type": "egia.vision.ingress@v0.1.frame",
    "render": {"max_width_px": 1280, "jpeg_quality": 80},
    "target_fps": 30,
    "grant_file": str(grant_path),
  }
  binding_path = egia / "vision-binding.json"
  binding_path.write_text(json.dumps(binding))
  binding_path.chmod(0o600)
  verifier = endpoint / "vision-verifier.json"
  verifier.write_text(
    json.dumps(
      {
        "schema_version": 1,
        "run_id": "run-one",
        "active_revision": _GRAPH,
        "projection_revision": _PROJECTION,
        "source_path": binding["source_path"],
        "gate_path": binding["gate_path"],
        "egia_vision_sha256": hashlib.sha256(bytes.fromhex(grant)).hexdigest(),
      }
    )
  )
  verifier.chmod(0o600)
  return binding, grant, binding_path, grant_path, endpoint


def _frame(
  binding: dict, *, generation: int = 1, jpeg: bytes = _JPEG, **overrides
) -> bytes:
  metadata = {
    "graph_revision": binding["graph_revision"],
    "projection_revision": binding["projection_revision"],
    "runtime_stream_id": _STREAM,
    "source_path": binding["source_path"],
    "source_frame_id": "frame-1",
    "gate_path": binding["gate_path"],
    "gate_generation": generation,
    "width_px": 1,
    "height_px": 1,
    "source_rate_hz": 30.0,
    "render": binding["render"],
    "jpeg_sha256": hashlib.sha256(jpeg).hexdigest(),
  }
  metadata.update(overrides)
  raw = json.dumps(metadata, separators=(",", ":")).encode()
  return struct.pack("<I", len(raw)) + raw + jpeg


def _attest(binding: dict, *, stream: str = _STREAM) -> dict:
  return {
    "type": "subscribed",
    "graph_revision": binding["graph_revision"],
    "projection_revision": binding["projection_revision"],
    "source_path": binding["source_path"],
    "gate_path": binding["gate_path"],
    "runtime_stream_id": stream,
    "gate_generation": 0,
  }


def test_binding_is_run_revision_and_owner_bound(tmp_path: Path) -> None:
  binding, grant, path, grant_path = _setup(tmp_path)
  assert load_binding(path, grant_path, "run-one", _GRAPH) == (binding, grant)
  with pytest.raises(ValueError, match="run_mismatch"):
    load_binding(path, grant_path, "run-two", _GRAPH)
  with pytest.raises(ValueError, match="revision_mismatch"):
    load_binding(path, grant_path, "run-one", "f" * 64)
  wrong = {
    **binding,
    "eos_endpoint": {"transport": "uds", "path": str(tmp_path / "old.sock")},
  }
  path.write_text(json.dumps(wrong))
  with pytest.raises(ValueError, match="endpoint_mismatch"):
    load_binding(path, grant_path, "run-one", _GRAPH)
  path.write_text(json.dumps(binding))
  grant_path.chmod(0o644)
  with pytest.raises(ValueError, match="binding_invalid"):
    load_binding(path, grant_path, "run-one", _GRAPH)


def test_unified_eos_binding_paths_keep_legacy_reader(tmp_path: Path) -> None:
  binding, grant, path, grant_path = _setup(tmp_path)
  binding["source_path"] = "isos/eos_hardware/camera/front"
  binding["gate_path"] = "isos/eos_interface/gate/front"
  path.write_text(json.dumps(binding))
  assert load_binding(path, grant_path, "run-one", _GRAPH) == (binding, grant)
  binding["source_path"] = "isos/ei_io/camera/front"
  path.write_text(json.dumps(binding))
  with pytest.raises(ValueError, match="binding_invalid"):
    load_binding(path, grant_path, "run-one", _GRAPH)


def test_short_endpoint_is_derived_and_verifier_bound(tmp_path: Path) -> None:
  binding, grant, path, grant_path, endpoint = _short_setup(tmp_path)
  assert load_binding(path, grant_path, "run-one", _GRAPH) == (binding, grant)
  for bad_endpoint in (
    endpoint.parent / ("f" * 32),
    endpoint.parent.parent / "other",
  ):
    wrong = {
      **binding,
      "eos_endpoint": {
        "transport": "uds",
        "path": str(bad_endpoint / "eos-vision-owner.sock"),
      },
    }
    path.write_text(json.dumps(wrong))
    with pytest.raises((ValueError, OSError)):
      load_binding(path, grant_path, "run-one", _GRAPH)
  path.write_text(json.dumps(binding))
  verifier = endpoint / "vision-verifier.json"
  original = verifier.read_text()
  verifier.write_text(original.replace(_PROJECTION, "b" * 64))
  with pytest.raises(ValueError, match="endpoint_mismatch"):
    load_binding(path, grant_path, "run-one", _GRAPH)
  verifier.write_text(original)
  endpoint.chmod(0o755)
  with pytest.raises(ValueError, match="binding_invalid"):
    load_binding(path, grant_path, "run-one", _GRAPH)
  endpoint.chmod(0o700)
  assert load_binding(path, grant_path, "run-one", _GRAPH) == (binding, grant)
  relocated = endpoint.with_name("relocated")
  endpoint.rename(relocated)
  endpoint.symlink_to(relocated, target_is_directory=True)
  with pytest.raises(ValueError, match="binding_invalid"):
    load_binding(path, grant_path, "run-one", _GRAPH)
  endpoint.unlink()
  relocated.rename(endpoint)
  assert load_binding(path, grant_path, "run-one", _GRAPH) == (binding, grant)


@pytest.mark.asyncio
async def test_short_managed_endpoint_stops_and_restarts(monkeypatch) -> None:
  with tempfile.TemporaryDirectory(prefix="ev-", dir=Path.home()) as name:
    root = Path(name)
    binding, _, path, grant_path, endpoint = _short_setup(root, long_home=False)
    etc = root / "etc"
    etc.mkdir()
    (etc / "stages.json").write_text(json.dumps({"revision": _GRAPH}))
    verifier = endpoint / "egia-vision-inspection-verifier.json"
    verifier.write_text(
      json.dumps(
        {
          "schema_version": 1,
          "run_id": "run-one",
          "graph_revision": _GRAPH,
          "inspection_sha256": hashlib.sha256(bytes.fromhex("4" * 64)).hexdigest(),
        }
      )
    )
    verifier.chmod(0o600)
    inspection_path = endpoint / "egia-vision-inspection.sock"
    monkeypatch.setenv("EGIA_VISION_BINDING_FILE", str(path))
    monkeypatch.setenv("EGIA_EOS_VISION_GRANT_FILE", str(grant_path))
    monkeypatch.setenv("EGIA_VISION_INSPECTION_SOCKET", str(inspection_path))
    monkeypatch.setenv("EGIA_VISION_INSPECTION_VERIFIER_FILE", str(verifier))
    for _ in range(2):
      service = await start_managed_vision(run_id="run-one", etc=etc)
      assert service is not None
      assert service[0].binding == binding
      assert inspection_path.is_socket()
      verifier.rename(verifier.with_name("revoked.json"))
      with pytest.raises(OSError):
        service[1]._verify()
      verifier.with_name("revoked.json").rename(verifier)
      await service[0].stop()
      await service[1].stop()
      assert not inspection_path.exists()
    monkeypatch.setenv(
      "EGIA_VISION_INSPECTION_SOCKET", str(grant_path.parent / inspection_path.name)
    )
    with pytest.raises(ValueError, match="endpoint_mismatch"):
      await start_managed_vision(run_id="run-one", etc=etc)


@pytest.mark.asyncio
async def test_frame_rejections_preserve_observation(tmp_path: Path) -> None:
  binding, grant, _, _ = _setup(tmp_path)
  evidence = VisionEvidence(binding, grant)
  await evidence.attest(_attest(binding))
  await evidence.accept(_frame(binding))
  baseline = evidence.snapshot()
  assert baseline["state"] == "available"
  assert baseline["jpeg_length_bytes"] == len(_JPEG)
  assert baseline["accepted_count"] == 1
  assert evidence._replay_metadata is None
  negatives = [
    (_frame(binding, generation=1), "generation_stale"),
    (_frame(binding, graph_revision="f" * 64, generation=2), "revision_mismatch"),
    (
      _frame(binding, source_path="eos/hardware/camera/other", generation=2),
      "source_mismatch",
    ),
    (
      _frame(binding, gate_path="eos/interface/gate/other", generation=2),
      "gate_mismatch",
    ),
    (
      _frame(binding, runtime_stream_id=str(uuid.uuid4()), generation=2),
      "stream_mismatch",
    ),
    (
      _frame(binding, render={"max_width_px": 640, "jpeg_quality": 80}, generation=2),
      "render_mismatch",
    ),
    (_frame(binding, projection_revision="f" * 64, generation=2), "revision_mismatch"),
    (_frame(binding, jpeg_sha256="f" * 64, generation=2), "digest_mismatch"),
    (_frame(binding, jpeg=b"bad", generation=2), "jpeg_invalid"),
    (
      _frame(
        binding,
        jpeg=_JPEG[:7] + (4097).to_bytes(2, "big") + _JPEG[9:],
        height_px=4097,
        generation=2,
      ),
      "jpeg_invalid",
    ),
    (_frame(binding, width_px=4097, generation=2), "metadata_invalid"),
    (_frame(binding, width_px=True, generation=2), "metadata_invalid"),
    (struct.pack("<I", 2) + b"{}" + b"x" * (8 * 1024 * 1024 + 1), "jpeg_too_large"),
    (struct.pack("<I", 2) + b"{}" + b"bad", "revision_mismatch"),
    (struct.pack("<I", 4097) + b"x" * 4097 + _JPEG, "metadata_too_large"),
    (struct.pack("<I", 1) + b"{" + _JPEG, "metadata_invalid"),
  ]
  for payload, reason in negatives:
    await evidence.accept(payload)
    snap = evidence.snapshot()
    assert snap["rejection_counts"][reason] >= 1
    for key in (
      "gate_generation",
      "source_frame_id",
      "jpeg_sha256",
      "jpeg_length_bytes",
    ):
      assert snap[key] == baseline[key]
    assert snap["accepted_count"] == 1
  await evidence.unavailable()
  snap = evidence.snapshot()
  assert snap["state"] == "disconnected"
  assert snap["jpeg_sha256"] is None
  assert snap["last_accepted"]["stale"] is True
  await evidence.stop()
  await evidence.accept(_frame(binding, generation=2))
  assert evidence.snapshot()["rejection_counts"]["stopped"] == 1


@pytest.mark.asyncio
async def test_owner_stub_receipt_and_read_only_inspection(tmp_path: Path) -> None:
  binding, grant, _, _ = _setup(tmp_path)
  run = tmp_path / "run"
  eos_path = Path(binding["eos_endpoint"]["path"])
  seen = asyncio.Event()

  async def eos_stub(ws):
    assert ws.request.path == "/api/eos/v2/vision/owner-ws"
    assert ws.request.headers["Authorization"] == f"Bearer {grant}"
    request = json.loads(await ws.recv())
    assert request == {
      "op": "subscribe",
      "gate_path": binding["gate_path"],
      "revision": _PROJECTION,
      "target_fps": 30,
      "max_width_px": 1280,
      "jpeg_quality": 80,
    }
    await ws.send(json.dumps(_attest(binding)))
    await ws.send(_frame(binding))
    await seen.wait()

  server = await unix_serve(eos_stub, str(eos_path))
  eos_path.chmod(0o600)
  inspection_grant = "2" * 64
  verifier = run / "egia-vision-inspection-verifier.json"
  verifier.write_text(
    json.dumps(
      {
        "schema_version": 1,
        "run_id": "run-one",
        "graph_revision": _GRAPH,
        "inspection_sha256": hashlib.sha256(
          bytes.fromhex(inspection_grant)
        ).hexdigest(),
      }
    )
  )
  verifier.chmod(0o600)
  inspection_path = run / "egia-vision-inspection.sock"
  evidence = VisionEvidence(binding, grant)
  inspection = VisionInspection(evidence, inspection_path, verifier)
  await inspection.start()
  await evidence.start()
  try:
    for _ in range(100):
      if evidence.snapshot()["state"] == "available":
        break
      await asyncio.sleep(0.01)
    assert evidence.snapshot()["state"] == "available"
    with pytest.raises(InvalidStatus):
      async with unix_connect(
        str(inspection_path),
        uri="ws://localhost/api/aether/egia/vision/evidence",
        additional_headers={"Authorization": f"Bearer {grant}"},
      ):
        pass
    async with unix_connect(
      str(inspection_path),
      uri="ws://localhost/api/aether/egia/vision/evidence",
      additional_headers={"Authorization": f"Bearer {inspection_grant}"},
    ) as ws:
      snap = json.loads(await ws.recv())
      assert snap["jpeg_sha256"] == hashlib.sha256(_JPEG).hexdigest()
      assert "jpeg" not in snap
      assert "grant" not in json.dumps(snap)
      await ws.send("actuate")
      await ws.wait_closed()
      assert ws.close_code == 1008
  finally:
    seen.set()
    await evidence.stop()
    await inspection.stop()
    server.close()
    await server.wait_closed()
    assert not inspection_path.exists()


@pytest.mark.asyncio
async def test_new_attested_stream_resets_only_after_handshake(tmp_path: Path) -> None:
  binding, grant, _, _ = _setup(tmp_path)
  evidence = VisionEvidence(binding, grant)
  await evidence.attest(_attest(binding))
  await evidence.accept(_frame(binding, generation=5))
  prior = evidence.snapshot()
  new_stream = str(uuid.uuid4())
  await evidence.attest(_attest(binding, stream=new_stream))
  assert evidence.snapshot()["state"] == "missing"
  assert evidence.snapshot()["last_accepted"]["gate_generation"] == 5
  await evidence.accept(_frame(binding, generation=6))
  assert evidence.snapshot()["rejection_counts"]["stream_mismatch"] == 1
  await evidence.accept(_frame(binding, generation=1, runtime_stream_id=new_stream))
  assert evidence.snapshot()["state"] == "available"
  assert evidence.snapshot()["gate_generation"] == 1
  assert evidence.snapshot()["runtime_stream_id"] == new_stream
  assert evidence.snapshot()["accepted_count"] == prior["accepted_count"] + 1


@pytest.mark.asyncio
async def test_grant_revocation_blocks_next_frame(tmp_path: Path) -> None:
  binding, grant, _, grant_path = _setup(tmp_path)
  eos_path = Path(binding["eos_endpoint"]["path"])
  release = asyncio.Event()

  async def eos_stub(ws):
    await ws.recv()
    await ws.send(json.dumps(_attest(binding)))
    await ws.send(_frame(binding))
    await release.wait()
    await ws.send(_frame(binding, generation=2))
    await ws.wait_closed()

  server = await unix_serve(eos_stub, str(eos_path))
  eos_path.chmod(0o600)
  evidence = VisionEvidence(binding, grant, stale_test=True)
  hook = VisionStaleTest(evidence, tmp_path / "run" / "egia-vision-inspection.sock")
  evidence._test_hook = hook
  await hook.start()
  await evidence.start()
  try:
    for _ in range(100):
      if evidence.snapshot()["state"] == "available":
        break
      await asyncio.sleep(0.01)
    assert evidence.snapshot()["gate_generation"] == 1
    grant_path.unlink()
    release.set()
    for _ in range(100):
      if evidence.snapshot()["state"] == "disconnected":
        break
      await asyncio.sleep(0.01)
    assert evidence.snapshot()["state"] == "disconnected"
    assert evidence.snapshot()["accepted_count"] == 1
    assert evidence.snapshot()["gate_generation"] is None
    assert evidence._replay_metadata is None
    assert not hook.path.exists()
  finally:
    release.set()
    await evidence.stop()
    server.close()
    await server.wait_closed()


@pytest.mark.asyncio
async def test_inspection_verifier_revocation_closes_active_peer(
  tmp_path: Path,
) -> None:
  binding, grant, _, _ = _setup(tmp_path)
  run = tmp_path / "run"
  inspection_grant = "3" * 64
  verifier = run / "egia-vision-inspection-verifier.json"
  verifier.write_text(
    json.dumps(
      {
        "schema_version": 1,
        "run_id": "run-one",
        "graph_revision": _GRAPH,
        "inspection_sha256": hashlib.sha256(
          bytes.fromhex(inspection_grant)
        ).hexdigest(),
      }
    )
  )
  verifier.chmod(0o600)
  evidence = VisionEvidence(binding, grant)
  inspection = VisionInspection(evidence, run / "egia-vision-inspection.sock", verifier)
  await inspection.start()
  try:
    async with unix_connect(
      str(inspection.path),
      uri="ws://localhost/api/aether/egia/vision/evidence",
      additional_headers={"Authorization": f"Bearer {inspection_grant}"},
    ) as ws:
      assert json.loads(await ws.recv())["state"] == "missing"
      verifier.unlink()
      await asyncio.wait_for(ws.wait_closed(), 2)
      assert ws.close_code == 1012
  finally:
    await evidence.stop()
    await inspection.stop()


@pytest.mark.asyncio
async def test_managed_env_start_checks_selected_stage_revision(
  tmp_path: Path, monkeypatch
) -> None:
  binding, _, binding_path, grant_path = _setup(tmp_path)
  run = tmp_path / "run"
  etc = tmp_path / "etc"
  etc.mkdir()
  (etc / "stages.json").write_text(json.dumps({"revision": _GRAPH}))
  inspection_grant = "4" * 64
  verifier = run / "egia-vision-inspection-verifier.json"
  verifier.write_text(
    json.dumps(
      {
        "schema_version": 1,
        "run_id": "run-one",
        "graph_revision": _GRAPH,
        "inspection_sha256": hashlib.sha256(
          bytes.fromhex(inspection_grant)
        ).hexdigest(),
      }
    )
  )
  verifier.chmod(0o600)
  monkeypatch.setenv("EGIA_VISION_BINDING_FILE", str(binding_path))
  monkeypatch.setenv("EGIA_EOS_VISION_GRANT_FILE", str(grant_path))
  monkeypatch.setenv(
    "EGIA_VISION_INSPECTION_SOCKET", str(run / "egia-vision-inspection.sock")
  )
  monkeypatch.setenv("EGIA_VISION_INSPECTION_VERIFIER_FILE", str(verifier))
  monkeypatch.delenv("EGIA_Q01_STALE_TEST", raising=False)
  with pytest.raises(ValueError, match="run_mismatch"):
    await start_managed_vision(run_id="run-two", etc=etc)
  (etc / "stages.json").write_text(json.dumps({"revision": "f" * 64}))
  with pytest.raises(ValueError, match="revision_mismatch"):
    await start_managed_vision(run_id="run-one", etc=etc)
  (etc / "stages.json").write_text(json.dumps({"revision": _GRAPH}))
  service = await start_managed_vision(run_id="run-one", etc=etc)
  assert service is not None
  evidence, inspection = service
  assert evidence.binding == binding
  assert inspection.path.exists()
  assert evidence._replay_metadata is None
  assert not (run / "egia-vision-stale-test.sock").exists()
  await evidence.stop()
  await inspection.stop()
  assert not inspection.path.exists()


@pytest.mark.asyncio
async def test_managed_exact_opt_in_creates_only_private_test_socket(
  tmp_path: Path, monkeypatch
) -> None:
  _, _, binding_path, grant_path = _setup(tmp_path)
  run = tmp_path / "run"
  etc = tmp_path / "etc"
  etc.mkdir()
  (etc / "stages.json").write_text(json.dumps({"revision": _GRAPH}))
  verifier = run / "egia-vision-inspection-verifier.json"
  verifier.write_text(
    json.dumps(
      {
        "schema_version": 1,
        "run_id": "run-one",
        "graph_revision": _GRAPH,
        "inspection_sha256": hashlib.sha256(bytes.fromhex("4" * 64)).hexdigest(),
      }
    )
  )
  verifier.chmod(0o600)
  monkeypatch.setenv("EGIA_VISION_BINDING_FILE", str(binding_path))
  monkeypatch.setenv("EGIA_EOS_VISION_GRANT_FILE", str(grant_path))
  monkeypatch.setenv(
    "EGIA_VISION_INSPECTION_SOCKET", str(run / "egia-vision-inspection.sock")
  )
  monkeypatch.setenv("EGIA_VISION_INSPECTION_VERIFIER_FILE", str(verifier))
  hook_path = run / "egia-vision-stale-test.sock"
  monkeypatch.setenv("EGIA_Q01_STALE_TEST", "0")
  service = await start_managed_vision(run_id="run-one", etc=etc)
  assert service is not None and not hook_path.exists()
  await service[0].stop()
  await service[1].stop()
  monkeypatch.setenv("EGIA_Q01_STALE_TEST", "1")
  service = await start_managed_vision(run_id="run-one", etc=etc)
  assert service is not None and not hook_path.exists()
  await service[0].stop()
  await service[1].stop()
  service = await start_managed_vision(
    run_id="run-one", etc=etc, stale_test_opt_in=True
  )
  assert service is not None
  evidence, inspection = service
  assert hook_path.is_socket()
  assert hook_path.stat().st_mode & 0o777 == 0o600
  await evidence.stop()
  await inspection.stop()
  assert not hook_path.exists()
  hook_path.symlink_to(run / "target")
  with pytest.raises(ValueError, match="binding_invalid"):
    await start_managed_vision(run_id="run-one", etc=etc, stale_test_opt_in=True)
  assert hook_path.is_symlink()
  assert not inspection.path.exists()


@pytest.mark.asyncio
async def test_managed_start_owns_hook_before_post_bind_failure(
  tmp_path: Path, monkeypatch
) -> None:
  _, _, binding_path, grant_path = _setup(tmp_path)
  run = tmp_path / "run"
  etc = tmp_path / "etc"
  etc.mkdir()
  (etc / "stages.json").write_text(json.dumps({"revision": _GRAPH}))
  verifier = run / "egia-vision-inspection-verifier.json"
  verifier.write_text(
    json.dumps(
      {
        "schema_version": 1,
        "run_id": "run-one",
        "graph_revision": _GRAPH,
        "inspection_sha256": hashlib.sha256(bytes.fromhex("4" * 64)).hexdigest(),
      }
    )
  )
  verifier.chmod(0o600)
  monkeypatch.setenv("EGIA_VISION_BINDING_FILE", str(binding_path))
  monkeypatch.setenv("EGIA_EOS_VISION_GRANT_FILE", str(grant_path))
  inspection_path = run / "egia-vision-inspection.sock"
  monkeypatch.setenv("EGIA_VISION_INSPECTION_SOCKET", str(inspection_path))
  monkeypatch.setenv("EGIA_VISION_INSPECTION_VERIFIER_FILE", str(verifier))
  created: list[asyncio.AbstractServer] = []

  async def fail_after_bind(hook: VisionStaleTest) -> None:
    hook.server = await asyncio.start_unix_server(hook._handle, path=str(hook.path))
    created.append(hook.server)
    assert hook.server.sockets
    hook._socket_inode = os.lstat(hook.path).st_ino
    raise OSError("injected after bind")

  monkeypatch.setattr(VisionStaleTest, "start", fail_after_bind)
  with pytest.raises(OSError, match="injected after bind"):
    await start_managed_vision(run_id="run-one", etc=etc, stale_test_opt_in=True)
  assert not (run / "egia-vision-stale-test.sock").exists()
  assert not inspection_path.exists()
  assert len(created) == 1 and not created[0].is_serving()


@pytest.mark.asyncio
async def test_frame_requires_attested_stream_before_publication(
  tmp_path: Path,
) -> None:
  binding, grant, _, _ = _setup(tmp_path)
  evidence = VisionEvidence(binding, grant)
  for payload in (
    _frame(binding),
    _frame(binding, runtime_stream_id=None),
  ):
    await evidence.accept(payload)
    snap = evidence.snapshot()
    assert snap["state"] == "missing"
    assert snap["accepted_count"] == 0
    assert snap["rejection_counts"]["stream_mismatch"] >= 1
    assert snap["jpeg_sha256"] is None
  raw = _frame(binding)
  size = struct.unpack_from("<I", raw)[0]
  metadata = json.loads(raw[4 : 4 + size])
  metadata.pop("runtime_stream_id")
  encoded = json.dumps(metadata).encode()
  await evidence.accept(struct.pack("<I", len(encoded)) + encoded + _JPEG)
  assert evidence.snapshot()["state"] == "missing"
  assert evidence.snapshot()["rejection_counts"]["stream_mismatch"] == 3
  await evidence.attest(_attest(binding))
  await evidence.accept(_frame(binding))
  assert evidence.snapshot()["state"] == "available"
  assert evidence.snapshot()["accepted_count"] == 1


@pytest.mark.asyncio
async def test_same_stream_reconnect_uses_attested_cursor(tmp_path: Path) -> None:
  binding, grant, _, _ = _setup(tmp_path)
  eos_path = Path(binding["eos_endpoint"]["path"])
  requests: list[dict] = []

  async def eos_stub(ws):
    request = json.loads(await ws.recv())
    requests.append(request)
    await ws.send(json.dumps(_attest(binding)))
    await ws.send(_frame(binding, generation=len(requests)))
    await ws.close()

  server = await unix_serve(eos_stub, str(eos_path))
  eos_path.chmod(0o600)
  evidence = VisionEvidence(binding, grant)
  await evidence.start()
  try:
    for _ in range(200):
      if evidence.snapshot()["accepted_count"] >= 2:
        break
      await asyncio.sleep(0.01)
    assert evidence.snapshot()["accepted_count"] == 2
    assert requests[0].get("after_generation") is None
    assert requests[1]["after_generation"] == 1
    assert requests[1]["after_runtime_stream_id"] == _STREAM
  finally:
    await evidence.stop()
    server.close()
    await server.wait_closed()


@pytest.mark.asyncio
async def test_private_duplicate_replay_is_one_shot_and_preserves_frame(
  tmp_path: Path,
) -> None:
  binding, grant, _, _ = _setup(tmp_path)
  evidence = VisionEvidence(binding, grant, stale_test=True)
  await evidence.attest(_attest(binding))
  await evidence.accept(_frame(binding))
  path = tmp_path / "run" / "egia-vision-stale-test.sock"
  hook = VisionStaleTest(evidence, tmp_path / "run" / "egia-vision-inspection.sock")
  evidence._test_hook = hook
  await hook.start()
  assert path.stat().st_mode & 0o777 == 0o600
  before = evidence.snapshot()
  response = await _replay(path, _replay_request(before))
  assert response is not None
  assert response["injected"] is True
  assert response["reason_code"] == "generation_stale"
  assert response["before"] == before
  after = response["after"]
  assert after["accepted_count"] == before["accepted_count"]
  assert after["rejected_count"] == before["rejected_count"] + 1
  assert after["rejection_counts"]["generation_stale"] == 1
  for key in (
    "runtime_stream_id",
    "gate_generation",
    "source_frame_id",
    "jpeg_sha256",
    "jpeg_length_bytes",
  ):
    assert after[key] == before[key]
  assert "jpeg" not in response
  assert "jpeg" not in response["before"]
  assert "jpeg" not in response["after"]
  assert "grant" not in json.dumps(response)
  assert not path.exists()
  with pytest.raises((FileNotFoundError, ConnectionRefusedError)):
    await _replay(path, _replay_request(after))
  await evidence.stop()


@pytest.mark.asyncio
async def test_duplicate_consumes_one_shot_when_caller_drops_response(
  tmp_path: Path,
) -> None:
  binding, grant, _, _ = _setup(tmp_path)
  evidence = VisionEvidence(binding, grant, stale_test=True)
  await evidence.attest(_attest(binding))
  await evidence.accept(_frame(binding))
  hook = VisionStaleTest(evidence, tmp_path / "run" / "egia-vision-inspection.sock")
  evidence._test_hook = hook
  await hook.start()
  _, writer = await asyncio.open_unix_connection(str(hook.path))
  writer.write(json.dumps(_replay_request(evidence.snapshot())).encode() + b"\n")
  await writer.drain()
  writer.transport.abort()
  for _ in range(100):
    if not hook.path.exists():
      break
    await asyncio.sleep(0.01)
  assert not hook.path.exists()
  snap = evidence.snapshot()
  assert snap["accepted_count"] == 1
  assert snap["rejection_counts"]["generation_stale"] == 1
  await evidence.stop()


@pytest.mark.asyncio
@pytest.mark.parametrize(
  "failure",
  [
    "first_lstat",
    "first_lstat_replaced",
    "first_lstat_replaced_socket",
    "chmod",
    "lstat",
    "replaced_path",
  ],
)
async def test_post_bind_failure_closes_owned_listener_without_unlinking_replacement(
  tmp_path: Path, monkeypatch, failure: str
) -> None:
  binding, grant, _, _ = _setup(tmp_path)
  evidence = VisionEvidence(binding, grant, stale_test=True)
  hook = VisionStaleTest(evidence, tmp_path / "run" / "egia-vision-inspection.sock")
  created: list[asyncio.AbstractServer] = []
  original_start = asyncio.start_unix_server

  async def capture_start(*args, **kwargs):
    server = await original_start(*args, **kwargs)
    created.append(server)
    return server

  monkeypatch.setattr(asyncio, "start_unix_server", capture_start)
  original_lstat = Path.lstat
  sentinel = b"not-the-owned-socket"
  replacement_socket = None
  if failure.startswith("first_lstat"):
    original_os_lstat = os.lstat
    failed = False

    def fail_first_lstat(path, *args, **kwargs):
      nonlocal failed, replacement_socket
      if created and Path(path) == hook.path and not failed:
        failed = True
        if failure in {"first_lstat_replaced", "first_lstat_replaced_socket"}:
          hook.path.unlink()
          if failure == "first_lstat_replaced_socket":
            replacement_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            replacement_socket.bind(str(hook.path))
            replacement_socket.listen()
          else:
            hook.path.write_bytes(sentinel)
        raise OSError("injected first lstat failure")
      return original_os_lstat(path, *args, **kwargs)

    monkeypatch.setattr(os, "lstat", fail_first_lstat)
  elif failure == "lstat":

    def fail_lstat(path: Path):
      if path == hook.path and path.exists():
        raise OSError("injected lstat failure")
      return original_lstat(path)

    monkeypatch.setattr(Path, "lstat", fail_lstat)
  else:
    original_chmod = Path.chmod

    def fail_chmod(path: Path, mode: int) -> None:
      if path == hook.path:
        if failure == "replaced_path":
          hook.path.unlink()
          hook.path.write_bytes(sentinel)
        raise OSError("injected chmod failure")
      original_chmod(path, mode)

    monkeypatch.setattr(Path, "chmod", fail_chmod)
  with pytest.raises(OSError, match="injected"):
    await hook.start()
  assert hook.server is None
  assert len(created) == 1 and not created[0].is_serving()
  if failure == "first_lstat_replaced_socket":
    assert replacement_socket is not None
    assert hook.path.is_socket()
    assert replacement_socket.getsockname() == str(hook.path)
    _reader, writer = await asyncio.open_unix_connection(str(hook.path))
    writer.close()
    await writer.wait_closed()
    replacement_socket.close()
    hook.path.unlink()
  elif failure == "first_lstat":
    assert hook.path.is_socket()
    with pytest.raises(ConnectionRefusedError):
      await asyncio.open_unix_connection(str(hook.path))
    hook.path.unlink()
  if failure in {"replaced_path", "first_lstat_replaced"}:
    assert hook.path.read_bytes() == sentinel
  elif failure not in {"first_lstat", "first_lstat_replaced_socket"}:
    assert not hook.path.exists()
  await hook.stop()


@pytest.mark.asyncio
async def test_invalid_private_requests_do_not_consume_replay(tmp_path: Path) -> None:
  binding, grant, _, _ = _setup(tmp_path)
  evidence = VisionEvidence(binding, grant, stale_test=True)
  await evidence.attest(_attest(binding))
  await evidence.accept(_frame(binding))
  hook = VisionStaleTest(evidence, tmp_path / "run" / "egia-vision-inspection.sock")
  evidence._test_hook = hook
  await hook.start()
  baseline = evidence.snapshot()
  request = _replay_request(baseline)
  fixture_path = Path(__file__).parents[1] / "fixtures/q01_vision_stale_test/cases.json"
  fixture = json.loads(fixture_path.read_text(encoding="utf-8"))
  assert fixture["schema"] == "q01-egia-vision-stale-test-v1"
  for case in fixture["identity_cases"]:
    response = await _replay(hook.path, {**request, case["field"]: case["replacement"]})
    assert response == {
      "schema_version": 1,
      "op": "replay_latest",
      "injected": False,
      "reason_code": case["reason_code"],
    }
  for malformed in (
    {**request, "jpeg": "bytes"},
    {**request, "schema_version": 2},
    b"{",
    b"x" * 1025,
  ):
    response = await _replay(hook.path, malformed)
    assert response is not None
    assert response["reason_code"] == "invalid_request"
    assert response["injected"] is False
  assert evidence.snapshot() == baseline
  assert hook.path.exists()
  response = await _replay(hook.path, request)
  assert response is not None and response["reason_code"] == "generation_stale"
  await evidence.stop()


@pytest.mark.asyncio
async def test_wrong_uid_and_stop_reject_private_replay(
  tmp_path: Path, monkeypatch
) -> None:
  binding, grant, _, _ = _setup(tmp_path)
  evidence = VisionEvidence(binding, grant, stale_test=True)
  await evidence.attest(_attest(binding))
  await evidence.accept(_frame(binding))
  hook = VisionStaleTest(evidence, tmp_path / "run" / "egia-vision-inspection.sock")
  evidence._test_hook = hook
  await hook.start()
  before = evidence.snapshot()
  monkeypatch.setattr(
    VisionStaleTest, "_peer_uid", staticmethod(lambda _writer: os.getuid() + 1)
  )
  assert await _replay(hook.path, _replay_request(before)) is None
  assert evidence.snapshot() == before
  monkeypatch.undo()
  await evidence.stop()
  assert not hook.path.exists()
  with pytest.raises((FileNotFoundError, ConnectionRefusedError)):
    await _replay(hook.path, _replay_request(before))


@pytest.mark.asyncio
async def test_grant_revocation_clears_replay_material_and_socket(
  tmp_path: Path,
) -> None:
  binding, grant, _, grant_path = _setup(tmp_path)
  evidence = VisionEvidence(binding, grant, stale_test=True)
  await evidence.attest(_attest(binding))
  await evidence.accept(_frame(binding))
  hook = VisionStaleTest(evidence, tmp_path / "run" / "egia-vision-inspection.sock")
  evidence._test_hook = hook
  await hook.start()
  request = _replay_request(evidence.snapshot())
  grant_path.unlink()
  response = await _replay(hook.path, request)
  assert response is not None
  assert response["injected"] is False
  assert response["reason_code"] == "unavailable"
  assert evidence.snapshot()["state"] == "disconnected"
  assert evidence._jpeg is None
  assert evidence._replay_metadata is None
  assert not hook.path.exists()
  await evidence.stop()


@pytest.mark.asyncio
async def test_later_normal_frame_wins_admission_lock_then_replay_retargets(
  tmp_path: Path,
) -> None:
  binding, grant, _, _ = _setup(tmp_path)
  evidence = VisionEvidence(binding, grant, stale_test=True)
  await evidence.attest(_attest(binding))
  await evidence.accept(_frame(binding))
  hook = VisionStaleTest(evidence, tmp_path / "run" / "egia-vision-inspection.sock")
  evidence._test_hook = hook
  await hook.start()
  old = _replay_request(evidence.snapshot())
  async with evidence._admission_lock:
    newer = asyncio.create_task(
      evidence.accept(_frame(binding, generation=2, source_frame_id="frame-2"))
    )
    await asyncio.sleep(0)
    replay = asyncio.create_task(_replay(hook.path, old))
    await asyncio.sleep(0)
  await newer
  response = await replay
  assert response is not None
  assert response["reason_code"] == "target_changed"
  assert response["injected"] is False
  assert evidence.snapshot()["gate_generation"] == 2
  assert evidence.snapshot()["rejected_count"] == 0
  assert hook.path.exists()
  accepted = await _replay(hook.path, _replay_request(evidence.snapshot()))
  assert accepted is not None and accepted["reason_code"] == "generation_stale"
  await evidence.stop()
