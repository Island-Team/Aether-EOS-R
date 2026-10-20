#!/usr/bin/env python3
"""Run one test against a fresh, isolated external EOS simulator."""

from __future__ import annotations

import argparse
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


READINESS_TIMEOUT_S = 10.0
SHUTDOWN_TIMEOUT_S = 5.0


class FixtureInterrupted(Exception):
  def __init__(self, signum: int):
    super().__init__(f"received signal {signum}")
    self.signum = signum


def interrupt(signum: int, _frame: object) -> None:
  raise FixtureInterrupted(signum)


def parse_args() -> argparse.Namespace:
  parser = argparse.ArgumentParser()
  parser.add_argument("--simulator", required=True, type=Path)
  parser.add_argument("--etc", required=True, type=Path)
  parser.add_argument("--contracts", required=True, type=Path)
  parser.add_argument("test", nargs=argparse.REMAINDER)
  args = parser.parse_args()
  if args.test[:1] == ["--"]:
    args.test = args.test[1:]
  if not args.test:
    parser.error("a test command is required after --")
  return args


def terminate(process: subprocess.Popen[bytes] | None) -> None:
  if process is None or process.poll() is not None:
    return
  try:
    os.killpg(process.pid, signal.SIGTERM)
    process.wait(timeout=SHUTDOWN_TIMEOUT_S)
  except ProcessLookupError:
    return
  except subprocess.TimeoutExpired:
    try:
      os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
      pass
    process.wait()


def socket_ready(path: Path) -> bool:
  client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
  client.settimeout(0.1)
  try:
    client.connect(str(path))
    return True
  except OSError:
    return False
  finally:
    client.close()


def reserve_port() -> int:
  with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
    listener.bind(("127.0.0.1", 0))
    return int(listener.getsockname()[1])


def port_ready(port: int) -> bool:
  try:
    with socket.create_connection(("127.0.0.1", port), timeout=0.1):
      return True
  except OSError:
    return False


def wait_until_ready(process: subprocess.Popen[bytes], socket_path: Path, port: int) -> None:
  deadline = time.monotonic() + READINESS_TIMEOUT_S
  while time.monotonic() < deadline:
    return_code = process.poll()
    if return_code is not None:
      raise RuntimeError(f"simulator exited before readiness with status {return_code}")
    if socket_ready(socket_path) and port_ready(port):
      return
    time.sleep(0.05)
  raise RuntimeError(
      f"simulator was not ready within {READINESS_TIMEOUT_S:.0f}s "
      f"(socket {socket_path}, port 127.0.0.1:{port})"
  )


def print_simulator_output(path: Path) -> None:
  try:
    output = path.read_text(encoding="utf-8", errors="replace")
  except OSError as error:
    print(f"could not read simulator output: {error}", file=sys.stderr)
    return
  if output:
    print("--- eos-simulator output ---", file=sys.stderr)
    print(output, file=sys.stderr, end="" if output.endswith("\n") else "\n")


def main() -> int:
  args = parse_args()
  signal.signal(signal.SIGINT, interrupt)
  signal.signal(signal.SIGTERM, interrupt)
  simulator: subprocess.Popen[bytes] | None = None
  test: subprocess.Popen[bytes] | None = None
  with tempfile.TemporaryDirectory(prefix="eos-simulator-test-") as temporary:
    root = Path(temporary)
    run_root = root / "run"
    socket_path = run_root / "simulator.sock"
    output_path = root / "simulator-output.log"
    config_path = root / "sim.toml"
    port = reserve_port()
    config_path.write_text(
        "[simulator]\n"
        "host = '127.0.0.1'\n"
        "port = 16004\n"
        "map_id = ''\n"
        "camera_source = 'scene'\n"
        "[simulator.camera]\n"
        "width_px = 1920\n"
        "height_px = 1080\n"
        "fps = 30\n",
        encoding="utf-8",
    )
    environment = os.environ.copy()
    environment.update(
        {
            "ISLAND_STATE": str(root / "state"),
            "ISLAND_CACHE": str(root / "cache"),
            "ISLAND_LOG": str(root / "log"),
            "ISLAND_RUN": str(run_root),
            "ISLAND_TMP": str(root / "tmp"),
            "EOS_SIMULATOR_SOCKET": str(socket_path),
        }
    )
    if environment.get("EOS_TEST_ALSA_NULL") == "1":
      alsa_config_path = root / "alsa.conf"
      alsa_config_path.write_text("pcm.!default {\n  type null\n}\n", encoding="utf-8")
      environment["ALSA_CONFIG_PATH"] = str(alsa_config_path)

    try:
      with output_path.open("wb") as output:
        simulator = subprocess.Popen(
            [
                str(args.simulator.resolve()),
                "--config",
                str(config_path),
                "--etc",
                str(args.etc.resolve()),
                "--contracts",
                str(args.contracts.resolve()),
                "--port",
                str(port),
            ],
            env=environment,
            stdout=output,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        wait_until_ready(simulator, socket_path, port)
        test = subprocess.Popen(args.test, env=environment, start_new_session=True)
        return_code = test.wait()
      if return_code != 0:
        print_simulator_output(output_path)
      return return_code
    except (OSError, RuntimeError) as error:
      print(f"simulator test fixture failed: {error}", file=sys.stderr)
      print_simulator_output(output_path)
      return 1
    except FixtureInterrupted as error:
      print(f"simulator test fixture interrupted: {error}", file=sys.stderr)
      return 128 + error.signum
    finally:
      terminate(test)
      terminate(simulator)


if __name__ == "__main__":
  raise SystemExit(main())
