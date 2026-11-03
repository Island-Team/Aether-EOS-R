use std::sync::{
  Arc, Mutex,
  atomic::{AtomicBool, AtomicU64, Ordering},
};

use aether_common::time::SystemClock;
use aether_dispatcher::AdapterRegistration;
use aether_hal::{
  BaseMovement, BaseNode, BaseObservation, CommandMeta, CommandOutcome, ControlledStopOutcome,
  HalError, Health, ManipulatorMovement, ManipulatorNode, ManipulatorObservation, NodeAdapter,
  NodeKind, NodeStatus, Profile,
  cxx_bridge::{CxxMockBase, CxxMockCamera, CxxMockManipulator},
};
use aether_runtime::{
  Runtime,
  grpc::{Projection, ServerHandle, v2},
};

struct ControlledBase {
  inner: Arc<CxxMockBase>,
  outcome: Mutex<ControlledStopOutcome>,
  controlled_calls: AtomicU64,
  effective_stops: AtomicU64,
  stopped: AtomicBool,
}

impl NodeAdapter for ControlledBase {
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
  fn supports_non_latching_stop(&self) -> bool {
    true
  }
  fn controlled_stop(&self) -> Result<ControlledStopOutcome, HalError> {
    self.controlled_calls.fetch_add(1, Ordering::SeqCst);
    if !self.stopped.swap(true, Ordering::SeqCst) {
      self.effective_stops.fetch_add(1, Ordering::SeqCst);
    }
    Ok(self.outcome.lock().unwrap().clone())
  }
}

impl BaseNode for ControlledBase {
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

struct ControlledArm {
  inner: Arc<CxxMockManipulator>,
  outcome: Mutex<ControlledStopOutcome>,
  stop_started: AtomicBool,
  stop_delay_ms: AtomicU64,
}

impl NodeAdapter for ControlledArm {
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
  fn supports_non_latching_stop(&self) -> bool {
    true
  }
  fn controlled_stop(&self) -> Result<ControlledStopOutcome, HalError> {
    self.stop_started.store(true, Ordering::SeqCst);
    std::thread::sleep(std::time::Duration::from_millis(
      self.stop_delay_ms.load(Ordering::SeqCst),
    ));
    Ok(self.outcome.lock().unwrap().clone())
  }
}

impl ManipulatorNode for ControlledArm {
  fn observe(&self) -> Result<ManipulatorObservation, HalError> {
    self.inner.observe()
  }
  fn move_manipulator(
    &self,
    meta: CommandMeta,
    movement: ManipulatorMovement,
  ) -> Result<CommandOutcome, HalError> {
    self.inner.move_manipulator(meta, movement)
  }
}

fn fixture(
  directory: Option<&std::path::Path>,
  existing_base: Option<Arc<ControlledBase>>,
) -> (Arc<Runtime>, Arc<ControlledBase>, Arc<ControlledArm>) {
  let profile = Profile::from_toml(include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  )))
  .unwrap();
  let clock = Arc::new(SystemClock);
  let base = existing_base.unwrap_or_else(|| {
    Arc::new(ControlledBase {
      inner: Arc::new(
        CxxMockBase::from_profile(
          profile
            .node("cores/eos/layers/hardware/groups/base/nodes/drive")
            .unwrap(),
          clock.clone(),
        )
        .unwrap(),
      ),
      outcome: Mutex::new(ControlledStopOutcome::Stopped),
      controlled_calls: AtomicU64::new(0),
      effective_stops: AtomicU64::new(0),
      stopped: AtomicBool::new(false),
    })
  });
  let arm = Arc::new(ControlledArm {
    inner: Arc::new(
      CxxMockManipulator::from_profile(
        profile
          .node("cores/eos/layers/hardware/groups/manipulator/nodes/arm")
          .unwrap(),
        clock.clone(),
      )
      .unwrap(),
    ),
    outcome: Mutex::new(ControlledStopOutcome::Stopped),
    stop_started: AtomicBool::new(false),
    stop_delay_ms: AtomicU64::new(0),
  });
  let camera = Arc::new(
    CxxMockCamera::from_profile(
      profile
        .node("cores/eos/layers/hardware/groups/camera/nodes/front")
        .unwrap(),
      clock.clone(),
    )
    .unwrap(),
  );
  let adapters = vec![
    AdapterRegistration::Base {
      node_id: "drive".into(),
      adapter: base.clone(),
    },
    AdapterRegistration::Manipulator {
      node_id: "arm".into(),
      adapter: arm.clone(),
    },
    AdapterRegistration::Camera {
      node_id: "front".into(),
      adapter: camera,
    },
  ];
  let runtime = Arc::new(match directory {
    Some(path) => Runtime::new_durable(profile, adapters, clock, path).unwrap(),
    None => Runtime::new(profile, adapters, clock).unwrap(),
  });
  (runtime, base, arm)
}

fn empty_request(capability_name: String, operation_id: &str) -> v2::SubmitOperationRequest {
  v2::SubmitOperationRequest {
    capability_name,
    arguments: Some(prost_types::Struct::default()),
    operation_id: operation_id.into(),
  }
}

#[test]
fn motion_stop_repeats_without_extra_effect_and_replays_id_after_restart() {
  let directory = std::env::temp_dir().join(format!(
    "r02-motion-stop-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  let (runtime, base, arm) = fixture(Some(&directory), None);
  let mut server = ServerHandle::start(Projection::new(runtime.clone()).unwrap(), 0).unwrap();
  let endpoint = format!("http://{}", server.address());
  let (capability, first) = tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut client = v2::capability_service_client::CapabilityServiceClient::connect(endpoint)
        .await
        .unwrap();
      let capability = client
        .list_capabilities(v2::ListCapabilitiesRequest {})
        .await
        .unwrap()
        .into_inner()
        .capabilities
        .into_iter()
        .find(|entry| entry.primitive == "motion.stop" && entry.device_id == "drive")
        .unwrap()
        .name;
      let first = client
        .submit_operation(empty_request(capability.clone(), "motion-first"))
        .await
        .unwrap()
        .into_inner();
      assert_eq!(first.status, v2::OperationStatus::Succeeded as i32);
      assert_eq!(base.controlled_calls.load(Ordering::SeqCst), 1);
      assert_eq!(base.effective_stops.load(Ordering::SeqCst), 1);
      assert!(!arm.stop_started.load(Ordering::SeqCst));

      let already_stopped = client
        .submit_operation(empty_request(capability.clone(), "motion-already-stopped"))
        .await
        .unwrap()
        .into_inner();
      assert_eq!(
        already_stopped.status,
        v2::OperationStatus::Succeeded as i32
      );
      assert_eq!(base.controlled_calls.load(Ordering::SeqCst), 2);
      assert_eq!(base.effective_stops.load(Ordering::SeqCst), 1);
      assert!(!arm.stop_started.load(Ordering::SeqCst));
      (capability, first)
    });
  server.shutdown();
  drop(server);
  drop(runtime);

  let (recovered, _, _) = fixture(Some(&directory), Some(base.clone()));
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
          .submit_operation(empty_request(capability, "motion-first"))
          .await
          .unwrap()
          .into_inner(),
        first
      );
      assert_eq!(base.controlled_calls.load(Ordering::SeqCst), 2);
      assert_eq!(base.effective_stops.load(Ordering::SeqCst), 1);
    });
  replay.shutdown();
  drop(replay);
  drop(recovered);
  std::fs::remove_dir_all(directory).unwrap();
}

#[test]
fn controlled_stop_catalog_partial_failure_and_public_recovery_fail_closed() {
  let (runtime, _, arm) = fixture(None, None);
  let mut server = ServerHandle::start(Projection::new(runtime.clone()).unwrap(), 0).unwrap();
  let endpoint = format!("http://{}", server.address());
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut client = v2::capability_service_client::CapabilityServiceClient::connect(endpoint)
        .await
        .unwrap();
      let catalog = client
        .list_capabilities(v2::ListCapabilitiesRequest {})
        .await
        .unwrap()
        .into_inner()
        .capabilities;
      let motion = catalog
        .iter()
        .find(|entry| entry.primitive == "motion.stop" && entry.device_id == "drive")
        .unwrap();
      let robot = catalog
        .iter()
        .find(|entry| entry.primitive == "robot.stop")
        .unwrap();
      assert!(robot.device_id.is_empty());
      let motion_result = client
        .submit_operation(empty_request(motion.name.clone(), "controlled-motion"))
        .await
        .unwrap()
        .into_inner();
      assert_eq!(motion_result.status, v2::OperationStatus::Succeeded as i32);
      let success = client
        .submit_operation(empty_request(robot.name.clone(), "controlled-robot-1"))
        .await
        .unwrap()
        .into_inner();
      assert_eq!(success.status, v2::OperationStatus::Succeeded as i32);
      let result = success
        .acknowledgement
        .unwrap()
        .fields
        .remove("result")
        .unwrap();
      let prost_types::value::Kind::StructValue(result) = result.kind.unwrap() else {
        panic!("result object");
      };
      let prost_types::value::Kind::ListValue(parts) =
        result.fields["parts"].kind.as_ref().unwrap()
      else {
        panic!("parts array");
      };
      assert_eq!(parts.values.len(), 2);
      *arm.outcome.lock().unwrap() =
        ControlledStopOutcome::ConfirmedFailure("injected stop failure".into());
      let failed = client
        .submit_operation(empty_request(robot.name.clone(), "controlled-robot-2"))
        .await
        .unwrap()
        .into_inner();
      assert_eq!(failed.status, v2::OperationStatus::Failed as i32);
      assert_eq!(failed.accepted, Some(true));
      assert!(runtime.dispatcher().movement_inhibited());
      let mut spoof =
        tonic::Request::new(empty_request(robot.name.clone(), "controlled-robot-spoof"));
      spoof
        .metadata_mut()
        .insert("x-local-authority", "true".parse().unwrap());
      let denied = client.submit_operation(spoof).await.unwrap().into_inner();
      assert_eq!(denied.status, v2::OperationStatus::Rejected as i32);
      assert_eq!(denied.accepted, Some(false));
      assert_eq!(
        denied.acknowledgement.as_ref().unwrap().fields["reason_code"],
        prost_types::Value {
          kind: Some(prost_types::value::Kind::StringValue(
            "rejected.unauthorized".into()
          ))
        }
      );
      assert_eq!(
        client
          .get_operation(v2::OperationRequest {
            operation_id: "controlled-robot-spoof".into()
          })
          .await
          .unwrap()
          .into_inner(),
        denied
      );
    });
  server.shutdown();
}

#[test]
fn unauthorized_recovery_reason_code_survives_journal_restart() {
  let directory = std::env::temp_dir().join(format!(
    "r02-rejection-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  let (runtime, _, arm) = fixture(Some(&directory), None);
  *arm.outcome.lock().unwrap() =
    ControlledStopOutcome::ConfirmedFailure("injected stop failure".into());
  let mut server = ServerHandle::start(Projection::new(runtime.clone()).unwrap(), 0).unwrap();
  let endpoint = format!("http://{}", server.address());
  let denied = tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut client = v2::capability_service_client::CapabilityServiceClient::connect(endpoint)
        .await
        .unwrap();
      let robot = client
        .list_capabilities(v2::ListCapabilitiesRequest {})
        .await
        .unwrap()
        .into_inner()
        .capabilities
        .into_iter()
        .find(|entry| entry.primitive == "robot.stop")
        .unwrap();
      let failed = client
        .submit_operation(empty_request(robot.name.clone(), "partial-before-restart"))
        .await
        .unwrap()
        .into_inner();
      assert_eq!(failed.status, v2::OperationStatus::Failed as i32);
      assert!(runtime.dispatcher().movement_inhibited());
      let denied = client
        .submit_operation(empty_request(robot.name, "denied-before-restart"))
        .await
        .unwrap()
        .into_inner();
      assert_eq!(denied.status, v2::OperationStatus::Rejected as i32);
      assert_eq!(denied.reason_code, "rejected.unauthorized");
      denied
    });
  server.shutdown();
  drop(server);
  drop(runtime);
  drop(arm);

  let (recovered, _, _) = fixture(Some(&directory), None);
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
            operation_id: "denied-before-restart".into()
          })
          .await
          .unwrap()
          .into_inner(),
        denied
      );
    });
  replay.shutdown();
  drop(replay);
  drop(recovered);
  std::fs::remove_dir_all(directory).unwrap();
}

#[test]
fn pending_robot_stop_cannot_be_cancelled_by_public_operation_rpc() {
  let (runtime, _, arm) = fixture(None, None);
  arm.stop_delay_ms.store(300, Ordering::SeqCst);
  let mut server = ServerHandle::start(Projection::new(runtime.clone()).unwrap(), 0).unwrap();
  let endpoint = format!("http://{}", server.address());
  tokio::runtime::Builder::new_multi_thread()
    .worker_threads(2)
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut client = v2::capability_service_client::CapabilityServiceClient::connect(endpoint)
        .await
        .unwrap();
      let robot = client
        .list_capabilities(v2::ListCapabilitiesRequest {})
        .await
        .unwrap()
        .into_inner()
        .capabilities
        .into_iter()
        .find(|entry| entry.primitive == "robot.stop")
        .unwrap();
      let mut submitting = client.clone();
      let submit = tokio::spawn(async move {
        submitting
          .submit_operation(empty_request(robot.name, "pending-robot-stop"))
          .await
      });
      let deadline = tokio::time::Instant::now() + std::time::Duration::from_secs(2);
      while !arm.stop_started.load(Ordering::SeqCst) {
        assert!(tokio::time::Instant::now() < deadline);
        tokio::time::sleep(std::time::Duration::from_millis(5)).await;
      }
      assert_eq!(
        client
          .cancel_operation(v2::OperationRequest {
            operation_id: "pending-robot-stop".into(),
          })
          .await
          .unwrap_err()
          .code(),
        tonic::Code::FailedPrecondition
      );
      let terminal = submit.await.unwrap().unwrap().into_inner();
      assert_eq!(terminal.status, v2::OperationStatus::Succeeded as i32);
    });
  server.shutdown();
}
