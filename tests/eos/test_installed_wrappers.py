#!/usr/bin/env python3
"""Stage the optional C++ build and check the public EOS install surface."""

from __future__ import annotations

import os
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: test_installed_wrappers.py CMAKE BUILD_DIR HTTP_ENABLED")
    cmake, build_dir, http_enabled = sys.argv[1:]
    cache = (Path(build_dir) / "CMakeCache.txt").read_text()
    rust_enabled = "ISLAND_BUILD_EOS:BOOL=ON" in cache
    with tempfile.TemporaryDirectory(prefix="eos-installed-smoke-") as temporary:
        root = Path(temporary)
        stage = root / "stage"
        outside = root / "outside"
        outside.mkdir()
        environment = dict(os.environ, DESTDIR=str(stage))
        subprocess.run(
            [cmake, "--install", build_dir, "--prefix", "/usr/local"],
            env=environment, check=True, capture_output=True, text=True,
        )
        prefix = stage / "usr/local"
        share = prefix / "share/island/eos"
        for required in (share / "contracts/eos/gateway.proto",
                         share / "schemas/eos/contracts.manifest.json"):
            if not required.is_file():
                raise RuntimeError(f"installed resource is missing: {required}")
        for excluded in (share / "workflows", share / "skills", share / "packages",
                         share / "ext", share / "README.md"):
            if excluded.exists():
                raise RuntimeError(f"install contains checkout-only resource: {excluded}")
        names = ("eos", "eosd", "eos-config", "eos-north-http", "eos-cli", "eos-repl",
                 "aether-launch")
        for name in names:
            if not (prefix / "bin" / name).is_file():
                raise RuntimeError(f"installed wrapper is missing: {name}")
        environment.update(ISLAND_PREFIX=str(prefix), ISLAND_STATE=str(root / "state"),
                           ISLAND_CACHE=str(root / "cache"), ISLAND_LOG=str(root / "log"),
                           ISLAND_RUN=str(root / "run"), ISLAND_TMP=str(root / "tmp"))
        result = subprocess.run([str(prefix / "bin/eos"), "--help"], cwd=outside,
                                env=environment, capture_output=True, text=True, timeout=10)
        if result.returncode or "usage: eos" not in result.stdout:
            raise RuntimeError(f"installed eos help failed: {result.stdout}{result.stderr}")
        for name in ("eosd", "eos-config", "eos-north-http", "eos-cli", "eos-repl"):
            result = subprocess.run([str(prefix / "bin" / name), "--capabilities"],
                                    cwd=outside, env=environment, capture_output=True,
                                    text=True, timeout=10)
            if rust_enabled:
                if name in ("eos-cli", "eos-repl"):
                    if result.returncode != 1 or "usage: aether-tui" not in result.stderr:
                        raise RuntimeError(f"installed {name} failed: {result.stdout}{result.stderr}")
                else:
                    expected = {"eosd": "aether-runtime", "eos-config": "aether-profile",
                                "eos-north-http": "aether-http-bridge"}[name]
                    if result.returncode or json.loads(result.stdout).get("binary") != expected:
                        raise RuntimeError(f"installed {name} failed: {result.stdout}{result.stderr}")
            elif result.returncode != 2 or "aether-launch: executable not found" not in result.stderr:
                raise RuntimeError(f"C++-only install did not report absent Rust runtime: {name}: "
                                   f"{result.stdout}{result.stderr}")
        if http_enabled.lower() in {"on", "true", "1"}:
            if not (prefix / "share/island/ext/north-http/openapi/north-http/openapi.json").is_file():
                raise RuntimeError("installed legacy HTTP OpenAPI document is missing")
        if (outside / "var").exists():
            raise RuntimeError("installed wrapper wrote state relative to caller cwd")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
