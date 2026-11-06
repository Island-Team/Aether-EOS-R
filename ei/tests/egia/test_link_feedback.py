"""A02 managed EOS discovery and durable operation feedback."""

from __future__ import annotations

import os
from pathlib import Path

import grpc
import pytest

from egia.contracts.actions import TerminalStatus
from egia.core.link import EosClient, LinkPrimitiveError
from egia.lib.config import GatewayConfig
from tests.egia.stub.gateway import start_stub_gateway, stop_stub_gateway

REVISION = "a" * 64
PROJECTION = "b" * 64
GRANT = "c" * 64
VELOCITY = {
  "linear_x_m_s": 0.1,
  "linear_y_m_s": 0.0,
  "angular_z_rad_s": 0.0,
  "valid_for_ms": 100,
}


def _managed(stub, monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> EosClient:
  grant_file = tmp_path / "egia.grant"
  grant_file.write_text(GRANT + "\n")
  grant_file.chmod(0o600)
  monkeypatch.setenv("EGIA_EOS_CONTROL_GRANT_FILE", str(grant_file))
  metadata = (("eos-active-revision", REVISION), ("eos-projection-sha256", PROJECTION))
  stub.gateway.generation_metadata = {
    method: metadata for method in ("Discover", "GetStatus", "ListCapabilities")
  }
  stub.gateway.expected_grant = GRANT
  return EosClient(
    GatewayConfig(host=stub.host, grpc_port=stub.port),
    backend="robot-a",
    expected_revision=REVISION,
  )


@pytest.mark.asyncio
async def test_managed_generation_and_same_id_recovery(
  monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
  stub = await start_stub_gateway()
  client = _managed(stub, monkeypatch, tmp_path)
  try:
    await client.initialize()
    assert client.active_revision == REVISION
    stub.gateway.pending_operations.add("base.velocity")
    stub.gateway.lost_watch_once = True
    watched = await client.call_primitive(
      "base.velocity", "base-0", VELOCITY, command_id="a02-lost-watch"
    )
    assert watched.status is TerminalStatus.SUCCEEDED
    assert len(stub.gateway.calls) == 1
    await client.initialize()
    stub.gateway.lost_submit_once = True
    ack = await client.call_primitive(
      "base.velocity", "base-0", VELOCITY, command_id="a02-lost-submit"
    )
    assert ack.status is TerminalStatus.SUCCEEDED
    assert len(stub.gateway.calls) == 2
    methods = [method for method, _ in stub.gateway.received_metadata]
    assert methods.count("SubmitOperation") == 2
    assert methods.count("GetOperation") >= 2
    assert all(
      metadata["x-eos-active-revision"] == REVISION
      for method, metadata in stub.gateway.received_metadata
      if method == "SubmitOperation"
    )
    assert all(
      "x-eos-active-revision" not in metadata
      for method, metadata in stub.gateway.received_metadata
      if method == "GetOperation"
    )
    assert client.active_revision is None
    await client.initialize()
    assert client.active_revision == REVISION
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_a03_restart_lookup_never_resubmits(
  monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
  stub = await start_stub_gateway()
  client = _managed(stub, monkeypatch, tmp_path)
  try:
    await client.call_primitive(
      "base.velocity", "base-0", VELOCITY, command_id="a03-stored-id"
    )
    submissions = len(stub.gateway.calls)
    receipt = await client.recover_operation("a03-stored-id", "base.velocity", "base-0")
    assert receipt.status is TerminalStatus.SUCCEEDED
    assert len(stub.gateway.calls) == submissions
    assert [name for name, _ in stub.gateway.received_metadata].count(
      "GetOperation"
    ) >= 1
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_mixed_generation_fences_writes(
  monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
  stub = await start_stub_gateway()
  client = _managed(stub, monkeypatch, tmp_path)
  try:
    await client.initialize()
    stub.gateway.generation_metadata["GetStatus"] = (
      ("eos-active-revision", "d" * 64),
      ("eos-projection-sha256", PROJECTION),
    )
    with pytest.raises(LinkPrimitiveError, match="generation changed"):
      await client.initialize(force=True)
    assert client.active_revision is None
    with pytest.raises(LinkPrimitiveError, match="generation changed"):
      await client.call_primitive("base.velocity", "base-0", VELOCITY)
    assert stub.gateway.calls == []
    stub.gateway.generation_metadata["GetStatus"] = stub.gateway.generation_metadata[
      "Discover"
    ]
    stub.gateway.generation_metadata["ListCapabilities"] = (
      ("eos-active-revision", REVISION),
      ("eos-active-revision", REVISION),
      ("eos-projection-sha256", PROJECTION),
    )
    with pytest.raises(LinkPrimitiveError, match="duplicate generation"):
      await client.initialize()
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_consistent_wrong_saved_graph_revision_never_publishes_ready(
  monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
  stub = await start_stub_gateway()
  client = _managed(stub, monkeypatch, tmp_path)
  wrong = (("eos-active-revision", "d" * 64), ("eos-projection-sha256", PROJECTION))
  try:
    stub.gateway.generation_metadata = {
      method: wrong for method in ("Discover", "GetStatus", "ListCapabilities")
    }
    with pytest.raises(LinkPrimitiveError, match="differs from selected IsOS graph"):
      await client.initialize()
    assert client.active_revision is None
    assert client.primitives == ()
    with pytest.raises(LinkPrimitiveError, match="differs from selected IsOS graph"):
      await client.call_primitive("robot.stop", "", {})
    assert stub.gateway.calls == []
    correct = (("eos-active-revision", REVISION), ("eos-projection-sha256", PROJECTION))
    stub.gateway.generation_metadata = {
      method: correct for method in ("Discover", "GetStatus", "ListCapabilities")
    }
    await client.initialize()
    assert client.active_revision == REVISION
    stub.gateway.generation_metadata = {
      method: wrong for method in ("Discover", "GetStatus", "ListCapabilities")
    }
    with pytest.raises(LinkPrimitiveError, match="differs from selected IsOS graph"):
      await client.initialize(force=True)
    assert client.active_revision is None
    with pytest.raises(LinkPrimitiveError, match="differs from selected IsOS graph"):
      await client.call_primitive("robot.stop", "", {})
    assert stub.gateway.calls == []
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_grant_file_and_metadata_are_required(
  monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
  stub = await start_stub_gateway()
  client = _managed(stub, monkeypatch, tmp_path)
  grant_path = Path(os.environ["EGIA_EOS_CONTROL_GRANT_FILE"])
  try:
    grant_path.chmod(0o644)
    with pytest.raises(LinkPrimitiveError, match="unsafe"):
      await client.initialize()
    grant_path.chmod(0o600)
    grant_path.write_text("x" * 64 + "\n")
    with pytest.raises(LinkPrimitiveError, match="malformed"):
      await client.initialize()
    grant_path.write_text(GRANT + "\n")
    grant_path.unlink()
    grant_path.symlink_to(tmp_path / "other")
    with pytest.raises(LinkPrimitiveError, match="unsafe"):
      await client.initialize()
    grant_path.unlink()
    grant_path.write_text(GRANT + "\n")
    grant_path.chmod(0o600)
    missing_selection = EosClient(
      GatewayConfig(host=stub.host, grpc_port=stub.port), backend="robot-a"
    )
    try:
      with pytest.raises(LinkPrimitiveError, match="selected IsOS graph revision"):
        await missing_selection.initialize()
    finally:
      await missing_selection.aclose()
    stub.gateway.generation_metadata.pop("ListCapabilities")
    with pytest.raises(LinkPrimitiveError, match="generation changed"):
      await client.initialize()
    assert stub.gateway.calls == []
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_stop_and_emergency_grant_metadata(
  monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
  stub = await start_stub_gateway()
  client = _managed(stub, monkeypatch, tmp_path)
  try:
    await client.initialize()
    stop = await client.call_primitive("robot.stop", "", {}, command_id="a02-stop")
    estop = await client.call_primitive("safety.estop", "", {}, command_id="a02-estop")
    assert stop.status is TerminalStatus.SUCCEEDED
    assert estop.status is TerminalStatus.SUCCEEDED
    submissions = [
      metadata
      for method, metadata in stub.gateway.received_metadata
      if method == "SubmitOperation"
    ]
    assert submissions[0]["x-eos-active-revision"] == REVISION
    assert "x-eos-active-revision" not in submissions[1]
    await client.cancel_primitive("a02-stop")
    cancellation = [
      metadata
      for method, metadata in stub.gateway.received_metadata
      if method == "CancelOperation"
    ]
    assert cancellation[-1]["authorization"] == f"Bearer {GRANT}"
    assert "x-eos-active-revision" not in cancellation[-1]
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_stale_revision_and_wrong_run_grant_deny_before_adapter(
  monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
  stub = await start_stub_gateway()
  client = _managed(stub, monkeypatch, tmp_path)
  try:
    await client.initialize()
    stub.gateway.expected_grant = "d" * 64  # another IsOS run's verifier role
    with pytest.raises(LinkPrimitiveError, match="missing EGIA grant") as wrong_run:
      await client.call_primitive("robot.stop", "", {}, command_id="a02-wrong-run")
    assert wrong_run.value.status == 401
    assert stub.gateway.calls == []
    assert client.active_revision is None
    stub.gateway.expected_grant = GRANT
    await client.initialize()
    stub.gateway.generation_metadata["Discover"] = (
      ("eos-active-revision", "d" * 64),
      ("eos-projection-sha256", PROJECTION),
    )
    # EOS has switched revisions while EGIA still holds the previous generation.
    with pytest.raises(LinkPrimitiveError, match="stale revision") as stale:
      await client.call_primitive("robot.stop", "", {}, command_id="a02-stale")
    assert stale.value.status == 412
    assert stub.gateway.calls == []
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_unauthenticated_lookup_is_never_resubmitted(
  monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
  stub = await start_stub_gateway()
  client = _managed(stub, monkeypatch, tmp_path)
  try:
    await client.initialize()
    stub.gateway.lost_submit_once = True
    stub.gateway.errors["GetOperation"] = (
      grpc.StatusCode.NOT_FOUND,
      "sealed operation",
    )
    with pytest.raises(LinkPrimitiveError):
      await client.call_primitive(
        "base.velocity", "base-0", VELOCITY, command_id="a02-sealed"
      )
    assert len(stub.gateway.calls) == 1
    assert client.active_revision is None
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)
