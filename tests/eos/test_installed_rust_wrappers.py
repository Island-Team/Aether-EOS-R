#!/usr/bin/env python3
"""Smoke the default installed Rust EOS commands from outside the checkout."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: test_installed_rust_wrappers.py CMAKE BUILD_DIR SOURCE")
    cmake, build_dir, source_arg = sys.argv[1:]
    source = Path(source_arg)
    with tempfile.TemporaryDirectory(prefix="eos-rust-installed-") as temporary:
        root = Path(temporary)
        stage = root / "stage"
        outside = root / "outside"
        outside.mkdir()
        subprocess.run(
            [cmake, "--install", build_dir, "--prefix", "/usr/local"],
            env=dict(os.environ, DESTDIR=str(stage)),
            check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        prefix = stage / "usr/local"
        for name in ("aether-runtime", "aether-profile", "aether-http-bridge",
                     "aether-config-server", "aether-tui"):
            assert (prefix / "libexec/island" / name).is_file(), name
        for name, binary in (("eosd", "aether-runtime"),
                             ("eos-config", "aether-profile"),
                             ("eos-north-http", "aether-http-bridge")):
            reply = subprocess.run(
                [str(prefix / "bin" / name), "--capabilities"], cwd=outside,
                capture_output=True, text=True, timeout=10, check=True,
            )
            assert json.loads(reply.stdout)["binary"] == binary, name
        help_reply = subprocess.run(
            [str(prefix / "bin/eos"), "--help"], cwd=outside,
            capture_output=True, text=True, timeout=10, check=True,
        )
        assert "usage: eos" in help_reply.stdout
        for name in ("eos-cli", "eos-repl"):
            reply = subprocess.run(
                [str(prefix / "bin" / name), "--help"], cwd=outside,
                capture_output=True, text=True, timeout=10,
            )
            assert reply.returncode == 1 and "usage: aether-tui" in reply.stderr, name
        cache = (Path(build_dir) / "CMakeCache.txt").read_text().splitlines()
        etcdir = next(line.split("=", 1)[1] for line in cache
                      if line.startswith("ISLAND_INSTALL_ETCDIR:PATH="))
        graph_dir = stage / etcdir.lstrip("/") / "eos/graphs"
        for name in ("mock-cxx.toml", "mujoco-socket.toml"):
            installed = graph_dir / name
            assert installed.read_bytes() == (source / "etc/eos/backends" / name).read_bytes()
        assert (prefix / "share/island/eos/contracts/eos/gateway.proto").is_file()
        assert (prefix / "share/island/eos/schemas/eos/contracts.manifest.json").is_file()
    print("INSTALLED_RUST_WRAPPERS_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
