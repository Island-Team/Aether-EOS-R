use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::Path;
use std::process::Command;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use aether_common::time::{Clock, ManualClock, SystemClock};
use aether_common::events::{EventSink, NonblockingStderr, Source};
use aether_dispatcher::{AdapterRegistration, AdmissionGeneration, OutcomeStatus};
use aether_hal::{
  BaseMovement, BaseNode, BaseObservation, ClockKind, CommandMeta, CommandOutcome, HalError,
  Health, MockBase, NodeAdapter, NodeKind, NodeStatus, Profile, SampleMeta,
};
use aether_runtime::{
  Runtime,
  grpc::{self, v1},
  host_skills::Catalog,
  managed_admission,
};
use sha2::{Digest, Sha256};

struct ConfirmedBase(Arc<MockBase>);
impl NodeAdapter for ConfirmedBase {
  fn kind(&self) -> NodeKind {
    self.0.kind()
  }
  fn adapter_type(&self) -> &str {
    self.0.adapter_type()
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
    true
  }
}

struct GatedBase {
  inner: Arc<MockBase>,
  gate_observe: bool,
  entered: AtomicBool,
  release: AtomicBool,
}
impl NodeAdapter for GatedBase {
  fn kind(&self) -> NodeKind {
    self.inner.kind()
  }
  fn adapter_type(&self) -> &str {
    self.inner.adapter_type()
  }
  fn status(&self) -> NodeStatus {
    self.inner.status()
  }
  fn health(&self) -> Health {
    self.inner.health()
  }
  fn stop(&self) -> Result<(), HalError> {
    self.inner.stop()
  }
  fn has_terminal_stop_fence(&self) -> bool {
    true
  }
}
impl BaseNode for GatedBase {
  fn observe(&self) -> Result<BaseObservation, HalError> {
    if self.gate_observe {
      self.entered.store(true, Ordering::Release);
      while !self.release.load(Ordering::Acquire) {
        std::thread::sleep(std::time::Duration::from_millis(1));
      }
    }
    self.inner.observe()
  }
  fn move_base(
    &self,
    meta: CommandMeta,
    movement: BaseMovement,
  ) -> Result<CommandOutcome, HalError> {
    if !self.gate_observe {
      self.entered.store(true, Ordering::Release);
      while !self.release.load(Ordering::Acquire) {
        std::thread::sleep(std::time::Duration::from_millis(1));
      }
    }
    self.inner.move_base(meta, movement)?;
    Ok(CommandOutcome::Completed)
  }
}
impl BaseNode for ConfirmedBase {
  fn observe(&self) -> Result<BaseObservation, HalError> {
    self.0.observe()
  }
  fn move_base(
    &self,
    meta: CommandMeta,
    movement: BaseMovement,
  ) -> Result<CommandOutcome, HalError> {
    self.0.move_base(meta, movement)?;
    Ok(CommandOutcome::Completed)
  }
}

fn digest(bytes: &[u8]) -> String {
  format!("{:x}", Sha256::digest(bytes))
}

fn private(path: &Path, bytes: &[u8]) {
  fs::write(path, bytes).unwrap();
  fs::set_permissions(path, fs::Permissions::from_mode(0o600)).unwrap();
}

fn authorized<T>(body: T, grant: u8, revision: Option<&str>) -> tonic::Request<T> {
  let mut request = tonic::Request::new(body);
  request.metadata_mut().insert(
    "authorization",
    format!("Bearer {}", format!("{grant:02x}").repeat(32))
      .parse()
      .unwrap(),
  );
  if let Some(revision) = revision {
    request
      .metadata_mut()
      .insert("x-eos-active-revision", revision.parse().unwrap());
  }
  request
}

fn payload_digest(root: &Path, file: &Path) -> String {
  let relative = file.strip_prefix(root).unwrap().to_str().unwrap();
  let bytes = fs::read(file).unwrap();
  let mut hash = Sha256::new();
  hash.update(b"isos-package-tree-v1\0");
  for (name, is_file, len, executable, content) in [
    ("lib", false, 0, false, None),
    (
      relative,
      true,
      bytes.len() as u64,
      true,
      Some(Sha256::digest(&bytes)),
    ),
  ] {
    hash.update((name.len() as u32).to_le_bytes());
    hash.update(name.as_bytes());
    hash.update([if is_file { b'F' } else { b'D' }]);
    hash.update([if executable { b'1' } else { b'0' }]);
    hash.update(len.to_le_bytes());
    if let Some(content) = content {
      hash.update(content);
    } else {
      hash.update([0u8; 32]);
    }
  }
  format!("{:x}", hash.finalize())
}

fn repin(
  package: &Path,
  library: &Path,
  selection: &Path,
  manifest_path: &Path,
  receipt: &mut serde_json::Value,
  selection_json: &mut serde_json::Value,
  manifest: &mut serde_json::Value,
) {
  let payload = payload_digest(package, library);
  receipt["payload_sha256"] = payload.clone().into();
  let receipt_bytes = serde_json::to_vec(receipt).unwrap();
  private(&package.join("receipt.json"), &receipt_bytes);
  selection_json["libraries"][0]["payload_sha256"] = payload.into();
  selection_json["libraries"][0]["receipt_sha256"] = digest(&receipt_bytes).into();
  selection_json["libraries"][0]["library_sha256"] = digest(&fs::read(library).unwrap()).into();
  let selection_bytes = serde_json::to_vec(selection_json).unwrap();
  private(selection, &selection_bytes);
  manifest["files"][1]["sha256"] = digest(&selection_bytes).into();
  private(manifest_path, &serde_json::to_vec(manifest).unwrap());
}

#[test]
fn selected_old_v2_library_is_inspected_only_in_worker() {
  let temp = tempfile::tempdir().unwrap();
  let package = temp.path().join("package");
  fs::create_dir_all(package.join("lib")).unwrap();
  let source = temp.path().join("fixture.c");
  private(&source, br#"
#include "eos/skill/abi.h"
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#define TEXT(x) ((eos_skill_text_v2){x, sizeof(x)-1})
#ifndef CLAIM_COUNT
#define CLAIM_COUNT 0
#endif
#ifndef SKILL_NAME
#define SKILL_NAME "fixture.old"
#endif
#ifndef SKILL_VERSION
#define SKILL_VERSION "1.0.0"
#endif
#ifdef BAD_SCHEMA
#define PARAM_SCHEMA "{\"type\":\"object\",\"properties\":\"oops\"}"
#else
#define PARAM_SCHEMA "{\"type\":\"object\"}"
#endif
#ifdef WITH_BASE_PRIMITIVE
static const eos_skill_primitive_reference_v2 primitive = {
  sizeof(primitive), TEXT("base.velocity"), TEXT("1.0.0")};
#define PRIMITIVE_COUNT 1
#define PRIMITIVE_POINTER &primitive
#else
#define PRIMITIVE_COUNT 0
#define PRIMITIVE_POINTER 0
#endif
#ifdef NONREENTRANT
#define REENTRANT_FLAG 0
#else
#define REENTRANT_FLAG 1
#endif
static eos_skill_execute_status_v2 run(void *ctx, const eos_skill_execution_v2 *exec,
    eos_skill_complete_v2 complete, void *completion_ctx) {
  (void)ctx; (void)exec;
#ifdef BLOCK_EXECUTE
  for (;;) pause();
#endif
  if (getenv("EOS_CONTROL_GRANTS_FILE") || getenv("EOS_SKILL_SELECTION_FILE") ||
      getenv("HOST_SKILL_TEST_SECRET")) return EOS_SKILL_EXECUTE_FAILED;
  int net = socket(AF_INET, SOCK_STREAM, 0);
  if (net >= 0) { close(net); return EOS_SKILL_EXECUTE_FAILED; }
  if (syscall(SYS_io_uring_setup, 1, 0) != -1 || errno != EPERM)
    return EOS_SKILL_EXECUTE_FAILED;
#ifdef __x86_64__
  if (syscall(0x40000000L | SYS_socket, AF_INET, SOCK_STREAM, 0) != -1 || errno != EPERM)
    return EOS_SKILL_EXECUTE_FAILED;
#endif
  int secret = open("/etc/passwd", O_RDONLY);
  if (secret >= 0) { close(secret); return EOS_SKILL_EXECUTE_FAILED; }
#ifdef PRIMITIVE_GUEST
  const eos_skill_primitive_request_v2 request = {
    sizeof(request), TEXT("base.velocity"), TEXT("1.0.0"), TEXT("drive"),
    TEXT("{\"linear_x_m_s\":0.1,\"linear_y_m_s\":0,\"angular_z_rad_s\":0,\"valid_for_ms\":100}")};
  eos_skill_primitive_response_v2 response = {sizeof(response), {0}, {0}};
  if (!exec->host || !exec->host->call_primitive ||
      exec->host->call_primitive(exec->host->context, &request, &response) != EOS_SKILL_HOST_OK)
    return EOS_SKILL_EXECUTE_FAILED;
  if (response.acknowledgement_json.size == 0 || response.acknowledgement_json.size > 4096)
    return EOS_SKILL_EXECUTE_FAILED;
  char acknowledgement[4097];
  memcpy(acknowledgement, response.acknowledgement_json.data, response.acknowledgement_json.size);
  acknowledgement[response.acknowledgement_json.size] = 0;
  if (!strstr(acknowledgement, "\"type\":\"primitive.ack\"") ||
      !strstr(acknowledgement, "\"accepted\":true") ||
      !strstr(acknowledgement, "\"status\":\"succeeded\"") ||
      !strstr(acknowledgement, "\"command_id\":\"sk-"))
    return EOS_SKILL_EXECUTE_FAILED;
#endif
#ifdef DENIED_THEN_VALID
  const eos_skill_primitive_request_v2 wrong = {
    sizeof(wrong), TEXT("base.velocity"), TEXT("1.0.0"), TEXT("wrong-device"),
    TEXT("{\"linear_x_m_s\":0.1,\"linear_y_m_s\":0,\"angular_z_rad_s\":0,\"valid_for_ms\":100}")};
  const eos_skill_primitive_request_v2 valid = {
    sizeof(valid), TEXT("base.velocity"), TEXT("1.0.0"), TEXT("drive"),
    TEXT("{\"linear_x_m_s\":0.1,\"linear_y_m_s\":0,\"angular_z_rad_s\":0,\"valid_for_ms\":100}")};
  eos_skill_primitive_response_v2 first_response = {sizeof(first_response), {0}, {0}};
  eos_skill_primitive_response_v2 second_response = {sizeof(second_response), {0}, {0}};
  if (exec->host->call_primitive(exec->host->context, &wrong, &first_response) == EOS_SKILL_HOST_OK)
    return EOS_SKILL_EXECUTE_FAILED;
  if (exec->host->call_primitive(exec->host->context, &valid, &second_response) == EOS_SKILL_HOST_OK)
    return EOS_SKILL_EXECUTE_FAILED;
#endif
#ifdef OVERSIZED_RESULT
  size_t length = 1024u * 1024u + 64u;
  char *body = malloc(length + 1u);
  if (!body) return EOS_SKILL_EXECUTE_FAILED;
  memcpy(body, "{\"payload\":\"", 12);
  memset(body + 12, 'x', length - 14);
  body[length - 2] = '"'; body[length - 1] = '}'; body[length] = 0;
  const eos_skill_completion_v2 result = {sizeof(result), 1, TEXT("done"), {body, (uint32_t)length}, TEXT("")};
  uint32_t first = complete(completion_ctx, &result);
  free(body);
#else
  const eos_skill_completion_v2 result = {sizeof(result), 1, TEXT("done"), TEXT("{}"), TEXT("")};
  uint32_t first = complete(completion_ctx, &result);
#endif
#ifdef DOUBLE_COMPLETE
  const eos_skill_completion_v2 second = {sizeof(second), 1, TEXT("second"), TEXT("not-json"), TEXT("")};
  (void)complete(completion_ctx, &second);
#endif
  return first == EOS_SKILL_HOST_OK ? EOS_SKILL_EXECUTE_OK : EOS_SKILL_EXECUTE_FAILED;
}
struct OldDescriptor { uint32_t struct_size; eos_skill_text_v2 name, version, description,
  parameters_schema_json, returns_schema_json; uint32_t primitive_count;
  const eos_skill_primitive_reference_v2 *primitives; uint32_t resource_count;
  const eos_skill_resource_v2 *resources; uint32_t signal_count;
  const eos_skill_signal_descriptor_v2 *signals; uint32_t claim_count;
  const eos_skill_claim_template_v2 *claims; uint32_t reentrant; void *context;
  eos_skill_execute_v2 execute; };
_Static_assert(sizeof(struct OldDescriptor) == EOS_SKILL_DESCRIPTOR_V2_BASE_SIZE, "ABI descriptor");
static const struct OldDescriptor skill = {sizeof(skill), TEXT(SKILL_NAME), TEXT(SKILL_VERSION),
  TEXT("old ABI fixture"), TEXT(PARAM_SCHEMA), TEXT("{\"type\":\"object\"}"),
  PRIMITIVE_COUNT, PRIMITIVE_POINTER, 0, 0, 0, 0, CLAIM_COUNT, 0, REENTRANT_FLAG, 0, run};
struct OldLibrary { uint32_t abi_version, struct_size; eos_skill_text_v2 name, version, build_id;
  uint32_t skill_count; const eos_skill_descriptor_v2 *skills; };
_Static_assert(sizeof(struct OldLibrary) == EOS_SKILL_LIBRARY_V2_BASE_SIZE, "ABI library");
static const struct OldLibrary library = {2, sizeof(library), TEXT("fixture.old"), TEXT("1.0.0"),
  TEXT("old"), 1, (const eos_skill_descriptor_v2 *)&skill};
const eos_skill_library_v2 *eos_skill_library_query_v2(uint32_t version) {
#ifdef BLOCK_QUERY
  for (;;) pause();
#endif
#ifdef FORGE_CATALOG
  static const char fake[] = "{\"version\":1,\"body\":{\"kind\":\"catalog\",\"descriptors\":[{\"name\":\"Bad/Name\",\"version\":\"1.0.0\",\"package_id\":\"\",\"package_version\":\"\",\"description\":\"forged\",\"parameters_schema\":{\"type\":\"object\"},\"returns_schema\":{\"type\":\"object\"},\"primitives\":[],\"reentrant\":true,\"library_index\":0,\"descriptor_index\":0}]}}";
  (void)send(3, fake, sizeof(fake)-1, 0);
#endif
  return version == 2 ? (const eos_skill_library_v2 *)&library : 0;
}
"#);
  let library = package.join("lib/fixture.skill.so");
  let output = Command::new("cc")
    .args(["-shared", "-fPIC", "-I"])
    .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../src/eos/include"))
    .arg(&source)
    .arg("-o")
    .arg(&library)
    .output()
    .unwrap();
  assert!(
    output.status.success(),
    "{}",
    String::from_utf8_lossy(&output.stderr)
  );
  fs::set_permissions(&library, fs::Permissions::from_mode(0o755)).unwrap();
  let library_bytes = fs::read(&library).unwrap();
  let payload = payload_digest(&package, &library);
  let release = serde_json::json!({
    "schema_version":4,"id":"island/fixture-skills","kind":"skill","version":"0.1.0+build.1",
    "source_commit":"a".repeat(40),
    "assets":[{"os":std::env::consts::OS,"arch":std::env::consts::ARCH,"url":"https://github.com/Island-Team/fixture","size_bytes":1,"sha256":"b".repeat(64)}]
  });
  let release_bytes = serde_json::to_vec(&release).unwrap();
  private(&package.join("release.json"), &release_bytes);
  let mut receipt = serde_json::json!({
    "schema_version":2,"id":"island/fixture-skills","version":"0.1.0+build.1",
    "source_kind":"local_candidate","source_commit":"a".repeat(40),
    "asset_sha256":"b".repeat(64),"local_link":null,
    "release_sha256":digest(&release_bytes),"payload_sha256":payload,
    "sidecar_sha256":null
  });
  let receipt_bytes = serde_json::to_vec(&receipt).unwrap();
  private(&package.join("receipt.json"), &receipt_bytes);
  let revision = "c".repeat(64);
  let generation = "d".repeat(64);
  let leaf = temp.path().join("v2").join(&revision).join(&generation);
  fs::create_dir_all(leaf.join("eos")).unwrap();
  fs::create_dir_all(leaf.join("isos")).unwrap();
  for dir in [
    temp.path().join("v2"),
    temp.path().join("v2").join(&revision),
    leaf.clone(),
    leaf.join("eos"),
    leaf.join("isos"),
  ] {
    fs::set_permissions(dir, fs::Permissions::from_mode(0o700)).unwrap();
  }
  let graph = leaf.join("eos/graph.json");
  let graph_bytes = include_bytes!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  ));
  private(&graph, graph_bytes);
  let selection = leaf.join("isos/skill-selection.json");
  let mut selection_json = serde_json::json!({
    "schema_version":1,"active_revision":revision,
    "libraries":[{
      "package_id":"island/fixture-skills","package_version":"0.1.0+build.1",
      "package_root":package,"payload_root":package,
      "receipt_sha256":digest(&receipt_bytes),"release_sha256":digest(&release_bytes),
      "payload_sha256":payload,"library":"lib/fixture.skill.so","library_sha256":digest(&library_bytes)
    }]
  });
  let selection_bytes = serde_json::to_vec(&selection_json).unwrap();
  private(&selection, &selection_bytes);
  let mut manifest = serde_json::json!({"schema_version":2,"revision":revision,"generation":generation,
    "files":[{"path":"eos/graph.json","sha256":digest(graph_bytes)},
      {"path":"isos/skill-selection.json","sha256":digest(&selection_bytes)}]});
  private(
    &leaf.join("manifest.json"),
    serde_json::to_vec(&manifest).unwrap().as_slice(),
  );
  let run = temp.path().join("run");
  fs::create_dir_all(&run).unwrap();
  fs::set_permissions(&run, fs::Permissions::from_mode(0o700)).unwrap();
  let verifier = run.join("verifier.json");
  private(
    &verifier,
    serde_json::to_vec(&serde_json::json!({
      "schema_version":1,"run_id":"run-1","active_revision":revision,
      "egia_control_sha256":digest(&[7u8;32]),"operator_control_sha256":digest(&[8u8;32])
    }))
    .unwrap()
    .as_slice(),
  );
  let managed = managed_admission::load(&graph, graph_bytes, Some(&verifier), "run-1")
    .unwrap()
    .unwrap();
  std::env::set_var("HOST_SKILL_TEST_SECRET", "never-forward-this");
  std::env::set_var("EOS_CONTROL_GRANTS_FILE", &verifier);
  std::env::set_var("EOS_SKILL_SELECTION_FILE", &selection);
  let catalog = Catalog::admit(
    &graph,
    Some(&managed),
    Some(&selection),
    Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
  )
  .unwrap();
  let catalog = Arc::new(catalog);
  assert_eq!(catalog.descriptors().len(), 1);
  assert_eq!(catalog.descriptors()[0].name, "fixture.old");
  assert_eq!(catalog.descriptors()[0].package_id, "island/fixture-skills");
  assert_eq!(catalog.descriptors()[0].package_version, "0.1.0+build.1");
  let probe = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
    .args(["--skill-catalog-preflight", "--profile"])
    .arg(&graph)
    .args(["--run-id", "run-1"])
    .output()
    .unwrap();
  assert!(
    probe.status.success(),
    "{}",
    String::from_utf8_lossy(&probe.stdout)
  );
  let lines: Vec<_> = probe.stdout.split(|byte| *byte == b'\n').collect();
  assert_eq!(lines.len(), 2);
  assert!(lines[1].is_empty());
  let probe: serde_json::Value = serde_json::from_slice(lines[0]).unwrap();
  assert_eq!(probe["ok"], true);
  assert_eq!(probe["active_revision"], revision);
  assert_eq!(probe["generation"], generation);
  assert_eq!(probe["run_id"], "run-1");
  assert_eq!(probe["selection_sha256"], digest(&selection_bytes));
  assert_eq!(probe["libraries"][0]["skill_count"], 1);
  assert_eq!(probe["libraries"][0]["abi_version"], 2);
  assert_eq!(
    probe["libraries"][0]["library_sha256"],
    selection_json["libraries"][0]["library_sha256"]
  );
  let forbidden = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
    .args(["--skill-catalog-preflight", "--profile"])
    .arg(&graph)
    .args(["--run-id", "run-1", "--socket", "/tmp/forbidden"])
    .output()
    .unwrap();
  assert!(!forbidden.status.success());
  assert_eq!(
    serde_json::from_slice::<serde_json::Value>(&forbidden.stdout).unwrap(),
    serde_json::json!({"schema_version":1,"ok":false,"stage":"input","code":"preflight_arguments_invalid"})
  );
  let stale_run = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
    .args(["--skill-catalog-preflight", "--profile"])
    .arg(&graph)
    .args(["--run-id", "stale-run"])
    .output()
    .unwrap();
  assert!(!stale_run.status.success());
  assert_eq!(
    serde_json::from_slice::<serde_json::Value>(&stale_run.stdout).unwrap(),
    serde_json::json!({"schema_version":1,"ok":false,"stage":"input","code":"preflight_grants_invalid"})
  );
  fs::write(&selection, b"{}").unwrap();
  let changed_selection = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
    .args(["--skill-catalog-preflight", "--profile"])
    .arg(&graph)
    .args(["--run-id", "run-1"])
    .output()
    .unwrap();
  assert!(!changed_selection.status.success());
  assert_eq!(
    serde_json::from_slice::<serde_json::Value>(&changed_selection.stdout).unwrap(),
    serde_json::json!({"schema_version":1,"ok":false,"stage":"selection","code":"selection_digest_mismatch"})
  );
  fs::write(&selection, &selection_bytes).unwrap();
  let source_bytes = fs::read(&library).unwrap();
  fs::write(&library, b"changed after preflight").unwrap();
  let changed = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
    .args(["--skill-catalog-preflight", "--profile"])
    .arg(&graph)
    .args(["--run-id", "run-1"])
    .output()
    .unwrap();
  assert!(!changed.status.success());
  assert_eq!(
    serde_json::from_slice::<serde_json::Value>(&changed.stdout).unwrap(),
    serde_json::json!({"schema_version":1,"ok":false,"stage":"package","code":"package_changed"})
  );
  assert!(Catalog::admit(
    &graph,
    Some(&managed),
    Some(&selection),
    Path::new(env!("CARGO_BIN_EXE_aether-runtime"))
  )
  .is_err());
  fs::write(&library, source_bytes).unwrap();
  let mut session = catalog
    .start_worker(
      Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
      "fixture.old",
      "1.0.0",
      "request-1",
      "{}",
    )
    .unwrap();
  let event = session.recv(std::time::Duration::from_secs(5)).unwrap();
  assert!(matches!(
    event,
    aether_runtime::host_skills::WorkerEvent::Completion { accepted: true, .. }
  ));
  assert!(session.reap(std::time::Duration::from_secs(1)).unwrap());
  let invalid_invocation = catalog
    .start_worker(
      Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
      "fixture.old",
      "1.0.0",
      "oversized-after-spawn",
      &"x".repeat(1024 * 1024 + 1),
    )
    .err()
    .unwrap();
  assert_eq!(invalid_invocation.code, "worker_request_invalid");
  assert_eq!(invalid_invocation.worker_reaped, Some(true));
  let runtime_profile = Profile::from_toml(include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  )))
  .unwrap();
  let runtime =
    Arc::new(Runtime::from_cxx_mock_profile(runtime_profile, Arc::new(SystemClock)).unwrap());
  let event_sink = Arc::new(
    EventSink::new(
      Source::Eos,
      managed.run_id.clone(),
      NonblockingStderr,
      SystemClock,
    )
    .unwrap(),
  );
  let projection = grpc::Projection::with_camera_cache_managed(
    runtime,
    Arc::new(grpc::CameraCache::default()),
    Some(Arc::new(managed.clone())),
    revision.clone(),
  )
  .unwrap()
  .with_host_skills(
    catalog.clone(),
    Path::new(env!("CARGO_BIN_EXE_aether-runtime")).into(),
    Some(event_sink),
  )
  .unwrap();
  let shutdown_projection = projection.clone();
  let mut server = grpc::ServerHandle::start(projection, 0).unwrap();
  let endpoint = format!("http://{}", server.address());
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut client = v1::skill_service_client::SkillServiceClient::connect(endpoint)
        .await
        .unwrap();
      let listed = client
        .list_skills(v1::ListSkillsRequest {})
        .await
        .unwrap()
        .into_inner();
      assert_eq!(listed.skills.len(), 1);
      assert_eq!(
        listed.skills[0].reference.as_ref().unwrap().name,
        "fixture.old"
      );
      assert_eq!(
        client
          .get_skill(v1::GetSkillRequest {
            reference: Some(v1::SkillReference {
              name: "fixture.old".into(),
              version: "1.0.0".into(),
            })
          })
          .await
          .unwrap()
          .into_inner(),
        listed.skills[0]
      );
      let start = |arguments: prost_types::Struct| v1::StartExecutionRequest {
        reference: Some(v1::SkillReference {
          name: "fixture.old".into(),
          version: "1.0.0".into(),
        }),
        arguments: Some(arguments),
        request_id: "请求-一".into(),
        timeout: Some(prost_types::Duration {
          seconds: 5,
          nanos: 0,
        }),
      };
      assert_eq!(
        client
          .start_execution(start(prost_types::Struct::default()))
          .await
          .unwrap_err()
          .code(),
        tonic::Code::Unauthenticated
      );
      let operator = authorized(start(prost_types::Struct::default()), 8, Some(&revision));
      assert_eq!(
        client.start_execution(operator).await.unwrap_err().code(),
        tonic::Code::PermissionDenied
      );
      let mut stale = authorized(start(prost_types::Struct::default()), 7, Some(&revision));
      stale
        .metadata_mut()
        .insert("x-eos-active-revision", "f".repeat(64).parse().unwrap());
      assert_eq!(
        client.start_execution(stale).await.unwrap_err().code(),
        tonic::Code::FailedPrecondition
      );
      let first = client
        .start_execution(authorized(
          start(prost_types::Struct::default()),
          7,
          Some(&revision),
        ))
        .await
        .unwrap()
        .into_inner();
      assert_eq!(first.status, v1::ExecutionStatus::Queued as i32);
      let duplicate = client
        .start_execution(authorized(
          start(prost_types::Struct::default()),
          7,
          Some(&revision),
        ))
        .await
        .unwrap()
        .into_inner();
      assert_eq!(duplicate.execution_id, first.execution_id);
      let different = prost_types::Struct {
        fields: [(
          "different".into(),
          prost_types::Value {
            kind: Some(prost_types::value::Kind::BoolValue(true)),
          },
        )]
        .into(),
      };
      assert_eq!(
        client
          .start_execution(authorized(start(different), 7, Some(&revision)))
          .await
          .unwrap_err()
          .code(),
        tonic::Code::AlreadyExists
      );
      let mut unknown = start(prost_types::Struct::default());
      unknown.reference.as_mut().unwrap().name = "missing.skill".into();
      assert_eq!(
        client
          .start_execution(authorized(unknown, 7, Some(&revision)))
          .await
          .unwrap_err()
          .code(),
        tonic::Code::AlreadyExists
      );
      let watch_request = authorized(
        v1::WatchExecutionRequest {
          execution_id: first.execution_id.clone(),
          after_sequence: 0,
        },
        7,
        None,
      );
      let mut stream = client
        .watch_execution(watch_request)
        .await
        .unwrap()
        .into_inner();
      let mut statuses = Vec::new();
      while let Some(snapshot) =
        tokio::time::timeout(std::time::Duration::from_secs(5), stream.message())
          .await
          .unwrap()
          .unwrap()
      {
        statuses.push(snapshot.status);
      }
      assert_eq!(
        statuses,
        [
          v1::ExecutionStatus::Queued as i32,
          v1::ExecutionStatus::Running as i32,
          v1::ExecutionStatus::Succeeded as i32
        ]
      );
      let finished = client
        .get_execution(authorized(
          v1::GetExecutionRequest {
            execution_id: first.execution_id.clone(),
          },
          7,
          None,
        ))
        .await
        .unwrap()
        .into_inner();
      assert_eq!(finished.status, v1::ExecutionStatus::Succeeded as i32);
      assert_eq!(
        client
          .list_executions(authorized(v1::ListExecutionsRequest {}, 7, None))
          .await
          .unwrap()
          .into_inner()
          .executions
          .len(),
        1
      );
      assert_eq!(
        client
          .terminate_execution(authorized(
            v1::TerminateExecutionRequest {
              execution_id: first.execution_id.clone()
            },
            7,
            Some(&revision)
          ))
          .await
          .unwrap()
          .into_inner(),
        finished
      );
    });
  server.shutdown();
  shutdown_projection.shutdown_host_skills().unwrap();
  let declared = Command::new("cc")
    .args(["-shared", "-fPIC", "-DWITH_BASE_PRIMITIVE", "-I"])
    .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../src/eos/include"))
    .arg(&source)
    .arg("-o")
    .arg(&library)
    .output()
    .unwrap();
  assert!(
    declared.status.success(),
    "{}",
    String::from_utf8_lossy(&declared.stderr)
  );
  fs::set_permissions(&library, fs::Permissions::from_mode(0o755)).unwrap();
  repin(
    &package,
    &library,
    &selection,
    &leaf.join("manifest.json"),
    &mut receipt,
    &mut selection_json,
    &mut manifest,
  );
  let declared_catalog = Arc::new(
    Catalog::admit(
      &graph,
      Some(&managed),
      Some(&selection),
      Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
    )
    .unwrap(),
  );
  assert_eq!(
    declared_catalog.descriptors()[0].primitives[0].name,
    "base.velocity"
  );
  let unbound_probe = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
    .args(["--skill-catalog-preflight", "--profile"])
    .arg(&graph)
    .args(["--run-id", "run-1"])
    .output()
    .unwrap();
  assert!(!unbound_probe.status.success());
  assert_eq!(
    serde_json::from_slice::<serde_json::Value>(&unbound_probe.stdout).unwrap(),
    serde_json::json!({"schema_version":1,"ok":false,"stage":"catalog","code":"catalog_binding_unavailable"})
  );
  let unbound_runtime = Arc::new(
    Runtime::from_cxx_mock_profile(
      Profile::from_toml(include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../etc/eos/backends/mock-cxx.toml"
      )))
      .unwrap(),
      Arc::new(SystemClock),
    )
    .unwrap(),
  );
  let unbound_projection = grpc::Projection::with_camera_cache_managed(
    unbound_runtime,
    Arc::new(grpc::CameraCache::default()),
    Some(Arc::new(managed.clone())),
    revision.clone(),
  )
  .unwrap();
  assert!(unbound_projection
    .with_host_skills(
      declared_catalog.clone(),
      Path::new(env!("CARGO_BIN_EXE_aether-runtime")).into(),
      None
    )
    .is_err());
  let primitive_guest = Command::new("cc")
    .args([
      "-shared",
      "-fPIC",
      "-DWITH_BASE_PRIMITIVE",
      "-DPRIMITIVE_GUEST",
      "-I",
    ])
    .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../src/eos/include"))
    .arg(&source)
    .arg("-o")
    .arg(&library)
    .output()
    .unwrap();
  assert!(
    primitive_guest.status.success(),
    "{}",
    String::from_utf8_lossy(&primitive_guest.stderr)
  );
  fs::set_permissions(&library, fs::Permissions::from_mode(0o755)).unwrap();
  repin(
    &package,
    &library,
    &selection,
    &leaf.join("manifest.json"),
    &mut receipt,
    &mut selection_json,
    &mut manifest,
  );
  let primitive_catalog = Arc::new(
    Catalog::admit(
      &graph,
      Some(&managed),
      Some(&selection),
      Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
    )
    .unwrap(),
  );
  let mut graph_profile = serde_json::to_value(
    Profile::from_toml(include_str!(concat!(
      env!("CARGO_MANIFEST_DIR"),
      "/../../etc/eos/backends/mock-cxx.toml"
    )))
    .unwrap(),
  )
  .unwrap();
  graph_profile["version"] = "0.1.0".into();
  graph_profile["cores"]
    .as_object_mut()
    .unwrap()
    .remove("egia");
  let layers = graph_profile["cores"]["eos"]["layers"]
    .as_object_mut()
    .unwrap();
  layers.retain(|name, _| name == "hardware" || name == "dispatcher");
  layers.get_mut("hardware").unwrap()["groups"]
    .as_object_mut()
    .unwrap()
    .retain(|name, _| name == "base");
  let base = &mut layers.get_mut("hardware").unwrap()["groups"]["base"]["nodes"]["drive"];
  base["node_type"] = "eos.hardware.base@v0.1".into();
  base["adapter"]["type"] = "mock".into();
  base["inputs"]["command"] =
    serde_json::json!([{"node":"eos/dispatcher/base/velocity","output":"command"}]);
  layers.get_mut("dispatcher").unwrap()["groups"] = serde_json::json!({"base":{"nodes":{
    "velocity":{"id":"velocity","node_type":"eos.dispatcher.primitive_base@v0.1",
      "label":"Base velocity","description":"Bounded velocity binding","observation":null,
      "movement":null,"settings":{"primitive_name":"base.velocity",
        "primitive_version":"1.0.0","movement_type":"body_velocity"}}
  }}});
  let bound_profile = Profile::from_json(&graph_profile.to_string()).unwrap();
  assert!(
    !managed_admission::admitted_bindings(&bound_profile).is_empty(),
    "{:?}",
    bound_profile
      .hardware_nodes()
      .map(|node| (&node.id, &node.inputs))
      .collect::<Vec<_>>()
  );
  grpc::validate_host_catalog_readiness(&bound_profile, true, &declared_catalog).unwrap();
  let clock: Arc<dyn Clock> = Arc::new(ManualClock::new(0, None));
  let base_adapter = Arc::new(MockBase::new(BaseObservation {
    meta: SampleMeta {
      source_timestamp_ns: None,
      clock: ClockKind::Unknown,
      received_monotonic_ns: 0,
      sequence: None,
      frame_id: "base_link".into(),
    },
    valid: true,
    x_m: 0.0,
    y_m: 0.0,
    yaw_rad: 0.0,
    x_m_s: 0.0,
    y_m_s: 0.0,
    yaw_rad_s: 0.0,
  }));
  let managed_runtime = Arc::new(
    Runtime::new_durable_managed(
      bound_profile.clone(),
      vec![AdapterRegistration::Base {
        node_id: "drive".into(),
        adapter: Arc::new(ConfirmedBase(base_adapter.clone())),
      }],
      clock,
      temp.path().join("journal"),
      AdmissionGeneration {
        run_id: managed.run_id.clone(),
        active_revision: managed.active_revision.clone(),
        bindings: managed_admission::admitted_bindings(&bound_profile),
      },
    )
    .unwrap(),
  );
  let primitive_projection = grpc::Projection::with_camera_cache_managed(
    managed_runtime.clone(),
    Arc::new(grpc::CameraCache::default()),
    Some(Arc::new(managed.clone())),
    revision.clone(),
  )
  .unwrap()
  .with_host_skills(
    primitive_catalog.clone(),
    Path::new(env!("CARGO_BIN_EXE_aether-runtime")).into(),
    None,
  )
  .unwrap();
  let mut primitive_server = grpc::ServerHandle::start(primitive_projection, 0).unwrap();
  let primitive_endpoint = format!("http://{}", primitive_server.address());
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut client = v1::skill_service_client::SkillServiceClient::connect(primitive_endpoint)
        .await
        .unwrap();
      let started = client
        .start_execution(authorized(
          v1::StartExecutionRequest {
            reference: Some(v1::SkillReference {
              name: "fixture.old".into(),
              version: "1.0.0".into(),
            }),
            arguments: Some(prost_types::Struct::default()),
            request_id: "primitive-one".into(),
            timeout: Some(prost_types::Duration {
              seconds: 5,
              nanos: 0,
            }),
          },
          7,
          Some(&revision),
        ))
        .await
        .unwrap()
        .into_inner();
      let mut watch = client
        .watch_execution(authorized(
          v1::WatchExecutionRequest {
            execution_id: started.execution_id,
            after_sequence: 0,
          },
          7,
          None,
        ))
        .await
        .unwrap()
        .into_inner();
      let mut final_status = None;
      while let Some(snapshot) =
        tokio::time::timeout(std::time::Duration::from_secs(5), watch.message())
          .await
          .unwrap()
          .unwrap()
      {
        final_status = Some(snapshot.status);
      }
      assert_eq!(final_status, Some(v1::ExecutionStatus::Succeeded as i32));
    });
  assert_eq!(base_adapter.commands().len(), 1);
  let command_id = &base_adapter.commands()[0].0.command_id;
  assert!(command_id.starts_with("sk-"));
  let operation = managed_runtime
    .dispatcher()
    .gateway_operation_authorized(
      command_id,
      &managed.run_id,
      aether_dispatcher::AuthorityRole::EgiaControl,
    )
    .unwrap()
    .unwrap();
  assert_eq!(operation.status, OutcomeStatus::Completed);
  assert_eq!(operation.primitive, "base.velocity");
  primitive_server.shutdown();
  for (gate_observe, expected_status, expected_reason) in [
    (true, v1::ExecutionStatus::Cancelled, "worker.cancelled"),
    (false, v1::ExecutionStatus::Failed, "worker.outcome_unknown"),
  ] {
    let guarded_base = Arc::new(GatedBase {
      inner: Arc::new(MockBase::new(BaseObservation {
        meta: SampleMeta {
          source_timestamp_ns: None,
          clock: ClockKind::Unknown,
          received_monotonic_ns: 0,
          sequence: None,
          frame_id: "base_link".into(),
        },
        valid: true,
        x_m: 0.0,
        y_m: 0.0,
        yaw_rad: 0.0,
        x_m_s: 0.0,
        y_m_s: 0.0,
        yaw_rad_s: 0.0,
      })),
      gate_observe,
      entered: AtomicBool::new(false),
      release: AtomicBool::new(false),
    });
    let guarded_runtime = Arc::new(
      Runtime::new_durable_managed(
        bound_profile.clone(),
        vec![AdapterRegistration::Base {
          node_id: "drive".into(),
          adapter: guarded_base.clone(),
        }],
        Arc::new(ManualClock::new(0, None)),
        temp.path().join(if gate_observe {
          "prepared-journal"
        } else {
          "releasing-journal"
        }),
        AdmissionGeneration {
          run_id: managed.run_id.clone(),
          active_revision: managed.active_revision.clone(),
          bindings: managed_admission::admitted_bindings(&bound_profile),
        },
      )
      .unwrap(),
    );
    let guarded_projection = grpc::Projection::with_camera_cache_managed(
      guarded_runtime.clone(),
      Arc::new(grpc::CameraCache::default()),
      Some(Arc::new(managed.clone())),
      revision.clone(),
    )
    .unwrap()
    .with_host_skills(
      primitive_catalog.clone(),
      Path::new(env!("CARGO_BIN_EXE_aether-runtime")).into(),
      None,
    )
    .unwrap();
    let mut guarded_server = grpc::ServerHandle::start(guarded_projection, 0).unwrap();
    let endpoint = format!("http://{}", guarded_server.address());
    tokio::runtime::Builder::new_current_thread()
      .enable_all()
      .build()
      .unwrap()
      .block_on(async {
        let mut client = v1::skill_service_client::SkillServiceClient::connect(endpoint)
          .await
          .unwrap();
        let started = client
          .start_execution(authorized(
            v1::StartExecutionRequest {
              reference: Some(v1::SkillReference {
                name: "fixture.old".into(),
                version: "1.0.0".into(),
              }),
              arguments: Some(prost_types::Struct::default()),
              request_id: if gate_observe {
                "prepared-cancel"
              } else {
                "releasing-cancel"
              }
              .into(),
              timeout: Some(prost_types::Duration {
                seconds: 5,
                nanos: 0,
              }),
            },
            7,
            Some(&revision),
          ))
          .await
          .unwrap()
          .into_inner();
        let until = std::time::Instant::now() + std::time::Duration::from_secs(5);
        while !guarded_base.entered.load(Ordering::Acquire) {
          assert!(
            std::time::Instant::now() < until,
            "adapter gate was not reached"
          );
          tokio::time::sleep(std::time::Duration::from_millis(5)).await;
        }
        let returned = tokio::time::timeout(
          std::time::Duration::from_secs(2),
          client.terminate_execution(authorized(
            v1::TerminateExecutionRequest {
              execution_id: started.execution_id.clone(),
            },
            7,
            Some(&revision),
          )),
        )
        .await
        .unwrap()
        .unwrap()
        .into_inner();
        assert!(returned.cancellation_requested);
        tokio::time::sleep(std::time::Duration::from_millis(150)).await;
        guarded_base.release.store(true, Ordering::Release);
        let mut stream = client
          .watch_execution(authorized(
            v1::WatchExecutionRequest {
              execution_id: started.execution_id,
              after_sequence: 0,
            },
            7,
            None,
          ))
          .await
          .unwrap()
          .into_inner();
        let mut terminal = None;
        while let Some(snapshot) =
          tokio::time::timeout(std::time::Duration::from_secs(5), stream.message())
            .await
            .unwrap()
            .unwrap()
        {
          terminal = Some(snapshot);
        }
        let terminal = terminal.unwrap();
        assert_eq!(terminal.status, expected_status as i32);
        assert_eq!(terminal.reason_code, expected_reason);
      });
    if gate_observe {
      assert!(guarded_base.inner.commands().is_empty());
    }
    guarded_server.shutdown();
  }
  let denied_guest = Command::new("cc")
    .args([
      "-shared",
      "-fPIC",
      "-DWITH_BASE_PRIMITIVE",
      "-DDENIED_THEN_VALID",
      "-I",
    ])
    .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../src/eos/include"))
    .arg(&source)
    .arg("-o")
    .arg(&library)
    .output()
    .unwrap();
  assert!(
    denied_guest.status.success(),
    "{}",
    String::from_utf8_lossy(&denied_guest.stderr)
  );
  fs::set_permissions(&library, fs::Permissions::from_mode(0o755)).unwrap();
  repin(
    &package,
    &library,
    &selection,
    &leaf.join("manifest.json"),
    &mut receipt,
    &mut selection_json,
    &mut manifest,
  );
  let denied_catalog = Arc::new(
    Catalog::admit(
      &graph,
      Some(&managed),
      Some(&selection),
      Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
    )
    .unwrap(),
  );
  let denied_projection = grpc::Projection::with_camera_cache_managed(
    managed_runtime.clone(),
    Arc::new(grpc::CameraCache::default()),
    Some(Arc::new(managed.clone())),
    revision.clone(),
  )
  .unwrap()
  .with_host_skills(
    denied_catalog,
    Path::new(env!("CARGO_BIN_EXE_aether-runtime")).into(),
    None,
  )
  .unwrap();
  let mut denied_server = grpc::ServerHandle::start(denied_projection, 0).unwrap();
  let denied_endpoint = format!("http://{}", denied_server.address());
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut client = v1::skill_service_client::SkillServiceClient::connect(denied_endpoint)
        .await
        .unwrap();
      let started = client
        .start_execution(authorized(
          v1::StartExecutionRequest {
            reference: Some(v1::SkillReference {
              name: "fixture.old".into(),
              version: "1.0.0".into(),
            }),
            arguments: Some(prost_types::Struct::default()),
            request_id: "denied-then-valid".into(),
            timeout: Some(prost_types::Duration {
              seconds: 5,
              nanos: 0,
            }),
          },
          7,
          Some(&revision),
        ))
        .await
        .unwrap()
        .into_inner();
      let mut watch = client
        .watch_execution(authorized(
          v1::WatchExecutionRequest {
            execution_id: started.execution_id,
            after_sequence: 0,
          },
          7,
          None,
        ))
        .await
        .unwrap()
        .into_inner();
      let mut finished = None;
      while let Some(snapshot) =
        tokio::time::timeout(std::time::Duration::from_secs(5), watch.message())
          .await
          .unwrap()
          .unwrap()
      {
        finished = Some(snapshot);
      }
      let finished = finished.unwrap();
      assert_eq!(finished.status, v1::ExecutionStatus::Failed as i32);
      assert_eq!(finished.reason_code, "worker.primitive_denied");
    });
  assert_eq!(
    base_adapter.commands().len(),
    1,
    "denied call must latch later release"
  );
  denied_server.shutdown();
  let blocked_guest = Command::new("cc")
    .args([
      "-shared",
      "-fPIC",
      "-DBLOCK_EXECUTE",
      "-DNONREENTRANT",
      "-I",
    ])
    .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../src/eos/include"))
    .arg(&source)
    .arg("-o")
    .arg(&library)
    .output()
    .unwrap();
  assert!(
    blocked_guest.status.success(),
    "{}",
    String::from_utf8_lossy(&blocked_guest.stderr)
  );
  fs::set_permissions(&library, fs::Permissions::from_mode(0o755)).unwrap();
  repin(
    &package,
    &library,
    &selection,
    &leaf.join("manifest.json"),
    &mut receipt,
    &mut selection_json,
    &mut manifest,
  );
  let blocked_catalog = Arc::new(
    Catalog::admit(
      &graph,
      Some(&managed),
      Some(&selection),
      Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
    )
    .unwrap(),
  );
  let blocked_projection = grpc::Projection::with_camera_cache_managed(
    managed_runtime.clone(),
    Arc::new(grpc::CameraCache::default()),
    Some(Arc::new(managed.clone())),
    revision.clone(),
  )
  .unwrap()
  .with_host_skills(
    blocked_catalog,
    Path::new(env!("CARGO_BIN_EXE_aether-runtime")).into(),
    None,
  )
  .unwrap();
  let mut blocked_server = grpc::ServerHandle::start(blocked_projection, 0).unwrap();
  let blocked_endpoint = format!("http://{}", blocked_server.address());
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut client = v1::skill_service_client::SkillServiceClient::connect(blocked_endpoint)
        .await
        .unwrap();
      let start = |request_id: &str| {
        authorized(
          v1::StartExecutionRequest {
            reference: Some(v1::SkillReference {
              name: "fixture.old".into(),
              version: "1.0.0".into(),
            }),
            arguments: Some(prost_types::Struct::default()),
            request_id: request_id.into(),
            timeout: None,
          },
          7,
          Some(&revision),
        )
      };
      let first = client
        .start_execution(start("blocked-first"))
        .await
        .unwrap()
        .into_inner();
      let second = client
        .start_execution(start("blocked-second"))
        .await
        .unwrap()
        .into_inner();
      let first_state = client
        .get_execution(authorized(
          v1::GetExecutionRequest {
            execution_id: first.execution_id.clone(),
          },
          7,
          None,
        ))
        .await
        .unwrap()
        .into_inner();
      assert_eq!(first_state.status, v1::ExecutionStatus::Running as i32);
      let second_state = client
        .get_execution(authorized(
          v1::GetExecutionRequest {
            execution_id: second.execution_id.clone(),
          },
          7,
          None,
        ))
        .await
        .unwrap()
        .into_inner();
      assert_eq!(second_state.status, v1::ExecutionStatus::Queued as i32);
      let cancelled = client
        .terminate_execution(authorized(
          v1::TerminateExecutionRequest {
            execution_id: second.execution_id.clone(),
          },
          7,
          Some(&revision),
        ))
        .await
        .unwrap()
        .into_inner();
      assert_eq!(cancelled.status, v1::ExecutionStatus::Cancelled as i32);
      client
        .terminate_execution(authorized(
          v1::TerminateExecutionRequest {
            execution_id: first.execution_id.clone(),
          },
          7,
          Some(&revision),
        ))
        .await
        .unwrap();
      let mut watch = client
        .watch_execution(authorized(
          v1::WatchExecutionRequest {
            execution_id: first.execution_id,
            after_sequence: 0,
          },
          7,
          None,
        ))
        .await
        .unwrap()
        .into_inner();
      let mut terminal = None;
      while let Some(snapshot) =
        tokio::time::timeout(std::time::Duration::from_secs(5), watch.message())
          .await
          .unwrap()
          .unwrap()
      {
        terminal = Some(snapshot.status);
      }
      assert_eq!(terminal, Some(v1::ExecutionStatus::Cancelled as i32));
      assert_eq!(
        client
          .get_execution(authorized(
            v1::GetExecutionRequest {
              execution_id: second.execution_id,
            },
            7,
            None
          ))
          .await
          .unwrap()
          .into_inner()
          .status,
        v1::ExecutionStatus::Cancelled as i32
      );
    });
  blocked_server.shutdown();
  let oversized_guest = Command::new("cc")
    .args(["-shared", "-fPIC", "-DOVERSIZED_RESULT", "-I"])
    .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../src/eos/include"))
    .arg(&source)
    .arg("-o")
    .arg(&library)
    .output()
    .unwrap();
  assert!(
    oversized_guest.status.success(),
    "{}",
    String::from_utf8_lossy(&oversized_guest.stderr)
  );
  fs::set_permissions(&library, fs::Permissions::from_mode(0o755)).unwrap();
  repin(
    &package,
    &library,
    &selection,
    &leaf.join("manifest.json"),
    &mut receipt,
    &mut selection_json,
    &mut manifest,
  );
  let oversized_catalog = Arc::new(
    Catalog::admit(
      &graph,
      Some(&managed),
      Some(&selection),
      Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
    )
    .unwrap(),
  );
  let oversized_projection = grpc::Projection::with_camera_cache_managed(
    managed_runtime.clone(),
    Arc::new(grpc::CameraCache::default()),
    Some(Arc::new(managed.clone())),
    revision.clone(),
  )
  .unwrap()
  .with_host_skills(
    oversized_catalog,
    Path::new(env!("CARGO_BIN_EXE_aether-runtime")).into(),
    None,
  )
  .unwrap();
  let mut oversized_server = grpc::ServerHandle::start(oversized_projection, 0).unwrap();
  let oversized_endpoint = format!("http://{}", oversized_server.address());
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut client = v1::skill_service_client::SkillServiceClient::connect(oversized_endpoint)
        .await
        .unwrap();
      let started = client
        .start_execution(authorized(
          v1::StartExecutionRequest {
            reference: Some(v1::SkillReference {
              name: "fixture.old".into(),
              version: "1.0.0".into(),
            }),
            arguments: Some(prost_types::Struct::default()),
            request_id: "oversized-result".into(),
            timeout: Some(prost_types::Duration {
              seconds: 5,
              nanos: 0,
            }),
          },
          7,
          Some(&revision),
        ))
        .await
        .unwrap()
        .into_inner();
      let mut stream = client
        .watch_execution(authorized(
          v1::WatchExecutionRequest {
            execution_id: started.execution_id,
            after_sequence: 0,
          },
          7,
          None,
        ))
        .await
        .unwrap()
        .into_inner();
      let mut terminal = None;
      while let Some(snapshot) =
        tokio::time::timeout(std::time::Duration::from_secs(5), stream.message())
          .await
          .unwrap()
          .unwrap()
      {
        terminal = Some(snapshot);
      }
      let terminal = terminal.unwrap();
      assert_eq!(terminal.status, v1::ExecutionStatus::Failed as i32);
      assert_eq!(terminal.reason_code, "worker.failed");
      assert!(terminal.result.is_none());
    });
  oversized_server.shutdown();
  fs::write(&library, &library_bytes).unwrap();
  repin(
    &package,
    &library,
    &selection,
    &leaf.join("manifest.json"),
    &mut receipt,
    &mut selection_json,
    &mut manifest,
  );
  assert!(Catalog::admit(
    &graph,
    Some(&managed),
    None,
    Path::new(env!("CARGO_BIN_EXE_aether-runtime"))
  )
  .is_err());
  let unrelated = package.join("lib/unrelated.txt");
  fs::write(&unrelated, b"not a skill library").unwrap();
  fs::set_permissions(&unrelated, fs::Permissions::from_mode(0o666)).unwrap();
  let unsafe_file = Catalog::admit(
    &graph,
    Some(&managed),
    Some(&selection),
    Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
  )
  .err()
  .unwrap();
  assert_eq!(unsafe_file.code, "package_file_unsafe");
  fs::remove_file(&unrelated).unwrap();
  selection_json["libraries"][0]["library"] = "lib/fixture.so".into();
  let wrong_suffix_bytes = serde_json::to_vec(&selection_json).unwrap();
  private(&selection, &wrong_suffix_bytes);
  manifest["files"][1]["sha256"] = digest(&wrong_suffix_bytes).into();
  private(
    &leaf.join("manifest.json"),
    &serde_json::to_vec(&manifest).unwrap(),
  );
  let wrong_suffix = Catalog::admit(
    &graph,
    Some(&managed),
    Some(&selection),
    Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
  )
  .err()
  .unwrap();
  assert_eq!(wrong_suffix.code, "selection_invalid");
  selection_json["libraries"][0]["library"] = format!("lib/{}.skill.so", "a".repeat(250)).into();
  let long_path_bytes = serde_json::to_vec(&selection_json).unwrap();
  private(&selection, &long_path_bytes);
  manifest["files"][1]["sha256"] = digest(&long_path_bytes).into();
  private(
    &leaf.join("manifest.json"),
    &serde_json::to_vec(&manifest).unwrap(),
  );
  let long_path = Catalog::admit(
    &graph,
    Some(&managed),
    Some(&selection),
    Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
  )
  .err()
  .unwrap();
  assert_eq!(long_path.code, "selection_invalid");
  selection_json["libraries"][0]["library"] = "lib/fixture.skill.so".into();
  private(&selection, &selection_bytes);
  manifest["files"][1]["sha256"] = digest(&selection_bytes).into();
  private(
    &leaf.join("manifest.json"),
    &serde_json::to_vec(&manifest).unwrap(),
  );
  fs::write(&library, b"tampered package bytes").unwrap();
  assert!(Catalog::admit(
    &graph,
    Some(&managed),
    Some(&selection),
    Path::new(env!("CARGO_BIN_EXE_aether-runtime"))
  )
  .is_err());
  let mut staged = catalog
    .start_worker(
      Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
      "fixture.old",
      "1.0.0",
      "request-2",
      "{}",
    )
    .unwrap();
  assert!(matches!(
    staged.recv(std::time::Duration::from_secs(5)).unwrap(),
    aether_runtime::host_skills::WorkerEvent::Completion { accepted: true, .. }
  ));
  assert!(staged.reap(std::time::Duration::from_secs(1)).unwrap());
  for (define, expected) in [
    (r#"-DSKILL_NAME="Bad/Name""#, "worker_inspection_failed"),
    (r#"-DSKILL_VERSION="abc""#, "worker_inspection_failed"),
    ("-DBAD_SCHEMA", "worker_inspection_failed"),
    ("-DFORGE_CATALOG", "abi_descriptor_invalid"),
  ] {
    let invalid = Command::new("cc")
      .args(["-shared", "-fPIC", define, "-I"])
      .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../src/eos/include"))
      .arg(&source)
      .arg("-o")
      .arg(&library)
      .output()
      .unwrap();
    assert!(
      invalid.status.success(),
      "{}",
      String::from_utf8_lossy(&invalid.stderr)
    );
    fs::set_permissions(&library, fs::Permissions::from_mode(0o755)).unwrap();
    repin(
      &package,
      &library,
      &selection,
      &leaf.join("manifest.json"),
      &mut receipt,
      &mut selection_json,
      &mut manifest,
    );
    let error = Catalog::admit(
      &graph,
      Some(&managed),
      Some(&selection),
      Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
    )
    .err()
    .unwrap();
    assert_eq!(error.code, expected, "fixture define: {define}");
  }
  let blocked = Command::new("cc")
    .args(["-shared", "-fPIC", "-DBLOCK_EXECUTE", "-I"])
    .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../src/eos/include"))
    .arg(&source)
    .arg("-o")
    .arg(&library)
    .output()
    .unwrap();
  assert!(
    blocked.status.success(),
    "{}",
    String::from_utf8_lossy(&blocked.stderr)
  );
  fs::set_permissions(&library, fs::Permissions::from_mode(0o755)).unwrap();
  repin(
    &package,
    &library,
    &selection,
    &leaf.join("manifest.json"),
    &mut receipt,
    &mut selection_json,
    &mut manifest,
  );
  let blocked_catalog = Catalog::admit(
    &graph,
    Some(&managed),
    Some(&selection),
    Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
  )
  .unwrap();
  let mut blocked_session = blocked_catalog
    .start_worker(
      Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
      "fixture.old",
      "1.0.0",
      "blocked",
      "{}",
    )
    .unwrap();
  let cancel_handle = blocked_session.cancellation_handle();
  let receiver = std::thread::spawn(move || {
    let event = blocked_session.recv(std::time::Duration::from_secs(5));
    let reaped = blocked_session.reap(std::time::Duration::from_secs(1));
    (event, reaped)
  });
  std::thread::sleep(std::time::Duration::from_millis(50));
  cancel_handle.terminate().unwrap();
  let (event, reaped) = receiver.join().unwrap();
  assert!(event.is_err());
  assert!(reaped.unwrap());
  let doubled = Command::new("cc")
    .args(["-shared", "-fPIC", "-DDOUBLE_COMPLETE", "-I"])
    .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../src/eos/include"))
    .arg(&source)
    .arg("-o")
    .arg(&library)
    .output()
    .unwrap();
  assert!(
    doubled.status.success(),
    "{}",
    String::from_utf8_lossy(&doubled.stderr)
  );
  fs::set_permissions(&library, fs::Permissions::from_mode(0o755)).unwrap();
  repin(
    &package,
    &library,
    &selection,
    &leaf.join("manifest.json"),
    &mut receipt,
    &mut selection_json,
    &mut manifest,
  );
  let doubled_catalog = Catalog::admit(
    &graph,
    Some(&managed),
    Some(&selection),
    Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
  )
  .unwrap();
  let mut doubled_session = doubled_catalog
    .start_worker(
      Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
      "fixture.old",
      "1.0.0",
      "double",
      "{}",
    )
    .unwrap();
  assert!(matches!(
    doubled_session.recv(std::time::Duration::from_secs(5)).unwrap(),
    aether_runtime::host_skills::WorkerEvent::Failure { code }
      if code == "worker_duplicate_completion"
  ));
  assert!(doubled_session
    .reap(std::time::Duration::from_secs(1))
    .unwrap());
  let rejected = Command::new("cc")
    .args(["-shared", "-fPIC", "-DCLAIM_COUNT=1", "-I"])
    .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../src/eos/include"))
    .arg(&source)
    .arg("-o")
    .arg(&library)
    .output()
    .unwrap();
  assert!(
    rejected.status.success(),
    "{}",
    String::from_utf8_lossy(&rejected.stderr)
  );
  fs::set_permissions(&library, fs::Permissions::from_mode(0o755)).unwrap();
  let next_payload = payload_digest(&package, &library);
  let mut next_receipt = receipt;
  next_receipt["payload_sha256"] = next_payload.clone().into();
  let next_receipt_bytes = serde_json::to_vec(&next_receipt).unwrap();
  private(&package.join("receipt.json"), &next_receipt_bytes);
  selection_json["libraries"][0]["payload_sha256"] = next_payload.into();
  selection_json["libraries"][0]["receipt_sha256"] = digest(&next_receipt_bytes).into();
  selection_json["libraries"][0]["library_sha256"] = digest(&fs::read(&library).unwrap()).into();
  let next_selection_bytes = serde_json::to_vec(&selection_json).unwrap();
  private(&selection, &next_selection_bytes);
  manifest["files"][1]["sha256"] = digest(&next_selection_bytes).into();
  private(
    &leaf.join("manifest.json"),
    &serde_json::to_vec(&manifest).unwrap(),
  );
  let error = Catalog::admit(
    &graph,
    Some(&managed),
    Some(&selection),
    Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
  )
  .err()
  .unwrap();
  assert_eq!(error.code, "worker_inspection_failed");
  let unsafe_elf = Command::new("cc")
    .args(["-shared", "-fPIC", "-Wl,-rpath,/tmp", "-I"])
    .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../src/eos/include"))
    .arg(&source)
    .arg("-o")
    .arg(&library)
    .output()
    .unwrap();
  assert!(
    unsafe_elf.status.success(),
    "{}",
    String::from_utf8_lossy(&unsafe_elf.stderr)
  );
  fs::set_permissions(&library, fs::Permissions::from_mode(0o755)).unwrap();
  let next_payload = payload_digest(&package, &library);
  next_receipt["payload_sha256"] = next_payload.clone().into();
  let next_receipt_bytes = serde_json::to_vec(&next_receipt).unwrap();
  private(&package.join("receipt.json"), &next_receipt_bytes);
  selection_json["libraries"][0]["payload_sha256"] = next_payload.into();
  selection_json["libraries"][0]["receipt_sha256"] = digest(&next_receipt_bytes).into();
  selection_json["libraries"][0]["library_sha256"] = digest(&fs::read(&library).unwrap()).into();
  let next_selection_bytes = serde_json::to_vec(&selection_json).unwrap();
  private(&selection, &next_selection_bytes);
  manifest["files"][1]["sha256"] = digest(&next_selection_bytes).into();
  private(
    &leaf.join("manifest.json"),
    &serde_json::to_vec(&manifest).unwrap(),
  );
  let error = Catalog::admit(
    &graph,
    Some(&managed),
    Some(&selection),
    Path::new(env!("CARGO_BIN_EXE_aether-runtime")),
  )
  .err()
  .unwrap();
  assert_eq!(error.code, "library_dependency_unsafe");
  let blocked_query = Command::new("cc")
    .args(["-shared", "-fPIC", "-DBLOCK_QUERY", "-I"])
    .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../src/eos/include"))
    .arg(&source)
    .arg("-o")
    .arg(&library)
    .output()
    .unwrap();
  assert!(
    blocked_query.status.success(),
    "{}",
    String::from_utf8_lossy(&blocked_query.stderr)
  );
  fs::set_permissions(&library, fs::Permissions::from_mode(0o755)).unwrap();
  repin(
    &package,
    &library,
    &selection,
    &leaf.join("manifest.json"),
    &mut next_receipt,
    &mut selection_json,
    &mut manifest,
  );
  let started = std::time::Instant::now();
  let timed_out = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
    .args(["--skill-catalog-preflight", "--profile"])
    .arg(&graph)
    .args(["--run-id", "run-1"])
    .output()
    .unwrap();
  assert!(!timed_out.status.success());
  assert!(started.elapsed() < std::time::Duration::from_secs(10));
  assert_eq!(
    serde_json::from_slice::<serde_json::Value>(&timed_out.stdout).unwrap(),
    serde_json::json!({"schema_version":1,"ok":false,"stage":"worker","code":"worker_timeout"})
  );
  std::env::remove_var("HOST_SKILL_TEST_SECRET");
  std::env::remove_var("EOS_CONTROL_GRANTS_FILE");
  std::env::remove_var("EOS_SKILL_SELECTION_FILE");
}
