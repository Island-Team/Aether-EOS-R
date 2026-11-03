use aether_hal::{
  ControlledStopOutcome, HalError, Health, HealthState, NodeAdapter, NodeKind, NodeState,
  NodeStatus,
};

struct LatchingOnly;

impl NodeAdapter for LatchingOnly {
  fn kind(&self) -> NodeKind {
    NodeKind::Base
  }
  fn adapter_type(&self) -> &str {
    "latching-only"
  }
  fn status(&self) -> NodeStatus {
    NodeStatus {
      state: NodeState::Stopped,
      active_command_id: None,
    }
  }
  fn health(&self) -> Health {
    Health {
      state: HealthState::Healthy,
      code: None,
      message: None,
      last_ok_monotonic_ns: Some(0),
    }
  }
  fn stop(&self) -> Result<(), HalError> {
    Ok(())
  }
  fn has_terminal_stop_fence(&self) -> bool {
    true
  }
}

#[test]
fn existing_latching_stop_does_not_implicitly_advertise_controlled_stop() {
  let adapter = LatchingOnly;
  assert!(adapter.has_terminal_stop_fence());
  assert!(!adapter.supports_non_latching_stop());
  assert!(matches!(
    adapter.controlled_stop(),
    Err(HalError::UnsupportedMode(_))
  ));
}

#[test]
fn controlled_stop_outcomes_keep_failure_and_uncertainty_distinct() {
  assert_ne!(
    ControlledStopOutcome::ConfirmedFailure("known".into()),
    ControlledStopOutcome::OutcomeUnknown,
  );
}
