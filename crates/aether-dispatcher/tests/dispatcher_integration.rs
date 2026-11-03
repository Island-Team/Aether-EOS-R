use std::sync::Arc;

use aether_common::time::ManualClock;
use aether_dispatcher::{
  AdapterRegistration, CommandRequest, Dispatcher, Movement, NodeFacet, OutcomeStatus,
};
use aether_hal::{
  BaseMovement, BaseNode, BaseObservation, ClockKind, CommandMeta, Health, HealthState,
  ManipulatorMovement, ManipulatorObservation, MockBase, MockManipulator, NodeState, Profile,
  SampleMeta,
};

fn profile() -> Profile {
  Profile::from_toml(
    r#"
version = "1.0.0"
[robot]
id = "robot-1"
name = "Test robot"
[cores.eos.layers.hardware.groups.base.nodes.base0]
id = "base0"
node_type = "eos.hardware.base@v1.0"
label = "Base"
description = "Mock base"
frame_id = "base_link"
capabilities = ["body_velocity", "relative_pose"]
[cores.eos.layers.hardware.groups.base.nodes.base0.adapter]
type = "mock"
[cores.eos.layers.hardware.groups.base.nodes.base0.health_timing]
poll_interval_ms = 10
stale_after_ms = 100
[cores.eos.layers.hardware.groups.base.nodes.base0.observation]
max_rate_hz = 50.0
max_payload_bytes = 4096
[cores.eos.layers.hardware.groups.base.nodes.base0.movement]
modes = ["body_velocity", "relative_pose"]
[cores.eos.layers.hardware.groups.base.nodes.base0.movement.limits]
max_linear_m_s = 1.0
max_angular_rad_s = 2.0
max_velocity_lease_ms = 500
max_translation_m = 3.0
max_rotation_rad = 1.0
[cores.eos.layers.hardware.groups.manipulator.nodes.arm0]
id = "arm0"
node_type = "eos.hardware.manipulator@v1.0"
label = "Arm"
description = "Mock arm"
frame_id = "arm_base"
joint_names = ["j1", "j2"]
capabilities = ["joint_positions"]
[cores.eos.layers.hardware.groups.manipulator.nodes.arm0.adapter]
type = "mock"
[cores.eos.layers.hardware.groups.manipulator.nodes.arm0.health_timing]
poll_interval_ms = 10
stale_after_ms = 100
[cores.eos.layers.hardware.groups.manipulator.nodes.arm0.observation]
max_rate_hz = 50.0
max_payload_bytes = 4096
[cores.eos.layers.hardware.groups.manipulator.nodes.arm0.movement]
modes = ["joint_positions"]
[cores.eos.layers.hardware.groups.manipulator.nodes.arm0.movement.limits]
max_joint_position_rad = 3.14
max_joint_speed_rad_s = 1.0
[cores.eos.layers.dispatcher.groups.control.nodes.base0]
id = "base0"
node_type = "eos.dispatcher.control@v1.0"
label = "Control node"
description = "Software node with a local name matching hardware"
[cores.egia.layers.common.groups.process.nodes.base0]
id = "base0"
node_type = "egia.common.guard@v1.0"
label = "Planning node"
description = "EGIA software node with a local name matching EOS hardware"
[cores.eos.layers.hardware.groups.base.nodes.base0.inputs.command]
node = "cores/eos/layers/dispatcher/groups/control/nodes/base0"
output = "base_command"
[cores.eos.layers.hardware.groups.manipulator.nodes.arm0.inputs.command]
node = "cores/eos/layers/dispatcher/groups/control/nodes/base0"
output = "manipulator_command"
"#,
  )
  .unwrap()
}

fn meta(frame_id: &str) -> SampleMeta {
  SampleMeta {
    source_timestamp_ns: None,
    clock: ClockKind::Unknown,
    received_monotonic_ns: 0,
    sequence: None,
    frame_id: frame_id.into(),
  }
}

fn fixture() -> (
  Dispatcher,
  Arc<MockBase>,
  Arc<MockManipulator>,
  Arc<ManualClock>,
) {
  let base = Arc::new(MockBase::new(BaseObservation {
    meta: meta("base_link"),
    valid: true,
    x_m: 0.0,
    y_m: 0.0,
    yaw_rad: 0.0,
    x_m_s: 0.0,
    y_m_s: 0.0,
    yaw_rad_s: 0.0,
  }));
  let arm = Arc::new(MockManipulator::new(ManipulatorObservation {
    meta: meta("arm_base"),
    joint_names: vec!["j1".into(), "j2".into()],
    joint_positions_rad: vec![0.0, 0.0],
    joint_velocities_rad_s: vec![0.0, 0.0],
    joints_valid: true,
    end_effector_pose: None,
    end_effector_valid: false,
  }));
  let clock = Arc::new(ManualClock::new(0, None));
  let dispatcher = Dispatcher::new(
    &profile(),
    vec![
      AdapterRegistration::Base {
        node_id: "base0".into(),
        adapter: base.clone(),
      },
      AdapterRegistration::Manipulator {
        node_id: "arm0".into(),
        adapter: arm.clone(),
      },
    ],
    clock.clone(),
  )
  .unwrap();
  (dispatcher, base, arm, clock)
}

fn base_request(id: &str, x_m_s: f64) -> CommandRequest {
  CommandRequest {
    node_path: "cores/eos/layers/hardware/groups/base/nodes/base0".into(),
    meta: CommandMeta {
      command_id: id.into(),
      deadline_monotonic_ns: 1_000_000_000,
    },
    movement: Movement::Base(BaseMovement::BodyVelocity {
      x_m_s,
      y_m_s: 0.0,
      yaw_rad_s: 0.0,
      valid_for_ms: 100,
    }),
  }
}

#[test]
fn generic_mock_does_not_claim_a_physical_stop_fence() {
  let (dispatcher, _, _, _) = fixture();
  assert!(!dispatcher.has_terminal_stop_fence("cores/eos/layers/hardware/groups/base/nodes/base0"));
}

#[test]
fn stopped_actuators_remain_observable_but_reject_movement() {
  let (dispatcher, base, arm, _) = fixture();
  let base_path = "cores/eos/layers/hardware/groups/base/nodes/base0";
  let arm_path = "cores/eos/layers/hardware/groups/manipulator/nodes/arm0";
  for path in [base_path, arm_path] {
    dispatcher.stop_node(path).unwrap();
    assert_eq!(dispatcher.status(path).unwrap().state, NodeState::Stopped);
    assert_eq!(dispatcher.health(path).unwrap().state, HealthState::Healthy);
  }
  assert!(dispatcher.observe_base(base_path).unwrap().valid);
  assert!(
    dispatcher
      .observe_manipulator(arm_path)
      .unwrap()
      .joints_valid
  );
  assert_eq!(
    dispatcher.dispatch(base_request("after-stop", 0.1)).status,
    OutcomeStatus::Rejected
  );
  assert!(base.commands().is_empty());
  assert!(arm.commands().is_empty());
}

#[test]
fn test_discovery_reports_hierarchy_and_facets() {
  let (dispatcher, _, _, _) = fixture();
  let nodes = dispatcher.discover();
  assert_eq!(nodes.len(), 2);
  assert_eq!(nodes[0].path, "eos/hardware/base/base0");
  assert!(nodes[0].facets.contains(&NodeFacet::Movement));
  assert!(nodes[0].facets.contains(&NodeFacet::Safety));
}

#[test]
fn test_canonical_paths_isolate_hardware_and_software_cores() {
  let (dispatcher, base, _, _) = fixture();
  for (index, path) in [
    "base0",
    "cores/eos/layers/dispatcher/groups/control/nodes/base0",
    "cores/egia/layers/common/groups/process/nodes/base0",
    "cores/egia/layers/hardware/groups/base/nodes/base0",
  ]
  .into_iter()
  .enumerate()
  {
    let mut request = base_request(&format!("isolation-{index}"), 0.2);
    request.node_path = path.into();
    assert_eq!(dispatcher.dispatch(request).status, OutcomeStatus::Rejected);
    assert!(dispatcher.health(path).is_err());
    assert!(dispatcher.stop_node(path).is_err());
  }
  assert!(base.commands().is_empty());
  assert!(dispatcher
    .health("cores/eos/layers/hardware/groups/base/nodes/base0")
    .is_ok());
}

#[test]
fn test_duplicate_command_id_replays_without_releasing_again() {
  let (dispatcher, base, _, _) = fixture();
  let first = dispatcher.dispatch(base_request("cmd-1", 0.5));
  assert_eq!(first.status, OutcomeStatus::OutcomeUnknown); // Mock confirms acceptance, not physical completion.
  assert_eq!(dispatcher.dispatch(base_request("cmd-1", 0.5)), first);
  assert_eq!(base.commands().len(), 1);
  let conflict = dispatcher.dispatch(base_request("cmd-1", 0.7));
  assert_eq!(conflict.status, OutcomeStatus::Rejected);
  assert_eq!(base.commands().len(), 1);
}

#[test]
fn test_durable_dispatch_does_not_release_duplicate_after_restart() {
  use std::sync::atomic::{AtomicU64, Ordering};

  static NEXT: AtomicU64 = AtomicU64::new(0);
  let directory = std::env::temp_dir().join(format!(
    "eos-dispatch-restart-{}-{}",
    std::process::id(),
    NEXT.fetch_add(1, Ordering::Relaxed)
  ));
  let base = Arc::new(MockBase::new(BaseObservation {
    meta: meta("base_link"),
    valid: true,
    x_m: 0.0,
    y_m: 0.0,
    yaw_rad: 0.0,
    x_m_s: 0.0,
    y_m_s: 0.0,
    yaw_rad_s: 0.0,
  }));
  let arm = Arc::new(MockManipulator::new(ManipulatorObservation {
    meta: meta("arm_base"),
    joint_names: vec!["j1".into(), "j2".into()],
    joint_positions_rad: vec![0.0, 0.0],
    joint_velocities_rad_s: vec![0.0, 0.0],
    joints_valid: true,
    end_effector_pose: None,
    end_effector_valid: false,
  }));
  let construct = || {
    Dispatcher::new_durable(
      &profile(),
      vec![
        AdapterRegistration::Base {
          node_id: "base0".into(),
          adapter: base.clone(),
        },
        AdapterRegistration::Manipulator {
          node_id: "arm0".into(),
          adapter: arm.clone(),
        },
      ],
      Arc::new(ManualClock::new(0, None)),
      &directory,
    )
    .unwrap()
  };
  let first_runtime = construct();
  let first = first_runtime.dispatch(base_request("restart-id", 0.2));
  assert_eq!(first.status, OutcomeStatus::OutcomeUnknown);
  assert_eq!(base.commands().len(), 1);
  assert!(matches!(
    first_runtime.gateway_operation("restart-id"),
    Err(aether_dispatcher::GatewayLookupError::LegacyRecord)
  ));
  let gateway_id = "g".repeat(128);
  let gateway_request = base_request(&gateway_id, 0.2);
  let gateway_first =
    first_runtime.dispatch_gateway_movement(gateway_request.clone(), "base.velocity");
  assert_eq!(gateway_first.status, OutcomeStatus::Rejected);
  assert!(first_runtime.movement_inhibited());
  let metadata = first_runtime
    .gateway_operation(&gateway_id)
    .unwrap()
    .unwrap();
  assert_eq!(metadata.primitive, "base.velocity");
  assert_eq!(metadata.version, "1.0.0");
  assert_eq!(metadata.accepted, Some(false));
  assert_eq!(base.commands().len(), 1);
  assert!(matches!(Dispatcher::new_durable(
        &profile(),
        vec![],
        Arc::new(ManualClock::new(0, None)),
        &directory
    ), Err(aether_dispatcher::DispatchError::Journal(error)) if error.kind() == std::io::ErrorKind::WouldBlock));
  drop(first_runtime);

  let restarted = construct();
  assert!(restarted.movement_inhibited());
  assert_eq!(
    restarted.gateway_operation(&gateway_id).unwrap().unwrap(),
    metadata
  );
  assert_eq!(
    restarted.dispatch_gateway_movement(gateway_request.clone(), "base.velocity"),
    gateway_first
  );
  assert_eq!(
    restarted
      .dispatch_gateway_movement(gateway_request, "base.navigate")
      .status,
    OutcomeStatus::Rejected
  );
  assert_eq!(restarted.dispatch(base_request("restart-id", 0.2)), first);
  assert_eq!(
    restarted.dispatch(base_request("restart-id", 0.3)).status,
    OutcomeStatus::Rejected
  );
  assert_eq!(base.commands().len(), 1);
  drop(restarted);
  std::fs::remove_dir_all(directory).unwrap();
}

#[test]
fn test_limits_and_stale_health_reject_before_release() {
  let (dispatcher, base, _, clock) = fixture();
  assert_eq!(
    dispatcher.dispatch(base_request("too-fast", 2.0)).status,
    OutcomeStatus::Rejected
  );
  assert!(base.commands().is_empty());
  clock.set_monotonic_ns(200_000_000);
  assert_eq!(
    dispatcher.dispatch(base_request("stale", 0.5)).status,
    OutcomeStatus::Rejected
  );
  assert!(base.commands().is_empty());
  base.set_health(Health {
    state: HealthState::Healthy,
    code: None,
    message: None,
    last_ok_monotonic_ns: Some(200_000_000),
  });
  // Observation remains stale even after health refresh.
  assert_eq!(
    dispatcher
      .dispatch(base_request("stale-observation", 0.5))
      .status,
    OutcomeStatus::Rejected
  );
}

#[test]
fn test_bad_base_observation_degrades_health_until_valid_probe() {
  let (dispatcher, base, _, _) = fixture();
  let mut invalid = base.observe().unwrap();
  invalid.x_m = f64::NAN;
  base.set_observation(invalid);
  assert_eq!(
    dispatcher.dispatch(base_request("bad-sample", 0.2)).status,
    OutcomeStatus::Rejected
  );
  assert_eq!(
    dispatcher
      .health("cores/eos/layers/hardware/groups/base/nodes/base0")
      .unwrap()
      .state,
    HealthState::Degraded
  );
  assert!(dispatcher
    .refresh_node("cores/eos/layers/hardware/groups/base/nodes/base0")
    .is_err());
  let mut valid = base.observe().unwrap();
  valid.x_m = 0.0;
  base.set_observation(valid);
  dispatcher
    .refresh_node("cores/eos/layers/hardware/groups/base/nodes/base0")
    .unwrap();
  assert_eq!(
    dispatcher
      .health("cores/eos/layers/hardware/groups/base/nodes/base0")
      .unwrap()
      .state,
    HealthState::Healthy
  );
  assert_eq!(
    dispatcher.dispatch(base_request("good-sample", 0.2)).status,
    OutcomeStatus::OutcomeUnknown
  );
  assert_eq!(base.commands().len(), 1);
}

#[test]
fn test_manipulator_joint_order_and_mode_are_admitted() {
  let (dispatcher, _, arm, _) = fixture();
  let request =
    |id: &str, joint_names: Vec<String>, movement: ManipulatorMovement| CommandRequest {
      node_path: "cores/eos/layers/hardware/groups/manipulator/nodes/arm0".into(),
      meta: CommandMeta {
        command_id: id.into(),
        deadline_monotonic_ns: 1_000_000_000,
      },
      movement: Movement::Manipulator(match movement {
        ManipulatorMovement::JointPositions {
          joint_positions_rad,
          max_speed_rad_s,
          ..
        } => ManipulatorMovement::JointPositions {
          joint_names,
          joint_positions_rad,
          max_speed_rad_s,
        },
        other => other,
      }),
    };
  let joints = ManipulatorMovement::JointPositions {
    joint_names: vec![],
    joint_positions_rad: vec![0.1, 0.2],
    max_speed_rad_s: Some(0.5),
  };
  assert_eq!(
    dispatcher
      .dispatch(request(
        "wrong-order",
        vec!["j2".into(), "j1".into()],
        joints.clone()
      ))
      .status,
    OutcomeStatus::Rejected
  );
  assert_eq!(
    dispatcher
      .dispatch(request("valid", vec!["j1".into(), "j2".into()], joints))
      .status,
    OutcomeStatus::OutcomeUnknown
  );
  assert_eq!(arm.commands().len(), 1);
}

#[test]
fn test_emergency_stop_latches_and_blocks_further_movement() {
  let (dispatcher, base, arm, _) = fixture();
  let stops = dispatcher.emergency_stop();
  assert_eq!(stops.len(), 2);
  assert!(stops.iter().all(|(_, result)| result.is_ok()));
  assert_eq!(
    dispatcher.dispatch(base_request("after-stop", 0.2)).status,
    OutcomeStatus::Rejected
  );
  assert!(base.commands().is_empty());
  assert!(arm.commands().is_empty());
}

#[test]
fn test_cancel_while_observation_is_blocked_prevents_release() {
  use aether_hal::{BaseNode, CommandOutcome, HalError, NodeAdapter, NodeKind, NodeStatus};
  use std::sync::{Condvar, Mutex};
  use std::thread;

  struct BlockingBase {
    inner: MockBase,
    gate: Arc<(Mutex<(bool, bool)>, Condvar)>,
  }
  impl NodeAdapter for BlockingBase {
    fn adapter_type(&self) -> &'static str {
      "mock"
    }
    fn kind(&self) -> NodeKind {
      self.inner.kind()
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
  }
  impl BaseNode for BlockingBase {
    fn observe(&self) -> Result<BaseObservation, HalError> {
      let (lock, cv) = &*self.gate;
      let mut state = lock.lock().unwrap();
      state.0 = true;
      cv.notify_all();
      while !state.1 {
        state = cv.wait(state).unwrap();
      }
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

  let gate = Arc::new((Mutex::new((false, false)), Condvar::new()));
  let base = Arc::new(BlockingBase {
    inner: MockBase::new(BaseObservation {
      meta: meta("base_link"),
      valid: true,
      x_m: 0.0,
      y_m: 0.0,
      yaw_rad: 0.0,
      x_m_s: 0.0,
      y_m_s: 0.0,
      yaw_rad_s: 0.0,
    }),
    gate: gate.clone(),
  });
  let arm = Arc::new(MockManipulator::new(ManipulatorObservation {
    meta: meta("arm_base"),
    joint_names: vec!["j1".into()],
    joint_positions_rad: vec![0.0],
    joint_velocities_rad_s: vec![0.0],
    joints_valid: true,
    end_effector_pose: None,
    end_effector_valid: false,
  }));
  let dispatcher = Arc::new(
    Dispatcher::new(
      &profile(),
      vec![
        AdapterRegistration::Base {
          node_id: "base0".into(),
          adapter: base.clone(),
        },
        AdapterRegistration::Manipulator {
          node_id: "arm0".into(),
          adapter: arm,
        },
      ],
      Arc::new(ManualClock::new(0, None)),
    )
    .unwrap(),
  );
  let handle = {
    let dispatcher = dispatcher.clone();
    thread::spawn(move || dispatcher.dispatch(base_request("queued-cancel", 0.2)))
  };
  let (lock, cv) = &*gate;
  let mut state = lock.lock().unwrap();
  while !state.0 {
    state = cv.wait(state).unwrap();
  }
  assert_eq!(
    dispatcher.cancel("queued-cancel").unwrap().status,
    OutcomeStatus::Cancelled
  );
  state.1 = true;
  cv.notify_all();
  drop(state);
  assert_eq!(handle.join().unwrap().status, OutcomeStatus::Cancelled);
  assert!(base.inner.commands().is_empty());
}

#[test]
fn test_velocity_lease_cannot_outlive_deadline() {
  let (dispatcher, base, _, _) = fixture();
  let mut request = base_request("long-lease", 0.2);
  request.meta.deadline_monotonic_ns = 50_000_000;
  assert_eq!(dispatcher.dispatch(request).status, OutcomeStatus::Rejected);
  assert!(base.commands().is_empty());
}

#[test]
fn test_oversized_command_signature_rejects_before_reservation() {
  let (dispatcher, base, _, _) = fixture();
  let request = CommandRequest {
    node_path: "cores/eos/layers/hardware/groups/base/nodes/base0".into(),
    meta: CommandMeta {
      command_id: "oversized".into(),
      deadline_monotonic_ns: 1_000_000_000,
    },
    movement: Movement::Base(BaseMovement::RelativePose {
      x_m: 0.0,
      y_m: 0.0,
      yaw_rad: 0.0,
      frame_id: "f".repeat(70_000),
    }),
  };
  assert_eq!(dispatcher.dispatch(request).status, OutcomeStatus::Rejected);
  assert!(dispatcher.result("oversized").is_none());
  assert!(base.commands().is_empty());
}

#[test]
fn test_camera_capture_validates_payload_and_freshness() {
  use aether_hal::{CameraFrame, ClockKind, DepthFrame, Intrinsics, MockCamera, SampleMeta};
  let camera_profile = Profile::from_toml(
    r#"
version = "1.0.0"
[robot]
id = "r"
name = "Robot"
[cores.eos.layers.hardware.groups.camera.nodes.cam0]
id = "cam0"
node_type = "eos.hardware.camera@v1.0"
label = "Camera"
description = "Test camera"
frame_id = "camera_link"
capabilities = ["rgb8", "aligned_depth_u16"]
[cores.eos.layers.hardware.groups.camera.nodes.cam0.adapter]
type = "mock"
[cores.eos.layers.hardware.groups.camera.nodes.cam0.health_timing]
poll_interval_ms = 10
stale_after_ms = 100
[cores.eos.layers.hardware.groups.camera.nodes.cam0.observation]
max_rate_hz = 30.0
max_payload_bytes = 64
"#,
  )
  .unwrap();
  let frame = CameraFrame {
    meta: SampleMeta {
      source_timestamp_ns: None,
      clock: ClockKind::Unknown,
      received_monotonic_ns: 0,
      sequence: None,
      frame_id: "camera_link".into(),
    },
    width_px: 2,
    height_px: 2,
    stride_bytes: 6,
    rgb8: vec![0; 12],
    depth: Some(DepthFrame {
      depth_u16: vec![1000; 4],
      depth_scale_m: 0.001,
      intrinsics: Intrinsics {
        fx: 1.0,
        fy: 1.0,
        cx: 0.5,
        cy: 0.5,
        distortion_model: "none".into(),
        distortion_coefficients: vec![],
      },
    }),
    preview_jpeg: None,
  };
  let camera = Arc::new(MockCamera::new(frame.clone()));
  let clock = Arc::new(ManualClock::new(0, None));
  let dispatcher = Dispatcher::new(
    &camera_profile,
    vec![AdapterRegistration::Camera {
      node_id: "cam0".into(),
      adapter: camera.clone(),
    }],
    clock.clone(),
  )
  .unwrap();
  assert_eq!(
    dispatcher
      .capture_camera("cores/eos/layers/hardware/groups/camera/nodes/cam0")
      .unwrap()
      .depth
      .unwrap()
      .depth_u16
      .len(),
    4
  );
  let mut invalid = frame;
  invalid.depth.as_mut().unwrap().depth_u16.pop();
  camera.set_frame(invalid);
  assert!(dispatcher
    .capture_camera("cores/eos/layers/hardware/groups/camera/nodes/cam0")
    .is_err());
  assert_eq!(
    dispatcher
      .health("cores/eos/layers/hardware/groups/camera/nodes/cam0")
      .unwrap()
      .state,
    HealthState::Degraded
  );
  assert!(dispatcher
    .refresh_node("cores/eos/layers/hardware/groups/camera/nodes/cam0")
    .is_err());
  camera.set_frame(CameraFrame {
    meta: meta("camera_link"),
    width_px: 2,
    height_px: 2,
    stride_bytes: 6,
    rgb8: vec![0; 12],
    depth: None,
    preview_jpeg: None,
  });
  dispatcher
    .refresh_node("cores/eos/layers/hardware/groups/camera/nodes/cam0")
    .unwrap();
  assert_eq!(
    dispatcher
      .health("cores/eos/layers/hardware/groups/camera/nodes/cam0")
      .unwrap()
      .state,
    HealthState::Healthy
  );
  clock.set_monotonic_ns(200_000_000);
  assert!(dispatcher
    .capture_camera("cores/eos/layers/hardware/groups/camera/nodes/cam0")
    .is_err());
}

#[test]
fn test_registry_rejects_adapter_type_mismatch() {
  let mut configured = profile();
  configured
    .cores
    .get_mut("eos")
    .unwrap()
    .layers
    .get_mut("hardware")
    .unwrap()
    .groups
    .get_mut("base")
    .unwrap()
    .nodes
    .get_mut("base0")
    .unwrap()
    .adapter
    .adapter_type = "mock-cxx".into();
  let base = Arc::new(MockBase::new(BaseObservation {
    meta: meta("base_link"),
    valid: true,
    x_m: 0.0,
    y_m: 0.0,
    yaw_rad: 0.0,
    x_m_s: 0.0,
    y_m_s: 0.0,
    yaw_rad_s: 0.0,
  }));
  let arm = Arc::new(MockManipulator::new(ManipulatorObservation {
    meta: meta("arm_base"),
    joint_names: vec!["j1".into()],
    joint_positions_rad: vec![0.0],
    joint_velocities_rad_s: vec![0.0],
    joints_valid: true,
    end_effector_pose: None,
    end_effector_valid: false,
  }));
  let result = Dispatcher::new(
    &configured,
    vec![
      AdapterRegistration::Base {
        node_id: "base0".into(),
        adapter: base,
      },
      AdapterRegistration::Manipulator {
        node_id: "arm0".into(),
        adapter: arm,
      },
    ],
    Arc::new(ManualClock::new(0, None)),
  );
  assert!(result.is_err());
}

#[test]
fn uncertain_base_release_blocks_concurrent_arm_before_it_can_actuate() {
  use aether_hal::{BaseNode, CommandOutcome, HalError, NodeAdapter, NodeKind, NodeStatus};
  use std::sync::{Condvar, Mutex};
  use std::thread;
  use std::time::Duration;

  struct BlockingReleaseBase {
    inner: MockBase,
    gate: Arc<(Mutex<(bool, bool)>, Condvar)>,
  }
  impl NodeAdapter for BlockingReleaseBase {
    fn adapter_type(&self) -> &'static str {
      "mock"
    }
    fn kind(&self) -> NodeKind {
      self.inner.kind()
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
  }
  impl BaseNode for BlockingReleaseBase {
    fn observe(&self) -> Result<BaseObservation, HalError> {
      self.inner.observe()
    }
    fn move_base(
      &self,
      meta: CommandMeta,
      movement: BaseMovement,
    ) -> Result<CommandOutcome, HalError> {
      let (lock, cv) = &*self.gate;
      let mut state = lock.lock().unwrap();
      state.0 = true;
      cv.notify_all();
      while !state.1 {
        state = cv.wait(state).unwrap();
      }
      drop(state);
      self.inner.move_base(meta, movement)
    }
  }

  let gate = Arc::new((Mutex::new((false, false)), Condvar::new()));
  let base = Arc::new(BlockingReleaseBase {
    inner: MockBase::new(BaseObservation {
      meta: meta("base_link"),
      valid: true,
      x_m: 0.0,
      y_m: 0.0,
      yaw_rad: 0.0,
      x_m_s: 0.0,
      y_m_s: 0.0,
      yaw_rad_s: 0.0,
    }),
    gate: gate.clone(),
  });
  let arm = Arc::new(MockManipulator::new(ManipulatorObservation {
    meta: meta("arm_base"),
    joint_names: vec!["j1".into(), "j2".into()],
    joint_positions_rad: vec![0.0, 0.0],
    joint_velocities_rad_s: vec![0.0, 0.0],
    joints_valid: true,
    end_effector_pose: None,
    end_effector_valid: false,
  }));
  let dispatcher = Arc::new(
    Dispatcher::new(
      &profile(),
      vec![
        AdapterRegistration::Base {
          node_id: "base0".into(),
          adapter: base,
        },
        AdapterRegistration::Manipulator {
          node_id: "arm0".into(),
          adapter: arm.clone(),
        },
      ],
      Arc::new(ManualClock::new(0, None)),
    )
    .unwrap(),
  );
  let first = {
    let dispatcher = dispatcher.clone();
    thread::spawn(move || dispatcher.dispatch(base_request("base-uncertain", 0.2)))
  };
  let (lock, cv) = &*gate;
  let mut state = lock.lock().unwrap();
  while !state.0 {
    state = cv.wait(state).unwrap();
  }
  drop(state);
  let (started_tx, started_rx) = std::sync::mpsc::sync_channel(1);
  let second = {
    let dispatcher = dispatcher.clone();
    thread::spawn(move || {
      started_tx.send(()).unwrap();
      dispatcher.dispatch(CommandRequest {
        node_path: "cores/eos/layers/hardware/groups/manipulator/nodes/arm0".into(),
        meta: CommandMeta {
          command_id: "arm-after-uncertain".into(),
          deadline_monotonic_ns: 1_000_000_000,
        },
        movement: Movement::Manipulator(ManipulatorMovement::JointPositions {
          joint_names: vec!["j1".into(), "j2".into()],
          joint_positions_rad: vec![0.1, 0.2],
          max_speed_rad_s: Some(0.5),
        }),
      })
    })
  };
  started_rx.recv_timeout(Duration::from_secs(1)).unwrap();
  thread::sleep(Duration::from_millis(30));
  assert!(
    arm.commands().is_empty(),
    "second node released while first release was unresolved"
  );
  let mut state = lock.lock().unwrap();
  state.1 = true;
  cv.notify_all();
  drop(state);
  assert_eq!(first.join().unwrap().status, OutcomeStatus::OutcomeUnknown);
  assert!(dispatcher.movement_inhibited());
  assert_eq!(second.join().unwrap().status, OutcomeStatus::Rejected);
  assert!(arm.commands().is_empty());
}

#[test]
fn test_emergency_stop_preempts_blocking_release() {
  use aether_hal::{BaseNode, CommandOutcome, HalError, NodeAdapter, NodeKind, NodeStatus};
  use std::sync::{Condvar, Mutex};
  use std::thread;
  use std::time::Duration;

  struct BlockingReleaseBase {
    inner: MockBase,
    gate: Arc<(Mutex<(bool, bool)>, Condvar)>,
  }
  impl NodeAdapter for BlockingReleaseBase {
    fn adapter_type(&self) -> &'static str {
      "mock"
    }
    fn kind(&self) -> NodeKind {
      self.inner.kind()
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
  }
  impl BaseNode for BlockingReleaseBase {
    fn observe(&self) -> Result<BaseObservation, HalError> {
      self.inner.observe()
    }
    fn move_base(
      &self,
      meta: CommandMeta,
      movement: BaseMovement,
    ) -> Result<CommandOutcome, HalError> {
      let (lock, cv) = &*self.gate;
      let mut state = lock.lock().unwrap();
      state.0 = true;
      cv.notify_all();
      while !state.1 {
        state = cv.wait(state).unwrap();
      }
      drop(state);
      self.inner.move_base(meta, movement)
    }
  }

  let gate = Arc::new((Mutex::new((false, false)), Condvar::new()));
  let base = Arc::new(BlockingReleaseBase {
    inner: MockBase::new(BaseObservation {
      meta: meta("base_link"),
      valid: true,
      x_m: 0.0,
      y_m: 0.0,
      yaw_rad: 0.0,
      x_m_s: 0.0,
      y_m_s: 0.0,
      yaw_rad_s: 0.0,
    }),
    gate: gate.clone(),
  });
  let arm = Arc::new(MockManipulator::new(ManipulatorObservation {
    meta: meta("arm_base"),
    joint_names: vec!["j1".into(), "j2".into()],
    joint_positions_rad: vec![0.0, 0.0],
    joint_velocities_rad_s: vec![0.0, 0.0],
    joints_valid: true,
    end_effector_pose: None,
    end_effector_valid: false,
  }));
  let dispatcher = Arc::new(
    Dispatcher::new(
      &profile(),
      vec![
        AdapterRegistration::Base {
          node_id: "base0".into(),
          adapter: base.clone(),
        },
        AdapterRegistration::Manipulator {
          node_id: "arm0".into(),
          adapter: arm,
        },
      ],
      Arc::new(ManualClock::new(0, None)),
    )
    .unwrap(),
  );
  let command_thread = {
    let dispatcher = dispatcher.clone();
    thread::spawn(move || dispatcher.dispatch(base_request("blocking-release", 0.2)))
  };
  let (lock, cv) = &*gate;
  let mut state = lock.lock().unwrap();
  while !state.0 {
    state = cv.wait(state).unwrap();
  }
  let (stop_tx, stop_rx) = std::sync::mpsc::channel();
  let stop_thread = {
    let dispatcher = dispatcher.clone();
    thread::spawn(move || stop_tx.send(dispatcher.emergency_stop()).unwrap())
  };
  let stopped = stop_rx
    .recv_timeout(Duration::from_secs(2))
    .expect("emergency stop must not wait for blocked release");
  assert!(stopped.iter().all(|(_, result)| result.is_ok()));
  state.1 = true;
  cv.notify_all();
  drop(state);
  assert_eq!(
    command_thread.join().unwrap().status,
    OutcomeStatus::OutcomeUnknown
  );
  stop_thread.join().unwrap();
  assert!(base.inner.commands().is_empty());
  assert_eq!(
    dispatcher
      .dispatch(base_request("after-blocking-stop", 0.1))
      .status,
    OutcomeStatus::Rejected
  );
}

#[test]
fn test_relative_pose_rejects_frame_without_implicit_transform() {
  let (dispatcher, base, _, _) = fixture();
  let request = CommandRequest {
    node_path: "cores/eos/layers/hardware/groups/base/nodes/base0".into(),
    meta: CommandMeta {
      command_id: "wrong-frame".into(),
      deadline_monotonic_ns: 1_000_000_000,
    },
    movement: Movement::Base(BaseMovement::RelativePose {
      x_m: 0.1,
      y_m: 0.0,
      yaw_rad: 0.0,
      frame_id: "map".into(),
    }),
  };
  assert_eq!(dispatcher.dispatch(request).status, OutcomeStatus::Rejected);
  assert!(base.commands().is_empty());
}
