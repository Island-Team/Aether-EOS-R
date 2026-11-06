"""Local sandbox EOS gateway and binary-media protocol tests."""

from __future__ import annotations

import asyncio
import io
import json
import wave
from typing import Any, cast

import pytest
import websockets

from egia.contracts.actions import TerminalStatus
from egia.core.link import EosClient
from egia.core.perception.admission import Admission
from egia.io.northbound import NorthboundServer
from egia.io.state import ServerState
from egia.lib.config import (
  GatewayConfig,
  SandboxRuntimeConfig,
  SandboxSpeakerConfig,
)
from egia.opt.sandbox import devices
from egia.opt.sandbox.devices import SpeakerAdapter
from egia.opt.sandbox.server import PlaygroundServer


def _wav() -> bytes:
  output = io.BytesIO()
  with wave.open(output, "wb") as stream:
    stream.setnchannels(1)
    stream.setsampwidth(2)
    stream.setframerate(16000)
    stream.writeframes(b"\x00\x00" * 160)
  return output.getvalue()


class FakeMic:
  present = True

  async def record(self, stop):
    await stop.wait()
    return _wav(), b"\x00\x00" * 160


class FakeCamera:
  present = True

  async def capture(self):
    return b"\xff\xd8frame\xff\xd9"


class BlockingCamera:
  present = True

  def __init__(self) -> None:
    self.started = asyncio.Event()
    self.cancelled = asyncio.Event()

  async def capture(self):
    self.started.set()
    try:
      await asyncio.Event().wait()
    except asyncio.CancelledError:
      self.cancelled.set()
      raise


class FakeSpeaker:
  present = True

  def __init__(self) -> None:
    self.played: list[bytes] = []

  async def play(self, audio):
    self.played.append(audio)


def test_speaker_uses_seekable_wav_path_for_pipewire(monkeypatch) -> None:
  monkeypatch.setattr(
    devices,
    "_audio_program_available",
    lambda program, direction: program == "pw-play" and direction == "playback",
  )
  adapter = SpeakerAdapter(SandboxSpeakerConfig())

  command = adapter.command("/tmp/playback.wav")

  assert command == ["pw-play", "/tmp/playback.wav"]
  assert "-" not in command


@pytest.fixture
async def playground(unused_tcp_port_factory):
  grpc_port = unused_tcp_port_factory()
  websocket_port = unused_tcp_port_factory()
  config = SandboxRuntimeConfig.model_validate(
    {
      "gateway": {"grpc_port": grpc_port},
      "websocket": {"port": websocket_port},
      "camera": {"max_frames": 3, "min_interval_ms": 0},
      "speaker": {"max_wav_bytes": 1024},
    }
  )
  server = PlaygroundServer(config)
  server.state.mic = cast(Any, FakeMic())
  server.state.camera = cast(Any, FakeCamera())
  speaker = FakeSpeaker()
  server.state.speaker = cast(Any, speaker)
  await server.start()
  yield server, grpc_port, websocket_port, speaker
  await server.stop()


@pytest.mark.asyncio
async def test_playground_is_an_existing_contract_eos_peer(playground) -> None:
  _, grpc_port, _, _ = playground
  client = EosClient(
    GatewayConfig(host="127.0.0.1", grpc_port=grpc_port), backend="sandbox"
  )
  try:
    discovery = await client.initialize()
    assert discovery.robot_model == "local-system-devices"
    assert [device.id for device in discovery.devices] == [
      "mic-0",
      "camera-0",
      "speaker-0",
    ]
    assert client.resources == ()
    assert {item.name for item in client.primitives} == {
      "robot.stop",
      "safety.estop",
    }
    ack = await client.call_primitive("safety.estop", "", {})
    assert ack.command_id
    assert ack.status is TerminalStatus.REJECTED
  finally:
    await client.aclose()


@pytest.mark.asyncio
async def test_gateway_reports_absent_devices_but_omits_routes(playground) -> None:
  server, grpc_port, _, _ = playground
  server.state.mic.present = False
  server.state.camera.present = False
  server.state.speaker.present = False
  client = EosClient(
    GatewayConfig(host="127.0.0.1", grpc_port=grpc_port), backend="sandbox"
  )
  try:
    discovery = await client.initialize(force=True)
    assert {device.id for device in discovery.devices} == {
      "mic-0",
      "camera-0",
      "speaker-0",
    }
    assert all(not device.alive and not device.admitted for device in discovery.devices)
    assert client.resources == ()
    assert {item.name for item in client.primitives} == {
      "robot.stop",
      "safety.estop",
    }
  finally:
    await client.aclose()


@pytest.mark.asyncio
async def test_runtime_status_rediscovery_tracks_hotplugged_devices(playground) -> None:
  server, grpc_port, _, _ = playground
  server.state.speaker.present = False
  client = EosClient(
    GatewayConfig(host="127.0.0.1", grpc_port=grpc_port), backend="sandbox"
  )
  try:
    await client.initialize(force=True)
    assert {item.name for item in client.primitives} == {
      "robot.stop",
      "safety.estop",
    }
    server.state.speaker.present = True

    status = await client.refresh_status()

    assert status["devices_alive"] == status["devices_total"]
    speaker = next(
      device for device in client.discovery.devices if device.id == "speaker-0"
    )
    assert speaker.alive is True
    assert {item.name for item in client.primitives} == {
      "robot.stop",
      "safety.estop",
    }
    assert client.resources == ()
  finally:
    await client.aclose()


@pytest.mark.asyncio
async def test_gateway_captures_frames_through_dedicated_rpc(playground) -> None:
  _, grpc_port, _, _ = playground
  client = EosClient(
    GatewayConfig(host="127.0.0.1", grpc_port=grpc_port), backend="sandbox"
  )
  await client.initialize()
  try:
    frames = await client.capture_frames("camera-0", 2)
    assert len(frames) == 2
    assert all(frame.jpeg.startswith(b"\xff\xd8") for frame in frames)
    assert all(frame.captured_at.endswith("Z") for frame in frames)
  finally:
    await client.aclose()


@pytest.mark.asyncio
async def test_websocket_records_captures_and_streams_wav(playground) -> None:
  _, _, websocket_port, speaker = playground
  async with websockets.connect(
    f"ws://127.0.0.1:{websocket_port}/v1/sandbox"
  ) as websocket:
    await websocket.send(json.dumps({"id": "mic", "op": "mic.start"}))
    assert json.loads(await websocket.recv())["event"] == "accepted"
    await websocket.send(json.dumps({"id": "mic", "op": "mic.stop"}))
    assert json.loads(await websocket.recv())["content_type"] == "audio/wav"
    assert await websocket.recv() == _wav()
    assert json.loads(await websocket.recv())["event"] == "complete"

    await websocket.send(
      json.dumps({"id": "camera", "op": "camera.capture", "count": 2, "interval_ms": 0})
    )
    assert json.loads(await websocket.recv())["event"] == "accepted"
    for index in range(2):
      metadata = json.loads(await websocket.recv())
      assert metadata["index"] == index
      assert await websocket.recv() == b"\xff\xd8frame\xff\xd9"
    assert json.loads(await websocket.recv())["event"] == "complete"

    audio = _wav()
    await websocket.send(json.dumps({"id": "speaker", "op": "speaker.start"}))
    assert json.loads(await websocket.recv())["event"] == "accepted"
    await websocket.send(audio[:20])
    await websocket.send(audio[20:])
    await websocket.send(json.dumps({"id": "speaker", "op": "speaker.finish"}))
    assert json.loads(await websocket.recv())["event"] == "complete"
    assert speaker.played[-1] == audio


@pytest.mark.asyncio
async def test_websocket_status_describes_each_local_device(playground) -> None:
  _, _, websocket_port, _ = playground
  async with websockets.connect(
    f"ws://127.0.0.1:{websocket_port}/v1/sandbox"
  ) as websocket:
    await websocket.send(json.dumps({"id": "status", "op": "status"}))
    reply = json.loads(await websocket.recv())

  assert reply["event"] == "complete"
  assert set(reply["devices"]) == {"camera", "mic", "speaker"}
  assert reply["devices"]["mic"] == {
    "id": "mic-0",
    "enabled": True,
    "available": True,
    "backend": "auto",
    "device": "default",
    "capturing": False,
    "sample_rate_hz": 16000,
    "channels": 1,
  }


@pytest.mark.asyncio
async def test_main_listener_proxies_sandbox_media_over_unix_socket(
  unused_tcp_port_factory, tmp_path
) -> None:
  config = SandboxRuntimeConfig.model_validate(
    {"gateway": {"grpc_port": unused_tcp_port_factory()}}
  )
  playground = PlaygroundServer(config)
  playground.state.mic = cast(Any, FakeMic())
  playground.state.camera = cast(Any, FakeCamera())
  playground.state.speaker = cast(Any, FakeSpeaker())
  media_socket = tmp_path / "sandbox-media.sock"
  await playground.start(media_socket=str(media_socket))
  northbound = NorthboundServer(
    ServerState(version="test"),
    Admission(4),
    port=0,
    sandbox_media_socket=str(media_socket),
  )
  await northbound.start()
  try:
    _, port = northbound.address
    async with websockets.connect(f"ws://127.0.0.1:{port}/v1/sandbox") as websocket:
      await websocket.send(json.dumps({"id": "status", "op": "status"}))
      reply = json.loads(await websocket.recv())
    assert reply["event"] == "complete"
    assert set(reply["devices"]) == {"camera", "mic", "speaker"}
  finally:
    await northbound.stop()
    await playground.stop()


@pytest.mark.asyncio
async def test_sandbox_media_route_failure_does_not_stop_main_listener() -> None:
  northbound = NorthboundServer(
    ServerState(version="test"), Admission(4), port=0, sandbox_media_socket="/missing"
  )
  await northbound.start()
  try:
    _, port = northbound.address
    with pytest.raises(websockets.exceptions.ConnectionClosedError) as closed:
      async with websockets.connect(f"ws://127.0.0.1:{port}/v1/sandbox") as websocket:
        await websocket.recv()
    assert closed.value.rcvd is not None and closed.value.rcvd.code == 1013
    async with websockets.connect(
      f"ws://127.0.0.1:{port}/api/aether/egia/health"
    ) as websocket:
      assert isinstance(json.loads(await websocket.recv()), dict)
  finally:
    await northbound.stop()


@pytest.mark.asyncio
async def test_websocket_mic_cancel_discards_audio_and_releases_device(
  playground,
) -> None:
  _, _, websocket_port, _ = playground
  async with websockets.connect(
    f"ws://127.0.0.1:{websocket_port}/v1/sandbox"
  ) as websocket:
    await websocket.send(json.dumps({"id": "mic", "op": "mic.start"}))
    assert json.loads(await websocket.recv())["event"] == "accepted"
    await websocket.send(json.dumps({"id": "mic", "op": "mic.cancel"}))
    assert json.loads(await websocket.recv())["event"] == "complete"

    await websocket.send(json.dumps({"id": "again", "op": "mic.start"}))
    assert json.loads(await websocket.recv())["event"] == "accepted"
    await websocket.send(json.dumps({"id": "again", "op": "mic.stop"}))
    assert json.loads(await websocket.recv())["event"] == "media"
    assert await websocket.recv() == _wav()
    assert json.loads(await websocket.recv())["event"] == "complete"


@pytest.mark.asyncio
async def test_websocket_rejects_conflicting_recording(playground) -> None:
  _, _, websocket_port, _ = playground
  first = await websockets.connect(f"ws://127.0.0.1:{websocket_port}/v1/sandbox")
  second = await websockets.connect(f"ws://127.0.0.1:{websocket_port}/v1/sandbox")
  try:
    await first.send(json.dumps({"id": "one", "op": "mic.start"}))
    await first.recv()
    await second.send(json.dumps({"id": "two", "op": "mic.start"}))
    error = json.loads(await second.recv())
    assert error["event"] == "error"
    assert error["code"] == "busy"
  finally:
    await first.close()
    await second.close()


@pytest.mark.asyncio
async def test_websocket_rejects_malformed_wav_and_releases_owner(playground) -> None:
  _, _, websocket_port, _ = playground
  async with websockets.connect(
    f"ws://127.0.0.1:{websocket_port}/v1/sandbox"
  ) as websocket:
    await websocket.send(json.dumps({"id": "bad", "op": "speaker.start"}))
    assert json.loads(await websocket.recv())["event"] == "accepted"
    await websocket.send(b"not wav")
    await websocket.send(json.dumps({"id": "bad", "op": "speaker.finish"}))
    error = json.loads(await websocket.recv())
    assert error["event"] == "error"
    assert "valid PCM WAV" in error["message"]

    await websocket.send(json.dumps({"id": "next", "op": "speaker.start"}))
    assert json.loads(await websocket.recv())["event"] == "accepted"


@pytest.mark.asyncio
async def test_websocket_enforces_camera_and_speaker_bounds(playground) -> None:
  _, _, websocket_port, _ = playground
  async with websockets.connect(
    f"ws://127.0.0.1:{websocket_port}/v1/sandbox"
  ) as websocket:
    await websocket.send(
      json.dumps({"id": "camera", "op": "camera.capture", "count": 4})
    )
    assert json.loads(await websocket.recv())["event"] == "accepted"
    assert json.loads(await websocket.recv())["event"] == "error"

    await websocket.send(json.dumps({"id": "speaker", "op": "speaker.start"}))
    assert json.loads(await websocket.recv())["event"] == "accepted"
    await websocket.send(b"x" * 1025)
    error = json.loads(await websocket.recv())
    assert error["event"] == "error"
    assert error["code"] == "limit"


@pytest.mark.asyncio
async def test_websocket_rejects_invalid_jpeg(playground) -> None:
  server, _, websocket_port, _ = playground
  server.state.camera.capture = lambda: _async_value(b"not jpeg")
  async with websockets.connect(
    f"ws://127.0.0.1:{websocket_port}/v1/sandbox"
  ) as websocket:
    await websocket.send(
      json.dumps({"id": "camera", "op": "camera.capture", "count": 1})
    )
    assert json.loads(await websocket.recv())["event"] == "accepted"
    error = json.loads(await websocket.recv())
    assert error["event"] == "error"
    assert "valid JPEG" in error["message"]


@pytest.mark.asyncio
async def test_websocket_disconnect_cancels_owned_camera_capture(playground) -> None:
  server, _, websocket_port, _ = playground
  camera = BlockingCamera()
  server.state.camera = camera
  websocket = await websockets.connect(f"ws://127.0.0.1:{websocket_port}/v1/sandbox")
  await websocket.send(json.dumps({"id": "camera", "op": "camera.capture", "count": 1}))
  assert json.loads(await websocket.recv())["event"] == "accepted"
  await camera.started.wait()
  await websocket.close()
  await asyncio.wait_for(camera.cancelled.wait(), timeout=1)
  assert server.state._camera_capturing is False


async def _async_value(value):
  return value


@pytest.mark.asyncio
async def test_sandbox_process_has_no_core_import() -> None:
  import ast
  from pathlib import Path

  root = Path(__file__).parents[2] / "src/egia/opt/sandbox"
  for source in root.glob("*.py"):
    tree = ast.parse(source.read_text(encoding="utf-8"))
    assert not any(
      isinstance(node, ast.ImportFrom)
      and node.module is not None
      and node.module.startswith("egia.core")
      for node in ast.walk(tree)
    )
