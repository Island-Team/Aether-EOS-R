use std::io::{Read, Write};
use std::net::TcpStream;
use std::os::fd::AsRawFd;
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::thread;
use std::time::{Duration, Instant};

use aether_control::http::Request;
use base64::engine::general_purpose::STANDARD;
use base64::Engine;
use serde_json::{json, Value};
use sha1::{Digest, Sha1};
use sha2::Sha256;

const MAX_SUBSCRIPTION_BYTES: usize = 4096;
const MAX_FRAME_BYTES: usize = 4 + 8 * 1024 * 1024 + 4096;

trait VisionStream: Read + Write + AsRawFd {
  fn timeout(&self, duration: Duration) -> std::io::Result<()>;
}
impl VisionStream for TcpStream {
  fn timeout(&self, duration: Duration) -> std::io::Result<()> {
    self.set_read_timeout(Some(duration))
  }
}
impl VisionStream for UnixStream {
  fn timeout(&self, duration: Duration) -> std::io::Result<()> {
    self.set_read_timeout(Some(duration))
  }
}

pub fn serve(stream: &mut TcpStream, request: &Request, socket: &Path) -> Result<(), String> {
  serve_inner(stream, request, socket, None)
}

pub fn serve_owner(
  stream: &mut UnixStream,
  request: &Request,
  socket: &Path,
  attest: &Value,
  selected_gate: &str,
  binding_current: &dyn Fn() -> bool,
) -> Result<(), String> {
  serve_inner(
    stream,
    request,
    socket,
    Some((attest, selected_gate, binding_current)),
  )
}

fn serve_inner(
  stream: &mut impl VisionStream,
  request: &Request,
  socket: &Path,
  owner: Option<(&Value, &str, &dyn Fn() -> bool)>,
) -> Result<(), String> {
  if request
    .headers
    .get("upgrade")
    .is_none_or(|v| !v.eq_ignore_ascii_case("websocket"))
    || request.headers.get("connection").is_none_or(|v| {
      !v.to_ascii_lowercase()
        .split(',')
        .any(|part| part.trim() == "upgrade")
    })
    || request
      .headers
      .get("sec-websocket-version")
      .map(String::as_str)
      != Some("13")
  {
    return Err("invalid WebSocket upgrade".into());
  }
  let key = request
    .headers
    .get("sec-websocket-key")
    .ok_or("missing WebSocket key")?;
  if STANDARD
    .decode(key)
    .map_err(|_| "invalid WebSocket key")?
    .len()
    != 16
  {
    return Err("invalid WebSocket key".into());
  }
  let mut sha = Sha1::new();
  sha.update(key.as_bytes());
  sha.update(b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
  let accept = STANDARD.encode(sha.finalize());
  write!(stream, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\nCache-Control: no-store\r\n\r\n")
        .map_err(|e| e.to_string())?;
  stream.flush().map_err(|e| e.to_string())?;
  stream
    .timeout(Duration::from_secs(5))
    .map_err(|e| e.to_string())?;
  let subscription = match read_text(stream) {
    Ok(value) => value,
    Err(error) if owner.is_some() => {
      send_error(stream, "invalid_metadata")?;
      send_close(stream, 1008)?;
      return Err(error);
    }
    Err(error) => return Err(error),
  };
  let request: Value = match serde_json::from_slice(&subscription) {
    Ok(value) => value,
    Err(error) if owner.is_some() => {
      send_error(stream, "invalid_metadata")?;
      send_close(stream, 1008)?;
      return Err(error.to_string());
    }
    Err(error) => return Err(error.to_string()),
  };
  if request.get("op").and_then(Value::as_str) != Some("subscribe") {
    send_error(
      stream,
      if owner.is_some() {
        "invalid_metadata"
      } else {
        "first message must subscribe"
      },
    )?;
    if owner.is_some() {
      send_close(stream, 1008)?;
    }
    return Ok(());
  }
  let Some(gate_path) = request.get("gate_path").and_then(Value::as_str) else {
    send_error(stream, "invalid_metadata")?;
    if owner.is_some() {
      send_close(stream, 1008)?;
    }
    return Ok(());
  };
  let Some(revision) = request.get("revision").and_then(Value::as_str) else {
    send_error(stream, "invalid_metadata")?;
    if owner.is_some() {
      send_close(stream, 1008)?;
    }
    return Ok(());
  };
  let discovery = aether_control::request(socket, &json!({"op":"vision_gates"}))?;
  let options = match subscription_options(&request, &discovery, gate_path, revision) {
    Ok(options) => options,
    Err(error) => {
      let reason = if owner.is_some() {
        match error.as_str() {
          "graph revision changed" => "revision_mismatch",
          "camera gate is not active" => "gate_inactive",
          _ => "invalid_metadata",
        }
      } else {
        &error
      };
      send_error(stream, reason)?;
      if owner.is_some() {
        send_close(stream, 1008)?;
      }
      return Ok(());
    }
  };
  if let Some((startup_attest, selected_gate, _)) = owner {
    if gate_path != selected_gate || revision != startup_attest["projection_revision"] {
      send_error(stream, "revision_mismatch")?;
      send_close(stream, 1008)?;
      return Ok(());
    }
    if options.width != 1280 || options.quality != 80 || options.target_fps != 30.0 {
      send_error(stream, "invalid_metadata")?;
      send_close(stream, 1008)?;
      return Ok(());
    }
  }
  let attestation =
    aether_control::request(socket, &json!({"op":"vision_attest","gate_path":gate_path}))
      .ok()
      .filter(|response| {
        response["ok"] == true
          && (owner.is_none() || response["ready"] == true)
          && response["runtime_stream_id"].is_string()
          && response["active_revision"].is_string()
      });
  if owner.is_some() && attestation.is_none() {
    send_error(stream, "gate_inactive")?;
    send_close(stream, 1008)?;
    return Ok(());
  }
  if let (Some((startup_attest, _, _)), Some(attest)) = (owner, &attestation) {
    for field in [
      "run_id",
      "active_revision",
      "projection_revision",
      "runtime_stream_id",
      "source_path",
      "gate_path",
    ] {
      if attest[field] != startup_attest[field] {
        send_error(stream, "stream_mismatch")?;
        send_close(stream, 1008)?;
        return Ok(());
      }
    }
  }
  let cursor = request.get("after_generation").and_then(Value::as_u64);
  let cursor_stream = request
    .get("after_runtime_stream_id")
    .and_then(Value::as_str);
  if request.get("after_generation").is_some() != request.get("after_runtime_stream_id").is_some()
    || (request.get("after_generation").is_some() && cursor.is_none())
    || (request.get("after_runtime_stream_id").is_some() && cursor_stream.is_none())
  {
    send_error(stream, "invalid_metadata")?;
    send_close(stream, 1008)?;
    return Ok(());
  }
  if let Some(attest) = &attestation {
    if cursor_stream.is_some_and(|value| Some(value) != attest["runtime_stream_id"].as_str()) {
      send_error(stream, "cursor_stale")?;
      send_close(stream, 1008)?;
      return Ok(());
    }
  } else if cursor.is_some() {
    send_error(stream, "cursor_stale")?;
    send_close(stream, 1008)?;
    return Ok(());
  }
  let target_fps = options.target_fps;
  let width = options.width;
  let quality = options.quality;
  let mut subscribed = json!({"type":"subscribed"});
  if let Some(attest) = &attestation {
    subscribed["graph_revision"] = attest["active_revision"].clone();
    for field in [
      "projection_revision",
      "runtime_stream_id",
      "source_path",
      "gate_path",
      "gate_generation",
    ] {
      subscribed[field] = attest[field].clone();
    }
  }
  send_frame(stream, 0x1, subscribed.to_string().as_bytes())?;
  let interval = Duration::from_secs_f64(1.0 / target_fps);
  let mut next_request = Instant::now();
  let mut generation = cursor.unwrap_or(0);
  loop {
    if crate::TERMINATE.load(std::sync::atomic::Ordering::Acquire) {
      let _ = send_close(stream, 1012);
      return Ok(());
    }
    if peer_closed(stream)? {
      return Ok(());
    }
    if let Some((startup_attest, _, binding_current)) = owner {
      if !binding_current() {
        send_close(stream, 1012)?;
        return Ok(());
      }
      let current =
        aether_control::request(socket, &json!({"op":"vision_attest","gate_path":gate_path}));
      if !current.as_ref().is_ok_and(|value| {
        value["ok"] == true
          && value["ready"] == true
          && value["runtime_stream_id"] == startup_attest["runtime_stream_id"]
          && value["active_revision"] == startup_attest["active_revision"]
          && value["projection_revision"] == startup_attest["projection_revision"]
          && value["source_path"] == startup_attest["source_path"]
          && value["gate_path"] == startup_attest["gate_path"]
      }) {
        send_close(stream, 1012)?;
        return Ok(());
      }
    }
    let response = aether_control::request(
      socket,
      &json!({
          "op":"vision_frame","gate_path":gate_path,"revision":revision,
          "after_generation":generation,"max_width_px":width,"jpeg_quality":quality,
      }),
    );
    let response = match response {
      Ok(value) => value,
      Err(error) => {
        if owner.is_some() {
          let _ = send_close(stream, 1012);
        }
        return Err(error);
      }
    };
    if response.get("ok").and_then(Value::as_bool) != Some(true) {
      send_error(
        stream,
        response
          .get("error")
          .and_then(Value::as_str)
          .unwrap_or("vision source failed"),
      )?;
      if owner.is_some() {
        send_close(stream, 1012)?;
      }
      return Ok(());
    }
    if response.get("changed").and_then(Value::as_bool) == Some(true) {
      let next_generation = response
        .get("generation")
        .and_then(Value::as_u64)
        .ok_or("missing frame generation")?;
      if next_generation <= generation {
        send_error(stream, "invalid_metadata")?;
        if owner.is_some() {
          send_close(stream, 1008)?;
        }
        return Ok(());
      }
      let metadata = serde_json::to_vec(response.get("metadata").ok_or("missing frame metadata")?)
        .map_err(|e| e.to_string())?;
      let jpeg = STANDARD
        .decode(
          response
            .get("jpeg_base64")
            .and_then(Value::as_str)
            .ok_or("missing JPEG")?,
        )
        .map_err(|e| e.to_string())?;
      if !payload_within_bounds(metadata.len(), jpeg.len()) {
        return send_error(stream, "vision frame exceeds bounds");
      }
      if let Some((startup_attest, _, binding_current)) = owner {
        let info = response.get("metadata").ok_or("invalid_metadata")?;
        if let Err(reason) =
          validate_owner_frame(info, &jpeg, startup_attest, next_generation, width, quality)
        {
          send_error(stream, reason)?;
          send_close(stream, 1008)?;
          return Ok(());
        }
        if !binding_current() {
          send_close(stream, 1012)?;
          return Ok(());
        }
      }
      let mut payload = Vec::with_capacity(4 + metadata.len() + jpeg.len());
      payload.extend_from_slice(&(metadata.len() as u32).to_le_bytes());
      payload.extend_from_slice(&metadata);
      payload.extend_from_slice(&jpeg);
      send_frame(stream, 0x2, &payload)?;
      generation = next_generation;
    }
    next_request += interval;
    let now = Instant::now();
    if next_request <= now {
      next_request = now;
    }
    thread::sleep(next_request.saturating_duration_since(now));
  }
}

fn payload_within_bounds(metadata_len: usize, jpeg_len: usize) -> bool {
  (1..=4096).contains(&metadata_len) && (1..=8 * 1024 * 1024).contains(&jpeg_len)
}

fn validate_owner_frame(
  metadata: &Value,
  jpeg: &[u8],
  attest: &Value,
  generation: u64,
  width: u64,
  quality: u64,
) -> Result<(), &'static str> {
  if !metadata.is_object()
    || generation == 0
    || metadata["gate_generation"].as_u64() != Some(generation)
    || metadata["runtime_stream_id"] != attest["runtime_stream_id"]
    || metadata["graph_revision"] != attest["active_revision"]
    || metadata["projection_revision"] != attest["projection_revision"]
    || metadata["source_path"] != attest["source_path"]
    || metadata["gate_path"] != attest["gate_path"]
    || metadata["render"]["max_width_px"].as_u64() != Some(width)
    || metadata["render"]["jpeg_quality"].as_u64() != Some(quality)
    || metadata["source_frame_id"]
      .as_str()
      .is_none_or(|value| value.trim().is_empty() || value.len() > 128)
    || metadata["width_px"]
      .as_u64()
      .is_none_or(|value| value == 0 || value > 4096)
    || metadata["height_px"]
      .as_u64()
      .is_none_or(|value| value == 0 || value > 4096)
  {
    return Err("invalid_metadata");
  }
  if jpeg.len() > 8 * 1024 * 1024 {
    return Err("jpeg_too_large");
  }
  if jpeg.len() < 4 || !jpeg.starts_with(&[0xff, 0xd8]) || !jpeg.ends_with(&[0xff, 0xd9]) {
    return Err("jpeg_invalid");
  }
  let digest = format!("{:x}", Sha256::digest(jpeg));
  if metadata["jpeg_sha256"].as_str() != Some(digest.as_str()) {
    return Err("jpeg_invalid");
  }
  Ok(())
}

fn peer_closed(stream: &impl VisionStream) -> Result<bool, String> {
  let mut byte = [0u8; 1];
  let result = unsafe {
    libc::recv(
      stream.as_raw_fd(),
      byte.as_mut_ptr().cast(),
      1,
      libc::MSG_PEEK | libc::MSG_DONTWAIT,
    )
  };
  if result == 0 {
    return Ok(true);
  }
  if result > 0 {
    return Ok(true);
  }
  let error = std::io::Error::last_os_error();
  if matches!(
    error.kind(),
    std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
  ) {
    Ok(false)
  } else {
    Err(error.to_string())
  }
}

fn send_close(stream: &mut impl Write, code: u16) -> Result<(), String> {
  send_frame(stream, 0x8, &code.to_be_bytes())
}

#[derive(Debug)]
struct SubscriptionOptions {
  target_fps: f64,
  width: u64,
  quality: u64,
}

fn subscription_options(
  request: &Value,
  discovery: &Value,
  gate_path: &str,
  revision: &str,
) -> Result<SubscriptionOptions, String> {
  if discovery.get("ok").and_then(Value::as_bool) != Some(true)
    || discovery.get("revision").and_then(Value::as_str) != Some(revision)
  {
    return Err("graph revision changed".into());
  }
  let gate = discovery
    .get("gates")
    .and_then(Value::as_array)
    .and_then(|gates| {
      gates
        .iter()
        .find(|g| g.get("path").and_then(Value::as_str) == Some(gate_path))
    })
    .ok_or("camera gate is not active")?;
  let source_rate = gate
    .get("source_rate_hz")
    .and_then(Value::as_f64)
    .ok_or("gate source rate missing")?;
  let target_fps = match request.get("target_fps") {
    Some(value) => value.as_f64().ok_or("target_fps must be a number")?,
    None => source_rate.min(60.0),
  };
  let width = match request.get("max_width_px") {
    Some(value) => value.as_u64().ok_or("max_width_px must be an integer")?,
    None => 1280,
  };
  let quality = match request.get("jpeg_quality") {
    Some(value) => value.as_u64().ok_or("jpeg_quality must be an integer")?,
    None => 85,
  };
  if !target_fps.is_finite()
    || !(0.0..=60.0).contains(&target_fps)
    || target_fps == 0.0
    || !(1..=4096).contains(&width)
    || !(1..=100).contains(&quality)
  {
    return Err("invalid vision subscription options".into());
  }
  Ok(SubscriptionOptions {
    target_fps,
    width,
    quality,
  })
}

fn read_text(stream: &mut impl Read) -> Result<Vec<u8>, String> {
  let mut header = [0u8; 2];
  stream.read_exact(&mut header).map_err(|e| e.to_string())?;
  if header[0] != 0x81 || header[1] & 0x80 == 0 {
    return Err("first WebSocket frame must be masked text".into());
  }
  let length = match header[1] & 0x7f {
    126 => {
      let mut bytes = [0u8; 2];
      stream.read_exact(&mut bytes).map_err(|e| e.to_string())?;
      u16::from_be_bytes(bytes) as usize
    }
    127 => return Err("subscription exceeds bounds".into()),
    n => n as usize,
  };
  if length > MAX_SUBSCRIPTION_BYTES {
    return Err("subscription exceeds bounds".into());
  }
  let mut mask = [0u8; 4];
  stream.read_exact(&mut mask).map_err(|e| e.to_string())?;
  let mut bytes = vec![0u8; length];
  stream.read_exact(&mut bytes).map_err(|e| e.to_string())?;
  for (index, byte) in bytes.iter_mut().enumerate() {
    *byte ^= mask[index % 4];
  }
  Ok(bytes)
}

fn send_error(stream: &mut impl Write, message: &str) -> Result<(), String> {
  let reason_code = message
    .bytes()
    .all(|value| value.is_ascii_lowercase() || value == b'_');
  send_frame(
    stream,
    0x1,
    if reason_code {
      json!({"type":"error","message":message,"reason_code":message})
    } else {
      json!({"type":"error","message":message})
    }
    .to_string()
    .as_bytes(),
  )
}

fn send_frame(stream: &mut impl Write, opcode: u8, payload: &[u8]) -> Result<(), String> {
  if payload.len() > MAX_FRAME_BYTES {
    return Err("WebSocket frame exceeds bounds".into());
  }
  let mut header = vec![0x80 | opcode];
  match payload.len() {
    n if n <= 125 => header.push(n as u8),
    n if n <= u16::MAX as usize => {
      header.push(126);
      header.extend_from_slice(&(n as u16).to_be_bytes());
    }
    n => {
      header.push(127);
      header.extend_from_slice(&(n as u64).to_be_bytes());
    }
  }
  stream
    .write_all(&header)
    .and_then(|_| stream.write_all(payload))
    .map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::collections::BTreeMap;
  use std::io::{BufRead, BufReader};
  use std::net::TcpListener;
  use std::os::unix::net::UnixListener;
  use std::sync::atomic::{AtomicBool, Ordering};
  use std::sync::Arc;
  use std::os::unix::net::UnixStream;

  fn ws_payload(stream: &mut impl Read) -> (u8, Vec<u8>) {
    let mut header = [0u8; 2];
    stream.read_exact(&mut header).unwrap();
    let length = match header[1] & 0x7f {
      126 => {
        let mut bytes = [0u8; 2];
        stream.read_exact(&mut bytes).unwrap();
        u16::from_be_bytes(bytes) as usize
      }
      127 => {
        let mut bytes = [0u8; 8];
        stream.read_exact(&mut bytes).unwrap();
        u64::from_be_bytes(bytes) as usize
      }
      n => n as usize,
    };
    let mut payload = vec![0; length];
    stream.read_exact(&mut payload).unwrap();
    (header[0] & 0x0f, payload)
  }

  fn owner_socket_case(
    second_generation: u64,
    revoke_after_first: bool,
    unready_after_first: bool,
  ) {
    let root = std::env::temp_dir().join(format!(
      "vision-ws-owner-{}-{}",
      std::process::id(),
      std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos()
    ));
    std::fs::create_dir(&root).unwrap();
    let socket = root.join("eos.sock");
    let listener = UnixListener::bind(&socket).unwrap();
    listener.set_nonblocking(true).unwrap();
    let done = Arc::new(AtomicBool::new(false));
    let stop = done.clone();
    let allow_second = Arc::new(AtomicBool::new(false));
    let server_allow_second = allow_second.clone();
    let unready = Arc::new(AtomicBool::new(false));
    let server_unready = unready.clone();
    let attest = json!({"ok":true,"ready":true,"run_id":"run-one",
      "active_revision":"a".repeat(64),"projection_revision":"b".repeat(64),
      "runtime_stream_id":"11111111-1111-4111-8111-111111111111",
      "source_path":"eos/hardware/camera/front","gate_path":"eos/interface/gate/front",
      "gate_generation":0});
    let jpeg = [0xff, 0xd8, 0xff, 0xd9];
    let metadata = json!({"gate_path":attest["gate_path"],"source_path":attest["source_path"],
      "graph_revision":attest["active_revision"],"projection_revision":attest["projection_revision"],
      "runtime_stream_id":attest["runtime_stream_id"],"gate_generation":1,
      "source_frame_id":"src-1","width_px":1,"height_px":1,"source_rate_hz":30.0,
      "render":{"max_width_px":1280,"jpeg_quality":80},
      "jpeg_sha256":format!("{:x}", Sha256::digest(jpeg))});
    let server_attest = attest.clone();
    let server_metadata = metadata.clone();
    let control = std::thread::spawn(move || {
      let mut attest_count = 0;
      while !stop.load(Ordering::Relaxed) {
        match listener.accept() {
          Ok((mut stream, _)) => {
            let mut line = String::new();
            BufReader::new(stream.try_clone().unwrap())
              .read_line(&mut line)
              .unwrap();
            let request: Value = serde_json::from_str(&line).unwrap();
            let reply = match request["op"].as_str().unwrap() {
              "vision_attest" => {
                attest_count += 1;
                if attest_count >= 3 {
                  while !server_allow_second.load(Ordering::Acquire) {
                    std::thread::sleep(Duration::from_millis(1));
                  }
                }
                let mut response = server_attest.clone();
                if server_unready.load(Ordering::Acquire) {
                  response["ready"] = json!(false);
                }
                response
              }
              "vision_gates" => json!({"ok":true,"revision":"b".repeat(64),
                "gates":[{"path":"eos/interface/gate/front","source_rate_hz":30.0}]}),
              "vision_frame" => {
                if request["after_generation"] != 0 {
                  while !server_allow_second.load(Ordering::Acquire) {
                    std::thread::sleep(Duration::from_millis(1));
                  }
                }
                let next = if request["after_generation"] == 0 {
                  1
                } else {
                  second_generation
                };
                let mut metadata = server_metadata.clone();
                metadata["gate_generation"] = json!(next);
                json!({"ok":true,"changed":true,"generation":next,
                  "metadata":metadata,"jpeg_base64":STANDARD.encode(jpeg)})
              }
              other => panic!("unexpected EOS op: {other}"),
            };
            stream.write_all(format!("{reply}\n").as_bytes()).unwrap();
          }
          Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
            std::thread::sleep(Duration::from_millis(2))
          }
          Err(error) => panic!("EOS stub: {error}"),
        }
      }
    });
    let (mut client, mut server) = UnixStream::pair().unwrap();
    let request = Request {
      method: "GET".into(),
      path: "/api/eos/v2/vision/owner-ws".into(),
      body: vec![],
      headers: BTreeMap::from([
        ("upgrade".into(), "websocket".into()),
        ("connection".into(), "Upgrade".into()),
        ("sec-websocket-version".into(), "13".into()),
        (
          "sec-websocket-key".into(),
          "dGhlIHNhbXBsZSBub25jZQ==".into(),
        ),
      ]),
    };
    let socket_clone = socket.clone();
    let worker_attest = attest.clone();
    let verifier_path = root.join("vision-verifier.json");
    std::fs::write(&verifier_path, b"active").unwrap();
    let worker_verifier_path = verifier_path.clone();
    let worker = std::thread::spawn(move || {
      serve_owner(
        &mut server,
        &request,
        &socket_clone,
        &worker_attest,
        "eos/interface/gate/front",
        &|| std::fs::read(&worker_verifier_path).is_ok_and(|bytes| bytes == b"active"),
      )
      .unwrap()
    });
    client
      .set_read_timeout(Some(Duration::from_secs(2)))
      .unwrap();
    let mut headers = BufReader::new(client.try_clone().unwrap());
    let mut line = String::new();
    headers.read_line(&mut line).unwrap();
    assert!(line.starts_with("HTTP/1.1 101"));
    while line != "\r\n" {
      line.clear();
      headers.read_line(&mut line).unwrap();
    }
    drop(headers);
    let subscribe = json!({"op":"subscribe","gate_path":"eos/interface/gate/front",
      "revision":"b".repeat(64),"target_fps":30,"max_width_px":1280,"jpeg_quality":80})
    .to_string();
    client.write_all(&[0x81, 0xfe]).unwrap();
    client
      .write_all(&(subscribe.len() as u16).to_be_bytes())
      .unwrap();
    client.write_all(&[1, 2, 3, 4]).unwrap();
    client
      .write_all(
        &subscribe
          .as_bytes()
          .iter()
          .enumerate()
          .map(|(i, b)| b ^ [1, 2, 3, 4][i % 4])
          .collect::<Vec<_>>(),
      )
      .unwrap();
    let (opcode, subscribed) = ws_payload(&mut client);
    assert_eq!(opcode, 1);
    let subscribed: Value = serde_json::from_slice(&subscribed).unwrap();
    assert_eq!(subscribed["type"], "subscribed");
    assert_eq!(subscribed["gate_generation"], 0);
    let (opcode, payload) = ws_payload(&mut client);
    assert_eq!(opcode, 2);
    let length = u32::from_le_bytes(payload[..4].try_into().unwrap()) as usize;
    let received: Value = serde_json::from_slice(&payload[4..4 + length]).unwrap();
    assert_eq!(received, metadata);
    assert_eq!(&payload[4 + length..], jpeg);
    if revoke_after_first || unready_after_first {
      if revoke_after_first {
        std::fs::remove_file(&verifier_path).unwrap();
      }
      if unready_after_first {
        unready.store(true, Ordering::Release);
      }
      allow_second.store(true, Ordering::Release);
      let (opcode, close) = ws_payload(&mut client);
      assert_eq!(opcode, 8);
      assert_eq!(close, 1012u16.to_be_bytes());
    } else {
      allow_second.store(true, Ordering::Release);
      let (opcode, error) = ws_payload(&mut client);
      assert_eq!(opcode, 1);
      let error: Value = serde_json::from_slice(&error).unwrap();
      assert_eq!(error["reason_code"], "invalid_metadata");
      let (opcode, close) = ws_payload(&mut client);
      assert_eq!(opcode, 8);
      assert_eq!(close, 1008u16.to_be_bytes());
    }
    drop(client);
    worker.join().unwrap();
    unready.store(false, Ordering::Release);
    let (mut stale_client, mut stale_server) = UnixStream::pair().unwrap();
    let stale_request = Request {
      method: "GET".into(),
      path: "/api/eos/v2/vision/owner-ws".into(),
      body: vec![],
      headers: BTreeMap::from([
        ("upgrade".into(), "websocket".into()),
        ("connection".into(), "Upgrade".into()),
        ("sec-websocket-version".into(), "13".into()),
        (
          "sec-websocket-key".into(),
          "dGhlIHNhbXBsZSBub25jZQ==".into(),
        ),
      ]),
    };
    let stale_socket = socket.clone();
    let stale_attest = attest.clone();
    let stale_worker = std::thread::spawn(move || {
      serve_owner(
        &mut stale_server,
        &stale_request,
        &stale_socket,
        &stale_attest,
        "eos/interface/gate/front",
        &|| true,
      )
      .unwrap()
    });
    stale_client
      .set_read_timeout(Some(Duration::from_secs(2)))
      .unwrap();
    let mut headers = BufReader::new(stale_client.try_clone().unwrap());
    let mut line = String::new();
    headers.read_line(&mut line).unwrap();
    assert!(line.starts_with("HTTP/1.1 101"));
    while line != "\r\n" {
      line.clear();
      headers.read_line(&mut line).unwrap();
    }
    drop(headers);
    let stale_subscribe = json!({"op":"subscribe","gate_path":"eos/interface/gate/front",
      "revision":"b".repeat(64),"target_fps":30,"max_width_px":1280,"jpeg_quality":80,
      "after_generation":1,"after_runtime_stream_id":"22222222-2222-4222-8222-222222222222"})
    .to_string();
    stale_client.write_all(&[0x81, 0xfe]).unwrap();
    stale_client
      .write_all(&(stale_subscribe.len() as u16).to_be_bytes())
      .unwrap();
    stale_client.write_all(&[1, 2, 3, 4]).unwrap();
    stale_client
      .write_all(
        &stale_subscribe
          .as_bytes()
          .iter()
          .enumerate()
          .map(|(i, b)| b ^ [1, 2, 3, 4][i % 4])
          .collect::<Vec<_>>(),
      )
      .unwrap();
    let (opcode, error) = ws_payload(&mut stale_client);
    assert_eq!(opcode, 1);
    let error: Value = serde_json::from_slice(&error).unwrap();
    assert_eq!(error["reason_code"], "cursor_stale");
    let (opcode, close) = ws_payload(&mut stale_client);
    assert_eq!(opcode, 8);
    assert_eq!(close, 1008u16.to_be_bytes());
    drop(stale_client);
    stale_worker.join().unwrap();
    done.store(true, Ordering::Relaxed);
    control.join().unwrap();
    std::fs::remove_dir_all(root).unwrap();
  }

  #[test]
  fn owner_socket_subscribes_then_rejects_duplicate_generation() {
    owner_socket_case(1, false, false);
  }

  #[test]
  fn owner_socket_rejects_regressing_generation() {
    owner_socket_case(0, false, false);
  }

  #[test]
  fn owner_socket_closes_when_binding_is_revoked_while_eos_remains_ready() {
    owner_socket_case(2, true, false);
  }

  #[test]
  fn owner_socket_closes_when_live_attestation_is_unready() {
    owner_socket_case(2, false, true);
  }

  #[test]
  fn owner_frame_requires_exact_attested_delivery_and_jpeg_digest() {
    let jpeg = [0xff, 0xd8, 0xff, 0xd9];
    let attest = json!({"active_revision":"a".repeat(64),"projection_revision":"b".repeat(64),
      "runtime_stream_id":"11111111-1111-4111-8111-111111111111",
      "source_path":"eos/hardware/camera/front","gate_path":"eos/interface/gate/front"});
    let mut metadata = json!({"graph_revision":attest["active_revision"],
      "projection_revision":attest["projection_revision"],"runtime_stream_id":attest["runtime_stream_id"],
      "source_path":attest["source_path"],"gate_path":attest["gate_path"],
      "source_frame_id":"src-1","gate_generation":1,"width_px":1,"height_px":1,
      "render":{"max_width_px":1280,"jpeg_quality":80},
      "jpeg_sha256":format!("{:x}", Sha256::digest(jpeg))});
    assert_eq!(
      validate_owner_frame(&metadata, &jpeg, &attest, 1, 1280, 80),
      Ok(())
    );
    assert_eq!(
      validate_owner_frame(&metadata, &jpeg, &attest, 2, 1280, 80),
      Err("invalid_metadata")
    );
    metadata["source_frame_id"] = " ".into();
    assert_eq!(
      validate_owner_frame(&metadata, &jpeg, &attest, 1, 1280, 80),
      Err("invalid_metadata")
    );
    metadata["source_frame_id"] = "src-1".into();
    assert_eq!(
      validate_owner_frame(
        &metadata,
        &[0xff, 0xd8, 0, 0xff, 0xd9],
        &attest,
        1,
        1280,
        80
      ),
      Err("jpeg_invalid")
    );
  }

  #[test]
  fn binary_bounds_include_four_byte_length_prefix() {
    assert!(payload_within_bounds(4096, 8 * 1024 * 1024));
    assert_eq!(MAX_FRAME_BYTES, 4 + 4096 + 8 * 1024 * 1024);
    assert!(!payload_within_bounds(4097, 1));
    assert!(!payload_within_bounds(1, 8 * 1024 * 1024 + 1));
    assert!(!payload_within_bounds(0, 1));
    assert!(!payload_within_bounds(1, 0));
  }

  #[test]
  fn masked_subscription_and_binary_frame_preserve_jpeg_bytes() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let address = listener.local_addr().unwrap();
    let worker = std::thread::spawn(move || {
      let (mut stream, _) = listener.accept().unwrap();
      let text = read_text(&mut stream).unwrap();
      assert_eq!(text, br#"{"op":"subscribe"}"#);
      let metadata = br#"{"width_px":2,"height_px":1}"#;
      let jpeg = [0xff, 0xd8, 0xff, 0xd9];
      let mut payload = (metadata.len() as u32).to_le_bytes().to_vec();
      payload.extend_from_slice(metadata);
      payload.extend_from_slice(&jpeg);
      send_frame(&mut stream, 0x2, &payload).unwrap();
    });
    let mut stream = TcpStream::connect(address).unwrap();
    let text = br#"{"op":"subscribe"}"#;
    stream.write_all(&[0x81, 0x80 | text.len() as u8]).unwrap();
    stream.write_all(&[1, 2, 3, 4]).unwrap();
    let masked: Vec<u8> = text
      .iter()
      .enumerate()
      .map(|(i, b)| b ^ [1, 2, 3, 4][i % 4])
      .collect();
    stream.write_all(&masked).unwrap();
    let mut header = [0u8; 2];
    stream.read_exact(&mut header).unwrap();
    assert_eq!(header[0], 0x82);
    let length = if header[1] == 126 {
      let mut size = [0u8; 2];
      stream.read_exact(&mut size).unwrap();
      u16::from_be_bytes(size) as usize
    } else {
      header[1] as usize
    };
    let mut payload = vec![0; length];
    stream.read_exact(&mut payload).unwrap();
    let metadata_len = u32::from_le_bytes(payload[..4].try_into().unwrap()) as usize;
    assert_eq!(
      &payload[4..4 + metadata_len],
      br#"{"width_px":2,"height_px":1}"#
    );
    assert_eq!(&payload[4 + metadata_len..], &[0xff, 0xd8, 0xff, 0xd9]);
    worker.join().unwrap();
  }

  #[test]
  fn stale_revision_and_inactive_gate_are_rejected() {
    let request = json!({"op":"subscribe","gate_path":"gate/front","revision":"old"});
    let discovery =
      json!({"ok":true,"revision":"new","gates":[{"path":"gate/front","source_rate_hz":30.0}]});
    assert_eq!(
      subscription_options(&request, &discovery, "gate/front", "old").unwrap_err(),
      "graph revision changed"
    );
    let inactive = json!({"ok":true,"revision":"old","gates":[]});
    assert_eq!(
      subscription_options(&request, &inactive, "gate/front", "old").unwrap_err(),
      "camera gate is not active"
    );
    let active =
      json!({"ok":true,"revision":"old","gates":[{"path":"gate/front","source_rate_hz":120.0}]});
    let options = subscription_options(&request, &active, "gate/front", "old").unwrap();
    assert_eq!(options.target_fps, 60.0);
  }

  #[test]
  fn browser_can_disconnect_and_subscribe_again() {
    let socket = std::env::temp_dir().join(format!(
      "vision-ws-{}-{}.sock",
      std::process::id(),
      std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos()
    ));
    let unix = UnixListener::bind(&socket).unwrap();
    unix.set_nonblocking(true).unwrap();
    let done = Arc::new(AtomicBool::new(false));
    let stop = done.clone();
    let control = std::thread::spawn(move || {
      while !stop.load(Ordering::Relaxed) {
        match unix.accept() {
          Ok((mut stream, _)) => {
            let mut line = String::new();
            BufReader::new(stream.try_clone().unwrap())
              .read_line(&mut line)
              .unwrap();
            let request: Value = serde_json::from_str(&line).unwrap();
            let response = if request["op"] == "vision_gates" {
              json!({"ok":true,"revision":"revision","gates":[{"path":"gate/front","source_rate_hz":30.0}]})
            } else {
              json!({"ok":true,"changed":false})
            };
            stream
              .write_all(format!("{response}\n").as_bytes())
              .unwrap();
          }
          Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
            std::thread::sleep(Duration::from_millis(2))
          }
          Err(error) => panic!("control listener: {error}"),
        }
      }
    });
    for _ in 0..2 {
      let listener = TcpListener::bind("127.0.0.1:0").unwrap();
      let address = listener.local_addr().unwrap();
      let socket_clone = socket.clone();
      let worker = std::thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let request = Request {
          method: "GET".into(),
          path: "/api/eos/v2/vision/ws".into(),
          body: vec![],
          headers: BTreeMap::from([
            ("upgrade".into(), "websocket".into()),
            ("connection".into(), "Upgrade".into()),
            ("sec-websocket-version".into(), "13".into()),
            (
              "sec-websocket-key".into(),
              "dGhlIHNhbXBsZSBub25jZQ==".into(),
            ),
          ]),
        };
        serve(&mut stream, &request, &socket_clone).unwrap();
      });
      let mut client = TcpStream::connect(address).unwrap();
      client
        .set_read_timeout(Some(Duration::from_secs(2)))
        .unwrap();
      let mut headers = BufReader::new(client.try_clone().unwrap());
      let mut line = String::new();
      headers.read_line(&mut line).unwrap();
      assert!(line.starts_with("HTTP/1.1 101"));
      while line != "\r\n" {
        line.clear();
        headers.read_line(&mut line).unwrap();
      }
      let payload = br#"{"op":"subscribe","gate_path":"gate/front","revision":"revision"}"#;
      client
        .write_all(&[0x81, 0x80 | payload.len() as u8, 1, 2, 3, 4])
        .unwrap();
      let masked: Vec<u8> = payload
        .iter()
        .enumerate()
        .map(|(i, b)| b ^ [1, 2, 3, 4][i % 4])
        .collect();
      client.write_all(&masked).unwrap();
      let mut header = [0; 2];
      client.read_exact(&mut header).unwrap();
      assert_eq!(header[0], 0x81);
      let mut text = vec![0; header[1] as usize];
      client.read_exact(&mut text).unwrap();
      assert_eq!(
        serde_json::from_slice::<Value>(&text).unwrap()["type"],
        "subscribed"
      );
      drop(headers);
      drop(client);
      worker.join().unwrap();
    }
    done.store(true, Ordering::Relaxed);
    control.join().unwrap();
    std::fs::remove_file(socket).unwrap();
  }
}
