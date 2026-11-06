//! EOS v2 process boundary. Every control request is one bounded JSON line.

use std::env;
use std::collections::HashMap;
use std::io::{Read, Write};
use std::os::unix::fs::FileTypeExt;
use std::os::unix::fs::PermissionsExt;
use std::os::unix::fs::MetadataExt;
use std::os::fd::AsRawFd;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use aether_common::io::read_bounded;
use aether_common::events::{
  Event, EventSink, NonblockingStderr, Reason, Source, Stage, local_run_id, valid_digest,
  valid_run_id,
};
use aether_common::time::{Clock, SystemClock};
use aether_dispatcher::{
  AdmissionContext, AdmissionGeneration, AdmissionIngress, AuthorityRole, CommandRequest, Movement,
};
use aether_hal::{
  is_eos_backend_type, BaseMovement, CommandMeta, EndEffectorPose, ManipulatorMovement, NodeKind,
  NodeTypeRegistry, PortTransport, Profile,
};
use aether_runtime::{
  adapter_library::LibrarySpec, grpc, host_skills, managed_admission, Runtime, MAX_PROFILE_BYTES,
};
use base64::engine::general_purpose::STANDARD;
use base64::Engine;
use serde_json::{json, Value};
use serde::Deserialize;
use sha2::{Digest, Sha256};

#[path = "aether-runtime/backend_child.rs"]
mod backend_child;
#[path = "aether-runtime/skills.rs"]
mod skills;
use backend_child::{BackendChild, BackendLaunch, BackendStopReport};
use skills::SkillManager;
#[path = "aether-runtime/vision.rs"]
mod vision;
use vision::VisionService;
#[path = "aether-runtime/routes.rs"]
mod routes;
#[path = "aether-runtime/preflight.rs"]
mod preflight;

const MAX_REQUEST_BYTES: usize = 64 * 1024;
const MAX_BROWSER_RESULT_BYTES: usize = 4 * 1024;
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct PrivateResultRequest {
  op: String,
  command_id: String,
  active_revision: String,
  authority_grant: String,
}

fn valid_private_result_request(bytes: &[u8]) -> bool {
  let Ok(request) = serde_json::from_slice::<PrivateResultRequest>(bytes) else {
    return false;
  };
  request.op == "primitive_result"
    && !request.command_id.is_empty()
    && request.command_id.len() <= 128
    && valid_digest(&request.active_revision)
    && valid_digest(&request.authority_grant)
}
const MAX_CLIENTS: usize = 16;
static TERMINATE: AtomicBool = AtomicBool::new(false);
static SHUTDOWN_CORRELATION: Mutex<Option<String>> = Mutex::new(None);

fn shutdown_correlation() -> Option<String> {
  SHUTDOWN_CORRELATION
    .lock()
    .ok()
    .and_then(|value| value.clone())
}

extern "C" fn on_signal(_: libc::c_int) {
  TERMINATE.store(true, Ordering::SeqCst);
}

struct Options {
  profile: PathBuf,
  journal_dir: PathBuf,
  socket: PathBuf,
  backend: Option<BackendLaunch>,
  node_types: Vec<PathBuf>,
  adapter_libraries: Vec<LibrarySpec>,
  run_id: String,
  backend_release_sha256: Option<String>,
  backend_payload_sha256: Option<String>,
  grpc_port: u16,
}

#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct PackageProcessSpec {
  contract: String,
  schema_version: String,
  owner_core: String,
  run_id: String,
  graph_revision: String,
  node_path: String,
  node_type: String,
  package: PackageIdentity,
  entrypoint: PathBuf,
  resources: HashMap<String, PackageResource>,
  lifecycle_adapter: String,
  release_sha256: String,
  payload_sha256: String,
}

#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct PackageResource {
  kind: String,
  id: String,
  path: Option<PathBuf>,
  sha256: Option<String>,
  metadata: Option<serde_json::Value>,
}

#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct PackageIdentity {
  id: String,
  version: String,
}

fn read_package_process(path: &Path) -> Result<PackageProcessSpec, String> {
  if !path.is_absolute() {
    return Err("--package-process path must be absolute".into());
  }
  let bytes = read_bounded(path, 64 * 1024).map_err(|error| error.to_string())?;
  let spec: PackageProcessSpec =
    serde_json::from_slice(&bytes).map_err(|error| error.to_string())?;
  if spec.contract != "isos.package-process@v1"
    || spec.schema_version != "0.1.0"
    || spec.owner_core != "eos"
    || !valid_run_id(&spec.run_id)
    || !valid_digest(&spec.graph_revision)
    || spec.node_path.is_empty()
    || spec.node_type.is_empty()
    || spec.package.id.is_empty()
    || spec.package.version.is_empty()
    || !spec.entrypoint.is_absolute()
    || spec.resources.is_empty()
    || spec.resources.len() > 32
    || spec.resources.iter().any(|(name, resource)| {
      name.is_empty()
        || name.len() > 64
        || !name
          .bytes()
          .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-'))
        || resource.kind.is_empty()
        || resource.id.is_empty()
        || resource
          .path
          .as_ref()
          .is_some_and(|path| !path.is_absolute())
        || resource
          .sha256
          .as_ref()
          .is_some_and(|digest| !valid_digest(digest))
        || resource
          .metadata
          .as_ref()
          .is_some_and(|value| !value.is_object())
    })
    || !valid_digest(&spec.release_sha256)
    || !valid_digest(&spec.payload_sha256)
  {
    return Err("invalid package process handoff".into());
  }
  if spec.lifecycle_adapter != "mujoco-v2"
    || spec.resources.len() != 1
    || spec
      .resources
      .get("scene")
      .and_then(|resource| resource.path.as_ref())
      .is_none()
  {
    return Err("package lifecycle adapter or resources are unsupported".into());
  }
  Ok(spec)
}

impl Options {
  fn parse() -> Result<Self, String> {
    let mut profile = None;
    let mut journal_dir = None;
    let mut socket = None;
    let mut backend_node = None;
    let mut package_process = None;
    let mut backend_executable = None;
    let mut backend_scene = None;
    let mut node_types = Vec::new();
    let mut adapter_libraries = Vec::new();
    let mut log_dir = None;
    let mut run_id = None;
    let mut backend_release_sha256 = None;
    let mut backend_payload_sha256 = None;
    let mut grpc_port = None;
    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
      if arg == "--help" {
        println!(
                    "usage: aether-runtime --profile <v2.toml> --journal-dir <dir> --socket <unix-socket> [--grpc-port <loopback-port>] [--run-id <id>] [--node-types <verified-json>]... [--adapter-library <family>=<absolute-file>@<sha256>]... [--log-dir <absolute-dir>] [--backend-node <graph-path> --backend-executable <file> --backend-scene <file> [--backend-release-sha256 <digest> --backend-payload-sha256 <digest>]]"
                );
        std::process::exit(0);
      }
      let value = args
        .next()
        .ok_or_else(|| format!("missing value for {arg}"))?;
      if arg == "--backend-node" {
        if backend_node.replace(value).is_some() {
          return Err("duplicate option: --backend-node".into());
        }
        continue;
      }
      if arg == "--package-process" {
        if package_process.replace(PathBuf::from(value)).is_some() {
          return Err("duplicate option: --package-process".into());
        }
        continue;
      }
      if arg == "--run-id" {
        if !valid_run_id(&value) || run_id.replace(value).is_some() {
          return Err("invalid or duplicate --run-id".into());
        }
        continue;
      }
      if arg == "--backend-release-sha256" {
        if !valid_digest(&value) || backend_release_sha256.replace(value).is_some() {
          return Err("invalid or duplicate --backend-release-sha256".into());
        }
        continue;
      }
      if arg == "--backend-payload-sha256" {
        if !valid_digest(&value) || backend_payload_sha256.replace(value).is_some() {
          return Err("invalid or duplicate --backend-payload-sha256".into());
        }
        continue;
      }
      if arg == "--node-types" {
        node_types.push(PathBuf::from(value));
        continue;
      }
      if arg == "--grpc-port" {
        let port = value.parse::<u16>().map_err(|_| "invalid --grpc-port")?;
        if grpc_port.replace(port).is_some() {
          return Err("duplicate option: --grpc-port".into());
        }
        continue;
      }
      if arg == "--adapter-library" {
        adapter_libraries.push(LibrarySpec::parse(&value)?);
        continue;
      }
      let target = match arg.as_str() {
        "--profile" => &mut profile,
        "--journal-dir" => &mut journal_dir,
        "--socket" => &mut socket,
        "--backend-executable" => &mut backend_executable,
        "--backend-scene" => &mut backend_scene,
        "--log-dir" => &mut log_dir,
        _ => return Err(format!("unknown option: {arg}")),
      };
      if target.replace(PathBuf::from(value)).is_some() {
        return Err(format!("duplicate option: {arg}"));
      }
    }
    if log_dir.as_ref().is_some_and(|path| !path.is_absolute()) {
      return Err("--log-dir must be absolute".into());
    }
    let mut backend = match (backend_node, backend_executable, backend_scene) {
      (None, None, None) => None,
      (Some(node), Some(executable), Some(scene)) => Some(BackendLaunch {
        node,
        executable,
        scene,
        log_path: log_dir.as_ref().map(|dir| dir.join("mujoco.log")),
        package: None,
        lifecycle_adapter: None,
        node_type: None,
      }),
      _ => return Err("backend launch requires node, executable, and scene".into()),
    };
    if let Some(path) = package_process {
      if backend.is_some() || backend_release_sha256.is_some() || backend_payload_sha256.is_some() {
        return Err("package process cannot be combined with legacy backend options".into());
      }
      let spec = read_package_process(&path)?;
      if run_id.as_deref() != Some(&spec.run_id) {
        return Err("package process run identity differs".into());
      }
      backend_release_sha256 = Some(spec.release_sha256);
      backend_payload_sha256 = Some(spec.payload_sha256);
      backend = Some(BackendLaunch {
        node: spec.node_path,
        executable: spec.entrypoint,
        scene: spec
          .resources
          .get("scene")
          .and_then(|resource| resource.path.clone())
          .ok_or("missing scene resource")?,
        log_path: log_dir.as_ref().map(|dir| dir.join("package-process.log")),
        package: Some((spec.package.id, spec.package.version)),
        lifecycle_adapter: Some(spec.lifecycle_adapter),
        node_type: Some(spec.node_type),
      });
    }
    if backend_release_sha256.is_some() != backend_payload_sha256.is_some()
      || (backend.is_none() && backend_release_sha256.is_some())
    {
      return Err("backend digests require a backend launch and must be paired".into());
    }
    Ok(Self {
      profile: profile.ok_or("--profile is required")?,
      journal_dir: journal_dir.ok_or("--journal-dir is required")?,
      socket: socket.ok_or("--socket is required")?,
      backend,
      node_types,
      adapter_libraries,
      run_id: run_id.unwrap_or_else(local_run_id),
      backend_release_sha256,
      backend_payload_sha256,
      grpc_port: grpc_port.unwrap_or(16_200),
    })
  }
}

struct SocketGuard(PathBuf);
impl Drop for SocketGuard {
  fn drop(&mut self) {
    let _ = std::fs::remove_file(&self.0);
  }
}

type Sink = EventSink<NonblockingStderr, SystemClock>;

struct StopGuard<'a> {
  runtime: &'a Runtime,
  sink: &'a Sink,
  managed: bool,
  stopped: bool,
  shutdown_marker_persisted: bool,
  results: Vec<(String, Result<(), String>)>,
}
impl StopGuard<'_> {
  fn stop(&mut self) -> &[(String, Result<(), String>)] {
    if self.stopped {
      return &self.results;
    }
    self.stopped = true;
    let correlation = shutdown_correlation();
    let _ = self
      .sink
      .emit_correlated(Event::Stopping, None, correlation.as_deref());
    if self.managed {
      if let Some((results, persisted)) = self.runtime.dispatcher().managed_shutdown_stop() {
        self.results = results;
        self.shutdown_marker_persisted = persisted;
      } else {
        self.results = self.runtime.dispatcher().emergency_stop();
      }
    } else {
      self.results = self.runtime.dispatcher().emergency_stop();
    }
    for (path, result) in &self.results {
      if result.is_err() {
        let _ =
          self
            .sink
            .emit_correlated(Event::StopUnconfirmed, Some(path), correlation.as_deref());
      }
    }
    &self.results
  }
}
impl Drop for StopGuard<'_> {
  fn drop(&mut self) {
    self.stop();
  }
}

fn write_terminal_receipt(value: &Value, deadline: Instant) -> std::io::Result<()> {
  let mut bytes = serde_json::to_vec(value)?;
  bytes.push(b'\n');
  if bytes.len() > 4096 {
    return Err(std::io::Error::new(
      std::io::ErrorKind::InvalidData,
      "terminal receipt exceeds PIPE_BUF",
    ));
  }
  let fd = libc::STDOUT_FILENO;
  unsafe {
    libc::signal(libc::SIGPIPE, libc::SIG_IGN);
  }
  let old_flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
  if old_flags < 0 || unsafe { libc::fcntl(fd, libc::F_SETFL, old_flags | libc::O_NONBLOCK) } < 0 {
    return Err(std::io::Error::last_os_error());
  }
  while Instant::now() < deadline {
    let remaining = deadline
      .saturating_duration_since(Instant::now())
      .as_millis()
      .clamp(1, i32::MAX as u128) as i32;
    let mut poll = libc::pollfd {
      fd,
      events: libc::POLLOUT,
      revents: 0,
    };
    let ready = unsafe { libc::poll(&mut poll, 1, remaining) };
    if ready > 0 {
      break;
    }
    if ready < 0 && std::io::Error::last_os_error().kind() != std::io::ErrorKind::Interrupted {
      return Err(std::io::Error::last_os_error());
    }
  }
  if Instant::now() >= deadline {
    return Err(std::io::Error::new(
      std::io::ErrorKind::TimedOut,
      "terminal receipt deadline",
    ));
  }
  let written = unsafe { libc::write(fd, bytes.as_ptr().cast(), bytes.len()) };
  if written != bytes.len() as isize {
    return Err(if written < 0 {
      std::io::Error::last_os_error()
    } else {
      std::io::Error::new(std::io::ErrorKind::WriteZero, "short terminal receipt")
    });
  }
  Ok(())
}

struct TerminalReceiptInput<'a> {
  options: &'a Options,
  active_revision: &'a str,
  profile: &'a Profile,
  verified_generation: Option<&'a str>,
  proof: Option<&'a aether_hal::RobotStopProof>,
  report: &'a BackendStopReport,
  stop_results: &'a [(String, Result<(), String>)],
  control_socket_removed: bool,
  other_shutdown_ok: bool,
}

fn terminal_receipt(input: TerminalReceiptInput<'_>) -> Value {
  let TerminalReceiptInput {
    options,
    active_revision,
    profile,
    verified_generation,
    proof,
    report,
    stop_results,
    control_socket_removed,
    other_shutdown_ok,
  } = input;
  let proof = proof.filter(|proof| verified_generation == Some(proof.generation_id.as_str()));
  let backend_path = options
    .backend
    .as_ref()
    .and_then(|backend| aether_hal::canonical_node_path(&backend.node));
  let mut parts: Vec<Value> = profile
    .hardware_nodes()
    .filter(|node| {
      node.movement.is_some()
        && backend_path.as_deref().is_some_and(|path| {
          node
            .adapter
            .settings
            .get("backend")
            .and_then(|value| value.as_str())
            .and_then(aether_hal::canonical_node_path)
            .as_deref()
            == Some(path)
            && node.references.iter().any(|reference| reference == path)
        })
    })
    .map(|node| {
      let outcome = if stop_results
        .iter()
        .any(|(path, result)| path == node.path() && result.is_ok())
      {
        "stopped"
      } else {
        "unknown"
      };
      let kind = match node.kind {
        NodeKind::Base => "base",
        NodeKind::Manipulator => "manipulator",
        _ => "unknown",
      };
      json!({"node_path":node.path(),"kind":kind,"outcome":outcome})
    })
    .collect();
  parts.sort_by(|a, b| a["node_path"].as_str().cmp(&b["node_path"].as_str()));
  let dual = proof.is_some_and(|proof| {
    proof.base == aether_hal::ControlledStopOutcome::Stopped
      && proof.arm == aether_hal::ControlledStopOutcome::Stopped
      && proof.generation_id.len() == 32
      && proof
        .generation_id
        .bytes()
        .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
      && proof.stop_epoch.as_deref().is_some_and(|epoch| {
        epoch
          .parse::<u64>()
          .ok()
          .is_some_and(|value| value > 0 && value.to_string() == epoch)
      })
  });
  let stop_ok =
    !parts.is_empty() && parts.len() <= 64 && parts.iter().all(|part| part["outcome"] == "stopped");
  let sockets_removed = report.sockets_removed && control_socket_removed;
  let confirmed = dual
    && stop_ok
    && other_shutdown_ok
    && report.group_drained
    && report.reaped
    && report.backend_pid > 0
    && report.backend_pgid == report.backend_pid as i32
    && sockets_removed
    && !report.forced_kill
    && report.reason.is_none();
  let reason_code = if confirmed {
    Value::Null
  } else {
    json!(report.reason.unwrap_or(if !sockets_removed {
      "socket_cleanup_failed"
    } else if !dual || !stop_ok {
      "stop_part_unknown"
    } else {
      "stop_timeout"
    }))
  };
  json!({
    "schema_version":1,"type":"eos_terminal_stop","run_id":options.run_id,
    "eos_pid":std::process::id(),"active_revision":active_revision,
    "backend_node":options.backend.as_ref().map(|launch| launch.node.as_str()).unwrap_or(""),
    "backend_pid":report.backend_pid,"backend_pgid":report.backend_pgid,
    "generation_id":verified_generation,
    "stop_epoch":proof.and_then(|proof| proof.stop_epoch.as_deref()),
    "admission_closed":true,"parts":parts,"backend_group_drained":report.group_drained,
    "backend_reaped":report.reaped,"sockets_removed":sockets_removed,
    "outcome":if confirmed { "confirmed" } else { "unknown" },"reason_code":reason_code,
  })
}

fn runtime_capabilities() -> Value {
  json!({"product_version":env!("CARGO_PKG_VERSION"),"graph_version":aether_hal::GRAPH_VERSION,
        "node_type_version":"1.0","supported_graph_versions":["1.0.0","0.1.0","0.2.0"],
        "supported_node_type_versions":["1.0","0.1","0.2"],
        "supported_node_types":aether_hal::builtin_reader_node_types(),
        "binary":"aether-runtime",
        "features":["short-paths","node-types","data-idl","primitive-binding","log-dir","vision-gate","vision-provenance-v1","graph-0.1-reader","mujoco-child-v2","package-process-v1","managed-control-grants","host-skill-abi-v2","managed-stop-v1"]})
}

fn main() {
  let args: Vec<String> = std::env::args().skip(1).collect();
  if args.first().is_some_and(|arg| arg == "--adapter-host") {
    if let Err(error) = aether_runtime::adapter_host::run_host(&args) {
      eprintln!("aether-adapter-host: {error}");
      std::process::exit(1);
    }
    return;
  }
  if args.as_slice() == ["--skill-worker"] {
    std::process::exit(host_skills::worker_main());
  }
  if args
    .first()
    .is_some_and(|arg| arg == "--skill-catalog-preflight")
  {
    std::process::exit(preflight::main(&args[1..]));
  }
  if std::env::args().nth(1).as_deref() == Some("--capabilities") {
    println!("{}", runtime_capabilities());
    return;
  }
  let options = match Options::parse() {
    Ok(options) => options,
    Err(_) => {
      eprintln!("aether-runtime: invalid launch arguments");
      std::process::exit(1);
    }
  };
  let sink = Arc::new(
    Sink::new(
      Source::Eos,
      options.run_id.clone(),
      NonblockingStderr::new().unwrap_or_else(|_| {
        eprintln!("aether-runtime: event stream unavailable");
        std::process::exit(1);
      }),
      SystemClock,
    )
    .expect("validated run ID"),
  );
  let _ = sink.emit(Event::Starting, None);
  let mut stage = Stage::Profile;
  let mut ready = false;
  if run(&options, &sink, &mut stage, &mut ready).is_err() {
    if !ready {
      let reason = match stage {
        Stage::Profile => Reason::ProfileInvalid,
        Stage::Registry => Reason::RegistryInvalid,
        Stage::Adapter => Reason::AdapterUnavailable,
        Stage::Journal => Reason::JournalUnavailable,
        Stage::Backend => Reason::BackendStartFailed,
        Stage::Listener => Reason::ListenerBindFailed,
        _ => Reason::OtherFailure,
      };
      let backend_node = (stage == Stage::Backend)
        .then(|| options.backend.as_ref().map(|launch| launch.node.as_str()))
        .flatten();
      let _ = sink.emit(Event::StartFailed { stage, reason }, backend_node);
    }
    std::process::exit(1);
  }
}

fn run(
  options: &Options,
  sink: &Arc<Sink>,
  stage: &mut Stage,
  ready: &mut bool,
) -> Result<(), Box<dyn std::error::Error>> {
  let boot = Instant::now();
  let bytes = read_bounded(&options.profile, MAX_PROFILE_BYTES)?;
  let verifier = env::var_os("EOS_CONTROL_GRANTS_FILE").map(PathBuf::from);
  let managed = managed_admission::load(
    &options.profile,
    &bytes,
    verifier.as_deref(),
    &options.run_id,
  )?
  .map(Arc::new);
  let source = std::str::from_utf8(&bytes)?;
  *stage = Stage::Registry;
  let registry = Arc::new(NodeTypeRegistry::with_contribution_files(
    &options.node_types,
  )?);
  *stage = Stage::Profile;
  let profile = if source.trim_start().starts_with('{') {
    Profile::from_json_with_registry(source, &registry)?
  } else {
    Profile::from_toml_with_registry(source, &registry)?
  };
  let has_backend = profile
    .all_nodes()
    .any(|node| is_eos_backend_type(&node.node_type));
  if profile
    .hardware_nodes()
    .any(|node| node.adapter.adapter_type == "mujoco-v2")
    && !profile.all_nodes().any(|node| {
      matches!(
        node.node_type.as_str(),
        "eos.process.mujoco@v1.0" | "eos.process.mujoco@v0.1" | "isos.eos_process.mujoco@v0.2"
      ) || (node.node_type == "eos.process.backend@v1.0"
        && node
          .settings
          .get("package")
          .and_then(|value| value.as_str())
          == Some("island/mujoco-sim")
        && node
          .settings
          .get("version")
          .and_then(|value| value.as_str())
          == Some("0.1.1"))
    })
  {
    return Err("package hardware requires its EOS MuJoCo backend".into());
  }
  if has_backend != options.backend.is_some() {
    return Err("EOS backend graph and launch arguments disagree".into());
  }
  for spec in &options.adapter_libraries {
    *stage = Stage::Adapter;
    if matches!(spec.family.as_str(), "mock-cxx" | "mujoco-v2")
      || registry.package_for_adapter_family(&spec.family).is_none()
      || registry
        .adapter_family(&spec.family)
        .is_none_or(|family| family.transport != PortTransport::InProcess)
    {
      return Err(
        format!(
          "{} is not an in-process package adapter family",
          spec.family
        )
        .into(),
      );
    }
  }
  let skill_selection = env::var_os("EOS_SKILL_SELECTION_FILE").map(PathBuf::from);
  let host_catalog = Arc::new(
    host_skills::Catalog::admit(
      &options.profile,
      managed.as_deref(),
      skill_selection.as_deref(),
      &std::env::current_exe()?,
    )
    .inspect_err(|error| {
      if error.code == "worker_reap_failed" {
        let _ = sink.emit(Event::SkillStopUnconfirmed, None);
      }
    })?,
  );
  *stage = Stage::Journal;
  let runtime = Arc::new(match &managed {
    Some(managed) => Runtime::from_profile_adapters_durable_managed_with_hosts(
      profile.clone(),
      Arc::new(SystemClock),
      &options.journal_dir,
      &options.adapter_libraries,
      &std::env::current_exe()?,
      AdmissionGeneration {
        run_id: managed.run_id.clone(),
        active_revision: managed.active_revision.clone(),
        bindings: managed_admission::admitted_bindings(&profile),
      },
    )?,
    None => Runtime::from_profile_adapters_durable_with_hosts(
      profile,
      Arc::new(SystemClock),
      &options.journal_dir,
      &options.adapter_libraries,
      &std::env::current_exe()?,
    )?,
  });
  let skills = SkillManager::new(runtime.clone(), Arc::new(SystemClock));
  // Declare the simulator guard before StopGuard so physical stop is sent
  // while the simulator is still alive. EOS owns this direct child.
  *stage = Stage::Backend;
  let mut backend = options
    .backend
    .as_ref()
    .map(|launch| BackendChild::start(runtime.profile(), launch, managed.is_some()))
    .transpose()?;
  if let Some(launch) = &options.backend {
    let event = match (
      managed.as_ref(),
      &options.backend_release_sha256,
      &options.backend_payload_sha256,
    ) {
      (Some(_), Some(release_sha256), Some(payload_sha256)) => Event::PackageAdmitted {
        release_sha256,
        payload_sha256,
      },
      _ => Event::PackageProvenanceUnavailable,
    };
    let _ = sink.emit(event, Some(&launch.node));
    let _ = sink.emit(Event::BackendReady, Some(&launch.node));
  }
  // A killed owner cannot unlink its socket. The journal lock above ensures
  // that only one EOS owner can recover this path at a time.
  if let Ok(metadata) = std::fs::symlink_metadata(&options.socket) {
    if !metadata.file_type().is_socket() {
      return Err(
        format!(
          "socket path is not a Unix socket: {}",
          options.socket.display()
        )
        .into(),
      );
    }
    match UnixStream::connect(&options.socket) {
      Ok(_) => return Err("socket is already served by a live process".into()),
      Err(error) if error.kind() == std::io::ErrorKind::ConnectionRefused => {
        std::fs::remove_file(&options.socket)?;
      }
      Err(error) => return Err(error.into()),
    }
  }
  // Binding happens after all adapters and the journal have been admitted.
  *stage = Stage::Listener;
  if managed.is_some() {
    let parent = options.socket.parent().ok_or("socket has no parent")?;
    let metadata = std::fs::symlink_metadata(parent)?;
    if !metadata.file_type().is_dir()
      || metadata.permissions().mode() & 0o777 != 0o700
      || metadata.uid() != unsafe { libc::geteuid() }
    {
      return Err("managed socket parent is not owner-private".into());
    }
  }
  let listener = UnixListener::bind(&options.socket)?;
  let _socket_guard = SocketGuard(options.socket.clone());
  let mut stop_guard = StopGuard {
    runtime: &runtime,
    sink,
    managed: managed.is_some(),
    stopped: false,
    shutdown_marker_persisted: false,
    results: Vec::new(),
  };
  let camera_cache = Arc::new(grpc::CameraCache::default());
  let vision = Arc::new(VisionService::start(
    runtime.clone(),
    registry,
    graph_revision(&bytes),
    managed
      .as_ref()
      .map(|managed| (managed.run_id.clone(), managed.active_revision.clone())),
    boot,
    camera_cache.clone(),
  )?);
  let projection = grpc::Projection::with_camera_cache_managed(
    runtime.clone(),
    camera_cache,
    managed.clone(),
    graph_revision(&bytes),
  )?
  .with_host_skills(host_catalog, std::env::current_exe()?, Some(sink.clone()))?;
  let mut grpc_server = grpc::ServerHandle::start(projection.clone(), options.grpc_port)?;

  let mut clients: Vec<std::thread::JoinHandle<()>> = Vec::new();
  // Every exit after evidence workers start reaches this finalizer. Rust drops
  // locals in reverse declaration order, which would otherwise join evidence
  // before the actuator stop guard on an early error.
  let run_result = (|| -> Result<(), Box<dyn std::error::Error>> {
    std::fs::set_permissions(&options.socket, std::fs::Permissions::from_mode(0o600))?;
    listener.set_nonblocking(true)?;
    unsafe {
      if libc::signal(libc::SIGINT, on_signal as *const () as libc::sighandler_t) == libc::SIG_ERR {
        return Err(std::io::Error::last_os_error().into());
      }
      if libc::signal(libc::SIGTERM, on_signal as *const () as libc::sighandler_t) == libc::SIG_ERR
      {
        return Err(std::io::Error::last_os_error().into());
      }
    }
    println!(
      "{}",
      json!({"version":runtime.profile().version,"status":"ready","socket":options.socket,
                 "grpc_address":grpc_server.address().to_string(),
                 "hardware_nodes":runtime.discover().len()})
    );
    std::io::stdout().flush()?;
    *ready = true;
    let _ = sink.emit(
      Event::Ready {
        node_count: u32::try_from(runtime.profile().all_nodes().count()).unwrap_or(u32::MAX),
      },
      None,
    );
    while !TERMINATE.load(Ordering::SeqCst) {
      if let Some(child) = backend.as_mut() {
        if child.check_alive().is_err() {
          if let Some(launch) = &options.backend {
            let _ = sink.emit(Event::BackendLost, Some(&launch.node));
          }
          return Err("backend child exited".into());
        }
      }
      let mut pending = Vec::new();
      for client in clients.drain(..) {
        if client.is_finished() {
          let _ = client.join();
        } else {
          pending.push(client);
        }
      }
      clients = pending;
      match listener.accept() {
        Ok((mut stream, _)) => {
          if clients.len() >= MAX_CLIENTS {
            let _ = write_reply(&mut stream, json!({"ok":false,"error":"server busy"}));
            continue;
          }
          let runtime = runtime.clone();
          let skills = skills.clone();
          let vision = vision.clone();
          let managed = managed.clone();
          clients.push(std::thread::spawn(move || {
            let _ = serve(stream, &runtime, &skills, &vision, managed.as_deref());
          }));
        }
        Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
          std::thread::sleep(Duration::from_millis(10));
        }
        Err(error) => return Err(error.into()),
      }
    }
    Ok(())
  })();
  let stop_started = Instant::now();
  runtime.dispatcher().quiesce();
  grpc_server.begin_shutdown();
  let seal_result = vision.begin_shutdown();
  let stop_results = stop_guard.stop().to_vec();
  runtime.dispatcher().cancel_prepared_browser_after_stop();
  let stop_phase_ok = stop_started.elapsed() <= Duration::from_secs(4);
  let skill_stop = projection.shutdown_host_skills();
  if skill_stop.is_err() {
    let correlation = shutdown_correlation();
    let _ = sink.emit_correlated(Event::SkillStopUnconfirmed, None, correlation.as_deref());
  }
  let backend_report = backend
    .as_mut()
    .map(|child| child.stop_managed(stop_started + Duration::from_secs(10)));
  let control_socket_removed = match std::fs::remove_file(&options.socket) {
    Ok(()) => true,
    Err(error) if error.kind() == std::io::ErrorKind::NotFound => true,
    Err(_) => false,
  };
  grpc_server.shutdown();
  let evidence_stop = vision.shutdown();

  for client in clients {
    if client.is_finished() {
      let _ = client.join();
    }
    // An in-flight owner request cannot delay actuator stop or evidence shutdown.
  }
  if managed.is_none() {
    skills.shutdown();
  }
  let shutdown_marker_persisted = stop_guard.shutdown_marker_persisted;
  drop(stop_guard);
  if let (Some(managed), Some(child), Some(report)) =
    (&managed, backend.as_ref(), backend_report.as_ref())
  {
    let proof = child.session().last_stop();
    let verified_generation = child
      .session()
      .token()
      .map(|token| token.generation_id)
      .or_else(|| child.ready_generation().map(str::to_owned));
    let mut receipt = terminal_receipt(TerminalReceiptInput {
      options,
      active_revision: &managed.active_revision,
      profile: runtime.profile(),
      verified_generation: verified_generation.as_deref(),
      proof: proof.as_ref(),
      report,
      stop_results: &stop_results,
      control_socket_removed,
      other_shutdown_ok: shutdown_marker_persisted
        && skill_stop.is_ok()
        && run_result.is_ok()
        && seal_result.is_ok()
        && evidence_stop.is_ok()
        && stop_phase_ok
        && stop_started.elapsed() <= Duration::from_secs(13),
    });
    if receipt["outcome"] == "confirmed" && !runtime.dispatcher().resolve_managed_shutdown() {
      receipt["outcome"] = json!("unknown");
      receipt["reason_code"] = json!("stop_part_unknown");
    }
    let write = write_terminal_receipt(&receipt, stop_started + Duration::from_secs(13));
    let confirmed = receipt["outcome"] == "confirmed"
      && write.is_ok()
      && stop_started.elapsed() <= Duration::from_secs(15);
    drop(backend);
    if !confirmed {
      return Err("managed terminal stop unconfirmed".into());
    }
  } else {
    drop(backend);
  }
  skill_stop?;
  seal_result?;
  evidence_stop?;
  run_result?;
  let correlation = shutdown_correlation();
  let _ = sink.emit_correlated(Event::Stopped, None, correlation.as_deref());
  Ok(())
}

fn serve(
  mut stream: UnixStream,
  runtime: &Runtime,
  skills: &Arc<SkillManager>,
  vision: &VisionService,
  managed: Option<&managed_admission::ManagedAdmission>,
) -> Result<(), Box<dyn std::error::Error>> {
  let owner_peer = peer_is_owner(&stream);
  stream.set_read_timeout(Some(Duration::from_secs(2)))?;
  stream.set_write_timeout(Some(Duration::from_secs(2)))?;
  let mut input = Vec::new();
  let mut chunk = [0u8; 4096];
  loop {
    let n = stream.read(&mut chunk)?;
    if n == 0 {
      break;
    }
    input.extend_from_slice(&chunk[..n]);
    if input.len() > MAX_REQUEST_BYTES {
      write_reply(&mut stream, json!({"ok":false,"error":"request too large"}))?;
      return Ok(());
    }
    if input.contains(&b'\n') {
      break;
    }
  }
  if !input.ends_with(b"\n") || input[..input.len() - 1].contains(&b'\n') {
    write_reply(
      &mut stream,
      json!({"ok":false,"error":"expected one JSON line"}),
    )?;
    return Ok(());
  }
  let reply = match serde_json::from_slice::<Value>(&input[..input.len() - 1]) {
    Ok(request)
      if request.get("op").and_then(Value::as_str) == Some("primitive_result")
        && !valid_private_result_request(&input[..input.len() - 1]) =>
    {
      json!({"ok":false,"error":"invalid result request"})
    }
    Ok(request) => handle_request_guarded(&request, runtime, skills, vision, managed, owner_peer),
    Err(error) => json!({"ok":false,"error":format!("invalid JSON: {error}")}),
  };
  write_reply(&mut stream, reply)?;
  Ok(())
}

fn peer_is_owner(stream: &UnixStream) -> bool {
  let mut credentials = libc::ucred {
    pid: 0,
    uid: 0,
    gid: 0,
  };
  let mut length = std::mem::size_of::<libc::ucred>() as libc::socklen_t;
  let result = unsafe {
    libc::getsockopt(
      stream.as_raw_fd(),
      libc::SOL_SOCKET,
      libc::SO_PEERCRED,
      (&mut credentials as *mut libc::ucred).cast(),
      &mut length,
    )
  };
  result == 0
    && length as usize == std::mem::size_of::<libc::ucred>()
    && credentials.uid == unsafe { libc::geteuid() }
}

fn write_reply(stream: &mut UnixStream, reply: Value) -> std::io::Result<()> {
  serde_json::to_writer(&mut *stream, &reply)?;
  stream.write_all(b"\n")?;
  stream.flush()
}

fn handle_request_guarded(
  request: &Value,
  runtime: &Runtime,
  skills: &Arc<SkillManager>,
  vision: &VisionService,
  managed: Option<&managed_admission::ManagedAdmission>,
  owner_peer: bool,
) -> Value {
  let Some(op) = request.get("op").and_then(Value::as_str) else {
    return json!({"ok":false,"error":"op must be a string"});
  };
  if matches!(
    op,
    "stop"
      | "result"
      | "cancel"
      | "manual"
      | "primitive"
      | "manual_stop"
      | "skill_execute"
      | "skill_result"
      | "skill_cancel"
      | "dispatch"
  ) {
    return json!({"ok":false,"error":"operation has no governed managed route"});
  }
  match op {
    "vision_attest" => {
      if !owner_peer || managed.is_none() {
        return json!({"ok":false,"error":"vision_unavailable"});
      }
      let Some(gate_path) = request.get("gate_path").and_then(Value::as_str) else {
        return json!({"ok":false,"error":"gate_inactive"});
      };
      if only_keys(request, &["op", "gate_path"]).is_err() {
        return json!({"ok":false,"error":"invalid_metadata"});
      }
      match vision.attest(runtime, gate_path) {
        Ok(value) => value,
        Err(error) => json!({"ok":false,"error":error}),
      }
    }
    "route_status" => match only_keys(request, &["op"]) {
      Ok(()) => vision.route_status(),
      Err(error) => json!({"ok":false,"error":error}),
    },
    "vision_gates" => {
      let mut response = vision.active_gates(runtime);
      response["ok"] = true.into();
      response
    }
    "vision_frame" => {
      let Some(gate_path) = request.get("gate_path").and_then(Value::as_str) else {
        return json!({"ok":false,"error":"gate_path must be a string"});
      };
      let Some(revision) = request.get("revision").and_then(Value::as_str) else {
        return json!({"ok":false,"error":"revision must be a string"});
      };
      let after = request
        .get("after_generation")
        .and_then(Value::as_u64)
        .unwrap_or(0);
      let width = request
        .get("max_width_px")
        .and_then(Value::as_u64)
        .unwrap_or(1280);
      let quality = request
        .get("jpeg_quality")
        .and_then(Value::as_u64)
        .unwrap_or(80);
      if !(1..=4096).contains(&width) || !(1..=100).contains(&quality) {
        return json!({"ok":false,"error":"invalid vision frame options"});
      }
      match vision.capture(
        runtime,
        gate_path,
        revision,
        after,
        width as u32,
        quality as u8,
      ) {
        Ok(Some(captured)) => {
          json!({"ok":true,"changed":true,"generation":captured.generation,
                        "metadata":captured.metadata,"jpeg_base64":STANDARD.encode(captured.frame.jpeg)})
        }
        Ok(None) => json!({"ok":true,"changed":false}),
        Err(error) => json!({"ok":false,"error":error}),
      }
    }
    "time" => json!({"ok":true,"monotonic_ns":SystemClock.monotonic_ns()}),
    "discover" => {
      let nodes: Vec<Value> = runtime.discover().into_iter().map(|node| {
                json!({"path":node.path,"node_id":node.node_id,
                    "kind":format!("{:?}",node.kind).to_lowercase(),"label":node.label,
                    "facets":node.facets.iter().map(|f| format!("{:?}",f).to_lowercase()).collect::<Vec<_>>(),
                    "capabilities":node.capabilities})
            }).collect();
      json!({"ok":true,"nodes":nodes})
    }
    "health" | "status" | "stop" => {
      let Some(path) = request.get("node_path").and_then(Value::as_str) else {
        return json!({"ok":false,"error":"node_path must be a string"});
      };
      if op == "stop" {
        match runtime.dispatcher().stop_node(path) {
          Ok(()) => json!({"ok":true}),
          Err(error) => json!({"ok":false,"error":error}),
        }
      } else if op == "status" {
        match runtime.dispatcher().status(path) {
          Ok(status) => json!({"ok":true,"status":{
                        "state":format!("{:?}",status.state).to_lowercase(),
                        "active_command_id":status.active_command_id}}),
          Err(error) => json!({"ok":false,"error":error}),
        }
      } else {
        match runtime.dispatcher().health(path) {
          Ok(health) => json!({"ok":true,"health":{
                        "state":format!("{:?}",health.state).to_lowercase(),
                        "code":health.code,"message":health.message,
                        "last_ok_monotonic_ns":health.last_ok_monotonic_ns}}),
          Err(error) => json!({"ok":false,"error":error}),
        }
      }
    }
    "capture" => {
      let Some(path) = request.get("node_path").and_then(Value::as_str) else {
        return json!({"ok":false,"error":"node_path must be a string"});
      };
      match runtime.dispatcher().capture_camera(path) {
        Ok(frame) => {
          let depth = frame.depth.as_ref().map(|depth| {
            let bytes: Vec<u8> = depth
              .depth_u16
              .iter()
              .flat_map(|sample| sample.to_le_bytes())
              .collect();
            json!({"depth_u16_le_base64":STANDARD.encode(bytes),
                            "depth_scale_m":depth.depth_scale_m,"intrinsics":depth.intrinsics})
          });
          json!({"ok":true,"frame":{"meta":frame.meta,"width_px":frame.width_px,
                        "height_px":frame.height_px,"stride_bytes":frame.stride_bytes,
                        "rgb8_base64":STANDARD.encode(frame.rgb8),"depth":depth}})
        }
        Err(error) => json!({"ok":false,"error":error}),
      }
    }
    "observe" => {
      let Some(path) = request.get("node_path").and_then(Value::as_str) else {
        return json!({"ok":false,"error":"node_path must be a string"});
      };
      match runtime.profile().node(path).map(|node| node.kind) {
        Some(NodeKind::Base) => match runtime.dispatcher().observe_base(path) {
          Ok(observation) => json!({"ok":true,"observation":observation}),
          Err(error) => json!({"ok":false,"error":error}),
        },
        Some(NodeKind::Manipulator) => match runtime.dispatcher().observe_manipulator(path) {
          Ok(observation) => json!({"ok":true,"observation":observation}),
          Err(error) => json!({"ok":false,"error":error}),
        },
        _ => json!({"ok":false,"error":"node is not a base or manipulator"}),
      }
    }
    "shutdown" => {
      if !owner_peer {
        return json!({"ok":false,"error":"shutdown requires owner peer"});
      }
      if let Some(correlation) = request.get("correlation_id") {
        let Some(id) = correlation.as_str().filter(|id| valid_run_id(id)) else {
          return json!({"ok":false,"error":"invalid correlation_id"});
        };
        if let Ok(mut slot) = SHUTDOWN_CORRELATION.lock() {
          *slot = Some(id.into());
        }
      }
      TERMINATE.store(true, Ordering::SeqCst);
      json!({"ok":true})
    }
    "result" => {
      let Some(id) = request.get("command_id").and_then(Value::as_str) else {
        return json!({"ok":false,"error":"command_id must be a string"});
      };
      match runtime.dispatcher().result(id) {
        Some(result) => json!({"ok":true,"result":result}),
        None => json!({"ok":false,"error":"unknown command ID"}),
      }
    }
    "primitive_result" => {
      let parsed = (|| -> Result<aether_dispatcher::DispatchResult, &'static str> {
        only_keys(
          request,
          &["op", "command_id", "active_revision", "authority_grant"],
        )
        .map_err(|_| "invalid result request")?;
        let id = required_str(request, "command_id").map_err(|_| "invalid result request")?;
        let revision =
          required_str(request, "active_revision").map_err(|_| "invalid result request")?;
        let grant =
          required_str(request, "authority_grant").map_err(|_| "invalid result request")?;
        if id.is_empty() || id.len() > 128 || !valid_digest(revision) || !valid_digest(grant) {
          return Err("invalid result request");
        }
        let managed = managed.ok_or("operator control grant required")?;
        if managed.grant_role(grant) != Some(managed_admission::GrantRole::OperatorControl) {
          return Err("operator control grant required");
        }
        if revision != managed.active_revision {
          return Err("stale active revision");
        }
        runtime
          .dispatcher()
          .browser_result_authorized_checked(id, &managed.run_id)
          .map_err(|_| "result unavailable")?
          .ok_or("unknown command ID")
      })();
      match parsed {
        Ok(result) => bounded_browser_result(result),
        Err(error) => json!({"ok":false,"error":error}),
      }
    }
    "cancel" => {
      let Some(id) = request.get("command_id").and_then(Value::as_str) else {
        return json!({"ok":false,"error":"command_id must be a string"});
      };
      match runtime.dispatcher().cancel(id) {
        Ok(result) => json!({"ok":true,"result":result}),
        Err(error) => json!({"ok":false,"error":error}),
      }
    }
    "manual" | "primitive" => {
      let control_path = match required_str(request, "node_path") {
        Ok(path) => path,
        Err(error) => return json!({"ok":false,"error":error}),
      };
      let expected_type = if op == "manual" {
        "eos.control.manual@v1.0"
      } else {
        "eos.control.primitive@v1.0"
      };
      if !runtime
        .profile()
        .node(control_path)
        .is_some_and(|node| node.node_type == expected_type)
      {
        return json!({"ok":false,"error":"control node type differs from operation"});
      }
      match parse_control_command(request)
        .and_then(|command| runtime.invoke_control(control_path, command))
      {
        Ok(result) => json!({"ok":true,"result":result}),
        Err(error) => json!({"ok":false,"error":error}),
      }
    }
    "primitive_typed" => {
      let parsed = (|| -> Result<aether_dispatcher::DispatchResult, String> {
        let (binding, command) = parse_typed_primitive(request, runtime)?;
        let Some(managed) = managed else {
          return Err("managed actuator admission unavailable".into());
        };
        let grant = required_str(request, "authority_grant")?;
        if managed.grant_role(grant) != Some(managed_admission::GrantRole::OperatorControl) {
          return Err("operator control grant required".into());
        }
        let header = request.get("header").ok_or("header is required")?;
        let revision = required_str(header, "active_revision")?;
        if !valid_digest(revision) || revision != managed.active_revision {
          return Err("stale or invalid active revision".into());
        }
        if header.get("delivery_mode").and_then(Value::as_str) == Some("stream")
          && required_str(header, "run_id")? != managed.run_id
        {
          return Err("stale or invalid run identity".into());
        }
        let context = AdmissionContext {
          run_id: managed.run_id.clone(),
          active_revision: Some(revision.into()),
          authority_role: AuthorityRole::OperatorControl,
          ingress: AdmissionIngress::PrivateBridge,
          binding_path: binding,
          primitive: required_str(header, "primitive_name")?.into(),
          primitive_version: required_str(header, "primitive_version")?.into(),
          delivery_mode: match header.get("delivery_mode").and_then(Value::as_str) {
            None | Some("one_shot") => aether_dispatcher::MovementDelivery::OneShot,
            Some("stream") => aether_dispatcher::MovementDelivery::Stream,
            _ => return Err("invalid delivery_mode".into()),
          },
        };
        runtime
          .dispatcher_shared()
          .submit_browser_movement(context, command)
      })();
      match parsed {
        Ok(result) => json!({"ok":true,"result":result}),
        Err(error) => json!({"ok":false,"error":error}),
      }
    }
    "manual_stop" => {
      let parsed = (|| {
        only_keys(request, &["op", "node_path"])?;
        runtime.manual_stop(required_str(request, "node_path")?)
      })();
      match parsed {
        Ok(()) => json!({"ok":true}),
        Err(error) => json!({"ok":false,"error":error}),
      }
    }
    "skill_execute" => {
      let parsed = (|| {
        only_keys(request, &["op", "node_path", "execution_id", "steps"])?;
        let path = required_str(request, "node_path")?;
        let id = required_str(request, "execution_id")?;
        let steps = request
          .get("steps")
          .and_then(Value::as_array)
          .ok_or("steps must be an array")?;
        if steps.is_empty() || steps.len() > 16 {
          return Err("steps must contain 1..16 entries".into());
        }
        let mut movements = Vec::with_capacity(steps.len());
        for step in steps {
          only_keys(step, &["movement"])?;
          movements.push(parse_movement(
            step.get("movement").ok_or("step movement is required")?,
          )?);
        }
        skills.execute(path, id, movements)
      })();
      match parsed {
        Ok(result) => result,
        Err(error) => json!({"ok":false,"error":error}),
      }
    }
    "skill_result" | "skill_cancel" => {
      let parsed = (|| {
        only_keys(request, &["op", "execution_id"])?;
        let id = required_str(request, "execution_id")?;
        if op == "skill_cancel" {
          skills.cancel(id)
        } else {
          skills.result(id).ok_or("unknown execution_id".into())
        }
      })();
      match parsed {
        Ok(result) => result,
        Err(error) => json!({"ok":false,"error":error}),
      }
    }
    "dispatch" => match parse_command(request) {
      Ok(command) => json!({"ok":true,"result":runtime.dispatch(command)}),
      Err(error) => json!({"ok":false,"error":error}),
    },
    _ => json!({"ok":false,"error":format!("unsupported op: {op}")}),
  }
}

fn bounded_browser_result(result: aether_dispatcher::DispatchResult) -> Value {
  if result
    .reason
    .as_ref()
    .is_some_and(|reason| reason.len() > 512)
  {
    return json!({"ok":false,"error":"result unavailable"});
  }
  let response = json!({"ok":true,"result":result});
  if serde_json::to_vec(&response).is_ok_and(|bytes| bytes.len() <= MAX_BROWSER_RESULT_BYTES) {
    response
  } else {
    json!({"ok":false,"error":"result unavailable"})
  }
}

fn graph_revision(bytes: &[u8]) -> String {
  format!("{:x}", Sha256::digest(bytes))
}

fn parse_control_command(value: &Value) -> Result<CommandRequest, String> {
  only_keys(
    value,
    &[
      "op",
      "node_path",
      "command_id",
      "deadline_monotonic_ns",
      "movement",
    ],
  )?;
  Ok(CommandRequest {
    node_path: String::new(),
    meta: CommandMeta {
      command_id: required_str(value, "command_id")?.into(),
      deadline_monotonic_ns: required_u64(value, "deadline_monotonic_ns")?,
    },
    movement: parse_movement(value.get("movement").ok_or("movement is required")?)?,
  })
}

/// Decode the bounded JSON command envelope before a binding may reach Dispatcher.
/// The graph edge, rather than this untrusted header, remains target authority.
fn parse_typed_primitive(
  value: &Value,
  runtime: &Runtime,
) -> Result<(String, CommandRequest), String> {
  only_keys(value, &["op", "header", "payload", "authority_grant"])?;
  let header = value.get("header").ok_or("header is required")?;
  only_keys(
    header,
    &[
      "data_type_id",
      "data_type_version",
      "primitive_name",
      "primitive_version",
      "active_revision",
      "source_path",
      "target_path",
      "command_id",
      "sequence",
      "deadline_monotonic_ns",
      "deadline_after_ms",
      "delivery_mode",
      "run_id",
      "gateway_path",
      "payload_length_bytes",
      "codec",
    ],
  )?;
  let source = required_str(header, "source_path")?;
  let binding = runtime
    .profile()
    .node(source)
    .ok_or("unknown primitive binding source")?;
  if binding.path() != source
    || !matches!(
      binding.node_type.as_str(),
      "eos.dispatcher.primitive_base@v1.0"
        | "eos.dispatcher.primitive_manipulator@v1.0"
        | "eos.dispatcher.primitive_base@v0.1"
        | "eos.dispatcher.primitive_manipulator@v0.1"
        | "isos.eos_dispatcher.primitive_base@v0.2"
        | "isos.eos_dispatcher.primitive_manipulator@v0.2"
    )
  {
    return Err("source_path must name a canonical primitive binding".into());
  }
  let payload = value.get("payload").ok_or("payload is required")?;
  let payload_bytes = serde_json::to_vec(payload).map_err(|e| e.to_string())?;
  if payload_bytes.is_empty()
    || payload_bytes.len() > 16 * 1024
    || required_u64(header, "payload_length_bytes")? != payload_bytes.len() as u64
  {
    return Err("payload length differs from bounded JSON payload".into());
  }
  if required_str(header, "codec")? != "json_v1"
    || required_str(header, "data_type_version")? != "1.0"
    || required_u64(header, "sequence")? == 0
  {
    return Err("unsupported codec, data type version, or sequence".into());
  }
  let command_id = required_str(header, "command_id")?;
  if command_id.is_empty() || command_id.len() > 128 {
    return Err("command_id must contain 1..128 bytes".into());
  }
  let movement = parse_movement(payload)?;
  let expected_data_type = match movement {
    Movement::Base(_) => "base_movement",
    Movement::Manipulator(_) => "manipulator_movement",
  };
  if required_str(header, "data_type_id")? != expected_data_type
    || required_str(header, "primitive_name")?
      != binding.settings["primitive_name"]
        .as_str()
        .unwrap_or_default()
    || required_str(header, "primitive_version")?
      != binding.settings["primitive_version"]
        .as_str()
        .unwrap_or_default()
  {
    return Err("header differs from registered primitive binding or payload type".into());
  }
  let target = runtime.control_target_for_movement(source, &movement)?;
  if required_str(header, "target_path")? != target {
    return Err("target_path differs from the graph binding edge".into());
  }
  if header.get("delivery_mode").and_then(Value::as_str) == Some("stream") {
    if !matches!(&movement, Movement::Base(BaseMovement::BodyVelocity { .. })) {
      return Err("stream delivery requires body velocity".into());
    }
    let gateway_path = required_str(header, "gateway_path")?;
    let gateway = runtime
      .profile()
      .node(gateway_path)
      .ok_or("unknown stream gateway")?;
    if gateway.node_type != "eos.interface.http_gateway@v0.1"
      || !binding.inputs.get("request").is_some_and(|sources| {
        sources
          .values()
          .iter()
          .any(|edge| edge.node == gateway_path && edge.output == "movement")
      })
    {
      return Err("stream gateway is not connected to primitive binding".into());
    }
  }
  let deadline_monotonic_ns = match (
    header.get("deadline_monotonic_ns"),
    header.get("deadline_after_ms"),
  ) {
    (Some(_), Some(_)) | (None, None) => {
      return Err("exactly one deadline form is required".into());
    }
    (Some(_), None) => required_u64(header, "deadline_monotonic_ns")?,
    (None, Some(_)) => {
      let duration_ms = required_u64(header, "deadline_after_ms")?;
      if !(1..=5_000).contains(&duration_ms) {
        return Err("deadline_after_ms must be within 1..=5000".into());
      }
      SystemClock
        .monotonic_ns()
        .saturating_add(duration_ms * 1_000_000)
    }
  };
  Ok((
    source.into(),
    CommandRequest {
      node_path: target,
      meta: CommandMeta {
        command_id: command_id.into(),
        deadline_monotonic_ns,
      },
      movement,
    },
  ))
}

fn parse_command(value: &Value) -> Result<CommandRequest, String> {
  only_keys(
    value,
    &[
      "op",
      "node_path",
      "command_id",
      "deadline_monotonic_ns",
      "movement",
    ],
  )?;
  let movement = value.get("movement").ok_or("movement is required")?;
  Ok(CommandRequest {
    node_path: required_str(value, "node_path")?.into(),
    meta: CommandMeta {
      command_id: required_str(value, "command_id")?.into(),
      deadline_monotonic_ns: required_u64(value, "deadline_monotonic_ns")?,
    },
    movement: parse_movement(movement)?,
  })
}

fn parse_movement(value: &Value) -> Result<Movement, String> {
  let kind = required_str(value, "type")?;
  match kind {
    "body_velocity" => {
      only_keys(
        value,
        &["type", "x_m_s", "y_m_s", "yaw_rad_s", "valid_for_ms"],
      )?;
      Ok(Movement::Base(BaseMovement::BodyVelocity {
        x_m_s: required_f64(value, "x_m_s")?,
        y_m_s: required_f64(value, "y_m_s")?,
        yaw_rad_s: required_f64(value, "yaw_rad_s")?,
        valid_for_ms: required_u64(value, "valid_for_ms")?,
      }))
    }
    "relative_pose" => {
      only_keys(value, &["type", "x_m", "y_m", "yaw_rad", "frame_id"])?;
      Ok(Movement::Base(BaseMovement::RelativePose {
        x_m: required_f64(value, "x_m")?,
        y_m: required_f64(value, "y_m")?,
        yaw_rad: required_f64(value, "yaw_rad")?,
        frame_id: required_str(value, "frame_id")?.into(),
      }))
    }
    "joint_positions" => {
      only_keys(
        value,
        &[
          "type",
          "joint_names",
          "joint_positions_rad",
          "max_speed_rad_s",
        ],
      )?;
      let names = value
        .get("joint_names")
        .and_then(Value::as_array)
        .ok_or("joint_names must be an array")?
        .iter()
        .map(|v| {
          v.as_str()
            .map(str::to_owned)
            .ok_or("joint_names must contain strings".into())
        })
        .collect::<Result<Vec<_>, String>>()?;
      let positions = number_array(value, "joint_positions_rad")?;
      Ok(Movement::Manipulator(ManipulatorMovement::JointPositions {
        joint_names: names,
        joint_positions_rad: positions,
        max_speed_rad_s: optional_f64(value, "max_speed_rad_s")?,
      }))
    }
    "end_effector_pose" => {
      only_keys(value, &["type", "pose", "max_speed_m_s"])?;
      let pose = value.get("pose").ok_or("pose is required")?;
      only_keys(pose, &["position_m", "orientation_xyzw", "frame_id"])?;
      let position_m: [f64; 3] = number_array(pose, "position_m")?
        .try_into()
        .map_err(|_| "position_m must contain exactly three numbers")?;
      let orientation_xyzw: [f64; 4] = number_array(pose, "orientation_xyzw")?
        .try_into()
        .map_err(|_| "orientation_xyzw must contain exactly four numbers")?;
      Ok(Movement::Manipulator(
        ManipulatorMovement::EndEffectorPose {
          pose: EndEffectorPose {
            position_m,
            orientation_xyzw,
            frame_id: required_str(pose, "frame_id")?.into(),
          },
          max_speed_m_s: optional_f64(value, "max_speed_m_s")?,
        },
      ))
    }
    _ => Err(format!("unsupported movement type: {kind}")),
  }
}

fn only_keys(value: &Value, allowed: &[&str]) -> Result<(), String> {
  let object = value.as_object().ok_or("expected an object")?;
  for key in object.keys() {
    if !allowed.contains(&key.as_str()) {
      return Err(format!("unexpected field: {key}"));
    }
  }
  Ok(())
}

fn required_str<'a>(value: &'a Value, key: &str) -> Result<&'a str, String> {
  value
    .get(key)
    .and_then(Value::as_str)
    .ok_or_else(|| format!("{key} must be a string"))
}

fn required_u64(value: &Value, key: &str) -> Result<u64, String> {
  value
    .get(key)
    .and_then(Value::as_u64)
    .ok_or_else(|| format!("{key} must be an unsigned integer"))
}

fn required_f64(value: &Value, key: &str) -> Result<f64, String> {
  value
    .get(key)
    .and_then(Value::as_f64)
    .ok_or_else(|| format!("{key} must be a number"))
}

fn optional_f64(value: &Value, key: &str) -> Result<Option<f64>, String> {
  match value.get(key) {
    None | Some(Value::Null) => Ok(None),
    Some(_) => required_f64(value, key).map(Some),
  }
}

fn number_array(value: &Value, key: &str) -> Result<Vec<f64>, String> {
  value
    .get(key)
    .and_then(Value::as_array)
    .ok_or_else(|| format!("{key} must be an array"))?
    .iter()
    .map(|v| {
      v.as_f64()
        .ok_or_else(|| format!("{key} must contain numbers"))
    })
    .collect()
}

#[cfg(test)]
mod revision_tests {
  use super::{
    BackendLaunch, BackendStopReport, Options, TerminalReceiptInput, graph_revision,
    bounded_browser_result, parse_typed_primitive, runtime_capabilities, terminal_receipt,
  };
  use aether_common::time::ManualClock;
  use aether_dispatcher::{DispatchResult, OutcomeStatus};
  use aether_hal::{ControlledStopOutcome, Profile, RobotStopProof};
  use aether_runtime::Runtime;
  use serde_json::{json, Value};
  use std::sync::Arc;

  #[test]
  fn managed_stop_capability_announces_contract_without_claiming_a_receipt() {
    let capabilities = runtime_capabilities();
    assert!(capabilities["features"]
      .as_array()
      .unwrap()
      .iter()
      .any(|feature| feature == "managed-stop-v1"));
    assert!(capabilities.get("eos_terminal_stop").is_none());
  }

  #[test]
  fn selected_terminal_receipt_needs_dual_proof_and_reaped_group() {
    let profile =
      Profile::from_toml(include_str!("../../../../examples/v2/mujoco-isos.toml")).unwrap();
    let options = Options {
      profile: "/tmp/test.toml".into(),
      journal_dir: "/tmp/test-journal".into(),
      socket: "/tmp/test-control.sock".into(),
      backend: Some(BackendLaunch {
        node: "eos/process/backend/simulator".into(),
        executable: "/tmp/test-simulator".into(),
        scene: "/tmp/test-scene.xml".into(),
        log_path: None,
        package: None,
        lifecycle_adapter: None,
        node_type: None,
      }),
      node_types: Vec::new(),
      adapter_libraries: Vec::new(),
      run_id: "test-run".into(),
      backend_release_sha256: None,
      backend_payload_sha256: None,
      grpc_port: 0,
    };
    let paths: Vec<_> = profile
      .hardware_nodes()
      .filter(|node| node.movement.is_some())
      .map(|node| node.path().to_owned())
      .collect();
    assert_eq!(paths.len(), 2);
    let results: Vec<_> = paths.iter().map(|path| (path.clone(), Ok(()))).collect();
    let proof = RobotStopProof {
      base: ControlledStopOutcome::Stopped,
      arm: ControlledStopOutcome::Stopped,
      generation_id: "00112233445566778899aabbccddeeff".into(),
      stop_epoch: Some("1".into()),
    };
    let mut report = BackendStopReport {
      backend_pid: 1234,
      backend_pgid: 1234,
      group_drained: true,
      reaped: true,
      sockets_removed: true,
      forced_kill: false,
      reason: None,
    };
    let receipt = |report: &BackendStopReport,
                   proof: Option<&RobotStopProof>,
                   verified_generation: Option<&str>,
                   stop_results: &[(String, Result<(), String>)]| {
      terminal_receipt(TerminalReceiptInput {
        options: &options,
        active_revision: &"a".repeat(64),
        profile: &profile,
        verified_generation,
        proof,
        report,
        stop_results,
        control_socket_removed: true,
        other_shutdown_ok: true,
      })
    };
    let generation = proof.generation_id.as_str();
    let confirmed = receipt(&report, Some(&proof), Some(generation), &results);
    assert_eq!(confirmed["outcome"], "confirmed");
    let host_worker_unreaped = terminal_receipt(TerminalReceiptInput {
      options: &options,
      active_revision: &"a".repeat(64),
      profile: &profile,
      verified_generation: Some(generation),
      proof: Some(&proof),
      report: &report,
      stop_results: &results,
      control_socket_removed: true,
      other_shutdown_ok: false,
    });
    assert_eq!(host_worker_unreaped["outcome"], "unknown");
    assert_eq!(confirmed["parts"].as_array().unwrap().len(), 2);
    assert_eq!(confirmed["backend_node"], "eos/process/backend/simulator");
    let unknown_results: Vec<_> = paths
      .iter()
      .map(|path| (path.clone(), Err("stop socket unavailable".into())))
      .collect();
    let lost_response = receipt(&report, None, Some(generation), &unknown_results);
    assert_eq!(lost_response["outcome"], "unknown");
    assert_eq!(lost_response["generation_id"], generation);
    assert!(lost_response["stop_epoch"].is_null());
    assert!(lost_response["parts"]
      .as_array()
      .unwrap()
      .iter()
      .all(|part| part["outcome"] == "unknown"));
    let before_ready = receipt(&report, None, None, &unknown_results);
    assert!(before_ready["generation_id"].is_null());
    assert!(before_ready["stop_epoch"].is_null());
    let different_generation = "ffeeddccbbaa99887766554433221100";
    let mismatched = receipt(&report, Some(&proof), Some(different_generation), &results);
    assert_eq!(mismatched["outcome"], "unknown");
    assert_eq!(mismatched["generation_id"], different_generation);
    assert!(mismatched["stop_epoch"].is_null());
    report.reaped = false;
    assert_eq!(
      receipt(&report, Some(&proof), Some(generation), &results)["outcome"],
      "unknown"
    );
    report.reaped = true;
    report.forced_kill = true;
    assert_eq!(
      receipt(&report, Some(&proof), Some(generation), &results)["outcome"],
      "unknown"
    );
    report.forced_kill = false;
    report.reason = Some("backend_exited");
    assert_eq!(
      receipt(&report, Some(&proof), Some(generation), &results)["outcome"],
      "unknown"
    );
  }

  #[test]
  fn test_package_process_handoff_rejects_unknown_fields_and_invalid_digests() {
    let path =
      std::env::temp_dir().join(format!("eos-package-process-{}.json", std::process::id()));
    let mut value = serde_json::json!({
      "contract": "isos.package-process@v1",
      "schema_version": "0.1.0",
      "owner_core": "eos",
      "run_id": "run-1",
      "graph_revision": "c".repeat(64),
      "node_path": "eos/process/backend/simulator",
      "node_type": "eos.process.backend@v1.0",
      "package": {"id": "example/simulator", "version": "1.0.0"},
      "entrypoint": "/tmp/simulator",
      "resources": {"scene": {"kind":"scene","id":"demo","path":"/tmp/scene.xml","sha256":"d".repeat(64),"metadata":null}},
      "lifecycle_adapter": "mujoco-v2",
      "release_sha256": "a".repeat(64),
      "payload_sha256": "b".repeat(64)
    });
    std::fs::write(&path, serde_json::to_vec(&value).unwrap()).unwrap();
    assert!(super::read_package_process(&path).is_ok());
    value["unexpected"] = serde_json::json!(true);
    std::fs::write(&path, serde_json::to_vec(&value).unwrap()).unwrap();
    assert!(super::read_package_process(&path).is_err());
    value.as_object_mut().unwrap().remove("unexpected");
    value["lifecycle_adapter"] = serde_json::json!("unknown-adapter");
    std::fs::write(&path, serde_json::to_vec(&value).unwrap()).unwrap();
    assert!(super::read_package_process(&path).is_err());
    value["lifecycle_adapter"] = serde_json::json!("mujoco-v2");
    value["resources"]["ignored"] =
      serde_json::json!({"kind":"scene","id":"ignored","path":"/tmp/ignored"});
    std::fs::write(&path, serde_json::to_vec(&value).unwrap()).unwrap();
    assert!(super::read_package_process(&path).is_err());
    value["resources"]
      .as_object_mut()
      .unwrap()
      .remove("ignored");
    value["payload_sha256"] = serde_json::json!("not-a-digest");
    std::fs::write(&path, serde_json::to_vec(&value).unwrap()).unwrap();
    assert!(super::read_package_process(&path).is_err());
    std::fs::remove_file(path).unwrap();
  }

  #[test]
  fn graph_revision_matches_applied_sha256() {
    assert_eq!(
      graph_revision(b"abc"),
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );
  }

  #[test]
  fn browser_result_preserves_all_statuses_and_fails_closed_on_response_bounds() {
    for status in [
      OutcomeStatus::Completed,
      OutcomeStatus::Failed,
      OutcomeStatus::Cancelled,
      OutcomeStatus::Rejected,
      OutcomeStatus::InProgress,
      OutcomeStatus::OutcomeUnknown,
    ] {
      let result = DispatchResult {
        command_id: "command".into(),
        status,
        reason: None,
      };
      assert_eq!(bounded_browser_result(result)["ok"], true);
    }
    for result in [
      DispatchResult {
        command_id: "command".into(),
        status: OutcomeStatus::Failed,
        reason: Some("é".repeat(257)),
      },
      DispatchResult {
        command_id: "x".repeat(4096),
        status: OutcomeStatus::Failed,
        reason: None,
      },
    ] {
      assert_eq!(
        bounded_browser_result(result),
        json!({"ok":false,"error":"result unavailable"})
      );
    }
  }

  #[test]
  fn typed_primitive_header_binds_payload_and_graph_target() {
    let mut graph: Value = serde_json::to_value(
      Profile::from_toml(include_str!("../../../../etc/eos/backends/mock-cxx.toml")).unwrap(),
    )
    .unwrap();
    graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["base"] = json!({
        "nodes": {"velocity": {
            "id":"velocity", "node_type":"eos.dispatcher.primitive_base@v1.0",
            "label":"Velocity", "description":"Test binding",
            "settings":{"primitive_name":"base.velocity","primitive_version":"1.0.0","movement_type":"body_velocity"}
        }}
    });
    graph["cores"]["eos"]["layers"]["hardware"]["groups"]["base"]["nodes"]["drive"]["inputs"]
      ["command"] = json!([{"node":"eos/dispatcher/base/velocity","output":"command"}]);
    graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["manipulator"] = json!({
        "nodes": {"movej": {
            "id":"movej", "node_type":"eos.dispatcher.primitive_manipulator@v1.0",
            "label":"MoveJ", "description":"Test binding",
            "settings":{"primitive_name":"arm.movej","primitive_version":"1.0.0","movement_type":"joint_positions"}
        }}
    });
    graph["cores"]["eos"]["layers"]["hardware"]["groups"]["manipulator"]["nodes"]["arm"]
      ["inputs"]["command"] =
      json!([{"node":"eos/dispatcher/manipulator/movej","output":"command"}]);
    let profile = Profile::from_json(&graph.to_string()).unwrap();
    let runtime =
      Runtime::from_cxx_mock_profile(profile, Arc::new(ManualClock::new(0, None))).unwrap();
    let payload =
      json!({"type":"body_velocity","x_m_s":0.1,"y_m_s":0.0,"yaw_rad_s":0.0,"valid_for_ms":100});
    let mut request = json!({"op":"primitive_typed","header":{
            "data_type_id":"base_movement","data_type_version":"1.0",
            "primitive_name":"base.velocity","primitive_version":"1.0.0",
            "source_path":"eos/dispatcher/base/velocity","target_path":"eos/hardware/base/drive",
            "command_id":"typed-1","sequence":1,"deadline_monotonic_ns":200_000_000,
            "payload_length_bytes":serde_json::to_vec(&payload).unwrap().len(),"codec":"json_v1"
        },"payload":payload});
    let (binding, command) = parse_typed_primitive(&request, &runtime).unwrap();
    assert_eq!(binding, "eos/dispatcher/base/velocity");
    assert_eq!(command.node_path, "eos/hardware/base/drive");
    assert!(runtime.invoke_control(&binding, command).is_ok());
    request["header"]["target_path"] = json!("eos/hardware/base/forged");
    assert!(parse_typed_primitive(&request, &runtime).is_err());
    request["header"]["target_path"] = json!("eos/hardware/base/drive");
    request["header"]["payload_length_bytes"] = json!(1);
    assert!(parse_typed_primitive(&request, &runtime).is_err());
    request["header"]["payload_length_bytes"] =
      json!(serde_json::to_vec(&request["payload"]).unwrap().len());
    request["header"]["primitive_version"] = json!("9.0.0");
    assert!(parse_typed_primitive(&request, &runtime).is_err());

    let arm_payload = json!({"type":"joint_positions","joint_names":["joint_1"],
            "joint_positions_rad":[0.1],"max_speed_rad_s":0.2});
    let arm_request = json!({"op":"primitive_typed","header":{
            "data_type_id":"manipulator_movement","data_type_version":"1.0",
            "primitive_name":"arm.movej","primitive_version":"1.0.0",
            "source_path":"eos/dispatcher/manipulator/movej",
            "target_path":"eos/hardware/manipulator/arm", "command_id":"typed-arm-1",
            "sequence":1,"deadline_monotonic_ns":200_000_000,
            "payload_length_bytes":serde_json::to_vec(&arm_payload).unwrap().len(),"codec":"json_v1"
        },"payload":arm_payload});
    let (binding, command) = parse_typed_primitive(&arm_request, &runtime).unwrap();
    assert!(runtime.invoke_control(&binding, command).is_ok());
  }

  #[test]
  fn stream_header_requires_declared_gateway_and_bounded_relative_deadline() {
    let mut graph: Value = serde_json::from_str(include_str!(
      "../../../../tests/fixtures/edge_contract/valid_bindings.json"
    ))
    .unwrap();
    graph["cores"]["eos"]["layers"]["interface"]["groups"]["gateway"] = json!({
      "nodes":{"control":{"id":"control","node_type":"eos.interface.http_gateway@v0.1",
        "label":"Control gateway","description":"HTTP to Dispatcher"}}
    });
    graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["base"]["nodes"]["velocity"]
      ["inputs"] = json!({"request":{"node":"eos/interface/gateway/control","output":"movement"}});
    let profile = Profile::from_json(&graph.to_string()).unwrap();
    let runtime =
      Runtime::from_cxx_mock_profile(profile, Arc::new(ManualClock::new(0, None))).unwrap();
    let payload =
      json!({"type":"body_velocity","x_m_s":0.1,"y_m_s":0.0,"yaw_rad_s":0.0,"valid_for_ms":100});
    let mut request = json!({"op":"primitive_typed","header":{
      "data_type_id":"base_movement","data_type_version":"1.0",
      "primitive_name":"base.velocity","primitive_version":"1.0.0",
      "source_path":"eos/dispatcher/base/velocity","target_path":"eos/hardware/base/drive",
      "gateway_path":"eos/interface/gateway/control","run_id":"run-a",
      "command_id":"stream-1","sequence":1,"deadline_after_ms":250,
      "delivery_mode":"stream","payload_length_bytes":serde_json::to_vec(&payload).unwrap().len(),
      "codec":"json_v1"},"payload":payload});
    let (_, command) = parse_typed_primitive(&request, &runtime).unwrap();
    assert!(command.meta.deadline_monotonic_ns >= 250_000_000);
    request["header"]["gateway_path"] = json!("eos/interface/gateway/forged");
    assert!(parse_typed_primitive(&request, &runtime).is_err());
    request["header"]["gateway_path"] = json!("eos/interface/gateway/control");
    request["header"]["deadline_after_ms"] = json!(5_001);
    assert!(parse_typed_primitive(&request, &runtime).is_err());
    request["header"]["deadline_after_ms"] = json!(250);
    request["header"]["deadline_monotonic_ns"] = json!(10_000_000);
    assert!(parse_typed_primitive(&request, &runtime).is_err());
  }
}
