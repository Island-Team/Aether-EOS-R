//! Verify a prebuilt IsosUI bundle and locate source only for explicit UI live mode.
use crate::app::config::Paths;
use anyhow::{Context, Result, ensure};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::{
  fs,
  path::{Path, PathBuf},
};

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Manifest {
  pub schema_version: u32,
  pub version: String,
  pub source_commit: String,
  pub api_schema_version: Value,
  pub build_target: String,
}

fn manifest(dir: &Path) -> Result<Manifest> {
  let value: Manifest = serde_json::from_slice(&fs::read(dir.join("manifest.json"))?)?;
  ensure!(value.schema_version == 1, "unknown IsosUI manifest schema");
  ensure!(
    value.api_schema_version == "0.1.0",
    "IsosUI bundle must target IsOS API schema 0.1.0"
  );
  ensure!(
    value.build_target == "local",
    "IsosUI build must target local"
  );
  ensure!(
    value.source_commit.len() == 40 && value.source_commit.bytes().all(|c| c.is_ascii_hexdigit()),
    "IsosUI source_commit must be a full Git SHA"
  );
  ensure!(
    dir.join("index.html").is_file(),
    "IsosUI index.html missing"
  );
  Ok(value)
}

fn files(dir: &Path, prefix: &Path, found: &mut Vec<PathBuf>) -> Result<()> {
  for entry in fs::read_dir(dir)? {
    let path = entry?.path();
    let metadata = fs::symlink_metadata(&path)?;
    ensure!(
      !metadata.file_type().is_symlink(),
      "IsosUI bundle contains symlink"
    );
    if metadata.is_dir() {
      files(&path, prefix, found)?;
    } else {
      ensure!(metadata.is_file(), "IsosUI bundle contains special file");
      found.push(path.strip_prefix(prefix)?.to_path_buf());
    }
  }
  Ok(())
}

fn digest(dir: &Path) -> Result<String> {
  let mut list = Vec::new();
  files(dir, dir, &mut list)?;
  list.sort();
  let mut hasher = Sha256::new();
  for relative in list {
    let name = relative
      .to_str()
      .ok_or_else(|| anyhow::anyhow!("non-UTF-8 IsosUI path"))?;
    let bytes = fs::read(dir.join(&relative))?;
    hasher.update(name.as_bytes());
    hasher.update([0]);
    hasher.update((bytes.len() as u64).to_be_bytes());
    hasher.update(&bytes);
  }
  Ok(hex::encode(hasher.finalize()))
}

fn checked_source_root(path: PathBuf) -> Result<PathBuf> {
  ensure!(
    path.is_absolute() && path.join("package.json").is_file(),
    "IsosUI source missing at {}; set ISOSUI_REPO_PATH to an absolute path or paths.ui_repo in config.toml",
    path.display()
  );
  Ok(path.canonicalize()?)
}

fn resolve_source_root(configured: Option<PathBuf>, sibling: PathBuf) -> Result<PathBuf> {
  checked_source_root(configured.unwrap_or(sibling))
}

pub fn source_root() -> Result<PathBuf> {
  resolve_source_root(
    std::env::var_os("ISOSUI_REPO_PATH").map(PathBuf::from),
    Path::new(env!("CARGO_MANIFEST_DIR"))
      .parent()
      .and_then(Path::parent)
      .unwrap()
      .join("ui"),
  )
}

pub fn bundle_for_serve(paths: &Paths) -> Result<PathBuf> {
  let _ = paths;
  let directory = std::env::var_os("ISOSUI_DIST_DIR")
    .map(PathBuf::from)
    .unwrap_or_else(|| {
      Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(Path::parent)
        .unwrap()
        .join("ui/dist/local")
    });
  ensure!(directory.is_absolute(), "ISOSUI_DIST_DIR must be absolute");
  manifest(&directory).with_context(|| {
    format!(
      "IsosUI bundle unavailable at {}; build it in this workspace and configure paths.ui_dist",
      directory.display()
    )
  })?;
  let _ = digest(&directory)?;
  Ok(directory.canonicalize()?)
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn source_root_uses_sibling_and_validates_override() {
    let temporary = tempfile::tempdir().unwrap();
    let root = temporary.path().join("IsosUI");
    fs::create_dir(&root).unwrap();
    assert!(resolve_source_root(None, root.clone()).is_err());
    fs::write(root.join("package.json"), "{}").unwrap();
    assert_eq!(resolve_source_root(None, root.clone()).unwrap(), root);
    assert!(resolve_source_root(Some(PathBuf::from("../IsosUI")), root.clone()).is_err());
    assert!(resolve_source_root(Some(root.join("missing")), root).is_err());
  }

  #[test]
  fn bundle_manifest_and_digest_reject_symlinks() {
    let temporary = tempfile::tempdir().unwrap();
    let bundle = temporary.path().join("bundle");
    fs::create_dir(&bundle).unwrap();
    fs::write(bundle.join("index.html"), "<html></html>").unwrap();
    let identity = Manifest {
      schema_version: 1,
      version: "0.1.0".into(),
      source_commit: "a".repeat(40),
      api_schema_version: serde_json::json!("0.1.0"),
      build_target: "local".into(),
    };
    fs::write(
      bundle.join("manifest.json"),
      serde_json::to_vec(&identity).unwrap(),
    )
    .unwrap();
    assert_eq!(manifest(&bundle).unwrap(), identity);
    let mut legacy = identity.clone();
    legacy.api_schema_version = serde_json::json!(2);
    fs::write(
      bundle.join("manifest.json"),
      serde_json::to_vec(&legacy).unwrap(),
    )
    .unwrap();
    assert!(manifest(&bundle).is_err());
    fs::write(
      bundle.join("manifest.json"),
      serde_json::to_vec(&identity).unwrap(),
    )
    .unwrap();
    assert_eq!(digest(&bundle).unwrap().len(), 64);
    std::os::unix::fs::symlink("index.html", bundle.join("linked.html")).unwrap();
    assert!(digest(&bundle).is_err());
  }
}
