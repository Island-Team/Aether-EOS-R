//! Process isolation for package hardware adapters. Dispatcher admission stays in EOS.
use std::fs;
use std::io::{Read, Write};
use std::os::fd::AsRawFd;
use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex, Weak};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use aether_hal::{
  BaseMovement, BaseNode, BaseObservation, CameraFrame, CameraNode, CommandMeta, CommandOutcome,
  HalError, Health, HealthState, ManipulatorMovement, ManipulatorNode, ManipulatorObservation,
  NodeAdapter, NodeKind, NodeProfile, NodeState, NodeStatus,
};
use serde::de::DeserializeOwned;
use serde_json::{json, Value};

use crate::adapter_library::{Libraries, LibrarySpec};

const REQUEST_LIMIT: u64 = 1024 * 1024;
const CAMERA_REPLY_LIMIT: u64 = 64 * 1024 * 1024 + 1024;
const OTHER_REPLY_LIMIT: u64 = 1024 * 1024;
const HEARTBEAT_INTERVAL: Duration = Duration::from_millis(100);
const HEARTBEAT_LEASE: Duration = Duration::from_millis(500);

struct SocketGuard(PathBuf);
impl Drop for SocketGuard {
  fn drop(&mut self) {
    let _ = fs::remove_file(&self.0);
  }
}

struct HostState {
  child: Mutex<Child>,
  directory: PathBuf,
  socket: PathBuf,
}

impl Drop for HostState {
  fn drop(&mut self) {
    if let Ok(mut stream) = UnixStream::connect(&self.socket) {
      let _ = stream.set_write_timeout(Some(Duration::from_millis(200)));
      let _ = stream.set_read_timeout(Some(Duration::from_millis(200)));
      let _ = stream.write_all(br#"{"version":1,"op":"stop"}"#);
      let _ = stream.shutdown(std::net::Shutdown::Write);
      let mut reply = [0u8; 256];
      let _ = stream.read(&mut reply);
    }
    if let Ok(child) = self.child.get_mut() {
      let _ = child.kill();
      let _ = child.wait();
    }
    let _ = fs::remove_dir_all(&self.directory);
  }
}

pub struct HostedAdapter {
  state: Arc<HostState>,
  socket: PathBuf,
  family: String,
  kind: NodeKind,
  max_payload: u64,
}

impl HostedAdapter {
  pub fn start(
    node: &NodeProfile,
    spec: &LibrarySpec,
    executable: &Path,
  ) -> Result<Arc<Self>, HalError> {
    let nonce = SystemTime::now()
      .duration_since(UNIX_EPOCH)
      .map_err(|e| HalError::Adapter(e.to_string()))?
      .as_nanos();
    let directory =
      std::env::temp_dir().join(format!("aether-adapter-{}-{nonce}", std::process::id()));
    fs::create_dir(&directory).map_err(|e| HalError::Adapter(e.to_string()))?;
    fs::set_permissions(&directory, fs::Permissions::from_mode(0o700))
      .map_err(|e| HalError::Adapter(e.to_string()))?;
    let node_file = directory.join("node.json");
    fs::write(
      &node_file,
      serde_json::to_vec(&json!({"node":node,"kind":node.kind,"path":node.path()}))
        .map_err(|e| HalError::Profile(e.to_string()))?,
    )
    .map_err(|e| HalError::Adapter(e.to_string()))?;
    let socket = directory.join("host.sock");
    let arg = format!("{}={}@{}", spec.family, spec.path.display(), spec.sha256);
    let child = Command::new(executable)
      .args([
        "--adapter-host",
        "--adapter-library",
        &arg,
        "--adapter-node",
      ])
      .arg(&node_file)
      .arg("--adapter-socket")
      .arg(&socket)
      .stdin(Stdio::null())
      .stdout(Stdio::null())
      .stderr(Stdio::inherit())
      .spawn()
      .map_err(|e| HalError::Adapter(format!("adapter host: {e}")))?;
    let state = Arc::new(HostState {
      child: Mutex::new(child),
      directory,
      socket: socket.clone(),
    });
    let adapter = Arc::new(Self {
      state: state.clone(),
      socket,
      family: spec.family.clone(),
      kind: node.kind,
      max_payload: node.observation.as_ref().map_or(0, |o| o.max_payload_bytes),
    });
    let deadline = Instant::now() + Duration::from_secs(3);
    loop {
      if adapter
        .request(json!({"version":1,"op":"heartbeat"}), OTHER_REPLY_LIMIT)
        .is_ok()
      {
        break;
      }
      if state
        .child
        .lock()
        .map_err(|e| HalError::Adapter(e.to_string()))?
        .try_wait()
        .map_err(|e| HalError::Adapter(e.to_string()))?
        .is_some()
      {
        return Err(HalError::Adapter(format!(
          "{}: adapter host exited during startup",
          node.path()
        )));
      }
      if Instant::now() >= deadline {
        return Err(HalError::Adapter(format!(
          "{}: adapter host startup timed out",
          node.path()
        )));
      }
      std::thread::sleep(Duration::from_millis(10));
    }
    let weak: Weak<Self> = Arc::downgrade(&adapter);
    std::thread::spawn(move || {
      while let Some(host) = weak.upgrade() {
        if host
          .request(json!({"version":1,"op":"heartbeat"}), OTHER_REPLY_LIMIT)
          .is_err()
        {
          break;
        }
        drop(host);
        std::thread::sleep(HEARTBEAT_INTERVAL);
      }
    });
    Ok(adapter)
  }

  fn request(&self, value: Value, limit: u64) -> Result<Value, HalError> {
    let _keep_alive = &self.state;
    let mut stream = UnixStream::connect(&self.socket)
      .map_err(|e| HalError::Adapter(format!("adapter host unavailable: {e}")))?;
    stream
      .set_read_timeout(Some(Duration::from_secs(2)))
      .map_err(|e| HalError::Adapter(e.to_string()))?;
    stream
      .set_write_timeout(Some(Duration::from_secs(2)))
      .map_err(|e| HalError::Adapter(e.to_string()))?;
    let bytes = serde_json::to_vec(&value).map_err(|e| HalError::Adapter(e.to_string()))?;
    if bytes.len() as u64 > REQUEST_LIMIT {
      return Err(HalError::Adapter("adapter host request too large".into()));
    }
    stream
      .write_all(&bytes)
      .map_err(|e| HalError::Adapter(format!("adapter host write: {e}")))?;
    stream
      .shutdown(std::net::Shutdown::Write)
      .map_err(|e| HalError::Adapter(e.to_string()))?;
    let mut reply = Vec::new();
    stream
      .take(limit + 1)
      .read_to_end(&mut reply)
      .map_err(|e| HalError::Adapter(format!("adapter host read: {e}")))?;
    if reply.len() as u64 > limit {
      return Err(HalError::Adapter("adapter host reply too large".into()));
    }
    let reply: Value = serde_json::from_slice(&reply)
      .map_err(|e| HalError::Adapter(format!("adapter host reply: {e}")))?;
    if reply["ok"] != true {
      return Err(HalError::Adapter(
        reply["error"]
          .as_str()
          .unwrap_or("adapter host rejected request")
          .into(),
      ));
    }
    Ok(reply["result"].clone())
  }

  fn call<T: DeserializeOwned>(&self, operation: u32, request: Value) -> Result<T, HalError> {
    let limit = if operation == 3 {
      CAMERA_REPLY_LIMIT
    } else {
      OTHER_REPLY_LIMIT
    };
    let result = self.request(
      json!({"version":1,"op":"call","operation":operation,"request":request}),
      limit,
    )?;
    serde_json::from_value(result)
      .map_err(|e| HalError::InvalidData(format!("adapter host result: {e}")))
  }
}

impl NodeAdapter for HostedAdapter {
  fn kind(&self) -> NodeKind {
    self.kind
  }
  fn adapter_type(&self) -> &str {
    &self.family
  }
  fn status(&self) -> NodeStatus {
    match self.call::<Value>(1, json!({})) {
      Ok(v) => NodeStatus {
        state: match v["state"].as_str() {
          Some("ready") => NodeState::Ready,
          Some("stopped") => NodeState::Stopped,
          Some("starting") => NodeState::Starting,
          _ => NodeState::Faulted,
        },
        active_command_id: v["active_command_id"].as_str().map(str::to_owned),
      },
      Err(_) => NodeStatus {
        state: NodeState::Faulted,
        active_command_id: None,
      },
    }
  }
  fn health(&self) -> Health {
    match self.call::<Value>(2, json!({})) {
      Ok(v) => Health {
        state: match v["state"].as_str() {
          Some("healthy") => HealthState::Healthy,
          Some("degraded") => HealthState::Degraded,
          _ => HealthState::Unavailable,
        },
        code: v["code"].as_str().map(str::to_owned),
        message: v["message"].as_str().map(str::to_owned),
        last_ok_monotonic_ns: v["last_ok_monotonic_ns"].as_u64(),
      },
      Err(e) => Health {
        state: HealthState::Unavailable,
        code: Some("adapter_host".into()),
        message: Some(e.to_string()),
        last_ok_monotonic_ns: None,
      },
    }
  }
  fn stop(&self) -> Result<(), HalError> {
    self
      .request(json!({"version":1,"op":"stop"}), OTHER_REPLY_LIMIT)
      .map(|_| ())
  }
}
impl CameraNode for HostedAdapter {
  fn capture(&self) -> Result<CameraFrame, HalError> {
    let frame: CameraFrame = self.call(3, json!({}))?;
    frame.validate(self.max_payload)?;
    Ok(frame)
  }
}
impl BaseNode for HostedAdapter {
  fn observe(&self) -> Result<BaseObservation, HalError> {
    self.call(4, json!({}))
  }
  fn move_base(
    &self,
    meta: CommandMeta,
    movement: BaseMovement,
  ) -> Result<CommandOutcome, HalError> {
    outcome(self.call::<Value>(5, json!({"meta":meta,"movement":movement}))?)
  }
}
impl ManipulatorNode for HostedAdapter {
  fn observe(&self) -> Result<ManipulatorObservation, HalError> {
    self.call(6, json!({}))
  }
  fn move_manipulator(
    &self,
    meta: CommandMeta,
    movement: ManipulatorMovement,
  ) -> Result<CommandOutcome, HalError> {
    outcome(self.call::<Value>(7, json!({"meta":meta,"movement":movement}))?)
  }
}
fn outcome(value: Value) -> Result<CommandOutcome, HalError> {
  match value.as_str() {
    Some("accepted") => Ok(CommandOutcome::Accepted),
    Some("completed") => Ok(CommandOutcome::Completed),
    Some("cancelled") => Ok(CommandOutcome::Cancelled),
    Some("ambiguous") => Ok(CommandOutcome::Ambiguous),
    _ => Err(HalError::InvalidData(
      "invalid adapter command outcome".into(),
    )),
  }
}

/// Entry point used only by the EOS-owned adapter child process.
pub fn run_host(args: &[String]) -> Result<(), Box<dyn std::error::Error>> {
  if args.len() != 7
    || args[0] != "--adapter-host"
    || args[1] != "--adapter-library"
    || args[3] != "--adapter-node"
    || args[5] != "--adapter-socket"
  {
    return Err("invalid adapter host arguments".into());
  }
  let spec = LibrarySpec::parse(&args[2])?;
  let input: Value = serde_json::from_slice(&fs::read(&args[4])?)?;
  let mut node: NodeProfile = serde_json::from_value(input["node"].clone())?;
  node.kind = serde_json::from_value(input["kind"].clone())?;
  node.path = input["path"]
    .as_str()
    .ok_or("adapter host node has no path")?
    .into();
  if node.adapter.adapter_type != spec.family || node.kind == NodeKind::Software {
    return Err("adapter node and verified library disagree".into());
  }
  let libraries = Libraries::open(&[spec])?;
  let adapter = libraries.create(&node)?;
  let socket = Path::new(&args[6]);
  let listener = UnixListener::bind(socket)?;
  let _socket_guard = SocketGuard(socket.to_path_buf());
  fs::set_permissions(socket, fs::Permissions::from_mode(0o600))?;
  listener.set_nonblocking(true)?;
  let last_heartbeat = Arc::new(Mutex::new(Instant::now()));
  loop {
    if last_heartbeat.lock().map_err(|e| e.to_string())?.elapsed() >= HEARTBEAT_LEASE {
      let _ = adapter.stop();
      return Err("adapter host heartbeat expired".into());
    }
    match listener.accept() {
      Ok((stream, _)) => {
        let adapter = adapter.clone();
        let heartbeat = last_heartbeat.clone();
        std::thread::spawn(move || {
          let _ = serve_request(stream, &adapter, &heartbeat);
        });
      }
      Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
        let mut event = libc::pollfd {
          fd: listener.as_raw_fd(),
          events: libc::POLLIN,
          revents: 0,
        };
        let result = unsafe { libc::poll(&mut event, 1, 50) };
        if result < 0 {
          let error = std::io::Error::last_os_error();
          if error.kind() != std::io::ErrorKind::Interrupted {
            return Err(error.into());
          }
        }
      }
      Err(e) => return Err(e.into()),
    }
  }
}

fn serve_request(
  mut stream: UnixStream,
  adapter: &crate::adapter_library::LoadedAdapter,
  heartbeat: &Mutex<Instant>,
) -> Result<(), Box<dyn std::error::Error>> {
  stream.set_read_timeout(Some(Duration::from_secs(2)))?;
  stream.set_write_timeout(Some(Duration::from_secs(2)))?;
  let mut bytes = Vec::new();
  (&mut stream)
    .take(REQUEST_LIMIT + 1)
    .read_to_end(&mut bytes)?;
  if bytes.len() as u64 > REQUEST_LIMIT {
    return Err("adapter host request too large".into());
  }
  let request: Value = serde_json::from_slice(&bytes)?;
  let result = if request["version"] != 1 {
    Err(HalError::Adapter(
      "unsupported adapter host IPC version".into(),
    ))
  } else {
    match request["op"].as_str() {
      Some("heartbeat") => {
        *heartbeat.lock().map_err(|e| e.to_string())? = Instant::now();
        Ok(json!(null))
      }
      Some("stop") => adapter.stop().map(|_| json!(null)),
      Some("call") => match (request["operation"].as_u64(), request.get("request")) {
        (Some(op), Some(payload)) if (1..=7).contains(&op) => {
          adapter.call_value(op as u32, payload.clone())
        }
        _ => Err(HalError::Adapter(
          "call requires operation 1–7 and request".into(),
        )),
      },
      _ => Err(HalError::Adapter("unknown adapter host operation".into())),
    }
  };
  let reply = match result {
    Ok(value) => json!({"ok":true,"result":value}),
    Err(e) => json!({"ok":false,"error":e.to_string()}),
  };
  let output = serde_json::to_vec(&reply)?;
  let limit = if request["operation"] == 3 {
    CAMERA_REPLY_LIMIT
  } else {
    OTHER_REPLY_LIMIT
  };
  if output.len() as u64 > limit {
    return Err("adapter host reply too large".into());
  }
  stream.write_all(&output)?;
  Ok(())
}
