from __future__ import annotations

from egia.contracts.endpoints import WORLD_RESOURCE_KINDS, ResourceKind
from egia.io import (
  PART_PATHS,
  PRIMITIVE_INVOKE_PATH,
  PRIMITIVES_PATH,
  SKILLS_PATH,
)
from egia.io.northbound import ALLOWED_ORIGINS


def test_only_public_webui_browser_origin_is_allowed() -> None:
  assert ALLOWED_ORIGINS == {"https://www.os-island.com"}


def test_world_and_primitive_routes_follow_the_contracts_taxonomy() -> None:
  assert PRIMITIVE_INVOKE_PATH.endswith("/primitive/invoke")
  assert PRIMITIVES_PATH.endswith("/primitives")
  assert SKILLS_PATH.endswith("/skills")


def test_removed_runtime_routes_have_no_export() -> None:
  from egia import io

  assert not hasattr(io, "ACTION_INVOKE_PATH")
  assert not hasattr(io, "TOOLS_PATH")
  assert not hasattr(io, "PERCEPTION_RASTER_MAP_PATH")


def test_routes_cover_every_v3_world_resource() -> None:
  assert set(PART_PATHS) == set(WORLD_RESOURCE_KINDS)
  expected_suffix = {
    ResourceKind.WORLD: "/perception/world/state",
    ResourceKind.DETECTION: "/perception/world/detection",
    ResourceKind.ASR: "/perception/world/asr",
  }
  for kind in WORLD_RESOURCE_KINDS:
    assert PART_PATHS[kind].endswith(expected_suffix[kind])
