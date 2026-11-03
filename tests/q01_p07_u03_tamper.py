#!/usr/bin/env python3
"""Rehash altered Q01 P07/U03 archives and require semantic rejection."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile

from verify_q01_p07_u03 import verify


def replace(path: Path, body: bytes) -> None:
    temporary = path.with_name(path.name + ".changed")
    temporary.write_bytes(body)
    os.replace(temporary, path)


def save(path: Path, value: object) -> None:
    replace(path, (json.dumps(value, indent=2, sort_keys=True) + "\n").encode())


def read(root: Path, name: str):
    return json.loads((root / name).read_text())


def rehash(root: Path) -> None:
    manifest = {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in root.rglob("*") if p.is_file() and p.name != "evidence.sha256.json"}
    save(root / "evidence.sha256.json", manifest)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    args = parser.parse_args()
    source = args.archive.resolve()
    with tempfile.TemporaryDirectory(prefix="q01-p07-tamper-", dir=source.parent) as temp:
        for name in ("anchor", "cursor", "segment", "cursor-state", "ui-assertion"):
            root = Path(temp) / name
            shutil.copytree(source, root, copy_function=os.link)
            if name == "anchor":
                reply = read(root, "found.json")
                reply["body"]["data"]["anchor"]["event"]["event_code"] = "eos_ready"
                save(root / "found.json", reply)
            elif name == "cursor":
                reply = read(root, "tampered-reply.json")
                reply["body"]["data"]["diagnostics"][0]["code"] = "log_cursor_filter_mismatch"
                save(root / "tampered-reply.json", reply)
            elif name == "segment":
                snapshots = read(root, "rotation-snapshots.json")
                meta = snapshots[0]["events.log"]
                path = root / meta["file"]
                replace(path, path.read_bytes().replace(b"deployment_preflight_failed", b"deployment_preflight_changed"))
                meta["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
                meta["size"] = path.stat().st_size
                save(root / "rotation-snapshots.json", snapshots)
            elif name == "cursor-state":
                state = read(root, "cursor-state-pruned.raw.json")
                state["manager_anchor"]["segment"] = "0-0-0"
                save(root / "cursor-state-pruned.raw.json", state)
            else:
                overlay = root / "ui-q01_p07_workspace_stop.test.tsx"
                content = overlay.read_text()
                assert "expect(mocks.stop).toHaveBeenCalledTimes(1);" in content
                replace(overlay, content.replace("expect(mocks.stop).toHaveBeenCalledTimes(1);", "void mocks.stop;").encode())
                pins = read(root, "selected-pins.json")
                pins["ui_overlays_sha256"]["q01_p07_workspace_stop.test.tsx"] = hashlib.sha256(overlay.read_bytes()).hexdigest()
                save(root / "selected-pins.json", pins)
            rehash(root)
            try:
                verify(root)
            except (AssertionError, KeyError, ValueError) as error:
                print(name, "REJECTED", type(error).__name__, str(error)[:160])
            else:
                raise AssertionError(f"{name}: semantic verifier accepted altered archive")


if __name__ == "__main__":
    main()
