"""Optional EOS native skills are validated semantic prompt guidance."""

from __future__ import annotations

import grpc
import pytest

from egia.core.link import EosClient, LinkPrimitiveError
from egia.core.registry import PrimitiveRegistry
from egia.io.catalog import render_skills, skills_snapshot
from egia.lib.config import GatewayConfig
from tests.egia.stub.gateway import start_stub_gateway, stop_stub_gateway

_PRIMITIVE_VERSIONS = {
  "arm.movel": "1.0.0",
  "speaker.play": "1.0.0",
}


def _definition(description: str, primitives: list[str]) -> dict:
  return {
    "description": description,
    "parameters": {"type": "object"},
    "returns": {"type": "object"},
    "primitives": [
      (name, _PRIMITIVE_VERSIONS.get(name, "1.0.0")) for name in primitives
    ],
    "resources": [],
  }


def _client(stub) -> EosClient:
  return EosClient(
    GatewayConfig(host=stub.host, grpc_port=stub.port), backend="robot-a"
  )


@pytest.mark.asyncio
async def test_skill_references_are_found_in_sorted_order_and_render_semantically() -> (
  None
):
  stub = await start_stub_gateway()
  client = _client(stub)
  stub.gateway.skill_definitions = {
    ("scan", "2.0.0"): _definition("Survey the nearby scene", ["gimbal.motion"]),
    ("approach", "1.0.0"): _definition(
      "Approach a visible target", ["base.velocity", "base.posture"]
    ),
  }
  try:
    await client.initialize()
    assert client.skills_available is True
    first = client.skills[0]
    assert (first.backend, first.name, first.version) == (
      "robot-a",
      "approach",
      "1.0.0",
    )
    assert first.description == "Approach a visible target"
    assert first.primitives == ("base.velocity", "base.posture")
    assert first.primitive_refs == (
      ("base.velocity", "1.0.0"),
      ("base.posture", "1.0.0"),
    )

    rendered = render_skills({"robot-a": client})
    assert "robot-a:approach@1.0.0" in rendered
    assert "Approach a visible target" in rendered
    assert "primitives=[base.velocity, base.posture]" in rendered
    assert "ExecuteSkill" not in rendered
    snapshot = skills_snapshot({"robot-a": client})
    assert snapshot.available is True
    assert snapshot.count == 2
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_temporarily_unavailable_skill_service_never_keeps_stale_claims() -> None:
  stub = await start_stub_gateway()
  client = _client(stub)
  stub.gateway.skill_definitions = {
    ("scan", "1.0.0"): _definition("Scan the scene", ["gimbal.motion"])
  }
  observed: list[tuple[bool, tuple[str, ...]]] = []
  client.add_refresh_listener(
    lambda current: observed.append(
      (current.skills_available, tuple(item.name for item in current.skills))
    )
  )
  try:
    discovery = await client.initialize()
    assert discovery.robot_id == stub.gateway.robot_id
    assert client.skills_available is True
    assert [item.name for item in client.skills] == ["scan"]

    stub.gateway.errors["ListSkills"] = (
      grpc.StatusCode.UNAVAILABLE,
      "provider unavailable",
    )
    refreshed = await client.initialize(force=True)

    assert refreshed.robot_id == discovery.robot_id
    assert client.skills_available is False
    assert client.skills == ()
    assert observed[-1] == (False, ())
    snapshot = skills_snapshot({"robot-a": client})
    assert snapshot.available is False
    assert snapshot.skills == []
    assert snapshot.unavailable_backends == ["robot-a"]
    assert render_skills({"robot-a": client}) == "unavailable: robot-a"
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
@pytest.mark.parametrize(
  ("failure", "configured"),
  [
    ("rpc", (grpc.StatusCode.UNIMPLEMENTED, "service absent")),
    ("malformed", None),
  ],
)
async def test_missing_or_malformed_skill_service_is_incompatible(
  failure: str, configured
) -> None:
  stub = await start_stub_gateway()
  client = _client(stub)
  try:
    if failure == "rpc":
      stub.gateway.errors["ListSkills"] = configured
    else:
      stub.gateway.malformed.add("ListSkills")
    with pytest.raises(LinkPrimitiveError, match="service absent|SkillService catalog"):
      await client.initialize()
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_empty_skill_catalog_is_valid() -> None:
  stub = await start_stub_gateway()
  client = _client(stub)
  try:
    await client.initialize()
    assert client.skills_available is True
    assert client.skills == ()
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_skill_with_unknown_primitive_invalidates_entire_snapshot() -> None:
  stub = await start_stub_gateway()
  client = _client(stub)
  stub.gateway.skill_definitions = {
    ("unsafe", "1.0.0"): _definition("Invalid composition", ["arm.teleport"])
  }
  try:
    with pytest.raises(LinkPrimitiveError, match="SkillService catalog"):
      await client.initialize()
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)


@pytest.mark.asyncio
async def test_skill_prompt_hides_compositions_with_model_hidden_primitives() -> None:
  stub = await start_stub_gateway()
  client = _client(stub)
  stub.gateway.skill_definitions = {
    ("reach", "1.0.0"): _definition("Reach a Cartesian target", ["arm.movel"])
  }
  try:
    await client.initialize()
    registry = PrimitiveRegistry.from_primitives(
      "robot-a", client.primitives, expose_operator=True
    )

    rendered = render_skills({"robot-a": client}, registry)

    assert "reach" not in rendered
    assert rendered == "unavailable compositions: 1"
  finally:
    await client.aclose()
    await stop_stub_gateway(stub)
