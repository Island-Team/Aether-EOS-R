use std::io::{BufRead, BufReader, Read};
use std::os::unix::fs::PermissionsExt;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

use aether_runtime::grpc::v2;
use serde_json::Value;

const BACKEND_NODE: &str = "cores/eos/layers/process/groups/backend/nodes/simulator";

fn velocity_arguments() -> prost_types::Struct {
  let number = |value| prost_types::Value {
    kind: Some(prost_types::value::Kind::NumberValue(value)),
  };
  prost_types::Struct {
    fields: [
      ("linear_x_m_s".into(), number(0.1)),
      ("linear_y_m_s".into(), number(0.0)),
      ("angular_z_rad_s".into(), number(0.0)),
      ("valid_for_ms".into(), number(100.0)),
    ]
    .into(),
  }
}

struct Fixture {
  dir: PathBuf,
  owner: Child,
  grpc_address: String,
  backend_pid: libc::pid_t,
}

impl Fixture {
  fn start() -> Self {
    let dir = std::env::temp_dir().join(format!(
      "r02-backend-death-{}-{}",
      std::process::id(),
      std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos()
    ));
    std::fs::create_dir(&dir).unwrap();
    std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o700)).unwrap();
    let backend_socket = dir.join("sim.sock");
    let profile = dir.join("graph.toml");
    let mut source = include_str!(concat!(
      env!("CARGO_MANIFEST_DIR"),
      "/../../etc/eos/backends/mock-cxx.toml"
    ))
    .to_owned();
    source = source.replace(
      "capabilities = [\"rgb8\", \"aligned_depth_u16\"]",
      &format!("capabilities = [\"rgb8\"]\nreferences = [\"{BACKEND_NODE}\"]"),
    );
    source = source.replacen(
      "type = \"mock-cxx\"",
      &format!(
        "type = \"mujoco-v2\"\n\
         [cores.eos.layers.hardware.groups.camera.nodes.front.adapter.settings]\n\
         socket_path = \"{}\"\nsimulator_node = \"cam-0\"\n\
         device_id = \"cam-0\"\nbackend = \"{BACKEND_NODE}\"",
        backend_socket.display()
      ),
      1,
    );
    source.push_str(&format!(
      "\n[cores.eos.layers.process.groups.backend.nodes.simulator]\n\
       id = \"simulator\"\n\
       node_type = \"eos.process.backend@v1.0\"\n\
       label = \"Test backend child\"\n\
       description = \"Direct child shutdown boundary\"\n\
       capabilities = [\"process\"]\n\
       [cores.eos.layers.process.groups.backend.nodes.simulator.settings]\n\
       package = \"island/mujoco-sim\"\n\
       version = \"0.1.1\"\n\
       scene = \"pick-place\"\n\
       socket = \"{}\"\n",
      backend_socket.display()
    ));
    let registry = aether_hal::NodeTypeRegistry::with_contribution_files(&[]).unwrap();
    assert!(
      aether_hal::Profile::from_toml_with_registry(&source, &registry).is_ok(),
      "invalid stub profile: {:?}",
      aether_hal::Profile::from_toml_with_registry(&source, &registry).err()
    );
    std::fs::write(&profile, source).unwrap();
    let executable = dir.join("sim.py");
    std::fs::write(
      &executable,
      "#!/usr/bin/python3\nimport json, os, socket, struct, sys, time\n".to_owned()
        + "assert sys.argv[1] == '--managed-strict-socket'\n"
        + "path = sys.argv[sys.argv.index('--v2-socket') + 1]\n"
        + "scene = sys.argv[sys.argv.index('--scene') + 1]\n"
        + "server = socket.socket(socket.AF_UNIX)\n"
        + "server.bind(path)\nserver.listen(1)\n"
        + "with open(scene + '.pid', 'w', encoding='ascii') as out:\n"
        + "    out.write(str(os.getpid()))\n"
        + "conn,_=server.accept()\nheader=conn.recv(8)\nn,_=struct.unpack('<II',header)\n"
        + "data=b''\nwhile len(data)<n: data+=conn.recv(n-len(data))\n"
        + "request=json.loads(data)\nassert request['op']=='health' and request['node']=='cam-0'\n"
        + "reply=json.dumps({'version':'2.0.0','id':request['id'],'ok':True,"
        + "'accepted':True,'features':['strict-socket-v1']}).encode()\n"
        + "conn.sendall(struct.pack('<II',len(reply),0)+reply)\nconn.close()\n"
        + "while True: time.sleep(0.1)\n",
    )
    .unwrap();
    std::fs::set_permissions(&executable, std::fs::Permissions::from_mode(0o700)).unwrap();
    let scene = dir.join("assets/scene.xml");
    std::fs::create_dir_all(scene.parent().unwrap()).unwrap();
    std::fs::write(&scene, "<mujoco/>").unwrap();
    let mut owner = Command::new(env!("CARGO_BIN_EXE_aether-runtime"))
      .args(["--profile", profile.to_str().unwrap()])
      .args(["--journal-dir", dir.join("journal").to_str().unwrap()])
      .args(["--socket", dir.join("eos.sock").to_str().unwrap()])
      .args(["--grpc-port", "0"])
      .args(["--backend-node", BACKEND_NODE])
      .args(["--backend-executable", executable.to_str().unwrap()])
      .args(["--backend-scene", scene.to_str().unwrap()])
      .stdout(Stdio::piped())
      .stderr(Stdio::piped())
      .spawn()
      .unwrap();
    let mut ready = String::new();
    BufReader::new(owner.stdout.take().unwrap())
      .read_line(&mut ready)
      .unwrap();
    if ready.is_empty() {
      let mut stderr = String::new();
      owner
        .stderr
        .take()
        .unwrap()
        .read_to_string(&mut stderr)
        .unwrap();
      panic!("EOS failed before readiness: {stderr}");
    }
    let ready: Value = serde_json::from_str(&ready).unwrap();
    assert_eq!(ready["status"], "ready");
    let grpc_address = ready["grpc_address"].as_str().unwrap().to_owned();
    let pid_path = PathBuf::from(format!("{}.pid", scene.display()));
    let deadline = Instant::now() + Duration::from_secs(2);
    while !pid_path.exists() {
      assert!(Instant::now() < deadline, "backend did not publish its PID");
      std::thread::sleep(Duration::from_millis(10));
    }
    let backend_pid = std::fs::read_to_string(pid_path)
      .unwrap()
      .parse::<libc::pid_t>()
      .unwrap();
    Self {
      dir,
      owner,
      grpc_address,
      backend_pid,
    }
  }

  fn kill_backend(&self) {
    assert_eq!(unsafe { libc::kill(self.backend_pid, libc::SIGKILL) }, 0);
  }

  fn wait_exit(&mut self) {
    let deadline = Instant::now() + Duration::from_secs(4);
    loop {
      if let Some(status) = self.owner.try_wait().unwrap() {
        assert!(!status.success(), "backend death must fail EOS owner");
        assert!(!self.dir.join("eos.sock").exists());
        return;
      }
      assert!(
        Instant::now() < deadline,
        "EOS did not exit after backend death"
      );
      std::thread::sleep(Duration::from_millis(10));
    }
  }
}

impl Drop for Fixture {
  fn drop(&mut self) {
    let _ = self.owner.kill();
    let _ = self.owner.wait();
    let _ = std::fs::remove_dir_all(&self.dir);
  }
}

#[test]
fn backend_child_death_after_serving_quiesces_grpc_and_rejects_new_write() {
  let mut fixture = Fixture::start();
  let endpoint = format!("http://{}", fixture.grpc_address);
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let channel = tonic::transport::Channel::from_shared(endpoint)
        .unwrap()
        .connect()
        .await
        .unwrap();
      let mut health = tonic_health::pb::health_client::HealthClient::new(channel.clone());
      let mut watch = health
        .watch(tonic_health::pb::HealthCheckRequest {
          service: String::new(),
        })
        .await
        .unwrap()
        .into_inner();
      assert_eq!(
        watch.message().await.unwrap().unwrap().status,
        tonic_health::pb::health_check_response::ServingStatus::Serving as i32
      );
      let mut capabilities = v2::capability_service_client::CapabilityServiceClient::new(channel);
      let velocity = capabilities
        .list_capabilities(v2::ListCapabilitiesRequest {})
        .await
        .unwrap()
        .into_inner()
        .capabilities
        .into_iter()
        .find(|entry| entry.primitive == "base.velocity")
        .unwrap();
      fixture.kill_backend();
      let changed = tokio::time::timeout(Duration::from_secs(3), watch.message())
        .await
        .unwrap()
        .unwrap()
        .unwrap();
      assert_eq!(
        changed.status,
        tonic_health::pb::health_check_response::ServingStatus::NotServing as i32
      );
      let request = v2::SubmitOperationRequest {
        capability_name: velocity.name,
        arguments: Some(velocity_arguments()),
        operation_id: "after-backend-death".into(),
      };
      let rejected = capabilities.submit_operation(request).await.unwrap_err();
      // The listener may close between health's NOT_SERVING update and the
      // request reaching the service. Both statuses reject this new write.
      assert!(
        matches!(
          rejected.code(),
          tonic::Code::Unavailable | tonic::Code::Cancelled
        ),
        "unexpected post-shutdown write status: {rejected}"
      );
      let ended = tokio::time::timeout(Duration::from_secs(3), watch.message())
        .await
        .unwrap();
      assert!(
        ended.is_err() || ended.unwrap().is_none(),
        "health watch remained open"
      );
    });
  fixture.wait_exit();
  assert!(!fixture.dir.join("sim.sock").exists());
}
