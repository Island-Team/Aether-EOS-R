//! Bounded, process-local client for the EOS v2 control socket.

pub mod http;

use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::time::Duration;

use serde_json::Value;

pub const MAX_CONTROL_BYTES: usize = 64 * 1024;
pub const MAX_CONTROL_RESPONSE_BYTES: usize = 16 * 1024 * 1024;

pub fn request(socket: impl AsRef<Path>, body: &Value) -> Result<Value, String> {
  request_with_response_limit(socket, body, MAX_CONTROL_RESPONSE_BYTES)
}

/// A caller may impose a smaller response frame limit for narrow public reads.
pub fn request_with_response_limit(
  socket: impl AsRef<Path>,
  body: &Value,
  response_limit: usize,
) -> Result<Value, String> {
  let mut encoded = serde_json::to_vec(body).map_err(|error| error.to_string())?;
  if encoded.len() >= MAX_CONTROL_BYTES || encoded.contains(&b'\n') {
    return Err("control request exceeds bounds".into());
  }
  encoded.push(b'\n');
  let mut stream = UnixStream::connect(socket).map_err(|error| error.to_string())?;
  stream
    .set_read_timeout(Some(Duration::from_secs(2)))
    .map_err(|error| error.to_string())?;
  stream
    .set_write_timeout(Some(Duration::from_secs(2)))
    .map_err(|error| error.to_string())?;
  stream
    .write_all(&encoded)
    .map_err(|error| error.to_string())?;
  let mut output = Vec::new();
  let mut chunk = [0u8; 4096];
  loop {
    let read = stream.read(&mut chunk).map_err(|error| error.to_string())?;
    if read == 0 {
      return Err("EOS closed without a complete response".into());
    }
    output.extend_from_slice(&chunk[..read]);
    if output.len() > response_limit {
      return Err("control response exceeds bounds".into());
    }
    if let Some(end) = output.iter().position(|byte| *byte == b'\n') {
      if end + 1 != output.len() {
        return Err("control response has extra data".into());
      }
      return serde_json::from_slice(&output[..end]).map_err(|error| error.to_string());
    }
  }
}
