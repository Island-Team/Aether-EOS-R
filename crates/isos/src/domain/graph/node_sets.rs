//! Package-contributed node sets are composed into the shared graph only on request.

use crate::{
  app::config::Paths,
  domain::packages,
  domain::registry::release::{NodeSet, Release},
};
use anyhow::{Context, Result, ensure};
use serde_json::{Value, json};
use std::collections::{BTreeMap, BTreeSet};

pub fn installed_release(paths: &Paths, id: &str, version: &str) -> Result<Release> {
  Ok(packages::verify(paths, id, version)?.release)
}

pub fn list(paths: &Paths) -> Result<Value> {
  let mut sets = Vec::new();
  for receipt in packages::list(paths)? {
    let Ok(verified) = packages::verify(paths, &receipt.id, &receipt.version) else {
      continue;
    };
    let release = verified.release;
    if let Some(contributions) = release.node_contributions {
      for set in contributions.node_sets {
        let mut value = serde_json::to_value(set)?;
        value["package"] = json!({"id":release.id,"version":release.version});
        sets.push(value);
      }
    }
  }
  Ok(json!({"sets":sets}))
}

fn substitute(value: &mut Value, parameters: &BTreeMap<String, String>) -> Result<()> {
  match value {
    Value::String(s) => {
      for (key, replacement) in parameters {
        *s = s.replace(&format!("${{{key}}}"), replacement);
      }
      ensure!(!s.contains("${"), "unresolved node set parameter in {s}");
    }
    Value::Array(items) => {
      for item in items {
        substitute(item, parameters)?;
      }
    }
    Value::Object(fields) => {
      for item in fields.values_mut() {
        substitute(item, parameters)?;
      }
    }
    _ => {}
  }
  Ok(())
}

fn resolve_refs(value: &mut Value, paths: &BTreeMap<String, String>) -> Result<()> {
  match value {
    Value::String(s) if s.starts_with('@') => {
      let reference = &s[1..];
      *s = paths
        .get(reference)
        .with_context(|| format!("unknown node ref {reference}"))?
        .clone();
    }
    Value::Array(items) => {
      for item in items {
        resolve_refs(item, paths)?;
      }
    }
    Value::Object(fields) => {
      for item in fields.values_mut() {
        resolve_refs(item, paths)?;
      }
    }
    _ => {}
  }
  Ok(())
}

fn short_path(path: &str) -> Option<String> {
  let parts: Vec<_> = path.split('/').collect();
  if parts.len() == 8
    && parts[0] == "cores"
    && parts[2] == "layers"
    && parts[4] == "groups"
    && parts[6] == "nodes"
  {
    Some(format!(
      "{}/{}/{}/{}",
      parts[1], parts[3], parts[5], parts[7]
    ))
  } else {
    None
  }
}

fn rewrite_paths(value: &mut Value, replacements: &BTreeMap<String, String>) {
  match value {
    Value::String(s) => {
      if let Some(replacement) = replacements.get(s) {
        *s = replacement.clone();
      } else if let Some(path) = short_path(s) {
        *s = path;
      }
    }
    Value::Array(items) => {
      for item in items {
        rewrite_paths(item, replacements);
      }
    }
    Value::Object(fields) => {
      for item in fields.values_mut() {
        rewrite_paths(item, replacements);
      }
    }
    _ => {}
  }
}

pub fn compose(
  paths: &Paths,
  id: &str,
  version: &str,
  set_id: &str,
  graph: &Value,
  values: &BTreeMap<String, String>,
) -> Result<Value> {
  let release = installed_release(paths, id, version)?;
  let contributed_types: BTreeSet<_> = release
    .node_contributions
    .as_ref()
    .into_iter()
    .flat_map(|contributions| contributions.node_types.iter())
    .filter_map(|node_type| node_type["id"].as_str())
    .collect();
  let set: &NodeSet = release
    .node_contributions
    .as_ref()
    .and_then(|c| c.node_sets.iter().find(|set| set.id == set_id))
    .with_context(|| format!("node set {set_id} is not installed in {id}@{version}"))?;
  let mut parameters = BTreeMap::new();
  for (name, definition) in &set.parameters {
    let value = values
      .get(name)
      .or(definition.default.as_ref())
      .with_context(|| format!("missing node set parameter {name}"))?;
    parameters.insert(name.clone(), value.clone());
  }
  ensure!(
    values.keys().all(|name| set.parameters.contains_key(name)),
    "unknown node set parameter"
  );
  let mut paths_by_ref = BTreeMap::new();
  let default_group = id.split_once('/').unwrap().1;
  for node in &set.nodes {
    let group = node.group.as_deref().unwrap_or(default_group);
    let instance = node.profile["id"]
      .as_str()
      .context("node profile requires id")?;
    paths_by_ref.insert(
      node.reference.clone(),
      format!("{}/{}/{group}/{instance}", node.core, node.layer),
    );
  }
  let mut draft = graph.clone();
  ensure!(
    draft["version"] == "0.1.0",
    "node graph version is incompatible with the selected node set"
  );
  rewrite_paths(&mut draft, &BTreeMap::new());
  let mut added_paths = Vec::new();
  let mut seen = BTreeSet::new();
  for node in &set.nodes {
    let path = &paths_by_ref[&node.reference];
    ensure!(seen.insert(path), "node set duplicates path {path}");
    let segments: Vec<_> = path.split('/').collect();
    let pointer = format!(
      "/cores/{}/layers/{}/groups/{}/nodes/{}",
      segments[0], segments[1], segments[2], segments[3]
    );
    ensure!(
      draft.pointer(&pointer).is_none(),
      "node path already exists: {path}"
    );
    let mut profile = node.profile.clone();
    substitute(&mut profile, &parameters)?;
    resolve_refs(&mut profile, &paths_by_ref)?;
    let package = json!({"id":id,"version":version});
    let node_type = profile["node_type"]
      .as_str()
      .context("node profile requires node_type")?;
    if contributed_types.contains(node_type) {
      ensure!(
        profile
          .get("package")
          .is_none_or(|current| current == &package),
        "node package pin differs"
      );
      profile["package"] = package;
    } else {
      ensure!(
        profile.get("package").is_none(),
        "built-in node type cannot carry package pin"
      );
    }
    let core = draft["cores"]
      .as_object_mut()
      .context("graph lacks cores object")?
      .entry(segments[0])
      .or_insert_with(|| json!({"layers":{}}));
    let layer = core["layers"]
      .as_object_mut()
      .context("core lacks layers object")?
      .entry(segments[1])
      .or_insert_with(|| json!({"groups":{}}));
    let group = layer["groups"]
      .as_object_mut()
      .context("layer lacks groups object")?
      .entry(segments[2])
      .or_insert_with(|| json!({"nodes":{}}));
    group["nodes"]
      .as_object_mut()
      .context("group lacks nodes object")?
      .insert(segments[3].to_owned(), profile);
    added_paths.push(path.clone());
  }
  Ok(json!({"graph":draft,"added_paths":added_paths}))
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::fs;
  use std::path::Path;

  fn fixture() -> (tempfile::TempDir, Paths) {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let bundle = temp.path().join("bundle");
    fs::create_dir(&bundle).unwrap();
    let manifest = temp.path().join("release.json");
    let release = json!({
        "contract":"isos.package@v1","schema_version":"0.1.0","id":"test/sim","kind":"skill","version":"2.0.0",
        "source_commit":"a".repeat(40),
        "assets":[{"os":"linux","arch":"x86_64","url":"https://github.com/test/sim/releases/download/v2.0.0/sim.tar.gz","size_bytes":1,"sha256":"b".repeat(64)}],
        "node_contributions":{"node_types":[
            {"id":"eos.process.demo@v1.0","core":"eos","layer":"process","group":"demo","description":"Synthetic process","kind":"software","lifecycle":"process","capabilities":[],"settings":[],"inputs":[],"outputs":[]},
            {"id":"eos.hardware.demo_camera@v1.0","core":"eos","layer":"hardware","group":"demo","description":"Synthetic camera","kind":"camera","lifecycle":"adapter","capabilities":[],"settings":[],"inputs":[],"outputs":[]}
        ],"safety_adapters":[],"data_types":[{"id":"test.vision.frame@v1.0","version":"1.0","purpose":"Test camera preview","media_type":"application/json","codec":"json_v1","schema":{"type":"object"},"max_payload_bytes":65536}],"node_sets":[{
            "id":"hardware","parameters":{"socket":{"type":"string"}},
            "nodes":[
                {"ref":"simulator","core":"eos","layer":"process","group":"demo","profile":{"id":"simulator","node_type":"eos.process.demo@v1.0","settings":{"socket":"${socket}"}}},
                {"ref":"front","core":"eos","layer":"hardware","group":"demo","profile":{"id":"front","node_type":"eos.hardware.demo_camera@v1.0","references":["@simulator"],"adapter":{"settings":{"backend":"@simulator","socket_path":"${socket}"}}}},
                {"ref":"gate","core":"eos","layer":"interface","group":"gate","profile":{"id":"gate","node_type":"eos.interface.camera_gate@v1.0","inputs":{"frame":{"node":"@front","output":"frame"}}}},
                {"ref":"vision","core":"isosui","layer":"system","group":"ui","profile":{"id":"vision","node_type":"isosui.system.vision@v1.0","inputs":{"preview":{"node":"@gate","output":"preview"}}}}
            ]}]}
    });
    fs::write(&manifest, serde_json::to_vec(&release).unwrap()).unwrap();
    packages::link_with_manifest(
      &paths,
      "test/sim",
      "2.0.0",
      Path::new(&bundle),
      Some(&manifest),
    )
    .unwrap();
    (temp, paths)
  }

  #[test]
  fn compose_resolves_refs_parameters_and_package_pin() {
    let (_temp, paths) = fixture();
    let graph = json!({"version":"0.1.0","robot":{"id":"robot","name":"Robot"},"cores":{}});
    let values = BTreeMap::from([("socket".to_owned(), "/tmp/sim.sock".to_owned())]);
    let preview = compose(&paths, "test/sim", "2.0.0", "hardware", &graph, &values).unwrap();
    assert_eq!(preview["added_paths"].as_array().unwrap().len(), 4);
    assert_eq!(
      preview["graph"]["cores"]["eos"]["layers"]["hardware"]["groups"]["demo"]["nodes"]["front"]["adapter"]
        ["settings"]["backend"],
      "eos/process/demo/simulator"
    );
    assert!(
      preview["graph"]["cores"]["eos"]["layers"]["interface"]["groups"]["gate"]["nodes"]["gate"]
        .get("package")
        .is_none()
    );
    assert!(
      preview["graph"]["cores"]["isosui"]["layers"]["system"]["groups"]["ui"]["nodes"]["vision"]
        .get("package")
        .is_none()
    );
    assert_eq!(
      preview["graph"]["cores"]["eos"]["layers"]["hardware"]["groups"]["demo"]["nodes"]["front"]["package"]
        ["version"],
      "2.0.0"
    );
    assert!(
      compose(
        &paths,
        "test/sim",
        "2.0.0",
        "hardware",
        &preview["graph"],
        &values
      )
      .is_err()
    );
    assert!(
      compose(
        &paths,
        "test/sim",
        "2.0.0",
        "hardware",
        &graph,
        &BTreeMap::new()
      )
      .is_err()
    );
  }
}
