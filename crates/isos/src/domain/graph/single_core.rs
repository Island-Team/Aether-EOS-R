//! Deterministic, non-publishing conversion to the unified IsOS graph core.

use super::graph_compat::{self, Diagnostic, SavedGraph};
use serde_json::{Value, json};
use std::collections::BTreeMap;

pub struct Proposal {
  pub graph: Value,
  pub bytes: Vec<u8>,
  pub revision: String,
}

/// Return a revision-bound view for an owner setup adapter. The source graph
/// remains authoritative; adapters receive no writable copy of other layers.
pub fn owner_projection(graph: &Value, role: &str) -> Result<Value, Diagnostic> {
  if graph["version"] != "0.2.0" {
    return Err(diagnostic(
      "/version",
      "unsupported_graph_version",
      "owner projection requires graph 0.2.0",
    ));
  }
  let prefix = match role {
    "eos" => "eos_",
    "ei" => "ei_",
    _ => {
      return Err(diagnostic(
        "/owner",
        "unsupported_owner",
        format!("unknown owner {role}"),
      ));
    }
  };
  let layers = graph
    .pointer("/cores/isos/layers")
    .and_then(Value::as_object)
    .ok_or_else(|| {
      diagnostic(
        "/cores/isos/layers",
        "invalid_graph",
        "IsOS layers must be an object",
      )
    })?;
  let selected: serde_json::Map<String, Value> = layers
    .iter()
    .filter(|(name, _)| name.starts_with(prefix))
    .map(|(name, layer)| (name.clone(), layer.clone()))
    .collect();
  Ok(json!({"version":"0.2.0","robot":graph["robot"],
    "cores":{"isos":{"layers":selected}}}))
}

fn diagnostic(
  path: impl Into<String>,
  code: &'static str,
  message: impl Into<String>,
) -> Diagnostic {
  Diagnostic {
    path: path.into(),
    code,
    message: message.into(),
    context: BTreeMap::new(),
  }
}

fn destination(core: &str, layer: &str) -> Result<String, Diagnostic> {
  let prefix = match core {
    "isos" => return Ok(layer.to_owned()),
    "eos" => "eos",
    "egia" => "ei",
    "isosui" => return Ok("ui".into()),
    _ => {
      return Err(diagnostic(
        format!("/cores/{core}"),
        "unsupported_core",
        "no single-core owner mapping",
      ));
    }
  };
  Ok(format!("{prefix}_{layer}"))
}

fn mapped_path(path: &str) -> Result<Option<String>, Diagnostic> {
  let absolute = path.starts_with('/');
  let parts: Vec<&str> = path.trim_start_matches('/').split('/').collect();
  if !absolute && parts.len() == 4 && matches!(parts[0], "isos" | "eos" | "egia" | "isosui") {
    let layer = destination(parts[0], parts[1])?;
    return Ok(Some(format!("isos/{layer}/{}/{}", parts[2], parts[3])));
  }
  if parts.len() < 4 || parts[0] != "cores" || parts[2] != "layers" {
    return Ok(None);
  }
  let layer = destination(parts[1], parts[3])?;
  let suffix = if parts.len() > 4 {
    format!("/{}", parts[4..].join("/"))
  } else {
    String::new()
  };
  Ok(Some(format!(
    "{}cores/isos/layers/{layer}{suffix}",
    if absolute { "/" } else { "" }
  )))
}

fn rewrite_paths(value: &mut Value, pointer: &str) -> Result<(), Diagnostic> {
  match value {
    Value::String(old) => {
      if let Some(new) = mapped_path(old)? {
        *old = new;
      }
    }
    Value::Array(values) => {
      for (index, item) in values.iter_mut().enumerate() {
        rewrite_paths(item, &format!("{pointer}/{index}"))?;
      }
    }
    Value::Object(values) => {
      let old = std::mem::take(values);
      for (key, mut item) in old {
        let new_key = mapped_path(&key)?.unwrap_or_else(|| key.clone());
        if values.contains_key(&new_key) {
          return Err(diagnostic(
            pointer,
            "ambiguous_path_mapping",
            format!("mapped key {new_key} already exists"),
          ));
        }
        rewrite_paths(
          &mut item,
          &format!("{pointer}/{}", graph_compat::pointer_segment(&key)),
        )?;
        values.insert(new_key, item);
      }
    }
    _ => {}
  }
  Ok(())
}

fn mapped_type(old: &str, layer: &str) -> Option<String> {
  let (family, version) = old.rsplit_once('@')?;
  if version != "v0.1" && !(family.starts_with("egia.common.") && version == "v1.0") {
    return None;
  }
  if !(family.starts_with("eos.")
    || family.starts_with("egia.")
    || family.starts_with("isos.")
    || family.starts_with("isosui."))
  {
    return None;
  }
  let name = family.split('.').next_back()?;
  Some(format!("isos.{layer}.{name}@v0.2"))
}

/// Build a complete candidate while retaining the original saved bytes and revision.
/// The caller must validate the candidate with all registered owners before publication.
pub fn propose(saved: &SavedGraph) -> Result<Proposal, Diagnostic> {
  if saved.value["version"] != "0.1.0" {
    return Err(diagnostic(
      "/version",
      "migration_required",
      "single-core migration requires graph 0.1.0",
    ));
  }
  let old_cores = saved.value["cores"]
    .as_object()
    .ok_or_else(|| diagnostic("/cores", "invalid_graph", "cores must be an object"))?;
  let mut merged = serde_json::Map::new();
  for (core_id, core) in old_cores {
    let layers = core["layers"].as_object().ok_or_else(|| {
      diagnostic(
        format!("/cores/{core_id}/layers"),
        "invalid_graph",
        "layers must be an object",
      )
    })?;
    for (layer_id, layer) in layers {
      let target_layer = destination(core_id, layer_id)?;
      if merged.contains_key(&target_layer) {
        return Err(diagnostic(
          format!("/cores/{core_id}/layers/{layer_id}"),
          "ambiguous_layer_mapping",
          format!("destination layer {target_layer} already exists"),
        ));
      }
      let mut layer = layer.clone();
      let groups = layer["groups"].as_object_mut().ok_or_else(|| {
        diagnostic(
          format!("/cores/{core_id}/layers/{layer_id}/groups"),
          "invalid_graph",
          "groups must be an object",
        )
      })?;
      for (group_id, group) in groups {
        let nodes = group["nodes"].as_object_mut().ok_or_else(|| {
          diagnostic(
            format!("/cores/{core_id}/layers/{layer_id}/groups/{group_id}/nodes"),
            "invalid_graph",
            "nodes must be an object",
          )
        })?;
        for (node_id, node) in nodes {
          let node_path =
            format!("/cores/{core_id}/layers/{layer_id}/groups/{group_id}/nodes/{node_id}");
          let old_type = node["node_type"].as_str().ok_or_else(|| {
            diagnostic(
              format!("{node_path}/node_type"),
              "unmapped_node_type",
              "node type is missing",
            )
          })?;
          if graph_compat::target_placement(old_type)
            != Some((core_id.as_str(), layer_id.as_str(), group_id.as_str()))
          {
            return Err(diagnostic(
              format!("{node_path}/node_type"),
              "unmapped_node_type",
              format!("{old_type} is not registered at this placement"),
            ));
          }
          let new_type = mapped_type(old_type, &target_layer).ok_or_else(|| {
            diagnostic(
              format!("{node_path}/node_type"),
              "unmapped_node_type",
              format!("{old_type} has no v0.2 mapping"),
            )
          })?;
          node["node_type"] = json!(new_type);
          rewrite_paths(node, &node_path)?;
        }
      }
      merged.insert(target_layer, layer);
    }
  }
  let mut graph = saved.value.clone();
  graph["version"] = json!("0.2.0");
  graph["cores"] = json!({"isos": {"layers": merged}});
  rewrite_paths(&mut graph, "")?;
  let bytes = serde_json::to_vec_pretty(&graph)
    .map_err(|error| diagnostic("/", "migration_serialization_failed", error.to_string()))?;
  Ok(Proposal {
    revision: graph_compat::revision(&bytes),
    graph,
    bytes,
  })
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn migration_merges_core_paths_and_keeps_source_revision() {
    let input = br#"{"version":"0.1.0","robot":{"id":"r","name":"R"},"cores":{"eos":{"layers":{"hardware":{"groups":{"camera":{"nodes":{"front":{"node_type":"eos.hardware.camera@v0.1","references":["cores/egia/layers/io/groups/link/nodes/gateway"]}}}}}}},"egia":{"layers":{"io":{"groups":{"link":{"nodes":{"gateway":{"node_type":"egia.link.gateway@v0.1"}}}}}}}}}"#;
    let saved = graph_compat::parse(input).unwrap();
    let original_revision = saved.revision.clone();
    let proposal = propose(&saved).unwrap();
    assert_eq!(saved.revision, original_revision);
    assert_eq!(saved.value["version"], "0.1.0");
    assert_eq!(proposal.graph["version"], "0.2.0");
    assert_eq!(
      proposal.graph["cores"]["isos"]["layers"]["eos_hardware"]["groups"]["camera"]["nodes"]["front"]
        ["references"][0],
      "cores/isos/layers/ei_io/groups/link/nodes/gateway"
    );
    assert_eq!(
      proposal.graph["cores"]["isos"]["layers"]["eos_hardware"]["groups"]["camera"]["nodes"]["front"]
        ["node_type"],
      "isos.eos_hardware.camera@v0.2"
    );
  }

  #[test]
  fn migration_rewrites_four_part_edge_and_binding_paths() {
    let saved = graph_compat::parse(br#"{"version":"0.1.0","cores":{"eos":{"layers":{"hardware":{"groups":{"camera":{"nodes":{"front":{"node_type":"eos.hardware.camera@v0.1","id":"front","inputs":{"frame":{"node":"egia/io/link/gateway"}},"references":["eos/hardware/camera/front"]}}}}}}}}}"#).unwrap();
    let proposal = propose(&saved).unwrap();
    let node = &proposal.graph["cores"]["isos"]["layers"]["eos_hardware"]["groups"]["camera"]["nodes"]
      ["front"];
    assert_eq!(node["inputs"]["frame"]["node"], "isos/ei_io/link/gateway");
    assert_eq!(node["references"][0], "isos/eos_hardware/camera/front");
  }

  #[test]
  fn migration_rejects_unknown_type_at_exact_path() {
    let saved = graph_compat::parse(br#"{"version":"0.1.0","cores":{"eos":{"layers":{"hardware":{"groups":{"camera":{"nodes":{"front":{"node_type":"vendor.camera@v0.1"}}}}}}}}}"#).unwrap();
    let error = propose(&saved).err().unwrap();
    assert_eq!(error.code, "unmapped_node_type");
    assert_eq!(
      error.path,
      "/cores/eos/layers/hardware/groups/camera/nodes/front/node_type"
    );
  }

  #[test]
  fn owner_projections_are_disjoint() {
    let graph = json!({"version":"0.2.0","robot":{"id":"r"},"cores":{"isos":{"layers":{
      "process":{"groups":{}},"eos_hardware":{"groups":{}},"ei_io":{"groups":{}},"ui":{"groups":{}}
    }}}});
    let eos = owner_projection(&graph, "eos").unwrap();
    let ei = owner_projection(&graph, "ei").unwrap();
    assert!(eos.pointer("/cores/isos/layers/eos_hardware").is_some());
    assert!(eos.pointer("/cores/isos/layers/ei_io").is_none());
    assert!(ei.pointer("/cores/isos/layers/ei_io").is_some());
    assert!(ei.pointer("/cores/isos/layers/eos_hardware").is_none());
    assert!(graph.pointer("/cores/isos/layers/ui").is_some());
  }

  #[test]
  fn historical_egia_and_ui_types_receive_new_identities() {
    assert_eq!(
      mapped_type("egia.common.source@v1.0", "ei_common").as_deref(),
      Some("isos.ei_common.source@v0.2")
    );
    assert_eq!(destination("isosui", "input").unwrap(), "ui");
    assert_eq!(
      mapped_type("isosui.input.keyboard@v0.1", "ui").as_deref(),
      Some("isos.ui.keyboard@v0.2")
    );
  }

  #[test]
  fn migration_rejects_layer_collision_at_source_pointer() {
    let saved = graph_compat::parse(
      br#"{"version":"0.1.0","cores":{
      "eos":{"layers":{"hardware":{"groups":{}}}},
      "isos":{"layers":{"eos_hardware":{"groups":{}}}}
    }}"#,
    )
    .unwrap();
    let error = propose(&saved).err().unwrap();
    assert_eq!(error.code, "ambiguous_layer_mapping");
    assert_eq!(error.path, "/cores/isos/layers/eos_hardware");
  }
}
