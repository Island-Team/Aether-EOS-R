//! Create the editable shared graph from the generic preset.

use crate::{app::config::Paths, domain::graph::graph_compat, domain::graph::graph_migration};
use anyhow::{Context, Result};
use serde_json::{Value, json};
use std::{
  fs,
  path::{Path, PathBuf},
};

fn at<'a>(graph: &'a mut Value, pointer: &str) -> Result<&'a mut Value> {
  graph
    .pointer_mut(pointer)
    .with_context(|| format!("preset lacks {pointer}"))
}

pub fn draft(paths: &Paths, name: &str) -> Result<Value> {
  let preset: toml::Value = toml::from_str(include_str!("../../../templates/generic-graph.toml"))?;
  let mut graph = serde_json::to_value(preset)?;
  at(&mut graph, "/robot/id")?.clone_from(&json!(name));
  let graph_path = paths.node_graph_path(name).to_string_lossy().into_owned();
  let eos_socket = paths
    .runtime
    .join(format!("{name}-eos.sock"))
    .to_string_lossy()
    .into_owned();
  let journal = paths
    .data
    .join("journal")
    .join(name)
    .to_string_lossy()
    .into_owned();
  let eos = "/cores/isos/layers/process/groups/component/nodes/eos/settings";
  at(&mut graph, &format!("{eos}/args"))?.clone_from(&json!([
    "--profile",
    graph_path,
    "--journal-dir",
    journal,
    "--socket",
    eos_socket
  ]));
  at(&mut graph, &format!("{eos}/ready_unix"))?.clone_from(&json!(eos_socket));
  at(
    &mut graph,
    "/cores/isos/layers/process/groups/component/nodes/bridge/settings/args",
  )?
  .clone_from(&json!([
    "--socket",
    eos_socket,
    "--listen",
    "127.0.0.1:16020"
  ]));
  at(
    &mut graph,
    "/cores/isos/layers/process/groups/component/nodes/config/settings/args",
  )?
  .clone_from(&json!([
    "--profile",
    graph_path,
    "--listen",
    "127.0.0.1:16021"
  ]));
  let bytes = serde_json::to_vec(&graph)?;
  let saved = graph_compat::parse(&bytes)?;
  Ok(
    graph_migration::propose(&saved)
      .map_err(|diagnostic| anyhow::anyhow!(diagnostic.to_string()))?
      .graph,
  )
}

pub fn init(paths: &Paths, name: &str) -> Result<PathBuf> {
  let path = paths.node_graph_path(name);
  if !path.exists() {
    let obsolete = paths
      .config
      .join("deployments")
      .join(format!("{name}.json"));
    anyhow::ensure!(
      !obsolete.exists(),
      "obsolete component graph {} cannot run; create and review {} with the current node graph format; the old file was left untouched",
      obsolete.display(),
      path.display()
    );
    let bytes = serde_json::to_vec_pretty(&draft(paths, name)?)?;
    crate::app::config::atomic_write(&path, &bytes)?;
  }
  Ok(path)
}

pub fn revision(path: &Path) -> Result<String> {
  use sha2::{Digest, Sha256};
  Ok(hex::encode(Sha256::digest(fs::read(path)?)))
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn starter_has_no_hardware_until_package_set_is_added() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let graph = draft(&paths, "robot-a").unwrap();
    assert!(graph.pointer("/cores/eos/layers/process").is_none());
    assert!(graph.pointer("/cores/eos/layers/hardware").is_none());
    assert_eq!(graph["robot"]["name"], "Aether deployment");
  }

  #[test]
  fn init_keeps_authored_graph_and_uses_private_runtime_socket() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let path = init(&paths, "robot-a").unwrap();
    let graph: Value = serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
    assert!(graph.pointer("/cores/eos/layers/process").is_none());
    assert_eq!(
      graph
        .pointer("/cores/isos/layers/process/groups/component/nodes/config/settings/depends_on/0")
        .and_then(Value::as_str),
      Some("cores/isos/layers/process/groups/component/nodes/eos")
    );
    fs::write(&path, "authored").unwrap();
    assert_eq!(init(&paths, "robot-a").unwrap(), path);
    assert_eq!(fs::read_to_string(path).unwrap(), "authored");
  }

  #[test]
  fn obsolete_component_graph_is_left_untouched() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let obsolete = paths.config.join("deployments/robot-a.json");
    fs::create_dir_all(obsolete.parent().unwrap()).unwrap();
    fs::write(&obsolete, b"old deployment").unwrap();
    let error = init(&paths, "robot-a").unwrap_err().to_string();
    assert!(error.contains("obsolete component graph"));
    assert!(error.contains("robot-a.nodes.json"));
    assert_eq!(fs::read(&obsolete).unwrap(), b"old deployment");
    assert!(!paths.node_graph_path("robot-a").exists());
  }

  #[test]
  fn starter_declares_dispatcher_without_uninstalled_hardware() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let graph = draft(&paths, "robot-a").unwrap();
    assert!(graph.pointer("/cores/eos/layers/control").is_none());
    assert_eq!(
      graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["control"]["nodes"]["command"]["node_type"],
      "eos.dispatcher.control@v0.1"
    );
  }
}
