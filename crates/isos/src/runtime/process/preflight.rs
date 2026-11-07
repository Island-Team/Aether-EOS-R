//! Verify the IsOS owner executable before launching its children.
use crate::app::config::Paths;
use anyhow::{Result, ensure};

pub fn verify_active_isos(paths: &Paths) -> Result<()> {
  let _ = paths;
  let executable = std::env::current_exe()?;
  ensure!(executable.is_file(), "IsOS executable is missing");
  Ok(())
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn active_isos_checks_current_executable() {
    let temporary = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temporary.path().join("config"),
      data: temporary.path().join("data"),
      runtime: temporary.path().join("run"),
    };
    assert!(verify_active_isos(&paths).is_ok());
  }
}
