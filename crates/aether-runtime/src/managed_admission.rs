//! Verify the IsOS-selected graph generation and run-scoped control verifier.
//!
//! The verifier contains grant digests, never plaintext grants. A standalone
//! profile has no saved-graph authority, even when its launcher has a run ID.

use std::fs::{self, OpenOptions};
use std::io::Read;
use std::os::unix::fs::{MetadataExt, PermissionsExt};
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Component, Path, PathBuf};

use aether_common::events::{valid_digest, valid_run_id};
use aether_dispatcher::AdmittedBinding;
use aether_hal::{NodeKind, NodeProfile, Profile};
use serde::Deserialize;
use sha2::{Digest, Sha256};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GrantRole {
  EgiaControl,
  OperatorControl,
}

#[derive(Clone, Debug)]
pub struct ManagedAdmission {
  pub run_id: String,
  pub active_revision: String,
  egia_digest: [u8; 32],
  operator_digest: [u8; 32],
}

/// Resolve one canonical graph declaration to a Dispatcher binding. Graph
/// edges declare authority scope; they never execute a command themselves.
pub fn binding_for(
  profile: &Profile,
  hardware: &NodeProfile,
  movement_type: &str,
  primitive: &str,
) -> Option<AdmittedBinding> {
  let sources = hardware.inputs.get("command")?.values();
  if sources.len() != 1 {
    return None;
  }
  let source = &sources[0];
  if source.output != "command" {
    return None;
  }
  let control = profile.node(&source.node)?;
  let expected_type = match hardware.kind {
    NodeKind::Base => "eos.dispatcher.primitive_base@v0.1",
    NodeKind::Manipulator => "eos.dispatcher.primitive_manipulator@v0.1",
    _ => return None,
  };
  if control.node_type != expected_type
    || control.settings.get("movement_type")?.as_str()? != movement_type
    || control.settings.get("primitive_name")?.as_str()? != primitive
    || control.settings.get("primitive_version")?.as_str()? != "1.0.0"
  {
    return None;
  }
  Some(AdmittedBinding {
    binding_path: control.path().into(),
    node_path: hardware.path().into(),
    primitive: primitive.into(),
    primitive_version: "1.0.0".into(),
  })
}

pub fn admitted_bindings(profile: &Profile) -> Vec<AdmittedBinding> {
  profile
    .hardware_nodes()
    .filter_map(|node| {
      let sources = node.inputs.get("command")?.values();
      let source = sources.first()?;
      let control = profile.node(&source.node)?;
      let movement_type = control.settings.get("movement_type")?.as_str()?;
      let primitive = control.settings.get("primitive_name")?.as_str()?;
      binding_for(profile, node, movement_type, primitive)
    })
    .collect()
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Verifier {
  schema_version: u32,
  run_id: String,
  active_revision: String,
  egia_control_sha256: String,
  operator_control_sha256: String,
}

fn bytes32(value: &str) -> Result<[u8; 32], String> {
  if !valid_digest(value) {
    return Err("invalid control grant digest".into());
  }
  let mut bytes = [0u8; 32];
  for (index, slot) in bytes.iter_mut().enumerate() {
    let pair = &value.as_bytes()[2 * index..2 * index + 2];
    let digit = |byte: u8| -> u8 {
      if byte <= b'9' {
        byte - b'0'
      } else {
        byte - b'a' + 10
      }
    };
    *slot = (digit(pair[0]) << 4) | digit(pair[1]);
  }
  Ok(bytes)
}

fn private_file(path: &Path, limit: usize) -> Result<Vec<u8>, String> {
  let file = OpenOptions::new()
    .read(true)
    .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
    .open(path)
    .map_err(|_| "control file unavailable")?;
  let metadata = file.metadata().map_err(|_| "control file unavailable")?;
  if !metadata.file_type().is_file()
    || metadata.permissions().mode() & 0o777 != 0o600
    || metadata.uid() != unsafe { libc::geteuid() }
  {
    return Err("unsafe control file".into());
  }
  if metadata.len() > limit as u64 {
    return Err("control file exceeds bound".into());
  }
  let mut bytes = Vec::new();
  file
    .take((limit + 1) as u64)
    .read_to_end(&mut bytes)
    .map_err(|_| "control file unavailable")?;
  if bytes.len() > limit {
    return Err("control file exceeds bound".into());
  }
  Ok(bytes)
}

fn private_dir(path: &Path) -> Result<(), String> {
  let metadata = fs::symlink_metadata(path).map_err(|_| "control directory unavailable")?;
  if !metadata.file_type().is_dir()
    || metadata.permissions().mode() & 0o777 != 0o700
    || metadata.uid() != unsafe { libc::geteuid() }
  {
    return Err("unsafe control directory".into());
  }
  Ok(())
}

fn no_symlink_ancestors(path: &Path) -> Result<(), String> {
  if !path.is_absolute() {
    return Err("managed control path must be absolute".into());
  }
  if !path
    .components()
    .all(|component| matches!(component, Component::RootDir | Component::Normal(_)))
  {
    return Err("managed control path is not canonical".into());
  }
  for ancestor in path.ancestors() {
    let metadata = fs::symlink_metadata(ancestor).map_err(|_| "control path unavailable")?;
    if metadata.file_type().is_symlink() {
      return Err("control path contains a symlink".into());
    }
  }
  Ok(())
}

/// Return the selected IsOS leaf only for the exact v2 profile suffix.
fn selected_leaf(profile_path: &Path) -> Option<(PathBuf, String, String)> {
  if profile_path.file_name()? != "graph.json" || profile_path.parent()?.file_name()? != "eos" {
    return None;
  }
  let leaf = profile_path.parent()?.parent()?;
  let generation = leaf.file_name()?.to_str()?;
  let revision = leaf.parent()?.file_name()?.to_str()?;
  if leaf.parent()?.parent()?.file_name()? != "v2"
    || !valid_digest(generation)
    || !valid_digest(revision)
  {
    return None;
  }
  Some((leaf.to_path_buf(), revision.into(), generation.into()))
}

/// Verify a profile/verifier pair before adapters, journal, listeners or ready.
/// `None` is deliberately read-only standalone admission.
pub fn load(
  profile_path: &Path,
  profile_bytes: &[u8],
  verifier_path: Option<&Path>,
  run_id: &str,
) -> Result<Option<ManagedAdmission>, String> {
  let selected = selected_leaf(profile_path);
  let (leaf, path_revision, path_generation, verifier_path) = match (selected, verifier_path) {
    (None, None) => return Ok(None),
    (Some((leaf, revision, generation)), Some(verifier)) => (leaf, revision, generation, verifier),
    _ => return Err("incomplete managed control selection".into()),
  };
  if !valid_run_id(run_id) {
    return Err("invalid managed run ID".into());
  }
  no_symlink_ancestors(profile_path)?;
  no_symlink_ancestors(verifier_path)?;
  for dir in [
    leaf.as_path(),
    leaf.parent().unwrap(),
    leaf.parent().unwrap().parent().unwrap(),
    profile_path.parent().unwrap(),
    verifier_path.parent().ok_or("verifier has no parent")?,
  ] {
    private_dir(dir)?;
  }
  let profile_disk = private_file(profile_path, crate::MAX_PROFILE_BYTES)?;
  if profile_disk != profile_bytes {
    return Err("selected profile changed during admission".into());
  }
  let manifest_bytes = private_file(&leaf.join("manifest.json"), 256 * 1024)?;
  let manifest: serde_json::Value =
    serde_json::from_slice(&manifest_bytes).map_err(|_| "invalid selected manifest")?;
  if manifest.get("schema_version").and_then(|v| v.as_u64()) != Some(2)
    || manifest.get("revision").and_then(|v| v.as_str()) != Some(path_revision.as_str())
    || manifest.get("generation").and_then(|v| v.as_str()) != Some(path_generation.as_str())
  {
    return Err("selected manifest identity mismatch".into());
  }
  let entries = manifest
    .get("files")
    .and_then(|v| v.as_array())
    .ok_or("selected manifest lacks files")?;
  let mut matches = entries
    .iter()
    .filter(|entry| entry.get("path").and_then(|v| v.as_str()) == Some("eos/graph.json"));
  let entry = matches
    .next()
    .ok_or("selected manifest lacks EOS profile")?;
  if matches.next().is_some() {
    return Err("selected manifest repeats EOS profile".into());
  }
  let expected = entry
    .get("sha256")
    .and_then(|v| v.as_str())
    .ok_or("selected manifest lacks EOS digest")?;
  if !valid_digest(expected) || format!("{:x}", Sha256::digest(profile_bytes)) != expected {
    return Err("selected EOS profile digest mismatch".into());
  }
  let verifier_bytes = private_file(verifier_path, 4096)?;
  let verifier: Verifier =
    serde_json::from_slice(&verifier_bytes).map_err(|_| "invalid control verifier")?;
  if verifier.schema_version != 1
    || verifier.run_id != run_id
    || verifier.active_revision != path_revision
  {
    return Err("control verifier identity mismatch".into());
  }
  Ok(Some(ManagedAdmission {
    run_id: run_id.into(),
    active_revision: path_revision,
    egia_digest: bytes32(&verifier.egia_control_sha256)?,
    operator_digest: bytes32(&verifier.operator_control_sha256)?,
  }))
}

impl ManagedAdmission {
  /// Compare decoded bearer grant digests in constant time. Invalid syntax is
  /// indistinguishable from an unknown grant at the public authentication seam.
  pub fn grant_role(&self, candidate: &str) -> Option<GrantRole> {
    let raw = bytes32(candidate).ok()?;
    let digest: [u8; 32] = Sha256::digest(raw).into();
    let equal = |expected: &[u8; 32]| -> bool {
      let mut difference = 0u8;
      for index in 0..32 {
        difference |= digest[index] ^ expected[index];
      }
      difference == 0
    };
    let egia = equal(&self.egia_digest);
    let operator = equal(&self.operator_digest);
    if egia {
      Some(GrantRole::EgiaControl)
    } else if operator {
      Some(GrantRole::OperatorControl)
    } else {
      None
    }
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::sync::atomic::{AtomicUsize, Ordering};

  static NEXT: AtomicUsize = AtomicUsize::new(0);

  struct Fixture {
    root: PathBuf,
    profile: PathBuf,
    verifier: PathBuf,
    bytes: Vec<u8>,
  }

  impl Drop for Fixture {
    fn drop(&mut self) {
      let _ = fs::remove_dir_all(&self.root);
    }
  }

  fn write_private(path: &Path, bytes: &[u8]) {
    fs::write(path, bytes).unwrap();
    fs::set_permissions(path, fs::Permissions::from_mode(0o600)).unwrap();
  }

  fn fixture() -> Fixture {
    let root = std::env::temp_dir().join(format!(
      "eos-r03-managed-{}-{}",
      std::process::id(),
      NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    let revision = "a".repeat(64);
    let generation = "b".repeat(64);
    let leaf = root.join("v2").join(&revision).join(&generation);
    let eos = leaf.join("eos");
    let run = root.join("run");
    fs::create_dir_all(&eos).unwrap();
    fs::create_dir_all(&run).unwrap();
    for dir in [
      &root,
      &root.join("v2"),
      &root.join("v2").join(&revision),
      &leaf,
      &eos,
      &run,
    ] {
      fs::set_permissions(dir, fs::Permissions::from_mode(0o700)).unwrap();
    }
    let bytes = b"{\"version\":\"0.1.0\"}".to_vec();
    let profile = eos.join("graph.json");
    write_private(&profile, &bytes);
    let profile_digest = format!("{:x}", Sha256::digest(&bytes));
    write_private(
      &leaf.join("manifest.json"),
      serde_json::json!({
        "schema_version": 2,
        "revision": revision,
        "generation": generation,
        "files": [{"path":"eos/graph.json", "sha256":profile_digest}]
      })
      .to_string()
      .as_bytes(),
    );
    let egia = [7u8; 32];
    let operator = [8u8; 32];
    let verifier = run.join("verifier.json");
    write_private(
      &verifier,
      serde_json::json!({
        "schema_version": 1,
        "run_id": "run-1",
        "active_revision": "a".repeat(64),
        "egia_control_sha256": format!("{:x}", Sha256::digest(egia)),
        "operator_control_sha256": format!("{:x}", Sha256::digest(operator))
      })
      .to_string()
      .as_bytes(),
    );
    Fixture {
      root,
      profile,
      verifier,
      bytes,
    }
  }

  #[test]
  fn exact_pair_binds_run_revision_and_distinct_roles() {
    let fixture = fixture();
    let managed = load(
      &fixture.profile,
      &fixture.bytes,
      Some(&fixture.verifier),
      "run-1",
    )
    .unwrap()
    .unwrap();
    assert_eq!(managed.active_revision, "a".repeat(64));
    assert_eq!(managed.run_id, "run-1");
    assert_eq!(
      managed.grant_role(&format!("{:02x}", 7u8).repeat(32)),
      Some(GrantRole::EgiaControl)
    );
    assert_eq!(
      managed.grant_role(&format!("{:02x}", 8u8).repeat(32)),
      Some(GrantRole::OperatorControl)
    );
    assert_eq!(managed.grant_role(&format!("{:02x}", 9u8).repeat(32)), None);
    assert_eq!(managed.grant_role("A"), None);
  }

  #[test]
  fn partial_selection_and_stale_or_tampered_files_fail_closed() {
    let fixture = fixture();
    assert!(load(&fixture.profile, &fixture.bytes, None, "run-1").is_err());
    assert!(load(
      Path::new("/tmp/standalone.json"),
      &fixture.bytes,
      Some(&fixture.verifier),
      "run-1"
    )
    .is_err());
    assert!(load(
      &fixture.profile,
      &fixture.bytes,
      Some(&fixture.verifier),
      "old-run"
    )
    .is_err());
    write_private(&fixture.profile, b"tampered");
    assert!(load(
      &fixture.profile,
      &fixture.bytes,
      Some(&fixture.verifier),
      "run-1"
    )
    .is_err());
  }

  #[test]
  fn unsafe_verifier_mode_or_path_fails_closed() {
    let fixture = fixture();
    fs::set_permissions(&fixture.verifier, fs::Permissions::from_mode(0o644)).unwrap();
    assert!(load(
      &fixture.profile,
      &fixture.bytes,
      Some(&fixture.verifier),
      "run-1"
    )
    .is_err());
    fs::set_permissions(&fixture.verifier, fs::Permissions::from_mode(0o600)).unwrap();
    let link = fixture.root.join("verifier-link.json");
    std::os::unix::fs::symlink(&fixture.verifier, &link).unwrap();
    assert!(load(&fixture.profile, &fixture.bytes, Some(&link), "run-1").is_err());
  }

  #[test]
  fn standalone_never_infers_saved_revision_from_run_id() {
    assert!(
      load(Path::new("/tmp/standalone.json"), b"{}", None, "run-1")
        .unwrap()
        .is_none()
    );
  }
}
