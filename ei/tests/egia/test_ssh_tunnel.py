"""Development SSH tunnel helper behavior."""

from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
import time
from pathlib import Path
from types import ModuleType


def _load_tunnel_module() -> ModuleType:
  """Load the hyphenated tunnel script as a Python module."""
  root = Path(__file__).resolve().parents[2]
  path = root / "scripts/ssh-tunnel.py"
  spec = importlib.util.spec_from_file_location("ssh_tunnel", path)
  assert spec is not None
  assert spec.loader is not None
  module = importlib.util.module_from_spec(spec)
  sys.modules[spec.name] = module
  spec.loader.exec_module(module)
  return module


def test_ssh_command_accepts_only_new_host_keys(monkeypatch: object) -> None:
  module = _load_tunnel_module()
  monkeypatch.delenv("http_proxy", raising=False)  # type: ignore[attr-defined]
  target = module.Target("lab", "developer@robot.example.com", 2222, (8000,), "PW")

  command = module.ssh_command(target)

  assert "StrictHostKeyChecking=accept-new" in command


def test_ssh_command_bounds_connection_and_stale_session_detection() -> None:
  module = _load_tunnel_module()
  target = module.Target("lab", "developer@robot.example.com", 2222, (8000,), "PW")

  command = module.ssh_command(target)

  assert "ConnectTimeout=5" in command
  assert "ServerAliveInterval=5" in command
  assert "ServerAliveCountMax=2" in command


def test_ssh_command_uses_proxy_without_expanding_credentials(
  monkeypatch: object,
) -> None:
  module = _load_tunnel_module()
  proxy = "http://user:secret@proxy.example.com:3128"
  monkeypatch.setenv("SSH_TUNNEL_PROXY", proxy)  # type: ignore[attr-defined]
  target = module.Target("lab", "developer@robot.example.com", 2222, (8000,), "PW")

  command = module.ssh_command(target)

  proxy_command = next(item for item in command if item.startswith("ProxyCommand="))
  assert "--proxy-connect %h %p" in proxy_command
  assert proxy not in proxy_command


def test_askpass_ignores_openssh_prompt_argument() -> None:
  root = Path(__file__).resolve().parents[2]
  result = subprocess.run(
    [
      sys.executable,
      str(root / "scripts/ssh-tunnel.py"),
      "developer@robot.example.com's password:",
    ],
    check=True,
    capture_output=True,
    env={
      **os.environ,
      "SSH_TUNNEL_ASKPASS": "1",
      "SSH_TUNNEL_PASSWORD": "test-password",
    },
    text=True,
  )

  assert result.stdout == "test-password\n"
  assert result.stderr == ""


def test_worker_terminates_ssh_child_on_sigterm(tmp_path: Path) -> None:
  root = Path(__file__).resolve().parents[2]
  fake_bin = tmp_path / "bin"
  fake_bin.mkdir(parents=True, exist_ok=True)
  marker = fake_bin / "terminated"
  fake_ssh = fake_bin / "ssh"
  fake_ssh.write_text(
    "#!/bin/sh\ntrap 'touch \"$MARKER\"; exit 0' TERM\nwhile :; do sleep 1; done\n",
    encoding="utf-8",
  )
  fake_ssh.chmod(0o755)
  environment = {
    **os.environ,
    "PATH": f"{fake_bin}:{os.environ['PATH']}",
    "MARKER": str(marker),
    "SSH_TUNNELS": "lab",
    "SSH_TUNNEL_LAB": "developer@robot.example.com:2222 8000",
    "SSH_TUNNEL_LAB_PASSWORD_ENV": "LAB_PASSWORD",
    "LAB_PASSWORD": "test-password",
  }
  worker = subprocess.Popen(
    [sys.executable, str(root / "scripts/ssh-tunnel.py")], env=environment
  )
  try:
    time.sleep(0.2)
    worker.terminate()
    worker.wait(timeout=5)
    for _ in range(20):
      if marker.exists():
        break
      time.sleep(0.05)
    assert marker.exists()
  finally:
    if worker.poll() is None:
      worker.kill()
