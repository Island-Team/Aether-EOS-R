"""Strict named Mission templates and protected initial plans."""

import shutil
from pathlib import Path

import pytest

from egia.lib.config import ConfigError, load_launch_config, load_mission_templates


def test_home_living_template_is_strict_and_dependency_complete(etc_root: Path) -> None:
  catalog = load_mission_templates(etc_root)
  template = catalog.resolve("home_living_2026")

  assert template is not None
  assert template.version == 1
  assert len(template.steps) >= 40
  assert all(step.required for step in template.steps)
  known = {step.id for step in template.steps}
  assert all(set(step.after) <= known for step in template.steps)
  assert catalog.resolve("unknown") is None


@pytest.mark.parametrize(
  "body, match",
  [
    (
      """
[[templates]]
id = "duplicate"
version = 1
description = "one"
mission = "one"
steps = [{id = "start", outcome = "start"}]

[[templates]]
id = "duplicate"
version = 2
description = "two"
mission = "two"
steps = [{id = "finish", outcome = "finish"}]
""",
      "IDs must be unique",
    ),
    (
      """
[[templates]]
id = "broken"
version = 1
description = "broken"
mission = "broken"
steps = [{id = "start", outcome = "start", after = ["missing"]}]
""",
      "unknown dependencies",
    ),
    (
      """
[[templates]]
id = "cycle"
version = 1
description = "cycle"
mission = "cycle"
steps = [
  {id = "one", outcome = "one", after = ["two"]},
  {id = "two", outcome = "two", after = ["one"]},
]
""",
      "dependency cycle",
    ),
    (
      """
[[templates]]
id = "unknown_field"
version = 1
description = "unknown field"
mission = "unknown field"
private_notes = "not part of the public schema"
steps = [{id = "start", outcome = "start"}]
""",
      "Extra inputs are not permitted",
    ),
    (
      """
[[templates]]
id = "unprotected_terminal"
version = 1
description = "unprotected terminal"
mission = "unprotected terminal"
steps = [{id = "exit", outcome = "exit", required = false, terminal = true}]
""",
      "terminal template step.*must be required",
    ),
  ],
)
def test_invalid_template_catalog_fails_closed(
  tmp_path: Path, body: str, match: str
) -> None:
  (tmp_path / "missions.toml").write_text(body, encoding="utf-8")

  with pytest.raises(ConfigError, match=match):
    load_mission_templates(tmp_path)


def test_missing_template_catalog_means_no_optional_templates(tmp_path: Path) -> None:
  assert load_mission_templates(tmp_path).templates == []


def test_launch_loads_template_catalog_only_when_enabled(
  tmp_path: Path, etc_root: Path
) -> None:
  root = tmp_path / "etc"
  shutil.copytree(etc_root, root)
  disabled = load_launch_config(etc=str(root))
  assert disabled.missions.templates == []

  config_file = root / "egia.toml"
  config_file.write_text(
    config_file.read_text(encoding="utf-8").replace(
      "load_mission_templates = false", "load_mission_templates = true"
    ),
    encoding="utf-8",
  )
  enabled = load_launch_config(etc=str(root))
  assert enabled.missions.resolve("home_living_2026") is not None
