//! Serialize changes to saved graph, generated configuration, and package links.

use crate::app::config::Paths;
use anyhow::{Result, ensure};
use std::{fs, os::fd::AsRawFd};

pub struct Lock(fs::File);

impl Lock {
  pub fn take(paths: &Paths) -> Result<Self> {
    paths.prepare()?;
    let file = fs::OpenOptions::new()
      .create(true)
      .truncate(false)
      .read(true)
      .write(true)
      .open(paths.runtime.join("manager.lock"))?;
    let result = unsafe { libc::flock(file.as_raw_fd(), libc::LOCK_EX) };
    ensure!(result == 0, "cannot lock IsOS manager state");
    Ok(Self(file))
  }
}

impl Drop for Lock {
  fn drop(&mut self) {
    unsafe {
      libc::flock(self.0.as_raw_fd(), libc::LOCK_UN);
    }
  }
}
