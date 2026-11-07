"""Device-specific numeric ranges advertised by EOS primitive discovery."""

from __future__ import annotations

from collections.abc import Mapping
from math import isfinite
from typing import Any

from pydantic import (
  AliasChoices,
  BaseModel,
  ConfigDict,
  Field,
  StrictBool,
  field_validator,
  model_validator,
)

from egia.contracts.endpoints import PrimitiveKind


class NumericRange(BaseModel):
  """One non-empty finite numeric interval from an inline primitive binding."""

  model_config = ConfigDict(extra="forbid", frozen=True, allow_inf_nan=False)

  minimum: float | None = None
  maximum: float | None = None
  minimum_exclusive: StrictBool = Field(
    default=False,
    validation_alias=AliasChoices("minimum_exclusive", "minimumExclusive"),
  )
  maximum_exclusive: StrictBool = Field(
    default=False,
    validation_alias=AliasChoices("maximum_exclusive", "maximumExclusive"),
  )

  @field_validator("minimum", "maximum", mode="before")
  @classmethod
  def _numeric_bound(cls, value: Any) -> Any:
    if value is not None and (
      isinstance(value, bool) or not isinstance(value, (int, float))
    ):
      raise TypeError("numeric bounds must be numbers")
    return value

  @model_validator(mode="after")
  def _valid_interval(self) -> NumericRange:
    if self.minimum is None and self.maximum is None:
      raise ValueError("a numeric range requires minimum or maximum")
    if self.minimum_exclusive and self.minimum is None:
      raise ValueError("minimum_exclusive requires minimum")
    if self.maximum_exclusive and self.maximum is None:
      raise ValueError("maximum_exclusive requires maximum")
    if self.minimum is not None and not isfinite(self.minimum):
      raise ValueError("minimum must be finite")
    if self.maximum is not None and not isfinite(self.maximum):
      raise ValueError("maximum must be finite")
    if (
      self.minimum is not None
      and self.maximum is not None
      and (
        self.minimum > self.maximum
        or (
          self.minimum == self.maximum
          and (self.minimum_exclusive or self.maximum_exclusive)
        )
      )
    ):
      raise ValueError("bounds do not form a non-empty range")
    return self

  def contains(self, value: float) -> bool:
    """Return whether ``value`` lies inside this interval."""
    if not isfinite(value):
      return False
    if self.minimum is not None and (
      value < self.minimum or (self.minimum_exclusive and value == self.minimum)
    ):
      return False
    return not (
      self.maximum is not None
      and (value > self.maximum or (self.maximum_exclusive and value == self.maximum))
    )


PRIMITIVE_LIMIT_FIELDS: Mapping[PrimitiveKind, frozenset[str]] = {
  PrimitiveKind.BASE_VELOCITY: frozenset(
    {"linear_x_m_s", "linear_y_m_s", "angular_z_rad_s", "valid_for_ms"}
  ),
  PrimitiveKind.BASE_NAVIGATE: frozenset({"x_m", "y_m", "yaw_rad"}),
  PrimitiveKind.GIMBAL_MOTION: frozenset({"pan_rad", "tilt_rad", "speed_rad_s"}),
  PrimitiveKind.ARM_MOVEJ: frozenset({"joint_positions_rad", "speed_rad_s"}),
  PrimitiveKind.ARM_MOVEL: frozenset({"speed_m_s"}),
  PrimitiveKind.GRIPPER_MOTION: frozenset({"force_n"}),
}

# EOS requires these fields to admit a primitive. Remaining limit fields are
# optional capabilities: their command values are valid only when advertised.
PRIMITIVE_REQUIRED_LIMIT_FIELDS: Mapping[PrimitiveKind, frozenset[str]] = {
  PrimitiveKind.BASE_VELOCITY: PRIMITIVE_LIMIT_FIELDS[PrimitiveKind.BASE_VELOCITY],
  PrimitiveKind.BASE_NAVIGATE: PRIMITIVE_LIMIT_FIELDS[PrimitiveKind.BASE_NAVIGATE],
  PrimitiveKind.GIMBAL_MOTION: frozenset({"pan_rad", "tilt_rad"}),
  PrimitiveKind.ARM_MOVEJ: frozenset({"joint_positions_rad"}),
}


def parse_numeric_limits(
  kind: PrimitiveKind, raw: Mapping[str, Any]
) -> dict[str, NumericRange]:
  """Validate one descriptor's required and optional numeric limits map."""
  allowed = PRIMITIVE_LIMIT_FIELDS.get(kind, frozenset())
  required = PRIMITIVE_REQUIRED_LIMIT_FIELDS.get(kind, frozenset())
  if not required.issubset(raw) or not set(raw).issubset(allowed):
    missing = sorted(required - set(raw))
    unknown = sorted(set(raw) - allowed)
    details = []
    if missing:
      details.append(f"missing {missing}")
    if unknown:
      details.append(f"unknown {unknown}")
    raise ValueError("invalid numeric limits: " + ", ".join(details))
  return {name: NumericRange.model_validate(value) for name, value in raw.items()}


def validate_numeric_arguments(
  kind: PrimitiveKind,
  limits: Mapping[str, NumericRange],
  arguments: Mapping[str, Any],
) -> None:
  """Enforce ranges and reject optional values absent from device capabilities."""
  unavailable = sorted(
    name
    for name in arguments
    if name in PRIMITIVE_LIMIT_FIELDS.get(kind, frozenset()) and name not in limits
  )
  if unavailable:
    raise ValueError(f"{unavailable[0]} is not admitted by this device")
  for name, interval in limits.items():
    if name not in arguments:
      continue
    value = arguments[name]
    values = value if isinstance(value, list) else [value]
    for item in values:
      if isinstance(item, bool) or not isinstance(item, (int, float)):
        raise TypeError(f"{name} must be numeric")
      if not interval.contains(float(item)):
        raise ValueError(f"{name}={item!r} is outside the admitted device range")


__all__ = [
  "PRIMITIVE_LIMIT_FIELDS",
  "PRIMITIVE_REQUIRED_LIMIT_FIELDS",
  "NumericRange",
  "parse_numeric_limits",
  "validate_numeric_arguments",
]
