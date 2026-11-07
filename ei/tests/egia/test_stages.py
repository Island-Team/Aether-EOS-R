"""Graph-owned stage identity and governed local delivery tests."""

from __future__ import annotations

import asyncio
import json
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

import pytest

from egia.config_cli import SetupDiagnostic, _setup_reply, _validate_egia_graph
from egia.stages import (
  MAX_STAGE_EVENT_AGE,
  STAGE_TYPES,
  StageGraphError,
  StagePayload,
  StageRuntime,
  StageValue,
  compose_stages,
  load_stage_runtime,
)

_FIELDS = {
  "discovery": {"backend": "robot-a", "generation": 1},
  "observations": {"items": []},
  "snapshot": {"text": ""},
  "selected": {"mission_id": "m1", "revision": 1},
  "proposal": {
    "backend": "robot-a",
    "device": "arm",
    "primitive": "move",
    "version": "1",
    "arguments": {},
  },
  "admission": {
    "backend": "robot-a",
    "device": "arm",
    "primitive": "move",
    "version": "1",
    "arguments": {},
  },
  "status": {"state": "unavailable", "episode": 0},
}


def _value(
  kind: str, event_id: str, run_id: str, source: str, when: datetime
) -> StageValue:
  return StageValue(
    kind,
    event_id,
    run_id,
    source,
    when,
    StagePayload(kind, _FIELDS[kind]),
    received_at=when,
  )


def _graph() -> dict:
  nodes: dict[str, dict] = {}
  for spec in STAGE_TYPES.values():
    path = f"egia/{spec.layer}/{spec.group}/{spec.id.split('.')[2].split('@')[0]}"
    node_id = path.rsplit("/", 1)[-1]
    settings = (
      {
        "backend_alias": "robot-a",
        "gateway": {"transport": "tcp", "host": "127.0.0.1", "port": 16022},
      }
      if spec.group == "link"
      else {}
    )
    nodes[path] = {
      "id": node_id,
      "description": spec.description,
      "node_type": spec.id,
      "settings": settings,
      "inputs": {},
    }

  def wire(target: str, input_name: str, source: str, output: str) -> None:
    nodes[target]["inputs"][input_name] = {"node": source, "output": output}

  paths = {
    spec.group: path
    for path, node in nodes.items()
    for spec in STAGE_TYPES.values()
    if node["node_type"] == spec.id
  }
  wire(paths["perception"], "resource", paths["link"], "resource")
  wire(paths["perception"], "result", paths["link"], "result")
  wire(paths["context"], "observations", paths["perception"], "observations")
  wire(paths["decision"], "selected", paths["missions"], "selected")
  wire(paths["decision"], "snapshot", paths["context"], "snapshot")
  wire(paths["safety"], "proposal", paths["decision"], "proposal")
  wire(paths["monitor"], "health", paths["link"], "health")
  layers: dict = {}
  for path, node in nodes.items():
    _, layer, group, node_id = path.split("/")
    layers.setdefault(layer, {"groups": {}})["groups"].setdefault(group, {"nodes": {}})[
      "nodes"
    ][node_id] = node
  return {"version": "0.1.0", "robot": {}, "cores": {"egia": {"layers": layers}}}


def _single_core_ei_graph() -> dict:
  """Project the seven published stages into the IsOS v0.2 namespace."""
  legacy = _graph()
  layers = {}
  for layer_id, layer in legacy["cores"]["egia"]["layers"].items():
    new_layer = json.loads(json.dumps(layer))
    for group in new_layer["groups"].values():
      for node in group["nodes"].values():
        old = node["node_type"]
        node["node_type"] = f"isos.ei_{layer_id}.{old.split('.')[2].split('@')[0]}@v0.2"
        for source in node["inputs"].values():
          source["node"] = source["node"].replace("egia/", "isos/ei_", 1)
    layers[f"ei_{layer_id}"] = new_layer
  return {"version": "0.2.0", "robot": {}, "cores": {"isos": {"layers": layers}}}


def test_single_core_projection_renders_revision_bound_ei_stages(
  tmp_path: Path,
) -> None:
  graph = _single_core_ei_graph()
  original = json.dumps(graph, sort_keys=True)
  request = {
    "schema_version": 2,
    "op": "render",
    "core": "egia",
    "revision": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "graph": graph,
    "endpoints": {
      "eos_gateway": {"transport": "tcp", "host": "127.0.0.1", "port": 16022}
    },
    "packages": [],
  }
  _validate_egia_graph(graph)
  files = _setup_reply(request)["files"]
  assert {item["path"] for item in files} == {
    "ei/egia.toml",
    "ei/modes.toml",
    "ei/profiles/isos.toml",
    "ei/stages.json",
  }
  stages = next(item for item in files if item["path"] == "ei/stages.json")
  payload = json.loads(stages["content"])
  assert (
    payload["revision"]
    == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
  )
  assert payload["version"] == "0.2.0"
  assert payload["robot"] == graph["robot"]
  assert payload["cores"] == graph["cores"]
  etc = tmp_path / "ei"
  etc.mkdir()
  (etc / "stages.json").write_text(stages["content"], encoding="utf-8")
  runtime = load_stage_runtime(etc, "run-1")
  assert runtime is not None
  assert len(runtime.composition.instances) == 7
  assert runtime.source_revision == request["revision"]
  assert json.dumps(graph, sort_keys=True) == original


def test_single_core_projection_rejects_unknown_type_at_exact_path() -> None:
  graph = _single_core_ei_graph()
  node = graph["cores"]["isos"]["layers"]["ei_io"]["groups"]["link"]["nodes"]["gateway"]
  node["node_type"] = "isos.ei_io.future@v0.2"
  with pytest.raises(SetupDiagnostic) as error:
    _validate_egia_graph(graph)
  assert (
    error.value.path
    == "/graph/cores/isos/layers/ei_io/groups/link/nodes/gateway/node_type"
  )
  assert error.value.code == "unsupported_node_type_version"


def test_composes_exact_seven_stage_instances() -> None:
  composition = compose_stages(_graph())
  assert len(composition.instances) == 7
  assert len(composition.edges) == 7
  assert all(instance.description for instance in composition.instances.values())


def test_stage_catalog_owns_exact_port_schemas_and_fixed_routes() -> None:
  catalog = _setup_reply(_setup(_graph(), "catalog"))["catalog"]
  assert "types" not in catalog
  assert {item["id"] for item in catalog["stage_types"]} == set(STAGE_TYPES)
  assert catalog["routed_edges"] == [
    {
      "source_type": "egia.link.gateway@v0.1",
      "output": "resource",
      "target_type": "egia.perception.admission@v0.1",
      "input": "resource",
      "controls": ["priority", "frequency_hz"],
    },
    {
      "source_type": "egia.perception.admission@v0.1",
      "output": "observations",
      "target_type": "egia.context.builder@v0.1",
      "input": "observations",
      "controls": ["priority", "frequency_hz"],
    },
  ]
  schemas = {item["id"]: item["schema"] for item in catalog["data_types"]}
  assert len(schemas) == 23
  assert schemas["egia.stage.resource@v0.1"]["properties"]["sample"]["required"] == [
    "text",
    "eos_robot_id",
  ]
  for stage in catalog["stage_types"]:
    for port in [*stage["inputs"], *stage["outputs"]]:
      assert port["data_type"] in schemas
      assert port["transport"] == "in_process"
      assert port["data_class"]
      schema = schemas[port["data_type"]]
      assert schema["type"] == "object"
      assert schema["required"]
      assert set(schema["required"]) == set(schema["properties"])
    assert all(port["cardinality"] == "one" for port in stage["inputs"])
  by_id = {stage["id"]: stage for stage in catalog["stage_types"]}
  link = by_id["egia.link.gateway@v0.1"]
  assert next(p for p in link["inputs"] if p["name"] == "admitted")["fixed"]
  assert next(p for p in link["inputs"] if p["name"] == "emergency_stop")["fixed"]
  assert (
    next(p for p in link["outputs"] if p["name"] == "result")["data_class"]
    == "feedback"
  )


def _setup(graph: dict, op: str = "render") -> dict:
  return {
    "schema_version": 1,
    "op": op,
    "core": "egia",
    "revision": "a" * 64,
    "graph": graph,
    "endpoints": {
      "eos_gateway": {"transport": "tcp", "host": "127.0.0.1", "port": 16022}
    },
    "packages": [],
  }


def test_catalog_defers_edge_diagnostics_to_graph_admission() -> None:
  graph = _graph()
  _, context = next((path, node) for path, node in _nodes(graph) if "/context/" in path)
  context["inputs"]["observations"]["priority"] = -21
  _validate_egia_graph(graph, validate_edges=False)
  assert len(_setup_reply(_setup(graph, "catalog"))["catalog"]["stage_types"]) == 7
  with pytest.raises(SetupDiagnostic, match="priority"):
    _validate_egia_graph(graph)


def test_only_exact_replaceable_evidence_routes_accept_qos() -> None:
  graph = _graph()
  nodes = {node["node_type"].split(".")[1]: node for _, node in _nodes(graph)}
  nodes["perception"]["inputs"]["resource"].update(priority=-20, frequency_hz=2.5)
  nodes["context"]["inputs"]["observations"].update(priority=19, frequency_hz=1)
  composition = compose_stages(graph)
  assert {
    (edge.output, edge.input, edge.priority, edge.frequency_hz)
    for edge in composition.edges
    if edge.frequency_hz is not None
  } == {
    ("resource", "resource", -20, 2.5),
    ("observations", "observations", 19, 1),
  }
  for target, input_name in (
    ("perception", "result"),
    ("monitor", "health"),
    ("decision", "snapshot"),
  ):
    rejected = _graph()
    node = next(
      node for _, node in _nodes(rejected) if node["node_type"].split(".")[1] == target
    )
    node["inputs"][input_name]["frequency_hz"] = 1
    with pytest.raises(StageGraphError, match="no edge QoS") as exc:
      compose_stages(rejected)
    assert exc.value.code == "unsupported_edge_control"


def test_single_source_array_preserves_cardinality_one() -> None:
  graph = _graph()
  context = next(
    node for _, node in _nodes(graph) if node["node_type"].split(".")[1] == "context"
  )
  source = context["inputs"]["observations"]
  context["inputs"]["observations"] = [source]
  assert len(compose_stages(graph).edges) == 7
  context["inputs"]["observations"] = [source, source]
  with pytest.raises(StageGraphError) as exc:
    compose_stages(graph)
  assert exc.value.code == "invalid_cardinality"


@pytest.mark.parametrize(
  "field,value,code,suffix",
  [
    ("priority", -21, "invalid_edge_priority", "/priority"),
    ("frequency_hz", 0, "invalid_edge_frequency", "/frequency_hz"),
    ("node", "missing", "unknown_source", "/node"),
    ("output", "missing", "unknown_output", "/output"),
  ],
)
def test_single_source_array_reports_exact_nested_pointer(
  field: str, value: object, code: str, suffix: str
) -> None:
  graph = _graph()
  context = next(
    node for _, node in _nodes(graph) if node["node_type"].split(".")[1] == "context"
  )
  source = context["inputs"]["observations"]
  source[field] = value
  context["inputs"]["observations"] = [source]
  with pytest.raises(StageGraphError) as exc:
    compose_stages(graph)
  assert exc.value.code == code
  assert exc.value.path == (
    "/graph/cores/egia/layers/cognition/groups/context/nodes/builder"
    f"/inputs/observations/0{suffix}"
  )


def test_single_source_array_unsupported_qos_reports_edge_pointer() -> None:
  graph = _graph()
  decision = next(
    node for _, node in _nodes(graph) if node["node_type"].split(".")[1] == "decision"
  )
  source = decision["inputs"]["snapshot"]
  source["priority"] = 1
  decision["inputs"]["snapshot"] = [source]
  with pytest.raises(StageGraphError) as exc:
    compose_stages(graph)
  assert exc.value.code == "unsupported_edge_control"
  assert exc.value.path == (
    "/graph/cores/egia/layers/cognition/groups/decision/nodes/planner/inputs/snapshot/0"
  )


@pytest.mark.parametrize("frequency", [0, -1, float("inf"), float("nan"), True, "2"])
def test_invalid_evidence_frequency_rejected_at_field(frequency: object) -> None:
  graph = _graph()
  node = next(
    node for _, node in _nodes(graph) if node["node_type"].split(".")[1] == "context"
  )
  node["inputs"]["observations"]["frequency_hz"] = frequency
  with pytest.raises(StageGraphError) as exc:
    compose_stages(graph)
  assert exc.value.code == "invalid_edge_frequency"
  assert exc.value.path.endswith("/frequency_hz")


def test_managed_render_loads_exact_stage_graph(tmp_path) -> None:
  response = _setup_reply(_setup(_graph()))
  assert response["ok"]
  assert [item["path"] for item in response["files"]][-1] == "egia/stages.json"
  root = tmp_path / "egia"
  root.mkdir()
  (root / "stages.json").write_text(response["files"][-1]["content"], encoding="utf-8")
  runtime = load_stage_runtime(root, "run-1")
  assert runtime is not None
  assert len(runtime.composition.instances) == 7
  assert json.loads(response["files"][-1]["content"])["revision"] == "a" * 64
  assert runtime.source_revision == "a" * 64


@pytest.mark.parametrize("revision", [None, "", "sha256:test", "A" * 64])
def test_managed_stage_overlay_rejects_missing_or_malformed_revision(
  tmp_path, revision: str | None
) -> None:
  response = _setup_reply(_setup(_graph()))
  payload = json.loads(response["files"][-1]["content"])
  if revision is None:
    payload.pop("revision")
  else:
    payload["revision"] = revision
  source = tmp_path / "stages.json"
  source.write_text(json.dumps(payload), encoding="utf-8")
  with pytest.raises(ValueError, match="lowercase SHA-256"):
    load_stage_runtime(tmp_path, "run-1")


def test_link_gateway_must_match_verified_endpoint() -> None:
  request = _setup(_graph(), "validate")
  request["endpoints"]["eos_gateway"]["port"] = 9999
  with pytest.raises(SetupDiagnostic) as exc:
    _setup_reply(request)
  assert exc.value.code == "gateway_mismatch"
  assert exc.value.path.endswith("/settings/gateway")


def test_empty_legacy_graph_remains_read_only() -> None:
  graph = {"version": "1.0.0", "cores": {"egia": {"layers": {}}}}
  assert compose_stages(graph).instances == {}
  assert _setup_reply(_setup(graph))["files"][-1]["path"] != "egia/stages.json"


@pytest.mark.parametrize(
  "group,input_name",
  [("link", "admitted"), ("link", "emergency_stop"), ("safety", "latch")],
)
def test_authored_command_or_latch_route_is_rejected(
  group: str, input_name: str
) -> None:
  graph = _graph()
  _, node = next((path, node) for path, node in _nodes(graph) if f"/{group}/" in path)
  node["inputs"][input_name] = {
    "node": "egia/cognition/decision/planner",
    "output": "proposal",
  }
  with pytest.raises(StageGraphError, match="fixed runtime input") as exc:
    compose_stages(graph)
  assert exc.value.code == "fixed_route"
  assert input_name in exc.value.path


def _nodes(graph: dict):
  for layer_id, layer in graph["cores"]["egia"]["layers"].items():
    for group_id, group in layer["groups"].items():
      for node_id, node in group["nodes"].items():
        yield f"egia/{layer_id}/{group_id}/{node_id}", node


@pytest.mark.parametrize("type_id", list(STAGE_TYPES))
def test_each_stage_rejects_malformed_duplicate_stale_and_stop(type_id: str) -> None:
  composition = compose_stages(_graph())
  origin = next(
    instance
    for instance in composition.instances.values()
    if instance.type.id == type_id
  )
  output, kind = next(iter(origin.type.outputs.items()))
  runtime = StageRuntime(composition, "run-1")
  for path in composition.instances:
    runtime.bind_owner(path, object())
  runtime.start()
  now = datetime.now(UTC)
  value = _value(kind, "event-1", "run-1", origin.path, now - timedelta(seconds=1))
  with pytest.raises(ValueError, match="source output"):
    runtime.publish("wrong", _value(kind, "event-2", "run-1", origin.path, now))
  with pytest.raises(ValueError, match="stale run"):
    runtime.publish(output, _value(kind, "event-2", "run-old", origin.path, now))
  delivered: list[StageValue] = []
  for edge in composition.edges:
    if edge.source == origin.path and edge.output == output:
      runtime.bind(edge.target, edge.input, delivered.append)
  assert runtime.publish(output, value) == len(delivered)
  assert runtime.publish(output, value) == 0
  assert runtime.diagnostics[-1].code == "duplicate_event"
  with pytest.raises(ValueError, match="conflicting duplicate"):
    runtime.publish(
      output,
      StageValue(
        kind,
        "event-1",
        "run-1",
        origin.path,
        now,
        StagePayload(kind, _FIELDS[kind]),
        received_at=now,
      ),
    )
  assert runtime.diagnostics[-1].code == "conflicting_event"
  runtime.stop()
  with pytest.raises(RuntimeError, match="stopping"):
    runtime.publish(output, value)


def test_link_disconnect_cannot_invent_successful_eos_result() -> None:
  composition = compose_stages(_graph())
  runtime = StageRuntime(composition, "run-1")
  for path in composition.instances:
    runtime.bind_owner(path, object())
  runtime.start()
  link = next(
    instance
    for instance in composition.instances.values()
    if instance.type.group == "link"
  )
  result_edge = next(
    edge
    for edge in composition.edges
    if edge.source == link.path and edge.output == "result"
  )
  seen: list[StageValue] = []
  runtime.bind(result_edge.target, result_edge.input, seen.append)
  now = datetime.now(UTC)
  with pytest.raises(ValueError, match="source output"):
    runtime.publish(
      "result",
      _value("admission", "e1", "run-1", link.path, now),
    )
  assert seen == []


def test_typed_payload_deep_freezes_admitted_arguments() -> None:
  fields: dict[str, Any] = {
    "backend": "robot-a",
    "device": "arm",
    "primitive": "move",
    "version": "1",
    "arguments": {"joint": [1, 2]},
  }
  payload = StagePayload("admission", fields)
  fields["arguments"]["joint"].append(3)
  assert payload.fields["arguments"]["joint"] == (1, 2)
  with pytest.raises(ValueError, match="proposal.device"):
    StagePayload("proposal", {"backend": "robot-a"})


def test_same_run_observation_age_is_bounded_before_deduplication() -> None:
  composition = compose_stages(_graph())
  now = datetime(2026, 11, 2, 12, 0, tzinfo=UTC)
  runtime = StageRuntime(composition, "run-1", clock=lambda: now)
  for path in composition.instances:
    runtime.bind_owner(path, object())
  runtime.start()
  link = next(
    path for path, item in composition.instances.items() if item.type.group == "link"
  )
  edge = next(
    edge
    for edge in composition.edges
    if edge.source == link and edge.output == "result"
  )
  seen: list[StageValue] = []
  runtime.bind(edge.target, edge.input, seen.append)
  old = now - MAX_STAGE_EVENT_AGE - timedelta(microseconds=1)
  stale = StageValue(
    "result",
    "old-1",
    "run-1",
    link,
    old,
    StagePayload("result", {"operation_id": "op-1", "terminal_state": "failed"}),
    received_at=old,
  )
  with pytest.raises(ValueError, match="stale stage observation"):
    runtime.publish("result", stale)
  assert runtime.diagnostics[-1].code == "stale_event"
  assert runtime.diagnostics[-1].node == link
  assert runtime.diagnostics[-1].event_id == "old-1"
  assert seen == []
  fresh = StageValue(
    "result",
    "old-1",
    "run-1",
    link,
    now,
    stale.payload,
    received_at=now,
  )
  assert runtime.publish("result", fresh) == 1
  assert seen == [fresh]


@pytest.mark.parametrize(
  "sample", [{}, {"text": ""}, {"text": "ready"}, {"text": "ready", "eos_robot_id": ""}]
)
def test_malformed_resource_is_rejected_before_dedupe(sample: dict) -> None:
  composition = compose_stages(_graph())
  now = datetime(2026, 11, 2, 12, 0, tzinfo=UTC)
  runtime = StageRuntime(composition, "run-1", clock=lambda: now)
  for path in composition.instances:
    runtime.bind_owner(path, object())
  runtime.start()
  link = next(
    path for path, item in composition.instances.items() if item.type.group == "link"
  )
  edge = next(
    edge
    for edge in composition.edges
    if edge.source == link and edge.output == "resource"
  )
  seen: list[StageValue] = []
  runtime.bind(edge.target, edge.input, seen.append)

  def value(resource_sample: dict) -> StageValue:
    return StageValue(
      "resource",
      "resource-1",
      "run-1",
      link,
      now,
      StagePayload(
        "resource",
        {"backend": "robot-a", "reference": "robot", "sample": resource_sample},
      ),
      received_at=now,
    )

  with pytest.raises(ValueError, match="resource sample requires"):
    runtime.publish("resource", value(sample))
  assert runtime.diagnostics[-1].code == "malformed_resource"
  assert runtime.diagnostics[-1].node == link
  assert runtime.diagnostics[-1].event_id == "resource-1"
  assert seen == []
  assert (
    runtime.publish("resource", value({"text": "ready", "eos_robot_id": "robot-a"}))
    == 1
  )
  assert len(seen) == 1


def test_capped_evidence_keeps_latest_and_reports_gaps_until_stop() -> None:
  graph = _graph()
  context = next(
    node for _, node in _nodes(graph) if node["node_type"].split(".")[1] == "context"
  )
  context["inputs"]["observations"].update(priority=-2, frequency_hz=2)
  now = datetime(2026, 11, 2, 12, 0, tzinfo=UTC)
  monotonic = [10_000_000_000]
  runtime = StageRuntime(
    compose_stages(graph),
    "run-1",
    clock=lambda: now,
    monotonic_clock=lambda: monotonic[0],
  )
  for path in runtime.composition.instances:
    runtime.bind_owner(path, object())
  edge = next(
    edge for edge in runtime.composition.edges if edge.output == "observations"
  )
  seen: list[StageValue] = []
  runtime.bind(edge.target, edge.input, seen.append)
  warnings: list[int] = []
  runtime.on_overflow = lambda _edge, counters: warnings.append(counters.overflow)
  runtime.start()
  for index in range(4):
    runtime.publish(
      "observations", _value("observations", f"o-{index}", "run-1", edge.source, now)
    )
  assert [value.event_id for value in seen] == ["o-0"]
  assert runtime.edge_counters[edge].gaps == 2
  assert runtime.edge_counters[edge].overflow == 2
  assert warnings == [1]
  assert runtime.diagnostics[-1].code == "evidence_overflow"
  monotonic[0] += 500_000_000
  assert runtime.drain_ready() == 1
  assert [value.event_id for value in seen] == ["o-0", "o-3"]
  runtime.publish(
    "observations", _value("observations", "o-4", "run-1", edge.source, now)
  )
  runtime.stop()
  monotonic[0] += 1_000_000_000
  assert runtime.drain_ready() == 0
  assert [value.event_id for value in seen] == ["o-0", "o-3"]


@pytest.mark.parametrize("frequency", [5e-324, 1e300])
def test_extreme_positive_caps_use_elapsed_nanoseconds(frequency: float) -> None:
  graph = _graph()
  context = next(
    node for _, node in _nodes(graph) if node["node_type"].split(".")[1] == "context"
  )
  context["inputs"]["observations"]["frequency_hz"] = frequency
  now = datetime(2026, 11, 2, 12, 0, tzinfo=UTC)
  tick = [100]
  runtime = StageRuntime(
    compose_stages(graph), "run-1", clock=lambda: now, monotonic_clock=lambda: tick[0]
  )
  for path in runtime.composition.instances:
    runtime.bind_owner(path, object())
  edge = next(
    edge for edge in runtime.composition.edges if edge.output == "observations"
  )
  seen: list[str] = []
  runtime.bind(edge.target, edge.input, lambda value: seen.append(value.event_id))
  runtime.start()
  runtime.publish(
    "observations", _value("observations", "first", "run-1", edge.source, now)
  )
  runtime.publish(
    "observations", _value("observations", "second", "run-1", edge.source, now)
  )
  assert seen == ["first"]
  tick[0] = 99
  assert runtime.drain_ready() == 0
  tick[0] = 101
  assert runtime.drain_ready() == (1 if frequency > 1 else 0)


def test_weighted_evidence_delivery_is_fair_under_overload() -> None:
  graph = _graph()
  for _, node in _nodes(graph):
    group = node["node_type"].split(".")[1]
    if group == "perception":
      node["inputs"]["resource"]["priority"] = -20
    if group == "context":
      node["inputs"]["observations"]["priority"] = 19
  now = datetime(2026, 11, 2, 12, 0, tzinfo=UTC)
  runtime = StageRuntime(
    compose_stages(graph), "run-1", clock=lambda: now, auto_drain=False
  )
  for path in runtime.composition.instances:
    runtime.bind_owner(path, object())
  edges = {
    edge.output: edge
    for edge in runtime.composition.edges
    if edge.output in {"resource", "observations"}
  }
  seen: list[str] = []
  for edge in edges.values():
    runtime.bind(edge.target, edge.input, lambda value: seen.append(value.kind))
  runtime.start()
  for index in range(200):
    resource = StageValue(
      "resource",
      f"r-{index}",
      "run-1",
      edges["resource"].source,
      now,
      StagePayload(
        "resource",
        {
          "backend": "robot-a",
          "reference": "robot",
          "sample": {"text": "ready", "eos_robot_id": "robot-a"},
        },
      ),
      received_at=now,
    )
    runtime.publish("resource", resource)
    runtime.publish(
      "observations",
      _value("observations", f"o-{index}", "run-1", edges["observations"].source, now),
    )
    assert runtime.drain_ready() == 1
  assert seen.count("resource") > seen.count("observations") > 0
  assert len(seen) == 200


@pytest.mark.asyncio
async def test_owner_overload_is_counted_and_next_evidence_still_runs() -> None:
  now = datetime(2026, 11, 2, 12, 0, tzinfo=UTC)
  runtime = StageRuntime(
    compose_stages(_graph()), "run-1", clock=lambda: now, auto_drain=False
  )
  for path in runtime.composition.instances:
    runtime.bind_owner(path, object())
  edge = next(edge for edge in runtime.composition.edges if edge.output == "resource")
  accepted: list[str] = []
  runtime.bind(edge.target, edge.input, lambda value: accepted.append(value.event_id))

  async def admit(value: StageValue) -> None:
    if value.event_id == "full":
      raise RuntimeError("perception queue full")

  runtime.bind_async(edge.target, edge.input, admit)
  runtime.start()

  def resource(event_id: str) -> StageValue:
    return StageValue(
      "resource",
      event_id,
      "run-1",
      edge.source,
      now,
      StagePayload(
        "resource",
        {
          "backend": "robot-a",
          "reference": "robot",
          "sample": {"text": "ready", "eos_robot_id": "robot-a"},
        },
      ),
      received_at=now,
    )

  assert await runtime.publish_async("resource", resource("full")) == 0
  assert await runtime.drain_async() == 0
  assert runtime.diagnostics[-1].code == "owner_admission_failed"
  assert runtime.edge_counters[edge].overflow == 1
  assert runtime.edge_counters[edge].gaps == 1
  assert accepted == []
  assert await runtime.publish_async("resource", resource("next")) == 0
  assert await runtime.drain_async() == 1
  assert accepted == ["next"]


@pytest.mark.asyncio
async def test_async_stop_waits_for_owner_and_discards_pending() -> None:
  now = datetime(2026, 11, 2, 12, 0, tzinfo=UTC)
  runtime = StageRuntime(
    compose_stages(_graph()), "run-1", clock=lambda: now, auto_drain=False
  )
  for path in runtime.composition.instances:
    runtime.bind_owner(path, object())
  edge = next(edge for edge in runtime.composition.edges if edge.output == "resource")
  accepted: list[str] = []
  entered = asyncio.Event()
  release = asyncio.Event()
  runtime.bind(edge.target, edge.input, lambda value: accepted.append(value.event_id))

  async def admit(value: StageValue) -> None:
    entered.set()
    await release.wait()

  runtime.bind_async(edge.target, edge.input, admit)
  runtime.start()

  def resource(event_id: str) -> StageValue:
    return StageValue(
      "resource",
      event_id,
      "run-1",
      edge.source,
      now,
      StagePayload(
        "resource",
        {
          "backend": "robot-a",
          "reference": "robot",
          "sample": {"text": "ready", "eos_robot_id": "robot-a"},
        },
      ),
      received_at=now,
    )

  await runtime.publish_async("resource", resource("in-flight"))
  delivery = asyncio.create_task(runtime.drain_async())
  await entered.wait()
  await runtime.publish_async("resource", resource("queued"))
  stopping = asyncio.create_task(runtime.stop_async())
  await asyncio.sleep(0)
  assert not stopping.done()
  release.set()
  await delivery
  await stopping
  assert accepted == ["in-flight"]
  assert runtime.edge_counters[edge].gaps == 1
  assert runtime.diagnostics[-1].code == "evidence_stopped"
