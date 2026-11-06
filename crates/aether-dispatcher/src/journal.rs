//! Durable command reservations. A release is allowed only after its reservation
//! and Releasing transition have been synced to disk.

use std::collections::HashMap;
use std::fs::{self, File, OpenOptions};
use std::io::{self, ErrorKind};
use std::path::{Path, PathBuf};
#[cfg(test)]
use std::sync::Mutex;

use aether_common::io::{read_bounded, write_atomic_bounded};
use fs2::FileExt;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

use crate::{AdmissionContext, DispatchResult, GatewayRejectionCode, OutcomeStatus};

const SCHEMA_VERSION: &str = "2.0.0";
const STOP_SCHEMA_VERSION: &str = "3.0.0";
const LEGACY_CONTROL_SCHEMA_VERSION: &str = "1.0.0";
const CONTROL_SCHEMA_VERSION: &str = "1.1.0";
const MAX_ID_BYTES: usize = 128;
const MAX_RECORD_BYTES: usize = 128 * 1024;
const MAX_RECORDS: usize = 100_000;

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub(crate) enum CommandPhase {
  Prepared,
  Releasing,
  Terminal,
}

#[derive(Clone)]
pub(crate) struct JournalEntry {
  pub(crate) node_path: String,
  pub(crate) signature: String,
  pub(crate) phase: CommandPhase,
  pub(crate) result: DispatchResult,
  pub(crate) operation: Option<GatewayOperation>,
}

/// The public operation identity is stored in the existing command record.
/// Legacy records have no identity and remain query-only, never guessed.
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub(crate) enum JournalTarget {
  Hardware { node_path: String },
  Robot { robot_id: String },
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub(crate) enum OperationKind {
  Movement,
  MotionStop,
  RobotStop,
  SafetyEstop,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub(crate) struct GatewayIdentity {
  pub(crate) primitive: String,
  pub(crate) version: String,
  pub(crate) target: JournalTarget,
  pub(crate) kind: OperationKind,
  /// Historical R02 records omit this. They remain query-only in managed mode.
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub(crate) admission: Option<AdmissionContext>,
  #[serde(default, skip_serializing_if = "is_false")]
  pub(crate) recovery: bool,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub(crate) struct GatewayOperation {
  pub(crate) identity: GatewayIdentity,
  pub(crate) accepted: Option<bool>,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub(crate) rejection_code: Option<GatewayRejectionCode>,
  #[serde(default, skip_serializing_if = "Vec::is_empty")]
  pub(crate) parts: Vec<StopPartRecord>,
  #[serde(default, skip_serializing_if = "is_false")]
  pub(crate) fenced: bool,
}

fn is_false(value: &bool) -> bool {
  !*value
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub(crate) struct StopPartRecord {
  pub(crate) node_path: String,
  pub(crate) attempting: bool,
  pub(crate) status: Option<OutcomeStatus>,
  pub(crate) reason: Option<String>,
}

#[derive(Clone, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct ControlBody {
  version: String,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  robot_id: Option<String>,
  robot_inhibited: bool,
  robot_targets: Vec<String>,
  estop_latched: bool,
  shutdown_pending: bool,
  shutdown_targets: Vec<String>,
}

/// Preserve the exact 1.0.0 serialization used to calculate its checksum.
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct LegacyControlBody {
  version: String,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  robot_id: Option<String>,
  robot_inhibited: bool,
  robot_targets: Vec<String>,
  estop_latched: bool,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct LegacyControlRecord {
  body: LegacyControlBody,
  checksum: u64,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct ControlRecord {
  body: ControlBody,
  checksum: u64,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct RecordBody {
  version: String,
  command_id: String,
  node_path: String,
  signature: String,
  phase: CommandPhase,
  result: DispatchResult,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  operation: Option<GatewayOperation>,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Record {
  body: RecordBody,
  checksum: u64,
}

/// The held lock excludes another runtime from using the same journal directory.
/// Dropping Journal releases it; the lock file itself is intentionally retained.
pub(crate) struct Journal {
  entries: HashMap<String, JournalEntry>,
  control: ControlBody,
  directory: Option<PathBuf>,
  _lock: Option<File>,
  record_limit: usize,
  #[cfg(test)]
  record_write_failures: Mutex<HashMap<String, usize>>,
}

impl Journal {
  #[cfg(any(test, feature = "test-hooks"))]
  pub(crate) fn set_record_limit_for_test(&mut self, limit: usize) {
    self.record_limit = limit;
  }
  #[cfg(test)]
  pub(crate) fn fail_next_record_writes_for_test(&self, id: &str, count: usize) {
    self
      .record_write_failures
      .lock()
      .expect("journal fault hook poisoned")
      .insert(id.into(), count);
  }
  /// A detached bounded stop worker retains this lock across Dispatcher drop.
  /// Another owner cannot reopen the same journal while its physical stop is
  /// still unresolved in this process.
  pub(crate) fn retain_owner_lock(&self) -> io::Result<Option<File>> {
    self._lock.as_ref().map(File::try_clone).transpose()
  }
  pub(crate) fn has_legacy_records(&self) -> bool {
    self.entries.values().any(|entry| entry.operation.is_none())
  }
  pub(crate) fn has_uncertain_movement(&self) -> bool {
    self.entries.values().any(|entry| {
      entry.result.status == OutcomeStatus::OutcomeUnknown
        && entry.operation.as_ref().is_none_or(|operation| {
          operation.identity.kind == OperationKind::Movement && !operation.fenced
        })
    })
  }

  pub(crate) fn unresolved_movement_targets(&self) -> Vec<String> {
    let mut targets: Vec<_> = self
      .entries
      .values()
      .filter(|entry| {
        entry.result.status == OutcomeStatus::OutcomeUnknown
          && entry
            .operation
            .as_ref()
            .is_none_or(|op| op.identity.kind == OperationKind::Movement && !op.fenced)
      })
      .map(|entry| entry.node_path.clone())
      .collect();
    targets.sort();
    targets.dedup();
    targets
  }

  pub(crate) fn memory() -> Self {
    Self {
      entries: HashMap::new(),
      control: ControlBody {
        version: CONTROL_SCHEMA_VERSION.into(),
        ..ControlBody::default()
      },
      directory: None,
      _lock: None,
      record_limit: MAX_RECORD_BYTES,
      #[cfg(test)]
      record_write_failures: Mutex::new(HashMap::new()),
    }
  }

  pub(crate) fn open(directory: impl AsRef<Path>) -> io::Result<Self> {
    let directory = directory.as_ref().to_path_buf();
    fs::create_dir_all(&directory)?;
    let lock = OpenOptions::new()
      .read(true)
      .write(true)
      .create(true)
      .truncate(false)
      .open(directory.join(".lock"))?;
    lock.try_lock_exclusive()?;
    let control_path = directory.join(".control");
    let control_exists = control_path.exists();
    let control = if control_exists {
      let bytes = read_bounded(&control_path, MAX_RECORD_BYTES)?;
      decode_control(&bytes)?
    } else {
      ControlBody {
        version: CONTROL_SCHEMA_VERSION.into(),
        ..ControlBody::default()
      }
    };
    let mut journal = Self {
      entries: HashMap::new(),
      control,
      directory: Some(directory.clone()),
      _lock: Some(lock),
      record_limit: MAX_RECORD_BYTES,
      #[cfg(test)]
      record_write_failures: Mutex::new(HashMap::new()),
    };
    for item in fs::read_dir(&directory)? {
      let item = item?;
      let path = item.path();
      if path.extension().is_none_or(|ext| ext != "rec") {
        continue;
      }
      if journal.entries.len() >= MAX_RECORDS {
        return Err(invalid_data("journal record count exceeds limit"));
      }
      let bytes = read_bounded(&path, MAX_RECORD_BYTES)?;
      let (id, mut entry) = decode(&bytes)?;
      if record_path(&directory, &id)? != path {
        return Err(invalid_data("journal filename disagrees with command ID"));
      }
      if journal.entries.contains_key(&id) {
        return Err(invalid_data("duplicate journal command ID"));
      }
      if control_exists && entry.phase == CommandPhase::Releasing {
        if let Some(operation) = &entry.operation {
          match operation.identity.kind {
            OperationKind::RobotStop if !journal.control.robot_inhibited => {
              let targets = operation
                .parts
                .iter()
                .map(|part| part.node_path.clone())
                .collect();
              journal.set_robot_inhibit(targets)?;
            }
            OperationKind::MotionStop if !journal.control.robot_inhibited => {
              journal.set_robot_inhibit(vec![entry.node_path.clone()])?;
            }
            OperationKind::SafetyEstop if !journal.control.estop_latched => {
              journal.latch_estop()?;
            }
            _ => {}
          }
        }
      }
      if entry.phase != CommandPhase::Terminal {
        let accepted = entry.phase == CommandPhase::Releasing;
        entry.phase = CommandPhase::Terminal;
        if let Some(operation) = &mut entry.operation {
          operation.accepted = Some(accepted);
          if accepted && operation.identity.kind == OperationKind::RobotStop {
            for part in &mut operation.parts {
              if part.status.is_none() {
                part.status = Some(OutcomeStatus::OutcomeUnknown);
              }
            }
          }
        }
        entry.result = DispatchResult {
          command_id: id.clone(),
          status: if accepted {
            OutcomeStatus::OutcomeUnknown
          } else {
            OutcomeStatus::Cancelled
          },
          reason: Some(
            if accepted {
              "physical outcome unconfirmed after runtime restart"
            } else {
              "prepared command sealed before physical release"
            }
            .into(),
          ),
        };
        journal.write_entry(&id, &entry)?;
      }
      journal.entries.insert(id, entry);
    }
    if !control_exists
      && journal.entries.values().any(|entry| {
        entry.operation.as_ref().is_some_and(|op| {
          matches!(
            op.identity.kind,
            OperationKind::RobotStop | OperationKind::SafetyEstop
          ) && op.accepted == Some(true)
        })
      })
    {
      return Err(invalid_data("released stop lacks durable control state"));
    }
    Ok(journal)
  }

  pub(crate) fn robot_inhibited(&self) -> bool {
    self.control.robot_inhibited
  }

  pub(crate) fn shutdown_pending(&self) -> bool {
    self.control.shutdown_pending
  }

  pub(crate) fn has_unresolved_movement(&self) -> bool {
    self.entries.values().any(|entry| {
      if entry.phase != CommandPhase::Terminal {
        return entry
          .operation
          .as_ref()
          .is_none_or(|operation| operation.identity.kind == OperationKind::Movement);
      }
      entry.result.status == OutcomeStatus::OutcomeUnknown
        && entry.operation.as_ref().is_none_or(|operation| {
          operation.identity.kind == OperationKind::Movement && !operation.fenced
        })
    })
  }

  pub(crate) fn public_stops_settled(&self) -> bool {
    self.entries.values().all(|entry| {
      entry.operation.as_ref().is_none_or(|operation| {
        operation.identity.kind == OperationKind::Movement
          || (entry.phase == CommandPhase::Terminal
            && (operation.identity.kind != OperationKind::SafetyEstop
              || (operation.accepted == Some(true) && self.control.estop_latched)))
      })
    })
  }

  /// Bind the durable control state and every whole-robot operation to the
  /// admitted profile before exposing a Dispatcher. An old active control
  /// record without any robot-target proof cannot be rebound by guessing.
  pub(crate) fn bind_robot_id(&mut self, expected: &str) -> io::Result<()> {
    if !valid_segment(expected) {
      return Err(invalid_input("invalid admitted robot ID"));
    }
    let mut has_robot_target = false;
    for entry in self.entries.values() {
      if let Some(JournalTarget::Robot { robot_id }) = entry
        .operation
        .as_ref()
        .map(|operation| &operation.identity.target)
      {
        has_robot_target = true;
        if robot_id != expected {
          return Err(invalid_data(
            "journal robot target differs from admitted profile",
          ));
        }
      }
    }
    if let Some(stored) = &self.control.robot_id {
      if stored != expected {
        return Err(invalid_data(
          "journal control robot differs from admitted profile",
        ));
      }
      return Ok(());
    }
    if (self.control.robot_inhibited || self.control.estop_latched || self.control.shutdown_pending)
      && !has_robot_target
      && self.control.robot_id.is_none()
    {
      return Err(invalid_data(
        "active journal control has no robot identity proof",
      ));
    }
    let mut next = self.control.clone();
    next.robot_id = Some(expected.into());
    validate_control(&next)?;
    self.write_control(&next)?;
    self.control = next;
    Ok(())
  }
  pub(crate) fn estop_latched(&self) -> bool {
    self.control.estop_latched
  }
  pub(crate) fn robot_targets(&self) -> &[String] {
    &self.control.robot_targets
  }

  /// Verify a pending shutdown belongs to this admitted profile. The marker
  /// is never adapted to a changed selected graph on reopen.
  pub(crate) fn bind_shutdown_targets(&self, admitted: &[String]) -> io::Result<()> {
    if self.control.shutdown_pending
      && self
        .control
        .shutdown_targets
        .iter()
        .any(|target| !admitted.contains(target))
    {
      return Err(invalid_data(
        "journal shutdown target differs from admitted profile",
      ));
    }
    Ok(())
  }

  pub(crate) fn set_shutdown_pending(&mut self, targets: Vec<String>) -> io::Result<()> {
    if self.control.shutdown_pending {
      return Err(invalid_data("journal shutdown is already pending"));
    }
    let mut next = self.control.clone();
    next.version = CONTROL_SCHEMA_VERSION.into();
    next.shutdown_pending = true;
    next.shutdown_targets = targets;
    validate_control(&next)?;
    self.write_control(&next)?;
    self.control = next;
    Ok(())
  }

  pub(crate) fn resolve_shutdown_pending(&mut self) -> io::Result<()> {
    if !self.control.shutdown_pending || !self.public_stops_settled() {
      return Err(invalid_data("journal shutdown cannot be resolved"));
    }
    // A selected dual-part, newer-epoch physical proof precedes this call.
    // Preserve every old recovery target while fencing old uncertain releases.
    let mut targets = self.control.robot_targets.clone();
    let unresolved = self.unresolved_movement_targets();
    if unresolved
      .iter()
      .any(|target| !self.control.shutdown_targets.contains(target))
    {
      return Err(invalid_data(
        "old movement target lacks selected physical stop proof",
      ));
    }
    targets.extend(unresolved);
    targets.sort();
    targets.dedup();
    if targets != self.control.robot_targets {
      self.set_robot_inhibit(targets)?;
    }
    self.fence_uncertain_movements()?;
    if self.has_unresolved_movement() {
      return Err(invalid_data("journal movement remains unresolved"));
    }
    let mut next = self.control.clone();
    next.version = CONTROL_SCHEMA_VERSION.into();
    next.shutdown_pending = false;
    next.shutdown_targets.clear();
    validate_control(&next)?;
    self.write_control(&next)?;
    self.control = next;
    Ok(())
  }

  pub(crate) fn set_robot_inhibit(&mut self, targets: Vec<String>) -> io::Result<()> {
    let mut next = self.control.clone();
    next.robot_inhibited = true;
    next.robot_targets = targets;
    validate_control(&next)?;
    self.write_control(&next)?;
    self.control = next;
    Ok(())
  }

  pub(crate) fn clear_robot_inhibit(&mut self) -> io::Result<()> {
    let mut next = self.control.clone();
    next.robot_inhibited = false;
    next.robot_targets.clear();
    self.write_control(&next)?;
    self.control = next;
    Ok(())
  }

  pub(crate) fn latch_estop(&mut self) -> io::Result<()> {
    let mut next = self.control.clone();
    next.estop_latched = true;
    self.write_control(&next)?;
    self.control = next;
    Ok(())
  }

  pub(crate) fn fence_uncertain_movements(&mut self) -> io::Result<()> {
    let ids: Vec<_> = self
      .entries
      .iter()
      .filter(|(_, entry)| {
        entry.result.status == OutcomeStatus::OutcomeUnknown
          && entry
            .operation
            .as_ref()
            .is_some_and(|op| op.identity.kind == OperationKind::Movement && !op.fenced)
      })
      .map(|(id, _)| id.clone())
      .collect();
    for id in ids {
      self.update(&id, |entry| {
        if let Some(operation) = &mut entry.operation {
          operation.fenced = true;
        }
      })?;
    }
    Ok(())
  }

  fn write_control(&self, body: &ControlBody) -> io::Result<()> {
    let Some(directory) = &self.directory else {
      return Ok(());
    };
    let mut body = body.clone();
    body.version = CONTROL_SCHEMA_VERSION.into();
    validate_control(&body)?;
    let checksum = checksum(&serde_json::to_vec(&body)?);
    let bytes = serde_json::to_vec(&ControlRecord { body, checksum })?;
    write_atomic_bounded(directory.join(".control"), &bytes, MAX_RECORD_BYTES)
  }

  pub(crate) fn get(&self, id: &str) -> Option<&JournalEntry> {
    self.entries.get(id)
  }

  pub(crate) fn reserved_ids(&self) -> impl Iterator<Item = &String> {
    self.entries.keys()
  }

  pub(crate) fn insert(&mut self, id: String, entry: JournalEntry) -> io::Result<()> {
    validate_entry(&id, &entry)?;
    if self.entries.contains_key(&id) {
      return Err(io::Error::new(
        ErrorKind::AlreadyExists,
        "command ID reserved",
      ));
    }
    if self.entries.len() >= MAX_RECORDS {
      return Err(io::Error::new(
        ErrorKind::OutOfMemory,
        "journal record limit reached",
      ));
    }
    if entry.phase != CommandPhase::Prepared {
      return Err(invalid_input("new reservation must be Prepared"));
    }
    self.write_entry(&id, &entry)?;
    self.entries.insert(id, entry);
    Ok(())
  }

  /// A full browser lane rejects a new command in one durable write. It must
  /// never pass through Prepared, which recovery would interpret as cancelled.
  pub(crate) fn insert_terminal_rejection(
    &mut self,
    id: String,
    entry: JournalEntry,
  ) -> io::Result<()> {
    validate_entry(&id, &entry)?;
    if entry.phase != CommandPhase::Terminal
      || entry.result.status != OutcomeStatus::Rejected
      || entry.operation.as_ref().and_then(|op| op.accepted) != Some(false)
    {
      return Err(invalid_input(
        "atomic rejection must be terminal and unaccepted",
      ));
    }
    if self.entries.contains_key(&id) {
      return Err(io::Error::new(
        ErrorKind::AlreadyExists,
        "command ID reserved",
      ));
    }
    if self.entries.len() >= MAX_RECORDS {
      return Err(io::Error::new(
        ErrorKind::OutOfMemory,
        "journal record limit reached",
      ));
    }
    self.write_entry(&id, &entry)?;
    self.entries.insert(id, entry);
    Ok(())
  }

  pub(crate) fn update(
    &mut self,
    id: &str,
    change: impl FnOnce(&mut JournalEntry),
  ) -> io::Result<()> {
    let current = self
      .entries
      .get(id)
      .ok_or_else(|| io::Error::new(ErrorKind::NotFound, "command ID not reserved"))?;
    let mut next = current.clone();
    change(&mut next);
    validate_entry(id, &next)?;
    if next.node_path != current.node_path
      || next.signature != current.signature
      || next.operation.as_ref().map(|o| &o.identity)
        != current.operation.as_ref().map(|o| &o.identity)
    {
      return Err(invalid_input("reservation identity cannot change"));
    }
    let valid_transition = matches!(
      (current.phase, next.phase),
      (
        CommandPhase::Prepared,
        CommandPhase::Prepared | CommandPhase::Releasing | CommandPhase::Terminal
      ) | (
        CommandPhase::Releasing,
        CommandPhase::Releasing | CommandPhase::Terminal
      ) | (CommandPhase::Terminal, CommandPhase::Terminal)
    );
    if !valid_transition {
      return Err(invalid_input("journal phase cannot move backward"));
    }
    self.write_entry(id, &next)?;
    self.entries.insert(id.into(), next);
    Ok(())
  }

  /// Fallback after a terminal write fails. Durable Prepared and Releasing
  /// records recover as uncertain on restart, so this aligns the live view.
  pub(crate) fn mark_uncertain_in_memory(
    &mut self,
    id: &str,
    reason: String,
  ) -> Option<DispatchResult> {
    let entry = self.entries.get_mut(id)?;
    if entry.phase == CommandPhase::Terminal {
      return None;
    }
    let accepted = entry.phase == CommandPhase::Releasing;
    entry.phase = CommandPhase::Terminal;
    if let Some(operation) = &mut entry.operation {
      operation.accepted = Some(accepted);
    }
    entry.result = DispatchResult {
      command_id: id.into(),
      status: OutcomeStatus::OutcomeUnknown,
      reason: Some(reason),
    };
    Some(entry.result.clone())
  }

  fn write_entry(&self, id: &str, entry: &JournalEntry) -> io::Result<()> {
    #[cfg(test)]
    {
      let mut failures = self
        .record_write_failures
        .lock()
        .expect("journal fault hook poisoned");
      if let Some(remaining) = failures.get_mut(id) {
        if *remaining > 0 {
          *remaining -= 1;
          return Err(io::Error::other("injected journal record write failure"));
        }
      }
    }
    let Some(directory) = &self.directory else {
      return Ok(());
    };
    let body = RecordBody {
      version: if entry
        .operation
        .as_ref()
        .is_some_and(|op| op.identity.kind != OperationKind::Movement)
      {
        STOP_SCHEMA_VERSION
      } else {
        SCHEMA_VERSION
      }
      .into(),
      command_id: id.into(),
      node_path: entry.node_path.clone(),
      signature: entry.signature.clone(),
      phase: entry.phase,
      result: entry.result.clone(),
      operation: entry.operation.clone(),
    };
    let checksum = checksum(&serde_json::to_vec(&body)?);
    let bytes = serde_json::to_vec(&Record { body, checksum })?;
    if bytes.len() > self.record_limit {
      return Err(io::Error::new(
        ErrorKind::OutOfMemory,
        "journal record capacity exceeded",
      ));
    }
    write_atomic_bounded(record_path(directory, id)?, &bytes, self.record_limit)
  }
}

fn validate_entry(id: &str, entry: &JournalEntry) -> io::Result<()> {
  if id.is_empty() || id.len() > MAX_ID_BYTES || entry.result.command_id != id {
    return Err(invalid_input("invalid command ID or result identity"));
  }
  let valid_target = match &entry.operation {
    None => valid_hardware_path(&entry.node_path),
    Some(operation) => {
      if entry.phase == CommandPhase::Terminal && operation.accepted.is_none() {
        return Err(invalid_input("terminal operation lacks admission state"));
      }
      if operation.rejection_code.is_some()
        && (entry.phase != CommandPhase::Terminal
          || entry.result.status != OutcomeStatus::Rejected
          || operation.accepted != Some(false))
      {
        return Err(invalid_input(
          "rejection code disagrees with operation state",
        ));
      }
      let identity = &operation.identity;
      if let Some(context) = &identity.admission {
        if context.run_id.is_empty()
          || context.run_id.len() > 128
          || context
            .active_revision
            .as_ref()
            .is_some_and(|revision| !crate::valid_revision(revision))
          || (identity.kind != OperationKind::SafetyEstop && context.active_revision.is_none())
          || context.binding_path.len() > 512
          || context.primitive != identity.primitive
          || context.primitive_version != identity.version
          || (identity.recovery && identity.kind != OperationKind::RobotStop)
        {
          return Err(invalid_input("invalid managed admission identity"));
        }
      }
      if identity.primitive.is_empty()
        || identity.primitive.len() > 64
        || identity.version != "1.0.0"
      {
        return Err(invalid_input("invalid operation primitive or version"));
      }
      if operation.parts.len() > 64 {
        return Err(invalid_input("stop part count exceeds 64"));
      }
      let mut previous = "";
      for part in &operation.parts {
        if !valid_hardware_path(&part.node_path)
          || part.node_path.len() > 512
          || part.node_path.as_str() <= previous
          || part
            .reason
            .as_ref()
            .is_some_and(|reason| reason.len() > 256)
          || part.status.is_some_and(|status| {
            !matches!(
              status,
              OutcomeStatus::Completed | OutcomeStatus::Failed | OutcomeStatus::OutcomeUnknown
            )
          })
        {
          return Err(invalid_input("invalid stop part record"));
        }
        previous = &part.node_path;
      }
      if !matches!(
        identity.kind,
        OperationKind::RobotStop | OperationKind::SafetyEstop
      ) && !operation.parts.is_empty()
      {
        return Err(invalid_input("non-robot operation contains stop parts"));
      }
      match (&identity.target, identity.kind) {
        (
          JournalTarget::Hardware { node_path },
          OperationKind::Movement | OperationKind::MotionStop,
        ) => node_path == &entry.node_path && valid_hardware_path(node_path),
        (
          JournalTarget::Robot { robot_id },
          OperationKind::RobotStop | OperationKind::SafetyEstop,
        ) => entry.node_path.is_empty() && valid_segment(robot_id),
        _ => false,
      }
    }
  };
  if !valid_target || entry.signature.is_empty() {
    return Err(invalid_input(
      "invalid hardware node path or request signature",
    ));
  }
  Ok(())
}

fn validate_control(control: &ControlBody) -> io::Result<()> {
  if control.version != CONTROL_SCHEMA_VERSION
    || control
      .robot_id
      .as_ref()
      .is_some_and(|id| !valid_segment(id))
    || control.robot_targets.len() > 64
    || (!control.robot_inhibited && !control.robot_targets.is_empty())
    || control.shutdown_targets.len() > 64
    || control.shutdown_pending != !control.shutdown_targets.is_empty()
    || (control.shutdown_pending && control.robot_id.is_none())
  {
    return Err(invalid_data("invalid journal control state"));
  }
  let mut previous = "";
  for target in &control.robot_targets {
    if !valid_hardware_path(target) || target.len() > 512 || target.as_str() <= previous {
      return Err(invalid_data("invalid robot inhibit target"));
    }
    previous = target;
  }
  previous = "";
  for target in &control.shutdown_targets {
    if !valid_hardware_path(target) || target.len() > 512 || target.as_str() <= previous {
      return Err(invalid_data("invalid shutdown target"));
    }
    previous = target;
  }
  Ok(())
}

fn decode_control(bytes: &[u8]) -> io::Result<ControlBody> {
  let value: serde_json::Value =
    serde_json::from_slice(bytes).map_err(|error| invalid_data(error.to_string()))?;
  match value
    .pointer("/body/version")
    .and_then(serde_json::Value::as_str)
  {
    Some(LEGACY_CONTROL_SCHEMA_VERSION) => {
      let record: LegacyControlRecord =
        serde_json::from_slice(bytes).map_err(|error| invalid_data(error.to_string()))?;
      if checksum(&serde_json::to_vec(&record.body)?) != record.checksum {
        return Err(invalid_data("journal control record is invalid"));
      }
      let body = ControlBody {
        version: CONTROL_SCHEMA_VERSION.into(),
        robot_id: record.body.robot_id,
        robot_inhibited: record.body.robot_inhibited,
        robot_targets: record.body.robot_targets,
        estop_latched: record.body.estop_latched,
        shutdown_pending: false,
        shutdown_targets: Vec::new(),
      };
      validate_control(&body)?;
      Ok(body)
    }
    Some(CONTROL_SCHEMA_VERSION) => {
      let record: ControlRecord =
        serde_json::from_slice(bytes).map_err(|error| invalid_data(error.to_string()))?;
      if checksum(&serde_json::to_vec(&record.body)?) != record.checksum {
        return Err(invalid_data("journal control record is invalid"));
      }
      validate_control(&record.body)?;
      Ok(record.body)
    }
    _ => Err(invalid_data("journal control version is unsupported")),
  }
}

fn valid_hardware_path(path: &str) -> bool {
  // The dispatcher registry verifies kind after reservation.
  let segments: Vec<_> = path.split('/').collect();
  (segments.len() == 4
    && ((segments[0] == "eos" && segments[1] == "hardware")
      || (segments[0] == "isos" && segments[1] == "eos_hardware"))
    && valid_segment(segments[2])
    && valid_segment(segments[3]))
    || (segments.len() == 8
      && segments[0] == "cores"
      && ((segments[1] == "eos" && segments[3] == "hardware")
        || (segments[1] == "isos" && segments[3] == "eos_hardware"))
      && segments[2] == "layers"
      && segments[4] == "groups"
      && valid_segment(segments[5])
      && segments[6] == "nodes"
      && valid_segment(segments[7]))
}

fn valid_segment(segment: &str) -> bool {
  !segment.is_empty()
    && segment
      .bytes()
      .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'-')
}

fn record_path(directory: &Path, id: &str) -> io::Result<PathBuf> {
  if id.is_empty() || id.len() > MAX_ID_BYTES {
    return Err(invalid_input("command ID exceeds journal path bound"));
  }
  // Preserve historical filenames while keeping the new 128-byte public
  // identity below common 255-byte filesystem component limits.
  let mut name = if id.len() <= 120 {
    let mut legacy = String::with_capacity(id.len() * 2 + 4);
    for byte in id.bytes() {
      use std::fmt::Write;
      write!(legacy, "{byte:02x}").expect("hex formatting cannot fail");
    }
    legacy
  } else {
    format!("sha256-{:x}", Sha256::digest(id.as_bytes()))
  };
  name.push_str(".rec");
  Ok(directory.join(name))
}

fn decode(bytes: &[u8]) -> io::Result<(String, JournalEntry)> {
  let record: Record =
    serde_json::from_slice(bytes).map_err(|error| invalid_data(error.to_string()))?;
  if record.body.version != SCHEMA_VERSION && record.body.version != STOP_SCHEMA_VERSION {
    return Err(invalid_data("unsupported journal schema version"));
  }
  let expected = checksum(&serde_json::to_vec(&record.body)?);
  if record.checksum != expected {
    return Err(invalid_data("journal checksum mismatch"));
  }
  let id = record.body.command_id;
  let entry = JournalEntry {
    node_path: record.body.node_path,
    signature: record.body.signature,
    phase: record.body.phase,
    result: record.body.result,
    operation: record.body.operation,
  };
  if record.body.version == STOP_SCHEMA_VERSION
    && entry
      .operation
      .as_ref()
      .is_none_or(|op| op.identity.kind == OperationKind::Movement)
  {
    return Err(invalid_data("stop journal version lacks stop operation"));
  }
  validate_entry(&id, &entry).map_err(|error| invalid_data(error.to_string()))?;
  Ok((id, entry))
}

fn checksum(bytes: &[u8]) -> u64 {
  bytes.iter().fold(0xcbf29ce484222325_u64, |hash, byte| {
    (hash ^ u64::from(*byte)).wrapping_mul(0x100000001b3)
  })
}

fn invalid_data(message: impl Into<String>) -> io::Error {
  io::Error::new(ErrorKind::InvalidData, message.into())
}

fn invalid_input(message: impl Into<String>) -> io::Error {
  io::Error::new(ErrorKind::InvalidInput, message.into())
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::sync::atomic::{AtomicU64, Ordering};

  static NEXT_TEST: AtomicU64 = AtomicU64::new(0);

  fn directory() -> PathBuf {
    let path = std::env::temp_dir().join(format!(
      "eos-journal-{}-{}",
      std::process::id(),
      NEXT_TEST.fetch_add(1, Ordering::Relaxed)
    ));
    fs::create_dir(&path).unwrap();
    path
  }

  fn entry(id: &str) -> JournalEntry {
    JournalEntry {
      node_path: "cores/eos/layers/hardware/groups/base/nodes/base".into(),
      signature: "2.0.0:{\"movement\":1}".into(),
      phase: CommandPhase::Prepared,
      result: DispatchResult {
        command_id: id.into(),
        status: OutcomeStatus::InProgress,
        reason: None,
      },
      operation: None,
    }
  }

  fn movement(id: &str) -> JournalEntry {
    let mut record = entry(id);
    record.operation = Some(GatewayOperation {
      identity: GatewayIdentity {
        primitive: "base.velocity".into(),
        version: "1.0.0".into(),
        target: JournalTarget::Hardware {
          node_path: record.node_path.clone(),
        },
        kind: OperationKind::Movement,
        admission: None,
        recovery: false,
      },
      accepted: None,
      rejection_code: None,
      parts: Vec::new(),
      fenced: false,
    });
    record
  }

  #[test]
  fn routine_shutdown_fences_terminal_unknown_but_keeps_its_robot_inhibit() {
    let path = directory();
    let target = entry("old-movement").node_path;
    let mut journal = Journal::open(&path).unwrap();
    journal.bind_robot_id("r").unwrap();
    journal
      .insert("old-movement".into(), movement("old-movement"))
      .unwrap();
    journal
      .update("old-movement", |record| {
        record.phase = CommandPhase::Terminal;
        record.result.status = OutcomeStatus::OutcomeUnknown;
        record.operation.as_mut().unwrap().accepted = Some(true);
      })
      .unwrap();
    journal.set_shutdown_pending(vec![target.clone()]).unwrap();
    assert!(journal.has_unresolved_movement());
    journal.resolve_shutdown_pending().unwrap();
    assert!(!journal.shutdown_pending());
    assert!(journal.robot_inhibited());
    assert_eq!(journal.robot_targets(), &[target]);
    assert!(
      journal
        .get("old-movement")
        .unwrap()
        .operation
        .as_ref()
        .unwrap()
        .fenced
    );
    drop(journal);
    let recovered = Journal::open(&path).unwrap();
    assert!(recovered.robot_inhibited());
    assert!(!recovered.shutdown_pending());
    assert!(
      recovered
        .get("old-movement")
        .unwrap()
        .operation
        .as_ref()
        .unwrap()
        .fenced
    );
    fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn nonterminal_movement_cannot_resolve_routine_shutdown() {
    let mut journal = Journal::memory();
    let target = entry("active-movement").node_path;
    journal.bind_robot_id("r").unwrap();
    journal
      .insert("active-movement".into(), movement("active-movement"))
      .unwrap();
    journal
      .update("active-movement", |record| {
        record.phase = CommandPhase::Releasing
      })
      .unwrap();
    journal.set_shutdown_pending(vec![target]).unwrap();
    assert!(journal.has_unresolved_movement());
    assert!(journal.resolve_shutdown_pending().is_err());
    assert!(journal.shutdown_pending());
  }

  #[test]
  fn changed_profile_cannot_fence_an_old_unselected_movement_target() {
    let mut journal = Journal::memory();
    journal.bind_robot_id("r").unwrap();
    let mut old = movement("old-target");
    old.node_path = "cores/eos/layers/hardware/groups/base/nodes/old".into();
    old.operation.as_mut().unwrap().identity.target = JournalTarget::Hardware {
      node_path: old.node_path.clone(),
    };
    journal.insert("old-target".into(), old).unwrap();
    journal
      .update("old-target", |record| {
        record.phase = CommandPhase::Terminal;
        record.result.status = OutcomeStatus::OutcomeUnknown;
        record.operation.as_mut().unwrap().accepted = Some(true);
      })
      .unwrap();
    journal
      .set_shutdown_pending(vec![
        "cores/eos/layers/hardware/groups/base/nodes/new".into()
      ])
      .unwrap();
    assert!(journal.resolve_shutdown_pending().is_err());
    assert!(journal.shutdown_pending());
    assert!(
      !journal
        .get("old-target")
        .unwrap()
        .operation
        .as_ref()
        .unwrap()
        .fenced
    );
  }

  #[test]
  fn crash_between_stop_clear_and_terminal_record_restores_inhibit() {
    let path = directory();
    let target = "cores/eos/layers/hardware/groups/base/nodes/base".to_string();
    let mut journal = Journal::open(&path).unwrap();
    let mut record = entry("clear-gap");
    record.node_path.clear();
    record.operation = Some(GatewayOperation {
      identity: GatewayIdentity {
        primitive: "robot.stop".into(),
        version: "1.0.0".into(),
        target: JournalTarget::Robot {
          robot_id: "r".into(),
        },
        kind: OperationKind::RobotStop,
        admission: None,
        recovery: false,
      },
      accepted: None,
      rejection_code: None,
      parts: vec![StopPartRecord {
        node_path: target.clone(),
        attempting: true,
        status: Some(OutcomeStatus::Completed),
        reason: None,
      }],
      fenced: false,
    });
    journal.insert("clear-gap".into(), record).unwrap();
    journal.set_robot_inhibit(vec![target.clone()]).unwrap();
    journal
      .update("clear-gap", |entry| entry.phase = CommandPhase::Releasing)
      .unwrap();
    journal.clear_robot_inhibit().unwrap();
    drop(journal);
    let recovered = Journal::open(&path).unwrap();
    assert!(recovered.robot_inhibited());
    assert_eq!(recovered.robot_targets(), &[target]);
    assert_eq!(
      recovered.get("clear-gap").unwrap().result.status,
      OutcomeStatus::OutcomeUnknown
    );
    drop(recovered);
    fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn reservation_is_durable_and_restart_marks_uncertain() {
    let path = directory();
    let mut journal = Journal::open(&path).unwrap();
    journal.insert("cmd-1".into(), entry("cmd-1")).unwrap();
    assert_eq!(journal.open_again_error_kind(&path), ErrorKind::WouldBlock);
    journal
      .update("cmd-1", |entry| entry.phase = CommandPhase::Releasing)
      .unwrap();
    drop(journal);
    let journal = Journal::open(&path).unwrap();
    let recovered = journal.get("cmd-1").unwrap();
    assert_eq!(recovered.phase, CommandPhase::Terminal);
    assert_eq!(recovered.result.status, OutcomeStatus::OutcomeUnknown);
    drop(journal);
    fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn prepared_legacy_record_recovers_cancelled_without_movement_inhibit() {
    let path = directory();
    let mut journal = Journal::open(&path).unwrap();
    journal
      .insert("prepared-v2".into(), entry("prepared-v2"))
      .unwrap();
    drop(journal);
    let journal = Journal::open(&path).unwrap();
    let recovered = journal.get("prepared-v2").unwrap();
    assert_eq!(recovered.phase, CommandPhase::Terminal);
    assert_eq!(recovered.result.status, OutcomeStatus::Cancelled);
    assert!(!journal.has_uncertain_movement());
    drop(journal);
    fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn typed_prepared_and_releasing_records_recover_with_distinct_admission() {
    let path = directory();
    let mut journal = Journal::open(&path).unwrap();
    for id in ["typed-prepared", "typed-releasing"] {
      let mut record = entry(id);
      record.operation = Some(GatewayOperation {
        identity: GatewayIdentity {
          primitive: "base.velocity".into(),
          version: "1.0.0".into(),
          target: JournalTarget::Hardware {
            node_path: record.node_path.clone(),
          },
          kind: OperationKind::Movement,
          admission: None,
          recovery: false,
        },
        accepted: None,
        rejection_code: None,
        parts: Vec::new(),
        fenced: false,
      });
      journal.insert(id.into(), record).unwrap();
    }
    journal
      .update("typed-releasing", |record| {
        record.phase = CommandPhase::Releasing
      })
      .unwrap();
    drop(journal);
    let journal = Journal::open(&path).unwrap();
    let prepared = journal.get("typed-prepared").unwrap();
    assert_eq!(prepared.result.status, OutcomeStatus::Cancelled);
    assert_eq!(prepared.operation.as_ref().unwrap().accepted, Some(false));
    let released = journal.get("typed-releasing").unwrap();
    assert_eq!(released.result.status, OutcomeStatus::OutcomeUnknown);
    assert_eq!(released.operation.as_ref().unwrap().accepted, Some(true));
    assert!(journal.has_uncertain_movement());
    drop(journal);
    fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn package_group_path_reserves_and_recovers() {
    let path = directory();
    let mut journal = Journal::open(&path).unwrap();
    let mut command = entry("mujoco-drive");
    command.node_path = "cores/eos/layers/hardware/groups/mujoco/nodes/drive".into();
    journal.insert("mujoco-drive".into(), command).unwrap();
    drop(journal);
    let recovered = Journal::open(&path).unwrap();
    assert_eq!(
      recovered.get("mujoco-drive").unwrap().node_path,
      "cores/eos/layers/hardware/groups/mujoco/nodes/drive"
    );
    drop(recovered);
    fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn public_operation_ids_allow_128_bytes_but_not_129() {
    let path = directory();
    let mut journal = Journal::open(&path).unwrap();
    let valid = "a".repeat(128);
    journal.insert(valid.clone(), entry(&valid)).unwrap();
    let invalid = "b".repeat(129);
    assert_eq!(
      journal
        .insert(invalid.clone(), entry(&invalid))
        .unwrap_err()
        .kind(),
      ErrorKind::InvalidInput
    );
    drop(journal);
    let recovered = Journal::open(&path).unwrap();
    assert!(recovered.get(&valid).is_some());
    drop(recovered);
    fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn terminal_result_survives_restart_and_conflict_is_rejected() {
    let path = directory();
    let mut journal = Journal::open(&path).unwrap();
    journal.insert("cmd-2".into(), entry("cmd-2")).unwrap();
    journal
      .update("cmd-2", |entry| {
        entry.phase = CommandPhase::Terminal;
        entry.result.status = OutcomeStatus::Completed;
      })
      .unwrap();
    drop(journal);
    let mut journal = Journal::open(&path).unwrap();
    assert_eq!(
      journal.get("cmd-2").unwrap().result.status,
      OutcomeStatus::Completed
    );
    assert_eq!(
      journal
        .insert("cmd-2".into(), entry("cmd-2"))
        .unwrap_err()
        .kind(),
      ErrorKind::AlreadyExists
    );
    assert!(journal
      .update("cmd-2", |entry| entry.signature = "different".into())
      .is_err());
    drop(journal);
    fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn corruption_and_oversize_block_open() {
    let path = directory();
    let mut journal = Journal::open(&path).unwrap();
    journal.insert("cmd-3".into(), entry("cmd-3")).unwrap();
    drop(journal);
    let file = record_path(&path, "cmd-3").unwrap();
    fs::write(&file, b"{\"broken\":true}").unwrap();
    assert_eq!(
      Journal::open(&path).err().unwrap().kind(),
      ErrorKind::InvalidData
    );
    fs::write(&file, vec![b'x'; MAX_RECORD_BYTES + 1]).unwrap();
    assert_eq!(
      Journal::open(&path).err().unwrap().kind(),
      ErrorKind::InvalidData
    );
    fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn failed_update_does_not_change_memory() {
    let mut journal = Journal::memory();
    journal.insert("cmd-4".into(), entry("cmd-4")).unwrap();
    assert!(journal
      .update("cmd-4", |entry| entry.node_path =
        "cores/eos/layers/hardware/groups/base/nodes/other".into())
      .is_err());
    assert_eq!(
      journal.get("cmd-4").unwrap().node_path,
      "cores/eos/layers/hardware/groups/base/nodes/base"
    );
    assert!(journal
      .update("cmd-4", |entry| entry.phase = CommandPhase::Terminal)
      .is_ok());
    assert!(journal
      .update("cmd-4", |entry| entry.phase = CommandPhase::Prepared)
      .is_err());
  }

  #[test]
  fn failed_persistence_does_not_advance_phase() {
    let path = directory();
    let mut journal = Journal::open(&path).unwrap();
    journal.insert("cmd-5".into(), entry("cmd-5")).unwrap();
    let file = record_path(&path, "cmd-5").unwrap();
    fs::remove_file(&file).unwrap();
    fs::create_dir(&file).unwrap();
    assert!(journal
      .update("cmd-5", |entry| entry.phase = CommandPhase::Releasing)
      .is_err());
    assert_eq!(journal.get("cmd-5").unwrap().phase, CommandPhase::Prepared);
    drop(journal);
    fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn post_release_failure_falls_back_to_uncertain() {
    let path = directory();
    let mut journal = Journal::open(&path).unwrap();
    journal.insert("cmd-6".into(), entry("cmd-6")).unwrap();
    journal
      .update("cmd-6", |entry| entry.phase = CommandPhase::Releasing)
      .unwrap();
    let file = record_path(&path, "cmd-6").unwrap();
    fs::remove_file(&file).unwrap();
    fs::create_dir(&file).unwrap();
    assert!(journal
      .update("cmd-6", |entry| {
        entry.phase = CommandPhase::Terminal;
        entry.result.status = OutcomeStatus::Completed;
      })
      .is_err());
    let fallback = journal
      .mark_uncertain_in_memory("cmd-6", "terminal journal write failed".into())
      .unwrap();
    assert_eq!(fallback.status, OutcomeStatus::OutcomeUnknown);
    assert_eq!(journal.get("cmd-6").unwrap().phase, CommandPhase::Terminal);
    drop(journal);
    fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn failed_pre_release_terminal_write_is_uncertain_in_memory() {
    let mut journal = Journal::memory();
    journal.insert("cmd-7".into(), entry("cmd-7")).unwrap();
    assert_eq!(
      journal
        .mark_uncertain_in_memory("cmd-7", "terminal write failed".into())
        .unwrap()
        .status,
      OutcomeStatus::OutcomeUnknown
    );
  }

  #[test]
  fn admitted_robot_binding_rejects_mismatched_stored_target() {
    let mut journal = Journal::memory();
    journal.bind_robot_id("r").unwrap();
    let mut record = entry("other-robot");
    record.node_path.clear();
    record.operation = Some(GatewayOperation {
      identity: GatewayIdentity {
        primitive: "robot.stop".into(),
        version: "1.0.0".into(),
        target: JournalTarget::Robot {
          robot_id: "other".into(),
        },
        kind: OperationKind::RobotStop,
        admission: None,
        recovery: false,
      },
      accepted: None,
      rejection_code: None,
      parts: Vec::new(),
      fenced: false,
    });
    journal.insert("other-robot".into(), record).unwrap();
    assert_eq!(
      journal.bind_robot_id("r").unwrap_err().kind(),
      ErrorKind::InvalidData
    );
  }

  #[test]
  fn active_unbound_legacy_control_has_no_rebind_proof() {
    let mut journal = Journal::memory();
    journal
      .set_robot_inhibit(vec![
        "cores/eos/layers/hardware/groups/base/nodes/base".into()
      ])
      .unwrap();
    assert_eq!(
      journal.bind_robot_id("r").unwrap_err().kind(),
      ErrorKind::InvalidData
    );
  }

  #[test]
  fn legacy_control_checksum_is_verified_before_upgrade_without_read_write() {
    let path = directory();
    fs::create_dir_all(&path).unwrap();
    let old = LegacyControlBody {
      version: LEGACY_CONTROL_SCHEMA_VERSION.into(),
      robot_id: Some("r".into()),
      robot_inhibited: false,
      robot_targets: Vec::new(),
      estop_latched: true,
    };
    let bytes = serde_json::to_vec(&LegacyControlRecord {
      checksum: checksum(&serde_json::to_vec(&old).unwrap()),
      body: old,
    })
    .unwrap();
    fs::write(path.join(".control"), &bytes).unwrap();
    let mut journal = Journal::open(&path).unwrap();
    journal.bind_robot_id("r").unwrap();
    assert_eq!(fs::read(path.join(".control")).unwrap(), bytes);
    assert!(journal.estop_latched());
    journal
      .set_robot_inhibit(vec!["eos/hardware/base/drive".into()])
      .unwrap();
    let upgraded: ControlRecord =
      serde_json::from_slice(&fs::read(path.join(".control")).unwrap()).unwrap();
    assert_eq!(upgraded.body.version, CONTROL_SCHEMA_VERSION);
    assert!(upgraded.body.estop_latched && upgraded.body.robot_inhibited);
    assert!(!upgraded.body.shutdown_pending);
    drop(journal);
    let mut malformed: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
    malformed["body"]["shutdown_pending"] = serde_json::json!(false);
    fs::write(
      path.join(".control"),
      serde_json::to_vec(&malformed).unwrap(),
    )
    .unwrap();
    assert_eq!(
      Journal::open(&path).err().unwrap().kind(),
      ErrorKind::InvalidData
    );
    fs::write(path.join(".control"), &bytes).unwrap();
    malformed = serde_json::from_slice(&bytes).unwrap();
    malformed["checksum"] = serde_json::json!(0);
    fs::write(
      path.join(".control"),
      serde_json::to_vec(&malformed).unwrap(),
    )
    .unwrap();
    assert_eq!(
      Journal::open(&path).err().unwrap().kind(),
      ErrorKind::InvalidData
    );
    fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn shutdown_marker_is_profile_bound_and_resolves_without_clearing_other_safety() {
    let path = directory();
    let target = "eos/hardware/base/drive".to_owned();
    let mut journal = Journal::open(&path).unwrap();
    journal.bind_robot_id("r").unwrap();
    journal.set_robot_inhibit(vec![target.clone()]).unwrap();
    journal.latch_estop().unwrap();
    journal.set_shutdown_pending(vec![target.clone()]).unwrap();
    assert!(journal.set_shutdown_pending(vec![target.clone()]).is_err());
    drop(journal);
    let mut recovered = Journal::open(&path).unwrap();
    recovered.bind_robot_id("r").unwrap();
    assert!(recovered.shutdown_pending());
    assert!(recovered
      .bind_shutdown_targets(&["eos/hardware/base/other".into()])
      .is_err());
    recovered
      .bind_shutdown_targets(std::slice::from_ref(&target))
      .unwrap();
    recovered.resolve_shutdown_pending().unwrap();
    assert!(!recovered.shutdown_pending());
    assert!(recovered.robot_inhibited() && recovered.estop_latched());
    drop(recovered);
    fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn unified_eos_hardware_shutdown_target_is_durable() {
    let path = directory();
    let target = "isos/eos_hardware/base/drive".to_owned();
    let mut journal = Journal::open(&path).unwrap();
    journal.bind_robot_id("r").unwrap();
    journal.set_shutdown_pending(vec![target.clone()]).unwrap();
    drop(journal);
    let recovered = Journal::open(&path).unwrap();
    recovered.bind_shutdown_targets(&[target]).unwrap();
    fs::remove_dir_all(path).unwrap();
  }

  #[test]
  fn invalid_shutdown_control_never_opens() {
    let path = directory();
    fs::create_dir_all(&path).unwrap();
    let cases = [
      (true, vec![], Some("r")),
      (false, vec!["eos/hardware/base/drive"], Some("r")),
      (true, vec!["eos/hardware/base/drive"], None),
      (
        true,
        vec!["eos/hardware/base/z", "eos/hardware/base/a"],
        Some("r"),
      ),
    ];
    for (pending, targets, robot) in cases {
      let body = ControlBody {
        version: CONTROL_SCHEMA_VERSION.into(),
        robot_id: robot.map(str::to_owned),
        robot_inhibited: false,
        robot_targets: Vec::new(),
        estop_latched: false,
        shutdown_pending: pending,
        shutdown_targets: targets.into_iter().map(str::to_owned).collect(),
      };
      let bytes = serde_json::to_vec(&ControlRecord {
        checksum: checksum(&serde_json::to_vec(&body).unwrap()),
        body,
      })
      .unwrap();
      fs::write(path.join(".control"), bytes).unwrap();
      assert_eq!(
        Journal::open(&path).err().unwrap().kind(),
        ErrorKind::InvalidData
      );
    }
    fs::remove_dir_all(path).unwrap();
  }

  impl Journal {
    fn open_again_error_kind(&self, path: &Path) -> ErrorKind {
      Journal::open(path).err().unwrap().kind()
    }
  }
}
