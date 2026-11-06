//! Explicit, deterministic conversion of saved legacy graph values.

use crate::{
  app::config::{self, Paths},
  runtime::owners::core_setup,
  domain::graph::edge_admission,
  runtime::owners::egia_state,
  domain::graph::graph_compat::{self, Diagnostic, SavedGraph},
  runtime::lock,
  domain::packages,
  runtime::process,
  runtime::process::reader_features,
};
use serde::Serialize;
use serde_json::{Value, json};
use std::{collections::BTreeMap, fs, path::PathBuf};

const TYPE_MAP: &[(&str, &str)] = &[
  ("eos.hardware.camera@v1.0", "eos.hardware.camera@v0.1"),
  ("eos.hardware.base@v1.0", "eos.hardware.base@v0.1"),
  (
    "eos.hardware.manipulator@v1.0",
    "eos.hardware.manipulator@v0.1",
  ),
  ("eos.dispatcher.control@v1.0", "eos.dispatcher.control@v0.1"),
  (
    "eos.dispatcher.primitive_base@v1.0",
    "eos.dispatcher.primitive_base@v0.1",
  ),
  (
    "eos.dispatcher.primitive_manipulator@v1.0",
    "eos.dispatcher.primitive_manipulator@v0.1",
  ),
  (
    "eos.interface.camera_gate@v1.0",
    "eos.interface.camera_gate@v0.1",
  ),
  ("isos.process.component@v1.1", "isos.process.component@v0.1"),
];
const PASS_THROUGH: &[&str] = &[
  "egia.common.source@v1.0",
  "egia.common.context@v1.0",
  "egia.common.planner@v1.0",
  "egia.common.guard@v1.0",
];
const ROOT_FIELDS: &[&str] = &["version", "robot", "cores"];
const ROBOT_FIELDS: &[&str] = &["id", "name"];
const NODE_FIELDS: &[&str] = &[
  "id",
  "node_type",
  "label",
  "description",
  "capabilities",
  "settings",
  "inputs",
  "references",
  "adapter",
  "package",
  "health_timing",
  "observation",
  "movement",
  "frame_id",
  "joint_names",
  "enabled",
];

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
pub struct Change {
  pub path: String,
  pub from: String,
  pub to: String,
}

pub struct Proposal {
  pub graph: Value,
  pub bytes: Vec<u8>,
  pub revision: String,
  pub changes: Vec<Change>,
}

pub struct Prepared {
  pub specs: Vec<process::ProcessSpec>,
  pub files: Vec<(PathBuf, Vec<u8>)>,
  pub edges: Vec<edge_admission::Edge>,
}

pub enum MigrationError {
  Conflict(String),
  Diagnostic(Diagnostic),
  Generated(crate::domain::graph::generated_v2::Diagnostic),
  Filesystem(anyhow::Error),
  OutcomeUncertain,
}

impl std::fmt::Display for MigrationError {
  fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    match self {
      Self::Conflict(message) => write!(formatter, "{message}"),
      Self::Diagnostic(diagnostic) => write!(formatter, "{diagnostic}"),
      Self::Generated(diagnostic) => write!(formatter, "{diagnostic}"),
      Self::Filesystem(error) => write!(formatter, "{error:#}"),
      Self::OutcomeUncertain => write!(
        formatter,
        "generated_commit_uncertain: Reread the saved graph and selected generation"
      ),
    }
  }
}

fn generated_error(error: anyhow::Error) -> MigrationError {
  if let Some(diagnostic) = error.downcast_ref::<crate::domain::graph::generated_v2::Diagnostic>() {
    return MigrationError::Generated(diagnostic.clone());
  }
  MigrationError::Filesystem(error)
}

#[cfg(test)]
static FAIL_GRAPH_PARENT_SYNC_FOR: std::sync::OnceLock<
  std::sync::Mutex<Option<std::path::PathBuf>>,
> = std::sync::OnceLock::new();

fn sync_graph_parent(path: &std::path::Path) -> Result<(), MigrationError> {
  #[cfg(test)]
  {
    let mut pending = FAIL_GRAPH_PARENT_SYNC_FOR
      .get_or_init(|| std::sync::Mutex::new(None))
      .lock()
      .unwrap();
    if pending.as_deref() == Some(path) {
      *pending = None;
      return Err(MigrationError::OutcomeUncertain);
    }
  }
  #[cfg(debug_assertions)]
  if crate::domain::graph::generated_v2::test_fail_parent_sync(path, "graph") {
    return Err(MigrationError::OutcomeUncertain);
  }
  fs::File::open(path.parent().unwrap())
    .and_then(|file| file.sync_all())
    .map_err(|_| MigrationError::OutcomeUncertain)
}

fn generated_prepare_error(error: anyhow::Error) -> Diagnostic {
  if let Some(generated) = error.downcast_ref::<crate::domain::graph::generated_v2::Diagnostic>() {
    let mut result = diagnostic(&generated.path, generated.code, generated.message);
    result
      .context
      .insert("stage".into(), Value::String(generated.stage.into()));
    return result;
  }
  let mut result = diagnostic(
    "",
    "generated_owner_rejected",
    "Restore the selected owner and correct the graph",
  );
  result
    .context
    .insert("stage".into(), Value::String("render".into()));
  result
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

pub(crate) fn package_diagnostic(error: &anyhow::Error) -> Diagnostic {
  if let Some(failure) = error
    .chain()
    .find_map(|cause| cause.downcast_ref::<packages::PackageFailure>())
  {
    let mut context = BTreeMap::new();
    context.insert("id".into(), Value::String(failure.id.clone()));
    context.insert("version".into(), Value::String(failure.version.clone()));
    context.insert("stage".into(), Value::String(failure.stage.into()));
    return Diagnostic {
      path: failure.path.clone(),
      code: failure.code,
      message: failure.to_string(),
      context,
    };
  }
  let message = error.to_string();
  let path = message
    .split(" at ")
    .nth(1)
    .filter(|path| path.starts_with('/'))
    .unwrap_or("/");
  diagnostic(
    path,
    "package_invalid_manifest",
    "Repair the incomplete or invalid package pin",
  )
}

fn package_or_owner_diagnostic(error: &anyhow::Error, owner_path: &str) -> Diagnostic {
  if error
    .chain()
    .any(|cause| cause.downcast_ref::<packages::PackageFailure>().is_some())
  {
    package_diagnostic(error)
  } else {
    diagnostic(owner_path, "owner_reader_unavailable", error.to_string())
  }
}

fn edge_diagnostic(edge: edge_admission::Diagnostic) -> Diagnostic {
  let mut context = BTreeMap::new();
  for (key, value) in [
    ("source", edge.source),
    ("target", edge.target),
    ("input", edge.input),
    ("source_data_type", edge.source_data_type),
    ("target_data_type", edge.target_data_type),
  ] {
    if let Some(value) = value {
      context.insert(key.into(), json!(value));
    }
  }
  Diagnostic {
    path: edge.path,
    code: edge.code,
    message: edge.message,
    context,
  }
}

fn keys(value: &Value, path: &str, allowed: &[&str]) -> Result<(), Diagnostic> {
  let object = value.as_object().ok_or_else(|| {
    diagnostic(
      path,
      "unmapped_graph_field",
      "expected an object in the legacy graph",
    )
  })?;
  for key in object.keys() {
    if !allowed.contains(&key.as_str()) {
      return Err(diagnostic(
        format!("{path}/{}", graph_compat::pointer_segment(key)),
        "unmapped_graph_field",
        format!("legacy field {key} is not declared by the owner schema"),
      ));
    }
  }
  Ok(())
}

pub fn propose(saved: &SavedGraph) -> Result<Proposal, Diagnostic> {
  propose_inner(saved, true)
}

pub fn propose_for_inspection(saved: &SavedGraph) -> Result<Proposal, Diagnostic> {
  propose_inner(saved, false)
}

fn propose_inner(saved: &SavedGraph, enforce_change_limit: bool) -> Result<Proposal, Diagnostic> {
  if saved.value["version"] != "1.0.0" {
    return Err(diagnostic(
      "/version",
      "migration_required",
      "only a saved 1.0.0 graph can be migrated",
    ));
  }
  keys(&saved.value, "", ROOT_FIELDS)?;
  keys(&saved.value["robot"], "/robot", ROBOT_FIELDS)?;
  let mut graph = saved.value.clone();
  graph["version"] = Value::String("0.1.0".into());
  let mut changes = vec![Change {
    path: "/version".into(),
    from: "1.0.0".into(),
    to: "0.1.0".into(),
  }];
  let cores = graph["cores"]
    .as_object_mut()
    .ok_or_else(|| diagnostic("/cores", "unmapped_graph_field", "cores must be an object"))?;
  for (core_id, core) in cores {
    let core_path = format!("/cores/{}", graph_compat::pointer_segment(core_id));
    keys(core, &core_path, &["layers"])?;
    let layers = core["layers"].as_object_mut().ok_or_else(|| {
      diagnostic(
        format!("{core_path}/layers"),
        "unmapped_graph_field",
        "layers must be an object",
      )
    })?;
    for (layer_id, layer) in layers {
      let layer_path = format!(
        "{core_path}/layers/{}",
        graph_compat::pointer_segment(layer_id)
      );
      keys(layer, &layer_path, &["groups"])?;
      let groups = layer["groups"].as_object_mut().ok_or_else(|| {
        diagnostic(
          format!("{layer_path}/groups"),
          "unmapped_graph_field",
          "groups must be an object",
        )
      })?;
      for (group_id, group) in groups {
        let group_path = format!(
          "{layer_path}/groups/{}",
          graph_compat::pointer_segment(group_id)
        );
        keys(group, &group_path, &["nodes"])?;
        let nodes = group["nodes"].as_object_mut().ok_or_else(|| {
          diagnostic(
            format!("{group_path}/nodes"),
            "unmapped_graph_field",
            "nodes must be an object",
          )
        })?;
        for (node_id, node) in nodes {
          let node_path = format!(
            "{group_path}/nodes/{}",
            graph_compat::pointer_segment(node_id)
          );
          keys(node, &node_path, NODE_FIELDS)?;
          let node_type = node["node_type"].as_str().unwrap_or("");
          if node_type == "isos.process.component@v1.0"
            || (core_id == "isos" && node.pointer("/settings/command").is_some())
          {
            let path = if node_type == "isos.process.component@v1.0" {
              format!("{node_path}/node_type")
            } else {
              format!("{node_path}/settings/command")
            };
            return Err(diagnostic(
              path,
              "unsafe_process_node",
              "arbitrary-command process nodes cannot be migrated",
            ));
          }
          let Some((_, target)) = TYPE_MAP.iter().find(|(old, _)| *old == node_type) else {
            if PASS_THROUGH.contains(&node_type) {
              continue;
            }
            return Err(diagnostic(
              format!("{node_path}/node_type"),
              "unmapped_node_type",
              format!(
                "{node_type} has no approved 0.1.0 mapping; remove it or author a registered replacement"
              ),
            ));
          };
          let from = node_type.to_owned();
          node["node_type"] = Value::String((*target).into());
          changes.push(Change {
            path: format!("{node_path}/node_type"),
            from,
            to: (*target).into(),
          });
          if enforce_change_limit && changes.len() > 256 {
            return Err(diagnostic(
              "/changes",
              "migration_change_limit",
              "migration changes exceed 256 entries",
            ));
          }
        }
      }
    }
  }
  let bytes = serde_json::to_vec_pretty(&graph)
    .map_err(|error| diagnostic("/", "migration_serialization_failed", error.to_string()))?;
  Ok(Proposal {
    revision: graph_compat::revision(&bytes),
    graph,
    bytes,
    changes,
  })
}

fn core_of(path: &str) -> Option<&str> {
  if path.starts_with("cores/") {
    path.split('/').nth(1)
  } else {
    path.split('/').next()
  }
}

pub fn cross_core_diagnostic(graph: &Value) -> Option<Diagnostic> {
  let cores = graph["cores"].as_object()?;
  for (core_id, core) in cores {
    for (layer_id, layer) in core["layers"].as_object()? {
      for (group_id, group) in layer["groups"].as_object()? {
        for (node_id, node) in group["nodes"].as_object()? {
          let base = format!(
            "/cores/{}/layers/{}/groups/{}/nodes/{}",
            graph_compat::pointer_segment(core_id),
            graph_compat::pointer_segment(layer_id),
            graph_compat::pointer_segment(group_id),
            graph_compat::pointer_segment(node_id)
          );
          if let Some(references) = node["references"].as_array() {
            for (index, value) in references.iter().enumerate() {
              if value
                .as_str()
                .and_then(core_of)
                .is_some_and(|source| source != core_id)
              {
                return Some(diagnostic(
                  format!("{base}/references/{index}"),
                  "cross_core_link_unavailable",
                  "cross-core references require G06 admission",
                ));
              }
            }
          }
          if let Some(dependencies) = node
            .pointer("/settings/depends_on")
            .and_then(Value::as_array)
          {
            for (index, value) in dependencies.iter().enumerate() {
              if value
                .as_str()
                .and_then(core_of)
                .is_some_and(|source| source != core_id)
              {
                return Some(diagnostic(
                  format!("{base}/settings/depends_on/{index}"),
                  "cross_core_link_unavailable",
                  "cross-core process dependencies require G06 admission",
                ));
              }
            }
          }
          // Input edges are admitted by edge_admission against the exact
          // registered port and trusted owner bridge catalog.
        }
      }
    }
  }
  None
}

fn eos_presence_diagnostic(graph: &Value) -> Option<Diagnostic> {
  let unified = graph["version"] == "0.2.0";
  let present = if unified {
    graph
      .pointer("/cores/isos/layers")
      .and_then(Value::as_object)
      .is_some_and(|layers| layers.keys().any(|id| id.starts_with("eos_")))
  } else {
    graph.pointer("/cores/eos").is_some()
  };
  (!present).then(|| {
    diagnostic(
      if unified {
        "/cores/isos/layers/eos_dispatcher"
      } else {
        "/cores/eos"
      },
      "owner_reader_unavailable",
      "deployment requires EOS-owned layers",
    )
  })
}

pub fn prepare(paths: &Paths, name: &str, proposal: &Proposal) -> Result<Prepared, Diagnostic> {
  let unified = proposal.graph["version"] == "0.2.0";
  let layers = proposal
    .graph
    .pointer("/cores/isos/layers")
    .and_then(Value::as_object);
  let unified_ei =
    unified && layers.is_some_and(|layers| layers.keys().any(|id| id.starts_with("ei_")));
  egia_state::reject_test_env_graph(&proposal.graph)
    .map_err(|error| egia_test_env_diagnostic(&error))?;
  crate::runtime::process::skill_selection::check_reserved(&proposal.graph)
    .map_err(skill_diagnostic)?;
  crate::runtime::process::skill_selection::entries(&proposal.graph).map_err(skill_diagnostic)?;
  // Reject an unowned core before probing binaries. A contributed core must
  // carry an exact package pin; its actual type and placement are checked
  // against the verified selected catalog below.
  if let Some(cores) = proposal.graph["cores"].as_object() {
    for (core_id, core) in cores {
      if if unified {
        core_id == "isos"
      } else {
        matches!(core_id.as_str(), "eos" | "isos" | "egia" | "isosui")
      } {
        continue;
      }
      if unified {
        return Err(diagnostic(
          format!("/cores/{core_id}"),
          "unsupported_core",
          "graph 0.2.0 requires one isos core",
        ));
      }
      let contributed = core["layers"]
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
        .any(|node| node["package"]["id"].is_string() && node["package"]["version"].is_string());
      if !contributed {
        return Err(diagnostic(
          format!("/cores/{core_id}"),
          "unsupported_core",
          format!("core {core_id} has no registered owner"),
        ));
      }
    }
  }
  packages::verify_graph_packages(paths, &proposal.graph)
    .map_err(|error| package_diagnostic(&error))?;
  crate::runtime::process::skill_selection::render(paths, &proposal.graph, &proposal.revision)
    .map_err(skill_diagnostic)?;
  if let Some(problem) = eos_presence_diagnostic(&proposal.graph) {
    return Err(problem);
  }
  reader_features::check_setup_basics(paths, &proposal.graph)?;
  let catalog = process::catalog_for_graph(paths, &proposal.graph).map_err(|error| {
    let owner = if unified_ei {
      "/cores/isos/layers/ei_io"
    } else if unified {
      "/cores/isos/layers/eos_dispatcher"
    } else if graph_compat::has_egia_stages(&proposal.graph) {
      "/cores/egia"
    } else {
      "/cores/eos"
    };
    package_or_owner_diagnostic(&error, owner)
  })?;
  if let Some(diagnostic) = graph_compat::admission_diagnostic(&proposal.graph, Some(&catalog)) {
    return Err(diagnostic);
  }
  let edges =
    edge_admission::admit(&proposal.graph, &catalog).map_err(|error| edge_diagnostic(*error))?;
  if let Some(diagnostic) = cross_core_diagnostic(&proposal.graph) {
    return Err(diagnostic);
  }
  let source = serde_json::to_string(&proposal.graph)
    .map_err(|error| diagnostic("/", "invalid_graph", error.to_string()))?;
  let specs = process::project_with_packages(paths, &source)
    .map_err(|error| package_or_owner_diagnostic(&error, "/"))?;
  crate::runtime::process::vision_binding::select(
    &proposal.graph,
    &catalog,
    &specs,
    &proposal.revision,
    b"",
  )
  .map_err(|error| diagnostic(error.path, error.code, error.message))?;
  reader_features::check(paths, &proposal.graph, &specs, &catalog)?;
  crate::domain::graph::generated_v2::check_defaults(&proposal.graph)
    .map_err(|error| generated_prepare_error(error.into()))?;
  for role in ["eos", "egia"] {
    if role == "egia" && proposal.graph.pointer("/cores/egia").is_none() && !unified_ei {
      continue;
    }
    let result = core_setup::call(
      paths,
      name,
      role,
      "validate",
      &proposal.graph,
      &proposal.revision,
    )
    .map_err(|error| {
      let _ = error;
      generated_prepare_error(crate::domain::graph::generated_v2::owner_rejected(role).into())
    })?;
    if result["ok"] != true {
      return Err(generated_prepare_error(
        crate::domain::graph::generated_v2::owner_rejected(role).into(),
      ));
    }
  }
  let mut files = core_setup::render_graph(paths, name, &proposal.graph, &proposal.revision)
    .map_err(generated_prepare_error)?;
  let projection = files
    .iter()
    .find(|(path, _)| path == std::path::Path::new("eos/graph.json"))
    .map(|(_, bytes)| bytes.as_slice())
    .ok_or_else(|| {
      diagnostic(
        if unified {
          "/cores/isos/layers/eos_dispatcher"
        } else {
          "/cores/eos"
        },
        "vision_endpoint_unavailable",
        "EOS projection is missing",
      )
    })?;
  if let Some(template) = crate::runtime::process::vision_binding::select(
    &proposal.graph,
    &catalog,
    &specs,
    &proposal.revision,
    projection,
  )
  .map_err(|error| diagnostic(error.path, error.code, error.message))?
  {
    let bytes = serde_json::to_vec_pretty(&template).map_err(|_| {
      diagnostic(
        "/vision_evidence",
        "vision_binding_changed",
        "Vision template serialization failed",
      )
    })?;
    files.push((
      std::path::PathBuf::from(crate::runtime::process::vision_binding::TEMPLATE_FILE),
      bytes,
    ));
  }
  Ok(Prepared {
    specs,
    files,
    edges,
  })
}

fn egia_test_env_diagnostic(error: &anyhow::Error) -> Diagnostic {
  let problem = error
    .downcast_ref::<egia_state::Diagnostic>()
    .expect("EGIA test graph guard returns a typed diagnostic");
  let mut diagnostic = diagnostic(&problem.path, problem.code, problem.message);
  diagnostic
    .context
    .insert("stage".into(), Value::String(problem.stage.into()));
  diagnostic
}

fn skill_diagnostic(problem: crate::runtime::process::skill_selection::Diagnostic) -> Diagnostic {
  let mut diagnostic = diagnostic(problem.path, problem.code, problem.message);
  diagnostic
    .context
    .insert("stage".into(), Value::String(problem.stage.into()));
  diagnostic
}

pub fn public_diagnostic(diagnostic: &Diagnostic) -> Value {
  if diagnostic.code.starts_with("skill_") {
    return json!({
        "code": diagnostic.code,
        "path": diagnostic.path,
        "message": diagnostic.message,
        "stage": diagnostic.context.get("stage").and_then(Value::as_str).unwrap_or("graph")
    });
  }
  json!(diagnostic)
}

fn check_digest(value: &str) -> Result<(), MigrationError> {
  if value.len() != 64
    || !value
      .bytes()
      .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
  {
    return Err(MigrationError::Diagnostic(diagnostic(
      "/revision",
      "invalid_revision",
      "revision must be a lowercase 64-character SHA-256 hex digest",
    )));
  }
  Ok(())
}

pub struct Preview {
  pub proposal: Proposal,
  pub diagnostic: Option<Diagnostic>,
}

pub fn preview(
  paths: &Paths,
  name: &str,
  expected_revision: &str,
) -> Result<Preview, MigrationError> {
  check_digest(expected_revision)?;
  let saved =
    graph_compat::read(&paths.node_graph_path(name)).map_err(MigrationError::Filesystem)?;
  if saved.revision != expected_revision {
    return Err(MigrationError::Conflict(
      "node graph revision changed; reload before migration".into(),
    ));
  }
  let proposal = propose(&saved).map_err(MigrationError::Diagnostic)?;
  let diagnostic = prepare(paths, name, &proposal).err();
  Ok(Preview {
    proposal,
    diagnostic,
  })
}

fn propose_unified(saved: &SavedGraph) -> Result<Proposal, Diagnostic> {
  let candidate = crate::domain::graph::single_core::propose(saved)?;
  Ok(Proposal {
    graph: candidate.graph,
    bytes: candidate.bytes,
    revision: candidate.revision,
    changes: Vec::new(),
  })
}

pub fn preview_unified(
  paths: &Paths,
  name: &str,
  expected_revision: &str,
) -> Result<Preview, MigrationError> {
  check_digest(expected_revision)?;
  let saved =
    graph_compat::read(&paths.node_graph_path(name)).map_err(MigrationError::Filesystem)?;
  if saved.revision != expected_revision {
    return Err(MigrationError::Conflict(
      "node graph revision changed; reload before migration".into(),
    ));
  }
  let proposal = propose_unified(&saved).map_err(MigrationError::Diagnostic)?;
  let diagnostic = prepare(paths, name, &proposal).err();
  Ok(Preview {
    proposal,
    diagnostic,
  })
}

/// Opens only after camera capture and admitted Dispatcher control replay passes.
pub fn unified_apply_ready() -> bool {
  true
}

/// Uses the same revisioned publication as the legacy migrator after the final replay gate.
pub fn commit_unified(
  paths: &Paths,
  name: &str,
  expected_revision: &str,
  expected_proposal: &str,
) -> Result<Proposal, MigrationError> {
  check_digest(expected_revision)?;
  check_digest(expected_proposal)?;
  let _lock = lock::Lock::take(paths).map_err(MigrationError::Filesystem)?;
  let path = paths.node_graph_path(name);
  let saved = graph_compat::read(&path).map_err(MigrationError::Filesystem)?;
  if saved.revision != expected_revision {
    return Err(MigrationError::Conflict(
      "node graph revision changed; reload before migration".into(),
    ));
  }
  let proposal = propose_unified(&saved).map_err(MigrationError::Diagnostic)?;
  if proposal.revision != expected_proposal {
    return Err(MigrationError::Conflict(
      "migration proposal changed; preview again".into(),
    ));
  }
  if !unified_apply_ready() {
    return Err(MigrationError::Diagnostic(diagnostic(
      "/cores/isos",
      "runtime_replay_required",
      "camera capture and admitted Dispatcher control replay is required before graph 0.2.0 Apply",
    )));
  }
  let prepared = prepare(paths, name, &proposal).map_err(MigrationError::Diagnostic)?;
  core_setup::write_revisioned(
    paths,
    name,
    &proposal.graph,
    &proposal.revision,
    prepared.files,
  )
  .map_err(generated_error)?;
  config::atomic_write(&path, &proposal.bytes).map_err(MigrationError::Filesystem)?;
  sync_graph_parent(&path)?;
  Ok(proposal)
}

pub fn commit(
  paths: &Paths,
  name: &str,
  expected_revision: &str,
  expected_proposal: &str,
) -> Result<Proposal, MigrationError> {
  check_digest(expected_revision)?;
  check_digest(expected_proposal)?;
  let _lock = lock::Lock::take(paths).map_err(MigrationError::Filesystem)?;
  let path = paths.node_graph_path(name);
  let saved = graph_compat::read(&path).map_err(MigrationError::Filesystem)?;
  if saved.revision != expected_revision {
    return Err(MigrationError::Conflict(
      "node graph revision changed; reload before migration".into(),
    ));
  }
  let proposal = propose(&saved).map_err(MigrationError::Diagnostic)?;
  if proposal.revision != expected_proposal {
    return Err(MigrationError::Conflict(
      "migration proposal changed; preview again".into(),
    ));
  }
  let prepared = prepare(paths, name, &proposal).map_err(MigrationError::Diagnostic)?;
  core_setup::write_revisioned(
    paths,
    name,
    &proposal.graph,
    &proposal.revision,
    prepared.files,
  )
  .map_err(generated_error)?;
  config::atomic_write(&path, &proposal.bytes).map_err(MigrationError::Filesystem)?;
  sync_graph_parent(&path)?;
  Ok(proposal)
}

pub fn apply_new(
  paths: &Paths,
  name: &str,
  graph: Value,
  expected_revision: &str,
) -> Result<(Proposal, Vec<process::ProcessSpec>), MigrationError> {
  if !expected_revision.is_empty() {
    check_digest(expected_revision)?;
  }
  if graph["version"] != "0.1.0" && graph["version"] != "0.2.0" {
    return Err(MigrationError::Diagnostic(diagnostic(
      "/version",
      "unsupported_graph_version",
      "new writer requires graph 0.1.0 or 0.2.0",
    )));
  }
  let bytes = serde_json::to_vec_pretty(&graph).map_err(|error| {
    MigrationError::Diagnostic(diagnostic("/", "invalid_graph", error.to_string()))
  })?;
  let revision = graph_compat::revision(&bytes);
  let proposal = Proposal {
    graph,
    bytes,
    revision,
    changes: Vec::new(),
  };
  let _lock = lock::Lock::take(paths).map_err(MigrationError::Filesystem)?;
  let path = paths.node_graph_path(name);
  let previous = match fs::read(&path) {
    Ok(bytes) => Some(bytes),
    Err(error) if error.kind() == std::io::ErrorKind::NotFound => None,
    Err(error) => return Err(MigrationError::Filesystem(error.into())),
  };
  if previous
    .as_deref()
    .map(graph_compat::revision)
    .as_deref()
    .unwrap_or("")
    != expected_revision
  {
    return Err(MigrationError::Conflict(
      "node graph revision changed; reload before applying".into(),
    ));
  }
  if let Some(previous) = &previous {
    let saved = graph_compat::parse(previous).map_err(MigrationError::Filesystem)?;
    if saved.value["version"] != proposal.graph["version"] {
      return Err(MigrationError::Diagnostic(diagnostic(
        "/version",
        "migration_required",
        "saved legacy graph requires explicit migration",
      )));
    }
  }
  let prepared = prepare(paths, name, &proposal).map_err(MigrationError::Diagnostic)?;
  if let Some(previous) = &previous {
    let saved = graph_compat::parse(previous).map_err(MigrationError::Filesystem)?;
    if saved.value == proposal.graph {
      core_setup::write_revisioned(paths, name, &saved.value, &saved.revision, prepared.files)
        .map_err(generated_error)?;
      return Ok((
        Proposal {
          revision: saved.revision,
          bytes: previous.clone(),
          ..proposal
        },
        prepared.specs,
      ));
    }
  }
  core_setup::write_revisioned(
    paths,
    name,
    &proposal.graph,
    &proposal.revision,
    prepared.files,
  )
  .map_err(generated_error)?;
  config::atomic_write(&path, &proposal.bytes).map_err(MigrationError::Filesystem)?;
  sync_graph_parent(&path)?;
  Ok((proposal, prepared.specs))
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn fresh_unified_apply_rejects_missing_owner_without_write() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let graph =
      json!({"version":"0.2.0","robot":{"id":"r","name":"R"},"cores":{"isos":{"layers":{}}}});
    assert!(matches!(
      apply_new(&paths, "default", graph, ""),
      Err(MigrationError::Diagnostic(_))
    ));
    assert!(!paths.node_graph_path("default").exists());
    assert!(!paths.generated("default").exists());
  }

  #[test]
  fn fresh_unified_writer_cannot_bypass_legacy_migration() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let path = paths.node_graph_path("default");
    let old = include_bytes!("../../../tests/fixtures/graph_compat/migration_expected_0_1_0.json");
    config::atomic_write(&path, old).unwrap();
    let graph =
      json!({"version":"0.2.0","robot":{"id":"r","name":"R"},"cores":{"isos":{"layers":{}}}});
    let error = apply_new(&paths, "default", graph, &graph_compat::revision(old))
      .err()
      .unwrap();
    let MigrationError::Diagnostic(diagnostic) = error else {
      panic!("expected migration diagnostic")
    };
    assert_eq!(diagnostic.code, "migration_required");
    assert_eq!(fs::read(path).unwrap(), old);
  }

  #[test]
  fn unified_commit_cas_and_owner_rejection_preserve_saved_bytes() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let path = paths.node_graph_path("default");
    let source =
      include_bytes!("../../../tests/fixtures/graph_compat/migration_expected_0_1_0.json");
    config::atomic_write(&path, source).unwrap();
    let saved = graph_compat::read(&path).unwrap();
    let candidate = propose_unified(&saved).unwrap();
    assert!(matches!(
      commit_unified(&paths, "default", &"0".repeat(64), &candidate.revision),
      Err(MigrationError::Conflict(_))
    ));
    assert!(matches!(
      commit_unified(&paths, "default", &saved.revision, &"0".repeat(64)),
      Err(MigrationError::Conflict(_))
    ));
    assert!(matches!(
      commit_unified(&paths, "default", &saved.revision, &candidate.revision),
      Err(MigrationError::Diagnostic(_))
    ));
    let preview = match preview_unified(&paths, "default", &saved.revision) {
      Ok(preview) => preview,
      Err(_) => panic!("preview should return a bounded owner diagnostic"),
    };
    assert!(
      preview.diagnostic.is_some(),
      "missing owners must reject preview"
    );
    assert_eq!(fs::read(&path).unwrap(), source);
    assert!(!paths.generated("default").exists());
  }

  /// Test-only staging bridge for the first complete manager replay.
  #[test]
  #[ignore = "requires real EOS and EI owner readers and an explicit isolated home"]
  fn unified_snapshot_fixture_for_supervisor_replay() {
    let source = std::env::var("ISOS_TEST_UNIFIED_GRAPH_PATH").unwrap();
    let home = PathBuf::from(std::env::var("ISOS_TEST_UNIFIED_HOME").unwrap());
    assert!(home.is_absolute());
    let paths = Paths {
      config: home.join("config"),
      data: home.join("data"),
      runtime: home.join("run"),
    };
    paths.prepare().unwrap();
    let path = paths.node_graph_path("default");
    assert!(!path.exists(), "test home already has a graph");
    let bytes = fs::read(source).unwrap();
    let graph: Value = serde_json::from_slice(&bytes).unwrap();
    assert_eq!(graph["version"], "0.2.0");
    let revision = graph_compat::revision(&bytes);
    let proposal = Proposal {
      graph,
      bytes,
      revision,
      changes: Vec::new(),
    };
    let prepared = prepare(&paths, "default", &proposal).unwrap();
    core_setup::write_revisioned(
      &paths,
      "default",
      &proposal.graph,
      &proposal.revision,
      prepared.files,
    )
    .unwrap();
    config::atomic_write(&path, &proposal.bytes).unwrap();
    assert!(sync_graph_parent(&path).is_ok());
    println!("unified test snapshot revision={}", proposal.revision);
  }

  #[test]
  fn unified_prepare_names_missing_eos_owner_layer() {
    let graph = json!({"version":"0.2.0","cores":{"isos":{"layers":{"ei_io":{}}}}});
    let diagnostic = eos_presence_diagnostic(&graph).unwrap();
    assert_eq!(diagnostic.code, "owner_reader_unavailable");
    assert_eq!(diagnostic.path, "/cores/isos/layers/eos_dispatcher");
  }

  #[test]
  fn graph_parent_sync_failure_reports_uncertain_after_visible_rename() {
    let temporary = tempfile::tempdir().unwrap();
    let path = temporary.path().join("default.nodes.json");
    let old = b"{\"version\":\"0.1.0\",\"robot\":{\"id\":\"old\"},\"cores\":{}}";
    let new = b"{\"version\":\"0.1.0\",\"robot\":{\"id\":\"new\"},\"cores\":{}}";
    config::atomic_write(&path, old).unwrap();
    config::atomic_write(&path, new).unwrap();
    *FAIL_GRAPH_PARENT_SYNC_FOR
      .get_or_init(|| std::sync::Mutex::new(None))
      .lock()
      .unwrap() = Some(path.clone());
    assert!(matches!(
      sync_graph_parent(&path),
      Err(MigrationError::OutcomeUncertain)
    ));
    let observed = fs::read(&path).unwrap();
    assert!(observed == old || observed == new);
    assert_eq!(observed, new);
    assert_eq!(
      graph_compat::revision(&observed),
      graph_compat::revision(new)
    );
  }

  #[test]
  fn edge_diagnostic_keeps_structured_cli_and_http_context() {
    let graph: Value = serde_json::from_str(include_str!(
      "../../../tests/fixtures/edge_admission/valid_0_1_0.json"
    ))
    .unwrap();
    let mut catalog: Value = serde_json::from_str(include_str!(
      "../../../tests/fixtures/edge_admission/catalog.json"
    ))
    .unwrap();
    catalog["types"][0]["outputs"][0]["data_type"] = Value::String("observation_ref".into());
    let error = edge_admission::admit(&graph, &catalog).unwrap_err();
    let diagnostic = edge_diagnostic(*error);
    let value = serde_json::to_value(&diagnostic).unwrap();
    assert_eq!(value["code"], "edge_type_mismatch");
    assert_eq!(value["source"], "eos/hardware/camera/front");
    assert_eq!(value["target"], "eos/interface/gate/preview");
    assert_eq!(value["input"], "frame");
    assert_eq!(value["source_data_type"], "observation_ref");
    assert_eq!(value["target_data_type"], "camera_frame");
    assert_eq!(
      serde_json::from_str::<Value>(&diagnostic.to_string()).unwrap(),
      value
    );
  }

  #[test]
  fn fixture_proposal_is_exact_and_preserves_source_bytes() {
    let source = include_bytes!("../../../tests/fixtures/graph_compat/legacy_1_0_0.json");
    let saved = graph_compat::parse(source).unwrap();
    let proposal = propose(&saved).unwrap();
    let expected: Value = serde_json::from_slice(include_bytes!(
      "../../../tests/fixtures/graph_compat/migration_expected_0_1_0.json"
    ))
    .unwrap();
    assert_eq!(proposal.graph, expected);
    assert_eq!(
      proposal.revision,
      "825d667d31c455844be7f22b565e0ab6b14b44aeb7ce4861a704ffd028081df1"
    );
    assert_eq!(proposal.changes.len(), 5);
    assert_eq!(saved.revision, graph_compat::revision(source));
  }

  #[test]
  fn proposal_preserves_pins_adapter_settings_references_and_edge_metadata() {
    let mut saved = graph_compat::parse(include_bytes!(
      "../../../tests/fixtures/graph_compat/legacy_1_0_0.json"
    ))
    .unwrap();
    let path = "/cores/eos/layers/hardware/groups/camera/nodes/front";
    saved.value["cores"]["eos"]["layers"]["hardware"] = serde_json::json!({
        "groups": {"camera": {"nodes": {"front": {
            "id":"front", "node_type":"eos.hardware.camera@v1.0",
            "package":{"id":"vendor/simulator","version":"0.1.1"},
            "adapter":{"type":"simulator-v1","package":{"id":"vendor/simulator","version":"0.1.1"}},
            "settings":{"device":"front","frame_id":"camera/front"},
            "references":["cores/eos/layers/process/groups/simulation/nodes/simulator"],
            "inputs":{"trigger":[{"node":"cores/eos/layers/dispatcher/groups/control/nodes/command",
                "port":"status","priority":4,"frequency_hz":10.0}]}
        }}}}
    });
    let original = saved.value.pointer(path).unwrap().clone();
    let proposal = propose(&saved).unwrap();
    let converted = proposal.graph.pointer(path).unwrap();
    for field in ["package", "adapter", "settings", "references", "inputs"] {
      assert_eq!(converted[field], original[field], "{field}");
    }
    assert_eq!(converted["node_type"], "eos.hardware.camera@v0.1");
  }

  #[test]
  fn unsafe_unmapped_and_unknown_fields_fail_at_paths() {
    let source = include_bytes!("../../../tests/fixtures/graph_compat/legacy_1_0_0.json");
    let mut saved = graph_compat::parse(source).unwrap();
    saved.value["extra"] = Value::Bool(true);
    let diagnostic = propose(&saved).err().unwrap();
    assert_eq!(
      (diagnostic.path.as_str(), diagnostic.code),
      ("/extra", "unmapped_graph_field")
    );
    saved.value.as_object_mut().unwrap().remove("extra");
    saved.value["cores"]["eos"]["layers"]["dispatcher"]["groups"]["control"]["nodes"]["command"]
      ["node_type"] = Value::String("vendor.foo@v1.0".into());
    let diagnostic = propose(&saved).err().unwrap();
    assert_eq!(diagnostic.code, "unmapped_node_type");
    let unsafe_saved = graph_compat::parse(include_bytes!(
      "../../../tests/fixtures/graph_compat/unsafe_component_1_0_0.json"
    ))
    .unwrap();
    let diagnostic = propose(&unsafe_saved).err().unwrap();
    assert_eq!(diagnostic.code, "unsafe_process_node");
  }

  #[test]
  fn cross_core_input_is_left_to_port_admission() {
    let mut graph = serde_json::json!({
        "cores": {
            "eos": { "layers": { "vision": { "groups": { "camera": {
                "nodes": { "cam": { "inputs": {
                    "frame": { "node": "cores/egia/layers/vision/groups/camera/nodes/source" }
                } } }
            } } } } }
        }
    });
    assert!(cross_core_diagnostic(&graph).is_none());
    graph["cores"]["eos"]["layers"]["vision"]["groups"]["camera"]["nodes"]["cam"]["inputs"]["frame"] = serde_json::json!([
        { "node": "cores/eos/layers/vision/groups/camera/nodes/local" },
        { "node": "cores/egia/layers/vision/groups/camera/nodes/source" }
    ]);
    assert!(cross_core_diagnostic(&graph).is_none());
  }

  #[test]
  fn new_apply_rejects_unowned_core_before_any_write() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let mut graph: Value = serde_json::from_slice(include_bytes!(
      "../../../tests/fixtures/graph_compat/migration_expected_0_1_0.json"
    ))
    .unwrap();
    graph["cores"]["evil"] = serde_json::json!({"layers":{}});
    let error = apply_new(&paths, "default", graph, "").err().unwrap();
    let MigrationError::Diagnostic(diagnostic) = error else {
      panic!("unexpected error type");
    };
    assert_eq!(diagnostic.path, "/cores/evil");
    assert_eq!(diagnostic.code, "unsupported_core");
    assert!(!paths.node_graph_path("default").exists());
    assert!(!paths.generated("default").exists());
  }
}
