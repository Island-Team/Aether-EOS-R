"""Compatibility exports for vision models now owned by world perception."""

from egia.contracts.perception.world.vision import (
  VisionDetection as VisionObjectDetection,
)
from egia.contracts.perception.world.vision import (
  VisionSnapshot,
)

__all__ = ["VisionObjectDetection", "VisionSnapshot"]
