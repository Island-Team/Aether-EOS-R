use std::sync::Arc;

use aether_common::time::SystemClock;
use aether_hal::{CameraJpeg, Profile};
use aether_runtime::{
  Runtime,
  grpc::{v1, CameraCache, Gateway, Projection},
};
use tonic::Request;

#[test]
fn capture_requires_distinct_source_samples_and_reports_oversized_jpeg() {
  use v1::gateway_service_server::GatewayService;
  let profile = Profile::from_toml(include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  )))
  .unwrap();
  let robot_name = profile.robot.name.clone();
  let path = profile
    .node("cores/eos/layers/hardware/groups/camera/nodes/front")
    .unwrap()
    .path()
    .to_owned();
  let runtime = Arc::new(Runtime::from_cxx_mock_profile(profile, Arc::new(SystemClock)).unwrap());
  let cache = Arc::new(CameraCache::default());
  let gateway = Gateway(Projection::with_camera_cache(runtime.clone(), cache.clone()).unwrap());
  let request = || v1::CaptureFramesRequest {
    robot_name: robot_name.clone(),
    device_id: "front".into(),
    frame_count: 2,
  };
  let first = runtime.dispatcher().capture_camera(&path).unwrap();
  cache.push(&path, Arc::new(first.clone()));
  cache.push(&path, Arc::new(first.clone()));
  let executor = tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap();
  executor.block_on(async {
    for frame_count in [0, 3] {
      assert_eq!(
        gateway
          .capture_frames(Request::new(v1::CaptureFramesRequest {
            frame_count,
            ..request()
          }))
          .await
          .unwrap_err()
          .code(),
        tonic::Code::InvalidArgument
      );
    }
    assert_eq!(
      gateway
        .capture_frames(Request::new(v1::CaptureFramesRequest {
          robot_name: "other".into(),
          ..request()
        }))
        .await
        .unwrap_err()
        .code(),
      tonic::Code::NotFound
    );
    assert_eq!(
      gateway
        .capture_frames(Request::new(v1::CaptureFramesRequest {
          device_id: "other".into(),
          ..request()
        }))
        .await
        .unwrap_err()
        .code(),
      tonic::Code::NotFound
    );
    assert_eq!(
      gateway
        .capture_frames(Request::new(request()))
        .await
        .unwrap_err()
        .code(),
      tonic::Code::Unavailable
    );
  });
  let mut second = first.clone();
  second.meta.received_monotonic_ns += 1;
  cache.push(&path, Arc::new(second));
  executor.block_on(async {
    let snapshot = gateway
      .capture_frames(Request::new(request()))
      .await
      .unwrap();
    assert!(snapshot.metadata().get("eos-sample-age-ms").is_some());
    let snapshot = snapshot.into_inner();
    assert_eq!(snapshot.frames.len(), 2);
    assert_ne!(
      snapshot.frames[0].captured_at,
      snapshot.frames[1].captured_at
    );
  });
  let mut over = first;
  over.meta.received_monotonic_ns += 2;
  let mut bytes = vec![0; 8 * 1024 * 1024 + 1];
  bytes[0..2].copy_from_slice(&[0xff, 0xd8]);
  let end = bytes.len();
  bytes[end - 2..].copy_from_slice(&[0xff, 0xd9]);
  over.preview_jpeg = Some(CameraJpeg { bytes, quality: 85 });
  cache.push(&path, Arc::new(over));
  executor.block_on(async {
    assert_eq!(
      gateway
        .capture_frames(Request::new(v1::CaptureFramesRequest {
          frame_count: 1,
          ..request()
        }))
        .await
        .unwrap_err()
        .code(),
      tonic::Code::ResourceExhausted
    );
  });
  std::thread::sleep(std::time::Duration::from_millis(1050));
  executor.block_on(async {
    assert_eq!(
      gateway
        .capture_frames(Request::new(v1::CaptureFramesRequest {
          frame_count: 1,
          ..request()
        }))
        .await
        .unwrap_err()
        .code(),
      tonic::Code::Unavailable
    );
  });
}
