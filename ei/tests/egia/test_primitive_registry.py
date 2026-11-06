"""Primitive-only discovery catalog and explicit target resolution."""

from __future__ import annotations

from dataclasses import dataclass, field

import pytest

from egia.contracts.limits import NumericRange
from egia.core.registry import PrimitiveRegistry


@dataclass(frozen=True)
class Descriptor:
  name: str
  eos_robot_id: str
  device_id: str | None
  version: str = "1.0.0"
  description: str = "test primitive"
  device_description: str = ""
  limits: dict[str, NumericRange] = field(default_factory=dict)


def _descriptors(robot_id: str, suffix: str) -> list[Descriptor]:
  return [
    Descriptor("base.velocity", robot_id, f"base-{suffix}"),
    Descriptor("base.navigate", robot_id, f"base-{suffix}"),
    Descriptor("arm.init", robot_id, f"arm-{suffix}"),
    Descriptor("arm.movej", robot_id, f"arm-{suffix}"),
    Descriptor("unknown.extension", robot_id, f"custom-{suffix}"),
  ]


def test_catalog_keeps_only_known_model_tier_primitives_by_default() -> None:
  registry = PrimitiveRegistry.from_primitives("robot-a", _descriptors("eos-a", "a"))

  assert [item.name for item in registry.primitives()] == [
    "base.velocity",
    "base.navigate",
  ]
  assert registry.primitives()[0].backend == "robot-a"
  assert registry.resolve("arm.movej", "robot-a", "arm-a") is None
  assert registry.resolve("unknown.extension", "robot-a", "custom-a") is None


def test_operator_primitives_require_explicit_configuration_and_stay_marked() -> None:
  registry = PrimitiveRegistry.from_primitives(
    "robot-a", _descriptors("eos-a", "a"), expose_operator=True
  )

  arm = registry.resolve("arm.movej", "robot-a", "arm-a")
  arm_init = registry.resolve("arm.init", "robot-a", "arm-a")
  assert arm is not None
  assert arm_init is not None
  assert arm.operator is True
  assert arm_init.operator is True
  assert arm.argument_model is not None
  assert arm_init.json_schema["properties"]["type"]["const"] == "arm.init"
  assert "arm.init" not in registry.render_for_prompt()
  assert "arm.movej" not in registry.render_for_prompt()


def test_device_description_is_retained_and_rendered_as_target_guidance() -> None:
  descriptor = Descriptor(
    "base.velocity",
    "eos-a",
    "base-a",
    description="Move a mobile base",
    device_description="rear loading platform",
  )
  registry = PrimitiveRegistry.from_primitives("robot-a", [descriptor])

  primitive = registry.primitives()[0]
  assert primitive.description == "Move a mobile base"
  assert primitive.device_description == "rear loading platform"
  assert "Move a mobile base; device: rear loading platform" in (
    registry.render_for_prompt()
  )


def test_privileged_primitives_never_enter_the_planner_catalog() -> None:
  registry = PrimitiveRegistry.from_primitives(
    "robot-a",
    [
      Descriptor("motion.stop", "eos-a", "base-a"),
      Descriptor("safety.estop", "eos-a", None),
    ],
    expose_operator=True,
  )

  assert registry.primitives() == []
  assert registry.render_for_prompt() == ""


@pytest.mark.parametrize(
  "retired_name",
  [
    "arm.drop",
    "arm.stow",
    "arm.joint",
    "arm.position",
    "arm.joint_position",
    "arm.cartesian",
    "base.move",
    "base.turn",
    "gimbal.turn",
    "gripper",
    "safety.estop.release",
  ],
)
def test_retired_primitive_names_are_not_resolvable(retired_name: str) -> None:
  registry = PrimitiveRegistry.from_contracts(expose_operator=True)

  assert registry.resolve(retired_name) is None


def test_resolution_always_requires_explicit_robot_and_device() -> None:
  registry = PrimitiveRegistry()
  registry.extend(
    PrimitiveRegistry.from_primitives("robot-a", _descriptors("eos-a", "a"))
  )
  registry.extend(
    PrimitiveRegistry.from_primitives("robot-b", _descriptors("eos-b", "b"))
  )

  with pytest.raises(LookupError, match="explicit target required"):
    registry.resolve("base.velocity")
  with pytest.raises(LookupError, match="explicit target required"):
    registry.resolve("base.velocity", "robot-a")
  assert registry.resolve("base.velocity", "robot-b", "base-b") is not None
  assert registry.resolve("base.velocity", "robot-b", "base-a") is None


def test_prompt_catalog_names_every_exact_robot_and_device_target_once() -> None:
  registry = PrimitiveRegistry.from_primitives(
    "robot-a",
    [
      Descriptor("base.velocity", "eos-runtime-a", "base-0"),
      Descriptor("base.velocity", "eos-runtime-a", "base-1"),
    ],
  )

  prompt = registry.render_for_prompt()

  assert prompt.count("base.velocity@robot-a/base-0") == 1
  assert prompt.count("base.velocity@robot-a/base-1") == 1
  assert "eos-runtime-a" not in prompt
  assert '"properties"' not in prompt


def test_prompt_catalog_renders_each_targets_discovered_numeric_limits() -> None:
  registry = PrimitiveRegistry.from_primitives(
    "robot-a",
    [
      Descriptor(
        "base.velocity",
        "eos-runtime-a",
        "base-0",
        limits={
          "linear_x_m_s": NumericRange(minimum=-0.5, maximum=0.75),
          "linear_y_m_s": NumericRange(minimum=0, maximum=0),
          "angular_z_rad_s": NumericRange(
            minimum=-1, maximum=1, maximum_exclusive=True
          ),
          "valid_for_ms": NumericRange(minimum=1, maximum=5000),
        },
      )
    ],
  )

  prompt = registry.render_for_prompt()

  assert "linear_x_m_s=<number[-0.5..0.75]>" in prompt
  assert "linear_y_m_s=<number[0..0]>" in prompt
  assert "angular_z_rad_s=<number[-1..1)>" in prompt
  assert "valid_for_ms=<integer[1..5000]>" in prompt


def test_default_contract_fixture_exposes_relative_navigation_to_planners() -> None:
  registry = PrimitiveRegistry.from_contracts()

  navigation = registry.resolve("base.navigate", "default", "base-0")
  assert navigation is not None
  assert navigation.operator is False
  assert set(navigation.limits) == {"x_m", "y_m", "yaw_rad"}
  assert (
    "base.navigate@default/base-0 "
    "reference=<relative|absolute> "
    "[frame_id=<string>] "
    "x_m=<number[-1000000..1000000]> "
    "y_m=<number[-1000000..1000000]> "
    "yaw_rad=<number[-1000000..1000000]>" in registry.render_for_prompt()
  )


def test_atomic_backend_replacement_drops_old_targets_only_for_that_backend() -> None:
  registry = PrimitiveRegistry()
  registry.extend(
    PrimitiveRegistry.from_primitives("robot-a", _descriptors("eos-a", "old"))
  )
  registry.extend(
    PrimitiveRegistry.from_primitives("robot-b", _descriptors("eos-b", "b"))
  )

  registry.replace_primitives(
    "robot-a", _descriptors("eos-a2", "new"), expose_operator=False
  )

  assert registry.resolve("base.velocity", "robot-a", "base-old") is None
  assert registry.resolve("base.velocity", "robot-a", "base-new") is not None
  assert registry.resolve("base.velocity", "robot-b", "base-b") is not None
