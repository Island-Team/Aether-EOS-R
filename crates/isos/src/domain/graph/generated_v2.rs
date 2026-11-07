//! Immutable 0.1 graph generations selected by the exact saved-byte revision.
use crate::{app::config::Paths, domain::graph::graph_compat, domain::packages};
use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::{
  collections::{BTreeMap, BTreeSet},
  ffi::OsString,
  fs,
  io::Write,
  os::unix::{
    ffi::OsStrExt,
    fs::{OpenOptionsExt, PermissionsExt},
  },
  path::{Component, Path, PathBuf},
};

const REQUIRED_EGIA: &[&str] = &[
  "egia.toml",
  "modes.toml",
  "profiles/isos.toml",
  "models.toml",
  "prompts.toml",
  "missions.toml",
  "safety/gate.toml",
  "opt/sandbox.toml",
];
type Files = Vec<(PathBuf, Vec<u8>)>;
type Defaults = (Option<DefaultRoots>, Vec<DefaultEntry>, Files);
type EffectiveFiles = (Files, Option<DefaultRoots>, Vec<DefaultEntry>);

#[derive(Clone, Debug, Serialize)]
pub struct Diagnostic {
  pub code: &'static str,
  pub path: String,
  pub message: &'static str,
  pub stage: &'static str,
}

impl std::fmt::Display for Diagnostic {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(f, "{}: {}", self.code, self.message)
  }
}
impl std::error::Error for Diagnostic {}

fn failure(code: &'static str, stage: &'static str, message: &'static str) -> Diagnostic {
  Diagnostic {
    code,
    stage,
    message,
    path: String::new(),
  }
}
fn missing(stage: &'static str) -> Diagnostic {
  failure(
    "generated_missing_input",
    stage,
    "Restore the selected installation and Apply again",
  )
}
fn unsafe_path(stage: &'static str) -> Diagnostic {
  failure(
    "generated_unsafe_path",
    stage,
    "Use regular files inside the selected installation",
  )
}
fn mismatch() -> Diagnostic {
  failure(
    "generated_manifest_mismatch",
    "snapshot",
    "Apply the saved graph again to restore its generated configuration",
  )
}
pub fn snapshot_mismatch() -> Diagnostic {
  mismatch()
}
fn revision_mismatch() -> Diagnostic {
  failure(
    "generated_revision_mismatch",
    "graph",
    "Reload the saved graph revision before launch",
  )
}
fn input_failure(stage: &'static str) -> Diagnostic {
  if stage == "snapshot" {
    mismatch()
  } else {
    missing(stage)
  }
}
fn render_mismatch() -> Diagnostic {
  failure(
    "generated_render_mismatch",
    "snapshot",
    "Apply the saved graph again to restore owner configuration",
  )
}
pub fn owner_rejected(role: &str) -> Diagnostic {
  Diagnostic {
    code: "generated_owner_rejected",
    stage: "render",
    path: format!("/cores/{role}"),
    message: "Restore the selected owner and correct the graph",
  }
}
pub fn unsafe_render() -> Diagnostic {
  unsafe_path("render")
}
pub fn commit_uncertain() -> Diagnostic {
  failure(
    "generated_commit_uncertain",
    "publish",
    "Reread the saved graph and selected generation",
  )
}
fn digest(bytes: &[u8]) -> String {
  hex::encode(Sha256::digest(bytes))
}
fn valid_hash(value: &str) -> bool {
  value.len() == 64
    && value
      .bytes()
      .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
struct Selection {
  schema_version: u32,
  entries: BTreeMap<String, Entry>,
}
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
enum Entry {
  G03,
  V2 { generation: String },
}
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
struct Marker {
  schema_version: u32,
  revision: String,
}
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
struct FileEntry {
  path: String,
  sha256: String,
}
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
struct PackageEntry {
  id: String,
  version: String,
  receipt_sha256: String,
}
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
struct LaunchInput {
  core: String,
  binary: String,
  path: String,
  sha256: String,
}
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
struct DefaultRoots {
  component: String,
  factory: String,
}
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
struct DefaultEntry {
  source: String,
  path: String,
  sha256: String,
}
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
struct SecretRef {
  node_path: String,
  env_from: BTreeMap<String, String>,
}
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
struct EgiaCodeEntry {
  root: String,
  path: String,
  sha256: String,
}
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
struct EgiaChain {
  prefix: String,
  mode: String,
  interpreter: String,
  interpreter_target: String,
  interpreter_sha256: String,
  binding: BTreeMap<String, String>,
  code: Vec<EgiaCodeEntry>,
}
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
struct Manifest {
  schema_version: u32,
  revision: String,
  generation: String,
  files: Vec<FileEntry>,
  launch_inputs: Vec<LaunchInput>,
  packages: Vec<PackageEntry>,
  default_roots: Option<DefaultRoots>,
  defaults: Vec<DefaultEntry>,
  secret_refs: Vec<SecretRef>,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  egia_chain: Option<EgiaChain>,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  vision_selection_sha256: Option<String>,
}

fn checked_relative(path: &Path) -> std::result::Result<String, Diagnostic> {
  if !path
    .components()
    .all(|part| matches!(part, Component::Normal(_)))
  {
    return Err(unsafe_path("snapshot"));
  }
  let value = path.to_str().ok_or_else(|| unsafe_path("snapshot"))?;
  if value.is_empty() || value.contains('\\') || value.len() > 256 {
    return Err(unsafe_path("snapshot"));
  }
  Ok(value.to_owned())
}

fn private_dir(path: &Path) -> Result<(), Diagnostic> {
  for ancestor in path.ancestors() {
    if let Ok(metadata) = fs::symlink_metadata(ancestor)
      && metadata.file_type().is_symlink()
    {
      return Err(unsafe_path("publish"));
    }
  }
  fs::create_dir_all(path).map_err(|_| unsafe_path("publish"))?;
  if !fs::symlink_metadata(path)
    .map_err(|_| unsafe_path("publish"))?
    .file_type()
    .is_dir()
  {
    return Err(unsafe_path("publish"));
  }
  fs::set_permissions(path, fs::Permissions::from_mode(0o700))
    .map_err(|_| unsafe_path("publish"))?;
  Ok(())
}

fn seal_dirs(root: &Path) -> Result<(), Diagnostic> {
  let mut pending = vec![root.to_path_buf()];
  while let Some(dir) = pending.pop() {
    private_dir(&dir)?;
    for child in fs::read_dir(&dir).map_err(|_| unsafe_path("publish"))? {
      let path = child.map_err(|_| unsafe_path("publish"))?.path();
      if fs::symlink_metadata(&path)
        .map_err(|_| unsafe_path("publish"))?
        .file_type()
        .is_dir()
      {
        pending.push(path);
      }
    }
  }
  Ok(())
}

fn write_private(path: &Path, bytes: &[u8]) -> Result<(), Diagnostic> {
  let parent = path.parent().ok_or_else(|| unsafe_path("publish"))?;
  private_dir(parent)?;
  let mut file = fs::OpenOptions::new()
    .write(true)
    .create_new(true)
    .mode(0o600)
    .open(path)
    .map_err(|_| unsafe_path("publish"))?;
  file
    .write_all(bytes)
    .and_then(|_| file.sync_all())
    .map_err(|_| unsafe_path("publish"))?;
  Ok(())
}

fn read_selection(generated: &Path) -> Result<Option<Selection>, Diagnostic> {
  let path = generated.join("selection.json");
  check_parent_dirs(&path, "snapshot")?;
  match fs::symlink_metadata(&path) {
    Ok(metadata) if !metadata.file_type().is_file() => return Err(unsafe_path("snapshot")),
    Ok(_) => {}
    Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
    Err(_) => return Err(mismatch()),
  }
  let bytes = match fs::read(path) {
    Ok(bytes) => bytes,
    Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
    Err(_) => return Err(mismatch()),
  };
  let selection: Selection = serde_json::from_slice(&bytes).map_err(|_| mismatch())?;
  if selection.schema_version != 2 || selection.entries.keys().any(|key| !valid_hash(key)) {
    return Err(mismatch());
  }
  for entry in selection.entries.values() {
    if let Entry::V2 { generation } = entry
      && !valid_hash(generation)
    {
      return Err(mismatch());
    }
  }
  Ok(Some(selection))
}

fn marker_path(generated: &Path, revision: &str) -> PathBuf {
  generated
    .join("v2/required")
    .join(format!("{revision}.json"))
}
fn leaf(generated: &Path, revision: &str, generation: &str) -> PathBuf {
  generated.join("v2").join(revision).join(generation)
}

fn inspect_regular(path: &Path, stage: &'static str) -> Result<Vec<u8>, Diagnostic> {
  check_parent_dirs(path, stage)?;
  let metadata = fs::symlink_metadata(path).map_err(|_| missing(stage))?;
  if !metadata.file_type().is_file() {
    return Err(unsafe_path(stage));
  }
  fs::read(path).map_err(|_| missing(stage))
}

fn check_parent_dirs(path: &Path, stage: &'static str) -> Result<(), Diagnostic> {
  for parent in path.ancestors().skip(1) {
    match fs::symlink_metadata(parent) {
      Ok(metadata) if !metadata.file_type().is_dir() => return Err(unsafe_path(stage)),
      Ok(_) => {}
      Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
      Err(_) => return Err(unsafe_path(stage)),
    }
  }
  Ok(())
}

fn walk_tree(root: &Path, source: &str) -> Result<(Vec<DefaultEntry>, Files), Diagnostic> {
  if !root.is_absolute() {
    return Err(unsafe_path("defaults"));
  }
  for ancestor in root.ancestors() {
    if let Ok(metadata) = fs::symlink_metadata(ancestor)
      && metadata.file_type().is_symlink()
    {
      return Err(unsafe_path("defaults"));
    }
  }
  if !fs::symlink_metadata(root)
    .map_err(|_| missing("defaults"))?
    .file_type()
    .is_dir()
  {
    return Err(unsafe_path("defaults"));
  }
  let mut pending = vec![root.to_path_buf()];
  let mut entries = Vec::new();
  let mut files = Vec::new();
  while let Some(dir) = pending.pop() {
    let children = fs::read_dir(&dir).map_err(|_| missing("defaults"))?;
    for child in children {
      let child = child.map_err(|_| missing("defaults"))?.path();
      let metadata = fs::symlink_metadata(&child).map_err(|_| missing("defaults"))?;
      if metadata.file_type().is_symlink() {
        return Err(unsafe_path("defaults"));
      }
      if metadata.file_type().is_dir() {
        pending.push(child);
        continue;
      }
      if !metadata.file_type().is_file() {
        return Err(unsafe_path("defaults"));
      }
      let relative = child
        .strip_prefix(root)
        .map_err(|_| unsafe_path("defaults"))?;
      let path = checked_relative(relative).map_err(|_| unsafe_path("defaults"))?;
      let bytes = inspect_regular(&child, "defaults")?;
      if bytes.len() > 1024 * 1024 || entries.len() >= 128 {
        return Err(unsafe_path("defaults"));
      }
      if child
        .extension()
        .is_some_and(|extension| extension == "toml")
        && std::str::from_utf8(&bytes)
          .ok()
          .and_then(|source| toml::from_str::<toml::Value>(source).ok())
          .is_none()
      {
        return Err(failure(
          "generated_invalid_default",
          "defaults",
          "Restore valid EGIA defaults and Apply again",
        ));
      }
      entries.push(DefaultEntry {
        source: source.to_owned(),
        path,
        sha256: digest(&bytes),
      });
      files.push((relative.to_path_buf(), bytes));
    }
  }
  entries.sort_by(|a, b| (&a.source, &a.path).cmp(&(&b.source, &b.path)));
  files.sort_by(|a, b| a.0.cmp(&b.0));
  Ok((entries, files))
}

fn defaults(graph: &Value) -> Result<Defaults, Diagnostic> {
  let enabled = crate::domain::graph::graph_compat::has_unified_ei_layers(graph)
    || graph
      .pointer("/cores/isos/layers/process/groups/component/nodes/egia")
      .is_some_and(|node| node["enabled"] != false);
  if !enabled {
    return Ok((None, Vec::new(), Vec::new()));
  }
  let component = std::env::var_os("ISOS_EI_DEFAULTS_DIR")
    .or_else(|| std::env::var_os("ISOS_EGIA_DEFAULTS_DIR"))
    .map(PathBuf::from)
    .ok_or_else(|| missing("defaults"))?;
  let factory = std::env::var_os("ISOS_EI_FACTORY_DEFAULTS_DIR")
    .or_else(|| std::env::var_os("ISOS_EGIA_FACTORY_DEFAULTS_DIR"))
    .map(PathBuf::from)
    .ok_or_else(|| missing("defaults"))?;
  let bin = std::env::var_os("ISOS_EI_BIN_DIR")
    .or_else(|| std::env::var_os("ISOS_EGIA_BIN_DIR"))
    .map(PathBuf::from)
    .ok_or_else(|| missing("binary"))?;
  let prefix = std::env::var_os("ISLAND_PREFIX")
    .map(PathBuf::from)
    .or_else(|| bin.parent().map(Path::to_path_buf))
    .ok_or_else(|| missing("binary"))?;
  validate_factory_root(&factory, &prefix)?;
  let (mut component_entries, component_files) = walk_tree(&component, "component")?;
  require_component_defaults(&component_entries)?;
  let (factory_entries, _) = walk_tree(&factory, "factory")?;
  component_entries.extend(factory_entries);
  component_entries.sort_by(|a, b| (&a.source, &a.path).cmp(&(&b.source, &b.path)));
  Ok((
    Some(DefaultRoots {
      component: component.to_string_lossy().into_owned(),
      factory: factory.to_string_lossy().into_owned(),
    }),
    component_entries,
    component_files,
  ))
}

fn require_component_defaults(entries: &[DefaultEntry]) -> Result<(), Diagnostic> {
  for required in REQUIRED_EGIA
    .iter()
    .copied()
    .filter(|path| *path != "profiles/isos.toml")
  {
    if !entries.iter().any(|entry| entry.path == required) {
      return Err(missing("defaults"));
    }
  }
  Ok(())
}

fn validate_factory_root(factory: &Path, prefix: &Path) -> Result<(), Diagnostic> {
  if factory != prefix.join("share/defaults/egia") {
    return Err(failure(
      "generated_invalid_default",
      "defaults",
      "Select the factory defaults used by the EGIA launcher",
    ));
  }
  Ok(())
}
pub fn check_defaults(graph: &Value) -> Result<(), Diagnostic> {
  defaults(graph).map(|_| ())
}

fn egia_enabled(graph: &Value) -> bool {
  crate::domain::graph::graph_compat::has_unified_ei_layers(graph)
    || graph
      .pointer("/cores/isos/layers/process/groups/component/nodes/egia")
      .is_some_and(|node| node["enabled"] != false)
}

fn resolve_interpreter(prefix: &Path, source_mode: bool) -> Result<PathBuf, Diagnostic> {
  let selected = std::env::var_os("EI_PYTHON")
    .or_else(|| std::env::var_os("EGIA_PYTHON"))
    .or_else(|| std::env::var_os("ISLAND_PYTHON"))
    .map(PathBuf::from)
    .unwrap_or_else(|| {
      if source_mode {
        prefix.join(".venv/bin/python")
      } else {
        PathBuf::from("python3.12")
      }
    });
  let absolute = if selected.is_absolute() {
    selected
  } else if selected.components().count() == 1 {
    std::env::split_paths(&std::env::var_os("PATH").unwrap_or_default())
      .map(|dir| dir.join(&selected))
      .find(|candidate| candidate.is_file())
      .ok_or_else(|| missing("binary"))?
  } else {
    return Err(unsafe_path("binary"));
  };
  let meta = fs::metadata(&absolute).map_err(|_| missing("binary"))?;
  if !meta.is_file() || meta.permissions().mode() & 0o111 == 0 {
    return Err(unsafe_path("binary"));
  }
  Ok(absolute)
}

fn code_entries(root: &Path) -> Result<Vec<EgiaCodeEntry>, Diagnostic> {
  if !root.is_absolute() || !root.is_dir() {
    return Err(missing("binary"));
  }
  for ancestor in root.ancestors() {
    if fs::symlink_metadata(ancestor).is_ok_and(|meta| meta.file_type().is_symlink()) {
      return Err(unsafe_path("binary"));
    }
  }
  let mut pending = vec![root.to_path_buf()];
  let mut entries = Vec::new();
  let mut total = 0usize;
  while let Some(dir) = pending.pop() {
    for child in fs::read_dir(&dir).map_err(|_| missing("binary"))? {
      let child = child.map_err(|_| missing("binary"))?.path();
      let meta = fs::symlink_metadata(&child).map_err(|_| missing("binary"))?;
      if meta.file_type().is_symlink() {
        return Err(unsafe_path("binary"));
      }
      if meta.file_type().is_dir() {
        if child.file_name().is_some_and(|name| name == "__pycache__") {
          continue;
        }
        pending.push(child);
        continue;
      }
      if !meta.file_type().is_file() {
        return Err(unsafe_path("binary"));
      }
      if child.extension().is_some_and(|ext| ext == "pyc") {
        continue;
      }
      let bytes = fs::read(&child).map_err(|_| missing("binary"))?;
      total += bytes.len();
      if entries.len() >= 4096 || total > 64 * 1024 * 1024 {
        return Err(unsafe_path("binary"));
      }
      let path = checked_relative(
        child
          .strip_prefix(root)
          .map_err(|_| unsafe_path("binary"))?,
      )?;
      entries.push(EgiaCodeEntry {
        root: root.to_string_lossy().into_owned(),
        path,
        sha256: digest(&bytes),
      });
    }
  }
  entries.sort_by(|a, b| (&a.root, &a.path).cmp(&(&b.root, &b.path)));
  if entries.is_empty() {
    return Err(missing("binary"));
  }
  Ok(entries)
}

fn capture_egia_chain(graph: &Value) -> Result<Option<EgiaChain>, Diagnostic> {
  if !egia_enabled(graph) {
    return Ok(None);
  }
  let bin = std::env::var_os("ISOS_EI_BIN_DIR")
    .or_else(|| std::env::var_os("ISOS_EGIA_BIN_DIR"))
    .map(PathBuf::from)
    .ok_or_else(|| missing("binary"))?;
  if !bin.is_absolute() {
    return Err(unsafe_path("binary"));
  }
  let prefix = std::env::var_os("ISLAND_PREFIX")
    .map(PathBuf::from)
    .or_else(|| bin.parent().map(Path::to_path_buf))
    .ok_or_else(|| missing("binary"))?;
  if !prefix.is_absolute() {
    return Err(unsafe_path("binary"));
  }
  if fs::symlink_metadata(prefix.join("pyproject.toml"))
    .is_ok_and(|meta| meta.file_type().is_symlink())
  {
    return Err(unsafe_path("binary"));
  }
  let source_mode = prefix.join("pyproject.toml").is_file() && prefix.join("src/egia").is_dir();
  let portable_mode = prefix.join("share/contracts").is_dir() && prefix.join("libexec").is_dir();
  let (mode, roots) = if source_mode {
    let mut roots = vec![prefix.join("src/egia")];
    if prefix.join("libexec").is_dir() {
      roots.push(prefix.join("libexec"));
    }
    ("source", roots)
  } else {
    let mut roots = vec![prefix.join("lib/island/egia")];
    if !portable_mode && prefix.join("libexec/island").is_dir() {
      roots.push(prefix.join("libexec/island"));
    } else if prefix.join("libexec").is_dir() {
      roots.push(prefix.join("libexec"));
    }
    ("installed", roots)
  };
  let interpreter = resolve_interpreter(&prefix, source_mode)?;
  let mut code = Vec::new();
  for root in roots {
    code.extend(code_entries(&root)?);
  }
  code.sort_by(|a, b| (&a.root, &a.path).cmp(&(&b.root, &b.path)));
  let mut binding = BTreeMap::new();
  binding.insert(
    "EGIA_PYTHON".into(),
    interpreter.to_string_lossy().into_owned(),
  );
  binding.insert(
    "EI_PYTHON".into(),
    interpreter.to_string_lossy().into_owned(),
  );
  binding.insert(
    "ISLAND_PREFIX".into(),
    prefix.to_string_lossy().into_owned(),
  );
  binding.insert("PYTHONPATH".into(), String::new());
  binding.insert("PYTHONDONTWRITEBYTECODE".into(), "1".into());
  if !source_mode {
    binding.insert(
      "ISLAND_LIB".into(),
      prefix.join("lib/island").to_string_lossy().into_owned(),
    );
    binding.insert(
      "ISLAND_LIBEXEC".into(),
      prefix
        .join(if portable_mode {
          "libexec"
        } else {
          "libexec/island"
        })
        .to_string_lossy()
        .into_owned(),
    );
  }
  Ok(Some(EgiaChain {
    prefix: prefix.to_string_lossy().into_owned(),
    mode: mode.into(),
    interpreter: interpreter.to_string_lossy().into_owned(),
    interpreter_target: interpreter
      .canonicalize()
      .map_err(|_| missing("binary"))?
      .to_string_lossy()
      .into_owned(),
    interpreter_sha256: digest(&fs::read(&interpreter).map_err(|_| missing("binary"))?),
    binding,
    code,
  }))
}

/// Nonsecret, pinned environment handed to both the capability probe and child.
pub fn egia_launch_env(graph: &Value) -> Result<BTreeMap<String, String>, Diagnostic> {
  Ok(capture_egia_chain(graph)?.map_or_else(BTreeMap::new, |chain| chain.binding))
}

pub fn egia_chain_digest(graph: &Value) -> Result<String, Diagnostic> {
  let chain = capture_egia_chain(graph)?.ok_or_else(|| missing("binary"))?;
  let bin = std::env::var_os("ISOS_EI_BIN_DIR")
    .or_else(|| std::env::var_os("ISOS_EGIA_BIN_DIR"))
    .map(PathBuf::from)
    .ok_or_else(|| missing("binary"))?;
  let mut launchers = BTreeMap::new();
  for name in ["egia", "egia-config"] {
    launchers.insert(name, digest(&inspect_regular(&bin.join(name), "binary")?));
  }
  Ok(digest(
    &serde_json::to_vec(&serde_json::json!({"chain":chain,"launchers":launchers}))
      .map_err(|_| mismatch())?,
  ))
}

fn package_inventory(paths: &Paths, graph: &Value) -> Result<Vec<PackageEntry>, Diagnostic> {
  let handles = packages::verify_graph_packages(paths, graph).map_err(|_| missing("package"))?;
  let mut result = Vec::new();
  for package in handles {
    let receipt = inspect_regular(&package.store.join("receipt.json"), "package")?;
    result.push(PackageEntry {
      id: package.release.id,
      version: package.release.version,
      receipt_sha256: digest(&receipt),
    });
  }
  result.sort_by(|a, b| (&a.id, &a.version).cmp(&(&b.id, &b.version)));
  Ok(result)
}

fn secret_refs(graph: &Value) -> Result<Vec<SecretRef>, Diagnostic> {
  let mut result = Vec::new();
  let Some(nodes) = graph
    .pointer("/cores/isos/layers/process/groups/component/nodes")
    .and_then(Value::as_object)
  else {
    return Ok(result);
  };
  for (id, node) in nodes {
    let Some(map) = node.pointer("/settings/env_from") else {
      continue;
    };
    let map = map.as_object().ok_or_else(|| {
      failure(
        "generated_invalid_default",
        "graph",
        "Use a valid child environment reference map",
      )
    })?;
    if map.len() > 32 {
      return Err(failure(
        "generated_invalid_default",
        "graph",
        "Use at most 32 child environment references",
      ));
    }
    let mut refs = BTreeMap::new();
    for (target, source) in map {
      let source = source.as_str().ok_or_else(|| {
        failure(
          "generated_invalid_default",
          "graph",
          "Use valid environment reference names",
        )
      })?;
      if !valid_env(target) || !valid_env(source) || node["settings"]["env"].get(target).is_some() {
        return Err(failure(
          "generated_invalid_default",
          "graph",
          "Use distinct valid environment reference names",
        ));
      }
      refs.insert(target.clone(), source.to_owned());
    }
    result.push(SecretRef {
      node_path: format!(
        "/cores/isos/layers/process/groups/component/nodes/{}",
        escape(id)
      ),
      env_from: refs,
    });
  }
  result.sort_by(|a, b| a.node_path.cmp(&b.node_path));
  Ok(result)
}
pub fn valid_env(value: &str) -> bool {
  let mut bytes = value.bytes();
  bytes
    .next()
    .is_some_and(|first| first.is_ascii_alphabetic() || first == b'_')
    && bytes.all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
}
pub fn escape(value: &str) -> String {
  value.replace('~', "~0").replace('/', "~1")
}

pub type SecretPlan = BTreeMap<String, BTreeMap<String, OsString>>;

pub fn secret_plan(
  specs: &[crate::runtime::process::ProcessSpec],
) -> Result<SecretPlan, Diagnostic> {
  let mut plan = SecretPlan::new();
  for spec in specs {
    let mut child = BTreeMap::new();
    for (target, source) in &spec.settings.env_from {
      let value = std::env::var_os(source)
        .filter(|value| !value.is_empty() && !value.as_bytes().contains(&0))
        .ok_or_else(|| Diagnostic {
          code: "generated_missing_secret",
          stage: "spawn",
          path: format!(
            "/cores/isos/layers/process/groups/component/nodes/{}/settings/env_from/{}",
            escape(&spec.id),
            escape(target)
          ),
          message: "Set the required child environment value and retry",
        })?;
      child.insert(target.clone(), value);
    }
    if !child.is_empty() {
      plan.insert(spec.id.clone(), child);
    }
  }
  Ok(plan)
}

fn effective_files(
  graph: &Value,
  owner_files: &[(PathBuf, Vec<u8>)],
) -> Result<EffectiveFiles, Diagnostic> {
  let (roots, defaults, component) = defaults(graph)?;
  let ei_dir = if graph["version"] == "0.2.0" {
    "ei"
  } else {
    "egia"
  };
  let retired_dir = if ei_dir == "ei" { "egia" } else { "ei" };
  if owner_files
    .iter()
    .any(|(path, _)| path.starts_with(retired_dir))
  {
    return Err(unsafe_path("render"));
  }
  if roots.is_some() {
    require_egia_owner_overlays(owner_files, ei_dir)?;
  } else if owner_files.iter().any(|(path, _)| path.starts_with(ei_dir)) {
    return Err(unsafe_path("render"));
  }
  let mut files = BTreeMap::new();
  let mut owner_paths = BTreeSet::new();
  for (relative, bytes) in component {
    files.insert(PathBuf::from(ei_dir).join(relative), bytes);
  }
  for (relative, bytes) in owner_files {
    let path = if relative == Path::new("launch-inputs.json") {
      PathBuf::from("core-setup/launch-inputs.json")
    } else {
      relative.clone()
    };
    checked_relative(&path).map_err(|_| unsafe_path("render"))?;
    if !owner_paths.insert(path.clone()) {
      return Err(unsafe_path("render"));
    }
    if files.insert(path, bytes.clone()).is_some() && !is_egia_overlay(relative, ei_dir) {
      return Err(unsafe_path("render"));
    }
  }
  if roots.is_some() {
    for required in REQUIRED_EGIA {
      if !files.contains_key(&PathBuf::from(ei_dir).join(required)) {
        return Err(missing("render"));
      }
    }
  }
  for required in ["eos/graph.json", "core-setup/launch-inputs.json"] {
    if !files.contains_key(Path::new(required)) {
      return Err(missing("render"));
    }
  }
  Ok((files.into_iter().collect(), roots, defaults))
}

fn is_egia_overlay(path: &Path, directory: &str) -> bool {
  [
    "egia.toml",
    "modes.toml",
    "profiles/isos.toml",
    "stages.json",
  ]
  .iter()
  .any(|allowed| path == Path::new(directory).join(allowed))
}

fn require_egia_owner_overlays(
  owner_files: &[(PathBuf, Vec<u8>)],
  directory: &str,
) -> Result<(), Diagnostic> {
  if owner_files
    .iter()
    .any(|(path, _)| path.starts_with(directory) && !is_egia_overlay(path, directory))
  {
    return Err(unsafe_path("render"));
  }
  for required in ["egia.toml", "modes.toml", "profiles/isos.toml"] {
    if !owner_files
      .iter()
      .any(|(path, _)| path == &Path::new(directory).join(required))
    {
      return Err(missing("render"));
    }
  }
  Ok(())
}

fn inventory(
  paths: &Paths,
  graph: &Value,
  revision: &str,
  owner_files: &[(PathBuf, Vec<u8>)],
) -> Result<(Manifest, Files), Diagnostic> {
  let (mut files, roots, defaults) = effective_files(graph, owner_files)?;
  if let Some(selection) = crate::runtime::process::skill_selection::render(paths, graph, revision)
    .map_err(|_| unsafe_path("render"))?
  {
    if files
      .iter()
      .any(|(path, _)| path == Path::new(crate::runtime::process::skill_selection::RELATIVE_FILE))
    {
      return Err(unsafe_path("render"));
    }
    files.push((
      PathBuf::from(crate::runtime::process::skill_selection::RELATIVE_FILE),
      selection,
    ));
    files.sort_by(|a, b| a.0.cmp(&b.0));
  }
  let mut entries = Vec::new();
  for (path, bytes) in &files {
    entries.push(FileEntry {
      path: checked_relative(path)?,
      sha256: digest(bytes),
    });
  }
  let launch_bytes = files
    .iter()
    .find(|(path, _)| path == Path::new("core-setup/launch-inputs.json"))
    .ok_or_else(|| missing("binary"))?
    .1
    .as_slice();
  let mut launch_inputs: Vec<LaunchInput> =
    serde_json::from_slice(launch_bytes).map_err(|_| missing("binary"))?;
  launch_inputs.sort_by(|a, b| (&a.core, &a.binary).cmp(&(&b.core, &b.binary)));
  check_launch_inputs(paths, graph, &launch_inputs, "binary")?;
  let mut manifest = Manifest {
    schema_version: 2,
    revision: revision.to_owned(),
    generation: String::new(),
    files: entries,
    launch_inputs,
    packages: package_inventory(paths, graph)?,
    default_roots: roots,
    defaults,
    secret_refs: secret_refs(graph)?,
    egia_chain: capture_egia_chain(graph)?,
    vision_selection_sha256: if files
      .iter()
      .any(|(path, _)| path == Path::new(crate::runtime::process::vision_binding::TEMPLATE_FILE))
    {
      let (gate, fingerprint) =
        crate::runtime::process::vision_binding::selected_gate_for_graph(graph)
          .map_err(|_| mismatch())?
          .ok_or_else(mismatch)?;
      let template_bytes = files
        .iter()
        .find(|(path, _)| path == Path::new(crate::runtime::process::vision_binding::TEMPLATE_FILE))
        .ok_or_else(mismatch)?
        .1
        .as_slice();
      let template: crate::runtime::process::vision_binding::Template =
        serde_json::from_slice(template_bytes).map_err(|_| mismatch())?;
      if template.gate_path != gate || template.graph_revision != revision {
        return Err(mismatch());
      }
      Some(fingerprint)
    } else {
      None
    },
  };
  manifest.generation = generation(&manifest).map_err(|_| mismatch())?;
  Ok((manifest, files))
}

fn check_launch_inputs(
  paths: &Paths,
  graph: &Value,
  inputs: &[LaunchInput],
  stage: &'static str,
) -> Result<(), Diagnostic> {
  let mut expected = BTreeSet::new();
  if graph["cores"].get("eos").is_some()
    || (graph["version"] == "0.2.0"
      && graph
        .pointer("/cores/isos/layers")
        .and_then(Value::as_object)
        .is_some_and(|layers| layers.keys().any(|name| name.starts_with("eos_"))))
  {
    for binary in [
      "aether-profile",
      "aether-runtime",
      "aether-http-bridge",
      "aether-config-server",
    ] {
      expected.insert(("eos".to_owned(), binary.to_owned()));
    }
  }
  if crate::domain::graph::graph_compat::has_unified_ei_layers(graph)
    || graph
      .pointer("/cores/isos/layers/process/groups/component/nodes/egia")
      .is_some_and(|node| node["enabled"] != false)
  {
    for binary in ["egia-config", "egia"] {
      expected.insert(("egia".to_owned(), binary.to_owned()));
    }
  }
  let present: BTreeSet<_> = inputs
    .iter()
    .map(|item| (item.core.clone(), item.binary.clone()))
    .collect();
  if present != expected || inputs.len() != expected.len() {
    return Err(input_failure(stage));
  }
  for input in inputs {
    let selected = match input.core.as_str() {
      "eos" => crate::runtime::process::core_bins::resolve(&input.binary)
        .map_err(|_| input_failure(stage))?,
      "egia" => crate::runtime::owners::core_setup::adapter_path(paths, "egia")
        .map_err(|_| input_failure(stage))?
        .with_file_name(&input.binary),
      _ => return Err(input_failure(stage)),
    };
    let path = Path::new(&input.path);
    if !path.is_absolute() || path != selected || !valid_hash(&input.sha256) {
      return Err(input_failure(stage));
    }
    if digest(&inspect_regular(path, stage).map_err(|_| input_failure(stage))?) != input.sha256 {
      return Err(input_failure(stage));
    }
  }
  Ok(())
}

fn generation(manifest: &Manifest) -> Result<String> {
  let mut value = serde_json::to_value(manifest)?;
  value
    .as_object_mut()
    .context("manifest object")?
    .remove("generation");
  Ok(digest(&serde_json::to_vec(&value)?))
}

#[cfg(test)]
static FAIL_PARENT_SYNC_FOR: std::sync::OnceLock<std::sync::Mutex<Option<PathBuf>>> =
  std::sync::OnceLock::new();

#[cfg(test)]
fn fail_parent_sync_once(path: &Path) {
  *FAIL_PARENT_SYNC_FOR
    .get_or_init(|| std::sync::Mutex::new(None))
    .lock()
    .unwrap() = Some(path.to_path_buf());
}

#[cfg(test)]
fn injected_parent_sync_failure(path: &Path) -> bool {
  let mut pending = FAIL_PARENT_SYNC_FOR
    .get_or_init(|| std::sync::Mutex::new(None))
    .lock()
    .unwrap();
  if pending.as_deref() == Some(path) {
    *pending = None;
    true
  } else {
    false
  }
}

/// Debug-build fault injection for the P04 HTTP publication replay only.
#[cfg(debug_assertions)]
pub(crate) fn test_fail_parent_sync(path: &Path, kind: &str) -> bool {
  use std::sync::atomic::{AtomicBool, Ordering};
  static FIRED: AtomicBool = AtomicBool::new(false);
  if std::env::var("ISOS_P04_TEST_FAIL_SYNC").ok().as_deref() != Some(kind) {
    return false;
  }
  let Some(home) = std::env::var_os("ISOS_HOME").map(PathBuf::from) else {
    return false;
  };
  let Some(deployment) = std::env::var_os("ISOS_P04_TEST_FAIL_SYNC_DEPLOYMENT") else {
    return false;
  };
  let deployment = deployment.to_string_lossy();
  if crate::runtime::process::validate_deployment_name(&deployment).is_err() {
    return false;
  }
  let required_parent = home.join(format!("data/generated/{deployment}/v2/required"));
  let scoped = match kind {
    "selection" => path == home.join(format!("data/generated/{deployment}/selection.json")),
    "marker" => {
      path.parent() == Some(required_parent.as_path())
        && path
          .file_stem()
          .and_then(|stem| stem.to_str())
          .is_some_and(valid_hash)
    }
    "graph" => path == home.join(format!("config/deployments/{deployment}.nodes.json")),
    _ => false,
  };
  scoped && !FIRED.swap(true, Ordering::SeqCst)
}

fn publish_file_replace(path: &Path, bytes: &[u8]) -> Result<(), Diagnostic> {
  let parent = path.parent().ok_or_else(|| unsafe_path("publish"))?;
  private_dir(parent)?;
  let temporary = parent.join(format!(
    ".{}.{}.tmp",
    path.file_name().unwrap().to_string_lossy(),
    std::process::id()
  ));
  let _ = fs::remove_file(&temporary);
  write_private(&temporary, bytes)?;
  fs::rename(&temporary, path).map_err(|_| unsafe_path("publish"))?;
  #[cfg(test)]
  if injected_parent_sync_failure(path) {
    return Err(commit_uncertain());
  }
  #[cfg(debug_assertions)]
  if test_fail_parent_sync(
    path,
    if path
      .file_name()
      .is_some_and(|name| name == "selection.json")
    {
      "selection"
    } else {
      "marker"
    },
  ) {
    return Err(commit_uncertain());
  }
  fs::File::open(parent)
    .and_then(|file| file.sync_all())
    .map_err(|_| {
      failure(
        "generated_commit_uncertain",
        "publish",
        "Reread the saved graph and selected generation",
      )
    })?;
  Ok(())
}

pub fn publish(
  paths: &Paths,
  name: &str,
  graph: &Value,
  revision: &str,
  owner_files: &[(PathBuf, Vec<u8>)],
) -> Result<PathBuf, Diagnostic> {
  if !valid_hash(revision) {
    return Err(mismatch());
  }
  let generated = paths.generated(name);
  private_dir(&generated)?;
  private_dir(&generated.join("v2"))?;
  let (manifest, files) = inventory(paths, graph, revision, owner_files)?;
  let root = leaf(&generated, revision, &manifest.generation);
  if root.exists() {
    verify_leaf(&root, &manifest)?;
  } else {
    let parent = root.parent().ok_or_else(|| unsafe_path("publish"))?;
    private_dir(parent)?;
    let staging = tempfile::Builder::new()
      .prefix("generation-")
      .tempdir_in(parent)
      .map_err(|_| unsafe_path("publish"))?;
    for (relative, bytes) in &files {
      write_private(&staging.path().join(relative), bytes)?;
    }
    write_private(
      &staging.path().join("manifest.json"),
      &serde_json::to_vec_pretty(&manifest).map_err(|_| mismatch())?,
    )?;
    seal_dirs(staging.path())?;
    fs::File::open(staging.path())
      .and_then(|file| file.sync_all())
      .map_err(|_| unsafe_path("publish"))?;
    fs::rename(staging.path(), &root).map_err(|_| unsafe_path("publish"))?;
    fs::File::open(parent)
      .and_then(|file| file.sync_all())
      .map_err(|_| {
        failure(
          "generated_commit_uncertain",
          "publish",
          "Reread the saved graph and selected generation",
        )
      })?;
  }
  crate::runtime::process::skill_selection::compare_selected(paths, graph, revision, &root)?;
  let mut selection = read_selection(&generated)?.unwrap_or(Selection {
    schema_version: 2,
    entries: BTreeMap::new(),
  });
  let graph_path = paths.node_graph_path(name);
  if let Ok(saved) = graph_compat::read(&graph_path)
    && saved.value["version"] == "0.1.0"
    && !selection.entries.contains_key(&saved.revision)
  {
    if marker_path(&generated, &saved.revision).exists() {
      if saved.revision != revision {
        return Err(mismatch());
      }
    } else if generated.join(&saved.revision).is_dir() {
      selection.entries.insert(saved.revision, Entry::G03);
    }
  }
  selection.entries.insert(
    revision.to_owned(),
    Entry::V2 {
      generation: manifest.generation.clone(),
    },
  );
  publish_file_replace(
    &generated.join("selection.json"),
    &serde_json::to_vec_pretty(&selection).map_err(|_| mismatch())?,
  )?;
  let marker = marker_path(&generated, revision);
  if !marker_present(&marker)? {
    private_dir(&generated.join("v2/required"))?;
    publish_file_replace(
      &marker,
      &serde_json::to_vec_pretty(&Marker {
        schema_version: 1,
        revision: revision.to_owned(),
      })
      .map_err(|_| mismatch())?,
    )?;
  } else {
    verify_marker(&marker, revision)?;
  }
  Ok(root)
}

fn verify_marker(path: &Path, revision: &str) -> Result<(), Diagnostic> {
  let marker: Marker =
    serde_json::from_slice(&inspect_regular(path, "snapshot")?).map_err(|_| mismatch())?;
  if marker.schema_version != 1 || marker.revision != revision {
    return Err(mismatch());
  }
  Ok(())
}

fn marker_present(path: &Path) -> Result<bool, Diagnostic> {
  check_parent_dirs(path, "snapshot")?;
  match fs::symlink_metadata(path) {
    Ok(metadata) if metadata.file_type().is_file() => Ok(true),
    Ok(_) => Err(unsafe_path("snapshot")),
    Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(false),
    Err(_) => Err(mismatch()),
  }
}

fn verify_leaf(root: &Path, expected: &Manifest) -> Result<(), Diagnostic> {
  check_parent_dirs(root, "snapshot")?;
  let bytes = inspect_regular(&root.join("manifest.json"), "snapshot")?;
  let manifest: Manifest = serde_json::from_slice(&bytes).map_err(|_| mismatch())?;
  if manifest != *expected || manifest.schema_version != 2 {
    return Err(mismatch());
  }
  let mut actual = BTreeSet::new();
  let mut pending = vec![root.to_path_buf()];
  while let Some(dir) = pending.pop() {
    let metadata = fs::symlink_metadata(&dir).map_err(|_| mismatch())?;
    if !metadata.file_type().is_dir() || metadata.permissions().mode() & 0o777 != 0o700 {
      return Err(unsafe_path("snapshot"));
    }
    for child in fs::read_dir(&dir).map_err(|_| mismatch())? {
      let path = child.map_err(|_| mismatch())?.path();
      let metadata = fs::symlink_metadata(&path).map_err(|_| mismatch())?;
      if metadata.file_type().is_dir() {
        pending.push(path);
        continue;
      }
      if !metadata.file_type().is_file() || metadata.permissions().mode() & 0o777 != 0o600 {
        return Err(mismatch());
      }
      let relative = path.strip_prefix(root).map_err(|_| mismatch())?;
      actual.insert(checked_relative(relative).map_err(|_| mismatch())?);
    }
  }
  let listed: BTreeSet<_> = manifest
    .files
    .iter()
    .map(|entry| entry.path.clone())
    .collect();
  if actual.len() != listed.len() + 1
    || !actual.contains("manifest.json")
    || !listed.is_subset(&actual)
    || listed.contains("manifest.json")
  {
    return Err(mismatch());
  }
  for entry in &manifest.files {
    let relative = checked_relative(Path::new(&entry.path)).map_err(|_| mismatch())?;
    if relative != entry.path
      || digest(&inspect_regular(&root.join(relative), "snapshot")?) != entry.sha256
    {
      return Err(mismatch());
    }
  }
  if generation(&manifest).map_err(|_| mismatch())? != manifest.generation {
    return Err(render_mismatch());
  }
  Ok(())
}

pub fn selected(
  paths: &Paths,
  name: &str,
  graph: &Value,
  revision: &str,
  owner_files: Option<&[(PathBuf, Vec<u8>)]>,
) -> Result<Option<PathBuf>, Diagnostic> {
  let saved = graph_compat::read(&paths.node_graph_path(name)).map_err(|_| revision_mismatch())?;
  if saved.revision != revision || saved.value != *graph {
    return Err(revision_mismatch());
  }
  let generated = paths.generated(name);
  let marker = marker_path(&generated, revision);
  let marked = marker_present(&marker)?;
  if marked {
    verify_marker(&marker, revision)?;
  }
  let selection = read_selection(&generated)?;
  let entry = selection.as_ref().and_then(|map| map.entries.get(revision));
  match entry {
    Some(Entry::V2 { generation }) => {
      let root = leaf(&generated, revision, generation);
      let manifest: Manifest =
        serde_json::from_slice(&inspect_regular(&root.join("manifest.json"), "snapshot")?)
          .map_err(|_| mismatch())?;
      if manifest.revision != revision || &manifest.generation != generation {
        return Err(mismatch());
      }
      let roots = defaults(graph)?;
      if manifest.default_roots != roots.0 || manifest.defaults != roots.1 {
        return Err(mismatch());
      }
      if manifest.packages != package_inventory(paths, graph)?
        || manifest.secret_refs != secret_refs(graph)?
        || manifest.egia_chain != capture_egia_chain(graph).map_err(|_| mismatch())?
      {
        return Err(mismatch());
      }
      let selected_gate = crate::runtime::process::vision_binding::selected_gate_for_graph(graph)
        .map_err(|_| mismatch())?;
      let stored_template = manifest
        .files
        .iter()
        .find(|entry| entry.path == crate::runtime::process::vision_binding::TEMPLATE_FILE);
      match (selected_gate, stored_template) {
        (None, None) => {}
        (Some((gate, fingerprint)), Some(entry)) => {
          let bytes = inspect_regular(
            &root.join(crate::runtime::process::vision_binding::TEMPLATE_FILE),
            "snapshot",
          )?;
          let template: crate::runtime::process::vision_binding::Template =
            serde_json::from_slice(&bytes).map_err(|_| mismatch())?;
          if template.gate_path != gate
            || template.graph_revision != revision
            || digest(&bytes) != entry.sha256
            || manifest.vision_selection_sha256.as_deref() != Some(fingerprint.as_str())
          {
            return Err(mismatch());
          }
        }
        _ => return Err(mismatch()),
      }
      check_launch_inputs(paths, graph, &manifest.launch_inputs, "snapshot")?;
      verify_leaf(&root, &manifest)?;
      crate::runtime::process::skill_selection::compare_selected(paths, graph, revision, &root)?;
      let mut stored_inputs: Vec<LaunchInput> = serde_json::from_slice(&inspect_regular(
        &root.join("core-setup/launch-inputs.json"),
        "snapshot",
      )?)
      .map_err(|_| mismatch())?;
      stored_inputs.sort_by(|a, b| (&a.core, &a.binary).cmp(&(&b.core, &b.binary)));
      if stored_inputs != manifest.launch_inputs {
        return Err(mismatch());
      }
      if let Some(owner_files) = owner_files {
        let (current, files) = inventory(paths, graph, revision, owner_files)?;
        if current.files != manifest.files
          || files.iter().any(|(path, bytes)| {
            fs::read(root.join(path)).ok().as_deref() != Some(bytes.as_slice())
          })
        {
          return Err(render_mismatch());
        }
      }
      if !marked {
        let _lock = crate::runtime::lock::Lock::take(paths).map_err(|_| mismatch())?;
        let saved = graph_compat::read(&paths.node_graph_path(name)).map_err(|_| mismatch())?;
        if saved.revision != revision {
          return Err(revision_mismatch());
        }
        let current = read_selection(&generated)?.ok_or_else(mismatch)?;
        if current.entries.get(revision)
          != Some(&Entry::V2 {
            generation: generation.clone(),
          })
        {
          return Err(mismatch());
        }
        if !marker_present(&marker)? {
          private_dir(&generated.join("v2/required"))?;
          publish_file_replace(
            &marker,
            &serde_json::to_vec_pretty(&Marker {
              schema_version: 1,
              revision: revision.to_owned(),
            })
            .map_err(|_| mismatch())?,
          )?;
        } else {
          verify_marker(&marker, revision)?;
        }
      }
      Ok(Some(root))
    }
    Some(Entry::G03) if !marked => Ok(None),
    None
      if !marked
        && selection
          .as_ref()
          .is_none_or(|map| !map.entries.contains_key(revision)) =>
    {
      Ok(None)
    }
    None if marked && selection.is_some() => Err(revision_mismatch()),
    _ => Err(mismatch()),
  }
}

pub fn selected_root(
  paths: &Paths,
  name: &str,
  graph: &Value,
  revision: &str,
) -> Result<PathBuf, Diagnostic> {
  Ok(
    selected(paths, name, graph, revision, None)?
      .unwrap_or_else(|| paths.generated(name).join(revision)),
  )
}

/// Return only an explicitly selected, fully verified schema-2 V2 generation.
pub fn selected_v2_root(
  paths: &Paths,
  name: &str,
  graph: &Value,
  revision: &str,
) -> Result<PathBuf, Diagnostic> {
  let root = selected(paths, name, graph, revision, None)?.ok_or_else(|| missing("snapshot"))?;
  let manifest: Manifest =
    serde_json::from_slice(&inspect_regular(&root.join("manifest.json"), "snapshot")?)
      .map_err(|_| mismatch())?;
  if manifest.schema_version != 2
    || manifest
      .files
      .iter()
      .filter(|file| file.path == "eos/graph.json")
      .count()
      != 1
  {
    return Err(mismatch());
  }
  inspect_regular(&root.join("eos/graph.json"), "snapshot")?;
  Ok(root)
}

#[cfg(test)]
mod tests {
  use super::*;
  use serde_json::json;

  #[test]
  fn unified_eos_layer_requires_owner_launch_inputs() {
    let temp = tempfile::tempdir().unwrap();
    let graph = json!({"version":"0.2.0","cores":{"isos":{"layers":{"eos_hardware":{}}}}});
    let error = check_launch_inputs(&paths(temp.path()), &graph, &[], "snapshot").unwrap_err();
    assert_eq!(error.code, "generated_manifest_mismatch");
  }

  fn paths(root: &Path) -> Paths {
    Paths {
      config: root.join("config"),
      data: root.join("data"),
      runtime: root.join("run"),
    }
  }
  fn graph() -> Value {
    json!({"version":"0.1.0","robot":{},"cores":{}})
  }
  fn files(profile: &[u8]) -> Vec<(PathBuf, Vec<u8>)> {
    vec![
      (PathBuf::from("eos/graph.json"), profile.to_vec()),
      (PathBuf::from("launch-inputs.json"), b"[]".to_vec()),
    ]
  }
  fn applied(paths: &Paths, graph: &Value) -> String {
    let bytes = serde_json::to_vec_pretty(graph).unwrap();
    let revision = graph_compat::revision(&bytes);
    crate::app::config::atomic_write(&paths.node_graph_path("test"), &bytes).unwrap();
    revision
  }
  #[test]
  fn digest_and_paths_are_stable() {
    assert!(valid_hash(&digest(b"graph")));
    assert!(!valid_hash(&"A".repeat(64)));
    assert!(checked_relative(Path::new("eos/graph.json")).is_ok());
    assert!(checked_relative(Path::new("../graph.json")).is_err());
    assert_eq!(escape("a~/b"), "a~0~1b");
  }

  #[test]
  fn publishes_private_revision_selected_generation_and_keeps_same_input_bytes() {
    let temp = tempfile::tempdir().unwrap();
    let paths = paths(temp.path());
    let graph = graph();
    let revision = applied(&paths, &graph);
    let files = files(b"eos projection");
    let first = publish(&paths, "test", &graph, &revision, &files).unwrap();
    let manifest_bytes = fs::read(first.join("manifest.json")).unwrap();
    let manifest: Manifest = serde_json::from_slice(&manifest_bytes).unwrap();
    assert_eq!(manifest.schema_version, 2);
    assert_eq!(manifest.revision, revision);
    assert_eq!(
      manifest
        .files
        .iter()
        .map(|item| item.path.as_str())
        .collect::<Vec<_>>(),
      vec!["core-setup/launch-inputs.json", "eos/graph.json"]
    );
    assert_eq!(manifest.generation, generation(&manifest).unwrap());
    assert_eq!(
      fs::symlink_metadata(&first).unwrap().permissions().mode() & 0o777,
      0o700
    );
    for directory in [
      paths.generated("test").join("v2"),
      paths.generated("test").join("v2/required"),
      paths.generated("test").join("v2").join(&revision),
    ] {
      assert_eq!(
        fs::symlink_metadata(directory)
          .unwrap()
          .permissions()
          .mode()
          & 0o777,
        0o700
      );
    }
    assert_eq!(
      fs::symlink_metadata(first.join("manifest.json"))
        .unwrap()
        .permissions()
        .mode()
        & 0o777,
      0o600
    );
    assert_eq!(
      publish(&paths, "test", &graph, &revision, &files).unwrap(),
      first
    );
    assert_eq!(
      fs::read(first.join("manifest.json")).unwrap(),
      manifest_bytes
    );
    assert_eq!(
      selected(&paths, "test", &graph, &revision, Some(&files)).unwrap(),
      Some(first.clone())
    );
    assert_eq!(
      selected_v2_root(&paths, "test", &graph, &revision).unwrap(),
      first
    );
  }

  #[test]
  fn managed_run_requires_explicit_v2_selection_not_g03_fallback() {
    let temp = tempfile::tempdir().unwrap();
    let paths = paths(temp.path());
    let graph = graph();
    let revision = applied(&paths, &graph);
    assert!(selected_root(&paths, "test", &graph, &revision).is_ok());
    assert_eq!(
      selected_v2_root(&paths, "test", &graph, &revision)
        .unwrap_err()
        .code,
      "generated_missing_input"
    );
    let mut selection = Selection {
      schema_version: 2,
      entries: BTreeMap::new(),
    };
    selection.entries.insert(revision.clone(), Entry::G03);
    private_dir(&paths.generated("test")).unwrap();
    write_private(
      &paths.generated("test").join("selection.json"),
      &serde_json::to_vec(&selection).unwrap(),
    )
    .unwrap();
    assert_eq!(
      selected_v2_root(&paths, "test", &graph, &revision)
        .unwrap_err()
        .code,
      "generated_missing_input"
    );
  }

  #[test]
  fn changed_render_selects_new_generation_without_mutating_old_leaf() {
    let temp = tempfile::tempdir().unwrap();
    let paths = paths(temp.path());
    let graph = graph();
    let revision = applied(&paths, &graph);
    let first = publish(&paths, "test", &graph, &revision, &files(b"first")).unwrap();
    let old = fs::read(first.join("eos/graph.json")).unwrap();
    let second = publish(&paths, "test", &graph, &revision, &files(b"second")).unwrap();
    assert_ne!(first, second);
    assert_eq!(fs::read(first.join("eos/graph.json")).unwrap(), old);
    assert_eq!(
      selected(&paths, "test", &graph, &revision, Some(&files(b"second"))).unwrap(),
      Some(second)
    );
  }

  #[test]
  fn independent_marker_blocks_deleted_selection_and_corrupt_leaf() {
    let temp = tempfile::tempdir().unwrap();
    let paths = paths(temp.path());
    let graph = graph();
    let revision = applied(&paths, &graph);
    let root = publish(&paths, "test", &graph, &revision, &files(b"first")).unwrap();
    fs::create_dir_all(paths.generated("test").join(&revision)).unwrap();
    fs::remove_file(paths.generated("test").join("selection.json")).unwrap();
    let error = selected(&paths, "test", &graph, &revision, None).unwrap_err();
    assert_eq!(
      (error.code, error.stage),
      ("generated_manifest_mismatch", "snapshot")
    );
    publish(&paths, "test", &graph, &revision, &files(b"first")).unwrap();
    fs::write(root.join("eos/graph.json"), b"changed").unwrap();
    assert_eq!(
      selected(&paths, "test", &graph, &revision, None)
        .unwrap_err()
        .code,
      "generated_manifest_mismatch"
    );
  }

  #[test]
  fn symlinked_generated_ancestors_reject_byte_identical_leaf() {
    let temp = tempfile::tempdir().unwrap();
    let paths = paths(temp.path());
    let graph = graph();
    let revision = applied(&paths, &graph);
    let root = publish(&paths, "test", &graph, &revision, &files(b"profile")).unwrap();
    let generated = paths.generated("test");
    let moved = paths.data.join("generated-copy");
    fs::rename(&generated, &moved).unwrap();
    std::os::unix::fs::symlink(&moved, &generated).unwrap();
    let error = selected(&paths, "test", &graph, &revision, None).unwrap_err();
    assert_eq!(
      (error.code, error.stage, error.path.as_str()),
      ("generated_unsafe_path", "snapshot", "")
    );
    fs::remove_file(&generated).unwrap();
    fs::rename(&moved, &generated).unwrap();
    let revision_dir = root.parent().unwrap();
    let moved_revision = generated.join("v2/revision-copy");
    fs::rename(revision_dir, &moved_revision).unwrap();
    std::os::unix::fs::symlink(&moved_revision, revision_dir).unwrap();
    let error = selected(&paths, "test", &graph, &revision, None).unwrap_err();
    assert_eq!(
      (error.code, error.stage, error.path.as_str()),
      ("generated_unsafe_path", "snapshot", "")
    );
  }

  #[test]
  fn valid_selection_recovers_missing_marker_only_for_saved_revision() {
    let temp = tempfile::tempdir().unwrap();
    let paths = paths(temp.path());
    let graph = graph();
    let revision = applied(&paths, &graph);
    let root = publish(&paths, "test", &graph, &revision, &files(b"first")).unwrap();
    let marker = marker_path(&paths.generated("test"), &revision);
    fs::remove_file(&marker).unwrap();
    assert_eq!(
      selected(&paths, "test", &graph, &revision, None).unwrap(),
      Some(root)
    );
    assert!(marker.exists());
    let mut selection = read_selection(&paths.generated("test")).unwrap().unwrap();
    selection.entries.remove(&revision);
    selection.entries.insert(
      digest(b"other"),
      Entry::V2 {
        generation: digest(b"gen"),
      },
    );
    publish_file_replace(
      &paths.generated("test").join("selection.json"),
      &serde_json::to_vec(&selection).unwrap(),
    )
    .unwrap();
    let error = selected(&paths, "test", &graph, &revision, None).unwrap_err();
    assert_eq!(
      (error.code, error.stage),
      ("generated_revision_mismatch", "graph")
    );
  }

  #[test]
  fn selection_symlink_and_unlisted_snapshot_file_fail_closed() {
    let temp = tempfile::tempdir().unwrap();
    let paths = paths(temp.path());
    let graph = graph();
    let revision = applied(&paths, &graph);
    let root = publish(&paths, "test", &graph, &revision, &files(b"first")).unwrap();
    let map = paths.generated("test").join("selection.json");
    let copy = paths.generated("test").join("selection-copy.json");
    fs::rename(&map, &copy).unwrap();
    std::os::unix::fs::symlink(&copy, &map).unwrap();
    assert_eq!(
      selected(&paths, "test", &graph, &revision, None)
        .unwrap_err()
        .code,
      "generated_unsafe_path"
    );
    fs::remove_file(&map).unwrap();
    fs::rename(&copy, &map).unwrap();
    fs::write(root.join("unlisted.txt"), b"data").unwrap();
    assert_eq!(
      selected(&paths, "test", &graph, &revision, None)
        .unwrap_err()
        .code,
      "generated_manifest_mismatch"
    );
  }

  #[test]
  fn paired_file_and_manifest_digest_tamper_fails_generation_check() {
    let temp = tempfile::tempdir().unwrap();
    let paths = paths(temp.path());
    let graph = graph();
    let revision = applied(&paths, &graph);
    let root = publish(&paths, "test", &graph, &revision, &files(b"first")).unwrap();
    fs::write(root.join("eos/graph.json"), b"changed").unwrap();
    let manifest_path = root.join("manifest.json");
    let mut manifest: Manifest =
      serde_json::from_slice(&fs::read(&manifest_path).unwrap()).unwrap();
    manifest
      .files
      .iter_mut()
      .find(|entry| entry.path == "eos/graph.json")
      .unwrap()
      .sha256 = digest(b"changed");
    fs::write(&manifest_path, serde_json::to_vec(&manifest).unwrap()).unwrap();
    let error = selected(&paths, "test", &graph, &revision, None).unwrap_err();
    assert_eq!(
      (error.code, error.stage),
      ("generated_render_mismatch", "snapshot")
    );
  }

  #[test]
  fn future_revision_selection_is_inert_for_saved_graph() {
    let temp = tempfile::tempdir().unwrap();
    let paths = paths(temp.path());
    let graph = graph();
    let revision = applied(&paths, &graph);
    let root = publish(&paths, "test", &graph, &revision, &files(b"first")).unwrap();
    let mut selection = read_selection(&paths.generated("test")).unwrap().unwrap();
    selection.entries.insert(
      digest(b"future graph"),
      Entry::V2 {
        generation: digest(b"future leaf"),
      },
    );
    publish_file_replace(
      &paths.generated("test").join("selection.json"),
      &serde_json::to_vec(&selection).unwrap(),
    )
    .unwrap();
    assert_eq!(
      selected(&paths, "test", &graph, &revision, None).unwrap(),
      Some(root)
    );
  }

  #[test]
  fn changed_graph_publication_keeps_old_entry_until_graph_rename() {
    let temp = tempfile::tempdir().unwrap();
    let paths = paths(temp.path());
    let old_graph = graph();
    let old_revision = applied(&paths, &old_graph);
    let old_leaf = publish(&paths, "test", &old_graph, &old_revision, &files(b"old")).unwrap();
    let mut new_graph = graph();
    new_graph["robot"] = json!({"id":"future"});
    let new_bytes = serde_json::to_vec_pretty(&new_graph).unwrap();
    let new_revision = graph_compat::revision(&new_bytes);
    let new_leaf = publish(&paths, "test", &new_graph, &new_revision, &files(b"new")).unwrap();
    assert_eq!(
      selected(&paths, "test", &old_graph, &old_revision, None).unwrap(),
      Some(old_leaf)
    );
    let error = selected(&paths, "test", &new_graph, &new_revision, None).unwrap_err();
    assert_eq!(
      (error.code, error.stage),
      ("generated_revision_mismatch", "graph")
    );
    crate::app::config::atomic_write(&paths.node_graph_path("test"), &new_bytes).unwrap();
    assert_eq!(
      selected(&paths, "test", &new_graph, &new_revision, None).unwrap(),
      Some(new_leaf)
    );
  }

  #[test]
  fn later_child_missing_secret_reports_exact_pointer_before_launch() {
    let first = crate::runtime::process::ProcessSpec {
      id: "eos".into(),
      path: "cores/isos/layers/process/groups/component/nodes/eos".into(),
      settings: serde_json::from_value(json!({})).unwrap(),
      package: None,
      fingerprint: None,
    };
    let mut second = first.clone();
    second.id = "egia".into();
    second.settings.env_from.insert(
      "EGIA_API_KEY".into(),
      "ISOS_P04_TEST_NEVER_SET_9AA129EB".into(),
    );
    let error = secret_plan(&[first, second]).unwrap_err();
    assert_eq!(
      (error.code, error.stage, error.path.as_str()),
      (
        "generated_missing_secret",
        "spawn",
        "/cores/isos/layers/process/groups/component/nodes/egia/settings/env_from/EGIA_API_KEY"
      )
    );
    assert!(!error.message.contains("ISOS_P04_TEST_NEVER_SET"));
  }

  #[test]
  fn default_tree_rejects_symlinks_and_invalid_toml() {
    let temp = tempfile::tempdir().unwrap();
    let root = temp.path().join("defaults");
    fs::create_dir(&root).unwrap();
    let file = root.join("models.toml");
    fs::write(&file, b"[models]\n").unwrap();
    assert_eq!(walk_tree(&root, "component").unwrap().0.len(), 1);
    fs::write(&file, b"[models\n").unwrap();
    let error = walk_tree(&root, "component").unwrap_err();
    assert_eq!(
      (error.code, error.stage),
      ("generated_invalid_default", "defaults")
    );
    fs::remove_file(&file).unwrap();
    std::os::unix::fs::symlink(temp.path().join("outside"), &file).unwrap();
    let error = walk_tree(&root, "component").unwrap_err();
    assert_eq!(
      (error.code, error.stage),
      ("generated_unsafe_path", "defaults")
    );
  }

  #[test]
  fn factory_default_root_must_match_selected_launcher_prefix() {
    let prefix = Path::new("/opt/selected-egia");
    assert!(
      validate_factory_root(Path::new("/opt/selected-egia/share/defaults/egia"), prefix).is_ok()
    );
    let error =
      validate_factory_root(Path::new("/opt/other-egia/share/defaults/egia"), prefix).unwrap_err();
    assert_eq!(
      (error.code, error.stage, error.path.as_str()),
      ("generated_invalid_default", "defaults", "")
    );
  }

  #[test]
  fn each_required_egia_default_is_checked_before_render() {
    let required: Vec<_> = REQUIRED_EGIA
      .iter()
      .copied()
      .filter(|path| *path != "profiles/isos.toml")
      .map(|path| DefaultEntry {
        source: "component".into(),
        path: path.into(),
        sha256: digest(path.as_bytes()),
      })
      .collect();
    require_component_defaults(&required).unwrap();
    for omitted in ["prompts.toml", "missions.toml", "safety/gate.toml"] {
      let subset: Vec<_> = required
        .iter()
        .filter(|entry| entry.path != omitted)
        .cloned()
        .collect();
      let error = require_component_defaults(&subset).unwrap_err();
      assert_eq!(
        (error.code, error.stage, error.path.as_str()),
        ("generated_missing_input", "defaults", ""),
        "{omitted}"
      );
    }
  }

  #[test]
  fn egia_isos_profile_must_come_from_owner_render() {
    let complete = vec![
      (PathBuf::from("egia/egia.toml"), b"mode = 'isos'\n".to_vec()),
      (
        PathBuf::from("egia/modes.toml"),
        b"mode = 'isos'\n".to_vec(),
      ),
      (
        PathBuf::from("egia/profiles/isos.toml"),
        b"managed = true\n".to_vec(),
      ),
    ];
    require_egia_owner_overlays(&complete, "egia").unwrap();
    let mut extra = complete.clone();
    extra.push((PathBuf::from("egia/prompts.toml"), b"override".to_vec()));
    let error = require_egia_owner_overlays(&extra, "egia").unwrap_err();
    assert_eq!(
      (error.code, error.stage, error.path.as_str()),
      ("generated_unsafe_path", "render", "")
    );
    for omitted in [
      "egia/egia.toml",
      "egia/modes.toml",
      "egia/profiles/isos.toml",
    ] {
      let incomplete: Vec<_> = complete
        .iter()
        .filter(|(path, _)| path != Path::new(omitted))
        .cloned()
        .collect();
      let error = require_egia_owner_overlays(&incomplete, "egia").unwrap_err();
      assert_eq!(
        (error.code, error.stage, error.path.as_str()),
        ("generated_missing_input", "render", ""),
        "{omitted}"
      );
    }
  }

  #[test]
  fn selection_parent_sync_failure_leaves_only_complete_old_or_new_generation() {
    let temp = tempfile::tempdir().unwrap();
    let paths = paths(temp.path());
    let graph = graph();
    let revision = applied(&paths, &graph);
    let old = publish(&paths, "test", &graph, &revision, &files(b"old")).unwrap();
    let old_bytes = fs::read(old.join("eos/graph.json")).unwrap();
    fail_parent_sync_once(&paths.generated("test").join("selection.json"));
    let error = publish(&paths, "test", &graph, &revision, &files(b"new")).unwrap_err();
    assert_eq!(
      (error.code, error.stage),
      ("generated_commit_uncertain", "publish")
    );
    assert_eq!(fs::read(old.join("eos/graph.json")).unwrap(), old_bytes);
    let observed = selected(&paths, "test", &graph, &revision, None)
      .unwrap()
      .unwrap();
    assert_eq!(fs::read(observed.join("eos/graph.json")).unwrap(), b"new");
    assert!(marker_present(&marker_path(&paths.generated("test"), &revision)).unwrap());
  }

  #[test]
  fn initial_marker_parent_sync_failure_recovers_selected_complete_leaf() {
    let temp = tempfile::tempdir().unwrap();
    let paths = paths(temp.path());
    let graph = graph();
    let revision = applied(&paths, &graph);
    let marker = marker_path(&paths.generated("test"), &revision);
    fail_parent_sync_once(&marker);
    let error = publish(&paths, "test", &graph, &revision, &files(b"profile")).unwrap_err();
    assert_eq!(
      (error.code, error.stage),
      ("generated_commit_uncertain", "publish")
    );
    let root = selected(&paths, "test", &graph, &revision, None)
      .unwrap()
      .unwrap();
    assert_eq!(fs::read(root.join("eos/graph.json")).unwrap(), b"profile");
    fs::remove_file(&marker).unwrap();
    assert_eq!(
      selected(&paths, "test", &graph, &revision, None).unwrap(),
      Some(root)
    );
    assert!(marker_present(&marker).unwrap());
  }
}
