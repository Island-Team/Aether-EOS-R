use std::sync::{
  Arc, Condvar, Mutex,
  atomic::{AtomicBool, AtomicUsize, Ordering},
};
use std::time::{Duration, Instant};

use aether_common::time::SystemClock;
use aether_dispatcher::AdapterRegistration;
use aether_hal::{
  CameraFrame, CameraNode, HalError, Health, NodeAdapter, NodeKind, NodeStatus, Profile,
  cxx_bridge::{CxxMockBase, CxxMockCamera, CxxMockManipulator},
};
use aether_runtime::{
  Runtime,
  grpc::{CameraCache, Projection, ServerHandle, v1, v2},
};

struct StallingCamera {
  inner: Arc<CxxMockCamera>,
  stall: AtomicBool,
  stall_status: AtomicBool,
  entered: AtomicUsize,
  active: AtomicUsize,
  maximum_active: AtomicUsize,
  lock: Mutex<()>,
  wake: Condvar,
}

impl StallingCamera {
  fn set_stall(&self, stalled: bool) {
    self.stall.store(stalled, Ordering::Release);
    self.wake.notify_all();
  }

  fn set_status_stall(&self, stalled: bool) {
    self.stall_status.store(stalled, Ordering::Release);
    self.wake.notify_all();
  }

  fn wait_if_stalled(&self, flag: &AtomicBool) {
    if flag.load(Ordering::Acquire) {
      let active = self.active.fetch_add(1, Ordering::AcqRel) + 1;
      self.maximum_active.fetch_max(active, Ordering::AcqRel);
      self.entered.fetch_add(1, Ordering::AcqRel);
      let mut lock = self.lock.lock().unwrap();
      while flag.load(Ordering::Acquire) {
        lock = self.wake.wait(lock).unwrap();
      }
      self.active.fetch_sub(1, Ordering::AcqRel);
    }
  }

  fn wait_for_probe(&self, after: usize) {
    let deadline = Instant::now() + Duration::from_secs(2);
    while self.entered.load(Ordering::Acquire) <= after {
      assert!(
        Instant::now() < deadline,
        "background health probe never entered"
      );
      std::thread::sleep(Duration::from_millis(5));
    }
  }
}

impl NodeAdapter for StallingCamera {
  fn kind(&self) -> NodeKind {
    self.inner.kind()
  }
  fn adapter_type(&self) -> &str {
    self.inner.adapter_type()
  }
  fn status(&self) -> NodeStatus {
    self.wait_if_stalled(&self.stall_status);
    self.inner.status()
  }
  fn health(&self) -> Health {
    self.wait_if_stalled(&self.stall);
    self.inner.health()
  }
  fn stop(&self) -> Result<(), HalError> {
    self.inner.stop()
  }
}

impl CameraNode for StallingCamera {
  fn capture(&self) -> Result<CameraFrame, HalError> {
    self.inner.capture()
  }
}

fn timed<T>(body: T) -> tonic::Request<T> {
  let mut request = tonic::Request::new(body);
  request.set_timeout(Duration::from_millis(250));
  request
}

#[test]
fn stalled_socket_equivalent_probe_does_not_block_reads_health_or_estop() {
  let profile = Profile::from_toml(include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  )))
  .unwrap();
  let clock = Arc::new(SystemClock);
  let base = Arc::new(
    CxxMockBase::from_profile(
      profile
        .node("cores/eos/layers/hardware/groups/base/nodes/drive")
        .unwrap(),
      clock.clone(),
    )
    .unwrap(),
  );
  let arm = Arc::new(
    CxxMockManipulator::from_profile(
      profile
        .node("cores/eos/layers/hardware/groups/manipulator/nodes/arm")
        .unwrap(),
      clock.clone(),
    )
    .unwrap(),
  );
  let camera = Arc::new(StallingCamera {
    inner: Arc::new(
      CxxMockCamera::from_profile(
        profile
          .node("cores/eos/layers/hardware/groups/camera/nodes/front")
          .unwrap(),
        clock.clone(),
      )
      .unwrap(),
    ),
    stall: AtomicBool::new(false),
    stall_status: AtomicBool::new(false),
    entered: AtomicUsize::new(0),
    active: AtomicUsize::new(0),
    maximum_active: AtomicUsize::new(0),
    lock: Mutex::new(()),
    wake: Condvar::new(),
  });
  let camera_path = "cores/eos/layers/hardware/groups/camera/nodes/front";
  let cache = Arc::new(CameraCache::default());
  cache.push(camera_path, Arc::new(camera.capture().unwrap()));
  let runtime = Arc::new(
    Runtime::new(
      profile.clone(),
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
          adapter: camera.clone(),
        },
      ],
      clock,
    )
    .unwrap(),
  );
  let projection = Projection::with_camera_cache(runtime, cache).unwrap();
  let mut server = ServerHandle::start(projection.clone(), 0).unwrap();
  let endpoint = format!("http://{}", server.address());
  let before = camera.entered.load(Ordering::Acquire);
  camera.set_stall(true);
  camera.wait_for_probe(before);
  std::thread::sleep(Duration::from_millis(550)); // beyond camera stale_after_ms=500

  tokio::runtime::Builder::new_multi_thread()
    .worker_threads(2)
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut gateway = v1::gateway_service_client::GatewayServiceClient::connect(endpoint.clone())
        .await
        .unwrap();
      let mut capabilities =
        v2::capability_service_client::CapabilityServiceClient::connect(endpoint.clone())
          .await
          .unwrap();
      let health_channel = tonic::transport::Channel::from_shared(endpoint)
        .unwrap()
        .connect()
        .await
        .unwrap();
      let mut health = tonic_health::pb::health_client::HealthClient::new(health_channel);
      let mut flood = Vec::new();
      for _ in 0..12 {
        let mut gateway = gateway.clone();
        let mut capabilities = capabilities.clone();
        let robot_name = profile.robot.name.clone();
        flood.push(tokio::spawn(async move {
          let started = Instant::now();
          let discovery_response = gateway.discover(timed(())).await.unwrap();
          assert!(discovery_response
            .metadata()
            .get("eos-sample-age-ms")
            .is_none());
          let discovery = discovery_response.into_inner();
          assert_eq!(
            discovery
              .devices
              .iter()
              .find(|d| d.id == "front")
              .unwrap()
              .health,
            "degraded"
          );
          assert_eq!(
            gateway
              .get_status(timed(()))
              .await
              .unwrap()
              .into_inner()
              .fields["status"],
            prost_types::Value {
              kind: Some(prost_types::value::Kind::StringValue("degraded".into()))
            }
          );
          let _ = capabilities
            .list_capabilities(timed(v2::ListCapabilitiesRequest {}))
            .await
            .unwrap();
          assert_eq!(
            gateway
              .capture_frames(timed(v1::CaptureFramesRequest {
                robot_name,
                device_id: "front".into(),
                frame_count: 1
              }))
              .await
              .unwrap_err()
              .code(),
            tonic::Code::Unavailable
          );
          assert!(
            started.elapsed() < Duration::from_millis(250),
            "read RPC waited for the stalled adapter"
          );
        }));
      }
      let started = Instant::now();
      assert_eq!(
        health
          .check(timed(tonic_health::pb::HealthCheckRequest {
            service: String::new()
          }))
          .await
          .unwrap()
          .into_inner()
          .status,
        tonic_health::pb::health_check_response::ServingStatus::Serving as i32
      );
      assert!(
        started.elapsed() < Duration::from_millis(250),
        "health Check waited for the stalled adapter"
      );
      let stop = capabilities
        .list_capabilities(v2::ListCapabilitiesRequest {})
        .await
        .unwrap()
        .into_inner()
        .capabilities
        .into_iter()
        .find(|entry| entry.primitive == "safety.estop")
        .unwrap();
      let started = Instant::now();
      let stopped = capabilities
        .submit_operation(timed(v2::SubmitOperationRequest {
          capability_name: stop.name,
          arguments: Some(prost_types::Struct::default()),
          operation_id: "stalled-health-estop".into(),
        }))
        .await
        .unwrap()
        .into_inner();
      assert_eq!(stopped.status, v2::OperationStatus::Succeeded as i32);
      assert!(
        started.elapsed() < Duration::from_millis(250),
        "estop waited for the stalled adapter health probe"
      );
      for result in flood {
        result.await.unwrap();
      }
      assert_eq!(camera.maximum_active.load(Ordering::Acquire), 1);

      camera.set_stall(false);
      let deadline = Instant::now() + Duration::from_secs(2);
      loop {
        let discovered = gateway.discover(()).await.unwrap();
        let fresh_age = discovered.metadata().get("eos-sample-age-ms").is_some();
        let discovery = discovered.into_inner();
        if discovery
          .devices
          .iter()
          .find(|d| d.id == "front")
          .unwrap()
          .health
          == "ready"
          && fresh_age
        {
          break;
        }
        assert!(
          Instant::now() < deadline,
          "fresh probe did not restore camera health"
        );
        tokio::time::sleep(Duration::from_millis(10)).await;
      }
      assert!(gateway
        .get_status(())
        .await
        .unwrap()
        .metadata()
        .get("eos-sample-age-ms")
        .is_some());
      assert!(capabilities
        .list_capabilities(v2::ListCapabilitiesRequest {})
        .await
        .unwrap()
        .metadata()
        .get("eos-sample-age-ms")
        .is_some());
    });
  let before = camera.entered.load(Ordering::Acquire);
  camera.set_stall(true);
  camera.wait_for_probe(before);
  let started = Instant::now();
  server.begin_shutdown();
  assert!(started.elapsed() < Duration::from_secs(1));
  assert_eq!(
    projection
      .discover()
      .devices
      .iter()
      .find(|d| d.id == "front")
      .unwrap()
      .health,
    "degraded"
  );
  let entered_before_release = camera.entered.load(Ordering::Acquire);
  camera.set_stall(false);
  server.shutdown();
  let deadline = Instant::now() + Duration::from_secs(1);
  while camera.active.load(Ordering::Acquire) != 0 {
    assert!(
      Instant::now() < deadline,
      "sampler did not exit after shutdown"
    );
    std::thread::sleep(Duration::from_millis(5));
  }
  std::thread::sleep(Duration::from_millis(20));
  assert_eq!(
    camera.entered.load(Ordering::Acquire),
    entered_before_release
  );
}

#[test]
fn delayed_status_cannot_republish_old_health_as_a_fresh_sample() {
  let source = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  ))
  .replacen("poll_interval_ms = 100", "poll_interval_ms = 500", 1);
  let profile = Profile::from_toml(&source).unwrap();
  let clock = Arc::new(SystemClock);
  let base = Arc::new(
    CxxMockBase::from_profile(
      profile
        .node("cores/eos/layers/hardware/groups/base/nodes/drive")
        .unwrap(),
      clock.clone(),
    )
    .unwrap(),
  );
  let arm = Arc::new(
    CxxMockManipulator::from_profile(
      profile
        .node("cores/eos/layers/hardware/groups/manipulator/nodes/arm")
        .unwrap(),
      clock.clone(),
    )
    .unwrap(),
  );
  let camera = Arc::new(StallingCamera {
    inner: Arc::new(
      CxxMockCamera::from_profile(
        profile
          .node("cores/eos/layers/hardware/groups/camera/nodes/front")
          .unwrap(),
        clock.clone(),
      )
      .unwrap(),
    ),
    stall: AtomicBool::new(false),
    stall_status: AtomicBool::new(false),
    entered: AtomicUsize::new(0),
    active: AtomicUsize::new(0),
    maximum_active: AtomicUsize::new(0),
    lock: Mutex::new(()),
    wake: Condvar::new(),
  });
  let runtime = Arc::new(
    Runtime::new(
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
          adapter: camera.clone(),
        },
      ],
      clock,
    )
    .unwrap(),
  );
  let mut server = ServerHandle::start(Projection::new(runtime).unwrap(), 0).unwrap();
  let endpoint = format!("http://{}", server.address());
  let before = camera.entered.load(Ordering::Acquire);
  camera.set_status_stall(true);
  camera.wait_for_probe(before); // health() returned before status() blocks
  std::thread::sleep(Duration::from_millis(600)); // beyond stale_after_ms=500
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut gateway = v1::gateway_service_client::GatewayServiceClient::connect(endpoint)
        .await
        .unwrap();
      let stalled = gateway.discover(timed(())).await.unwrap();
      assert!(stalled.metadata().get("eos-sample-age-ms").is_none());
      assert_eq!(
        stalled
          .into_inner()
          .devices
          .iter()
          .find(|d| d.id == "front")
          .unwrap()
          .health,
        "degraded"
      );

      camera.set_status_stall(false);
      let release_deadline = Instant::now() + Duration::from_secs(1);
      while camera.active.load(Ordering::Acquire) != 0 {
        assert!(
          Instant::now() < release_deadline,
          "delayed status did not release"
        );
        tokio::time::sleep(Duration::from_millis(5)).await;
      }
      tokio::time::sleep(Duration::from_millis(20)).await;
      let late = gateway.discover(timed(())).await.unwrap();
      assert!(
        late.metadata().get("eos-sample-age-ms").is_none(),
        "late probe must not report a young sample age"
      );
      assert_eq!(
        late
          .into_inner()
          .devices
          .iter()
          .find(|d| d.id == "front")
          .unwrap()
          .health,
        "degraded",
        "old health must remain unavailable after delayed status"
      );

      let recovered_by = Instant::now() + Duration::from_secs(2);
      loop {
        let fresh = gateway.discover(timed(())).await.unwrap();
        let complete = fresh.metadata().get("eos-sample-age-ms").is_some();
        let ready = fresh
          .into_inner()
          .devices
          .iter()
          .find(|d| d.id == "front")
          .unwrap()
          .health
          == "ready";
        if ready && complete {
          break;
        }
        assert!(
          Instant::now() < recovered_by,
          "next complete probe did not restore availability"
        );
        tokio::time::sleep(Duration::from_millis(10)).await;
      }
    });
  server.shutdown();
}
