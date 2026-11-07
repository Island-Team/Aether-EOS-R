//! Run-bound public node event ingress. Payloads carry data, not authority.

use super::*;
use crate::domain::graph::{control_path, edge_admission};
use axum::body::Bytes;
use std::{
  collections::{BTreeMap, BTreeSet},
  net::SocketAddr,
  sync::Mutex,
  time::{Duration, Instant},
};

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct EventRequest {
  schema_version: String,
  source_node: String,
  output: String,
  data_type: String,
  run_id: String,
  active_revision: String,
  payload: KeyState,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct KeyState {
  state: KeyPhase,
  activation_id: String,
  sequence: u64,
}

#[derive(Clone, Copy, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
enum KeyPhase {
  Pressed,
  Refresh,
  Released,
}

struct ActiveKey {
  run_id: String,
  activation_id: String,
  sequence: u64,
  last_event: Instant,
  expires: Instant,
}

#[derive(Default)]
struct EventState {
  active: BTreeMap<(String, String), ActiveKey>,
  completed: BTreeMap<(String, String, String), BTreeSet<String>>,
  owner_sequence: BTreeMap<(String, String, String), u64>,
  routes: BTreeMap<(String, String, String), control_path::ControlPath>,
}

fn state() -> &'static Mutex<EventState> {
  static STORE: OnceLock<Mutex<EventState>> = OnceLock::new();
  STORE.get_or_init(|| Mutex::new(EventState::default()))
}

impl EventState {
  fn advance(
    &mut self,
    deployment: &str,
    request: &EventRequest,
    route: &control_path::ControlPath,
    now: Instant,
  ) -> std::result::Result<u64, Failure> {
    self.active.retain(|(name, _), entry| {
      name != deployment || (entry.run_id == request.run_id && now < entry.expires)
    });
    self
      .owner_sequence
      .retain(|(name, run, _), _| name != deployment || run == &request.run_id);
    self
      .completed
      .retain(|(name, run, _), _| name != deployment || run == &request.run_id);
    let key = (deployment.to_owned(), request.source_node.clone());
    let completed_key = (
      deployment.to_owned(),
      request.run_id.clone(),
      request.source_node.clone(),
    );
    let completed = self.completed.get(&completed_key);
    let active = self.active.get(&key);
    match request.payload.state {
      KeyPhase::Pressed
        if active.is_some()
          || completed.is_some_and(|ids| ids.contains(&request.payload.activation_id)) =>
      {
        return Err(invalid("activation_already_active"));
      }
      KeyPhase::Refresh | KeyPhase::Released
        if !active.is_some_and(|entry| {
          entry.activation_id == request.payload.activation_id
            && request.payload.sequence > entry.sequence
        }) =>
      {
        return Err(invalid("activation_stale"));
      }
      _ => {}
    }
    if request.payload.state == KeyPhase::Pressed
      && completed.is_some_and(|ids| ids.len() >= 65_536)
    {
      return Err(invalid("activation_capacity_exceeded"));
    }
    if request.payload.state == KeyPhase::Refresh
      && active
        .is_some_and(|entry| now.duration_since(entry.last_event) < Duration::from_millis(50))
    {
      return Err(invalid("event_rate_exceeded"));
    }
    let sequence_key = (
      deployment.to_owned(),
      request.run_id.clone(),
      route.binding.clone(),
    );
    let owner_sequence = self.owner_sequence.entry(sequence_key).or_insert(0);
    *owner_sequence = owner_sequence
      .checked_add(1)
      .ok_or_else(|| invalid("owner_sequence_exhausted"))?;
    let sequence = *owner_sequence;
    if request.payload.state == KeyPhase::Released {
      self.active.remove(&key);
      self
        .completed
        .entry(completed_key)
        .or_default()
        .insert(request.payload.activation_id.clone());
    } else {
      self.active.insert(
        key,
        ActiveKey {
          run_id: request.run_id.clone(),
          activation_id: request.payload.activation_id.clone(),
          sequence: request.payload.sequence,
          last_event: now,
          expires: now + Duration::from_millis(route.valid_for_ms),
        },
      );
    }
    Ok(sequence)
  }
}

#[derive(Debug)]
struct Failure {
  status: StatusCode,
  code: &'static str,
  message: &'static str,
}

fn reject(status: StatusCode, code: &'static str, message: &'static str) -> Failure {
  Failure {
    status,
    code,
    message,
  }
}

fn invalid(code: &'static str) -> Failure {
  reject(
    StatusCode::UNPROCESSABLE_ENTITY,
    code,
    "node event rejected",
  )
}

fn admitted_path(
  paths: &Paths,
  graph: &Value,
  revision: &str,
  deployment: &str,
  source: &str,
) -> std::result::Result<control_path::ControlPath, Failure> {
  let key = (
    deployment.to_owned(),
    revision.to_owned(),
    source.to_owned(),
  );
  if let Some(path) = state()
    .lock()
    .map_err(|_| invalid("event_state_unavailable"))?
    .routes
    .get(&key)
  {
    return Ok(path.clone());
  }
  let catalog =
    process::catalog_for_graph(paths, graph).map_err(|_| invalid("node_catalog_unavailable"))?;
  edge_admission::admit(graph, &catalog).map_err(|_| invalid("node_edges_invalid"))?;
  let route = control_path::resolve(graph, source).map_err(|_| invalid("node_route_invalid"))?;
  let mut store = state()
    .lock()
    .map_err(|_| invalid("event_state_unavailable"))?;
  store
    .routes
    .retain(|(name, rev, _), _| name != deployment || rev == revision);
  store.routes.insert(key, route.clone());
  Ok(route)
}

fn read_run(
  paths: &Paths,
  deployment: &str,
  request: &EventRequest,
) -> std::result::Result<(), Failure> {
  let status =
    crate::runtime::manager::manager_status_for_http(paths, deployment).map_err(|_| {
      reject(
        StatusCode::SERVICE_UNAVAILABLE,
        "manager_unavailable",
        "run status unavailable",
      )
    })?;
  if status["state"] != "running"
    || status["run_id_state"] != "known"
    || status["run_id"] != request.run_id
    || status["active_revision"] != request.active_revision
  {
    return Err(reject(
      StatusCode::CONFLICT,
      "run_fence_stale",
      "node event run fence is stale",
    ));
  }
  if !status["processes"].as_array().is_some_and(|entries| {
    entries
      .iter()
      .any(|item| item["id"] == "bridge" && item["state"] == "running")
  }) {
    return Err(reject(
      StatusCode::SERVICE_UNAVAILABLE,
      "owner_bridge_unavailable",
      "owner bridge is unavailable",
    ));
  }
  Ok(())
}

fn validate_request(request: &EventRequest) -> std::result::Result<(), Failure> {
  if request.schema_version != "0.1.0" {
    return Err(reject(
      StatusCode::BAD_REQUEST,
      "unsupported_schema_version",
      "node event schema is invalid",
    ));
  }
  if request.output != "state"
    || request.data_type != "key_state"
    || request.source_node.len() > 200
    || request.source_node.split('/').count() != 4
    || !request.source_node.split('/').all(|segment| {
      !segment.is_empty()
        && segment
          .bytes()
          .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.'))
    })
    || request.run_id.is_empty()
    || request.run_id.len() > 128
    || request.active_revision.len() != 64
    || !request
      .active_revision
      .bytes()
      .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    || request.payload.activation_id.is_empty()
    || request.payload.activation_id.len() > 64
    || !request
      .payload
      .activation_id
      .bytes()
      .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_')
    || request.payload.sequence == 0
  {
    return Err(reject(
      StatusCode::BAD_REQUEST,
      "node_event_invalid",
      "node event fields are invalid",
    ));
  }
  Ok(())
}

fn send_movement(
  endpoint: SocketAddr,
  request: &EventRequest,
  route: &control_path::ControlPath,
  owner_sequence: u64,
) -> std::result::Result<Value, Failure> {
  let released = request.payload.state == KeyPhase::Released;
  let lease = if released { 50 } else { route.valid_for_ms };
  let command_deadline_ms = lease + 500;
  let identity = format!(
    "{}:{}:{}",
    request.run_id, request.payload.activation_id, owner_sequence
  );
  let digest = Sha256::digest(identity.as_bytes());
  let command_id = format!("ui-{}", hex::encode(&digest[..16]));
  let payload = json!({"type":"body_velocity",
    "x_m_s":if released { 0.0 } else { route.x_m_s },
    "y_m_s":if released { 0.0 } else { route.y_m_s },
    "yaw_rad_s":if released { 0.0 } else { route.yaw_rad_s },
    "valid_for_ms":lease});
  let payload_length_bytes = serde_json::to_vec(&payload)
    .map_err(|_| invalid("node_payload_invalid"))?
    .len();
  let body = json!({"op":"primitive_typed","header":{
    "data_type_id":"base_movement","data_type_version":"1.0",
    "primitive_name":"base.velocity","primitive_version":"1.0.0",
    "source_path":route.binding,"target_path":route.hardware,
    "gateway_path":route.eos_gateway,"run_id":request.run_id,
    "active_revision":request.active_revision,"command_id":command_id,
    "sequence":owner_sequence,"deadline_after_ms":command_deadline_ms,
    "payload_length_bytes":payload_length_bytes,"codec":"json_v1","delivery_mode":"stream"
  },"payload":payload});
  let client = reqwest::blocking::Client::builder()
    .connect_timeout(Duration::from_millis(250))
    .timeout(Duration::from_secs(2))
    .build()
    .map_err(|_| invalid("owner_bridge_unavailable"))?;
  let response = client
    .post(format!("http://{endpoint}/api/eos/v2/control"))
    .json(&body)
    .send()
    .map_err(|_| {
      reject(
        StatusCode::SERVICE_UNAVAILABLE,
        "owner_outcome_unknown",
        "owner command outcome is unknown",
      )
    })?;
  let status = response.status();
  let bytes = response
    .bytes()
    .map_err(|_| invalid("owner_reply_invalid"))?;
  if bytes.len() > 8192 {
    return Err(invalid("owner_reply_oversized"));
  }
  let value: Value = serde_json::from_slice(&bytes).map_err(|_| invalid("owner_reply_invalid"))?;
  if !status.is_success() || value["ok"] != true {
    return Err(reject(
      StatusCode::UNPROCESSABLE_ENTITY,
      "owner_command_rejected",
      "owner rejected node command",
    ));
  }
  Ok(json!({"command_id":command_id,"state":if released {"released"} else {"active"}}))
}

fn dispatch(
  paths: Paths,
  deployment: String,
  request: EventRequest,
) -> std::result::Result<Value, Failure> {
  validate_request(&request)?;
  read_run(&paths, &deployment, &request)?;
  let graph_path = paths.node_graph_path(&deployment);
  let bytes = fs::read(graph_path).map_err(|_| invalid("graph_unavailable"))?;
  if bytes.len() > graph_compat::MAX_GRAPH_BYTES || revision(&bytes) != request.active_revision {
    return Err(reject(
      StatusCode::CONFLICT,
      "graph_fence_stale",
      "saved graph revision is stale",
    ));
  }
  let graph: Value = serde_json::from_slice(&bytes).map_err(|_| invalid("graph_invalid"))?;
  let route = admitted_path(
    &paths,
    &graph,
    &request.active_revision,
    &deployment,
    &request.source_node,
  )?;
  let mut store = state()
    .lock()
    .map_err(|_| invalid("event_state_unavailable"))?;
  let owner_sequence = store.advance(&deployment, &request, &route, Instant::now())?;
  // Serialize events through the owner reply: a late refresh must not overtake release.
  send_movement(route.endpoint, &request, &route, owner_sequence)
}

pub(crate) async fn node_event(
  State(app): State<Arc<App>>,
  headers: HeaderMap,
  bytes: Bytes,
) -> ApiResult {
  authorized(&app, &headers)?;
  if bytes.len() > 1024 {
    return Err(fail(
      StatusCode::PAYLOAD_TOO_LARGE,
      "node_event_oversized",
      "node event exceeds 1024 bytes",
      Value::Null,
    ));
  }
  let request: EventRequest = serde_json::from_slice(&bytes).map_err(|_| {
    fail(
      StatusCode::BAD_REQUEST,
      "node_event_invalid",
      "node event is invalid",
      Value::Null,
    )
  })?;
  let paths = app.paths.clone();
  let deployment = app.deployment.clone();
  let result = tokio::task::spawn_blocking(move || dispatch(paths, deployment, request))
    .await
    .map_err(|_| {
      err(
        StatusCode::SERVICE_UNAVAILABLE,
        "node event worker unavailable",
      )
    })?
    .map_err(|failure| fail(failure.status, failure.code, failure.message, Value::Null))?;
  ok(result)
}

#[cfg(test)]
mod tests {
  use super::*;
  use axum::{body::to_bytes, http::Request};
  use std::{
    io::{BufRead, BufReader, Read, Write},
    net::TcpListener,
    os::unix::net::UnixListener,
  };
  use tower::ServiceExt;

  fn event(phase: KeyPhase, activation: &str, sequence: u64) -> EventRequest {
    EventRequest {
      schema_version: "0.1.0".into(),
      source_node: "isosui/input/keyboard/drive".into(),
      output: "state".into(),
      data_type: "key_state".into(),
      run_id: "r-test".into(),
      active_revision: "a".repeat(64),
      payload: KeyState {
        state: phase,
        activation_id: activation.into(),
        sequence,
      },
    }
  }

  fn route() -> control_path::ControlPath {
    control_path::ControlPath {
      eos_gateway: "eos/interface/gateway/control".into(),
      binding: "eos/dispatcher/base/velocity".into(),
      hardware: "eos/hardware/base/drive".into(),
      endpoint: "127.0.0.1:16020".parse().unwrap(),
      x_m_s: 0.2,
      y_m_s: 0.0,
      yaw_rad_s: 0.0,
      valid_for_ms: 150,
    }
  }

  #[test]
  fn activation_order_and_lease_are_fenced() {
    let mut store = EventState::default();
    let now = Instant::now();
    assert_eq!(
      store
        .advance("default", &event(KeyPhase::Pressed, "a", 1), &route(), now)
        .unwrap(),
      1
    );
    assert_eq!(
      store
        .advance(
          "default",
          &event(KeyPhase::Refresh, "a", 2),
          &route(),
          now + Duration::from_millis(60)
        )
        .unwrap(),
      2
    );
    assert_eq!(
      store
        .advance(
          "default",
          &event(KeyPhase::Refresh, "a", 2),
          &route(),
          now + Duration::from_millis(60)
        )
        .unwrap_err()
        .code,
      "activation_stale"
    );
    assert_eq!(
      store
        .advance(
          "default",
          &event(KeyPhase::Refresh, "a", 3),
          &route(),
          now + Duration::from_millis(61)
        )
        .unwrap_err()
        .code,
      "event_rate_exceeded"
    );
    assert_eq!(
      store
        .advance(
          "default",
          &event(KeyPhase::Released, "wrong", 3),
          &route(),
          now
        )
        .unwrap_err()
        .code,
      "activation_stale"
    );
    assert_eq!(
      store
        .advance(
          "default",
          &event(KeyPhase::Released, "a", 3),
          &route(),
          now + Duration::from_millis(60)
        )
        .unwrap(),
      3
    );
    assert_eq!(
      store
        .advance(
          "default",
          &event(KeyPhase::Pressed, "a", 1),
          &route(),
          now + Duration::from_millis(61)
        )
        .unwrap_err()
        .code,
      "activation_already_active"
    );
    assert_eq!(
      store
        .advance(
          "default",
          &event(KeyPhase::Pressed, "b", 1),
          &route(),
          now + Duration::from_millis(60)
        )
        .unwrap(),
      4
    );
    assert_eq!(
      store
        .advance(
          "default",
          &event(KeyPhase::Refresh, "b", 2),
          &route(),
          now + Duration::from_millis(211)
        )
        .unwrap_err()
        .code,
      "activation_stale"
    );
  }

  #[tokio::test]
  async fn malformed_and_oversized_node_events_fail_before_manager_access() {
    let app = Arc::new(App {
      paths: Paths {
        config: "/tmp/isos-node-test/config".into(),
        data: "/tmp/isos-node-test/data".into(),
        runtime: "/tmp/isos-node-test/run".into(),
      },
      deployment: "default".into(),
    });
    for (body, expected) in [
      ("{".to_owned(), StatusCode::BAD_REQUEST),
      ("x".repeat(1025), StatusCode::PAYLOAD_TOO_LARGE),
    ] {
      let response = router(app.clone())
        .oneshot(
          Request::builder()
            .method(Method::POST)
            .uri("/api/isos/v1/nodes/events")
            .header(header::HOST, "127.0.0.1:16000")
            .header(header::CONTENT_TYPE, "application/json")
            .body(Body::from(body))
            .unwrap(),
        )
        .await
        .unwrap();
      assert_eq!(response.status(), expected);
      let value: Value =
        serde_json::from_slice(&to_bytes(response.into_body(), 4096).await.unwrap()).unwrap();
      assert_eq!(value["ok"], false);
    }
  }

  #[test]
  fn admitted_event_forwards_graph_values_and_release_zero_with_run_fence() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let deployment = format!("key-test-{}", std::process::id());
    let bytes = br#"{"version":"0.1.0","cores":{}}"#;
    let revision = revision(bytes);
    let graph_path = paths.node_graph_path(&deployment);
    fs::create_dir_all(graph_path.parent().unwrap()).unwrap();
    fs::write(&graph_path, bytes).unwrap();

    let eos = TcpListener::bind("127.0.0.1:0").unwrap();
    let eos_address = eos.local_addr().unwrap();
    let manager = UnixListener::bind(paths.socket()).unwrap();
    let status_revision = revision.clone();
    let manager_thread = std::thread::spawn(move || {
      for _ in 0..2 {
        let (mut stream, _) = manager.accept().unwrap();
        let mut line = String::new();
        BufReader::new(stream.try_clone().unwrap())
          .read_line(&mut line)
          .unwrap();
        assert!(line.contains("deployment_status"));
        writeln!(
          stream,
          "{}",
          json!({"schema_version":"0.1.0","ok":true,"data":{
            "state":"running","run_id_state":"known","run_id":"r-test",
            "active_revision":status_revision,
            "processes":[{"id":"bridge","state":"running"}]
          }})
        )
        .unwrap();
      }
    });
    let eos_thread = std::thread::spawn(move || {
      let mut bodies = Vec::new();
      for _ in 0..2 {
        let (mut stream, _) = eos.accept().unwrap();
        stream
          .set_read_timeout(Some(Duration::from_secs(2)))
          .unwrap();
        let mut bytes = Vec::new();
        let mut chunk = [0u8; 4096];
        let header_end = loop {
          let count = stream.read(&mut chunk).unwrap();
          assert!(count > 0);
          bytes.extend_from_slice(&chunk[..count]);
          assert!(bytes.len() < 8192);
          if let Some(index) = bytes.windows(4).position(|part| part == b"\r\n\r\n") {
            break index + 4;
          }
        };
        let header = std::str::from_utf8(&bytes[..header_end]).unwrap();
        let length: usize = header
          .lines()
          .find_map(|line| {
            line
              .to_ascii_lowercase()
              .strip_prefix("content-length: ")
              .map(str::to_owned)
          })
          .unwrap()
          .parse()
          .unwrap();
        while bytes.len() - header_end < length {
          let count = stream.read(&mut chunk).unwrap();
          assert!(count > 0);
          bytes.extend_from_slice(&chunk[..count]);
        }
        bodies
          .push(serde_json::from_slice::<Value>(&bytes[header_end..header_end + length]).unwrap());
        stream.write_all(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\nConnection: close\r\n\r\n{\"ok\":true}").unwrap();
      }
      bodies
    });
    let mut route = route();
    route.endpoint = eos_address;
    state().lock().unwrap().routes.insert(
      (
        deployment.clone(),
        revision.clone(),
        "isosui/input/keyboard/drive".into(),
      ),
      route,
    );
    let mut pressed = event(KeyPhase::Pressed, "activation", 1);
    pressed.active_revision = revision.clone();
    let mut released = event(KeyPhase::Released, "activation", 2);
    released.active_revision = revision;
    assert_eq!(
      dispatch(paths.clone(), deployment.clone(), pressed).unwrap()["state"],
      "active"
    );
    assert_eq!(
      dispatch(paths, deployment, released).unwrap()["state"],
      "released"
    );
    manager_thread.join().unwrap();
    let bodies = eos_thread.join().unwrap();
    assert_eq!(bodies[0]["header"]["run_id"], "r-test");
    assert_eq!(bodies[0]["header"]["delivery_mode"], "stream");
    assert_eq!(bodies[0]["payload"]["x_m_s"], 0.2);
    assert_eq!(bodies[0]["header"]["deadline_after_ms"], 650);
    assert_eq!(
      bodies[0]["header"]["payload_length_bytes"],
      serde_json::to_vec(&bodies[0]["payload"]).unwrap().len()
    );
    assert_eq!(bodies[1]["payload"]["x_m_s"], 0.0);
    assert_eq!(bodies[1]["payload"]["valid_for_ms"], 50);
    assert_eq!(bodies[1]["header"]["deadline_after_ms"], 550);
  }
}
