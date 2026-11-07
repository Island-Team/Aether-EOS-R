"""Compact deterministic rendering of discovered robot hardware."""

from __future__ import annotations

from egia.core.context.hardware import render_hardware
from egia.core.link import DeviceBinding


def _device(device_id: str, device_class: str, part_id: str) -> DeviceBinding:
  return DeviceBinding(
    id=device_id,
    device_class=device_class,
    part_id=part_id,
    description=f"test {device_class}",
    state="idle",
    health="ready",
    primitives=(),
  )


def test_hardware_groups_repeated_part_kinds_and_sorts_ids_once() -> None:
  rendered = render_hardware(
    "robot-a",
    "warehouse/v2",
    [
      _device("camera-2", "camera", "vision"),
      _device("base-0", "base", "drive"),
      _device("camera-0", "camera", "vision"),
      _device("camera-1", "camera", "vision"),
      _device("depth-0", "camera", "depth"),
    ],
  )

  assert rendered == (
    "robot=robot-a; model=warehouse/v2; devices: "
    "base/drive=[base-0]; camera/depth=[depth-0]; "
    "camera/vision=[camera-0,camera-1,camera-2]"
  )
  for device_id in ("base-0", "depth-0", "camera-0", "camera-1", "camera-2"):
    assert rendered.count(device_id) == 1


def test_hardware_renderer_keeps_multiple_robots_separate() -> None:
  devices = [
    _device("camera-1", "camera", "vision"),
    _device("camera-0", "camera", "vision"),
  ]
  robot_a = render_hardware("robot-a", "profile-a", devices)
  robot_b = render_hardware("robot-b", "profile-b", reversed(devices))

  assert robot_a == (
    "robot=robot-a; model=profile-a; devices: camera/vision=[camera-0,camera-1]"
  )
  assert robot_b == (
    "robot=robot-b; model=profile-b; devices: camera/vision=[camera-0,camera-1]"
  )
