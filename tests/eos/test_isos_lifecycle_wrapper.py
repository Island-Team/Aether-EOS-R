#!/usr/bin/env python3
"""Check that the EOS launcher forwards deployment commands without changing the REPL."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile


WRAPPER = Path(__file__).resolve().parents[2] / "bin/eos"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="eos-wrapper-") as temporary:
        root = Path(temporary)
        bin_dir = root / "bin"
        bin_dir.mkdir()
        wrapper = bin_dir / "eos"
        wrapper.write_bytes(WRAPPER.read_bytes())
        wrapper.chmod(0o755)
        for name in ("isos", "eos-cli"):
            executable = bin_dir / name
            executable.write_text("#!/usr/bin/env bash\nprintf '%s\\n' \"$0\" \"$@\"\n")
            executable.chmod(0o755)
        environment = dict(os.environ, ISOS_BIN=str(bin_dir / "isos"))
        cases = {
            "start": ["isos", "run", "--deployment", "test"],
            "stop": ["isos", "deployment", "stop", "--deployment", "test"],
            "shutdown": ["isos", "deployment", "stop", "--deployment", "test"],
            "restart": ["isos", "restart", "--deployment", "test"],
            "status": ["isos", "status", "--deployment", "test"],
            "enter": ["eos-cli", "--deployment", "test"],
        }
        for command, expected in cases.items():
            result = subprocess.run(
                [str(wrapper), command, "--deployment", "test"],
                env=environment,
                check=True,
                capture_output=True,
                text=True,
            )
            actual = result.stdout.splitlines()
            assert Path(actual[0]).name == expected[0], (command, actual)
            assert actual[1:] == expected[1:], (command, actual)
        help_text = subprocess.run(
            [str(wrapper), "--help"], check=True, capture_output=True, text=True
        ).stdout
        assert "isos config apply" in help_text
        assert "isos v2" not in help_text
        unknown = subprocess.run([str(wrapper), "obsolete"], capture_output=True, text=True)
        assert unknown.returncode == 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
