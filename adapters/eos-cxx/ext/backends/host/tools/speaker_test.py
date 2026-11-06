#!/usr/bin/env python3
"""Play a short tone through the selected deployment speaker route."""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import tomllib
from pathlib import Path

from alsa_device import ETC, selected_binding


def speaker_route(etc: Path) -> str:
    binding_path, profile_name = selected_binding(etc)
    binding = tomllib.loads(binding_path.read_text(encoding="utf-8"))
    devices = binding["profile"][profile_name]["device"]
    speakers = [device for device in devices if device.get("class") == "speaker"]
    if len(speakers) != 1:
        raise RuntimeError(f"{profile_name} must declare exactly one speaker device")
    backend = speakers[0]["backend"]
    if backend.get("type") != "aplay":
        raise RuntimeError(f"{profile_name} speaker must use the aplay backend")
    route = backend["aplay"].get("alsa_device", "default")
    if not isinstance(route, str) or not route:
        raise RuntimeError(f"{profile_name} speaker has an invalid ALSA route")
    return route


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--etc", type=Path, default=ETC, help="EOS config root (default: etc/eos)")
    args = parser.parse_args()
    program = shutil.which("speaker-test")
    if program is None:
        print("speaker_test: speaker-test is not installed (install alsa-utils)", file=sys.stderr)
        return 127
    try:
        route = speaker_route(args.etc)
    except (RuntimeError, OSError, KeyError, tomllib.TOMLDecodeError) as error:
        print(f"speaker_test: {error}", file=sys.stderr)
        return 1
    os.execv(program, [program, "-D", route, "-t", "sine", "-f", "440", "-c", "1", "-l", "1"])


if __name__ == "__main__":
    raise SystemExit(main())
