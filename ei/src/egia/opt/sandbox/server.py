"""EOS-compatible local playground and binary-media WebSocket server."""

from __future__ import annotations

import asyncio
import json
import secrets
import time
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, cast
from uuid import UUID

import grpc
from google.protobuf import struct_pb2
from grpc_health.v1 import health_pb2, health_pb2_grpc
from websockets.asyncio.server import Server, serve, unix_serve
from websockets.exceptions import ConnectionClosed

from egia.lib.config import SandboxRuntimeConfig
from egia.lib.logger import get_logger
from egia.opt.sandbox.devices import (
  CameraAdapter,
  DeviceError,
  MicrophoneAdapter,
  SpeakerAdapter,
  validate_wav,
)
from island.generated.eos import (
  capabilities_pb2,
  capabilities_pb2_grpc,
  gateway_pb2_grpc,
  skills_pb2_grpc,
)
from island.generated.eos.perception import camera_pb2
from island.generated.eos.robot import descriptor_pb2, primitive_pb2
from island.generated.eos.skills import catalog_pb2
from island.generated.eos.system import resource_pb2


def _uuid7() -> str:
  timestamp = int(time.time() * 1000) & ((1 << 48) - 1)
  value = timestamp << 80
  value |= 0x7 << 76
  value |= secrets.randbits(12) << 64
  value |= 0x2 << 62
  value |= secrets.randbits(62)
  return str(UUID(int=value))


def _struct(value: dict[str, Any]) -> struct_pb2.Struct:
  message = struct_pb2.Struct()
  message.update(value)
  return message


class PlaygroundState:
  """Own local devices, replaceable resource state, and watch notifications."""

  def __init__(self, config: SandboxRuntimeConfig) -> None:
    self.config = config
    self._log = get_logger("egia.opt.sandbox")
    self.robot_id = _uuid7()
    self.robot_name = "sandbox-playground"
    self.started = datetime.now(UTC)
    self.mic = MicrophoneAdapter(config.mic)
    self.camera = CameraAdapter(config.camera)
    self.speaker = SpeakerAdapter(config.speaker)
    prefix = f"robots/{self.robot_name}/devices"
    self.uris = {
      "mic": f"{prefix}/mic-0/state",
      "camera": f"{prefix}/camera-0/state",
      "speaker": f"{prefix}/speaker-0/state",
    }
    self._sequence = 0
    self._watchers: dict[str, set[asyncio.Queue[str]]] = {}
    self._mic_owner: object | None = None
    self._mic_stop: asyncio.Event | None = None
    self._mic_task: asyncio.Task[tuple[bytes, bytes]] | None = None
    self._camera_jpeg: bytes | None = None
    self._camera_capturing = False
    self._speaker_owner: object | None = None
    self._speaker_playing = False
    self._last_tone: str | None = None
    self._mic_lock = asyncio.Lock()
    self._camera_lock = asyncio.Lock()
    self._speaker_lock = asyncio.Lock()

  def _next(self) -> int:
    self._sequence += 1
    return self._sequence

  async def _changed(self, kind: str) -> None:
    uri = self.uris[kind]
    for queue in tuple(self._watchers.get(uri, set())):
      if queue.empty():
        queue.put_nowait(uri)

  def resources(self) -> list[dict[str, str | None]]:
    """Return semantic resources; raw device state lives in Discover."""
    return []

  def device_status(self) -> dict[str, dict[str, Any]]:
    """Return configured device identity plus current playground state."""
    return {
      "mic": {
        "id": "mic-0",
        "enabled": self.config.mic.enabled,
        "available": self.mic.present,
        "backend": self.config.mic.backend,
        "device": self.config.mic.device,
        "capturing": self._mic_task is not None and not self._mic_task.done(),
        "sample_rate_hz": self.config.mic.sample_rate_hz,
        "channels": self.config.mic.channels,
      },
      "camera": {
        "id": "camera-0",
        "enabled": self.config.camera.enabled,
        "available": self.camera.present,
        "backend": self.config.camera.backend,
        "device": self.config.camera.device,
        "capturing": self._camera_capturing,
        "width_px": self.config.camera.width_px,
        "height_px": self.config.camera.height_px,
      },
      "speaker": {
        "id": "speaker-0",
        "enabled": self.config.speaker.enabled,
        "available": self.speaker.present,
        "backend": self.config.speaker.backend,
        "device": self.config.speaker.device,
        "playing": self._speaker_playing,
      },
    }

  def snapshot(self, uri: str) -> dict[str, Any] | None:
    del uri
    return None

  def subscribe(self, uri: str) -> asyncio.Queue[str]:
    queue: asyncio.Queue[str] = asyncio.Queue(maxsize=1)
    self._watchers.setdefault(uri, set()).add(queue)
    return queue

  def unsubscribe(self, uri: str, queue: asyncio.Queue[str]) -> None:
    self._watchers.get(uri, set()).discard(queue)

  async def start_mic(self, owner: object) -> None:
    async with self._mic_lock:
      if not self.mic.present:
        raise DeviceError("default microphone is unavailable")
      if self._mic_owner is not None:
        raise DeviceError("busy")
      self._mic_owner = owner
      self._mic_stop = asyncio.Event()
      self._mic_task = asyncio.create_task(self.mic.record(self._mic_stop))
      self._mic_task.add_done_callback(self._mic_recording_done)
      self._next()
      await self._changed("mic")

  def _mic_recording_done(self, task: asyncio.Task[tuple[bytes, bytes]]) -> None:
    if not task.cancelled():
      # Observe adapter failures even when a client never asks for mic.stop;
      # awaiting the task later still raises the same exception.
      task.exception()
    asyncio.create_task(self._notify_mic_completion(task))

  async def _notify_mic_completion(
    self, task: asyncio.Task[tuple[bytes, bytes]]
  ) -> None:
    async with self._mic_lock:
      if self._mic_task is not task:
        return
      self._next()
    await self._changed("mic")

  async def stop_mic(self, owner: object) -> bytes:
    async with self._mic_lock:
      if self._mic_owner is not owner or self._mic_task is None:
        raise DeviceError("this connection does not own a recording")
      if self._mic_stop is not None:
        self._mic_stop.set()
      task = self._mic_task
    try:
      wav, _ = await task
    finally:
      async with self._mic_lock:
        self._mic_task = None
        self._mic_owner = None
        self._mic_stop = None
        self._next()
      await self._changed("mic")
    return wav

  async def cancel_mic(self, owner: object) -> None:
    async with self._mic_lock:
      if self._mic_owner is not owner or self._mic_task is None:
        return
      if self._mic_stop is not None:
        self._mic_stop.set()
      task = self._mic_task
    try:
      await task
    except (asyncio.CancelledError, DeviceError):
      pass
    async with self._mic_lock:
      self._mic_task = None
      self._mic_owner = None
      self._mic_stop = None
      self._next()
    await self._changed("mic")

  async def capture(self, count: int, interval_ms: int):
    if not self.camera.present:
      raise DeviceError("default camera is unavailable")
    if count < 1 or count > self.config.camera.max_frames:
      raise DeviceError("camera frame count is outside the configured limit")
    if interval_ms < 0 or interval_ms > 60000:
      raise DeviceError("camera frame interval is outside the protocol limit")
    if count > 1 and interval_ms < self.config.camera.min_interval_ms:
      raise DeviceError("camera frame interval is below the configured minimum")
    if self._camera_lock.locked():
      raise DeviceError("busy")
    async with self._camera_lock:
      self._camera_capturing = True
      self._next()
      await self._changed("camera")
      try:
        for index in range(count):
          jpeg = await self.camera.capture()
          if not (jpeg.startswith(b"\xff\xd8") and jpeg.endswith(b"\xff\xd9")):
            raise DeviceError("camera did not return a valid JPEG frame")
          self._camera_jpeg = jpeg
          self._next()
          await self._changed("camera")
          yield index, jpeg
          if index + 1 < count:
            await asyncio.sleep(interval_ms / 1000)
      finally:
        self._camera_capturing = False
        self._next()
        await self._changed("camera")

  async def claim_speaker(self, owner: object) -> None:
    async with self._speaker_lock:
      if not self.speaker.present:
        raise DeviceError("default speaker is unavailable")
      if self._speaker_owner is not None:
        raise DeviceError("busy")
      self._speaker_owner = owner

  async def release_speaker(self, owner: object) -> None:
    async with self._speaker_lock:
      if self._speaker_owner is owner:
        self._speaker_owner = None

  async def play(self, audio: bytes, *, tone: str, owner: object | None = None) -> None:
    claimed = owner is not None and self._speaker_owner is owner
    if owner is not None and not claimed:
      raise DeviceError("this connection does not own speaker playback")
    if owner is None:
      await self.claim_speaker(self)
      owner = self
    try:
      validate_wav(audio, self.config.speaker)
      self._speaker_playing = True
      self._next()
      await self._changed("speaker")
      await self.speaker.play(audio)
      self._last_tone = tone
    finally:
      if self._speaker_playing:
        self._speaker_playing = False
        self._next()
        await self._changed("speaker")
      await self.release_speaker(owner)


class _Health(health_pb2_grpc.HealthServicer):
  async def Check(self, request, context):
    if request.service:
      await context.abort(grpc.StatusCode.NOT_FOUND, "service name unknown")
    return health_pb2.HealthCheckResponse(status=health_pb2.HealthCheckResponse.SERVING)


class PlaygroundGateway(
  gateway_pb2_grpc.GatewayServiceServicer,
  capabilities_pb2_grpc.CapabilityServiceServicer,
  skills_pb2_grpc.SkillServiceServicer,
):
  def __init__(self, state: PlaygroundState) -> None:
    self.state = state
    self._calls = 0
    self._operations: dict[str, capabilities_pb2.Operation] = {}

  async def Discover(self, request, context):
    del request, context
    devices = []
    definitions = (
      (
        "mic-0",
        "microphone",
        self.state.config.mic.enabled,
        self.state.mic.present,
        "listening" if self.state._mic_task is not None else "idle",
      ),
      (
        "camera-0",
        "camera",
        self.state.config.camera.enabled,
        self.state.camera.present,
        "capturing" if self.state._camera_capturing else "idle",
      ),
      (
        "speaker-0",
        "speaker",
        self.state.config.speaker.enabled,
        self.state.speaker.present,
        "active" if self.state._speaker_playing else "idle",
      ),
    )
    for (
      device_id,
      device_class,
      enabled,
      present,
      state,
    ) in definitions:
      if not enabled:
        continue
      devices.append(
        descriptor_pb2.DeviceDescriptor(
          id=device_id,
          part="default",
          state=state,
          health="ready" if present else "unavailable",
          description=f"system default {device_class}",
          primitives=[],
          **{"class": device_class},
        )
      )
    healthy = all(not item[2] or item[3] for item in definitions)
    robot_primitives = [
      primitive_pb2.PrimitiveBinding(
        name=name,
        version="1.0.0",
        kind="direct",
        health="ready",
        description=description,
        limits=_struct({}),
      )
      for name, description in (
        ("robot.stop", "Stop all active sandbox robot parts."),
        ("safety.estop", "Engage the sandbox robot emergency stop."),
      )
    ]
    return descriptor_pb2.DiscoverResponse(
      robot=descriptor_pb2.RobotDescriptor(
        id=self.state.robot_id,
        name=self.state.robot_name,
        model="local-system-devices",
        description="Local camera, microphone, and speaker playground",
        state="idle",
        health="ready" if healthy else "degraded",
        estop=False,
        primitives=robot_primitives,
      ),
      devices=devices,
    )

  async def GetStatus(self, request, context):
    del request, context
    devices = self.state.device_status()
    enabled = [item for item in devices.values() if item["enabled"]]
    alive = sum(bool(item["available"]) for item in enabled)
    total = len(enabled)
    ready = total > 0 and alive == total
    now = datetime.now(UTC)
    return _struct(
      {
        "robot_id": self.state.robot_id,
        "robot_name": self.state.robot_name,
        "status": "ready" if ready else "degraded",
        "version": "1.0.0",
        "started_at": self.state.started.isoformat().replace("+00:00", "Z"),
        "uptime_seconds": (now - self.state.started).total_seconds(),
        "correlation_id": _uuid7(),
        "subsystems": [],
        "devices_total": total,
        "devices_alive": alive,
        "robots": [
          {
            "id": self.state.robot_id,
            "robot_name": self.state.robot_name,
            "model": "local-system-devices",
            "base_uri": f"robots/{self.state.robot_name}",
            "status": "ready" if ready else "degraded",
            "mode": "auto",
            "estop": not ready,
            "devices_total": total,
            "devices_alive": alive,
            "faults": [],
          }
        ],
      }
    )

  async def ListResources(self, request, context):
    del request, context
    values = struct_pb2.ListValue()
    values.extend(cast(Any, self.state.resources()))
    return values

  async def ReadResource(self, request, context):
    value = self.state.snapshot(request.uri)
    if value is None:
      await context.abort(grpc.StatusCode.NOT_FOUND, "unknown resource")
    assert value is not None
    return _struct(value)

  async def WatchResource(self, request, context):
    if self.state.snapshot(request.uri) is None:
      await context.abort(grpc.StatusCode.NOT_FOUND, "unknown resource")
    queue = self.state.subscribe(request.uri)
    try:
      while True:
        yield resource_pb2.ResourceChanged(uri=await queue.get())
    finally:
      self.state.unsubscribe(request.uri, queue)

  async def ListSkills(self, request, context):
    del request, context
    return catalog_pb2.ListSkillsResponse()

  async def ListCapabilities(self, request, context):
    del request, context
    response = capabilities_pb2.ListCapabilitiesResponse()
    for name in ("robot.stop", "safety.estop"):
      response.capabilities.add(
        name=f"runtime/{self.state.robot_id}/{name}@1.0.0",
        primitive=name,
        version="1.0.0",
        available=True,
      )
    return response

  async def SubmitOperation(self, request, context):
    del context
    primitive = request.capability_name.rsplit("/", 1)[-1].split("@", 1)[0]
    self._calls += 1
    acknowledgement = _struct(
      {
        "type": "primitive.ack",
        "primitive": primitive,
        "primitive_version": "1.0.0",
        "accepted": False,
        "reason": "primitive is not available",
        "reason_code": "rejected.invalid_state",
        "command_id": request.operation_id,
        "sequence": self._calls,
        "result": {"status": "rejected"},
      }
    )
    operation = capabilities_pb2.Operation(
      operation_id=request.operation_id,
      status=capabilities_pb2.OPERATION_STATUS_REJECTED,
      accepted=False,
      acknowledgement=acknowledgement,
    )
    self._operations[request.operation_id] = operation
    return operation

  async def GetOperation(self, request, context):
    operation = self._operations.get(request.operation_id)
    if operation is None:
      await context.abort(grpc.StatusCode.NOT_FOUND, "unknown operation")
    return operation

  async def WatchOperation(self, request, context):
    yield await self.GetOperation(request, context)

  async def CancelOperation(self, request, context):
    del context
    self._calls += 1
    acknowledgement = _struct(
      {
        "type": "primitive.ack",
        "primitive": "robot.stop",
        "primitive_version": "1.0.0",
        "accepted": False,
        "reason": "command is not cancellable",
        "reason_code": "rejected.invalid_state",
        "command_id": request.operation_id,
        "sequence": self._calls,
        "result": {"status": "rejected"},
      }
    )
    return capabilities_pb2.Operation(
      operation_id=request.operation_id,
      status=capabilities_pb2.OPERATION_STATUS_REJECTED,
      accepted=False,
      acknowledgement=acknowledgement,
    )

  async def CaptureFrames(self, request, context):
    if request.robot_name != self.state.robot_name or request.device_id != "camera-0":
      await context.abort(grpc.StatusCode.NOT_FOUND, "camera not found")
    if request.frame_count not in {1, 2}:
      await context.abort(
        grpc.StatusCode.INVALID_ARGUMENT, "frame_count must be one or two"
      )
    frames = []
    interval_ms = self.state.config.camera.min_interval_ms
    try:
      async for _, jpeg in self.state.capture(request.frame_count, interval_ms):
        frames.append(
          camera_pb2.CameraFrame(
            jpeg=jpeg,
            width_px=self.state.config.camera.width_px,
            height_px=self.state.config.camera.height_px,
            captured_at=datetime.now(UTC).isoformat().replace("+00:00", "Z"),
          )
        )
    except DeviceError as exc:
      await context.abort(grpc.StatusCode.UNAVAILABLE, str(exc))
    return camera_pb2.CaptureFramesResponse(frames=frames)


async def _send(websocket, value: dict[str, Any]) -> None:
  await websocket.send(json.dumps(value, separators=(",", ":")))


class PlaygroundWebSocket:
  def __init__(self, state: PlaygroundState) -> None:
    self.state = state

  async def handle(self, websocket) -> None:
    if websocket.request.path != "/v1/sandbox":
      await websocket.close(1008, "unknown sandbox route")
      return
    speaker_id: str | None = None
    speaker_audio = bytearray()
    try:
      async for message in websocket:
        if isinstance(message, bytes):
          if speaker_id is None:
            await _send(
              websocket,
              {
                "event": "error",
                "code": "unexpected_binary",
                "message": "speaker.start is required",
              },
            )
            continue
          speaker_audio.extend(message)
          if len(speaker_audio) > self.state.config.speaker.max_wav_bytes:
            failed_id = speaker_id
            await self.state.release_speaker(websocket)
            speaker_id = None
            speaker_audio.clear()
            await _send(
              websocket,
              {
                "id": failed_id,
                "event": "error",
                "code": "limit",
                "message": "WAV exceeds the configured byte limit",
              },
            )
          continue
        try:
          request = json.loads(message)
          if not isinstance(request, dict):
            raise TypeError
          request_id = request["id"]
          operation = request["op"]
          if (
            not isinstance(request_id, str)
            or not request_id
            or not isinstance(operation, str)
            or not operation
          ):
            raise TypeError
        except (KeyError, TypeError, ValueError, json.JSONDecodeError):
          await _send(
            websocket,
            {
              "event": "error",
              "code": "invalid_request",
              "message": "JSON object with id and op is required",
            },
          )
          continue
        try:
          if operation == "status":
            await _send(
              websocket,
              {
                "id": request_id,
                "event": "complete",
                "devices": self.state.device_status(),
              },
            )
          elif operation == "mic.start":
            await self.state.start_mic(websocket)
            await _send(websocket, {"id": request_id, "event": "accepted"})
          elif operation == "mic.stop":
            wav = await self.state.stop_mic(websocket)
            await _send(
              websocket,
              {
                "id": request_id,
                "event": "media",
                "content_type": "audio/wav",
                "bytes": len(wav),
              },
            )
            await websocket.send(wav)
            await _send(websocket, {"id": request_id, "event": "complete"})
          elif operation == "mic.cancel":
            await self.state.cancel_mic(websocket)
            await _send(websocket, {"id": request_id, "event": "complete"})
          elif operation == "camera.capture":
            count = request.get("count", 1)
            interval = request.get("interval_ms", 0)
            if (
              not isinstance(count, int)
              or isinstance(count, bool)
              or not isinstance(interval, int)
              or isinstance(interval, bool)
            ):
              raise DeviceError("camera count and interval_ms must be integers")
            await _send(websocket, {"id": request_id, "event": "accepted"})
            frames = self.state.capture(count, interval)
            disconnected = asyncio.create_task(websocket.wait_closed())
            next_frame: asyncio.Task[tuple[int, bytes]] | None = None
            try:
              while True:
                next_frame = asyncio.create_task(anext(frames))
                done, _ = await asyncio.wait(
                  {next_frame, disconnected}, return_when=asyncio.FIRST_COMPLETED
                )
                if disconnected in done:
                  next_frame.cancel()
                  await asyncio.gather(next_frame, return_exceptions=True)
                  return
                try:
                  index, jpeg = next_frame.result()
                except StopAsyncIteration:
                  break
                await _send(
                  websocket,
                  {
                    "id": request_id,
                    "event": "media",
                    "content_type": "image/jpeg",
                    "index": index,
                    "bytes": len(jpeg),
                  },
                )
                await websocket.send(jpeg)
            finally:
              if next_frame is not None and not next_frame.done():
                next_frame.cancel()
                await asyncio.gather(next_frame, return_exceptions=True)
              disconnected.cancel()
              await asyncio.gather(disconnected, return_exceptions=True)
              await frames.aclose()
            await _send(websocket, {"id": request_id, "event": "complete"})
          elif operation == "speaker.start":
            if speaker_id is not None:
              raise DeviceError("busy")
            await self.state.claim_speaker(websocket)
            speaker_id = request_id
            speaker_audio.clear()
            await _send(websocket, {"id": request_id, "event": "accepted"})
          elif operation == "speaker.finish":
            if speaker_id is None or request_id != speaker_id:
              raise DeviceError("this connection does not own speaker playback")
            audio = bytes(speaker_audio)
            speaker_id = None
            speaker_audio.clear()
            playback = asyncio.create_task(
              self.state.play(audio, tone="stream", owner=websocket)
            )
            disconnected = asyncio.create_task(websocket.wait_closed())
            try:
              done, _ = await asyncio.wait(
                {playback, disconnected}, return_when=asyncio.FIRST_COMPLETED
              )
              if disconnected in done:
                playback.cancel()
                await asyncio.gather(playback, return_exceptions=True)
                return
              await playback
            except asyncio.CancelledError:
              playback.cancel()
              await asyncio.gather(playback, return_exceptions=True)
              raise
            finally:
              disconnected.cancel()
              await asyncio.gather(disconnected, return_exceptions=True)
            await _send(websocket, {"id": request_id, "event": "complete"})
          else:
            raise DeviceError("unknown operation")
        except (DeviceError, TypeError, ValueError) as exc:
          await _send(
            websocket,
            {
              "id": request_id,
              "event": "error",
              "code": "busy" if str(exc) == "busy" else "invalid_operation",
              "message": str(exc),
            },
          )
    except ConnectionClosed:
      pass
    finally:
      await self.state.cancel_mic(websocket)
      await self.state.release_speaker(websocket)


class PlaygroundServer:
  """Run the local gRPC and WebSocket listeners as one lifecycle."""

  def __init__(
    self,
    config: SandboxRuntimeConfig,
  ) -> None:
    self.state = PlaygroundState(config)
    self.gateway = PlaygroundGateway(self.state)
    self._grpc: grpc.aio.Server | None = None
    self._websocket: Server | None = None
    self._media_socket: Path | None = None

  async def start(self, *, media_socket: str | None = None) -> None:
    """Start gRPC and the media transport, using Unix IPC when provided."""
    config = self.state.config
    grpc_server = grpc.aio.server()
    gateway_pb2_grpc.add_GatewayServiceServicer_to_server(self.gateway, grpc_server)
    capabilities_pb2_grpc.add_CapabilityServiceServicer_to_server(
      self.gateway, grpc_server
    )
    skills_pb2_grpc.add_SkillServiceServicer_to_server(self.gateway, grpc_server)
    health_pb2_grpc.add_HealthServicer_to_server(_Health(), grpc_server)
    bound = grpc_server.add_insecure_port(
      f"{config.gateway.host}:{config.gateway.grpc_port}"
    )
    if bound == 0:
      raise RuntimeError("sandbox gRPC listener could not bind")
    await grpc_server.start()
    self._grpc = grpc_server
    handler = PlaygroundWebSocket(self.state)
    if media_socket is None:
      if config.websocket.host is None or config.websocket.port is None:
        await grpc_server.stop(grace=0)
        self._grpc = None
        raise RuntimeError("sandbox media transport requires EGIA proxy IPC")
      self._websocket = await serve(
        handler.handle,
        config.websocket.host,
        config.websocket.port,
        max_size=config.websocket.max_message_bytes,
      )
    else:
      socket_path = Path(media_socket)
      socket_path.parent.mkdir(parents=True, exist_ok=True)
      socket_path.unlink(missing_ok=True)
      self._media_socket = socket_path
      self._websocket = await unix_serve(
        handler.handle, media_socket, max_size=config.websocket.max_message_bytes
      )

  async def stop(self) -> None:
    if self._websocket is not None:
      self._websocket.close()
      await self._websocket.wait_closed()
    if self._grpc is not None:
      await self._grpc.stop(grace=1.0)
    if self._media_socket is not None:
      self._media_socket.unlink(missing_ok=True)
      self._media_socket = None
