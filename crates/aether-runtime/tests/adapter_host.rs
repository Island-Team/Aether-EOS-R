use std::fs;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Arc;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use aether_hal::{NodeAdapter, NodeKind, Profile};
use aether_runtime::adapter_host::HostedAdapter;
use aether_runtime::adapter_library::LibrarySpec;
use sha2::{Digest, Sha256};

fn fixture() -> (PathBuf, LibrarySpec, aether_hal::NodeProfile) {
  let nonce = SystemTime::now()
    .duration_since(UNIX_EPOCH)
    .unwrap()
    .as_nanos();
  let directory =
    std::env::temp_dir().join(format!("aether-host-test-{}-{nonce}", std::process::id()));
  fs::create_dir(&directory).unwrap();
  let source = directory.join("stub.c");
  let library = directory.join("libstub.so");
  fs::write(&source, r#"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "aether/adapter_v1.h"
typedef struct { atomic_int stopped; } Instance;
static void* create(const uint8_t* data, size_t len) { (void)data; (void)len; return calloc(1, sizeof(Instance)); }
static int32_t call(void* raw, uint32_t op, const uint8_t* input, size_t input_len,
                    uint8_t* output, size_t cap, size_t* len) {
  (void)input; (void)input_len;
  const char* result = "null";
  if (op == 1) result = "{\"state\":\"ready\",\"active_command_id\":null}";
  if (op == 2) result = "{\"state\":\"healthy\",\"code\":null,\"message\":null,\"last_ok_monotonic_ns\":null}";
  if (op == 5) { usleep(200000); result = atomic_load(&((Instance*)raw)->stopped) ? "\"ambiguous\"" : "\"completed\""; }
  *len = strlen(result);
  if (*len > cap) return -1;
  memcpy(output, result, *len);
  return 0;
}
static int32_t stop(void* raw) {
  atomic_store(&((Instance*)raw)->stopped, 1);
  const char* marker = getenv("AETHER_TEST_STOP_FILE");
  if (marker) { FILE* f = fopen(marker, "w"); if (f) { fputs("stopped", f); fclose(f); } }
  return 0;
}
static void destroy(void* raw) { free(raw); }
static const AetherAdapterV1 desc = {1, "package-stub", AETHER_ADAPTER_BASE, create, call, stop, destroy};
const AetherAdapterV1* aether_adapter_v1(void) { return &desc; }
"#).unwrap();
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
  let spec = LibrarySpec {
    family: "package-stub".into(),
    path: library.clone(),
    sha256: format!("{:x}", Sha256::digest(fs::read(&library).unwrap())),
  };
  let profile = Profile::from_toml(include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  )))
  .unwrap();
  let mut node = profile
    .hardware_nodes()
    .find(|n| n.kind == NodeKind::Base)
    .unwrap()
    .clone();
  node.adapter.adapter_type = spec.family.clone();
  (directory, spec, node)
}

#[test]
fn package_adapter_runs_in_child_and_stops_during_blocked_call() {
  let (dir, spec, node) = fixture();
  let exe = Path::new(env!("CARGO_BIN_EXE_aether-runtime"));
  let adapter = HostedAdapter::start(&node, &spec, exe).unwrap();
  assert_eq!(adapter.status().state, aether_hal::NodeState::Ready);
  assert_eq!(adapter.health().state, aether_hal::HealthState::Healthy);
  let other = Arc::clone(&adapter);
  let work = std::thread::spawn(move || {
    use aether_hal::BaseNode;
    // The raw C call is exercised through a typed movement with a valid payload.
    other.move_base(
      aether_hal::CommandMeta {
        command_id: "host-stop".into(),
        deadline_monotonic_ns: 1_000_000_000,
      },
      aether_hal::BaseMovement::BodyVelocity {
        x_m_s: 0.1,
        y_m_s: 0.0,
        yaw_rad_s: 0.0,
        valid_for_ms: 100,
      },
    )
  });
  std::thread::sleep(Duration::from_millis(30));
  adapter.stop().unwrap();
  assert_eq!(
    work.join().unwrap().unwrap(),
    aether_hal::CommandOutcome::Ambiguous
  );
  drop(adapter);
  fs::remove_dir_all(dir).unwrap();
}

#[test]
fn wrong_digest_prevents_host_admission() {
  let (dir, mut spec, node) = fixture();
  spec.sha256 = "0".repeat(64);
  assert!(HostedAdapter::start(
    &node,
    &spec,
    Path::new(env!("CARGO_BIN_EXE_aether-runtime"))
  )
  .is_err());
  fs::remove_dir_all(dir).unwrap();
}

#[test]
fn missing_parent_heartbeat_stops_adapter_and_exits() {
  let (dir, spec, node) = fixture();
  let node_file = dir.join("node.json");
  let socket = dir.join("host.sock");
  let marker = dir.join("stopped.txt");
  fs::write(
    &node_file,
    serde_json::to_vec(&serde_json::json!({
        "node":node,"kind":node.kind,"path":node.path()
    }))
    .unwrap(),
  )
  .unwrap();
  let arg = format!("{}={}@{}", spec.family, spec.path.display(), spec.sha256);
  let mut child = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
    .args([
      "--adapter-host",
      "--adapter-library",
      &arg,
      "--adapter-node",
    ])
    .arg(&node_file)
    .arg("--adapter-socket")
    .arg(&socket)
    .env("AETHER_TEST_STOP_FILE", &marker)
    .spawn()
    .unwrap();
  let socket_deadline = std::time::Instant::now() + Duration::from_secs(1);
  while !socket.exists() {
    assert!(
      std::time::Instant::now() < socket_deadline,
      "host socket not ready"
    );
    std::thread::sleep(Duration::from_millis(5));
  }
  let mut stream = std::os::unix::net::UnixStream::connect(&socket).unwrap();
  stream
    .write_all(br#"{"version":2,"op":"heartbeat"}"#)
    .unwrap();
  stream.shutdown(std::net::Shutdown::Write).unwrap();
  let mut reply = String::new();
  stream.read_to_string(&mut reply).unwrap();
  assert!(reply.contains("unsupported adapter host IPC version"));
  let deadline = std::time::Instant::now() + Duration::from_secs(3);
  loop {
    if child.try_wait().unwrap().is_some() {
      break;
    }
    assert!(
      std::time::Instant::now() < deadline,
      "host did not expire its lease"
    );
    std::thread::sleep(Duration::from_millis(20));
  }
  assert_eq!(fs::read_to_string(&marker).unwrap(), "stopped");
  fs::remove_dir_all(dir).unwrap();
}

#[test]
fn deployed_runtime_admits_package_node_through_host() {
  use aether_hal::{
    AdapterFamilyDescriptor, NodeTypeContribution, NodeTypeRegistry, PackageRef, PortTransport,
  };
  let (dir, spec, _) = fixture();
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
      adapter_families: vec![AdapterFamilyDescriptor {
        id: "package-stub".into(),
        device_kinds: vec![NodeKind::Base],
        transport: PortTransport::InProcess,
        description: "Test base library".into(),
        settings_schema: None,
        supported_primitives: vec!["base.velocity@1.0.0".into()],
      }],
    })
    .unwrap();
  let original = Profile::from_toml(include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  )))
  .unwrap();
  let mut graph = serde_json::to_value(original).unwrap();
  let adapter =
    &mut graph["cores"]["eos"]["layers"]["hardware"]["groups"]["base"]["nodes"]["drive"]["adapter"];
  adapter["type"] = serde_json::json!("package-stub");
  adapter["package"] = serde_json::to_value(package).unwrap();
  let profile = Profile::from_json_with_registry(&graph.to_string(), &registry).unwrap();
  let second_profile = profile.clone();
  let second_spec = spec.clone();
  let runtime = aether_runtime::Runtime::from_profile_adapters_durable_with_hosts(
    profile,
    Arc::new(aether_common::time::SystemClock),
    dir.join("journal"),
    &[spec],
    Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
  )
  .unwrap();
  assert!(runtime
    .discover()
    .iter()
    .any(|node| node.node_id == "drive"));
  assert!(
    aether_runtime::Runtime::from_profile_adapters_durable_with_hosts(
      second_profile,
      Arc::new(aether_common::time::SystemClock),
      dir.join("journal"),
      &[second_spec],
      Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
    )
    .is_err()
  );
  drop(runtime);
  fs::remove_dir_all(dir).unwrap();
}
