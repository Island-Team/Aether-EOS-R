#!/usr/bin/env python3
"""Stage-install and run every public EOS wrapper from an unrelated cwd."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: test_installed_wrappers.py CMAKE BUILD_DIR HTTP_ENABLED")
    cmake, build_dir, http_enabled = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="eos-installed-smoke-") as temporary:
        root = Path(temporary)
        stage = root / "stage"
        work = root / "outside"
        work.mkdir()
        environment = dict(os.environ)
        environment["DESTDIR"] = str(stage)
        subprocess.run(
            [cmake, "--install", build_dir, "--prefix", "/usr/local"],
            env=environment,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        prefix = stage / "usr/local"
        environment.update(
            {
                "ISLAND_PREFIX": str(prefix),
                "ISLAND_STATE": str(root / "state"),
                "ISLAND_CACHE": str(root / "cache"),
                "ISLAND_LOG": str(root / "log"),
                "ISLAND_RUN": str(root / "run"),
                "ISLAND_TMP": str(root / "tmp"),
                "EOS_ETC": str(stage / "etc/island/eos"),
                "EOS_CONTRACTS": str(prefix / "share/island"),
            }
        )
        names = ["eosd", "eos-simulator"]
        if http_enabled.lower() in {"on", "true", "1"}:
            names.extend(["eos-north-http", "eos-repl"])
            if not (prefix / "share/island/ext/north_http/openapi.json").is_file():
                raise RuntimeError("installed OpenAPI document is missing")
        else:
            for absent in (prefix / "bin/eos-north-http",
                           prefix / "libexec/island/eos-north-http",
                           prefix / "share/island/ext/north_http/openapi.json"):
                if absent.exists():
                    raise RuntimeError(f"HTTP-disabled install contains {absent}")
        for name in names:
            wrapper = prefix / "bin" / name
            if not wrapper.is_file():
                raise RuntimeError(f"installed wrapper is missing: {wrapper}")
            result = subprocess.run(
                [str(wrapper), "--help"],
                cwd=work,
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=10,
            )
            if result.returncode != 0 or "usage" not in result.stdout.lower():
                raise RuntimeError(
                    f"{name} installed smoke failed ({result.returncode}):\n{result.stdout}"
                )
        if "eos-repl" in names:
            latest = root / "log/eos/latest"
            if not latest.is_symlink():
                raise RuntimeError(f"latest diagnostic run link is missing: {latest}")
            repl_log = latest.resolve() / "repl/events.jsonl"
            if not repl_log.is_file():
                raise RuntimeError(f"REPL did not honor the isolated log root: {repl_log}")
        if (work / "var").exists():
            raise RuntimeError("an installed wrapper wrote mutable state relative to the caller cwd")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
