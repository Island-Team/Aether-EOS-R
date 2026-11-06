"""Source-checkout contract and prompt inspection scripts."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import threading
import tomllib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import ClassVar


def test_generate_eos_proto_is_deterministic() -> None:
  root = Path(__file__).resolve().parents[2]
  generated = root / "src/island/generated/eos"
  before = {
    path.relative_to(generated).as_posix(): path.read_bytes()
    for path in sorted(generated.rglob("*"))
    if path.is_file() and path.suffix in {".py", ".pyi"}
  }
  result = subprocess.run(
    [sys.executable, str(root / "scripts/generate_eos_proto.py")],
    cwd=root,
    check=True,
    capture_output=True,
    text=True,
  )

  assert all(
    "Import eos/robot/primitive.proto is unused." in line
    for line in result.stderr.splitlines()
  )
  assert before == {
    path.relative_to(generated).as_posix(): path.read_bytes()
    for path in sorted(generated.rglob("*"))
    if path.is_file() and path.suffix in {".py", ".pyi"}
  }


def test_packaged_decision_models_define_fast_and_reasoning_lanes() -> None:
  root = Path(__file__).resolve().parents[2]
  for path in (
    root / "etc/egia/models.toml",
    root / "share/defaults/egia/models.toml",
  ):
    with path.open("rb") as stream:
      document = tomllib.load(stream)
    assert document["models"] == {
      "gemma4-fast": {
        "endpoint": "http://127.0.0.1:18011/v1",
        "model": "Gemma4-26B",
        "api_key_env": "",
        "timeout_seconds": 120,
        "context_window_tokens": 262144,
        "max_tokens": 512,
        "reasoning_effort": "none",
        "reasoning_options": ["none"],
      },
      "gemma4-reasoning": {
        "endpoint": "http://127.0.0.1:18011/v1",
        "model": "Gemma4-26B",
        "api_key_env": "",
        "timeout_seconds": 120,
        "context_window_tokens": 262144,
        "max_tokens": 8192,
        "reasoning_effort": "high",
        "reasoning_options": ["high"],
      },
    }


def test_model_script_calls_selection_from_egia_toml(tmp_path: Path) -> None:
  class Handler(BaseHTTPRequestHandler):
    request: ClassVar[dict[str, object]] = {}

    def do_POST(self) -> None:
      assert self.path == "/v1/chat/completions"
      length = int(self.headers["Content-Length"])
      Handler.request = json.loads(self.rfile.read(length))
      body = json.dumps(
        {
          "model": "Gemma4-26B",
          "choices": [
            {
              "message": {"content": "EGIA model OK"},
              "finish_reason": "stop",
            }
          ],
          "usage": {
            "prompt_tokens": 5,
            "completion_tokens": 4,
            "total_tokens": 9,
          },
        }
      ).encode()
      self.send_response(200)
      self.send_header("Content-Type", "application/json")
      self.send_header("Content-Length", str(len(body)))
      self.end_headers()
      self.wfile.write(body)

    def log_message(self, _format: str, *_args: object) -> None:
      return

  server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
  thread = threading.Thread(target=server.serve_forever, daemon=True)
  thread.start()
  root = Path(__file__).resolve().parents[2]
  etc = tmp_path / "etc"
  etc.mkdir()
  (etc / "egia.toml").write_text('[model]\nname = "gemma4"\n')
  port = server.server_address[1]
  (etc / "models.toml").write_text(
    "[models.gemma4]\n"
    f'endpoint = "http://127.0.0.1:{port}/v1"\n'
    'model = "Gemma4-26B"\n'
    'api_key_env = ""\n'
  )
  shutil.copy(root / "etc/egia/prompts.toml", etc / "prompts.toml")
  shutil.copy(root / "etc/egia/missions.toml", etc / "missions.toml")
  uv = shutil.which("uv")
  assert uv is not None
  clean_path = f"/usr/bin:/bin:{Path(uv).parent}"
  try:
    result = subprocess.run(
      [
        str(root / "scripts/test_model.py"),
        "--etc",
        str(etc),
        "--max-tokens",
        "7",
        "--mission-template",
        "home_living_2026",
        "--json",
      ],
      cwd=tmp_path,
      check=True,
      capture_output=True,
      env={**os.environ, "PATH": clean_path},
      text=True,
      timeout=10,
    )
  finally:
    server.shutdown()
    server.server_close()
    thread.join(timeout=2)

  output = json.loads(result.stdout)
  assert output["binding"] == "gemma4"
  assert output["reply"] == "EGIA model OK"
  assert Handler.request["model"] == "Gemma4-26B"
  assert Handler.request["max_tokens"] == 7
  messages = Handler.request["messages"]
  assert isinstance(messages, list)
  assert [message["role"] for message in messages] == ["system", "user"]
  assert "Home Living Competition Reference:" not in messages[0]["content"]
  assert (
    'MISSION role=active state=running order="Complete the 2026 home-living'
    in (messages[1]["content"])
  )
  assert (
    "STEP role=future id=owner-1-announce-name state=pending" in messages[1]["content"]
  )
  assert (
    "STEP role=future id=owner-1-announce-behavior state=pending"
    in messages[1]["content"]
  )


def test_shared_root_resolver_uses_product_config_from_arbitrary_cwd(
  tmp_path: Path,
) -> None:
  root = Path(__file__).resolve().parents[2]
  result = subprocess.run(
    [
      sys.executable,
      str(root / "scripts/resolve_shared_roots.py"),
      "--prefix",
      str(root),
      "--config",
      str(root / "etc/island.toml"),
    ],
    cwd=tmp_path,
    check=True,
    capture_output=True,
    text=True,
  )

  assert f"[==[{root / 'share'}]==]" in result.stdout
  assert f"[==[{(root / '../share').resolve()}]==]" in result.stdout


def test_print_prompt_renders_all_lanes_offline_from_arbitrary_cwd(
  tmp_path: Path,
) -> None:
  root = Path(__file__).resolve().parents[2]
  result = subprocess.run(
    [
      sys.executable,
      str(root / "scripts/print_prompt.py"),
      "--etc",
      str(root / "etc/egia"),
      "--mode",
      "sandbox",
    ],
    cwd=tmp_path,
    check=True,
    capture_output=True,
    text=True,
  )

  for lane in (
    "planner",
    "reasoner",
    "summarizer",
    "proposer",
    "sentinel",
    "profile_writer",
  ):
    assert f"=== {lane}: static ===" in result.stdout
    assert f"=== {lane}: final dynamic preview ===" in result.stdout
  assert "Hardware: robot=mock-robot" in result.stdout
  assert "camera/vision=[camera-0,camera-1]" in result.stdout
  assert "base.velocity@mock-robot/base-0" in result.stdout
  assert "Skills:\nunavailable" in result.stdout
  assert "Mode: SANDBOX" in result.stdout
  assert "Available Mission templates:\n- none" in result.stdout
  assert "MISSION_TABLE empty" in result.stdout
  assert "context.read" not in result.stdout
  assert "{tools}" not in result.stdout
  assert "Capabilities:" not in result.stdout
  assert "Raster map" not in result.stdout


def test_print_prompt_json_matches_text_preview_from_arbitrary_cwd(
  tmp_path: Path,
) -> None:
  root = Path(__file__).resolve().parents[2]
  result = subprocess.run(
    [
      sys.executable,
      str(root / "scripts/print_prompt.py"),
      "--etc",
      str(root / "etc/egia"),
      "--json",
    ],
    cwd=tmp_path,
    check=True,
    capture_output=True,
    text=True,
  )

  snapshot = json.loads(result.stdout)
  assert snapshot["version"] == "4"
  assert set(snapshot["lanes"]) == {
    "planner",
    "reasoner",
    "summarizer",
    "proposer",
    "sentinel",
    "profile_writer",
  }
  planner = snapshot["lanes"]["planner"]
  assert planner["static"]["sections"]
  assert planner["static"]["message"].startswith("Role:")
  assert "Execution history: none" in planner["dynamic"]
  assert planner["dynamic"].endswith("Execution history: none")
  assert "camera/vision=[camera-0,camera-1]" in planner["dynamic"]
  assert "Skills:\nunavailable" in planner["dynamic"]
  assert "base.velocity@mock-robot/base-0" in planner["dynamic"]
  assert all(section["dynamic"] is True for section in snapshot["context_sections"])
  expected_endings = {
    "planner": "Execution history: none",
    "summarizer": "Detailed execution events: none",
    "proposer": "MISSION_TABLE empty",
    "sentinel": "Newest safety-relevant state: unavailable",
    "profile_writer": "Previous profile validation: none",
  }
  for lane, ending in expected_endings.items():
    rendered = snapshot["lanes"][lane]
    assert rendered["static"]["message"].strip()
    assert rendered["dynamic"].endswith(ending)
    combined = rendered["static"]["message"] + "\n" + rendered["dynamic"]
    assert "context.read" not in combined
    assert "{tools}" not in combined
    assert "capability" not in combined.lower()
