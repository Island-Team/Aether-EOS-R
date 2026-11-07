"""Per-detection rendering — the one mission-addressable line per object.

Shared by the two halves of the detection path: the parts family renders raw
per-detection lines (uncapped, snapshot order) into the admitted detection
record, and the context digest re-renders them capped. Keeping it here means
ingest stays raw and dumb while context owns the token-budget cap.
"""

from __future__ import annotations

from egia.contracts.perception.world.detection import Detection

__all__ = ["render_detection"]


def render_detection(detection: Detection) -> str:
  """Render one typed semantic detection as a compact agent fact."""
  parts = [detection.id, detection.label, f"confidence={detection.confidence:.2f}"]
  if detection.distance_m is not None:
    parts.append(f"distance={detection.distance_m:.1f}m")
  if detection.bearing_rad is not None:
    parts.append(f"bearing={detection.bearing_rad:.2f}rad")
  if detection.attributes:
    parts.extend(
      f"{name}={value}" for name, value in sorted(detection.attributes.items())
    )
  return " ".join(parts)
