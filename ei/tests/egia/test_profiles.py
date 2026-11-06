"""Job profiles — skeleton validation, file store, quarantine, rendering."""

from __future__ import annotations

from pathlib import Path

import pytest
from pydantic import ValidationError

from egia.core.decision.profiles import (
  Profile,
  ProfileError,
  ProfileStep,
  ProfileStore,
  ready_steps,
  render_active,
  render_catalog,
  unknown_primitives,
)
from egia.core.registry import PrimitiveRegistry

_STEPS = [
  {
    "id": "fetch",
    "primitive": "gripper.motion",
    "tags": ["pick-place"],
    "outputs": ["breaker-1"],
  },
  {
    "id": "place",
    "primitive": "gripper.motion",
    "after": ["fetch"],
    "inputs": ["breaker-1"],
    "est_duration_s": 2.5,
  },
  {
    "id": "check",
    "primitive": "process.inspect",
    "after": ["place"],
    "zone": "cell-a",
  },
]


def _profile(**overrides) -> Profile:
  payload = {
    "name": "cabinet-a2",
    "order": "assemble breaker into cabinet A2",
    "intro": "Pick from tray, place at fixture, then verify.",
    "steps": _STEPS,
  }
  payload.update(overrides)
  return Profile.model_validate(payload)


# --------------------------------------------------------------- skeleton


def test_skeleton_validates_the_happy_path() -> None:
  profile = _profile()
  assert [step.id for step in profile.steps] == ["fetch", "place", "check"]
  assert profile.steps[1].est_duration_s == 2.5
  assert profile.steps[2].zone == "cell-a"
  assert profile.step("place") is not None and profile.step("nope") is None


def test_duplicate_step_ids_rejected() -> None:
  with pytest.raises(ValidationError, match="duplicate step ids"):
    _profile(
      steps=[
        {"id": "a", "primitive": "gripper.motion"},
        {"id": "a", "primitive": "gripper.motion"},
      ]
    )


def test_unknown_dependency_rejected() -> None:
  with pytest.raises(ValidationError, match="unknown steps"):
    _profile(steps=[{"id": "a", "primitive": "gripper.motion", "after": ["ghost"]}])
  with pytest.raises(ValidationError):
    Profile.model_validate(
      {
        "name": "Bad_Name",
        "order": "o",
        "steps": [{"id": "a", "primitive": "gripper.motion"}],
      }
    )  # name pattern


def test_dependency_cycle_rejected() -> None:
  with pytest.raises(ValidationError, match="dependency cycle"):
    _profile(
      steps=[
        {"id": "a", "primitive": "gripper.motion", "after": ["b"]},
        {"id": "b", "primitive": "gripper.motion", "after": ["a"]},
      ]
    )


def test_empty_steps_rejected() -> None:
  with pytest.raises(ValidationError):
    _profile(steps=[])


def test_extra_fields_rejected() -> None:
  with pytest.raises(ValidationError):
    ProfileStep.model_validate(
      {"id": "a", "primitive": "gripper.motion", "surprise": 1}
    )


def test_old_tool_field_is_rejected_without_migration() -> None:
  with pytest.raises(ValidationError):
    ProfileStep.model_validate({"id": "a", "tool": "gripper.motion"})


def test_ready_steps_follow_dependencies() -> None:
  profile = _profile()
  assert [step.id for step in ready_steps(profile, set())] == ["fetch"]
  assert [step.id for step in ready_steps(profile, {"fetch"})] == ["place"]
  assert [step.id for step in ready_steps(profile, {"fetch", "place"})] == ["check"]
  assert ready_steps(profile, {"fetch", "place", "check"}) == []


def test_parallel_steps_are_ready_together() -> None:
  profile = _profile(
    steps=[
      {"id": "root", "primitive": "gripper.motion"},
      {"id": "left", "primitive": "gripper.motion", "after": ["root"]},
      {"id": "right", "primitive": "gripper.motion", "after": ["root"]},
    ]
  )
  assert [step.id for step in ready_steps(profile, {"root"})] == ["left", "right"]


def test_unknown_primitives_against_registry() -> None:
  registry = PrimitiveRegistry.from_contracts(["b1"])
  profile = _profile()
  assert unknown_primitives(profile, registry) == ["process.inspect"]


def test_unknown_primitives_accepts_multi_backend_names() -> None:
  registry = PrimitiveRegistry.from_contracts(["b1", "b2"])
  profile = _profile(steps=[{"id": "a", "primitive": "gripper.motion"}])
  assert unknown_primitives(profile, registry) == []


# ------------------------------------------------------------------ store


def test_store_round_trip_preserves_skeleton_and_intro(tmp_path: Path) -> None:
  store = ProfileStore(tmp_path)
  profile = _profile(intro='note with "quotes"\nand a newline')
  path = store.save(profile)
  assert path.name == "cabinet-a2.v1.toml"
  loaded = store.load("cabinet-a2")
  assert loaded == profile


def test_store_returns_latest_version(tmp_path: Path) -> None:
  store = ProfileStore(tmp_path)
  store.save(_profile())
  store.save(_profile(version=2, order="assemble breaker into cabinet A2 (revised)"))
  latest = store.load("cabinet-a2")
  assert latest is not None and latest.version == 2
  explicit = store.load("cabinet-a2", version=1)
  assert explicit is not None and explicit.version == 1
  names = [profile.name for profile in store.load_all()]
  assert names == ["cabinet-a2"]


def test_store_quarantines_invalid_files_without_crashing(tmp_path: Path) -> None:
  store = ProfileStore(tmp_path)
  store.save(_profile())
  (tmp_path / "hand-edited.v1.toml").write_text(
    'name = "hand-edited"\nversion = 1\norder = "o"\n\n[[steps]]\n'
    'id = "a"\nprimitive = "gripper.motion"\nafter = ["missing"]\n',
    encoding="utf-8",
  )
  profiles = store.load_all()
  assert [profile.name for profile in profiles] == ["cabinet-a2"]
  quarantine = tmp_path / "quarantine"
  assert (quarantine / "hand-edited.v1.toml.invalid").is_file()
  assert not (tmp_path / "hand-edited.v1.toml").is_file()


def test_store_load_missing_returns_none(tmp_path: Path) -> None:
  store = ProfileStore(tmp_path)
  assert store.load("ghost") is None
  assert store.load_all() == []


def test_store_write_is_deterministic(tmp_path: Path) -> None:
  store = ProfileStore(tmp_path)
  path = store.save(_profile())
  first = path.read_text(encoding="utf-8")
  store.save(_profile())
  assert path.read_text(encoding="utf-8") == first


# -------------------------------------------------------------- rendering


def test_render_active_shows_skeleton_and_intro() -> None:
  block = render_active(_profile())
  assert "profile cabinet-a2 v1 (3 steps)" in block
  assert "intro: Pick from tray" in block
  assert "- fetch: gripper.motion tags [pick-place] ~1s" in block
  assert "- place: gripper.motion after [fetch] ~2.5s" in block
  assert "- check: process.inspect after [place] ~1s" in block


def test_render_catalog_is_one_line_per_profile() -> None:
  catalog = render_catalog([_profile(), _profile(name="cabinet-b1")])
  assert catalog.splitlines() == [
    "- cabinet-a2 v1: 3 steps, assemble breaker into cabinet A2",
    "- cabinet-b1 v1: 3 steps, assemble breaker into cabinet A2",
  ]


def test_profile_error_is_exported_for_the_writer_seam() -> None:
  assert issubclass(ProfileError, Exception)
