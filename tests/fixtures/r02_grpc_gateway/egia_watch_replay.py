#!/usr/bin/env python3
"""Replay R02-12 watch recovery through the selected EGIA typed client.

This is client-layer evidence against a controlled gRPC producer. EOS producer
behavior is separately tested in crates/aether-runtime/tests/grpc_resources.rs.
Run with Island-egia's dependency environment from the EOS repository root.
"""

from __future__ import annotations

import asyncio
import json
import os
from pathlib import Path
import sys

if not os.environ.get("EGIA_ROOT"):
  raise SystemExit("set EGIA_ROOT to the selected Island-egia checkout before this replay")
EGIA_ROOT = Path(os.environ["EGIA_ROOT"]).resolve(strict=True)
if not (EGIA_ROOT / "src/egia").is_dir():
  raise SystemExit(f"EGIA_ROOT does not contain src/egia: {EGIA_ROOT}")
sys.path.insert(0, str(EGIA_ROOT))
sys.path.insert(0, str(EGIA_ROOT / "src"))

import grpc  # noqa: E402
from grpc_health.v1 import health_pb2, health_pb2_grpc  # noqa: E402

from egia.contracts.endpoints import ResourceKind  # noqa: E402
from egia.core.link.client import EosClient  # noqa: E402
from egia.lib.config import GatewayConfig  # noqa: E402
from island.generated.eos import (  # noqa: E402
  capabilities_pb2_grpc, gateway_pb2_grpc, skills_pb2_grpc,
)
from tests.egia.stub.gateway import StubGatewayService, _Health  # noqa: E402


class FailingWatch(StubGatewayService):
  """Fail the first live watch after its initial ReadResource has completed."""

  def __init__(self, code: grpc.StatusCode) -> None:
    super().__init__()
    self.code = code
    self.release_failure = asyncio.Event()
    self.discover_count = 0
    self.watch_uris: list[str] = []
    self.closed_watch_uris: list[str] = []
    self.all_watches_closed = asyncio.Event()
    self.read_uris: list[str] = []
    self.old_uri = next(uri for uri in self._payloads if "/world/state" in uri)
    self.new_uri = self.old_uri.replace("/world/state", "/generation-2/world/state")

  async def Discover(self, request, context):
    self.discover_count += 1
    return await super().Discover(request, context)

  async def ReadResource(self, request, context):
    self.read_uris.append(request.uri)
    return await super().ReadResource(request, context)

  async def WatchResource(self, request, context):
    self.watch_uris.append(request.uri)
    self.all_watches_closed.clear()
    try:
      if len(self.watch_uris) == 1:
        await self.release_failure.wait()
        payload = dict(self._payloads.pop(self.old_uri))
        payload["sequence"] = 2
        self._payloads[self.new_uri] = payload
        await context.abort(self.code, "injected watch break")
      async for changed in super().WatchResource(request, context):
        yield changed
    finally:
      self.closed_watch_uris.append(request.uri)
      if len(self.closed_watch_uris) == len(self.watch_uris):
        self.all_watches_closed.set()


async def replay(code: grpc.StatusCode) -> dict[str, object]:
  service = FailingWatch(code)
  server = grpc.aio.server()
  gateway_pb2_grpc.add_GatewayServiceServicer_to_server(service, server)
  capabilities_pb2_grpc.add_CapabilityServiceServicer_to_server(service, server)
  skills_pb2_grpc.add_SkillServiceServicer_to_server(service, server)
  health_pb2_grpc.add_HealthServicer_to_server(
    _Health(health_pb2.HealthCheckResponse.SERVING), server,
  )
  port = server.add_insecure_port("127.0.0.1:0")
  await server.start()
  client = EosClient(GatewayConfig(host="127.0.0.1", grpc_port=port))
  stream = None
  replacement = None
  try:
    await client.initialize()
    assert service.discover_count == 1
    world = next(item for item in client.resources if item.kind is ResourceKind.WORLD)
    assert world.uri == service.old_uri
    stream = client.stream_resource(
      world.uri, kind=ResourceKind.WORLD, poll_interval_s=10.0,
    )
    first = await asyncio.wait_for(anext(stream), 3)
    assert first.sequence == 1
    assert service.read_uris == [service.old_uri]
    service.release_failure.set()

    if code in {grpc.StatusCode.NOT_FOUND, grpc.StatusCode.UNAVAILABLE}:
      recovered = await asyncio.wait_for(anext(stream), 3)
      assert recovered.sequence == 2
      assert service.discover_count == 2
      assert service.watch_uris == [service.old_uri, service.new_uri]
      assert service.read_uris == [service.old_uri, service.new_uri]
      assert next(item for item in client.resources if item.kind is ResourceKind.WORLD).uri == service.new_uri
      return {"code": code.name, "behavior": "auto-rediscover-and-read",
              "discoveries": service.discover_count, "reads": service.read_uris}

    try:
      await asyncio.wait_for(anext(stream), 3)
    except OSError as exc:
      assert "resource watch failed" in str(exc)
    else:
      raise AssertionError("RESOURCE_EXHAUSTED did not surface to caller")
    assert service.discover_count == 1, "client unexpectedly auto-rediscovers on exhaustion"
    assert service.read_uris == [service.old_uri]

    # The caller owns recovery from exhaustion: rediscover, select the new URI,
    # start a new watch, and accept its initial fresh ReadResource result.
    await client.initialize(force=True)
    world = next(item for item in client.resources if item.kind is ResourceKind.WORLD)
    assert world.uri == service.new_uri
    replacement = client.stream_resource(
      world.uri, kind=ResourceKind.WORLD, poll_interval_s=10.0,
    )
    fresh = await asyncio.wait_for(anext(replacement), 3)
    assert fresh.sequence == 2
    assert service.discover_count == 2
    assert service.watch_uris == [service.old_uri, service.new_uri]
    assert service.read_uris == [service.old_uri, service.new_uri]
    return {"code": code.name, "behavior": "caller-rediscover-restart-and-read",
            "discoveries": service.discover_count, "reads": service.read_uris}
  finally:
    if replacement is not None:
      await replacement.aclose()
    if stream is not None:
      await stream.aclose()
    await asyncio.wait_for(service.all_watches_closed.wait(), 3)
    assert service.closed_watch_uris == service.watch_uris
    await client.aclose()
    await server.stop(grace=0)


async def main() -> None:
  results = [await replay(code) for code in (
    grpc.StatusCode.NOT_FOUND,
    grpc.StatusCode.UNAVAILABLE,
    grpc.StatusCode.RESOURCE_EXHAUSTED,
  )]
  print(json.dumps({"case": "R02-12", "producer": "controlled-grpc-stub",
                    "selected_client": str(EGIA_ROOT), "results": results}, sort_keys=True))


if __name__ == "__main__":
  asyncio.run(main())
