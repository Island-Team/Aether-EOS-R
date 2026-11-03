//! Selected native skill catalog. Package bytes are data until a confined worker
//! has admitted every descriptor; this module never loads package code.

use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::os::fd::{AsRawFd, FromRawFd};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt};
use std::path::{Component, Path, PathBuf};
use std::time::{Duration, Instant};

use aether_common::events::valid_digest;
use aether_hal::PRIMITIVE_SPECS;
use goblin::elf::dynamic;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use thiserror::Error;

use crate::managed_admission::ManagedAdmission;

mod worker;
pub use worker::{WorkerCancelHandle, WorkerEvent, WorkerSession, worker_main};

const MAX_LIBRARIES: usize = 16;
const MAX_LIBRARY_BYTES: u64 = 64 * 1024 * 1024;
const MAX_TREE_ENTRIES: usize = 16_384;
const MAX_TREE_BYTES: u64 = 4_000_000_000;
const TRUSTED_SONAMES: &[&str] = &[
  "libc.so.6",
  "libm.so.6",
  "libpthread.so.0",
  "libdl.so.2",
  "libgcc_s.so.1",
  "libstdc++.so.6",
];

#[derive(Debug, Error)]
#[error("host skill admission failed: {code}")]
pub struct HostError {
  pub code: &'static str,
}

/// Carries child-reap evidence across a post-spawn invocation failure.
#[derive(Debug, Error)]
#[error("host skill worker start failed: {code}")]
pub struct WorkerStartError {
  pub code: &'static str,
  /// None means no worker was spawned; false leaves the host stop unconfirmed.
  pub worker_reaped: Option<bool>,
}

impl From<HostError> for WorkerStartError {
  fn from(error: HostError) -> Self {
    Self {
      code: error.code,
      worker_reaped: (error.code == "worker_reap_failed").then_some(false),
    }
  }
}

fn fail(code: &'static str) -> HostError {
  HostError { code }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Selection {
  schema_version: u32,
  active_revision: String,
  libraries: Vec<SelectedLibrary>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct SelectedLibrary {
  package_id: String,
  package_version: String,
  package_root: PathBuf,
  payload_root: PathBuf,
  receipt_sha256: String,
  release_sha256: String,
  payload_sha256: String,
  library: String,
  library_sha256: String,
}

/// Descriptor data copied from a worker; pointers and package paths never cross
/// this public catalog boundary.
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct SkillDescriptor {
  pub name: String,
  pub version: String,
  /// Safe pin from the parent-verified selection, never worker-authored.
  pub package_id: String,
  pub package_version: String,
  pub description: String,
  pub parameters_schema: serde_json::Value,
  pub returns_schema: serde_json::Value,
  pub primitives: Vec<PrimitiveReference>,
  pub reentrant: bool,
  pub(crate) library_index: usize,
  pub(crate) descriptor_index: usize,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct PrimitiveReference {
  pub name: String,
  pub version: String,
}

pub(crate) fn valid_abi_name(value: &str) -> bool {
  let bytes = value.as_bytes();
  !bytes.is_empty()
    && bytes.len() <= 4096
    && bytes[0].is_ascii_lowercase()
    && bytes.windows(2).all(|pair| {
      !matches!(pair[0], b'.' | b'_' | b'-')
        || pair[1].is_ascii_lowercase()
        || pair[1].is_ascii_digit()
    })
    && !matches!(bytes.last(), Some(b'.' | b'_' | b'-'))
    && bytes.iter().all(|byte| {
      byte.is_ascii_lowercase() || byte.is_ascii_digit() || matches!(byte, b'.' | b'_' | b'-')
    })
}

pub(crate) fn valid_abi_version(value: &str) -> bool {
  value.len() <= 4096 && semver::Version::parse(value).is_ok()
}

fn has_external_reference(value: &serde_json::Value) -> bool {
  match value {
    serde_json::Value::Object(map) => map.iter().any(|(key, value)| {
      (matches!(key.as_str(), "$ref" | "$dynamicRef" | "$recursiveRef")
        && value
          .as_str()
          .is_none_or(|reference| !reference.starts_with('#')))
        || has_external_reference(value)
    }),
    serde_json::Value::Array(values) => values.iter().any(has_external_reference),
    _ => false,
  }
}

pub(crate) fn validate_schema(schema: &serde_json::Value) -> Result<(), HostError> {
  if schema["type"].as_str() != Some("object")
    || serde_json::to_vec(schema).map_or(true, |bytes| bytes.len() > 1024 * 1024)
    || has_external_reference(schema)
    || !jsonschema::meta::try_is_valid(schema).unwrap_or(false)
    || jsonschema::validator_for(schema).is_err()
  {
    return Err(fail("abi_schema_invalid"));
  }
  Ok(())
}

fn validate_copied_descriptor(descriptor: &SkillDescriptor) -> Result<(), HostError> {
  if !valid_abi_name(&descriptor.name)
    || !valid_abi_version(&descriptor.version)
    || !descriptor.package_id.is_empty()
    || !descriptor.package_version.is_empty()
    || descriptor.description.len() > 4096
    || descriptor.descriptor_index >= 256
    || descriptor.primitives.len() > 32
  {
    return Err(fail("abi_descriptor_invalid"));
  }
  validate_schema(&descriptor.parameters_schema)?;
  validate_schema(&descriptor.returns_schema)?;
  let mut seen = BTreeSet::new();
  for primitive in &descriptor.primitives {
    if !valid_abi_name(&primitive.name)
      || !valid_abi_version(&primitive.version)
      || !PRIMITIVE_SPECS
        .iter()
        .any(|spec| spec.name == primitive.name && spec.version == primitive.version)
      || !seen.insert((&primitive.name, &primitive.version))
    {
      return Err(fail("abi_primitive_invalid"));
    }
  }
  Ok(())
}

/// The admitted catalog owns staged bytes for subsequent invocation workers.
pub struct Catalog {
  descriptors: Vec<SkillDescriptor>,
  stages: Vec<File>,
  _stage_dir: Option<tempfile::TempDir>,
  preflight: Option<PreflightCatalog>,
}

#[derive(Clone, Debug, Serialize)]
pub struct PreflightLibrary {
  package_id: String,
  package_version: String,
  library: String,
  library_sha256: String,
  abi_version: u32,
  pub skill_count: usize,
}

#[derive(Clone, Debug, Serialize)]
pub struct PreflightCatalog {
  pub active_revision: String,
  pub generation: String,
  pub selection_sha256: String,
  pub libraries: Vec<PreflightLibrary>,
}

impl Catalog {
  pub fn empty() -> Self {
    Self {
      descriptors: Vec::new(),
      stages: Vec::new(),
      _stage_dir: None,
      preflight: None,
    }
  }

  #[cfg(test)]
  pub(crate) fn from_descriptors_for_test(descriptors: Vec<SkillDescriptor>) -> Self {
    Self {
      descriptors,
      stages: Vec::new(),
      _stage_dir: None,
      preflight: None,
    }
  }

  pub fn descriptors(&self) -> &[SkillDescriptor] {
    &self.descriptors
  }

  pub fn is_empty(&self) -> bool {
    self.descriptors.is_empty()
  }

  pub fn preflight(&self) -> Option<&PreflightCatalog> {
    self.preflight.as_ref()
  }

  pub fn finish_preflight(mut self) -> Result<PreflightCatalog, HostError> {
    let metadata = self
      .preflight
      .take()
      .ok_or_else(|| fail("selection_invalid"))?;
    self.stages.clear();
    if let Some(directory) = self._stage_dir.take() {
      directory.close().map_err(|_| fail("stage_unavailable"))?;
    }
    Ok(metadata)
  }

  /// The service may only publish descriptors whose every declared callback
  /// has a complete Rust broker route. This is an atomic pre-ready check.
  pub fn validate_broker_coverage(&self) -> Result<(), HostError> {
    if self
      .descriptors
      .iter()
      .flat_map(|skill| &skill.primitives)
      .any(|primitive| {
        !matches!(
          (primitive.name.as_str(), primitive.version.as_str()),
          ("base.velocity", "1.0.0") | ("arm.movej", "1.0.0")
        )
      })
    {
      return Err(fail("broker_primitive_unavailable"));
    }
    Ok(())
  }

  pub fn start_worker(
    &self,
    executable: &Path,
    name: &str,
    version: &str,
    command_id: &str,
    arguments_json: &str,
  ) -> Result<WorkerSession, WorkerStartError> {
    let descriptor = self
      .descriptors
      .iter()
      .find(|descriptor| descriptor.name == name && descriptor.version == version)
      .ok_or_else(|| fail("skill_not_admitted"))?;
    let stage = self
      .stages
      .get(descriptor.library_index)
      .ok_or_else(|| fail("stage_unavailable"))?;
    let mut session = WorkerSession::spawn(executable, stage, worker::WorkerMode::Invoke)?;
    if let Err(error) = session.invoke(descriptor.descriptor_index, command_id, arguments_json) {
      let reaped = session.terminate_reap().unwrap_or(false);
      return Err(WorkerStartError {
        code: error.code,
        worker_reaped: Some(reaped),
      });
    }
    Ok(session)
  }

  /// Admission is atomic: no descriptor is published if any selected library
  /// fails exact file, ELF, worker or ABI checks.
  pub fn admit(
    profile_path: &Path,
    managed: Option<&ManagedAdmission>,
    selection_path: Option<&Path>,
    executable: &Path,
  ) -> Result<Self, HostError> {
    let Some(managed) = managed else {
      return if selection_path.is_none() {
        Ok(Self::empty())
      } else {
        Err(fail("selection_without_managed_generation"))
      };
    };
    let leaf = profile_path
      .parent()
      .and_then(Path::parent)
      .ok_or_else(|| fail("selection_path_invalid"))?;
    if profile_path != leaf.join("eos/graph.json") {
      return Err(fail("selection_path_invalid"));
    }
    let revision = leaf
      .parent()
      .and_then(Path::file_name)
      .and_then(|name| name.to_str())
      .ok_or_else(|| fail("selection_path_invalid"))?;
    let generation = leaf
      .file_name()
      .and_then(|name| name.to_str())
      .ok_or_else(|| fail("selection_path_invalid"))?;
    if !valid_digest(revision)
      || !valid_digest(generation)
      || leaf
        .parent()
        .and_then(Path::parent)
        .and_then(Path::file_name)
        != Some(std::ffi::OsStr::new("v2"))
      || revision != managed.active_revision
    {
      return Err(fail("selection_revision_mismatch"));
    }
    let manifest_bytes = private_file(&leaf.join("manifest.json"), 256 * 1024)
      .map_err(|_| fail("selection_manifest_invalid"))?;
    let manifest: serde_json::Value =
      serde_json::from_slice(&manifest_bytes).map_err(|_| fail("selection_manifest_invalid"))?;
    if manifest["schema_version"].as_u64() != Some(2)
      || manifest["revision"].as_str() != Some(revision)
      || manifest["generation"].as_str() != Some(generation)
    {
      return Err(fail("selection_manifest_invalid"));
    }
    let files = manifest["files"]
      .as_array()
      .ok_or_else(|| fail("selection_manifest_invalid"))?;
    let selected: Vec<_> = files
      .iter()
      .filter(|entry| entry["path"].as_str() == Some("isos/skill-selection.json"))
      .collect();
    let Some(selection_path) = selection_path else {
      return if selected.is_empty() {
        Ok(Self::empty())
      } else {
        Err(fail("selection_handoff_incomplete"))
      };
    };
    if selection_path != leaf.join("isos/skill-selection.json") || selected.len() != 1 {
      return Err(fail("selection_manifest_invalid"));
    }
    let selection_dir =
      fs::symlink_metadata(leaf.join("isos")).map_err(|_| fail("selection_path_invalid"))?;
    if !selection_dir.is_dir()
      || selection_dir.uid() != unsafe { libc::geteuid() }
      || selection_dir.permissions().mode() & 0o777 != 0o700
    {
      return Err(fail("selection_path_invalid"));
    }
    let expected = selected[0]["sha256"]
      .as_str()
      .ok_or_else(|| fail("selection_manifest_invalid"))?;
    let bytes = private_file(selection_path, 64 * 1024).map_err(|_| fail("selection_invalid"))?;
    if !valid_digest(expected) || digest(&bytes) != expected {
      return Err(fail("selection_digest_mismatch"));
    }
    let selection: Selection =
      serde_json::from_slice(&bytes).map_err(|_| fail("selection_invalid"))?;
    if selection.schema_version != 1
      || selection.active_revision != revision
      || selection.libraries.len() > MAX_LIBRARIES
    {
      return Err(fail("selection_invalid"));
    }
    let mut previous = None;
    let stage_dir = tempfile::Builder::new()
      .prefix("eos-skill-stage-")
      .tempdir()
      .map_err(|_| fail("stage_unavailable"))?;
    fs::set_permissions(stage_dir.path(), fs::Permissions::from_mode(0o700))
      .map_err(|_| fail("stage_unavailable"))?;
    let mut stages = Vec::new();
    let mut descriptors = Vec::new();
    let mut identities = BTreeSet::new();
    let mut preflight_libraries = Vec::new();
    for (index, library) in selection.libraries.iter().enumerate() {
      let key = (
        &library.package_id,
        &library.package_version,
        &library.library,
      );
      if previous.as_ref().is_some_and(|last| &key <= last) {
        return Err(fail("selection_order_invalid"));
      }
      previous = Some(key);
      let content = verify_library(library)?;
      validate_elf(&content)?;
      let stage = stage_dir.path().join(format!("{index}.so"));
      let mut opened = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&stage)
        .map_err(|_| fail("stage_unavailable"))?;
      opened
        .write_all(&content)
        .map_err(|_| fail("stage_unavailable"))?;
      opened.sync_all().map_err(|_| fail("stage_unavailable"))?;
      fs::set_permissions(&stage, fs::Permissions::from_mode(0o400))
        .map_err(|_| fail("stage_unavailable"))?;
      let staged = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(&stage)
        .map_err(|_| fail("stage_unavailable"))?;
      stages.push(staged);
    }
    // Complete every data-only rejection before the first worker can run a
    // library constructor or query function.
    for (index, staged) in stages.iter().enumerate() {
      let mut found = inspect_worker(executable, staged, Duration::from_secs(5))?;
      preflight_libraries.push(PreflightLibrary {
        package_id: selection.libraries[index].package_id.clone(),
        package_version: selection.libraries[index].package_version.clone(),
        library: selection.libraries[index].library.clone(),
        library_sha256: selection.libraries[index].library_sha256.clone(),
        abi_version: 2,
        skill_count: found.len(),
      });
      for descriptor in &mut found {
        descriptor.library_index = index;
        descriptor
          .package_id
          .clone_from(&selection.libraries[index].package_id);
        descriptor
          .package_version
          .clone_from(&selection.libraries[index].package_version);
        if !identities.insert((descriptor.name.clone(), descriptor.version.clone())) {
          return Err(fail("catalog_duplicate_identity"));
        }
        descriptors.push(descriptor.clone());
        if descriptors.len() > 256 {
          return Err(fail("catalog_capacity"));
        }
      }
    }
    descriptors.sort_by(|a, b| (&a.name, &a.version).cmp(&(&b.name, &b.version)));
    Ok(Self {
      descriptors,
      stages,
      _stage_dir: Some(stage_dir),
      preflight: Some(PreflightCatalog {
        active_revision: revision.into(),
        generation: generation.into(),
        selection_sha256: digest(&bytes),
        libraries: preflight_libraries,
      }),
    })
  }
}

fn digest(bytes: &[u8]) -> String {
  format!("{:x}", Sha256::digest(bytes))
}

fn private_file(path: &Path, max: usize) -> Result<Vec<u8>, HostError> {
  let file = OpenOptions::new()
    .read(true)
    .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC | libc::O_NONBLOCK)
    .open(path)
    .map_err(|_| fail("private_file_unavailable"))?;
  let metadata = file
    .metadata()
    .map_err(|_| fail("private_file_unavailable"))?;
  if !metadata.is_file()
    || metadata.uid() != unsafe { libc::geteuid() }
    || metadata.permissions().mode() & 0o777 != 0o600
    || metadata.len() > max as u64
  {
    return Err(fail("private_file_unsafe"));
  }
  let mut bytes = Vec::new();
  file
    .take((max + 1) as u64)
    .read_to_end(&mut bytes)
    .map_err(|_| fail("private_file_unavailable"))?;
  if bytes.len() > max {
    return Err(fail("private_file_oversize"));
  }
  Ok(bytes)
}

fn checked_root(path: &Path) -> Result<(), HostError> {
  if !path.is_absolute()
    || !path
      .components()
      .all(|part| matches!(part, Component::RootDir | Component::Normal(_)))
  {
    return Err(fail("package_path_unsafe"));
  }
  for ancestor in path.ancestors() {
    if fs::symlink_metadata(ancestor)
      .map_err(|_| fail("package_path_unsafe"))?
      .file_type()
      .is_symlink()
    {
      return Err(fail("package_path_unsafe"));
    }
  }
  if !path.is_dir() {
    return Err(fail("package_path_unsafe"));
  }
  Ok(())
}

fn open_package_file(
  root: &Path,
  relative: &str,
  max: u64,
) -> Result<(File, fs::Metadata), HostError> {
  let parts: Vec<_> = relative.split('/').collect();
  if parts.is_empty()
    || parts
      .iter()
      .any(|part| part.is_empty() || *part == "." || *part == ".." || part.contains('\\'))
    || relative.starts_with('/')
  {
    return Err(fail("package_path_unsafe"));
  }
  let mut directory = OpenOptions::new()
    .read(true)
    .custom_flags(libc::O_DIRECTORY | libc::O_NOFOLLOW)
    .open(root)
    .map_err(|_| fail("package_path_unsafe"))?;
  for part in &parts[..parts.len() - 1] {
    let part = std::ffi::CString::new(*part).map_err(|_| fail("package_path_unsafe"))?;
    let fd = unsafe {
      libc::openat(
        directory.as_raw_fd(),
        part.as_ptr(),
        libc::O_RDONLY | libc::O_CLOEXEC | libc::O_NOFOLLOW | libc::O_DIRECTORY,
      )
    };
    if fd < 0 {
      return Err(fail("package_path_unsafe"));
    }
    directory = unsafe { File::from_raw_fd(fd) };
  }
  let part =
    std::ffi::CString::new(*parts.last().unwrap()).map_err(|_| fail("package_path_unsafe"))?;
  let fd = unsafe {
    libc::openat(
      directory.as_raw_fd(),
      part.as_ptr(),
      libc::O_RDONLY | libc::O_CLOEXEC | libc::O_NOFOLLOW | libc::O_NONBLOCK,
    )
  };
  if fd < 0 {
    return Err(fail("package_path_unsafe"));
  }
  let file = unsafe { File::from_raw_fd(fd) };
  let before = file.metadata().map_err(|_| fail("package_file_changed"))?;
  if !before.is_file()
    || before.uid() != unsafe { libc::geteuid() }
    || before.permissions().mode() & 0o022 != 0
    || before.len() > max
  {
    return Err(fail("package_file_unsafe"));
  }
  Ok((file, before))
}

fn unchanged_file(
  root: &Path,
  relative: &str,
  before: &fs::Metadata,
  read_len: u64,
) -> Result<(), HostError> {
  let after =
    fs::symlink_metadata(root.join(relative)).map_err(|_| fail("package_file_changed"))?;
  if read_len != before.len()
    || (
      before.dev(),
      before.ino(),
      before.len(),
      before.mtime(),
      before.mtime_nsec(),
    ) != (
      after.dev(),
      after.ino(),
      after.len(),
      after.mtime(),
      after.mtime_nsec(),
    )
  {
    return Err(fail("package_file_changed"));
  }
  Ok(())
}

fn package_file(root: &Path, relative: &str, max: u64) -> Result<Vec<u8>, HostError> {
  let (file, before) = open_package_file(root, relative, max)?;
  let mut bytes = Vec::new();
  file
    .take(max + 1)
    .read_to_end(&mut bytes)
    .map_err(|_| fail("package_file_changed"))?;
  unchanged_file(root, relative, &before, bytes.len() as u64)?;
  Ok(bytes)
}

fn payload_digest(root: &Path, installed: bool) -> Result<String, HostError> {
  let mut entries = BTreeMap::new();
  let mut pending = vec![root.to_path_buf()];
  while let Some(dir) = pending.pop() {
    for entry in fs::read_dir(&dir).map_err(|_| fail("package_tree_unsafe"))? {
      let entry = entry.map_err(|_| fail("package_tree_unsafe"))?;
      let path = entry.path();
      let relative = path
        .strip_prefix(root)
        .map_err(|_| fail("package_tree_unsafe"))?;
      let name = relative
        .to_str()
        .ok_or_else(|| fail("package_tree_unsafe"))?;
      if installed
        && !name.contains('/')
        && matches!(name, "receipt.json" | "release.json" | "node-types.json")
      {
        continue;
      }
      if name.len() > 4096 || entries.len() >= MAX_TREE_ENTRIES {
        return Err(fail("package_tree_capacity"));
      }
      let metadata = fs::symlink_metadata(&path).map_err(|_| fail("package_tree_unsafe"))?;
      if !metadata.is_dir() && !metadata.is_file() {
        return Err(fail("package_tree_unsafe"));
      }
      if metadata.is_dir() {
        pending.push(path.clone());
      }
      entries.insert(name.to_owned(), (path, metadata));
    }
  }
  let mut tree = Sha256::new();
  tree.update(b"isos-package-tree-v1\0");
  let mut total = 0u64;
  for (name, (path, before)) in entries {
    tree.update((name.len() as u32).to_le_bytes());
    tree.update(name.as_bytes());
    let file = before.is_file();
    tree.update([if file { b'F' } else { b'D' }]);
    tree.update([if file && before.permissions().mode() & 0o111 != 0 {
      b'1'
    } else {
      b'0'
    }]);
    tree.update((if file { before.len() } else { 0 }).to_le_bytes());
    if file {
      total = total
        .checked_add(before.len())
        .ok_or_else(|| fail("package_tree_capacity"))?;
      if total > MAX_TREE_BYTES {
        return Err(fail("package_tree_capacity"));
      }
      let relative = path
        .strip_prefix(root)
        .map_err(|_| fail("package_tree_unsafe"))?
        .to_str()
        .ok_or_else(|| fail("package_tree_unsafe"))?;
      let (mut input, opened) = open_package_file(root, relative, MAX_TREE_BYTES)?;
      if (opened.dev(), opened.ino(), opened.len()) != (before.dev(), before.ino(), before.len()) {
        return Err(fail("package_tree_changed"));
      }
      let mut content = Sha256::new();
      let mut count = 0u64;
      let mut buffer = [0u8; 1024 * 1024];
      loop {
        let read = input
          .read(&mut buffer)
          .map_err(|_| fail("package_tree_changed"))?;
        if read == 0 {
          break;
        }
        count += read as u64;
        if count > before.len() {
          return Err(fail("package_tree_changed"));
        }
        content.update(&buffer[..read]);
      }
      unchanged_file(root, relative, &before, count)?;
      tree.update(content.finalize());
    } else {
      tree.update([0_u8; 32]);
    }
    let after = fs::symlink_metadata(&path).map_err(|_| fail("package_tree_unsafe"))?;
    if (
      before.dev(),
      before.ino(),
      before.len(),
      before.mtime(),
      before.mtime_nsec(),
    ) != (
      after.dev(),
      after.ino(),
      after.len(),
      after.mtime(),
      after.mtime_nsec(),
    ) {
      return Err(fail("package_tree_changed"));
    }
  }
  Ok(format!("{:x}", tree.finalize()))
}

fn verify_library(library: &SelectedLibrary) -> Result<Vec<u8>, HostError> {
  if library.library.is_empty()
    || library.library.len() > 256
    || !library.library.ends_with(".skill.so")
  {
    return Err(fail("selection_invalid"));
  }
  if !valid_selected_pin(&library.package_id, &library.package_version)
    || ![
      &library.receipt_sha256,
      &library.release_sha256,
      &library.payload_sha256,
      &library.library_sha256,
    ]
    .iter()
    .all(|value| valid_digest(value))
  {
    return Err(fail("selection_invalid"));
  }
  checked_root(&library.package_root)?;
  checked_root(&library.payload_root)?;
  let receipt = package_file(&library.package_root, "receipt.json", 65_536)?;
  if digest(&receipt) != library.receipt_sha256 {
    return Err(fail("receipt_changed"));
  }
  let metadata: serde_json::Value =
    serde_json::from_slice(&receipt).map_err(|_| fail("receipt_invalid"))?;
  let source_kind = metadata["source_kind"].as_str();
  if metadata["schema_version"].as_u64() != Some(2)
    || metadata["id"].as_str() != Some(&library.package_id)
    || metadata["version"].as_str() != Some(&library.package_version)
    || metadata["release_sha256"].as_str() != Some(&library.release_sha256)
    || metadata["payload_sha256"].as_str() != Some(&library.payload_sha256)
    || !matches!(
      source_kind,
      Some("approved_catalog" | "local_candidate" | "local_link")
    )
  {
    return Err(fail("receipt_invalid"));
  }
  let release = package_file(&library.package_root, "release.json", 4_194_304)?;
  if digest(&release) != library.release_sha256 {
    return Err(fail("release_changed"));
  }
  let release_value: serde_json::Value =
    serde_json::from_slice(&release).map_err(|_| fail("release_invalid"))?;
  if release_value["id"].as_str() != Some(&library.package_id)
    || release_value["version"].as_str() != Some(&library.package_version)
    || release_value["kind"].as_str() != Some("skill")
    || release_value["schema_version"].as_u64() != Some(4)
    || release_value["source_commit"]
      .as_str()
      .is_none_or(|sha| sha.len() != 40 || !sha.bytes().all(|byte| byte.is_ascii_hexdigit()))
    || release_value["source_commit"] != metadata["source_commit"]
  {
    return Err(fail("release_invalid"));
  }
  let linked = metadata["local_link"].as_str();
  match linked {
    Some(link)
      if source_kind == Some("local_link")
        && Path::new(link) == library.payload_root
        && metadata["asset_sha256"].is_null() => {}
    None if source_kind != Some("local_link") && library.payload_root == library.package_root => {
      let selected_asset = selected_asset(&release_value)?;
      let sha = selected_asset["sha256"]
        .as_str()
        .ok_or_else(|| fail("release_asset_invalid"))?;
      let sha = sha
        .strip_prefix("sha256:")
        .unwrap_or(sha)
        .to_ascii_lowercase();
      if metadata["asset_sha256"].as_str() != Some(sha.as_str()) {
        return Err(fail("release_asset_mismatch"));
      }
    }
    _ => return Err(fail("receipt_invalid")),
  }
  let installed = library.payload_root == library.package_root;
  if payload_digest(&library.payload_root, installed)? != library.payload_sha256 {
    return Err(fail("payload_changed"));
  }
  let bytes = package_file(&library.payload_root, &library.library, MAX_LIBRARY_BYTES)?;
  if digest(&bytes) != library.library_sha256 {
    return Err(fail("library_changed"));
  }
  Ok(bytes)
}

fn valid_selected_pin(id: &str, version: &str) -> bool {
  let slug = |text: &str| {
    !text.is_empty()
      && text
        .bytes()
        .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-')
  };
  id.len() <= 96
    && id.split_once('/').is_some_and(|(a, b)| slug(a) && slug(b))
    && version.len() <= 64
    && version.split('.').count() >= 3
    && version
      .bytes()
      .next()
      .is_some_and(|byte| byte.is_ascii_digit())
    && version
      .bytes()
      .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'+'))
}

fn selected_asset(release: &serde_json::Value) -> Result<&serde_json::Value, HostError> {
  let assets = release["assets"]
    .as_array()
    .ok_or_else(|| fail("release_asset_invalid"))?;
  if assets.is_empty() {
    return Err(fail("release_asset_invalid"));
  }
  let gpu = std::env::var("ISOS_GPU").ok();
  let ros = std::env::var("ISOS_ROS").ok();
  let libc_version = unsafe {
    let pointer = libc::gnu_get_libc_version();
    if pointer.is_null() {
      String::new()
    } else {
      std::ffi::CStr::from_ptr(pointer)
        .to_string_lossy()
        .into_owned()
    }
  };
  assets
    .iter()
    .find(|asset| {
      asset["os"].as_str() == Some(std::env::consts::OS)
        && asset["arch"].as_str() == Some(std::env::consts::ARCH)
        && asset["gpu"]
          .as_str()
          .is_none_or(|expected| gpu.as_deref() == Some(expected))
        && asset["ros"]
          .as_str()
          .is_none_or(|expected| ros.as_deref() == Some(expected))
        && {
          let numbers = |text: &str| -> Option<Vec<u32>> {
            text.split('.').map(|part| part.parse().ok()).collect()
          };
          let need = asset["libc"]
            .as_str()
            .and_then(|value| value.strip_prefix("glibc>="))
            .and_then(numbers)
            .or_else(|| asset["glibc_min"].as_str().and_then(numbers));
          need.is_none_or(|need| numbers(&libc_version).is_some_and(|have| have >= need))
        }
    })
    .ok_or_else(|| fail("release_asset_invalid"))
}

fn validate_elf(bytes: &[u8]) -> Result<(), HostError> {
  let object = goblin::Object::parse(bytes).map_err(|_| fail("library_not_elf"))?;
  let goblin::Object::Elf(elf) = object else {
    return Err(fail("library_not_elf"));
  };
  if elf.header.e_type != goblin::elf::header::ET_DYN
    || elf
      .libraries
      .iter()
      .any(|name| !TRUSTED_SONAMES.contains(name))
  {
    return Err(fail("library_dependency_unsafe"));
  }
  if elf.dynamic.as_ref().is_some_and(|table| {
    table.dyns.iter().any(|entry| {
      matches!(
        entry.d_tag,
        dynamic::DT_RPATH
          | dynamic::DT_RUNPATH
          | dynamic::DT_AUDIT
          | dynamic::DT_DEPAUDIT
          | 0x7fff_ffff
          | 0x7fff_fffd
      )
    })
  }) {
    return Err(fail("library_dependency_unsafe"));
  }
  Ok(())
}

fn inspect_worker(
  _executable: &Path,
  _stage: &File,
  _timeout: Duration,
) -> Result<Vec<SkillDescriptor>, HostError> {
  let deadline = Instant::now() + _timeout;
  let mut worker = WorkerSession::spawn(_executable, _stage, worker::WorkerMode::Inspect)?;
  let event = match worker.recv(deadline.saturating_duration_since(Instant::now())) {
    Ok(event) => event,
    Err(error) => {
      if !worker.terminate_reap()? {
        return Err(fail("worker_reap_failed"));
      }
      return Err(error);
    }
  };
  let reaped = worker.reap(Duration::from_secs(1))?;
  if !reaped {
    if !worker.terminate_reap()? {
      return Err(fail("worker_reap_failed"));
    }
    return Err(fail("worker_reap_failed"));
  }
  match event {
    WorkerEvent::Catalog { descriptors }
      if worker.exit_status().is_some_and(|status| status.success()) =>
    {
      let remaining = deadline.saturating_duration_since(Instant::now());
      if remaining.is_zero() {
        return Err(fail("worker_timeout"));
      }
      let (sender, receiver) = std::sync::mpsc::sync_channel(1);
      std::thread::spawn(move || {
        let result = (|| {
          if descriptors.is_empty() || descriptors.len() > 256 {
            return Err(fail("abi_library_invalid"));
          }
          let mut identities = BTreeSet::new();
          for (index, descriptor) in descriptors.iter().enumerate() {
            if descriptor.library_index != 0
              || descriptor.descriptor_index != index
              || !identities.insert((&descriptor.name, &descriptor.version))
            {
              return Err(fail("abi_descriptor_invalid"));
            }
            validate_copied_descriptor(descriptor)?;
          }
          Ok(descriptors)
        })();
        let _ = sender.send(result);
      });
      receiver
        .recv_timeout(remaining)
        .map_err(|_| fail("worker_timeout"))?
    }
    _ => Err(fail("worker_inspection_failed")),
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn selected_asset_uses_isos_glibc_min_fallback() {
    let asset = |sha: &str, glibc_min: &str| {
      serde_json::json!({
        "os": std::env::consts::OS,
        "arch": std::env::consts::ARCH,
        "sha256": sha,
        "glibc_min": glibc_min
      })
    };
    let release = serde_json::json!({
      "assets": [
        asset("incompatible", "999.0"),
        asset("compatible", "2.0")
      ]
    });
    let selected = selected_asset(&release).unwrap();
    assert_eq!(selected["sha256"], "compatible");
  }

  #[test]
  fn selected_skill_pin_matches_jsonl_bounds_and_p01_version_characters() {
    assert!(valid_selected_pin("island/fixture-skills", "0.1.0+Build.1"));
    assert!(!valid_selected_pin(
      &format!("island/{}", "x".repeat(90)),
      "0.1.0"
    ));
    assert!(!valid_selected_pin(
      "island/fixture-skills",
      &format!("1.2.{}", "x".repeat(62))
    ));
    assert!(!valid_selected_pin("island/fixture-skills", "0.1.0/secret"));
    assert!(!valid_selected_pin("island/fixture-skills", "v0.1.0"));
  }

  #[test]
  fn invoke_pidfd_reap_failure_retains_unreaped_status() {
    let failed = WorkerStartError::from(fail("worker_reap_failed"));
    assert_eq!(failed.worker_reaped, Some(false));
    let before_spawn = WorkerStartError::from(fail("worker_spawn_failed"));
    assert_eq!(before_spawn.worker_reaped, None);
  }

  #[test]
  fn abi_identity_and_schema_validation_match_owner_contract() {
    assert!(valid_abi_name("base.velocity"));
    for invalid in ["Bad/Name", "0leading", "two..dots", "trailing-"] {
      assert!(!valid_abi_name(invalid));
    }
    assert!(valid_abi_version("1.0.0+build.2"));
    assert!(!valid_abi_version("abc"));
    for invalid in [
      serde_json::json!({"type":"object","properties":"oops"}),
      serde_json::json!({"type":"object","$ref":"https://example.invalid/schema"}),
      serde_json::json!({"type":"object","$dynamicRef":"file:///tmp/other"}),
    ] {
      assert_eq!(
        validate_schema(&invalid).unwrap_err().code,
        "abi_schema_invalid"
      );
    }
  }
}
