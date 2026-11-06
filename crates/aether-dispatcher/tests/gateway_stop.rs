use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::time::Duration;

use aether_common::time::ManualClock;
use aether_dispatcher::{
  AdapterRegistration, AdmissionContext, AdmissionGeneration, AdmissionIngress, AdmittedBinding,
  AuthorityRole, CommandRequest, Dispatcher, GatewayLookupError, GatewayOperationKind,
  GatewayRejectionCode, GatewayTarget, Movement, OutcomeStatus, StopDispatchOutcome, StopKind,
  StopRequest,
};
use aether_hal::{
  BaseMovement, BaseNode, BaseObservation, ClockKind, CommandMeta, CommandOutcome,
  ControlledStopOutcome, HalError, Health, HealthState, NodeAdapter, NodeKind, NodeState,
  NodeStatus, Profile, SampleMeta,
};

const BASE_A: &str = "cores/eos/layers/hardware/groups/base/nodes/a";

pub(crate) struct ProofBase {
  state: Mutex<NodeState>,
  controlled: Mutex<ControlledStopOutcome>,
  controlled_calls: AtomicUsize,
  panic_on_controlled: std::sync::atomic::AtomicBool,
  emergency_calls: AtomicUsize,
  movement_calls: AtomicUsize,
  health_ns: AtomicU64,
  observation_ns: AtomicU64,
  movement_outcome: Mutex<CommandOutcome>,
  pause: Mutex<Option<(std::sync::mpsc::Sender<()>, std::sync::mpsc::Receiver<()>)>>,
  controlled_pause: Mutex<Option<(std::sync::mpsc::Sender<()>, std::sync::mpsc::Receiver<()>)>>,
  emergency_pause: Mutex<Option<(std::sync::mpsc::Sender<()>, std::sync::mpsc::Receiver<()>)>>,
}

impl ProofBase {
  pub(crate) fn new() -> Self {
    Self {
      state: Mutex::new(NodeState::Ready),
      controlled: Mutex::new(ControlledStopOutcome::Stopped),
      controlled_calls: AtomicUsize::new(0),
      panic_on_controlled: std::sync::atomic::AtomicBool::new(false),
      emergency_calls: AtomicUsize::new(0),
      movement_calls: AtomicUsize::new(0),
      health_ns: AtomicU64::new(0),
      observation_ns: AtomicU64::new(0),
      movement_outcome: Mutex::new(CommandOutcome::Completed),
      pause: Mutex::new(None),
      controlled_pause: Mutex::new(None),
      emergency_pause: Mutex::new(None),
    }
  }
  fn outcome(&self, outcome: ControlledStopOutcome) {
    *self.controlled.lock().unwrap() = outcome;
  }
  fn controlled_calls(&self) -> usize {
    self.controlled_calls.load(Ordering::SeqCst)
  }
  #[allow(dead_code)] // Also used when this fixture is included by dispatcher unit boundary tests.
  pub(crate) fn movement_outcome(&self, outcome: CommandOutcome) {
    *self.movement_outcome.lock().unwrap() = outcome;
  }
  #[allow(dead_code)]
  pub(crate) fn controlled_outcome(&self, outcome: ControlledStopOutcome) {
    self.outcome(outcome);
  }
  #[allow(dead_code)]
  pub(crate) fn calls(&self) -> usize {
    self.controlled_calls()
  }
  #[allow(dead_code)]
  pub(crate) fn movement_calls(&self) -> usize {
    self.movement_calls.load(Ordering::SeqCst)
  }
  #[allow(dead_code)]
  pub(crate) fn pause_controlled(
    &self,
    entered: std::sync::mpsc::Sender<()>,
    release: std::sync::mpsc::Receiver<()>,
  ) {
    *self.controlled_pause.lock().unwrap() = Some((entered, release));
  }
  #[allow(dead_code)]
  pub(crate) fn pause_movement(
    &self,
    entered: std::sync::mpsc::Sender<()>,
    release: std::sync::mpsc::Receiver<()>,
  ) {
    *self.pause.lock().unwrap() = Some((entered, release));
  }
  #[allow(dead_code)]
  pub(crate) fn set_state(&self, state: NodeState) {
    *self.state.lock().unwrap() = state;
  }
}

impl NodeAdapter for ProofBase {
  fn kind(&self) -> NodeKind {
    NodeKind::Base
  }
  fn adapter_type(&self) -> &str {
    "mock"
  }
  fn status(&self) -> NodeStatus {
    NodeStatus {
      state: *self.state.lock().unwrap(),
      active_command_id: None,
    }
  }
  fn health(&self) -> Health {
    Health {
      state: HealthState::Healthy,
      code: None,
      message: None,
      last_ok_monotonic_ns: Some(self.health_ns.load(Ordering::SeqCst)),
    }
  }
  fn stop(&self) -> Result<(), HalError> {
    self.emergency_calls.fetch_add(1, Ordering::SeqCst);
    if let Some((entered, release)) = self.emergency_pause.lock().unwrap().take() {
      let _ = entered.send(());
      let _ = release.recv();
    }
    *self.state.lock().unwrap() = NodeState::Stopped;
    Ok(())
  }
  fn has_terminal_stop_fence(&self) -> bool {
    true
  }
  fn supports_non_latching_stop(&self) -> bool {
    true
  }
  fn controlled_stop(&self) -> Result<ControlledStopOutcome, HalError> {
    self.controlled_calls.fetch_add(1, Ordering::SeqCst);
    if let Some((entered, release)) = self.controlled_pause.lock().unwrap().take() {
      let _ = entered.send(());
      let _ = release.recv();
    }
    if self.panic_on_controlled.load(Ordering::SeqCst) {
      panic!("injected process crash during controlled stop");
    }
    Ok(self.controlled.lock().unwrap().clone())
  }
}

impl BaseNode for ProofBase {
  fn observe(&self) -> Result<BaseObservation, HalError> {
    Ok(BaseObservation {
      meta: SampleMeta {
        source_timestamp_ns: None,
        clock: ClockKind::Unknown,
        received_monotonic_ns: self.observation_ns.load(Ordering::SeqCst),
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
    })
  }
  fn move_base(&self, _: CommandMeta, _: BaseMovement) -> Result<CommandOutcome, HalError> {
    if let Some((entered, release)) = self.pause.lock().unwrap().take() {
      entered.send(()).unwrap();
      release.recv().unwrap();
    }
    self.movement_calls.fetch_add(1, Ordering::SeqCst);
    Ok(*self.movement_outcome.lock().unwrap())
  }
  fn publish_base_stream_fenced(
    &self,
    meta: CommandMeta,
    movement: BaseMovement,
    _: Option<aether_hal::MovementFenceToken>,
  ) -> Result<CommandOutcome, HalError> {
    self.move_base(meta, movement)?;
    Ok(CommandOutcome::Accepted)
  }
}

fn profile() -> Profile {
  profile_with_nodes(&["a", "b"])
}

fn profile_with_nodes(ids: &[&str]) -> Profile {
  Profile::from_toml(&format!(
    "version = \"1.0.0\"\n[robot]\nid = \"r\"\nname = \"R\"\n{}",
    ids.iter().map(|id| node_toml(id)).collect::<String>()
  ))
  .unwrap()
}

fn node_toml(id: &str) -> String {
  format!(
    "[cores.eos.layers.hardware.groups.base.nodes.{id}]\nid = \"{id}\"\nnode_type = \"eos.hardware.base@v1.0\"\nlabel = \"{id}\"\ndescription = \"Proof base\"\nframe_id = \"base_link\"\ncapabilities = [\"body_velocity\"]\n[cores.eos.layers.hardware.groups.base.nodes.{id}.adapter]\ntype = \"mock\"\n[cores.eos.layers.hardware.groups.base.nodes.{id}.health_timing]\npoll_interval_ms = 10\nstale_after_ms = 1000\n[cores.eos.layers.hardware.groups.base.nodes.{id}.observation]\nmax_rate_hz = 50.0\nmax_payload_bytes = 4096\n[cores.eos.layers.hardware.groups.base.nodes.{id}.movement]\nmodes = [\"body_velocity\"]\n[cores.eos.layers.hardware.groups.base.nodes.{id}.movement.limits]\nmax_linear_m_s = 1.0\nmax_angular_rad_s = 1.0\nmax_velocity_lease_ms = 500\n[cores.eos.layers.dispatcher.groups.control.nodes.{id}]\nid = \"{id}\"\nnode_type = \"eos.dispatcher.control@v1.0\"\nlabel = \"Control\"\ndescription = \"Control\"\n[cores.eos.layers.hardware.groups.base.nodes.{id}.inputs.command]\nnode = \"cores/eos/layers/dispatcher/groups/control/nodes/{id}\"\noutput = \"base_command\"\n"
  )
}

pub(crate) fn directory() -> PathBuf {
  static NEXT: AtomicUsize = AtomicUsize::new(0);
  let path = std::env::temp_dir().join(format!(
    "r02-stop-{}-{}",
    std::process::id(),
    NEXT.fetch_add(1, Ordering::SeqCst)
  ));
  std::fs::create_dir(&path).unwrap();
  path
}

pub(crate) fn fixture(path: &PathBuf, a: Arc<ProofBase>, b: Arc<ProofBase>) -> Dispatcher {
  try_fixture(path, a, b).unwrap()
}

fn try_fixture(
  path: &PathBuf,
  a: Arc<ProofBase>,
  b: Arc<ProofBase>,
) -> Result<Dispatcher, aether_dispatcher::DispatchError> {
  try_fixture_profile(path, a, b, profile())
}

fn try_fixture_profile(
  path: &PathBuf,
  a: Arc<ProofBase>,
  b: Arc<ProofBase>,
  profile: Profile,
) -> Result<Dispatcher, aether_dispatcher::DispatchError> {
  Dispatcher::new_durable(
    &profile,
    vec![
      AdapterRegistration::Base {
        node_id: "a".into(),
        adapter: a,
      },
      AdapterRegistration::Base {
        node_id: "b".into(),
        adapter: b,
      },
    ],
    Arc::new(ManualClock::new(0, None)),
    path,
  )
}

fn try_fixture_nodes(
  path: &PathBuf,
  nodes: &[(&str, Arc<ProofBase>)],
) -> Result<Dispatcher, aether_dispatcher::DispatchError> {
  let ids: Vec<_> = nodes.iter().map(|(id, _)| *id).collect();
  let registrations = nodes
    .iter()
    .map(|(id, adapter)| AdapterRegistration::Base {
      node_id: (*id).into(),
      adapter: adapter.clone(),
    })
    .collect();
  Dispatcher::new_durable(
    &profile_with_nodes(&ids),
    registrations,
    Arc::new(ManualClock::new(0, None)),
    path,
  )
}

#[test]
fn durable_robot_control_cannot_be_reopened_as_another_profile_robot() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let dispatcher = fixture(&path, a.clone(), b.clone());
  assert_eq!(
    dispatcher
      .dispatch_gateway_stop(stop(
        "bound-estop",
        StopKind::SafetyEstop {
          robot_id: "r".into()
        },
        false,
      ))
      .status,
    OutcomeStatus::Completed,
  );
  drop(dispatcher);
  let before = (a.controlled_calls(), b.controlled_calls());
  let mut wrong = profile();
  wrong.robot.id = "other".into();
  assert!(matches!(
    try_fixture_profile(&path, a.clone(), b.clone(), wrong),
    Err(aether_dispatcher::DispatchError::Journal(_))
  ));
  assert_eq!((a.controlled_calls(), b.controlled_calls()), before);
  let recovered = fixture(&path, a.clone(), b.clone());
  assert!(recovered.emergency_stopped());
  drop(recovered);
  std::fs::remove_dir_all(path).unwrap();
}

struct SharedStopBase {
  inner: Arc<ProofBase>,
  epoch: AtomicU64,
  physical_calls: AtomicUsize,
}

impl NodeAdapter for SharedStopBase {
  fn kind(&self) -> NodeKind {
    NodeKind::Base
  }
  fn adapter_type(&self) -> &str {
    "mujoco-v2"
  }
  fn status(&self) -> NodeStatus {
    self.inner.status()
  }
  fn health(&self) -> Health {
    self.inner.health()
  }
  fn stop(&self) -> Result<(), HalError> {
    Ok(())
  }
  fn has_terminal_stop_fence(&self) -> bool {
    true
  }
  fn supports_non_latching_stop(&self) -> bool {
    true
  }
  fn movement_fence_token(&self) -> Option<aether_hal::MovementFenceToken> {
    Some(aether_hal::MovementFenceToken {
      generation_id: "00112233445566778899aabbccddeeff".into(),
      stop_epoch: self.epoch.load(Ordering::SeqCst).to_string(),
    })
  }
  fn managed_robot_stop(&self, _: Duration) -> Option<aether_hal::RobotStopProof> {
    self.physical_calls.fetch_add(1, Ordering::SeqCst);
    let epoch = self.epoch.fetch_add(1, Ordering::SeqCst) + 1;
    Some(aether_hal::RobotStopProof {
      base: ControlledStopOutcome::Stopped,
      arm: ControlledStopOutcome::Stopped,
      generation_id: "00112233445566778899aabbccddeeff".into(),
      stop_epoch: Some(epoch.to_string()),
    })
  }
}

impl BaseNode for SharedStopBase {
  fn observe(&self) -> Result<BaseObservation, HalError> {
    self.inner.observe()
  }
  fn move_base(
    &self,
    meta: CommandMeta,
    movement: BaseMovement,
  ) -> Result<CommandOutcome, HalError> {
    self.inner.move_base(meta, movement)
  }
}

fn shared_managed_fixture(
  path: &PathBuf,
) -> (Arc<Dispatcher>, Arc<SharedStopBase>, Arc<ProofBase>) {
  let inner = Arc::new(ProofBase::new());
  let adapter = Arc::new(SharedStopBase {
    inner: inner.clone(),
    epoch: AtomicU64::new(0),
    physical_calls: AtomicUsize::new(0),
  });
  let base = node_toml("a")
    .replace("[cores.eos.layers.hardware.groups.base.nodes.a.adapter]",
      "references = [\"cores/eos/layers/process/groups/backend/nodes/simulator\"]\n[cores.eos.layers.hardware.groups.base.nodes.a.adapter]")
    .replace("type = \"mock\"", "type = \"mujoco-v2\"\n[cores.eos.layers.hardware.groups.base.nodes.a.adapter.settings]\nsocket_path = \"/tmp/r06-stub.sock\"\nsimulator_node = \"base-0\"\ndevice_id = \"base-0\"\nbackend = \"cores/eos/layers/process/groups/backend/nodes/simulator\"");
  let source = format!("version = \"1.0.0\"\n[robot]\nid = \"r\"\nname = \"R\"\n{base}\n[cores.eos.layers.process.groups.backend.nodes.simulator]\nid = \"simulator\"\nnode_type = \"eos.process.backend@v1.0\"\nlabel = \"Simulator\"\ndescription = \"Stub backend\"\ncapabilities = [\"process\"]\n[cores.eos.layers.process.groups.backend.nodes.simulator.settings]\npackage = \"island/mujoco-sim\"\nversion = \"0.1.1\"\nscene = \"pick-place\"\nsocket = \"/tmp/r06-stub.sock\"\n");
  let profile = Profile::from_toml(&source).unwrap();
  let dispatcher = Arc::new(
    Dispatcher::new_durable_managed(
      &profile,
      vec![AdapterRegistration::Base {
        node_id: "a".into(),
        adapter: adapter.clone(),
      }],
      Arc::new(ManualClock::new(0, None)),
      path,
      AdmissionGeneration {
        run_id: "run-a".into(),
        active_revision: REVISION_A.into(),
        bindings: vec![AdmittedBinding {
          binding_path: BINDING_A.into(),
          node_path: BASE_A.into(),
          primitive: "base.velocity".into(),
          primitive_version: "1.0.0".into(),
        }],
      },
    )
    .unwrap(),
  );
  (dispatcher, adapter, inner)
}

#[test]
fn managed_routine_shutdown_resolves_only_its_marker_and_reopens_ready() {
  let path = directory();
  let (dispatcher, adapter, _) = shared_managed_fixture(&path);
  dispatcher.quiesce();
  let (results, persisted) = dispatcher.managed_shutdown_stop().unwrap();
  assert!(persisted && results.iter().all(|(_, result)| result.is_ok()));
  assert_eq!(adapter.physical_calls.load(Ordering::SeqCst), 1);
  assert!(dispatcher.movement_inhibited());
  assert!(!dispatcher.emergency_stopped());
  assert!(dispatcher.resolve_managed_shutdown());
  assert!(dispatcher.movement_inhibited());
  drop(dispatcher);
  let (next, _, _) = shared_managed_fixture(&path);
  assert!(!next.movement_inhibited());
  assert!(!next.emergency_stopped());
  drop(next);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn pending_shutdown_reopen_stays_inhibited_and_cannot_claim_old_marker() {
  let path = directory();
  let (dispatcher, _, _) = shared_managed_fixture(&path);
  dispatcher.quiesce();
  assert!(dispatcher.managed_shutdown_stop().unwrap().1);
  drop(dispatcher);
  let (next, _, _) = shared_managed_fixture(&path);
  assert!(next.movement_inhibited());
  next.quiesce();
  assert!(!next.resolve_managed_shutdown());
  assert!(!next.managed_shutdown_stop().unwrap().1);
  drop(next);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn routine_shutdown_never_clears_genuine_estop() {
  let path = directory();
  let (dispatcher, _, _) = shared_managed_fixture(&path);
  assert!(dispatcher
    .emergency_stop()
    .iter()
    .all(|(_, result)| result.is_ok()));
  dispatcher.quiesce();
  assert!(dispatcher.managed_shutdown_stop().unwrap().1);
  assert!(dispatcher.resolve_managed_shutdown());
  drop(dispatcher);
  let (next, _, _) = shared_managed_fixture(&path);
  assert!(next.emergency_stopped());
  drop(next);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn closed_managed_shutdown_gate_rejects_late_public_stop() {
  let path = directory();
  let (dispatcher, adapter, _) = shared_managed_fixture(&path);
  dispatcher.quiesce();
  let late = dispatcher.dispatch_managed_stop_checked(
    managed_context(AuthorityRole::EgiaControl, AdmissionIngress::Grpc),
    stop(
      "late-estop",
      StopKind::SafetyEstop {
        robot_id: "r".into(),
      },
      false,
    ),
  );
  assert_eq!(late.result().status, OutcomeStatus::Rejected);
  assert_eq!(adapter.physical_calls.load(Ordering::SeqCst), 0);
  assert!(!dispatcher.emergency_stopped());
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn failed_admitted_estop_latch_cannot_be_hidden_by_later_routine_marker_write() {
  let path = directory();
  let (dispatcher, adapter, _) = shared_managed_fixture(&path);
  let control = path.join(".control");
  let saved = path.join(".control.saved");
  std::fs::rename(&control, &saved).unwrap();
  std::fs::create_dir(&control).unwrap();
  let result = dispatcher.dispatch_managed_stop_checked(
    stop_context(AuthorityRole::EgiaControl, "safety.estop", None),
    stop(
      "failed-estop-latch",
      StopKind::SafetyEstop {
        robot_id: "r".into(),
      },
      false,
    ),
  );
  assert_eq!(result.result().status, OutcomeStatus::Rejected);
  assert!(dispatcher.emergency_stopped());
  std::fs::remove_dir(&control).unwrap();
  std::fs::rename(&saved, &control).unwrap();
  dispatcher.quiesce();
  let (parts, marker) = dispatcher.managed_shutdown_stop().unwrap();
  assert!(marker && parts.iter().all(|(_, part)| part.is_ok()));
  assert!(adapter.physical_calls.load(Ordering::SeqCst) >= 1);
  assert!(!dispatcher.resolve_managed_shutdown());
  drop(dispatcher);
  let (reopened, _, _) = shared_managed_fixture(&path);
  assert!(reopened.movement_inhibited());
  assert!(!reopened.emergency_stopped());
  assert!(!reopened.resolve_managed_shutdown());
  drop(reopened);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn routine_shutdown_intent_failure_still_reaches_physical_lane_and_cannot_resolve() {
  let path = directory();
  let (dispatcher, adapter, _) = shared_managed_fixture(&path);
  std::fs::remove_file(path.join(".control")).unwrap();
  std::fs::create_dir(path.join(".control")).unwrap();
  dispatcher.quiesce();
  let (results, persisted) = dispatcher.managed_shutdown_stop().unwrap();
  assert!(!persisted && results.iter().all(|(_, result)| result.is_err()));
  assert_eq!(adapter.physical_calls.load(Ordering::SeqCst), 1);
  assert!(!dispatcher.resolve_managed_shutdown());
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn routine_shutdown_marker_resolution_failure_stays_pending_in_memory() {
  let path = directory();
  let (dispatcher, adapter, _) = shared_managed_fixture(&path);
  dispatcher.quiesce();
  assert!(dispatcher.managed_shutdown_stop().unwrap().1);
  std::fs::remove_file(path.join(".control")).unwrap();
  std::fs::create_dir(path.join(".control")).unwrap();
  assert!(!dispatcher.resolve_managed_shutdown());
  assert_eq!(adapter.physical_calls.load(Ordering::SeqCst), 1);
  assert!(dispatcher.movement_inhibited());
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}

#[cfg(feature = "test-hooks")]
#[test]
fn routine_shutdown_physical_stop_preempts_stalled_movement_journal() {
  let path = directory();
  let (dispatcher, adapter, _) = shared_managed_fixture(&path);
  let (entered_tx, entered_rx) = std::sync::mpsc::channel();
  let (release_tx, release_rx) = std::sync::mpsc::channel();
  dispatcher.pause_journal_release_for_test(entered_tx, release_rx);
  let moving = {
    let dispatcher = dispatcher.clone();
    std::thread::spawn(move || {
      dispatcher.dispatch_managed_movement(
        managed_context(AuthorityRole::EgiaControl, AdmissionIngress::Grpc),
        move_a("stalled-before-routine-shutdown"),
      )
    })
  };
  entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
  dispatcher.quiesce();
  let stopping = {
    let dispatcher = dispatcher.clone();
    std::thread::spawn(move || dispatcher.managed_shutdown_stop().unwrap())
  };
  let until = std::time::Instant::now() + Duration::from_secs(1);
  while adapter.physical_calls.load(Ordering::SeqCst) == 0 && std::time::Instant::now() < until {
    std::thread::sleep(Duration::from_millis(5));
  }
  assert_eq!(adapter.physical_calls.load(Ordering::SeqCst), 1);
  release_tx.send(()).unwrap();
  let (results, marker) = stopping.join().unwrap();
  assert!(marker && results.iter().all(|(_, result)| result.is_ok()));
  assert_eq!(moving.join().unwrap().status, OutcomeStatus::OutcomeUnknown);
  // The newer physical stop epoch is proved and the old release has reached
  // a terminal unknown state. Resolution fences it durably, preserving the
  // recovery inhibit while removing only this routine marker.
  assert!(dispatcher.resolve_managed_shutdown());
  assert!(dispatcher.movement_inhibited());
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn managed_whole_robot_stop_preempts_blocked_release_and_fences_late_completion() {
  let path = directory();
  let (dispatcher, adapter, inner) = shared_managed_fixture(&path);
  let (entered_tx, entered_rx) = std::sync::mpsc::channel();
  let (release_tx, release_rx) = std::sync::mpsc::channel();
  *inner.pause.lock().unwrap() = Some((entered_tx, release_rx));
  let moving = {
    let dispatcher = dispatcher.clone();
    std::thread::spawn(move || {
      dispatcher.dispatch_managed_movement(
        managed_context(AuthorityRole::EgiaControl, AdmissionIngress::Grpc),
        move_a("old-epoch"),
      )
    })
  };
  entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
  let stop_result = dispatcher
    .dispatch_managed_stop_checked(
      AdmissionContext {
        run_id: "run-a".into(),
        active_revision: Some(REVISION_A.into()),
        authority_role: AuthorityRole::OperatorControl,
        ingress: AdmissionIngress::Grpc,
        binding_path: String::new(),
        primitive: "robot.stop".into(),
        primitive_version: "1.0.0".into(),
        delivery_mode: aether_dispatcher::MovementDelivery::OneShot,
      },
      stop(
        "new-stop",
        StopKind::RobotStop {
          robot_id: "r".into(),
        },
        false,
      ),
    )
    .result();
  assert_eq!(stop_result.status, OutcomeStatus::Completed);
  assert_eq!(adapter.physical_calls.load(Ordering::SeqCst), 1);
  assert!(!dispatcher.supports_motion_stop(BASE_A));
  release_tx.send(()).unwrap();
  assert_eq!(moving.join().unwrap().status, OutcomeStatus::OutcomeUnknown);
  assert!(dispatcher.movement_inhibited());
  #[cfg(feature = "test-hooks")]
  {
    dispatcher.set_journal_record_limit_for_test(64);
    let failed_intent = dispatcher
      .dispatch_managed_stop_checked(
        AdmissionContext {
          run_id: "run-a".into(),
          active_revision: Some(REVISION_A.into()),
          authority_role: AuthorityRole::OperatorControl,
          ingress: AdmissionIngress::Grpc,
          binding_path: String::new(),
          primitive: "robot.stop".into(),
          primitive_version: "1.0.0".into(),
          delivery_mode: aether_dispatcher::MovementDelivery::OneShot,
        },
        stop(
          "failed-intent-stop",
          StopKind::RobotStop {
            robot_id: "r".into(),
          },
          true,
        ),
      )
      .result();
    assert_ne!(failed_intent.status, OutcomeStatus::Completed);
    assert_eq!(adapter.physical_calls.load(Ordering::SeqCst), 2);
    assert!(dispatcher.movement_inhibited());
  }
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}

#[cfg(feature = "test-hooks")]
#[test]
fn managed_stop_reaches_physical_lane_while_releasing_journal_is_stalled() {
  let path = directory();
  let (dispatcher, adapter, _) = shared_managed_fixture(&path);
  let (entered_tx, entered_rx) = std::sync::mpsc::channel();
  let (release_tx, release_rx) = std::sync::mpsc::channel();
  dispatcher.pause_journal_release_for_test(entered_tx, release_rx);
  let moving = {
    let dispatcher = dispatcher.clone();
    std::thread::spawn(move || {
      dispatcher.dispatch_managed_movement(
        managed_context(AuthorityRole::EgiaControl, AdmissionIngress::Grpc),
        move_a("stalled-releasing"),
      )
    })
  };
  entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
  let stopping = {
    let dispatcher = dispatcher.clone();
    std::thread::spawn(move || {
      dispatcher
        .dispatch_managed_stop_checked(
          AdmissionContext {
            run_id: "run-a".into(),
            active_revision: Some(REVISION_A.into()),
            authority_role: AuthorityRole::OperatorControl,
            ingress: AdmissionIngress::Grpc,
            binding_path: String::new(),
            primitive: "robot.stop".into(),
            primitive_version: "1.0.0".into(),
            delivery_mode: aether_dispatcher::MovementDelivery::OneShot,
          },
          stop(
            "journal-stalled-stop",
            StopKind::RobotStop {
              robot_id: "r".into(),
            },
            false,
          ),
        )
        .result()
    })
  };
  let until = std::time::Instant::now() + Duration::from_secs(1);
  while adapter.physical_calls.load(Ordering::SeqCst) == 0 && std::time::Instant::now() < until {
    std::thread::sleep(Duration::from_millis(5));
  }
  let physical_before_journal_release = adapter.physical_calls.load(Ordering::SeqCst);
  release_tx.send(()).unwrap();
  assert_eq!(physical_before_journal_release, 1);
  assert_eq!(stopping.join().unwrap().status, OutcomeStatus::Completed);
  assert_eq!(moving.join().unwrap().status, OutcomeStatus::OutcomeUnknown);
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}

#[cfg(feature = "test-hooks")]
#[test]
fn managed_emergency_stop_reaches_physical_lane_while_journal_is_stalled() {
  let path = directory();
  let (dispatcher, adapter, _) = shared_managed_fixture(&path);
  let (entered_tx, entered_rx) = std::sync::mpsc::channel();
  let (release_tx, release_rx) = std::sync::mpsc::channel();
  dispatcher.pause_journal_release_for_test(entered_tx, release_rx);
  let moving = {
    let dispatcher = dispatcher.clone();
    std::thread::spawn(move || {
      dispatcher.dispatch_managed_movement(
        managed_context(AuthorityRole::EgiaControl, AdmissionIngress::Grpc),
        move_a("stalled-before-shutdown"),
      )
    })
  };
  entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
  let stopping = {
    let dispatcher = dispatcher.clone();
    std::thread::spawn(move || dispatcher.emergency_stop())
  };
  let until = std::time::Instant::now() + Duration::from_secs(1);
  while adapter.physical_calls.load(Ordering::SeqCst) == 0 && std::time::Instant::now() < until {
    std::thread::sleep(Duration::from_millis(5));
  }
  let physical_before_journal_release = adapter.physical_calls.load(Ordering::SeqCst);
  release_tx.send(()).unwrap();
  assert_eq!(physical_before_journal_release, 1);
  let stopped = stopping.join().unwrap();
  assert_eq!(stopped.len(), 1);
  assert!(stopped[0].1.is_ok());
  assert_eq!(moving.join().unwrap().status, OutcomeStatus::OutcomeUnknown);
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}

#[cfg(feature = "test-hooks")]
#[test]
fn browser_worker_stalled_at_journal_cannot_delay_managed_physical_stop() {
  let path = directory();
  let (dispatcher, adapter, inner) = shared_managed_fixture(&path);
  let (entered_tx, entered_rx) = std::sync::mpsc::channel();
  let (release_tx, release_rx) = std::sync::mpsc::channel();
  dispatcher.pause_journal_release_for_test(entered_tx, release_rx);
  let submitted = dispatcher
    .submit_browser_movement(
      managed_context(
        AuthorityRole::OperatorControl,
        AdmissionIngress::PrivateBridge,
      ),
      move_a("browser-journal-stalled"),
    )
    .unwrap();
  assert_eq!(submitted.status, OutcomeStatus::InProgress);
  entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
  let stopping = {
    let dispatcher = dispatcher.clone();
    std::thread::spawn(move || dispatcher.emergency_stop())
  };
  let until = std::time::Instant::now() + Duration::from_secs(1);
  while adapter.physical_calls.load(Ordering::SeqCst) == 0 && std::time::Instant::now() < until {
    std::thread::sleep(Duration::from_millis(5));
  }
  let physical_before_journal_release = adapter.physical_calls.load(Ordering::SeqCst);
  release_tx.send(()).unwrap();
  assert_eq!(physical_before_journal_release, 1);
  assert!(stopping
    .join()
    .unwrap()
    .iter()
    .all(|(_, result)| result.is_ok()));
  for _ in 0..1000 {
    if dispatcher
      .browser_result_authorized("browser-journal-stalled", "run-a")
      .is_some_and(|result| result.status == OutcomeStatus::OutcomeUnknown)
    {
      break;
    }
    std::thread::sleep(Duration::from_millis(1));
  }
  assert_eq!(
    dispatcher
      .browser_result_authorized("browser-journal-stalled", "run-a")
      .unwrap()
      .status,
    OutcomeStatus::OutcomeUnknown,
  );
  assert_eq!(inner.movement_calls(), 1);
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}

const BINDING_A: &str = "cores/eos/layers/dispatcher/groups/primitive_base/nodes/velocity_a";
const REVISION_A: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

pub(crate) fn managed_fixture(
  path: &PathBuf,
  a: Arc<ProofBase>,
  b: Arc<ProofBase>,
  clock: Arc<ManualClock>,
  run_id: &str,
) -> Dispatcher {
  managed_fixture_revision(path, a, b, clock, run_id, REVISION_A)
}

fn managed_fixture_revision(
  path: &PathBuf,
  a: Arc<ProofBase>,
  b: Arc<ProofBase>,
  clock: Arc<ManualClock>,
  run_id: &str,
  revision: &str,
) -> Dispatcher {
  Dispatcher::new_durable_managed(
    &profile(),
    vec![
      AdapterRegistration::Base {
        node_id: "a".into(),
        adapter: a,
      },
      AdapterRegistration::Base {
        node_id: "b".into(),
        adapter: b,
      },
    ],
    clock,
    path,
    AdmissionGeneration {
      run_id: run_id.into(),
      active_revision: revision.into(),
      bindings: vec![AdmittedBinding {
        binding_path: BINDING_A.into(),
        node_path: BASE_A.into(),
        primitive: "base.velocity".into(),
        primitive_version: "1.0.0".into(),
      }],
    },
  )
  .unwrap()
}

pub(crate) fn managed_context(role: AuthorityRole, ingress: AdmissionIngress) -> AdmissionContext {
  AdmissionContext {
    run_id: "run-a".into(),
    active_revision: Some(REVISION_A.into()),
    authority_role: role,
    ingress,
    binding_path: BINDING_A.into(),
    primitive: "base.velocity".into(),
    primitive_version: "1.0.0".into(),
    delivery_mode: aether_dispatcher::MovementDelivery::OneShot,
  }
}

#[test]
fn stream_lease_publication_releases_browser_slots_for_refresh() {
  let path = directory();
  let base = Arc::new(ProofBase::new());
  let dispatcher = Arc::new(managed_fixture(
    &path,
    base.clone(),
    Arc::new(ProofBase::new()),
    Arc::new(ManualClock::new(0, None)),
    "run-a",
  ));
  let mut context = managed_context(
    AuthorityRole::OperatorControl,
    AdmissionIngress::PrivateBridge,
  );
  context.delivery_mode = aether_dispatcher::MovementDelivery::Stream;
  for index in 0..16 {
    let id = format!("stream-{index}");
    let mut request = move_a(&id);
    if index == 15 {
      request.movement = Movement::Base(BaseMovement::BodyVelocity {
        x_m_s: 0.0,
        y_m_s: 0.0,
        yaw_rad_s: 0.0,
        valid_for_ms: 50,
      });
    }
    assert_eq!(
      dispatcher
        .submit_browser_movement(context.clone(), request)
        .unwrap()
        .status,
      OutcomeStatus::InProgress
    );
    let deadline = std::time::Instant::now() + Duration::from_secs(2);
    loop {
      let result = dispatcher.browser_result_authorized(&id, "run-a").unwrap();
      if result.status == OutcomeStatus::Completed {
        assert_eq!(result.reason.as_deref(), Some("stream lease published"));
        break;
      }
      assert!(
        std::time::Instant::now() < deadline,
        "stream publication timed out"
      );
      std::thread::sleep(Duration::from_millis(2));
    }
  }
  assert_eq!(base.movement_calls(), 16);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn browser_result_requires_current_run_operator_and_private_movement_ingress() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let clock = Arc::new(ManualClock::new(0, None));
  let dispatcher = managed_fixture(&path, a.clone(), b.clone(), clock.clone(), "run-a");
  let private = managed_context(
    AuthorityRole::OperatorControl,
    AdmissionIngress::PrivateBridge,
  );
  let grpc = managed_context(AuthorityRole::EgiaControl, AdmissionIngress::Grpc);
  assert_eq!(
    dispatcher
      .dispatch_managed_movement(private, move_a("browser-command"))
      .status,
    OutcomeStatus::Completed
  );
  assert_eq!(
    dispatcher
      .dispatch_managed_movement(grpc, move_a("grpc-command"))
      .status,
    OutcomeStatus::Completed
  );
  assert_eq!(
    dispatcher
      .browser_result_authorized("browser-command", "run-a")
      .unwrap()
      .status,
    OutcomeStatus::Completed
  );
  for id in ["grpc-command", "missing", "browser-command-other"] {
    assert!(dispatcher.browser_result_authorized(id, "run-a").is_none());
  }
  let stopped = dispatcher.dispatch_managed_stop_checked(
    stop_context(AuthorityRole::EgiaControl, "safety.estop", None),
    stop(
      "stop-command",
      StopKind::SafetyEstop {
        robot_id: "r".into(),
      },
      false,
    ),
  );
  assert_eq!(stopped.result().status, OutcomeStatus::Completed);
  assert!(dispatcher
    .browser_result_authorized("stop-command", "run-a")
    .is_none());
  assert!(dispatcher
    .browser_result_authorized("browser-command", "run-b")
    .is_none());
  assert_eq!(a.movement_calls(), 2);
  drop(dispatcher);
  let revised = managed_fixture_revision(
    &path,
    a.clone(),
    b.clone(),
    clock.clone(),
    "run-a",
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
  );
  assert!(revised
    .browser_result_authorized("browser-command", "run-a")
    .is_none());
  drop(revised);
  let later = managed_fixture(&path, a.clone(), b, clock, "run-b");
  assert!(later
    .browser_result_authorized("browser-command", "run-b")
    .is_none());
  assert_eq!(a.movement_calls(), 2);
  drop(later);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn browser_result_reads_in_progress_then_terminal_without_replaying_adapter() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let clock = Arc::new(ManualClock::new(0, None));
  let dispatcher = Arc::new(managed_fixture(&path, a.clone(), b, clock, "run-a"));
  let (entered_tx, entered_rx) = std::sync::mpsc::channel();
  let (release_tx, release_rx) = std::sync::mpsc::channel();
  *a.pause.lock().unwrap() = Some((entered_tx, release_rx));
  let worker = {
    let dispatcher = dispatcher.clone();
    std::thread::spawn(move || {
      dispatcher.dispatch_managed_movement(
        managed_context(
          AuthorityRole::OperatorControl,
          AdmissionIngress::PrivateBridge,
        ),
        move_a("moving-now"),
      )
    })
  };
  entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
  assert_eq!(
    dispatcher
      .browser_result_authorized("moving-now", "run-a")
      .unwrap()
      .status,
    OutcomeStatus::InProgress
  );
  release_tx.send(()).unwrap();
  assert_eq!(worker.join().unwrap().status, OutcomeStatus::Completed);
  assert_eq!(
    dispatcher
      .browser_result_authorized("moving-now", "run-a")
      .unwrap()
      .status,
    OutcomeStatus::Completed
  );
  assert_eq!(a.movement_calls(), 1);
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn browser_async_ack_is_durable_and_duplicate_cannot_release_again() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let dispatcher = Arc::new(managed_fixture(
    &path,
    a.clone(),
    b,
    Arc::new(ManualClock::new(0, None)),
    "run-a",
  ));
  let (entered_tx, entered_rx) = std::sync::mpsc::channel();
  let (release_tx, release_rx) = std::sync::mpsc::channel();
  *a.pause.lock().unwrap() = Some((entered_tx, release_rx));
  let context = managed_context(
    AuthorityRole::OperatorControl,
    AdmissionIngress::PrivateBridge,
  );
  let request = move_a("browser-async");
  let first = dispatcher
    .submit_browser_movement(context.clone(), request.clone())
    .unwrap();
  assert_eq!(first.status, OutcomeStatus::InProgress);
  entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
  assert_eq!(
    dispatcher
      .submit_browser_movement(context.clone(), request.clone())
      .unwrap()
      .status,
    OutcomeStatus::InProgress,
  );
  let mut changed = request.clone();
  // Change the typed movement to prove the full durable signature check.
  changed.movement = Movement::Base(BaseMovement::BodyVelocity {
    x_m_s: 0.2,
    y_m_s: 0.0,
    yaw_rad_s: 0.0,
    valid_for_ms: 100,
  });
  assert_eq!(
    dispatcher
      .submit_browser_movement(context.clone(), changed)
      .unwrap()
      .status,
    OutcomeStatus::Rejected
  );
  let mut changed_deadline = request.clone();
  changed_deadline.meta.deadline_monotonic_ns += 1_000_000_000;
  assert_eq!(
    dispatcher
      .submit_browser_movement(context, changed_deadline)
      .unwrap()
      .status,
    OutcomeStatus::Rejected
  );
  release_tx.send(()).unwrap();
  for _ in 0..1000 {
    if dispatcher
      .browser_result_authorized("browser-async", "run-a")
      .unwrap()
      .status
      == OutcomeStatus::Completed
    {
      break;
    }
    std::thread::sleep(Duration::from_millis(1));
  }
  assert_eq!(a.movement_calls(), 1);
  assert_eq!(
    dispatcher
      .browser_result_authorized("browser-async", "run-a")
      .unwrap()
      .status,
    OutcomeStatus::Completed
  );
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn browser_async_invalid_static_payload_never_reserves_or_releases() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let dispatcher = Arc::new(managed_fixture(
    &path,
    a.clone(),
    b,
    Arc::new(ManualClock::new(0, None)),
    "run-a",
  ));
  let context = managed_context(
    AuthorityRole::OperatorControl,
    AdmissionIngress::PrivateBridge,
  );
  let mut excessive = move_a("browser-static-limit");
  excessive.movement = Movement::Base(BaseMovement::BodyVelocity {
    x_m_s: 1_000_000.0,
    y_m_s: 0.0,
    yaw_rad_s: 0.0,
    valid_for_ms: 100,
  });
  assert!(dispatcher
    .submit_browser_movement(context.clone(), excessive)
    .is_err());
  assert!(dispatcher
    .browser_result_authorized("browser-static-limit", "run-a")
    .is_none());
  assert!(dispatcher
    .gateway_operation_authorized(
      "browser-static-limit",
      "run-a",
      AuthorityRole::OperatorControl
    )
    .unwrap()
    .is_none());
  let mut short_deadline = move_a("browser-short-lease");
  short_deadline.meta.deadline_monotonic_ns = 50_000_000;
  assert!(dispatcher
    .submit_browser_movement(context.clone(), short_deadline)
    .is_err());
  assert!(dispatcher
    .browser_result_authorized("browser-short-lease", "run-a")
    .is_none());
  let mut expired = move_a("browser-expired-before-prepare");
  expired.meta.deadline_monotonic_ns = 0;
  assert!(dispatcher
    .submit_browser_movement(context, expired)
    .is_err());
  assert!(dispatcher
    .browser_result_authorized("browser-expired-before-prepare", "run-a")
    .is_none());
  assert_eq!(a.movement_calls(), 0);
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn browser_async_lane_rejects_ninth_atomically_and_stop_cancels_prepared() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let dispatcher = Arc::new(managed_fixture(
    &path,
    a.clone(),
    b,
    Arc::new(ManualClock::new(0, None)),
    "run-a",
  ));
  let (entered_tx, entered_rx) = std::sync::mpsc::channel();
  let (release_tx, release_rx) = std::sync::mpsc::channel();
  *a.pause.lock().unwrap() = Some((entered_tx, release_rx));
  let context = managed_context(
    AuthorityRole::OperatorControl,
    AdmissionIngress::PrivateBridge,
  );
  assert_eq!(
    dispatcher
      .submit_browser_movement(context.clone(), move_a("async-0"))
      .unwrap()
      .status,
    OutcomeStatus::InProgress
  );
  entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
  for index in 1..8 {
    let id = format!("async-{index}");
    assert_eq!(
      dispatcher
        .submit_browser_movement(context.clone(), move_a(&id))
        .unwrap()
        .status,
      OutcomeStatus::InProgress
    );
  }
  let ninth = dispatcher
    .submit_browser_movement(context.clone(), move_a("async-8"))
    .unwrap();
  assert_eq!(ninth.status, OutcomeStatus::Rejected);
  assert_eq!(
    dispatcher
      .submit_browser_movement(context.clone(), move_a("async-8"))
      .unwrap()
      .status,
    OutcomeStatus::Rejected
  );
  dispatcher.quiesce();
  dispatcher.cancel_prepared_browser_after_stop();
  for index in 1..8 {
    let id = format!("async-{index}");
    assert_eq!(
      dispatcher
        .browser_result_authorized(&id, "run-a")
        .unwrap()
        .status,
      OutcomeStatus::Cancelled
    );
  }
  release_tx.send(()).unwrap();
  for _ in 0..1000 {
    if (0..8).all(|index| {
      dispatcher
        .browser_result_authorized(&format!("async-{index}"), "run-a")
        .is_some_and(|result| result.status != OutcomeStatus::InProgress)
    }) {
      break;
    }
    std::thread::sleep(Duration::from_millis(1));
  }
  assert_eq!(a.movement_calls(), 1);
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn browser_async_rechecks_deadline_after_waiting_for_release_gate() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let clock = Arc::new(ManualClock::new(0, None));
  let dispatcher = Arc::new(managed_fixture(&path, a.clone(), b, clock.clone(), "run-a"));
  let (entered_tx, entered_rx) = std::sync::mpsc::channel();
  let (release_tx, release_rx) = std::sync::mpsc::channel();
  a.pause_movement(entered_tx, release_rx);
  let context = managed_context(
    AuthorityRole::OperatorControl,
    AdmissionIngress::PrivateBridge,
  );
  assert_eq!(
    dispatcher
      .submit_browser_movement(context.clone(), move_a("browser-hold"))
      .unwrap()
      .status,
    OutcomeStatus::InProgress
  );
  entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
  let mut queued = move_a("browser-expired");
  queued.meta.deadline_monotonic_ns = 300_000_000;
  assert_eq!(
    dispatcher
      .submit_browser_movement(context, queued)
      .unwrap()
      .status,
    OutcomeStatus::InProgress
  );
  clock.set_monotonic_ns(400_000_000);
  release_tx.send(()).unwrap();
  for _ in 0..1000 {
    if dispatcher
      .browser_result_authorized("browser-expired", "run-a")
      .unwrap()
      .status
      == OutcomeStatus::Rejected
    {
      break;
    }
    std::thread::sleep(Duration::from_millis(1));
  }
  assert_eq!(a.movement_calls(), 1);
  assert_eq!(
    dispatcher
      .browser_result_authorized("browser-expired", "run-a")
      .unwrap()
      .status,
    OutcomeStatus::Rejected
  );
  for _ in 0..1000 {
    if Arc::strong_count(&dispatcher) == 1 {
      break;
    }
    std::thread::sleep(Duration::from_millis(1));
  }
  assert_eq!(Arc::strong_count(&dispatcher), 1);
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn managed_movement_identity_and_owner_survive_restart_without_replay() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let clock = Arc::new(ManualClock::new(0, None));
  let dispatcher = managed_fixture(&path, a.clone(), b.clone(), clock.clone(), "run-a");
  let ctx = managed_context(AuthorityRole::EgiaControl, AdmissionIngress::Grpc);
  let request = move_a("managed-one");
  assert_eq!(
    dispatcher.dispatch(request.clone()).status,
    OutcomeStatus::Rejected
  );
  assert_eq!(
    dispatcher
      .dispatch_gateway_movement(request.clone(), "base.velocity")
      .status,
    OutcomeStatus::Rejected
  );
  assert!(dispatcher.gateway_operation("managed-one").is_err());
  assert_eq!(
    dispatcher
      .dispatch_managed_movement(ctx.clone(), request.clone())
      .status,
    OutcomeStatus::Completed
  );
  assert_eq!(a.movement_calls(), 1);
  let mut retry = request.clone();
  retry.meta.deadline_monotonic_ns += 1_000_000_000;
  assert_eq!(
    dispatcher
      .dispatch_managed_movement(ctx.clone(), retry)
      .status,
    OutcomeStatus::Completed
  );
  assert_eq!(a.movement_calls(), 1);
  assert_eq!(
    dispatcher.gateway_operation_authorized("managed-one", "run-a", AuthorityRole::OperatorControl),
    Err(GatewayLookupError::Unauthorized)
  );
  assert!(dispatcher
    .gateway_operation_authorized("managed-one", "run-a", AuthorityRole::EgiaControl)
    .unwrap()
    .is_some());
  assert_eq!(
    dispatcher.cancel_authorized("managed-one", "run-a", AuthorityRole::OperatorControl),
    Err(GatewayLookupError::Unauthorized)
  );
  assert_eq!(
    dispatcher
      .cancel_authorized("managed-one", "run-a", AuthorityRole::EgiaControl)
      .unwrap()
      .status,
    OutcomeStatus::Completed
  );
  let mut changed = ctx.clone();
  changed.authority_role = AuthorityRole::OperatorControl;
  changed.ingress = AdmissionIngress::PrivateBridge;
  assert_eq!(
    dispatcher
      .dispatch_managed_movement(changed, request.clone())
      .status,
    OutcomeStatus::Rejected
  );
  assert_eq!(a.movement_calls(), 1);
  drop(dispatcher);
  let restarted = managed_fixture(&path, a.clone(), b.clone(), clock.clone(), "run-a");
  assert_eq!(
    restarted
      .dispatch_managed_movement(ctx.clone(), request.clone())
      .status,
    OutcomeStatus::Completed
  );
  assert_eq!(a.movement_calls(), 1);
  assert_eq!(
    restarted.gateway_operation_authorized("managed-one", "run-b", AuthorityRole::EgiaControl),
    Err(GatewayLookupError::WrongRun)
  );
  drop(restarted);
  let new_run = managed_fixture(&path, a.clone(), b, clock, "run-b");
  let mut new_ctx = ctx;
  new_ctx.run_id = "run-b".into();
  assert_eq!(
    new_run.dispatch_managed_movement(new_ctx, request).status,
    OutcomeStatus::Rejected
  );
  assert_eq!(a.movement_calls(), 1);
  drop(new_run);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn managed_binding_role_freshness_and_limits_fail_before_release() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let clock = Arc::new(ManualClock::new(0, None));
  let dispatcher = managed_fixture(&path, a.clone(), b, clock.clone(), "run-a");
  let ctx = managed_context(AuthorityRole::EgiaControl, AdmissionIngress::Grpc);
  for (id, invalid) in [
    (
      "role",
      managed_context(AuthorityRole::OperatorControl, AdmissionIngress::Grpc),
    ),
    (
      "revision",
      AdmissionContext {
        active_revision: Some(
          "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb".into(),
        ),
        ..ctx.clone()
      },
    ),
    (
      "binding",
      AdmissionContext {
        binding_path: "cores/eos/layers/dispatcher/groups/primitive_base/nodes/other".into(),
        ..ctx.clone()
      },
    ),
  ] {
    assert_eq!(
      dispatcher
        .dispatch_managed_movement(invalid.clone(), move_a(id))
        .status,
      OutcomeStatus::Rejected
    );
  }
  let mut wrong_target = move_a("target");
  wrong_target.node_path = "cores/eos/layers/hardware/groups/base/nodes/b".into();
  assert_eq!(
    dispatcher
      .dispatch_managed_movement(ctx.clone(), wrong_target)
      .status,
    OutcomeStatus::Rejected
  );
  let mut out_of_range = move_a("range");
  out_of_range.movement = Movement::Base(BaseMovement::BodyVelocity {
    x_m_s: 2.0,
    y_m_s: 0.0,
    yaw_rad_s: 0.0,
    valid_for_ms: 100,
  });
  assert_eq!(
    dispatcher
      .dispatch_managed_movement(ctx.clone(), out_of_range)
      .status,
    OutcomeStatus::Rejected
  );
  clock.set_monotonic_ns(1_500_000_000);
  assert_eq!(
    dispatcher
      .dispatch_managed_movement(ctx.clone(), move_a("stale"))
      .status,
    OutcomeStatus::Rejected
  );
  a.health_ns.store(1_500_000_000, Ordering::SeqCst);
  a.observation_ns.store(1_500_000_000, Ordering::SeqCst);
  let mut fresh = move_a("fresh");
  fresh.meta.deadline_monotonic_ns = 3_000_000_000;
  assert_eq!(
    dispatcher.dispatch_managed_movement(ctx, fresh).status,
    OutcomeStatus::Completed
  );
  assert_eq!(a.movement_calls(), 1);
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}

fn stop_context(role: AuthorityRole, primitive: &str, revision: Option<&str>) -> AdmissionContext {
  AdmissionContext {
    run_id: "run-a".into(),
    active_revision: revision.map(str::to_string),
    authority_role: role,
    ingress: AdmissionIngress::Grpc,
    binding_path: String::new(),
    primitive: primitive.into(),
    primitive_version: "1.0.0".into(),
    delivery_mode: aether_dispatcher::MovementDelivery::OneShot,
  }
}

#[test]
fn managed_stop_recovery_is_operator_only_and_estop_ignores_stale_revision() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let clock = Arc::new(ManualClock::new(0, None));
  a.movement_outcome(CommandOutcome::Ambiguous);
  let dispatcher = managed_fixture(&path, a.clone(), b.clone(), clock, "run-a");
  let movement = managed_context(AuthorityRole::EgiaControl, AdmissionIngress::Grpc);
  assert_eq!(
    dispatcher
      .dispatch_managed_movement(movement, move_a("uncertain"))
      .status,
    OutcomeStatus::OutcomeUnknown
  );
  assert!(dispatcher.movement_inhibited());
  let robot = StopKind::RobotStop {
    robot_id: "r".into(),
  };
  let egia = stop_context(AuthorityRole::EgiaControl, "robot.stop", Some(REVISION_A));
  assert_eq!(
    dispatcher
      .dispatch_managed_stop_checked(egia, stop("egia-recovery", robot.clone(), true))
      .result()
      .status,
    OutcomeStatus::Rejected
  );
  let operator = stop_context(
    AuthorityRole::OperatorControl,
    "robot.stop",
    Some(REVISION_A),
  );
  assert_eq!(
    dispatcher
      .dispatch_managed_stop_checked(operator.clone(), stop("unmarked", robot.clone(), false))
      .result()
      .status,
    OutcomeStatus::Rejected
  );
  assert_eq!(
    dispatcher
      .dispatch_managed_stop_checked(operator.clone(), stop("marked", robot.clone(), true))
      .result()
      .status,
    OutcomeStatus::Completed
  );
  assert!(!dispatcher.movement_inhibited());
  let before = a.calls() + b.calls();
  assert_eq!(
    dispatcher
      .dispatch_managed_stop_checked(operator, stop("marked", robot, false))
      .result()
      .status,
    OutcomeStatus::Rejected
  );
  assert_eq!(a.calls() + b.calls(), before);
  let estop = stop_context(AuthorityRole::EgiaControl, "safety.estop", None);
  assert_eq!(
    dispatcher
      .dispatch_managed_stop_checked(
        estop,
        stop(
          "safety",
          StopKind::SafetyEstop {
            robot_id: "r".into()
          },
          false
        )
      )
      .result()
      .status,
    OutcomeStatus::Completed
  );
  assert!(dispatcher.emergency_stopped());
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn managed_generation_seals_historical_operation_identity_and_raw_stop() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let legacy = fixture(&path, a.clone(), b.clone());
  assert_eq!(
    legacy
      .dispatch_gateway_movement(move_a("historical"), "base.velocity")
      .status,
    OutcomeStatus::Completed
  );
  drop(legacy);
  let managed = managed_fixture(
    &path,
    a.clone(),
    b,
    Arc::new(ManualClock::new(0, None)),
    "run-a",
  );
  assert_eq!(
    managed.gateway_operation_authorized("historical", "run-a", AuthorityRole::EgiaControl),
    Err(GatewayLookupError::LegacyRecord)
  );
  assert_eq!(
    managed
      .dispatch_managed_movement(
        managed_context(AuthorityRole::EgiaControl, AdmissionIngress::Grpc),
        move_a("historical")
      )
      .status,
    OutcomeStatus::Rejected
  );
  assert!(managed.result("historical").is_none());
  assert!(managed.stop_node(BASE_A).is_err());
  assert_eq!(
    managed
      .dispatch_gateway_stop(stop(
        "raw-stop",
        StopKind::SafetyEstop {
          robot_id: "r".into()
        },
        false
      ))
      .status,
    OutcomeStatus::Rejected
  );
  assert_eq!(a.calls(), 0);
  drop(managed);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn old_revision_submit_cannot_replay_success_after_new_generation() {
  const REVISION_B: &str = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let clock = Arc::new(ManualClock::new(0, None));
  let old = managed_fixture(&path, a.clone(), b.clone(), clock.clone(), "run-a");
  let ctx = managed_context(AuthorityRole::EgiaControl, AdmissionIngress::Grpc);
  assert_eq!(
    old
      .dispatch_managed_movement(ctx.clone(), move_a("old-success"))
      .status,
    OutcomeStatus::Completed
  );
  drop(old);
  let current = managed_fixture_revision(&path, a.clone(), b, clock, "run-a", REVISION_B);
  assert_eq!(
    current
      .dispatch_managed_movement(ctx, move_a("old-success"))
      .status,
    OutcomeStatus::Rejected
  );
  assert_eq!(a.movement_calls(), 1);
  assert_eq!(
    current
      .gateway_operation_authorized("old-success", "run-a", AuthorityRole::EgiaControl)
      .unwrap()
      .unwrap()
      .status,
    OutcomeStatus::Completed
  );
  drop(current);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn managed_queued_movement_rechecks_fake_clock_before_adapter_release() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let clock = Arc::new(ManualClock::new(0, None));
  let dispatcher = Arc::new(managed_fixture(&path, a.clone(), b, clock.clone(), "run-a"));
  let (entered_tx, entered_rx) = std::sync::mpsc::channel();
  let (release_tx, release_rx) = std::sync::mpsc::channel();
  *a.pause.lock().unwrap() = Some((entered_tx, release_rx));
  let first = {
    let dispatcher = dispatcher.clone();
    std::thread::spawn(move || {
      dispatcher.dispatch_managed_movement(
        managed_context(AuthorityRole::EgiaControl, AdmissionIngress::Grpc),
        move_a("prior"),
      )
    })
  };
  entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
  let queued = {
    let dispatcher = dispatcher.clone();
    std::thread::spawn(move || {
      let mut request = move_a("queued");
      request.meta.deadline_monotonic_ns = 300_000_000;
      dispatcher.dispatch_managed_movement(
        managed_context(AuthorityRole::EgiaControl, AdmissionIngress::Grpc),
        request,
      )
    })
  };
  std::thread::sleep(Duration::from_millis(10));
  clock.set_monotonic_ns(400_000_000);
  release_tx.send(()).unwrap();
  assert_eq!(first.join().unwrap().status, OutcomeStatus::Completed);
  assert_eq!(queued.join().unwrap().status, OutcomeStatus::Rejected);
  assert_eq!(a.movement_calls(), 1);
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn skill_revocation_before_durable_release_never_calls_adapter() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let clock = Arc::new(ManualClock::new(0, None));
  let dispatcher = managed_fixture(&path, a.clone(), b, clock, "run-a");
  let revoked = AtomicBool::new(true);
  let result = dispatcher.dispatch_skill_movement(
    managed_context(AuthorityRole::EgiaControl, AdmissionIngress::SkillWorker),
    move_a("skill-revoked-before-release"),
    &revoked,
  );
  assert_eq!(result.status, OutcomeStatus::Rejected);
  assert_eq!(a.movement_calls(), 0);
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn skill_cancel_after_durable_release_is_bounded_and_latches_unknown() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let clock = Arc::new(ManualClock::new(0, None));
  let dispatcher = Arc::new(managed_fixture(&path, a.clone(), b, clock, "run-a"));
  let revoked = Arc::new(AtomicBool::new(false));
  let (entered_tx, entered_rx) = std::sync::mpsc::channel();
  let (release_tx, release_rx) = std::sync::mpsc::channel();
  *a.pause.lock().unwrap() = Some((entered_tx, release_rx));
  let moving = {
    let dispatcher = dispatcher.clone();
    let revoked = revoked.clone();
    std::thread::spawn(move || {
      dispatcher.dispatch_skill_movement(
        managed_context(AuthorityRole::EgiaControl, AdmissionIngress::SkillWorker),
        move_a("skill-releasing"),
        &revoked,
      )
    })
  };
  entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
  revoked.store(true, Ordering::Release);
  let (cancel_tx, cancel_rx) = std::sync::mpsc::channel();
  let cancelling = {
    let dispatcher = dispatcher.clone();
    std::thread::spawn(move || {
      let result =
        dispatcher.cancel_authorized("skill-releasing", "run-a", AuthorityRole::EgiaControl);
      cancel_tx.send(result).unwrap();
    })
  };
  let cancelled = cancel_rx
    .recv_timeout(Duration::from_secs(1))
    .expect("cancel cannot wait for blocked adapter")
    .unwrap();
  assert_eq!(cancelled.status, OutcomeStatus::OutcomeUnknown);
  assert!(dispatcher.movement_inhibited());
  release_tx.send(()).unwrap();
  assert_eq!(moving.join().unwrap().status, OutcomeStatus::OutcomeUnknown);
  cancelling.join().unwrap();
  assert_eq!(a.movement_calls(), 1);
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}

pub(crate) fn stop(id: &str, kind: StopKind, authorized_recovery: bool) -> StopRequest {
  StopRequest {
    command_id: id.into(),
    deadline_monotonic_ns: 2_000_000_000,
    kind,
    authorized_recovery,
  }
}

pub(crate) fn move_a(id: &str) -> CommandRequest {
  CommandRequest {
    node_path: BASE_A.into(),
    meta: CommandMeta {
      command_id: id.into(),
      deadline_monotonic_ns: 2_000_000_000,
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
fn robot_stop_known_partial_failure_is_durable_and_new_authorized_id_recovers() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  b.outcome(ControlledStopOutcome::ConfirmedFailure(
    "known\nfailed".into(),
  ));
  let dispatcher = fixture(&path, a.clone(), b.clone());
  assert!(dispatcher.supports_non_latching_robot_stop());
  let first = stop(
    "robot-first",
    StopKind::RobotStop {
      robot_id: "r".into(),
    },
    false,
  );
  assert_eq!(
    dispatcher.dispatch_gateway_stop(first.clone()).status,
    OutcomeStatus::Failed
  );
  assert!(dispatcher.movement_inhibited());
  let info = dispatcher
    .gateway_operation("robot-first")
    .unwrap()
    .unwrap();
  assert_eq!(info.kind, GatewayOperationKind::RobotStop);
  assert_eq!(
    info.target,
    GatewayTarget::Robot {
      robot_id: "r".into()
    }
  );
  assert_eq!(info.parts.len(), 2);
  assert_eq!(info.parts[0].status, OutcomeStatus::Completed);
  assert_eq!(info.parts[1].status, OutcomeStatus::Failed);
  assert_eq!(info.parts[1].reason.as_deref(), Some("known failed"));
  assert_eq!(
    dispatcher.dispatch_gateway_stop(first.clone()).status,
    OutcomeStatus::Failed
  );
  assert_eq!((a.controlled_calls(), b.controlled_calls()), (1, 1));
  drop(dispatcher);
  let recovered = fixture(&path, a.clone(), b.clone());
  assert!(recovered.movement_inhibited());
  assert_eq!(
    recovered.dispatch_gateway_stop(first).status,
    OutcomeStatus::Failed
  );
  let denied = stop(
    "unauthorized",
    StopKind::RobotStop {
      robot_id: "r".into(),
    },
    false,
  );
  assert!(matches!(
    recovered.dispatch_gateway_stop_checked(denied.clone()),
    StopDispatchOutcome::Operation(result) if result.status == OutcomeStatus::Rejected
  ));
  assert!(matches!(
    recovered.dispatch_gateway_stop_checked(denied),
    StopDispatchOutcome::Operation(result) if result.status == OutcomeStatus::Rejected
  ));
  let unauthorized = recovered
    .gateway_operation("unauthorized")
    .unwrap()
    .unwrap();
  assert_eq!(unauthorized.status, OutcomeStatus::Rejected);
  assert_eq!(unauthorized.accepted, Some(false));
  assert_eq!(
    unauthorized.rejection_code,
    Some(GatewayRejectionCode::Unauthorized)
  );
  assert_eq!((a.controlled_calls(), b.controlled_calls()), (1, 1));
  drop(recovered);
  let recovered = fixture(&path, a.clone(), b.clone());
  assert_eq!(
    recovered
      .gateway_operation("unauthorized")
      .unwrap()
      .unwrap()
      .rejection_code,
    Some(GatewayRejectionCode::Unauthorized)
  );
  b.outcome(ControlledStopOutcome::Stopped);
  assert_eq!(
    recovered
      .dispatch_gateway_stop(stop(
        "robot-recovery",
        StopKind::RobotStop {
          robot_id: "r".into()
        },
        true
      ))
      .status,
    OutcomeStatus::Completed
  );
  assert!(!recovered.movement_inhibited());
  assert_eq!(a.emergency_calls.load(Ordering::SeqCst), 0);
  assert_eq!(b.emergency_calls.load(Ordering::SeqCst), 0);
  drop(recovered);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn motion_stop_is_single_target_and_estop_latch_survives_restart() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let dispatcher = fixture(&path, a.clone(), b.clone());
  let motion = stop(
    "motion",
    StopKind::MotionStop {
      node_path: BASE_A.into(),
    },
    false,
  );
  assert_eq!(
    dispatcher.dispatch_gateway_stop(motion.clone()).status,
    OutcomeStatus::Completed
  );
  assert_eq!(
    (
      a.emergency_calls.load(Ordering::SeqCst),
      b.emergency_calls.load(Ordering::SeqCst)
    ),
    (0, 0)
  );
  assert_eq!(a.controlled_calls(), 1);
  assert_eq!(
    dispatcher.dispatch_gateway_stop(motion).status,
    OutcomeStatus::Completed
  );
  assert_eq!(a.controlled_calls(), 1);
  let estop = stop(
    "estop",
    StopKind::SafetyEstop {
      robot_id: "r".into(),
    },
    false,
  );
  assert_eq!(
    dispatcher.dispatch_gateway_stop(estop.clone()).status,
    OutcomeStatus::Completed
  );
  assert_eq!(b.emergency_calls.load(Ordering::SeqCst), 1);
  drop(dispatcher);
  let recovered = fixture(&path, a.clone(), b.clone());
  assert_eq!(
    recovered.dispatch_gateway_stop(estop).status,
    OutcomeStatus::Completed
  );
  assert_eq!(b.emergency_calls.load(Ordering::SeqCst), 1);
  assert_eq!(
    recovered.dispatch(move_a("after-estop")).status,
    OutcomeStatus::Rejected
  );
  drop(recovered);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn robot_stop_waits_for_a_paused_prior_movement_before_fanout() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let (entered_tx, entered_rx) = std::sync::mpsc::channel();
  let (release_tx, release_rx) = std::sync::mpsc::channel();
  *a.pause.lock().unwrap() = Some((entered_tx, release_rx));
  let dispatcher = Arc::new(fixture(&path, a.clone(), b.clone()));
  let moving = {
    let dispatcher = dispatcher.clone();
    std::thread::spawn(move || dispatcher.dispatch(move_a("prior")))
  };
  entered_rx.recv_timeout(Duration::from_secs(5)).unwrap();
  let stopping = {
    let dispatcher = dispatcher.clone();
    std::thread::spawn(move || {
      dispatcher.dispatch_gateway_stop(stop(
        "ordered",
        StopKind::RobotStop {
          robot_id: "r".into(),
        },
        false,
      ))
    })
  };
  for _ in 0..5000 {
    if dispatcher.movement_inhibited() {
      break;
    }
    std::thread::sleep(Duration::from_millis(1));
  }
  assert!(dispatcher.movement_inhibited());
  assert_eq!((a.controlled_calls(), b.controlled_calls()), (0, 0));
  release_tx.send(()).unwrap();
  assert_eq!(moving.join().unwrap().status, OutcomeStatus::Completed);
  assert_eq!(stopping.join().unwrap().status, OutcomeStatus::Completed);
  assert_eq!((a.controlled_calls(), b.controlled_calls()), (1, 1));
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn panicked_worker_after_first_confirmed_part_never_replays_old_id() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  b.panic_on_controlled.store(true, Ordering::SeqCst);
  let dispatcher = fixture(&path, a.clone(), b.clone());
  let request = stop(
    "partial-crash",
    StopKind::RobotStop {
      robot_id: "r".into(),
    },
    false,
  );
  assert_eq!(
    dispatcher.dispatch_gateway_stop(request.clone()).status,
    OutcomeStatus::OutcomeUnknown
  );
  assert_eq!((a.controlled_calls(), b.controlled_calls()), (1, 1));
  drop(dispatcher);
  b.panic_on_controlled.store(false, Ordering::SeqCst);
  let recovered = fixture(&path, a.clone(), b.clone());
  assert!(recovered.movement_inhibited());
  assert_eq!(
    recovered.dispatch_gateway_stop(request).status,
    OutcomeStatus::OutcomeUnknown
  );
  let parts = recovered
    .gateway_operation("partial-crash")
    .unwrap()
    .unwrap()
    .parts;
  assert_eq!(parts[0].status, OutcomeStatus::Completed);
  assert_eq!(parts[1].status, OutcomeStatus::OutcomeUnknown);
  assert_eq!((a.controlled_calls(), b.controlled_calls()), (1, 1));
  assert_eq!(
    recovered
      .dispatch_gateway_stop(stop(
        "new-recovery",
        StopKind::RobotStop {
          robot_id: "r".into()
        },
        true
      ))
      .status,
    OutcomeStatus::Completed
  );
  assert!(!recovered.movement_inhibited());
  drop(recovered);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn restart_recovery_snapshots_union_of_old_and_current_stop_targets() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let c = Arc::new(ProofBase::new());
  b.outcome(ControlledStopOutcome::OutcomeUnknown);
  let original = try_fixture_nodes(&path, &[("a", a.clone()), ("b", b.clone())]).unwrap();
  assert_eq!(
    original
      .dispatch_gateway_stop(stop(
        "old-stop",
        StopKind::RobotStop {
          robot_id: "r".into()
        },
        false,
      ))
      .status,
    OutcomeStatus::OutcomeUnknown,
  );
  let old_parts = original
    .gateway_operation("old-stop")
    .unwrap()
    .unwrap()
    .parts;
  assert_eq!(old_parts[0].status, OutcomeStatus::Completed);
  assert_eq!(old_parts[1].status, OutcomeStatus::OutcomeUnknown);
  assert!(original.movement_inhibited());
  assert_eq!(
    (
      a.controlled_calls(),
      b.controlled_calls(),
      c.controlled_calls()
    ),
    (1, 1, 0)
  );
  drop(original);

  let missing_old = try_fixture_nodes(&path, &[("a", a.clone()), ("c", c.clone())]).unwrap();
  assert!(missing_old.movement_inhibited());
  assert_eq!(
    missing_old
      .dispatch_gateway_stop(stop(
        "old-stop",
        StopKind::RobotStop {
          robot_id: "r".into()
        },
        false,
      ))
      .status,
    OutcomeStatus::OutcomeUnknown,
  );
  assert_eq!(
    missing_old
      .gateway_operation("old-stop")
      .unwrap()
      .unwrap()
      .parts[0]
      .status,
    OutcomeStatus::Completed
  );
  assert_eq!(
    missing_old
      .gateway_operation("old-stop")
      .unwrap()
      .unwrap()
      .parts[1]
      .status,
    OutcomeStatus::OutcomeUnknown
  );
  assert_eq!(
    missing_old
      .dispatch_gateway_stop(stop(
        "missing-old-b",
        StopKind::RobotStop {
          robot_id: "r".into()
        },
        true,
      ))
      .status,
    OutcomeStatus::Rejected,
  );
  assert!(missing_old
    .gateway_operation("missing-old-b")
    .unwrap()
    .is_none());
  assert_eq!(
    (
      a.controlled_calls(),
      b.controlled_calls(),
      c.controlled_calls()
    ),
    (1, 1, 0)
  );
  drop(missing_old);

  let expanded = try_fixture_nodes(
    &path,
    &[("a", a.clone()), ("b", b.clone()), ("c", c.clone())],
  )
  .unwrap();
  let still_unknown = expanded.dispatch_gateway_stop(stop(
    "union-unknown",
    StopKind::RobotStop {
      robot_id: "r".into(),
    },
    true,
  ));
  assert_eq!(still_unknown.status, OutcomeStatus::OutcomeUnknown);
  let parts = expanded
    .gateway_operation("union-unknown")
    .unwrap()
    .unwrap()
    .parts;
  assert_eq!(
    parts
      .iter()
      .map(|part| part.node_path.rsplit('/').next().unwrap())
      .collect::<Vec<_>>(),
    ["a", "b", "c"]
  );
  assert_eq!(
    parts.iter().map(|part| part.status).collect::<Vec<_>>(),
    [
      OutcomeStatus::Completed,
      OutcomeStatus::OutcomeUnknown,
      OutcomeStatus::Completed
    ]
  );
  assert!(expanded.movement_inhibited());
  assert_eq!(
    (
      a.controlled_calls(),
      b.controlled_calls(),
      c.controlled_calls()
    ),
    (2, 2, 1)
  );
  b.outcome(ControlledStopOutcome::Stopped);
  assert_eq!(
    expanded
      .dispatch_gateway_stop(stop(
        "union-confirmed",
        StopKind::RobotStop {
          robot_id: "r".into()
        },
        true,
      ))
      .status,
    OutcomeStatus::Completed,
  );
  assert!(!expanded.movement_inhibited());
  assert_eq!(
    (
      a.controlled_calls(),
      b.controlled_calls(),
      c.controlled_calls()
    ),
    (3, 3, 2)
  );
  drop(expanded);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn blocked_controlled_stop_times_out_once_and_safety_estop_still_runs() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let (entered_tx, entered_rx) = std::sync::mpsc::channel();
  let (release_tx, release_rx) = std::sync::mpsc::channel();
  *a.controlled_pause.lock().unwrap() = Some((entered_tx, release_rx));
  let dispatcher = Arc::new(fixture(&path, a.clone(), b.clone()));
  let mut first = stop(
    "blocked",
    StopKind::RobotStop {
      robot_id: "r".into(),
    },
    false,
  );
  first.deadline_monotonic_ns = 500_000_000;
  let stopping = {
    let dispatcher = dispatcher.clone();
    std::thread::spawn(move || dispatcher.dispatch_gateway_stop(first))
  };
  entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
  assert_eq!(
    dispatcher
      .gateway_operation("blocked")
      .unwrap()
      .unwrap()
      .status,
    OutcomeStatus::InProgress,
  );
  let mut during_fanout = move_a("movement-during-fanout");
  during_fanout.meta.deadline_monotonic_ns = 50_000_000;
  let admission_start = std::time::Instant::now();
  assert_eq!(
    dispatcher
      .dispatch_gateway_movement(during_fanout, "base.velocity")
      .status,
    OutcomeStatus::Rejected,
  );
  assert!(admission_start.elapsed() < Duration::from_millis(250));
  assert_eq!((a.movement_calls(), b.movement_calls()), (0, 0));
  assert_eq!(
    dispatcher
      .gateway_operation("blocked")
      .unwrap()
      .unwrap()
      .status,
    OutcomeStatus::InProgress,
  );
  let estop = dispatcher.dispatch_gateway_stop(stop(
    "emergency-during-block",
    StopKind::SafetyEstop {
      robot_id: "r".into(),
    },
    false,
  ));
  assert_eq!(estop.status, OutcomeStatus::Completed);
  assert_eq!(a.emergency_calls.load(Ordering::SeqCst), 1);
  assert_eq!(
    stopping.join().unwrap().status,
    OutcomeStatus::OutcomeUnknown
  );
  assert!(dispatcher.movement_inhibited());
  assert_eq!(a.controlled_calls(), 1);
  let retry = dispatcher.dispatch_gateway_stop(stop(
    "new-id-while-blocked",
    StopKind::RobotStop {
      robot_id: "r".into(),
    },
    true,
  ));
  assert_eq!(retry.status, OutcomeStatus::OutcomeUnknown);
  assert_eq!(a.controlled_calls(), 1);
  assert_eq!(
    dispatcher.dispatch(move_a("movement-stays-blocked")).status,
    OutcomeStatus::Rejected
  );
  drop(dispatcher);
  assert!(try_fixture(&path, a.clone(), b.clone()).is_err());
  release_tx.send(()).unwrap();
  let mut recovered = None;
  for _ in 0..100 {
    if let Ok(owner) = try_fixture(&path, a.clone(), b.clone()) {
      recovered = Some(owner);
      break;
    }
    std::thread::sleep(Duration::from_millis(1));
  }
  let recovered = recovered.expect("physical worker must release retained journal lock");
  assert!(recovered.movement_inhibited());
  assert_eq!(
    recovered
      .gateway_operation("blocked")
      .unwrap()
      .unwrap()
      .status,
    OutcomeStatus::OutcomeUnknown
  );
  drop(recovered);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn blocked_safety_stop_is_bounded_unknown_and_latch_survives_restart() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let (entered_tx, entered_rx) = std::sync::mpsc::channel();
  let (release_tx, release_rx) = std::sync::mpsc::channel();
  *a.emergency_pause.lock().unwrap() = Some((entered_tx, release_rx));
  let dispatcher = Arc::new(fixture(&path, a.clone(), b.clone()));
  let mut request = stop(
    "blocked-safety",
    StopKind::SafetyEstop {
      robot_id: "r".into(),
    },
    false,
  );
  request.deadline_monotonic_ns = 50_000_000;
  let stopping = {
    let dispatcher = dispatcher.clone();
    std::thread::spawn(move || dispatcher.dispatch_gateway_stop(request))
  };
  entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
  assert_eq!(
    stopping.join().unwrap().status,
    OutcomeStatus::OutcomeUnknown
  );
  assert_eq!(a.emergency_calls.load(Ordering::SeqCst), 1);
  let mut second = stop(
    "second-safety",
    StopKind::SafetyEstop {
      robot_id: "r".into(),
    },
    false,
  );
  second.deadline_monotonic_ns = 50_000_000;
  assert_eq!(
    dispatcher.dispatch_gateway_stop(second).status,
    OutcomeStatus::OutcomeUnknown
  );
  assert_eq!(a.emergency_calls.load(Ordering::SeqCst), 1);
  assert_eq!(
    dispatcher.dispatch(move_a("safety-latched")).status,
    OutcomeStatus::Rejected
  );
  drop(dispatcher);
  assert!(try_fixture(&path, a.clone(), b.clone()).is_err());
  release_tx.send(()).unwrap();
  let mut recovered = None;
  for _ in 0..100 {
    if let Ok(owner) = try_fixture(&path, a.clone(), b.clone()) {
      recovered = Some(owner);
      break;
    }
    std::thread::sleep(Duration::from_millis(1));
  }
  let recovered = recovered.expect("physical worker must release retained journal lock");
  assert_eq!(
    recovered
      .gateway_operation("blocked-safety")
      .unwrap()
      .unwrap()
      .status,
    OutcomeStatus::OutcomeUnknown
  );
  assert_eq!(
    recovered.dispatch(move_a("latched-after-restart")).status,
    OutcomeStatus::Rejected
  );
  drop(recovered);
  std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn failed_durable_clear_never_publishes_completed() {
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  let (entered_tx, entered_rx) = std::sync::mpsc::channel();
  let (release_tx, release_rx) = std::sync::mpsc::channel();
  *a.controlled_pause.lock().unwrap() = Some((entered_tx, release_rx));
  let dispatcher = Arc::new(fixture(&path, a.clone(), b.clone()));
  let stopping = {
    let dispatcher = dispatcher.clone();
    std::thread::spawn(move || {
      dispatcher.dispatch_gateway_stop(stop(
        "clear-fails",
        StopKind::RobotStop {
          robot_id: "r".into(),
        },
        false,
      ))
    })
  };
  entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
  let control = path.join(".control");
  let inhibited_bytes = std::fs::read(&control).unwrap();
  std::fs::remove_file(&control).unwrap();
  std::fs::create_dir(&control).unwrap();
  release_tx.send(()).unwrap();
  assert_eq!(
    stopping.join().unwrap().status,
    OutcomeStatus::OutcomeUnknown
  );
  assert_eq!(
    dispatcher
      .gateway_operation("clear-fails")
      .unwrap()
      .unwrap()
      .status,
    OutcomeStatus::OutcomeUnknown
  );
  assert!(dispatcher.movement_inhibited());
  assert_eq!(
    dispatcher.dispatch(move_a("after-clear-failure")).status,
    OutcomeStatus::Rejected
  );
  std::fs::remove_dir(&control).unwrap();
  std::fs::write(&control, inhibited_bytes).unwrap();
  drop(dispatcher);
  let recovered = fixture(&path, a, b);
  assert!(recovered.movement_inhibited());
  assert_eq!(
    recovered
      .gateway_operation("clear-fails")
      .unwrap()
      .unwrap()
      .status,
    OutcomeStatus::OutcomeUnknown
  );
  drop(recovered);
  std::fs::remove_dir_all(path).unwrap();
}
