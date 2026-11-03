use std::sync::Arc;

use aether_common::time::SystemClock;
use aether_hal::Profile;
use aether_runtime::{
  Runtime,
  grpc::{Capabilities, Projection, ServerHandle, v2},
};
use tonic::Request;

fn fixture() -> (Arc<Runtime>, Projection, ServerHandle) {
  let profile = Profile::from_toml(include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  )))
  .unwrap();
  let runtime = Arc::new(Runtime::from_cxx_mock_profile(profile, Arc::new(SystemClock)).unwrap());
  let projection = Projection::new(runtime.clone()).unwrap();
  let server = ServerHandle::start(projection.clone(), 0).unwrap();
  (runtime, projection, server)
}

fn number(value: f64) -> prost_types::Value {
  prost_types::Value {
    kind: Some(prost_types::value::Kind::NumberValue(value)),
  }
}

fn velocity_arguments() -> prost_types::Struct {
  prost_types::Struct {
    fields: [
      ("linear_x_m_s".into(), number(0.1)),
      ("linear_y_m_s".into(), number(0.0)),
      ("angular_z_rad_s".into(), number(0.0)),
      ("valid_for_ms".into(), number(100.0)),
    ]
    .into(),
  }
}

fn request(name: &str, id: &str) -> v2::SubmitOperationRequest {
  v2::SubmitOperationRequest {
    capability_name: name.into(),
    arguments: Some(velocity_arguments()),
    operation_id: id.into(),
  }
}

#[test]
fn malformed_or_unadvertised_submit_fails_before_reservation() {
  let (runtime, _, mut server) = fixture();
  let endpoint = format!("http://{}", server.address());
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut client = v2::capability_service_client::CapabilityServiceClient::connect(endpoint)
        .await
        .unwrap();
      let catalog = client
        .list_capabilities(v2::ListCapabilitiesRequest {})
        .await
        .unwrap()
        .into_inner()
        .capabilities;
      let velocity = catalog
        .iter()
        .find(|entry| entry.primitive == "base.velocity")
        .unwrap();
      assert!(!catalog
        .iter()
        .any(|entry| entry.primitive == "base.navigate"));
      let name = velocity.name.as_str();

      let mut missing = request(name, "missing-arguments");
      missing.arguments = None;
      assert_eq!(
        client.submit_operation(missing).await.unwrap_err().code(),
        tonic::Code::InvalidArgument
      );
      assert!(runtime
        .dispatcher()
        .gateway_operation("missing-arguments")
        .unwrap()
        .is_none());

      for id in [String::new(), "x".repeat(129)] {
        assert_eq!(
          client
            .submit_operation(request(name, &id))
            .await
            .unwrap_err()
            .code(),
          tonic::Code::InvalidArgument
        );
      }

      for (name, id) in [
        (
          "hardware/demo-0/base/drive/not-a-primitive@1.0.0",
          "unknown-name",
        ),
        (
          "hardware/demo-0/base/drive/base.navigate@1.0.0",
          "unsupported-conversion",
        ),
      ] {
        assert_eq!(
          client
            .submit_operation(request(name, id))
            .await
            .unwrap_err()
            .code(),
          tonic::Code::NotFound
        );
        assert!(runtime
          .dispatcher()
          .gateway_operation(id)
          .unwrap()
          .is_none());
      }
    });
  server.shutdown();
}

#[test]
fn expired_deadline_and_transport_request_id_cannot_replace_operation_id() {
  use v2::capability_service_server::CapabilityService;

  let (runtime, projection, mut server) = fixture();
  let endpoint = format!("http://{}", server.address());
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut client = v2::capability_service_client::CapabilityServiceClient::connect(endpoint)
        .await
        .unwrap();
      let name = client
        .list_capabilities(v2::ListCapabilitiesRequest {})
        .await
        .unwrap()
        .into_inner()
        .capabilities
        .into_iter()
        .find(|entry| entry.primitive == "base.velocity")
        .unwrap()
        .name;
      let direct = Capabilities::new(projection);
      let mut expired = Request::new(request(&name, "expired-before-reservation"));
      expired
        .metadata_mut()
        .insert("grpc-timeout", "0n".parse().unwrap());
      assert_eq!(
        direct.submit_operation(expired).await.unwrap_err().code(),
        tonic::Code::DeadlineExceeded
      );
      assert!(runtime
        .dispatcher()
        .gateway_operation("expired-before-reservation")
        .unwrap()
        .is_none());

      let mut empty_id = Request::new(request(&name, ""));
      empty_id
        .metadata_mut()
        .insert("x-request-id", "transport-only".parse().unwrap());
      assert_eq!(
        direct.submit_operation(empty_id).await.unwrap_err().code(),
        tonic::Code::InvalidArgument
      );
      assert!(runtime
        .dispatcher()
        .gateway_operation("transport-only")
        .unwrap()
        .is_none());

      let mut admitted = Request::new(request(&name, "body-command"));
      admitted
        .metadata_mut()
        .insert("x-request-id", "transport-only".parse().unwrap());
      let result = client
        .submit_operation(admitted)
        .await
        .unwrap()
        .into_inner();
      assert_eq!(result.operation_id, "body-command");
      assert_eq!(
        result.acknowledgement.as_ref().unwrap().fields["command_id"].kind,
        Some(prost_types::value::Kind::StringValue("body-command".into()))
      );
      assert_eq!(
        client
          .get_operation(v2::OperationRequest {
            operation_id: "transport-only".into()
          })
          .await
          .unwrap_err()
          .code(),
        tonic::Code::NotFound
      );
      assert_eq!(
        client
          .get_operation(v2::OperationRequest {
            operation_id: "body-command".into()
          })
          .await
          .unwrap()
          .into_inner(),
        result
      );
    });
  server.shutdown();
}
