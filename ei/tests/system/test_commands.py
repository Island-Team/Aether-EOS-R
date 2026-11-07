"""Checkout command smoke tests from outside the repository directory."""

from __future__ import annotations

import os
import pty
import subprocess
from pathlib import Path

import pytest


def test_public_commands_resolve_the_checkout_from_their_location(
  tmp_path: Path,
) -> None:
  root = Path(__file__).resolve().parents[2]

  launch = subprocess.run(
    [str(root / "bin/egia"), "--help"],
    cwd=tmp_path,
    check=True,
    capture_output=True,
    text=True,
  )
  stop = subprocess.run(
    [str(root / "bin/island-stop"), "--help"],
    cwd=tmp_path,
    check=True,
    capture_output=True,
    text=True,
  )
  kill = subprocess.run(
    [str(root / "kill.sh"), "--help"],
    cwd=tmp_path,
    check=True,
    capture_output=True,
    text=True,
  )
  island = subprocess.run(
    [str(root / "bin/island"), "--help"],
    cwd=tmp_path,
    check=True,
    capture_output=True,
    text=True,
  )
  ctl = subprocess.run(
    [str(root / "bin/islandctl"), "paths"],
    cwd=tmp_path,
    check=True,
    capture_output=True,
    text=True,
  )
  validated = subprocess.run(
    [str(root / "bin/islandctl"), "validate"],
    cwd=tmp_path,
    check=False,
    capture_output=True,
    text=True,
  )

  assert "EGIA decision layer" in launch.stdout
  assert "--state-root" in launch.stdout
  assert "--cache-root" in launch.stdout
  assert "--log-root" in launch.stdout
  assert "--run-root" in launch.stdout
  assert "--tmp-root" in launch.stdout
  assert "Shut down EGIA" in stop.stdout
  assert "Stop processes launched from this checkout" in kill.stdout
  assert "--os" in kill.stdout and "--all" in kill.stdout
  assert "Start the selected IsOS deployment" in island.stdout
  assert f"prefix={root}" in ctl.stdout
  assert validated.returncode == 0
  assert f"config={root / 'etc/island.toml'}" in validated.stdout


def test_egia_launch_script_forwards_no_tui_from_an_arbitrary_cwd(
  tmp_path: Path,
) -> None:
  root = Path(__file__).resolve().parents[2]
  result = subprocess.run(
    [str(root / "egia-launch.sh"), "--no-tui", "--help"],
    cwd=tmp_path,
    check=True,
    capture_output=True,
    text=True,
  )

  assert "EGIA decision layer" in result.stdout
  assert "--no-tui" in result.stdout


@pytest.mark.parametrize("machine", ["aarch64", "arm64", "x86_64", "amd64"])
def test_egia_launch_accepts_supported_architecture_aliases(
  tmp_path: Path, machine: str
) -> None:
  root = Path(__file__).resolve().parents[2]
  commands = tmp_path / "commands"
  commands.mkdir()
  uname = commands / "uname"
  uname.write_text(f"#!/bin/sh\nprintf '%s\\n' '{machine}'\n")
  uname.chmod(0o755)
  environment = {**os.environ, "PATH": f"{commands}:{os.environ['PATH']}"}

  result = subprocess.run(
    [str(root / "egia-launch.sh"), "--no-tui", "--help"],
    cwd=tmp_path,
    env=environment,
    check=True,
    capture_output=True,
    text=True,
  )

  assert "EGIA decision layer" in result.stdout


def test_egia_launch_rejects_unsupported_architecture(tmp_path: Path) -> None:
  root = Path(__file__).resolve().parents[2]
  commands = tmp_path / "commands"
  commands.mkdir()
  uname = commands / "uname"
  uname.write_text("#!/bin/sh\nprintf '%s\\n' 'riscv64'\n")
  uname.chmod(0o755)
  environment = {**os.environ, "PATH": f"{commands}:{os.environ['PATH']}"}

  result = subprocess.run(
    [str(root / "egia-launch.sh"), "--help"],
    cwd=tmp_path,
    env=environment,
    check=False,
    capture_output=True,
    text=True,
  )

  assert result.returncode == 2
  assert "unsupported architecture 'riscv64'" in result.stderr


@pytest.mark.parametrize("machine", ["aarch64", "x86_64"])
def test_egia_build_uses_synchronized_build_environment(
  tmp_path: Path, machine: str
) -> None:
  root = Path(__file__).resolve().parents[2]
  commands = tmp_path / "commands"
  commands.mkdir()
  log = tmp_path / "uv.log"
  uname = commands / "uname"
  uname.write_text(f"#!/bin/sh\nprintf '%s\\n' '{machine}'\n")
  uname.chmod(0o755)
  uv = commands / "uv"
  uv.write_text(f"#!/bin/sh\nprintf '%s\\n' \"$*\" >> '{log}'\n")
  uv.chmod(0o755)
  environment = {**os.environ, "PATH": f"{commands}:{os.environ['PATH']}"}

  subprocess.run(
    [str(root / "egia-build.sh"), "--wheel"],
    cwd=tmp_path,
    env=environment,
    check=True,
  )

  assert log.read_text().splitlines() == [
    "sync --python 3.12 --all-extras --no-install-project",
    "sync --python 3.12 --all-extras --no-build-isolation",
    "build --no-build-isolation --wheel",
  ]


def test_isos_aether_launches_the_product_from_an_arbitrary_cwd(
  tmp_path: Path,
) -> None:
  root = Path(__file__).resolve().parents[2]

  result = subprocess.run(
    [str(root / "isOS-Aether.sh"), "--help"],
    cwd=tmp_path,
    check=True,
    capture_output=True,
    text=True,
  )

  assert "Start the selected IsOS deployment" in result.stdout
  assert "isos config init" in result.stdout


def test_island_wrapper_delegates_to_isos(tmp_path: Path) -> None:
  root = Path(__file__).resolve().parents[2]
  executable = tmp_path / "isos"
  executable.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\"\n", encoding="utf-8")
  executable.chmod(0o755)

  result = subprocess.run(
    [str(root / "bin/island"), "--deployment", "home"],
    cwd=tmp_path,
    env={**os.environ, "ISOS_BIN": str(executable)},
    check=True,
    capture_output=True,
    text=True,
  )

  assert result.stdout.splitlines() == ["start", "--deployment", "home"]


@pytest.mark.parametrize("legacy", ["--config", "--config=old.toml", "--no-tui"])
def test_island_wrapper_rejects_legacy_options(tmp_path: Path, legacy: str) -> None:
  root = Path(__file__).resolve().parents[2]
  result = subprocess.run(
    [str(root / "bin/island"), legacy],
    cwd=tmp_path,
    check=False,
    capture_output=True,
    text=True,
  )

  assert result.returncode == 2
  assert "old combined launcher" in result.stderr
  assert "isos config init" in result.stderr


def test_egia_without_optional_tui_runs_core_on_terminal(tmp_path: Path) -> None:
  root = Path(__file__).resolve().parents[2]
  prefix = tmp_path / "prefix"
  (prefix / "share/contracts").mkdir(parents=True)
  (prefix / "libexec").mkdir()
  fake_python = tmp_path / "python"
  called = tmp_path / "called"
  fake_python.write_text(
    "#!/bin/sh\n"
    'if [ "$1" = "-c" ]; then exit 1; fi\n'
    'printf "%s\\n" "$@" > "$EGIA_TEST_ARGS"\n',
    encoding="utf-8",
  )
  fake_python.chmod(0o755)
  master, slave = pty.openpty()
  try:
    result = subprocess.run(
      [str(root / "bin/egia"), "--mode", "live"],
      cwd=tmp_path,
      env={
        **os.environ,
        "ISLAND_PREFIX": str(prefix),
        "EGIA_PYTHON": str(fake_python),
        "EGIA_TEST_ARGS": str(called),
      },
      stdout=slave,
      stderr=subprocess.PIPE,
      text=True,
      check=True,
    )
  finally:
    os.close(master)
    os.close(slave)

  assert "optional operator console is unavailable" in result.stderr
  assert called.read_text(encoding="utf-8").splitlines() == [
    "-m",
    "egia.main",
    "--mode",
    "live",
  ]
