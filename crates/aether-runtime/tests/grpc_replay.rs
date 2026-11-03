use std::sync::{
  Arc,
  atomic::{AtomicUsize, Ordering},
};

use aether_common::time::{Clock, SystemClock};
use aether_dispatcher::AdapterRegistration;
use aether_hal::{
  BaseMovement, BaseNode, BaseObservation, CommandMeta, CommandOutcome, HalError, Health,
  NodeAdapter, NodeKind, NodeStatus, Profile,
  cxx_bridge::{CxxMockBase, CxxMockCamera, CxxMockManipulator},
};
use aether_runtime::{
  Runtime,
  grpc::{Capabilities, Projection, ServerHandle, v2},
};

struct CountedBase {
  inner: Arc<CxxMockBase>,
  releases: AtomicUsize,
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
    self.inner.stop()
  }
  fn has_terminal_stop_fence(&self) -> bool {
    true
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
    self.releases.fetch_add(1, Ordering::SeqCst);
    self.inner.move_base(meta, movement)
  }
}

fn profile() -> Profile {
  Profile::from_toml(include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  )))
  .unwrap()
}

fn create_runtime(
  profile: Profile,
  base: Arc<CountedBase>,
  directory: &std::path::Path,
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
  let arm = Arc::new(
    CxxMockManipulator::from_profile(
      profile
        .node("cores/eos/layers/hardware/groups/manipulator/nodes/arm")
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
        AdapterRegistration::Camera {
          node_id: "front".into(),
          adapter: camera,
        },
        AdapterRegistration::Manipulator {
          node_id: "arm".into(),
          adapter: arm,
        },
      ],
      clock,
      directory,
    )
    .unwrap(),
  )
}

fn request() -> v2::SubmitOperationRequest {
  let number = |value| prost_types::Value {
    kind: Some(prost_types::value::Kind::NumberValue(value)),
  };
  v2::SubmitOperationRequest {
    capability_name: "hardware/demo-0/base/drive/base.velocity@1.0.0".into(),
    arguments: Some(prost_types::Struct {
      fields: [
        ("linear_x_m_s".into(), number(0.1)),
        ("linear_y_m_s".into(), number(0.0)),
        ("angular_z_rad_s".into(), number(0.0)),
        ("valid_for_ms".into(), number(100.0)),
      ]
      .into(),
    }),
    operation_id: "concurrent-and-lost-response".into(),
  }
}

#[test]
fn concurrent_duplicate_and_lost_submit_response_never_release_again_after_restart() {
  let directory = std::env::temp_dir().join(format!(
    "r02-grpc-replay-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  let profile = profile();
  let inner = Arc::new(
    CxxMockBase::from_profile(
      profile
        .node("cores/eos/layers/hardware/groups/base/nodes/drive")
        .unwrap(),
      Arc::new(SystemClock),
    )
    .unwrap(),
  );
  inner.set_mock_submit_delay_ms(250);
  let base = Arc::new(CountedBase {
    inner: inner.clone(),
    releases: AtomicUsize::new(0),
  });
  let runtime = create_runtime(profile.clone(), base.clone(), &directory);
  let projection = Projection::new(runtime.clone()).unwrap();
  let direct = Capabilities::new(projection.clone());
  let mut server = ServerHandle::start(projection, 0).unwrap();
  let endpoint = format!("http://{}", server.address());
  tokio::runtime::Builder::new_multi_thread()
    .worker_threads(2)
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut first =
        v2::capability_service_client::CapabilityServiceClient::connect(endpoint.clone())
          .await
          .unwrap();
      let mut duplicate =
        v2::capability_service_client::CapabilityServiceClient::connect(endpoint.clone())
          .await
          .unwrap();
      let submit = request();
      let mut first_request = tonic::Request::new(submit.clone());
      first_request
        .metadata_mut()
        .insert("x-request-id", "transport-one".parse().unwrap());
      let lost_response = tokio::spawn(async move { first.submit_operation(first_request).await });
      let deadline = tokio::time::Instant::now() + std::time::Duration::from_secs(2);
      while !inner.mock_submit_started() {
        assert!(tokio::time::Instant::now() < deadline);
        tokio::time::sleep(std::time::Duration::from_millis(5)).await;
      }
      use tokio_stream::StreamExt;
      let lookup = || v2::OperationRequest {
        operation_id: submit.operation_id.clone(),
      };
      let mut first_watch = v2::capability_service_server::CapabilityService::watch_operation(
        &direct,
        tonic::Request::new(lookup()),
      )
      .await
      .unwrap()
      .into_inner();
      assert_eq!(
        first_watch.next().await.unwrap().unwrap().status,
        v2::OperationStatus::Pending as i32
      );
      drop(first_watch);
      let mut resumed_watch = v2::capability_service_server::CapabilityService::watch_operation(
        &direct,
        tonic::Request::new(lookup()),
      )
      .await
      .unwrap()
      .into_inner();
      assert_eq!(
        resumed_watch.next().await.unwrap().unwrap().status,
        v2::OperationStatus::Pending as i32
      );
      let mut duplicate_request = tonic::Request::new(submit.clone());
      duplicate_request
        .metadata_mut()
        .insert("x-request-id", "transport-two".parse().unwrap());
      let joined = duplicate
        .submit_operation(duplicate_request)
        .await
        .unwrap()
        .into_inner();
      lost_response.abort();
      assert_eq!(joined.operation_id, submit.operation_id);
      assert_eq!(joined.status, v2::OperationStatus::Pending as i32);
      let terminal = tokio::time::timeout(std::time::Duration::from_secs(2), resumed_watch.next())
        .await
        .unwrap()
        .unwrap()
        .unwrap();
      assert_eq!(terminal.status, v2::OperationStatus::OutcomeUnknown as i32);
      assert!(resumed_watch.next().await.is_none());
      let settled = duplicate
        .get_operation(v2::OperationRequest {
          operation_id: submit.operation_id.clone(),
        })
        .await
        .unwrap()
        .into_inner();
      assert_eq!(settled.operation_id, submit.operation_id);
      assert_eq!(terminal, settled);
      assert_eq!(base.releases.load(Ordering::SeqCst), 1);
      let mut changed = submit.clone();
      changed.arguments.as_mut().unwrap().fields.insert(
        "linear_x_m_s".into(),
        prost_types::Value {
          kind: Some(prost_types::value::Kind::NumberValue(0.2)),
        },
      );
      assert_eq!(
        duplicate
          .submit_operation(changed)
          .await
          .unwrap_err()
          .code(),
        tonic::Code::AlreadyExists
      );
      assert_eq!(base.releases.load(Ordering::SeqCst), 1);
      let changed_binding = v2::SubmitOperationRequest {
        capability_name: "runtime/demo-0/safety.estop@1.0.0".into(),
        arguments: Some(prost_types::Struct::default()),
        operation_id: submit.operation_id.clone(),
      };
      assert_eq!(
        duplicate
          .submit_operation(changed_binding)
          .await
          .unwrap_err()
          .code(),
        tonic::Code::AlreadyExists
      );
      assert_eq!(base.releases.load(Ordering::SeqCst), 1);
      assert!(!runtime.dispatcher().emergency_stopped());
    });
  server.shutdown();
  drop(server);
  drop(direct);
  drop(runtime);

  let recovered = create_runtime(profile, base.clone(), &directory);
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
      let before = client
        .get_operation(v2::OperationRequest {
          operation_id: request().operation_id,
        })
        .await
        .unwrap()
        .into_inner();
      let after = client
        .submit_operation(request())
        .await
        .unwrap()
        .into_inner();
      assert_eq!(after, before);
      assert_eq!(base.releases.load(Ordering::SeqCst), 1);
    });
  replay.shutdown();
  drop(replay);
  drop(recovered);
  std::fs::remove_dir_all(directory).unwrap();
}

#[test]
fn cancel_releasing_command_is_unknown_and_does_not_release_again() {
  let directory = std::env::temp_dir().join(format!(
    "r02-grpc-cancel-releasing-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  let profile = profile();
  let inner = Arc::new(
    CxxMockBase::from_profile(
      profile
        .node("cores/eos/layers/hardware/groups/base/nodes/drive")
        .unwrap(),
      Arc::new(SystemClock),
    )
    .unwrap(),
  );
  inner.set_mock_submit_delay_ms(500);
  let base = Arc::new(CountedBase {
    inner: inner.clone(),
    releases: AtomicUsize::new(0),
  });
  let runtime = create_runtime(profile, base.clone(), &directory);
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
      let mut first = client.clone();
      let mut submit = request();
      submit.operation_id = "cancel-during-releasing".into();
      let pending = tokio::spawn(async move { first.submit_operation(submit).await });
      let deadline = tokio::time::Instant::now() + std::time::Duration::from_secs(2);
      while !inner.mock_submit_started() {
        assert!(tokio::time::Instant::now() < deadline);
        tokio::time::sleep(std::time::Duration::from_millis(5)).await;
      }
      assert_eq!(base.releases.load(Ordering::SeqCst), 1);
      let lookup = || v2::OperationRequest {
        operation_id: "cancel-during-releasing".into(),
      };
      let cancelled = client
        .cancel_operation(lookup())
        .await
        .unwrap()
        .into_inner();
      assert_eq!(cancelled.status, v2::OperationStatus::OutcomeUnknown as i32);
      assert_eq!(cancelled.accepted, Some(true));
      assert_eq!(
        client.get_operation(lookup()).await.unwrap().into_inner(),
        cancelled
      );
      let original = pending.await.unwrap().unwrap().into_inner();
      assert_eq!(original, cancelled);
      assert_eq!(base.releases.load(Ordering::SeqCst), 1);
      let mut duplicate = request();
      duplicate.operation_id = "cancel-during-releasing".into();
      assert_eq!(
        client
          .submit_operation(duplicate)
          .await
          .unwrap()
          .into_inner(),
        cancelled
      );
      assert_eq!(base.releases.load(Ordering::SeqCst), 1);
    });
  server.shutdown();
  drop(server);
  drop(runtime);
  std::fs::remove_dir_all(directory).unwrap();
}
