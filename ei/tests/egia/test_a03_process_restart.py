"""A03 owner process restart proof with one stable-run EOS stub and grant."""

from __future__ import annotations

import asyncio
import hashlib
import json
import os
import re
import signal
import socket
import stat
import sys
from pathlib import Path

import pytest
from test_stages import _graph

from egia.lib.config import BackendEndpointConfig, GatewayConfig
from egia.lib.operation_ledger import OperationLedger
from egia.lib.safety_state import SafetyState, SafetyStateError, runtime_lock
from tests.egia.stub.gateway import start_stub_gateway, stop_stub_gateway

REVISION = "a" * 64
PROJECTION = "b" * 64
GRANT = "c" * 64
RUN_ID = "ra03_owner_restart"


def _unused_port() -> int:
  with socket.socket() as probe:
    probe.bind(("127.0.0.1", 0))
    return int(probe.getsockname()[1])


def _fixture_files(
  tmp_path: Path, stub, stub_launch_config
) -> tuple[Path, Path, Path, dict[str, str]]:
  gateway = GatewayConfig(host=stub.host, grpc_port=stub.port)
  config = stub_launch_config.model_copy(
    update={
      "mode": "isos",
      "gateway": gateway,
      "backends": [BackendEndpointConfig(id="robot-a", gateway=gateway)],
      "server": stub_launch_config.server.model_copy(update={"port": _unused_port()}),
      "launch": stub_launch_config.launch.model_copy(update={"memory": False}),
      "decision": stub_launch_config.decision.model_copy(
        update={
          "proposer": stub_launch_config.decision.proposer.model_copy(
            update={"enabled": False}
          )
        }
      ),
    }
  )
  config_path = tmp_path / "config.json"
  config_path.write_text(config.model_dump_json())
  graph = _graph()
  graph["revision"] = REVISION
  graph_path = tmp_path / "stages.json"
  graph_path.write_text(json.dumps(graph))
  grant_path = tmp_path / "egia.grant"
  grant_path.write_text(GRANT + "\n")
  grant_path.chmod(0o600)
  metadata = (("eos-active-revision", REVISION), ("eos-projection-sha256", PROJECTION))
  stub.gateway.generation_metadata = {
    method: metadata for method in ("Discover", "GetStatus", "ListCapabilities")
  }
  stub.gateway.expected_grant = GRANT
  env = os.environ.copy()
  env.update(
    {
      "EGIA_EOS_CONTROL_GRANT_FILE": str(grant_path),
      "ISOS_RUN_ID": RUN_ID,
      "A03_REVISION": REVISION,
      "PYTHONPATH": os.pathsep.join(
        (
          str(Path(__file__).resolve().parents[2]),
          str(Path(__file__).resolve().parents[2] / "src"),
        )
      ),
    }
  )
  return config_path, graph_path, grant_path, env


async def _worker(
  phase: str,
  *,
  config: Path,
  graph: Path,
  root: Path,
  etc_root: Path,
  operation_id: str,
  env: dict[str, str],
):
  """Start one interpreter with the same fixture inputs and fixed run binding."""
  return await asyncio.create_subprocess_exec(
    sys.executable,
    "-m",
    "tests.egia.a03_restart_worker",
    phase,
    "--config",
    str(config),
    "--graph",
    str(graph),
    "--state-root",
    str(root),
    "--etc",
    str(etc_root),
    "--operation-id",
    operation_id,
    cwd=Path(__file__).resolve().parents[2],
    env=env,
    stdout=asyncio.subprocess.PIPE,
    stderr=asyncio.subprocess.PIPE,
  )


async def _reap(process, *, terminate: bool = False) -> tuple[str, str]:
  if terminate and process.returncode is None:
    process.send_signal(signal.SIGTERM)
  try:
    stdout, stderr = await asyncio.wait_for(process.communicate(), timeout=20)
  except TimeoutError:
    process.kill()
    stdout, stderr = await process.communicate()
    raise AssertionError("A03 worker did not stop within 20 seconds")
  return stdout.decode(errors="replace"), stderr.decode(errors="replace")


async def _until(predicate, process, *, timeout: float = 30.0) -> None:
  deadline = asyncio.get_running_loop().time() + timeout
  while asyncio.get_running_loop().time() < deadline:
    if predicate():
      return
    if process.returncode is not None:
      raise AssertionError(f"A03 startup worker exited early: {process.returncode}")
    await asyncio.sleep(0.05)
  raise AssertionError("A03 startup checkpoint did not converge")


def _event_lines(stderr: str) -> list[dict]:
  return [json.loads(line) for line in stderr.splitlines() if line.startswith("{")]


def _record(label: str, stdout: str, stderr: str, stub) -> None:
  """Preserve raw child output and exact stub RPC identities when requested."""
  location = os.environ.get("A03_EVIDENCE_DIR")
  if location is None:
    return
  root = _evidence_root(Path(location))
  _write_private(root / f"{label}.stdout.log", stdout.encode("utf-8"))
  _write_private(root / f"{label}.stderr.log", stderr.encode("utf-8"))
  rpc_bytes = (
    json.dumps(
      {
        "run_id": RUN_ID,
        "revision": REVISION,
        "operation_requests": stub.gateway.operation_requests,
        "rpc_methods": stub.gateway.rpc_methods,
        "physical_calls": stub.gateway.calls,
      },
      indent=2,
    )
    + "\n"
  ).encode("utf-8")
  _write_private(root / f"{label}.rpc.json", rpc_bytes)


def _evidence_root(root: Path) -> Path:
  root.mkdir(mode=0o700, parents=True, exist_ok=True)
  metadata = root.lstat()
  if (
    not stat.S_ISDIR(metadata.st_mode)
    or metadata.st_uid != os.getuid()
    or stat.S_IMODE(metadata.st_mode) != 0o700
  ):
    raise AssertionError("A03 evidence root must be a same-UID private directory")
  return root


def _write_private(path: Path, payload: bytes) -> None:
  with os.fdopen(
    os.open(
      path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600
    ),
    "wb",
  ) as output_file:
    output_file.write(payload)
    output_file.flush()
    os.fchmod(output_file.fileno(), 0o600)


def _snapshot_state(
  label: str, state_root: Path, operation_id: str, *, safety: bool
) -> None:
  """Copy the closed owner-file inventory at a stable worker-exit boundary."""
  location = os.environ.get("A03_EVIDENCE_DIR")
  if location is None:
    return
  if label not in {
    "positive-before",
    "positive-after",
    "negative-before",
    "negative-after",
    "negative-refused",
  }:
    raise AssertionError("invalid A03 evidence label")
  if re.fullmatch(r"[A-Za-z0-9_-]{1,64}", operation_id) is None:
    raise AssertionError("invalid A03 evidence operation ID")
  inventory = [
    Path("operations-history.json"),
    Path("operations") / f"{operation_id}.index",
    Path("operations") / f"{operation_id}.json",
  ]
  if safety:
    inventory.extend(
      (Path("safety/safety-history.json"), Path("safety/safety-latch.json"))
    )
  evidence_root = _evidence_root(Path(location))
  snapshot = evidence_root / label
  snapshot.mkdir(mode=0o700)
  manifest: list[dict[str, str | int]] = []
  for relative in inventory:
    source = state_root / relative
    for directory in (state_root, source.parent):
      directory_metadata = directory.lstat()
      if (
        not stat.S_ISDIR(directory_metadata.st_mode)
        or directory_metadata.st_uid != os.getuid()
        or stat.S_IMODE(directory_metadata.st_mode) != 0o700
      ):
        raise AssertionError(
          f"A03 owner directory must be same-UID mode 0700: {directory}"
        )
    flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC
    with os.fdopen(os.open(source, flags), "rb") as input_file:
      metadata = os.fstat(input_file.fileno())
      if (
        not stat.S_ISREG(metadata.st_mode)
        or metadata.st_uid != os.getuid()
        or stat.S_IMODE(metadata.st_mode) != 0o600
      ):
        raise AssertionError(
          f"A03 owner file must be same-UID regular mode 0600: {relative}"
        )
      payload = input_file.read()
    target = snapshot / relative
    target.parent.mkdir(mode=0o700, exist_ok=True)
    _write_private(target, payload)
    manifest.append(
      {
        "path": relative.as_posix(),
        "sha256": hashlib.sha256(payload).hexdigest(),
        "bytes": len(payload),
        "mode": "0600",
      }
    )
  manifest_path = snapshot / "manifest.json"
  manifest_bytes = (
    json.dumps(
      {"label": label, "operation_id": operation_id, "files": manifest},
      sort_keys=True,
      indent=2,
    )
    + "\n"
  ).encode("utf-8")
  _write_private(manifest_path, manifest_bytes)


def _snapshot_fixture(root: Path, operation_id: str) -> None:
  for relative in (
    "operations-history.json",
    f"operations/{operation_id}.index",
    f"operations/{operation_id}.json",
    "safety/safety-history.json",
    "safety/safety-latch.json",
    "safety/safety-reset.json",
  ):
    path = root / relative
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    path.write_bytes(relative.encode("utf-8"))
    path.chmod(0o600)


def test_a03_snapshot_inventory_is_private_and_excludes_reset(
  tmp_path: Path, monkeypatch
) -> None:
  state = tmp_path / "state"
  state.mkdir(mode=0o700)
  _snapshot_fixture(state, "operation-1")
  evidence = tmp_path / "evidence"
  monkeypatch.delenv("A03_EVIDENCE_DIR", raising=False)
  _snapshot_state("negative-after", state, "operation-1", safety=True)
  assert not evidence.exists()
  monkeypatch.setenv("A03_EVIDENCE_DIR", str(evidence))
  _snapshot_state("negative-after", state, "operation-1", safety=True)
  snapshot = evidence / "negative-after"
  manifest = json.loads((snapshot / "manifest.json").read_text())
  assert manifest["label"] == "negative-after"
  assert manifest["operation_id"] == "operation-1"
  assert [item["path"] for item in manifest["files"]] == [
    "operations-history.json",
    "operations/operation-1.index",
    "operations/operation-1.json",
    "safety/safety-history.json",
    "safety/safety-latch.json",
  ]
  assert not (snapshot / "safety/safety-reset.json").exists()
  assert (
    stat.S_IMODE(evidence.stat().st_mode)
    == stat.S_IMODE(snapshot.stat().st_mode)
    == 0o700
  )
  for item in manifest["files"]:
    source = state / item["path"]
    copied = snapshot / item["path"]
    assert copied.read_bytes() == source.read_bytes()
    assert item["sha256"] == hashlib.sha256(copied.read_bytes()).hexdigest()
    assert item["bytes"] == len(copied.read_bytes())
    assert stat.S_IMODE(copied.stat().st_mode) == 0o600
  assert stat.S_IMODE((snapshot / "manifest.json").stat().st_mode) == 0o600


@pytest.mark.parametrize("damage", ["missing", "symlink", "mode", "directory"])
def test_a03_snapshot_rejects_unsafe_owner_file(
  tmp_path: Path, monkeypatch, damage: str
) -> None:
  state = tmp_path / "state"
  state.mkdir(mode=0o700)
  _snapshot_fixture(state, "operation-1")
  path = state / "operations/operation-1.json"
  if damage == "missing":
    path.unlink()
  elif damage == "symlink":
    path.unlink()
    path.symlink_to(state / "operations-history.json")
  elif damage == "mode":
    path.chmod(0o644)
  else:
    path.unlink()
    path.mkdir(mode=0o700)
  monkeypatch.setenv("A03_EVIDENCE_DIR", str(tmp_path / "evidence"))
  with pytest.raises((AssertionError, OSError)):
    _snapshot_state("negative-after", state, "operation-1", safety=True)


def _episode_if_complete(safety: SafetyState) -> tuple[str, str] | None:
  """Wait through the intentional history-before-latch atomic write window."""
  try:
    return safety.episode()
  except SafetyStateError:
    return None


async def _until_event(process, code: str) -> str:
  """Read exact owner events until the requested startup boundary is reached."""
  lines: list[str] = []

  async def read() -> None:
    while True:
      line = await process.stderr.readline()
      if not line:
        raise AssertionError(f"A03 worker closed stderr before {code}")
      decoded = line.decode(errors="replace")
      lines.append(decoded)
      if decoded.startswith("{") and json.loads(decoded).get("event_code") == code:
        return

  await asyncio.wait_for(read(), timeout=30)
  return "".join(lines)


@pytest.mark.asyncio
async def test_pending_receipt_settles_after_new_egia_process_same_stub_run(
  stub_launch_config, tmp_path: Path, etc_root: Path
) -> None:
  stub = await start_stub_gateway()
  config, graph, grant, env = _fixture_files(tmp_path, stub, stub_launch_config)
  root = tmp_path / "owner-state"
  operation_id = "a03-process-terminal"
  try:
    first = await _worker(
      "submit",
      config=config,
      graph=graph,
      root=root,
      etc_root=etc_root,
      operation_id=operation_id,
      env=env,
    )
    first_out, first_err = await _reap(first)
    _record("positive-worker-a", first_out, first_err, stub)
    assert first.returncode == 71, (first_out, first_err)
    assert OperationLedger(root).pending()[0].operation_id == operation_id
    assert (
      stat.S_IMODE((root / "operations" / f"{operation_id}.json").stat().st_mode)
      == 0o600
    )
    assert stub.gateway.operation_requests.count(("SubmitOperation", operation_id)) == 1
    assert len([call for call in stub.gateway.calls if call[0] == "base.velocity"]) == 1
    before_restart = len(stub.gateway.rpc_methods)
    _snapshot_state("positive-before", root, operation_id, safety=False)

    second = await _worker(
      "launch",
      config=config,
      graph=graph,
      root=root,
      etc_root=etc_root,
      operation_id=operation_id,
      env=env,
    )
    try:
      await _until(lambda: not OperationLedger(root).pending(), second)
      with (
        pytest.raises(SafetyStateError, match="runtime is active"),
        runtime_lock(root / "safety"),
      ):
        pass
      observed_stderr = await _until_event(second, "egia_ready")
    finally:
      second_out, second_err = await _reap(second, terminate=True)
    second_err = observed_stderr + second_err
    _record("positive-worker-b", second_out, second_err, stub)
    assert second.returncode == 0, (second_out, second_err)
    assert OperationLedger(root).pending() == []
    checkpoint = json.loads((root / "operations" / f"{operation_id}.json").read_text())
    assert checkpoint["state"] == "terminal" and checkpoint["outcome"] == "succeeded"
    assert checkpoint["graph_revision"] == REVISION
    assert checkpoint["operation_id"] == operation_id
    assert (
      checkpoint["alias"] == "robot-a"
      and checkpoint["eos_robot_id"] == stub.gateway.robot_id
    )
    assert stub.gateway.operation_requests.count(("SubmitOperation", operation_id)) == 1
    assert stub.gateway.operation_requests.count(("GetOperation", operation_id)) >= 1
    assert len([call for call in stub.gateway.calls if call[0] == "base.velocity"]) == 1
    restarted_rpcs = stub.gateway.rpc_methods[before_restart:]
    first_lookup = restarted_rpcs.index("GetOperation")
    assert all(
      method in restarted_rpcs[:first_lookup]
      for method in ("Discover", "GetStatus", "ListCapabilities")
    )
    assert "SubmitOperation" not in restarted_rpcs[:first_lookup]
    events = _event_lines(second_err)
    codes = [item["event_code"] for item in events]
    assert codes.index("egia_starting") < codes.index("egia_operation_terminal")
    terminal = next(
      item for item in events if item["event_code"] == "egia_operation_terminal"
    )
    assert terminal["correlation_id"] == operation_id
    assert terminal["run_id"] == RUN_ID
    assert codes.index("egia_operation_terminal") < codes.index("egia_ready")
    assert all(
      meta["authorization"] == f"Bearer {GRANT}"
      for method, meta in stub.gateway.received_metadata
      if method in {"SubmitOperation", "GetOperation"}
    )
    assert grant.stat().st_mode & 0o777 == 0o600
    _snapshot_state("positive-after", root, operation_id, safety=False)
  finally:
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_missing_receipt_stays_pending_and_refuses_reset_after_restart(
  stub_launch_config, tmp_path: Path, etc_root: Path
) -> None:
  stub = await start_stub_gateway()
  config, graph, _, env = _fixture_files(tmp_path, stub, stub_launch_config)
  root = tmp_path / "unresolved-state"
  operation_id = "a03-process-unknown"
  try:
    first = await _worker(
      "reserve_only",
      config=config,
      graph=graph,
      root=root,
      etc_root=etc_root,
      operation_id=operation_id,
      env=env,
    )
    first_out, first_err = await _reap(first)
    _record("negative-worker-a", first_out, first_err, stub)
    assert first.returncode == 72, (first_out, first_err)
    assert stub.gateway.operation_requests.count(("SubmitOperation", operation_id)) == 0
    before_restart = len(stub.gateway.rpc_methods)
    assert OperationLedger(root).pending()[0].operation_id == operation_id
    _snapshot_state("negative-before", root, operation_id, safety=False)

    second = await _worker(
      "launch",
      config=config,
      graph=graph,
      root=root,
      etc_root=etc_root,
      operation_id=operation_id,
      env=env,
    )
    safety = SafetyState(root / "safety")
    try:
      await _until(lambda: _episode_if_complete(safety) is not None, second)
      with (
        pytest.raises(SafetyStateError, match="runtime is active"),
        runtime_lock(safety.root),
      ):
        pass
      observed_stderr = await _until_event(second, "egia_ready")
    finally:
      second_out, second_err = await _reap(second, terminate=True)
    second_err = observed_stderr + second_err
    _record("negative-worker-b", second_out, second_err, stub)
    assert second.returncode == 0, (second_out, second_err)
    episode = safety.episode()
    assert episode is not None and episode[1] == REVISION
    assert operation_id in {
      item.operation_id for item in OperationLedger(root).pending()
    }
    codes = [item["event_code"] for item in _event_lines(second_err)]
    assert "egia_operation_unknown" in codes and "egia_recovery_required" in codes
    assert codes.index("egia_operation_unknown") < codes.index("egia_ready")
    assert safety.identities() == {"robot-a": stub.gateway.robot_id}
    assert stub.gateway.operation_requests.count(("GetOperation", operation_id)) >= 1
    assert stub.gateway.operation_requests.count(("SubmitOperation", operation_id)) == 0
    assert not any(call[0] == "base.velocity" for call in stub.gateway.calls)
    restarted_rpcs = stub.gateway.rpc_methods[before_restart:]
    first_lookup = restarted_rpcs.index("GetOperation")
    assert all(
      method in restarted_rpcs[:first_lookup]
      for method in ("Discover", "GetStatus", "ListCapabilities")
    )
    _snapshot_state("negative-after", root, operation_id, safety=True)
    with pytest.raises(SafetyStateError):
      safety.authorize_reset(frozenset({"robot-a"}), episode[0], "f" * 64)
    safety.authorize_reset(frozenset({"robot-a"}), *episode)
    assert stat.S_IMODE((safety.root / "safety-reset.json").stat().st_mode) == 0o600

    third = await _worker(
      "launch",
      config=config,
      graph=graph,
      root=root,
      etc_root=etc_root,
      operation_id=operation_id,
      env=env,
    )
    try:
      observed_stderr = await _until_event(third, "egia_safety_reset_refused")
      observed_stderr += await _until_event(third, "egia_ready")
    finally:
      third_out, third_err = await _reap(third, terminate=True)
    third_err = observed_stderr + third_err
    _record("negative-worker-c", third_out, third_err, stub)
    assert third.returncode == 0, (third_out, third_err)
    assert safety.episode() == episode
    assert operation_id in {
      item.operation_id for item in OperationLedger(root).pending()
    }
    events = _event_lines(third_err)
    refused = [
      item for item in events if item["event_code"] == "egia_safety_reset_refused"
    ]
    assert any(item["fields"]["reason_code"] == "lookup_unresolved" for item in refused)
    assert [item["event_code"] for item in events].index(
      "egia_safety_reset_refused"
    ) < [item["event_code"] for item in events].index("egia_ready")
    assert stub.gateway.operation_requests.count(("SubmitOperation", operation_id)) == 0
    assert not any(call[0] == "base.velocity" for call in stub.gateway.calls)
    for path in (
      safety.root / "safety-history.json",
      safety.root / "safety-latch.json",
    ):
      assert stat.S_IMODE(path.stat().st_mode) == 0o600
    _snapshot_state("negative-refused", root, operation_id, safety=True)
  finally:
    await stop_stub_gateway(stub)
