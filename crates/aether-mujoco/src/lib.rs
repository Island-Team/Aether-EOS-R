//! MuJoCo v2 transport adapters. The simulator owns its devices and EOS owns
//! command admission; this crate only translates typed HAL calls to bounded
//! local frames.

use std::fmt;
use std::collections::HashMap;
use std::io::{self, Read, Write};
use std::os::fd::{AsRawFd, FromRawFd};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, OnceLock, Weak};
use std::time::{Duration, Instant};

use aether_common::time::Clock;
use aether_hal::{
  BaseMovement, BaseNode, BaseObservation, CameraFrame, CameraNode, ClockKind, CommandMeta,
  CommandOutcome, EndEffectorPose, HalError, Health, HealthState, ManipulatorMovement,
  ManipulatorNode, ManipulatorObservation, MovementFenceToken, NodeAdapter, NodeKind, NodeProfile,
  NodeState, NodeStatus, RobotStopProof, ControlledStopOutcome, SampleMeta,
};
use jpeg_decoder::{Decoder, PixelFormat};
use serde_json::{json, Map, Value};
use uuid::Uuid;

pub const MAX_METADATA_BYTES: usize = 64 * 1024;
pub const MAX_PAYLOAD_BYTES: usize = 8 * 1024 * 1024;

#[derive(Default)]
struct SessionState {
  generation_id: Option<String>,
  stop_epoch: u64,
  stop_inflight: bool,
  last_stop: Option<RobotStopProof>,
  movement_releases: u64,
  last_stop_release_count: Option<u64>,
}

/// Shared by every graph-declared device of one child. The physical stop has
/// one companion-socket caller even when two logical stop IDs race.
pub struct ManagedSession {
  socket: PathBuf,
  state: Mutex<SessionState>,
  changed: Condvar,
}

fn shared_session(socket: &PathBuf) -> Arc<ManagedSession> {
  static SESSIONS: OnceLock<Mutex<HashMap<PathBuf, Weak<ManagedSession>>>> = OnceLock::new();
  let mut sessions = SESSIONS
    .get_or_init(|| Mutex::new(HashMap::new()))
    .lock()
    .expect("MuJoCo session registry poisoned");
  if let Some(existing) = sessions.get(socket).and_then(Weak::upgrade) {
    return existing;
  }
  let session = Arc::new(ManagedSession {
    socket: socket.clone(),
    state: Mutex::new(SessionState::default()),
    changed: Condvar::new(),
  });
  sessions.insert(socket.clone(), Arc::downgrade(&session));
  session
}

impl ManagedSession {
  fn stop_socket(&self) -> PathBuf {
    let mut path = self.socket.as_os_str().to_os_string();
    path.push(".stop");
    PathBuf::from(path)
  }

  pub fn install_generation(&self, generation_id: &str) -> Result<(), HalError> {
    if generation_id.len() != 32
      || !generation_id
        .bytes()
        .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    {
      return Err(HalError::InvalidData(
        "invalid MuJoCo generation identity".into(),
      ));
    }
    let mut state = self.state.lock().expect("MuJoCo session poisoned");
    if state.generation_id.as_deref() != Some(generation_id) {
      state.generation_id = Some(generation_id.into());
      state.stop_epoch = 0;
      state.last_stop = None;
      state.movement_releases = 0;
      state.last_stop_release_count = None;
    }
    Ok(())
  }

  pub fn invalidate(&self) {
    let mut state = self.state.lock().expect("MuJoCo session poisoned");
    state.generation_id = None;
    state.last_stop = None;
    state.last_stop_release_count = None;
  }

  pub fn token(&self) -> Option<MovementFenceToken> {
    let state = self.state.lock().expect("MuJoCo session poisoned");
    state
      .generation_id
      .as_ref()
      .map(|generation_id| MovementFenceToken {
        generation_id: generation_id.clone(),
        stop_epoch: state.stop_epoch.to_string(),
      })
  }

  pub fn last_stop(&self) -> Option<RobotStopProof> {
    self
      .state
      .lock()
      .expect("MuJoCo session poisoned")
      .last_stop
      .clone()
  }

  fn note_movement_release(&self) {
    let mut state = self.state.lock().expect("MuJoCo session poisoned");
    state.movement_releases = state.movement_releases.saturating_add(1);
  }

  pub fn stop(&self, timeout: Duration) -> RobotStopProof {
    let deadline = Instant::now() + timeout;
    let mut state = self.state.lock().expect("MuJoCo session poisoned");
    let Some(generation_id) = state.generation_id.clone() else {
      return RobotStopProof {
        base: ControlledStopOutcome::OutcomeUnknown,
        arm: ControlledStopOutcome::OutcomeUnknown,
        generation_id: String::new(),
        stop_epoch: None,
      };
    };
    if state.last_stop_release_count == Some(state.movement_releases) {
      if let Some(proof) = &state.last_stop {
        if proof.base == ControlledStopOutcome::Stopped
          && proof.arm == ControlledStopOutcome::Stopped
        {
          return proof.clone();
        }
      }
    }
    if state.stop_inflight {
      while state.stop_inflight {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
          break;
        }
        let (next, _) = self
          .changed
          .wait_timeout(state, remaining)
          .expect("MuJoCo session poisoned");
        state = next;
      }
      if state.stop_inflight || Instant::now() > deadline {
        return RobotStopProof {
          base: ControlledStopOutcome::OutcomeUnknown,
          arm: ControlledStopOutcome::OutcomeUnknown,
          generation_id,
          stop_epoch: None,
        };
      }
      return state.last_stop.clone().unwrap_or(RobotStopProof {
        base: ControlledStopOutcome::OutcomeUnknown,
        arm: ControlledStopOutcome::OutcomeUnknown,
        generation_id,
        stop_epoch: None,
      });
    }
    state.stop_inflight = true;
    state.last_stop = None;
    let old_epoch = state.stop_epoch;
    drop(state);
    let result = self.stop_exchange(&generation_id, old_epoch, deadline);
    let mut state = self.state.lock().expect("MuJoCo session poisoned");
    if result.base == ControlledStopOutcome::Stopped
      && result.arm == ControlledStopOutcome::Stopped
      && state.generation_id.as_deref() == Some(generation_id.as_str())
    {
      if let Some(epoch) = result
        .stop_epoch
        .as_deref()
        .and_then(|value| value.parse::<u64>().ok())
      {
        state.stop_epoch = epoch;
      }
    }
    state.last_stop = Some(result.clone());
    state.last_stop_release_count = (result.base == ControlledStopOutcome::Stopped
      && result.arm == ControlledStopOutcome::Stopped)
      .then_some(state.movement_releases);
    state.stop_inflight = false;
    self.changed.notify_all();
    result
  }

  fn stop_exchange(
    &self,
    generation_id: &str,
    old_epoch: u64,
    deadline: Instant,
  ) -> RobotStopProof {
    let unknown = || RobotStopProof {
      base: ControlledStopOutcome::OutcomeUnknown,
      arm: ControlledStopOutcome::OutcomeUnknown,
      generation_id: generation_id.into(),
      stop_epoch: None,
    };
    let endpoint = Endpoint::new(
      self.stop_socket(),
      "base-0".into(),
      String::new(),
      Vec::new(),
      Arc::new(aether_common::time::SystemClock),
    );
    let id = Endpoint::next_id();
    let remaining = deadline.saturating_duration_since(Instant::now());
    if remaining.is_zero() {
      return unknown();
    }
    let Ok((reply, payload)) = endpoint.request_timeout_raw(
      "stop",
      &id,
      json!({"command_id":id,"generation_id":generation_id}),
      remaining,
      true,
    ) else {
      return unknown();
    };
    if !payload.is_empty()
      || reply.get("generation_id").and_then(Value::as_str) != Some(generation_id)
    {
      return unknown();
    }
    let Some(epoch) = reply.get("stop_epoch").and_then(Value::as_str) else {
      return unknown();
    };
    if epoch.is_empty()
      || (epoch.starts_with('0') && epoch != "0")
      || !epoch.bytes().all(|byte| byte.is_ascii_digit())
    {
      return unknown();
    }
    let Ok(new_epoch) = epoch.parse::<u64>() else {
      return unknown();
    };
    if new_epoch <= old_epoch {
      return unknown();
    }
    let part = |name| match reply.get(name).and_then(Value::as_str) {
      Some("stopped") => Some(ControlledStopOutcome::Stopped),
      Some("unknown") => Some(ControlledStopOutcome::OutcomeUnknown),
      _ => None,
    };
    let (Some(base), Some(arm)) = (part("base_stop"), part("arm_stop")) else {
      return unknown();
    };
    let full = base == ControlledStopOutcome::Stopped && arm == ControlledStopOutcome::Stopped;
    let valid = if full {
      reply.get("ok").and_then(Value::as_bool) == Some(true)
        && reply.get("accepted").and_then(Value::as_bool) == Some(true)
    } else {
      reply.get("ok").and_then(Value::as_bool) == Some(false)
        && reply.get("accepted").and_then(Value::as_bool) == Some(false)
        && reply.pointer("/error/code").and_then(Value::as_str) == Some("stop_outcome_unknown")
    };
    if !valid {
      return unknown();
    }
    RobotStopProof {
      base,
      arm,
      generation_id: generation_id.into(),
      stop_epoch: Some(epoch.into()),
    }
  }
}

#[cfg(test)]
mod startup_transport_tests {
  use super::*;

  #[test]
  fn unconnected_startup_socket_is_retryable_but_truncated_frame_is_invalid() {
    assert!(io_failure(io::Error::from(io::ErrorKind::NotConnected)).is_transient());
    assert!(!io_failure(io::Error::from(io::ErrorKind::UnexpectedEof)).is_transient());
  }
}

/// A startup probe may retry transport races but must reject a contradictory
/// health response from an already reachable simulator immediately.
#[derive(Debug)]
pub enum StrictHealthError {
  Transient(io::Error),
  Invalid(HalError),
}

impl StrictHealthError {
  pub fn is_transient(&self) -> bool {
    matches!(self, Self::Transient(_))
  }

  fn into_hal(self) -> HalError {
    match self {
      Self::Transient(error) => HalError::Adapter(error.to_string()),
      Self::Invalid(error) => error,
    }
  }
}

impl fmt::Display for StrictHealthError {
  fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
    match self {
      Self::Transient(error) => write!(formatter, "transient MuJoCo transport: {error}"),
      Self::Invalid(error) => write!(formatter, "invalid MuJoCo response: {error}"),
    }
  }
}

fn io_failure(error: io::Error) -> StrictHealthError {
  match error.kind() {
    io::ErrorKind::NotFound
    | io::ErrorKind::ConnectionRefused
    | io::ErrorKind::ConnectionAborted
    | io::ErrorKind::ConnectionReset
    | io::ErrorKind::NotConnected
    | io::ErrorKind::BrokenPipe
    | io::ErrorKind::TimedOut
    | io::ErrorKind::WouldBlock
    | io::ErrorKind::Interrupted => StrictHealthError::Transient(error),
    io::ErrorKind::UnexpectedEof => {
      StrictHealthError::Invalid(HalError::Adapter("truncated MuJoCo response frame".into()))
    }
    _ => StrictHealthError::Invalid(HalError::Adapter(error.to_string())),
  }
}

fn wait_fd(fd: libc::c_int, events: libc::c_short, deadline: Instant) -> io::Result<()> {
  loop {
    let remaining = deadline.saturating_duration_since(Instant::now());
    if remaining.is_zero() {
      return Err(io::Error::new(
        io::ErrorKind::TimedOut,
        "MuJoCo request deadline",
      ));
    }
    let millis = remaining.as_millis().clamp(1, i32::MAX as u128) as i32;
    let mut descriptor = libc::pollfd {
      fd,
      events,
      revents: 0,
    };
    let result = unsafe { libc::poll(&mut descriptor, 1, millis) };
    if result > 0 {
      return Ok(());
    }
    if result == 0 {
      continue;
    }
    let error = io::Error::last_os_error();
    if error.kind() != io::ErrorKind::Interrupted {
      return Err(error);
    }
  }
}

fn connect_until(path: &std::path::Path, deadline: Instant) -> io::Result<UnixStream> {
  if deadline <= Instant::now() {
    return Err(io::Error::new(
      io::ErrorKind::TimedOut,
      "MuJoCo request deadline",
    ));
  }
  let bytes = path.as_os_str().as_bytes();
  let mut address: libc::sockaddr_un = unsafe { std::mem::zeroed() };
  if bytes.is_empty() || bytes.contains(&0) || bytes.len() >= address.sun_path.len() {
    return Err(io::Error::new(
      io::ErrorKind::InvalidInput,
      "invalid MuJoCo socket path",
    ));
  }
  address.sun_family = libc::AF_UNIX as libc::sa_family_t;
  for (destination, source) in address.sun_path.iter_mut().zip(bytes) {
    *destination = *source as libc::c_char;
  }
  let fd = unsafe {
    libc::socket(
      libc::AF_UNIX,
      libc::SOCK_STREAM | libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC,
      0,
    )
  };
  if fd < 0 {
    return Err(io::Error::last_os_error());
  }
  let stream = unsafe { UnixStream::from_raw_fd(fd) };
  let length =
    (std::mem::offset_of!(libc::sockaddr_un, sun_path) + bytes.len() + 1) as libc::socklen_t;
  let result = unsafe { libc::connect(fd, (&address as *const libc::sockaddr_un).cast(), length) };
  if result == 0 {
    return Ok(stream);
  }
  let error = io::Error::last_os_error();
  if !matches!(
    error.raw_os_error(),
    Some(libc::EINPROGRESS | libc::EAGAIN | libc::EALREADY)
  ) {
    return Err(error);
  }
  wait_fd(fd, libc::POLLOUT, deadline)?;
  let mut socket_error = 0;
  let mut size = std::mem::size_of_val(&socket_error) as libc::socklen_t;
  if unsafe {
    libc::getsockopt(
      fd,
      libc::SOL_SOCKET,
      libc::SO_ERROR,
      (&mut socket_error as *mut i32).cast(),
      &mut size,
    )
  } < 0
  {
    return Err(io::Error::last_os_error());
  }
  if socket_error != 0 {
    return Err(io::Error::from_raw_os_error(socket_error));
  }
  Ok(stream)
}

fn write_until(stream: &mut UnixStream, mut bytes: &[u8], deadline: Instant) -> io::Result<()> {
  while !bytes.is_empty() {
    if deadline <= Instant::now() {
      return Err(io::Error::new(
        io::ErrorKind::TimedOut,
        "MuJoCo request deadline",
      ));
    }
    match stream.write(bytes) {
      Ok(0) => {
        return Err(io::Error::new(
          io::ErrorKind::WriteZero,
          "MuJoCo socket closed",
        ))
      }
      Ok(count) => bytes = &bytes[count..],
      Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
        wait_fd(stream.as_raw_fd(), libc::POLLOUT, deadline)?
      }
      Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
      Err(error) => return Err(error),
    }
  }
  Ok(())
}

fn read_until(stream: &mut UnixStream, mut bytes: &mut [u8], deadline: Instant) -> io::Result<()> {
  while !bytes.is_empty() {
    if deadline <= Instant::now() {
      return Err(io::Error::new(
        io::ErrorKind::TimedOut,
        "MuJoCo request deadline",
      ));
    }
    match stream.read(bytes) {
      Ok(0) => {
        return Err(io::Error::new(
          io::ErrorKind::UnexpectedEof,
          "MuJoCo socket closed mid-frame",
        ))
      }
      Ok(count) => bytes = &mut bytes[count..],
      Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
        wait_fd(stream.as_raw_fd(), libc::POLLIN, deadline)?
      }
      Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
      Err(error) => return Err(error),
    }
  }
  Ok(())
}

/// A single configured simulator endpoint and its local clock domain.
pub struct Endpoint {
  socket: PathBuf,
  session: Arc<ManagedSession>,
  node: String,
  frame_id: String,
  joint_names: Vec<String>,
  clock: Arc<dyn Clock>,
  stopped: AtomicBool,
}

impl Endpoint {
  pub fn from_profile(node: &NodeProfile, clock: Arc<dyn Clock>) -> Result<Arc<Self>, HalError> {
    if node.adapter.adapter_type != "mujoco-v2" {
      return Err(HalError::Profile("adapter type must be mujoco-v2".into()));
    }
    match node.kind {
      NodeKind::Base => {
        let movement = node
          .movement
          .as_ref()
          .ok_or_else(|| HalError::Profile("MuJoCo base requires movement".into()))?;
        if movement.modes != ["body_velocity"]
          || !movement
            .limits
            .max_velocity_lease_ms
            .is_some_and(|limit| (1..=1000).contains(&limit))
        {
          return Err(HalError::Profile(
            "MuJoCo base supports body_velocity with a 1..1000 ms lease".into(),
          ));
        }
      }
      NodeKind::Manipulator => {
        let movement = node
          .movement
          .as_ref()
          .ok_or_else(|| HalError::Profile("MuJoCo manipulator requires movement".into()))?;
        if movement.modes != ["joint_positions"]
          || node
            .joint_names
            .as_ref()
            .is_none_or(|names| names.len() != 6)
        {
          return Err(HalError::Profile(
            "MuJoCo manipulator requires six joints and joint_positions mode".into(),
          ));
        }
      }
      NodeKind::Camera => {
        if node.capabilities.iter().any(|mode| mode == "depth") {
          return Err(HalError::Profile(
            "current MuJoCo camera does not provide depth".into(),
          ));
        }
      }
      NodeKind::Software => {
        return Err(HalError::Profile("MuJoCo adapter requires hardware".into()));
      }
    }
    let setting = |name: &str| {
      node
        .adapter
        .settings
        .get(name)
        .and_then(toml_string)
        .filter(|value| !value.trim().is_empty())
        .ok_or_else(|| HalError::Profile(format!("missing adapter setting: {name}")))
    };
    let socket = PathBuf::from(setting("socket_path")?);
    let simulator_node = setting("simulator_node")?;
    Ok(Arc::new(Self {
      session: shared_session(&socket),
      socket,
      node: simulator_node,
      frame_id: node.frame_id.clone(),
      joint_names: node.joint_names.clone().unwrap_or_default(),
      clock,
      stopped: AtomicBool::new(false),
    }))
  }

  pub fn new(
    socket: PathBuf,
    node: String,
    frame_id: String,
    joint_names: Vec<String>,
    clock: Arc<dyn Clock>,
  ) -> Arc<Self> {
    Arc::new(Self {
      session: shared_session(&socket),
      socket,
      node,
      frame_id,
      joint_names,
      clock,
      stopped: AtomicBool::new(false),
    })
  }

  fn request(&self, op: &str, id: &str, fields: Value) -> Result<(Value, Vec<u8>), HalError> {
    self
      .request_timeout(op, id, fields, Duration::from_secs(2))
      .map_err(StrictHealthError::into_hal)
  }

  fn request_timeout(
    &self,
    op: &str,
    id: &str,
    fields: Value,
    timeout: Duration,
  ) -> Result<(Value, Vec<u8>), StrictHealthError> {
    self.request_timeout_raw(op, id, fields, timeout, false)
  }

  fn request_timeout_raw(
    &self,
    op: &str,
    id: &str,
    fields: Value,
    timeout: Duration,
    allow_error: bool,
  ) -> Result<(Value, Vec<u8>), StrictHealthError> {
    let deadline = Instant::now() + timeout;
    if id.is_empty() || id.len() > 128 || id.trim().is_empty() {
      return Err(StrictHealthError::Invalid(HalError::InvalidData(
        "request ID must be 1..128 UTF-8 bytes".into(),
      )));
    }
    let mut metadata = Map::new();
    metadata.insert("version".into(), json!("2.0.0"));
    metadata.insert("id".into(), json!(id));
    metadata.insert("op".into(), json!(op));
    metadata.insert("node".into(), json!(self.node));
    let fields = fields.as_object().ok_or_else(|| {
      StrictHealthError::Invalid(HalError::InvalidData(
        "request fields must be an object".into(),
      ))
    })?;
    for (key, value) in fields {
      metadata.insert(key.clone(), value.clone());
    }
    let encoded = serde_json::to_vec(&metadata)
      .map_err(|error| StrictHealthError::Invalid(HalError::Adapter(error.to_string())))?;
    if encoded.len() > MAX_METADATA_BYTES {
      return Err(StrictHealthError::Invalid(HalError::InvalidData(
        "request metadata too large".into(),
      )));
    }
    let mut stream = connect_until(&self.socket, deadline).map_err(io_failure)?;
    write_until(&mut stream, &(encoded.len() as u32).to_le_bytes(), deadline)
      .map_err(io_failure)?;
    write_until(&mut stream, &0u32.to_le_bytes(), deadline).map_err(io_failure)?;
    write_until(&mut stream, &encoded, deadline).map_err(io_failure)?;
    let mut lengths = [0u8; 8];
    read_until(&mut stream, &mut lengths, deadline).map_err(io_failure)?;
    let metadata_len = u32::from_le_bytes(lengths[..4].try_into().unwrap()) as usize;
    let payload_len = u32::from_le_bytes(lengths[4..].try_into().unwrap()) as usize;
    if metadata_len == 0 || metadata_len > MAX_METADATA_BYTES || payload_len > MAX_PAYLOAD_BYTES {
      return Err(StrictHealthError::Invalid(HalError::Adapter(
        "response frame exceeds bounds".into(),
      )));
    }
    let mut body = vec![0u8; metadata_len];
    let mut payload = vec![0u8; payload_len];
    read_until(&mut stream, &mut body, deadline).map_err(io_failure)?;
    read_until(&mut stream, &mut payload, deadline).map_err(io_failure)?;
    let reply: Value = serde_json::from_slice(&body).map_err(|error| {
      StrictHealthError::Invalid(HalError::Adapter(format!(
        "invalid response metadata: {error}"
      )))
    })?;
    if reply.get("version").and_then(Value::as_str) != Some("2.0.0")
      || reply.get("id").and_then(Value::as_str) != Some(id)
    {
      return Err(StrictHealthError::Invalid(HalError::Adapter(
        "response version or ID mismatch".into(),
      )));
    }
    if !allow_error && reply.get("ok").and_then(Value::as_bool) != Some(true) {
      let message = reply
        .pointer("/error/message")
        .and_then(Value::as_str)
        .unwrap_or("simulator rejected request");
      return Err(StrictHealthError::Invalid(HalError::Adapter(
        message.into(),
      )));
    }
    Ok((reply, payload))
  }

  fn next_id() -> String {
    format!("eos-{}", Uuid::new_v4())
  }

  /// Probe a graph-declared device for the managed, single-route simulator mode.
  pub fn strict_health(&self, timeout: Duration) -> Result<(), StrictHealthError> {
    let (reply, payload) = self.request_timeout("health", &Self::next_id(), json!({}), timeout)?;
    if !payload.is_empty()
      || reply.get("accepted").and_then(Value::as_bool) != Some(true)
      || !reply
        .get("features")
        .and_then(Value::as_array)
        .is_some_and(|features| {
          features.iter().all(Value::is_string)
            && features
              .iter()
              .any(|value| value.as_str() == Some("strict-socket-v1"))
        })
    {
      return Err(StrictHealthError::Invalid(HalError::Adapter(
        "strict MuJoCo health proof is missing".into(),
      )));
    }
    Ok(())
  }

  /// Probe both independent sockets before enabling managed movement. R05
  /// read-only readiness remains possible when this optional proof is absent.
  pub fn strict_stop_health(&self, timeout: Duration) -> Result<String, StrictHealthError> {
    let require = |reply: &Value, payload: &[u8]| -> Result<String, StrictHealthError> {
      let features = reply.get("features").and_then(Value::as_array);
      let generation = reply.get("generation_id").and_then(Value::as_str);
      if !payload.is_empty()
        || reply.get("accepted").and_then(Value::as_bool) != Some(true)
        || !features.is_some_and(|values| {
          values.iter().all(Value::is_string)
            && ["strict-socket-v1", "strict-stop-v1"]
              .iter()
              .all(|required| values.iter().any(|value| value.as_str() == Some(*required)))
        })
        || !generation.is_some_and(|value| {
          value.len() == 32
            && value
              .bytes()
              .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
        })
      {
        return Err(StrictHealthError::Invalid(HalError::Adapter(
          "strict MuJoCo stop health proof is missing".into(),
        )));
      }
      Ok(generation.unwrap().into())
    };
    let (main, payload) = self.request_timeout("health", &Self::next_id(), json!({}), timeout)?;
    let main_generation = require(&main, &payload)?;
    let stop = Self::new(
      self.session.stop_socket(),
      self.node.clone(),
      String::new(),
      Vec::new(),
      self.clock.clone(),
    );
    let (stop_reply, stop_payload) =
      stop.request_timeout("health", &Self::next_id(), json!({}), timeout)?;
    let stop_generation = require(&stop_reply, &stop_payload)?;
    if main_generation != stop_generation {
      return Err(StrictHealthError::Invalid(HalError::Adapter(
        "MuJoCo stop generation mismatch".into(),
      )));
    }
    Ok(main_generation)
  }

  pub fn session(&self) -> Arc<ManagedSession> {
    self.session.clone()
  }

  fn health(&self) -> Health {
    let result = if self.session.token().is_some() && self.node != "cam-0" {
      self.strict_stop_health(Duration::from_secs(2)).map(|_| ())
    } else {
      self.strict_health(Duration::from_secs(2))
    };
    if result.is_err() && self.session.token().is_some() && self.node != "cam-0" {
      self.session.invalidate();
    }
    match result {
      Ok(_) => Health {
        state: HealthState::Healthy,
        code: None,
        message: None,
        last_ok_monotonic_ns: Some(self.clock.monotonic_ns()),
      },
      Err(error) => Health {
        state: HealthState::Unavailable,
        code: Some("mujoco_unavailable".into()),
        message: Some(error.to_string()),
        last_ok_monotonic_ns: None,
      },
    }
  }

  fn status(&self) -> NodeStatus {
    NodeStatus {
      state: if self.stopped.load(Ordering::Acquire) {
        NodeState::Stopped
      } else {
        NodeState::Ready
      },
      active_command_id: None,
    }
  }

  fn stop(&self) -> Result<(), HalError> {
    if self.session.token().is_some() {
      let proof = self.session.stop(Duration::from_secs(3));
      if proof.base == ControlledStopOutcome::Stopped && proof.arm == ControlledStopOutcome::Stopped
      {
        self.stopped.store(true, Ordering::Release);
        return Ok(());
      }
      return Err(HalError::Adapter(
        "MuJoCo whole-robot stop outcome unknown".into(),
      ));
    }
    self.stopped.store(true, Ordering::Release);
    let id = Self::next_id();
    self.request("stop", &id, json!({"command_id":id}))?;
    Ok(())
  }

  fn sample_meta(&self, reply: &Value) -> SampleMeta {
    SampleMeta {
      source_timestamp_ns: reply.get("timestamp_ns").and_then(Value::as_i64),
      clock: ClockKind::Device,
      received_monotonic_ns: self.clock.monotonic_ns(),
      sequence: reply.get("sequence").and_then(Value::as_u64),
      frame_id: self.frame_id.clone(),
    }
  }
}

fn toml_string(value: &toml::Value) -> Option<String> {
  value.as_str().map(str::to_owned)
}

fn number(reply: &Value, key: &str) -> Result<f64, HalError> {
  let value = reply
    .get(key)
    .and_then(Value::as_f64)
    .ok_or_else(|| HalError::InvalidData(format!("missing {key}")))?;
  if !value.is_finite() {
    return Err(HalError::InvalidData(format!("nonfinite {key}")));
  }
  Ok(value)
}

fn array<const N: usize>(reply: &Value, key: &str) -> Result<[f64; N], HalError> {
  let values = reply
    .get(key)
    .and_then(Value::as_array)
    .ok_or_else(|| HalError::InvalidData(format!("missing {key}")))?;
  if values.len() != N {
    return Err(HalError::InvalidData(format!("invalid {key} length")));
  }
  let parsed: Vec<f64> = values
    .iter()
    .enumerate()
    .map(|(index, value)| {
      value
        .as_f64()
        .filter(|value| value.is_finite())
        .ok_or_else(|| HalError::InvalidData(format!("invalid {key}[{index}]")))
    })
    .collect::<Result<_, _>>()?;
  parsed
    .try_into()
    .map_err(|_| HalError::InvalidData(format!("invalid {key} length")))
}

pub struct MujocoCamera(pub Arc<Endpoint>);
pub struct MujocoBase(pub Arc<Endpoint>);
pub struct MujocoManipulator(pub Arc<Endpoint>);

macro_rules! node_adapter {
  ($name:ident, $kind:expr) => {
    impl NodeAdapter for $name {
      fn kind(&self) -> NodeKind {
        $kind
      }
      fn adapter_type(&self) -> &'static str {
        "mujoco-v2"
      }
      fn status(&self) -> NodeStatus {
        self.0.status()
      }
      fn health(&self) -> Health {
        self.0.health()
      }
      fn stop(&self) -> Result<(), HalError> {
        self.0.stop()
      }
      fn has_terminal_stop_fence(&self) -> bool {
        $kind != NodeKind::Camera && self.0.session.token().is_some()
      }
      fn movement_fence_token(&self) -> Option<MovementFenceToken> {
        self.0.session.token()
      }
      fn managed_robot_stop(&self, timeout: Duration) -> Option<RobotStopProof> {
        ($kind != NodeKind::Camera && self.0.session.token().is_some())
          .then(|| self.0.session.stop(timeout))
      }
      fn controlled_stop(&self) -> Result<ControlledStopOutcome, HalError> {
        let proof = self
          .managed_robot_stop(Duration::from_secs(3))
          .ok_or_else(|| HalError::UnsupportedMode("MuJoCo stop lane unavailable".into()))?;
        if proof.base == ControlledStopOutcome::Stopped
          && proof.arm == ControlledStopOutcome::Stopped
        {
          Ok(ControlledStopOutcome::Stopped)
        } else {
          Ok(ControlledStopOutcome::OutcomeUnknown)
        }
      }
      fn supports_non_latching_stop(&self) -> bool {
        $kind != NodeKind::Camera && self.0.session.token().is_some()
      }
    }
  };
}
node_adapter!(MujocoCamera, NodeKind::Camera);
node_adapter!(MujocoBase, NodeKind::Base);
node_adapter!(MujocoManipulator, NodeKind::Manipulator);

impl CameraNode for MujocoCamera {
  fn capture(&self) -> Result<CameraFrame, HalError> {
    let (reply, payload) = self
      .0
      .request("camera_capture", &Endpoint::next_id(), json!({}))?;
    if reply.get("format").and_then(Value::as_str) != Some("jpeg") {
      return Err(HalError::InvalidData("camera format is not jpeg".into()));
    }
    if reply.get("rgb_length").and_then(Value::as_u64) != Some(payload.len() as u64) {
      return Err(HalError::InvalidData("JPEG payload length mismatch".into()));
    }
    if reply.get("depth_available").and_then(Value::as_bool) != Some(false) {
      return Err(HalError::UnsupportedMode(
        "depth transport is not advertised by this simulator".into(),
      ));
    }
    let width = reply
      .get("width_px")
      .and_then(Value::as_u64)
      .and_then(|value| u32::try_from(value).ok())
      .ok_or_else(|| HalError::InvalidData("missing width_px".into()))?;
    let height = reply
      .get("height_px")
      .and_then(Value::as_u64)
      .and_then(|value| u32::try_from(value).ok())
      .ok_or_else(|| HalError::InvalidData("missing height_px".into()))?;
    let mut decoder = Decoder::new(payload.as_slice());
    let rgb8 = decoder
      .decode()
      .map_err(|error| HalError::InvalidData(format!("JPEG decode: {error}")))?;
    let info = decoder
      .info()
      .ok_or_else(|| HalError::InvalidData("JPEG has no dimensions".into()))?;
    if info.pixel_format != PixelFormat::RGB24
      || u32::from(info.width) != width
      || u32::from(info.height) != height
    {
      return Err(HalError::InvalidData("JPEG metadata mismatch".into()));
    }
    Ok(CameraFrame {
      meta: self.0.sample_meta(&reply),
      width_px: width,
      height_px: height,
      stride_bytes: width
        .checked_mul(3)
        .ok_or_else(|| HalError::InvalidData("camera stride overflow".into()))?,
      rgb8,
      depth: None,
      preview_jpeg: Some(aether_hal::CameraJpeg {
        bytes: payload,
        quality: 85,
      }),
    })
  }
}

impl BaseNode for MujocoBase {
  fn observe(&self) -> Result<BaseObservation, HalError> {
    let (reply, _) = self
      .0
      .request("base_observe", &Endpoint::next_id(), json!({}))?;
    let position = array::<2>(&reply, "position_m")?;
    let velocity = array::<2>(&reply, "linear_velocity_m_s")?;
    Ok(BaseObservation {
      meta: self.0.sample_meta(&reply),
      valid: true,
      x_m: position[0],
      y_m: position[1],
      yaw_rad: number(&reply, "yaw_rad")?,
      x_m_s: velocity[0],
      y_m_s: velocity[1],
      yaw_rad_s: number(&reply, "angular_velocity_rad_s")?,
    })
  }

  fn move_base(
    &self,
    meta: CommandMeta,
    movement: BaseMovement,
  ) -> Result<CommandOutcome, HalError> {
    self.move_base_fenced(meta, movement, None)
  }

  fn move_base_fenced(
    &self,
    meta: CommandMeta,
    movement: BaseMovement,
    token: Option<MovementFenceToken>,
  ) -> Result<CommandOutcome, HalError> {
    if self.0.stopped.load(Ordering::Acquire) {
      return Err(HalError::Adapter("base is stopped".into()));
    }
    let BaseMovement::BodyVelocity {
      x_m_s,
      y_m_s,
      yaw_rad_s,
      valid_for_ms,
    } = movement
    else {
      return Err(HalError::UnsupportedMode("relative_pose".into()));
    };
    if !(-1.0..=1.0).contains(&x_m_s)
      || !(-1.0..=1.0).contains(&y_m_s)
      || !(-1.5..=1.5).contains(&yaw_rad_s)
      || !(1..=5000).contains(&valid_for_ms)
    {
      return Err(HalError::InvalidData(
        "MuJoCo base movement is out of range".into(),
      ));
    }
    let mut fields = json!({"linear_x_m_s":x_m_s,"linear_y_m_s":y_m_s,
                   "angular_z_rad_s":yaw_rad_s,"valid_for_ms":valid_for_ms,
                   "command_id":meta.command_id});
    if self.0.session.token().is_some() {
      let token =
        token.ok_or_else(|| HalError::Adapter("missing captured movement fence".into()))?;
      fields["generation_id"] = json!(token.generation_id);
      fields["stop_epoch"] = json!(token.stop_epoch);
    }
    self.0.session.note_movement_release();
    let (reply, _) = self.0.request("base_move", &meta.command_id, fields)?;
    if reply.get("accepted").and_then(Value::as_bool) != Some(true) {
      return Err(HalError::Adapter("base command was not accepted".into()));
    }
    // BaseDrive acknowledges release, not completion. Confirm the simulator has
    // advanced past this lease and reports a stationary base before returning
    // a terminal result to the dispatcher.
    let Some(start_ns) = reply.get("timestamp_ns").and_then(Value::as_f64) else {
      return Ok(CommandOutcome::Ambiguous);
    };
    let target_ns = start_ns + valid_for_ms as f64 * 1_000_000.0;
    let start_sequence = reply.get("sequence").and_then(Value::as_u64);
    let wall_deadline = Instant::now() + Duration::from_secs(3);
    while !self.0.stopped.load(Ordering::Acquire)
      && self.0.clock.monotonic_ns() < meta.deadline_monotonic_ns
      && Instant::now() < wall_deadline
    {
      let Ok((sample, _)) = self
        .0
        .request("base_observe", &Endpoint::next_id(), json!({}))
      else {
        return Ok(CommandOutcome::Ambiguous);
      };
      let time = sample.get("timestamp_ns").and_then(Value::as_f64);
      let sequence = sample.get("sequence").and_then(Value::as_u64);
      let linear = array::<2>(&sample, "linear_velocity_m_s");
      let angular = number(&sample, "angular_velocity_rad_s");
      if let (Some(time), Some(sequence), Ok(linear), Ok(angular)) =
        (time, sequence, linear, angular)
      {
        if time >= target_ns
          && start_sequence.is_none_or(|start| sequence > start)
          && linear.iter().all(|speed| speed.abs() < 0.02)
          && angular.abs() < 0.02
          && !self.0.stopped.load(Ordering::Acquire)
        {
          return Ok(CommandOutcome::Completed);
        }
      } else {
        return Ok(CommandOutcome::Ambiguous);
      }
      std::thread::sleep(Duration::from_millis(10));
    }
    Ok(CommandOutcome::Ambiguous)
  }
}

impl ManipulatorNode for MujocoManipulator {
  fn observe(&self) -> Result<ManipulatorObservation, HalError> {
    self.0.session.note_movement_release();
    let (reply, _) = self
      .0
      .request("manipulator_observe", &Endpoint::next_id(), json!({}))?;
    let count = self.0.joint_names.len();
    let positions = reply
      .get("joint_positions_rad")
      .and_then(Value::as_array)
      .ok_or_else(|| HalError::InvalidData("missing joint positions".into()))?;
    let velocities = reply
      .get("joint_velocities_rad_s")
      .and_then(Value::as_array)
      .ok_or_else(|| HalError::InvalidData("missing joint velocities".into()))?;
    if count == 0 || positions.len() != count || velocities.len() != count {
      return Err(HalError::InvalidData("joint array length mismatch".into()));
    }
    let parse = |values: &[Value]| -> Result<Vec<f64>, HalError> {
      values
        .iter()
        .map(|value| {
          value
            .as_f64()
            .filter(|number| number.is_finite())
            .ok_or_else(|| HalError::InvalidData("invalid joint value".into()))
        })
        .collect()
    };
    Ok(ManipulatorObservation {
      meta: self.0.sample_meta(&reply),
      joint_names: self.0.joint_names.clone(),
      joint_positions_rad: parse(positions)?,
      joint_velocities_rad_s: parse(velocities)?,
      joints_valid: true,
      end_effector_pose: Some(EndEffectorPose {
        position_m: array::<3>(&reply, "end_position_m")?,
        orientation_xyzw: array::<4>(&reply, "end_orientation_xyzw")?,
        frame_id: self.0.frame_id.clone(),
      }),
      end_effector_valid: true,
    })
  }

  fn move_manipulator(
    &self,
    meta: CommandMeta,
    movement: ManipulatorMovement,
  ) -> Result<CommandOutcome, HalError> {
    self.move_manipulator_fenced(meta, movement, None)
  }

  fn move_manipulator_fenced(
    &self,
    meta: CommandMeta,
    movement: ManipulatorMovement,
    token: Option<MovementFenceToken>,
  ) -> Result<CommandOutcome, HalError> {
    if self.0.stopped.load(Ordering::Acquire) {
      return Err(HalError::Adapter("manipulator is stopped".into()));
    }
    let ManipulatorMovement::JointPositions {
      joint_names,
      joint_positions_rad,
      ..
    } = movement
    else {
      return Err(HalError::UnsupportedMode("end_effector_pose".into()));
    };
    if joint_names != self.0.joint_names || joint_positions_rad.len() != joint_names.len() {
      return Err(HalError::InvalidData(
        "joint names or count mismatch".into(),
      ));
    }
    if joint_positions_rad.iter().any(|value| !value.is_finite()) {
      return Err(HalError::InvalidData(
        "nonfinite MuJoCo joint target".into(),
      ));
    }
    let mut fields =
      json!({"joint_positions_rad":joint_positions_rad,"command_id":meta.command_id});
    if self.0.session.token().is_some() {
      let token =
        token.ok_or_else(|| HalError::Adapter("missing captured movement fence".into()))?;
      fields["generation_id"] = json!(token.generation_id);
      fields["stop_epoch"] = json!(token.stop_epoch);
    }
    let (reply, _) = self
      .0
      .request("manipulator_move", &meta.command_id, fields)?;
    if reply.get("accepted").and_then(Value::as_bool) != Some(true) {
      return Err(HalError::Adapter(
        "manipulator command was not accepted".into(),
      ));
    }
    // MuJoCo ArmMoveJ replies accepted only after Engine::Execute reaches
    // the joint target. Check a fresh typed sample as independent evidence.
    if self.0.stopped.load(Ordering::Acquire)
      || self.0.clock.monotonic_ns() >= meta.deadline_monotonic_ns
    {
      return Ok(CommandOutcome::Ambiguous);
    }
    match self.observe() {
      Ok(observation)
        if !self.0.stopped.load(Ordering::Acquire)
          && observation
            .joint_positions_rad
            .iter()
            .zip(&joint_positions_rad)
            .all(|(actual, target)| (actual - target).abs() <= 0.035) =>
      {
        Ok(CommandOutcome::Completed)
      }
      _ => Ok(CommandOutcome::Ambiguous),
    }
  }
}
