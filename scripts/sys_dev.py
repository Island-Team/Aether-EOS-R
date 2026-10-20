#!/usr/bin/env python3
"""Inspect and choose host audio defaults without changing EOS configuration."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path


CAMERA_LINE = re.compile(r"^(?P<name>.*?)\s{2,}(?P<serial>\S+)\s{2,}(?P<firmware>\S+)$")
COLOR = {
    "heading": "\033[1;36m",
    "current": "\033[32m",
    "muted": "\033[2m",
    "reset": "\033[0m",
}
PREFERRED_AUDIO = {
    "mic": "alsa_input.usb-KTMicro_TAKSTAR_MS-580USB_319D_000D-00.mono-fallback",
    "speaker": "alsa_output.usb-ACTIONS_Pebble_V3-00.analog-stereo",
}


def color(text: str, name: str) -> str:
    if not sys.stdout.isatty() or "NO_COLOR" in os.environ:
        return text
    return f"{COLOR[name]}{text}{COLOR['reset']}"


def camera_routes_from_text(text: str) -> list[tuple[str, str]]:
    result: list[tuple[str, str]] = []
    for line in text.splitlines():
        match = CAMERA_LINE.match(line.strip())
        if match is None or match["name"] == "Device Name":
            continue
        entry = (match["serial"], f"{match['name'].strip()} - firmware {match['firmware']}")
        if entry not in result:
            result.append(entry)
    return result


def camera_routes() -> list[tuple[str, str]]:
    program = "/opt/ros/humble/bin/rs-enumerate-devices"
    if not Path(program).is_file():
        located = shutil.which("rs-enumerate-devices")
        if located is None:
            raise RuntimeError("rs-enumerate-devices is not installed")
        program = located
    try:
        completed = subprocess.run([program, "--short"], check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as error:
        detail = error.stderr.strip() or f"exit {error.returncode}"
        raise RuntimeError(f"{program} --short failed: {detail}") from error
    return camera_routes_from_text(completed.stdout)


def pulse_endpoints_from_text(text: str, kind: str) -> list[tuple[str, str]]:
    result: list[tuple[str, str]] = []
    for block in re.split(rf"(?m)^{kind.title()} #\d+\n", text):
        name = re.search(r"(?m)^\s*Name: (.+)$", block)
        description = re.search(r"(?m)^\s*Description: (.+)$", block)
        card_name = re.search(r'(?m)^\s*alsa\.card_name = "(.+)"$', block)
        monitor = re.search(r"(?m)^\s*Monitor of Sink: (.+)$", block)
        if name is None or description is None or card_name is None:
            continue
        if kind == "source" and monitor is not None and monitor.group(1) != "n/a":
            continue
        endpoint = (name.group(1), description.group(1))
        if endpoint not in result:
            result.append(endpoint)
    return result


def _pactl(arguments: list[str]) -> str:
    program = shutil.which("pactl")
    if program is None:
        raise RuntimeError("pactl is not installed")
    try:
        return subprocess.run(
            [program, *arguments], check=True, capture_output=True, text=True,
            env={**os.environ, "LC_ALL": "C"},
        ).stdout
    except subprocess.CalledProcessError as error:
        detail = error.stderr.strip() or f"exit {error.returncode}"
        raise RuntimeError(f"pactl {' '.join(arguments)} failed: {detail}") from error


def audio_role_kind(role: str) -> str:
    return "source" if role == "mic" else "sink"


def audio_routes(role: str) -> list[tuple[str, str]]:
    kind = audio_role_kind(role)
    return pulse_endpoints_from_text(_pactl(["list", f"{kind}s"]), kind)


def audio_default(role: str) -> str | None:
    return _pactl([f"get-default-{audio_role_kind(role)}"]).strip() or None


def print_audio_role(role: str) -> None:
    title = "MICROPHONE" if role == "mic" else "SPEAKER"
    endpoints = audio_routes(role)
    selected = audio_default(role)
    print(color(f"=== {title} ===", "heading"))
    if selected is None:
        print("System default: not set")
    else:
        current = next((entry for entry in endpoints if entry[0] == selected), None)
        label = current[1] if current is not None else selected
        print(f"System default: {color(label, 'current')}")
        print(color(f"  {selected}", "muted"))
    print("Selectable physical devices:")
    if not endpoints:
        print("  no PulseAudio-compatible hardware endpoints found")
        return
    for index, (name, description) in enumerate(endpoints, start=1):
        marker = color(" [DEFAULT]", "current") if name == selected else ""
        print(f"  [{index}] {description}{marker}\n      {name}")
    print(color(f"Select: ./scripts/sys_dev.sh --set {role} --index N", "muted"))


def print_camera_role() -> None:
    print(color("=== CAMERA ===", "heading"))
    print(color("RealSense has no system-wide default-device setting.", "muted"))
    print("Detected cameras:")
    routes = camera_routes()
    if not routes:
        print("  no cameras found")
        return
    for index, (serial, label) in enumerate(routes, start=1):
        print(f"  [{index}] {label}\n      {serial}")


def print_current() -> None:
    for role in ("mic", "speaker"):
        kind = audio_role_kind(role)
        selected = audio_default(role)
        print(f"{role} system default {kind}: {selected or 'not set'}")


def ensure_preferred_audio() -> tuple[str, str]:
    """Select each preferred endpoint when present; leave absent roles alone."""
    states = []
    for role, preferred in PREFERRED_AUDIO.items():
        if preferred not in {name for name, _ in audio_routes(role)}:
            states.append(f"{role}: waiting for {preferred}")
            continue
        if audio_default(role) != preferred:
            _pactl([f"set-default-{audio_role_kind(role)}", preferred])
        states.append(f"{role}: {preferred}")
    return tuple(states)


def watch_preferred_audio() -> None:
    previous = None
    while True:
        try:
            state = ensure_preferred_audio()
        except RuntimeError as error:
            state = (f"audio server unavailable: {error}",)
        if state != previous:
            print("sys_dev: " + "; ".join(state), file=sys.stderr, flush=True)
            previous = state
        time.sleep(2)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    operation = parser.add_mutually_exclusive_group(required=True)
    operation.add_argument("-l", "--list", action="store_true", help="show host defaults and selectable devices")
    operation.add_argument("--list-only", choices=("mic", "speaker", "camera"), metavar="ROLE")
    operation.add_argument("--current", action="store_true", help="show host audio defaults only")
    operation.add_argument("--ensure-preferred", action="store_true",
                           help="select the configured mic and speaker when present")
    operation.add_argument("--watch-preferred", action="store_true",
                           help="keep selecting the configured mic and speaker after hotplug")
    operation.add_argument("-s", "--set", choices=("mic", "speaker", "camera"), metavar="ROLE",
                           help="set a host audio default; requires --index")
    parser.add_argument("-i", "--index", type=int, metavar="INDEX", help="one-based index within --set ROLE")
    arguments = parser.parse_args()
    try:
        if arguments.watch_preferred:
            watch_preferred_audio()
            return 0
        if arguments.ensure_preferred:
            print("\n".join(ensure_preferred_audio()))
            return 0
        if arguments.current:
            print_current()
            return 0
        if arguments.list or arguments.list_only:
            roles = ("mic", "speaker", "camera") if arguments.list else (arguments.list_only,)
            for index, role in enumerate(roles):
                if index:
                    print()
                if role == "camera":
                    print_camera_role()
                else:
                    print_audio_role(role)
            return 0
        if arguments.set == "camera":
            raise RuntimeError("camera selection is unavailable: RealSense has no system-wide default")
        if arguments.index is None:
            raise RuntimeError("--set requires --index")
        endpoints = audio_routes(arguments.set)
        if arguments.index < 1 or arguments.index > len(endpoints):
            raise RuntimeError(f"device index must be in [1, {len(endpoints)}]")
        name, description = endpoints[arguments.index - 1]
        _pactl([f"set-default-{audio_role_kind(arguments.set)}", name])
        print(f"set system {arguments.set} default to {description}", file=sys.stderr)
        return 0
    except RuntimeError as error:
        print(f"sys_dev: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
