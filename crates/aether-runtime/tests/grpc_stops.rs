use std::sync::Arc;

use aether_common::time::{Clock, SystemClock};
use aether_hal::Profile;
use aether_runtime::{
  Runtime,
  grpc::{Projection, ServerHandle, v1, v2},
};

fn profile() -> Profile {
  Profile::from_toml(include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  )))
  .unwrap()
}

#[test]
fn mock_advertises_only_proven_stop_bindings_and_replays_typed_results() {
  let directory = std::env::temp_dir().join(format!(
    "r02-stop-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  let clock: Arc<dyn Clock> = Arc::new(SystemClock);
  let runtime =
    Arc::new(Runtime::from_cxx_mock_profile_durable(profile(), clock.clone(), &directory).unwrap());
  let projection = Projection::new(runtime.clone()).unwrap();
  let mut server = ServerHandle::start(projection, 0).unwrap();
  let endpoint = format!("http://{}", server.address());
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut gateway = v1::gateway_service_client::GatewayServiceClient::connect(endpoint.clone())
        .await
        .unwrap();
      let discovery = gateway.discover(()).await.unwrap().into_inner();
      assert!(discovery.robot.unwrap().primitives.iter().any(|p| p.name == "safety.estop"));
      let base = discovery.devices.iter().find(|device| device.r#class == "base").unwrap();
      assert!(!base.primitives.iter().any(|p| p.name == "motion.stop"));
      let mut client = v2::capability_service_client::CapabilityServiceClient::connect(endpoint)
        .await
        .unwrap();
      let entries = client
        .list_capabilities(v2::ListCapabilitiesRequest {})
        .await
        .unwrap()
        .into_inner()
        .capabilities;
      assert!(!entries.iter().any(|entry| entry.primitive == "robot.stop"));
      assert!(!entries.iter().any(|entry| entry.primitive == "motion.stop"));
      let safety = entries.iter().find(|entry| entry.primitive == "safety.estop").unwrap();
      assert!(safety.device_id.is_empty());
      let stop = v2::SubmitOperationRequest {
        capability_name: safety.name.clone(),
        arguments: Some(prost_types::Struct::default()),
        operation_id: "estop-1".into(),
      };
      let estop = client.submit_operation(stop.clone()).await.unwrap().into_inner();
      assert_eq!(estop.operation_id, "estop-1");
      assert_eq!(estop.accepted, Some(true));
      assert!(matches!(estop.status, status if status == v2::OperationStatus::Succeeded as i32 || status == v2::OperationStatus::OutcomeUnknown as i32));
      assert_eq!(client.submit_operation(stop.clone()).await.unwrap().into_inner(), estop);
      assert_eq!(client.cancel_operation(v2::OperationRequest { operation_id: stop.operation_id.clone() }).await.unwrap().into_inner(), estop);
      let mut invalid = stop;
      invalid.operation_id = "estop-bad-args".into();
      invalid.arguments.as_mut().unwrap().fields.insert("extra".into(), prost_types::Value::default());
      assert_eq!(client.submit_operation(invalid).await.unwrap_err().code(), tonic::Code::InvalidArgument);
    });
  server.shutdown();
  drop(server);
  drop(runtime);
  let recovered = Runtime::from_cxx_mock_profile_durable(profile(), clock, &directory).unwrap();
  let estop = recovered
    .dispatcher()
    .gateway_operation("estop-1")
    .unwrap()
    .unwrap();
  assert_eq!(estop.primitive, "safety.estop");
  assert_eq!(
    estop.kind,
    aether_dispatcher::GatewayOperationKind::SafetyEstop
  );
  std::fs::remove_dir_all(directory).unwrap();
}
