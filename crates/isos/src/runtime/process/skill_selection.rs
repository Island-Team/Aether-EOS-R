//! Exact, non-executing selection of verified host-skill libraries for EOS.
use crate::{app::config::Paths, domain::packages};
use anyhow::{Result, ensure};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
  collections::{BTreeMap, BTreeSet},
  ffi::CString,
  fs::{self, File, OpenOptions},
  io::Read,
  os::{
    fd::{AsRawFd, FromRawFd},
    unix::fs::{MetadataExt, OpenOptionsExt},
  },
  path::{Path, PathBuf},
};

const BASE: &str = "/cores/isos/layers/process/groups/component/nodes";
const MAX_LIBRARY_BYTES: u64 = 64 * 1024 * 1024;
const MAX_SELECTION_BYTES: usize = 64 * 1024;
pub const ENV: &str = "EOS_SKILL_SELECTION_FILE";
pub const RELATIVE_FILE: &str = "isos/skill-selection.json";

pub fn setting_descriptor() -> Value {
  json!({
      "name":"skill_libraries","data_type":"object_array","required":false,
      "description":"Explicit verified native skill libraries for the selected EOS runtime",
      "min_items":0,"max_items":16,
      "items_schema":{
          "type":"object","additionalProperties":false,"required":["package","library"],
          "properties":{
              "package":{"type":"object","additionalProperties":false,"required":["id","version"],
                  "properties":{"id":{"type":"string","minLength":1},"version":{"type":"string","minLength":1}}},
              "library":{"type":"string","minLength":1,"maxLength":256}
          }
      }
  })
}

#[derive(Clone, Debug, Serialize)]
pub struct Diagnostic {
  pub code: &'static str,
  pub path: String,
  pub message: &'static str,
  pub stage: &'static str,
}

impl std::fmt::Display for Diagnostic {
  fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      formatter,
      "{} at {}: {}",
      self.code, self.path, self.message
    )
  }
}
impl std::error::Error for Diagnostic {}

fn diagnostic(code: &'static str, path: String) -> Diagnostic {
  let (stage, message) = match code {
    "skill_package_incompatible" => ("package", "Select a verified native skill release"),
    "skill_library_unsafe" => (
      "package",
      "Use same-owner, non-writable-by-others package files and a bounded .skill.so",
    ),
    "skill_library_changed" => ("package", "Reverify the selected library and Apply again"),
    _ => (
      "graph",
      "Correct the EOS skill selection and Validate again",
    ),
  };
  Diagnostic {
    code,
    path,
    message,
    stage,
  }
}

#[derive(Clone, Debug, Deserialize, PartialEq, Eq, PartialOrd, Ord)]
#[serde(deny_unknown_fields)]
pub struct Pin {
  pub id: String,
  pub version: String,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Eq, PartialOrd, Ord)]
#[serde(deny_unknown_fields)]
pub struct Entry {
  pub package: Pin,
  pub library: String,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct SelectedLibrary {
  pub package_id: String,
  pub package_version: String,
  pub package_root: String,
  pub payload_root: String,
  pub receipt_sha256: String,
  pub release_sha256: String,
  pub payload_sha256: String,
  pub library: String,
  pub library_sha256: String,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Selection {
  pub schema_version: u32,
  pub active_revision: String,
  pub libraries: Vec<SelectedLibrary>,
}

fn pointer(id: &str) -> String {
  format!(
    "{BASE}/{}/settings/skill_libraries",
    crate::domain::graph::graph_compat::pointer_segment(id)
  )
}

fn valid_library(path: &str) -> bool {
  !path.is_empty()
    && path.len() <= 256
    && path.ends_with(".skill.so")
    && !path.contains(['\\', '\0', ':', '*', '?', '[', ']'])
    && !path.chars().any(char::is_control)
    && path.split('/').all(|part| !matches!(part, "" | "." | ".."))
}

fn valid_pin(pin: &Pin) -> bool {
  if pin.id.len() > 96 || pin.version.len() > 64 || pin.version.split('.').count() < 3 {
    return false;
  }
  let Some((namespace, name)) = pin.id.split_once('/') else {
    return false;
  };
  let slug = |value: &str| {
    !value.is_empty()
      && value
        .bytes()
        .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-')
  };
  slug(namespace)
    && slug(name)
    && pin
      .version
      .bytes()
      .next()
      .is_some_and(|byte| byte.is_ascii_digit())
    && pin
      .version
      .bytes()
      .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'+'))
}

/// Parse the raw graph before generic process deserialization erases field pointers.
pub fn entries(graph: &Value) -> std::result::Result<Vec<(Entry, String)>, Diagnostic> {
  let mut result = Vec::new();
  if let Some(cores) = graph["cores"].as_object() {
    for (core_id, core) in cores {
      let Some(layers) = core["layers"].as_object() else {
        continue;
      };
      for (layer_id, layer) in layers {
        let Some(groups) = layer["groups"].as_object() else {
          continue;
        };
        for (group_id, group) in groups {
          let Some(nodes) = group["nodes"].as_object() else {
            continue;
          };
          for (id, node) in nodes {
            if node.pointer("/settings/skill_libraries").is_some()
              && !(core_id == "isos" && layer_id == "process" && group_id == "component")
            {
              return Err(diagnostic(
                "skill_selection_invalid",
                format!(
                  "/cores/{}/layers/{}/groups/{}/nodes/{}/settings/skill_libraries",
                  crate::domain::graph::graph_compat::pointer_segment(core_id),
                  crate::domain::graph::graph_compat::pointer_segment(layer_id),
                  crate::domain::graph::graph_compat::pointer_segment(group_id),
                  crate::domain::graph::graph_compat::pointer_segment(id)
                ),
              ));
            }
          }
        }
      }
    }
  }
  let Some(nodes) = graph.pointer(BASE).and_then(Value::as_object) else {
    return Ok(result);
  };
  for (id, node) in nodes {
    let field = pointer(id);
    let Some(value) = node.pointer("/settings/skill_libraries") else {
      continue;
    };
    if graph["version"] != "0.1.0"
      || node["node_type"] != "isos.process.component@v0.1"
      || id != "eos"
      || node["settings"]["role"] != "eos"
      || node["settings"]["command"].is_array()
    {
      return Err(diagnostic("skill_selection_invalid", field));
    }
    let list = value
      .as_array()
      .ok_or_else(|| diagnostic("skill_selection_invalid", field.clone()))?;
    if list.len() > 16 {
      return Err(diagnostic("skill_selection_invalid", field));
    }
    let mut seen = BTreeSet::new();
    for (index, item) in list.iter().enumerate() {
      let item_path = format!("{field}/{index}");
      let entry: Entry = serde_json::from_value(item.clone())
        .map_err(|_| diagnostic("skill_selection_invalid", item_path.clone()))?;
      if !valid_pin(&entry.package) {
        return Err(diagnostic(
          "skill_selection_invalid",
          format!("{item_path}/package"),
        ));
      }
      if !valid_library(&entry.library) {
        return Err(diagnostic(
          "skill_library_unsafe",
          format!("{item_path}/library"),
        ));
      }
      if !seen.insert((
        entry.package.id.clone(),
        entry.package.version.clone(),
        entry.library.clone(),
      )) {
        return Err(diagnostic("skill_selection_invalid", item_path));
      }
      result.push((entry, item_path));
    }
  }
  Ok(result)
}

pub fn check_reserved(graph: &Value) -> std::result::Result<(), Diagnostic> {
  let Some(nodes) = graph.pointer(BASE).and_then(Value::as_object) else {
    return Ok(());
  };
  for (id, node) in nodes {
    let base = format!(
      "{BASE}/{}",
      crate::domain::graph::graph_compat::pointer_segment(id)
    );
    if node.pointer(&format!("/settings/env/{ENV}")).is_some() {
      return Err(diagnostic(
        "skill_selection_invalid",
        format!("{base}/settings/env/{ENV}"),
      ));
    }
    if node.pointer(&format!("/settings/env_from/{ENV}")).is_some() {
      return Err(diagnostic(
        "skill_selection_invalid",
        format!("{base}/settings/env_from/{ENV}"),
      ));
    }
    if let Some(sources) = node
      .pointer("/settings/env_from")
      .and_then(Value::as_object)
      && let Some((target, _)) = sources
        .iter()
        .find(|(_, source)| source.as_str() == Some(ENV))
    {
      return Err(diagnostic(
        "skill_selection_invalid",
        format!(
          "{base}/settings/env_from/{}",
          crate::domain::graph::graph_compat::pointer_segment(target)
        ),
      ));
    }
    if let Some(args) = node.pointer("/settings/args").and_then(Value::as_array)
      && let Some(index) = args.iter().position(|arg| {
        arg
          .as_str()
          .is_some_and(|arg| arg.starts_with("--skill-selection"))
      })
    {
      return Err(diagnostic(
        "skill_selection_invalid",
        format!("{base}/settings/args/{index}"),
      ));
    }
  }
  Ok(())
}

fn selected_file(
  root: &Path,
  relative: &str,
  path: &str,
) -> std::result::Result<String, Diagnostic> {
  let file = packages::checked_file(root, relative, false)
    .map_err(|_| diagnostic("skill_library_unsafe", format!("{path}/library")))?;
  let before = fs::symlink_metadata(&file)
    .map_err(|_| diagnostic("skill_library_unsafe", format!("{path}/library")))?;
  if !before.file_type().is_file() || !(1..=MAX_LIBRARY_BYTES).contains(&before.len()) {
    return Err(diagnostic(
      "skill_library_unsafe",
      format!("{path}/library"),
    ));
  }
  let mut directory = OpenOptions::new()
    .read(true)
    .custom_flags(libc::O_DIRECTORY | libc::O_NOFOLLOW)
    .open(root)
    .map_err(|_| diagnostic("skill_library_unsafe", format!("{path}/library")))?;
  let pieces: Vec<_> = relative.split('/').collect();
  for piece in &pieces[..pieces.len() - 1] {
    let name = CString::new(*piece)
      .map_err(|_| diagnostic("skill_library_unsafe", format!("{path}/library")))?;
    let fd = unsafe {
      libc::openat(
        directory.as_raw_fd(),
        name.as_ptr(),
        libc::O_RDONLY | libc::O_CLOEXEC | libc::O_NOFOLLOW | libc::O_DIRECTORY,
      )
    };
    if fd < 0 {
      return Err(diagnostic(
        "skill_library_unsafe",
        format!("{path}/library"),
      ));
    }
    directory = unsafe { File::from_raw_fd(fd) };
  }
  let name = CString::new(*pieces.last().unwrap())
    .map_err(|_| diagnostic("skill_library_unsafe", format!("{path}/library")))?;
  let (mut opened, actual) = open_selected(&directory, &name, path)?;
  if (before.dev(), before.ino(), before.len()) != (actual.dev(), actual.ino(), actual.len()) {
    return Err(diagnostic(
      "skill_library_changed",
      format!("{path}/library"),
    ));
  }
  let mut hash = Sha256::new();
  let mut total = 0_u64;
  let mut buffer = [0_u8; 1024 * 1024];
  loop {
    let count = opened
      .read(&mut buffer)
      .map_err(|_| diagnostic("skill_library_changed", format!("{path}/library")))?;
    if count == 0 {
      break;
    }
    total += count as u64;
    if total > MAX_LIBRARY_BYTES {
      return Err(diagnostic(
        "skill_library_changed",
        format!("{path}/library"),
      ));
    }
    hash.update(&buffer[..count]);
  }
  let after = fs::symlink_metadata(&file)
    .map_err(|_| diagnostic("skill_library_changed", format!("{path}/library")))?;
  if total != before.len()
    || (before.dev(), before.ino(), before.len()) != (after.dev(), after.ino(), after.len())
  {
    return Err(diagnostic(
      "skill_library_changed",
      format!("{path}/library"),
    ));
  }
  Ok(hex::encode(hash.finalize()))
}

fn open_selected(
  directory: &File,
  name: &CString,
  path: &str,
) -> std::result::Result<(File, fs::Metadata), Diagnostic> {
  let fd = unsafe {
    libc::openat(
      directory.as_raw_fd(),
      name.as_ptr(),
      libc::O_RDONLY | libc::O_CLOEXEC | libc::O_NOFOLLOW | libc::O_NONBLOCK,
    )
  };
  if fd < 0 {
    return Err(diagnostic(
      "skill_library_unsafe",
      format!("{path}/library"),
    ));
  }
  let opened = unsafe { File::from_raw_fd(fd) };
  let actual = opened
    .metadata()
    .map_err(|_| diagnostic("skill_library_changed", format!("{path}/library")))?;
  if !actual.file_type().is_file()
    || !(1..=MAX_LIBRARY_BYTES).contains(&actual.len())
    || !owner_mode_safe(&actual)
  {
    return Err(diagnostic(
      "skill_library_unsafe",
      format!("{path}/library"),
    ));
  }
  Ok((opened, actual))
}

fn owner_mode_safe(metadata: &fs::Metadata) -> bool {
  metadata.uid() == unsafe { libc::geteuid() } && metadata.mode() & 0o022 == 0
}

fn digest_file(path: &Path, max: u64, pointer: &str) -> std::result::Result<String, Diagnostic> {
  let unsafe_file = || diagnostic("skill_library_unsafe", pointer.into());
  let file = OpenOptions::new()
    .read(true)
    .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
    .open(path)
    .map_err(|_| unsafe_file())?;
  let metadata = file.metadata().map_err(|_| unsafe_file())?;
  if !metadata.file_type().is_file() || metadata.len() > max || !owner_mode_safe(&metadata) {
    return Err(unsafe_file());
  }
  let mut bytes = Vec::new();
  file
    .take(max + 1)
    .read_to_end(&mut bytes)
    .map_err(|_| diagnostic("skill_library_changed", pointer.into()))?;
  if bytes.len() as u64 != metadata.len() {
    return Err(diagnostic("skill_library_changed", pointer.into()));
  }
  Ok(hex::encode(Sha256::digest(&bytes)))
}

fn check_payload_ownership(
  root: &Path,
  installed: bool,
  pointer: &str,
) -> std::result::Result<(), Diagnostic> {
  let unsafe_file = || diagnostic("skill_library_unsafe", pointer.into());
  let mut pending = vec![root.to_path_buf()];
  let mut count = 0_usize;
  while let Some(directory) = pending.pop() {
    for entry in fs::read_dir(&directory).map_err(|_| unsafe_file())? {
      let entry = entry.map_err(|_| unsafe_file())?;
      let path = entry.path();
      let relative = path.strip_prefix(root).map_err(|_| unsafe_file())?;
      let name = relative.to_str().ok_or_else(unsafe_file)?;
      if installed
        && !name.contains('/')
        && matches!(name, "receipt.json" | "release.json" | "node-types.json")
      {
        continue;
      }
      count += 1;
      if count > 16_384 || name.len() > 4096 {
        return Err(unsafe_file());
      }
      let before = fs::symlink_metadata(&path).map_err(|_| unsafe_file())?;
      if before.file_type().is_dir() {
        pending.push(path);
      } else if before.file_type().is_file() {
        let file = OpenOptions::new()
          .read(true)
          .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
          .open(&path)
          .map_err(|_| unsafe_file())?;
        let actual = file.metadata().map_err(|_| unsafe_file())?;
        if !actual.file_type().is_file()
          || !owner_mode_safe(&actual)
          || (before.dev(), before.ino(), before.len())
            != (actual.dev(), actual.ino(), actual.len())
        {
          return Err(unsafe_file());
        }
      } else {
        return Err(unsafe_file());
      }
    }
  }
  Ok(())
}

pub fn render(
  paths: &Paths,
  graph: &Value,
  revision: &str,
) -> std::result::Result<Option<Vec<u8>>, Diagnostic> {
  check_reserved(graph)?;
  let mut selected = entries(graph)?;
  if selected.is_empty() {
    return Ok(None);
  }
  selected.sort_by(|(left, _), (right, _)| left.cmp(right));
  let mut libraries = Vec::new();
  let mut verified = BTreeMap::new();
  for (entry, pointer) in selected {
    let key = (entry.package.id.clone(), entry.package.version.clone());
    let first = !verified.contains_key(&key);
    if first {
      let package =
        packages::verify(paths, &entry.package.id, &entry.package.version).map_err(|error| {
          error
            .downcast::<packages::PackageFailure>()
            .map(|failure| Diagnostic {
              code: failure.code,
              path: format!("{pointer}/package"),
              stage: failure.stage,
              message: "Reinstall or relink the exact selected package",
            })
            .unwrap_or_else(|_| diagnostic("skill_library_changed", format!("{pointer}/package")))
        })?;
      if package.release.kind != "skill" {
        return Err(diagnostic(
          "skill_package_incompatible",
          format!("{pointer}/package"),
        ));
      }
      let metadata_pointer = format!("{pointer}/package");
      digest_file(
        &package.store.join("receipt.json"),
        65_536,
        &metadata_pointer,
      )?;
      let release_sha256 = digest_file(
        &package.store.join("release.json"),
        4_194_304,
        &metadata_pointer,
      )?;
      if package.receipt.release_sha256.as_deref() != Some(&release_sha256) {
        return Err(diagnostic("skill_library_changed", metadata_pointer));
      }
      verified.insert(key.clone(), package);
    }
    let package = &verified[&key];
    if package.release.kind != "skill" {
      return Err(diagnostic(
        "skill_package_incompatible",
        format!("{pointer}/package"),
      ));
    }
    let hash = selected_file(&package.root, &entry.library, &pointer)?;
    if first {
      check_payload_ownership(
        &package.root,
        package.root == package.store,
        &format!("{pointer}/package"),
      )?;
    }
    let receipt_sha256 = digest_file(
      &package.store.join("receipt.json"),
      65_536,
      &format!("{pointer}/package"),
    )?;
    let release_sha256 = package
      .receipt
      .release_sha256
      .clone()
      .ok_or_else(|| diagnostic("skill_library_changed", format!("{pointer}/package")))?;
    let payload_sha256 = package
      .receipt
      .payload_sha256
      .clone()
      .ok_or_else(|| diagnostic("skill_library_changed", format!("{pointer}/package")))?;
    libraries.push(SelectedLibrary {
      package_id: entry.package.id,
      package_version: entry.package.version,
      package_root: package.store.to_string_lossy().into_owned(),
      payload_root: package.root.to_string_lossy().into_owned(),
      receipt_sha256,
      release_sha256,
      payload_sha256,
      library: entry.library,
      library_sha256: hash,
    });
  }
  Ok(Some(encode_selection(revision, libraries)?))
}

fn encode_selection(
  revision: &str,
  libraries: Vec<SelectedLibrary>,
) -> std::result::Result<Vec<u8>, Diagnostic> {
  let bytes = serde_json::to_vec(&Selection {
    schema_version: 1,
    active_revision: revision.into(),
    libraries,
  })
  .map_err(|_| diagnostic("skill_selection_invalid", pointer("eos")))?;
  if bytes.len() > MAX_SELECTION_BYTES {
    return Err(diagnostic("skill_selection_invalid", pointer("eos")));
  }
  Ok(bytes)
}

pub fn compare_selected(
  paths: &Paths,
  graph: &Value,
  revision: &str,
  root: &Path,
) -> std::result::Result<Option<PathBuf>, crate::domain::graph::generated_v2::Diagnostic> {
  let expected = render(paths, graph, revision)
    .map_err(|_| crate::domain::graph::generated_v2::snapshot_mismatch())?;
  let path = root.join(RELATIVE_FILE);
  match expected {
    Some(bytes) => {
      let actual = read_selected_bytes(root, bytes.len())?;
      if actual != bytes {
        return Err(crate::domain::graph::generated_v2::snapshot_mismatch());
      }
      Ok(Some(path))
    }
    None => {
      match fs::symlink_metadata(&path) {
        Ok(_) => return Err(crate::domain::graph::generated_v2::snapshot_mismatch()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(_) => return Err(crate::domain::graph::generated_v2::snapshot_mismatch()),
      }
      Ok(None)
    }
  }
}

fn read_selected_bytes(
  root: &Path,
  expected_len: usize,
) -> std::result::Result<Vec<u8>, crate::domain::graph::generated_v2::Diagnostic> {
  let mismatch = crate::domain::graph::generated_v2::snapshot_mismatch;
  if expected_len == 0 || expected_len > MAX_SELECTION_BYTES {
    return Err(mismatch());
  }
  let directory = OpenOptions::new()
    .read(true)
    .custom_flags(libc::O_DIRECTORY | libc::O_NOFOLLOW)
    .open(root)
    .map_err(|_| mismatch())?;
  let isos = CString::new("isos").unwrap();
  let fd = unsafe {
    libc::openat(
      directory.as_raw_fd(),
      isos.as_ptr(),
      libc::O_RDONLY | libc::O_CLOEXEC | libc::O_NOFOLLOW | libc::O_DIRECTORY,
    )
  };
  if fd < 0 {
    return Err(mismatch());
  }
  let directory = unsafe { File::from_raw_fd(fd) };
  let selection = CString::new("skill-selection.json").unwrap();
  let fd = unsafe {
    libc::openat(
      directory.as_raw_fd(),
      selection.as_ptr(),
      libc::O_RDONLY | libc::O_CLOEXEC | libc::O_NOFOLLOW | libc::O_NONBLOCK,
    )
  };
  if fd < 0 {
    return Err(mismatch());
  }
  let mut file = unsafe { File::from_raw_fd(fd) };
  let metadata = file.metadata().map_err(|_| mismatch())?;
  if !metadata.file_type().is_file() || metadata.len() != expected_len as u64 {
    return Err(mismatch());
  }
  let mut actual = vec![0; expected_len];
  file.read_exact(&mut actual).map_err(|_| mismatch())?;
  let mut extra = [0_u8; 1];
  if file.read(&mut extra).map_err(|_| mismatch())? != 0 {
    return Err(mismatch());
  }
  Ok(actual)
}

pub fn apply_env(specs: &mut [crate::runtime::process::ProcessSpec], selected: Option<&Path>) {
  for spec in specs {
    spec.settings.env.remove(ENV);
    if spec.id == "eos"
      && spec.settings.role.as_deref() == Some("eos")
      && let Some(path) = selected
    {
      spec
        .settings
        .env
        .insert(ENV.into(), path.to_string_lossy().into_owned());
    }
  }
}

pub fn verify_child_handoff(
  paths: &Paths,
  deployment: &str,
  spec: &crate::runtime::process::ProcessSpec,
) -> Result<()> {
  let graph_path = paths.node_graph_path(deployment);
  let saved = match crate::domain::graph::graph_compat::read(&graph_path) {
    Ok(saved) => saved,
    Err(_error)
      if !graph_path.exists()
        && spec.settings.skill_libraries.is_empty()
        && !spec.settings.env.contains_key(ENV) =>
    {
      return Ok(());
    }
    Err(error) => return Err(error),
  };
  let has_selection = !entries(&saved.value)?.is_empty();
  let expected = spec.settings.env.get(ENV);
  if !has_selection {
    ensure!(
      expected.is_none(),
      "unexpected EOS skill handoff; Apply again"
    );
    return Ok(());
  }
  let expected = expected
    .ok_or_else(|| anyhow::anyhow!("selected EOS skill handoff is missing; Apply again"))?;
  let root = crate::domain::graph::generated_v2::selected_v2_root(
    paths,
    deployment,
    &saved.value,
    &saved.revision,
  )?;
  let selected = compare_selected(paths, &saved.value, &saved.revision, &root)?;
  ensure!(
    selected.as_deref() == Some(Path::new(expected)),
    "selected EOS skill handoff changed; Apply again"
  );
  Ok(())
}

pub fn scrub_inherited(command: &mut std::process::Command) {
  command.env_remove(ENV);
}

#[cfg(test)]
pub(crate) mod tests {
  use super::*;
  use serde_json::json;
  use std::os::unix::{
    ffi::OsStrExt,
    fs::{PermissionsExt, symlink},
  };

  fn fifo(path: &Path) {
    let name = CString::new(path.as_os_str().as_bytes()).unwrap();
    assert_eq!(unsafe { libc::mkfifo(name.as_ptr(), 0o600) }, 0);
  }

  pub(crate) fn graph(list: Value) -> Value {
    json!({"version":"0.1.0","robot":{},"cores":{"isos":{"layers":{"process":{"groups":{"component":{"nodes":{
        "eos":{"id":"eos","node_type":"isos.process.component@v0.1","settings":{"role":"eos","skill_libraries":list}}
    }}}}}}}})
  }

  pub(crate) fn pin(path: &str) -> Value {
    json!({"package":{"id":"test/verified","version":"0.1.1"},"library":path})
  }

  fn fixture_kind(kind: &str) -> (tempfile::TempDir, Paths, PathBuf) {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let source = temp.path().join("bundle");
    fs::create_dir_all(source.join("lib/island/skills")).unwrap();
    fs::write(
      source.join("lib/island/skills/example.skill.so"),
      b"native-skill-fixture",
    )
    .unwrap();
    fs::set_permissions(
      source.join("lib/island/skills/example.skill.so"),
      fs::Permissions::from_mode(0o755),
    )
    .unwrap();
    fs::write(source.join("lib/island/skills/helper.txt"), b"fixture").unwrap();
    fs::set_permissions(
      source.join("lib/island/skills/helper.txt"),
      fs::Permissions::from_mode(0o755),
    )
    .unwrap();
    let platform = crate::domain::registry::release::Platform::current();
    let mut release = json!({"contract":"isos.package@v1","schema_version":"0.1.0","id":"test/verified","kind":kind,"version":"0.1.1",
            "node_contributions":{},
            "source_commit":"a".repeat(40),"assets":[{"os":platform.os,"arch":platform.arch,
            "url":"https://github.com/test/verified/releases/download/v0.1.1/bundle.tar.gz",
            "size_bytes":1,"sha256":"b".repeat(64)}]});
    if kind == "backend" {
      release["adapters"] = json!([{"family":"test-adapter","library":"lib/island/skills/example.skill.so",
                "sha256":hex::encode(Sha256::digest(b"native-skill-fixture"))}]);
    }
    fs::write(
      source.join("release.json"),
      serde_json::to_vec(&release).unwrap(),
    )
    .unwrap();
    fs::set_permissions(
      source.join("release.json"),
      fs::Permissions::from_mode(0o644),
    )
    .unwrap();
    packages::link_with_manifest(&paths, "test/verified", "0.1.1", &source, None).unwrap();
    (temp, paths, source)
  }

  pub(crate) fn fixture() -> (tempfile::TempDir, Paths, PathBuf) {
    fixture_kind("skill")
  }

  #[test]
  fn published_setting_descriptor_matches_frozen_catalog_fixture() {
    let frozen: Value = serde_json::from_slice(include_bytes!(
      "../../../tests/fixtures/skill_selection/catalog-setting.example.json"
    ))
    .unwrap();
    assert_eq!(setting_descriptor(), frozen);
  }

  #[test]
  fn exact_graph_shape_and_json_pointers_are_admitted_before_process_parser() {
    let mut value = graph(json!([pin("lib/island/skills/example.skill.so")]));
    assert_eq!(entries(&value).unwrap().len(), 1);
    value["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["eos"]["settings"]
      ["skill_libraries"] = json!([
      pin("lib/island/skills/example.skill.so"),
      pin("lib/island/skills/example.skill.so")
    ]);
    let error = entries(&value).unwrap_err();
    assert_eq!(
      (error.code, error.path.as_str()),
      (
        "skill_selection_invalid",
        "/cores/isos/layers/process/groups/component/nodes/eos/settings/skill_libraries/1"
      )
    );
    value["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["eos"]["settings"]
      ["skill_libraries"] = json!([pin("../escape.skill.so")]);
    let error = entries(&value).unwrap_err();
    assert_eq!(
      (error.code, error.path.as_str()),
      (
        "skill_library_unsafe",
        "/cores/isos/layers/process/groups/component/nodes/eos/settings/skill_libraries/0/library"
      )
    );
    value["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["eos"]["settings"]
      ["skill_libraries"] = json!([{"package":{"id":"test/verified","version":"0.1.1"},"library":"x.skill.so","extra":1}]);
    assert_eq!(
      entries(&value).unwrap_err().path,
      "/cores/isos/layers/process/groups/component/nodes/eos/settings/skill_libraries/0"
    );
    value["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["bridge"] = json!({"id":"bridge","node_type":"isos.process.component@v0.1","settings":{"role":"eos","skill_libraries":[]}});
    assert_eq!(
      entries(&value).unwrap_err().path,
      "/cores/isos/layers/process/groups/component/nodes/bridge/settings/skill_libraries"
    );
  }

  #[test]
  fn selected_pin_limits_reject_p01_valid_release_before_eos_probe() {
    let (_temp, paths, source) = fixture();
    let version = format!("0.1.{}", "a".repeat(61));
    assert_eq!(version.len(), 65);
    let manifest = source.join("release.json");
    let mut release: Value = serde_json::from_slice(&fs::read(&manifest).unwrap()).unwrap();
    release["version"] = json!("1");
    fs::write(&manifest, serde_json::to_vec(&release).unwrap()).unwrap();
    assert!(packages::link_with_manifest(&paths, "test/verified", "1", &source, None).is_err());
    release["version"] = json!(version);
    fs::write(&manifest, serde_json::to_vec(&release).unwrap()).unwrap();
    packages::link_with_manifest(&paths, "test/verified", &version, &source, None).unwrap();
    packages::verify(&paths, "test/verified", &version).unwrap();

    let mut item = pin("lib/island/skills/example.skill.so");
    item["package"]["version"] = json!(version);
    let error = entries(&graph(json!([item]))).unwrap_err();
    assert_eq!(error.code, "skill_selection_invalid");
    assert_eq!(
      error.path,
      "/cores/isos/layers/process/groups/component/nodes/eos/settings/skill_libraries/0/package"
    );

    let long_id = format!("{}/verified", "a".repeat(88));
    assert_eq!(long_id.len(), 97);
    release["id"] = json!(long_id);
    release["version"] = json!("0.1.1");
    fs::write(&manifest, serde_json::to_vec(&release).unwrap()).unwrap();
    packages::link_with_manifest(&paths, &long_id, "0.1.1", &source, None).unwrap();
    packages::verify(&paths, &long_id, "0.1.1").unwrap();
    let mut item = pin("lib/island/skills/example.skill.so");
    item["package"]["id"] = json!(long_id);
    assert_eq!(
      entries(&graph(json!([item]))).unwrap_err().code,
      "skill_selection_invalid"
    );
  }

  #[test]
  fn count_owner_and_reserved_env_fail_at_exact_pointers() {
    let mut value = graph(json!(vec![pin("x.skill.so"); 17]));
    assert_eq!(
      entries(&value).unwrap_err().path,
      "/cores/isos/layers/process/groups/component/nodes/eos/settings/skill_libraries"
    );
    value = graph(json!([]));
    value["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["eos"]["settings"]
      ["env"] = json!({ENV:"/tmp/forged"});
    assert_eq!(
      check_reserved(&value).unwrap_err().path,
      "/cores/isos/layers/process/groups/component/nodes/eos/settings/env/EOS_SKILL_SELECTION_FILE"
    );
    value = graph(json!([]));
    value["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["eos"]["settings"]
      ["args"] = json!(["--skill-selection-file=/tmp/forged"]);
    assert_eq!(
      check_reserved(&value).unwrap_err().path,
      "/cores/isos/layers/process/groups/component/nodes/eos/settings/args/0"
    );
  }

  #[test]
  fn pinned_selection_is_sorted_and_source_mutation_fails_before_replacement() {
    let (_temp, paths, source) = fixture();
    let value = graph(json!([pin("lib/island/skills/example.skill.so")]));
    let revision = "a".repeat(64);
    let bytes = render(&paths, &value, &revision).unwrap().unwrap();
    let selected: Selection = serde_json::from_slice(&bytes).unwrap();
    assert_eq!(selected.active_revision, revision);
    assert_eq!(selected.libraries.len(), 1);
    assert_eq!(
      selected.libraries[0].library_sha256,
      hex::encode(Sha256::digest(b"native-skill-fixture"))
    );
    assert!(
      selected.libraries[0]
        .package_root
        .starts_with(&paths.data.to_string_lossy().to_string())
    );
    fs::write(
      source.join("lib/island/skills/example.skill.so"),
      b"changed",
    )
    .unwrap();
    assert_eq!(
      render(&paths, &value, &revision).unwrap_err().code,
      "package_changed"
    );
  }

  #[test]
  fn selected_library_symlink_and_wrong_release_kind_fail_closed() {
    let (_temp, paths, source) = fixture();
    let value = graph(json!([pin("lib/island/skills/example.skill.so")]));
    let file = source.join("lib/island/skills/example.skill.so");
    fs::rename(&file, source.join("original.skill.so")).unwrap();
    symlink(source.join("original.skill.so"), &file).unwrap();
    assert_eq!(
      render(&paths, &value, &"a".repeat(64)).unwrap_err().code,
      "package_unsafe_path"
    );
    let (_temp, paths, _source) = fixture_kind("backend");
    assert_eq!(
      render(&paths, &value, &"a".repeat(64)).unwrap_err().code,
      "skill_package_incompatible"
    );
  }

  #[test]
  fn group_writable_selected_and_unrelated_payload_files_fail_preflight() {
    for relative in [
      "lib/island/skills/example.skill.so",
      "lib/island/skills/helper.txt",
    ] {
      let (_temp, paths, source) = fixture();
      let value = graph(json!([pin("lib/island/skills/example.skill.so")]));
      render(&paths, &value, &"a".repeat(64)).unwrap();
      fs::set_permissions(source.join(relative), fs::Permissions::from_mode(0o775)).unwrap();
      let error = render(&paths, &value, &"a".repeat(64)).unwrap_err();
      assert_eq!(error.code, "skill_library_unsafe");
      assert_eq!(
        error.path,
        format!(
          "/cores/isos/layers/process/groups/component/nodes/eos/settings/skill_libraries/0/{}",
          if relative.ends_with(".skill.so") {
            "library"
          } else {
            "package"
          }
        )
      );
    }
  }

  #[test]
  fn group_writable_receipt_and_release_fail_preflight() {
    for name in ["receipt.json", "release.json"] {
      let (_temp, paths, _source) = fixture();
      let value = graph(json!([pin("lib/island/skills/example.skill.so")]));
      render(&paths, &value, &"a".repeat(64)).unwrap();
      let metadata = paths.data.join("packages/test/verified/0.1.1").join(name);
      fs::set_permissions(&metadata, fs::Permissions::from_mode(0o660)).unwrap();
      let error = render(&paths, &value, &"a".repeat(64)).unwrap_err();
      assert_eq!(error.code, "skill_library_unsafe");
      assert!(error.path.ends_with("/skill_libraries/0/package"));
    }
  }

  #[test]
  fn replaced_library_fifo_is_rejected_without_blocking() {
    let temp = tempfile::tempdir().unwrap();
    let directory = File::open(temp.path()).unwrap();
    fifo(&temp.path().join("swapped.skill.so"));
    let name = CString::new("swapped.skill.so").unwrap();
    let started = std::time::Instant::now();
    let error = open_selected(&directory, &name, "/selection/0").unwrap_err();
    assert_eq!(error.code, "skill_library_unsafe");
    assert!(started.elapsed() < std::time::Duration::from_secs(1));
  }

  #[test]
  fn selected_manifest_replacement_never_follows_symlink_or_blocks_on_fifo() {
    let temp = tempfile::tempdir().unwrap();
    let directory = temp.path().join("isos");
    fs::create_dir(&directory).unwrap();
    let selected = directory.join("skill-selection.json");
    fs::write(&selected, b"abc").unwrap();
    assert_eq!(read_selected_bytes(temp.path(), 3).unwrap(), b"abc");
    fs::remove_file(&selected).unwrap();
    symlink(temp.path().join("outside.json"), &selected).unwrap();
    assert_eq!(
      read_selected_bytes(temp.path(), 3).unwrap_err().code,
      "generated_manifest_mismatch"
    );
    fs::remove_file(&selected).unwrap();
    fifo(&selected);
    let started = std::time::Instant::now();
    assert_eq!(
      read_selected_bytes(temp.path(), 3).unwrap_err().code,
      "generated_manifest_mismatch"
    );
    assert!(started.elapsed() < std::time::Duration::from_secs(1));
  }

  #[test]
  fn selected_manifest_over_eos_reader_limit_fails_before_publication() {
    let item = SelectedLibrary {
      package_id: "test/verified".into(),
      package_version: "0.1.1".into(),
      package_root: "/tmp/package".into(),
      payload_root: format!("/tmp/{}", "a/".repeat(1800)),
      receipt_sha256: "a".repeat(64),
      release_sha256: "b".repeat(64),
      payload_sha256: "c".repeat(64),
      library: "lib/island/skills/example.skill.so".into(),
      library_sha256: "d".repeat(64),
    };
    assert!(encode_selection(&"a".repeat(64), vec![item.clone()]).is_ok());
    let error = encode_selection(&"a".repeat(64), vec![item; 16]).unwrap_err();
    assert_eq!(
      (error.code, error.path.as_str()),
      (
        "skill_selection_invalid",
        "/cores/isos/layers/process/groups/component/nodes/eos/settings/skill_libraries"
      )
    );
  }

  #[test]
  fn private_v2_generation_lists_selection_and_rejects_mutation() {
    let (_temp, paths, _source) = fixture();
    let value = graph(json!([pin("lib/island/skills/example.skill.so")]));
    let bytes = serde_json::to_vec_pretty(&value).unwrap();
    let revision = crate::domain::graph::graph_compat::revision(&bytes);
    crate::app::config::atomic_write(&paths.node_graph_path("test"), &bytes).unwrap();
    let owner = vec![
      (PathBuf::from("eos/graph.json"), b"projection".to_vec()),
      (PathBuf::from("launch-inputs.json"), b"[]".to_vec()),
    ];
    let root =
      crate::domain::graph::generated_v2::publish(&paths, "test", &value, &revision, &owner)
        .unwrap();
    let manifest: Value =
      serde_json::from_slice(&fs::read(root.join("manifest.json")).unwrap()).unwrap();
    assert_eq!(
      manifest["files"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|file| file["path"] == RELATIVE_FILE)
        .count(),
      1
    );
    assert_eq!(manifest["packages"].as_array().unwrap().len(), 1);
    assert_eq!(
      fs::metadata(root.join(RELATIVE_FILE))
        .unwrap()
        .permissions()
        .mode()
        & 0o777,
      0o600
    );
    assert_eq!(
      crate::domain::graph::generated_v2::selected_v2_root(&paths, "test", &value, &revision)
        .unwrap(),
      root
    );
    let mut specs = vec![crate::runtime::process::ProcessSpec {
      id: "eos".into(),
      path: "isos/process/component/eos".into(),
      settings: serde_json::from_value(json!({"command":["/bin/true"],"role":"eos"})).unwrap(),
      package: None,
      fingerprint: None,
    }];
    apply_env(&mut specs, Some(&root.join(RELATIVE_FILE)));
    verify_child_handoff(&paths, "test", &specs[0]).unwrap();
    specs[0].settings.env.remove(ENV);
    assert!(
      verify_child_handoff(&paths, "test", &specs[0])
        .unwrap_err()
        .to_string()
        .contains("handoff is missing")
    );
    apply_env(&mut specs, Some(&root.join(RELATIVE_FILE)));
    fs::write(root.join(RELATIVE_FILE), b"{}").unwrap();
    assert!(verify_child_handoff(&paths, "test", &specs[0]).is_err());
    assert_eq!(
      crate::domain::graph::generated_v2::selected_v2_root(&paths, "test", &value, &revision)
        .unwrap_err()
        .code,
      "generated_manifest_mismatch"
    );
  }

  #[test]
  fn eos_is_only_child_receiving_selection_even_with_inherited_value() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    let script = temp.path().join("record.sh");
    fs::write(
      &script,
      b"#!/bin/sh\nprintf '%s' \"${EOS_SKILL_SELECTION_FILE-unset}\" > \"$1\"\nsleep 30\n",
    )
    .unwrap();
    fs::set_permissions(&script, fs::Permissions::from_mode(0o700)).unwrap();
    let make = |id: &str| crate::runtime::process::ProcessSpec {
      id: id.into(),
      path: format!("isos/process/component/{id}"),
      settings: serde_json::from_value(
        json!({"command":[script,temp.path().join(format!("{id}.txt"))],"role":"eos"}),
      )
      .unwrap(),
      package: None,
      fingerprint: None,
    };
    let mut specs = vec![make("eos"), make("bridge")];
    let selection = temp.path().join("skill-selection.json");
    apply_env(&mut specs, Some(&selection));
    for spec in &mut specs {
      spec.settings.role = None;
    }
    unsafe { std::env::set_var(ENV, "/tmp/inherited-forgery") };
    let mut supervisor = crate::runtime::process::Supervisor::new(&paths, "test").unwrap();
    supervisor.start(&specs).unwrap();
    let wait = |id: &str| {
      let path = temp.path().join(format!("{id}.txt"));
      for _ in 0..50 {
        if let Ok(value) = fs::read_to_string(&path) {
          return value;
        }
        std::thread::sleep(std::time::Duration::from_millis(20));
      }
      panic!("child did not write env record");
    };
    assert_eq!(wait("eos"), selection.to_string_lossy());
    assert_eq!(wait("bridge"), "unset");
    supervisor.stop();
    unsafe { std::env::remove_var(ENV) };
  }
}
