"""Canonical-schema and typed-model validation for EOS payloads."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any, cast

from jsonschema import Draft202012Validator, FormatChecker
from jsonschema.exceptions import ValidationError as JsonSchemaValidationError
from pydantic import BaseModel, ValidationError

from egia.contracts.actions.ack import PrimitiveAck
from egia.contracts.endpoints import DeviceKind, PrimitiveKind, ResourceKind
from egia.contracts.perception.world.asr import AsrSnapshot
from egia.contracts.perception.world.detection import DetectionSnapshot
from egia.contracts.perception.world.state import WorldStateSnapshot
from egia.contracts.registry import (
  ACTION_CONTRACTS,
  DEVICE_CONTRACTS,
  RESOURCE_CONTRACTS,
)
from egia.contracts.source import load_json, schema_registry

__all__ = [
  "PRIMITIVE_MODELS",
  "load_device_schema",
  "load_primitive_ack_schema",
  "load_primitive_schema",
  "load_resource_schema",
  "validate_device",
  "validate_primitive",
  "validate_primitive_ack",
  "validate_resource",
]

PRIMITIVE_MODELS: Mapping[PrimitiveKind, type[BaseModel]] = {
  kind: contract.model for kind, contract in ACTION_CONTRACTS.items()
}


def _canonical_validate(schema: dict[str, Any], data: dict[str, Any]) -> None:
  try:
    Draft202012Validator(
      schema, format_checker=FormatChecker(), registry=schema_registry()
    ).validate(data)
  except JsonSchemaValidationError as exc:
    location = ".".join(str(part) for part in exc.absolute_path) or "<root>"
    raise ValueError(f"{location}: {exc.message}") from exc


def load_device_schema(kind: DeviceKind) -> dict[str, Any]:
  """Load the canonical schema for one discovered EOS device class."""
  return load_json(DEVICE_CONTRACTS[kind].schema_path)


def load_resource_schema(kind: ResourceKind) -> dict[str, Any]:
  """Load the canonical schema for one readable EOS resource class."""
  return load_json(RESOURCE_CONTRACTS[kind].schema_path)


def load_primitive_schema(kind: PrimitiveKind) -> dict[str, Any]:
  """Load the canonical schema for one EOS primitive."""
  return load_json(ACTION_CONTRACTS[kind].schema_path)


def load_primitive_ack_schema() -> dict[str, Any]:
  """Load the canonical EOS primitive acknowledgement schema."""
  return load_json("eos/schemas/eos/actions/ack.v1.json")


def validate_device(kind: DeviceKind, data: dict[str, Any]) -> BaseModel:
  """Validate class-specific content from one discovered device descriptor."""
  try:
    _canonical_validate(load_device_schema(kind), data)
    return DEVICE_CONTRACTS[kind].model.model_validate(data)
  except (KeyError, TypeError, ValidationError, ValueError) as exc:
    raise ValueError(str(exc)) from exc


def validate_resource(kind: ResourceKind, data: dict[str, Any]) -> BaseModel:
  """Validate one readable EOS resource against its canonical contract."""
  try:
    _canonical_validate(load_resource_schema(kind), data)
    return RESOURCE_CONTRACTS[kind].model.model_validate(data)
  except (KeyError, TypeError, ValidationError, ValueError) as exc:
    raise ValueError(str(exc)) from exc


def validate_primitive(kind: PrimitiveKind, data: dict[str, Any]) -> BaseModel:
  """Validate one primitive argument object against its canonical contract."""
  try:
    _canonical_validate(load_primitive_schema(kind), data)
    return PRIMITIVE_MODELS[kind].model_validate(data)
  except (KeyError, TypeError, ValidationError, ValueError) as exc:
    raise ValueError(str(exc)) from exc


def validate_primitive_ack(data: dict[str, Any]) -> PrimitiveAck:
  """Validate an EOS acknowledgement against its canonical contract."""
  try:
    _canonical_validate(load_primitive_ack_schema(), data)
    return PrimitiveAck.model_validate(data)
  except (TypeError, ValidationError, ValueError) as exc:
    raise ValueError(str(exc)) from exc


def load_world_schema() -> dict[str, Any]:
  """Load the canonical fused-world schema."""
  return load_resource_schema(ResourceKind.WORLD)


def load_detection_schema() -> dict[str, Any]:
  """Load the canonical semantic-detection schema."""
  return load_resource_schema(ResourceKind.DETECTION)


def load_asr_schema() -> dict[str, Any]:
  """Load the canonical normalized-ASR schema."""
  return load_resource_schema(ResourceKind.ASR)


def validate_world(data: dict[str, Any] | WorldStateSnapshot) -> WorldStateSnapshot:
  """Validate or retain one typed fused-world snapshot."""
  if isinstance(data, WorldStateSnapshot):
    return data
  return cast(WorldStateSnapshot, validate_resource(ResourceKind.WORLD, data))


def validate_detection(
  data: dict[str, Any] | DetectionSnapshot,
) -> DetectionSnapshot:
  """Validate or retain one typed semantic-detection snapshot."""
  if isinstance(data, DetectionSnapshot):
    return data
  return cast(DetectionSnapshot, validate_resource(ResourceKind.DETECTION, data))


def validate_asr(data: dict[str, Any] | AsrSnapshot) -> AsrSnapshot:
  """Validate or retain one typed normalized-ASR snapshot."""
  if isinstance(data, AsrSnapshot):
    return data
  return cast(AsrSnapshot, validate_resource(ResourceKind.ASR, data))
