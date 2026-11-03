//! Durable, typed public stop operations. The release gate gives stop writers
//! priority over new movement permits without holding the journal during an
//! adapter call.

use std::collections::{BTreeSet, HashMap, HashSet};
use std::io::ErrorKind;
use std::sync::{Arc, Condvar, Mutex, mpsc};
use std::sync::atomic::Ordering;
use std::thread;
use std::time::{Duration, Instant};

use aether_hal::{ControlledStopOutcome, NodeKind, NodeState, RobotStopProof, canonical_node_path};

use crate::journal::{
  CommandPhase, GatewayIdentity, GatewayOperation, JournalEntry, JournalTarget, OperationKind,
  StopPartRecord,
};
use crate::{
  AdmissionContext, AdmissionIngress, AuthorityRole, DispatchResult, Dispatcher,
  GatewayRejectionCode, OutcomeStatus, StopKind, StopRequest,
};
use crate::registry::AdapterRegistration;

const MAX_STOP_PARTS: usize = 64;
const MAX_REASON_BYTES: usize = 256;
const MAX_STOP_WAIT: Duration = Duration::from_secs(5);

pub(crate) struct ManagedStopKicks {
  known_ids: HashSet<String>,
  workers: HashMap<String, Arc<ManagedStopKick>>,
}

impl ManagedStopKicks {
  pub(crate) fn new(known_ids: HashSet<String>) -> Self {
    Self {
      known_ids,
      workers: HashMap::new(),
    }
  }

  pub(crate) fn reserve_existing_id(&mut self, id: String) {
    self.known_ids.insert(id);
  }
}

pub(crate) struct ManagedStopKick {
  result: Mutex<Option<Option<RobotStopProof>>>,
  changed: Condvar,
  deadline: Instant,
}

impl ManagedStopKick {
  fn new() -> Self {
    Self {
      result: Mutex::new(None),
      changed: Condvar::new(),
      deadline: Instant::now() + Duration::from_secs(3),
    }
  }

  pub(crate) fn wait(&self, deadline: Instant) -> Option<RobotStopProof> {
    let deadline = deadline.min(self.deadline);
    let mut result = self.result.lock().expect("managed stop result poisoned");
    while result.is_none() {
      let remaining = deadline.saturating_duration_since(Instant::now());
      if remaining.is_zero() {
        return None;
      }
      (result, _) = self
        .changed
        .wait_timeout(result, remaining)
        .expect("managed stop result poisoned");
    }
    result.clone().flatten()
  }
}

/// Separates a transport/storage failure before physical release from a
/// durable domain operation. The enclosed result keeps the legacy Dispatcher
/// call shape available to non-gRPC callers.
#[derive(Clone, Debug)]
pub enum StopDispatchOutcome {
  Operation(DispatchResult),
  PreReleaseCapacity(DispatchResult),
  PreReleaseDurability(DispatchResult),
}

impl StopDispatchOutcome {
  pub fn result(self) -> DispatchResult {
    match self {
      Self::Operation(result)
      | Self::PreReleaseCapacity(result)
      | Self::PreReleaseDurability(result) => result,
    }
  }
}

#[derive(Clone, Copy)]
enum PreReleaseFailure {
  Capacity,
  Durability,
}

#[derive(Default)]
struct GateState {
  active_movements: usize,
  waiting_writers: usize,
  active_writer: bool,
}

/// A writer-preferred barrier. A stop registers as a writer before syncing its
/// inhibit; movements arriving afterward cannot cross the release boundary.
pub(crate) struct ReleaseGate {
  state: Mutex<GateState>,
  changed: Condvar,
}

pub(crate) struct MovementPermit<'a>(&'a ReleaseGate);
pub(crate) struct StopPermit<'a> {
  gate: &'a ReleaseGate,
  active: bool,
}

impl ReleaseGate {
  pub(crate) fn new() -> Self {
    Self {
      state: Mutex::new(GateState::default()),
      changed: Condvar::new(),
    }
  }

  pub(crate) fn movement_permit(&self, deadline: Instant) -> Option<MovementPermit<'_>> {
    let mut state = self.state.lock().expect("release gate poisoned");
    while state.waiting_writers > 0 || state.active_writer || state.active_movements > 0 {
      let now = Instant::now();
      if now >= deadline {
        return None;
      }
      let (next, _) = self
        .changed
        .wait_timeout(state, deadline - now)
        .expect("release gate poisoned");
      state = next;
    }
    if Instant::now() >= deadline {
      return None;
    }
    state.active_movements += 1;
    Some(MovementPermit(self))
  }

  fn stop_permit(&self) -> StopPermit<'_> {
    let mut state = self.state.lock().expect("release gate poisoned");
    state.waiting_writers += 1;
    StopPermit {
      gate: self,
      active: false,
    }
  }
}

impl Drop for MovementPermit<'_> {
  fn drop(&mut self) {
    let mut state = self.0.state.lock().expect("release gate poisoned");
    state.active_movements -= 1;
    self.0.changed.notify_all();
  }
}

impl StopPermit<'_> {
  fn wait_drained(&mut self, deadline: Instant) -> bool {
    let mut state = self.gate.state.lock().expect("release gate poisoned");
    loop {
      if state.active_movements == 0 && !state.active_writer {
        state.active_writer = true;
        self.active = true;
        return true;
      }
      let now = Instant::now();
      if now >= deadline {
        return false;
      }
      let (next, _) = self
        .gate
        .changed
        .wait_timeout(state, deadline - now)
        .expect("release gate poisoned");
      state = next;
    }
  }
}

impl Drop for StopPermit<'_> {
  fn drop(&mut self) {
    let mut state = self.gate.state.lock().expect("release gate poisoned");
    state.waiting_writers -= 1;
    if self.active {
      state.active_writer = false;
    }
    self.gate.changed.notify_all();
  }
}

fn clean_reason(reason: &str) -> String {
  let mut result = String::with_capacity(reason.len().min(MAX_REASON_BYTES));
  for ch in reason.chars() {
    if result.len() + ch.len_utf8() > MAX_REASON_BYTES {
      break;
    }
    result.push(if ch.is_control() { ' ' } else { ch });
  }
  result
}

impl Dispatcher {
  pub(crate) fn start_managed_physical_stop(&self, targets: &[String]) -> Arc<ManagedStopKick> {
    let kick = Arc::new(ManagedStopKick::new());
    let adapter = targets
      .first()
      .and_then(|path| self.registry.nodes.get(path))
      .map(|entry| entry.adapter.clone());
    let worker_kick = kick.clone();
    let owner_lock = self.stop_owner_lock.clone();
    let spawn = thread::Builder::new()
      .name("aether-managed-stop".into())
      .spawn(move || {
        let _owner_lock = owner_lock;
        let proof = match adapter {
          Some(AdapterRegistration::Base { adapter, .. }) => {
            adapter.managed_robot_stop(Duration::from_secs(3))
          }
          Some(AdapterRegistration::Manipulator { adapter, .. }) => {
            adapter.managed_robot_stop(Duration::from_secs(3))
          }
          _ => None,
        };
        *worker_kick
          .result
          .lock()
          .expect("managed stop result poisoned") = Some(proof);
        worker_kick.changed.notify_all();
      });
    if spawn.is_err() {
      *kick.result.lock().expect("managed stop result poisoned") = Some(None);
      kick.changed.notify_all();
    }
    kick
  }

  fn preemptive_managed_stop(&self, admission: &AdmissionContext, request: &StopRequest) {
    if request.command_id.is_empty()
      || request.command_id.len() > 128
      || self.clock.monotonic_ns() >= request.deadline_monotonic_ns
    {
      return;
    }
    let kind = &request.kind;
    let supported = match kind {
      StopKind::RobotStop { robot_id } => {
        robot_id == &self.robot_id
          && self.supports_non_latching_robot_stop()
          && (!self.movement_inhibited() || request.authorized_recovery)
      }
      StopKind::SafetyEstop { robot_id } => {
        robot_id == &self.robot_id && self.supports_safety_estop()
      }
      StopKind::MotionStop { .. } => false,
    };
    let targets = self.stop_targets();
    if !supported
      || !self.is_managed_mujoco_stop(&targets)
      || self
        .validate_managed_stop(admission, kind, request.authorized_recovery)
        .is_err()
    {
      return;
    }
    let mut kicks = self
      .managed_stop_kicks
      .lock()
      .expect("managed stop kicks poisoned");
    if !kicks.known_ids.insert(request.command_id.clone()) {
      return;
    }
    // The caller registers the release-gate writer before this worker runs.
    self.movement_inhibited.store(true, Ordering::SeqCst);
    if matches!(kind, StopKind::SafetyEstop { .. }) {
      self.emergency_stopped.store(true, Ordering::SeqCst);
    }
    kicks.workers.insert(
      request.command_id.clone(),
      self.start_managed_physical_stop(&targets),
    );
  }

  fn managed_kick(&self, id: &str) -> Option<Arc<ManagedStopKick>> {
    self
      .managed_stop_kicks
      .lock()
      .expect("managed stop kicks poisoned")
      .workers
      .get(id)
      .cloned()
  }

  pub(crate) fn is_managed_mujoco_stop(&self, targets: &[String]) -> bool {
    self.managed.is_some()
      && !targets.is_empty()
      && targets.iter().all(|path| {
        self
          .registry
          .nodes
          .get(path)
          .is_some_and(|entry| match &entry.adapter {
            AdapterRegistration::Base { adapter, .. } => adapter.adapter_type() == "mujoco-v2",
            AdapterRegistration::Manipulator { adapter, .. } => {
              adapter.adapter_type() == "mujoco-v2"
            }
            AdapterRegistration::Camera { .. } => false,
          })
      })
  }

  pub(crate) fn shared_mujoco_proof(
    &self,
    targets: &[String],
    timeout: Duration,
  ) -> Option<RobotStopProof> {
    let entry = self.registry.nodes.get(targets.first()?)?;
    match &entry.adapter {
      AdapterRegistration::Base { adapter, .. } => adapter.managed_robot_stop(timeout),
      AdapterRegistration::Manipulator { adapter, .. } => adapter.managed_robot_stop(timeout),
      AdapterRegistration::Camera { .. } => None,
    }
  }

  fn pre_release_managed_stop_fallback(&self, targets: &[String], kind: OperationKind) {
    if kind == OperationKind::MotionStop || !self.is_managed_mujoco_stop(targets) {
      return;
    }
    // A failed journal intent cannot become a confirmed operation, but it
    // must not strand an actuator while its ordinary release permit is held.
    self.movement_inhibited.store(true, Ordering::SeqCst);
    if kind == OperationKind::SafetyEstop {
      self.emergency_stopped.store(true, Ordering::SeqCst);
      // A later successful routine-marker write must never hide this failed
      // emergency intent, even when no stop record could be reserved.
      self
        .public_estop_durability_failed
        .store(true, Ordering::SeqCst);
    }
    // Every eligible managed stop already scheduled its physical request
    // before the journal was touched. A denied request has no safety grant.
  }

  fn dispatch_shared_mujoco_stop(
    &self,
    id: &str,
    kind: OperationKind,
    prior_inhibit: bool,
    targets: &[String],
    deadline: Instant,
  ) -> DispatchResult {
    let deadline = self
      .managed_kick(id)
      .map_or(deadline, |kick| deadline.min(kick.deadline));
    // One durable update marks every selected part attempting before the one
    // whole-robot request. A failed write cannot suppress the physical stop.
    let intent_ok = self
      .journal
      .lock()
      .expect("journal poisoned")
      .update(id, |entry| {
        for part in &mut entry.operation.as_mut().expect("typed stop").parts {
          part.attempting = true;
        }
      })
      .is_ok();
    let proof = match self.managed_kick(id) {
      Some(kick) => kick.wait(deadline),
      None => self.shared_mujoco_proof(targets, deadline.saturating_duration_since(Instant::now())),
    };
    let outcome_for = |path: &str| {
      let kind = self
        .registry
        .nodes
        .get(path)
        .map(|entry| entry.profile.kind);
      match (kind, proof.as_ref()) {
        (Some(NodeKind::Base), Some(proof)) => proof.base.clone(),
        (Some(NodeKind::Manipulator), Some(proof)) => proof.arm.clone(),
        _ => ControlledStopOutcome::OutcomeUnknown,
      }
    };
    let outcomes: Vec<_> = targets.iter().map(|path| outcome_for(path)).collect();
    let parts_ok = self
      .journal
      .lock()
      .expect("journal poisoned")
      .update(id, |entry| {
        for (part, outcome) in entry
          .operation
          .as_mut()
          .expect("typed stop")
          .parts
          .iter_mut()
          .zip(&outcomes)
        {
          part.status = Some(if *outcome == ControlledStopOutcome::Stopped {
            OutcomeStatus::Completed
          } else {
            OutcomeStatus::OutcomeUnknown
          });
          part.reason = (*outcome != ControlledStopOutcome::Stopped)
            .then(|| "physical stop outcome unknown".into());
        }
      })
      .is_ok();
    let dual = proof.as_ref().is_some_and(|proof| {
      proof.base == ControlledStopOutcome::Stopped && proof.arm == ControlledStopOutcome::Stopped
    });
    if !intent_ok || !parts_ok || !dual || Instant::now() > deadline {
      self.movement_inhibited.store(true, Ordering::SeqCst);
      return self.finish_stop(
        id,
        OutcomeStatus::OutcomeUnknown,
        "whole-robot stop did not durably confirm",
        true,
      );
    }
    if kind == OperationKind::SafetyEstop {
      return self.finish_stop(id, OutcomeStatus::Completed, "", true);
    }
    self.finish_confirmed_stop(id, kind, prior_inhibit, targets)
  }

  fn validate_managed_stop(
    &self,
    context: &AdmissionContext,
    kind: &StopKind,
    recovery: bool,
  ) -> Result<(), String> {
    let generation = self
      .managed
      .as_ref()
      .ok_or("managed generation unavailable")?;
    if context.run_id != generation.run_id {
      return Err("deployment run differs from active generation".into());
    }
    if context.ingress != AdmissionIngress::Grpc {
      return Err("managed stop ingress is not admitted".into());
    }
    match kind {
      StopKind::MotionStop { node_path } => {
        if context.active_revision.as_deref() != Some(generation.active_revision.as_str()) {
          return Err("saved active revision differs from admitted generation".into());
        }
        if context.primitive != "motion.stop" || context.primitive_version != "1.0.0" {
          return Err("stop primitive identity differs".into());
        }
        let bound_target = generation
          .bindings
          .get(&context.binding_path)
          .map(|binding| binding.node_path.as_str());
        if context.binding_path != *node_path && bound_target != Some(node_path.as_str()) {
          return Err("motion stop differs from admitted device binding".into());
        }
        if recovery {
          return Err("motion stop cannot authorize recovery".into());
        }
      }
      StopKind::RobotStop { .. } => {
        if context.active_revision.as_deref() != Some(generation.active_revision.as_str()) {
          return Err("saved active revision differs from admitted generation".into());
        }
        if context.primitive != "robot.stop"
          || context.primitive_version != "1.0.0"
          || !context.binding_path.is_empty()
        {
          return Err("robot stop identity differs".into());
        }
        if recovery && context.authority_role != AuthorityRole::OperatorControl {
          return Err("robot stop recovery requires operator authority".into());
        }
      }
      StopKind::SafetyEstop { .. } => {
        if context.primitive != "safety.estop"
          || context.primitive_version != "1.0.0"
          || !context.binding_path.is_empty()
          || recovery
        {
          return Err("safety estop identity differs".into());
        }
      }
    }
    Ok(())
  }

  /// True only for a typed, terminal, single-device stop path.
  pub fn supports_motion_stop(&self, node_path: &str) -> bool {
    let Some(path) = canonical_node_path(node_path) else {
      return false;
    };
    self.registry.nodes.get(&path).is_some_and(|entry| {
      entry.profile.movement.is_some()
        && match &entry.adapter {
          AdapterRegistration::Base { adapter, .. } => adapter.adapter_type() != "mujoco-v2",
          AdapterRegistration::Manipulator { adapter, .. } => adapter.adapter_type() != "mujoco-v2",
          AdapterRegistration::Camera { .. } => false,
        }
        && match &entry.adapter {
          AdapterRegistration::Base { adapter, .. } => {
            adapter.has_terminal_stop_fence() && adapter.supports_non_latching_stop()
          }
          AdapterRegistration::Manipulator { adapter, .. } => {
            adapter.has_terminal_stop_fence() && adapter.supports_non_latching_stop()
          }
          AdapterRegistration::Camera { .. } => false,
        }
    })
  }

  /// A robot-wide non-latching stop requires an explicit promise from every
  /// admitted actuator. CXX mock and current MuJoCo both fail this predicate.
  pub fn supports_non_latching_robot_stop(&self) -> bool {
    let targets = self.stop_targets();
    !targets.is_empty()
      && targets.len() <= MAX_STOP_PARTS
      && targets.iter().all(|path| {
        self
          .registry
          .nodes
          .get(path)
          .is_some_and(|entry| match &entry.adapter {
            AdapterRegistration::Base { adapter, .. } => {
              adapter.has_terminal_stop_fence() && adapter.supports_non_latching_stop()
            }
            AdapterRegistration::Manipulator { adapter, .. } => {
              adapter.has_terminal_stop_fence() && adapter.supports_non_latching_stop()
            }
            AdapterRegistration::Camera { .. } => false,
          })
      })
  }

  /// The safety path is present when at least one admitted actuator can be
  /// latched. Terminal success still requires each adapter's own fence proof.
  pub fn supports_safety_estop(&self) -> bool {
    let targets = self.stop_targets();
    !targets.is_empty() && targets.len() <= MAX_STOP_PARTS
  }

  fn stop_targets(&self) -> Vec<String> {
    self
      .registry
      .nodes
      .iter()
      .filter(|(_, entry)| entry.profile.movement.is_some())
      .map(|(path, _)| path.clone())
      .collect()
  }

  /// One durable operation ID owns one logical stop release. The synchronous
  /// return is a snapshot; duplicate IDs are query-only and never call an
  /// adapter again.
  pub fn dispatch_gateway_stop(&self, request: StopRequest) -> DispatchResult {
    self.dispatch_gateway_stop_checked(request).result()
  }

  /// Preserve transport classification only when persistence fails before a
  /// physical call. Semantic denial is a durable `Operation::Rejected`.
  pub fn dispatch_gateway_stop_checked(&self, request: StopRequest) -> StopDispatchOutcome {
    if self.managed.is_some() {
      return StopDispatchOutcome::Operation(DispatchResult::new(
        &request.command_id,
        OutcomeStatus::Rejected,
        Some("managed generation requires governed admission".into()),
      ));
    }
    let mut pre_release_failure = None;
    let result = self.dispatch_gateway_stop_inner(request, None, &mut pre_release_failure);
    match pre_release_failure {
      None => StopDispatchOutcome::Operation(result),
      Some(PreReleaseFailure::Capacity) => StopDispatchOutcome::PreReleaseCapacity(result),
      Some(PreReleaseFailure::Durability) => StopDispatchOutcome::PreReleaseDurability(result),
    }
  }

  pub fn dispatch_managed_stop_checked(
    &self,
    admission: AdmissionContext,
    request: StopRequest,
  ) -> StopDispatchOutcome {
    let Some(_public_stop) = self.begin_public_stop() else {
      return StopDispatchOutcome::Operation(DispatchResult::new(
        &request.command_id,
        OutcomeStatus::Rejected,
        Some("runtime is quiescing".into()),
      ));
    };
    // Register a writer before touching the journal: a stalled Releasing
    // fsync cannot delay the independent companion physical stop.
    let _preemptive_writer = self.release_gate.stop_permit();
    self.preemptive_managed_stop(&admission, &request);
    let estop_requested = matches!(request.kind, StopKind::SafetyEstop { .. });
    let command_id = request.command_id.clone();
    let mut pre_release_failure = None;
    let result =
      self.dispatch_gateway_stop_inner(request, Some(&admission), &mut pre_release_failure);
    if estop_requested {
      let journal = self.journal.lock().expect("journal poisoned");
      if journal.get(&command_id).is_some_and(|entry| {
        entry.operation.as_ref().is_some_and(|operation| {
          operation.identity.kind == OperationKind::SafetyEstop
            && operation
              .identity
              .admission
              .as_ref()
              .is_some_and(|context| context.run_id == admission.run_id)
            && (operation.accepted != Some(true) || !journal.estop_latched())
        })
      }) {
        self
          .public_estop_durability_failed
          .store(true, Ordering::SeqCst);
      }
    }
    match pre_release_failure {
      None => StopDispatchOutcome::Operation(result),
      Some(PreReleaseFailure::Capacity) => StopDispatchOutcome::PreReleaseCapacity(result),
      Some(PreReleaseFailure::Durability) => StopDispatchOutcome::PreReleaseDurability(result),
    }
  }

  fn dispatch_gateway_stop_inner(
    &self,
    request: StopRequest,
    admission: Option<&AdmissionContext>,
    pre_release_failure: &mut Option<PreReleaseFailure>,
  ) -> DispatchResult {
    let id = &request.command_id;
    let reject =
      |reason: &str| DispatchResult::new(id, OutcomeStatus::Rejected, Some(reason.into()));
    if id.is_empty() || id.len() > 128 {
      return reject("invalid command ID");
    }
    let canonical_kind = match &request.kind {
      StopKind::MotionStop { node_path } => StopKind::MotionStop {
        node_path: canonical_node_path(node_path).unwrap_or_else(|| node_path.clone()),
      },
      other => other.clone(),
    };
    let signature = match admission {
      Some(context) => serde_json::to_string(&(
        "4.0.0",
        &canonical_kind,
        context,
        request.authorized_recovery,
      )),
      None => serde_json::to_string(&("3.0.0", &canonical_kind)),
    };
    let signature = match signature {
      Ok(signature) if signature.len() <= 64 * 1024 => signature,
      _ => return reject("stop request exceeds journal bound"),
    };
    if let Some(existing) = self.journal.lock().expect("journal poisoned").get(id) {
      return if existing.signature == signature {
        match admission {
          Some(context) => {
            match self.validate_managed_stop(context, &canonical_kind, request.authorized_recovery)
            {
              Ok(()) => existing.result.clone(),
              Err(reason) => reject(&reason),
            }
          }
          None => existing.result.clone(),
        }
      } else {
        reject("conflicting command ID reuse")
      };
    }
    if let Some(context) = admission {
      if let Err(reason) =
        self.validate_managed_stop(context, &canonical_kind, request.authorized_recovery)
      {
        return reject(&reason);
      }
    } else if self.managed.is_some() {
      return reject("managed generation requires governed admission");
    }
    let _stop_serial = if matches!(request.kind, StopKind::SafetyEstop { .. }) {
      None
    } else {
      Some(self.stop_serial.lock().expect("stop serial lock poisoned"))
    };
    // A concurrent identical RobotStop may have reserved and finished while
    // this caller waited on stop_serial. Replay remains query-only even when
    // the first result left the recovery inhibit engaged.
    if let Some(existing) = self.journal.lock().expect("journal poisoned").get(id) {
      return if existing.signature == signature {
        match admission {
          Some(context) => {
            match self.validate_managed_stop(context, &canonical_kind, request.authorized_recovery)
            {
              Ok(()) => existing.result.clone(),
              Err(reason) => reject(&reason),
            }
          }
          None => existing.result.clone(),
        }
      } else {
        reject("conflicting command ID reuse")
      };
    }
    if self.clock.monotonic_ns() >= request.deadline_monotonic_ns {
      return reject("stop deadline expired");
    }
    let (kind, primitive, target, node_path, targets) = match &canonical_kind {
      StopKind::MotionStop { node_path } => {
        let Some(path) = canonical_node_path(node_path) else {
          return reject("unknown stop device");
        };
        if !self.supports_motion_stop(&path) {
          return reject("terminal motion stop unavailable");
        }
        (
          OperationKind::MotionStop,
          "motion.stop",
          JournalTarget::Hardware {
            node_path: path.clone(),
          },
          path.clone(),
          vec![path],
        )
      }
      StopKind::RobotStop { robot_id } => {
        if robot_id != &self.robot_id {
          return reject("unknown robot");
        }
        if !self.supports_non_latching_robot_stop() {
          return reject("non-latching robot stop unavailable");
        }
        let mut all: BTreeSet<_> = self.stop_targets().into_iter().collect();
        all.extend(
          self
            .journal
            .lock()
            .expect("journal poisoned")
            .robot_targets()
            .iter()
            .cloned(),
        );
        all.extend(
          self
            .journal
            .lock()
            .expect("journal poisoned")
            .unresolved_movement_targets(),
        );
        (
          OperationKind::RobotStop,
          "robot.stop",
          JournalTarget::Robot {
            robot_id: robot_id.clone(),
          },
          String::new(),
          all.into_iter().collect(),
        )
      }
      StopKind::SafetyEstop { robot_id } => {
        if robot_id != &self.robot_id {
          return reject("unknown robot");
        }
        if !self.supports_safety_estop() {
          return reject("safety stop unavailable");
        }
        (
          OperationKind::SafetyEstop,
          "safety.estop",
          JournalTarget::Robot {
            robot_id: robot_id.clone(),
          },
          String::new(),
          self.stop_targets(),
        )
      }
    };
    if targets.len() > MAX_STOP_PARTS {
      return reject("stop target capacity exceeded");
    }
    if targets
      .iter()
      .any(|path| !self.registry.nodes.contains_key(path))
    {
      return reject("previous stop target is unavailable");
    }
    if kind == OperationKind::RobotStop
      && targets.iter().any(|path| {
        self
          .registry
          .nodes
          .get(path)
          .is_none_or(|entry| match &entry.adapter {
            AdapterRegistration::Base { adapter, .. } => {
              !adapter.has_terminal_stop_fence() || !adapter.supports_non_latching_stop()
            }
            AdapterRegistration::Manipulator { adapter, .. } => {
              !adapter.has_terminal_stop_fence() || !adapter.supports_non_latching_stop()
            }
            AdapterRegistration::Camera { .. } => true,
          })
      })
    {
      return reject("previous stop target lacks a controlled physical fence");
    }
    let parts = if kind == OperationKind::RobotStop
      || (kind == OperationKind::SafetyEstop && self.is_managed_mujoco_stop(&targets))
    {
      targets
        .iter()
        .map(|path| StopPartRecord {
          node_path: path.clone(),
          attempting: false,
          status: None,
          reason: None,
        })
        .collect()
    } else {
      Vec::new()
    };
    let operation = GatewayOperation {
      identity: GatewayIdentity {
        primitive: primitive.into(),
        version: "1.0.0".into(),
        target,
        kind,
        admission: admission.cloned(),
        recovery: admission.is_some() && request.authorized_recovery,
      },
      accepted: None,
      rejection_code: None,
      parts,
      fenced: false,
    };
    {
      let mut journal = self.journal.lock().expect("journal poisoned");
      if let Some(existing) = journal.get(id) {
        return if existing.signature == signature {
          existing.result.clone()
        } else {
          reject("conflicting command ID reuse")
        };
      }
      if let Err(error) = journal.insert(
        id.clone(),
        JournalEntry {
          node_path,
          signature,
          phase: CommandPhase::Prepared,
          result: DispatchResult::new(id, OutcomeStatus::InProgress, None),
          operation: Some(operation),
        },
      ) {
        *pre_release_failure = Some(if error.kind() == ErrorKind::OutOfMemory {
          PreReleaseFailure::Capacity
        } else {
          PreReleaseFailure::Durability
        });
        drop(journal);
        self.pre_release_managed_stop_fallback(&targets, kind);
        return reject("stop reservation could not be persisted");
      }
    }
    if kind == OperationKind::RobotStop
      && self.movement_inhibited()
      && !request.authorized_recovery
      && self.managed_kick(id).is_none()
    {
      return self.finish_stop_with_code(
        id,
        OutcomeStatus::Rejected,
        "robot stop recovery requires local authority",
        false,
        GatewayRejectionCode::Unauthorized,
      );
    }
    let mut writer = if kind == OperationKind::SafetyEstop {
      None
    } else {
      Some(self.release_gate.stop_permit())
    };
    let prior_inhibit = self
      .journal
      .lock()
      .expect("journal poisoned")
      .robot_inhibited();
    if kind == OperationKind::RobotStop || (kind == OperationKind::MotionStop && !prior_inhibit) {
      if self
        .journal
        .lock()
        .expect("journal poisoned")
        .set_robot_inhibit(targets.clone())
        .is_err()
      {
        *pre_release_failure = Some(PreReleaseFailure::Durability);
        self.pre_release_managed_stop_fallback(&targets, kind);
        return self.finish_stop_with_code(
          id,
          OutcomeStatus::Rejected,
          "stop inhibit could not be persisted",
          false,
          GatewayRejectionCode::Internal,
        );
      }
      self.movement_inhibited.store(true, Ordering::SeqCst);
      #[cfg(test)]
      if let Some((entered, release)) = self
        .stop_inhibit_pause
        .lock()
        .expect("test stop inhibit pause poisoned")
        .take()
      {
        let _ = entered.send(());
        let _ = release.recv();
      }
    }
    if kind == OperationKind::SafetyEstop {
      if self
        .journal
        .lock()
        .expect("journal poisoned")
        .latch_estop()
        .is_err()
      {
        *pre_release_failure = Some(PreReleaseFailure::Durability);
        self.pre_release_managed_stop_fallback(&targets, kind);
        return self.finish_stop_with_code(
          id,
          OutcomeStatus::Rejected,
          "safety latch could not be persisted",
          false,
          GatewayRejectionCode::Internal,
        );
      }
      self.emergency_stopped.store(true, Ordering::SeqCst);
    }
    if self
      .journal
      .lock()
      .expect("journal poisoned")
      .update(id, |entry| {
        entry.phase = CommandPhase::Releasing;
      })
      .is_err()
    {
      *pre_release_failure = Some(PreReleaseFailure::Durability);
      self.pre_release_managed_stop_fallback(&targets, kind);
      return self.finish_stop_with_code(
        id,
        OutcomeStatus::Rejected,
        "stop release could not be persisted",
        false,
        GatewayRejectionCode::Internal,
      );
    }
    let stop_deadline = Instant::now()
      + MAX_STOP_WAIT.min(Duration::from_nanos(
        request
          .deadline_monotonic_ns
          .saturating_sub(self.clock.monotonic_ns()),
      ));
    if self.is_managed_mujoco_stop(&targets) && kind != OperationKind::MotionStop {
      return self.dispatch_shared_mujoco_stop(
        id,
        kind,
        prior_inhibit,
        &targets,
        stop_deadline.min(Instant::now() + Duration::from_secs(3)),
      );
    }
    if let Some(gate) = &mut writer {
      if !gate.wait_drained(stop_deadline) {
        return self.finish_stop(
          id,
          OutcomeStatus::OutcomeUnknown,
          "prior movement did not reach a physical fence",
          true,
        );
      }
    }
    let mut any_failed = false;
    let mut any_unknown = false;
    for (index, path) in targets.iter().enumerate() {
      if self.clock.monotonic_ns() >= request.deadline_monotonic_ns
        || Instant::now() >= stop_deadline
      {
        any_unknown = true;
        break;
      }
      if kind == OperationKind::RobotStop
        && self
          .journal
          .lock()
          .expect("journal poisoned")
          .update(id, |entry| {
            entry.operation.as_mut().expect("typed stop").parts[index].attempting = true;
          })
          .is_err()
      {
        any_unknown = true;
        break;
      }
      let outcome = self.stop_part(path, kind, stop_deadline);
      if kind == OperationKind::RobotStop {
        let (status, reason) = outcome.clone();
        if self
          .journal
          .lock()
          .expect("journal poisoned")
          .update(id, |entry| {
            let part = &mut entry.operation.as_mut().expect("typed stop").parts[index];
            part.status = Some(status);
            part.reason = reason;
          })
          .is_err()
        {
          any_unknown = true;
          break;
        }
        #[cfg(test)]
        {
          // Model process loss after this part is synced, before the next
          // part can be marked attempting. Consume and drop the hook lock
          // before panicking so this is solely a crash boundary test.
          let crash = {
            let mut hook = self
              .stop_crash_after_part
              .lock()
              .expect("stop crash hook poisoned");
            if *hook == Some(index) {
              hook.take();
              true
            } else {
              false
            }
          };
          if crash {
            panic!("injected crash after durable RobotStop part result");
          }
        }
      }
      match outcome.0 {
        OutcomeStatus::Completed => {}
        OutcomeStatus::Failed => any_failed = true,
        _ => any_unknown = true,
      }
    }
    let status = if any_unknown {
      OutcomeStatus::OutcomeUnknown
    } else if any_failed {
      OutcomeStatus::Failed
    } else {
      OutcomeStatus::Completed
    };
    if status == OutcomeStatus::Completed && kind != OperationKind::SafetyEstop {
      return self.finish_confirmed_stop(id, kind, prior_inhibit, &targets);
    }
    self.finish_stop(id, status, "stop operation did not fully confirm", true)
  }

  fn finish_confirmed_stop(
    &self,
    id: &str,
    kind: OperationKind,
    prior_inhibit: bool,
    targets: &[String],
  ) -> DispatchResult {
    let mut journal = self.journal.lock().expect("journal poisoned");
    // Readers of this journal cannot observe Completed until the durable
    // movement fence and inhibit clear both succeed. A crash in this gap
    // leaves Releasing on disk; Journal::open reinstates its inhibit.
    let clear = if kind == OperationKind::RobotStop {
      journal
        .fence_uncertain_movements()
        .and_then(|_| journal.clear_robot_inhibit())
    } else if !prior_inhibit {
      journal.clear_robot_inhibit()
    } else {
      Ok(())
    };
    if clear.is_err() {
      drop(journal);
      return self.finish_stop(
        id,
        OutcomeStatus::OutcomeUnknown,
        "stop clear could not be persisted",
        true,
      );
    }
    let result = DispatchResult::new(id, OutcomeStatus::Completed, None);
    if journal
      .update(id, |entry| {
        entry.phase = CommandPhase::Terminal;
        entry.result = result.clone();
        entry.operation.as_mut().expect("typed stop").accepted = Some(true);
      })
      .is_err()
    {
      journal.mark_uncertain_in_memory(id, "stop outcome could not be persisted".into());
      if !prior_inhibit || kind == OperationKind::RobotStop {
        let _ = journal.set_robot_inhibit(targets.to_vec());
      }
      self.movement_inhibited.store(true, Ordering::SeqCst);
    } else {
      self.movement_inhibited.store(
        journal.has_uncertain_movement() || journal.robot_inhibited(),
        Ordering::SeqCst,
      );
    }
    journal
      .get(id)
      .expect("reserved stop missing")
      .result
      .clone()
  }

  fn stop_part(
    &self,
    path: &str,
    kind: OperationKind,
    deadline: Instant,
  ) -> (OutcomeStatus, Option<String>) {
    let Some(entry) = self.registry.nodes.get(path) else {
      return (
        OutcomeStatus::OutcomeUnknown,
        Some("stop target unavailable".into()),
      );
    };
    // Safety uses a separate preemptive slot. A blocked controlled stop may
    // never hold the emergency path, while a second ID cannot spawn a repeat
    // of either outstanding physical call.
    let key = format!(
      "{}:{path}",
      if kind == OperationKind::SafetyEstop {
        "safety"
      } else {
        "controlled"
      }
    );
    {
      let mut active = self
        .stop_inflight
        .lock()
        .expect("stop in-flight lock poisoned");
      if !active.insert(key.clone()) {
        return (
          OutcomeStatus::OutcomeUnknown,
          Some("physical stop already in flight".into()),
        );
      }
    }
    let owner_lock = match self
      .journal
      .lock()
      .expect("journal poisoned")
      .retain_owner_lock()
    {
      Ok(lock) => lock,
      Err(_) => {
        self
          .stop_inflight
          .lock()
          .expect("stop in-flight lock poisoned")
          .remove(&key);
        return (
          OutcomeStatus::OutcomeUnknown,
          Some("journal owner could not be retained".into()),
        );
      }
    };
    let adapter = entry.adapter.clone();
    let active = self.stop_inflight.clone();
    let (sender, receiver) = mpsc::sync_channel(1);
    let failure_key = key.clone();
    let worker = thread::Builder::new()
      .name("aether-stop-part".into())
      .spawn(move || {
        let _owner_lock = owner_lock;
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
          physical_stop(adapter, kind)
        }))
        .unwrap_or((
          OutcomeStatus::OutcomeUnknown,
          Some("physical stop worker panicked".into()),
        ));
        active
          .lock()
          .expect("stop in-flight lock poisoned")
          .remove(&key);
        let _ = sender.send(result);
      });
    if worker.is_err() {
      self
        .stop_inflight
        .lock()
        .expect("stop in-flight lock poisoned")
        .remove(&failure_key);
      return (
        OutcomeStatus::OutcomeUnknown,
        Some("physical stop worker unavailable".into()),
      );
    }
    match receiver.recv_timeout(deadline.saturating_duration_since(Instant::now())) {
      Ok(outcome) => outcome,
      Err(mpsc::RecvTimeoutError::Timeout) => (
        OutcomeStatus::OutcomeUnknown,
        Some("physical stop deadline exceeded".into()),
      ),
      Err(mpsc::RecvTimeoutError::Disconnected) => (
        OutcomeStatus::OutcomeUnknown,
        Some("physical stop worker ended without proof".into()),
      ),
    }
  }

  fn finish_stop(
    &self,
    id: &str,
    status: OutcomeStatus,
    reason: &str,
    accepted: bool,
  ) -> DispatchResult {
    self.finish_stop_with_code(
      id,
      status,
      reason,
      accepted,
      GatewayRejectionCode::InvalidState,
    )
  }

  fn finish_stop_with_code(
    &self,
    id: &str,
    status: OutcomeStatus,
    reason: &str,
    accepted: bool,
    rejection_code: GatewayRejectionCode,
  ) -> DispatchResult {
    let mut journal = self.journal.lock().expect("journal poisoned");
    let result = DispatchResult::new(
      id,
      status,
      (status != OutcomeStatus::Completed).then(|| reason.into()),
    );
    if journal
      .update(id, |entry| {
        entry.phase = CommandPhase::Terminal;
        entry.result = result.clone();
        if let Some(operation) = &mut entry.operation {
          operation.accepted = Some(accepted);
          operation.rejection_code = (status == OutcomeStatus::Rejected).then_some(rejection_code);
        }
      })
      .is_err()
    {
      journal.mark_uncertain_in_memory(id, "stop outcome could not be persisted".into());
      self.movement_inhibited.store(true, Ordering::SeqCst);
    }
    journal
      .get(id)
      .expect("reserved stop missing")
      .result
      .clone()
  }
}

fn physical_stop(
  adapter: AdapterRegistration,
  kind: OperationKind,
) -> (OutcomeStatus, Option<String>) {
  let fence = match &adapter {
    AdapterRegistration::Base { adapter, .. } => adapter.has_terminal_stop_fence(),
    AdapterRegistration::Manipulator { adapter, .. } => adapter.has_terminal_stop_fence(),
    AdapterRegistration::Camera { .. } => false,
  };
  if kind == OperationKind::MotionStop && fence {
    let state = match &adapter {
      AdapterRegistration::Base { adapter, .. } => adapter.status().state,
      AdapterRegistration::Manipulator { adapter, .. } => adapter.status().state,
      AdapterRegistration::Camera { .. } => unreachable!("validated stop target"),
    };
    if state == NodeState::Stopped {
      return (OutcomeStatus::Completed, None);
    }
  }
  if kind != OperationKind::SafetyEstop {
    let outcome = match &adapter {
      AdapterRegistration::Base { adapter, .. } => adapter.controlled_stop(),
      AdapterRegistration::Manipulator { adapter, .. } => adapter.controlled_stop(),
      AdapterRegistration::Camera { .. } => unreachable!("validated stop target"),
    };
    return match outcome {
      Ok(ControlledStopOutcome::Stopped) if fence => (OutcomeStatus::Completed, None),
      Ok(ControlledStopOutcome::ConfirmedFailure(reason)) => {
        (OutcomeStatus::Failed, Some(clean_reason(&reason)))
      }
      Ok(ControlledStopOutcome::OutcomeUnknown | ControlledStopOutcome::Stopped) => (
        OutcomeStatus::OutcomeUnknown,
        Some("controlled stop lacks terminal proof".into()),
      ),
      Err(error) => (
        OutcomeStatus::OutcomeUnknown,
        Some(clean_reason(&error.to_string())),
      ),
    };
  }
  let outcome = match &adapter {
    AdapterRegistration::Base { adapter, .. } => adapter.stop().map(|()| adapter.status().state),
    AdapterRegistration::Manipulator { adapter, .. } => {
      adapter.stop().map(|()| adapter.status().state)
    }
    AdapterRegistration::Camera { .. } => unreachable!("validated stop target"),
  };
  match outcome {
    Ok(NodeState::Stopped) if fence => (OutcomeStatus::Completed, None),
    Ok(_) => (
      OutcomeStatus::OutcomeUnknown,
      Some("stop lacks terminal proof".into()),
    ),
    Err(error) => (
      OutcomeStatus::OutcomeUnknown,
      Some(clean_reason(&error.to_string())),
    ),
  }
}
