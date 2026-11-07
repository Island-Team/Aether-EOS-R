"""Stub EOS replay for the managed graph stage evidence path."""

from __future__ import annotations

import asyncio
import socket

import pytest
from test_stages import _graph

from egia.core import launch
from egia.core.missions import MissionQueue, MissionStore
from egia.lib.config import BackendEndpointConfig, GatewayConfig
from egia.stages import StageRuntime, compose_stages
from tests.egia.stub.gateway import start_stub_gateway, stop_stub_gateway


def _unused_port() -> int:
  with socket.socket() as probe:
    probe.bind(("127.0.0.1", 0))
    return int(probe.getsockname()[1])


@pytest.mark.asyncio
async def test_stub_eos_evidence_reaches_context_and_decision_stage_owners(
  stub_launch_config, tmp_path, etc_root
) -> None:
  stub = await start_stub_gateway()
  gateway = GatewayConfig(host=stub.host, grpc_port=stub.port)
  config = stub_launch_config.model_copy(
    update={
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
  runtime = StageRuntime(compose_stages(_graph()), "run-stage-test")
  state_dir = tmp_path / "state"
  mission_store = MissionStore(state_dir / "missions")
  saved = await MissionQueue(2, store=mission_store).submit("inspect", source="test")
  assert saved is not None
  assert config.decision.mission_preload is False
  stop = asyncio.Event()
  stub_closed = False
  task = asyncio.create_task(
    launch(
      config,
      stage_runtime=runtime,
      managed=True,
      state_dir=state_dir,
      etc=str(etc_root),
      external_stop=stop,
    )
  )
  try:
    context = next(
      path
      for path, instance in runtime.composition.instances.items()
      if instance.type.group == "context"
    )
    decision = next(
      path
      for path, instance in runtime.composition.instances.items()
      if instance.type.group == "decision"
    )
    for _ in range(200):
      if (
        runtime.latest(context, "observations") is not None
        and runtime.latest(decision, "snapshot") is not None
      ):
        break
      if task.done():
        await task
      await asyncio.sleep(0.05)
    observations = runtime.latest(context, "observations")
    snapshot = runtime.latest(decision, "snapshot")
    assert observations is not None
    assert observations.payload.fields["items"]
    assert snapshot is not None
    assert snapshot.payload.fields["text"]
    await stop_stub_gateway(stub)
    stub_closed = True
    perception = next(
      path
      for path, instance in runtime.composition.instances.items()
      if instance.type.group == "perception"
    )
    await asyncio.sleep(0.1)
    assert runtime.latest(perception, "result") is None
  finally:
    stop.set()
    await asyncio.wait_for(task, timeout=15)
    if not stub_closed:
      await stop_stub_gateway(stub)
  assert not runtime.active
  assert not runtime.diagnostics
  assert mission_store.load(saved.id) is not None
