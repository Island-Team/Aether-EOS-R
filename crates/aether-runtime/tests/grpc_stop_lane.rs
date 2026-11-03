//! Public R02 stop-lane saturation replay. A blocked movement adapter must
//! consume only movement admission capacity, never the reserved stop lanes.

use std::sync::{
  Arc,
  atomic::{AtomicBool, AtomicUsize, Ordering},
};
use std::time::Duration;

use aether_common::time::{Clock, SystemClock};
use aether_dispatcher::{AdapterRegistration, OutcomeStatus};
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

struct StalledBase {
  inner: Arc<CxxMockBase>,
  entered: AtomicBool,
  release: AtomicBool,
  movement_calls: AtomicUsize,
  controlled_calls: AtomicUsize,
  emergency_calls: AtomicUsize,
}

impl NodeAdapter for StalledBase {
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
    self.emergency_calls.fetch_add(1, Ordering::SeqCst);
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
    // This proof-capable test adapter confirms a separate controlled clamp;
    // it does not use the CXX mock's permanently latching stop path.
    Ok(ControlledStopOutcome::Stopped)
  }
}

impl BaseNode for StalledBase {
  fn observe(&self) -> Result<BaseObservation, HalError> {
    self.entered.store(true, Ordering::SeqCst);
    while !self.release.load(Ordering::SeqCst) {
      std::thread::sleep(Duration::from_millis(1));
    }
    self.inner.observe()
  }
  fn move_base(
    &self,
    meta: CommandMeta,
    movement: BaseMovement,
  ) -> Result<CommandOutcome, HalError> {
    self.movement_calls.fetch_add(1, Ordering::SeqCst);
    self.inner.move_base(meta, movement)
  }
}

struct ProofArm {
  inner: Arc<CxxMockManipulator>,
  controlled_calls: AtomicUsize,
  emergency_calls: AtomicUsize,
}

impl NodeAdapter for ProofArm {
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
    self.emergency_calls.fetch_add(1, Ordering::SeqCst);
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

impl ManipulatorNode for ProofArm {
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

fn fixture() -> (Arc<Runtime>, Arc<StalledBase>, Arc<ProofArm>) {
  let profile = Profile::from_toml(include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  )))
  .unwrap();
  let clock: Arc<dyn Clock> = Arc::new(SystemClock);
  let base = Arc::new(StalledBase {
    inner: Arc::new(
      CxxMockBase::from_profile(
        profile
          .node("cores/eos/layers/hardware/groups/base/nodes/drive")
          .unwrap(),
        clock.clone(),
      )
      .unwrap(),
    ),
    entered: AtomicBool::new(false),
    release: AtomicBool::new(false),
    movement_calls: AtomicUsize::new(0),
    controlled_calls: AtomicUsize::new(0),
    emergency_calls: AtomicUsize::new(0),
  });
  let arm = Arc::new(ProofArm {
    inner: Arc::new(
      CxxMockManipulator::from_profile(
        profile
          .node("cores/eos/layers/hardware/groups/manipulator/nodes/arm")
          .unwrap(),
        clock.clone(),
      )
      .unwrap(),
    ),
    controlled_calls: AtomicUsize::new(0),
    emergency_calls: AtomicUsize::new(0),
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
  let runtime = Arc::new(
    Runtime::new(
      profile,
      vec![
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
      ],
      clock,
    )
    .unwrap(),
  );
  (runtime, base, arm)
}

fn movement(name: &str, id: String) -> v2::SubmitOperationRequest {
  let number = |value| prost_types::Value {
    kind: Some(prost_types::value::Kind::NumberValue(value)),
  };
  v2::SubmitOperationRequest {
    capability_name: name.into(),
    arguments: Some(prost_types::Struct {
      fields: [
        ("linear_x_m_s".into(), number(0.1)),
        ("linear_y_m_s".into(), number(0.0)),
        ("angular_z_rad_s".into(), number(0.0)),
        ("valid_for_ms".into(), number(100.0)),
      ]
      .into(),
    }),
    operation_id: id,
  }
}

fn stop(name: &str, id: &str) -> v2::SubmitOperationRequest {
  v2::SubmitOperationRequest {
    capability_name: name.into(),
    arguments: Some(prost_types::Struct::default()),
    operation_id: id.into(),
  }
}

#[test]
fn full_movement_lane_preserves_robot_and_emergency_stop_admission() {
  let (runtime, base, arm) = fixture();
  let mut server = ServerHandle::start(Projection::new(runtime.clone()).unwrap(), 0).unwrap();
  let endpoint = format!("http://{}", server.address());
  tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build().unwrap().block_on(async {
    let mut client = v2::capability_service_client::CapabilityServiceClient::connect(endpoint).await.unwrap();
    let catalog = client.list_capabilities(v2::ListCapabilitiesRequest {}).await.unwrap().into_inner().capabilities;
    let capability = |primitive: &str| catalog.iter().find(|entry| entry.primitive == primitive).unwrap().name.clone();
    let movement_name = capability("base.velocity");
    let robot_name = capability("robot.stop");
    let estop_name = capability("safety.estop");

    let mut pending = Vec::new();
    for index in 0..16 {
      let mut lane_client = client.clone();
      let mut request = tonic::Request::new(movement(&movement_name, format!("lane-move-{index}")));
      request.set_timeout(Duration::from_secs(10));
      pending.push(tokio::spawn(async move { lane_client.submit_operation(request).await }));
    }
    let admitted = tokio::time::Instant::now() + Duration::from_secs(2);
    while (0..16).any(|index| runtime.dispatcher().gateway_operation(&format!("lane-move-{index}")).unwrap().is_none()) {
      assert!(tokio::time::Instant::now() < admitted, "all 16 movement workers must reserve IDs");
      tokio::time::sleep(Duration::from_millis(2)).await;
    }
    assert!(base.entered.load(Ordering::SeqCst));
    let mut overflow = tonic::Request::new(movement(&movement_name, "lane-overflow".into()));
    overflow.set_timeout(Duration::from_secs(1));
    assert_eq!(client.submit_operation(overflow).await.unwrap_err().code(), tonic::Code::ResourceExhausted);
    assert!(runtime.dispatcher().gateway_operation("lane-overflow").unwrap().is_none());

    let mut robot_client = client.clone();
    let mut robot_request = tonic::Request::new(stop(&robot_name, "lane-robot-stop"));
    robot_request.set_timeout(Duration::from_secs(10));
    let robot = tokio::spawn(async move { robot_client.submit_operation(robot_request).await });
    let entered = tokio::time::Instant::now() + Duration::from_secs(2);
    while runtime.dispatcher().gateway_operation("lane-robot-stop").unwrap().is_none() {
      assert!(tokio::time::Instant::now() < entered, "robot.stop must enter Dispatcher while movement lane is full");
      tokio::time::sleep(Duration::from_millis(2)).await;
    }
    while !runtime.dispatcher().movement_inhibited() {
      assert!(tokio::time::Instant::now() < entered, "robot.stop must persist inhibit while movement is stalled");
      tokio::time::sleep(Duration::from_millis(2)).await;
    }
    assert_eq!(base.controlled_calls.load(Ordering::SeqCst), 0);
    assert_eq!(arm.controlled_calls.load(Ordering::SeqCst), 0);
    let estop = tokio::time::timeout(Duration::from_secs(2), client.submit_operation(stop(&estop_name, "lane-estop"))).await.unwrap().unwrap().into_inner();
    assert_eq!(estop.status, v2::OperationStatus::Succeeded as i32);
    assert!(runtime.dispatcher().emergency_stopped());
    assert!(base.emergency_calls.load(Ordering::SeqCst) > 0);
    assert!(arm.emergency_calls.load(Ordering::SeqCst) > 0);

    base.release.store(true, Ordering::SeqCst);
    for submit in pending {
      let result = submit.await.unwrap().unwrap().into_inner();
      assert_eq!(result.status, v2::OperationStatus::Rejected as i32);
    }
    assert_eq!(base.movement_calls.load(Ordering::SeqCst), 0);
    let robot_result = robot.await.unwrap().unwrap().into_inner();
    assert!(matches!(robot_result.status, x if x == v2::OperationStatus::Succeeded as i32 || x == v2::OperationStatus::OutcomeUnknown as i32));
    assert!(base.controlled_calls.load(Ordering::SeqCst) <= 1);
    assert!(arm.controlled_calls.load(Ordering::SeqCst) <= 1);
    assert_eq!(runtime.dispatcher().gateway_operation("lane-robot-stop").unwrap().unwrap().status,
      if robot_result.status == v2::OperationStatus::Succeeded as i32 { OutcomeStatus::Completed } else { OutcomeStatus::OutcomeUnknown });
  });
  server.shutdown();
}
