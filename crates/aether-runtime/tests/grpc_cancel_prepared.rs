use std::sync::{
  Arc,
  atomic::{AtomicBool, AtomicUsize, Ordering},
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
  grpc::{Projection, ServerHandle, v2},
};

struct PausedObservationBase {
  inner: Arc<CxxMockBase>,
  entered: AtomicBool,
  release: AtomicBool,
  movement_calls: AtomicUsize,
}

impl NodeAdapter for PausedObservationBase {
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

impl BaseNode for PausedObservationBase {
  fn observe(&self) -> Result<BaseObservation, HalError> {
    self.entered.store(true, Ordering::SeqCst);
    while !self.release.load(Ordering::SeqCst) {
      std::thread::sleep(std::time::Duration::from_millis(1));
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

#[test]
fn cancel_prepared_operation_is_terminal_without_adapter_release() {
  let profile = Profile::from_toml(include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  )))
  .unwrap();
  let clock: Arc<dyn Clock> = Arc::new(SystemClock);
  let base = Arc::new(PausedObservationBase {
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
  let arm = Arc::new(
    CxxMockManipulator::from_profile(
      profile
        .node("cores/eos/layers/hardware/groups/manipulator/nodes/arm")
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
    )
    .unwrap(),
  );
  let mut server = ServerHandle::start(Projection::new(runtime).unwrap(), 0).unwrap();
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
      let capability = client
        .list_capabilities(v2::ListCapabilitiesRequest {})
        .await
        .unwrap()
        .into_inner()
        .capabilities
        .into_iter()
        .find(|entry| entry.primitive == "base.velocity")
        .unwrap();
      let number = |value| prost_types::Value {
        kind: Some(prost_types::value::Kind::NumberValue(value)),
      };
      let request = v2::SubmitOperationRequest {
        capability_name: capability.name,
        arguments: Some(prost_types::Struct {
          fields: [
            ("linear_x_m_s".into(), number(0.1)),
            ("linear_y_m_s".into(), number(0.0)),
            ("angular_z_rad_s".into(), number(0.0)),
            ("valid_for_ms".into(), number(100.0)),
          ]
          .into(),
        }),
        operation_id: "cancel-while-prepared".into(),
      };
      let mut submitting = client.clone();
      let submit = tokio::spawn(async move { submitting.submit_operation(request).await });
      let deadline = tokio::time::Instant::now() + std::time::Duration::from_secs(2);
      while !base.entered.load(Ordering::SeqCst) {
        assert!(tokio::time::Instant::now() < deadline);
        tokio::time::sleep(std::time::Duration::from_millis(5)).await;
      }
      let cancelled = client
        .cancel_operation(v2::OperationRequest {
          operation_id: "cancel-while-prepared".into(),
        })
        .await
        .unwrap()
        .into_inner();
      assert_eq!(cancelled.status, v2::OperationStatus::Cancelled as i32);
      assert_eq!(cancelled.accepted, Some(false));
      base.release.store(true, Ordering::SeqCst);
      assert_eq!(submit.await.unwrap().unwrap().into_inner(), cancelled);
      assert_eq!(base.movement_calls.load(Ordering::SeqCst), 0);
      assert_eq!(
        client
          .get_operation(v2::OperationRequest {
            operation_id: "cancel-while-prepared".into(),
          })
          .await
          .unwrap()
          .into_inner(),
        cancelled
      );
    });
  server.shutdown();
}
