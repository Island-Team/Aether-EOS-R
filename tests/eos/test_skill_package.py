#!/usr/bin/env python3
"""Qualify the installed native skill SDK from an external CMake project."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        **kwargs,
    )


def main() -> int:
    argv = sys.argv[1:]
    if len(argv) != 4:
        raise SystemExit(
            "usage: test_skill_package.py CMAKE NM SOURCE_DIR BUILD_DIR"
        )
    cmake, nm, source_dir_arg, build_dir_arg = argv
    source_dir = Path(source_dir_arg)
    build_dir = Path(build_dir_arg)
    with tempfile.TemporaryDirectory(prefix="eos-skill-package-") as temporary:
        root = Path(temporary)
        stage = root / "stage"
        environment = dict(os.environ)
        environment["DESTDIR"] = str(stage)
        run([cmake, "--install", str(build_dir), "--prefix", "/usr/local"], env=environment)

        consumer_build = root / "consumer"
        prefix = stage / "usr/local"
        run(
            [
                cmake,
                "-S",
                str(source_dir / "tests/eos/skill_package"),
                "-B",
                str(consumer_build),
                f"-DCMAKE_PREFIX_PATH={prefix}",
            ]
        )
        run([cmake, "--build", str(consumer_build), "-j2"])
        module = consumer_build / "skills/standalone.skill.so"
        run([str(consumer_build / "standalone_loader"), str(module)])
        skill_install = root / "skill-install"
        run([cmake, "--install", str(consumer_build), "--component", "island_pkg_native_skill",
             "--prefix", str(skill_install)])
        installed_module = skill_install / "lib/island/skills/standalone.skill.so"
        if not installed_module.is_file() or installed_module.is_symlink():
            raise RuntimeError("native skill component did not install a regular .skill.so file")
        run([str(consumer_build / "standalone_loader"), str(installed_module)])
        symbols = run([nm, "-D", "--defined-only", str(module)]).stdout.splitlines()
        exported = [line.split()[-1] for line in symbols if line.split()]
        if exported != ["eos_skill_library_query_v2"]:
            raise RuntimeError(f"unexpected exported symbols: {exported}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
