//! Resolve the Aether executables used by the IsOS node graph.

use anyhow::{Result, ensure};
use std::{
  env, fs,
  os::unix::fs::PermissionsExt,
  path::{Path, PathBuf},
};

fn runnable(path: &PathBuf) -> bool {
  fs::metadata(path)
    .is_ok_and(|metadata| metadata.is_file() && metadata.permissions().mode() & 0o111 != 0)
}

fn required_in_dir(name: &str, dir: &Path) -> Result<PathBuf> {
  ensure!(dir.is_absolute(), "ISOS_AETHER_BIN_DIR must be absolute");
  let path = dir.join(name);
  ensure!(
    runnable(&path),
    "{} is missing or not executable; check ISOS_AETHER_BIN_DIR",
    path.display()
  );
  Ok(path)
}

pub fn resolve(name: &str) -> Result<PathBuf> {
  if let Some(dir) = env::var_os("ISOS_AETHER_BIN_DIR") {
    return required_in_dir(name, &PathBuf::from(dir));
  }
  let workspace = Path::new(env!("CARGO_MANIFEST_DIR"))
    .parent()
    .and_then(Path::parent)
    .unwrap()
    .join("target/release");
  required_in_dir(name, &workspace).map_err(|_| anyhow::anyhow!(
        "{name} is unavailable; build EOS in this workspace and set paths.aether_bin_dir in config/isos.toml to the prebuilt binary directory"
    ))
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::os::unix::fs::PermissionsExt;

  #[test]
  fn resolves_explicit_aether_directory_and_reports_missing_binary() {
    let temp = tempfile::tempdir().unwrap();
    let name = "aether-test-only-resolver";
    let path = temp.path().join(name);
    fs::write(&path, "#!/bin/sh\nexit 0\n").unwrap();
    fs::set_permissions(&path, fs::Permissions::from_mode(0o755)).unwrap();
    let found = required_in_dir(name, temp.path()).unwrap();
    assert_eq!(found, path);
    assert!(required_in_dir("missing", temp.path()).is_err());
  }
}
