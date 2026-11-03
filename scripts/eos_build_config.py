#!/usr/bin/env python3
"""Validate the selected EOS binding before a core build."""

from __future__ import annotations

import argparse
import importlib.machinery
import importlib.util
from pathlib import Path
import sys
import tomllib


def load_manager(root: Path):
    loader = importlib.machinery.SourceFileLoader("island_launch_for_build", str(root / "bin/island-launch"))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    if spec is None:
        raise ValueError("cannot load island-launch")
    manager = importlib.util.module_from_spec(spec)
    loader.exec_module(manager)
    return manager


def resolve(root: Path, bundle: str | None, config: str | None) -> tuple[Path, list[str], list[str]]:
    manager = load_manager(root)
    if bundle and config:
        raise ValueError("--bundle and --config cannot be combined")
    if config:
        eos_config = Path(config).expanduser().resolve()
    else:
        selected = manager.profile_path(bundle) if bundle else manager.read_state()["selected"]
        eos_config = manager.config_paths(selected)[1]
    with eos_config.open("rb") as stream:
        eos = tomllib.load(stream)
    bindings = eos.get("mode", {}).get("binding", [])
    if not isinstance(bindings, list) or not bindings:
        raise ValueError(f"{eos_config}: expected at least one [[mode.binding]]")
    profiles: list[str] = []
    types: set[str] = set()
    for binding in bindings:
        if not isinstance(binding, dict) or not isinstance(binding.get("backend"), str) or not isinstance(binding.get("profile"), str):
            raise ValueError(f"{eos_config}: each binding requires backend and profile")
        path = Path(binding["backend"])
        if not path.is_absolute():
            path = eos_config.parent / path
        path = path.resolve()
        with path.open("rb") as stream:
            backend = tomllib.load(stream)
        name = binding["profile"]
        profile = backend.get("profile", {}).get(name)
        if not isinstance(profile, dict):
            raise ValueError(f"{path}: profile {name} does not exist")
        devices = profile.get("device", [])
        if not isinstance(devices, list) or not devices:
            raise ValueError(f"{path}: profile {name} has no devices")
        for device in devices:
            kind = device.get("backend", {}).get("type") if isinstance(device, dict) else None
            if not isinstance(kind, str) or not kind:
                raise ValueError(f"{path}: profile {name} has a device without backend type")
            types.add(kind)
        profiles.append(name)
    return eos_config, profiles, sorted(types)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--bundle")
    parser.add_argument("--config")
    args = parser.parse_args()
    try:
        config, profiles, types = resolve(args.root.resolve(), args.bundle, args.config)
    except (OSError, ValueError, tomllib.TOMLDecodeError) as error:
        print(f"eos-build: {error}", file=sys.stderr)
        return 2
    print(config)
    print(",".join(profiles))
    print(",".join(types))
    return 0


if __name__ == "__main__":
    sys.exit(main())
