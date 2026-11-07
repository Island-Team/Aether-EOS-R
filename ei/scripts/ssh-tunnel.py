#!/usr/bin/env python3
"""Maintain development SSH port forwards configured through the local environment."""

from __future__ import annotations

import argparse
import base64
import os
import re
import select
import signal
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO
from urllib.parse import unquote, urlsplit

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
RETRY_SECONDS = 5


@dataclass(frozen=True)
class Target:
  """One SSH destination and the ports forwarded through it."""

  name: str
  destination: str
  ssh_port: int
  forward_ports: tuple[int, ...]
  password_env: str


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
  """Parse development tunnel options."""
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument(
    "--backend",
    action="store_true",
    help="detach and maintain forwarding without opening a remote shell",
  )
  parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
  parser.add_argument("--askpass", action="store_true", help=argparse.SUPPRESS)
  parser.add_argument(
    "--proxy-connect", nargs=2, metavar=("HOST", "PORT"), help=argparse.SUPPRESS
  )
  return parser.parse_args(argv)


def _port(value: str, field: str) -> int:
  """Validate one TCP port from the development configuration."""
  if not value.isdecimal() or not 1 <= int(value) <= 65535:
    raise ValueError(f"invalid {field}: {value!r}")
  return int(value)


def configured_targets() -> list[Target]:
  """Parse named tunnel records and their password-variable references."""
  names = os.environ.get("SSH_TUNNELS", "").split()
  if not names:
    raise ValueError("SSH_TUNNELS must contain at least one tunnel name")

  targets: list[Target] = []
  local_ports: set[int] = set()
  for name in names:
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", name):
      raise ValueError(f"invalid tunnel name: {name!r}")
    key = name.upper()
    record_var = f"SSH_TUNNEL_{key}"
    password_env_var = f"{record_var}_PASSWORD_ENV"
    record = os.environ.get(record_var, "")
    password_env = os.environ.get(password_env_var, "")
    if not record:
      raise ValueError(f"{record_var} must be set")
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", password_env):
      raise ValueError(f"{password_env_var} must name an environment variable")
    if not os.environ.get(password_env):
      raise ValueError(f"password variable {password_env} must be set")
    fields = record.split()
    if len(fields) < 2:
      raise ValueError(f"target needs at least one forward port: {record!r}")
    try:
      destination, ssh_port_value = fields[0].rsplit(":", 1)
    except ValueError as exc:
      raise ValueError(f"target needs an SSH port: {fields[0]!r}") from exc
    if not re.fullmatch(r"[A-Za-z0-9_.-]+@[A-Za-z0-9_.-]+", destination):
      raise ValueError(f"invalid user@host target: {destination!r}")
    ssh_port = _port(ssh_port_value, "SSH port")
    forward_ports = tuple(
      dict.fromkeys(_port(value, "forward port") for value in fields[1:])
    )
    duplicates = local_ports.intersection(forward_ports)
    if duplicates:
      duplicate = min(duplicates)
      raise ValueError(
        f"local forward port {duplicate} is assigned to multiple targets"
      )
    local_ports.update(forward_ports)
    targets.append(Target(name, destination, ssh_port, forward_ports, password_env))
  return targets


def ssh_command(target: Target) -> list[str]:
  """Build the SSH command for the configured local-to-remote forwards."""
  command = [
    "ssh",
    "-p",
    str(target.ssh_port),
    "-o",
    "ExitOnForwardFailure=yes",
    "-o",
    "ConnectTimeout=5",
    "-o",
    "ServerAliveInterval=5",
    "-o",
    "ServerAliveCountMax=2",
    "-o",
    "StrictHostKeyChecking=accept-new",
  ]
  if os.environ.get("SSH_TUNNEL_PROXY", os.environ.get("http_proxy", "")):
    command.extend(
      [
        "-o",
        f"ProxyCommand={Path(__file__).resolve()} --proxy-connect %h %p",
      ]
    )
  for port in target.forward_ports:
    command.extend(["-L", f"127.0.0.1:{port}:127.0.0.1:{port}"])
  command.extend(["-N", target.destination])
  return command


def askpass_environment(target: Target) -> dict[str, str]:
  """Configure OpenSSH to obtain one target's password from this script."""
  environment = os.environ.copy()
  environment.update(
    {
      "DISPLAY": environment.get("DISPLAY", "dev"),
      "SSH_ASKPASS": str(Path(__file__).resolve()),
      "SSH_ASKPASS_REQUIRE": "force",
      "SSH_TUNNEL_ASKPASS": "1",
      "SSH_TUNNEL_PASSWORD": environment[target.password_env],
    }
  )
  return environment


def proxy_connect(host: str, port_value: str) -> int:
  """Relay standard I/O through the configured HTTP CONNECT proxy."""
  proxy_value = os.environ.get("SSH_TUNNEL_PROXY", os.environ.get("http_proxy", ""))
  proxy = urlsplit(proxy_value)
  if proxy.scheme != "http" or proxy.hostname is None:
    print("ssh-tunnel: proxy must be an http:// URL", file=sys.stderr)
    return 2
  port = _port(port_value, "proxy destination port")
  proxy_port = proxy.port or 80
  headers = [
    f"CONNECT {host}:{port} HTTP/1.1",
    f"Host: {host}:{port}",
    "Proxy-Connection: Keep-Alive",
  ]
  if proxy.username is not None:
    credentials = f"{unquote(proxy.username)}:{unquote(proxy.password or '')}"
    token = base64.b64encode(credentials.encode()).decode()
    headers.append(f"Proxy-Authorization: Basic {token}")
  connection = socket.create_connection((proxy.hostname, proxy_port), timeout=10)
  try:
    connection.sendall(("\r\n".join(headers) + "\r\n\r\n").encode())
    response = b""
    while b"\r\n\r\n" not in response:
      chunk = connection.recv(4096)
      if not chunk or len(response) + len(chunk) > 65536:
        raise ConnectionError("proxy returned an incomplete HTTP response")
      response += chunk
    header, remainder = response.split(b"\r\n\r\n", 1)
    status = header.split(b"\r\n", 1)[0].split()
    if len(status) < 2 or status[1] != b"200":
      raise ConnectionError(f"proxy CONNECT failed: {header.splitlines()[0].decode()}")
    connection.settimeout(None)
    if remainder:
      os.write(sys.stdout.fileno(), remainder)
    inputs: list[socket.socket | BinaryIO] = [connection, sys.stdin.buffer]
    while connection in inputs:
      for ready in select.select(inputs, [], [])[0]:
        if ready is connection:
          data = connection.recv(65536)
          if not data:
            return 0
          os.write(sys.stdout.fileno(), data)
        else:
          data = os.read(sys.stdin.fileno(), 65536)
          if data:
            connection.sendall(data)
          else:
            connection.shutdown(socket.SHUT_WR)
            inputs.remove(sys.stdin.buffer)
  except (ConnectionError, OSError) as exc:
    print(f"ssh-tunnel: {exc}", file=sys.stderr)
    return 1
  finally:
    connection.close()
  return 0


def run_forever(targets: list[Target]) -> None:
  """Maintain every configured target and retry each one independently."""

  def stop(_signum: int, _frame: object) -> None:
    raise KeyboardInterrupt

  previous_sigterm = signal.signal(signal.SIGTERM, stop)
  processes: dict[Target, tuple[subprocess.Popen[bytes] | None, float]] = {
    target: (None, 0) for target in targets
  }
  try:
    while True:
      now = time.monotonic()
      for target, (process, retry_at) in processes.items():
        if process is None and now >= retry_at:
          process = subprocess.Popen(
            ssh_command(target), env=askpass_environment(target)
          )
          processes[target] = (process, retry_at)
          print(f"ssh-tunnel: worker started for {target.destination}", flush=True)
        elif process is not None and (status := process.poll()) is not None:
          print(
            f"ssh-tunnel: ssh to {target.destination} exited with status {status}; "
            f"retrying in {RETRY_SECONDS}s",
            file=sys.stderr,
            flush=True,
          )
          processes[target] = (None, now + RETRY_SECONDS)
      time.sleep(1)
  except KeyboardInterrupt:
    for process, _ in processes.values():
      if process is not None and process.poll() is None:
        process.terminate()
  finally:
    signal.signal(signal.SIGTERM, previous_sigterm)


def start_backend() -> None:
  """Start a detached tunnel worker and write its output under ``var/``."""
  log_dir = REPOSITORY_ROOT / "var"
  log_dir.mkdir(parents=True, exist_ok=True)
  log_path = log_dir / "ssh-tunnel.log"
  with log_path.open("ab") as log:
    process = subprocess.Popen(
      [sys.executable, str(Path(__file__).resolve()), "--worker"],
      stdin=subprocess.DEVNULL,
      stdout=log,
      stderr=subprocess.STDOUT,
      start_new_session=True,
    )
  print(f"ssh-tunnel: backend started (pid {process.pid}, log {log_path})")


def main(argv: list[str] | None = None) -> int:
  """Run the password helper, foreground session, or detached worker."""
  if os.environ.get("SSH_TUNNEL_ASKPASS") == "1":
    print(os.environ.get("SSH_TUNNEL_PASSWORD", ""))
    return 0

  args = parse_args(argv)
  if args.askpass:
    print(os.environ.get("SSH_TUNNEL_PASSWORD", ""))
    return 0
  if args.proxy_connect:
    return proxy_connect(*args.proxy_connect)

  try:
    targets = configured_targets()
  except ValueError as exc:
    print(f"ssh-tunnel: {exc}", file=sys.stderr)
    return 2

  if args.backend:
    start_backend()
  else:
    run_forever(targets)
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
