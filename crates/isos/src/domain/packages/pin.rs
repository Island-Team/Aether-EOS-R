//! Exact package identity extraction from an authored node graph.

use super::*;

#[derive(Debug, Clone)]
pub struct PackagePin {
  pub id: String,
  pub version: String,
  pub path: String,
}

/// Collect only canonical, complete package pins declared by graph nodes.
pub fn collect_pins(graph: &serde_json::Value) -> Result<Vec<PackagePin>> {
  let mut pins = BTreeMap::new();
  let cores = graph
    .get("cores")
    .and_then(serde_json::Value::as_object)
    .context("graph requires cores object before package pin collection")?;
  for (core_id, core) in cores {
    let layers = core["layers"]
      .as_object()
      .with_context(|| format!("core {core_id} requires layers object"))?;
    for (layer_id, layer) in layers {
      let groups = layer["groups"]
        .as_object()
        .with_context(|| format!("{core_id}/{layer_id} requires groups object"))?;
      for (group_id, group) in groups {
        let nodes = group["nodes"]
          .as_object()
          .with_context(|| format!("{core_id}/{layer_id}/{group_id} requires nodes object"))?;
        for (node_id, node) in nodes {
          ensure!(
            node.is_object(),
            "{core_id}/{layer_id}/{group_id}/{node_id} requires node object"
          );
          let base = format!(
            "/cores/{}/layers/{}/groups/{}/nodes/{}",
            crate::domain::graph::graph_compat::pointer_segment(core_id),
            crate::domain::graph::graph_compat::pointer_segment(layer_id),
            crate::domain::graph::graph_compat::pointer_segment(group_id),
            crate::domain::graph::graph_compat::pointer_segment(node_id)
          );
          for (path, package) in [
            (format!("{base}/package"), node.get("package")),
            (
              format!("{base}/adapter/package"),
              node
                .get("adapter")
                .and_then(|adapter| adapter.get("package")),
            ),
          ] {
            if let Some(package) = package {
              let id = package
                .get("id")
                .and_then(serde_json::Value::as_str)
                .ok_or_else(|| anyhow::anyhow!("incomplete package pin at {path}"))?;
              let version = package
                .get("version")
                .and_then(serde_json::Value::as_str)
                .ok_or_else(|| anyhow::anyhow!("incomplete package pin at {path}"))?;
              validate_identity(id, version)
                .with_context(|| format!("invalid package pin at {path}"))?;
              pins
                .entry((id.to_owned(), version.to_owned()))
                .and_modify(|existing: &mut String| {
                  if path < *existing {
                    *existing = path.clone();
                  }
                })
                .or_insert(path);
            }
          }
          let settings = &node["settings"];
          if let Some(libraries) = settings["skill_libraries"].as_array() {
            for (index, entry) in libraries.iter().enumerate() {
              let path = format!("{base}/settings/skill_libraries/{index}/package");
              let package = &entry["package"];
              let id = package["id"]
                .as_str()
                .ok_or_else(|| anyhow::anyhow!("incomplete package pin at {path}"))?;
              let version = package["version"]
                .as_str()
                .ok_or_else(|| anyhow::anyhow!("incomplete package pin at {path}"))?;
              validate_identity(id, version)
                .with_context(|| format!("invalid package pin at {path}"))?;
              pins
                .entry((id.to_owned(), version.to_owned()))
                .and_modify(|existing: &mut String| {
                  if path < *existing {
                    *existing = path.clone();
                  }
                })
                .or_insert(path);
            }
          }
          if !settings["package"].is_null() || !settings["version"].is_null() {
            let path = format!("{base}/settings/package");
            let id = settings["package"]
              .as_str()
              .ok_or_else(|| anyhow::anyhow!("incomplete package pin at {path}"))?;
            let version = settings["version"]
              .as_str()
              .ok_or_else(|| anyhow::anyhow!("incomplete package pin at {path}"))?;
            validate_identity(id, version)
              .with_context(|| format!("invalid package pin at {path}"))?;
            pins
              .entry((id.to_owned(), version.to_owned()))
              .and_modify(|existing: &mut String| {
                if path < *existing {
                  *existing = path.clone();
                }
              })
              .or_insert(path);
          }
        }
      }
    }
  }
  Ok(
    pins
      .into_iter()
      .map(|((id, version), path)| PackagePin { id, version, path })
      .collect(),
  )
}

#[cfg(test)]
mod pin_tests {
  use super::*;

  #[test]
  fn adapter_only_package_pin_selects_sidecar_and_blocks_removal() {
    let graph = serde_json::json!({
        "cores": {"eos": {"layers": {"hardware": {"groups": {"base": {"nodes": {
            "drive": {"node_type": "eos.hardware.base@v1.0", "adapter": {
                "type": "vendor-base", "package": {"id": "vendor/devices", "version": "2.0.0"}
            }}
        }}}}}}}
    });
    let pins = collect_pins(&graph).unwrap();
    assert_eq!(pins.len(), 1);
    assert_eq!(
      (pins[0].id.as_str(), pins[0].version.as_str()),
      ("vendor/devices", "2.0.0")
    );
  }

  #[test]
  fn incomplete_adapter_pin_rejects_graph() {
    let graph = serde_json::json!({
        "cores": {"eos": {"layers": {"hardware": {"groups": {"base": {"nodes": {
            "drive": {"adapter": {"package": {"id": "vendor/devices"}}}
        }}}}}}}
    });
    assert!(collect_pins(&graph).is_err());
  }

  #[test]
  fn malformed_graph_shape_cannot_hide_package_pins() {
    for graph in [
      serde_json::json!({}),
      serde_json::json!({"cores": {"eos": {}}}),
      serde_json::json!({"cores": {"eos": {"layers": {"process": {}}}}}),
      serde_json::json!({"cores": {"eos": {"layers": {"process": {"groups": {"sim": {}}}}}}}),
      serde_json::json!({"cores": {"eos": {"layers": {"process": {"groups": {"sim": {"nodes": {"one": null}}}}}}}}),
    ] {
      assert!(collect_pins(&graph).is_err());
    }
  }

  #[test]
  fn duplicate_pin_uses_lexically_first_graph_pointer() {
    let graph = serde_json::json!({"cores":{"eos":{"layers":{"hardware":{"groups":{"base":{"nodes":{"drive":{"adapter":{"package":{"id":"vendor/devices","version":"2.0.0"}}}}}}},"process":{"groups":{"sim":{"nodes":{"one":{"package":{"id":"vendor/devices","version":"2.0.0"}}}}}}}}}});
    let pins = collect_pins(&graph).unwrap();
    assert_eq!(pins.len(), 1);
    assert!(pins[0].path.contains("/hardware/"));
  }
}
