//! File helpers with explicit bounds and crash-safe replacement.

use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::path::Path;
use std::sync::atomic::{AtomicU64, Ordering};

/// An in-memory writer that fails before growing beyond a declared byte limit.
pub struct BoundedWriter {
  bytes: Vec<u8>,
  max_bytes: usize,
}

impl BoundedWriter {
  pub fn new(max_bytes: usize) -> Self {
    Self {
      bytes: Vec::new(),
      max_bytes,
    }
  }

  pub fn into_inner(self) -> Vec<u8> {
    self.bytes
  }
}

impl Write for BoundedWriter {
  fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
    if buf.len() > self.max_bytes.saturating_sub(self.bytes.len()) {
      return Err(io::Error::new(
        io::ErrorKind::InvalidInput,
        "write exceeds byte limit",
      ));
    }
    self.bytes.extend_from_slice(buf);
    Ok(buf.len())
  }

  fn flush(&mut self) -> io::Result<()> {
    Ok(())
  }
}

/// Read at most `max_bytes`; an oversized file is an error, never truncated data.
pub fn read_bounded(path: impl AsRef<Path>, max_bytes: usize) -> io::Result<Vec<u8>> {
  let mut file = File::open(path)?;
  let mut bytes = Vec::new();
  let limit = u64::try_from(max_bytes)
    .unwrap_or(u64::MAX)
    .saturating_add(1);
  Read::by_ref(&mut file)
    .take(limit)
    .read_to_end(&mut bytes)?;
  if bytes.len() > max_bytes {
    return Err(io::Error::new(
      io::ErrorKind::InvalidData,
      "file exceeds byte limit",
    ));
  }
  Ok(bytes)
}

/// Atomically replace a file in the same directory with a bounded payload.
/// The caller decides whether the path may contain sensitive data.
pub fn write_atomic_bounded(
  path: impl AsRef<Path>,
  bytes: &[u8],
  max_bytes: usize,
) -> io::Result<()> {
  if bytes.len() > max_bytes {
    return Err(io::Error::new(
      io::ErrorKind::InvalidInput,
      "payload exceeds byte limit",
    ));
  }
  let path = path.as_ref();
  let parent = path
    .parent()
    .filter(|p| !p.as_os_str().is_empty())
    .unwrap_or_else(|| Path::new("."));
  fs::create_dir_all(parent)?;
  static NEXT_TEMP: AtomicU64 = AtomicU64::new(0);
  let suffix = NEXT_TEMP.fetch_add(1, Ordering::Relaxed);
  let file_name = path
    .file_name()
    .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "path has no file name"))?;
  let temp_name = format!(
    ".{}.{}.{}.tmp",
    file_name.to_string_lossy(),
    std::process::id(),
    suffix
  );
  let temp_path = parent.join(temp_name);
  let result = (|| {
    let mut file = OpenOptions::new()
      .write(true)
      .create_new(true)
      .open(&temp_path)?;
    file.write_all(bytes)?;
    file.sync_all()?;
    fs::rename(&temp_path, path)?;
    File::open(parent)?.sync_all()
  })();
  if result.is_err() {
    let _ = fs::remove_file(&temp_path);
  }
  result
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn bounded_round_trip_and_rejection() {
    let path = std::env::temp_dir().join(format!(
      "aether-common-{}-{}.txt",
      std::process::id(),
      NEXT_TEST.fetch_add(1, Ordering::Relaxed)
    ));
    write_atomic_bounded(&path, b"hello", 5).unwrap();
    assert_eq!(read_bounded(&path, 5).unwrap(), b"hello");
    assert_eq!(
      read_bounded(&path, 4).unwrap_err().kind(),
      io::ErrorKind::InvalidData
    );
    assert_eq!(
      write_atomic_bounded(&path, b"longer", 5)
        .unwrap_err()
        .kind(),
      io::ErrorKind::InvalidInput
    );
    assert_eq!(read_bounded(&path, 5).unwrap(), b"hello");
    fs::remove_file(path).unwrap();
  }

  #[test]
  fn bounded_writer_rejects_growth_without_partial_write() {
    let mut writer = BoundedWriter::new(3);
    writer.write_all(b"ab").unwrap();
    assert_eq!(
      writer.write(b"cd").unwrap_err().kind(),
      io::ErrorKind::InvalidInput
    );
    writer.write_all(b"c").unwrap();
    assert_eq!(writer.into_inner(), b"abc");
  }

  static NEXT_TEST: AtomicU64 = AtomicU64::new(0);
}
