"""Shared planar primitives for robot-part contracts.

Geometry shared by spatial parts (base, gimbal, manipulator). These are
composable value objects, not polled resources: they carry no envelope
(``schema_version`` / ``sequence`` / ``timestamp``) and have no resource URI of
their own. They appear inline inside part snapshots and inside the scene
aggregate.

Units are SI throughout: metres, radians (counter-clockwise), seconds. Angles
are yaw about the map-frame +Z axis; the body heading equals ``yaw_rad``.
"""

from __future__ import annotations

from pydantic import BaseModel


class PlanarPose(BaseModel):
  """Pose in the map frame using metres and counter-clockwise radians."""

  x_m: float
  y_m: float
  yaw_rad: float


class PlanarVelocity(BaseModel):
  """Planar velocity in the map frame (m/s and rad/s)."""

  x_m_s: float
  y_m_s: float
  yaw_rad_s: float
