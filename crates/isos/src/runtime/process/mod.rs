//! IsOS-owned process projection of the shared v1 node graph.
//!
//! The graph remains the authored source. This module admits only registered
//! IsOS process nodes and keeps direct child handles in the manager process.

pub(crate) mod candidate_skill_preflight;
pub(crate) mod core_bins;
pub(crate) mod preflight;
pub(crate) mod reader_features;
pub(crate) mod readiness;
pub(crate) mod skill_selection;
pub(crate) mod vision_binding;
use crate::{
  app::config::{self, Paths},
  runtime::owners::control_grants,
  runtime::owners::dependent_stop,
  domain::graph::edge_admission,
  domain::graph::graph_compat,
  domain::graph::graph_migration,
  runtime::logs as log_store,
  domain::launch as package_process,
  domain::packages,
};
use anyhow::{Context, Result, bail, ensure};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::{
  collections::{BTreeMap, BTreeSet},
  fs,
  net::{TcpStream, ToSocketAddrs},
  os::unix::{ffi::OsStrExt, fs::PermissionsExt, process::CommandExt},
  path::{Path, PathBuf},
  process::{Child, Command, ExitStatus, Stdio},
  sync::{
    Arc, Mutex,
    mpsc::{self, Receiver},
  },
  thread,
  time::{Duration, Instant},
};

const MAX_GRAPH_BYTES: u64 = 1024 * 1024;
const MAX_COMMAND_ARGS: usize = 64;
const MAX_RESTARTS: u32 = 10;

#[derive(Debug)]
pub(crate) struct PackageLoadFailure {
  pub id: String,
  pub version: String,
  pub stage: &'static str,
  pub node_path: Option<String>,
  pub source: anyhow::Error,
}

impl std::fmt::Display for PackageLoadFailure {
  fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      formatter,
      "Reinstall or relink {}@{}",
      self.id, self.version
    )
  }
}

impl std::error::Error for PackageLoadFailure {
  fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
    Some(self.source.as_ref())
  }
}

fn canonical_node_path(path: &str) -> String {
  let parts: Vec<_> = path.split('/').collect();
  if parts.len() == 8
    && parts[0] == "cores"
    && parts[2] == "layers"
    && parts[4] == "groups"
    && parts[6] == "nodes"
  {
    format!("{}/{}/{}/{}", parts[1], parts[3], parts[5], parts[7])
  } else {
    path.to_owned()
  }
}

fn verify_admitted_handoff_fingerprint(
  executable: &Path,
  handoff_digest: &str,
  admitted_fingerprint: Option<&str>,
) -> Result<()> {
  let mut digest = Sha256::new();
  digest.update(fs::read(executable)?);
  digest.update(handoff_digest.as_bytes());
  ensure!(
    admitted_fingerprint == Some(hex::encode(digest.finalize()).as_str()),
    "package handoff differs from admitted process launch"
  );
  Ok(())
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
struct Graph {
  version: String,
  #[allow(dead_code)]
  robot: Value,
  cores: BTreeMap<String, Core>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
struct Core {
  layers: BTreeMap<String, Layer>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
struct Layer {
  groups: BTreeMap<String, Group>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
struct Group {
  nodes: BTreeMap<String, GraphNode>,
}

#[derive(Debug, Clone, Deserialize)]
struct GraphNode {
  id: String,
  node_type: String,
  #[serde(default)]
  settings: Value,
  #[serde(default)]
  adapter: Option<Value>,
  #[serde(default)]
  package: Option<package_process::PackageIdentity>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ProcessSettings {
  #[serde(default)]
  pub command: Vec<String>,
  #[serde(default)]
  pub role: Option<String>,
  #[serde(default)]
  pub args: Vec<String>,
  #[serde(default)]
  pub cwd: Option<PathBuf>,
  #[serde(default)]
  pub env: BTreeMap<String, String>,
  #[serde(default)]
  pub env_from: BTreeMap<String, String>,
  #[serde(default)]
  pub skill_libraries: Vec<crate::runtime::process::skill_selection::Entry>,
  #[serde(default)]
  pub ready_tcp: Option<String>,
  #[serde(default)]
  pub ready_unix: Option<PathBuf>,
  #[serde(default = "default_start_timeout_ms")]
  pub start_timeout_ms: u64,
  #[serde(default)]
  #[allow(dead_code)]
  // The graph field remains readable; R06 blocks automatic restart after uncertain exit.
  pub restart: Restart,
  #[serde(default)]
  pub max_restarts: u32,
  #[serde(default)]
  pub depends_on: Vec<String>,
}

fn default_start_timeout_ms() -> u64 {
  30_000
}

#[derive(Debug, Clone, Copy, Default, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Restart {
  #[default]
  Never,
  OnFailure,
  Always,
}

#[derive(Debug, Clone)]
pub struct ProcessSpec {
  pub id: String,
  pub path: String,
  pub settings: ProcessSettings,
  pub package: Option<package_process::PackageIdentity>,
  pub fingerprint: Option<String>,
}

fn valid_segment(value: &str) -> bool {
  !value.is_empty()
    && value.len() <= 48
    && value
      .bytes()
      .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_')
}

pub fn validate_deployment_name(name: &str) -> Result<()> {
  ensure!(valid_segment(name), "invalid deployment name");
  Ok(())
}

impl ProcessSpec {
  fn validate(&self) -> Result<()> {
    ensure!(
      valid_segment(&self.id),
      "invalid process node id {}",
      self.id
    );
    let s = &self.settings;
    ensure!(
      !s.command.is_empty()
        && s.command.len() <= MAX_COMMAND_ARGS
        && s
          .command
          .iter()
          .all(|arg| !arg.is_empty() && arg.len() <= 4096),
      "{}: command must have 1..={MAX_COMMAND_ARGS} bounded arguments",
      self.id
    );
    ensure!(
      Path::new(&s.command[0]).is_absolute(),
      "{}: command executable must be an absolute path",
      self.id
    );
    if let Some(cwd) = &s.cwd {
      ensure!(cwd.is_absolute(), "{}: cwd must be absolute", self.id);
    }
    ensure!(
      (1..=120_000).contains(&s.start_timeout_ms),
      "{}: start_timeout_ms must be 1..=120000",
      self.id
    );
    ensure!(
      s.max_restarts <= MAX_RESTARTS,
      "{}: max_restarts exceeds {MAX_RESTARTS}",
      self.id
    );
    if let Some(endpoint) = &s.ready_tcp {
      ensure!(
        endpoint.to_socket_addrs()?.next().is_some(),
        "{}: ready_tcp has no address",
        self.id
      );
    }
    ensure!(
      s.ready_tcp.is_none() || s.ready_unix.is_none(),
      "{}: choose only one of ready_tcp and ready_unix",
      self.id
    );
    if let Some(path) = &s.ready_unix {
      ensure!(
        path.is_absolute()
          && !path.as_os_str().as_bytes().contains(&0)
          && path.as_os_str().as_bytes().len() <= 107,
        "{}: ready_unix must be an absolute Unix socket path of at most 107 bytes without NUL",
        self.id
      );
    }
    for (key, value) in &s.env {
      ensure!(
        !key.is_empty()
          && key.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_')
          && !value.contains('\0'),
        "{}: invalid environment variable {key}",
        self.id
      );
    }
    ensure!(
      s.env_from.len() <= 32,
      "{}: too many env_from entries",
      self.id
    );
    for (target, source) in &s.env_from {
      ensure!(
        crate::domain::graph::generated_v2::valid_env(target)
          && crate::domain::graph::generated_v2::valid_env(source)
          && !s.env.contains_key(target),
        "{}: invalid or duplicate env_from target",
        self.id
      );
    }
    Ok(())
  }
}

/// Extract and order IsOS-owned process nodes from the single authored graph.
#[cfg(test)]
pub fn project(source: &str) -> Result<Vec<ProcessSpec>> {
  project_inner(None, source)
}

pub fn project_with_packages(paths: &Paths, source: &str) -> Result<Vec<ProcessSpec>> {
  let specs = project_inner(Some(paths), source)?;
  for spec in &specs {
    let executable = Path::new(&spec.settings.command[0]);
    let metadata = fs::metadata(executable).with_context(|| {
      format!(
        "{}: executable {} is missing",
        spec.id,
        executable.display()
      )
    })?;
    ensure!(
      metadata.is_file() && metadata.permissions().mode() & 0o111 != 0,
      "{}: executable {} is not runnable",
      spec.id,
      executable.display()
    );
  }
  Ok(specs)
}

pub fn use_generated_eos_profile(specs: &mut [ProcessSpec], path: &Path) -> Result<()> {
  ensure!(
    path.is_absolute() && path.is_file(),
    "verified EOS generated profile is missing"
  );
  for id in ["eos", "config"] {
    let spec = specs
      .iter_mut()
      .find(|spec| spec.id == id)
      .with_context(|| format!("graph lacks {id} process"))?;
    let mut matches = spec
      .settings
      .command
      .iter()
      .enumerate()
      .filter(|(_, arg)| arg.as_str() == "--profile");
    let index = matches
      .next()
      .map(|(index, _)| index)
      .with_context(|| format!("{id} lacks --profile"))?;
    ensure!(
      matches.next().is_none() && index + 1 < spec.settings.command.len(),
      "{id} has invalid --profile arguments"
    );
    spec.settings.command[index + 1] = path.to_string_lossy().into_owned();
  }
  Ok(())
}

pub fn use_generated_egia_config(specs: &mut [ProcessSpec], root: &Path) -> Result<()> {
  let Some(spec) = specs.iter_mut().find(|spec| spec.id == "egia") else {
    return Ok(());
  };
  ensure!(
    root.is_absolute() && root.is_dir(),
    "selected EGIA configuration is missing"
  );
  let env_name = if root.file_name().is_some_and(|name| name == "ei") {
    "EI_ETC"
  } else {
    "EGIA_ETC"
  };
  ensure!(
    !spec.settings.command.iter().any(|arg| arg == "--etc")
      && !spec.settings.env.contains_key(env_name),
    "EGIA configuration must use the selected generated root"
  );
  let path = root.to_string_lossy().into_owned();
  spec.settings.command.extend(["--etc".into(), path.clone()]);
  spec.settings.env.insert(env_name.into(), path);
  for flag in ["--managed", "--no-tui"] {
    if !spec.settings.command.iter().any(|arg| arg == flag) {
      spec.settings.command.push(flag.into());
    }
  }
  if !spec.settings.command.iter().any(|arg| arg == "--mode") {
    spec
      .settings
      .command
      .extend(["--mode".into(), "isos".into()]);
  }
  Ok(())
}

fn project_inner(paths: Option<&Paths>, source: &str) -> Result<Vec<ProcessSpec>> {
  ensure!(
    source.len() as u64 <= MAX_GRAPH_BYTES,
    "node graph exceeds 1 MiB"
  );
  let value = normalize_source(source)?;
  crate::runtime::process::skill_selection::check_reserved(&value)?;
  crate::runtime::process::skill_selection::entries(&value)?;
  let sidecars = paths
    .map(|paths| packages::node_type_sidecars_for_graph(paths, &value))
    .transpose()?
    .unwrap_or_default();
  let graph: Graph = serde_json::from_value(value.clone())?;
  ensure!(
    matches!(graph.version.as_str(), "1.0.0" | "0.1.0" | "0.2.0"),
    "graph version must be 1.0.0, 0.1.0, or 0.2.0"
  );
  let mut specs = BTreeMap::new();
  let mut package_processes = BTreeMap::new();
  for (core_id, core) in &graph.cores {
    for (layer_id, layer) in &core.layers {
      for (group_id, group) in &layer.groups {
        for (key, node) in &group.nodes {
          if !matches!(layer_id.as_str(), "process" | "eos_process") || node.package.is_none() {
            continue;
          }
          let path = format!("{core_id}/{layer_id}/{group_id}/{key}");
          ensure!(
            ((core_id == "eos" && layer_id == "process")
              || (graph.version == "0.2.0" && core_id == "isos" && layer_id == "eos_process"))
              && key == &node.id,
            "{path}: package process has invalid placement or id"
          );
          let paths = paths.context("package process requires IsOS package resolver")?;
          let package = node
            .package
            .as_ref()
            .context("package process requires exact package identity")?;
          let (package_id, version) = (package.id.as_str(), package.version.as_str());
          let resolved =
            package_process::resolve(paths, package, &node.node_type, &path, &node.settings)?
              .context("package process lacks a verified launch declaration")
              .map_err(|source| PackageLoadFailure {
                id: package_id.to_owned(),
                version: version.to_owned(),
                stage: "process",
                node_path: Some(format!(
                  "/cores/{core_id}/layers/{layer_id}/groups/{group_id}/nodes/{key}"
                )),
                source,
              })?;
          ensure!(
            package_processes.insert(path, resolved).is_none(),
            "duplicate package process path"
          );
        }
      }
    }
  }
  let mut adapter_libraries = BTreeMap::new();
  let mut owner_catalog = None;
  let hardware = if graph.version == "0.2.0" {
    graph
      .cores
      .get("isos")
      .and_then(|core| core.layers.get("eos_hardware"))
  } else {
    graph
      .cores
      .get("eos")
      .and_then(|core| core.layers.get("hardware"))
  };
  if let Some(hardware) = hardware {
    for (group_id, group) in &hardware.groups {
      for (node_id, node) in &group.nodes {
        let Some(adapter) = &node.adapter else {
          continue;
        };
        let Some(family) = adapter.get("type").and_then(Value::as_str) else {
          continue;
        };
        let package = adapter
          .get("package")
          .and_then(Value::as_object)
          .and_then(|package| {
            Some((
              package.get("id")?.as_str()?,
              package.get("version")?.as_str()?,
            ))
          })
          .or_else(|| {
            node
              .package
              .as_ref()
              .map(|package| (package.id.as_str(), package.version.as_str()))
          });
        let Some(package) = package else {
          // A built-in adapter is checked against the selected owner's registry.
          continue;
        };
        let paths = paths.context("packaged adapter requires IsOS package resolver")?;
        if owner_catalog.is_none() {
          owner_catalog = Some(catalog_for_option_graph(paths, &value)?);
        }
        let compiled = owner_catalog.as_ref().is_some_and(|catalog| {
          catalog["adapter_families"]
            .as_array()
            .is_some_and(|families| {
              families
                .iter()
                .any(|entry| entry["id"] == family && entry["compiled"] == true)
            })
        });
        if compiled {
          // The selected owner already implements this family. Its package pin and
          // declared resources are verified separately during graph admission.
          continue;
        }
        let (library, digest) = packages::verified_adapter_library(
          paths, package.0, package.1, family,
        )
        .map_err(|source| PackageLoadFailure {
          id: package.0.to_owned(),
          version: package.1.to_owned(),
          stage: "adapter",
          node_path: Some(format!(
            "/cores/{}/layers/{}/groups/{group_id}/nodes/{node_id}",
            if graph.version == "0.2.0" {
              "isos"
            } else {
              "eos"
            },
            if graph.version == "0.2.0" {
              "eos_hardware"
            } else {
              "hardware"
            }
          )),
          source,
        })?;
        let argument = format!("{family}={}@{digest}", library.display());
        if let Some(previous) = adapter_libraries.insert(family.to_owned(), argument.clone()) {
          ensure!(
            previous == argument,
            "adapter family {family} resolves to conflicting libraries"
          );
        }
      }
    }
  }
  for (core_id, core) in graph.cores {
    for (layer_id, layer) in core.layers {
      for (group_id, group) in layer.groups {
        for (key, node) in group.nodes {
          let path = format!("{core_id}/{layer_id}/{group_id}/{key}");
          if matches!(layer_id.as_str(), "process" | "eos_process") && node.package.is_some() {
            continue;
          }
          if matches!(
            node.node_type.as_str(),
            "isos.process.component@v1.1"
              | "isos.process.component@v0.1"
              | "isos.process.component@v0.2"
          ) {
            ensure!(
              core_id == "isos" && layer_id == "process" && group_id == "component",
              "{path}: process type has invalid placement"
            );
            ensure!(key == node.id, "{path}: node id differs from map key");
            let mut settings: ProcessSettings = serde_json::from_value(node.settings)
              .with_context(|| format!("{path}: invalid process settings"))?;
            ensure!(
              matches!(
                node.node_type.as_str(),
                "isos.process.component@v0.1" | "isos.process.component@v0.2"
              ) || settings.env_from.is_empty(),
              "{path}: env_from requires the @v0.1 component type"
            );
            ensure!(
              matches!(
                node.node_type.as_str(),
                "isos.process.component@v0.1" | "isos.process.component@v0.2"
              ) || settings.skill_libraries.is_empty(),
              "{path}: skill_libraries requires the @v0.1 component type"
            );
            let binary = match key.as_str() {
              "eos" => "aether-runtime",
              "bridge" => "aether-http-bridge",
              "config" => "aether-config-server",
              _ => "",
            };
            if !binary.is_empty() && paths.is_some() {
              ensure!(
                settings.role.as_deref().is_none_or(|role| role == "eos"),
                "{path}: role must be eos"
              );
              ensure!(
                settings.command.is_empty(),
                "{path}: command is obsolete; use role and args"
              );
              let args = settings.args.clone();
              settings.command =
                std::iter::once(core_bins::resolve(binary)?.to_string_lossy().into_owned())
                  .chain(args)
                  .collect();
              settings.role = Some("eos".into());
            } else if key == "egia" && paths.is_some() {
              ensure!(
                settings.role.as_deref().is_none_or(|role| role == "egia"),
                "{path}: role must be egia"
              );
              ensure!(
                settings.command.is_empty(),
                "{path}: command is obsolete; use role and args"
              );
              let args = settings.args.clone();
              let executable =
                crate::runtime::owners::core_setup::adapter_path(paths.unwrap(), "egia")?
                  .with_file_name("egia");
              ensure!(
                executable.is_file(),
                "EGIA executable missing at {}",
                executable.display()
              );
              settings.command = std::iter::once(executable.to_string_lossy().into_owned())
                .chain(args)
                .collect();
              settings.role = Some("egia".into());
              settings
                .env
                .extend(crate::domain::graph::generated_v2::egia_launch_env(&value)?);
            }
            for dependency in &mut settings.depends_on {
              *dependency = canonical_node_path(dependency);
            }
            let mut spec = ProcessSpec {
              id: key.clone(),
              path,
              settings,
              package: None,
              fingerprint: None,
            };
            if key == "eos" || key == "config" {
              for sidecar in &sidecars {
                spec.settings.command.extend([
                  "--node-types".into(),
                  sidecar.to_string_lossy().into_owned(),
                ]);
              }
            }
            if key == "eos" {
              for argument in adapter_libraries.values() {
                spec
                  .settings
                  .command
                  .extend(["--adapter-library".into(), argument.clone()]);
              }
              ensure!(
                package_processes.len() <= 1,
                "selected owner supports one package process"
              );
              for resolved in package_processes.values() {
                let handoff = paths
                  .context("package handoff requires IsOS paths")?
                  .runtime
                  .join(format!("package-process-{}.json", resolved.fingerprint));
                config::atomic_write(&handoff, &resolved.launch_bytes)?;
                spec.settings.command.extend([
                  "--package-process".into(),
                  handoff.to_string_lossy().into_owned(),
                ]);
                spec.settings.env.extend(resolved.environment.clone());
                spec.package = Some(resolved.package.clone());
                spec.fingerprint = Some(resolved.fingerprint.clone());
              }
            }
            if paths.is_some() && matches!(key.as_str(), "eos" | "bridge" | "config" | "egia") {
              let mut digest = Sha256::new();
              digest.update(fs::read(&spec.settings.command[0])?);
              if let Some(backend) = &spec.fingerprint {
                digest.update(backend.as_bytes());
              }
              spec.fingerprint = Some(hex::encode(digest.finalize()));
            }
            spec.validate()?;
            ensure!(
              specs.insert(key.clone(), spec).is_none(),
              "duplicate process id {key}"
            );
          } else if core_id == "isos" {
            if graph.version == "0.2.0"
              && node.node_type.starts_with(&format!("isos.{layer_id}."))
              && node.node_type.ends_with("@v0.2")
            {
              continue;
            }
            if node.node_type == "isos.ui.vision@v1.0"
              && layer_id == "ui"
              && group_id == "panel"
              && key == node.id
            {
              continue;
            }
            if node.node_type == "isos.interface.http_gateway@v0.1"
              && layer_id == "interface"
              && group_id == "gateway"
              && key == node.id
            {
              // The gateway is handled inside IsOS's HTTP route, not a child process.
              continue;
            }
            bail!("{path}: unregistered IsOS node type {}", node.node_type);
          }
        }
      }
    }
  }
  ensure!(
    package_processes.is_empty() || specs.contains_key("eos"),
    "package process requires a selected owner runtime"
  );
  validate_listener_ports(&specs)?;
  let mut ordered = Vec::with_capacity(specs.len());
  let mut pending: BTreeMap<String, BTreeSet<String>> = BTreeMap::new();
  for (id, spec) in &specs {
    let mut deps = BTreeSet::new();
    for dep in &spec.settings.depends_on {
      let dep_id = dep.rsplit('/').next().unwrap_or(dep);
      let provider = specs
        .get(dep_id)
        .ok_or_else(|| anyhow::anyhow!("{} depends on unknown process {dep}", id))?;
      ensure!(
        dep == &provider.path,
        "{}: noncanonical process dependency {dep}",
        id
      );
      ensure!(id != dep_id, "{id} cannot depend on itself");
      ensure!(
        deps.insert(dep_id.to_owned()),
        "{id}: duplicate dependency {dep}"
      );
    }
    pending.insert(id.clone(), deps);
  }
  while !pending.is_empty() {
    let Some(next) = pending
      .iter()
      .find(|(_, deps)| deps.is_empty())
      .map(|(id, _)| id.clone())
    else {
      bail!("IsOS process dependencies contain a cycle")
    };
    ordered.push(specs.remove(&next).unwrap());
    pending.remove(&next);
    for deps in pending.values_mut() {
      deps.remove(&next);
    }
  }
  Ok(ordered)
}

/// Reject obsolete graph shapes before delegating admission to the core registry.
fn reject_obsolete_nodes(graph: &Value) -> Result<()> {
  if let Some(nodes) = graph
    .pointer("/cores/isos/layers/process/groups/component/nodes")
    .and_then(Value::as_object)
  {
    for (id, node) in nodes {
      ensure!(
        node["node_type"] != "isos.process.component@v1.0"
          && node["settings"].get("command").is_none(),
        "{id}: obsolete executable command; set node_type to isos.process.component@v1.1 and use settings.role plus settings.args, then save the graph"
      );
    }
  }
  Ok(())
}

fn validate_listener_ports(specs: &BTreeMap<String, ProcessSpec>) -> Result<()> {
  let api = crate::app::server::local_port()?;
  let ui = crate::app::server::ui_port()?;
  let mut claimed = BTreeMap::<u16, &str>::new();
  for (id, spec) in specs {
    let mut endpoints = Vec::new();
    if let Some(ready) = &spec.settings.ready_tcp {
      endpoints.push(ready.as_str());
    }
    for pair in spec.settings.command.windows(2) {
      if matches!(id.as_str(), "eos" | "bridge" | "config" | "egia") && pair[0] == "--listen" {
        endpoints.push(&pair[1]);
      }
    }
    for endpoint in endpoints {
      for address in endpoint
        .to_socket_addrs()
        .with_context(|| format!("{id}: invalid listener address {endpoint}"))?
      {
        if !address.ip().is_loopback() {
          continue;
        }
        let port = address.port();
        ensure!(
          port != api && port != ui,
          "{id}: listener port {port} conflicts with IsOS workbench"
        );
        if let Some(owner) = claimed.insert(port, id) {
          ensure!(
            owner == id,
            "{id}: listener port {port} conflicts with {owner}"
          );
        }
      }
    }
  }
  Ok(())
}

pub fn normalize_source(source: &str) -> Result<Value> {
  ensure!(
    source.len() as u64 <= MAX_GRAPH_BYTES,
    "node graph exceeds 1 MiB"
  );
  let graph = if source.trim_start().starts_with('{') {
    serde_json::from_str(source)?
  } else {
    let value: toml::Value = toml::from_str(source)?;
    serde_json::to_value(value)?
  };
  if let Some(diagnostic) = graph_compat::version_diagnostic(&graph) {
    bail!(diagnostic);
  }
  Ok(graph)
}

pub fn validate_value(paths: &Paths, graph: &Value) -> Result<()> {
  crate::runtime::owners::egia_state::reject_test_env_graph(graph)?;
  crate::runtime::process::skill_selection::check_reserved(graph)?;
  crate::runtime::process::skill_selection::entries(graph)?;
  packages::verify_graph_packages(paths, graph)?;
  package_process::validate_graph(paths, graph)?;
  graph_compat::admit_legacy_mapped(graph, catalog().ok().as_ref())?;
  validate_value_with_profile(paths, graph, &core_bins::resolve("aether-profile")?)
}

pub fn validate_value_with_profile(paths: &Paths, graph: &Value, profile: &Path) -> Result<()> {
  crate::runtime::owners::egia_state::reject_test_env_graph(graph)?;
  crate::runtime::process::skill_selection::check_reserved(graph)?;
  crate::runtime::process::skill_selection::entries(graph)?;
  packages::verify_graph_packages(paths, graph)?;
  package_process::validate_graph(paths, graph)?;
  graph_compat::admit_legacy_mapped(graph, catalog().ok().as_ref())?;
  reject_obsolete_nodes(graph)?;
  let eos_graph = if graph["version"] == "0.1.0" {
    if let Some(diagnostic) = graph_migration::cross_core_diagnostic(graph) {
      bail!(diagnostic);
    }
    let catalog = catalog_for_graph(paths, graph)?;
    edge_admission::admit(graph, &catalog).map_err(|diagnostic| anyhow::anyhow!("{diagnostic}"))?;
    let mut projection = graph.clone();
    projection["cores"]
      .as_object_mut()
      .context("graph cores must be an object")?
      .retain(|owner, _| owner == "eos");
    projection
  } else {
    graph.clone()
  };
  paths.prepare()?;
  let mut file = tempfile::Builder::new()
    .prefix("isos-graph-")
    .suffix(".json")
    .tempfile_in(&paths.runtime)?;
  serde_json::to_writer(&mut file, &eos_graph)?;
  use std::io::Write;
  file.flush()?;
  validate_with_program(file.path(), profile)?;
  project_with_packages(paths, &serde_json::to_string(graph)?)?;
  Ok(())
}

/// Admit the full owner graph, then validate only the EOS projection with its
/// registry. IsOS rechecks process nodes before supervising any child.
pub fn validate_full_graph(path: &Path) -> Result<()> {
  let graph = normalize_source(&fs::read_to_string(path)?)?;
  let paths = Paths::discover()?;
  validate_value(&paths, &graph)
}

mod catalog;
pub use catalog::{catalog, catalog_at, catalog_for_graph, catalog_for_option_graph};
#[cfg(test)]
pub(crate) use catalog::merge_egia_stage_catalog;

fn validate_with_program(path: &Path, program: &Path) -> Result<()> {
  let mut command = Command::new(program);
  command.arg("validate").arg("--file").arg(path);
  let graph = normalize_source(&fs::read_to_string(path)?)?;
  for sidecar in packages::node_type_sidecars_for_graph(&Paths::discover()?, &graph)? {
    command.arg("--node-types").arg(sidecar);
  }
  let output = command
    .output()
    .with_context(|| format!("run EOS node graph validator {}", program.display()))?;
  ensure!(
    output.status.success(),
    "EOS node graph validation failed: {}",
    String::from_utf8_lossy(&output.stderr).trim()
  );
  Ok(())
}

mod supervisor;
pub use supervisor::Supervisor;

#[cfg(test)]
mod projection_tests {
  use super::*;

  #[test]
  fn registered_isos_http_gateway_has_no_child_process() {
    let graph = serde_json::json!({"version":"0.1.0","robot":{},"cores":{
      "isos":{"layers":{"interface":{"groups":{"gateway":{"nodes":{
        "control":{"id":"control","node_type":"isos.interface.http_gateway@v0.1","settings":{}}
      }}}}}}
    }});
    assert!(project(&graph.to_string()).unwrap().is_empty());
  }
}
