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
    if len(argv) == 4:
        # Manual invocation without the nlohmann location; fall back below.
        argv += ["-", "-"]
    if len(argv) != 6:
        raise SystemExit(
            "usage: test_skill_package.py CMAKE NM SOURCE_DIR BUILD_DIR "
            "[NLOHMANN_DIR NLOHMANN_SOURCE]"
        )
    cmake, nm, source_dir_arg, build_dir_arg, nlohmann_dir, nlohmann_source = argv
    source_dir = Path(source_dir_arg)
    build_dir = Path(build_dir_arg)
    with tempfile.TemporaryDirectory(prefix="eos-skill-package-") as temporary:
        root = Path(temporary)
        stage = root / "stage"
        environment = dict(os.environ)
        environment["DESTDIR"] = str(stage)
        run([cmake, "--install", str(build_dir), "--prefix", "/usr/local"], env=environment)

        # EOS took nlohmann_json either from a system package or from a
        # FetchContent clone, and CTest reports which. A system package is
        # already reachable, so the consumer is pointed straight at it (its
        # EOSSkill config does find_dependency(nlohmann_json 3.11)); a clone has
        # to be built and staged into the prefix first.
        consumer_nlohmann: list[str] = []
        if nlohmann_dir != "-":
            consumer_nlohmann = [f"-Dnlohmann_json_DIR={nlohmann_dir}"]
        else:
            json_source = (
                Path(nlohmann_source)
                if nlohmann_source != "-"
                else build_dir / "_deps/nlohmann_json-src"
            )
            if not json_source.is_dir():
                raise SystemExit(
                    "nlohmann_json source not supplied by CTest and "
                    f"{json_source} does not exist"
                )
            json_build = root / "nlohmann-build"
            run(
                [
                    cmake,
                    "-S",
                    str(json_source),
                    "-B",
                    str(json_build),
                    "-DJSON_BuildTests=OFF",
                    "-DJSON_Install=ON",
                    "-DCMAKE_INSTALL_PREFIX=/usr/local",
                ]
            )
            run([cmake, "--install", str(json_build)], env=environment)

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
                *consumer_nlohmann,
            ]
        )
        run([cmake, "--build", str(consumer_build), "-j2"])
        module = consumer_build / "skills/standalone.skill.so"
        run([str(consumer_build / "standalone_loader"), str(module)])
        symbols = run([nm, "-D", "--defined-only", str(module)]).stdout.splitlines()
        exported = [line.split()[-1] for line in symbols if line.split()]
        if exported != ["eos_skill_library_query_v2"]:
            raise RuntimeError(f"unexpected exported symbols: {exported}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
