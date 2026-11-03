#!/usr/bin/env python3
"""Check the default install resources without building the optional C++ graph."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_installed_owner_resources.py CMAKE SOURCE_DIR")
    cmake, source_arg = sys.argv[1:]
    source = Path(source_arg)
    with tempfile.TemporaryDirectory(prefix="eos-owner-resources-") as temporary:
        root = Path(temporary)
        build = root / "build"
        stage = root / "stage"
        subprocess.run(
            [cmake, "-S", str(source), "-B", str(build),
             "-DISLAND_BUILD_EOS=OFF", "-DISLAND_BUILD_EOS_CPP=OFF",
             "-DISLAND_BUILD_EOS_SKILL_SDK=OFF", "-DISLAND_BUILD_GECA=OFF",
             "-DISLAND_BUILD_TESTS=OFF"],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        environment = dict(os.environ, DESTDIR=str(stage))
        subprocess.run(
            [cmake, "--install", str(build), "--prefix", "/usr/local"],
            env=environment, check=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True,
        )
        prefix = stage / "usr/local"
        for relative in (
            "share/island/eos/contracts/eos/gateway.proto",
            "share/island/eos/schemas/eos/contracts.manifest.json",
            "share/doc/island/docs/eos/develop/hardware.md",
            "share/doc/island/docs_cn/eos/develop/hardware_cn.md",
        ):
            installed = prefix / relative
            original = source / relative.replace("share/island/eos/", "share/eos/")
            if relative.startswith("share/doc/island/docs_cn/") or relative.startswith("share/doc/island/docs/"):
                original = source / relative.removeprefix("share/doc/island/")
            if not installed.is_file() or installed.is_symlink():
                raise RuntimeError(f"installed owner resource missing: {installed}")
            if installed.read_bytes() != original.read_bytes():
                raise RuntimeError(f"installed owner resource differs: {installed}")
        english = prefix / "share/doc/island/docs/eos/develop/hardware.md"
        chinese = prefix / "share/doc/island/docs_cn/eos/develop/hardware_cn.md"
        if not chinese.is_file() or not english.is_file():
            raise RuntimeError("installed bilingual hardware guides are missing")
        if not (english.parent / "../../../docs_cn/eos/develop/hardware_cn.md").is_file():
            raise RuntimeError("installed English-to-Chinese guide link is broken")
        if not (chinese.parent / "../../../docs/eos/develop/hardware.md").is_file():
            raise RuntimeError("installed Chinese-to-English guide link is broken")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
