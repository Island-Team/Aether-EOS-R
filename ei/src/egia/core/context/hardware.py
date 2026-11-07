"""Deterministic, compact hardware prompt rendering."""

from __future__ import annotations

from collections import defaultdict
from collections.abc import Iterable
from typing import Protocol


class DeviceLike(Protocol):
  @property
  def id(self) -> str: ...

  @property
  def device_class(self) -> str: ...

  @property
  def part_id(self) -> str: ...


def render_hardware(
  robot_id: str,
  robot_model: str,
  devices: Iterable[DeviceLike],
) -> str:
  """Group equivalent devices and list each sorted identifier exactly once."""
  grouped: dict[tuple[str, str], list[str]] = defaultdict(list)
  for device in devices:
    grouped[(device.device_class, device.part_id)].append(device.id)
  parts = [
    f"{device_class}/{part_id}=[{','.join(sorted(ids))}]"
    for (device_class, part_id), ids in sorted(grouped.items())
  ]
  return f"robot={robot_id}; model={robot_model}; devices: " + (
    "; ".join(parts) if parts else "none"
  )


__all__ = ["render_hardware"]
