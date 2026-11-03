//! Read-only snapshot of one validated authored v2 graph.

use std::env;
use std::net::{IpAddr, SocketAddr, TcpListener, TcpStream};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use aether_common::events::{
  Event, EventSink, NonblockingStderr, Reason, Source, Stage, local_run_id, valid_run_id,
};
use aether_common::io::read_bounded;
use aether_common::time::SystemClock;
use aether_control::http::{read_request, reply};
use aether_node_profile::{NodeTypeRegistry, Profile, PRIMITIVE_SPECS};
use serde_json::json;

const MAX_PROFILE_BYTES: usize = 1024 * 1024;
static TERMINATE: AtomicBool = AtomicBool::new(false);
static EVENT_INITIALIZED: AtomicBool = AtomicBool::new(false);
extern "C" fn on_signal(_: libc::c_int) {
  TERMINATE.store(true, Ordering::SeqCst);
}

fn main() {
  if env::args().nth(1).as_deref() == Some("--capabilities") {
    println!(
      "{}",
      json!({"product_version":env!("CARGO_PKG_VERSION"),"graph_version":aether_node_profile::GRAPH_VERSION,
            "node_type_version":"1.0","supported_graph_versions":["1.0.0","0.1.0"],
            "supported_node_type_versions":["1.0","0.1"],
            "supported_node_types":aether_node_profile::builtin_reader_node_types(),
            "binary":"aether-config-server",
            "features":["short-paths","node-types","data-idl","primitive-binding","catalog","graph-0.1-reader"]})
    );
    return;
  }
  if launch().is_err() {
    if !EVENT_INITIALIZED.load(Ordering::SeqCst) {
      eprintln!("aether-config-server: invalid launch arguments");
    }
    std::process::exit(1);
  }
}

fn launch() -> Result<(), Box<dyn std::error::Error>> {
  let mut args = env::args().skip(1);
  let mut profile_path = None;
  let mut listen = None;
  let mut node_types = Vec::<PathBuf>::new();
  let mut run_id = None;
  while let Some(flag) = args.next() {
    let value = args.next().ok_or(format!("missing value for {flag}"))?;
    match flag.as_str() {
      "--profile" if profile_path.is_none() => profile_path = Some(value),
      "--listen" if listen.is_none() => listen = Some(value.parse::<SocketAddr>()?),
      "--node-types" => node_types.push(PathBuf::from(value)),
      "--run-id" if run_id.is_none() && valid_run_id(&value) => run_id = Some(value),
      _ => return Err(format!("unknown or duplicate option: {flag}").into()),
    }
  }
  let path = profile_path.ok_or("--profile is required")?;
  let address: SocketAddr = listen.ok_or("--listen is required")?;
  if !matches!(address.ip(), IpAddr::V4(ip) if ip.is_loopback())
    && !matches!(address.ip(), IpAddr::V6(ip) if ip.is_loopback())
  {
    return Err("config server must bind a loopback address".into());
  }
  let sink = EventSink::new(
    Source::Config,
    run_id.unwrap_or_else(local_run_id),
    NonblockingStderr::new()?,
    SystemClock,
  )?;
  EVENT_INITIALIZED.store(true, Ordering::SeqCst);
  let _ = sink.emit(Event::Starting, None);
  let mut stage = Stage::Profile;
  let result = run(path, address, &node_types, &sink, &mut stage);
  if result.is_err() {
    let reason = match stage {
      Stage::Profile => Reason::ProfileInvalid,
      Stage::Registry => Reason::RegistryInvalid,
      Stage::Listener => Reason::ListenerBindFailed,
      _ => Reason::OtherFailure,
    };
    let _ = sink.emit(Event::StartFailed { stage, reason }, None);
  }
  result
}

fn run(
  path: String,
  address: SocketAddr,
  node_types: &[PathBuf],
  sink: &EventSink<NonblockingStderr, SystemClock>,
  stage: &mut Stage,
) -> Result<(), Box<dyn std::error::Error>> {
  let bytes = read_bounded(path, MAX_PROFILE_BYTES)?;
  *stage = Stage::Registry;
  let registry = NodeTypeRegistry::with_contribution_files(node_types)?;
  *stage = Stage::Profile;
  let source = std::str::from_utf8(&bytes)?;
  let (profile, content_type) = if source.trim_start().starts_with('{') {
    (
      Profile::from_json_with_registry(source, &registry)?,
      "application/json",
    )
  } else {
    (
      Profile::from_toml_with_registry(source, &registry)?,
      "application/toml",
    )
  };
  let summary = json!({"ok":true,"version":profile.version,"robot_id":profile.robot.id,
        "nodes":profile.all_nodes().count()})
  .to_string();
  let types = serde_json::to_vec(&registry.all().collect::<Vec<_>>())?;
  let data_types = serde_json::to_vec(&registry.data_types().collect::<Vec<_>>())?;
  let adapter_families = serde_json::to_vec(&registry.catalog_adapter_families())?;
  let primitive_specs = serde_json::to_vec(PRIMITIVE_SPECS)?;
  let catalog = CatalogResponses {
    profile: &bytes,
    content_type,
    summary: &summary,
    types: &types,
    data_types: &data_types,
    adapter_families: &adapter_families,
    primitive_specs: &primitive_specs,
  };
  *stage = Stage::Listener;
  let listener = TcpListener::bind(address)?;
  listener.set_nonblocking(true)?;
  unsafe {
    if libc::signal(libc::SIGINT, on_signal as *const () as libc::sighandler_t) == libc::SIG_ERR
      || libc::signal(libc::SIGTERM, on_signal as *const () as libc::sighandler_t) == libc::SIG_ERR
    {
      return Err(std::io::Error::last_os_error().into());
    }
  }
  let _ = sink.emit(Event::Ready { node_count: 0 }, None);
  while !TERMINATE.load(Ordering::SeqCst) {
    match listener.accept() {
      Ok((mut stream, _)) => {
        if let Err(error) = serve(&mut stream, &catalog) {
          let body = json!({"ok":false,"error":error}).to_string();
          let _ = reply(&mut stream, 400, "application/json", body.as_bytes());
        }
      }
      Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
        std::thread::sleep(Duration::from_millis(20))
      }
      Err(_) => std::thread::sleep(Duration::from_millis(20)),
    }
  }
  let _ = sink.emit(Event::Stopping, None);
  let _ = sink.emit(Event::Stopped, None);
  Ok(())
}

struct CatalogResponses<'a> {
  profile: &'a [u8],
  content_type: &'a str,
  summary: &'a str,
  types: &'a [u8],
  data_types: &'a [u8],
  adapter_families: &'a [u8],
  primitive_specs: &'a [u8],
}

fn serve(stream: &mut TcpStream, catalog: &CatalogResponses<'_>) -> Result<(), String> {
  let request = read_request(stream)?;
  let (status, mime, body): (u16, &str, &[u8]) =
    match (request.method.as_str(), request.path.as_str()) {
      ("GET", "/api/eos/v2/config") => (200, catalog.content_type, catalog.profile),
      ("GET", "/api/eos/v2/config/validate") => {
        (200, "application/json", catalog.summary.as_bytes())
      }
      ("GET", "/api/eos/v2/types") => (200, "application/json", catalog.types),
      ("GET", "/api/eos/v2/data-types") => (200, "application/json", catalog.data_types),
      ("GET", "/api/eos/v2/adapter-families") => {
        (200, "application/json", catalog.adapter_families)
      }
      ("GET", "/api/eos/v2/primitive-specs") => (200, "application/json", catalog.primitive_specs),
      ("GET", _) => (
        404,
        "application/json",
        b"{\"ok\":false,\"error\":\"not found\"}",
      ),
      _ => (
        405,
        "application/json",
        b"{\"ok\":false,\"error\":\"method not allowed\"}",
      ),
    };
  reply(stream, status, mime, body).map_err(|error| error.to_string())
}
