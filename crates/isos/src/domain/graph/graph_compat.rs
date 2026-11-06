//! Exact-version inspection of saved deployment graph bytes.

use anyhow::{Result, bail, ensure};
use serde::Serialize;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{collections::BTreeMap, fs, path::Path};

pub const MAX_GRAPH_BYTES: usize = 1024 * 1024;
pub const SUPPORTED_VERSIONS: [&str; 3] = ["1.0.0", "0.1.0", "0.2.0"];

pub fn uses_revisioned_generation(graph: &Value) -> bool {
  matches!(graph["version"].as_str(), Some("0.1.0" | "0.2.0"))
}

const KNOWN_LEGACY_TYPES: &[&str] = &[
  "eos.hardware.camera@v1.0",
  "eos.hardware.base@v1.0",
  "eos.hardware.manipulator@v1.0",
  "eos.dispatcher.control@v1.0",
  "eos.dispatcher.primitive_base@v1.0",
  "eos.dispatcher.primitive_manipulator@v1.0",
  "eos.control.manual@v1.0",
  "eos.control.primitive@v1.0",
  "eos.control.skill@v1.0",
  "eos.interface.camera_gate@v1.0",
  "egia.common.source@v1.0",
  "egia.common.context@v1.0",
  "egia.common.planner@v1.0",
  "egia.common.guard@v1.0",
  "isos.process.component@v1.1",
  "isos.ui.vision@v1.0",
  "isosui.system.vision@v1.0",
];
const KNOWN_NEW_TYPES: &[&str] = &[
  "eos.hardware.camera@v0.1",
  "eos.hardware.base@v0.1",
  "eos.hardware.manipulator@v0.1",
  "eos.dispatcher.control@v0.1",
  "eos.dispatcher.primitive_base@v0.1",
  "eos.dispatcher.primitive_manipulator@v0.1",
  "eos.interface.camera_gate@v0.1",
  "isos.process.component@v0.1",
  "egia.link.gateway@v0.1",
  "egia.perception.admission@v0.1",
  "egia.context.builder@v0.1",
  "egia.missions.queue@v0.1",
  "egia.decision.planner@v0.1",
  "egia.safety.gate@v0.1",
  "egia.monitor.health@v0.1",
  "isosui.input.keyboard@v0.1",
  "isos.interface.http_gateway@v0.1",
  "eos.interface.http_gateway@v0.1",
];
const EGIA_TARGET_PASS_THROUGH: &[&str] = &[
  "egia.common.source@v1.0",
  "egia.common.context@v1.0",
  "egia.common.planner@v1.0",
  "egia.common.guard@v1.0",
];

pub(crate) fn target_placement(
  node_type: &str,
) -> Option<(&'static str, &'static str, &'static str)> {
  match node_type {
    "eos.hardware.camera@v0.1" => Some(("eos", "hardware", "camera")),
    "eos.hardware.base@v0.1" => Some(("eos", "hardware", "base")),
    "eos.hardware.manipulator@v0.1" => Some(("eos", "hardware", "manipulator")),
    "eos.dispatcher.control@v0.1" => Some(("eos", "dispatcher", "control")),
    "eos.dispatcher.primitive_base@v0.1" => Some(("eos", "dispatcher", "base")),
    "eos.dispatcher.primitive_manipulator@v0.1" => Some(("eos", "dispatcher", "manipulator")),
    "eos.interface.camera_gate@v0.1" => Some(("eos", "interface", "gate")),
    "eos.interface.http_gateway@v0.1" => Some(("eos", "interface", "gateway")),
    "isos.interface.http_gateway@v0.1" => Some(("isos", "interface", "gateway")),
    "isosui.input.keyboard@v0.1" => Some(("isosui", "input", "keyboard")),
    "isos.process.component@v0.1" => Some(("isos", "process", "component")),
    "egia.link.gateway@v0.1" => Some(("egia", "io", "link")),
    "egia.perception.admission@v0.1" => Some(("egia", "evidence", "perception")),
    "egia.context.builder@v0.1" => Some(("egia", "cognition", "context")),
    "egia.missions.queue@v0.1" => Some(("egia", "cognition", "missions")),
    "egia.decision.planner@v0.1" => Some(("egia", "cognition", "decision")),
    "egia.safety.gate@v0.1" => Some(("egia", "governance", "safety")),
    "egia.monitor.health@v0.1" => Some(("egia", "governance", "monitor")),
    "egia.common.source@v1.0" | "egia.common.context@v1.0" => Some(("egia", "common", "data")),
    "egia.common.planner@v1.0" | "egia.common.guard@v1.0" => Some(("egia", "common", "process")),
    _ => None,
  }
}

pub fn is_isos_target_type(node_type: &str) -> bool {
  node_type == "isos.process.component@v0.1"
}

pub(crate) fn has_egia_stages(graph: &Value) -> bool {
  graph["cores"]["egia"]["layers"]
    .as_object()
    .into_iter()
    .flat_map(|layers| layers.values())
    .flat_map(|layer| {
      layer["groups"]
        .as_object()
        .into_iter()
        .flat_map(|groups| groups.values())
    })
    .flat_map(|group| {
      group["nodes"]
        .as_object()
        .into_iter()
        .flat_map(|nodes| nodes.values())
    })
    .any(|node| {
      node["node_type"]
        .as_str()
        .is_some_and(|id| id.starts_with("egia.") && id.ends_with("@v0.1"))
    })
}

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
pub struct Diagnostic {
  pub path: String,
  pub code: &'static str,
  pub message: String,
  #[serde(flatten)]
  pub context: BTreeMap<String, Value>,
}

impl std::fmt::Display for Diagnostic {
  fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    if self.context.is_empty() {
      write!(formatter, "{}: {}: {}", self.path, self.code, self.message)
    } else {
      write!(
        formatter,
        "{}",
        serde_json::to_string(self).map_err(|_| std::fmt::Error)?
      )
    }
  }
}
impl std::error::Error for Diagnostic {}

#[derive(Debug)]
pub struct SavedGraph {
  pub value: Value,
  pub revision: String,
  pub read_only: bool,
  pub diagnostics: Vec<Diagnostic>,
}

#[derive(Clone, Debug, Serialize)]
pub struct LegacyProjection {
  pub read_only: bool,
  pub source_revision: String,
  pub graph: Value,
  pub mappings: Vec<Value>,
  pub defaults: Vec<Value>,
  pub display_descriptions: BTreeMap<String, String>,
}

fn diagnostic(path: String, code: &'static str, message: String) -> Diagnostic {
  Diagnostic {
    path,
    code,
    message,
    context: BTreeMap::new(),
  }
}

pub(crate) fn pointer_segment(segment: &str) -> String {
  segment.replace('~', "~0").replace('/', "~1")
}

fn valid_path_segment(segment: &str) -> bool {
  !segment.is_empty()
    && segment
      .bytes()
      .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'-')
}

/// Inspect a legacy graph without ever replacing its saved value or revision.
pub fn legacy_projection(
  graph: &Value,
  source_revision: &str,
  catalog: Option<&Value>,
) -> Result<Option<LegacyProjection>, Diagnostic> {
  if graph["version"] != "1.0.0" {
    return version_diagnostic(graph).map_or(Ok(None), Err);
  }
  if let Some(problem) = admission_diagnostic(graph, catalog) {
    return Err(problem);
  }
  // The proposal is discarded. It supplies the one G03 mapping and field-shape
  // boundary, so inspection cannot silently admit a type migration rejects.
  let proposal = crate::domain::graph::graph_migration::propose_for_inspection(&SavedGraph {
    value: graph.clone(),
    revision: source_revision.to_owned(),
    read_only: true,
    diagnostics: Vec::new(),
  })?;
  let robot = graph["robot"].as_object().ok_or_else(|| {
    diagnostic(
      "/robot".into(),
      "invalid_graph",
      "robot must be an object".into(),
    )
  })?;
  for field in ["id", "name"] {
    if !robot
      .get(field)
      .and_then(Value::as_str)
      .is_some_and(|value| !value.trim().is_empty())
    {
      return Err(diagnostic(
        format!("/robot/{field}"),
        "invalid_graph",
        format!("robot {field} must be a nonblank string"),
      ));
    }
  }
  let mut projected = graph.clone();
  let mut mappings = Vec::new();
  let mut defaults = Vec::new();
  let mut display_descriptions = BTreeMap::new();
  let cores = graph["cores"].as_object().expect("proposal checked cores");
  if cores.is_empty() {
    return Err(diagnostic(
      "/cores".into(),
      "invalid_graph",
      "legacy graph requires a registered core".into(),
    ));
  }
  for (core_id, core) in cores {
    if !matches!(core_id.as_str(), "eos" | "isos" | "egia") {
      return Err(diagnostic(
        format!("/cores/{}", pointer_segment(core_id)),
        "unsupported_core",
        format!("core {core_id} has no registered legacy owner"),
      ));
    }
    for (layer_id, layer) in core["layers"].as_object().expect("proposal checked layers") {
      if !valid_path_segment(layer_id) {
        return Err(diagnostic(
          format!("/cores/{core_id}/layers/{}", pointer_segment(layer_id)),
          "invalid_graph",
          "layer key is not a valid graph path segment".into(),
        ));
      }
      for (group_id, group) in layer["groups"]
        .as_object()
        .expect("proposal checked groups")
      {
        if !valid_path_segment(group_id) {
          return Err(diagnostic(
            format!(
              "/cores/{core_id}/layers/{layer_id}/groups/{}",
              pointer_segment(group_id)
            ),
            "invalid_graph",
            "group key is not a valid graph path segment".into(),
          ));
        }
        for (node_id, node) in group["nodes"].as_object().expect("proposal checked nodes") {
          let path = format!(
            "/cores/{}/layers/{}/groups/{}/nodes/{}",
            pointer_segment(core_id),
            pointer_segment(layer_id),
            pointer_segment(group_id),
            pointer_segment(node_id)
          );
          if !valid_path_segment(node_id) {
            return Err(diagnostic(
              path,
              "invalid_graph",
              "node key is not a valid graph path segment".into(),
            ));
          }
          let source_type = node["node_type"].as_str().unwrap_or("");
          let candidate_type = proposal.graph["cores"][core_id]["layers"][layer_id]["groups"]
            [group_id]["nodes"][node_id]["node_type"]
            .as_str()
            .unwrap_or(source_type);
          if let Some((expected_core, expected_layer, expected_group)) =
            target_placement(candidate_type)
            && (core_id.as_str(), layer_id.as_str(), group_id.as_str())
              != (expected_core, expected_layer, expected_group)
          {
            return Err(diagnostic(
              format!("{path}/node_type"),
              "invalid_node_placement",
              format!("{source_type} belongs at {expected_core}/{expected_layer}/{expected_group}"),
            ));
          }
          let owner_type = catalog
            .and_then(|catalog| catalog["types"].as_array())
            .and_then(|types| types.iter().find(|item| item["id"] == source_type));
          if let Some(owner_type) = owner_type {
            if owner_type["core"] != *core_id
              || owner_type["layer"] != *layer_id
              || owner_type["group"] != *group_id
            {
              return Err(diagnostic(
                format!("{path}/node_type"),
                "invalid_node_placement",
                format!(
                  "{source_type} is registered at {}/{}/{}",
                  owner_type["core"].as_str().unwrap_or("?"),
                  owner_type["layer"].as_str().unwrap_or("?"),
                  owner_type["group"].as_str().unwrap_or("?")
                ),
              ));
            }
            if let Some(description) = owner_type["description"].as_str() {
              display_descriptions.insert(path.clone(), description.to_owned());
            }
          }
          mappings.push(json!({"path":format!("{path}/node_type"),
                        "source_type":source_type,"candidate_type":candidate_type}));
          let projected_node = &mut projected["cores"][core_id]["layers"][layer_id]["groups"]
            [group_id]["nodes"][node_id];
          let object = projected_node.as_object_mut().ok_or_else(|| {
            diagnostic(
              path.clone(),
              "unmapped_graph_field",
              "node must be an object".into(),
            )
          })?;
          for field in ["id", "label", "description"] {
            if !object.get(field).is_some_and(Value::is_string) {
              return Err(diagnostic(
                format!("{path}/{field}"),
                "unmapped_graph_field",
                format!("{field} must be a string"),
              ));
            }
          }
          if object["id"] != *node_id {
            return Err(diagnostic(
              format!("{path}/id"),
              "invalid_graph",
              "node id must match its graph path key".into(),
            ));
          }
          for (field, correct) in [
            ("capabilities", Value::is_array as fn(&Value) -> bool),
            ("inputs", Value::is_object),
            ("references", Value::is_array),
            ("settings", Value::is_object),
          ] {
            if object.get(field).is_some_and(|value| !correct(value)) {
              return Err(diagnostic(
                format!("{path}/{field}"),
                "unmapped_graph_field",
                format!("{field} has invalid shape"),
              ));
            }
          }
          for (field, value) in [
            ("capabilities", json!([])),
            ("inputs", json!({})),
            ("references", json!([])),
            ("settings", json!({})),
          ] {
            if !object.contains_key(field) {
              object.insert(field.into(), value.clone());
              defaults.push(json!({"path":format!("{path}/{field}"),"value":value}));
            }
          }
          if source_type == "isos.process.component@v1.1" {
            let settings = object["settings"].as_object_mut().ok_or_else(|| {
              diagnostic(
                format!("{path}/settings"),
                "unmapped_graph_field",
                "settings must be an object".into(),
              )
            })?;
            if !settings
              .get("role")
              .and_then(Value::as_str)
              .is_some_and(|role| !role.is_empty())
            {
              return Err(diagnostic(
                format!("{path}/settings/role"),
                "unmapped_graph_field",
                "role is required for a role-selected component".into(),
              ));
            }
            serde_json::from_value::<crate::runtime::process::ProcessSettings>(Value::Object(
              settings.clone(),
            ))
            .map_err(|error| {
              diagnostic(
                format!("{path}/settings"),
                "unmapped_graph_field",
                format!("invalid component settings: {error}"),
              )
            })?;
            for (field, value) in [
              ("args", json!([])),
              ("env", json!({})),
              ("start_timeout_ms", json!(30_000)),
              ("restart", json!("never")),
              ("max_restarts", json!(0)),
              ("depends_on", json!([])),
            ] {
              if !settings.contains_key(field) {
                settings.insert(field.into(), value.clone());
                defaults.push(json!({"path":format!("{path}/settings/{field}"),"value":value}));
              }
            }
          }
        }
      }
    }
  }
  mappings.sort_by(|a, b| a["path"].as_str().cmp(&b["path"].as_str()));
  defaults.sort_by(|a, b| a["path"].as_str().cmp(&b["path"].as_str()));
  Ok(Some(LegacyProjection {
    read_only: true,
    source_revision: source_revision.to_owned(),
    graph: projected,
    mappings,
    defaults,
    display_descriptions,
  }))
}

pub fn legacy_mapping_diagnostic(graph: &Value, catalog: Option<&Value>) -> Option<Diagnostic> {
  if graph["version"] != "1.0.0" {
    return admission_diagnostic(graph, catalog);
  }
  legacy_projection(graph, "", catalog).err()
}

pub fn admit_legacy_mapped(graph: &Value, catalog: Option<&Value>) -> Result<()> {
  if graph["version"] != "1.0.0" {
    return admit(graph, catalog);
  }
  if let Some(problem) = legacy_mapping_diagnostic(graph, catalog) {
    bail!(problem);
  }
  Ok(())
}

pub fn projected_input_diagnostic(
  saved: &SavedGraph,
  incoming: &Value,
  catalog: Option<&Value>,
) -> Option<Diagnostic> {
  if saved.value["version"] != "1.0.0" || saved.value == *incoming {
    return None;
  }
  let Ok(Some(projection)) = legacy_projection(&saved.value, &saved.revision, catalog) else {
    return None;
  };
  (projection.graph == *incoming).then(|| diagnostic(
        "/version".into(),
        "projection_read_only",
        "legacy inspection projection cannot replace saved graph bytes; edit the original graph or use explicit migration".into(),
    ))
}

pub fn revision(bytes: &[u8]) -> String {
  hex::encode(Sha256::digest(bytes))
}

pub fn read(path: &Path) -> Result<SavedGraph> {
  let bytes = fs::read(path)?;
  parse(&bytes)
}

pub fn parse(bytes: &[u8]) -> Result<SavedGraph> {
  ensure!(bytes.len() <= MAX_GRAPH_BYTES, "node graph exceeds 1 MiB");
  let value: Value = serde_json::from_slice(bytes)?;
  ensure!(value.is_object(), "node graph must be a JSON object");
  let diagnostics = version_diagnostic(&value).into_iter().collect();
  Ok(SavedGraph {
    revision: revision(bytes),
    read_only: true,
    value,
    diagnostics,
  })
}

pub fn version_diagnostic(graph: &Value) -> Option<Diagnostic> {
  let version = graph.get("version").and_then(Value::as_str);
  if version.is_some_and(|version| SUPPORTED_VERSIONS.contains(&version)) {
    return None;
  }
  Some(Diagnostic {
    path: "/version".into(),
    code: "unsupported_graph_version",
    message: format!(
      "graph version {} is unsupported; supported graph versions: {}",
      version.unwrap_or("<missing or invalid>"),
      SUPPORTED_VERSIONS.join(", ")
    ),
    context: BTreeMap::new(),
  })
}

pub fn admission_diagnostic(graph: &Value, catalog: Option<&Value>) -> Option<Diagnostic> {
  if let Some(diagnostic) = version_diagnostic(graph) {
    return Some(diagnostic);
  }
  let target = graph["version"] == "0.1.0";
  let unified = graph["version"] == "0.2.0";
  let cores = match graph.get("cores").and_then(Value::as_object) {
    Some(cores) => cores,
    None => {
      return Some(Diagnostic {
        path: "/cores".into(),
        code: "invalid_graph",
        message: "cores must be an object".into(),
        context: BTreeMap::new(),
      });
    }
  };
  if unified && !cores.contains_key("isos") {
    return Some(diagnostic(
      "/cores/isos".into(),
      "invalid_graph",
      "graph 0.2.0 requires the isos core".into(),
    ));
  }
  for (core_id, core) in cores {
    if unified && core_id != "isos" {
      return Some(Diagnostic {
        path: format!("/cores/{}", pointer_segment(core_id)),
        code: "unsupported_core",
        message: "graph 0.2.0 has one isos core".into(),
        context: BTreeMap::new(),
      });
    }
    if target
      && !matches!(core_id.as_str(), "eos" | "isos" | "egia" | "isosui")
      && !catalog.is_some_and(|catalog| {
        catalog["types"]
          .as_array()
          .is_some_and(|types| types.iter().any(|entry| entry["core"] == *core_id))
      })
    {
      return Some(Diagnostic {
        path: format!("/cores/{}", pointer_segment(core_id)),
        code: "unsupported_core",
        message: format!("core {core_id} has no 0.1.0 owner reader"),
        context: BTreeMap::new(),
      });
    }
    let Some(layers) = core.get("layers").and_then(Value::as_object) else {
      continue;
    };
    for (layer_id, layer) in layers {
      let Some(groups) = layer.get("groups").and_then(Value::as_object) else {
        continue;
      };
      for (group_id, group) in groups {
        let Some(nodes) = group.get("nodes").and_then(Value::as_object) else {
          continue;
        };
        for (node_id, node) in nodes {
          let path = format!(
            "/cores/{}/layers/{}/groups/{}/nodes/{}",
            pointer_segment(core_id),
            pointer_segment(layer_id),
            pointer_segment(group_id),
            pointer_segment(node_id)
          );
          let node_type = node.get("node_type").and_then(Value::as_str).unwrap_or("");
          if unified {
            let expected_prefix = format!("isos.{layer_id}.");
            let registered = node_type.starts_with(&expected_prefix)
              && node_type.ends_with("@v0.2")
              && ((node_type == "isos.process.component@v0.2" && group_id == "component")
                || catalog.is_some_and(|catalog| {
                  catalog["types"].as_array().is_some_and(|types| {
                    types.iter().any(|entry| {
                      entry["id"] == node_type
                        && entry["core"] == "isos"
                        && entry["layer"] == *layer_id
                        && entry["group"] == *group_id
                    })
                  })
                }));
            if !registered {
              return Some(Diagnostic {
                path: format!("{path}/node_type"),
                code: "unsupported_node_type",
                message: format!("node type {node_type} has no registered 0.2.0 placement"),
                context: BTreeMap::new(),
              });
            }
          }
          if target {
            let owner_placement = catalog
              .and_then(|catalog| catalog["types"].as_array())
              .and_then(|types| types.iter().find(|entry| entry["id"] == node_type))
              .and_then(|entry| {
                Some((
                  entry["core"].as_str()?,
                  entry["layer"].as_str()?,
                  entry["group"].as_str()?,
                ))
              });
            let Some((expected_core, expected_layer, expected_group)) =
              owner_placement.or_else(|| target_placement(node_type))
            else {
              return Some(Diagnostic {
                path: format!("{path}/node_type"),
                code: "unsupported_node_type",
                message: format!("node type {node_type} is not approved for graph 0.1.0"),
                context: BTreeMap::new(),
              });
            };
            if (core_id.as_str(), layer_id.as_str(), group_id.as_str())
              != (expected_core, expected_layer, expected_group)
            {
              return Some(Diagnostic {
                path: format!("{path}/node_type"),
                code: "invalid_node_placement",
                message: format!(
                  "{node_type} belongs at {expected_core}/{expected_layer}/{expected_group}"
                ),
                context: BTreeMap::new(),
              });
            }
          }
          if node_type == "isos.process.component@v1.0" {
            return Some(Diagnostic {
              path: format!("{path}/node_type"),
              code: "unsafe_process_node",
              message:
                "arbitrary-command process nodes cannot be admitted; use a role-selected component"
                  .into(),
              context: BTreeMap::new(),
            });
          }
          if core_id == "isos"
            && layer_id == "process"
            && group_id == "component"
            && node.pointer("/settings/command").is_some()
          {
            return Some(Diagnostic {
              path: format!("{path}/settings/command"),
              code: "unsafe_process_node",
              message: "settings.command cannot be admitted; use role and bounded args".into(),
              context: BTreeMap::new(),
            });
          }
          if node_type == "eos.process.backend@v1.0" {
            return Some(Diagnostic {
              path: format!("{path}/node_type"),
              code: "migration_required",
              message: "legacy backend needs an exact verified replacement before Apply".into(),
              context: BTreeMap::new(),
            });
          }
          let registered = catalog.map_or_else(
            || KNOWN_LEGACY_TYPES.contains(&node_type) || KNOWN_NEW_TYPES.contains(&node_type),
            |catalog| {
              is_isos_target_type(node_type)
                || (graph["version"] == "0.1.0"
                  && (KNOWN_NEW_TYPES.contains(&node_type)
                    || EGIA_TARGET_PASS_THROUGH.contains(&node_type)))
                || catalog["types"]
                  .as_array()
                  .is_some_and(|types| types.iter().any(|item| item["id"] == node_type))
            },
          );
          if !registered {
            return Some(Diagnostic {
              path: format!("{path}/node_type"),
              code: "unsupported_node_type",
              message: format!("node type {node_type} is not registered"),
              context: BTreeMap::new(),
            });
          }
          if target
            && let Some(definition) = catalog
              .and_then(|catalog| catalog["types"].as_array())
              .and_then(|types| types.iter().find(|entry| entry["id"] == node_type))
            && definition["settings"].is_array()
            && let Err(error) =
              crate::domain::registry::node_schema::validate_instance_settings(node, definition)
          {
            return Some(Diagnostic {
              path: format!("{path}/settings"),
              code: "invalid_node_settings",
              message: error.to_string(),
              context: BTreeMap::new(),
            });
          }
        }
      }
    }
  }
  None
}

pub fn admit(graph: &Value, catalog: Option<&Value>) -> Result<()> {
  if let Some(diagnostic) = admission_diagnostic(graph, catalog) {
    bail!(diagnostic);
  }
  Ok(())
}

pub fn changed_legacy_diagnostic(saved: &SavedGraph, incoming: &Value) -> Option<Diagnostic> {
  (saved.value["version"] == "1.0.0" && saved.value != *incoming).then(|| Diagnostic {
    path: "/version".into(),
    code: "migration_required",
    message: "editing a saved 1.0.0 graph requires explicit migration".into(),
    context: BTreeMap::new(),
  })
}

pub fn requires_migration(saved: &SavedGraph, incoming: &Value, applied: bool) -> bool {
  applied && changed_legacy_diagnostic(saved, incoming).is_some()
}

pub fn has_unified_ei_layers(graph: &Value) -> bool {
  graph["version"] == "0.2.0"
    && graph
      .pointer("/cores/isos/layers")
      .and_then(Value::as_object)
      .is_some_and(|layers| layers.keys().any(|name| name.starts_with("ei_")))
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn unified_graph_uses_managed_generation_and_legacy_does_not() {
    assert!(uses_revisioned_generation(&json!({"version":"0.2.0"})));
    assert!(uses_revisioned_generation(&json!({"version":"0.1.0"})));
    assert!(!uses_revisioned_generation(&json!({"version":"1.0.0"})));
  }

  #[test]
  fn unified_graph_requires_one_core_and_registered_placement() {
    let catalog = json!({"types":[{"id":"isos.eos_hardware.camera@v0.2",
      "core":"isos","layer":"eos_hardware","group":"camera"}]});
    let mut graph = json!({"version":"0.2.0","cores":{"isos":{"layers":{
      "eos_hardware":{"groups":{"camera":{"nodes":{"front":{
        "node_type":"isos.eos_hardware.camera@v0.2"}}}}}}}}});
    assert!(admission_diagnostic(&graph, Some(&catalog)).is_none());
    graph["cores"]["eos"] = json!({"layers":{}});
    assert_eq!(
      admission_diagnostic(&graph, Some(&catalog)).unwrap().path,
      "/cores/eos"
    );
    graph["cores"].as_object_mut().unwrap().remove("eos");
    graph["cores"]["isos"]["layers"]["eos_hardware"]["groups"]["camera"]["nodes"]["front"]["node_type"] =
      json!("isos.eos_hardware.unknown@v0.2");
    assert_eq!(
      admission_diagnostic(&graph, Some(&catalog)).unwrap().code,
      "unsupported_node_type"
    );
  }

  const LEGACY: &[u8] = include_bytes!("../../../tests/fixtures/graph_compat/legacy_1_0_0.json");
  const WHITESPACE: &[u8] =
    include_bytes!("../../../tests/fixtures/graph_compat/legacy_1_0_0_whitespace.json");
  const FUTURE: &[u8] = include_bytes!("../../../tests/fixtures/graph_compat/future_2_0_0.json");
  const UNKNOWN_TYPE: &[u8] =
    include_bytes!("../../../tests/fixtures/graph_compat/unknown_type_1_0_0.json");
  const UNSAFE: &[u8] =
    include_bytes!("../../../tests/fixtures/graph_compat/unsafe_component_1_0_0.json");
  const OPTIONAL: &[u8] =
    include_bytes!("../../../tests/fixtures/graph_compat/legacy_optional_defaults_1_0_0.json");
  const UNMAPPED: &[u8] =
    include_bytes!("../../../tests/fixtures/graph_compat/legacy_unmapped_1_0_0.json");

  #[test]
  fn legacy_projection_keeps_raw_identity_and_reports_only_optional_defaults() {
    let source = parse(OPTIONAL).unwrap();
    let original = source.value.clone();
    let projection = legacy_projection(&source.value, &source.revision, None)
      .unwrap()
      .unwrap();
    assert_eq!(source.value, original);
    assert_eq!(source.revision, revision(OPTIONAL));
    assert_eq!(projection.graph["version"], "1.0.0");
    let command = "/cores/eos/layers/dispatcher/groups/control/nodes/command";
    assert_eq!(
      projection.graph.pointer(&format!("{command}/node_type")),
      Some(&json!("eos.dispatcher.control@v1.0"))
    );
    assert_eq!(
      projection.graph.pointer(&format!("{command}/description")),
      Some(&json!(""))
    );
    for (path, value) in [
      (format!("{command}/capabilities"), json!([])),
      (format!("{command}/inputs"), json!({})),
      (format!("{command}/references"), json!([])),
      (format!("{command}/settings"), json!({})),
      (
        "/cores/isos/layers/process/groups/component/nodes/eos/settings/max_restarts".into(),
        json!(0),
      ),
      (
        "/cores/isos/layers/process/groups/component/nodes/bridge/settings/restart".into(),
        json!("never"),
      ),
    ] {
      assert_eq!(projection.graph.pointer(&path), Some(&value), "{path}");
      assert!(
        projection
          .defaults
          .iter()
          .any(|default| default["path"] == path && default["value"] == value),
        "{path}"
      );
      assert!(original.pointer(&path).is_none(), "{path}");
    }
    assert_eq!(projection.mappings.len(), 4);
    assert!(
      projection
        .mappings
        .iter()
        .any(
          |mapping| mapping["source_type"] == "eos.dispatcher.control@v1.0"
            && mapping["candidate_type"] == "eos.dispatcher.control@v0.1"
        )
    );
    assert!(!projection.graph.to_string().contains("frequency_hz"));
    assert!(!projection.graph.to_string().contains("priority"));
  }

  #[test]
  fn whitespace_distinct_sources_project_equally_without_revision_aliasing() {
    let first = parse(LEGACY).unwrap();
    let second = parse(WHITESPACE).unwrap();
    let a = legacy_projection(&first.value, &first.revision, None)
      .unwrap()
      .unwrap();
    let b = legacy_projection(&second.value, &second.revision, None)
      .unwrap()
      .unwrap();
    assert_eq!(a.graph, b.graph);
    assert_eq!(a.mappings, b.mappings);
    assert_eq!(a.defaults, b.defaults);
    assert_eq!(a.display_descriptions, b.display_descriptions);
    assert_ne!(a.source_revision, b.source_revision);
  }

  #[test]
  fn legacy_projection_escapes_pointer_segments_without_panicking() {
    let mut graph = parse(LEGACY).unwrap().value;
    let nodes = graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["control"]["nodes"]
      .as_object_mut()
      .unwrap();
    let mut node = nodes.remove("command").unwrap();
    node["id"] = json!("command/~");
    nodes.insert("command/~".into(), node);
    let pointer = "/cores/eos/layers/dispatcher/groups/control/nodes/command~1~0/node_type";
    let issue = legacy_projection(&graph, "revision", None).unwrap_err();
    assert_eq!(
      (issue.path.as_str(), issue.code),
      (
        "/cores/eos/layers/dispatcher/groups/control/nodes/command~1~0",
        "invalid_graph"
      )
    );
    let node =
      &mut graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["control"]["nodes"]["command/~"];
    node["node_type"] = json!("eos.control.manual@v1.0");
    let issue = legacy_projection(&graph, "revision", None).unwrap_err();
    assert_eq!(
      (issue.path.as_str(), issue.code),
      (pointer, "unmapped_node_type")
    );
    graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["control"]["nodes"]["command/~"]["node_type"] =
      json!("vendor.unknown@v1.0");
    let issue = legacy_projection(&graph, "revision", None).unwrap_err();
    assert_eq!(
      (issue.path.as_str(), issue.code),
      (pointer, "unsupported_node_type")
    );
    graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["control"]["nodes"]["command/~"]["node_type"] =
      json!("eos.dispatcher.control@v1.0");
    graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["control"]["nodes"]["command/~"]["extra/~"] =
      json!(true);
    let issue = legacy_projection(&graph, "revision", None).unwrap_err();
    assert_eq!(
      (issue.path.as_str(), issue.code),
      (
        "/cores/eos/layers/dispatcher/groups/control/nodes/command~1~0/extra~1~0",
        "unmapped_graph_field"
      )
    );
  }

  #[test]
  fn whole_graph_mapping_failure_yields_no_projection_and_exact_path() {
    let unmapped = parse(UNMAPPED).unwrap();
    let error = legacy_projection(&unmapped.value, &unmapped.revision, None).unwrap_err();
    assert_eq!(error.code, "unmapped_node_type");
    assert_eq!(
      error.path,
      "/cores/eos/layers/control/groups/manual/nodes/manual_0/node_type"
    );
    let unsafe_graph = parse(UNSAFE).unwrap();
    assert_eq!(
      legacy_projection(&unsafe_graph.value, &unsafe_graph.revision, None)
        .unwrap_err()
        .code,
      "unsafe_process_node"
    );
    let unknown = parse(UNKNOWN_TYPE).unwrap();
    assert_eq!(
      legacy_projection(&unknown.value, &unknown.revision, None)
        .unwrap_err()
        .code,
      "unsupported_node_type"
    );
    let future = parse(FUTURE).unwrap();
    assert_eq!(
      legacy_projection(&future.value, &future.revision, None)
        .unwrap_err()
        .code,
      "unsupported_graph_version"
    );
    let mut malformed = parse(LEGACY).unwrap().value;
    malformed["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["eos"]["settings"]
            .as_object_mut().unwrap().remove("role");
    let error = legacy_projection(&malformed, "raw-sha", None).unwrap_err();
    assert_eq!(
      (error.path.as_str(), error.code),
      (
        "/cores/isos/layers/process/groups/component/nodes/eos/settings/role",
        "unmapped_graph_field"
      )
    );
    let mut unknown_core = parse(LEGACY).unwrap().value;
    unknown_core["cores"]["evil"] = json!({"layers":{}});
    let error = legacy_projection(&unknown_core, "raw-sha", None).unwrap_err();
    assert_eq!(
      (error.path.as_str(), error.code),
      ("/cores/evil", "unsupported_core")
    );
    let empty = json!({"version":"1.0.0","robot":{},"cores":{}});
    let error = legacy_projection(&empty, "raw-sha", None).unwrap_err();
    assert_eq!(
      (error.path.as_str(), error.code),
      ("/robot/id", "invalid_graph")
    );
    let empty = json!({"version":"1.0.0","robot":{"id":"robot","name":"Robot"},"cores":{}});
    let error = legacy_projection(&empty, "raw-sha", None).unwrap_err();
    assert_eq!(
      (error.path.as_str(), error.code),
      ("/cores", "invalid_graph")
    );
  }

  #[test]
  fn frozen_bytes_have_distinct_original_revisions() {
    let legacy = parse(LEGACY).unwrap();
    let whitespace = parse(WHITESPACE).unwrap();
    assert_eq!(legacy.value, whitespace.value);
    assert_ne!(LEGACY, WHITESPACE);
    assert_eq!(
      legacy.revision,
      "7a8daee6b7db9105f5483506ffc1743625b63089710dfbf936622329c471016b"
    );
    assert_eq!(
      whitespace.revision,
      "27e137a1387393d7fb2e1abc1b735315cbdefd47a02ae2b265fa3596720158e9"
    );
    assert!(legacy.read_only);
  }

  #[test]
  fn future_and_unsafe_inputs_have_exact_paths() {
    let future = parse(FUTURE).unwrap();
    assert_eq!(future.diagnostics[0].path, "/version");
    assert_eq!(future.diagnostics[0].code, "unsupported_graph_version");
    let unsafe_graph = parse(UNSAFE).unwrap();
    let unsafe_diagnostic = admission_diagnostic(&unsafe_graph.value, None).unwrap();
    assert_eq!(
      unsafe_diagnostic.path,
      "/cores/isos/layers/process/groups/component/nodes/eos/node_type"
    );
    assert_eq!(unsafe_diagnostic.code, "unsafe_process_node");
    let catalog = serde_json::json!({"types":[]});
    let unknown = parse(UNKNOWN_TYPE).unwrap();
    let type_diagnostic = admission_diagnostic(&unknown.value, Some(&catalog)).unwrap();
    assert_eq!(
      type_diagnostic.path,
      "/cores/eos/layers/dispatcher/groups/control/nodes/command/node_type"
    );
    assert_eq!(type_diagnostic.code, "unsupported_node_type");
    let mut invented = parse(LEGACY).unwrap().value;
    invented["cores"]["eos"]["layers"]["dispatcher"]["groups"]["control"]["nodes"]["command"]["node_type"] =
      Value::String("vendor.foo@v1.0".into());
    let invented_diagnostic = admission_diagnostic(&invented, None).unwrap();
    assert_eq!(
      invented_diagnostic.path,
      "/cores/eos/layers/dispatcher/groups/control/nodes/command/node_type"
    );
    assert_eq!(invented_diagnostic.code, "unsupported_node_type");
  }

  #[test]
  fn malformed_and_oversized_inputs_fail_closed() {
    assert!(parse(b"not JSON").is_err());
    assert!(parse(br#"{"version":"1.0.0"}"#).is_ok());
    assert!(parse(&vec![b' '; MAX_GRAPH_BYTES + 1]).is_err());
    assert_eq!(
      version_diagnostic(&serde_json::json!({})).unwrap().path,
      "/version"
    );
  }

  #[test]
  fn target_graph_rejects_unknown_core_legacy_ui_and_misplaced_type() {
    let source =
      include_bytes!("../../../tests/fixtures/graph_compat/migration_expected_0_1_0.json");
    let mut graph = parse(source).unwrap().value;
    graph["cores"]["evil"] = serde_json::json!({"layers":{}});
    let diagnostic = admission_diagnostic(&graph, None).unwrap();
    assert_eq!(
      (diagnostic.path.as_str(), diagnostic.code),
      ("/cores/evil", "unsupported_core")
    );
    graph["cores"].as_object_mut().unwrap().remove("evil");
    let node =
      &mut graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["control"]["nodes"]["command"];
    node["node_type"] = Value::String("isos.ui.vision@v1.0".into());
    let diagnostic = admission_diagnostic(&graph, None).unwrap();
    assert_eq!(diagnostic.code, "unsupported_node_type");
    assert_eq!(
      diagnostic.path,
      "/cores/eos/layers/dispatcher/groups/control/nodes/command/node_type"
    );
    graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["control"]["nodes"]["command"]["node_type"] =
      Value::String("eos.hardware.camera@v0.1".into());
    let diagnostic = admission_diagnostic(&graph, None).unwrap();
    assert_eq!(diagnostic.code, "invalid_node_placement");
    assert_eq!(
      diagnostic.path,
      "/cores/eos/layers/dispatcher/groups/control/nodes/command/node_type"
    );
  }

  #[test]
  fn legacy_edit_comparison_uses_structure_without_rewriting_bytes() {
    let saved = parse(WHITESPACE).unwrap();
    let same = parse(LEGACY).unwrap();
    assert!(changed_legacy_diagnostic(&saved, &same.value).is_none());
    let mut changed = same.value;
    changed["robot"]["name"] = Value::String("Changed".into());
    assert_eq!(
      changed_legacy_diagnostic(&saved, &changed).unwrap().code,
      "migration_required"
    );
    assert_eq!(saved.revision, revision(WHITESPACE));
    assert!(!requires_migration(&saved, &changed, false));
    assert!(requires_migration(&saved, &changed, true));
  }

  #[test]
  fn seven_egia_stages_have_exact_target_placements_and_legacy_remains_distinct() {
    let stages = [
      ("egia.link.gateway@v0.1", "io", "link"),
      ("egia.perception.admission@v0.1", "evidence", "perception"),
      ("egia.context.builder@v0.1", "cognition", "context"),
      ("egia.missions.queue@v0.1", "cognition", "missions"),
      ("egia.decision.planner@v0.1", "cognition", "decision"),
      ("egia.safety.gate@v0.1", "governance", "safety"),
      ("egia.monitor.health@v0.1", "governance", "monitor"),
    ];
    for (id, layer, group) in stages {
      let mut graph = json!({"version":"0.1.0","cores":{"egia":{"layers":{}}}});
      graph["cores"]["egia"]["layers"][layer]["groups"][group]["nodes"]["stage"] =
        json!({"id":"stage","node_type":id});
      assert!(admission_diagnostic(&graph, None).is_none(), "{id}");
      assert!(has_egia_stages(&graph));
      graph["cores"]["egia"]["layers"][layer]["groups"][group]["nodes"]["stage"]["node_type"] =
        json!("egia.common.source@v1.0");
      assert_eq!(
        admission_diagnostic(&graph, None).unwrap().code,
        "invalid_node_placement"
      );
    }
    assert_eq!(
      target_placement("egia.common.source@v1.0"),
      Some(("egia", "common", "data"))
    );
    assert!(target_placement("egia.tensor@v0.1").is_none());
  }
}
