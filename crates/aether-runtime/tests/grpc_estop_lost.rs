use std::sync::{
  Arc,
  atomic::{AtomicBool, AtomicUsize, Ordering},
};

use aether_common::time::{Clock, SystemClock};
use aether_dispatcher::AdapterRegistration;
use aether_hal::{
  BaseMovement, BaseNode, BaseObservation, ClockKind, CommandMeta, CommandOutcome, HalError,
  Health, MockBase, NodeAdapter, NodeKind, NodeStatus, Profile, SampleMeta,
};
use aether_runtime::{
  Runtime,
  grpc::{Projection, ServerHandle, v2},
};

struct PausedEmergencyBase {
  inner: MockBase,
  entered: AtomicBool,
  release: AtomicBool,
  stop_calls: AtomicUsize,
}

impl NodeAdapter for PausedEmergencyBase {
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
    self.stop_calls.fetch_add(1, Ordering::SeqCst);
    self.inner.stop()?;
    self.entered.store(true, Ordering::SeqCst);
    while !self.release.load(Ordering::SeqCst) {
      std::thread::sleep(std::time::Duration::from_millis(1));
    }
    Ok(())
  }
  fn has_terminal_stop_fence(&self) -> bool {
    true
  }
}

impl BaseNode for PausedEmergencyBase {
  fn observe(&self) -> Result<BaseObservation, HalError> {
    self.inner.observe()
  }
  fn move_base(
    &self,
    meta: CommandMeta,
    movement: BaseMovement,
  ) -> Result<CommandOutcome, HalError> {
    self.inner.move_base(meta, movement)
  }
}

fn profile() -> Profile {
  Profile::from_toml(
    r#"
version = "1.0.0"
[robot]
id = "r"
name = "R"
[cores.eos.layers.hardware.groups.base.nodes.drive]
id = "drive"
node_type = "eos.hardware.base@v1.0"
label = "Drive"
description = "Emergency base"
frame_id = "base_link"
capabilities = ["body_velocity"]
[cores.eos.layers.hardware.groups.base.nodes.drive.adapter]
type = "mock"
[cores.eos.layers.hardware.groups.base.nodes.drive.health_timing]
poll_interval_ms = 10
stale_after_ms = 1000
[cores.eos.layers.hardware.groups.base.nodes.drive.observation]
max_rate_hz = 50.0
max_payload_bytes = 4096
[cores.eos.layers.hardware.groups.base.nodes.drive.movement]
modes = ["body_velocity"]
[cores.eos.layers.hardware.groups.base.nodes.drive.movement.limits]
max_linear_m_s = 1.0
max_angular_rad_s = 1.0
max_velocity_lease_ms = 500
[cores.eos.layers.dispatcher.groups.control.nodes.command]
id = "command"
node_type = "eos.dispatcher.control@v1.0"
label = "Command"
description = "Local command"
[cores.eos.layers.hardware.groups.base.nodes.drive.inputs.command]
node = "cores/eos/layers/dispatcher/groups/control/nodes/command"
output = "base_command"
"#,
  )
  .unwrap()
}

fn runtime(base: Arc<PausedEmergencyBase>, directory: &std::path::Path) -> Arc<Runtime> {
  let clock: Arc<dyn Clock> = Arc::new(SystemClock);
  Arc::new(
    Runtime::new_durable(
      profile(),
      vec![AdapterRegistration::Base {
        node_id: "drive".into(),
        adapter: base,
      }],
      clock,
      directory,
    )
    .unwrap(),
  )
}

#[test]
fn lost_estop_submit_reply_recovers_by_id_without_releasing_again() {
  let directory = std::env::temp_dir().join(format!(
    "r02-lost-estop-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  let base = Arc::new(PausedEmergencyBase {
    inner: MockBase::new(BaseObservation {
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
    }),
    entered: AtomicBool::new(false),
    release: AtomicBool::new(false),
    stop_calls: AtomicUsize::new(0),
  });
  let admitted = runtime(base.clone(), &directory);
  let mut server = ServerHandle::start(Projection::new(admitted.clone()).unwrap(), 0).unwrap();
  let endpoint = format!("http://{}", server.address());
  let terminal = tokio::runtime::Builder::new_multi_thread()
    .worker_threads(2)
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut client = v2::capability_service_client::CapabilityServiceClient::connect(endpoint)
        .await
        .unwrap();
      let safety = client
        .list_capabilities(v2::ListCapabilitiesRequest {})
        .await
        .unwrap()
        .into_inner()
        .capabilities
        .into_iter()
        .find(|entry| entry.primitive == "safety.estop")
        .unwrap();
      let request = v2::SubmitOperationRequest {
        capability_name: safety.name,
        arguments: Some(prost_types::Struct::default()),
        operation_id: "lost-estop-response".into(),
      };
      let mut submitting = client.clone();
      let pending = tokio::spawn(async move { submitting.submit_operation(request.clone()).await });
      let deadline = tokio::time::Instant::now() + std::time::Duration::from_secs(2);
      while !base.entered.load(Ordering::SeqCst) {
        assert!(tokio::time::Instant::now() < deadline);
        tokio::time::sleep(std::time::Duration::from_millis(5)).await;
      }
      pending.abort();
      base.release.store(true, Ordering::SeqCst);
      let terminal = loop {
        let state = client
          .get_operation(v2::OperationRequest {
            operation_id: "lost-estop-response".into(),
          })
          .await
          .unwrap()
          .into_inner();
        if state.status != v2::OperationStatus::Pending as i32 {
          break state;
        }
        assert!(tokio::time::Instant::now() < deadline);
        tokio::time::sleep(std::time::Duration::from_millis(5)).await;
      };
      assert_eq!(terminal.operation_id, "lost-estop-response");
      assert_eq!(terminal.status, v2::OperationStatus::Succeeded as i32);
      assert_eq!(base.stop_calls.load(Ordering::SeqCst), 1);
      terminal
    });
  server.shutdown();
  drop(server);
  drop(admitted);

  let recovered = runtime(base.clone(), &directory);
  assert!(recovered.dispatcher().emergency_stopped());
  let mut replay = ServerHandle::start(Projection::new(recovered.clone()).unwrap(), 0).unwrap();
  let endpoint = format!("http://{}", replay.address());
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut client = v2::capability_service_client::CapabilityServiceClient::connect(endpoint)
        .await
        .unwrap();
      assert_eq!(
        client
          .get_operation(v2::OperationRequest {
            operation_id: "lost-estop-response".into()
          })
          .await
          .unwrap()
          .into_inner(),
        terminal
      );
      assert_eq!(
        client
          .submit_operation(v2::SubmitOperationRequest {
            capability_name: "runtime/r/safety.estop@1.0.0".into(),
            arguments: Some(prost_types::Struct::default()),
            operation_id: "lost-estop-response".into(),
          })
          .await
          .unwrap()
          .into_inner(),
        terminal
      );
      assert_eq!(base.stop_calls.load(Ordering::SeqCst), 1);
    });
  replay.shutdown();
  drop(replay);
  drop(recovered);
  std::fs::remove_dir_all(directory).unwrap();
}
