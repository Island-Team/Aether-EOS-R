"""Typed EGIA stage composition and governed in-process delivery.

IsOS owns graph bytes and admission. EGIA owns local delivery. This module admits only exact local stage
edges and keeps actuator and emergency egress outside the authored graph.
"""

from __future__ import annotations

import asyncio
import json
import re
import time
from collections import deque
from collections.abc import Awaitable, Callable, Mapping
from dataclasses import dataclass, field
from datetime import UTC, datetime, timedelta
from math import isfinite
from pathlib import Path
from types import MappingProxyType
from typing import Any

from egia.lib import shared_events


@dataclass(frozen=True)
class StageType:
  """Registered stage placement and its typed local ports."""

  id: str
  layer: str
  group: str
  description: str
  inputs: dict[str, str]
  outputs: dict[str, str]
  settings: tuple[str, ...] = ()


STAGE_TYPES = {
  item.id: item
  for item in (
    StageType(
      "egia.link.gateway@v0.1",
      "io",
      "link",
      "Connects a stable backend alias to verified EOS discovery and operation results.",
      {"gateway": "endpoint", "admitted": "admission", "emergency_stop": "stop"},
      {
        "discovery": "discovery",
        "resource": "resource",
        "result": "result",
        "health": "health",
      },
      ("backend_alias", "gateway"),
    ),
    StageType(
      "egia.perception.admission@v0.1",
      "evidence",
      "perception",
      "Validates and bounds attributed EOS and operator evidence.",
      {"resource": "resource", "hearing": "hearing", "result": "result"},
      {"observations": "observations"},
      ("capacity", "history_per_kind"),
    ),
    StageType(
      "egia.context.builder@v0.1",
      "cognition",
      "context",
      "Renders a deterministic bounded context snapshot from evidence.",
      {"observations": "observations", "body": "body", "memory": "memory"},
      {"snapshot": "snapshot"},
    ),
    StageType(
      "egia.missions.queue@v0.1",
      "cognition",
      "missions",
      "Owns mission admission, selection, revisions, and durable lifecycle.",
      {"intent": "intent", "result": "result"},
      {"selected": "selected", "lifecycle": "lifecycle"},
      ("mission_capacity",),
    ),
    StageType(
      "egia.decision.planner@v0.1",
      "cognition",
      "decision",
      "Produces typed primitive proposals and plan state from a mission and context.",
      {"selected": "selected", "snapshot": "snapshot"},
      {"proposal": "proposal", "plan_state": "plan_state"},
      ("controller.model", "reasoner.model", "max_invalid_replies", "step_timeout_s"),
    ),
    StageType(
      "egia.safety.gate@v0.1",
      "governance",
      "safety",
      "Admits only exact current typed primitives through the fixed link route.",
      {"proposal": "proposal", "registry": "registry", "latch": "latch"},
      {"admission": "admission", "rejection": "rejection"},
    ),
    StageType(
      "egia.monitor.health@v0.1",
      "governance",
      "monitor",
      "Tracks backend health and progress and owns the emergency latch.",
      {"health": "health", "progress": "progress", "bounds": "bounds"},
      {"status": "status", "latch": "latch"},
      ("heartbeat_interval_s", "failure_threshold", "stall_timeout_s"),
    ),
  )
}

# These schema IDs are global catalog identities. Local delivery still uses the
# short kind names above; the prefix prevents collisions with EOS-owned types.
STAGE_DATA_CLASSES = {
  "endpoint": "configuration",
  "admission": "command",
  "stop": "command",
  "discovery": "evidence",
  "resource": "evidence",
  "result": "feedback",
  "health": "feedback",
  "hearing": "evidence",
  "observations": "evidence",
  "body": "evidence",
  "memory": "evidence",
  "snapshot": "evidence",
  "intent": "mission",
  "selected": "mission",
  "lifecycle": "mission",
  "proposal": "proposal",
  "plan_state": "mission",
  "registry": "evidence",
  "latch": "safety",
  "rejection": "feedback",
  "progress": "feedback",
  "bounds": "evidence",
  "status": "feedback",
}

_FIELD_SCHEMAS: dict[str, dict[str, dict[str, object]]] = {
  "endpoint": {"transport": {"type": "string"}, "address": {"type": "string"}},
  "admission": {
    "backend": {"type": "string"},
    "device": {"type": "string"},
    "primitive": {"type": "string"},
    "version": {"type": "string"},
    "arguments": {"type": "object"},
  },
  "stop": {"backend": {"type": "string"}, "reason": {"type": "string"}},
  "discovery": {"backend": {"type": "string"}, "generation": {"type": "integer"}},
  "resource": {
    "backend": {"type": "string"},
    "reference": {"type": "string"},
    "sample": {
      "type": "object",
      "properties": {
        "text": {"type": "string", "minLength": 1},
        "eos_robot_id": {"type": "string", "minLength": 1},
      },
      "required": ["text", "eos_robot_id"],
      "additionalProperties": True,
    },
  },
  "result": {"operation_id": {"type": "string"}, "terminal_state": {"type": "string"}},
  "health": {"backend": {"type": "string"}, "state": {"type": "string"}},
  "hearing": {
    "speaker": {"type": "string"},
    "text": {"type": "string"},
    "observed_at": {"type": "string", "format": "date-time", "x-unit": "UTC"},
  },
  "observations": {"items": {"type": "array", "items": {"type": "object"}}},
  "body": {"backend": {"type": "string"}, "sample": {"type": "object"}},
  "memory": {"items": {"type": "array", "items": {"type": "object"}}},
  "snapshot": {"text": {"type": "string"}},
  "intent": {"mission_id": {"type": "string"}, "revision": {"type": "integer"}},
  "selected": {"mission_id": {"type": "string"}, "revision": {"type": "integer"}},
  "lifecycle": {
    "mission_id": {"type": "string"},
    "revision": {"type": "integer"},
    "state": {"type": "string"},
  },
  "proposal": {
    "backend": {"type": "string"},
    "device": {"type": "string"},
    "primitive": {"type": "string"},
    "version": {"type": "string"},
    "arguments": {"type": "object"},
  },
  "plan_state": {
    "plan_id": {"type": "string"},
    "revision": {"type": "integer"},
    "phase": {"type": "string"},
  },
  "registry": {"backend": {"type": "string"}, "generation": {"type": "integer"}},
  "latch": {"engaged": {"type": "boolean"}, "episode": {"type": "integer"}},
  "rejection": {"reason": {"type": "string"}},
  "progress": {
    "operation_id": {"type": "string"},
    "elapsed_s": {"type": "number", "x-unit": "s"},
  },
  "bounds": {
    "source": {"type": "string"},
    "value": {"type": "number"},
    "limit": {"type": "number"},
    "unit": {"type": "string"},
  },
  "status": {"state": {"type": "string"}, "episode": {"type": "integer"}},
}


def stage_data_types() -> list[dict[str, object]]:
  """Project one EGIA-owned object schema for every registered port kind."""
  kinds = {
    kind
    for stage in STAGE_TYPES.values()
    for kind in (*stage.inputs.values(), *stage.outputs.values())
  }
  if kinds != set(_FIELD_SCHEMAS) or kinds != set(STAGE_DATA_CLASSES):
    raise RuntimeError(
      "stage data schemas and classes must cover exact registered ports"
    )
  return [
    {
      "id": f"egia.stage.{kind}@v0.1",
      "schema": {
        "type": "object",
        "properties": _FIELD_SCHEMAS[kind],
        "required": list(_FIELD_SCHEMAS[kind]),
        "additionalProperties": True,
      },
    }
    for kind in sorted(kinds)
  ]


SETTING_DEFAULTS: dict[str, object] = {
  "capacity": 64,
  "history_per_kind": 8,
  "mission_capacity": 8,
  "controller.model": "gemma4-fast",
  "reasoner.model": "gemma4-reasoning",
  "max_invalid_replies": 3,
  "step_timeout_s": 30.0,
  "heartbeat_interval_s": 5.0,
  "failure_threshold": 3,
  "stall_timeout_s": 150.0,
}

# A same-run observation older than this cannot be forwarded as current evidence.
MAX_STAGE_EVENT_AGE = timedelta(seconds=300)

# These are process-owned ingress or fixed egress, never graph-editable edges.
FIXED_INPUTS = frozenset(
  {
    ("egia.link.gateway@v0.1", "gateway"),
    ("egia.link.gateway@v0.1", "admitted"),
    ("egia.link.gateway@v0.1", "emergency_stop"),
    ("egia.missions.queue@v0.1", "intent"),
    ("egia.safety.gate@v0.1", "registry"),
    ("egia.safety.gate@v0.1", "latch"),
    ("egia.monitor.health@v0.1", "progress"),
  }
)
FIXED_OUTPUTS = frozenset(
  {("egia.safety.gate@v0.1", "admission"), ("egia.monitor.health@v0.1", "latch")}
)
OPTIONAL_INPUTS = frozenset(
  {
    ("egia.perception.admission@v0.1", "hearing"),
    ("egia.context.builder@v0.1", "body"),
    ("egia.context.builder@v0.1", "memory"),
    ("egia.missions.queue@v0.1", "result"),
    ("egia.monitor.health@v0.1", "bounds"),
  }
)


class StageGraphError(ValueError):
  """A path-specific graph composition failure."""

  def __init__(self, path: str, code: str, message: str) -> None:
    super().__init__(message)
    self.path = path
    self.code = code


@dataclass(frozen=True)
class StageInstance:
  """One graph instance; its canonical path attributes every stage value."""

  path: str
  type: StageType
  description: str
  settings: dict[str, Any]


@dataclass(frozen=True)
class StageEdge:
  """One validated evidence or proposal edge within the EGIA process."""

  source: str
  output: str
  target: str
  input: str
  source_pointer: str
  priority: int = 0
  frequency_hz: float | None = None


# Only these replaceable, process-local evidence values have a bounded consumer.
# Exact type/port pairs prevent a new evidence-shaped route acquiring QoS by accident.
SCHEDULED_EDGES = frozenset(
  {
    (
      "egia.link.gateway@v0.1",
      "resource",
      "egia.perception.admission@v0.1",
      "resource",
    ),
    (
      "egia.perception.admission@v0.1",
      "observations",
      "egia.context.builder@v0.1",
      "observations",
    ),
  }
)


@dataclass(frozen=True)
class StageComposition:
  """Immutable stage instances and their permitted local delivery edges."""

  instances: dict[str, StageInstance]
  edges: tuple[StageEdge, ...]
  source_revision: str | None = None


def _pointer(value: str) -> str:
  return value.replace("~", "~0").replace("/", "~1")


def compose_stages(graph: dict[str, Any]) -> StageComposition:
  """Validate exact new stage identities and reject every authored command route.

  Legacy common design nodes remain readable, but do not become live instances.
  """
  if not isinstance(graph, dict) or graph.get("version") not in {"0.1.0", "1.0.0"}:
    raise StageGraphError(
      "/graph/version",
      "unsupported_graph_version",
      "supported graph versions: 0.1.0, 1.0.0",
    )
  cores = graph.get("cores")
  if not isinstance(cores, dict) or not isinstance(cores.get("egia"), dict):
    raise StageGraphError(
      "/graph/cores/egia", "invalid_graph", "EGIA core must be an object"
    )
  core = cores["egia"]
  layers = core.get("layers", {})
  if not isinstance(layers, dict):
    raise StageGraphError(
      "/graph/cores/egia/layers", "invalid_graph", "EGIA layers must be an object"
    )
  instances: dict[str, StageInstance] = {}
  raw_nodes: list[tuple[str, str, dict[str, Any]]] = []
  for layer_id, layer in layers.items():
    layer_path = f"/graph/cores/egia/layers/{_pointer(layer_id)}"
    if not isinstance(layer, dict) or not isinstance(layer.get("groups"), dict):
      raise StageGraphError(
        f"{layer_path}/groups", "invalid_graph", "groups must be an object"
      )
    for group_id, group in layer["groups"].items():
      group_path = f"{layer_path}/groups/{_pointer(group_id)}"
      if not isinstance(group, dict) or not isinstance(group.get("nodes"), dict):
        raise StageGraphError(
          f"{group_path}/nodes", "invalid_graph", "nodes must be an object"
        )
      for node_id, node in group["nodes"].items():
        pointer = f"/graph/cores/egia/layers/{_pointer(layer_id)}/groups/{_pointer(group_id)}/nodes/{_pointer(node_id)}"
        if not isinstance(node, dict):
          raise StageGraphError(pointer, "invalid_node", "node must be an object")
        if node.get("id", node_id) != node_id:
          raise StageGraphError(
            f"{pointer}/id", "invalid_node_id", "node id must match its graph key"
          )
        type_id = node.get("node_type")
        if type_id not in STAGE_TYPES:
          if type_id in {
            "egia.common.source@v1.0",
            "egia.common.context@v1.0",
            "egia.common.planner@v1.0",
            "egia.common.guard@v1.0",
          }:
            continue
          raise StageGraphError(
            f"{pointer}/node_type",
            "unsupported_node_type_version",
            f"unsupported EGIA type {type_id!r}; supported: {', '.join(sorted(STAGE_TYPES))}",
          )
        spec = STAGE_TYPES[type_id]
        if (layer_id, group_id) != (spec.layer, spec.group):
          raise StageGraphError(
            f"{pointer}/node_type",
            "invalid_node_placement",
            f"{type_id} belongs at egia/{spec.layer}/{spec.group}",
          )
        path = f"egia/{layer_id}/{group_id}/{node_id}"
        description = node.get("description", "")
        if not isinstance(description, str) or not description.strip():
          raise StageGraphError(
            f"{pointer}/description",
            "invalid_description",
            "stage instance description is required",
          )
        settings = node.get("settings", {})
        if not isinstance(settings, dict):
          raise StageGraphError(
            f"{pointer}/settings", "invalid_settings", "settings must be an object"
          )
        for name, value in settings.items():
          setting_path = f"{pointer}/settings/{_pointer(name)}"
          if name not in spec.settings:
            raise StageGraphError(
              setting_path, "unknown_setting", f"unknown setting {name}"
            )
          if name in {"backend_alias", "controller.model", "reasoner.model"}:
            if not isinstance(value, str) or not value.strip():
              raise StageGraphError(
                setting_path, "invalid_setting", "setting must be a nonblank string"
              )
          elif name == "gateway":
            if not isinstance(value, dict) or value.get("transport", "tcp") not in {
              "tcp",
              "uds",
            }:
              raise StageGraphError(
                setting_path,
                "invalid_setting",
                "gateway must be a TCP or UDS endpoint object",
              )
            if value.get("transport", "tcp") == "uds":
              valid_endpoint = (
                isinstance(value.get("path"), str)
                and value["path"].startswith("/")
                and "host" not in value
                and "port" not in value
              )
            else:
              port = value.get("port")
              valid_endpoint = (
                isinstance(value.get("host"), str)
                and bool(value["host"].strip())
                and isinstance(port, int)
                and not isinstance(port, bool)
                and 1 <= port <= 65535
                and "path" not in value
              )
            if not valid_endpoint:
              raise StageGraphError(
                setting_path,
                "invalid_setting",
                "gateway requires a valid transport-specific EOS endpoint",
              )
          elif name in {"step_timeout_s", "heartbeat_interval_s", "stall_timeout_s"}:
            if (
              isinstance(value, bool)
              or not isinstance(value, (int, float))
              or not isfinite(value)
              or value <= 0
            ):
              raise StageGraphError(
                setting_path,
                "invalid_setting",
                "seconds must be finite and greater than zero",
              )
          else:
            bounds = {
              "capacity": 10000,
              "history_per_kind": 10000,
              "mission_capacity": 1024,
            }
            max_count = bounds[name] if name in bounds else 2**31 - 1
            if (
              isinstance(value, bool)
              or not isinstance(value, int)
              or not 1 <= value <= max_count
            ):
              raise StageGraphError(
                setting_path,
                "invalid_setting",
                "count is outside the supported positive range",
              )
        if spec.id == "egia.link.gateway@v0.1" and not settings.get("backend_alias"):
          raise StageGraphError(
            f"{pointer}/settings/backend_alias",
            "missing_setting",
            "link requires a stable backend alias",
          )
        if spec.id == "egia.link.gateway@v0.1" and "gateway" not in settings:
          raise StageGraphError(
            f"{pointer}/settings/gateway",
            "missing_setting",
            "managed link requires a verified EOS gateway reference",
          )
        instances[path] = StageInstance(path, spec, description, settings)
        raw_nodes.append((path, pointer, node))
  edges: list[StageEdge] = []
  for target_path, pointer, node in raw_nodes:
    target = instances[target_path]
    inputs = node.get("inputs", {})
    if not isinstance(inputs, dict):
      raise StageGraphError(
        f"{pointer}/inputs", "invalid_inputs", "inputs must be an object"
      )
    for input_name, source in inputs.items():
      edge_path = f"{pointer}/inputs/{_pointer(input_name)}"
      if input_name not in target.type.inputs:
        raise StageGraphError(edge_path, "unknown_input", f"unknown input {input_name}")
      if (target.type.id, input_name) in FIXED_INPUTS:
        raise StageGraphError(
          edge_path, "fixed_route", "fixed runtime input cannot be authored"
        )
      if isinstance(source, list):
        if len(source) != 1:
          raise StageGraphError(
            edge_path, "invalid_cardinality", "stage input requires exactly one source"
          )
        source = source[0]
        edge_path = f"{edge_path}/0"
      if (
        not isinstance(source, dict)
        or not isinstance(source.get("node"), str)
        or not isinstance(source.get("output"), str)
      ):
        raise StageGraphError(
          edge_path, "invalid_source", "source requires node and output strings"
        )
      origin = instances.get(source["node"])
      if origin is None:
        raise StageGraphError(
          f"{edge_path}/node",
          "unknown_source",
          "source must be a configured EGIA stage",
        )
      output_name = source["output"]
      if output_name not in origin.type.outputs:
        raise StageGraphError(
          f"{edge_path}/output", "unknown_output", f"unknown output {output_name}"
        )
      if (origin.type.id, output_name) in FIXED_OUTPUTS or origin.type.outputs[
        output_name
      ] != target.type.inputs[input_name]:
        raise StageGraphError(
          edge_path, "invalid_route", "fixed or mismatched stage value cannot be routed"
        )
      route = (origin.type.id, output_name, target.type.id, input_name)
      controlled = "priority" in source or "frequency_hz" in source
      if controlled and route not in SCHEDULED_EDGES:
        raise StageGraphError(
          edge_path, "unsupported_edge_control", "this exact EGIA route has no edge QoS"
        )
      priority = source.get("priority", 0)
      if controlled and (
        isinstance(priority, bool)
        or not isinstance(priority, int)
        or not -20 <= priority <= 19
      ):
        raise StageGraphError(
          f"{edge_path}/priority",
          "invalid_edge_priority",
          "priority must be an integer from -20 to 19",
        )
      frequency = source.get("frequency_hz")
      if "frequency_hz" in source and (
        isinstance(frequency, bool)
        or not isinstance(frequency, (int, float))
        or not isfinite(frequency)
        or frequency <= 0
      ):
        raise StageGraphError(
          f"{edge_path}/frequency_hz",
          "invalid_edge_frequency",
          "frequency_hz must be finite and greater than zero",
        )
      edges.append(
        StageEdge(
          origin.path,
          output_name,
          target.path,
          input_name,
          edge_path,
          priority,
          frequency,
        )
      )
    for required in target.type.inputs:
      if (
        target.type.id,
        required,
      ) not in FIXED_INPUTS | OPTIONAL_INPUTS and required not in inputs:
        raise StageGraphError(
          f"{pointer}/inputs/{required}",
          "missing_input",
          f"required input {required} is missing",
        )
  aliases = [
    instance.settings["backend_alias"]
    for instance in instances.values()
    if instance.type.id == "egia.link.gateway@v0.1"
  ]
  if len(aliases) != len(set(aliases)):
    raise StageGraphError(
      "/graph/cores/egia",
      "duplicate_backend_alias",
      "link backend aliases must be unique",
    )
  counts: dict[str, int] = {}
  for instance in instances.values():
    counts[instance.type.id] = counts.get(instance.type.id, 0) + 1
  if counts and (
    set(counts) != set(STAGE_TYPES) or any(count != 1 for count in counts.values())
  ):
    raise StageGraphError(
      "/graph/cores/egia",
      "incomplete_stage_pipeline",
      "a live stage graph requires exactly one instance of each registered stage",
    )
  outgoing: dict[str, list[str]] = {path: [] for path in instances}
  for edge in edges:
    outgoing[edge.source].append(edge.target)
  visiting: set[str] = set()
  visited: set[str] = set()

  def visit(path: str) -> None:
    if path in visiting:
      raise StageGraphError(
        "/graph/cores/egia", "stage_cycle", "authored EGIA stage edges contain a cycle"
      )
    if path in visited:
      return
    visiting.add(path)
    for target in outgoing[path]:
      visit(target)
    visiting.remove(path)
    visited.add(path)

  for path in instances:
    visit(path)
  source_revision = graph.get("revision")
  if source_revision is not None and (
    not isinstance(source_revision, str)
    or re.fullmatch(r"[0-9a-f]{64}", source_revision) is None
  ):
    raise StageGraphError(
      "/revision",
      "invalid_revision",
      "source graph revision must be a lowercase SHA-256",
    )
  return StageComposition(instances, tuple(edges), source_revision)


_PAYLOAD_FIELDS: dict[str, dict[str, type]] = {
  "discovery": {"backend": str, "generation": int},
  "resource": {"backend": str, "reference": str, "sample": dict},
  "result": {"operation_id": str, "terminal_state": str},
  "health": {"backend": str, "state": str},
  "observations": {"items": list},
  "snapshot": {"text": str},
  "selected": {"mission_id": str, "revision": int},
  "lifecycle": {"mission_id": str, "revision": int, "state": str},
  "proposal": {
    "backend": str,
    "device": str,
    "primitive": str,
    "version": str,
    "arguments": dict,
  },
  "plan_state": {"plan_id": str, "revision": int, "phase": str},
  "admission": {
    "backend": str,
    "device": str,
    "primitive": str,
    "version": str,
    "arguments": dict,
  },
  "rejection": {"reason": str},
  "status": {"state": str, "episode": int},
  "latch": {"engaged": bool, "episode": int},
}


def _freeze(value: object) -> object:
  """Copy supported stage data so a producer cannot mutate an admitted value."""
  if isinstance(value, Mapping):
    return MappingProxyType({key: _freeze(item) for key, item in value.items()})
  if isinstance(value, (list, tuple)):
    return tuple(_freeze(item) for item in value)
  if value is None or isinstance(value, (str, int, float, bool)):
    return value
  raise ValueError(f"unsupported mutable stage data {type(value).__name__}")


@dataclass(frozen=True)
class StagePayload:
  """Validated kind-specific stage fields, immutable across local delivery."""

  kind: str
  fields: Mapping[str, object]

  def __post_init__(self) -> None:
    required = _PAYLOAD_FIELDS.get(self.kind)
    if required is None:
      raise ValueError(f"unsupported stage payload kind {self.kind!r}")
    for name, expected in required.items():
      value = self.fields.get(name)
      if not isinstance(value, expected) or (
        expected is str
        and name != "text"
        and isinstance(value, str)
        and not value.strip()
      ):
        raise ValueError(f"{self.kind}.{name} must be a valid {expected.__name__}")
      if expected is int and isinstance(value, bool):
        raise ValueError(f"{self.kind}.{name} must be an integer")
    object.__setattr__(self, "fields", _freeze(self.fields))


@dataclass(frozen=True)
class StageValue:
  """One typed in-process value with immutable source and correlation identity."""

  kind: str
  event_id: str
  run_id: str
  source_node: str
  observed_at: datetime
  payload: StagePayload
  backend: str | None = None
  received_at: datetime = field(default_factory=lambda: datetime.now(UTC))

  def __post_init__(self) -> None:
    if self.kind != self.payload.kind:
      raise ValueError("stage value kind and typed payload kind differ")


@dataclass(frozen=True)
class StageDiagnostic:
  """Stable stage delivery diagnostic with attribution and correlation."""

  code: str
  node: str
  event_id: str


@dataclass
class EdgeCounters:
  """Visible per-edge delivery and replacement counts."""

  delivered: int = 0
  gaps: int = 0
  overflow: int = 0


class StageRuntime:
  """Delivers validated values to one owner per port and stops before egress."""

  def __init__(
    self,
    composition: StageComposition,
    run_id: str,
    *,
    clock: Callable[[], datetime] | None = None,
    monotonic_clock: Callable[[], int] | None = None,
    auto_drain: bool = True,
  ) -> None:
    if not run_id.strip():
      raise ValueError("run_id is required")
    self.composition = composition
    self.run_id = run_id
    self.source_revision = composition.source_revision
    self._clock = clock or (lambda: datetime.now(UTC))
    self._monotonic_clock = monotonic_clock or time.monotonic_ns
    self._auto_drain = auto_drain
    self._handlers: dict[tuple[str, str], Callable[[StageValue], None]] = {}
    self._async_handlers: dict[
      tuple[str, str], Callable[[StageValue], Awaitable[None]]
    ] = {}
    self._owners: dict[str, object] = {}
    self._received: dict[tuple[str, str], StageValue] = {}
    self._seen: set[str] = set()
    self._seen_values: dict[str, tuple[object, ...]] = {}
    self._seen_order: deque[str] = deque()
    self._pending: dict[StageEdge, StageValue] = {}
    self._last_delivered_ns: dict[StageEdge, int] = {}
    self._fair_credit: dict[StageEdge, int] = {}
    self._last_warning_ns: dict[StageEdge, int] = {}
    self.on_overflow: Callable[[StageEdge, EdgeCounters], None] | None = None
    self.edge_counters: dict[StageEdge, EdgeCounters] = {
      edge: EdgeCounters() for edge in composition.edges if self._scheduled(edge)
    }
    self._async_pending: deque[tuple[StageEdge, StageValue]] = deque()
    self._async_lock = asyncio.Lock()
    self.diagnostics: deque[StageDiagnostic] = deque(maxlen=1000)
    self._stopped = False
    self._started = False

  def bind_owner(self, node: str, owner: object) -> None:
    """Associate one configured stage instance with its existing core owner."""
    if node not in self.composition.instances or node in self._owners:
      raise ValueError("unknown or already owned stage")
    self._owners[node] = owner

  def start(self) -> None:
    """Begin delivery only when every configured stage has exactly one owner."""
    if self._stopped or set(self._owners) != set(self.composition.instances):
      raise RuntimeError("stage owners are incomplete or stopping")
    self._started = True

  @property
  def active(self) -> bool:
    """Whether the configured owner set can still accept stage values."""
    return self._started and not self._stopped

  def bind(
    self, node: str, input_name: str, handler: Callable[[StageValue], None]
  ) -> None:
    """Bind the sole receiving owner for a configured input port."""
    instance = self.composition.instances[node]
    if input_name not in instance.type.inputs or (node, input_name) in self._handlers:
      raise ValueError("unknown or already bound stage input")
    self._handlers[node, input_name] = handler

  def receive(self, node: str, input_name: str, value: StageValue) -> None:
    """Store the latest value delivered to an explicitly owned stage input."""
    if (
      node not in self._owners
      or self.composition.instances[node].type.inputs.get(input_name) != value.kind
    ):
      raise ValueError("value does not match a bound stage owner input")
    self._received[node, input_name] = value

  def bind_async(
    self, node: str, input_name: str, handler: Callable[[StageValue], Awaitable[None]]
  ) -> None:
    """Bind an owner action after typed local delivery, without command authority."""
    if (node, input_name) in self._async_handlers or (
      node,
      input_name,
    ) not in self._handlers:
      raise ValueError("async stage input must have one bound receiving owner")
    self._async_handlers[node, input_name] = handler

  def latest(self, node: str, input_name: str) -> StageValue | None:
    """Read the last value accepted by an owned input port."""
    return self._received.get((node, input_name))

  def _scheduled(self, edge: StageEdge) -> bool:
    source = self.composition.instances[edge.source].type.id
    target = self.composition.instances[edge.target].type.id
    return (source, edge.output, target, edge.input) in SCHEDULED_EDGES

  def _deliver(self, edge: StageEdge, value: StageValue) -> None:
    self._handlers[edge.target, edge.input](value)
    if edge in self.edge_counters:
      self.edge_counters[edge].delivered += 1
      if edge.frequency_hz is not None:
        self._last_delivered_ns[edge] = self._monotonic_clock()

  def _warn_overflow(self, edge: StageEdge) -> None:
    now_ns = self._monotonic_clock()
    if self.on_overflow is not None and (
      edge not in self._last_warning_ns
      or now_ns - self._last_warning_ns[edge] >= 1_000_000_000
    ):
      self.on_overflow(edge, self.edge_counters[edge])
      self._last_warning_ns[edge] = now_ns

  def drain_ready(self, limit: int = 1) -> int:
    """Deliver ready latest evidence with smooth weighted fairness."""
    if not self.active:
      return 0
    delivered = 0
    for _ in range(limit):
      now = self._monotonic_clock()
      ready = [
        edge
        for edge in self._pending
        if edge.frequency_hz is None
        or edge not in self._last_delivered_ns
        or (
          now > self._last_delivered_ns[edge]
          and (now - self._last_delivered_ns[edge]) * edge.frequency_hz >= 1_000_000_000
        )
      ]
      if not ready:
        break
      total = sum(20 - edge.priority for edge in ready)
      for edge in ready:
        self._fair_credit[edge] = self._fair_credit.get(edge, 0) + 20 - edge.priority
      edge = max(
        ready,
        key=lambda item: (self._fair_credit[item], -self.composition.edges.index(item)),
      )
      self._fair_credit[edge] -= total
      value = self._pending.pop(edge)
      if self._clock() - value.observed_at > MAX_STAGE_EVENT_AGE:
        self.edge_counters[edge].gaps += 1
        self.diagnostics.append(
          StageDiagnostic("stale_event", edge.target, value.event_id)
        )
        continue
      if (edge.target, edge.input) in self._async_handlers:
        self._async_pending.append((edge, value))
      else:
        self._deliver(edge, value)
        delivered += 1
    return delivered

  async def drain_async(self, limit: int = 1, *, raise_on_error: bool = False) -> int:
    async with self._async_lock:
      delivered = self.drain_ready(limit)
      while self._async_pending and self.active:
        edge, value = self._async_pending.popleft()
        try:
          await self._async_handlers[edge.target, edge.input](value)
          if self.active:
            self._deliver(edge, value)
            delivered += 1
        except Exception:
          self.diagnostics.append(
            StageDiagnostic("owner_admission_failed", edge.target, value.event_id)
          )
          self.edge_counters[edge].overflow += 1
          self.edge_counters[edge].gaps += 1
          self._warn_overflow(edge)
          if raise_on_error:
            raise
      return delivered

  async def run_scheduler(self) -> None:
    """Flush capped latest evidence until owner shutdown."""
    while self.active:
      await self.drain_async()
      await asyncio.sleep(0.01)

  def publish(self, output: str, value: StageValue) -> int:
    """Deliver one value after source, kind, time, and deduplication checks."""
    if self._stopped or not self._started:
      raise RuntimeError("stage runtime is stopping")
    origin = self.composition.instances.get(value.source_node)
    if origin is None or origin.type.outputs.get(output) != value.kind:
      raise ValueError("value does not belong to the source output")
    if value.run_id != self.run_id or not value.event_id.strip():
      self.diagnostics.append(
        StageDiagnostic("stale_or_empty_event", value.source_node, value.event_id)
      )
      raise ValueError("stale run or empty event identity")
    now = self._clock()
    if (
      value.observed_at.tzinfo is None
      or value.received_at.tzinfo is None
      or now.tzinfo is None
      or value.observed_at > value.received_at
      or value.received_at > now
    ):
      self.diagnostics.append(
        StageDiagnostic("invalid_event_time", value.source_node, value.event_id)
      )
      raise ValueError("invalid or future observation time")
    if now - value.observed_at > MAX_STAGE_EVENT_AGE:
      self.diagnostics.append(
        StageDiagnostic("stale_event", value.source_node, value.event_id)
      )
      raise ValueError("stale stage observation")
    if value.kind == "resource":
      sample = value.payload.fields["sample"]
      if not isinstance(sample, Mapping) or any(
        not isinstance(sample.get(name), str) or not sample[name].strip()
        for name in ("text", "eos_robot_id")
      ):
        self.diagnostics.append(
          StageDiagnostic("malformed_resource", value.source_node, value.event_id)
        )
        raise ValueError("resource sample requires nonblank text and eos_robot_id")
    identity = (
      value.kind,
      value.source_node,
      value.observed_at,
      value.payload,
      value.backend,
    )
    if value.event_id in self._seen:
      if self._seen_values[value.event_id] != identity:
        self.diagnostics.append(
          StageDiagnostic("conflicting_event", value.source_node, value.event_id)
        )
        raise ValueError("conflicting duplicate stage event")
      self.diagnostics.append(
        StageDiagnostic("duplicate_event", value.source_node, value.event_id)
      )
      return 0
    routes = [
      edge
      for edge in self.composition.edges
      if edge.source == value.source_node and edge.output == output
    ]
    if any((edge.target, edge.input) not in self._handlers for edge in routes):
      raise RuntimeError("required stage owner is not bound")
    self._seen.add(value.event_id)
    self._seen_values[value.event_id] = identity
    self._seen_order.append(value.event_id)
    if len(self._seen_order) > 10000:
      evicted = self._seen_order.popleft()
      self._seen.remove(evicted)
      del self._seen_values[evicted]
    delivered = 0
    for edge in routes:
      if edge in self.edge_counters:
        if edge in self._pending:
          counters = self.edge_counters[edge]
          counters.gaps += 1
          counters.overflow += 1
          self.diagnostics.append(
            StageDiagnostic("evidence_overflow", edge.target, value.event_id)
          )
          self._warn_overflow(edge)
        self._pending[edge] = value
      else:
        self._deliver(edge, value)
        delivered += 1
    if self._auto_drain:
      delivered += self.drain_ready()
    if shared_events.valid_opaque_id(value.event_id):
      shared_events.emit(
        "egia_stage_transition",
        fields={"kind": value.kind},
        correlation_id=value.event_id,
      )
    return delivered

  async def publish_async(self, output: str, value: StageValue) -> int:
    """Deliver typed evidence and await its owning stage's admission action."""
    delivered = self.publish(output, value)
    if self._auto_drain:
      delivered += await self.drain_async(0, raise_on_error=True)
    return delivered

  def stop(self) -> None:
    """Close admission of further values before owner shutdown."""
    self._stopped = True
    for edge, value in self._pending.items():
      self.edge_counters[edge].gaps += 1
      self.diagnostics.append(
        StageDiagnostic("evidence_stopped", edge.target, value.event_id)
      )
    for edge, value in self._async_pending:
      self.edge_counters[edge].gaps += 1
      self.diagnostics.append(
        StageDiagnostic("evidence_stopped", edge.target, value.event_id)
      )
    self._pending.clear()
    self._async_pending.clear()

  async def stop_async(self) -> None:
    """Fence in-flight owner admission before stopping the stage runtime."""
    async with self._async_lock:
      self.stop()


def load_stage_runtime(etc_root: Path, run_id: str) -> StageRuntime | None:
  """Load the IsOS-rendered graph overlay; legacy deployments have no stages."""
  source = etc_root / "stages.json"
  if not source.exists():
    return None
  payload = json.loads(source.read_text(encoding="utf-8"))
  if (
    not isinstance(payload, dict)
    or not isinstance(payload.get("revision"), str)
    or re.fullmatch(r"[0-9a-f]{64}", payload["revision"]) is None
  ):
    raise ValueError("stages.json requires a lowercase SHA-256 source graph revision")
  if payload.get("version") == "0.2.0":
    # Keep the owner projection on disk; adapt only the process-local stage model.
    from egia.config_cli import _legacy_stage_graph

    payload = _legacy_stage_graph(payload) | {"revision": payload["revision"]}
  return StageRuntime(compose_stages(payload), run_id, auto_drain=False)
