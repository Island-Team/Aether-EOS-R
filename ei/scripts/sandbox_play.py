#!/usr/bin/env python3
"""Manually smoke-test the local sandbox camera and microphone.

Run from a source checkout with ``python scripts/sandbox_play.py``.  The
script reads sandbox device settings from the normal ``etc/egia`` configuration,
so it exercises the same capture backends as the sandbox process.
"""

from __future__ import annotations

import argparse
import asyncio
import sys
import time
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]


def _add_repository_import_root() -> None:
  """Make direct script execution work in a source checkout."""
  source = str(REPOSITORY_ROOT / "src")
  if source not in sys.path:
    sys.path.insert(0, source)


_add_repository_import_root()

from egia.lib.config import load_sandbox_config
from egia.opt.sandbox.devices import DeviceError, MicrophoneAdapter


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
  """Parse the smoke-test options."""
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument(
    "--etc",
    type=Path,
    default=REPOSITORY_ROOT / "etc/egia",
    help="configuration directory (default: %(default)s)",
  )
  parser.add_argument(
    "--var-dir",
    type=Path,
    default=REPOSITORY_ROOT / "var",
    help="directory in which the microphone WAV is saved (default: %(default)s)",
  )
  parser.add_argument(
    "--camera-seconds",
    type=float,
    default=10.0,
    help="camera preview duration (default: %(default)s)",
  )
  parser.add_argument(
    "--mic-seconds",
    type=float,
    default=None,
    help="microphone recording duration; defaults to opt/sandbox.toml",
  )
  return parser.parse_args(argv)


def preview_camera(seconds: float, device: str, width: int, height: int) -> None:
  """Display the default camera for a bounded interval when one is available."""
  try:
    import cv2
  except ImportError:
    print("Camera: skipped (OpenCV is not installed)")
    return

  source: int | str = 0 if device == "default" else device
  camera = cv2.VideoCapture(source)
  if not camera.isOpened():
    camera.release()
    print("Camera: not found; no preview window opened")
    return

  camera.set(cv2.CAP_PROP_FRAME_WIDTH, width)
  camera.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
  title = "EGIA sandbox camera test (press q or Esc to close)"
  print(f"Camera: previewing for up to {seconds:g} seconds")
  deadline = time.monotonic() + seconds
  try:
    while time.monotonic() < deadline:
      ok, frame = camera.read()
      if not ok:
        print("Camera: capture stopped before the preview completed")
        break
      cv2.imshow(title, frame)
      if cv2.waitKey(1) & 0xFF in (ord("q"), 27):
        break
  finally:
    camera.release()
    cv2.destroyAllWindows()


async def record_microphone(etc: Path, var_dir: Path, seconds: float | None) -> None:
  """Record a short WAV using the sandbox microphone backend."""
  sandbox = load_sandbox_config(etc)
  mic_config = sandbox.mic
  if seconds is not None:
    mic_config = mic_config.model_copy(update={"capture_duration_s": seconds})
  adapter = MicrophoneAdapter(mic_config)
  if not adapter.present:
    print("Microphone: skipped (no configured recorder is available)")
    return

  print(f"Microphone: recording for {mic_config.capture_duration_s:g} seconds")
  try:
    wav, _ = await adapter.record(asyncio.Event())
  except DeviceError as exc:
    print(f"Microphone: failed ({exc})")
    return

  output_path = var_dir / "lib/egia/sandbox/mic-check.wav"
  output_path.parent.mkdir(parents=True, exist_ok=True)
  output_path.write_bytes(wav)
  print(f"Microphone: saved {output_path}")


async def run(args: argparse.Namespace) -> None:
  """Run the two hardware checks in the requested order."""
  if args.camera_seconds <= 0:
    raise ValueError("--camera-seconds must be positive")
  if args.mic_seconds is not None and args.mic_seconds <= 0:
    raise ValueError("--mic-seconds must be positive")

  sandbox = load_sandbox_config(args.etc)
  preview_camera(
    args.camera_seconds,
    sandbox.camera.device,
    sandbox.camera.width_px,
    sandbox.camera.height_px,
  )
  await record_microphone(args.etc, args.var_dir, args.mic_seconds)


def main(argv: list[str] | None = None) -> int:
  """Run the sandbox hardware smoke test."""
  args = parse_args(argv)
  try:
    asyncio.run(run(args))
  except (DeviceError, OSError, ValueError) as exc:
    print(f"sandbox hardware test failed: {exc}", file=sys.stderr)
    return 1
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
