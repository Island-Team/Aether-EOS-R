use aether_node_profile::{
  DeliveryMode, EosEdgeClass, NodeTypeRegistry, PortCardinality, PortTransport, Profile,
};
use serde_json::{Value, json};

fn bindings() -> Value {
  serde_json::from_str(include_str!(
    "../../../tests/fixtures/edge_contract/valid_bindings.json"
  ))
  .unwrap()
}

fn evidence() -> Value {
  serde_json::from_str(include_str!(
    "../../../tests/fixtures/edge_contract/valid_evidence.json"
  ))
  .unwrap()
}

fn validate(graph: &Value, registry: &NodeTypeRegistry) -> Result<Profile, String> {
  Profile::from_json_with_registry(&graph.to_string(), registry).map_err(|error| error.to_string())
}

fn frame(graph: &mut Value) -> &mut Value {
  &mut graph["cores"]["eos"]["layers"]["interface"]["groups"]["gate"]["nodes"]["front"]["inputs"]
    ["frame"]
}

fn command(graph: &mut Value) -> &mut Value {
  &mut graph["cores"]["eos"]["layers"]["hardware"]["groups"]["base"]["nodes"]["drive"]["inputs"]
    ["command"][0]
}

#[test]
fn http_gateway_uses_a_typed_stream_edge_into_the_primitive_binding() {
  let registry = NodeTypeRegistry::builtin();
  let gateway = registry.get("eos.interface.http_gateway@v0.1").unwrap();
  assert_eq!(gateway.inputs[0].transport, Some(PortTransport::Http));
  assert_eq!(gateway.outputs[0].delivery_mode, DeliveryMode::Stream);
  let mut graph = bindings();
  graph["cores"]["eos"]["layers"]["interface"]["groups"]["gateway"] = json!({
    "nodes":{"control":{"id":"control","node_type":"eos.interface.http_gateway@v0.1",
      "label":"Operator movement gateway","description":"Run-fenced movement ingress"}}
  });
  graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["base"]["nodes"]["velocity"]["inputs"] =
    json!({"request":{"node":"eos/interface/gateway/control","output":"movement"}});
  let profile = validate(&graph, &registry).unwrap();
  let edge = profile
    .eos_edge_contract(&registry)
    .unwrap()
    .into_iter()
    .find(|edge| edge.target_input == "request" && edge.target_node.ends_with("/velocity"))
    .unwrap();
  assert_eq!(edge.transport, PortTransport::InProcess);
  assert_eq!(edge.class, EosEdgeClass::FixedSafety);

  let mut mismatched = registry.clone();
  let mut wrong = gateway.clone();
  wrong.id = "eos.interface.wrong_delivery@v0.1".into();
  wrong.outputs[0].delivery_mode = DeliveryMode::Message;
  mismatched.register(wrong).unwrap();
  graph["cores"]["eos"]["layers"]["interface"]["groups"]["gateway"]["nodes"]["control"]
    ["node_type"] = json!("eos.interface.wrong_delivery@v0.1");
  assert!(validate(&graph, &mismatched)
    .unwrap_err()
    .contains("delivery mode"));
}

#[test]
fn camera_evidence_is_the_only_user_scheduled_eos_route() {
  let registry = NodeTypeRegistry::builtin();
  let profile = validate(&evidence(), &registry).unwrap();
  let edges = profile.eos_edge_contract(&registry).unwrap();
  let camera = edges
    .iter()
    .find(|edge| edge.target_node == "eos/interface/gate/front")
    .unwrap();
  assert_eq!(camera.class, EosEdgeClass::EosLocal);
  assert_eq!(camera.priority, -20);
  assert_eq!(camera.frequency_hz, Some(15.0));
  assert!(edges
    .iter()
    .filter(|edge| edge.target_input == "command")
    .all(|edge| edge.class == EosEdgeClass::FixedSafety && edge.frequency_hz.is_none()));
  assert_eq!(edges.len(), 3);
}

#[test]
fn legacy_one_source_array_and_absent_qos_stay_valid() {
  let registry = NodeTypeRegistry::builtin();
  let mut graph = evidence();
  graph["cores"]["eos"]["layers"]["interface"]["groups"]["gate"]["nodes"]["front"]["inputs"]
    ["frame"] = json!([{"node":"eos/hardware/camera/front","output":"frame"}]);
  let profile = validate(&graph, &registry).unwrap();
  let camera = profile
    .eos_edge_contract(&registry)
    .unwrap()
    .into_iter()
    .find(|edge| edge.target_input == "frame")
    .unwrap();
  assert_eq!(camera.priority, 0);
  assert_eq!(camera.frequency_hz, None);
}

#[test]
fn qos_is_bounded_and_forbidden_on_fixed_command_paths() {
  let registry = NodeTypeRegistry::builtin();
  let mut graph = evidence();
  frame(&mut graph)["priority"] = json!(20);
  assert!(validate(&graph, &registry)
    .unwrap_err()
    .contains("/priority"));
  frame(&mut graph)["priority"] = json!(-21);
  assert!(validate(&graph, &registry)
    .unwrap_err()
    .contains("/priority"));
  frame(&mut graph)["priority"] = json!(0);
  frame(&mut graph)["frequency_hz"] = json!(0);
  assert!(validate(&graph, &registry)
    .unwrap_err()
    .contains("/frequency_hz"));
  frame(&mut graph)["frequency_hz"] = json!(1.0);
  command(&mut graph)["priority"] = json!(0);
  assert!(validate(&graph, &registry)
    .unwrap_err()
    .contains("/priority"));
  command(&mut graph)
    .as_object_mut()
    .unwrap()
    .remove("priority");
  command(&mut graph)["frequency_hz"] = json!(5.0);
  assert!(validate(&graph, &registry)
    .unwrap_err()
    .contains("/frequency_hz"));
}

#[test]
fn transport_and_cardinality_mismatch_fail_before_route_construction() {
  let mut registry = NodeTypeRegistry::builtin();
  let mut bad_gate = registry
    .get("eos.interface.camera_gate@v0.1")
    .unwrap()
    .clone();
  bad_gate.id = "eos.interface.wrong_transport@v0.1".into();
  bad_gate.inputs[0].transport = Some(PortTransport::Websocket);
  registry.register(bad_gate).unwrap();
  let mut graph = evidence();
  graph["cores"]["eos"]["layers"]["interface"]["groups"]["gate"]["nodes"]["front"]["node_type"] =
    json!("eos.interface.wrong_transport@v0.1");
  assert!(validate(&graph, &registry)
    .unwrap_err()
    .contains("transport"));
  graph["cores"]["eos"]["layers"]["interface"]["groups"]["gate"]["nodes"]["front"]["node_type"] =
    json!("eos.interface.camera_gate@v0.1");
  *frame(&mut graph) = json!([
    {"node":"eos/hardware/camera/front","output":"frame"},
    {"node":"eos/hardware/camera/front","output":"frame"}
  ]);
  assert!(validate(&graph, &registry)
    .unwrap_err()
    .contains("cardinality"));
  assert_eq!(
    registry
      .get("eos.interface.camera_gate@v0.1")
      .unwrap()
      .inputs[0]
      .cardinality,
    PortCardinality::One
  );
}

#[test]
fn exact_type_and_source_identity_are_required() {
  let registry = NodeTypeRegistry::builtin();
  let mut graph = evidence();
  frame(&mut graph)["output"] = json!("health");
  assert!(validate(&graph, &registry)
    .unwrap_err()
    .contains("incompatible source"));
  frame(&mut graph)["output"] = json!("frame");
  frame(&mut graph)["node"] = json!("eos/hardware/camera/missing");
  assert!(validate(&graph, &registry)
    .unwrap_err()
    .contains("missing node"));
  frame(&mut graph)["node"] = json!("eos/hardware/camera/front");
  graph["cores"]["eos"]["layers"]["interface"]["groups"]["gate"]["nodes"]["front"]["references"] =
    json!(["eos/hardware/camera/missing"]);
  assert!(validate(&graph, &registry)
    .unwrap_err()
    .contains("invalid reference"));
}

#[test]
fn repeated_sources_and_missing_required_input_fail() {
  let registry = NodeTypeRegistry::builtin();
  let mut graph = evidence();
  *frame(&mut graph) = json!([
    {"node":"eos/hardware/camera/front","output":"frame"},
    {"node":"eos/hardware/camera/front","output":"frame"}
  ]);
  assert!(validate(&graph, &registry)
    .unwrap_err()
    .contains("invalid cardinality"));
  let mut graph = bindings();
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["base"]["nodes"]["drive"]["inputs"]
    ["command"] = json!([
    {"node":"eos/dispatcher/base/velocity","output":"command"},
    {"node":"eos/dispatcher/base/velocity","output":"command"}
  ]);
  assert!(validate(&graph, &registry)
    .unwrap_err()
    .contains("repeats a source"));
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["base"]["nodes"]["drive"]["inputs"]
    ["command"] = json!([]);
  assert!(validate(&graph, &registry)
    .unwrap_err()
    .contains("invalid cardinality"));
  let mut graph = evidence();
  graph["cores"]["eos"]["layers"]["interface"]["groups"]["gate"]["nodes"]["front"]["inputs"] =
    json!({});
  assert!(validate(&graph, &registry)
    .unwrap_err()
    .contains("requires input frame"));
}

#[test]
fn contributed_command_shape_cannot_bypass_dispatcher() {
  let mut registry = NodeTypeRegistry::builtin();
  let mut fake = registry
    .get("eos.dispatcher.primitive_base@v0.1")
    .unwrap()
    .clone();
  fake.id = "eos.dispatcher.fake_base@v0.1".into();
  registry.register(fake).unwrap();
  let mut graph = bindings();
  graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["base"]["nodes"]["velocity"]
    ["node_type"] = json!("eos.dispatcher.fake_base@v0.1");
  assert!(validate(&graph, &registry)
    .unwrap_err()
    .contains("unadmitted command source"));
}

#[test]
fn eos_edge_cycles_fail_even_when_each_port_type_matches() {
  let mut registry = NodeTypeRegistry::builtin();
  let mut loop_type = registry
    .get("eos.interface.camera_gate@v0.1")
    .unwrap()
    .clone();
  loop_type.id = "eos.interface.loop@v0.1".into();
  loop_type.outputs = vec![registry.get("eos.hardware.camera@v0.1").unwrap().outputs[0].clone()];
  registry.register(loop_type).unwrap();
  let mut graph = bindings();
  graph["cores"]["eos"]["layers"]["interface"] = json!({"groups":{"gate":{"nodes":{
    "first":{"id":"first","node_type":"eos.interface.loop@v0.1","label":"First","description":"First loop endpoint","inputs":{"frame":{"node":"eos/interface/gate/second","output":"frame"}}},
    "second":{"id":"second","node_type":"eos.interface.loop@v0.1","label":"Second","description":"Second loop endpoint","inputs":{"frame":{"node":"eos/interface/gate/first","output":"frame"}}}
  }}}});
  assert!(validate(&graph, &registry)
    .unwrap_err()
    .contains("edge cycle"));
}
