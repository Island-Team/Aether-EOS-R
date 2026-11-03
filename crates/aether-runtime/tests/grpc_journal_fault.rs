//! R02-24 public transport evidence for bounded journal faults. All mutations
//! are confined to a fresh private temporary journal directory and restored
//! before the restart replay.

use std::path::{Path, PathBuf};
use std::sync::{
  Arc, Mutex, Weak,
  atomic::{AtomicUsize, Ordering},
};

use aether_common::time::{Clock, SystemClock};
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

struct CountedBase {
  inner: Arc<CxxMockBase>,
  stop_calls: AtomicUsize,
  controlled_calls: AtomicUsize,
  movement_calls: AtomicUsize,
  block_terminal_write: Mutex<Option<PathBuf>>,
  saved_record: Mutex<Option<(PathBuf, Vec<u8>)>>,
}

impl CountedBase {
  fn arm_record_fault(&self, directory: &Path) {
    *self.block_terminal_write.lock().unwrap() = Some(directory.to_path_buf());
  }

  fn restore_record(&self) {
    let (path, bytes) = self
      .saved_record
      .lock()
      .unwrap()
      .take()
      .expect("fault was injected");
    std::fs::remove_dir(&path).unwrap();
    std::fs::write(path, bytes).unwrap();
  }
}

impl NodeAdapter for CountedBase {
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
    Ok(ControlledStopOutcome::Stopped)
  }
}

impl BaseNode for CountedBase {
  fn observe(&self) -> Result<BaseObservation, HalError> {
    self.inner.observe()
  }

  fn move_base(
    &self,
    meta: CommandMeta,
    movement: BaseMovement,
  ) -> Result<CommandOutcome, HalError> {
    self.movement_calls.fetch_add(1, Ordering::SeqCst);
    self.inner.move_base(meta, movement)?;
    if let Some(directory) = self.block_terminal_write.lock().unwrap().take() {
      let record = std::fs::read_dir(directory)
        .unwrap()
        .map(|item| item.unwrap().path())
        .find(|path| path.extension().is_some_and(|extension| extension == "rec"))
        .expect("durable Releasing record before adapter call");
      let bytes = std::fs::read(&record).unwrap();
      std::fs::remove_file(&record).unwrap();
      std::fs::create_dir(&record).unwrap();
      *self.saved_record.lock().unwrap() = Some((record, bytes));
    }
    // The test adapter supplies terminal proof so a failed terminal journal
    // write, rather than the historical CXX mock's Accepted result, causes
    // OUTCOME_UNKNOWN.
    Ok(CommandOutcome::Completed)
  }
}

struct CountedArm {
  inner: Arc<CxxMockManipulator>,
  stop_calls: AtomicUsize,
  controlled_calls: AtomicUsize,
  post_part_capacity_fault: Mutex<Option<(Weak<Runtime>, PathBuf)>>,
}

impl CountedArm {
  fn arm_post_part_capacity_fault(&self, runtime: &Arc<Runtime>, directory: &Path) {
    *self.post_part_capacity_fault.lock().unwrap() =
      Some((Arc::downgrade(runtime), directory.to_path_buf()));
  }
}

impl NodeAdapter for CountedArm {
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
    if let Some((runtime, directory)) = self.post_part_capacity_fault.lock().unwrap().take() {
      let record = std::fs::read_dir(directory)
        .unwrap()
        .map(|item| item.unwrap().path())
        .find(|path| path.extension().is_some_and(|extension| extension == "rec"))
        .expect("first part result and second attempt are durably recorded");
      let current_size = std::fs::metadata(record).unwrap().len() as usize;
      runtime
        .upgrade()
        .unwrap()
        .dispatcher()
        .set_journal_record_limit_for_test(current_size + 1);
    }
    Ok(ControlledStopOutcome::Stopped)
  }
}

impl ManipulatorNode for CountedArm {
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

fn profile() -> Profile {
  Profile::from_toml(include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  )))
  .unwrap()
}

fn directory(label: &str) -> PathBuf {
  let path = std::env::temp_dir().join(format!(
    "r02-journal-fault-{label}-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos(),
  ));
  std::fs::create_dir(&path).unwrap();
  path
}

fn adapters(
  profile: &Profile,
  clock: Arc<dyn Clock>,
) -> (Arc<CountedBase>, Arc<CountedArm>, Vec<AdapterRegistration>) {
  let base = Arc::new(CountedBase {
    inner: Arc::new(
      CxxMockBase::from_profile(
        profile
          .node("cores/eos/layers/hardware/groups/base/nodes/drive")
          .unwrap(),
        clock.clone(),
      )
      .unwrap(),
    ),
    stop_calls: AtomicUsize::new(0),
    controlled_calls: AtomicUsize::new(0),
    movement_calls: AtomicUsize::new(0),
    block_terminal_write: Mutex::new(None),
    saved_record: Mutex::new(None),
  });
  let arm = Arc::new(CountedArm {
    inner: Arc::new(
      CxxMockManipulator::from_profile(
        profile
          .node("cores/eos/layers/hardware/groups/manipulator/nodes/arm")
          .unwrap(),
        clock.clone(),
      )
      .unwrap(),
    ),
    stop_calls: AtomicUsize::new(0),
    controlled_calls: AtomicUsize::new(0),
    post_part_capacity_fault: Mutex::new(None),
  });
  let camera = Arc::new(
    CxxMockCamera::from_profile(
      profile
        .node("cores/eos/layers/hardware/groups/camera/nodes/front")
        .unwrap(),
      clock,
    )
    .unwrap(),
  );
  let registrations = vec![
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
  (base, arm, registrations)
}

fn durable_runtime(
  profile: Profile,
  base: Arc<CountedBase>,
  arm: Arc<CountedArm>,
  directory: &Path,
) -> Arc<Runtime> {
  let clock: Arc<dyn Clock> = Arc::new(SystemClock);
  let camera = Arc::new(
    CxxMockCamera::from_profile(
      profile
        .node("cores/eos/layers/hardware/groups/camera/nodes/front")
        .unwrap(),
      clock.clone(),
    )
    .unwrap(),
  );
  Arc::new(
    Runtime::new_durable(
      profile,
      vec![
        AdapterRegistration::Base {
          node_id: "drive".into(),
          adapter: base,
        },
        AdapterRegistration::Manipulator {
          node_id: "arm".into(),
          adapter: arm,
        },
        AdapterRegistration::Camera {
          node_id: "front".into(),
          adapter: camera,
        },
      ],
      clock,
      directory,
    )
    .unwrap(),
  )
}

fn empty_request(capability_name: String, operation_id: &str) -> v2::SubmitOperationRequest {
  v2::SubmitOperationRequest {
    capability_name,
    arguments: Some(prost_types::Struct::default()),
    operation_id: operation_id.into(),
  }
}

fn velocity_request(capability_name: String, operation_id: &str) -> v2::SubmitOperationRequest {
  let number = |value| prost_types::Value {
    kind: Some(prost_types::value::Kind::NumberValue(value)),
  };
  v2::SubmitOperationRequest {
    capability_name,
    arguments: Some(prost_types::Struct {
      fields: [
        ("linear_x_m_s".into(), number(0.1)),
        ("linear_y_m_s".into(), number(0.0)),
        ("angular_z_rad_s".into(), number(0.0)),
        ("valid_for_ms".into(), number(100.0)),
      ]
      .into(),
    }),
    operation_id: operation_id.into(),
  }
}

fn part_statuses(operation: &v2::Operation) -> Vec<String> {
  let acknowledgement = operation.acknowledgement.as_ref().unwrap();
  let prost_types::value::Kind::StructValue(result) =
    acknowledgement.fields["result"].kind.as_ref().unwrap()
  else {
    panic!("result must be an object");
  };
  let prost_types::value::Kind::ListValue(parts) = result.fields["parts"].kind.as_ref().unwrap()
  else {
    panic!("parts must be a list");
  };
  parts
    .values
    .iter()
    .map(|value| {
      let prost_types::value::Kind::StructValue(part) = value.kind.as_ref().unwrap() else {
        panic!("part must be an object");
      };
      let prost_types::value::Kind::StringValue(status) =
        part.fields["status"].kind.as_ref().unwrap()
      else {
        panic!("part status must be a string");
      };
      status.clone()
    })
    .collect()
}

#[test]
fn pre_release_control_write_fault_returns_internal_without_adapter_call() {
  let directory = directory("pre");
  let profile = profile();
  let clock: Arc<dyn Clock> = Arc::new(SystemClock);
  let (base, arm, registrations) = adapters(&profile, clock.clone());
  let runtime = Arc::new(Runtime::new_durable(profile, registrations, clock, &directory).unwrap());
  let mut server = ServerHandle::start(Projection::new(runtime.clone()).unwrap(), 0).unwrap();
  std::fs::remove_file(directory.join(".control")).unwrap();
  std::fs::create_dir(directory.join(".control")).unwrap();
  let endpoint = format!("http://{}", server.address());
  tokio::runtime::Builder::new_current_thread()
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
        .find(|item| item.primitive == "safety.estop")
        .unwrap();
      let error = client
        .submit_operation(empty_request(capability.name, "pre-store-failure"))
        .await
        .unwrap_err();
      assert_eq!(error.code(), tonic::Code::Internal);
    });
  assert_eq!(base.stop_calls.load(Ordering::SeqCst), 0);
  assert_eq!(arm.stop_calls.load(Ordering::SeqCst), 0);
  assert!(!runtime.dispatcher().emergency_stopped());
  assert_eq!(
    runtime
      .dispatcher()
      .gateway_operation("pre-store-failure")
      .unwrap()
      .unwrap()
      .accepted,
    Some(false)
  );
  std::fs::remove_dir(directory.join(".control")).unwrap();
  server.shutdown();
  drop(server);
  drop(runtime);
  std::fs::remove_dir_all(directory).unwrap();
}

#[test]
fn pre_release_record_capacity_fault_returns_resource_exhausted_without_reservation() {
  let directory = directory("capacity");
  let profile = profile();
  let clock: Arc<dyn Clock> = Arc::new(SystemClock);
  let (base, arm, registrations) = adapters(&profile, clock.clone());
  let runtime = Arc::new(Runtime::new_durable(profile, registrations, clock, &directory).unwrap());
  // The bounded two-part RobotStop snapshot exceeds this test-only record
  // cap before any part can be attempted.
  runtime.dispatcher().set_journal_record_limit_for_test(512);
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
      let capability = client
        .list_capabilities(v2::ListCapabilitiesRequest {})
        .await
        .unwrap()
        .into_inner()
        .capabilities
        .into_iter()
        .find(|item| item.primitive == "robot.stop")
        .unwrap();
      let error = client
        .submit_operation(empty_request(capability.name, "capacity-before-release"))
        .await
        .unwrap_err();
      assert_eq!(error.code(), tonic::Code::ResourceExhausted);
    });
  assert_eq!(base.stop_calls.load(Ordering::SeqCst), 0);
  assert_eq!(arm.stop_calls.load(Ordering::SeqCst), 0);
  assert_eq!(base.controlled_calls.load(Ordering::SeqCst), 0);
  assert_eq!(arm.controlled_calls.load(Ordering::SeqCst), 0);
  assert!(!runtime.dispatcher().emergency_stopped());
  assert!(!runtime.dispatcher().movement_inhibited());
  assert!(runtime
    .dispatcher()
    .gateway_operation("capacity-before-release")
    .unwrap()
    .is_none());
  server.shutdown();
  drop(server);
  drop(runtime);
  std::fs::remove_dir_all(directory).unwrap();
}

#[test]
fn post_part_result_capacity_fault_keeps_known_part_and_never_replays() {
  let directory = directory("part-capacity");
  let profile = profile();
  let clock: Arc<dyn Clock> = Arc::new(SystemClock);
  let (base, arm, registrations) = adapters(&profile, clock.clone());
  let runtime =
    Arc::new(Runtime::new_durable(profile.clone(), registrations, clock, &directory).unwrap());
  arm.arm_post_part_capacity_fault(&runtime, &directory);
  let mut server = ServerHandle::start(Projection::new(runtime.clone()).unwrap(), 0).unwrap();
  let endpoint = format!("http://{}", server.address());
  let request = tokio::runtime::Builder::new_current_thread()
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
        .find(|item| item.primitive == "robot.stop")
        .unwrap();
      let request = empty_request(capability.name, "post-part-overflow");
      let outcome = client
        .submit_operation(request.clone())
        .await
        .unwrap()
        .into_inner();
      assert_eq!(outcome.status, v2::OperationStatus::OutcomeUnknown as i32);
      assert_eq!(outcome.accepted, Some(true));
      assert_eq!(part_statuses(&outcome), ["succeeded", "outcome_unknown"]);
      assert_eq!(
        client
          .get_operation(v2::OperationRequest {
            operation_id: request.operation_id.clone()
          })
          .await
          .unwrap()
          .into_inner(),
        outcome
      );
      assert_eq!(
        client
          .submit_operation(request.clone())
          .await
          .unwrap()
          .into_inner(),
        outcome
      );
      request
    });
  assert_eq!(base.controlled_calls.load(Ordering::SeqCst), 1);
  assert_eq!(arm.controlled_calls.load(Ordering::SeqCst), 1);
  assert!(runtime.dispatcher().movement_inhibited());
  server.shutdown();
  drop(server);
  drop(runtime);
  let recovered = durable_runtime(profile, base.clone(), arm.clone(), &directory);
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
      let queried = client
        .get_operation(v2::OperationRequest {
          operation_id: request.operation_id.clone(),
        })
        .await
        .unwrap()
        .into_inner();
      assert_eq!(queried.status, v2::OperationStatus::OutcomeUnknown as i32);
      assert_eq!(part_statuses(&queried), ["succeeded", "outcome_unknown"]);
      assert_eq!(
        client.submit_operation(request).await.unwrap().into_inner(),
        queried
      );
    });
  assert_eq!(base.controlled_calls.load(Ordering::SeqCst), 1);
  assert_eq!(arm.controlled_calls.load(Ordering::SeqCst), 1);
  assert!(recovered.dispatcher().movement_inhibited());
  replay.shutdown();
  drop(replay);
  drop(recovered);
  std::fs::remove_dir_all(directory).unwrap();
}

#[test]
fn post_release_terminal_write_fault_is_unknown_and_never_replays_after_restart() {
  let directory = directory("post");
  let profile = profile();
  let clock: Arc<dyn Clock> = Arc::new(SystemClock);
  let (base, arm, registrations) = adapters(&profile, clock.clone());
  let runtime =
    Arc::new(Runtime::new_durable(profile.clone(), registrations, clock, &directory).unwrap());
  base.arm_record_fault(&directory);
  let mut server = ServerHandle::start(Projection::new(runtime.clone()).unwrap(), 0).unwrap();
  let endpoint = format!("http://{}", server.address());
  let request = tokio::runtime::Builder::new_current_thread()
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
        .find(|item| item.primitive == "base.velocity")
        .unwrap();
      let request = velocity_request(capability.name, "post-store-failure");
      let result = client
        .submit_operation(request.clone())
        .await
        .unwrap()
        .into_inner();
      assert_eq!(result.status, v2::OperationStatus::OutcomeUnknown as i32);
      assert_eq!(result.accepted, Some(true));
      let queried = client
        .get_operation(v2::OperationRequest {
          operation_id: request.operation_id.clone(),
        })
        .await
        .unwrap()
        .into_inner();
      assert_eq!(queried, result);
      request
    });
  assert_eq!(base.movement_calls.load(Ordering::SeqCst), 1);
  assert!(runtime.dispatcher().movement_inhibited());
  server.shutdown();
  drop(server);
  drop(runtime);
  base.restore_record();
  let recovered = durable_runtime(profile, base.clone(), arm, &directory);
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
      let queried = client
        .get_operation(v2::OperationRequest {
          operation_id: request.operation_id.clone(),
        })
        .await
        .unwrap()
        .into_inner();
      assert_eq!(queried.status, v2::OperationStatus::OutcomeUnknown as i32);
      assert_eq!(queried.accepted, Some(true));
      let duplicate = client.submit_operation(request).await.unwrap().into_inner();
      assert_eq!(duplicate, queried);
    });
  assert_eq!(base.movement_calls.load(Ordering::SeqCst), 1);
  assert!(recovered.dispatcher().movement_inhibited());
  replay.shutdown();
  drop(replay);
  drop(recovered);
  std::fs::remove_dir_all(directory).unwrap();
}
