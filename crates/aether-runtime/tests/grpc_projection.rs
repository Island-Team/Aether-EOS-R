use std::sync::Arc;

use aether_common::time::SystemClock;
use aether_dispatcher::AdapterRegistration;
use aether_hal::{BaseObservation, ClockKind, MockBase, Profile, SampleMeta};
use aether_runtime::{
  Runtime,
  grpc::{Projection, ServerHandle, v1, v2},
};

#[test]
fn rejected_candidate_cannot_replace_serving_projection_or_mix_catalog_generations() {
  let profile = Profile::from_toml(include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  )))
  .unwrap();
  let runtime =
    Arc::new(Runtime::from_cxx_mock_profile(profile.clone(), Arc::new(SystemClock)).unwrap());
  let mut server = ServerHandle::start(Projection::new(runtime.clone()).unwrap(), 0).unwrap();
  let endpoint = format!("http://{}", server.address());

  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut gateway = v1::gateway_service_client::GatewayServiceClient::connect(endpoint.clone())
        .await
        .unwrap();
      let mut capabilities =
        v2::capability_service_client::CapabilityServiceClient::connect(endpoint)
          .await
          .unwrap();
      let original_discovery = gateway.discover(()).await.unwrap().into_inner();
      let original_resources = gateway.list_resources(()).await.unwrap().into_inner();
      let original_status = gateway.get_status(()).await.unwrap().into_inner();
      let original_catalog = capabilities
        .list_capabilities(v2::ListCapabilitiesRequest {})
        .await
        .unwrap()
        .into_inner();

      // A proposed new generation with an incomplete adapter set fails admission.
      // R02 has no in-process graph swap: the existing listener keeps its
      // previously admitted immutable projection.
      assert!(Runtime::new(profile, Vec::new(), Arc::new(SystemClock)).is_err());
      assert_eq!(
        gateway.discover(()).await.unwrap().into_inner(),
        original_discovery
      );
      assert_eq!(
        gateway.list_resources(()).await.unwrap().into_inner(),
        original_resources
      );
      assert_eq!(
        gateway.get_status(()).await.unwrap().into_inner(),
        original_status
      );
      assert_eq!(
        capabilities
          .list_capabilities(v2::ListCapabilitiesRequest {})
          .await
          .unwrap()
          .into_inner(),
        original_catalog
      );

      // Live status may degrade without changing the admitted binding identity.
      runtime.dispatcher().emergency_stop();
      let degraded = gateway.discover(()).await.unwrap().into_inner();
      assert_eq!(degraded.robot.as_ref().unwrap().health, "degraded");
      assert!(degraded.robot.as_ref().unwrap().estop);
      assert_eq!(
        gateway.list_resources(()).await.unwrap().into_inner(),
        original_resources
      );
      let degraded_status = gateway.get_status(()).await.unwrap().into_inner();
      for field in ["robot_id", "robot_name", "version", "started_at"] {
        assert_eq!(degraded_status.fields[field], original_status.fields[field]);
      }
      assert_ne!(
        degraded_status.fields["status"],
        original_status.fields["status"]
      );
      assert_eq!(degraded.devices.len(), original_discovery.devices.len());
      for (current, original) in degraded.devices.iter().zip(&original_discovery.devices) {
        assert_eq!(current.id, original.id);
        assert_eq!(current.part, original.part);
        assert_eq!(current.r#class, original.r#class);
      }
      let current_catalog = capabilities
        .list_capabilities(v2::ListCapabilitiesRequest {})
        .await
        .unwrap()
        .into_inner();
      assert_eq!(
        current_catalog
          .capabilities
          .iter()
          .map(|entry| (&entry.name, &entry.primitive, &entry.device_id))
          .collect::<Vec<_>>(),
        original_catalog
          .capabilities
          .iter()
          .map(|entry| (&entry.name, &entry.primitive, &entry.device_id))
          .collect::<Vec<_>>()
      );
      assert!(current_catalog
        .capabilities
        .iter()
        .filter(|entry| entry.primitive != "safety.estop")
        .all(|entry| !entry.available));
    });
  server.shutdown();
}

#[test]
fn more_than_64_stop_targets_cannot_publish_a_serving_projection() {
  let mut source = "version = \"1.0.0\"\n[robot]\nid = \"r\"\nname = \"R\"\n".to_owned();
  let mut adapters = Vec::new();
  for index in 0..65 {
    let id = format!("base{index:02}");
    source.push_str(&format!(
      "[cores.eos.layers.hardware.groups.base.nodes.{id}]\nid=\"{id}\"\nnode_type=\"eos.hardware.base@v1.0\"\nlabel=\"{id}\"\ndescription=\"Base\"\nframe_id=\"base_link\"\ncapabilities=[\"body_velocity\"]\n[cores.eos.layers.hardware.groups.base.nodes.{id}.adapter]\ntype=\"mock\"\n[cores.eos.layers.hardware.groups.base.nodes.{id}.health_timing]\npoll_interval_ms=10\nstale_after_ms=1000\n[cores.eos.layers.hardware.groups.base.nodes.{id}.observation]\nmax_rate_hz=50.0\nmax_payload_bytes=4096\n[cores.eos.layers.hardware.groups.base.nodes.{id}.movement]\nmodes=[\"body_velocity\"]\n[cores.eos.layers.hardware.groups.base.nodes.{id}.movement.limits]\nmax_linear_m_s=1.0\nmax_angular_rad_s=1.0\nmax_velocity_lease_ms=500\n[cores.eos.layers.dispatcher.groups.control.nodes.{id}]\nid=\"{id}\"\nnode_type=\"eos.dispatcher.control@v1.0\"\nlabel=\"Control\"\ndescription=\"Control\"\n[cores.eos.layers.hardware.groups.base.nodes.{id}.inputs.command]\nnode=\"cores/eos/layers/dispatcher/groups/control/nodes/{id}\"\noutput=\"base_command\"\n"
    ));
    let observation = BaseObservation {
      meta: SampleMeta {
        source_timestamp_ns: None,
        clock: ClockKind::Unknown,
        received_monotonic_ns: 0,
        sequence: None,
        frame_id: "base_link".into(),
      },
      valid: true,
      x_m: 0.0,
      y_m: 0.0,
      yaw_rad: 0.0,
      x_m_s: 0.0,
      y_m_s: 0.0,
      yaw_rad_s: 0.0,
    };
    adapters.push(AdapterRegistration::Base {
      node_id: id,
      adapter: Arc::new(MockBase::new(observation)),
    });
  }
  let profile = Profile::from_toml(&source).unwrap();
  let runtime = Arc::new(Runtime::new(profile, adapters, Arc::new(SystemClock)).unwrap());
  assert!(Projection::new(runtime).is_err());
}
