//! Full-graph, owner-catalog edge admission for graph 0.1.0.
//!
//! This module has no file or process effects. Callers must use the exact catalog
//! and saved graph revision that they subsequently render or launch.

use serde::Serialize;
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct Diagnostic {
  pub path: String,
  pub code: &'static str,
  pub message: String,
  #[serde(skip_serializing_if = "Option::is_none")]
  pub source: Option<String>,
  #[serde(skip_serializing_if = "Option::is_none")]
  pub target: Option<String>,
  #[serde(skip_serializing_if = "Option::is_none")]
  pub input: Option<String>,
  #[serde(skip_serializing_if = "Option::is_none")]
  pub source_data_type: Option<String>,
  #[serde(skip_serializing_if = "Option::is_none")]
  pub target_data_type: Option<String>,
}

impl std::fmt::Display for Diagnostic {
  fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(formatter, "{}: {}: {}", self.path, self.code, self.message)
  }
}

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct Edge {
  pub path: String,
  pub source: String,
  pub target: String,
  pub input: String,
  pub route_class: &'static str,
  pub fixed: bool,
  pub qos_eligible: bool,
  pub routed: bool,
  pub priority: i64,
  #[serde(skip_serializing_if = "Option::is_none")]
  pub frequency_hz: Option<f64>,
  pub data_type: String,
  pub transport: String,
  pub delivery_mode: String,
}

struct Node<'a> {
  path: String,
  pointer: String,
  core: String,
  owner: String,
  value: &'a Value,
  definition: &'a Value,
}

fn escaped(value: &str) -> String {
  value.replace('~', "~0").replace('/', "~1")
}

fn error(
  path: impl Into<String>,
  code: &'static str,
  message: impl Into<String>,
) -> Box<Diagnostic> {
  Box::new(Diagnostic {
    path: path.into(),
    code,
    message: message.into(),
    source: None,
    target: None,
    input: None,
    source_data_type: None,
    target_data_type: None,
  })
}

#[allow(clippy::too_many_arguments)]
fn edge_error(
  path: &str,
  code: &'static str,
  message: impl Into<String>,
  source: &str,
  target: &str,
  input: &str,
  source_type: Option<&str>,
  target_type: Option<&str>,
) -> Box<Diagnostic> {
  Box::new(Diagnostic {
    path: path.into(),
    code,
    message: message.into(),
    source: Some(source.into()),
    target: Some(target.into()),
    input: Some(input.into()),
    source_data_type: source_type.map(str::to_owned),
    target_data_type: target_type.map(str::to_owned),
  })
}

fn port<'a>(definition: &'a Value, direction: &str, name: &str) -> Option<&'a Value> {
  definition[direction]
    .as_array()?
    .iter()
    .find(|port| port["name"] == name)
}

fn transport(port: &Value) -> Option<&str> {
  port["transport"].as_str().or_else(|| {
    port
      .get("transport")
      .is_none_or(Value::is_null)
      .then_some("in_process")
  })
}

fn bridge_port(
  catalog: &Value,
  node: &Node<'_>,
  direction: &str,
  name: &str,
  transport: &str,
) -> bool {
  let definition = node.definition;
  definition["capabilities"]
    .as_array()
    .is_some_and(|values| values.iter().any(|v| v == "bridge"))
    && definition["bridge"]["adapter"] == transport
    && definition["bridge"][direction]
      .as_array()
      .is_some_and(|ports| ports.iter().any(|v| v == name))
    && catalog["bridge_adapters"]
      .as_array()
      .is_some_and(|adapters| {
        adapters.iter().any(|adapter| {
          adapter["core"] == node.core
            && adapter["adapter"] == transport
            && adapter["node_type"] == definition["id"]
        })
      })
}

fn valid_unit_annotations(schema: &Value) -> bool {
  match schema {
    Value::Object(fields) => {
      fields
        .get("x-unit")
        .is_none_or(|unit| unit.as_str().is_some_and(|text| !text.trim().is_empty()))
        && fields.values().all(valid_unit_annotations)
    }
    Value::Array(items) => items.iter().all(valid_unit_annotations),
    _ => true,
  }
}

fn valid_path(path: &str) -> bool {
  path.split('/').count() == 4
    && path.split('/').all(|part| {
      !part.is_empty()
        && part.len() <= 48
        && part
          .bytes()
          .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_')
    })
}

fn fixed(data_type: &str, data_class: Option<&str>, output: &str, input: &str) -> bool {
  matches!(
    data_type,
    "base_movement"
      | "manipulator_movement"
      | "control_request"
      | "command_outcome"
      | "skill_outcome"
      | "health"
      | "status"
  ) || data_class == Some("command")
    || [output, input].iter().any(|name| {
      matches!(
        *name,
        "stop" | "result" | "outcome" | "health" | "status" | "readiness" | "latch"
      )
    })
}

fn routed_camera(
  source: &Node<'_>,
  output: &str,
  target: &Node<'_>,
  input: &str,
  output_port: &Value,
  input_port: &Value,
) -> bool {
  matches!(
    source.value["node_type"].as_str(),
    Some("eos.hardware.camera@v0.1" | "isos.eos_hardware.camera@v0.2")
  ) && output == "frame"
    && matches!(
      target.value["node_type"].as_str(),
      Some("eos.interface.camera_gate@v0.1" | "isos.eos_interface.camera_gate@v0.2")
    )
    && input == "frame"
    && output_port["data_type"] == "camera_frame"
    && input_port["data_type"] == "camera_frame"
    && output_port["transport"] == "in_process"
    && input_port["transport"] == "in_process"
    && output_port["data_class"] == "image"
    && input_port["data_class"] == "image"
}

fn routed_egia_evidence(
  catalog: &Value,
  source: &Node<'_>,
  output: &str,
  target: &Node<'_>,
  input: &str,
  output_port: &Value,
  input_port: &Value,
) -> bool {
  let source_type = source.value["node_type"].as_str();
  let target_type = target.value["node_type"].as_str();
  let pair = match (source_type, output, target_type, input) {
    (
      Some("egia.link.gateway@v0.1"),
      "resource",
      Some("egia.perception.admission@v0.1"),
      "resource",
    ) => Some("egia.stage.resource@v0.1"),
    (
      Some("egia.perception.admission@v0.1"),
      "observations",
      Some("egia.context.builder@v0.1"),
      "observations",
    ) => Some("egia.stage.observations@v0.1"),
    (
      Some("isos.ei_io.gateway@v0.2"),
      "resource",
      Some("isos.ei_evidence.admission@v0.2"),
      "resource",
    ) => Some("egia.stage.resource@v0.1"),
    (
      Some("isos.ei_evidence.admission@v0.2"),
      "observations",
      Some("isos.ei_cognition.builder@v0.2"),
      "observations",
    ) => Some("egia.stage.observations@v0.1"),
    _ => None,
  };
  pair.is_some_and(|data_type| {
    matches!(source.owner.as_str(), "egia" | "ei")
      && source.owner == target.owner
      && output_port["data_type"] == data_type
      && input_port["data_type"] == data_type
      && output_port["transport"] == "in_process"
      && input_port["transport"] == "in_process"
      && output_port["data_class"] == "evidence"
      && input_port["data_class"] == "evidence"
      && catalog[if source.owner == "ei" {
        "ei_routed_edges"
      } else {
        "egia_routed_edges"
      }]
      .as_array()
      .is_some_and(|routes| {
        routes.iter().any(|route| {
          route["source_type"] == source_type.unwrap()
            && route["output"] == output
            && route["target_type"] == target_type.unwrap()
            && route["input"] == input
            && route["controls"] == serde_json::json!(["priority", "frequency_hz"])
        })
      })
  })
}

fn visit(
  path: &str,
  successors: &BTreeMap<String, Vec<(String, String)>>,
  active: &mut BTreeSet<String>,
  done: &mut BTreeSet<String>,
) -> Result<(), Box<Diagnostic>> {
  if done.contains(path) {
    return Ok(());
  }
  if !active.insert(path.into()) {
    return Err(error(
      "/",
      "edge_cycle",
      format!("edge cycle reaches {path}"),
    ));
  }
  if let Some(next) = successors.get(path) {
    for (target, pointer) in next {
      if active.contains(target) {
        return Err(error(
          pointer,
          "edge_cycle",
          format!("edge cycle reaches {target}"),
        ));
      }
      visit(target, successors, active, done)?;
    }
  }
  active.remove(path);
  done.insert(path.into());
  Ok(())
}

/// Validate all graph edges using exact owner type and data schemas.
///
/// The resulting list is available only when the whole graph is admitted. It
/// describes declarations, not live health or delivery.
pub fn admit(graph: &Value, catalog: &Value) -> Result<Vec<Edge>, Box<Diagnostic>> {
  if graph["version"] != "0.1.0" && graph["version"] != "0.2.0" {
    return Err(error(
      "/version",
      "unsupported_graph_version",
      "edge admission requires graph 0.1.0 or 0.2.0",
    ));
  }
  let types = catalog["types"].as_array().ok_or_else(|| {
    error(
      "/cores",
      "owner_catalog_unavailable",
      "owner catalog has no types",
    )
  })?;
  let data_types = catalog["data_types"].as_array().ok_or_else(|| {
    error(
      "/cores",
      "owner_catalog_unavailable",
      "owner catalog has no data types",
    )
  })?;
  let mut definitions = BTreeMap::<&str, &Value>::new();
  let mut ambiguous = BTreeSet::new();
  for definition in types {
    let id = definition["id"].as_str().ok_or_else(|| {
      error(
        "/cores",
        "owner_catalog_unavailable",
        "node type lacks an ID",
      )
    })?;
    if definitions.insert(id, definition).is_some() {
      ambiguous.insert(id);
    }
  }
  let mut data_definitions = BTreeMap::<&str, &Value>::new();
  let mut ambiguous_data = BTreeSet::new();
  for definition in data_types {
    let id = definition["id"].as_str().ok_or_else(|| {
      error(
        "/cores",
        "owner_catalog_unavailable",
        "data type lacks an ID",
      )
    })?;
    if data_definitions.insert(id, definition).is_some() {
      ambiguous_data.insert(id);
    }
  }
  let cores = graph["cores"]
    .as_object()
    .ok_or_else(|| error("/cores", "invalid_graph", "cores must be an object"))?;
  let mut nodes = BTreeMap::<String, Node<'_>>::new();
  for (core_id, core) in cores {
    let core_pointer = format!("/cores/{}", escaped(core_id));
    let layers = core["layers"].as_object().ok_or_else(|| {
      error(
        format!("{core_pointer}/layers"),
        "invalid_graph",
        "layers must be an object",
      )
    })?;
    for (layer_id, layer) in layers {
      let layer_pointer = format!("{core_pointer}/layers/{}", escaped(layer_id));
      let groups = layer["groups"].as_object().ok_or_else(|| {
        error(
          format!("{layer_pointer}/groups"),
          "invalid_graph",
          "groups must be an object",
        )
      })?;
      for (group_id, group) in groups {
        let group_pointer = format!("{layer_pointer}/groups/{}", escaped(group_id));
        let entries = group["nodes"].as_object().ok_or_else(|| {
          error(
            format!("{group_pointer}/nodes"),
            "invalid_graph",
            "nodes must be an object",
          )
        })?;
        for (node_id, value) in entries {
          let pointer = format!("{group_pointer}/nodes/{}", escaped(node_id));
          let path = format!("{core_id}/{layer_id}/{group_id}/{node_id}");
          if !valid_path(&path) || value["id"] != *node_id {
            return Err(error(
              &pointer,
              "edge_placement",
              "node path or instance ID is invalid",
            ));
          }
          let type_id = value["node_type"].as_str().ok_or_else(|| {
            error(
              format!("{pointer}/node_type"),
              "unsupported_node_type",
              "node type is missing",
            )
          })?;
          if ambiguous.contains(type_id) {
            return Err(error(
              format!("{pointer}/node_type"),
              "owner_catalog_ambiguous",
              format!("type {type_id} is registered more than once"),
            ));
          }
          let definition = definitions.get(type_id).ok_or_else(|| {
            error(
              format!("{pointer}/node_type"),
              "unsupported_node_type",
              format!("type {type_id} is not registered"),
            )
          })?;
          if definition["core"] != *core_id
            || definition["layer"] != *layer_id
            || definition["group"] != *group_id
          {
            return Err(error(
              format!("{pointer}/node_type"),
              "edge_placement",
              format!("type {type_id} does not belong at {path}"),
            ));
          }
          nodes.insert(
            path.clone(),
            Node {
              path,
              pointer,
              core: core_id.clone(),
              owner: if graph["version"] == "0.2.0" {
                if layer_id.starts_with("eos_") {
                  "eos"
                } else if layer_id.starts_with("ei_") {
                  "ei"
                } else if layer_id == "ui" {
                  "ui"
                } else {
                  "isos"
                }
              } else {
                core_id
              }
              .to_owned(),
              value,
              definition,
            },
          );
        }
      }
    }
  }

  let mut edges = Vec::new();
  let mut successors = BTreeMap::<String, Vec<(String, String)>>::new();
  let mut binding_targets = BTreeMap::<String, Vec<String>>::new();
  for target in nodes.values() {
    if let Some(references) = target.value.get("references") {
      let values = references.as_array().ok_or_else(|| {
        error(
          format!("{}/references", target.pointer),
          "edge_reference",
          "references must be an array",
        )
      })?;
      let mut seen = BTreeSet::new();
      for (index, reference) in values.iter().enumerate() {
        let pointer = format!("{}/references/{index}", target.pointer);
        let path = reference.as_str().ok_or_else(|| {
          error(
            &pointer,
            "edge_reference",
            "reference must be a canonical node path",
          )
        })?;
        let referent = nodes.get(path).ok_or_else(|| {
          error(
            &pointer,
            "edge_reference",
            format!("reference {path} does not exist"),
          )
        })?;
        if path == target.path || !seen.insert(path) {
          return Err(error(
            pointer,
            "edge_reference",
            "reference is self or duplicated",
          ));
        }
        if referent.core != target.core {
          return Err(error(
            pointer,
            "cross_core_reference",
            "cross-core reference has no declared M1 purpose",
          ));
        }
      }
    }
    let empty_inputs = serde_json::Map::new();
    let inputs = match target.value.get("inputs") {
      Some(value) => value.as_object().ok_or_else(|| {
        error(
          format!("{}/inputs", target.pointer),
          "edge_input",
          "inputs must be an object",
        )
      })?,
      None => &empty_inputs,
    };
    for required in target.definition["inputs"].as_array().into_iter().flatten() {
      if required["required"] == true {
        let name = required["name"].as_str().ok_or_else(|| {
          error(
            &target.pointer,
            "owner_catalog_unavailable",
            "registered input lacks a name",
          )
        })?;
        if !inputs.contains_key(name) {
          return Err(error(
            format!("{}/inputs/{}", target.pointer, escaped(name)),
            "edge_required_input",
            format!("required input {name} is not connected"),
          ));
        }
      }
    }
    for (input_name, raw_sources) in inputs {
      let base = format!("{}/inputs/{}", target.pointer, escaped(input_name));
      let input = port(target.definition, "inputs", input_name).ok_or_else(|| {
        error(
          &base,
          "edge_unknown_input",
          format!("{} has no registered input {input_name}", target.path),
        )
      })?;
      let sources: Vec<(String, &Value)> = if let Some(values) = raw_sources.as_array() {
        values
          .iter()
          .enumerate()
          .map(|(index, value)| (format!("{base}/{index}"), value))
          .collect()
      } else {
        vec![(base.clone(), raw_sources)]
      };
      let one = match input["cardinality"].as_str().unwrap_or("one") {
        "one" => true,
        "many" => false,
        _ => {
          return Err(error(
            &base,
            "owner_catalog_unavailable",
            "registered input cardinality is invalid",
          ));
        }
      };
      if sources.is_empty() || (one && sources.len() != 1) {
        return Err(error(
          &base,
          "edge_cardinality",
          "input source count violates registered cardinality",
        ));
      }
      let mut seen = BTreeSet::new();
      for (pointer, source_value) in sources {
        let source_object = source_value
          .as_object()
          .ok_or_else(|| error(&pointer, "edge_source", "source must be an object"))?;
        for key in source_object.keys() {
          if !matches!(
            key.as_str(),
            "node" | "output" | "priority" | "frequency_hz"
          ) {
            return Err(error(
              format!("{pointer}/{}", escaped(key)),
              "edge_source_field",
              format!("source field {key} is not registered"),
            ));
          }
        }
        let source_path = source_value["node"].as_str().ok_or_else(|| {
          error(
            &pointer,
            "edge_source",
            "source node must be a canonical path",
          )
        })?;
        let output_name = source_value["output"]
          .as_str()
          .ok_or_else(|| error(&pointer, "edge_unknown_output", "source output is missing"))?;
        if !valid_path(source_path) {
          return Err(edge_error(
            &pointer,
            "edge_source",
            "source node path is not canonical",
            source_path,
            &target.path,
            input_name,
            None,
            input["data_type"].as_str(),
          ));
        }
        let source = nodes.get(source_path).ok_or_else(|| {
          edge_error(
            &pointer,
            "edge_source",
            format!("source node {source_path} does not exist"),
            source_path,
            &target.path,
            input_name,
            None,
            input["data_type"].as_str(),
          )
        })?;
        if source.path == target.path || !seen.insert((source_path, output_name)) {
          return Err(edge_error(
            &pointer,
            "edge_duplicate_source",
            "source is self or duplicated",
            source_path,
            &target.path,
            input_name,
            None,
            input["data_type"].as_str(),
          ));
        }
        let output = port(source.definition, "outputs", output_name).ok_or_else(|| {
          edge_error(
            &pointer,
            "edge_unknown_output",
            format!("{source_path} has no registered output {output_name}"),
            source_path,
            &target.path,
            input_name,
            None,
            input["data_type"].as_str(),
          )
        })?;
        let output_type = output["data_type"].as_str();
        let input_type = input["data_type"].as_str();
        if (source.core == "egia" || target.core == "egia")
          && (output["fixed"] == true || input["fixed"] == true)
        {
          return Err(edge_error(
            &pointer,
            "edge_fixed_route",
            "EGIA fixed runtime port cannot be graph wired",
            source_path,
            &target.path,
            input_name,
            output_type,
            input_type,
          ));
        }
        if output_type.is_none() || input_type.is_none() {
          return Err(edge_error(
            &pointer,
            "owner_catalog_unavailable",
            "registered port lacks a data type",
            source_path,
            &target.path,
            input_name,
            output_type,
            input_type,
          ));
        }
        let output_type = output_type.unwrap();
        let input_type = input_type.unwrap();
        if output_type != input_type {
          return Err(edge_error(
            &pointer,
            "edge_type_mismatch",
            format!(
              "{source_path}.{output_name} ({output_type}) cannot feed {}.{input_name} ({input_type}); no unit conversion is defined",
              target.path
            ),
            source_path,
            &target.path,
            input_name,
            Some(output_type),
            Some(input_type),
          ));
        }
        if !data_definitions.get(output_type).is_some_and(|definition| {
          definition["schema"].is_object() && valid_unit_annotations(&definition["schema"])
        }) || ambiguous_data.contains(output_type)
        {
          return Err(edge_error(
            &pointer,
            "edge_unit_schema",
            format!("data type {output_type} has no unique registered unit schema"),
            source_path,
            &target.path,
            input_name,
            Some(output_type),
            Some(input_type),
          ));
        }
        let output_transport = transport(output).ok_or_else(|| {
          edge_error(
            &pointer,
            "edge_transport",
            "source transport is invalid",
            source_path,
            &target.path,
            input_name,
            Some(output_type),
            Some(input_type),
          )
        })?;
        let input_transport = transport(input).ok_or_else(|| {
          edge_error(
            &pointer,
            "edge_transport",
            "input transport is invalid",
            source_path,
            &target.path,
            input_name,
            Some(output_type),
            Some(input_type),
          )
        })?;
        if output_transport != input_transport
          || !matches!(
            input_transport,
            "in_process" | "unix_socket" | "websocket" | "ros2" | "http"
          )
        {
          return Err(edge_error(
            &pointer,
            "edge_transport",
            "source and input transports differ or are unavailable",
            source_path,
            &target.path,
            input_name,
            Some(output_type),
            Some(input_type),
          ));
        }
        let output_mode = output["delivery_mode"].as_str().unwrap_or("message");
        let input_mode = input["delivery_mode"].as_str().unwrap_or("message");
        if !matches!(output_mode, "message" | "event" | "stream") || output_mode != input_mode {
          return Err(edge_error(
            &pointer,
            "edge_delivery_mode",
            "source and input delivery modes differ or are invalid",
            source_path,
            &target.path,
            input_name,
            Some(output_type),
            Some(input_type),
          ));
        }
        let cross_core = source.owner != target.owner;
        let bridged = input_transport == "http"
          && (bridge_port(catalog, source, "outputs", output_name, "http")
            || bridge_port(catalog, target, "inputs", input_name, "http"));
        if cross_core && !bridged {
          return Err(edge_error(
            &pointer,
            "edge_bridge_undeclared",
            "cross-core edge has no trusted declared bridge",
            source_path,
            &target.path,
            input_name,
            Some(output_type),
            Some(input_type),
          ));
        }
        if !cross_core && input_transport != "in_process" && !bridged {
          return Err(edge_error(
            &pointer,
            "edge_transport",
            "local graph edge transport is unavailable",
            source_path,
            &target.path,
            input_name,
            Some(output_type),
            Some(input_type),
          ));
        }
        let output_class = output["data_class"].as_str();
        let input_class = input["data_class"].as_str();
        if output_class.is_some() && input_class.is_some() && output_class != input_class {
          return Err(edge_error(
            &pointer,
            "edge_data_class",
            "source and input data classes differ",
            source_path,
            &target.path,
            input_name,
            Some(output_type),
            Some(input_type),
          ));
        }
        let data_class = output_class.or(input_class);
        let is_fixed = fixed(output_type, data_class, output_name, input_name);
        let is_routed = source.owner == "eos"
          && target.owner == "eos"
          && routed_camera(source, output_name, target, input_name, output, input)
          || routed_egia_evidence(
            catalog,
            source,
            output_name,
            target,
            input_name,
            output,
            input,
          );
        let route_class = if cross_core && bridged {
          "owner_bridge"
        } else if target.owner == "eos" && (is_fixed || is_routed) {
          "eos_local"
        } else if target.owner == "ei" {
          "ei_local"
        } else if target.owner == "egia" {
          "egia_local"
        } else {
          "design_only"
        };
        let priority = if let Some(value) = source_value.get("priority") {
          let priority = value.as_i64().ok_or_else(|| {
            edge_error(
              &format!("{pointer}/priority"),
              "edge_priority",
              "priority must be an integer in -20..19",
              source_path,
              &target.path,
              input_name,
              Some(output_type),
              Some(input_type),
            )
          })?;
          if !(-20..=19).contains(&priority) {
            return Err(edge_error(
              &format!("{pointer}/priority"),
              "edge_priority",
              "priority must be -20..19",
              source_path,
              &target.path,
              input_name,
              Some(output_type),
              Some(input_type),
            ));
          }
          if !is_routed {
            return Err(edge_error(
              &format!("{pointer}/priority"),
              "edge_qos_forbidden",
              "user priority is forbidden on a fixed or unrouted edge",
              source_path,
              &target.path,
              input_name,
              Some(output_type),
              Some(input_type),
            ));
          }
          priority
        } else {
          0
        };
        let frequency_hz = if let Some(value) = source_value.get("frequency_hz") {
          let rate = value.as_f64().ok_or_else(|| {
            edge_error(
              &format!("{pointer}/frequency_hz"),
              "edge_frequency",
              "frequency_hz must be a finite positive number",
              source_path,
              &target.path,
              input_name,
              Some(output_type),
              Some(input_type),
            )
          })?;
          if !rate.is_finite() || rate <= 0.0 {
            return Err(edge_error(
              &format!("{pointer}/frequency_hz"),
              "edge_frequency",
              "frequency_hz must be finite and positive",
              source_path,
              &target.path,
              input_name,
              Some(output_type),
              Some(input_type),
            ));
          }
          if !is_routed {
            return Err(edge_error(
              &format!("{pointer}/frequency_hz"),
              "edge_qos_forbidden",
              "user frequency cap is forbidden on a fixed or unrouted edge",
              source_path,
              &target.path,
              input_name,
              Some(output_type),
              Some(input_type),
            ));
          }
          Some(rate)
        } else {
          None
        };
        let command_input = input_name == "command"
          && matches!(
            target.value["node_type"].as_str(),
            Some(
              "eos.hardware.base@v0.1"
                | "eos.hardware.manipulator@v0.1"
                | "isos.eos_hardware.base@v0.2"
                | "isos.eos_hardware.manipulator@v0.2"
            )
          );
        if command_input {
          let expected_binding = match target.value["node_type"].as_str() {
            Some("eos.hardware.base@v0.1") => "eos.dispatcher.primitive_base@v0.1",
            Some("eos.hardware.manipulator@v0.1") => "eos.dispatcher.primitive_manipulator@v0.1",
            Some("isos.eos_hardware.base@v0.2") => "isos.eos_dispatcher.primitive_base@v0.2",
            _ => "isos.eos_dispatcher.primitive_manipulator@v0.2",
          };
          if source.value["node_type"] != expected_binding || output_name != "command" {
            return Err(edge_error(
              &pointer,
              "edge_actuator_bypass",
              "actuator command must come from its exact Dispatcher primitive binding",
              source_path,
              &target.path,
              input_name,
              Some(output_type),
              Some(input_type),
            ));
          }
          binding_targets
            .entry(source_path.into())
            .or_default()
            .push(pointer.clone());
        }
        successors
          .entry(source_path.into())
          .or_default()
          .push((target.path.clone(), pointer.clone()));
        edges.push(Edge {
          path: pointer,
          source: source_path.into(),
          target: target.path.clone(),
          input: input_name.clone(),
          route_class,
          fixed: is_fixed,
          qos_eligible: is_routed,
          routed: is_routed,
          priority,
          frequency_hz,
          data_type: output_type.into(),
          transport: input_transport.into(),
          delivery_mode: input_mode.into(),
        });
      }
    }
  }
  for (path, node) in &nodes {
    if matches!(
      node.value["node_type"].as_str(),
      Some(
        "eos.dispatcher.primitive_base@v0.1"
          | "eos.dispatcher.primitive_manipulator@v0.1"
          | "isos.eos_dispatcher.primitive_base@v0.2"
          | "isos.eos_dispatcher.primitive_manipulator@v0.2"
      )
    ) {
      let count = binding_targets.get(path).map_or(0, Vec::len);
      if count != 1 {
        return Err(error(
          format!("{}/node_type", node.pointer),
          "edge_binding_target",
          format!("primitive binding {path} must target exactly one actuator"),
        ));
      }
    }
  }
  let mut done = BTreeSet::new();
  for path in nodes.keys() {
    visit(path, &successors, &mut BTreeSet::new(), &mut done)?;
  }
  edges.sort_by(|left, right| left.path.cmp(&right.path));
  Ok(edges)
}
