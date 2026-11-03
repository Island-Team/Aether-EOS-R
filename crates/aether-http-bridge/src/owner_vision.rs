//! Owner-local, read-only Vision transport. Authorization precedes upgrade.

use std::fs::{self, OpenOptions};
use std::io::{Read, Write};
use std::os::fd::AsRawFd;
use std::os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Component, Path, PathBuf};

use aether_common::events::{valid_digest, valid_run_id};
use aether_control::http::read_unix_request;
use serde::Deserialize;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};

use crate::vision_ws;

#[derive(Clone, Deserialize)]
#[serde(deny_unknown_fields)]
struct Verifier {
  schema_version: u32,
  run_id: String,
  active_revision: String,
  projection_revision: String,
  source_path: String,
  gate_path: String,
  egia_vision_sha256: String,
}

#[derive(Clone)]
pub struct OwnerVision {
  path: PathBuf,
  verifier_path: PathBuf,
  verifier: Verifier,
}

pub struct OwnerSocketGuard(PathBuf);

impl Drop for OwnerSocketGuard {
  fn drop(&mut self) {
    let _ = fs::remove_file(&self.0);
  }
}

impl OwnerVision {
  pub fn load(
    path: PathBuf,
    verifier_path: PathBuf,
    run_id: &str,
    socket: &Path,
  ) -> Result<Self, String> {
    validate_socket_path(&path)?;
    if verifier_path.parent() != path.parent() {
      return Err("Vision verifier must share run directory".into());
    }
    let bytes = read_private_file(&verifier_path)?;
    let verifier: Verifier =
      serde_json::from_slice(&bytes).map_err(|_| "invalid Vision verifier")?;
    if verifier.schema_version != 1
      || verifier.run_id != run_id
      || !valid_run_id(run_id)
      || !valid_digest(&verifier.active_revision)
      || !valid_digest(&verifier.projection_revision)
      || !valid_digest(&verifier.egia_vision_sha256)
      || verifier.source_path.is_empty()
      || verifier.gate_path.is_empty()
    {
      return Err("invalid Vision verifier".into());
    }
    let this = Self {
      path,
      verifier_path,
      verifier,
    };
    this
      .check_attest(socket)
      .map_err(|_| "Vision owner binding unavailable")?;
    Ok(this)
  }

  pub fn bind(&self) -> Result<UnixListener, String> {
    validate_socket_path(&self.path)?;
    if self.path.exists() {
      return Err("owner Vision socket already exists".into());
    }
    let listener = UnixListener::bind(&self.path).map_err(|e| e.to_string())?;
    if let Err(error) = fs::set_permissions(&self.path, fs::Permissions::from_mode(0o600))
      .and_then(|()| listener.set_nonblocking(true))
    {
      let _ = fs::remove_file(&self.path);
      return Err(error.to_string());
    }
    Ok(listener)
  }

  pub fn unlink(&self) {
    let _ = fs::remove_file(&self.path);
  }

  pub fn guard(&self) -> OwnerSocketGuard {
    OwnerSocketGuard(self.path.clone())
  }

  fn check_attest(&self, socket: &Path) -> Result<Value, String> {
    let response = aether_control::request(
      socket,
      &json!({"op":"vision_attest","gate_path":self.verifier.gate_path}),
    )?;
    if response["ok"] != true || response["ready"] != true {
      return Err("vision_unavailable".into());
    }
    for (key, expected) in [
      ("run_id", &self.verifier.run_id),
      ("active_revision", &self.verifier.active_revision),
      ("projection_revision", &self.verifier.projection_revision),
      ("source_path", &self.verifier.source_path),
      ("gate_path", &self.verifier.gate_path),
    ] {
      if response[key].as_str() != Some(expected) {
        return Err("owner_binding_stale".into());
      }
    }
    Ok(response)
  }

  pub fn serve(&self, mut stream: UnixStream, socket: &Path) -> Result<(), String> {
    if !same_uid(&stream) {
      return deny(&mut stream, 403, "owner_grant_denied");
    }
    let request = match read_unix_request(&mut stream) {
      Ok(request) => request,
      Err(_) => return deny(&mut stream, 403, "owner_grant_denied"),
    };
    if request.method != "GET"
      || request.path != "/api/eos/v2/vision/owner-ws"
      || request.headers.get("host").map(String::as_str) != Some("localhost")
      || request.headers.contains_key("origin")
      || request
        .headers
        .keys()
        .any(|key| key.starts_with("sec-fetch-"))
      || !request.body.is_empty()
    {
      return deny(&mut stream, 403, "owner_grant_denied");
    }
    let bearer = request
      .headers
      .get("authorization")
      .and_then(|value| value.strip_prefix("Bearer "));
    let Some(bearer) = bearer.filter(|value| valid_digest(value)) else {
      return deny(&mut stream, 403, "owner_grant_denied");
    };
    let bytes = decode_hex_32(bearer).ok_or("invalid grant")?;
    let digest = format!("{:x}", Sha256::digest(bytes));
    if !constant_time_equal(
      digest.as_bytes(),
      self.verifier.egia_vision_sha256.as_bytes(),
    ) {
      return deny(&mut stream, 403, "owner_grant_denied");
    }
    if !self.binding_current() {
      return deny(&mut stream, 409, "owner_binding_stale");
    }
    let attest = match self.check_attest(socket) {
      Ok(attest) => attest,
      Err(error) if error == "owner_binding_stale" => {
        return deny(&mut stream, 409, "owner_binding_stale")
      }
      Err(_) => return deny(&mut stream, 503, "vision_unavailable"),
    };
    vision_ws::serve_owner(
      &mut stream,
      &request,
      socket,
      &attest,
      &self.verifier.gate_path,
      &|| self.binding_current(),
    )
  }

  fn binding_current(&self) -> bool {
    let current = read_private_file(&self.verifier_path)
      .ok()
      .and_then(|bytes| serde_json::from_slice::<Verifier>(&bytes).ok());
    current.as_ref().is_some_and(|value| {
      value.schema_version == 1
        && value.run_id == self.verifier.run_id
        && value.active_revision == self.verifier.active_revision
        && value.projection_revision == self.verifier.projection_revision
        && value.egia_vision_sha256 == self.verifier.egia_vision_sha256
        && value.source_path == self.verifier.source_path
        && value.gate_path == self.verifier.gate_path
    })
  }
}

fn deny(stream: &mut UnixStream, status: u16, error: &str) -> Result<(), String> {
  let body = json!({"ok":false,"error":error}).to_string();
  let reason = match status {
    403 => "Forbidden",
    409 => "Conflict",
    _ => "Service Unavailable",
  };
  write!(stream, "HTTP/1.1 {status} {reason}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n{body}", body.len()).map_err(|e| e.to_string())
}

fn same_uid(stream: &UnixStream) -> bool {
  let mut cred = libc::ucred {
    pid: 0,
    uid: 0,
    gid: 0,
  };
  let mut len = std::mem::size_of::<libc::ucred>() as libc::socklen_t;
  let ok = unsafe {
    libc::getsockopt(
      stream.as_raw_fd(),
      libc::SOL_SOCKET,
      libc::SO_PEERCRED,
      (&mut cred as *mut libc::ucred).cast(),
      &mut len,
    )
  };
  ok == 0
    && len as usize == std::mem::size_of::<libc::ucred>()
    && cred.uid == unsafe { libc::geteuid() }
}

fn constant_time_equal(left: &[u8], right: &[u8]) -> bool {
  let mut different = left.len() ^ right.len();
  for (a, b) in left.iter().zip(right) {
    different |= usize::from(a ^ b);
  }
  different == 0
}

fn decode_hex_32(value: &str) -> Option<[u8; 32]> {
  if !valid_digest(value) {
    return None;
  }
  let mut bytes = [0u8; 32];
  for index in 0..32 {
    bytes[index] = u8::from_str_radix(&value[index * 2..index * 2 + 2], 16).ok()?;
  }
  Some(bytes)
}

fn validate_socket_path(path: &Path) -> Result<(), String> {
  if !path.is_absolute()
    || path.file_name().and_then(|v| v.to_str()) != Some("eos-vision-owner.sock")
    || path
      .components()
      .any(|part| matches!(part, Component::CurDir | Component::ParentDir))
  {
    return Err("unsafe owner Vision socket path".into());
  }
  for ancestor in path.ancestors().skip(1) {
    if fs::symlink_metadata(ancestor)
      .map_err(|_| "owner Vision path unavailable")?
      .file_type()
      .is_symlink()
    {
      return Err("unsafe owner Vision socket path".into());
    }
  }
  let parent = path.parent().ok_or("owner Vision parent missing")?;
  let metadata = fs::symlink_metadata(parent).map_err(|_| "owner Vision parent missing")?;
  if !metadata.file_type().is_dir()
    || metadata.permissions().mode() & 0o777 != 0o700
    || metadata.uid() != unsafe { libc::geteuid() }
  {
    return Err("unsafe owner Vision socket parent".into());
  }
  Ok(())
}

fn read_private_file(path: &Path) -> Result<Vec<u8>, String> {
  if !path.is_absolute()
    || path
      .components()
      .any(|part| matches!(part, Component::CurDir | Component::ParentDir))
  {
    return Err("unsafe Vision verifier path".into());
  }
  for ancestor in path.ancestors() {
    if fs::symlink_metadata(ancestor)
      .map_err(|_| "Vision verifier unavailable")?
      .file_type()
      .is_symlink()
    {
      return Err("unsafe Vision verifier path".into());
    }
  }
  let parent = path.parent().ok_or("Vision verifier parent missing")?;
  let meta = fs::symlink_metadata(parent).map_err(|_| "Vision verifier parent missing")?;
  if !meta.file_type().is_dir()
    || meta.permissions().mode() & 0o777 != 0o700
    || meta.uid() != unsafe { libc::geteuid() }
  {
    return Err("unsafe Vision verifier parent".into());
  }
  let file = OpenOptions::new()
    .read(true)
    .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
    .open(path)
    .map_err(|_| "Vision verifier unavailable")?;
  let meta = file.metadata().map_err(|_| "Vision verifier unavailable")?;
  if !meta.file_type().is_file()
    || meta.permissions().mode() & 0o777 != 0o600
    || meta.uid() != unsafe { libc::geteuid() }
    || meta.len() > 4096
  {
    return Err("unsafe Vision verifier".into());
  }
  let mut bytes = Vec::new();
  file
    .take(4097)
    .read_to_end(&mut bytes)
    .map_err(|_| "Vision verifier unavailable")?;
  if bytes.len() > 4096 {
    return Err("Vision verifier exceeds bound".into());
  }
  Ok(bytes)
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::io::BufRead;
  use std::sync::atomic::{AtomicBool, Ordering};
  use std::sync::Arc;
  use std::time::Duration;

  fn fixture() -> (PathBuf, OwnerVision, String) {
    let root = std::env::temp_dir().join(format!(
      "vision-owner-test-{}-{}",
      std::process::id(),
      std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos()
    ));
    fs::create_dir(&root).unwrap();
    fs::set_permissions(&root, fs::Permissions::from_mode(0o700)).unwrap();
    let grant = "11".repeat(32);
    let verifier = Verifier {
      schema_version: 1,
      run_id: "run-one".into(),
      active_revision: "a".repeat(64),
      projection_revision: "b".repeat(64),
      source_path: "eos/hardware/camera/front".into(),
      gate_path: "eos/interface/gate/front".into(),
      egia_vision_sha256: format!("{:x}", Sha256::digest(decode_hex_32(&grant).unwrap())),
    };
    let verifier_path = root.join("vision-verifier.json");
    fs::write(
      &verifier_path,
      json!({"schema_version":1,"run_id":verifier.run_id,
      "active_revision":verifier.active_revision,"projection_revision":verifier.projection_revision,
      "source_path":verifier.source_path,"gate_path":verifier.gate_path,
      "egia_vision_sha256":verifier.egia_vision_sha256})
      .to_string(),
    )
    .unwrap();
    fs::set_permissions(&verifier_path, fs::Permissions::from_mode(0o600)).unwrap();
    (
      root.clone(),
      OwnerVision {
        path: root.join("eos-vision-owner.sock"),
        verifier_path,
        verifier,
      },
      grant,
    )
  }

  fn request(owner: OwnerVision, socket: PathBuf, grant: &str, extra: &str) -> String {
    let (mut client, server) = UnixStream::pair().unwrap();
    let worker = std::thread::spawn(move || owner.serve(server, &socket));
    write!(client, "GET /api/eos/v2/vision/owner-ws HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {grant}\r\n{extra}\r\n").unwrap();
    client
      .set_read_timeout(Some(Duration::from_secs(2)))
      .unwrap();
    let mut response = String::new();
    client.read_to_string(&mut response).unwrap();
    worker.join().unwrap().unwrap();
    response
  }
  #[test]
  fn grant_digest_is_over_decoded_secret_bytes() {
    let grant = "11".repeat(32);
    let digest = format!("{:x}", Sha256::digest(decode_hex_32(&grant).unwrap()));
    assert_eq!(
      digest,
      "02d449a31fbb267c8f352e9968a79e3e5fc95c1bbeaa502fd6454ebde5a4bedc"
    );
    assert!(!constant_time_equal(
      digest.as_bytes(),
      "00".repeat(32).as_bytes()
    ));
  }

  #[test]
  fn owner_grant_and_browser_headers_are_rejected_before_upgrade() {
    let (root, owner, grant) = fixture();
    let wrong = request(
      owner.clone(),
      root.join("missing-eos.sock"),
      &"22".repeat(32),
      "",
    );
    assert!(wrong.starts_with("HTTP/1.1 403"));
    assert!(wrong.contains("owner_grant_denied"));
    let browser = request(
      owner,
      root.join("missing-eos.sock"),
      &grant,
      "Origin: https://example.test\r\n",
    );
    assert!(browser.starts_with("HTTP/1.1 403"));
    fs::remove_dir_all(root).unwrap();
  }

  #[test]
  fn selected_verifier_and_live_attest_fence_the_owner_listener() {
    let (root, owner, grant) = fixture();
    let eos = root.join("eos.sock");
    let listener = UnixListener::bind(&eos).unwrap();
    listener.set_nonblocking(true).unwrap();
    let live = Arc::new(AtomicBool::new(true));
    let stop = live.clone();
    let expected = owner.verifier.clone();
    let worker = std::thread::spawn(move || {
      while stop.load(Ordering::Acquire) {
        match listener.accept() {
          Ok((mut stream, _)) => {
            let mut line = String::new();
            std::io::BufReader::new(stream.try_clone().unwrap())
              .read_line(&mut line)
              .unwrap();
            let request: Value = serde_json::from_str(&line).unwrap();
            assert_eq!(request["op"], "vision_attest");
            stream.write_all(format!("{}\n", json!({"ok":true,"ready":true,
              "run_id":expected.run_id,"active_revision":expected.active_revision,
              "projection_revision":expected.projection_revision,"runtime_stream_id":"11111111-1111-4111-8111-111111111111",
              "source_path":expected.source_path,"gate_path":expected.gate_path,"gate_generation":0})).as_bytes()).unwrap();
          }
          Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
            std::thread::sleep(Duration::from_millis(2))
          }
          Err(error) => panic!("EOS stub: {error}"),
        }
      }
    });
    let loaded = OwnerVision::load(
      owner.path.clone(),
      owner.verifier_path.clone(),
      "run-one",
      &eos,
    )
    .unwrap();
    assert_eq!(loaded.verifier.run_id, "run-one");
    let listener = loaded.bind().unwrap();
    assert_eq!(
      fs::metadata(&loaded.path).unwrap().permissions().mode() & 0o777,
      0o600
    );
    drop(listener);
    loaded.unlink();
    fs::write(&loaded.verifier_path, b"{}").unwrap();
    assert!(!loaded.binding_current());
    let stale = request(loaded, eos, &grant, "");
    assert!(stale.starts_with("HTTP/1.1 409"));
    assert!(stale.contains("owner_binding_stale"));
    live.store(false, Ordering::Release);
    worker.join().unwrap();
    fs::remove_dir_all(root).unwrap();
  }
}
