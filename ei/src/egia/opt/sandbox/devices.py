"""Linux system-device adapters for the local sandbox playground."""

from __future__ import annotations

import asyncio
import io
import os
import shutil
import tempfile
import wave
from dataclasses import dataclass
from pathlib import Path

from egia.lib.config import (
  SandboxCameraConfig,
  SandboxMicConfig,
  SandboxSpeakerConfig,
)


class DeviceError(RuntimeError):
  """Stable local media-device failure."""


def _which(name: str) -> bool:
  return shutil.which(name) is not None


def _runtime_socket(relative: str) -> bool:
  runtime = os.environ.get("XDG_RUNTIME_DIR")
  root = Path(runtime) if runtime else Path(f"/run/user/{os.getuid()}")
  return (root / relative).exists()


def _audio_program_available(program: str, direction: str) -> bool:
  if not _which(program):
    return False
  if program in {"pw-record", "pw-play"}:
    return bool(os.environ.get("PIPEWIRE_REMOTE")) or _runtime_socket("pipewire-0")
  if program in {"parec", "paplay"}:
    return bool(os.environ.get("PULSE_SERVER")) or _runtime_socket("pulse/native")
  if program == "arecord" or program == "aplay":
    try:
      pcm = Path("/proc/asound/pcm").read_text(encoding="utf-8").lower()
    except OSError:
      return False
    return direction in pcm
  # An explicit deployment adapter is authoritative when its executable exists.
  return True


def validate_wav(audio: bytes, config: SandboxSpeakerConfig) -> None:
  """Validate raw uploaded WAV bounds before any playback adapter."""
  if len(audio) > config.max_wav_bytes:
    raise DeviceError("WAV exceeds the configured byte limit")
  try:
    with wave.open(io.BytesIO(audio), "rb") as stream:
      rate = stream.getframerate()
      frame_count = stream.getnframes()
      channels = stream.getnchannels()
      sample_width = stream.getsampwidth()
      duration = frame_count / rate if rate else 0.0
      if (
        stream.getcomptype() != "NONE"
        or sample_width not in (1, 2, 3, 4)
        or channels < 1
        or frame_count < 1
      ):
        raise DeviceError("speaker input is not a valid PCM WAV")
      if len(stream.readframes(frame_count)) != frame_count * channels * sample_width:
        raise DeviceError("speaker input is not a complete PCM WAV")
  except (EOFError, wave.Error) as exc:
    raise DeviceError("speaker input is not a valid PCM WAV") from exc
  if duration > config.max_play_s:
    raise DeviceError("WAV exceeds the configured duration limit")


@dataclass(slots=True)
class MicrophoneAdapter:
  config: SandboxMicConfig

  @property
  def present(self) -> bool:
    return self.config.enabled and any(
      _audio_program_available(name, "capture") for name in self._candidates()
    )

  def _candidates(self) -> tuple[str, ...]:
    selected = self.config.backend
    if selected == "auto":
      return ("pw-record", "parec", "arecord")
    return {
      "pipewire": ("pw-record",),
      "pulseaudio": ("parec",),
      "alsa": ("arecord",),
    }.get(selected, (selected,))

  def command(self) -> list[str]:
    rate = str(self.config.sample_rate_hz)
    channels = str(self.config.channels)
    for program in self._candidates():
      if not _audio_program_available(program, "capture"):
        continue
      if program == "pw-record":
        command = [program, "--format=s16", f"--rate={rate}", f"--channels={channels}"]
        if self.config.device != "default":
          command.append(f"--target={self.config.device}")
        return [*command, "-"]
      if program == "parec":
        device = (
          "@DEFAULT_SOURCE@" if self.config.device == "default" else self.config.device
        )
        return [
          program,
          "--record",
          f"--device={device}",
          "--raw",
          "--format=s16le",
          f"--rate={rate}",
          f"--channels={channels}",
        ]
      device = "default" if self.config.device == "default" else self.config.device
      return [
        program,
        "-q",
        "-D",
        device,
        "-t",
        "raw",
        "-f",
        "S16_LE",
        "-r",
        rate,
        "-c",
        channels,
      ]
    raise DeviceError("no microphone recorder found")

  async def record(self, stop: asyncio.Event) -> tuple[bytes, bytes]:
    try:
      process = await asyncio.create_subprocess_exec(
        *self.command(),
        stdin=asyncio.subprocess.DEVNULL,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
      )
    except OSError as exc:
      raise DeviceError("could not start the default microphone") from exc
    assert process.stdout is not None
    pcm = bytearray()
    deadline = asyncio.get_running_loop().time() + self.config.capture_duration_s
    try:
      while not stop.is_set() and asyncio.get_running_loop().time() < deadline:
        try:
          chunk = await asyncio.wait_for(process.stdout.read(65536), timeout=0.1)
        except TimeoutError:
          continue
        if not chunk:
          break
        pcm.extend(chunk)
    finally:
      if process.returncode is None:
        process.terminate()
      await process.wait()
    if not pcm:
      detail = ""
      if process.stderr is not None:
        detail = (await process.stderr.read()).decode(errors="replace").strip()
      raise DeviceError(detail or "microphone recording was empty")
    output = io.BytesIO()
    with wave.open(output, "wb") as stream:
      stream.setnchannels(self.config.channels)
      stream.setsampwidth(2)
      stream.setframerate(self.config.sample_rate_hz)
      stream.writeframes(pcm)
    window_bytes = (
      self.config.sample_rate_hz
      * self.config.channels
      * 2
      * self.config.snapshot_window_ms
      // 1000
    )
    return output.getvalue(), bytes(pcm[-window_bytes:])


@dataclass(slots=True)
class CameraAdapter:
  config: SandboxCameraConfig

  @property
  def present(self) -> bool:
    if not self.config.enabled or not _which("gst-launch-1.0"):
      return False
    if self.config.device != "default":
      return Path(self.config.device).exists()
    return any(Path("/dev").glob("video*"))

  def command(self) -> list[str]:
    if self.config.backend not in {"auto", "gstreamer"}:
      raise DeviceError(f"unsupported camera backend: {self.config.backend}")
    if not _which("gst-launch-1.0"):
      raise DeviceError("GStreamer camera capture is unavailable")
    source = (
      ["autovideosrc"]
      if self.config.device == "default"
      else ["v4l2src", f"device={self.config.device}"]
    )
    return [
      "gst-launch-1.0",
      "-q",
      *source,
      "num-buffers=1",
      "!",
      "videoconvert",
      "!",
      "videoscale",
      "!",
      f"video/x-raw,width={self.config.width_px},height={self.config.height_px}",
      "!",
      "jpegenc",
      f"quality={self.config.jpeg_quality}",
      "!",
      "fdsink",
      "fd=1",
    ]

  async def capture(self) -> bytes:
    try:
      process = await asyncio.create_subprocess_exec(
        *self.command(),
        stdin=asyncio.subprocess.DEVNULL,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
      )
    except OSError as exc:
      raise DeviceError("could not start the default camera") from exc
    try:
      stdout, stderr = await process.communicate()
    except asyncio.CancelledError:
      if process.returncode is None:
        process.terminate()
      await process.wait()
      raise
    if process.returncode != 0 or not stdout.startswith(b"\xff\xd8"):
      detail = stderr.decode(errors="replace").strip()
      raise DeviceError(detail or "camera did not return a JPEG frame")
    return stdout


@dataclass(slots=True)
class SpeakerAdapter:
  config: SandboxSpeakerConfig

  @property
  def present(self) -> bool:
    return self.config.enabled and any(
      _audio_program_available(name, "playback") for name in self._candidates()
    )

  def _candidates(self) -> tuple[str, ...]:
    selected = self.config.backend
    if selected == "auto":
      return ("pw-play", "paplay", "aplay")
    return {
      "pipewire": ("pw-play",),
      "pulseaudio": ("paplay",),
      "alsa": ("aplay",),
    }.get(selected, (selected,))

  def command(self, audio_path: str) -> list[str]:
    for program in self._candidates():
      if not _audio_program_available(program, "playback"):
        continue
      if program == "pw-play":
        command = [program]
        if self.config.device != "default":
          command.append(f"--target={self.config.device}")
        return [*command, audio_path]
      if program == "paplay":
        command = [program]
        if self.config.device != "default":
          command.append(f"--device={self.config.device}")
        return [*command, audio_path]
      device = "default" if self.config.device == "default" else self.config.device
      return [program, "-q", "-D", device, audio_path]
    raise DeviceError("no speaker player found")

  def validate(self, audio: bytes) -> None:
    validate_wav(audio, self.config)

  async def play(self, audio: bytes) -> None:
    self.validate(audio)
    # Give every backend a seekable WAV file. In particular, pw-play may not
    # reliably probe container metadata from a pipe and can then replay PCM at
    # its raw-stream defaults instead of the WAV's declared sample rate.
    with tempfile.TemporaryDirectory(prefix="egia-speaker-") as directory:
      audio_path = Path(directory) / "playback.wav"
      audio_path.write_bytes(audio)
      try:
        process = await asyncio.create_subprocess_exec(
          *self.command(str(audio_path)),
          stdin=asyncio.subprocess.DEVNULL,
          stdout=asyncio.subprocess.DEVNULL,
          stderr=asyncio.subprocess.PIPE,
        )
      except OSError as exc:
        raise DeviceError("could not open the default speaker") from exc
      try:
        _, stderr = await process.communicate()
      except asyncio.CancelledError:
        if process.returncode is None:
          process.terminate()
        await process.wait()
        raise
    if process.returncode != 0:
      raise DeviceError(
        stderr.decode(errors="replace").strip() or "speaker playback failed"
      )
