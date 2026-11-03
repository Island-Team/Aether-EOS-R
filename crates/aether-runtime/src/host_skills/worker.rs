//! Private re-exec protocol and ABI inspection. No package pointer leaves the
//! worker process. The packet protocol is build-private, not part of ABI v2.

use std::collections::BTreeSet;
use std::ffi::{c_char, c_void, CString};
use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::mem::{align_of, offset_of, size_of};
use std::os::fd::{AsRawFd, FromRawFd, RawFd};
use std::os::unix::process::CommandExt;
use std::path::Path;
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use aether_hal::PRIMITIVE_SPECS;
use serde::{Deserialize, Serialize};

use super::{
  fail, valid_abi_name, valid_abi_version, validate_elf, validate_schema, HostError,
  PrimitiveReference, SkillDescriptor,
};

const SOCKET_FD: RawFd = 3;
const STAGE_FD: RawFd = 4;
const MAX_PACKET: usize = 64 * 1024;
const MAX_BULK: usize = 64 * 1024 * 1024;
const BULK_MARKER: &[u8] = b"\0EOS-SKILL-BULK1";
static CANCELLED: AtomicBool = AtomicBool::new(false);

#[derive(Clone, Copy, Debug)]
pub enum WorkerMode {
  Inspect,
  Invoke,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
pub enum WorkerEvent {
  Catalog {
    descriptors: Vec<SkillDescriptor>,
  },
  PrimitiveCall {
    name: String,
    version: String,
    device_id: String,
    arguments_json: String,
  },
  Completion {
    accepted: bool,
    reason_code: String,
    result_json: String,
    message: String,
    status: u32,
  },
  Failure {
    code: String,
  },
}

#[derive(Deserialize, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
enum WorkerRequest {
  Inspect,
  Invoke {
    descriptor_index: usize,
    command_id: String,
    arguments_json: String,
  },
  PrimitiveReply {
    status: u32,
    acknowledgement_json: String,
  },
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Packet<T> {
  version: u32,
  body: T,
}

pub struct WorkerSession {
  child: Option<Child>,
  socket: File,
  cancel_handle: WorkerCancelHandle,
  reaped: bool,
  exit_status: Option<ExitStatus>,
}

/// Cloneable, PID-reuse-safe signal handle. The session remains the sole
/// owner of the child and must still perform the bounded reap.
#[derive(Clone)]
pub struct WorkerCancelHandle {
  pidfd: Arc<File>,
}

impl WorkerCancelHandle {
  fn signal(&self, signal: libc::c_int) -> Result<(), HostError> {
    let result = unsafe {
      libc::syscall(
        libc::SYS_pidfd_send_signal,
        self.pidfd.as_raw_fd(),
        signal,
        std::ptr::null::<libc::siginfo_t>(),
        0,
      )
    };
    if result < 0 {
      return Err(fail("worker_signal_failed"));
    }
    Ok(())
  }

  pub fn cancel(&self) -> Result<(), HostError> {
    self.signal(libc::SIGUSR1)
  }

  pub fn terminate(&self) -> Result<(), HostError> {
    self.signal(libc::SIGTERM)
  }

  pub fn kill(&self) -> Result<(), HostError> {
    self.signal(libc::SIGKILL)
  }
}

impl WorkerSession {
  pub(crate) fn spawn(
    executable: &Path,
    staged: &File,
    mode: WorkerMode,
  ) -> Result<Self, HostError> {
    let mut sockets = [0; 2];
    if unsafe {
      libc::socketpair(
        libc::AF_UNIX,
        libc::SOCK_SEQPACKET | libc::SOCK_CLOEXEC,
        0,
        sockets.as_mut_ptr(),
      )
    } < 0
    {
      return Err(fail("worker_socket_failed"));
    }
    let parent = unsafe { File::from_raw_fd(sockets[0]) };
    let child_socket = unsafe { File::from_raw_fd(sockets[1]) };
    let independent_stage = File::open(format!("/proc/self/fd/{}", staged.as_raw_fd()))
      .map_err(|_| fail("worker_stage_invalid"))?;
    let socket = unsafe { libc::fcntl(child_socket.as_raw_fd(), libc::F_DUPFD_CLOEXEC, 5) };
    let source = unsafe { libc::fcntl(independent_stage.as_raw_fd(), libc::F_DUPFD_CLOEXEC, 5) };
    if socket < 0 || source < 0 {
      if socket >= 0 {
        unsafe {
          libc::close(socket);
        }
      }
      if source >= 0 {
        unsafe {
          libc::close(source);
        }
      }
      return Err(fail("worker_fd_failed"));
    }
    let socket_copy = unsafe { File::from_raw_fd(socket) };
    let source_copy = unsafe { File::from_raw_fd(source) };
    let mut command = Command::new(executable);
    command.arg("--skill-worker");
    command.env_clear();
    command
      .stdin(Stdio::null())
      .stdout(Stdio::null())
      .stderr(Stdio::null());
    // Only async-signal-safe syscalls run after fork and before exec.
    unsafe {
      command.pre_exec(move || {
        if libc::dup2(socket, SOCKET_FD) < 0 || libc::dup2(source, STAGE_FD) < 0 {
          return Err(io::Error::last_os_error());
        }
        if libc::syscall(libc::SYS_close_range, 5u32, u32::MAX, 0u32) < 0 {
          return Err(io::Error::last_os_error());
        }
        Ok(())
      });
    }
    let child = command.spawn().map_err(|_| fail("worker_spawn_failed"))?;
    let pidfd = unsafe { libc::syscall(libc::SYS_pidfd_open, child.id() as libc::pid_t, 0) };
    if pidfd < 0 {
      let mut child = child;
      let _ = child.kill();
      let until = Instant::now() + Duration::from_secs(1);
      loop {
        match child.try_wait() {
          Ok(Some(_)) => return Err(fail("worker_pidfd_failed")),
          Ok(None) if Instant::now() < until => std::thread::sleep(Duration::from_millis(10)),
          _ => {
            std::thread::spawn(move || {
              let _ = child.wait();
            });
            return Err(fail("worker_reap_failed"));
          }
        }
      }
    }
    drop(socket_copy);
    drop(source_copy);
    drop(child_socket);
    let mut session = Self {
      child: Some(child),
      socket: parent,
      cancel_handle: WorkerCancelHandle {
        pidfd: Arc::new(unsafe { File::from_raw_fd(pidfd as RawFd) }),
      },
      reaped: false,
      exit_status: None,
    };
    let request = match mode {
      WorkerMode::Inspect => WorkerRequest::Inspect,
      WorkerMode::Invoke => return Ok(session),
    };
    if let Err(error) = session.send(&request) {
      let reaped = session.terminate_reap().unwrap_or(false);
      return Err(post_spawn_error(error, reaped));
    }
    Ok(session)
  }

  pub fn pid(&self) -> u32 {
    self.child.as_ref().expect("worker child retained").id()
  }

  pub fn cancellation_handle(&self) -> WorkerCancelHandle {
    self.cancel_handle.clone()
  }

  pub fn exit_status(&self) -> Option<ExitStatus> {
    self.exit_status
  }

  pub fn invoke(
    &mut self,
    descriptor_index: usize,
    command_id: &str,
    arguments_json: &str,
  ) -> Result<(), HostError> {
    if command_id.is_empty() || command_id.len() > 96 || arguments_json.len() > 1024 * 1024 {
      return Err(fail("worker_request_invalid"));
    }
    self.send(&WorkerRequest::Invoke {
      descriptor_index,
      command_id: command_id.into(),
      arguments_json: arguments_json.into(),
    })
  }

  pub fn reply_primitive(
    &mut self,
    status: u32,
    acknowledgement_json: &str,
  ) -> Result<(), HostError> {
    if status > 4 || acknowledgement_json.len() > 1024 * 1024 {
      return Err(fail("worker_reply_invalid"));
    }
    self.send(&WorkerRequest::PrimitiveReply {
      status,
      acknowledgement_json: acknowledgement_json.into(),
    })
  }

  fn send(&mut self, message: &WorkerRequest) -> Result<(), HostError> {
    let payload = serde_json::to_vec(&Packet {
      version: 1,
      body: message,
    })
    .map_err(|_| fail("worker_protocol_invalid"))?;
    send_packet(self.socket.as_raw_fd(), &payload)
  }

  pub fn recv(&mut self, timeout: Duration) -> Result<WorkerEvent, HostError> {
    let payload = recv_packet(self.socket.as_raw_fd(), timeout)?;
    let packet: Packet<WorkerEvent> =
      serde_json::from_slice(&payload).map_err(|_| fail("worker_protocol_invalid"))?;
    if packet.version != 1 {
      return Err(fail("worker_protocol_invalid"));
    }
    Ok(packet.body)
  }

  pub fn cancel(&mut self) {
    if !self.reaped {
      let _ = self.cancel_handle.cancel();
    }
  }

  pub fn reap(&mut self, timeout: Duration) -> Result<bool, HostError> {
    if self.reaped {
      return Ok(true);
    }
    let until = Instant::now() + timeout;
    loop {
      match self
        .child
        .as_mut()
        .ok_or_else(|| fail("worker_reap_failed"))?
        .try_wait()
      {
        Ok(Some(status)) => {
          self.reaped = true;
          self.exit_status = Some(status);
          return Ok(true);
        }
        Ok(None) if Instant::now() < until => std::thread::sleep(Duration::from_millis(10)),
        Ok(None) => return Ok(false),
        Err(_) => return Err(fail("worker_reap_failed")),
      }
    }
  }

  pub fn terminate_reap(&mut self) -> Result<bool, HostError> {
    if self.reaped {
      return Ok(true);
    }
    self.cancel();
    if self.reap(Duration::from_secs(1)).unwrap_or(false) {
      return Ok(true);
    }
    let _ = self.cancel_handle.terminate();
    if self.reap(Duration::from_secs(1)).unwrap_or(false) {
      return Ok(true);
    }
    let _ = self.cancel_handle.kill();
    self.reap(Duration::from_secs(1))
  }
}

impl Drop for WorkerSession {
  fn drop(&mut self) {
    if !self.reaped && !self.terminate_reap().unwrap_or(false) {
      // Retain ownership of an unreaped child in a background waiter. The
      // caller already receives worker_reap_failed from explicit shutdown.
      if let Some(mut child) = self.child.take() {
        std::thread::spawn(move || {
          let _ = child.wait();
        });
      }
    }
  }
}

fn post_spawn_error(error: HostError, worker_reaped: bool) -> HostError {
  if worker_reaped {
    error
  } else {
    fail("worker_reap_failed")
  }
}

fn send_packet(fd: RawFd, bytes: &[u8]) -> Result<(), HostError> {
  send_packet_with_timeout(fd, bytes, Duration::from_secs(5))
}

fn bounded_send(
  fd: RawFd,
  expected: usize,
  timeout: Duration,
  mut operation: impl FnMut() -> isize,
) -> Result<(), HostError> {
  let deadline = Instant::now() + timeout;
  loop {
    let written = operation();
    if written == expected as isize {
      return Ok(());
    }
    if written >= 0 {
      return Err(fail("worker_protocol_io"));
    }
    let error = io::Error::last_os_error();
    if error.kind() == io::ErrorKind::Interrupted {
      continue;
    }
    if error.kind() != io::ErrorKind::WouldBlock {
      return Err(fail("worker_protocol_io"));
    }
    let remaining = deadline.saturating_duration_since(Instant::now());
    if remaining.is_zero() {
      return Err(fail("worker_timeout"));
    }
    let mut poll = libc::pollfd {
      fd,
      events: libc::POLLOUT,
      revents: 0,
    };
    let wait = remaining.as_millis().clamp(1, i32::MAX as u128) as i32;
    let ready = unsafe { libc::poll(&mut poll, 1, wait) };
    if ready == 0 {
      return Err(fail("worker_timeout"));
    }
    if ready < 0 && io::Error::last_os_error().kind() != io::ErrorKind::Interrupted {
      return Err(fail("worker_protocol_io"));
    }
  }
}

fn send_packet_with_timeout(fd: RawFd, bytes: &[u8], timeout: Duration) -> Result<(), HostError> {
  if bytes.len() > MAX_BULK {
    return Err(fail("worker_packet_oversize"));
  }
  if bytes.len() <= MAX_PACKET {
    return bounded_send(fd, bytes.len(), timeout, || unsafe {
      libc::send(
        fd,
        bytes.as_ptr().cast(),
        bytes.len(),
        libc::MSG_NOSIGNAL | libc::MSG_DONTWAIT,
      )
    });
  }
  let label = CString::new("eos-skill-packet").expect("static label");
  let bulk_fd =
    unsafe { libc::memfd_create(label.as_ptr(), libc::MFD_CLOEXEC | libc::MFD_ALLOW_SEALING) };
  if bulk_fd < 0 {
    return Err(fail("worker_bulk_unavailable"));
  }
  let mut bulk = unsafe { File::from_raw_fd(bulk_fd) };
  bulk
    .write_all(bytes)
    .map_err(|_| fail("worker_bulk_unavailable"))?;
  bulk
    .seek(SeekFrom::Start(0))
    .map_err(|_| fail("worker_bulk_unavailable"))?;
  let seals = libc::F_SEAL_SEAL | libc::F_SEAL_SHRINK | libc::F_SEAL_GROW | libc::F_SEAL_WRITE;
  if unsafe { libc::fcntl(bulk_fd, libc::F_ADD_SEALS, seals) } < 0 {
    return Err(fail("worker_bulk_unavailable"));
  }
  let mut header = Vec::with_capacity(BULK_MARKER.len() + 8);
  header.extend_from_slice(BULK_MARKER);
  header.extend_from_slice(&(bytes.len() as u64).to_le_bytes());
  let mut iov = libc::iovec {
    iov_base: header.as_mut_ptr().cast(),
    iov_len: header.len(),
  };
  let mut control = vec![0u8; unsafe { libc::CMSG_SPACE(size_of::<RawFd>() as u32) } as usize];
  let mut message: libc::msghdr = unsafe { std::mem::zeroed() };
  message.msg_iov = &mut iov;
  message.msg_iovlen = 1;
  message.msg_control = control.as_mut_ptr().cast();
  message.msg_controllen = control.len();
  let cmsg = unsafe { libc::CMSG_FIRSTHDR(&message) };
  if cmsg.is_null() {
    return Err(fail("worker_bulk_unavailable"));
  }
  unsafe {
    (*cmsg).cmsg_level = libc::SOL_SOCKET;
    (*cmsg).cmsg_type = libc::SCM_RIGHTS;
    (*cmsg).cmsg_len = libc::CMSG_LEN(size_of::<RawFd>() as u32) as usize;
    std::ptr::write(libc::CMSG_DATA(cmsg).cast::<RawFd>(), bulk_fd);
  }
  bounded_send(fd, header.len(), timeout, || unsafe {
    libc::sendmsg(fd, &message, libc::MSG_NOSIGNAL | libc::MSG_DONTWAIT)
  })
}

fn recv_packet(fd: RawFd, timeout: Duration) -> Result<Vec<u8>, HostError> {
  let milliseconds = timeout.as_millis().min(i32::MAX as u128) as i32;
  let mut poll = libc::pollfd {
    fd,
    events: libc::POLLIN,
    revents: 0,
  };
  if unsafe { libc::poll(&mut poll, 1, milliseconds) } <= 0 {
    return Err(fail("worker_timeout"));
  }
  let mut bytes = vec![0u8; MAX_PACKET + 1];
  let mut iov = libc::iovec {
    iov_base: bytes.as_mut_ptr().cast(),
    iov_len: bytes.len(),
  };
  let mut control = vec![0u8; unsafe { libc::CMSG_SPACE(size_of::<RawFd>() as u32) } as usize];
  let mut message: libc::msghdr = unsafe { std::mem::zeroed() };
  message.msg_iov = &mut iov;
  message.msg_iovlen = 1;
  message.msg_control = control.as_mut_ptr().cast();
  message.msg_controllen = control.len();
  let length = unsafe { libc::recvmsg(fd, &mut message, libc::MSG_CMSG_CLOEXEC) };
  if length <= 0
    || length as usize > MAX_PACKET
    || message.msg_flags & (libc::MSG_TRUNC | libc::MSG_CTRUNC) != 0
  {
    return Err(fail("worker_packet_invalid"));
  }
  bytes.truncate(length as usize);
  let cmsg = unsafe { libc::CMSG_FIRSTHDR(&message) };
  if bytes.starts_with(BULK_MARKER) {
    if bytes.len() != BULK_MARKER.len() + 8 || cmsg.is_null() {
      return Err(fail("worker_packet_invalid"));
    }
    let expected = u64::from_le_bytes(
      bytes[BULK_MARKER.len()..]
        .try_into()
        .expect("length checked"),
    );
    if expected as usize > MAX_BULK || expected < MAX_PACKET as u64 + 1 {
      return Err(fail("worker_packet_invalid"));
    }
    if unsafe {
      (*cmsg).cmsg_level != libc::SOL_SOCKET
        || (*cmsg).cmsg_type != libc::SCM_RIGHTS
        || (*cmsg).cmsg_len != libc::CMSG_LEN(size_of::<RawFd>() as u32) as usize
        || !libc::CMSG_NXTHDR(&message, cmsg).is_null()
    } {
      return Err(fail("worker_packet_invalid"));
    }
    let bulk_fd = unsafe { std::ptr::read(libc::CMSG_DATA(cmsg).cast::<RawFd>()) };
    let bulk = unsafe { File::from_raw_fd(bulk_fd) };
    let seals = unsafe { libc::fcntl(bulk_fd, libc::F_GET_SEALS) };
    let required = libc::F_SEAL_SEAL | libc::F_SEAL_SHRINK | libc::F_SEAL_GROW | libc::F_SEAL_WRITE;
    if seals < 0
      || seals & required != required
      || bulk.metadata().map_or(true, |meta| meta.len() != expected)
    {
      return Err(fail("worker_packet_invalid"));
    }
    let mut content = Vec::with_capacity(expected as usize);
    bulk
      .take(expected + 1)
      .read_to_end(&mut content)
      .map_err(|_| fail("worker_packet_invalid"))?;
    if content.len() != expected as usize {
      return Err(fail("worker_packet_invalid"));
    }
    return Ok(content);
  }
  if !cmsg.is_null() {
    return Err(fail("worker_packet_invalid"));
  }
  Ok(bytes)
}

#[repr(C)]
#[derive(Clone, Copy)]
struct CText {
  data: *const c_char,
  size: u32,
}

#[repr(C)]
struct CLibrary {
  abi_version: u32,
  struct_size: u32,
  name: CText,
  version: CText,
  build_id: CText,
  skill_count: u32,
  skills: *const CDescriptor,
  skill_descriptor_size: u32,
}

#[repr(C)]
struct CLibraryBase {
  abi_version: u32,
  struct_size: u32,
  name: CText,
  version: CText,
  build_id: CText,
  skill_count: u32,
  skills: *const CDescriptor,
}

#[repr(C)]
struct CPrimitive {
  struct_size: u32,
  name: CText,
  version: CText,
}

#[repr(C)]
struct CDescriptor {
  struct_size: u32,
  name: CText,
  version: CText,
  description: CText,
  parameters_schema_json: CText,
  returns_schema_json: CText,
  primitive_count: u32,
  primitives: *const CPrimitive,
  resource_count: u32,
  resources: *const c_void,
  signal_count: u32,
  signals: *const c_void,
  claim_count: u32,
  claims: *const c_void,
  reentrant: u32,
  context: *mut c_void,
  execute:
    Option<unsafe extern "C" fn(*mut c_void, *const CExecution, CComplete, *mut c_void) -> u32>,
  source_count: u32,
  sources: *const c_void,
  inference_count: u32,
  inferences: *const c_void,
  ipc_channel_count: u32,
  ipc_channels: *const c_void,
}

#[repr(C)]
struct CDescriptorBase {
  struct_size: u32,
  name: CText,
  version: CText,
  description: CText,
  parameters_schema_json: CText,
  returns_schema_json: CText,
  primitive_count: u32,
  primitives: *const CPrimitive,
  resource_count: u32,
  resources: *const c_void,
  signal_count: u32,
  signals: *const c_void,
  claim_count: u32,
  claims: *const c_void,
  reentrant: u32,
  context: *mut c_void,
  execute:
    Option<unsafe extern "C" fn(*mut c_void, *const CExecution, CComplete, *mut c_void) -> u32>,
}

type CComplete = unsafe extern "C" fn(*mut c_void, *const CCompletion) -> u32;

#[repr(C)]
struct CHost {
  abi_version: u32,
  struct_size: u32,
  context: *mut c_void,
  call_primitive: Option<
    unsafe extern "C" fn(*mut c_void, *const CPrimitiveRequest, *mut CPrimitiveResponse) -> u32,
  >,
  is_cancelled: Option<unsafe extern "C" fn(*mut c_void) -> u32>,
  read_resource: *const c_void,
  receive_signal: *const c_void,
  emit_signal: *const c_void,
}

#[repr(C)]
struct CExecution {
  struct_size: u32,
  command_id: CText,
  arguments_json: CText,
  host: *const CHost,
}

#[repr(C)]
struct CCompletion {
  struct_size: u32,
  accepted: u32,
  reason_code: CText,
  result_json: CText,
  message: CText,
}

#[repr(C)]
struct CCompletionBase {
  struct_size: u32,
  accepted: u32,
  reason_code: CText,
  result_json: CText,
}

#[repr(C)]
struct CPrimitiveRequest {
  struct_size: u32,
  primitive: CText,
  version: CText,
  device_id: CText,
  arguments_json: CText,
}

#[repr(C)]
struct CPrimitiveResponse {
  struct_size: u32,
  acknowledgement_json: CText,
  error_message: CText,
}

fn copy_text(text: CText, max: usize, required: bool) -> Result<String, HostError> {
  if text.size as usize > max
    || (text.size > 0 && text.data.is_null())
    || (required && text.size == 0)
  {
    return Err(fail("abi_text_invalid"));
  }
  if text.size == 0 {
    return Ok(String::new());
  }
  let bytes = unsafe { std::slice::from_raw_parts(text.data.cast::<u8>(), text.size as usize) };
  String::from_utf8(bytes.to_vec()).map_err(|_| fail("abi_text_invalid"))
}

fn valid_name(value: &str) -> bool {
  valid_abi_name(value)
}

fn valid_version(value: &str) -> bool {
  valid_abi_version(value)
}

fn schema(text: CText) -> Result<serde_json::Value, HostError> {
  let source = copy_text(text, 1024 * 1024, true)?;
  let parsed: serde_json::Value =
    serde_json::from_str(&source).map_err(|_| fail("abi_schema_invalid"))?;
  validate_schema(&parsed)?;
  Ok(parsed)
}

unsafe fn inspect_library(library: *const CLibrary) -> Result<Vec<SkillDescriptor>, HostError> {
  if library.is_null() {
    return Err(fail("abi_library_invalid"));
  }
  let base = unsafe { &*library.cast::<CLibraryBase>() };
  let base_size = size_of::<CLibraryBase>();
  if base.abi_version != 2 || (base.struct_size as usize) < base_size {
    return Err(fail("abi_library_invalid"));
  }
  let name = copy_text(base.name, 4096, true)?;
  let version = copy_text(base.version, 4096, true)?;
  let _build = copy_text(base.build_id, 4096, false)?;
  if !valid_name(&name)
    || !valid_version(&version)
    || base.skill_count == 0
    || base.skill_count > 256
    || base.skills.is_null()
  {
    return Err(fail("abi_library_invalid"));
  }
  let stride = if base.struct_size as usize >= size_of::<CLibrary>() {
    unsafe { (*library).skill_descriptor_size as usize }
  } else {
    size_of::<CDescriptorBase>()
  };
  let descriptor_base = size_of::<CDescriptorBase>();
  if stride < descriptor_base || stride > 4096 || stride % align_of::<CDescriptor>() != 0 {
    return Err(fail("abi_stride_invalid"));
  }
  let mut descriptors = Vec::new();
  let mut seen = BTreeSet::new();
  for index in 0..base.skill_count as usize {
    let raw = unsafe {
      (base.skills as *const u8)
        .add(index * stride)
        .cast::<CDescriptor>()
    };
    let descriptor = unsafe { &*raw.cast::<CDescriptorBase>() };
    if (descriptor.struct_size as usize) < descriptor_base
      || descriptor.struct_size as usize > stride
      || descriptor.execute.is_none()
      || descriptor.reentrant > 1
    {
      return Err(fail("abi_descriptor_invalid"));
    }
    let unsupported =
      descriptor.resource_count != 0 || descriptor.signal_count != 0 || descriptor.claim_count != 0;
    if unsupported {
      return Err(fail("abi_capability_unowned"));
    }
    for offset in [
      offset_of!(CDescriptor, source_count),
      offset_of!(CDescriptor, inference_count),
      offset_of!(CDescriptor, ipc_channel_count),
    ] {
      if descriptor.struct_size as usize >= offset + size_of::<u32>()
        && unsafe { std::ptr::read((raw as *const u8).add(offset).cast::<u32>()) } != 0
      {
        return Err(fail("abi_capability_unowned"));
      }
    }
    let name = copy_text(descriptor.name, 4096, true)?;
    let version = copy_text(descriptor.version, 4096, true)?;
    let description = copy_text(descriptor.description, 4096, false)?;
    if !valid_name(&name)
      || !valid_version(&version)
      || !seen.insert((name.clone(), version.clone()))
    {
      return Err(fail("abi_descriptor_invalid"));
    }
    let parameters_schema = schema(descriptor.parameters_schema_json)?;
    let returns_schema = schema(descriptor.returns_schema_json)?;
    if descriptor.primitive_count > 32
      || (descriptor.primitive_count != 0 && descriptor.primitives.is_null())
    {
      return Err(fail("abi_primitive_invalid"));
    }
    let mut primitives = Vec::new();
    let mut seen_primitives = BTreeSet::new();
    for primitive in 0..descriptor.primitive_count as usize {
      let reference = unsafe { &*descriptor.primitives.add(primitive) };
      if reference.struct_size as usize != size_of::<CPrimitive>() {
        return Err(fail("abi_primitive_invalid"));
      }
      let name = copy_text(reference.name, 4096, true)?;
      let version = copy_text(reference.version, 4096, true)?;
      if !PRIMITIVE_SPECS
        .iter()
        .any(|spec| spec.name == name && spec.version == version)
        || !seen_primitives.insert((name.clone(), version.clone()))
      {
        return Err(fail("abi_primitive_invalid"));
      }
      primitives.push(PrimitiveReference { name, version });
    }
    descriptors.push(SkillDescriptor {
      name,
      version,
      package_id: String::new(),
      package_version: String::new(),
      description,
      parameters_schema,
      returns_schema,
      primitives,
      reentrant: descriptor.reentrant == 1,
      library_index: 0,
      descriptor_index: index,
    });
  }
  Ok(descriptors)
}

fn open_library() -> Result<libloading::Library, HostError> {
  let mut stage = unsafe { File::from_raw_fd(STAGE_FD) };
  let mut bytes = Vec::new();
  use std::io::Read;
  (&mut stage)
    .take(64 * 1024 * 1024 + 1)
    .read_to_end(&mut bytes)
    .map_err(|_| fail("worker_stage_invalid"))?;
  std::mem::forget(stage);
  if bytes.len() > 64 * 1024 * 1024 {
    return Err(fail("worker_stage_invalid"));
  }
  validate_elf(&bytes)?;
  unsafe { libloading::Library::new("/proc/self/fd/4") }.map_err(|_| fail("worker_load_failed"))
}

fn query(library: &libloading::Library) -> Result<*const CLibrary, HostError> {
  let symbol: libloading::Symbol<unsafe extern "C" fn(u32) -> *const CLibrary> =
    unsafe { library.get(b"eos_skill_library_query_v2\0") }
      .map_err(|_| fail("abi_query_missing"))?;
  Ok(unsafe { symbol(2) })
}

extern "C" fn on_cancel(_: libc::c_int) {
  CANCELLED.store(true, Ordering::SeqCst);
}

pub fn worker_main() -> i32 {
  if std::env::vars_os().next().is_some() || unexpected_fds() || !parent_is_peer() {
    return 2;
  }
  unsafe {
    libc::signal(libc::SIGUSR1, on_cancel as *const () as usize);
  }
  if sandbox().is_err() {
    return 3;
  }
  let request = match recv_packet(SOCKET_FD, Duration::from_secs(5)) {
    Ok(packet) => match serde_json::from_slice::<Packet<WorkerRequest>>(&packet) {
      Ok(request) if request.version == 1 => request.body,
      _ => return 4,
    },
    Err(_) => return 4,
  };
  let library = match open_library() {
    Ok(library) => library,
    Err(error) => {
      send_failure(error.code);
      return 5;
    }
  };
  let table = match query(&library) {
    Ok(table) => table,
    Err(error) => {
      send_failure(error.code);
      return 5;
    }
  };
  let descriptors = match unsafe { inspect_library(table) } {
    Ok(descriptors) => descriptors,
    Err(error) => {
      send_failure(error.code);
      return 5;
    }
  };
  match request {
    WorkerRequest::Inspect => {
      if send_event(&WorkerEvent::Catalog { descriptors }).is_err() {
        return 6;
      }
      0
    }
    WorkerRequest::Invoke {
      descriptor_index,
      command_id,
      arguments_json,
    } => invoke(
      table,
      &descriptors,
      descriptor_index,
      &command_id,
      &arguments_json,
    ),
    WorkerRequest::PrimitiveReply { .. } => 4,
  }
}

fn send_event(event: &WorkerEvent) -> Result<(), HostError> {
  let bytes = serde_json::to_vec(&Packet {
    version: 1,
    body: event,
  })
  .map_err(|_| fail("worker_protocol_invalid"))?;
  send_packet(SOCKET_FD, &bytes)
}

fn send_failure(code: &str) {
  let _ = send_event(&WorkerEvent::Failure { code: code.into() });
}

fn unexpected_fds() -> bool {
  let Ok(entries) = std::fs::read_dir("/proc/self/fd") else {
    return true;
  };
  let directory = format!("/proc/{}/fd", std::process::id());
  entries.filter_map(Result::ok).any(|entry| {
    entry
      .file_name()
      .to_str()
      .and_then(|name| name.parse::<i32>().ok())
      .is_some_and(|fd| {
        fd > 4
          && std::fs::read_link(entry.path()).map_or(true, |target| target != Path::new(&directory))
      })
  })
}

fn parent_is_peer() -> bool {
  let mut peer: libc::ucred = unsafe { std::mem::zeroed() };
  let mut length = size_of::<libc::ucred>() as libc::socklen_t;
  let okay = unsafe {
    libc::getsockopt(
      SOCKET_FD,
      libc::SOL_SOCKET,
      libc::SO_PEERCRED,
      (&mut peer as *mut libc::ucred).cast(),
      &mut length,
    )
  } == 0;
  okay
    && length as usize == size_of::<libc::ucred>()
    && peer.pid == unsafe { libc::getppid() }
    && peer.uid == unsafe { libc::geteuid() }
}

fn sandbox() -> Result<(), HostError> {
  if unsafe { libc::prctl(libc::PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) } != 0 {
    return Err(fail("sandbox_nnp_failed"));
  }
  for (resource, value) in [
    (libc::RLIMIT_NOFILE, 16u64),
    (libc::RLIMIT_CORE, 0),
    (libc::RLIMIT_FSIZE, MAX_BULK as u64),
    (libc::RLIMIT_AS, 512 * 1024 * 1024),
    (libc::RLIMIT_NPROC, 0),
  ] {
    let limit = libc::rlimit {
      rlim_cur: value,
      rlim_max: value,
    };
    if unsafe { libc::setrlimit(resource, &limit) } != 0 {
      return Err(fail("sandbox_rlimit_failed"));
    }
  }
  landlock()?;
  seccomp()?;
  Ok(())
}

#[repr(C)]
struct RulesetAttr {
  handled_access_fs: u64,
  handled_access_net: u64,
  scoped: u64,
}
#[repr(C)]
struct PathRule {
  allowed_access: u64,
  parent_fd: i32,
}

fn landlock() -> Result<(), HostError> {
  const READ: u64 = (1 << 0) | (1 << 2) | (1 << 3);
  const ALL: u64 = (1 << 15) - 1;
  let attr = RulesetAttr {
    handled_access_fs: ALL,
    handled_access_net: 0,
    scoped: 0,
  };
  let ruleset = unsafe {
    libc::syscall(
      libc::SYS_landlock_create_ruleset,
      &attr,
      size_of::<RulesetAttr>(),
      0,
    )
  } as i32;
  if ruleset < 0 {
    return Err(fail("sandbox_landlock_failed"));
  }
  let add = |fd: i32, access: u64| -> Result<(), HostError> {
    let rule = PathRule {
      allowed_access: access,
      parent_fd: fd,
    };
    if unsafe { libc::syscall(libc::SYS_landlock_add_rule, ruleset, 1, &rule, 0) } < 0 {
      Err(fail("sandbox_landlock_failed"))
    } else {
      Ok(())
    }
  };
  add(STAGE_FD, (1 << 0) | (1 << 2))?;
  for path in [
    "/lib",
    "/lib64",
    "/usr/lib",
    "/usr/lib64",
    "/etc/ld.so.cache",
    "/proc/self/fd",
  ] {
    if let Ok(cpath) = CString::new(path) {
      let fd = unsafe { libc::open(cpath.as_ptr(), libc::O_PATH | libc::O_CLOEXEC) };
      if fd >= 0 {
        if path != "/proc/self/fd" {
          let mut metadata: libc::stat = unsafe { std::mem::zeroed() };
          let trusted = unsafe { libc::fstat(fd, &mut metadata) } == 0
            && metadata.st_uid == 0
            && metadata.st_mode & 0o022 == 0
            && if path == "/etc/ld.so.cache" {
              metadata.st_mode & libc::S_IFMT == libc::S_IFREG
            } else {
              metadata.st_mode & libc::S_IFMT == libc::S_IFDIR
            };
          if !trusted {
            unsafe { libc::close(fd) };
            unsafe { libc::close(ruleset) };
            return Err(fail("sandbox_system_path_unsafe"));
          }
        }
        let result = add(
          fd,
          if path == "/etc/ld.so.cache" {
            1 << 2
          } else {
            READ
          },
        );
        unsafe {
          libc::close(fd);
        }
        result?;
      }
    }
  }
  let result = unsafe { libc::syscall(libc::SYS_landlock_restrict_self, ruleset, 0) };
  unsafe {
    libc::close(ruleset);
  }
  if result < 0 {
    return Err(fail("sandbox_landlock_failed"));
  }
  Ok(())
}

fn seccomp() -> Result<(), HostError> {
  const LOAD: u16 = 0x20;
  const JUMP_EQ: u16 = 0x15;
  const JUMP_SET: u16 = 0x45;
  const RETURN: u16 = 0x06;
  const DENY: u32 = 0x0005_0000 | libc::EPERM as u32;
  const ALLOW: u32 = 0x7fff_0000;
  #[cfg(target_arch = "x86_64")]
  const AUDIT_ARCH: u32 = 0xc000_003e;
  #[cfg(target_arch = "aarch64")]
  const AUDIT_ARCH: u32 = 0xc000_00b7;
  #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
  compile_error!("native skill worker seccomp requires a reviewed Linux architecture");
  let mut filter = vec![
    libc::sock_filter {
      code: LOAD,
      jt: 0,
      jf: 0,
      k: 4, // seccomp_data.arch
    },
    libc::sock_filter {
      code: JUMP_EQ,
      jt: 1,
      jf: 0,
      k: AUDIT_ARCH,
    },
    libc::sock_filter {
      code: RETURN,
      jt: 0,
      jf: 0,
      k: DENY,
    },
    libc::sock_filter {
      code: LOAD,
      jt: 0,
      jf: 0,
      k: 0, // seccomp_data.nr
    },
  ];
  #[cfg(target_arch = "x86_64")]
  filter.extend([
    libc::sock_filter {
      code: JUMP_SET,
      jt: 0,
      jf: 1,
      k: 0x4000_0000, // x32 syscall marker
    },
    libc::sock_filter {
      code: RETURN,
      jt: 0,
      jf: 0,
      k: DENY,
    },
  ]);
  #[cfg(target_arch = "aarch64")]
  let _ = JUMP_SET;
  for syscall in [
    libc::SYS_socket,
    libc::SYS_socketpair,
    libc::SYS_connect,
    libc::SYS_bind,
    libc::SYS_listen,
    libc::SYS_accept,
    libc::SYS_accept4,
    libc::SYS_clone,
    libc::SYS_clone3,
    libc::SYS_fork,
    libc::SYS_vfork,
    libc::SYS_execve,
    libc::SYS_execveat,
    libc::SYS_io_uring_setup,
    libc::SYS_io_uring_enter,
    libc::SYS_io_uring_register,
  ] {
    filter.push(libc::sock_filter {
      code: JUMP_EQ,
      jt: 0,
      jf: 1,
      k: syscall as u32,
    });
    filter.push(libc::sock_filter {
      code: RETURN,
      jt: 0,
      jf: 0,
      k: DENY,
    });
  }
  filter.push(libc::sock_filter {
    code: RETURN,
    jt: 0,
    jf: 0,
    k: ALLOW,
  });
  let program = libc::sock_fprog {
    len: filter.len() as u16,
    filter: filter.as_mut_ptr(),
  };
  if unsafe { libc::prctl(libc::PR_SET_SECCOMP, libc::SECCOMP_MODE_FILTER, &program) } != 0 {
    return Err(fail("sandbox_seccomp_failed"));
  }
  Ok(())
}

struct CallbackContext {
  response: Vec<u8>,
}
struct CompletionContext {
  event: Option<WorkerEvent>,
  duplicate: bool,
}

unsafe extern "C" fn is_cancelled(_: *mut c_void) -> u32 {
  u32::from(CANCELLED.load(Ordering::SeqCst))
}

unsafe extern "C" fn call_primitive(
  context: *mut c_void,
  request: *const CPrimitiveRequest,
  response: *mut CPrimitiveResponse,
) -> u32 {
  if context.is_null() || request.is_null() || response.is_null() {
    return 1;
  }
  let request = unsafe { &*request };
  if (request.struct_size as usize) < size_of::<CPrimitiveRequest>() {
    return 1;
  }
  let result = (|| -> Result<(u32, Vec<u8>), HostError> {
    let name = copy_text(request.primitive, 4096, true)?;
    let version = copy_text(request.version, 4096, true)?;
    let device_id = copy_text(request.device_id, 4096, false)?;
    let arguments_json = copy_text(request.arguments_json, 1024 * 1024, true)?;
    send_event(&WorkerEvent::PrimitiveCall {
      name,
      version,
      device_id,
      arguments_json,
    })?;
    let packet = recv_packet(SOCKET_FD, Duration::from_secs(5))?;
    let reply: Packet<WorkerRequest> =
      serde_json::from_slice(&packet).map_err(|_| fail("worker_protocol_invalid"))?;
    if reply.version != 1 {
      return Err(fail("worker_protocol_invalid"));
    }
    match reply.body {
      WorkerRequest::PrimitiveReply {
        status,
        acknowledgement_json,
      } if status <= 4 => Ok((status, acknowledgement_json.into_bytes())),
      _ => Err(fail("worker_protocol_invalid")),
    }
  })();
  let (status, bytes) = match result {
    Ok(value) => value,
    Err(_) => return 4,
  };
  let context = unsafe { &mut *(context as *mut CallbackContext) };
  context.response = bytes;
  unsafe {
    (*response).struct_size = size_of::<CPrimitiveResponse>() as u32;
    (*response).acknowledgement_json = CText {
      data: context.response.as_ptr().cast(),
      size: context.response.len() as u32,
    };
    (*response).error_message = CText {
      data: std::ptr::null(),
      size: 0,
    };
  }
  status
}

unsafe extern "C" fn complete(context: *mut c_void, completion: *const CCompletion) -> u32 {
  if context.is_null() || completion.is_null() {
    return 1;
  }
  let base = unsafe { &*completion.cast::<CCompletionBase>() };
  if (base.struct_size as usize) < size_of::<CCompletionBase>() {
    return 1;
  }
  let context = unsafe { &mut *(context as *mut CompletionContext) };
  if context.event.is_some() {
    context.duplicate = true;
    return 1;
  }
  if base.accepted > 1 {
    return 1;
  }
  let result = (|| -> Result<WorkerEvent, HostError> {
    let reason_code = copy_text(base.reason_code, 4096, false)?;
    let mut result_json = copy_text(base.result_json, 1024 * 1024, false)?;
    if result_json.is_empty() {
      result_json = "{}".into();
    }
    let message = if base.struct_size as usize >= size_of::<CCompletion>() {
      copy_text(unsafe { (*completion).message }, 4096, false)?
    } else {
      String::new()
    };
    if !result_json.is_empty()
      && serde_json::from_str::<serde_json::Value>(&result_json)
        .map_or(true, |value| !value.is_object())
    {
      return Err(fail("abi_result_invalid"));
    }
    Ok(WorkerEvent::Completion {
      accepted: base.accepted == 1,
      reason_code,
      result_json,
      message,
      status: 0,
    })
  })();
  match result {
    Ok(event) => {
      context.event = Some(event);
      0
    }
    Err(_) => 1,
  }
}

fn invoke(
  table: *const CLibrary,
  descriptors: &[SkillDescriptor],
  index: usize,
  command_id: &str,
  arguments_json: &str,
) -> i32 {
  if index >= descriptors.len()
    || command_id.is_empty()
    || command_id.len() > 96
    || serde_json::from_str::<serde_json::Value>(arguments_json)
      .map_or(true, |value| !value.is_object())
  {
    send_failure("worker_invocation_invalid");
    return 4;
  }
  let base = unsafe { &*table.cast::<CLibraryBase>() };
  let stride = if base.struct_size as usize >= size_of::<CLibrary>() {
    unsafe { (*table).skill_descriptor_size as usize }
  } else {
    size_of::<CDescriptorBase>()
  };
  let descriptor = unsafe {
    &*((base.skills as *const u8)
      .add(index * stride)
      .cast::<CDescriptorBase>())
  };
  let mut context = CallbackContext {
    response: Vec::new(),
  };
  let host = CHost {
    abi_version: 2,
    struct_size: size_of::<CHost>() as u32,
    context: (&mut context as *mut CallbackContext).cast(),
    call_primitive: Some(call_primitive),
    is_cancelled: Some(is_cancelled),
    read_resource: std::ptr::null(),
    receive_signal: std::ptr::null(),
    emit_signal: std::ptr::null(),
  };
  let execution = CExecution {
    struct_size: size_of::<CExecution>() as u32,
    command_id: CText {
      data: command_id.as_ptr().cast(),
      size: command_id.len() as u32,
    },
    arguments_json: CText {
      data: arguments_json.as_ptr().cast(),
      size: arguments_json.len() as u32,
    },
    host: &host,
  };
  let mut completion = CompletionContext {
    event: None,
    duplicate: false,
  };
  let status = unsafe {
    descriptor.execute.unwrap()(
      descriptor.context,
      &execution,
      complete,
      (&mut completion as *mut CompletionContext).cast(),
    )
  };
  if completion.duplicate {
    send_failure("worker_duplicate_completion");
    return 5;
  }
  if status != 0 {
    send_failure("worker_execute_failed");
    return 5;
  }
  let Some(event) = completion.event else {
    send_failure("worker_missing_completion");
    return 5;
  };
  if send_event(&event).is_err() {
    return 6;
  }
  0
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn inline_and_sealed_bulk_packets_roundtrip_with_exact_bounds() {
    let mut sockets = [0; 2];
    assert_eq!(
      unsafe {
        libc::socketpair(
          libc::AF_UNIX,
          libc::SOCK_SEQPACKET | libc::SOCK_CLOEXEC,
          0,
          sockets.as_mut_ptr(),
        )
      },
      0
    );
    let first = unsafe { File::from_raw_fd(sockets[0]) };
    let second = unsafe { File::from_raw_fd(sockets[1]) };
    for size in [MAX_PACKET, MAX_PACKET + 1, 1024 * 1024] {
      let payload = vec![b'x'; size];
      send_packet(first.as_raw_fd(), &payload).unwrap();
      assert_eq!(
        recv_packet(second.as_raw_fd(), Duration::from_secs(1))
          .unwrap_or_else(|error| panic!("{size}: {}", error.code)),
        payload
      );
    }
    assert_eq!(
      send_packet(first.as_raw_fd(), &vec![0; MAX_BULK + 1])
        .unwrap_err()
        .code,
      "worker_packet_oversize"
    );
  }

  #[test]
  fn stalled_peer_cannot_block_send_without_a_deadline() {
    let mut sockets = [0; 2];
    assert_eq!(
      unsafe {
        libc::socketpair(
          libc::AF_UNIX,
          libc::SOCK_SEQPACKET | libc::SOCK_CLOEXEC,
          0,
          sockets.as_mut_ptr(),
        )
      },
      0
    );
    let sender = unsafe { File::from_raw_fd(sockets[0]) };
    let _stalled = unsafe { File::from_raw_fd(sockets[1]) };
    let packet = [b'x'; 4096];
    loop {
      let written = unsafe {
        libc::send(
          sender.as_raw_fd(),
          packet.as_ptr().cast(),
          packet.len(),
          libc::MSG_DONTWAIT | libc::MSG_NOSIGNAL,
        )
      };
      if written < 0 {
        assert_eq!(io::Error::last_os_error().kind(), io::ErrorKind::WouldBlock);
        break;
      }
    }
    let start = Instant::now();
    assert_eq!(
      send_packet_with_timeout(sender.as_raw_fd(), &packet, Duration::from_millis(40))
        .unwrap_err()
        .code,
      "worker_timeout"
    );
    assert!(start.elapsed() < Duration::from_secs(1));
  }

  #[test]
  fn unconfirmed_inspection_reap_overrides_send_failure() {
    assert_eq!(
      post_spawn_error(fail("worker_protocol_io"), true).code,
      "worker_protocol_io"
    );
    assert_eq!(
      post_spawn_error(fail("worker_protocol_io"), false).code,
      "worker_reap_failed"
    );
  }
}
