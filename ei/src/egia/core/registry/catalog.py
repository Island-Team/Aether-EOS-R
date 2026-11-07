"""Discovery-backed catalog of EOS primitives."""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass, field
from typing import Any, Protocol

from pydantic import BaseModel

from egia.contracts.endpoints import OPERATOR_PRIMITIVE_KINDS, PrimitiveKind
from egia.contracts.limits import PRIMITIVE_LIMIT_FIELDS, NumericRange
from egia.contracts.registry import action_contract

__all__ = ["Primitive", "PrimitiveRegistry"]


class PrimitiveDescriptor(Protocol):
  @property
  def name(self) -> str: ...

  @property
  def eos_robot_id(self) -> str: ...

  @property
  def device_id(self) -> str | None: ...

  @property
  def version(self) -> str: ...

  @property
  def description(self) -> str: ...

  @property
  def device_description(self) -> str: ...

  @property
  def limits(self) -> dict[str, NumericRange]: ...

  @property
  def scope(self) -> str: ...

  @property
  def kind(self) -> str: ...

  @property
  def health(self) -> str: ...

  @property
  def references(self) -> tuple[str, ...]: ...

  @property
  def absolute_frame(self) -> str | None: ...

  @property
  def dependencies(self) -> tuple[tuple[str, str], ...]: ...

  @property
  def accepts(self) -> tuple[str, ...]: ...


@dataclass(frozen=True)
class _PrimitiveDescriptor:
  name: str
  eos_robot_id: str
  device_id: str | None
  version: str
  description: str
  device_description: str = ""
  limits: dict[str, NumericRange] = field(default_factory=dict)
  scope: str = "device"
  kind: str = "direct"
  health: str = "ready"
  references: tuple[str, ...] = ()
  absolute_frame: str | None = None
  dependencies: tuple[tuple[str, str], ...] = ()
  accepts: tuple[str, ...] = ()


@dataclass(frozen=True)
class Primitive:
  """One EOS primitive bound to a discovered robot device."""

  kind: PrimitiveKind | None
  name: str
  uri: str
  backend: str
  device_id: str | None
  eos_robot_id: str
  version: str = ""
  description: str = ""
  device_description: str = ""
  limits: dict[str, NumericRange] = field(default_factory=dict)
  json_schema: dict[str, Any] = field(default_factory=dict)
  argument_model: type[BaseModel] | None = None
  operator: bool = False
  scope: str = "device"
  binding_kind: str = "direct"
  health: str = "ready"
  references: tuple[str, ...] = ()
  absolute_frame: str | None = None
  dependencies: tuple[tuple[str, str], ...] = ()
  accepts: tuple[str, ...] = ()


class PrimitiveRegistry:
  """Deterministic registry of model-visible EOS primitives."""

  def __init__(self, primitives: list[Primitive] | None = None) -> None:
    self._primitives: list[Primitive] = []
    self._by_key: dict[tuple[str, str | None, str | None], Primitive] = {}
    for primitive in primitives or []:
      self.register(primitive)

  @classmethod
  def from_contracts(
    cls,
    backends: Sequence[str] | None = None,
    *,
    expose_operator: bool = False,
    **_ignored,
  ) -> PrimitiveRegistry:
    """Build a deterministic exact-EOS catalog for isolated unit tests.

    Runtime composition always uses :meth:`from_primitives`; this helper has
    no transport claims and exists for units that do not construct a peer.
    """
    device_for = {
      PrimitiveKind.BASE_VELOCITY: "base-0",
      PrimitiveKind.BASE_NAVIGATE: "base-0",
      PrimitiveKind.BASE_POSTURE: "base-0",
      PrimitiveKind.GIMBAL_MOTION: "gimbal-0",
      PrimitiveKind.ARM_INIT: "arm-0",
      PrimitiveKind.ARM_MOVEJ: "arm-0",
      PrimitiveKind.ARM_MOVEL: "arm-0",
      PrimitiveKind.ARM_PICK: "arm-0",
      PrimitiveKind.GRIPPER_MOTION: "gripper-0",
      PrimitiveKind.SPEAKER_PLAY: "speaker-0",
      PrimitiveKind.SPEAKER_SPEAK: "speaker-0",
      PrimitiveKind.FACE_REGISTER: "camera-0",
    }
    registry = cls()
    for backend in backends or ["default"]:
      descriptors = [
        _PrimitiveDescriptor(
          name=kind.value,
          eos_robot_id="00000000-0000-7000-8000-000000000000",
          device_id=device,
          version=action_contract(kind).version,
          description=kind.value,
          limits={
            name: NumericRange(minimum=-1_000_000, maximum=1_000_000)
            for name in PRIMITIVE_LIMIT_FIELDS.get(kind, ())
          },
        )
        for kind, device in device_for.items()
      ]
      registry.extend(
        cls.from_primitives(backend, descriptors, expose_operator=expose_operator)
      )
    return registry

  @classmethod
  def from_primitives(
    cls,
    robot_alias: str,
    primitives: Sequence[PrimitiveDescriptor],
    *,
    expose_operator: bool = False,
  ) -> PrimitiveRegistry:
    catalog: list[Primitive] = []
    hidden_operator = set(OPERATOR_PRIMITIVE_KINDS) if not expose_operator else set()
    for descriptor in primitives:
      if getattr(descriptor, "health", "ready") != "ready":
        continue
      try:
        kind = PrimitiveKind(descriptor.name)
      except ValueError:
        continue
      if kind in hidden_operator:
        continue
      # Privileged primitives are deliberately absent from EOS discovery and
      # remain monitor-only even if a non-conforming peer advertises one.
      if kind in {
        PrimitiveKind.MOTION_STOP,
        PrimitiveKind.ROBOT_STOP,
        PrimitiveKind.SAFETY_ESTOP,
      }:
        continue
      contract = action_contract(kind)
      catalog.append(
        Primitive(
          kind=kind,
          name=kind.value,
          uri=kind.value,
          backend=robot_alias,
          device_id=descriptor.device_id,
          eos_robot_id=descriptor.eos_robot_id,
          version=descriptor.version,
          description=descriptor.description or kind.value,
          device_description=descriptor.device_description,
          limits=dict(descriptor.limits),
          json_schema=contract.model.model_json_schema(),
          argument_model=contract.model,
          operator=kind in OPERATOR_PRIMITIVE_KINDS,
          scope=getattr(descriptor, "scope", "device"),
          binding_kind=getattr(descriptor, "kind", "direct"),
          health=getattr(descriptor, "health", "ready"),
          references=getattr(descriptor, "references", ()),
          absolute_frame=getattr(descriptor, "absolute_frame", None),
          dependencies=getattr(descriptor, "dependencies", ()),
          accepts=getattr(descriptor, "accepts", ()),
        )
      )
    return cls(catalog)

  def extend(self, other: PrimitiveRegistry) -> None:
    for primitive in other.primitives():
      self.register(primitive)

  def replace_primitives(
    self,
    robot_alias: str,
    primitives: Sequence[PrimitiveDescriptor],
    *,
    expose_operator: bool = False,
  ) -> None:
    """Atomically replace one alias's catalog after EOS rediscovery."""
    retained = [
      primitive for primitive in self._primitives if primitive.backend != robot_alias
    ]
    replacement = self.from_primitives(
      robot_alias, primitives, expose_operator=expose_operator
    ).primitives()
    self._primitives = []
    self._by_key = {}
    for primitive in [*retained, *replacement]:
      self.register(primitive)

  def register(self, primitive: Primitive) -> None:
    key = (primitive.name, primitive.backend, primitive.device_id)
    if key in self._by_key:
      raise ValueError(
        f"primitive already registered: {primitive.name} on "
        f"{primitive.backend!r}/{primitive.device_id!r}"
      )
    self._by_key[key] = primitive
    self._primitives.append(primitive)

  def primitives(self) -> list[Primitive]:
    """Return primitives in stable discovery order."""
    return list(self._primitives)

  def planner_primitives(self) -> list[Primitive]:
    """Return the non-operator primitives eligible for model planning."""
    return [primitive for primitive in self._primitives if not primitive.operator]

  def render_for_prompt(self) -> str:
    lines: list[str] = []
    for primitive in self.planner_primitives():
      target = f"@{primitive.backend}/{primitive.device_id}"
      guidance = f" — {primitive.description}"
      if primitive.device_description:
        guidance += f"; device: {primitive.device_description}"
      lines.append(
        f"- {primitive.name}{target} "
        f"{_command_signature(primitive.json_schema, primitive.limits)}{guidance}"
      )
    return "\n".join(lines)

  def resolve(
    self,
    name: str,
    backend: str | None = None,
    device_id: str | None = None,
  ) -> Primitive | None:
    named = [primitive for primitive in self._primitives if primitive.name == name]
    if not named:
      return None
    if backend is None or device_id is None:
      targets = sorted(f"{item.backend}/{item.device_id}" for item in named)
      raise LookupError(f"explicit target required for {name!r}: {targets}")
    matches = [
      primitive
      for primitive in named
      if primitive.backend == backend and primitive.device_id == device_id
    ]
    if not matches:
      return None
    if len(matches) > 1:
      targets = sorted(f"{item.backend}/{item.device_id}" for item in matches)
      raise LookupError(f"ambiguous primitive {name!r}: present on {targets}")
    return matches[0]


_MODEL_HIDDEN_ARGUMENTS = frozenset({"type", "command_id"})


def _command_signature(schema: dict[str, Any], limits: dict[str, NumericRange]) -> str:
  properties = schema.get("properties")
  if not isinstance(properties, dict):
    return "[key=value ...]"
  required = set(schema.get("required") or [])
  parts: list[str] = []
  for name, shape in properties.items():
    if name in _MODEL_HIDDEN_ARGUMENTS or not isinstance(shape, dict):
      continue
    entry = f"{name}=<{_shape_name(shape, limits.get(name))}>"
    parts.append(entry if name in required else f"[{entry}]")
  return " ".join(parts) if parts else "[no arguments]"


def _shape_name(shape: dict[str, Any], limit: NumericRange | None = None) -> str:
  enum = shape.get("enum")
  if isinstance(enum, list):
    values = [str(value) for value in enum if value is not None]
    if values and len(values) <= 5:
      return "|".join(values)
  alternatives = shape.get("anyOf")
  if isinstance(alternatives, list):
    names = [
      _shape_name(option, limit)
      for option in alternatives
      if isinstance(option, dict) and option.get("type") != "null"
    ]
    if names:
      return "|".join(dict.fromkeys(names))
  if "$ref" in shape:
    return "value"
  kind = shape.get("type")
  if kind == "array":
    items = shape.get("items")
    item_name = _shape_name(items, limit) if isinstance(items, dict) else "value"
    return f"{item_name},..."
  if isinstance(kind, list):
    values = [str(value) for value in kind if value != "null"]
    return "|".join(values) or "value"
  name = str(kind or "value")
  if limit is None or name not in {"integer", "number"}:
    return name
  return f"{name}{_range_signature(limit)}"


def _range_signature(interval: NumericRange) -> str:
  if interval.minimum is None:
    assert interval.maximum is not None
    operator = "<" if interval.maximum_exclusive else "<="
    return f"[{operator}{_format_bound(interval.maximum)}]"
  if interval.maximum is None:
    operator = ">" if interval.minimum_exclusive else ">="
    return f"[{operator}{_format_bound(interval.minimum)}]"
  left = "(" if interval.minimum_exclusive else "["
  right = ")" if interval.maximum_exclusive else "]"
  return (
    f"{left}{_format_bound(interval.minimum)}..{_format_bound(interval.maximum)}{right}"
  )


def _format_bound(value: float) -> str:
  return str(int(value)) if value.is_integer() else f"{value:g}"
