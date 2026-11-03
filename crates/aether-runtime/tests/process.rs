use std::io::{BufRead, BufReader, Read, Write};
use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::mpsc;
use std::time::{Duration, Instant};

use base64::Engine;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};

#[test]
fn capabilities_probe_advertises_managed_stop_contract() {
  let output = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
    .arg("--capabilities")
    .output()
    .unwrap();
  assert!(output.status.success());
  let capabilities: Value = serde_json::from_slice(&output.stdout).unwrap();
  assert!(capabilities["features"]
    .as_array()
    .unwrap()
    .iter()
    .any(|feature| feature == "managed-stop-v1"));
  assert!(capabilities["features"]
    .as_array()
    .unwrap()
    .iter()
    .any(|feature| feature == "vision-provenance-v1"));
  assert!(capabilities.get("outcome").is_none());
}

struct Fixture {
  dir: PathBuf,
  child: Child,
  socket: PathBuf,
  grpc_address: String,
}

impl Fixture {
  fn start(profile: &Path) -> Self {
    Self::start_with_version(profile, "1.0.0")
  }

  fn start_with_version(profile: &Path, expected_version: &str) -> Self {
    let dir = std::env::temp_dir().join(format!(
      "aether-runtime-test-{}-{}",
      std::process::id(),
      std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos()
    ));
    std::fs::create_dir(&dir).unwrap();
    let socket = dir.join("eos.sock");
    let mut child = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
      .arg("--grpc-port")
      .arg("0")
      .arg("--profile")
      .arg(profile)
      .arg("--journal-dir")
      .arg(dir.join("journal"))
      .arg("--socket")
      .arg(&socket)
      .stdout(Stdio::piped())
      .stderr(Stdio::piped())
      .spawn()
      .unwrap();
    let mut ready = String::new();
    BufReader::new(child.stdout.take().unwrap())
      .read_line(&mut ready)
      .unwrap();
    assert!(!ready.is_empty(), "process failed before ready");
    let ready: Value = serde_json::from_str(&ready).unwrap();
    assert_eq!(ready["version"], expected_version);
    assert_eq!(ready["status"], "ready");
    assert_eq!(ready["hardware_nodes"], 3);
    let grpc_address = ready["grpc_address"].as_str().unwrap().to_string();
    Self {
      dir,
      child,
      socket,
      grpc_address,
    }
  }

  fn request(&self, request: Value) -> Value {
    let mut stream = UnixStream::connect(&self.socket).unwrap();
    stream
      .set_read_timeout(Some(Duration::from_secs(2)))
      .unwrap();
    writeln!(stream, "{request}").unwrap();
    let mut response = String::new();
    stream.read_to_string(&mut response).unwrap();
    serde_json::from_str(&response).unwrap()
  }

  fn wait_exit(&mut self) {
    let deadline = Instant::now() + Duration::from_secs(3);
    while Instant::now() < deadline {
      if let Some(status) = self.child.try_wait().unwrap() {
        assert!(status.success(), "process exited with {status}");
        assert!(!self.socket.exists(), "socket was not cleaned up");
        return;
      }
      std::thread::sleep(Duration::from_millis(10));
    }
    panic!("process did not exit");
  }

  fn restart(&mut self, profile: &Path) {
    let mut child = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
      .arg("--grpc-port")
      .arg("0")
      .arg("--profile")
      .arg(profile)
      .arg("--journal-dir")
      .arg(self.dir.join("journal"))
      .arg("--socket")
      .arg(&self.socket)
      .stdout(Stdio::piped())
      .stderr(Stdio::piped())
      .spawn()
      .unwrap();
    let mut ready = String::new();
    BufReader::new(child.stdout.take().unwrap())
      .read_line(&mut ready)
      .unwrap();
    let ready: Value = serde_json::from_str(&ready).unwrap();
    assert_eq!(ready["status"], "ready");
    self.grpc_address = ready["grpc_address"].as_str().unwrap().to_string();
    self.child = child;
  }
}

#[test]
fn killed_process_recovers_its_stale_socket() {
  let mut fixture = Fixture::start(&demo_profile());
  fixture.child.kill().unwrap();
  fixture.child.wait().unwrap();
  assert!(fixture.socket.exists());
  fixture.restart(&demo_profile());
  assert_eq!(fixture.request(json!({"op":"discover"}))["ok"], true);
  assert_eq!(fixture.request(json!({"op":"shutdown"}))["ok"], true);
  fixture.wait_exit();
}

impl Drop for Fixture {
  fn drop(&mut self) {
    let _ = self.child.kill();
    let _ = self.child.wait();
    let _ = std::fs::remove_dir_all(&self.dir);
  }
}

fn demo_profile() -> PathBuf {
  Path::new(env!("CARGO_MANIFEST_DIR")).join("../../etc/eos/backends/mock-cxx.toml")
}

fn private_dir(path: &Path) {
  std::fs::create_dir_all(path).unwrap();
  std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o700)).unwrap();
}

fn private_json(path: &Path, value: &Value) {
  std::fs::write(path, serde_json::to_vec(value).unwrap()).unwrap();
  std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600)).unwrap();
}

fn start_managed_process() -> (Fixture, String, String, String) {
  start_managed_process_with_mock_delay(None)
}

fn start_managed_process_with_mock_delay(
  mock_delay_ms: Option<u32>,
) -> (Fixture, String, String, String) {
  start_managed_process_with_graph(mock_delay_ms, "valid_bindings.json")
}

fn start_managed_process_with_graph(
  mock_delay_ms: Option<u32>,
  graph_fixture: &str,
) -> (Fixture, String, String, String) {
  let dir = std::env::temp_dir().join(format!(
    "aether-managed-process-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  private_dir(&dir);
  let revision = format!("{:x}", Sha256::digest(b"saved graph bytes for this test"));
  let bytes = std::fs::read(
    Path::new(env!("CARGO_MANIFEST_DIR"))
      .join("../../tests/fixtures/edge_contract")
      .join(graph_fixture),
  )
  .unwrap();
  let mut manifest = json!({
    "schema_version":2,
    "revision":revision,
    "files":[{"path":"eos/graph.json","sha256":format!("{:x}",Sha256::digest(&bytes))}],
    "launch_inputs":[],"packages":[],"default_roots":null,
    "defaults":[],"secret_refs":[]
  });
  // IsOS computes the generation from the serialized manifest with its
  // generation field omitted, then publishes that digest in the path.
  let generation = format!(
    "{:x}",
    Sha256::digest(serde_json::to_vec(&manifest).unwrap())
  );
  manifest["generation"] = generation.clone().into();
  let v2 = dir.join("data/generated/demo/v2");
  private_dir(&v2);
  let revision_dir = v2.join(&revision);
  private_dir(&revision_dir);
  let leaf = revision_dir.join(&generation);
  private_dir(&leaf);
  private_dir(&leaf.join("eos"));
  let profile = leaf.join("eos/graph.json");
  std::fs::write(&profile, &bytes).unwrap();
  std::fs::set_permissions(&profile, std::fs::Permissions::from_mode(0o600)).unwrap();
  private_json(&leaf.join("manifest.json"), &manifest);
  let egia = "11".repeat(32);
  let operator = "22".repeat(32);
  let verifier_dir = dir.join("control");
  private_dir(&verifier_dir);
  let verifier = verifier_dir.join("verifier.json");
  private_json(
    &verifier,
    &json!({
      "schema_version":1,"run_id":"managed_run_1","active_revision":revision,
      "egia_control_sha256":format!("{:x}",Sha256::digest([0x11;32])),
      "operator_control_sha256":format!("{:x}",Sha256::digest([0x22;32]))
    }),
  );
  let socket = dir.join("eos.sock");
  let mut command = Command::new(env!("CARGO_BIN_EXE_aether-runtime"));
  command.env_remove("EOS_TEST_MOCK_SUBMIT_DELAY_MS");
  #[cfg(feature = "test-hooks")]
  if let Some(delay_ms) = mock_delay_ms {
    command.env("EOS_TEST_MOCK_SUBMIT_DELAY_MS", delay_ms.to_string());
  }
  #[cfg(not(feature = "test-hooks"))]
  let _ = mock_delay_ms;
  let mut child = command
    .arg("--grpc-port")
    .arg("0")
    .arg("--profile")
    .arg(&profile)
    .arg("--journal-dir")
    .arg(dir.join("journal"))
    .arg("--socket")
    .arg(&socket)
    .arg("--run-id")
    .arg("managed_run_1")
    .env("EOS_CONTROL_GRANTS_FILE", &verifier)
    .stdout(Stdio::piped())
    .stderr(Stdio::piped())
    .spawn()
    .unwrap();
  let mut ready = String::new();
  BufReader::new(child.stdout.take().unwrap())
    .read_line(&mut ready)
    .unwrap();
  assert!(!ready.is_empty(), "managed process failed before ready");
  let ready: Value = serde_json::from_str(&ready).unwrap();
  assert_eq!(ready["status"], "ready", "{ready}");
  (
    Fixture {
      dir,
      child,
      socket,
      grpc_address: ready["grpc_address"].as_str().unwrap().into(),
    },
    revision,
    egia,
    operator,
  )
}

#[test]
fn managed_gate_attests_saved_and_projection_revisions_and_exact_jpeg() {
  let (mut fixture, graph_revision, _, _) =
    start_managed_process_with_graph(None, "valid_evidence.json");
  let gate = "eos/interface/gate/front";
  let discovery = fixture.request(json!({"op":"vision_gates"}));
  let projection = discovery["revision"].as_str().unwrap().to_owned();
  assert_ne!(graph_revision, projection);
  let attest = fixture.request(json!({"op":"vision_attest","gate_path":gate}));
  assert_eq!(attest["ok"], true, "{attest}");
  assert_eq!(attest["active_revision"], graph_revision);
  assert_eq!(attest["projection_revision"], projection);
  assert_eq!(attest["run_id"], "managed_run_1");
  assert_eq!(attest["source_path"], "eos/hardware/camera/front");
  let stream = attest["runtime_stream_id"].as_str().unwrap().to_owned();
  assert_eq!(stream.len(), 36);
  let deadline = Instant::now() + Duration::from_secs(3);
  let frame = loop {
    let frame = fixture.request(json!({"op":"vision_frame","gate_path":gate,
      "revision":projection,"after_generation":0,"max_width_px":1280,"jpeg_quality":80}));
    if frame["changed"] == true {
      break frame;
    }
    assert!(Instant::now() < deadline, "managed gate timed out: {frame}");
    std::thread::sleep(Duration::from_millis(10));
  };
  let metadata = &frame["metadata"];
  assert_eq!(metadata["graph_revision"], graph_revision);
  assert_eq!(metadata["projection_revision"], projection);
  assert_eq!(metadata["runtime_stream_id"], stream);
  assert_eq!(metadata["gate_generation"], frame["generation"]);
  assert_eq!(
    metadata["render"],
    json!({"max_width_px":1280,"jpeg_quality":80})
  );
  assert!(metadata["source_frame_id"]
    .as_str()
    .unwrap()
    .starts_with("src-"));
  let jpeg = base64::engine::general_purpose::STANDARD
    .decode(frame["jpeg_base64"].as_str().unwrap())
    .unwrap();
  assert_eq!(
    metadata["jpeg_sha256"],
    format!("{:x}", Sha256::digest(&jpeg))
  );
  assert!(jpeg.starts_with(&[0xff, 0xd8]) && jpeg.ends_with(&[0xff, 0xd9]));
  assert!(serde_json::to_vec(metadata).unwrap().len() <= 4096);
  let stale = fixture.request(json!({"op":"vision_frame","gate_path":gate,
    "revision":graph_revision,"after_generation":0}));
  assert_eq!(stale["ok"], false);
  assert_eq!(fixture.request(json!({"op":"shutdown"}))["ok"], true);
  fixture.wait_exit();
}

#[test]
fn managed_mujoco_routine_stop_reaps_child_and_allows_new_owner_generation() {
  let dir = std::env::temp_dir().join(format!(
    "eos-managed-stop-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  private_dir(&dir);
  let backend_socket = dir.join("sim.sock");
  let source = include_str!("../../../examples/v2/mujoco-isos.toml")
    .replace("/tmp/mujoco-v2.sock", backend_socket.to_str().unwrap())
    .replace(
      "scene = \"default\"\nsocket = ",
      "scene = \"pick-place\"\nstart_timeout_ms = 5000\nsocket = ",
    );
  let bytes = source.as_bytes();
  let revision = format!("{:x}", Sha256::digest(bytes));
  let mut manifest = json!({
    "schema_version":2,"revision":revision,
    "files":[{"path":"eos/graph.json","sha256":revision}],
    "launch_inputs":[],"packages":[],"default_roots":null,
    "defaults":[],"secret_refs":[]
  });
  let generation = format!(
    "{:x}",
    Sha256::digest(serde_json::to_vec(&manifest).unwrap())
  );
  manifest["generation"] = generation.clone().into();
  let v2 = dir.join("data/generated/demo/v2");
  private_dir(&v2);
  let revision_dir = v2.join(&revision);
  private_dir(&revision_dir);
  let leaf = revision_dir.join(&generation);
  private_dir(&leaf);
  private_dir(&leaf.join("eos"));
  let profile = leaf.join("eos/graph.json");
  std::fs::write(&profile, bytes).unwrap();
  std::fs::set_permissions(&profile, std::fs::Permissions::from_mode(0o600)).unwrap();
  private_json(&leaf.join("manifest.json"), &manifest);
  let control = dir.join("control");
  private_dir(&control);
  let verifier = control.join("verifier.json");
  let sim = dir.join("sim.py");
  std::fs::write(
    &sim,
    r##"#!/usr/bin/python3
import json, os, signal, socket, struct, sys, threading, time
assert sys.argv[1] == '--managed-strict-socket'
path = sys.argv[sys.argv.index('--v2-socket')+1]
generation = f'{os.getpid():032x}'
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
def serve(path, stop):
    listener = socket.socket(socket.AF_UNIX)
    listener.bind(path)
    listener.listen(8)
    while True:
        connection, _ = listener.accept()
        with connection:
            header = connection.recv(8)
            if len(header) != 8: continue
            size, payload = struct.unpack('<II', header)
            assert payload == 0
            body = b''
            while len(body) < size: body += connection.recv(size-len(body))
            request = json.loads(body)
            if request['op'] == 'health':
                reply = {'version':'2.0.0','id':request['id'],'ok':True,
                         'accepted':True,'generation_id':generation,
                         'features':['strict-socket-v1','strict-stop-v1']}
            elif stop and request['op'] == 'stop':
                partial = os.environ.get('EOS_TEST_PARTIAL_STOP') == '1'
                reply = {'version':'2.0.0','id':request['id'],'ok':not partial,
                         'accepted':not partial,'generation_id':generation,
                         'stop_epoch':'1','base_stop':'stopped',
                         'arm_stop':'unknown' if partial else 'stopped'}
                if partial:
                    reply['error'] = {'code':'stop_outcome_unknown','message':'arm unknown'}
            else:
                reply = {'version':'2.0.0','id':request['id'],'ok':False,
                         'accepted':False,'error':{'code':'unsupported','message':'test stub'}}
            data = json.dumps(reply).encode()
            connection.sendall(struct.pack('<II',len(data),0)+data)
def delayed_stop_listener():
    time.sleep(float(os.environ.get('EOS_TEST_STOP_BIND_DELAY_S', '0')))
    serve(path+'.stop',True)
if os.environ.get('EOS_TEST_NO_STOP') != '1':
    threading.Thread(target=delayed_stop_listener,daemon=True).start()
serve(path,False)
"##,
  )
  .unwrap();
  std::fs::set_permissions(&sim, std::fs::Permissions::from_mode(0o700)).unwrap();
  let scene = dir.join("assets/scene.xml");
  std::fs::create_dir_all(scene.parent().unwrap()).unwrap();
  std::fs::write(&scene, "<mujoco/>").unwrap();
  let socket = dir.join("eos.sock");
  let journal = dir.join("journal");
  for (index, run_id) in [
    "managed_stop_old",
    "managed_stop_new",
    "managed_stop_partial",
  ]
  .into_iter()
  .enumerate()
  {
    private_json(
      &verifier,
      &json!({"schema_version":1,"run_id":run_id,"active_revision":revision,
        "egia_control_sha256":format!("{:x}",Sha256::digest([index as u8+1;32])),
        "operator_control_sha256":format!("{:x}",Sha256::digest([index as u8+3;32]))}),
    );
    let started = Instant::now();
    let mut child = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
      .args(["--profile", profile.to_str().unwrap()])
      .args(["--journal-dir", journal.to_str().unwrap()])
      .args(["--socket", socket.to_str().unwrap()])
      .args(["--grpc-port", "0"])
      .args(["--run-id", run_id])
      .args([
        "--backend-node",
        "cores/eos/layers/process/groups/backend/nodes/simulator",
      ])
      .args(["--backend-executable", sim.to_str().unwrap()])
      .args(["--backend-scene", scene.to_str().unwrap()])
      .env("EOS_CONTROL_GRANTS_FILE", &verifier)
      .env("EOS_TEST_PARTIAL_STOP", if index == 2 { "1" } else { "0" })
      .env(
        "EOS_TEST_STOP_BIND_DELAY_S",
        if index == 0 { "1.2" } else { "0" },
      )
      .stdout(Stdio::piped())
      .stderr(Stdio::piped())
      .spawn()
      .unwrap();
    let mut stdout = BufReader::new(child.stdout.take().unwrap());
    let mut ready = String::new();
    stdout.read_line(&mut ready).unwrap();
    if ready.is_empty() {
      let mut stderr = String::new();
      child
        .stderr
        .take()
        .unwrap()
        .read_to_string(&mut stderr)
        .unwrap();
      panic!("EOS failed before ready: {stderr}");
    }
    let ready: Value = serde_json::from_str(&ready).unwrap();
    assert_eq!(ready["status"], "ready", "{ready}");
    if index == 0 {
      assert!(
        started.elapsed() >= Duration::from_secs(1),
        "ready before stop proof"
      );
      assert!(PathBuf::from(format!("{}.stop", backend_socket.display())).exists());
    }
    let mut request = UnixStream::connect(&socket).unwrap();
    request.write_all(b"{\"op\":\"shutdown\"}\n").unwrap();
    let mut reply = String::new();
    request.read_to_string(&mut reply).unwrap();
    assert_eq!(serde_json::from_str::<Value>(&reply).unwrap()["ok"], true);
    let deadline = Instant::now() + Duration::from_secs(17);
    let status = loop {
      if let Some(status) = child.try_wait().unwrap() {
        break status;
      }
      assert!(Instant::now() < deadline, "managed EOS did not stop");
      std::thread::sleep(Duration::from_millis(10));
    };
    let mut terminal = String::new();
    stdout.read_to_string(&mut terminal).unwrap();
    assert_eq!(
      status.success(),
      index < 2,
      "unexpected EOS exit: {terminal}"
    );
    let receipt: Value = serde_json::from_str(terminal.trim()).unwrap();
    assert_eq!(receipt["type"], "eos_terminal_stop");
    assert_eq!(
      receipt["outcome"],
      if index == 2 { "unknown" } else { "confirmed" },
      "{receipt}"
    );
    assert_eq!(receipt["run_id"], run_id);
    assert_eq!(receipt["backend_reaped"], true);
    let control: Value =
      serde_json::from_slice(&std::fs::read(journal.join(".control")).unwrap()).unwrap();
    assert_eq!(control["body"]["version"], "1.1.0");
    assert_eq!(control["body"]["shutdown_pending"], index == 2);
    assert_eq!(control["body"]["estop_latched"], false);
    assert!(!socket.exists());
    assert!(!backend_socket.exists());
    assert!(!PathBuf::from(format!("{}.stop", backend_socket.display())).exists());
  }
  private_json(
    &verifier,
    &json!({"schema_version":1,"run_id":"managed_stop_missing",
      "active_revision":revision,
      "egia_control_sha256":format!("{:x}",Sha256::digest([7u8;32])),
      "operator_control_sha256":format!("{:x}",Sha256::digest([9u8;32]))}),
  );
  let missing_started = Instant::now();
  let missing = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
    .args(["--profile", profile.to_str().unwrap()])
    .args([
      "--journal-dir",
      dir.join("missing-journal").to_str().unwrap(),
    ])
    .args(["--socket", socket.to_str().unwrap()])
    .args(["--grpc-port", "0"])
    .args(["--run-id", "managed_stop_missing"])
    .args([
      "--backend-node",
      "cores/eos/layers/process/groups/backend/nodes/simulator",
    ])
    .args(["--backend-executable", sim.to_str().unwrap()])
    .args(["--backend-scene", scene.to_str().unwrap()])
    .env("EOS_CONTROL_GRANTS_FILE", &verifier)
    .env("EOS_TEST_NO_STOP", "1")
    .output()
    .unwrap();
  assert!(
    !missing.status.success(),
    "missing stop lane announced ready"
  );
  assert!(
    missing.stdout.is_empty(),
    "missing stop lane emitted output"
  );
  assert!(missing_started.elapsed() >= Duration::from_secs(5));
  assert!(
    String::from_utf8_lossy(&missing.stderr).contains("\"event_code\":\"eos_start_failed\""),
    "{}",
    String::from_utf8_lossy(&missing.stderr)
  );
  assert!(
    String::from_utf8_lossy(&missing.stderr).contains("\"reason_code\":\"backend_start_failed\""),
    "{}",
    String::from_utf8_lossy(&missing.stderr)
  );
  assert!(!backend_socket.exists());
  assert!(!PathBuf::from(format!("{}.stop", backend_socket.display())).exists());
  std::fs::remove_dir_all(dir).unwrap();
}

#[test]
fn routed_camera_frames_are_gate_specific_and_report_live_status() {
  let source = Path::new(env!("CARGO_MANIFEST_DIR"))
    .join("../../tests/fixtures/edge_contract/valid_evidence.json");
  let mut graph: Value = serde_json::from_slice(&std::fs::read(source).unwrap()).unwrap();
  let gates = graph
    .pointer_mut("/cores/eos/layers/interface/groups/gate/nodes")
    .unwrap()
    .as_object_mut()
    .unwrap();
  let mut slow = gates["front"].clone();
  slow["id"] = "slow".into();
  slow["label"] = "Slow preview".into();
  slow["inputs"]["frame"]["frequency_hz"] = json!(1e-9);
  gates.insert("slow".into(), slow);
  let bytes = serde_json::to_vec(&graph).unwrap();
  let profile_path = std::env::temp_dir().join(format!("r01-profile-{}.json", std::process::id()));
  std::fs::write(&profile_path, &bytes).unwrap();
  let mut fixture = Fixture::start_with_version(&profile_path, "0.1.0");
  std::fs::remove_file(&profile_path).unwrap();

  let revision = format!("{:x}", Sha256::digest(&bytes));
  let status = fixture.request(json!({"op":"route_status"}));
  assert_eq!(status["ok"], true, "{status}");
  assert_eq!(status["projection_sha256"], revision);
  let rows = status["routes"].as_array().unwrap();
  let key = |row: &Value| {
    [
      "source_node",
      "source_output",
      "target_node",
      "target_input",
    ]
    .map(|field| row[field].as_str().unwrap().to_owned())
  };
  assert!(rows.windows(2).all(|pair| key(&pair[0]) <= key(&pair[1])));
  assert!(rows.iter().all(|row| row.get("frame").is_none()
    && row.get("health").is_none()
    && row.get("jpeg_base64").is_none()));
  assert_eq!(
    rows
      .iter()
      .filter(|row| row["delivery_state"] == "active")
      .count(),
    2
  );
  assert!(rows
    .iter()
    .any(|row| row["delivery_state"] == "fixed_not_scheduled"));
  assert!(rows.iter().all(
    |row| row["source_node"].as_str().unwrap().starts_with("eos/")
      && row["target_node"].as_str().unwrap().starts_with("eos/")
  ));
  let fast = rows
    .iter()
    .find(|row| row["target_node"] == "eos/interface/gate/front")
    .unwrap();
  assert_eq!(fast["priority"], -20);
  assert_eq!(fast["frequency_hz"], 15.0);
  assert_eq!(fast["weight"], 40);
  assert_eq!(fast["queue_class"], "latest");

  let frame = |path: &str, after: u64| {
    fixture.request(json!({"op":"vision_frame",
    "gate_path":path,"revision":revision,"after_generation":after}))
  };
  let deadline = Instant::now() + Duration::from_secs(2);
  let (first_fast, first_slow) = loop {
    let fast = frame("eos/interface/gate/front", 0);
    let slow = frame("eos/interface/gate/slow", 0);
    if fast["changed"] == true && slow["changed"] == true {
      break (fast, slow);
    }
    assert!(
      Instant::now() < deadline,
      "route deliveries timed out: {fast}, {slow}"
    );
    std::thread::sleep(Duration::from_millis(10));
  };
  let fast_generation = first_fast["generation"].as_u64().unwrap();
  assert!(first_fast["metadata"].get("graph_revision").is_none());
  assert!(first_fast["metadata"].get("runtime_stream_id").is_none());
  assert_eq!(
    fixture.request(json!({"op":"vision_attest",
    "gate_path":"eos/interface/gate/front"}))["error"],
    "vision_unavailable"
  );
  let slow_generation = first_slow["generation"].as_u64().unwrap();
  let deadline = Instant::now() + Duration::from_secs(2);
  loop {
    let next = frame("eos/interface/gate/front", fast_generation);
    if next["changed"] == true {
      break;
    }
    assert!(
      Instant::now() < deadline,
      "fast route did not advance: {next}"
    );
    std::thread::sleep(Duration::from_millis(10));
  }
  assert_eq!(
    frame("eos/interface/gate/slow", slow_generation)["changed"],
    false
  );
  let status = fixture.request(json!({"op":"route_status"}));
  let fast = status["routes"]
    .as_array()
    .unwrap()
    .iter()
    .find(|row| row["target_node"] == "eos/interface/gate/front")
    .unwrap();
  assert!(fast["delivered_count"].as_u64().unwrap() >= 2);
  assert!(fast["last_delivery_monotonic_ns"].as_u64().is_some());
  let slow = status["routes"]
    .as_array()
    .unwrap()
    .iter()
    .find(|row| row["target_node"] == "eos/interface/gate/slow")
    .unwrap();
  assert_eq!(slow["delivered_count"], 1);
  assert!(slow["cap_wait_count"].as_u64().unwrap() > 0);
  assert_eq!(
    fixture.request(json!({"op":"route_status","unexpected":true}))["ok"],
    false
  );
  let mut stalled_client = UnixStream::connect(&fixture.socket).unwrap();
  stalled_client.write_all(b"{\"op\":").unwrap();
  assert_eq!(fixture.request(json!({"op":"shutdown"}))["ok"], true);
  fixture.wait_exit();
  drop(stalled_client);
}

#[test]
fn legacy_camera_preview_keeps_direct_path_without_live_route() {
  let source = Path::new(env!("CARGO_MANIFEST_DIR"))
    .join("../../tests/fixtures/edge_contract/valid_evidence.json");
  let mut graph: Value = serde_json::from_slice(&std::fs::read(source).unwrap()).unwrap();
  graph["version"] = "1.0.0".into();
  fn legacy_types(value: &mut Value) {
    match value {
      Value::Object(map) => {
        for (key, item) in map {
          if key == "node_type" {
            if let Some(id) = item.as_str() {
              *item = id.replace("@v0.1", "@v1.0").into();
            }
          } else {
            legacy_types(item);
          }
        }
      }
      Value::Array(items) => {
        for item in items {
          legacy_types(item);
        }
      }
      _ => {}
    }
  }
  legacy_types(&mut graph);
  graph["cores"]["eos"]["layers"]["interface"]["groups"]["gate"]["nodes"]["front"]["inputs"]
    ["frame"] = json!({"node":"eos/hardware/camera/front","output":"frame"});
  graph["cores"]["egia"] = json!({"layers":{"common":{"groups":{"process":{"nodes":{
    "guard":{"id":"guard","node_type":"egia.common.guard@v1.0","label":"Guard",
      "description":"Legacy guard validation edge"}
  }}}}}});
  graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["control"]["nodes"]["command"]
    ["inputs"]["request"] = json!({"node":"egia/common/process/guard","output":"request"});
  let bytes = serde_json::to_vec(&graph).unwrap();
  let profile_path =
    std::env::temp_dir().join(format!("r01-legacy-profile-{}.json", std::process::id()));
  std::fs::write(&profile_path, &bytes).unwrap();
  let mut fixture = Fixture::start_with_version(&profile_path, "1.0.0");
  std::fs::remove_file(&profile_path).unwrap();
  let status = fixture.request(json!({"op":"route_status"}));
  assert_eq!(status["ok"], true, "{status}");
  let gate = status["routes"]
    .as_array()
    .unwrap()
    .iter()
    .find(|row| row["target_node"] == "eos/interface/gate/front")
    .unwrap();
  assert_eq!(gate["delivery_state"], "not_routed");
  assert_eq!(gate["routed"], false);
  assert!(status["routes"]
    .as_array()
    .unwrap()
    .iter()
    .all(|row| row["source_node"].as_str().unwrap().starts_with("eos/")));
  let revision = format!("{:x}", Sha256::digest(&bytes));
  let deadline = Instant::now() + Duration::from_secs(2);
  loop {
    let frame = fixture.request(
      json!({"op":"vision_frame","gate_path":"eos/interface/gate/front",
      "revision":revision,"after_generation":0}),
    );
    if frame["changed"] == true {
      break;
    }
    assert!(
      Instant::now() < deadline,
      "legacy preview timed out: {frame}"
    );
    std::thread::sleep(Duration::from_millis(10));
  }
  assert_eq!(fixture.request(json!({"op":"shutdown"}))["ok"], true);
  fixture.wait_exit();
}

#[test]
fn subnormal_camera_rate_starts_and_stops_without_cadence_panic() {
  let source = Path::new(env!("CARGO_MANIFEST_DIR"))
    .join("../../tests/fixtures/edge_contract/valid_evidence.json");
  let mut graph: Value = serde_json::from_slice(&std::fs::read(source).unwrap()).unwrap();
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["camera"]["nodes"]["front"]
    ["observation"]["max_rate_hz"] = json!(f64::from_bits(1));
  let bytes = serde_json::to_vec(&graph).unwrap();
  let profile_path =
    std::env::temp_dir().join(format!("r01-tiny-rate-{}.json", std::process::id()));
  std::fs::write(&profile_path, &bytes).unwrap();
  let mut fixture = Fixture::start_with_version(&profile_path, "0.1.0");
  std::fs::remove_file(&profile_path).unwrap();
  assert_eq!(fixture.request(json!({"op":"route_status"}))["ok"], true);
  assert_eq!(fixture.request(json!({"op":"shutdown"}))["ok"], true);
  fixture.wait_exit();
}

#[test]
fn grpc_health_discovery_remain_available_without_managed_admission() {
  use aether_runtime::grpc::{v1, v2};
  let mut fixture = Fixture::start(&demo_profile());
  let endpoint = format!("http://{}", fixture.grpc_address);
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let channel = tonic::transport::Channel::from_shared(endpoint.clone())
        .unwrap()
        .connect()
        .await
        .unwrap();
      let mut health = tonic_health::pb::health_client::HealthClient::new(channel);
      let reply = health
        .check(tonic_health::pb::HealthCheckRequest {
          service: String::new(),
        })
        .await
        .unwrap()
        .into_inner();
      assert_eq!(
        reply.status,
        tonic_health::pb::health_check_response::ServingStatus::Serving as i32
      );
      let skill_health = health
        .check(tonic_health::pb::HealthCheckRequest {
          service: "island.eos.v1.SkillService".into(),
        })
        .await
        .unwrap()
        .into_inner();
      assert_eq!(
        skill_health.status,
        tonic_health::pb::health_check_response::ServingStatus::Serving as i32
      );
      let mut gateway = v1::gateway_service_client::GatewayServiceClient::connect(endpoint.clone())
        .await
        .unwrap();
      let discover = gateway.discover(()).await.unwrap().into_inner();
      let robot = discover.robot.unwrap();
      assert_eq!(robot.id, "demo-0");
      assert!(!robot.name.is_empty());
      assert_eq!(robot.model, "eos-runtime");
      assert_eq!(discover.devices.len(), 3);
      let status = gateway.get_status(()).await.unwrap().into_inner();
      assert_eq!(
        status.fields["robot_id"].kind,
        Some(prost_types::value::Kind::StringValue(robot.id))
      );
      assert_eq!(
        status.fields["robot_name"].kind,
        Some(prost_types::value::Kind::StringValue(robot.name.clone()))
      );
      assert_eq!(
        gateway
          .list_resources(())
          .await
          .unwrap()
          .into_inner()
          .values
          .len(),
        0
      );
      let camera = discover
        .devices
        .iter()
        .find(|d| d.r#class == "camera")
        .unwrap();
      assert_eq!(
        gateway
          .capture_frames(v1::CaptureFramesRequest {
            robot_name: robot.name.clone(),
            device_id: camera.id.clone(),
            frame_count: 0,
          })
          .await
          .unwrap_err()
          .code(),
        tonic::Code::InvalidArgument
      );
      assert_eq!(
        gateway
          .capture_frames(v1::CaptureFramesRequest {
            robot_name: robot.name.clone(),
            device_id: "missing".into(),
            frame_count: 1,
          })
          .await
          .unwrap_err()
          .code(),
        tonic::Code::NotFound
      );
      assert_eq!(
        gateway
          .capture_frames(v1::CaptureFramesRequest {
            robot_name: "other robot".into(),
            device_id: camera.id.clone(),
            frame_count: 1,
          })
          .await
          .unwrap_err()
          .code(),
        tonic::Code::NotFound
      );
      let capture_deadline = Instant::now() + Duration::from_secs(3);
      let snapshot = loop {
        assert!(
          Instant::now() < capture_deadline,
          "camera cache did not warm"
        );
        match gateway
          .capture_frames(v1::CaptureFramesRequest {
            robot_name: robot.name.clone(),
            device_id: camera.id.clone(),
            frame_count: 2,
          })
          .await
        {
          Ok(snapshot) => break snapshot.into_inner(),
          Err(status) if status.code() == tonic::Code::Unavailable => {
            tokio::time::sleep(Duration::from_millis(20)).await;
          }
          Err(status) => panic!("camera capture failed: {status}"),
        }
      };
      assert_eq!(snapshot.r#type, "vision.snapshot");
      assert_eq!(snapshot.schema_version, "1.0");
      assert_eq!(snapshot.device_id, camera.id);
      assert_eq!(snapshot.frames.len(), 2);
      for frame in &snapshot.frames {
        assert!(frame.jpeg.starts_with(&[0xff, 0xd8]));
        assert!(frame.jpeg.ends_with(&[0xff, 0xd9]));
        assert!(frame.width_px > 0 && frame.height_px > 0);
        chrono::DateTime::parse_from_rfc3339(&frame.captured_at).unwrap();
      }
      assert_ne!(
        snapshot.frames[0].captured_at,
        snapshot.frames[1].captured_at
      );
      let mut capabilities =
        v2::capability_service_client::CapabilityServiceClient::connect(endpoint)
          .await
          .unwrap();
      let catalog = capabilities
        .list_capabilities(v2::ListCapabilitiesRequest {})
        .await
        .unwrap()
        .into_inner()
        .capabilities;
      assert_eq!(catalog.len(), 4);
      let velocity = catalog
        .iter()
        .find(|entry| entry.primitive == "base.velocity")
        .unwrap();
      assert!(!velocity.available);
      assert!(catalog
        .iter()
        .any(|entry| entry.primitive == "safety.estop"));
      assert!(!catalog.iter().any(|entry| entry.primitive == "robot.stop"));
      let base = discover
        .devices
        .iter()
        .find(|d| d.r#class == "base")
        .unwrap();
      assert_eq!(base.primitives.len(), 1);
      let arm = discover
        .devices
        .iter()
        .find(|d| d.r#class == "manipulator")
        .unwrap();
      assert_eq!(arm.primitives.len(), 2);
      let arguments = prost_types::Struct {
        fields: [
          (
            "linear_x_m_s".into(),
            prost_types::Value {
              kind: Some(prost_types::value::Kind::NumberValue(0.1)),
            },
          ),
          (
            "linear_y_m_s".into(),
            prost_types::Value {
              kind: Some(prost_types::value::Kind::NumberValue(0.0)),
            },
          ),
          (
            "angular_z_rad_s".into(),
            prost_types::Value {
              kind: Some(prost_types::value::Kind::NumberValue(0.0)),
            },
          ),
          (
            "valid_for_ms".into(),
            prost_types::Value {
              kind: Some(prost_types::value::Kind::NumberValue(100.0)),
            },
          ),
        ]
        .into(),
      };
      let submit = v2::SubmitOperationRequest {
        capability_name: velocity.name.clone(),
        arguments: Some(arguments.clone()),
        operation_id: "grpc-motion-1".into(),
      };
      assert_eq!(
        capabilities
          .submit_operation(submit.clone())
          .await
          .unwrap_err()
          .code(),
        tonic::Code::Unavailable
      );
      let unavailable = capabilities
        .list_capabilities(v2::ListCapabilitiesRequest {})
        .await
        .unwrap()
        .into_inner();
      assert!(
        !unavailable
          .capabilities
          .iter()
          .find(|entry| entry.primitive == "base.velocity")
          .unwrap()
          .available
      );
      assert!(
        !unavailable
          .capabilities
          .iter()
          .find(|entry| entry.primitive == "safety.estop")
          .unwrap()
          .available
      );
      assert_eq!(
        capabilities
          .submit_operation(submit.clone())
          .await
          .unwrap_err()
          .code(),
        tonic::Code::Unavailable
      );
      let mut later = submit.clone();
      later.operation_id = "grpc-motion-2".into();
      assert_eq!(
        capabilities
          .submit_operation(later)
          .await
          .unwrap_err()
          .code(),
        tonic::Code::Unavailable
      );
      let mut conflict = submit;
      conflict
        .arguments
        .as_mut()
        .unwrap()
        .fields
        .get_mut("linear_x_m_s")
        .unwrap()
        .kind = Some(prost_types::value::Kind::NumberValue(0.2));
      assert_eq!(
        capabilities
          .submit_operation(conflict)
          .await
          .unwrap_err()
          .code(),
        tonic::Code::Unavailable
      );
      assert_eq!(
        capabilities
          .get_operation(v2::OperationRequest {
            operation_id: "grpc-motion-1".into()
          })
          .await
          .unwrap_err()
          .code(),
        tonic::Code::Unavailable
      );
      let mut skills = v1::skill_service_client::SkillServiceClient::connect(format!(
        "http://{}",
        fixture.grpc_address
      ))
      .await
      .unwrap();
      let optional = skills
        .list_skills(v1::ListSkillsRequest::default())
        .await
        .unwrap()
        .into_inner();
      assert!(optional.skills.is_empty());
    });
  assert_eq!(fixture.request(json!({"op":"shutdown"}))["ok"], true);
  fixture.wait_exit();
}

#[test]
fn managed_process_requires_grant_and_saved_revision_for_movement() {
  use aether_runtime::grpc::{v1, v2};
  use tonic::{Code, Request};

  let (mut fixture, revision, egia, operator) = start_managed_process();
  let endpoint = format!("http://{}", fixture.grpc_address);
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut gateway = v1::gateway_service_client::GatewayServiceClient::connect(endpoint.clone())
        .await
        .unwrap();
      let discovery = gateway.discover(()).await.unwrap();
      assert_eq!(
        discovery
          .metadata()
          .get("eos-active-revision")
          .unwrap()
          .to_str()
          .unwrap(),
        revision
      );
      let projection = discovery
        .metadata()
        .get("eos-projection-sha256")
        .unwrap()
        .to_str()
        .unwrap();
      assert_eq!(projection.len(), 64);
      assert_ne!(projection, revision);
      let mut capabilities =
        v2::capability_service_client::CapabilityServiceClient::connect(endpoint)
          .await
          .unwrap();
      let catalog = capabilities
        .list_capabilities(v2::ListCapabilitiesRequest {})
        .await
        .unwrap()
        .into_inner();
      let velocity = catalog
        .capabilities
        .iter()
        .find(|capability| capability.primitive == "base.velocity")
        .unwrap();
      let submit = v2::SubmitOperationRequest {
        capability_name: velocity.name.clone(),
        arguments: Some(prost_types::Struct {
          fields: [
            (
              "linear_x_m_s".into(),
              prost_types::Value {
                kind: Some(prost_types::value::Kind::NumberValue(0.1)),
              },
            ),
            (
              "linear_y_m_s".into(),
              prost_types::Value {
                kind: Some(prost_types::value::Kind::NumberValue(0.0)),
              },
            ),
            (
              "angular_z_rad_s".into(),
              prost_types::Value {
                kind: Some(prost_types::value::Kind::NumberValue(0.0)),
              },
            ),
            (
              "valid_for_ms".into(),
              prost_types::Value {
                kind: Some(prost_types::value::Kind::NumberValue(100.0)),
              },
            ),
          ]
          .into(),
        }),
        operation_id: "managed-movement-1".into(),
      };
      assert_eq!(
        capabilities
          .submit_operation(submit.clone())
          .await
          .unwrap_err()
          .code(),
        Code::Unauthenticated
      );
      let authorized = |grant: &str, active_revision: Option<&str>| {
        let mut request = Request::new(submit.clone());
        request
          .metadata_mut()
          .insert("authorization", format!("Bearer {grant}").parse().unwrap());
        if let Some(active_revision) = active_revision {
          request
            .metadata_mut()
            .insert("x-eos-active-revision", active_revision.parse().unwrap());
        }
        request
      };
      assert_eq!(
        capabilities
          .submit_operation(authorized(&"33".repeat(32), Some(&revision)))
          .await
          .unwrap_err()
          .code(),
        Code::Unauthenticated
      );
      assert_eq!(
        capabilities
          .submit_operation(authorized(&egia, None))
          .await
          .unwrap_err()
          .code(),
        Code::InvalidArgument
      );
      assert_eq!(
        capabilities
          .submit_operation(authorized(&egia, Some(&"44".repeat(32))))
          .await
          .unwrap_err()
          .code(),
        Code::FailedPrecondition
      );
      assert_eq!(
        capabilities
          .submit_operation(authorized(&operator, Some(&revision)))
          .await
          .unwrap_err()
          .code(),
        Code::PermissionDenied
      );
      let accepted = capabilities
        .submit_operation(authorized(&egia, Some(&revision)))
        .await
        .unwrap()
        .into_inner();
      assert_eq!(accepted.operation_id, "managed-movement-1");
      assert_eq!(accepted.accepted, Some(true));
      assert_eq!(accepted.status, v2::OperationStatus::OutcomeUnknown as i32);
      let read_as = |grant: &str, operation_id: &str| {
        let mut request = Request::new(v2::OperationRequest {
          operation_id: operation_id.into(),
        });
        request
          .metadata_mut()
          .insert("authorization", format!("Bearer {grant}").parse().unwrap());
        request
          .metadata_mut()
          .insert("x-eos-active-revision", revision.parse().unwrap());
        request
      };
      let other_role = capabilities
        .get_operation(read_as(&operator, "managed-movement-1"))
        .await
        .unwrap_err();
      let unknown = capabilities
        .get_operation(read_as(&operator, "unknown-operation"))
        .await
        .unwrap_err();
      assert_eq!(other_role.code(), Code::NotFound);
      assert_eq!(other_role.message(), unknown.message());
      assert_eq!(
        capabilities
          .get_operation(v2::OperationRequest {
            operation_id: "managed-movement-1".into()
          })
          .await
          .unwrap_err()
          .code(),
        Code::Unauthenticated
      );
    });
  assert_eq!(fixture.request(json!({"op":"dispatch"}))["ok"], false);
  assert_eq!(fixture.request(json!({"op":"shutdown"}))["ok"], true);
  fixture.wait_exit();
}

#[test]
fn managed_private_result_requires_operator_revision_and_browser_admission() {
  let (mut fixture, revision, egia, operator) = start_managed_process();
  let raw = fixture.request(json!({"op":"result","command_id":"browser-one"}));
  assert_eq!(raw["error"], "operation has no governed managed route");
  let query = |id: &str, grant: &str, active: &str| {
    json!({"op":"primitive_result","command_id":id,
      "active_revision":active,"authority_grant":grant})
  };
  assert_eq!(
    fixture.request(query("browser-one", &egia, &revision))["error"],
    "operator control grant required"
  );
  assert_eq!(
    fixture.request(query("browser-one", &operator, &"b".repeat(64)))["error"],
    "stale active revision"
  );
  assert_eq!(
    fixture.request(query("browser-one", &operator, &revision))["error"],
    "unknown command ID"
  );
  let invalid = json!({"op":"primitive_result","command_id":"browser-one",
    "active_revision":revision,"authority_grant":operator,"target_path":"forged"});
  assert_eq!(fixture.request(invalid)["error"], "invalid result request");
  let mut duplicate = UnixStream::connect(&fixture.socket).unwrap();
  writeln!(
    duplicate,
    "{{\"op\":\"primitive_result\",\"command_id\":\"browser-one\",\"command_id\":\"other\",\"active_revision\":\"{revision}\",\"authority_grant\":\"{operator}\"}}"
  )
  .unwrap();
  let mut duplicate_reply = String::new();
  duplicate.read_to_string(&mut duplicate_reply).unwrap();
  let duplicate_reply: Value = serde_json::from_str(&duplicate_reply).unwrap();
  assert_eq!(duplicate_reply["error"], "invalid result request");
  let payload = json!({"type":"body_velocity","x_m_s":0.1,"y_m_s":0.0,
    "yaw_rad_s":0.0,"valid_for_ms":100});
  let now = fixture.request(json!({"op":"time"}))["monotonic_ns"]
    .as_u64()
    .unwrap();
  let typed = json!({"op":"primitive_typed","authority_grant":operator,
    "header":{
      "data_type_id":"base_movement","data_type_version":"1.0",
      "primitive_name":"base.velocity","primitive_version":"1.0.0",
      "active_revision":revision,
      "source_path":"eos/dispatcher/base/velocity",
      "target_path":"eos/hardware/base/drive",
      "command_id":"browser-one","sequence":1,
      "deadline_monotonic_ns":now + 2_000_000_000_u64,
      "payload_length_bytes":serde_json::to_vec(&payload).unwrap().len(),
      "codec":"json_v1"},"payload":payload});
  let mut excessive = typed.clone();
  excessive["header"]["command_id"] = json!("browser-static-limit");
  excessive["payload"]["x_m_s"] = json!(1_000_000.0);
  excessive["header"]["payload_length_bytes"] =
    json!(serde_json::to_vec(&excessive["payload"]).unwrap().len());
  let rejected = fixture.request(excessive);
  assert_eq!(rejected["ok"], false, "{rejected}");
  assert_eq!(
    fixture.request(query("browser-static-limit", &operator, &revision))["error"],
    "unknown command ID"
  );
  let submitted = fixture.request(typed);
  assert_eq!(submitted["ok"], true, "{submitted}");
  let returned = fixture.request(query("browser-one", &operator, &revision));
  assert_eq!(returned["ok"], true, "{returned}");
  assert_eq!(returned["result"]["command_id"], "browser-one");
  assert_eq!(submitted["result"]["command_id"], "browser-one");
  let mut terminal = returned;
  for _ in 0..100 {
    if terminal["result"]["status"] != "InProgress" {
      break;
    }
    std::thread::sleep(Duration::from_millis(10));
    terminal = fixture.request(query("browser-one", &operator, &revision));
  }
  assert_eq!(terminal["result"]["status"], "OutcomeUnknown", "{terminal}");
  assert_eq!(
    fixture.request(query("browser-one", &egia, &revision))["error"],
    "operator control grant required"
  );
  assert_eq!(fixture.request(json!({"op":"shutdown"}))["ok"], true);
  fixture.wait_exit();
}

#[cfg(feature = "test-hooks")]
#[test]
fn managed_delayed_mock_acknowledges_prepared_before_terminal_result() {
  let (mut fixture, revision, _egia, operator) = start_managed_process_with_mock_delay(Some(600));
  let payload = json!({"type":"body_velocity","x_m_s":0.1,"y_m_s":0.0,
    "yaw_rad_s":0.0,"valid_for_ms":100});
  let now = fixture.request(json!({"op":"time"}))["monotonic_ns"]
    .as_u64()
    .unwrap();
  let typed = json!({"op":"primitive_typed","authority_grant":operator,
    "header":{
      "data_type_id":"base_movement","data_type_version":"1.0",
      "primitive_name":"base.velocity","primitive_version":"1.0.0",
      "active_revision":revision,
      "source_path":"eos/dispatcher/base/velocity",
      "target_path":"eos/hardware/base/drive",
      "command_id":"browser-delayed","sequence":1,
      "deadline_monotonic_ns":now + 2_000_000_000_u64,
      "payload_length_bytes":serde_json::to_vec(&payload).unwrap().len(),
      "codec":"json_v1"},"payload":payload});
  let submitted = fixture.request(typed.clone());
  assert_eq!(submitted["ok"], true, "{submitted}");
  assert_eq!(submitted["result"]["status"], "InProgress", "{submitted}");
  let query = || {
    json!({"op":"primitive_result","command_id":"browser-delayed",
    "active_revision":revision,"authority_grant":operator})
  };
  let pending = fixture.request(query());
  assert_eq!(pending["result"]["status"], "InProgress", "{pending}");
  let mut terminal = pending;
  for _ in 0..100 {
    if terminal["result"]["status"] != "InProgress" {
      break;
    }
    std::thread::sleep(Duration::from_millis(10));
    terminal = fixture.request(query());
  }
  assert_eq!(terminal["result"]["status"], "OutcomeUnknown", "{terminal}");
  let duplicate = fixture.request(typed);
  assert_eq!(
    duplicate["result"]["command_id"], "browser-delayed",
    "{duplicate}"
  );
  assert_eq!(
    duplicate["result"]["status"], "OutcomeUnknown",
    "{duplicate}"
  );
  assert_eq!(fixture.request(json!({"op":"shutdown"}))["ok"], true);
  fixture.wait_exit();
}

#[test]
fn unregistered_managed_navigation_binding_fails_profile_validation() {
  use aether_hal::Profile;

  let source = Path::new(env!("CARGO_MANIFEST_DIR"))
    .join("../../tests/fixtures/edge_contract/valid_bindings.json");
  let mut graph: Value = serde_json::from_slice(&std::fs::read(source).unwrap()).unwrap();
  let base = graph
    .pointer_mut("/cores/eos/layers/hardware/groups/base/nodes/drive")
    .unwrap();
  base["capabilities"] = json!(["relative_pose"]);
  base["movement"]["modes"] = json!(["relative_pose"]);
  let control = graph
    .pointer_mut("/cores/eos/layers/dispatcher/groups/base/nodes/velocity/settings")
    .unwrap();
  control["primitive_name"] = json!("base.navigate");
  control["movement_type"] = json!("relative_pose");
  let error = Profile::from_json(&graph.to_string()).unwrap_err();
  assert!(
    error
      .to_string()
      .contains("setting primitive_name has invalid type or value"),
    "{error}"
  );
}

#[test]
fn selected_managed_profile_without_verifier_fails_before_listener() {
  let (mut fixture, revision, _, _) = start_managed_process();
  assert_eq!(fixture.request(json!({"op":"shutdown"}))["ok"], true);
  fixture.wait_exit();
  let revision_dir = fixture.dir.join("data/generated/demo/v2").join(&revision);
  let generation_dir = std::fs::read_dir(revision_dir)
    .unwrap()
    .next()
    .unwrap()
    .unwrap()
    .path();
  let missing_verifier_socket = fixture.dir.join("missing-verifier.sock");
  let rejected = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
    .arg("--grpc-port")
    .arg("0")
    .arg("--profile")
    .arg(generation_dir.join("eos/graph.json"))
    .arg("--journal-dir")
    .arg(fixture.dir.join("second-journal"))
    .arg("--socket")
    .arg(&missing_verifier_socket)
    .arg("--run-id")
    .arg("managed_run_1")
    .env_remove("EOS_CONTROL_GRANTS_FILE")
    .output()
    .unwrap();
  assert!(!rejected.status.success());
  assert!(!missing_verifier_socket.exists());
  assert!(!String::from_utf8_lossy(&rejected.stdout).contains("\"status\":\"ready\""));

  let verifier = fixture.dir.join("control/verifier.json");
  let mut tampered: Value = serde_json::from_slice(&std::fs::read(&verifier).unwrap()).unwrap();
  tampered["active_revision"] = "00".repeat(32).into();
  private_json(&verifier, &tampered);
  let mismatch_socket = fixture.dir.join("mismatch-verifier.sock");
  let rejected = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
    .arg("--grpc-port")
    .arg("0")
    .arg("--profile")
    .arg(generation_dir.join("eos/graph.json"))
    .arg("--journal-dir")
    .arg(fixture.dir.join("third-journal"))
    .arg("--socket")
    .arg(&mismatch_socket)
    .arg("--run-id")
    .arg("managed_run_1")
    .env("EOS_CONTROL_GRANTS_FILE", &verifier)
    .output()
    .unwrap();
  assert!(!rejected.status.success());
  assert!(!mismatch_socket.exists());
  assert!(!String::from_utf8_lossy(&rejected.stdout).contains("\"status\":\"ready\""));
}

#[test]
fn setup_projection_starts_runtime_with_new_graph_version() {
  let graph = json!({
    "version":"0.1.0","robot":{"id":"robot","name":"Robot"},
    "cores":{
      "eos":{"layers":{"dispatcher":{"groups":{"control":{"nodes":{
        "command":{"id":"command","node_type":"eos.dispatcher.control@v0.1",
          "label":"Dispatcher","description":"Command admission"}
      }}}}}},
      "isos":{"layers":{"process":{"groups":{"component":{"nodes":{
        "eos":{"id":"eos","node_type":"isos.process.component@v0.1",
          "label":"EOS process","description":"IsOS-owned process"}
      }}}}}}
    }
  });
  let request = json!({"schema_version":1,"op":"render","core":"eos",
    "revision":"test-revision","graph":graph,"endpoints":{},"packages":[]});
  let mut setup = Command::new(env!("CARGO_BIN_EXE_aether-profile"))
    .args(["setup", "--json"])
    .stdin(Stdio::piped())
    .stdout(Stdio::piped())
    .spawn()
    .unwrap();
  setup
    .stdin
    .take()
    .unwrap()
    .write_all(request.to_string().as_bytes())
    .unwrap();
  let output = setup.wait_with_output().unwrap();
  assert!(output.status.success());
  let response: Value = serde_json::from_slice(&output.stdout).unwrap();
  assert_eq!(response["ok"], true, "{response}");
  let projected = response["files"][0]["content"].as_str().unwrap();
  let dir = std::env::temp_dir().join(format!(
    "aether-runtime-projection-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  std::fs::create_dir(&dir).unwrap();
  let path = dir.join("eos.json");
  std::fs::write(&path, projected).unwrap();
  let socket = dir.join("eos.sock");
  let mut child = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
    .arg("--grpc-port")
    .arg("0")
    .arg("--profile")
    .arg(&path)
    .arg("--journal-dir")
    .arg(dir.join("journal"))
    .arg("--socket")
    .arg(&socket)
    .stdout(Stdio::piped())
    .stderr(Stdio::piped())
    .spawn()
    .unwrap();
  let stdout = child.stdout.take().unwrap();
  let (sender, receiver) = mpsc::channel();
  let reader = std::thread::spawn(move || {
    let mut ready = String::new();
    let read = BufReader::new(stdout).read_line(&mut ready);
    let _ = sender.send((read, ready));
  });
  let (read, ready) = match receiver.recv_timeout(Duration::from_secs(3)) {
    Ok(value) => value,
    Err(error) => {
      let _ = child.kill();
      let _ = child.wait();
      reader.join().unwrap();
      panic!("runtime readiness timed out: {error}");
    }
  };
  read.unwrap();
  reader.join().unwrap();
  assert!(!ready.is_empty(), "runtime failed before ready");
  let ready: Value = serde_json::from_str(&ready).unwrap();
  assert_eq!(ready["version"], "0.1.0");
  assert_eq!(ready["status"], "ready");
  let grpc_address = ready["grpc_address"].as_str().unwrap().to_string();
  let mut fixture = Fixture {
    dir,
    child,
    socket,
    grpc_address,
  };
  assert_eq!(fixture.request(json!({"op":"shutdown"}))["ok"], true);
  fixture.wait_exit();
}

#[test]
fn process_discovers_isolated_hardware_and_rejects_raw_stop() {
  let mut fixture = Fixture::start(&demo_profile());
  let nodes = fixture.request(json!({"op":"discover"}));
  assert_eq!(nodes["ok"], true);
  assert_eq!(nodes["nodes"].as_array().unwrap().len(), 3);
  let base_path = "cores/eos/layers/hardware/groups/base/nodes/drive";
  assert_eq!(
    fixture.request(json!({"op":"health","node_path":base_path}))["ok"],
    true
  );
  assert_eq!(
    fixture.request(json!({"op":"stop","node_path":base_path}))["ok"],
    false
  );
  assert_eq!(
    fixture.request(json!({"op":"status","node_path":base_path}))["ok"],
    true
  );
  assert_eq!(
    fixture.request(json!({"op":"observe","node_path":base_path}))["ok"],
    true
  );
  assert_eq!(
    fixture.request(json!({"op":"health","node_path":"drive"}))["ok"],
    false
  );
  assert_eq!(fixture.request(json!({"op":"dispatch"}))["ok"], false);
  let mut oversized = UnixStream::connect(&fixture.socket).unwrap();
  oversized
    .set_read_timeout(Some(Duration::from_secs(2)))
    .unwrap();
  oversized.write_all(&vec![b'x'; 65 * 1024]).unwrap();
  let mut reply = String::new();
  oversized.read_to_string(&mut reply).unwrap();
  assert_eq!(serde_json::from_str::<Value>(&reply).unwrap()["ok"], false);
  assert_eq!(fixture.request(json!({"op":"shutdown"}))["ok"], true);
  fixture.wait_exit();
}

#[test]
fn shutdown_is_served_while_another_client_is_stalled() {
  let mut fixture = Fixture::start(&demo_profile());
  let mut stalled = UnixStream::connect(&fixture.socket).unwrap();
  stalled.write_all(b"{\"op\":").unwrap();
  std::thread::sleep(Duration::from_millis(30));
  let started = Instant::now();
  let response = fixture.request(json!({"op":"shutdown"}));
  assert_eq!(response["ok"], true);
  assert!(started.elapsed() < Duration::from_millis(1500));
  drop(stalled);
  fixture.wait_exit();
}

#[test]
fn signal_triggers_graceful_stop_and_socket_cleanup() {
  let mut fixture = Fixture::start(&demo_profile());
  unsafe { libc::kill(fixture.child.id() as libc::pid_t, libc::SIGTERM) };
  fixture.wait_exit();
}

#[test]
fn startup_rejects_non_cxx_adapter_before_binding() {
  let source = std::fs::read_to_string(demo_profile()).unwrap();
  let dir = std::env::temp_dir().join(format!("aether-runtime-reject-{}", std::process::id()));
  std::fs::create_dir_all(&dir).unwrap();
  let profile = dir.join("bad.toml");
  let bad_source = source.replace(
    "[cores.eos.layers.hardware.groups.manipulator.nodes.arm.adapter]\ntype = \"mock-cxx\"",
    "[cores.eos.layers.hardware.groups.manipulator.nodes.arm.adapter]\ntype = \"mock\"",
  );
  assert_ne!(bad_source, source);
  std::fs::write(&profile, bad_source).unwrap();
  let socket = dir.join("bad.sock");
  let result = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
    .arg("--grpc-port")
    .arg("0")
    .args([
      "--profile",
      profile.to_str().unwrap(),
      "--journal-dir",
      dir.join("journal").to_str().unwrap(),
      "--socket",
      socket.to_str().unwrap(),
    ])
    .output()
    .unwrap();
  assert!(!result.status.success());
  assert!(!socket.exists());
  let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn raw_dispatch_and_result_remain_unavailable_across_process_restart() {
  let profile = demo_profile();
  let mut fixture = Fixture::start(&profile);
  let clock = fixture.request(json!({"op":"time"}));
  assert_eq!(clock["ok"], true);
  let deadline = clock["monotonic_ns"].as_u64().unwrap() + 2_000_000_000;
  let command = json!({
      "op":"dispatch",
      "node_path":"cores/eos/layers/hardware/groups/base/nodes/drive",
      "command_id":"process-command-1",
      "deadline_monotonic_ns":deadline,
      "movement":{"type":"body_velocity","x_m_s":0.2,"y_m_s":0.0,
                  "yaw_rad_s":0.0,"valid_for_ms":100}
  });
  let mut invalid = command.clone();
  invalid["command_id"] = json!("invalid-shape");
  invalid["movement"]["unexpected"] = json!(true);
  assert_eq!(fixture.request(invalid)["ok"], false);
  assert_eq!(
    fixture.request(json!({"op":"result","command_id":"invalid-shape"}))["ok"],
    false
  );
  let first = fixture.request(command.clone());
  assert_eq!(first["ok"], false);
  assert_eq!(
    fixture.request(json!({"op":"result","command_id":"process-command-1"}))["ok"],
    false
  );
  assert_eq!(fixture.request(json!({"op":"shutdown"}))["ok"], true);
  fixture.wait_exit();
  fixture.restart(&profile);
  assert_eq!(fixture.request(command.clone())["ok"], false);
  let mut conflict = command;
  conflict["deadline_monotonic_ns"] = json!(deadline + 1);
  let conflicting = fixture.request(conflict);
  assert_eq!(conflicting["ok"], false);
  assert_eq!(
    fixture.request(json!({"op":"result","command_id":"process-command-1"}))["ok"],
    false
  );
  fixture.request(json!({"op":"shutdown"}));
  fixture.wait_exit();
}

#[test]
fn journal_rejects_second_process_owner() {
  let fixture = Fixture::start(&demo_profile());
  let second = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
    .arg("--grpc-port")
    .arg("0")
    .arg("--profile")
    .arg(demo_profile())
    .arg("--journal-dir")
    .arg(fixture.dir.join("journal"))
    .arg("--socket")
    .arg(fixture.dir.join("second.sock"))
    .output()
    .unwrap();
  assert!(!second.status.success());
  assert!(!fixture.dir.join("second.sock").exists());
  assert!(fixture.request(json!({"op":"discover"}))["ok"]
    .as_bool()
    .unwrap());
}

#[test]
fn invalid_profile_version_fails_before_socket_bind() {
  let source = std::fs::read_to_string(demo_profile()).unwrap();
  let dir = std::env::temp_dir().join(format!("aether-runtime-invalid-{}", std::process::id()));
  std::fs::create_dir_all(&dir).unwrap();
  let profile = dir.join("invalid.toml");
  std::fs::write(
    &profile,
    source.replacen("version = \"1.0.0\"", "version = \"2.0.0\"", 1),
  )
  .unwrap();
  let socket = dir.join("bad.sock");
  let result = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
    .arg("--grpc-port")
    .arg("0")
    .arg("--profile")
    .arg(&profile)
    .arg("--journal-dir")
    .arg(dir.join("journal"))
    .arg("--socket")
    .arg(&socket)
    .output()
    .unwrap();
  assert!(!result.status.success());
  assert!(!socket.exists());
  let _ = std::fs::remove_dir_all(dir);
}
