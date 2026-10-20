#!/usr/bin/env python3
"""Display live microphone energy and results from the deployed VAD service."""

from __future__ import annotations

import argparse
import array
import contextlib
import json
import math
import queue
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid
import wave
from dataclasses import dataclass, field
from io import BytesIO


SAMPLE_RATE = 16000
CHANNELS = 1
SAMPLE_WIDTH = 2
METER_MS = 100


@dataclass
class Detection:
  speech: bool = False
  probability: float = 0.0
  speakers: list[str] = field(default_factory=list)
  latency_ms: float = 0.0
  error: str = "waiting for first VAD result"


def wav_bytes(pcm: bytes) -> bytes:
  output = BytesIO()
  with wave.open(output, "wb") as wav:
    wav.setnchannels(CHANNELS)
    wav.setsampwidth(SAMPLE_WIDTH)
    wav.setframerate(SAMPLE_RATE)
    wav.writeframes(pcm)
  return output.getvalue()


def post_audio(url: str, pcm: bytes, *, standard: bool, timeout: float) -> dict:
  boundary = f"island-vad-{uuid.uuid4().hex}"
  parts: list[bytes] = []

  def field(name: str, value: str) -> None:
    parts.append(
      f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n'
      f"{value}\r\n".encode()
    )

  parts.append(
    (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="microphone.wav"\r\n'
     'Content-Type: audio/wav\r\n\r\n').encode()
    + wav_bytes(pcm)
    + b"\r\n"
  )
  if standard:
    field("sample_rate_hz", str(SAMPLE_RATE))
    field("channels", str(CHANNELS))
  parts.append(f"--{boundary}--\r\n".encode())
  request = urllib.request.Request(
    url,
    data=b"".join(parts),
    headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
    method="POST",
  )
  with urllib.request.urlopen(request, timeout=timeout) as response:
    return json.loads(response.read())


def speaker_labels(response: dict) -> list[str]:
  labels: set[str] = set()
  for chunk in response.get("chunks", []):
    for speaker in chunk.get("speakers", []):
      if speaker.get("segments"):
        labels.add(str(speaker.get("speaker", "unknown")))
  return sorted(labels)


def worker(
  windows: queue.Queue[bytes], state: Detection, lock: threading.Lock,
  endpoint: str, include_speakers: bool, timeout: float,
) -> None:
  while True:
    pcm = windows.get()
    if not pcm:
      return
    started = time.monotonic()
    try:
      result = post_audio(f"{endpoint}/v1/audio/vad", pcm, standard=True, timeout=timeout)
      speakers: list[str] = []
      if include_speakers:
        speakers = speaker_labels(
          post_audio(f"{endpoint}/segments", pcm, standard=False, timeout=timeout)
        )
      with lock:
        state.speech = bool(result.get("speech"))
        state.probability = float(result.get("probability", 0.0))
        state.speakers = speakers
        state.latency_ms = (time.monotonic() - started) * 1000
        state.error = ""
    except (OSError, ValueError, json.JSONDecodeError, urllib.error.HTTPError) as error:
      detail = error.read().decode(errors="replace") if isinstance(error, urllib.error.HTTPError) else str(error)
      with lock:
        state.error = detail.strip()


def meter(rms: int, state: Detection, tty: bool) -> None:
  dbfs = 20 * math.log10(max(rms, 1) / 32768)
  level = min(30, int(max(0.0, dbfs + 60.0) / 2.0))
  bar = "#" * level + "." * (30 - level)
  if state.error:
    verdict = f"VAD ERROR: {state.error}"
  else:
    verdict = "VOICE" if state.speech else "silence"
    verdict += f" p={state.probability:.2f} vad={state.latency_ms:.0f}ms"
    if state.speakers:
      verdict += " speakers=" + ",".join(state.speakers)
  line = f"energy [{bar}] rms={rms:5d} {dbfs:6.1f} dBFS | {verdict}"
  print(("\r" if tty else "") + line.ljust(120), end="" if tty else "\n", flush=True)


def pcm_rms(block: bytes) -> int:
  samples = array.array("h")
  samples.frombytes(block)
  if sys.byteorder != "little":
    samples.byteswap()
  return int(math.sqrt(sum(sample * sample for sample in samples) / len(samples)))


def main() -> int:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--device", default="default", help="ALSA capture device (default: %(default)s)")
  parser.add_argument("--endpoint", default="http://127.0.0.1:18018", help="VAD service base URL")
  parser.add_argument("--window-ms", type=int, default=500, help="VAD window duration (default: %(default)s)")
  parser.add_argument("--timeout", type=float, default=30.0, help="HTTP request timeout in seconds")
  parser.add_argument("--speakers", action="store_true", help="query pyannote's optional /segments endpoint")
  args = parser.parse_args()
  if args.window_ms < METER_MS or args.window_ms % METER_MS:
    parser.error(f"--window-ms must be a multiple of {METER_MS} and at least {METER_MS}")
  if shutil.which("arecord") is None:
    parser.error("arecord is not installed (install alsa-utils)")

  frame_bytes = SAMPLE_RATE * METER_MS // 1000 * SAMPLE_WIDTH
  command = [
    "arecord", "-q", "-D", args.device, "-t", "raw", "-f", "S16_LE",
    "-r", str(SAMPLE_RATE), "-c", str(CHANNELS),
  ]
  process = subprocess.Popen(command, stdout=subprocess.PIPE)
  assert process.stdout is not None
  windows: queue.Queue[bytes] = queue.Queue(maxsize=1)
  state = Detection()
  lock = threading.Lock()
  thread = threading.Thread(
    target=worker,
    args=(windows, state, lock, args.endpoint.rstrip("/"), args.speakers, args.timeout),
    daemon=True,
  )
  thread.start()
  accumulated = bytearray()
  tty = sys.stdout.isatty()
  print(f"Capturing ALSA {args.device!r}; Ctrl-C to stop. VAD: {args.endpoint}")
  try:
    while True:
      block = process.stdout.read(frame_bytes)
      if len(block) != frame_bytes:
        raise RuntimeError("microphone capture ended unexpectedly")
      accumulated.extend(block)
      with lock:
        snapshot = Detection(**vars(state))
      meter(pcm_rms(block), snapshot, tty)
      wanted = SAMPLE_RATE * args.window_ms // 1000 * SAMPLE_WIDTH
      if len(accumulated) >= wanted:
        window = bytes(accumulated[:wanted])
        del accumulated[:wanted]
        try:
          windows.put_nowait(window)
        except queue.Full:
          pass
  except KeyboardInterrupt:
    if tty:
      print()
    return 0
  except RuntimeError as error:
    if tty:
      print()
    print(f"vad_test: {error}", file=sys.stderr)
    return 1
  finally:
    with contextlib.suppress(ProcessLookupError, KeyboardInterrupt):
      if process.poll() is None:
        process.terminate()
      process.wait(timeout=2)
    try:
      windows.put_nowait(b"")
    except queue.Full:
      pass


if __name__ == "__main__":
  raise SystemExit(main())
