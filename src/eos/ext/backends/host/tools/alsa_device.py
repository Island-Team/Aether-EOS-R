#!/usr/bin/env python3
"""List and atomically select a deployment binding's ALSA audio route."""

from __future__ import annotations

import argparse
import json
import os
import re
import stat
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[6]
ETC = ROOT / "etc/eos"
ROLE_CONFIG = {
    "mic": ("mic", "alsa", "arecord"),
    "speaker": ("speaker", "aplay", "/usr/bin/aplay"),
}
HARDWARE_LINE = re.compile(
    r"^card \d+: (?P<card>[^ ]+) \[(?P<name>[^]]+)\], device (?P<device>\d+): "
    r"(?P<device_name>[^[]+)\[(?P<subdevice>[^]]*)\]$"
)


def selected_binding(etc: Path) -> tuple[Path, str]:
    selected = tomllib.loads((etc / "eos.toml").read_text(encoding="utf-8"))["mode"]["binding"]
    if len(selected) != 1:
        raise RuntimeError("eos.toml must select exactly one binding")
    binding = selected[0]
    profile = binding["profile"]
    config = (etc / binding["backend"]).resolve()
    if not config.is_relative_to(etc.resolve()):
        raise RuntimeError("binding path must stay under the EOS config root")
    return config, profile


def aliases(role: str) -> list[str]:
    _, _, program = ROLE_CONFIG[role]
    try:
        completed = subprocess.run(
            [program, "-L"], check=True, capture_output=True, text=True, env={**os.environ, "LC_ALL": "C"}
        )
    except FileNotFoundError as error:
        raise RuntimeError(f"{program} is not installed") from error
    except subprocess.CalledProcessError as error:
        detail = error.stderr.strip() or f"exit {error.returncode}"
        raise RuntimeError(f"{program} -L failed: {detail}") from error

    result: list[str] = []
    for line in completed.stdout.splitlines():
        if not line or line[0].isspace():
            continue
        alias = line.strip()
        if alias != "null" and alias not in result:
            result.append(alias)
    return result


def routes(role: str) -> list[tuple[str, str]]:
    _, _, program = ROLE_CONFIG[role]
    try:
        completed = subprocess.run([program, "-l"], check=True, capture_output=True, text=True)
    except FileNotFoundError as error:
        raise RuntimeError(f"{program} is not installed") from error
    except subprocess.CalledProcessError as error:
        detail = error.stderr.strip() or f"exit {error.returncode}"
        raise RuntimeError(f"{program} -l failed: {detail}") from error

    listed = set(aliases(role))
    result: list[tuple[str, str]] = []
    for line in completed.stdout.splitlines():
        match = HARDWARE_LINE.match(line)
        if match is None or match["card"] == "APE":
            continue
        alias = f"plughw:CARD={match['card']},DEV={match['device']}"
        if alias not in listed:
            continue
        label = f"{match['name']} - {match['device_name'].strip()}"
        if (alias, label) not in result:
            result.append((alias, label))
    return result


def recommended(role: str, available: list[tuple[str, str]]) -> str | None:
    return available[0][0] if available else None


def alias_at_index(available: list[tuple[str, str]], index: int) -> str:
    if index < 1 or index > len(available):
        raise RuntimeError(f"route index must be in [1, {len(available)}]")
    return available[index - 1][0]


def print_routes(role: str, available: list[tuple[str, str]]) -> None:
    title = "microphone" if role == "mic" else "speaker"
    if not available:
        raise RuntimeError(f"no physical {title} routes found")
    print(f"Available {title} routes:")
    for index, (alias, label) in enumerate(available, start=1):
        print(f"  {index}. {label}\n     {alias}")
    choice = recommended(role, available)
    print(f"\nRecommended: {choice}")
    script = "mic" if role == "mic" else "speaker"
    print(f"Select with: ./scripts/{script}.sh --set {choice}")
    print(f"          or ./scripts/{script}.sh --set-index 1")


def _target_block(text: str, profile: str, role: str) -> tuple[int, int]:
    device_class, backend, _ = ROLE_CONFIG[role]
    header = f"[[profile.{profile}.device]]"
    starts = [offset for offset in range(len(text)) if text.startswith(header, offset)]
    for index, start in enumerate(starts):
        end = starts[index + 1] if index + 1 < len(starts) else len(text)
        block = text[start:end]
        if f'class = "{device_class}"' not in block:
            continue
        if f'type = "{backend}"' not in block:
            raise RuntimeError(f"{profile} {role} device does not use backend '{backend}'")
        if sum(f'class = "{device_class}"' in text[left:right] for left, right in zip(starts, starts[1:] + [len(text)])) != 1:
            raise RuntimeError(f"{profile} config must declare exactly one {role} device")
        return start, end
    raise RuntimeError(f"{profile} config has no {role} device")


def _table_range(text: str, start: int, end: int, profile: str, role: str) -> tuple[int, int]:
    table = f"[profile.{profile}.device.backend.{ROLE_CONFIG[role][1]}]"
    table_start = text.find(table, start, end)
    if table_start < 0:
        raise RuntimeError(f"{profile} {role} device has no {table} table")
    value_start = table_start + len(table)
    next_table = text.find("\n[", value_start, end)
    return value_start, end if next_table < 0 else next_table


def update_device(path: Path, profile: str, role: str, alias: str) -> None:
    if not alias or any(ord(character) < 32 or ord(character) == 127 for character in alias):
        raise RuntimeError("ALSA alias must be non-empty and contain no control characters")
    metadata = path.lstat()
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise RuntimeError(f"refusing non-regular config file: {path}")
    original = path.read_text(encoding="utf-8")

    start, end = _target_block(original, profile, role)
    value_start, value_end = _table_range(original, start, end, profile, role)
    encoded = f"alsa_device = {json.dumps(alias)}"
    section = original[value_start:value_end]
    lines = section.splitlines(keepends=True)
    matches = [index for index, line in enumerate(lines) if line.startswith("alsa_device =")]
    if len(matches) > 1:
        raise RuntimeError(f"{profile} {role} ALSA table has duplicate alsa_device keys")
    if matches:
        suffix = "\n" if lines[matches[0]].endswith("\n") else ""
        lines[matches[0]] = encoded + suffix
    else:
        lines.insert(0, "\n" + encoded + "\n")
    updated = original[:value_start] + "".join(lines) + original[value_end:]
    updated_start, updated_end = _target_block(updated, profile, role)
    updated_value_start, updated_value_end = _table_range(updated, updated_start, updated_end, profile, role)
    updated_lines = updated[updated_value_start:updated_value_end].splitlines()
    if updated_lines.count(encoded) != 1:
        raise RuntimeError("selector did not update the expected ALSA route")

    descriptor = os.open(path, os.O_RDONLY)
    try:
        if os.read(descriptor, len(original.encode("utf-8"))).decode("utf-8") != original:
            raise RuntimeError("binding config changed while selecting ALSA route")
        temporary = tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent, delete=False)
        try:
            temporary.write(updated)
            temporary.flush()
            os.fsync(temporary.fileno())
            os.chmod(temporary.name, stat.S_IMODE(metadata.st_mode))
        finally:
            temporary.close()
        os.replace(temporary.name, path)
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        os.close(descriptor)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("role", choices=ROLE_CONFIG)
    parser.add_argument("--etc", type=Path, default=ETC, help="EOS config root (default: etc/eos)")
    operation = parser.add_mutually_exclusive_group(required=True)
    operation.add_argument("--list", action="store_true")
    operation.add_argument("--set", metavar="ALIAS")
    operation.add_argument("--set-index", metavar="INDEX", type=int)
    arguments = parser.parse_args()
    try:
        available = routes(arguments.role)
        if arguments.list:
            print_routes(arguments.role, available)
            return 0
        alias = arguments.set
        if arguments.set_index is not None:
            alias = alias_at_index(available, arguments.set_index)
        if alias not in [candidate for candidate, _ in available]:
            raise RuntimeError(f"unknown {arguments.role} ALSA alias: {alias}")
        config, profile = selected_binding(arguments.etc)
        update_device(config, profile, arguments.role, alias)
        print(f"set {profile} {arguments.role} ALSA route to {alias}; restart eosd", file=sys.stderr)
        return 0
    except (RuntimeError, OSError, KeyError, tomllib.TOMLDecodeError) as error:
        print(f"alsa selector: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
