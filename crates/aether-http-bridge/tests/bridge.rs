use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::os::unix::net::UnixListener;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStderr, Command, Stdio};
use std::sync::Mutex;
use std::time::Duration;

struct Bridge {
  child: Child,
  address: String,
  socket: PathBuf,
  stderr: Mutex<BufReader<ChildStderr>>,
}

#[test]
fn capabilities_advertise_owner_vision_uds() {
  let output = Command::new(env!("CARGO_BIN_EXE_aether-http-bridge"))
    .arg("--capabilities")
    .output()
    .unwrap();
  assert!(output.status.success());
  let value: serde_json::Value = serde_json::from_slice(&output.stdout).unwrap();
  assert!(value["features"]
    .as_array()
    .unwrap()
    .iter()
    .any(|feature| feature == "vision-owner-uds-v1"));
}

impl Bridge {
  fn start(socket: PathBuf) -> Self {
    Self::start_with_ui_port(socket, None)
  }

  fn start_with_ui_port(socket: PathBuf, ui_port: Option<u16>) -> Self {
    Self::start_with_grant(socket, ui_port, None)
  }

  fn start_with_grant(socket: PathBuf, ui_port: Option<u16>, grant: Option<&Path>) -> Self {
    let reservation = TcpListener::bind("127.0.0.1:0").unwrap();
    let address = reservation.local_addr().unwrap().to_string();
    drop(reservation);
    let mut command = Command::new(env!("CARGO_BIN_EXE_aether-http-bridge"));
    command
      .arg("--socket")
      .arg(&socket)
      .arg("--listen")
      .arg(&address)
      .stderr(Stdio::piped());
    if let Some(port) = ui_port {
      command.env("ISOSUI_PORT", port.to_string());
    }
    if let Some(grant) = grant {
      command.env("AETHER_BRIDGE_CONTROL_GRANT_FILE", grant);
    }
    let mut child = command.spawn().unwrap();
    let mut stderr = BufReader::new(child.stderr.take().unwrap());
    let mut line = String::new();
    stderr.read_line(&mut line).unwrap();
    let started: serde_json::Value = serde_json::from_str(&line).unwrap();
    assert_eq!(started["event_code"], "bridge_starting");
    line.clear();
    stderr.read_line(&mut line).unwrap();
    let ready: serde_json::Value = serde_json::from_str(&line).unwrap();
    assert_eq!(ready["event_code"], "bridge_ready");
    Self {
      child,
      address,
      socket,
      stderr: Mutex::new(stderr),
    }
  }

  fn request(&self, input: &str) -> String {
    let mut stream = TcpStream::connect(&self.address).unwrap();
    stream
      .set_read_timeout(Some(Duration::from_secs(2)))
      .unwrap();
    stream.write_all(input.as_bytes()).unwrap();
    let mut output = String::new();
    stream.read_to_string(&mut output).unwrap();
    output
  }

  fn event(&self) -> serde_json::Value {
    let mut line = String::new();
    self.stderr.lock().unwrap().read_line(&mut line).unwrap();
    serde_json::from_str(&line).unwrap()
  }
}

#[test]
fn browser_cannot_supply_grant_and_bridge_keeps_operator_grant_private() {
  let root = std::env::temp_dir().join(format!(
    "aether-bridge-grant-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  std::fs::create_dir(&root).unwrap();
  std::fs::set_permissions(&root, std::fs::Permissions::from_mode(0o700)).unwrap();
  let grant = root.join("operator.grant");
  let secret = "12".repeat(32);
  std::fs::write(&grant, format!("{secret}\n")).unwrap();
  std::fs::set_permissions(&grant, std::fs::Permissions::from_mode(0o600)).unwrap();
  let socket = root.join("eos.sock");
  let listener = UnixListener::bind(&socket).unwrap();
  let bridge = Bridge::start_with_grant(socket.clone(), None, Some(&grant));
  let body = r#"{"op":"primitive_typed","authority_grant":"malicious"}"#;
  let denied = bridge.request(&format!(
    "POST /api/eos/v2/control HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\n\r\n{body}",
    body.len()
  ));
  assert!(denied.starts_with("HTTP/1.1 403 Forbidden\r\n"));
  let public_grant = bridge.request(&format!(
    "GET /api/eos/v2/vision/gates HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {}\r\n\r\n",
    "11".repeat(32)
  ));
  assert!(public_grant.starts_with("HTTP/1.1 403 Forbidden\r\n"));
  assert!(public_grant.contains("owner_grant_denied"));
  let server = std::thread::spawn(move || {
    let (mut stream, _) = listener.accept().unwrap();
    let mut line = String::new();
    BufReader::new(&mut stream).read_line(&mut line).unwrap();
    let value: serde_json::Value = serde_json::from_str(&line).unwrap();
    assert_eq!(value["authority_grant"], secret);
    assert_eq!(value["op"], "primitive_typed");
    stream
      .write_all(b"{\"ok\":false,\"error\":\"test rejection\"}\n")
      .unwrap();
  });
  let body = r#"{"op":"primitive_typed","header":{"payload_length_bytes":2},"payload":{}}"#;
  let response = bridge.request(&format!(
    "POST /api/eos/v2/control HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\n\r\n{body}",
    body.len()
  ));
  assert!(response.starts_with("HTTP/1.1 200 OK\r\n"));
  assert!(!response.contains(&"12".repeat(32)));
  server.join().unwrap();
  std::fs::write(&grant, "tampered\n").unwrap();
  let after_tamper = bridge.request(&format!(
    "POST /api/eos/v2/control HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\n\r\n{body}",
    body.len()
  ));
  assert!(after_tamper.starts_with("HTTP/1.1 503 Service Unavailable\r\n"));
  drop(bridge);
  std::fs::remove_dir_all(root).unwrap();
}

impl Drop for Bridge {
  fn drop(&mut self) {
    let _ = self.child.kill();
    let _ = self.child.wait();
    let _ = std::fs::remove_file(&self.socket);
  }
}

#[test]
fn test_control_bridge_forwards_one_request_and_reports_boundaries() {
  let socket = std::env::temp_dir().join(format!(
    "aether-bridge-{}-{}.sock",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  let listener = UnixListener::bind(&socket).unwrap();
  let bridge = Bridge::start(socket);
  let server = std::thread::spawn(move || {
    let (mut stream, _) = listener.accept().unwrap();
    let mut line = String::new();
    BufReader::new(&mut stream).read_line(&mut line).unwrap();
    assert_eq!(line, "{\"op\":\"discover\"}\n");
    stream.write_all(b"{\"ok\":true,\"nodes\":[]}\n").unwrap();
  });

  let body = "{\"op\":\"discover\"}";
  let response = bridge.request(&format!(
    "POST /api/eos/v2/control HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\n\r\n{body}",
    body.len()
  ));
  assert!(response.starts_with("HTTP/1.1 200 OK\r\n"));
  assert!(response.ends_with("{\"nodes\":[],\"ok\":true}"));
  server.join().unwrap();

  let bad = bridge
    .request("POST /api/eos/v2/control HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\n\r\n{");
  assert!(bad.starts_with("HTTP/1.1 400 Bad Request\r\n"));
  let missing = bridge.request("GET /api/eos/v2/missing HTTP/1.1\r\nHost: localhost\r\n\r\n");
  assert!(missing.starts_with("HTTP/1.1 404 Not Found\r\n"));
  let unavailable = bridge.request("GET /api/eos/v2/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  assert!(unavailable.starts_with("HTTP/1.1 502 Bad Gateway\r\n"));
}

#[test]
fn browser_origins_get_scoped_cors_and_cannot_dispatch_raw_hardware() {
  let socket = std::env::temp_dir().join(format!(
    "aether-bridge-cors-{}-{}.sock",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  let listener = UnixListener::bind(&socket).unwrap();
  let bridge = Bridge::start(socket);
  let preflight = bridge.request(&format!(
        "OPTIONS /api/eos/v2/control HTTP/1.1\r\nHost: {}\r\nOrigin: https://www.os-island.com\r\nAccess-Control-Request-Method: POST\r\nAccess-Control-Request-Headers: content-type\r\nAccess-Control-Request-Private-Network: true\r\n\r\n",
        bridge.address
    ));
  assert!(preflight.starts_with("HTTP/1.1 204 No Content\r\n"));
  assert!(preflight.contains("Access-Control-Allow-Origin: https://www.os-island.com"));
  assert!(preflight.contains("Access-Control-Allow-Private-Network: true"));

  let body = r#"{"op":"dispatch"}"#;
  let denied = bridge.request(&format!(
        "POST /api/eos/v2/control HTTP/1.1\r\nHost: {}\r\nOrigin: http://127.0.0.1:16001\r\nContent-Length: {}\r\n\r\n{body}",
        bridge.address,
        body.len()
    ));
  assert!(denied.starts_with("HTTP/1.1 403 Forbidden\r\n"));

  let invalid_origin = bridge.request(&format!(
        "POST /api/eos/v2/control HTTP/1.1\r\nHost: {}\r\nOrigin: https://evil.example\r\nContent-Length: {}\r\n\r\n{body}",
        bridge.address,
        body.len()
    ));
  assert!(invalid_origin.starts_with("HTTP/1.1 403 Forbidden\r\n"));

  let denied_vision = bridge.request(&format!(
        "GET /api/eos/v2/vision/ws HTTP/1.1\r\nHost: {}\r\nOrigin: https://evil.example\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
        bridge.address
    ));
  assert!(denied_vision.starts_with("HTTP/1.1 403 Forbidden\r\n"));

  let legacy_body = r#"{"op":"skill_result","execution_id":"demo-1"}"#;
  let legacy = bridge.request(&format!(
        "POST /api/eos/v2/control HTTP/1.1\r\nHost: {}\r\nOrigin: http://127.0.0.1:16001\r\nContent-Length: {}\r\n\r\n{legacy_body}",
        bridge.address,
        legacy_body.len()
    ));
  assert!(legacy.starts_with("HTTP/1.1 403 Forbidden\r\n"));

  let server = std::thread::spawn(move || {
    let (mut stream, _) = listener.accept().unwrap();
    let mut line = String::new();
    BufReader::new(&mut stream).read_line(&mut line).unwrap();
    assert_eq!(line, "{\"op\":\"discover\"}\n");
    stream.write_all(b"{\"ok\":true,\"nodes\":[]}\n").unwrap();
  });
  let body = r#"{"op":"discover"}"#;
  let allowed = bridge.request(&format!(
        "POST /api/eos/v2/control HTTP/1.1\r\nHost: {}\r\nOrigin: http://127.0.0.1:16001\r\nContent-Length: {}\r\n\r\n{body}",
        bridge.address,
        body.len()
    ));
  assert!(allowed.starts_with("HTTP/1.1 200 OK\r\n"));
  assert!(allowed.contains("Access-Control-Allow-Origin: http://127.0.0.1:16001"));
  server.join().unwrap();
}

#[test]
fn ui_port_override_changes_only_the_approved_local_origin() {
  let socket = std::env::temp_dir().join(format!(
    "aether-bridge-port-{}-{}.sock",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  let bridge = Bridge::start_with_ui_port(socket, Some(18001));
  let request = |origin: &str| {
    bridge.request(&format!(
            "OPTIONS /api/eos/v2/control HTTP/1.1\r\nHost: {}\r\nOrigin: {origin}\r\nAccess-Control-Request-Method: POST\r\nAccess-Control-Request-Headers: content-type\r\n\r\n",
            bridge.address
        ))
  };
  assert!(request("http://127.0.0.1:18001").starts_with("HTTP/1.1 204"));
  assert!(request("http://127.0.0.1:16001").starts_with("HTTP/1.1 403"));
}

#[test]
fn browser_status_is_read_only_and_forwarded() {
  let socket = std::env::temp_dir().join(format!(
    "aether-bridge-status-{}-{}.sock",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  let listener = UnixListener::bind(&socket).unwrap();
  let bridge = Bridge::start(socket);
  let server = std::thread::spawn(move || {
    let (mut stream, _) = listener.accept().unwrap();
    let mut line = String::new();
    BufReader::new(&mut stream).read_line(&mut line).unwrap();
    assert_eq!(
      line,
      "{\"node_path\":\"cores/eos/layers/hardware/groups/base/nodes/drive\",\"op\":\"status\"}\n"
    );
    stream
      .write_all(b"{\"ok\":true,\"status\":{\"state\":\"stopped\",\"active_command_id\":null}}\n")
      .unwrap();
  });
  let body = r#"{"op":"status","node_path":"cores/eos/layers/hardware/groups/base/nodes/drive"}"#;
  let response = bridge.request(&format!(
        "POST /api/eos/v2/control HTTP/1.1\r\nHost: {}\r\nOrigin: https://www.os-island.com\r\nContent-Length: {}\r\n\r\n{body}",
        bridge.address,
        body.len()
    ));
  assert!(response.starts_with("HTTP/1.1 200 OK\r\n"));
  assert!(response.contains("\"state\":\"stopped\""));
  server.join().unwrap();
}

#[test]
fn browser_result_rewrites_only_exact_public_request_and_keeps_grant_private() {
  let root = std::env::temp_dir().join(format!(
    "aether-browser-result-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  std::fs::create_dir(&root).unwrap();
  std::fs::set_permissions(&root, std::fs::Permissions::from_mode(0o700)).unwrap();
  let grant = root.join("operator.grant");
  let secret = "ab".repeat(32);
  std::fs::write(&grant, format!("{secret}\n")).unwrap();
  std::fs::set_permissions(&grant, std::fs::Permissions::from_mode(0o600)).unwrap();
  let socket = root.join("eos.sock");
  let listener = UnixListener::bind(&socket).unwrap();
  let bridge = Bridge::start_with_grant(socket, None, Some(&grant));
  let revision = "a".repeat(64);
  let post = |body: &str| {
    bridge.request(&format!(
      "POST /api/eos/v2/control HTTP/1.1\r\nHost: localhost\r\nOrigin: http://127.0.0.1:16001\r\nContent-Length: {}\r\n\r\n{body}",
      body.len()
    ))
  };
  for body in [
    format!(
      r#"{{"op":"result","command_id":"x","active_revision":"{revision}","target_path":"forged"}}"#
    ),
    format!(r#"{{"op":"result","command_id":"","active_revision":"{revision}"}}"#),
    r#"{"op":"result","command_id":"x","active_revision":"ABC"}"#.to_string(),
    format!(
      r#"{{"op":"result","command_id":"x","command_id":"y","active_revision":"{revision}"}}"#
    ),
  ] {
    let response = post(&body);
    assert!(response.starts_with("HTTP/1.1 400 Bad Request"));
    assert!(response.contains("invalid result request"));
  }
  let private = post(&format!(
    r#"{{"op":"primitive_result","command_id":"x","active_revision":"{revision}"}}"#
  ));
  assert!(private.starts_with("HTTP/1.1 403 Forbidden"));
  let forged = post(&format!(
    r#"{{"op":"result","command_id":"x","active_revision":"{revision}","authority_grant":"forged"}}"#
  ));
  assert!(forged.starts_with("HTTP/1.1 403 Forbidden"));
  let server_secret = secret.clone();
  let server = std::thread::spawn(move || {
    for status in [
      "InProgress",
      "Completed",
      "Failed",
      "Cancelled",
      "Rejected",
      "OutcomeUnknown",
    ] {
      let (mut stream, _) = listener.accept().unwrap();
      let mut line = String::new();
      BufReader::new(&mut stream).read_line(&mut line).unwrap();
      let value: serde_json::Value = serde_json::from_str(&line).unwrap();
      assert_eq!(value["op"], "primitive_result");
      assert_eq!(value["command_id"], "x");
      assert_eq!(value["authority_grant"], server_secret);
      assert_eq!(value.as_object().unwrap().len(), 4);
      let reply =
        format!(r#"{{"ok":true,"result":{{"command_id":"x","status":"{status}","reason":null}}}}"#)
          + "\n";
      stream.write_all(reply.as_bytes()).unwrap();
    }
  });
  let body = format!(r#"{{"op":"result","command_id":"x","active_revision":"{revision}"}}"#);
  for status in [
    "InProgress",
    "Completed",
    "Failed",
    "Cancelled",
    "Rejected",
    "OutcomeUnknown",
  ] {
    let response = post(&body);
    assert!(response.starts_with("HTTP/1.1 200 OK"));
    assert!(response.contains(&format!("\"status\":\"{status}\"")));
    assert!(!response.contains(&secret));
  }
  server.join().unwrap();
  drop(bridge);
  std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn browser_result_bounds_private_response_and_reports_runtime_outage() {
  let root = std::env::temp_dir().join(format!("aether-result-bound-{}", std::process::id()));
  std::fs::create_dir_all(&root).unwrap();
  std::fs::set_permissions(&root, std::fs::Permissions::from_mode(0o700)).unwrap();
  let grant = root.join("operator.grant");
  std::fs::write(&grant, format!("{}\n", "cd".repeat(32))).unwrap();
  std::fs::set_permissions(&grant, std::fs::Permissions::from_mode(0o600)).unwrap();
  let socket = root.join("eos.sock");
  let listener = UnixListener::bind(&socket).unwrap();
  let bridge = Bridge::start_with_grant(socket, None, Some(&grant));
  let body = format!(
    r#"{{"op":"result","command_id":"x","active_revision":"{}"}}"#,
    "a".repeat(64)
  );
  let post = || {
    bridge.request(&format!(
      "POST /api/eos/v2/control HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\n\r\n{body}",
      body.len()
    ))
  };
  let server = std::thread::spawn(move || {
    let (mut stream, _) = listener.accept().unwrap();
    let mut line = String::new();
    BufReader::new(&mut stream).read_line(&mut line).unwrap();
    stream
      .write_all(format!("{{\"data\":\"{}\"}}\n", "x".repeat(4100)).as_bytes())
      .unwrap();
  });
  let oversized = post();
  assert!(oversized.starts_with("HTTP/1.1 502 Bad Gateway"));
  assert!(oversized.contains("result unavailable"));
  server.join().unwrap();
  let outage = post();
  assert!(outage.starts_with("HTTP/1.1 502 Bad Gateway"));
  assert!(outage.contains("runtime unavailable"));
  drop(bridge);
  std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn typed_browser_float_length_is_normalized_for_private_ipc() {
  let root = std::env::temp_dir().join(format!(
    "aether-typed-length-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  std::fs::create_dir(&root).unwrap();
  std::fs::set_permissions(&root, std::fs::Permissions::from_mode(0o700)).unwrap();
  let grant = root.join("operator.grant");
  let secret = "ef".repeat(32);
  std::fs::write(&grant, format!("{secret}\n")).unwrap();
  std::fs::set_permissions(&grant, std::fs::Permissions::from_mode(0o600)).unwrap();
  let socket = root.join("eos.sock");
  let listener = UnixListener::bind(&socket).unwrap();
  let bridge = Bridge::start_with_grant(socket, None, Some(&grant));
  let body = r#"{"op":"primitive_typed","header":{"data_type_id":"base_movement","data_type_version":"1.0","primitive_name":"base.velocity","primitive_version":"1.0.0","source_path":"eos/dispatcher/base/velocity","target_path":"eos/hardware/base/drive","active_revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","command_id":"small-float-1","sequence":1,"deadline_monotonic_ns":5000000000,"payload_length_bytes":84,"codec":"json_v1"},"payload":{"type":"body_velocity","x_m_s":0.000001,"y_m_s":0,"yaw_rad_s":0,"valid_for_ms":100}}"#;
  let expected: serde_json::Value = serde_json::from_str(body).unwrap();
  assert_eq!(serde_json::to_vec(&expected["payload"]).unwrap().len(), 80);
  let server = std::thread::spawn(move || {
    let (mut stream, _) = listener.accept().unwrap();
    let mut line = String::new();
    BufReader::new(&mut stream).read_line(&mut line).unwrap();
    let private: serde_json::Value = serde_json::from_str(&line).unwrap();
    assert_eq!(private["op"], "primitive_typed");
    assert_eq!(private["header"]["payload_length_bytes"], 80);
    assert_eq!(private["header"]["command_id"], "small-float-1");
    assert_eq!(private["header"]["target_path"], "eos/hardware/base/drive");
    assert_eq!(private["payload"], expected["payload"]);
    assert_eq!(private["authority_grant"], secret);
    stream.write_all(b"{\"ok\":true,\"result\":{\"command_id\":\"small-float-1\",\"status\":\"Completed\",\"reason\":null}}\n").unwrap();
  });
  let response = bridge.request(&format!(
    "POST /api/eos/v2/control HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\n\r\n{body}",
    body.len()
  ));
  assert!(response.starts_with("HTTP/1.1 200 OK"));
  assert!(response.contains("small-float-1"));
  server.join().unwrap();
  drop(bridge);
  std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn invalid_typed_payload_lengths_fail_before_grant_or_private_ipc() {
  let root = std::env::temp_dir().join(format!("aether-typed-invalid-{}", std::process::id()));
  std::fs::create_dir_all(&root).unwrap();
  let socket = root.join("eos.sock");
  let listener = UnixListener::bind(&socket).unwrap();
  listener.set_nonblocking(true).unwrap();
  let bridge = Bridge::start(socket);
  let post = |body: &str| {
    bridge.request(&format!(
      "POST /api/eos/v2/control HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\n\r\n{body}",
      body.len()
    ))
  };
  let huge = "x".repeat(16 * 1024);
  for body in [
    r#"{"op":"primitive_typed","header":{},"payload":{}}"#.to_string(),
    r#"{"op":"primitive_typed","header":{"payload_length_bytes":0},"payload":{}}"#.to_string(),
    r#"{"op":"primitive_typed","header":{"payload_length_bytes":16385},"payload":{}}"#.to_string(),
    r#"{"op":"primitive_typed","header":{"payload_length_bytes":1.5},"payload":{}}"#.to_string(),
    r#"{"op":"primitive_typed","header":{"payload_length_bytes":2}}"#.to_string(),
    r#"{"op":"primitive_typed","header":{"payload_length_bytes":4},"payload":null}"#.to_string(),
    format!(
      r#"{{"op":"primitive_typed","header":{{"payload_length_bytes":2}},"payload":{{"data":"{huge}"}}}}"#
    ),
  ] {
    let response = post(&body);
    assert!(
      response.starts_with("HTTP/1.1 400 Bad Request"),
      "{response}"
    );
    assert!(response.contains("invalid typed request"), "{response}");
    assert!(
      listener.accept().is_err(),
      "invalid request reached private IPC"
    );
  }
  let valid = post(r#"{"op":"primitive_typed","header":{"payload_length_bytes":2},"payload":{}}"#);
  assert!(valid.starts_with("HTTP/1.1 503 Service Unavailable"));
  assert!(valid.contains("operator control unavailable"));
  assert!(listener.accept().is_err());
  drop(bridge);
  std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn browser_events_correlate_long_utf8_id_and_reject_contradictory_result() {
  let root = std::env::temp_dir().join(format!(
    "aether-result-events-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  std::fs::create_dir(&root).unwrap();
  std::fs::set_permissions(&root, std::fs::Permissions::from_mode(0o700)).unwrap();
  let grant = root.join("operator.grant");
  let secret = "ef".repeat(32);
  std::fs::write(&grant, format!("{secret}\n")).unwrap();
  std::fs::set_permissions(&grant, std::fs::Permissions::from_mode(0o600)).unwrap();
  let socket = root.join("eos.sock");
  let listener = UnixListener::bind(&socket).unwrap();
  let bridge = Bridge::start_with_grant(socket, None, Some(&grant));
  let id = "é".repeat(64);
  assert_eq!(id.len(), 128);
  let server_id = id.clone();
  let server_secret = secret.clone();
  let server = std::thread::spawn(move || {
    for (op, status, reply_id) in [
      ("primitive_typed", "InProgress", server_id.as_str()),
      ("primitive_result", "InProgress", server_id.as_str()),
      ("primitive_result", "Completed", server_id.as_str()),
      ("primitive_result", "Completed", "wrong-id"),
    ] {
      let (mut stream, _) = listener.accept().unwrap();
      let mut line = String::new();
      BufReader::new(&mut stream).read_line(&mut line).unwrap();
      let request: serde_json::Value = serde_json::from_str(&line).unwrap();
      assert_eq!(request["op"], op);
      let request_id = if op == "primitive_typed" {
        &request["header"]["command_id"]
      } else {
        &request["command_id"]
      };
      assert_eq!(request_id, &server_id);
      assert_eq!(request["authority_grant"], server_secret);
      stream.write_all(format!("{}\n", serde_json::json!({"ok":true,"result":{"command_id":reply_id,"status":status,"reason":"private secret"}})).as_bytes()).unwrap();
    }
  });
  let revision = "a".repeat(64);
  let typed = serde_json::json!({"op":"primitive_typed","header":{"command_id":id,"active_revision":revision,"payload_length_bytes":2},"payload":{}}).to_string();
  let followup =
    serde_json::json!({"op":"result","command_id":id,"active_revision":revision}).to_string();
  let post = |body: &str| {
    bridge.request(&format!(
      "POST /api/eos/v2/control HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\n\r\n{body}",
      body.len()
    ))
  };
  for body in [&typed, &followup, &followup] {
    assert!(post(body).starts_with("HTTP/1.1 200 OK"));
  }
  let bad = post(&followup);
  assert!(bad.starts_with("HTTP/1.1 502 Bad Gateway"));
  assert!(bad.contains("result unavailable"));
  let first = bridge.event();
  let second = bridge.event();
  for (event, status, phase) in [
    (&first, "InProgress", "submit"),
    (&second, "Completed", "followup"),
  ] {
    assert_eq!(event["source"], "bridge");
    assert_eq!(event["event_code"], "bridge_primitive_result");
    assert_eq!(event["node_path"], serde_json::Value::Null);
    assert_eq!(
      event["fields"],
      serde_json::json!({"status":status,"phase":phase})
    );
    assert_eq!(
      event["correlation_id"],
      "h_0bc44b0f1cfea7f694f47673b4eb2c24f1c208e0e50a0555278ad3a0ad4154"
    );
    let raw = event.to_string();
    assert!(!raw.contains(&id));
    assert!(!raw.contains(&secret));
    assert!(!raw.contains("private secret"));
  }
  server.join().unwrap();
  drop(bridge);
  std::fs::remove_dir_all(root).unwrap();
}
