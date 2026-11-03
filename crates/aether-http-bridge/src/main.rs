//! Loopback HTTP control bridge for the EOS v2 process socket.

use std::env;
use std::fs::{self, OpenOptions};
use std::io::Read;
use std::net::{IpAddr, SocketAddr, TcpListener, TcpStream};
use std::os::unix::fs::{MetadataExt, PermissionsExt};
use std::os::unix::fs::OpenOptionsExt;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use aether_common::events::{
  Event, EventSink, NonblockingStderr, Reason, Source, Stage, local_run_id, valid_digest,
  valid_run_id,
};
use aether_common::time::SystemClock;
use aether_control::http::{read_request, reply, reply_with_headers};
use serde::Deserialize;
use serde_json::{json, Value};

mod vision_ws;
mod owner_vision;
mod result_events;
const MAX_RESULT_RESPONSE_BYTES: usize = 4 * 1024;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct PublicResultRequest {
  op: String,
  command_id: String,
  active_revision: String,
}
static TERMINATE: AtomicBool = AtomicBool::new(false);
static EVENT_INITIALIZED: AtomicBool = AtomicBool::new(false);
extern "C" fn on_signal(_: libc::c_int) {
  TERMINATE.store(true, Ordering::SeqCst);
}

fn main() {
  if env::args().nth(1).as_deref() == Some("--capabilities") {
    println!(
      "{}",
      json!({"product_version":env!("CARGO_PKG_VERSION"),"graph_version":aether_node_profile::GRAPH_VERSION,
            "node_type_version":"1.0","binary":"aether-http-bridge",
            "features":["short-paths","node-types","data-idl","primitive-binding","vision-ws","vision-owner-uds-v1"]})
    );
    return;
  }
  if launch().is_err() {
    if !EVENT_INITIALIZED.load(Ordering::SeqCst) {
      eprintln!("aether-http-bridge: invalid launch arguments");
    }
    std::process::exit(1);
  }
}

fn launch() -> Result<(), Box<dyn std::error::Error>> {
  let mut args = env::args().skip(1);
  let mut socket = None;
  let mut listen = None;
  let mut run_id = None;
  let mut owner_vision_socket = None;
  while let Some(flag) = args.next() {
    let value = args.next().ok_or(format!("missing value for {flag}"))?;
    match flag.as_str() {
      "--socket" if socket.is_none() => socket = Some(PathBuf::from(value)),
      "--listen" if listen.is_none() => listen = Some(value.parse::<SocketAddr>()?),
      "--run-id" if run_id.is_none() && valid_run_id(&value) => run_id = Some(value),
      "--owner-vision-socket" if owner_vision_socket.is_none() => {
        owner_vision_socket = Some(PathBuf::from(value))
      }
      _ => return Err(format!("unknown or duplicate option: {flag}").into()),
    }
  }
  let socket = socket.ok_or("--socket is required")?;
  let address: SocketAddr = listen.ok_or("--listen is required")?;
  if !matches!(address.ip(), IpAddr::V4(ip) if ip.is_loopback())
    && !matches!(address.ip(), IpAddr::V6(ip) if ip.is_loopback())
  {
    return Err("HTTP bridge must bind a loopback address".into());
  }
  let owner_vision = if let Some(path) = owner_vision_socket {
    let run_id = run_id
      .as_deref()
      .ok_or("--run-id is required for owner Vision")?;
    let verifier_path = env::var_os("AETHER_BRIDGE_VISION_VERIFIER_FILE")
      .ok_or("AETHER_BRIDGE_VISION_VERIFIER_FILE is required")?;
    Some(owner_vision::OwnerVision::load(
      path,
      PathBuf::from(verifier_path),
      run_id,
      &socket,
    )?)
  } else {
    None
  };
  let sink = Arc::new(EventSink::new(
    Source::Bridge,
    run_id.unwrap_or_else(local_run_id),
    NonblockingStderr::new()?,
    SystemClock,
  )?);
  EVENT_INITIALIZED.store(true, Ordering::SeqCst);
  let _ = sink.emit(Event::Starting, None);
  let grant = match env::var_os("AETHER_BRIDGE_CONTROL_GRANT_FILE") {
    Some(path) => {
      let path = PathBuf::from(path);
      match read_control_grant(&path) {
        Ok(_) => Some(Arc::new(path)),
        Err(_) => {
          let _ = sink.emit(
            Event::StartFailed {
              stage: Stage::Profile,
              reason: Reason::ProfileInvalid,
            },
            None,
          );
          return Err("bridge control grant unavailable".into());
        }
      }
    }
    None => None,
  };
  if run(socket, address, grant, owner_vision, &sink).is_err() {
    let _ = sink.emit(
      Event::StartFailed {
        stage: Stage::Listener,
        reason: Reason::ListenerBindFailed,
      },
      None,
    );
    return Err("bridge listener failed".into());
  }
  Ok(())
}

fn run(
  socket: PathBuf,
  address: SocketAddr,
  grant: Option<Arc<PathBuf>>,
  owner_vision: Option<owner_vision::OwnerVision>,
  sink: &Arc<EventSink<NonblockingStderr, SystemClock>>,
) -> Result<(), Box<dyn std::error::Error>> {
  let listener = TcpListener::bind(address)?;
  listener.set_nonblocking(true)?;
  let owner_listener = owner_vision
    .as_ref()
    .map(|config| config.bind())
    .transpose()?;
  let _owner_socket_guard = owner_vision.as_ref().map(|config| config.guard());
  unsafe {
    if libc::signal(libc::SIGINT, on_signal as *const () as libc::sighandler_t) == libc::SIG_ERR
      || libc::signal(libc::SIGTERM, on_signal as *const () as libc::sighandler_t) == libc::SIG_ERR
    {
      return Err(std::io::Error::last_os_error().into());
    }
  }
  let _ = sink.emit(Event::Ready { node_count: 0 }, None);
  let mut clients: Vec<std::thread::JoinHandle<()>> = Vec::new();
  let result_events = Arc::new(result_events::ResultEvents::default());
  while !TERMINATE.load(Ordering::SeqCst) {
    if let (Some(config), Some(owner_listener)) = (&owner_vision, &owner_listener) {
      match owner_listener.accept() {
        Ok((stream, _)) => {
          clients.retain(|client| !client.is_finished());
          if clients.len() < 16 {
            let config = config.clone();
            let socket = socket.clone();
            clients.push(std::thread::spawn(move || {
              let _ = config.serve(stream, &socket);
            }));
          }
        }
        Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {}
        Err(_) => return Err("owner Vision listener failed".into()),
      }
    }
    match listener.accept() {
      Ok((mut stream, _)) => {
        clients.retain(|client| !client.is_finished());
        if clients.len() >= 16 {
          let _ = reply(
            &mut stream,
            503,
            "application/json",
            b"{\"ok\":false,\"error\":\"busy\"}",
          );
          continue;
        }
        let socket = socket.clone();
        let grant = grant.clone();
        let sink = Arc::clone(sink);
        let result_events = Arc::clone(&result_events);
        clients.push(std::thread::spawn(move || {
          if let Err(error) = serve(
            &mut stream,
            &socket,
            grant.as_deref(),
            &sink,
            &result_events,
          ) {
            let body = json!({"ok":false,"error":error}).to_string();
            let _ = reply(&mut stream, 400, "application/json", body.as_bytes());
          }
        }));
      }
      Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
        std::thread::sleep(Duration::from_millis(20))
      }
      Err(_) => std::thread::sleep(Duration::from_millis(20)),
    }
  }
  let _ = sink.emit(Event::Stopping, None);
  drop(owner_listener);
  if let Some(config) = &owner_vision {
    config.unlink();
  }
  for client in clients {
    let _ = client.join();
  }
  let _ = sink.emit(Event::Stopped, None);
  Ok(())
}

fn read_control_grant(path: &PathBuf) -> Result<String, String> {
  if !path.is_absolute()
    || path.components().any(|part| {
      matches!(
        part,
        std::path::Component::ParentDir | std::path::Component::CurDir
      )
    })
  {
    return Err("unsafe control grant path".into());
  }
  for ancestor in path.ancestors() {
    if fs::symlink_metadata(ancestor)
      .map_err(|_| "control grant unavailable")?
      .file_type()
      .is_symlink()
    {
      return Err("unsafe control grant path".into());
    }
  }
  let parent = path.parent().ok_or("control grant has no parent")?;
  let directory =
    fs::symlink_metadata(parent).map_err(|_| "control grant directory unavailable")?;
  if !directory.file_type().is_dir()
    || directory.permissions().mode() & 0o777 != 0o700
    || directory.uid() != unsafe { libc::geteuid() }
  {
    return Err("unsafe control grant directory".into());
  }
  let file = OpenOptions::new()
    .read(true)
    .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
    .open(path)
    .map_err(|_| "control grant unavailable")?;
  let metadata = file.metadata().map_err(|_| "control grant unavailable")?;
  if !metadata.file_type().is_file()
    || metadata.permissions().mode() & 0o777 != 0o600
    || metadata.uid() != unsafe { libc::geteuid() }
  {
    return Err("unsafe control grant".into());
  }
  let mut bytes = Vec::new();
  file
    .take(66)
    .read_to_end(&mut bytes)
    .map_err(|_| "control grant unavailable")?;
  if bytes.len() != 65 || bytes[64] != b'\n' {
    return Err("invalid control grant".into());
  }
  let value = std::str::from_utf8(&bytes[..64]).map_err(|_| "invalid control grant")?;
  if !valid_digest(value) {
    return Err("invalid control grant".into());
  }
  Ok(value.into())
}

fn serve(
  stream: &mut TcpStream,
  socket: &PathBuf,
  grant: Option<&PathBuf>,
  sink: &EventSink<NonblockingStderr, SystemClock>,
  result_events: &result_events::ResultEvents,
) -> Result<(), String> {
  let request = read_request(stream)?;
  if request.headers.contains_key("authorization") {
    return send(
      stream,
      403,
      json!({"ok":false,"error":"owner_grant_denied"}),
      None,
    );
  }
  let host = request.headers.get("host").ok_or("Host is required")?;
  let port = stream
    .local_addr()
    .map_err(|error| error.to_string())?
    .port();
  let allowed_hosts = [
    stream
      .local_addr()
      .map_err(|error| error.to_string())?
      .to_string(),
    format!("127.0.0.1:{port}"),
    format!("localhost:{port}"),
    "localhost".into(),
    "127.0.0.1".into(),
  ];
  if !allowed_hosts.iter().any(|allowed| allowed == host) {
    return send(
      stream,
      403,
      json!({"ok":false,"error":"invalid Host"}),
      None,
    );
  }
  let origin = request.headers.get("origin").map(String::as_str);
  let isos_port = configured_port("ISOS_HTTP_PORT", 16000)?;
  let ui_port = configured_port("ISOSUI_PORT", 16001)?;
  let allowed_origins = [
    format!("http://127.0.0.1:{isos_port}"),
    format!("http://127.0.0.1:{ui_port}"),
    format!("http://localhost:{ui_port}"),
    "https://www.os-island.com".into(),
    "https://os-island.com".into(),
  ];
  if origin.is_some_and(|origin| !allowed_origins.iter().any(|allowed| allowed == origin))
    || (origin.is_none()
      && request
        .headers
        .get("sec-fetch-site")
        .is_some_and(|value| matches!(value.as_str(), "cross-site" | "same-site")))
  {
    return send(
      stream,
      403,
      json!({"ok":false,"error":"invalid Origin"}),
      None,
    );
  }
  if request.method == "GET" && request.path == "/api/eos/v2/vision/gates" {
    return match aether_control::request(socket, &json!({"op":"vision_gates"})) {
      Ok(mut response) if response.get("ok").and_then(Value::as_bool) == Some(true) => {
        response.as_object_mut().unwrap().remove("ok");
        send(stream, 200, response, origin)
      }
      Ok(response) => send(stream, 502, response, origin),
      Err(error) => send(stream, 502, json!({"ok":false,"error":error}), origin),
    };
  }
  if request.method == "GET" && request.path == "/api/eos/v2/vision/ws" {
    let _ = vision_ws::serve(stream, &request, socket);
    return Ok(());
  }
  if request.method == "OPTIONS" && request.path == "/api/eos/v2/control" {
    if origin.is_none()
      || request
        .headers
        .get("access-control-request-method")
        .map(String::as_str)
        != Some("POST")
      || request
        .headers
        .get("access-control-request-headers")
        .is_some_and(|value| !value.eq_ignore_ascii_case("content-type"))
    {
      return send(
        stream,
        403,
        json!({"ok":false,"error":"invalid preflight"}),
        None,
      );
    }
    let headers = cors_headers(origin, true);
    return reply_with_headers(stream, 204, "application/json", b"", &headers)
      .map_err(|error| error.to_string());
  }
  let control = match (request.method.as_str(), request.path.as_str()) {
    ("GET", "/api/eos/v2/health") => json!({"op":"time"}),
    ("POST", "/api/eos/v2/control") => {
      let mut value: Value = match serde_json::from_slice(&request.body) {
        Ok(value) => value,
        Err(_) => {
          return send(
            stream,
            400,
            json!({"ok":false,"error":"invalid result request"}),
            origin,
          );
        }
      };
      if value.get("authority_grant").is_some() {
        return send(
          stream,
          403,
          json!({"ok":false,"error":"authority is owner supplied"}),
          origin,
        );
      }
      let allowed: &[&str] = &[
        "time",
        "discover",
        "health",
        "status",
        "capture",
        "observe",
        "primitive_typed",
        "result",
      ];
      if !value
        .get("op")
        .and_then(Value::as_str)
        .is_some_and(|op| allowed.contains(&op))
      {
        return send(
          stream,
          403,
          json!({"ok":false,"error":"unsupported control operation"}),
          origin,
        );
      }
      if value.get("op").and_then(Value::as_str) == Some("result") {
        if !valid_public_result(&request.body) {
          return send(
            stream,
            400,
            json!({"ok":false,"error":"invalid result request"}),
            origin,
          );
        }
        value["op"] = Value::String("primitive_result".into());
      }
      if value.get("op").and_then(Value::as_str) == Some("primitive_typed")
        && !normalize_public_typed_payload_length(&mut value)
      {
        return send(
          stream,
          400,
          json!({"ok":false,"error":"invalid typed request"}),
          origin,
        );
      }
      if matches!(
        value.get("op").and_then(Value::as_str),
        Some("primitive_typed" | "primitive_result")
      ) {
        let Some(grant) = grant else {
          return send(
            stream,
            503,
            json!({"ok":false,"error":"operator control unavailable"}),
            origin,
          );
        };
        let grant = match read_control_grant(grant) {
          Ok(value) => value,
          Err(_) => {
            return send(
              stream,
              503,
              json!({"ok":false,"error":"operator control unavailable"}),
              origin,
            )
          }
        };
        value
          .as_object_mut()
          .ok_or("control request must be an object")?
          .insert("authority_grant".into(), Value::String(grant));
      }
      value
    }
    ("GET" | "POST", _) => {
      send(stream, 404, json!({"ok":false,"error":"not found"}), origin)?;
      return Ok(());
    }
    _ => {
      send(
        stream,
        405,
        json!({"ok":false,"error":"method not allowed"}),
        origin,
      )?;
      return Ok(());
    }
  };
  let result_phase = match control.get("op").and_then(Value::as_str) {
    Some("primitive_typed") => Some(result_events::Phase::Submit),
    Some("primitive_result") => Some(result_events::Phase::Followup),
    _ => None,
  };
  let command_id = result_phase.and_then(|phase| result_events::public_command_id(&control, phase));
  if result_phase == Some(result_events::Phase::Followup) {
    return match aether_control::request_with_response_limit(
      socket,
      &control,
      MAX_RESULT_RESPONSE_BYTES + 1,
    ) {
      Ok(response) if response.to_string().len() <= MAX_RESULT_RESPONSE_BYTES => {
        send_control_response(
          stream,
          response,
          origin,
          command_id,
          result_phase,
          sink,
          result_events,
        )
      }
      Ok(_) => send(
        stream,
        502,
        json!({"ok":false,"error":"result unavailable"}),
        origin,
      ),
      Err(error) if error == "control response exceeds bounds" => send(
        stream,
        502,
        json!({"ok":false,"error":"result unavailable"}),
        origin,
      ),
      Err(_) => send(
        stream,
        502,
        json!({"ok":false,"error":"runtime unavailable"}),
        origin,
      ),
    };
  }
  match aether_control::request(socket, &control) {
    Ok(response) => send_control_response(
      stream,
      response,
      origin,
      command_id,
      result_phase,
      sink,
      result_events,
    ),
    Err(error) => send(stream, 502, json!({"ok":false,"error":error}), origin),
  }
}

fn send_control_response<W: std::io::Write, C: aether_common::time::Clock>(
  stream: &mut TcpStream,
  response: Value,
  origin: Option<&str>,
  command_id: Option<&str>,
  phase: Option<result_events::Phase>,
  sink: &EventSink<W, C>,
  result_events: &result_events::ResultEvents,
) -> Result<(), String> {
  if let Some(phase) = phase {
    match result_events::validated_status(&response, command_id) {
      Ok(Some(status)) => {
        result_events.observe(sink, command_id.expect("validated ID"), status, phase)
      }
      Ok(None) => {}
      Err(()) => {
        return send(
          stream,
          502,
          json!({"ok":false,"error":"result unavailable"}),
          origin,
        )
      }
    }
  }
  send(stream, 200, response, origin)
}

fn valid_public_result(bytes: &[u8]) -> bool {
  let Ok(request) = serde_json::from_slice::<PublicResultRequest>(bytes) else {
    return false;
  };
  request.op == "result"
    && !request.command_id.is_empty()
    && request.command_id.len() <= 128
    && valid_digest(&request.active_revision)
}

/// Public byte counts are only bounded hints: this bridge serializes the
/// private IPC request and therefore owns its exact compact payload length.
fn normalize_public_typed_payload_length(value: &mut Value) -> bool {
  let Some(public_length) = value
    .get("header")
    .and_then(Value::as_object)
    .and_then(|header| header.get("payload_length_bytes"))
    .and_then(Value::as_u64)
  else {
    return false;
  };
  if !(1..=16 * 1024).contains(&public_length) {
    return false;
  }
  let Some(payload) = value.get("payload").filter(|payload| payload.is_object()) else {
    return false;
  };
  let Ok(serialized) = serde_json::to_vec(payload) else {
    return false;
  };
  if serialized.is_empty() || serialized.len() > 16 * 1024 {
    return false;
  }
  value["header"]["payload_length_bytes"] = Value::from(serialized.len());
  true
}

fn configured_port(name: &str, default: u16) -> Result<u16, String> {
  match env::var(name) {
    Ok(value) => value
      .parse::<u16>()
      .ok()
      .filter(|port| *port > 0)
      .ok_or_else(|| format!("invalid {name}")),
    Err(env::VarError::NotPresent) => Ok(default),
    Err(error) => Err(error.to_string()),
  }
}

fn cors_headers(origin: Option<&str>, preflight: bool) -> Vec<(&'static str, &str)> {
  let mut headers = Vec::new();
  if let Some(origin) = origin {
    headers.push(("Access-Control-Allow-Origin", origin));
    headers.push(("Vary", "Origin"));
    if preflight {
      headers.push(("Access-Control-Allow-Methods", "GET, POST, OPTIONS"));
      headers.push(("Access-Control-Allow-Headers", "content-type"));
      headers.push(("Access-Control-Allow-Private-Network", "true"));
    }
  }
  headers
}

fn send(
  stream: &mut TcpStream,
  status: u16,
  body: Value,
  origin: Option<&str>,
) -> Result<(), String> {
  let body = body.to_string();
  reply_with_headers(
    stream,
    status,
    "application/json",
    body.as_bytes(),
    &cors_headers(origin, false),
  )
  .map_err(|error| error.to_string())
}
