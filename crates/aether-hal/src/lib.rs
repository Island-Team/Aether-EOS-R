pub mod cxx_bridge;
mod data_hub;
mod mock;

pub use aether_node_profile::*;
pub use data_hub::{HubSubscription, TypedDataHub};
pub use mock::{MockBase, MockCamera, MockManipulator};

use serde::{Deserialize, Serialize};
use thiserror::Error;

#[derive(Debug, Error)]
pub enum HalError {
  #[error("profile: {0}")]
  Profile(String),
  #[error("invalid data: {0}")]
  InvalidData(String),
  #[error("unsupported mode: {0}")]
  UnsupportedMode(String),
  #[error("adapter: {0}")]
  Adapter(String),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ClockKind {
  Unix,
  Ros,
  Device,
  Unknown,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct SampleMeta {
  pub source_timestamp_ns: Option<i64>,
  pub clock: ClockKind,
  pub received_monotonic_ns: u64,
  pub sequence: Option<u64>,
  pub frame_id: String,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HealthState {
  Healthy,
  Degraded,
  Unavailable,
}

#[derive(Clone, Debug)]
pub struct Health {
  pub state: HealthState,
  pub code: Option<String>,
  pub message: Option<String>,
  pub last_ok_monotonic_ns: Option<u64>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum NodeState {
  Starting,
  Ready,
  Stopped,
  Faulted,
}

#[derive(Clone, Debug)]
pub struct NodeStatus {
  pub state: NodeState,
  pub active_command_id: Option<String>,
}

#[derive(Clone, Debug, Serialize)]
pub struct CommandMeta {
  pub command_id: String,
  pub deadline_monotonic_ns: u64,
}

/// Child-local physical write fence captured before a durable movement release.
/// It is transport metadata, never a graph field or an authorization grant.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct MovementFenceToken {
  pub generation_id: String,
  pub stop_epoch: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RobotStopProof {
  pub base: ControlledStopOutcome,
  pub arm: ControlledStopOutcome,
  pub generation_id: String,
  pub stop_epoch: Option<String>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CommandOutcome {
  Accepted,
  Completed,
  Cancelled,
  Ambiguous,
}

/// Result of a recoverable controlled stop. An adapter may report a known
/// failure only when it can prove no unconfirmed physical write remains.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ControlledStopOutcome {
  Stopped,
  ConfirmedFailure(String),
  OutcomeUnknown,
}

pub trait NodePayload {
  const DATA_TYPE: PortDataType;
}

/// Typed in-process data identity; this envelope does not authorize commands.
#[derive(Clone, Debug, Serialize)]
pub struct NodeDataEnvelope<T: NodePayload> {
  pub source_node: String,
  pub port: String,
  pub data_type: PortDataType,
  pub contract_version: String,
  pub payload: T,
}

impl<T: NodePayload> NodeDataEnvelope<T> {
  pub fn output(
    node: &NodeProfile,
    registry: &NodeTypeRegistry,
    port: &str,
    payload: T,
  ) -> Result<Self, HalError> {
    if node.path().is_empty() {
      return Err(HalError::InvalidData(
        "node profile is not validated".into(),
      ));
    }
    let definition = registry
      .get(&node.node_type)
      .ok_or_else(|| HalError::InvalidData(format!("unregistered node type {}", node.node_type)))?;
    if !definition
      .outputs
      .iter()
      .any(|output| output.name == port && output.data_type == T::DATA_TYPE)
    {
      return Err(HalError::InvalidData(format!(
        "{} has no {} output with the requested data type",
        node.path(),
        port
      )));
    }
    Ok(Self {
      source_node: node.path().into(),
      port: port.into(),
      data_type: T::DATA_TYPE,
      contract_version: "2.0.0".into(),
      payload,
    })
  }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct Intrinsics {
  pub fx: f64,
  pub fy: f64,
  pub cx: f64,
  pub cy: f64,
  pub distortion_model: String,
  pub distortion_coefficients: Vec<f64>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct DepthFrame {
  pub depth_u16: Vec<u16>,
  pub depth_scale_m: f64,
  pub intrinsics: Intrinsics,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct CameraFrame {
  pub meta: SampleMeta,
  pub width_px: u32,
  pub height_px: u32,
  pub stride_bytes: u32,
  pub rgb8: Vec<u8>,
  pub depth: Option<DepthFrame>,
  #[serde(default)]
  pub preview_jpeg: Option<CameraJpeg>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct CameraJpeg {
  pub bytes: Vec<u8>,
  pub quality: u8,
}

#[derive(Clone, Debug)]
pub struct VisionFrame {
  pub gate_path: String,
  pub sequence: Option<u64>,
  pub timestamp_unix_ms: Option<i64>,
  pub width_px: u32,
  pub height_px: u32,
  pub source_rate_hz: f64,
  pub jpeg: Vec<u8>,
}

/// A compiled codec boundary for converting one registered payload into another.
pub trait DataTransformer<Input, Output> {
  type Options;
  const CODEC_ID: &'static str;
  fn transform(&self, input: &Input, options: &Self::Options) -> Result<Output, HalError>;
}

pub struct CameraJpegTransformer;

pub struct CameraJpegOptions {
  pub gate_path: String,
  pub source_rate_hz: f64,
  pub max_width_px: u32,
  pub quality: u8,
}

impl DataTransformer<CameraFrame, VisionFrame> for CameraJpegTransformer {
  type Options = CameraJpegOptions;
  const CODEC_ID: &'static str = "vision_frame_jpeg_v1";

  fn transform(
    &self,
    input: &CameraFrame,
    options: &Self::Options,
  ) -> Result<VisionFrame, HalError> {
    VisionFrame::encode_jpeg(
      &options.gate_path,
      input,
      options.source_rate_hz,
      options.max_width_px,
      options.quality,
    )
  }
}

impl NodePayload for VisionFrame {
  const DATA_TYPE: PortDataType = PortDataType::VisionFrame;
}

impl VisionFrame {
  pub fn encode_jpeg(
    gate_path: &str,
    frame: &CameraFrame,
    source_rate_hz: f64,
    max_width_px: u32,
    quality: u8,
  ) -> Result<Self, HalError> {
    if gate_path.is_empty()
      || !source_rate_hz.is_finite()
      || source_rate_hz <= 0.0
      || !(1..=4096).contains(&max_width_px)
      || !(1..=100).contains(&quality)
    {
      return Err(HalError::InvalidData(
        "invalid vision encoding options".into(),
      ));
    }
    frame.validate(16 * 1024 * 1024)?;
    let width = frame.width_px.min(max_width_px);
    let height =
      (u64::from(frame.height_px) * u64::from(width) / u64::from(frame.width_px)).max(1) as u32;
    if height > 4096 {
      return Err(HalError::InvalidData(
        "vision height exceeds 4096 px".into(),
      ));
    }
    let jpeg = if let Some(preview) = frame
      .preview_jpeg
      .as_ref()
      .filter(|preview| width == frame.width_px && preview.quality == quality)
    {
      preview.bytes.clone()
    } else {
      let mut pixels = Vec::with_capacity((width as usize) * (height as usize) * 3);
      for y in 0..height {
        let src_y = (u64::from(y) * u64::from(frame.height_px) / u64::from(height)) as usize;
        for x in 0..width {
          let src_x = (u64::from(x) * u64::from(frame.width_px) / u64::from(width)) as usize;
          let start = src_y * frame.stride_bytes as usize + src_x * 3;
          pixels.extend_from_slice(&frame.rgb8[start..start + 3]);
        }
      }
      let mut jpeg = Vec::new();
      jpeg_encoder::Encoder::new(&mut jpeg, quality)
        .encode(
          &pixels,
          width as u16,
          height as u16,
          jpeg_encoder::ColorType::Rgb,
        )
        .map_err(|e| HalError::InvalidData(format!("JPEG encode failed: {e}")))?;
      jpeg
    };
    if jpeg.len() > 8 * 1024 * 1024 {
      return Err(HalError::InvalidData(
        "JPEG exceeds vision frame bound".into(),
      ));
    }
    Ok(Self {
      gate_path: gate_path.into(),
      sequence: frame.meta.sequence,
      timestamp_unix_ms: if frame.meta.clock == ClockKind::Unix {
        frame
          .meta
          .source_timestamp_ns
          .map(|ns| ns.div_euclid(1_000_000))
      } else {
        None
      },
      width_px: width,
      height_px: height,
      source_rate_hz,
      jpeg,
    })
  }
}

impl NodePayload for CameraFrame {
  const DATA_TYPE: PortDataType = PortDataType::CameraFrame;
}

impl CameraFrame {
  pub fn validate(&self, max_payload_bytes: u64) -> Result<(), HalError> {
    let width = self.width_px as usize;
    let height = self.height_px as usize;
    let stride = self.stride_bytes as usize;
    if width == 0
      || height == 0
      || stride
        < width
          .checked_mul(3)
          .ok_or_else(|| HalError::InvalidData("camera width overflow".into()))?
      || self.rgb8.len()
        != stride
          .checked_mul(height)
          .ok_or_else(|| HalError::InvalidData("camera RGB size overflow".into()))?
    {
      return Err(HalError::InvalidData(
        "camera dimensions or RGB buffer length".into(),
      ));
    }
    let mut total = self.rgb8.len() as u64;
    if let Some(depth) = &self.depth {
      if depth.depth_u16.len()
        != width
          .checked_mul(height)
          .ok_or_else(|| HalError::InvalidData("camera depth size overflow".into()))?
        || !depth.depth_scale_m.is_finite()
        || depth.depth_scale_m <= 0.0
        || ![
          depth.intrinsics.fx,
          depth.intrinsics.fy,
          depth.intrinsics.cx,
          depth.intrinsics.cy,
        ]
        .iter()
        .all(|v| v.is_finite())
        || depth.intrinsics.fx <= 0.0
        || depth.intrinsics.fy <= 0.0
        || depth.intrinsics.distortion_model.is_empty()
        || !depth
          .intrinsics
          .distortion_coefficients
          .iter()
          .all(|v| v.is_finite())
      {
        return Err(HalError::InvalidData(
          "camera depth alignment or calibration".into(),
        ));
      }
      total = total.saturating_add((depth.depth_u16.len() as u64).saturating_mul(2));
    }
    if let Some(preview) = &self.preview_jpeg {
      if !(1..=100).contains(&preview.quality)
        || preview.bytes.len() < 4
        || preview.bytes.len() > 8 * 1024 * 1024
        || !preview.bytes.starts_with(&[0xff, 0xd8])
        || !preview.bytes.ends_with(&[0xff, 0xd9])
      {
        return Err(HalError::InvalidData("invalid camera JPEG preview".into()));
      }
      total = total.saturating_add(preview.bytes.len() as u64);
    }
    if total > max_payload_bytes {
      return Err(HalError::InvalidData(
        "camera payload exceeds configured maximum".into(),
      ));
    }
    Ok(())
  }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct BaseObservation {
  pub meta: SampleMeta,
  pub valid: bool,
  pub x_m: f64,
  pub y_m: f64,
  pub yaw_rad: f64,
  pub x_m_s: f64,
  pub y_m_s: f64,
  pub yaw_rad_s: f64,
}

impl NodePayload for BaseObservation {
  const DATA_TYPE: PortDataType = PortDataType::BaseObservation;
}

#[derive(Clone, Debug, Serialize)]
pub enum BaseMovement {
  BodyVelocity {
    x_m_s: f64,
    y_m_s: f64,
    yaw_rad_s: f64,
    valid_for_ms: u64,
  },
  RelativePose {
    x_m: f64,
    y_m: f64,
    yaw_rad: f64,
    frame_id: String,
  },
}

impl NodePayload for BaseMovement {
  const DATA_TYPE: PortDataType = PortDataType::BaseMovement;
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct EndEffectorPose {
  pub position_m: [f64; 3],
  pub orientation_xyzw: [f64; 4],
  pub frame_id: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct ManipulatorObservation {
  pub meta: SampleMeta,
  pub joint_names: Vec<String>,
  pub joint_positions_rad: Vec<f64>,
  pub joint_velocities_rad_s: Vec<f64>,
  pub joints_valid: bool,
  pub end_effector_pose: Option<EndEffectorPose>,
  pub end_effector_valid: bool,
}

impl NodePayload for ManipulatorObservation {
  const DATA_TYPE: PortDataType = PortDataType::ManipulatorObservation;
}

#[derive(Clone, Debug, Serialize)]
pub enum ManipulatorMovement {
  JointPositions {
    joint_names: Vec<String>,
    joint_positions_rad: Vec<f64>,
    max_speed_rad_s: Option<f64>,
  },
  EndEffectorPose {
    pose: EndEffectorPose,
    max_speed_m_s: Option<f64>,
  },
}

impl NodePayload for ManipulatorMovement {
  const DATA_TYPE: PortDataType = PortDataType::ManipulatorMovement;
}

pub trait NodeAdapter: Send + Sync {
  fn kind(&self) -> NodeKind;
  fn adapter_type(&self) -> &str;
  fn status(&self) -> NodeStatus;
  fn health(&self) -> Health;
  /// Latch stop independently of a blocked observation or command call.
  /// Once this returns, a queued command must not begin actuation. Physical
  /// adapters must also enforce their own bounded stop and velocity leases.
  fn stop(&self) -> Result<(), HalError>;
  /// True only when stop provides a terminal physical write fence: after a
  /// confirmed stop, a previously delayed release cannot actuate. The default
  /// is fail-closed; an adapter name or healthy status is not proof.
  fn has_terminal_stop_fence(&self) -> bool {
    false
  }
  /// A recoverable controlled stop, distinct from the emergency latch.
  /// `Ok` proves a terminal physical write fence: earlier delayed releases
  /// cannot actuate after this call returns. Implementations must remain
  /// movable after a later, separately admitted command. The default is
  /// deliberately unavailable, even if `stop()` is supported.
  fn controlled_stop(&self) -> Result<ControlledStopOutcome, HalError> {
    Err(HalError::UnsupportedMode(
      "non-latching controlled stop".into(),
    ))
  }
  /// Typed advertisement proof for `controlled_stop`; never infer it from an
  /// adapter name, health, or the emergency `stop()` implementation.
  fn supports_non_latching_stop(&self) -> bool {
    false
  }
  /// A shared child session may expose the same token to all actuator nodes.
  fn movement_fence_token(&self) -> Option<MovementFenceToken> {
    None
  }
  /// A whole-robot physical stop. Only a run-scoped coordinator implements it;
  /// the caller maps both returned part outcomes to selected graph paths.
  fn managed_robot_stop(&self, _timeout: std::time::Duration) -> Option<RobotStopProof> {
    None
  }
}

pub trait CameraNode: NodeAdapter {
  fn capture(&self) -> Result<CameraFrame, HalError>;
}

pub trait BaseNode: NodeAdapter {
  fn observe(&self) -> Result<BaseObservation, HalError>;
  fn move_base(
    &self,
    meta: CommandMeta,
    movement: BaseMovement,
  ) -> Result<CommandOutcome, HalError>;
  fn move_base_fenced(
    &self,
    meta: CommandMeta,
    movement: BaseMovement,
    _token: Option<MovementFenceToken>,
  ) -> Result<CommandOutcome, HalError> {
    self.move_base(meta, movement)
  }
}

pub trait ManipulatorNode: NodeAdapter {
  fn observe(&self) -> Result<ManipulatorObservation, HalError>;
  fn move_manipulator(
    &self,
    meta: CommandMeta,
    movement: ManipulatorMovement,
  ) -> Result<CommandOutcome, HalError>;
  fn move_manipulator_fenced(
    &self,
    meta: CommandMeta,
    movement: ManipulatorMovement,
    _token: Option<MovementFenceToken>,
  ) -> Result<CommandOutcome, HalError> {
    self.move_manipulator(meta, movement)
  }
}
