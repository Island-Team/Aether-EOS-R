//! Verified package adapter libraries. The C ABI contains no Rust-owned types.
use std::collections::BTreeMap;
use std::ffi::{c_char, c_void, CStr, CString};
use std::fs::File;
use std::io::{Seek, SeekFrom};
use std::os::fd::AsRawFd;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use aether_hal::{
  BaseMovement, BaseNode, BaseObservation, CameraFrame, CameraNode, CommandMeta, CommandOutcome,
  HalError, Health, HealthState, ManipulatorMovement, ManipulatorNode, ManipulatorObservation,
  NodeAdapter, NodeKind, NodeProfile, NodeState, NodeStatus,
};
use serde::de::DeserializeOwned;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};

const ABI_VERSION: u32 = 1;
// JSON byte arrays can be several times larger than the validated raw frame.
const MAX_REPLY: usize = 64 * 1024 * 1024;
const CAMERA: u32 = 1;
const BASE: u32 = 2;
const MANIPULATOR: u32 = 4;

/// One IsOS-verified library for one package-contributed adapter family.
#[derive(Clone, Debug)]
pub struct LibrarySpec {
  pub family: String,
  pub path: PathBuf,
  pub sha256: String,
}

impl LibrarySpec {
  pub fn parse(value: &str) -> Result<Self, String> {
    let (family, rest) = value
      .split_once('=')
      .ok_or("expected family=/absolute/library@sha256")?;
    let (path, sha256) = rest.rsplit_once('@').ok_or("expected library@sha256")?;
    if family.is_empty()
      || !family
        .bytes()
        .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
    {
      return Err("invalid adapter family ID".into());
    }
    if !Path::new(path).is_absolute()
      || sha256.len() != 64
      || !sha256.bytes().all(|b| b.is_ascii_hexdigit())
    {
      return Err("adapter library needs an absolute path and SHA-256 hex digest".into());
    }
    Ok(Self {
      family: family.into(),
      path: path.into(),
      sha256: sha256.to_ascii_lowercase(),
    })
  }
}

/// C ABI v1: call writes a UTF-8 JSON result on success or a UTF-8 error on failure.
/// `stop` must remain callable while `call` is blocked. The library owns its
/// instance and must make concurrent stop/destroy safe after calls complete.
#[repr(C)]
struct Descriptor {
  abi_version: u32,
  family: *const c_char,
  kind_mask: u32,
  create: unsafe extern "C" fn(*const u8, usize) -> *mut c_void,
  call: unsafe extern "C" fn(*mut c_void, u32, *const u8, usize, *mut u8, usize, *mut usize) -> i32,
  stop: unsafe extern "C" fn(*mut c_void) -> i32,
  destroy: unsafe extern "C" fn(*mut c_void),
}

type DescriptorFn = unsafe extern "C" fn() -> *const Descriptor;

struct Library {
  handle: *mut c_void,
  _file: File,
  descriptor: Descriptor,
}
unsafe impl Send for Library {}
unsafe impl Sync for Library {}
impl Drop for Library {
  fn drop(&mut self) {
    unsafe {
      libc::dlclose(self.handle);
    }
  }
}

impl Library {
  fn open(spec: &LibrarySpec) -> Result<Arc<Self>, HalError> {
    let mut file = File::open(&spec.path)
      .map_err(|e| HalError::Adapter(format!("{}: {e}", spec.path.display())))?;
    let mut digest = Sha256::new();
    std::io::copy(&mut file, &mut digest).map_err(|e| HalError::Adapter(e.to_string()))?;
    if format!("{:x}", digest.finalize()) != spec.sha256 {
      return Err(HalError::Adapter(format!(
        "{}: SHA-256 changed",
        spec.path.display()
      )));
    }
    file
      .seek(SeekFrom::Start(0))
      .map_err(|e| HalError::Adapter(e.to_string()))?;
    // Load the same opened inode that was hashed; a changed pathname cannot
    // substitute a different library between verification and dlopen.
    let fd_path = CString::new(format!("/proc/self/fd/{}", file.as_raw_fd())).unwrap();
    let handle = unsafe { libc::dlopen(fd_path.as_ptr(), libc::RTLD_NOW | libc::RTLD_LOCAL) };
    if handle.is_null() {
      return Err(HalError::Adapter(dl_error()));
    }
    let symbol = unsafe { libc::dlsym(handle, c"aether_adapter_v1".as_ptr()) };
    if symbol.is_null() {
      unsafe {
        libc::dlclose(handle);
      }
      return Err(HalError::Adapter("missing aether_adapter_v1 symbol".into()));
    }
    let getter: DescriptorFn = unsafe { std::mem::transmute(symbol) };
    let ptr = unsafe { getter() };
    if ptr.is_null() {
      unsafe {
        libc::dlclose(handle);
      }
      return Err(HalError::Adapter("null adapter descriptor".into()));
    }
    let desc = unsafe { &*ptr };
    if desc.abi_version != ABI_VERSION || desc.family.is_null() {
      unsafe {
        libc::dlclose(handle);
      }
      return Err(HalError::Adapter(
        "unsupported adapter ABI or missing family".into(),
      ));
    }
    let family = unsafe { CStr::from_ptr(desc.family) }
      .to_str()
      .map_err(|e| HalError::Adapter(e.to_string()))?;
    if family != spec.family {
      unsafe {
        libc::dlclose(handle);
      }
      return Err(HalError::Adapter(format!(
        "adapter family mismatch: expected {}",
        spec.family
      )));
    }
    let descriptor = Descriptor {
      abi_version: desc.abi_version,
      family: desc.family,
      kind_mask: desc.kind_mask,
      create: desc.create,
      call: desc.call,
      stop: desc.stop,
      destroy: desc.destroy,
    };
    Ok(Arc::new(Self {
      handle,
      _file: file,
      descriptor,
    }))
  }
}
fn dl_error() -> String {
  let p = unsafe { libc::dlerror() };
  if p.is_null() {
    "dlopen failed".into()
  } else {
    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
  }
}

pub struct Libraries(BTreeMap<String, Arc<Library>>);
impl Libraries {
  pub fn open(specs: &[LibrarySpec]) -> Result<Self, HalError> {
    let mut libraries = BTreeMap::new();
    for spec in specs {
      if libraries.contains_key(&spec.family) {
        return Err(HalError::Profile(format!(
          "duplicate adapter library {}",
          spec.family
        )));
      }
      libraries.insert(spec.family.clone(), Library::open(spec)?);
    }
    Ok(Self(libraries))
  }
  pub fn create(&self, node: &NodeProfile) -> Result<Arc<LoadedAdapter>, HalError> {
    let library = self
      .0
      .get(&node.adapter.adapter_type)
      .ok_or_else(|| HalError::Profile(format!("{}: no verified adapter library", node.path())))?
      .clone();
    let kind_bit = match node.kind {
      NodeKind::Camera => CAMERA,
      NodeKind::Base => BASE,
      NodeKind::Manipulator => MANIPULATOR,
      NodeKind::Software => return Err(HalError::Profile("software has no adapter".into())),
    };
    if library.descriptor.kind_mask & kind_bit == 0 {
      return Err(HalError::Profile(format!(
        "{}: adapter library does not support node kind",
        node.path()
      )));
    }
    let input = serde_json::to_vec(&json!({"path":node.path(),"kind":node.kind,"node":node}))
      .map_err(|e| HalError::Profile(e.to_string()))?;
    let instance = unsafe { (library.descriptor.create)(input.as_ptr(), input.len()) };
    if instance.is_null() {
      return Err(HalError::Adapter(format!(
        "{}: adapter create failed",
        node.path()
      )));
    }
    Ok(Arc::new(LoadedAdapter {
      library,
      instance,
      family: node.adapter.adapter_type.clone(),
      kind: node.kind,
      max_payload: node.observation.as_ref().map_or(0, |o| o.max_payload_bytes),
    }))
  }
}

pub struct LoadedAdapter {
  library: Arc<Library>,
  instance: *mut c_void,
  family: String,
  kind: NodeKind,
  max_payload: u64,
}
unsafe impl Send for LoadedAdapter {}
unsafe impl Sync for LoadedAdapter {}
impl Drop for LoadedAdapter {
  fn drop(&mut self) {
    unsafe { (self.library.descriptor.destroy)(self.instance) }
  }
}
impl LoadedAdapter {
  fn call<T: DeserializeOwned>(&self, operation: u32, request: Value) -> Result<T, HalError> {
    serde_json::from_value(self.call_value(operation, request)?)
      .map_err(|e| HalError::InvalidData(format!("adapter response: {e}")))
  }

  pub(crate) fn call_value(&self, operation: u32, request: Value) -> Result<Value, HalError> {
    let input = serde_json::to_vec(&request).map_err(|e| HalError::Adapter(e.to_string()))?;
    let capacity = if operation == 3 {
      MAX_REPLY
    } else {
      1024 * 1024
    };
    let mut output = vec![0u8; capacity];
    let mut len = 0usize;
    let result = unsafe {
      (self.library.descriptor.call)(
        self.instance,
        operation,
        input.as_ptr(),
        input.len(),
        output.as_mut_ptr(),
        output.len(),
        &mut len,
      )
    };
    if len > output.len() {
      return Err(HalError::Adapter(
        "adapter response exceeds operation limit".into(),
      ));
    }
    output.truncate(len);
    if result != 0 {
      return Err(HalError::Adapter(
        String::from_utf8_lossy(&output).into_owned(),
      ));
    }
    serde_json::from_slice(&output)
      .map_err(|e| HalError::InvalidData(format!("adapter response: {e}")))
  }
}
impl NodeAdapter for LoadedAdapter {
  fn kind(&self) -> NodeKind {
    self.kind
  }
  fn adapter_type(&self) -> &str {
    &self.family
  }
  fn status(&self) -> NodeStatus {
    let value: Result<Value, _> = self.call(1, json!({}));
    match value {
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
    let value: Result<Value, _> = self.call(2, json!({}));
    match value {
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
        code: Some("adapter_response".into()),
        message: Some(e.to_string()),
        last_ok_monotonic_ns: None,
      },
    }
  }
  fn stop(&self) -> Result<(), HalError> {
    if unsafe { (self.library.descriptor.stop)(self.instance) } == 0 {
      Ok(())
    } else {
      Err(HalError::Adapter("adapter stop failed".into()))
    }
  }
}
impl CameraNode for LoadedAdapter {
  fn capture(&self) -> Result<CameraFrame, HalError> {
    let frame: CameraFrame = self.call(3, json!({}))?;
    frame.validate(self.max_payload)?;
    Ok(frame)
  }
}
impl BaseNode for LoadedAdapter {
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
impl ManipulatorNode for LoadedAdapter {
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

#[cfg(test)]
mod tests {
  use super::*;
  use std::process::Command;
  use std::time::{Duration, SystemTime, UNIX_EPOCH};

  fn fixture(abi: u32) -> (PathBuf, LibrarySpec) {
    let unique = SystemTime::now()
      .duration_since(UNIX_EPOCH)
      .unwrap()
      .as_nanos();
    let directory =
      std::env::temp_dir().join(format!("aether-adapter-{unique}-{}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let source = directory.join("stub.c");
    let library = directory.join("libstub.so");
    let body = format!(
      r#"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "aether/adapter_v1.h"
typedef struct {{ atomic_int stopped; }} Instance;
static void* create(const uint8_t* data, size_t len) {{ (void)data; (void)len; return calloc(1, sizeof(Instance)); }}
static int32_t call(void* raw, uint32_t op, const uint8_t* input, size_t input_len,
                    uint8_t* output, size_t cap, size_t* len) {{
  (void)input; (void)input_len;
  const char* result = "null";
  if (op == 1) result = "{{\"state\":\"ready\",\"active_command_id\":null}}";
  if (op == 2) result = "{{\"state\":\"healthy\",\"code\":null,\"message\":null,\"last_ok_monotonic_ns\":null}}";
  if (op == 5) {{ usleep(100000); result = atomic_load(&((Instance*)raw)->stopped) ? "\"ambiguous\"" : "\"completed\""; }}
  *len = strlen(result);
  if (*len > cap) return -1;
  memcpy(output, result, *len);
  return 0;
}}
static int32_t stop(void* raw) {{ atomic_store(&((Instance*)raw)->stopped, 1); return 0; }}
static void destroy(void* raw) {{ free(raw); }}
static const AetherAdapterV1 desc = {{{abi}, "package-stub", AETHER_ADAPTER_BASE,
    create, call, stop, destroy}};
const AetherAdapterV1* aether_adapter_v1(void) {{ return &desc; }}
"#
    );
    std::fs::write(&source, body).unwrap();
    let include = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../include");
    assert!(Command::new("cc")
      .args(["-std=c11", "-D_DEFAULT_SOURCE", "-shared", "-fPIC", "-I"])
      .arg(include)
      .arg(&source)
      .arg("-o")
      .arg(&library)
      .status()
      .unwrap()
      .success());
    let bytes = std::fs::read(&library).unwrap();
    let spec = LibrarySpec {
      family: "package-stub".into(),
      path: library,
      sha256: format!("{:x}", Sha256::digest(&bytes)),
    };
    (directory, spec)
  }

  #[test]
  fn rejects_changed_library_and_wrong_abi() {
    let (directory, mut spec) = fixture(ABI_VERSION + 1);
    assert!(Library::open(&spec)
      .err()
      .unwrap()
      .to_string()
      .contains("ABI"));
    spec.sha256 = "0".repeat(64);
    assert!(Library::open(&spec)
      .err()
      .unwrap()
      .to_string()
      .contains("SHA-256"));
    std::fs::remove_dir_all(directory).unwrap();
  }

  #[test]
  fn loads_typed_adapter_and_stops_while_call_is_active() {
    let (directory, spec) = fixture(ABI_VERSION);
    let libraries = Libraries::open(&[spec]).unwrap();
    let source = include_str!(concat!(
      env!("CARGO_MANIFEST_DIR"),
      "/../../etc/eos/backends/mock-cxx.toml"
    ));
    let profile = aether_hal::Profile::from_toml(source).unwrap();
    let mut node = profile
      .hardware_nodes()
      .find(|node| node.kind == NodeKind::Base)
      .unwrap()
      .clone();
    node.adapter.adapter_type = "package-stub".into();
    let adapter = libraries.create(&node).unwrap();
    assert_eq!(adapter.status().state, NodeState::Ready);
    assert_eq!(adapter.health().state, HealthState::Healthy);
    let other = adapter.clone();
    let work = std::thread::spawn(move || other.call::<Value>(5, json!({})).unwrap());
    std::thread::sleep(Duration::from_millis(10));
    adapter.stop().unwrap();
    assert_eq!(work.join().unwrap(), json!("ambiguous"));
    drop(adapter);
    drop(libraries);
    std::fs::remove_dir_all(directory).unwrap();
  }

  #[test]
  fn package_family_is_admitted_through_runtime_registry() {
    use aether_hal::{NodeTypeContribution, NodeTypeRegistry, PackageRef, PortTransport};
    let (directory, spec) = fixture(ABI_VERSION);
    let mut registry = NodeTypeRegistry::builtin();
    let package = PackageRef {
      id: "test/stub".into(),
      version: "2.0.0".into(),
    };
    registry
      .register_contribution(NodeTypeContribution {
        package: package.clone(),
        node_types: vec![],
        safety_adapters: vec![],
        data_types: vec![],
        adapter_families: vec![aether_hal::AdapterFamilyDescriptor {
          id: "package-stub".into(),
          device_kinds: vec![NodeKind::Base],
          transport: PortTransport::InProcess,
          description: "Test base library".into(),
          settings_schema: None,
          supported_primitives: vec!["base.velocity@1.0.0".into()],
        }],
      })
      .unwrap();
    let source = include_str!(concat!(
      env!("CARGO_MANIFEST_DIR"),
      "/../../etc/eos/backends/mock-cxx.toml"
    ));
    let original = aether_hal::Profile::from_toml(source).unwrap();
    let mut graph = serde_json::to_value(original).unwrap();
    let adapter = &mut graph["cores"]["eos"]["layers"]["hardware"]["groups"]["base"]["nodes"]
      ["drive"]["adapter"];
    adapter["type"] = json!("package-stub");
    adapter["package"] = serde_json::to_value(package).unwrap();
    let profile =
      aether_hal::Profile::from_json_with_registry(&graph.to_string(), &registry).unwrap();
    let runtime = crate::Runtime::from_profile_adapters_durable_with_libraries(
      profile,
      Arc::new(aether_common::time::SystemClock),
      directory.join("journal"),
      &[spec],
    )
    .unwrap();
    assert!(runtime
      .discover()
      .iter()
      .any(|node| node.node_id == "drive"));
    drop(runtime);
    std::fs::remove_dir_all(directory).unwrap();
  }
}
