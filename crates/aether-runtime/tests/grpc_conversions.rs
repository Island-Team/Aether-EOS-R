use std::sync::Arc;

use aether_common::time::SystemClock;
use aether_hal::Profile;
use aether_runtime::{
  Runtime,
  grpc::{Projection, ServerHandle, v1, v2},
};

fn text(value: &str) -> prost_types::Value {
  prost_types::Value {
    kind: Some(prost_types::value::Kind::StringValue(value.into())),
  }
}

fn number(value: f64) -> prost_types::Value {
  prost_types::Value {
    kind: Some(prost_types::value::Kind::NumberValue(value)),
  }
}

fn numbers(values: &[f64]) -> prost_types::Value {
  prost_types::Value {
    kind: Some(prost_types::value::Kind::ListValue(
      prost_types::ListValue {
        values: values.iter().copied().map(number).collect(),
      },
    )),
  }
}

fn struct_fields(fields: Vec<(&str, prost_types::Value)>) -> prost_types::Struct {
  prost_types::Struct {
    fields: fields
      .into_iter()
      .map(|(name, value)| (name.into(), value))
      .collect(),
  }
}

fn profile(relative_base: bool) -> Profile {
  let source = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  ));
  if relative_base {
    let source = source
      .replace(
        "capabilities = [\"body_velocity\"]",
        "capabilities = [\"body_velocity\", \"relative_pose\"]",
      )
      .replace(
        "modes = [\"body_velocity\"]",
        "modes = [\"body_velocity\", \"relative_pose\"]",
      );
    Profile::from_toml(&source).unwrap()
  } else {
    Profile::from_toml(source).unwrap()
  }
}

fn replay(
  primitive: &str,
  arguments: prost_types::Struct,
  wrong_reference: prost_types::Struct,
  relative_base: bool,
) {
  let runtime = Arc::new(
    Runtime::from_cxx_mock_profile(profile(relative_base), Arc::new(SystemClock)).unwrap(),
  );
  let mut server = ServerHandle::start(Projection::new(runtime).unwrap(), 0).unwrap();
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
      let mut client = v2::capability_service_client::CapabilityServiceClient::connect(endpoint)
        .await
        .unwrap();
      let capabilities = client
        .list_capabilities(v2::ListCapabilitiesRequest {})
        .await
        .unwrap()
        .into_inner()
        .capabilities;
      let entry = capabilities
        .iter()
        .find(|entry| entry.primitive == primitive)
        .unwrap();
      let device = discovery
        .devices
        .iter()
        .find(|device| device.id == entry.device_id)
        .unwrap();
      let inline = device
        .primitives
        .iter()
        .find(|binding| binding.name == primitive)
        .unwrap();
      assert_eq!(inline.version, entry.version);
      assert!(entry.available);
      assert!(inline.references == ["relative"] || inline.references == ["absolute"]);
      let invalid = v2::SubmitOperationRequest {
        capability_name: entry.name.clone(),
        arguments: Some(wrong_reference),
        operation_id: format!("{primitive}-invalid"),
      };
      assert_eq!(
        client.submit_operation(invalid).await.unwrap_err().code(),
        tonic::Code::InvalidArgument
      );
      let request = v2::SubmitOperationRequest {
        capability_name: entry.name.clone(),
        arguments: Some(arguments),
        operation_id: format!("{primitive}-valid"),
      };
      let result = client
        .submit_operation(request.clone())
        .await
        .unwrap()
        .into_inner();
      assert_eq!(result.operation_id, request.operation_id);
      assert_eq!(result.accepted, Some(true));
      let ack = result.acknowledgement.as_ref().unwrap();
      assert_eq!(ack.fields["primitive"], text(primitive));
      assert_eq!(
        client
          .get_operation(v2::OperationRequest {
            operation_id: request.operation_id.clone()
          })
          .await
          .unwrap()
          .into_inner(),
        result
      );
      assert_eq!(
        client.submit_operation(request).await.unwrap().into_inner(),
        result
      );
    });
  server.shutdown();
}

#[test]
fn relative_base_navigation_is_exactly_mapped_and_rejects_absolute_reference() {
  let valid = struct_fields(vec![
    ("reference", text("relative")),
    ("x_m", number(0.1)),
    ("y_m", number(0.0)),
    ("yaw_rad", number(0.1)),
  ]);
  let mut invalid = valid.clone();
  invalid.fields.insert("reference".into(), text("absolute"));
  replay("base.navigate", valid, invalid, true);
}

#[test]
fn absolute_arm_joints_use_configured_order_and_reject_relative_reference() {
  let valid = struct_fields(vec![
    ("reference", text("absolute")),
    (
      "joint_positions_rad",
      numbers(&[0.0, 0.1, 0.0, 0.0, 0.0, 0.0]),
    ),
    ("speed_rad_s", number(0.2)),
  ]);
  let mut invalid = valid.clone();
  invalid.fields.insert("reference".into(), text("relative"));
  replay("arm.movej", valid, invalid, false);
}

#[test]
fn absolute_arm_tool_pose_requires_admitted_frame_and_unit_quaternion() {
  let valid = struct_fields(vec![
    ("reference", text("absolute")),
    ("frame_id", text("arm_base")),
    ("position_m", numbers(&[0.1, 0.0, 0.0])),
    ("orientation_quat_xyzw", numbers(&[0.0, 0.0, 0.0, 1.0])),
    ("speed_m_s", number(0.1)),
  ]);
  let mut invalid = valid.clone();
  invalid.fields.insert("frame_id".into(), text("wrong"));
  replay("arm.movel", valid, invalid, false);
}
