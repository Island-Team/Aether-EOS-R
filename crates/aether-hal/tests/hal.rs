use aether_common::time::ManualClock;
use aether_hal::*;
use std::sync::Arc;

fn meta() -> SampleMeta {
  SampleMeta {
    source_timestamp_ns: Some(42),
    clock: ClockKind::Device,
    received_monotonic_ns: 100,
    sequence: Some(1),
    frame_id: "front".into(),
  }
}

#[test]
fn demo_profile_builds_three_isolated_nodes() {
  let source = include_str!("../../../etc/eos/backends/mock-cxx.toml");
  let profile = Profile::from_toml(source).unwrap();
  assert_eq!(profile.hardware_nodes().count(), 3);
  assert!(profile
    .hardware_nodes()
    .any(|n| n.path() == "eos/hardware/camera/front"));
  assert!(
    Profile::from_toml(&source.replace("version = \"1.0.0\"", "version = \"3.0.0\"")).is_err()
  );
  assert!(
    Profile::from_toml(&source.replace("poll_interval_ms = 100", "poll_interval_ms = 0")).is_err()
  );
  assert!(Profile::from_toml(&source.replace("id = \"front\"", "id = \"bad/id\"")).is_err());
  assert!(Profile::from_toml(
    &source.replace("max_velocity_lease_ms = 500", "max_velocity_lease_ms = 0")
  )
  .is_err());
  assert!(Profile::from_toml(&source.replace(
    "capabilities = [\"body_velocity\"]",
    "capabilities = [\"relative_pose\"]"
  ))
  .is_err());
}

#[test]
fn depth_alignment_and_payload_bounds() {
  let frame = CameraFrame {
    meta: meta(),
    width_px: 2,
    height_px: 2,
    stride_bytes: 6,
    rgb8: vec![0; 12],
    depth: Some(DepthFrame {
      depth_u16: vec![1; 4],
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
  assert!(frame.validate(20).is_ok());
  assert!(frame.validate(19).is_err());
  let mut invalid = frame.clone();
  invalid.depth.as_mut().unwrap().depth_u16.pop();
  assert!(invalid.validate(20).is_err());
}

#[test]
fn typed_envelope_identifies_registered_output() {
  let profile =
    Profile::from_toml(include_str!("../../../etc/eos/backends/mock-cxx.toml")).unwrap();
  let camera = profile
    .node("cores/eos/layers/hardware/groups/camera/nodes/front")
    .unwrap();
  let frame = CameraFrame {
    meta: meta(),
    width_px: 1,
    height_px: 1,
    stride_bytes: 3,
    rgb8: vec![0, 0, 0],
    depth: None,
    preview_jpeg: None,
  };
  let registry = NodeTypeRegistry::builtin();
  let envelope = NodeDataEnvelope::output(camera, &registry, "frame", frame.clone()).unwrap();
  assert_eq!(envelope.source_node, camera.path());
  assert_eq!(envelope.data_type, PortDataType::CameraFrame);
  assert!(NodeDataEnvelope::output(camera, &registry, "health", frame).is_err());

  let hub = Arc::new(TypedDataHub::default());
  let mut subscriber = hub
    .subscribe::<CameraFrame>(camera, &registry, "frame")
    .unwrap();
  assert!(subscriber.poll_latest().unwrap().is_none());
  hub
    .publish(camera, &registry, "frame", envelope.payload.clone())
    .unwrap();
  let mut newer = envelope.payload;
  newer.meta.sequence = Some(2);
  hub.publish(camera, &registry, "frame", newer).unwrap();
  assert_eq!(
    subscriber
      .poll_latest()
      .unwrap()
      .unwrap()
      .payload
      .meta
      .sequence,
    Some(2)
  );
  assert!(subscriber.poll_latest().unwrap().is_none());
  drop(subscriber);
  assert!(hub
    .subscribe::<BaseObservation>(camera, &registry, "frame")
    .is_err());
  assert_eq!(
    hub
      .latest::<CameraFrame>(camera.path(), "frame")
      .unwrap()
      .unwrap()
      .port,
    "frame"
  );
  assert!(hub
    .latest::<BaseObservation>(camera.path(), "frame")
    .is_err());
}

#[test]
fn vision_frame_jpeg_codec_scales_and_preserves_identity() {
  let frame = CameraFrame {
    meta: SampleMeta {
      source_timestamp_ns: Some(2_000_000_000),
      clock: ClockKind::Unix,
      received_monotonic_ns: 10,
      sequence: Some(7),
      frame_id: "front".into(),
    },
    width_px: 2,
    height_px: 2,
    stride_bytes: 6,
    rgb8: vec![255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255],
    depth: None,
    preview_jpeg: None,
  };
  assert_eq!(CameraJpegTransformer::CODEC_ID, "vision_frame_jpeg_v1");
  let vision = CameraJpegTransformer
    .transform(
      &frame,
      &CameraJpegOptions {
        gate_path: "cores/eos/layers/interface/groups/gate/nodes/preview".into(),
        source_rate_hz: 30.0,
        max_width_px: 1,
        quality: 80,
      },
    )
    .unwrap();
  assert_eq!((vision.width_px, vision.height_px), (1, 1));
  assert_eq!(vision.sequence, Some(7));
  assert_eq!(vision.timestamp_unix_ms, Some(2_000));
  let mut decoder = jpeg_decoder::Decoder::new(vision.jpeg.as_slice());
  assert_eq!(decoder.decode().unwrap().len(), 3);
  assert_eq!(
    (
      decoder.info().unwrap().width,
      decoder.info().unwrap().height
    ),
    (1, 1)
  );
  assert!(VisionFrame::encode_jpeg("gate", &frame, 30.0, 1, 0).is_err());
}

#[test]
fn vision_frame_reuses_matching_source_jpeg_and_rejects_invalid_preview() {
  let mut frame = CameraFrame {
    meta: meta(),
    width_px: 2,
    height_px: 2,
    stride_bytes: 6,
    rgb8: vec![128; 12],
    depth: None,
    preview_jpeg: None,
  };
  let original = VisionFrame::encode_jpeg("gate", &frame, 30.0, 2, 85).unwrap();
  frame.preview_jpeg = Some(aether_hal::CameraJpeg {
    bytes: original.jpeg.clone(),
    quality: 85,
  });
  let reused = VisionFrame::encode_jpeg("gate", &frame, 30.0, 2, 85).unwrap();
  assert_eq!(reused.jpeg, original.jpeg);
  assert_eq!((reused.width_px, reused.height_px), (2, 2));

  frame.preview_jpeg.as_mut().unwrap().bytes.pop();
  assert!(VisionFrame::encode_jpeg("gate", &frame, 30.0, 2, 85).is_err());
}

#[test]
fn mock_nodes_have_independent_command_and_health_state() {
  let observation = BaseObservation {
    meta: meta(),
    valid: true,
    x_m: 0.0,
    y_m: 0.0,
    yaw_rad: 0.0,
    x_m_s: 0.0,
    y_m_s: 0.0,
    yaw_rad_s: 0.0,
  };
  let one = MockBase::new(observation.clone());
  let two = MockBase::new(observation);
  one
    .move_base(
      CommandMeta {
        command_id: "c1".into(),
        deadline_monotonic_ns: 200,
      },
      BaseMovement::BodyVelocity {
        x_m_s: 0.1,
        y_m_s: 0.0,
        yaw_rad_s: 0.0,
        valid_for_ms: 100,
      },
    )
    .unwrap();
  assert_eq!(one.commands().len(), 1);
  assert!(two.commands().is_empty());
  one.stop().unwrap();
  assert_eq!(one.status().state, NodeState::Stopped);
  assert_eq!(two.status().state, NodeState::Ready);
}

#[test]
fn cxx_mock_base_transfers_commands_and_samples() {
  assert!(cxx_bridge::mock_base_roundtrip());
  let clock = Arc::new(ManualClock::new(123, None));
  let base = cxx_bridge::CxxMockBase::with_clock(clock.clone(), "drive_frame".into());
  assert_eq!(base.health().last_ok_monotonic_ns, Some(123));
  assert_eq!(base.observe().unwrap().meta.received_monotonic_ns, 123);
  clock.advance_ns(10);
  assert_eq!(base.observe().unwrap().meta.received_monotonic_ns, 133);
  base
    .move_base(
      CommandMeta {
        command_id: "lease".into(),
        deadline_monotonic_ns: 1_000_000,
      },
      BaseMovement::BodyVelocity {
        x_m_s: 0.2,
        y_m_s: 0.0,
        yaw_rad_s: 0.0,
        valid_for_ms: 1,
      },
    )
    .unwrap();
  std::thread::sleep(std::time::Duration::from_millis(5));
  assert_eq!(base.observe().unwrap().x_m_s, 0.0);
}

#[test]
fn cxx_camera_depth_and_manipulator_are_isolated() {
  let clock = Arc::new(ManualClock::new(1_000, None));
  let profile =
    Profile::from_toml(include_str!("../../../etc/eos/backends/mock-cxx.toml")).unwrap();
  let camera_profile = profile
    .hardware_nodes()
    .find(|node| node.id == "front")
    .unwrap();
  let camera = cxx_bridge::CxxMockCamera::from_profile(camera_profile, clock.clone()).unwrap();
  let frame = camera.capture().unwrap();
  assert_eq!(frame.meta.received_monotonic_ns, 1_000);
  assert_eq!(frame.depth.as_ref().unwrap().depth_u16.len(), 4);
  assert!(frame.validate(19).is_err());

  let arm_profile = profile
    .hardware_nodes()
    .find(|node| node.id == "arm")
    .unwrap();
  let names = arm_profile.joint_names.clone().unwrap();
  let one = cxx_bridge::CxxMockManipulator::from_profile(arm_profile, clock.clone()).unwrap();
  let two = cxx_bridge::CxxMockManipulator::from_profile(arm_profile, clock).unwrap();
  assert_eq!(
    one.observe().unwrap().joint_positions_rad,
    vec![0.0; names.len()]
  );
  one
    .move_manipulator(
      CommandMeta {
        command_id: "joint-command".into(),
        deadline_monotonic_ns: 2_000,
      },
      ManipulatorMovement::JointPositions {
        joint_names: names.clone(),
        joint_positions_rad: vec![0.5; names.len()],
        max_speed_rad_s: None,
      },
    )
    .unwrap();
  assert_eq!(
    one.observe().unwrap().joint_positions_rad,
    vec![0.5; names.len()]
  );
  assert_eq!(
    two.observe().unwrap().joint_positions_rad,
    vec![0.0; names.len()]
  );
  one
    .move_manipulator(
      CommandMeta {
        command_id: "pose-command".into(),
        deadline_monotonic_ns: 2_000,
      },
      ManipulatorMovement::EndEffectorPose {
        pose: EndEffectorPose {
          position_m: [0.1, 0.2, 0.3],
          orientation_xyzw: [0.0, 0.0, 0.0, 1.0],
          frame_id: "arm".into(),
        },
        max_speed_m_s: None,
      },
    )
    .unwrap();
  assert_eq!(
    one.observe().unwrap().end_effector_pose.unwrap().position_m,
    [0.1, 0.2, 0.3]
  );
}
