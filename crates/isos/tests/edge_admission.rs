#[path = "../src/domain/graph/edge_admission.rs"]
mod edge_admission;

use edge_admission::admit;
use serde_json::{Value, json};

fn catalog() -> Value {
  serde_json::from_str(include_str!("fixtures/edge_admission/catalog.json")).unwrap()
}

fn graph() -> Value {
  serde_json::from_str(include_str!("fixtures/edge_admission/valid_0_1_0.json")).unwrap()
}

fn vision_bridge() -> Value {
  serde_json::from_str(include_str!(
    "fixtures/edge_admission/vision_bridge_unregistered.json"
  ))
  .unwrap()
}

fn fixed_command_qos() -> Value {
  serde_json::from_str(include_str!(
    "fixtures/edge_admission/fixed_command_qos.json"
  ))
  .unwrap()
}

fn cross_core_guard_request() -> Value {
  serde_json::from_str(include_str!(
    "fixtures/edge_admission/cross_core_guard_request.json"
  ))
  .unwrap()
}

#[test]
fn unrelated_package_families_share_typed_edge_admission() {
  for (family, data_type) in [
    ("sensor", "field.record@v1.0"),
    ("synth", "studio.record@v1.0"),
  ] {
    let source_type = format!("{family}.process.source@v0.1");
    let sink_type = format!("{family}.process.sink@v0.1");
    let graph = json!({"version":"0.1.0","cores":{"eos":{"layers":{"process":{"groups":{
      "component":{"nodes":{
        "source":{"id":"source","node_type":source_type},
        "sink":{"id":"sink","node_type":sink_type,"inputs":{
          "record":{"node":"eos/process/component/source","output":"record"}}}
      }}
    }}}}}});
    let mut catalog = json!({"types":[
      {"id":source_type,"core":"eos","layer":"process","group":"component",
        "inputs":[],"outputs":[{"name":"record","data_type":data_type,
          "transport":"in_process","delivery_mode":"message"}]},
      {"id":sink_type,"core":"eos","layer":"process","group":"component",
        "inputs":[{"name":"record","data_type":data_type,
          "transport":"in_process","delivery_mode":"message"}],"outputs":[]}
    ],"data_types":[{"id":data_type,"schema":{"type":"object"}}]});
    let edges = admit(&graph, &catalog).unwrap();
    assert_eq!(edges.len(), 1, "{family}");
    assert_eq!(edges[0].data_type, data_type);
    assert_eq!(edges[0].transport, "in_process");

    catalog["types"][1]["inputs"][0]["transport"] = json!("unix_socket");
    assert_eq!(admit(&graph, &catalog).unwrap_err().code, "edge_transport");
    catalog["types"][1]["inputs"][0]["transport"] = json!("in_process");
    catalog["types"][1]["inputs"][0]["data_type"] = json!("other.record@v1.0");
    assert_eq!(
      admit(&graph, &catalog).unwrap_err().code,
      "edge_type_mismatch"
    );
  }
}

#[test]
fn registered_http_bridge_admits_matching_cross_core_event_only() {
  let graph = json!({"version":"0.1.0","cores":{
    "isosui":{"layers":{"input":{"groups":{"keyboard":{"nodes":{"key":{
      "id":"key","node_type":"ui.keyboard@v0.1"}}}}}}},
    "isos":{"layers":{"interface":{"groups":{"gateway":{"nodes":{"bridge":{
      "id":"bridge","node_type":"isos.gateway@v0.1",
      "inputs":{"state":{"node":"isosui/input/keyboard/key","output":"state"}}}}}}}}}
  }});
  let mut catalog = json!({"types":[
    {"id":"ui.keyboard@v0.1","core":"isosui","layer":"input","group":"keyboard",
      "inputs":[],"outputs":[{"name":"state","data_type":"key_state","transport":"http","delivery_mode":"event"}]},
    {"id":"isos.gateway@v0.1","core":"isos","layer":"interface","group":"gateway",
      "capabilities":["bridge"],"bridge":{"adapter":"http","inputs":["state"],"outputs":[]},
      "inputs":[{"name":"state","data_type":"key_state","transport":"http","delivery_mode":"event"}],"outputs":[]}
  ],"data_types":[{"id":"key_state","schema":{"type":"object"}}],
    "bridge_adapters":[{"core":"isos","adapter":"http","node_type":"isos.gateway@v0.1"}]});
  let admitted = admit(&graph, &catalog).unwrap();
  assert_eq!(admitted.len(), 1);
  assert_eq!(admitted[0].route_class, "owner_bridge");
  assert_eq!(admitted[0].delivery_mode, "event");
  catalog["bridge_adapters"] = json!([]);
  assert_eq!(
    admit(&graph, &catalog).unwrap_err().code,
    "edge_bridge_undeclared"
  );
  catalog["bridge_adapters"] =
    json!([{"core":"isos","adapter":"http","node_type":"isos.gateway@v0.1"}]);
  catalog["types"][1]["inputs"][0]["delivery_mode"] = json!("stream");
  assert_eq!(
    admit(&graph, &catalog).unwrap_err().code,
    "edge_delivery_mode"
  );
}

fn camera_input(graph: &mut Value) -> &mut Value {
  &mut graph["cores"]["eos"]["layers"]["interface"]["groups"]["gate"]["nodes"]["preview"]["inputs"]
    ["frame"]
}

fn egia_evidence() -> (Value, Value) {
  let graph = json!({"version":"0.1.0","cores":{"egia":{"layers":{
      "io":{"groups":{"link":{"nodes":{"gateway":{"id":"gateway","node_type":"egia.link.gateway@v0.1"}}}}},
      "evidence":{"groups":{"perception":{"nodes":{"admission":{"id":"admission","node_type":"egia.perception.admission@v0.1","inputs":{"resource":{"node":"egia/io/link/gateway","output":"resource","priority":-20,"frequency_hz":12}}}}}}},
      "cognition":{"groups":{"context":{"nodes":{"builder":{"id":"builder","node_type":"egia.context.builder@v0.1","inputs":{"observations":{"node":"egia/evidence/perception/admission","output":"observations"}}}}}}}
  }}}});
  let catalog = json!({"types":[
      {"id":"egia.link.gateway@v0.1","core":"egia","layer":"io","group":"link","inputs":[],"outputs":[{"name":"resource","data_type":"egia.stage.resource@v0.1","transport":"in_process","data_class":"evidence"}]},
      {"id":"egia.perception.admission@v0.1","core":"egia","layer":"evidence","group":"perception","inputs":[{"name":"resource","data_type":"egia.stage.resource@v0.1","transport":"in_process","data_class":"evidence"}],"outputs":[{"name":"observations","data_type":"egia.stage.observations@v0.1","transport":"in_process","data_class":"evidence"}]},
      {"id":"egia.context.builder@v0.1","core":"egia","layer":"cognition","group":"context","inputs":[{"name":"observations","data_type":"egia.stage.observations@v0.1","transport":"in_process","data_class":"evidence"}],"outputs":[]}
  ],"data_types":[
      {"id":"egia.stage.resource@v0.1","schema":{"type":"object"}},
      {"id":"egia.stage.observations@v0.1","schema":{"type":"object"}}
  ],"egia_routed_edges":[
      {"source_type":"egia.link.gateway@v0.1","output":"resource","target_type":"egia.perception.admission@v0.1","input":"resource","controls":["priority","frequency_hz"]},
      {"source_type":"egia.perception.admission@v0.1","output":"observations","target_type":"egia.context.builder@v0.1","input":"observations","controls":["priority","frequency_hz"]}
  ]});
  (graph, catalog)
}

#[test]
fn selected_egia_evidence_routes_are_exact_and_defaults_are_routed() {
  let (graph, catalog) = egia_evidence();
  let edges = admit(&graph, &catalog).unwrap();
  assert_eq!(edges.len(), 2);
  assert!(edges.iter().all(|edge| edge.route_class == "egia_local"
    && edge.routed
    && edge.qos_eligible
    && !edge.fixed));
  assert_eq!(edges[0].priority, 0);
  assert_eq!(edges[1].priority, -20);
  assert_eq!(edges[1].frequency_hz, Some(12.0));
  let mut missing = catalog.clone();
  missing.as_object_mut().unwrap().remove("egia_routed_edges");
  let mut controlled = graph.clone();
  controlled["cores"]["egia"]["layers"]["evidence"]["groups"]["perception"]["nodes"]["admission"]
    ["inputs"]["resource"]["priority"] = json!(0);
  assert_eq!(
    admit(&controlled, &missing).unwrap_err().code,
    "edge_qos_forbidden"
  );
  let mut fixed = catalog;
  fixed["types"][0]["outputs"][0]["fixed"] = json!(true);
  assert_eq!(admit(&graph, &fixed).unwrap_err().code, "edge_fixed_route");
}

#[test]
fn exact_eos_route_and_descriptive_egia_edge() {
  let edges = admit(&graph(), &catalog()).unwrap();
  assert_eq!(edges.len(), 2);
  assert_eq!(edges[0].route_class, "egia_local");
  assert!(!edges[0].routed);
  assert!(!edges[0].qos_eligible);
  assert_eq!(edges[1].route_class, "eos_local");
  assert!(edges[1].routed);
  assert_eq!(edges[1].priority, 5);
  assert_eq!(edges[1].frequency_hz, Some(15.0));
  assert_eq!(
    edges[1].path,
    "/cores/eos/layers/interface/groups/gate/nodes/preview/inputs/frame"
  );
}

#[test]
fn egia_fixed_port_is_never_graph_wired_or_qos_eligible() {
  let mut definitions = catalog();
  definitions["types"][2]["outputs"][0]["fixed"] = json!(true);
  let error = admit(&graph(), &definitions).unwrap_err();
  assert_eq!(error.code, "edge_fixed_route");
  assert_eq!(
    error.path,
    "/cores/egia/layers/common/groups/data/nodes/context/inputs/observation"
  );
  let edges = admit(&graph(), &catalog()).unwrap();
  let egia = edges
    .iter()
    .find(|edge| edge.route_class == "egia_local")
    .unwrap();
  assert!(!egia.routed);
  assert!(!egia.qos_eligible);
}

#[test]
fn frozen_vision_bridge_stays_closed_without_owner_registration_or_route() {
  let graph = vision_bridge();
  let error = admit(&graph, &catalog()).unwrap_err();
  assert_eq!(error.code, "unsupported_node_type");
  assert_eq!(
    error.path,
    "/cores/isosui/layers/system/groups/vision/nodes/panel/node_type"
  );

  let mut catalog = catalog();
  catalog["types"].as_array_mut().unwrap().push(json!({
        "id":"isosui.system.vision@v0.1","core":"isosui","layer":"system","group":"vision",
        "inputs":[{"name":"frame","data_type":"vision_frame","transport":"websocket","data_class":"image","required":true}]
    }));
  let error = admit(&graph, &catalog).unwrap_err();
  assert_eq!(error.code, "edge_bridge_undeclared");
  assert_eq!(
    error.path,
    "/cores/isosui/layers/system/groups/vision/nodes/panel/inputs/frame"
  );
  assert_eq!(error.source.as_deref(), Some("eos/interface/gate/preview"));
}

#[test]
fn frozen_command_route_rejects_explicit_qos() {
  let error = admit(&fixed_command_qos(), &catalog()).unwrap_err();
  assert_eq!(error.code, "edge_qos_forbidden");
  assert_eq!(
    error.path,
    "/cores/eos/layers/hardware/groups/base/nodes/drive/inputs/command/0/priority"
  );
}

#[test]
fn frozen_cross_core_guard_request_has_exact_bridge_diagnostic() {
  let error = admit(&cross_core_guard_request(), &catalog()).unwrap_err();
  assert_eq!(error.code, "edge_bridge_undeclared");
  assert_eq!(
    error.path,
    "/cores/eos/layers/dispatcher/groups/control/nodes/command/inputs/request"
  );
  assert_eq!(error.source.as_deref(), Some("egia/common/process/guard"));
  assert_eq!(
    error.target.as_deref(),
    Some("eos/dispatcher/control/command")
  );
  assert_eq!(error.input.as_deref(), Some("request"));
  assert_eq!(error.source_data_type.as_deref(), Some("control_request"));
  assert_eq!(error.target_data_type.as_deref(), Some("control_request"));
}

#[test]
fn exact_pointer_and_type_unit_identity() {
  let mut value = graph();
  camera_input(&mut value)["output"] = json!("unknown");
  let error = admit(&value, &catalog()).unwrap_err();
  assert_eq!(
    (error.path.as_str(), error.code),
    (
      "/cores/eos/layers/interface/groups/gate/nodes/preview/inputs/frame",
      "edge_unknown_output"
    )
  );
  let mut value = graph();
  camera_input(&mut value)["node"] = json!("egia/common/data/source");
  camera_input(&mut value)["output"] = json!("observation");
  let error = admit(&value, &catalog()).unwrap_err();
  assert_eq!(error.code, "edge_type_mismatch");
  assert_eq!(error.source_data_type.as_deref(), Some("observation_ref"));
  assert_eq!(error.target_data_type.as_deref(), Some("camera_frame"));
  let mut definitions = catalog();
  definitions["data_types"].as_array_mut().unwrap().push(json!({"id":"camera_frame","schema":{"type":"object","properties":{"width_px":{"x-unit":"m"}}}}));
  let error = admit(&graph(), &definitions).unwrap_err();
  assert_eq!(error.code, "edge_unit_schema");
  let mut definitions = catalog();
  definitions["data_types"][0]["schema"]["properties"]["width_px"] =
    json!({"type":"integer","x-unit":17});
  let error = admit(&graph(), &definitions).unwrap_err();
  assert_eq!(error.code, "edge_unit_schema");
}

#[test]
fn cardinality_transport_placement_and_cross_core_fail_closed() {
  let mut value = graph();
  value["cores"]["eos"]["layers"]["interface"]["groups"]["gate"]["nodes"]["preview"]
    .as_object_mut()
    .unwrap()
    .remove("inputs");
  assert_eq!(
    admit(&value, &catalog()).unwrap_err().code,
    "edge_required_input"
  );
  let mut value = graph();
  *camera_input(&mut value) = json!([]);
  assert_eq!(
    admit(&value, &catalog()).unwrap_err().code,
    "edge_cardinality"
  );
  let mut value = graph();
  *camera_input(&mut value) = json!([{"node":"eos/hardware/camera/front","output":"frame"}]);
  let edges = admit(&value, &catalog()).unwrap();
  assert!(edges[1].path.ends_with("/frame/0"));
  let mut value = graph();
  camera_input(&mut value)["node"] = json!("eos/hardware/camera/missing");
  assert_eq!(admit(&value, &catalog()).unwrap_err().code, "edge_source");
  let mut definitions = catalog();
  definitions["types"][0]["group"] = json!("other");
  assert_eq!(
    admit(&graph(), &definitions).unwrap_err().code,
    "edge_placement"
  );
  let mut definitions = catalog();
  definitions["types"][0]["outputs"][0]["transport"] = json!("websocket");
  assert_eq!(
    admit(&graph(), &definitions).unwrap_err().code,
    "edge_transport"
  );
  let mut value = graph();
  camera_input(&mut value)["node"] = json!("egia/common/data/source");
  camera_input(&mut value)["output"] = json!("observation");
  value["cores"]["egia"]["layers"]["common"]["groups"]["data"]["nodes"]
    .as_object_mut()
    .unwrap()
    .remove("context");
  let mut definitions = catalog();
  definitions["types"][2]["outputs"][0]["data_type"] = json!("camera_frame");
  definitions["types"][2]["outputs"][0]["data_class"] = json!("image");
  let error = admit(&value, &definitions).unwrap_err();
  assert_eq!(error.code, "edge_bridge_undeclared");
  assert_eq!(
    error.path,
    "/cores/eos/layers/interface/groups/gate/nodes/preview/inputs/frame"
  );
}

#[test]
fn qos_is_limited_to_the_exact_evidence_route() {
  let mut value = graph();
  camera_input(&mut value)["opaque_override"] = json!(true);
  assert_eq!(
    admit(&value, &catalog()).unwrap_err().code,
    "edge_source_field"
  );
  for priority in [json!(-21), json!(20), json!(1.5), json!("0")] {
    let mut value = graph();
    camera_input(&mut value)["priority"] = priority;
    assert_eq!(admit(&value, &catalog()).unwrap_err().code, "edge_priority");
  }
  for frequency in [json!(0), json!(-1), json!("NaN")] {
    let mut value = graph();
    camera_input(&mut value)["frequency_hz"] = frequency;
    assert_eq!(
      admit(&value, &catalog()).unwrap_err().code,
      "edge_frequency"
    );
  }
  let mut value = graph();
  value["cores"]["egia"]["layers"]["common"]["groups"]["data"]["nodes"]["context"]["inputs"]["observation"]
    ["priority"] = json!(0);
  assert_eq!(
    admit(&value, &catalog()).unwrap_err().code,
    "edge_qos_forbidden"
  );
}

#[test]
fn actuator_bypass_and_binding_target_count_are_rejected() {
  let mut value = graph();
  value["cores"]["eos"]["layers"]["hardware"]["groups"]["base"]["nodes"]["drive"] = json!({"id":"drive","node_type":"eos.hardware.base@v0.1","inputs":{"command":[{"node":"eos/fake/command/raw","output":"command"}]}});
  value["cores"]["eos"]["layers"]["fake"]["groups"]["command"]["nodes"]["raw"] =
    json!({"id":"raw","node_type":"eos.fake.command@v0.1"});
  let error = admit(&value, &catalog()).unwrap_err();
  assert_eq!(error.code, "edge_actuator_bypass");
  let mut value = graph();
  value["cores"]["eos"]["layers"]["dispatcher"]["groups"]["base"]["nodes"]["bind"] =
    json!({"id":"bind","node_type":"eos.dispatcher.primitive_base@v0.1"});
  assert_eq!(
    admit(&value, &catalog()).unwrap_err().code,
    "edge_binding_target"
  );
}

#[test]
fn cycle_and_reference_rejections_name_the_original_path() {
  let mut value = graph();
  value["cores"]["egia"]["layers"]["common"]["groups"]["data"]["nodes"]["source"]["references"] =
    json!(["eos/hardware/camera/front"]);
  let error = admit(&value, &catalog()).unwrap_err();
  assert_eq!(
    (error.code, error.path.as_str()),
    (
      "cross_core_reference",
      "/cores/egia/layers/common/groups/data/nodes/source/references/0"
    )
  );
  let mut value = graph();
  let nodes = &mut value["cores"]["egia"]["layers"]["common"]["groups"]["data"]["nodes"];
  nodes["source"]["inputs"] =
    json!({"observation":{"node":"egia/common/data/context","output":"observation"}});
  let mut catalog = catalog();
  catalog["types"][2]["inputs"] = json!([{"name":"observation","data_type":"observation_ref"}]);
  catalog["types"][3]["outputs"] = json!([{"name":"observation","data_type":"observation_ref"}]);
  assert_eq!(admit(&value, &catalog).unwrap_err().code, "edge_cycle");
}
