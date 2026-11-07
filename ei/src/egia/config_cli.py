"""Side-effect-free IsOS configuration adapter for EGIA."""

from __future__ import annotations

import argparse
import copy
import json
import sys
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, ValidationError

from egia.lib.config import GatewayConfig, ServerConfig
from egia.reader_features import SETUP_FEATURES, product_version
from egia.stages import (
  FIXED_INPUTS,
  FIXED_OUTPUTS,
  OPTIONAL_INPUTS,
  SCHEDULED_EDGES,
  SETTING_DEFAULTS,
  STAGE_DATA_CLASSES,
  STAGE_TYPES,
  StageGraphError,
  compose_stages,
  stage_data_types,
)

SCHEMA_VERSION = 1
SETUP_SCHEMA_VERSION = 2
MAX_SETUP_INPUT_BYTES = 1024 * 1024
PROFILE_VERSION = "1.0.0"
SUPPORTED_GRAPH_VERSIONS = ("0.1.0", "1.0.0", "0.2.0")
SUPPORTED_NODE_TYPE_VERSIONS = ("0.1", "1.0", "0.2")
EI_STAGE_TYPES = {
  f"isos.ei_{stage.layer}.{stage.id.split('.')[2].split('@')[0]}@v0.2": stage
  for stage in STAGE_TYPES.values()
}
LEGACY_EGIA_TYPES = {
  "egia.common.source@v1.0": ("common", "data"),
  "egia.common.context@v1.0": ("common", "data"),
  "egia.common.planner@v1.0": ("common", "process"),
  "egia.common.guard@v1.0": ("common", "process"),
}
EI_LEGACY_TYPES = {
  f"isos.ei_common.{old.split('.')[2].split('@')[0]}@v0.2": (old, placement)
  for old, placement in LEGACY_EGIA_TYPES.items()
}
REQUIRED_CONTRACTS = [
  {"name": "island.eos.v1.GatewayService", "version": "1.0.0"},
  {"name": "island.eos.v2.CapabilityService", "version": "2.0.0"},
  {"name": "island.eos.v1.SkillService", "version": "1.0.0"},
]


class ManagedServer(ServerConfig):
  """Strictly validate the IsOS-editable EGIA listener fields."""

  model_config = ConfigDict(extra="forbid")


class ManagedValues(BaseModel):
  """Values IsOS may edit without owning EGIA's full TOML schema."""

  model_config = ConfigDict(extra="forbid")

  mode: Literal["isos"] = "isos"
  server: ManagedServer = Field(default_factory=ManagedServer)
  gateway: GatewayConfig = Field(
    default_factory=lambda: GatewayConfig(transport="tcp", host="127.0.0.1")
  )


class SetupDiagnostic(ValueError):
  """Carry a graph JSON pointer and stable error code to the setup envelope."""

  def __init__(self, path: str, code: str, message: str) -> None:
    super().__init__(message)
    self.path = path
    self.code = code


def _pointer_segment(value: str) -> str:
  return value.replace("~", "~0").replace("/", "~1")


def _legacy_stage_graph(graph: dict[str, Any]) -> dict[str, Any]:
  """Adapt a selected EI projection to the existing in-process stage model."""
  if graph.get("version") != "0.2.0":
    return graph
  layers = graph["cores"]["isos"]["layers"]
  converted: dict[str, Any] = {}
  for layer_id, layer in layers.items():
    converted[layer_id.removeprefix("ei_")] = copy.deepcopy(layer)
    for group in converted[layer_id.removeprefix("ei_")]["groups"].values():
      for node in group["nodes"].values():
        node_type = node["node_type"]
        node["node_type"] = (
          EI_STAGE_TYPES[node_type].id
          if node_type in EI_STAGE_TYPES
          else EI_LEGACY_TYPES[node_type][0]
        )

  def old_path(value: Any) -> Any:
    if isinstance(value, str) and value.startswith("isos/ei_"):
      parts = value.split("/")
      if len(parts) >= 4:
        return "/".join(("egia", parts[1].removeprefix("ei_"), *parts[2:]))
    if isinstance(value, dict):
      return {key: old_path(item) for key, item in value.items()}
    if isinstance(value, list):
      return [old_path(item) for item in value]
    return value

  return old_path(
    {
      "version": "0.1.0",
      "robot": graph.get("robot", {}),
      "cores": {"egia": {"layers": converted}},
    }
  )


def _validate_egia_graph(graph: Any, *, validate_edges: bool = True) -> None:
  """Read exact identities; compose wiring for Validate and Render only."""
  if (
    not isinstance(graph, dict) or graph.get("version") not in SUPPORTED_GRAPH_VERSIONS
  ):
    raise SetupDiagnostic(
      "/graph/version",
      "unsupported_graph_version",
      "EI supports graph 0.2.0 and legacy 0.1.0/1.0.0",
    )
  if graph["version"] == "0.2.0":
    _validate_ei_projection(graph, validate_edges=validate_edges)
    return
  cores = graph.get("cores")
  if not isinstance(cores, dict) or not isinstance(cores.get("egia"), dict):
    raise SetupDiagnostic(
      "/graph/cores/egia", "invalid_graph", "setup graph must contain the egia core"
    )
  layers = cores["egia"].get("layers")
  if not isinstance(layers, dict):
    raise SetupDiagnostic(
      "/graph/cores/egia/layers", "invalid_graph", "EGIA layers must be an object"
    )
  for layer_id, layer in layers.items():
    layer_path = f"/graph/cores/egia/layers/{_pointer_segment(layer_id)}"
    if not isinstance(layer, dict) or not isinstance(layer.get("groups"), dict):
      raise SetupDiagnostic(
        f"{layer_path}/groups", "invalid_graph", "EGIA groups must be an object"
      )
    for group_id, group in layer["groups"].items():
      group_path = f"{layer_path}/groups/{_pointer_segment(group_id)}"
      if not isinstance(group, dict) or not isinstance(group.get("nodes"), dict):
        raise SetupDiagnostic(
          f"{group_path}/nodes", "invalid_graph", "EGIA nodes must be an object"
        )
      for node_id, node in group["nodes"].items():
        type_path = f"{group_path}/nodes/{_pointer_segment(node_id)}/node_type"
        node_type = node.get("node_type") if isinstance(node, dict) else None
        placement = (
          LEGACY_EGIA_TYPES.get(node_type) if isinstance(node_type, str) else None
        )
        if node_type in STAGE_TYPES:
          stage = STAGE_TYPES[node_type]
          placement = (stage.layer, stage.group)
        if placement is None:
          raise SetupDiagnostic(
            type_path,
            "unsupported_node_type_version",
            f"unsupported EGIA type {node_type!r}; supported: {', '.join(sorted((*STAGE_TYPES, *LEGACY_EGIA_TYPES)))}",
          )
        if placement != (layer_id, group_id):
          raise SetupDiagnostic(
            type_path,
            "invalid_node_placement",
            f"{node_type} belongs at egia/{placement[0]}/{placement[1]}",
          )
  if validate_edges:
    try:
      compose_stages(graph)
    except StageGraphError as exc:
      raise SetupDiagnostic(exc.path, exc.code, str(exc)) from exc


def _validate_ei_projection(graph: dict[str, Any], *, validate_edges: bool) -> None:
  """Require exact IsOS EI layers and v0.2 types before composing local stages."""
  cores = graph.get("cores")
  if (
    not isinstance(cores, dict)
    or set(cores) != {"isos"}
    or not isinstance(cores["isos"], dict)
  ):
    raise SetupDiagnostic(
      "/graph/cores/isos", "invalid_graph", "EI projection requires only the isos core"
    )
  layers = cores["isos"].get("layers")
  if not isinstance(layers, dict):
    raise SetupDiagnostic(
      "/graph/cores/isos/layers", "invalid_graph", "EI layers must be an object"
    )
  for layer_id, layer in layers.items():
    layer_path = f"/graph/cores/isos/layers/{_pointer_segment(layer_id)}"
    if (
      not layer_id.startswith("ei_")
      or not isinstance(layer, dict)
      or not isinstance(layer.get("groups"), dict)
    ):
      raise SetupDiagnostic(
        layer_path, "invalid_graph", "EI projection requires ei_* layers with groups"
      )
    for group_id, group in layer["groups"].items():
      group_path = f"{layer_path}/groups/{_pointer_segment(group_id)}"
      if not isinstance(group, dict) or not isinstance(group.get("nodes"), dict):
        raise SetupDiagnostic(
          f"{group_path}/nodes", "invalid_graph", "EI group nodes must be an object"
        )
      for node_id, node in group["nodes"].items():
        type_path = f"{group_path}/nodes/{_pointer_segment(node_id)}/node_type"
        node_type = node.get("node_type") if isinstance(node, dict) else None
        stage = EI_STAGE_TYPES.get(node_type) if isinstance(node_type, str) else None
        legacy = EI_LEGACY_TYPES.get(node_type) if isinstance(node_type, str) else None
        if stage is None and legacy is None:
          raise SetupDiagnostic(
            type_path,
            "unsupported_node_type_version",
            f"unsupported EI type {node_type!r}",
          )
        if stage is not None:
          placement = (f"ei_{stage.layer}", stage.group)
        else:
          assert legacy is not None
          placement = ("ei_common", legacy[1][1])
        if (layer_id, group_id) != placement:
          raise SetupDiagnostic(
            type_path,
            "invalid_node_placement",
            f"{node_type} belongs at isos/{placement[0]}/{placement[1]}",
          )
  if validate_edges:
    try:
      compose_stages(_legacy_stage_graph(graph))
    except StageGraphError as exc:
      path = exc.path.replace(
        "/graph/cores/egia/layers/", "/graph/cores/isos/layers/ei_"
      )
      raise SetupDiagnostic(path, exc.code, str(exc)) from exc


def _reply(
  data: Any = None, diagnostics: list[dict[str, str]] | None = None
) -> dict[str, Any]:
  issues = diagnostics or []
  return {
    "schema_version": SCHEMA_VERSION,
    "ok": not issues,
    "data": data,
    "diagnostics": issues,
  }


def _read_values() -> ManagedValues:
  try:
    payload = json.load(sys.stdin)
  except (ValueError, UnicodeDecodeError) as exc:
    raise ValueError(f"invalid JSON input: {exc}") from exc
  if not isinstance(payload, dict):
    raise TypeError("input must be a JSON object")
  if payload.get("schema_version") != SCHEMA_VERSION:
    raise ValueError(f"schema_version must be {SCHEMA_VERSION}")
  return ManagedValues.model_validate(payload.get("values", {}))


def _read_setup_request() -> dict[str, Any]:
  """Read one bounded graph setup request without touching runtime state."""
  raw = (
    sys.stdin.buffer.read(MAX_SETUP_INPUT_BYTES + 1)
    if hasattr(sys.stdin, "buffer")
    else sys.stdin.read(MAX_SETUP_INPUT_BYTES + 1).encode("utf-8")
  )
  if len(raw) > MAX_SETUP_INPUT_BYTES:
    raise ValueError("setup request exceeds 1 MiB")
  try:
    request = json.loads(raw)
  except (ValueError, UnicodeDecodeError) as exc:
    raise ValueError(f"invalid JSON input: {exc}") from exc
  if not isinstance(request, dict):
    raise TypeError("setup request must be an object")
  if request.get("op") == "identity":
    if set(request) != {"schema_version", "op", "core"}:
      raise ValueError("identity request fields must be schema_version, op, core")
    if request["schema_version"] != SETUP_SCHEMA_VERSION:
      raise ValueError(f"setup schema_version must be {SETUP_SCHEMA_VERSION}")
    if request["core"] != "egia":
      raise ValueError("setup core must be egia")
    return request
  if set(request) != {
    "schema_version",
    "op",
    "core",
    "revision",
    "graph",
    "endpoints",
    "packages",
  }:
    raise ValueError(
      "setup request fields must be schema_version, op, core, revision, graph, endpoints, packages"
    )
  if request["schema_version"] != SETUP_SCHEMA_VERSION:
    raise ValueError(f"setup schema_version must be {SETUP_SCHEMA_VERSION}")
  if request["op"] not in {"catalog", "validate", "render"}:
    raise ValueError("setup op must be catalog, validate, or render")
  if request["core"] != "egia":
    raise ValueError("setup core must be egia")
  if (
    not isinstance(request["revision"], str) or not 1 <= len(request["revision"]) <= 128
  ):
    raise ValueError("setup revision must be a 1-128 character string")
  graph = request["graph"]
  _validate_egia_graph(graph, validate_edges=request["op"] != "catalog")
  if not isinstance(request["endpoints"], dict):
    raise TypeError("setup endpoints must be an object")
  if not isinstance(request["packages"], list) or len(request["packages"]) > 256:
    raise ValueError("setup packages must be an array of at most 256 entries")
  return request


def _setup_values(request: dict[str, Any]) -> ManagedValues:
  """Project EGIA settings from IsOS-resolved endpoints and its graph core."""
  endpoints = request["endpoints"]
  northbound = endpoints.get("egia_northbound", {})
  gateway = endpoints.get("eos_gateway")
  if gateway is None:
    raise ValueError(
      "endpoints.eos_gateway is required for EGIA; EOS HTTP control is not a gRPC gateway"
    )
  if not isinstance(northbound, dict) or not isinstance(gateway, dict):
    raise TypeError("egia_northbound and eos_gateway endpoints must be objects")
  if gateway.get("transport", "tcp") not in {"tcp", "uds"}:
    raise ValueError("endpoints.eos_gateway.transport must be tcp or uds")
  server = {
    "host": northbound.get("host", "127.0.0.1"),
    "port": northbound.get("port", 16060),
  }
  if gateway.get("transport", "tcp") == "uds":
    gateway_values = {"transport": "uds", "path": gateway.get("path")}
  else:
    gateway_values = {
      "transport": "tcp",
      "host": gateway.get("host", "127.0.0.1"),
      "grpc_port": gateway.get("port", 16200),
    }
  return ManagedValues.model_validate({"server": server, "gateway": gateway_values})


def _setup_reply(request: dict[str, Any]) -> dict[str, Any]:
  op = request["op"]
  if op == "identity":
    return {
      "schema_version": SETUP_SCHEMA_VERSION,
      "ok": True,
      "diagnostics": [],
      "files": [],
      "identity": {
        "core": "egia",
        "product_version": product_version(),
        "graph_version": "0.2.0",
        "node_type_version": "0.2",
        "supported_graph_versions": list(SUPPORTED_GRAPH_VERSIONS),
        "supported_node_type_versions": list(SUPPORTED_NODE_TYPE_VERSIONS),
        "supported_node_types": sorted(
          (*LEGACY_EGIA_TYPES, *STAGE_TYPES, *EI_LEGACY_TYPES, *EI_STAGE_TYPES)
        ),
        "features": list(SETUP_FEATURES),
        "setup_schema_version": SETUP_SCHEMA_VERSION,
      },
    }
  if op == "catalog":
    return {
      "schema_version": SETUP_SCHEMA_VERSION,
      "ok": True,
      "diagnostics": [],
      "files": [],
      "catalog": {
        "profiles": [_profile()],
        "stage_types": [
          _stage_descriptor(stage, new=request["graph"]["version"] == "0.2.0")
          for stage in STAGE_TYPES.values()
        ],
        "routed_edges": [
          {
            "source_type": _ei_type(source_type)
            if request["graph"]["version"] == "0.2.0"
            else source_type,
            "output": output,
            "target_type": _ei_type(target_type)
            if request["graph"]["version"] == "0.2.0"
            else target_type,
            "input": input_name,
            "controls": ["priority", "frequency_hz"],
          }
          for source_type, output, target_type, input_name in sorted(SCHEDULED_EDGES)
        ],
        "data_types": stage_data_types(),
        "defaults": ManagedValues().model_dump(mode="json"),
      },
    }
  values = _setup_values(request) if op != "catalog" else None
  composition = compose_stages(_legacy_stage_graph(request["graph"]))
  if values is not None:
    for instance in composition.instances.values():
      if instance.type.group != "link":
        continue
      saved = instance.settings["gateway"]
      resolved = request["endpoints"]["eos_gateway"]
      if not isinstance(saved, dict) or (
        saved.get("transport", "tcp") != resolved.get("transport", "tcp")
        or (
          (saved.get("path") != resolved.get("path"))
          if resolved.get("transport", "tcp") == "uds"
          else (
            saved.get("host") != resolved.get("host")
            or saved.get("port") != resolved.get("port")
          )
        )
      ):
        raise SetupDiagnostic(
          f"/graph/cores/{'isos/layers/ei_io' if request['graph']['version'] == '0.2.0' else 'egia/layers/io'}/groups/link/nodes/{_pointer_segment(instance.path.rsplit('/', 1)[-1])}/settings/gateway",
          "gateway_mismatch",
          "link gateway must match the IsOS-verified EOS gRPC endpoint",
        )
  stage_files: list[dict[str, str]] = []
  single_core = request["graph"]["version"] == "0.2.0"
  if op == "render" and composition.instances:
    stage_files = [
      {
        "path": "ei/stages.json" if single_core else "egia/stages.json",
        "content": json.dumps(
          {
            "revision": request["revision"],
            "version": request["graph"]["version"],
            **({"robot": request["graph"]["robot"]} if single_core else {}),
            "cores": request["graph"]["cores"],
          },
          ensure_ascii=False,
          sort_keys=True,
          separators=(",", ":"),
        )
        + "\n",
      }
    ]
  overlays = (
    _render(values, composition)["files"]
    if op == "render" and values is not None
    else []
  )
  if single_core:
    overlays = [
      {**file, "path": file["path"].replace("egia/", "ei/", 1)} for file in overlays
    ]
  return {
    "schema_version": SETUP_SCHEMA_VERSION,
    "ok": True,
    "diagnostics": [],
    "files": overlays + stage_files,
  }


def _toml_string(value: str) -> str:
  return json.dumps(value, ensure_ascii=False)


def _render(values: ManagedValues, composition: Any = None) -> dict[str, Any]:
  gateway = values.gateway
  server = values.server
  server_content = (
    "# Managed by IsOS; other EGIA settings layer from factory configuration.\n"
    "[server]\n"
    f"host = {_toml_string(server.host)}\n"
    f"port = {server.port}\n"
    f"status_frequency_hz = {server.status_frequency_hz}\n"
  )
  if composition is not None and composition.instances:
    by_group = {
      instance.type.group: instance.settings
      for instance in composition.instances.values()
      if instance.type.group != "link"
    }
    for section in ("perception", "decision", "monitor"):
      scalar = {
        name: value
        for name, value in by_group.get(section, {}).items()
        if "." not in name
      }
      if section == "decision" and "mission_capacity" in by_group.get("missions", {}):
        scalar["mission_capacity"] = by_group["missions"]["mission_capacity"]
      if scalar:
        server_content += f"\n[{section}]\n" + "".join(
          f"{name} = {json.dumps(value)}\n" for name, value in scalar.items()
        )
    decision_settings = by_group.get("decision", {})
    for lane in ("controller", "reasoner"):
      name = f"{lane}.model"
      if name in decision_settings:
        server_content += (
          f"\n[decision.{lane}]\nmodel = {_toml_string(decision_settings[name])}\n"
        )
  gateway_content = "[modes.isos.gateway]\n"
  if gateway.transport == "uds":
    gateway_content += f'transport = "uds"\npath = {_toml_string(gateway.path or "")}\n'
  else:
    gateway_content += (
      'transport = "tcp"\n'
      f"host = {_toml_string(gateway.host or '')}\n"
      f"grpc_port = {gateway.grpc_port}\n"
    )
  if composition is not None and composition.instances:
    links = [
      instance
      for instance in composition.instances.values()
      if instance.type.group == "link"
    ]
    if len(links) != 1:
      raise ValueError("managed stage render currently requires exactly one EOS link")
    alias = links[0].settings["backend_alias"]
    gateway_content += f"\n[[modes.isos.backends]]\nid = {_toml_string(alias)}\n"
    if gateway.transport == "uds":
      gateway_content += f'gateway = {{ transport = "uds", path = {_toml_string(gateway.path or "")} }}\n'
    else:
      gateway_content += f'gateway = {{ transport = "tcp", host = {_toml_string(gateway.host or "")}, grpc_port = {gateway.grpc_port} }}\n'
  return {
    "files": [
      {"path": "egia/egia.toml", "content": server_content},
      {"path": "egia/modes.toml", "content": 'default = "isos"\n'},
      {"path": "egia/profiles/isos.toml", "content": gateway_content},
    ]
  }


def _profile() -> dict[str, Any]:
  settings_fields = [
    {
      "name": "server.host",
      "type": "string",
      "label": "Listener host",
      "description": "Address for the EGIA northbound WebSocket listener.",
      "default": "127.0.0.1",
      "required": True,
    },
    {
      "name": "server.port",
      "type": "integer",
      "label": "Listener port",
      "description": "Port for the EGIA northbound WebSocket listener.",
      "default": 16060,
      "min": 1,
      "max": 65535,
      "required": True,
    },
    {
      "name": "server.status_frequency_hz",
      "type": "number",
      "label": "Status frequency (Hz)",
      "description": "Rate of health status updates; 0.5 sends one update every two seconds.",
      "default": 0.5,
      "exclusive_min": 0,
      "required": True,
    },
    {
      "name": "gateway.transport",
      "type": "enum",
      "label": "EOS transport",
      "description": "Connection type for the EOS gateway.",
      "default": "tcp",
      "options": ["uds", "tcp"],
      "required": True,
    },
    {
      "name": "gateway.host",
      "type": "string",
      "label": "EOS host",
      "description": "EOS gateway address when using TCP; omit for a Unix socket.",
      "default": "127.0.0.1",
    },
    {
      "name": "gateway.grpc_port",
      "type": "integer",
      "label": "EOS gRPC port",
      "description": "EOS gateway port when using TCP.",
      "default": 16200,
      "min": 1,
      "max": 65535,
    },
    {
      "name": "gateway.path",
      "type": "absolute_path",
      "label": "EOS socket path",
      "description": "Absolute Unix socket path when using UDS; omit for TCP.",
    },
  ]
  return {
    "id": "egia.agent",
    "version": PROFILE_VERSION,
    "template": "process",
    "inputs": [
      {
        "name": "eos",
        "type": "grpc",
        "required": True,
        "contracts": REQUIRED_CONTRACTS,
        "transports": ["uds", "tcp"],
      }
    ],
    "outputs": [],
    "settings_fields": settings_fields,
    "fields": {
      "server.host": {"type": "string", "default": "127.0.0.1"},
      "server.port": {"type": "integer", "default": 16060},
      "gateway.transport": {"type": "enum", "values": ["uds", "tcp"]},
      "gateway.path": {"type": "absolute_path"},
    },
    "provides": [],
    "requires": REQUIRED_CONTRACTS,
    "readiness": {
      "kind": "tcp",
      "host_field": "server.host",
      "port_field": "server.port",
    },
  }


def _ei_type(old_type: str) -> str:
  stage = STAGE_TYPES[old_type]
  return f"isos.ei_{stage.layer}.{old_type.split('.')[2].split('@')[0]}@v0.2"


def _stage_descriptor(stage: Any, *, new: bool = False) -> dict[str, Any]:
  """Project the exact in-process stage contract for IsOS catalog consumers."""
  return {
    "id": _ei_type(stage.id) if new else stage.id,
    "core": "isos" if new else "egia",
    "layer": f"ei_{stage.layer}" if new else stage.layer,
    "group": stage.group,
    "description": stage.description,
    "inputs": [
      {
        "name": name,
        "data_type": f"egia.stage.{data_type}@v0.1",
        "transport": "in_process",
        "data_class": STAGE_DATA_CLASSES[data_type],
        "cardinality": "one",
        "fixed": (stage.id, name) in FIXED_INPUTS,
        "required": (stage.id, name) not in OPTIONAL_INPUTS | FIXED_INPUTS,
      }
      for name, data_type in stage.inputs.items()
    ],
    "outputs": [
      {
        "name": name,
        "data_type": f"egia.stage.{data_type}@v0.1",
        "transport": "in_process",
        "data_class": STAGE_DATA_CLASSES[data_type],
        "fixed": (stage.id, name) in FIXED_OUTPUTS,
      }
      for name, data_type in stage.outputs.items()
    ],
    "settings_fields": [_stage_setting_descriptor(name) for name in stage.settings],
  }


def _stage_setting_descriptor(name: str) -> dict[str, Any]:
  """Describe one graph-editable stage setting with its owner validation."""
  label = name.replace("_", " ").replace(".", " ").capitalize()
  result: dict[str, Any] = {
    "name": name,
    "label": label,
    "description": f"EGIA-owned {label.lower()} setting.",
    "required": name in {"backend_alias", "gateway"},
  }
  if name in {"backend_alias", "controller.model", "reasoner.model"}:
    result["type"] = "string"
  elif name == "gateway":
    result["type"] = "endpoint"
  elif name.endswith("_s"):
    result.update(type="number", exclusive_min=0)
  else:
    result.update(type="integer", min=1)
    if name in {"capacity", "history_per_kind"}:
      result["max"] = 10000
    elif name == "mission_capacity":
      result["max"] = 1024
  if name in SETTING_DEFAULTS:
    result["default"] = SETTING_DEFAULTS[name]
  return result


def main(argv: list[str] | None = None) -> int:
  """Emit one versioned JSON reply; never create files or start runtime services."""
  public_name = "ei-config" if __package__ == "ei" else "egia-config"
  parser = argparse.ArgumentParser(prog=public_name)
  parser.add_argument(
    "operation",
    choices=["describe", "defaults", "profiles", "render", "validate", "setup"],
  )
  parser.add_argument(
    "--json", action="store_true", help="JSON is the only output format"
  )
  operation = parser.parse_args(argv).operation
  try:
    if operation == "setup":
      response = _setup_reply(_read_setup_request())
    elif operation == "describe":
      response = _reply(
        {
          "component": "egia",
          "version": product_version(),
          "provides": [],
          "requires": REQUIRED_CONTRACTS,
          "profile_version": PROFILE_VERSION,
        }
      )
    elif operation == "defaults":
      response = _reply(ManagedValues().model_dump(mode="json"))
    elif operation == "profiles":
      response = _reply([_profile()])
    else:
      values = _read_values()
      response = _reply(
        _render(values)
        if operation == "render"
        else {"values": values.model_dump(mode="json")}
      )
  except SetupDiagnostic as exc:
    response = {
      "schema_version": SETUP_SCHEMA_VERSION,
      "ok": False,
      "diagnostics": [{"path": exc.path, "code": exc.code, "message": str(exc)}],
      "files": [],
    }
  except ValidationError as exc:
    diagnostics = [
      {
        "path": ("endpoints." if operation == "setup" else "values.")
        + ".".join(str(part) for part in item["loc"]),
        "code": str(item["type"]),
        "message": str(item["msg"]),
      }
      for item in exc.errors()
    ]
    response = (
      {
        "schema_version": SETUP_SCHEMA_VERSION,
        "ok": False,
        "diagnostics": diagnostics,
        "files": [],
      }
      if operation == "setup"
      else _reply(diagnostics=diagnostics)
    )
  except (TypeError, ValueError) as exc:
    diagnostics = [{"path": "$", "code": "invalid_input", "message": str(exc)}]
    response = (
      {
        "schema_version": SETUP_SCHEMA_VERSION,
        "ok": False,
        "diagnostics": diagnostics,
        "files": [],
      }
      if operation == "setup"
      else _reply(diagnostics=diagnostics)
    )
  json.dump(response, sys.stdout, ensure_ascii=False, separators=(",", ":"))
  sys.stdout.write("\n")
  return 0 if response["ok"] else 2


if __name__ == "__main__":
  raise SystemExit(main())
