#!/usr/bin/env python3
"""Check explicit EOS binding selection and the Rust build entry point."""

from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def invoke(*args: str, env=None):
    return subprocess.run([sys.executable, str(ROOT / "scripts/eos_build_config.py"),
                           "--root", str(ROOT), *args], text=True, capture_output=True, env=env)


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="eos-build-config-") as temporary:
        bundle = Path(temporary)
        (bundle / "eos.toml").write_text(
            "[[mode.binding]]\nbackend='hardware.toml'\nprofile='selected'\n")
        (bundle / "hardware.toml").write_text(
            "[profile.selected]\n[[profile.selected.device]]\n"
            "[profile.selected.device.backend]\ntype='ros2'\n"
            "[profile.unused]\n[[profile.unused.device]]\n"
            "[profile.unused.device.backend]\ntype='vendor'\n")
        result = invoke("--config", str(bundle / "eos.toml"))
        assert result.returncode == 0, result.stderr
        assert result.stdout.splitlines() == [str(bundle / "eos.toml"), "selected", "ros2"]
        result = subprocess.run([str(ROOT / "build.sh"), "--print-build-dir"],
                                text=True, capture_output=True)
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == str(ROOT / "target/release")
        result = subprocess.run([str(ROOT / "build.sh"), "--bundle", str(bundle)],
                                text=True, capture_output=True)
        assert result.returncode == 2 and "unknown option: --bundle" in result.stderr
        (bundle / "eos.toml").write_text(
            "[[mode.binding]]\nbackend='hardware.toml'\nprofile='unused'\n")
        result = invoke("--config", str(bundle / "eos.toml"))
        assert result.returncode == 0, result.stderr
        assert result.stdout.splitlines()[-1] == "vendor"


if __name__ == "__main__":
    main()
