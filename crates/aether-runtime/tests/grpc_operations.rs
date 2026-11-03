use std::sync::Arc;

use aether_common::time::{Clock, SystemClock};
use aether_dispatcher::{CommandRequest, Movement, OutcomeStatus};
use aether_hal::{BaseMovement, CommandMeta, Profile};
use aether_runtime::{
  grpc::{Projection, ServerHandle, v2},
  Runtime,
};

fn profile() -> Profile {
  Profile::from_toml(include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  )))
  .unwrap()
}

#[test]
fn typed_operation_lookup_and_watch_survive_dispatcher_restart() {
  let directory = std::env::temp_dir().join(format!(
    "r02-operation-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  let clock: Arc<dyn Clock> = Arc::new(SystemClock);
  let runtime =
    Arc::new(Runtime::from_cxx_mock_profile_durable(profile(), clock.clone(), &directory).unwrap());
  let id = "g".repeat(128);
  let request = CommandRequest {
    node_path: "cores/eos/layers/hardware/groups/base/nodes/drive".into(),
    meta: CommandMeta {
      command_id: id.clone(),
      deadline_monotonic_ns: clock.monotonic_ns() + 2_000_000_000,
    },
    movement: Movement::Base(BaseMovement::BodyVelocity {
      x_m_s: 0.1,
      y_m_s: 0.0,
      yaw_rad_s: 0.0,
      valid_for_ms: 100,
    }),
  };
  let first = runtime
    .dispatcher()
    .dispatch_gateway_movement(request.clone(), "base.velocity");
  assert!(matches!(
    first.status,
    OutcomeStatus::Completed | OutcomeStatus::OutcomeUnknown
  ));
  let projection = Projection::new(runtime.clone()).unwrap();
  let mut server = ServerHandle::start(projection, 0).unwrap();
  let endpoint = format!("http://{}", server.address());
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut client = v2::capability_service_client::CapabilityServiceClient::connect(endpoint)
        .await
        .unwrap();
      let operation = client
        .get_operation(v2::OperationRequest {
          operation_id: id.clone(),
        })
        .await
        .unwrap()
        .into_inner();
      assert_eq!(operation.operation_id, id);
      assert_eq!(operation.accepted, Some(true));
      let ack = operation.acknowledgement.as_ref().unwrap();
      assert_eq!(
        ack.fields["primitive"].kind,
        Some(prost_types::value::Kind::StringValue(
          "base.velocity".into()
        ))
      );
      assert_eq!(
        ack.fields["command_id"].kind,
        Some(prost_types::value::Kind::StringValue(id.clone()))
      );
      let mut watch = client
        .watch_operation(v2::OperationRequest {
          operation_id: id.clone(),
        })
        .await
        .unwrap()
        .into_inner();
      assert_eq!(watch.message().await.unwrap().unwrap(), operation);
      assert!(watch.message().await.unwrap().is_none());
      let cancelled = client
        .cancel_operation(v2::OperationRequest {
          operation_id: id.clone(),
        })
        .await
        .unwrap()
        .into_inner();
      assert_eq!(cancelled, operation);
      let missing = client
        .get_operation(v2::OperationRequest {
          operation_id: "missing".into(),
        })
        .await
        .unwrap_err();
      assert_eq!(missing.code(), tonic::Code::NotFound);
    });
  server.shutdown();
  drop(server);
  drop(runtime);
  let recovered = Runtime::from_cxx_mock_profile_durable(profile(), clock, &directory).unwrap();
  let replay = recovered
    .dispatcher()
    .dispatch_gateway_movement(request, "base.velocity");
  assert_eq!(replay, first);
  assert_eq!(
    recovered
      .dispatcher()
      .gateway_operation(&id)
      .unwrap()
      .unwrap()
      .primitive,
    "base.velocity"
  );
  drop(recovered);
  std::fs::remove_dir_all(directory).unwrap();
}

#[test]
fn blocked_submit_does_not_hold_grpc_shutdown_or_admit_new_release() {
  use aether_dispatcher::AdapterRegistration;
  use aether_hal::cxx_bridge::{CxxMockBase, CxxMockCamera, CxxMockManipulator};
  use aether_runtime::grpc::Capabilities;
  use std::time::{Duration, Instant};

  let profile = profile();
  let clock: Arc<dyn Clock> = Arc::new(SystemClock);
  let base = Arc::new(
    CxxMockBase::from_profile(
      profile
        .node("cores/eos/layers/hardware/groups/base/nodes/drive")
        .unwrap(),
      clock.clone(),
    )
    .unwrap(),
  );
  base.set_mock_submit_delay_ms(2_000);
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
  let projection = Projection::new(runtime.clone()).unwrap();
  let direct = Capabilities::new(projection.clone());
  let mut server = ServerHandle::start(projection, 0).unwrap();
  let endpoint = format!("http://{}", server.address());
  let args = prost_types::Struct {
    fields: [
      (
        "linear_x_m_s".into(),
        prost_types::Value {
          kind: Some(prost_types::value::Kind::NumberValue(0.1)),
        },
      ),
      (
        "linear_y_m_s".into(),
        prost_types::Value {
          kind: Some(prost_types::value::Kind::NumberValue(0.0)),
        },
      ),
      (
        "angular_z_rad_s".into(),
        prost_types::Value {
          kind: Some(prost_types::value::Kind::NumberValue(0.0)),
        },
      ),
      (
        "valid_for_ms".into(),
        prost_types::Value {
          kind: Some(prost_types::value::Kind::NumberValue(100.0)),
        },
      ),
    ]
    .into_iter()
    .collect(),
  };
  let capability = "hardware/demo-0/base/drive/base.velocity@1.0.0".to_owned();
  let executor = tokio::runtime::Builder::new_multi_thread()
    .worker_threads(2)
    .enable_all()
    .build()
    .unwrap();
  executor.block_on(async {
    let mut expired = tonic::Request::new(v2::SubmitOperationRequest {
      capability_name: capability.clone(),
      arguments: Some(args.clone()),
      operation_id: "expired-before-release".into(),
    });
    expired
      .metadata_mut()
      .insert("grpc-timeout", "0n".parse().unwrap());
    let timeout =
      v2::capability_service_server::CapabilityService::submit_operation(&direct, expired)
        .await
        .unwrap_err();
    assert_eq!(timeout.code(), tonic::Code::DeadlineExceeded);
    assert!(!base.mock_submit_started());
    let health_channel = tonic::transport::Endpoint::from_shared(endpoint.clone())
      .unwrap()
      .connect()
      .await
      .unwrap();
    let mut health = tonic_health::pb::health_client::HealthClient::new(health_channel);
    let mut health_watch = health
      .watch(tonic_health::pb::HealthCheckRequest {
        service: String::new(),
      })
      .await
      .unwrap()
      .into_inner();
    assert_eq!(
      health_watch.message().await.unwrap().unwrap().status,
      tonic_health::pb::health_check_response::ServingStatus::Serving as i32
    );
    let mut client = v2::capability_service_client::CapabilityServiceClient::connect(endpoint)
      .await
      .unwrap();
    let request = v2::SubmitOperationRequest {
      capability_name: capability.clone(),
      arguments: Some(args.clone()),
      operation_id: "blocked-release".into(),
    };
    let submit = tokio::spawn(async move { client.submit_operation(request).await });
    let wait = Instant::now() + Duration::from_secs(1);
    while !base.mock_submit_started() {
      assert!(Instant::now() < wait, "mock release did not block");
      tokio::time::sleep(Duration::from_millis(5)).await;
    }
    let mut watch_request = tonic::Request::new(v2::OperationRequest {
      operation_id: "blocked-release".into(),
    });
    watch_request
      .metadata_mut()
      .insert("grpc-timeout", "100m".parse().unwrap());
    let mut pending =
      v2::capability_service_server::CapabilityService::watch_operation(&direct, watch_request)
        .await
        .unwrap()
        .into_inner();
    use tokio_stream::StreamExt;
    assert_eq!(
      pending.next().await.unwrap().unwrap().status,
      v2::OperationStatus::Pending as i32
    );
    assert_eq!(
      pending.next().await.unwrap().unwrap_err().code(),
      tonic::Code::DeadlineExceeded
    );
    server.begin_shutdown();
    assert_eq!(
      tokio::time::timeout(Duration::from_secs(1), health_watch.message())
        .await
        .unwrap()
        .unwrap()
        .unwrap()
        .status,
      tonic_health::pb::health_check_response::ServingStatus::NotServing as i32
    );
    runtime.dispatcher().quiesce();
    let direct_request = tonic::Request::new(v2::SubmitOperationRequest {
      capability_name: capability,
      arguments: Some(args),
      operation_id: "after-quiesce".into(),
    });
    let error =
      v2::capability_service_server::CapabilityService::submit_operation(&direct, direct_request)
        .await
        .unwrap_err();
    assert_eq!(error.code(), tonic::Code::Unavailable);
    let began = Instant::now();
    server.shutdown();
    assert!(
      began.elapsed() < Duration::from_millis(1500),
      "gRPC join held blocked adapter"
    );
    let stopped = runtime.dispatcher().emergency_stop();
    assert!(stopped.iter().all(|(_, result)| result.is_ok()));
    let _ = submit.await;
  });
}
