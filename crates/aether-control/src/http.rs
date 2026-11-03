//! Small bounded HTTP/1.1 request reader for loopback-only v2 services.

use std::collections::BTreeMap;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::os::unix::net::UnixStream;
use std::time::Duration;

pub const MAX_BODY_BYTES: usize = 64 * 1024;
const MAX_HEADER_BYTES: usize = 8 * 1024;

pub struct Request {
  pub method: String,
  pub path: String,
  pub body: Vec<u8>,
  pub headers: BTreeMap<String, String>,
}

pub fn read_request(stream: &mut TcpStream) -> Result<Request, String> {
  stream
    .set_read_timeout(Some(Duration::from_secs(2)))
    .map_err(|error| error.to_string())?;
  stream
    .set_write_timeout(Some(Duration::from_secs(2)))
    .map_err(|error| error.to_string())?;
  parse_request(stream)
}

pub fn read_unix_request(stream: &mut UnixStream) -> Result<Request, String> {
  stream
    .set_read_timeout(Some(Duration::from_secs(5)))
    .map_err(|error| error.to_string())?;
  stream
    .set_write_timeout(Some(Duration::from_secs(2)))
    .map_err(|error| error.to_string())?;
  parse_request(stream)
}

fn parse_request(stream: &mut impl Read) -> Result<Request, String> {
  let mut bytes = Vec::new();
  let mut chunk = [0u8; 4096];
  let header_end = loop {
    let read = stream.read(&mut chunk).map_err(|error| error.to_string())?;
    if read == 0 {
      return Err("request closed before headers".into());
    }
    bytes.extend_from_slice(&chunk[..read]);
    if let Some(position) = bytes.windows(4).position(|item| item == b"\r\n\r\n") {
      break position + 4;
    }
    if bytes.len() > MAX_HEADER_BYTES {
      return Err("HTTP headers exceed bounds".into());
    }
  };
  if header_end > MAX_HEADER_BYTES {
    return Err("HTTP headers exceed bounds".into());
  }
  let header = std::str::from_utf8(&bytes[..header_end])
    .map_err(|_| "HTTP headers are not UTF-8".to_string())?;
  let mut lines = header.split("\r\n");
  let first = lines.next().ok_or("missing request line")?;
  let mut parts = first.split_whitespace();
  let method = parts.next().ok_or("missing method")?.to_string();
  let path = parts.next().ok_or("missing path")?.to_string();
  if parts.next() != Some("HTTP/1.1") || parts.next().is_some() {
    return Err("only HTTP/1.1 is supported".into());
  }
  if !path.starts_with('/') || path.contains('?') {
    return Err("invalid request path".into());
  }
  let mut content_length = None;
  let mut headers = BTreeMap::new();
  for line in lines.filter(|line| !line.is_empty()) {
    let (name, value) = line.split_once(':').ok_or("malformed HTTP header")?;
    let key = name.to_ascii_lowercase();
    if headers.insert(key, value.trim().to_owned()).is_some() {
      return Err("duplicate HTTP header".into());
    }
    if name.eq_ignore_ascii_case("transfer-encoding") {
      return Err("transfer encoding is unsupported".into());
    }
    if name.eq_ignore_ascii_case("content-length") {
      if content_length.is_some() {
        return Err("duplicate content length".into());
      }
      content_length = Some(
        value
          .trim()
          .parse::<usize>()
          .map_err(|_| "invalid content length")?,
      );
    }
  }
  let length = content_length.unwrap_or(0);
  if length > MAX_BODY_BYTES || bytes.len().saturating_sub(header_end) > length {
    return Err("HTTP body exceeds declared bounds".into());
  }
  while bytes.len() - header_end < length {
    let read = stream.read(&mut chunk).map_err(|error| error.to_string())?;
    if read == 0 {
      return Err("request closed before body".into());
    }
    bytes.extend_from_slice(&chunk[..read]);
    if bytes.len() - header_end > length {
      return Err("HTTP body exceeds declared bounds".into());
    }
  }
  Ok(Request {
    method,
    path,
    body: bytes[header_end..].to_vec(),
    headers,
  })
}

pub fn reply(
  stream: &mut TcpStream,
  status: u16,
  content_type: &str,
  body: &[u8],
) -> std::io::Result<()> {
  reply_with_headers(stream, status, content_type, body, &[])
}

pub fn reply_with_headers(
  stream: &mut TcpStream,
  status: u16,
  content_type: &str,
  body: &[u8],
  headers: &[(&str, &str)],
) -> std::io::Result<()> {
  let reason = match status {
    200 => "OK",
    400 => "Bad Request",
    404 => "Not Found",
    405 => "Method Not Allowed",
    403 => "Forbidden",
    204 => "No Content",
    502 => "Bad Gateway",
    503 => "Service Unavailable",
    _ => "Internal Server Error",
  };
  write!(stream, "HTTP/1.1 {status} {reason}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nConnection: close\r\nCache-Control: no-store\r\n", body.len())?;
  for (name, value) in headers {
    write!(stream, "{name}: {value}\r\n")?;
  }
  write!(stream, "\r\n")?;
  stream.write_all(body)
}
