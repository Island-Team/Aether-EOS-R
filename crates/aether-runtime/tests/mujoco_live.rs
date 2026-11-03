//! Run with a live eos-mujoco-sim --v2-socket /tmp/mujoco-v2.sock.

use std::sync::Arc;

use aether_common::time::{Clock, SystemClock};
use aether_dispatcher::{CommandRequest, Movement, OutcomeStatus};
use aether_hal::{BaseMovement, CommandMeta, ManipulatorMovement, Profile};
use aether_runtime::Runtime;

#[test]
#[ignore = "requires a running MuJoCo v2 simulator"]
fn camera_base_arm_and_stop_cross_the_live_simulator_boundary() {
  let source = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mujoco-socket.toml"
  ));
  let profile = Profile::from_toml(source).unwrap();
  let clock = Arc::new(SystemClock);
  let runtime = Runtime::from_profile_adapters(profile, clock.clone()).unwrap();
  assert_eq!(runtime.discover().len(), 3);
  let camera = "cores/eos/layers/hardware/groups/camera/nodes/front";
  let base = "cores/eos/layers/hardware/groups/base/nodes/drive";
  let arm = "cores/eos/layers/hardware/groups/manipulator/nodes/arm";
  let frame = runtime.dispatcher().capture_camera(camera).unwrap();
  assert!(frame.width_px > 0 && frame.height_px > 0);
  assert!(frame.depth.is_none());
  runtime.dispatcher().refresh_node(base).unwrap();
  runtime.dispatcher().refresh_node(arm).unwrap();
  let base_result = runtime.dispatch(CommandRequest {
    node_path: base.into(),
    meta: CommandMeta {
      command_id: format!("live-base-{}", std::process::id()),
      deadline_monotonic_ns: clock.monotonic_ns() + 2_000_000_000,
    },
    movement: Movement::Base(BaseMovement::BodyVelocity {
      x_m_s: 0.05,
      y_m_s: 0.0,
      yaw_rad_s: 0.0,
      valid_for_ms: 100,
    }),
  });
  assert_eq!(base_result.status, OutcomeStatus::OutcomeUnknown);
  let arm_result = runtime.dispatch(CommandRequest {
    node_path: arm.into(),
    meta: CommandMeta {
      command_id: format!("live-arm-{}", std::process::id()),
      deadline_monotonic_ns: clock.monotonic_ns() + 2_000_000_000,
    },
    movement: Movement::Manipulator(ManipulatorMovement::JointPositions {
      joint_names: ["joint1", "joint2", "joint3", "joint4", "joint5", "joint6"]
        .map(str::to_owned)
        .to_vec(),
      joint_positions_rad: vec![0.0; 6],
      max_speed_rad_s: Some(0.2),
    }),
  });
  assert_eq!(arm_result.status, OutcomeStatus::OutcomeUnknown);
  for (_, result) in runtime.dispatcher().emergency_stop() {
    result.unwrap();
  }
}
