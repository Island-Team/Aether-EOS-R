use std::sync::Arc;

use aether_common::time::{Clock, ManualClock};
use aether_dispatcher::{AdapterRegistration, CommandRequest, Movement, OutcomeStatus};
use aether_hal::{
  BaseMovement, BaseNode, BaseObservation, ClockKind, CommandMeta, CommandOutcome, HalError,
  Health, MockBase, NodeAdapter, NodeKind, NodeStatus, Profile, SampleMeta,
};
use aether_runtime::{
  Runtime,
  grpc::{Projection, ServerHandle, v1, v2},
};

const BASE_PATH: &str = "cores/eos/layers/hardware/groups/base/nodes/drive";

struct ConfirmedBase(Arc<MockBase>);

impl NodeAdapter for ConfirmedBase {
  fn kind(&self) -> NodeKind {
    self.0.kind()
  }
  fn adapter_type(&self) -> &str {
    self.0.adapter_type()
  }
  fn status(&self) -> NodeStatus {
    self.0.status()
  }
  fn health(&self) -> Health {
    self.0.health()
  }
  fn stop(&self) -> Result<(), HalError> {
    self.0.stop()
  }
}

impl BaseNode for ConfirmedBase {
  fn observe(&self) -> Result<BaseObservation, HalError> {
    self.0.observe()
  }
  fn move_base(
    &self,
    meta: CommandMeta,
    movement: BaseMovement,
  ) -> Result<CommandOutcome, HalError> {
    self.0.move_base(meta, movement)?;
    Ok(CommandOutcome::Completed)
  }
}

fn profile() -> Profile {
  Profile::from_toml(
    r#"
version = "1.0.0"
[robot]
id = "legacy-robot"
name = "Legacy robot"
[cores.eos.layers.hardware.groups.base.nodes.drive]
id = "drive"
node_type = "eos.hardware.base@v1.0"
label = "Drive"
description = "Counting base"
frame_id = "base_link"
capabilities = ["body_velocity"]
[cores.eos.layers.hardware.groups.base.nodes.drive.adapter]
type = "mock"
[cores.eos.layers.hardware.groups.base.nodes.drive.health_timing]
poll_interval_ms = 10
stale_after_ms = 1000
[cores.eos.layers.hardware.groups.base.nodes.drive.observation]
max_rate_hz = 50.0
max_payload_bytes = 4096
[cores.eos.layers.hardware.groups.base.nodes.drive.movement]
modes = ["body_velocity"]
[cores.eos.layers.hardware.groups.base.nodes.drive.movement.limits]
max_linear_m_s = 1.0
max_angular_rad_s = 1.0
max_velocity_lease_ms = 500
[cores.eos.layers.dispatcher.groups.control.nodes.command]
id = "command"
node_type = "eos.dispatcher.control@v1.0"
label = "Command"
description = "Local command admission"
[cores.eos.layers.hardware.groups.base.nodes.drive.inputs.command]
node = "cores/eos/layers/dispatcher/groups/control/nodes/command"
output = "base_command"
"#,
  )
  .unwrap()
}

fn request() -> CommandRequest {
  CommandRequest {
    node_path: BASE_PATH.into(),
    meta: CommandMeta {
      command_id: "legacy-direct-command".into(),
      deadline_monotonic_ns: 1_000_000_000,
    },
    movement: Movement::Base(BaseMovement::BodyVelocity {
      x_m_s: 0.1,
      y_m_s: 0.0,
      yaw_rad_s: 0.0,
      valid_for_ms: 100,
    }),
  }
}

#[test]
fn legacy_record_is_query_only_data_loss_after_restart_without_releasing_again() {
  let directory = std::env::temp_dir().join(format!(
    "r02-legacy-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  let base = Arc::new(MockBase::new(BaseObservation {
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
  }));
  let clock: Arc<dyn Clock> = Arc::new(ManualClock::new(0, None));
  let open = || {
    Runtime::new_durable(
      profile(),
      vec![AdapterRegistration::Base {
        node_id: "drive".into(),
        adapter: Arc::new(ConfirmedBase(base.clone())),
      }],
      clock.clone(),
      &directory,
    )
    .unwrap()
  };
  let first = open();
  let legacy_result = first.dispatcher().dispatch(request());
  assert_eq!(legacy_result.status, OutcomeStatus::Completed);
  assert_eq!(base.commands().len(), 1);
  drop(first);

  let recovered = Arc::new(open());
  assert!(recovered.dispatcher().has_legacy_journal_records());
  let mut server = ServerHandle::start(Projection::new(recovered.clone()).unwrap(), 0).unwrap();
  let endpoint = format!("http://{}", server.address());
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut gateway = v1::gateway_service_client::GatewayServiceClient::connect(endpoint.clone())
        .await
        .unwrap();
      let discovered = gateway.discover(()).await.unwrap().into_inner();
      assert_eq!(discovered.robot.unwrap().health, "degraded");
      let status = gateway.get_status(()).await.unwrap().into_inner();
      assert_eq!(
        status.fields["status"].kind,
        Some(prost_types::value::Kind::StringValue("degraded".into()))
      );
      let mut operations =
        v2::capability_service_client::CapabilityServiceClient::connect(endpoint)
          .await
          .unwrap();
      let lookup = v2::OperationRequest {
        operation_id: "legacy-direct-command".into(),
      };
      assert_eq!(
        operations
          .get_operation(lookup.clone())
          .await
          .unwrap_err()
          .code(),
        tonic::Code::DataLoss
      );
      assert_eq!(
        operations
          .watch_operation(lookup.clone())
          .await
          .unwrap_err()
          .code(),
        tonic::Code::DataLoss
      );
      assert_eq!(
        operations
          .cancel_operation(lookup)
          .await
          .unwrap_err()
          .code(),
        tonic::Code::DataLoss
      );
    });
  assert_eq!(recovered.dispatcher().dispatch(request()), legacy_result);
  assert_eq!(base.commands().len(), 1, "legacy ID must not release again");
  server.shutdown();
  drop(server);
  drop(recovered);
  std::fs::remove_dir_all(directory).unwrap();
}
