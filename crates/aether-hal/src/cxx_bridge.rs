#[cxx::bridge(namespace = "eos::hal")]
pub mod ffi {
  #[derive(Clone, Copy)]
  pub struct BaseReading {
    pub x_m: f64,
    pub y_m: f64,
    pub yaw_rad: f64,
    pub x_m_s: f64,
    pub y_m_s: f64,
    pub yaw_rad_s: f64,
    pub valid: bool,
  }

  pub struct CameraReading {
    pub width_px: u32,
    pub height_px: u32,
    pub stride_bytes: u32,
    pub rgb8: Vec<u8>,
    pub depth_u16: Vec<u16>,
    pub depth_scale_m: f64,
    pub fx: f64,
    pub fy: f64,
    pub cx: f64,
    pub cy: f64,
    pub has_depth: bool,
  }

  pub struct ManipulatorReading {
    pub joint_positions_rad: Vec<f64>,
    pub joint_velocities_rad_s: Vec<f64>,
    pub end_position_m: Vec<f64>,
    pub end_orientation_xyzw: Vec<f64>,
    pub valid: bool,
  }

  unsafe extern "C++" {
    include!("mock_hardware.h");
    type HardwareBase;
    fn new_mock_base() -> SharedPtr<HardwareBase>;
    fn read_base(self: &HardwareBase) -> BaseReading;
    fn submit_body_velocity(
      self: &HardwareBase,
      x_m_s: f64,
      y_m_s: f64,
      yaw_rad_s: f64,
      valid_for_ms: u64,
    ) -> bool;
    fn stop(self: &HardwareBase) -> bool;
    fn is_stopped(self: &HardwareBase) -> bool;
    fn set_mock_submit_delay_ms(self: &HardwareBase, delay_ms: u32);
    fn mock_submit_started(self: &HardwareBase) -> bool;

    type HardwareCamera;
    fn new_mock_camera() -> UniquePtr<HardwareCamera>;
    fn read_camera(self: &HardwareCamera) -> CameraReading;

    type HardwareManipulator;
    fn new_mock_manipulator(joint_count: u32) -> SharedPtr<HardwareManipulator>;
    fn read_manipulator(self: &HardwareManipulator) -> ManipulatorReading;
    fn submit_joint_positions(
      self: &HardwareManipulator,
      positions: &[f64],
      has_max_speed: bool,
      max_speed_rad_s: f64,
    ) -> bool;
    fn submit_end_effector_pose(
      self: &HardwareManipulator,
      position: &[f64],
      orientation_xyzw: &[f64],
      has_max_speed: bool,
      max_speed_m_s: f64,
    ) -> bool;
    fn stop(self: &HardwareManipulator) -> bool;
    fn is_stopped(self: &HardwareManipulator) -> bool;
    fn set_mock_submit_delay_ms(self: &HardwareManipulator, delay_ms: u32);
    fn mock_submit_started(self: &HardwareManipulator) -> bool;
  }
}

pub fn mock_base_roundtrip() -> bool {
  let base = ffi::new_mock_base();
  let before = base.read_base();
  let accepted = base.submit_body_velocity(0.2, 0.0, 0.0, 100);
  let after = base.read_base();
  accepted && before.valid && after.valid && (after.x_m_s - 0.2).abs() < f64::EPSILON && base.stop()
}

use std::sync::Arc;
use std::sync::Mutex;

use aether_common::time::{Clock, SystemClock};

use crate::{
  BaseMovement, BaseNode, BaseObservation, CameraFrame, CameraNode, ClockKind, CommandMeta,
  CommandOutcome, DepthFrame, EndEffectorPose, HalError, Health, HealthState, Intrinsics,
  ManipulatorMovement, ManipulatorNode, ManipulatorObservation, NodeAdapter, NodeKind, NodeProfile,
  NodeState, NodeStatus, SampleMeta,
};

pub struct CxxMockBase {
  device: cxx::SharedPtr<ffi::HardwareBase>,
  status: Mutex<NodeStatus>,
  clock: Arc<dyn Clock>,
  frame_id: String,
}

// C++ owns a mutex-protected value state and an atomic stop latch. SharedPtr
// permits stop to run while a submission is blocked in the mock device.
unsafe impl Send for ffi::HardwareBase {}
unsafe impl Sync for ffi::HardwareBase {}
// The camera has immutable value-only state and remains behind a Rust Mutex.
unsafe impl Send for ffi::HardwareCamera {}
// The C++ manipulator uses the same internal synchronization as the base.
unsafe impl Send for ffi::HardwareManipulator {}
unsafe impl Sync for ffi::HardwareManipulator {}

fn node_health(clock: &dyn Clock) -> Health {
  Health {
    state: HealthState::Healthy,
    code: None,
    message: None,
    last_ok_monotonic_ns: Some(clock.monotonic_ns()),
  }
}

fn sample_meta(clock: &dyn Clock, frame_id: &str) -> SampleMeta {
  SampleMeta {
    source_timestamp_ns: None,
    clock: ClockKind::Unknown,
    received_monotonic_ns: clock.monotonic_ns(),
    sequence: None,
    frame_id: frame_id.into(),
  }
}

impl CxxMockBase {
  pub fn new() -> Self {
    Self::with_clock(Arc::new(SystemClock), "base_link".into())
  }
  pub fn with_clock(clock: Arc<dyn Clock>, frame_id: String) -> Self {
    Self {
      device: ffi::new_mock_base(),
      status: Mutex::new(NodeStatus {
        state: NodeState::Ready,
        active_command_id: None,
      }),
      clock,
      frame_id,
    }
  }

  /// Test seam for proving bounded shutdown around a blocked mock release.
  pub fn set_mock_submit_delay_ms(&self, delay_ms: u32) {
    self.device.set_mock_submit_delay_ms(delay_ms);
  }

  pub fn mock_submit_started(&self) -> bool {
    self.device.mock_submit_started()
  }
  pub fn from_profile(profile: &NodeProfile, clock: Arc<dyn Clock>) -> Result<Self, HalError> {
    check_profile(profile, NodeKind::Base)?;
    Ok(Self::with_clock(clock, profile.frame_id.clone()))
  }
}

impl Default for CxxMockBase {
  fn default() -> Self {
    Self::new()
  }
}

impl NodeAdapter for CxxMockBase {
  fn kind(&self) -> NodeKind {
    NodeKind::Base
  }
  fn adapter_type(&self) -> &'static str {
    "mock-cxx"
  }
  fn status(&self) -> NodeStatus {
    self.status.lock().unwrap().clone()
  }
  fn health(&self) -> Health {
    node_health(self.clock.as_ref())
  }
  fn stop(&self) -> Result<(), HalError> {
    if !self.device.stop() {
      return Err(HalError::Adapter("C++ base stop failed".into()));
    }
    self.status.lock().unwrap().state = NodeState::Stopped;
    Ok(())
  }
  fn has_terminal_stop_fence(&self) -> bool {
    true
  }
}

impl BaseNode for CxxMockBase {
  fn observe(&self) -> Result<BaseObservation, HalError> {
    let reading = self.device.read_base();
    Ok(BaseObservation {
      meta: sample_meta(self.clock.as_ref(), &self.frame_id),
      valid: reading.valid,
      x_m: reading.x_m,
      y_m: reading.y_m,
      yaw_rad: reading.yaw_rad,
      x_m_s: reading.x_m_s,
      y_m_s: reading.y_m_s,
      yaw_rad_s: reading.yaw_rad_s,
    })
  }
  fn move_base(
    &self,
    meta: CommandMeta,
    movement: BaseMovement,
  ) -> Result<CommandOutcome, HalError> {
    let BaseMovement::BodyVelocity {
      x_m_s,
      y_m_s,
      yaw_rad_s,
      valid_for_ms,
    } = movement
    else {
      return Err(HalError::UnsupportedMode(
        "C++ mock base accepts body velocity only".into(),
      ));
    };
    if !self
      .device
      .submit_body_velocity(x_m_s, y_m_s, yaw_rad_s, valid_for_ms)
    {
      return Err(HalError::Adapter("C++ base rejected body velocity".into()));
    }
    let mut status = self.status.lock().unwrap();
    if self.device.is_stopped() {
      return Err(HalError::Adapter(
        "C++ base stopped during submission".into(),
      ));
    }
    status.active_command_id = Some(meta.command_id);
    Ok(CommandOutcome::Accepted)
  }
}

pub struct CxxMockCamera {
  device: Mutex<cxx::UniquePtr<ffi::HardwareCamera>>,
  clock: Arc<dyn Clock>,
  frame_id: String,
  max_payload_bytes: u64,
}

impl CxxMockCamera {
  pub fn with_clock(clock: Arc<dyn Clock>, frame_id: String, max_payload_bytes: u64) -> Self {
    Self {
      device: Mutex::new(ffi::new_mock_camera()),
      clock,
      frame_id,
      max_payload_bytes,
    }
  }
  pub fn from_profile(profile: &NodeProfile, clock: Arc<dyn Clock>) -> Result<Self, HalError> {
    check_profile(profile, NodeKind::Camera)?;
    let max_payload_bytes = profile
      .observation
      .as_ref()
      .ok_or_else(|| HalError::Profile("camera observation missing".into()))?
      .max_payload_bytes;
    Ok(Self::with_clock(
      clock,
      profile.frame_id.clone(),
      max_payload_bytes,
    ))
  }
}

impl NodeAdapter for CxxMockCamera {
  fn kind(&self) -> NodeKind {
    NodeKind::Camera
  }
  fn adapter_type(&self) -> &'static str {
    "mock-cxx"
  }
  fn status(&self) -> NodeStatus {
    NodeStatus {
      state: NodeState::Ready,
      active_command_id: None,
    }
  }
  fn health(&self) -> Health {
    node_health(self.clock.as_ref())
  }
  fn stop(&self) -> Result<(), HalError> {
    Err(HalError::UnsupportedMode(
      "camera has no actuator stop".into(),
    ))
  }
}

impl CameraNode for CxxMockCamera {
  fn capture(&self) -> Result<CameraFrame, HalError> {
    let reading = self.device.lock().unwrap().read_camera();
    let depth = reading.has_depth.then(|| DepthFrame {
      depth_u16: reading.depth_u16,
      depth_scale_m: reading.depth_scale_m,
      intrinsics: Intrinsics {
        fx: reading.fx,
        fy: reading.fy,
        cx: reading.cx,
        cy: reading.cy,
        distortion_model: "none".into(),
        distortion_coefficients: vec![],
      },
    });
    let frame = CameraFrame {
      meta: sample_meta(self.clock.as_ref(), &self.frame_id),
      width_px: reading.width_px,
      height_px: reading.height_px,
      stride_bytes: reading.stride_bytes,
      rgb8: reading.rgb8,
      depth,
      preview_jpeg: None,
    };
    frame.validate(self.max_payload_bytes)?;
    Ok(frame)
  }
}

pub struct CxxMockManipulator {
  device: cxx::SharedPtr<ffi::HardwareManipulator>,
  clock: Arc<dyn Clock>,
  frame_id: String,
  joint_names: Vec<String>,
  status: Mutex<NodeStatus>,
}

impl CxxMockManipulator {
  /// Test seam for bounded delayed mock submission.
  pub fn set_mock_submit_delay_ms(&self, delay_ms: u32) {
    self.device.set_mock_submit_delay_ms(delay_ms);
  }
  pub fn with_clock(
    clock: Arc<dyn Clock>,
    frame_id: String,
    joint_names: Vec<String>,
  ) -> Result<Self, HalError> {
    if joint_names.is_empty() || joint_names.iter().any(|name| name.trim().is_empty()) {
      return Err(HalError::InvalidData(
        "manipulator requires named joints".into(),
      ));
    }
    let count = u32::try_from(joint_names.len())
      .map_err(|_| HalError::InvalidData("too many joints".into()))?;
    Ok(Self {
      device: ffi::new_mock_manipulator(count),
      clock,
      frame_id,
      joint_names,
      status: Mutex::new(NodeStatus {
        state: NodeState::Ready,
        active_command_id: None,
      }),
    })
  }
  pub fn from_profile(profile: &NodeProfile, clock: Arc<dyn Clock>) -> Result<Self, HalError> {
    check_profile(profile, NodeKind::Manipulator)?;
    let names = profile
      .joint_names
      .clone()
      .ok_or_else(|| HalError::Profile("manipulator joint_names missing".into()))?;
    Self::with_clock(clock, profile.frame_id.clone(), names)
  }
}

fn check_profile(profile: &NodeProfile, kind: NodeKind) -> Result<(), HalError> {
  if profile.kind != kind || profile.adapter.adapter_type != "mock-cxx" {
    return Err(HalError::Profile(format!(
      "node {} is not a mock-cxx {kind}",
      profile.id
    )));
  }
  Ok(())
}

impl NodeAdapter for CxxMockManipulator {
  fn kind(&self) -> NodeKind {
    NodeKind::Manipulator
  }
  fn adapter_type(&self) -> &'static str {
    "mock-cxx"
  }
  fn status(&self) -> NodeStatus {
    self.status.lock().unwrap().clone()
  }
  fn health(&self) -> Health {
    node_health(self.clock.as_ref())
  }
  fn stop(&self) -> Result<(), HalError> {
    if !self.device.stop() {
      return Err(HalError::Adapter("C++ manipulator stop failed".into()));
    }
    self.status.lock().unwrap().state = NodeState::Stopped;
    Ok(())
  }
  fn has_terminal_stop_fence(&self) -> bool {
    true
  }
}

impl ManipulatorNode for CxxMockManipulator {
  fn observe(&self) -> Result<ManipulatorObservation, HalError> {
    let reading = self.device.read_manipulator();
    if reading.joint_positions_rad.len() != self.joint_names.len()
      || reading.joint_velocities_rad_s.len() != self.joint_names.len()
      || reading.end_position_m.len() != 3
      || reading.end_orientation_xyzw.len() != 4
    {
      return Err(HalError::InvalidData("C++ manipulator sample shape".into()));
    }
    let end_effector_pose = EndEffectorPose {
      position_m: reading
        .end_position_m
        .try_into()
        .map_err(|_| HalError::InvalidData("end position".into()))?,
      orientation_xyzw: reading
        .end_orientation_xyzw
        .try_into()
        .map_err(|_| HalError::InvalidData("end orientation".into()))?,
      frame_id: self.frame_id.clone(),
    };
    Ok(ManipulatorObservation {
      meta: sample_meta(self.clock.as_ref(), &self.frame_id),
      joint_names: self.joint_names.clone(),
      joint_positions_rad: reading.joint_positions_rad,
      joint_velocities_rad_s: reading.joint_velocities_rad_s,
      joints_valid: reading.valid,
      end_effector_pose: Some(end_effector_pose),
      end_effector_valid: reading.valid,
    })
  }
  fn move_manipulator(
    &self,
    meta: CommandMeta,
    movement: ManipulatorMovement,
  ) -> Result<CommandOutcome, HalError> {
    let accepted = match movement {
      ManipulatorMovement::JointPositions {
        joint_names,
        joint_positions_rad,
        max_speed_rad_s,
      } => {
        if joint_names != self.joint_names {
          return Err(HalError::InvalidData("joint order mismatch".into()));
        }
        self.device.submit_joint_positions(
          &joint_positions_rad,
          max_speed_rad_s.is_some(),
          max_speed_rad_s.unwrap_or(0.0),
        )
      }
      ManipulatorMovement::EndEffectorPose {
        pose,
        max_speed_m_s,
      } => self.device.submit_end_effector_pose(
        &pose.position_m,
        &pose.orientation_xyzw,
        max_speed_m_s.is_some(),
        max_speed_m_s.unwrap_or(0.0),
      ),
    };
    if !accepted {
      return Err(HalError::Adapter(
        "C++ manipulator rejected movement".into(),
      ));
    }
    let mut status = self.status.lock().unwrap();
    if self.device.is_stopped() {
      return Err(HalError::Adapter(
        "C++ manipulator stopped during submission".into(),
      ));
    }
    status.active_command_id = Some(meta.command_id);
    Ok(CommandOutcome::Accepted)
  }
}

#[cfg(test)]
mod stop_tests {
  use super::*;
  use std::time::{Duration, Instant};

  fn wait_for_submit(started: impl Fn() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(2);
    while !started() {
      assert!(Instant::now() < deadline, "C++ submit did not start");
      std::thread::yield_now();
    }
  }

  #[test]
  fn base_stop_preempts_blocked_submit_and_latches() {
    let base = Arc::new(CxxMockBase::new());
    base.device.set_mock_submit_delay_ms(400);
    let moving = Arc::clone(&base);
    let submit = std::thread::spawn(move || {
      moving.move_base(
        CommandMeta {
          command_id: "delayed-base".into(),
          deadline_monotonic_ns: u64::MAX,
        },
        BaseMovement::BodyVelocity {
          x_m_s: 0.5,
          y_m_s: 0.0,
          yaw_rad_s: 0.0,
          valid_for_ms: 1000,
        },
      )
    });
    wait_for_submit(|| base.device.mock_submit_started());
    let started = Instant::now();
    base.stop().unwrap();
    assert!(started.elapsed() < Duration::from_millis(250));
    assert!(submit.join().unwrap().is_err());
    assert_eq!(base.observe().unwrap().x_m_s, 0.0);
    assert_eq!(base.status().state, NodeState::Stopped);
    assert!(base
      .move_base(
        CommandMeta {
          command_id: "after-stop".into(),
          deadline_monotonic_ns: u64::MAX,
        },
        BaseMovement::BodyVelocity {
          x_m_s: 0.1,
          y_m_s: 0.0,
          yaw_rad_s: 0.0,
          valid_for_ms: 1000,
        },
      )
      .is_err());
    assert_eq!(base.observe().unwrap().x_m_s, 0.0);
  }

  #[test]
  fn manipulator_stop_preempts_blocked_submit_and_latches() {
    let arm = Arc::new(
      CxxMockManipulator::with_clock(Arc::new(SystemClock), "arm".into(), vec!["joint_1".into()])
        .unwrap(),
    );
    arm.device.set_mock_submit_delay_ms(400);
    let moving = Arc::clone(&arm);
    let submit = std::thread::spawn(move || {
      moving.move_manipulator(
        CommandMeta {
          command_id: "delayed-arm".into(),
          deadline_monotonic_ns: u64::MAX,
        },
        ManipulatorMovement::JointPositions {
          joint_names: vec!["joint_1".into()],
          joint_positions_rad: vec![0.5],
          max_speed_rad_s: None,
        },
      )
    });
    wait_for_submit(|| arm.device.mock_submit_started());
    let started = Instant::now();
    arm.stop().unwrap();
    assert!(started.elapsed() < Duration::from_millis(250));
    assert!(submit.join().unwrap().is_err());
    assert_eq!(arm.observe().unwrap().joint_positions_rad, vec![0.0]);
    assert_eq!(arm.status().state, NodeState::Stopped);
    assert!(arm
      .move_manipulator(
        CommandMeta {
          command_id: "after-stop".into(),
          deadline_monotonic_ns: u64::MAX,
        },
        ManipulatorMovement::JointPositions {
          joint_names: vec!["joint_1".into()],
          joint_positions_rad: vec![0.5],
          max_speed_rad_s: None,
        },
      )
      .is_err());
    assert_eq!(arm.observe().unwrap().joint_positions_rad, vec![0.0]);
  }
}
