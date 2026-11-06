//! Run-bound stop intent and durable owner-confirmed terminal outcomes.

use crate::app::config::Paths;
use anyhow::{Result, ensure};
use serde::{Deserialize, Serialize};
use std::{
  collections::BTreeSet,
  fs::{self, OpenOptions},
  io::{Read, Write},
  os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt},
  path::{Path, PathBuf},
  sync::atomic::{AtomicU64, Ordering},
};

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
pub struct ChildOutcome {
  pub id: String,
  pub outcome: &'static str,
  pub reason_code: Option<&'static str>,
}

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
pub struct Terminal {
  pub run_id: String,
  pub outcome: &'static str,
  pub reason_code: Option<&'static str>,
  #[serde(skip_serializing_if = "Option::is_none")]
  pub recovery_method: Option<&'static str>,
  pub processes: Vec<ChildOutcome>,
}

impl Terminal {
  pub fn unknown(run_id: String, reason: &'static str, ids: &[String]) -> Self {
    Self {
      run_id,
      outcome: "unknown",
      reason_code: Some(reason),
      recovery_method: None,
      processes: ids
        .iter()
        .map(|id| ChildOutcome {
          id: id.clone(),
          outcome: "unknown",
          reason_code: Some(reason),
        })
        .collect(),
    }
  }
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Record {
  schema_version: u32,
  deployment: String,
  run_id: String,
  phase: String,
  outcome: Option<String>,
  selected_processes: Vec<String>,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  recovery_method: Option<String>,
}

fn token(value: &str, max: usize) -> bool {
  !value.is_empty()
    && value.len() <= max
    && value
      .bytes()
      .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
}

fn private_dir(path: &Path) -> Result<()> {
  if !path.exists() {
    fs::create_dir(path)?;
    fs::set_permissions(path, fs::Permissions::from_mode(0o700))?;
  }
  let meta = fs::symlink_metadata(path)?;
  ensure!(
    meta.file_type().is_dir() && meta.uid() == unsafe { libc::geteuid() },
    "unsafe stop state directory"
  );
  if meta.permissions().mode() & 0o7777 != 0o700 {
    fs::set_permissions(path, fs::Permissions::from_mode(0o700))?;
  }
  Ok(())
}

fn record_path(paths: &Paths, deployment: &str, run_id: &str) -> Result<PathBuf> {
  ensure!(
    token(deployment, 48) && token(run_id, 64),
    "invalid stop identity"
  );
  Ok(
    paths
      .data
      .join("stops")
      .join(deployment)
      .join(format!("{run_id}.json")),
  )
}

fn checked_parent(path: &Path) -> Result<()> {
  let root = path
    .parent()
    .and_then(Path::parent)
    .expect("validated stop path");
  private_dir(root.parent().expect("stop data root"))?;
  private_dir(root)?;
  private_dir(path.parent().expect("stop deployment root"))?;
  Ok(())
}

fn validate(record: &Record, deployment: &str, run_id: &str) -> Result<()> {
  ensure!(
    record.schema_version == 1 && record.deployment == deployment && record.run_id == run_id,
    "invalid stop record identity"
  );
  ensure!(
    matches!(
      (record.phase.as_str(), record.outcome.as_deref()),
      ("running", None)
        | ("stopping", Some("unknown"))
        | ("confirmed", Some("confirmed"))
        | ("recovered", Some("confirmed"))
    ),
    "invalid stop record phase"
  );
  ensure!(
    (record.phase == "recovered"
      && record.recovery_method.as_deref() == Some("operator_attestation"))
      || (record.phase != "recovered" && record.recovery_method.is_none()),
    "invalid stop recovery method"
  );
  let mut seen = BTreeSet::new();
  ensure!(
    record.selected_processes.len() <= 64
      && record
        .selected_processes
        .iter()
        .all(|id| token(id, 48) && seen.insert(id)),
    "invalid stop process selection"
  );
  Ok(())
}

fn read(path: &Path, deployment: &str, run_id: &str) -> Result<Record> {
  let meta = fs::symlink_metadata(path)?;
  ensure!(
    meta.file_type().is_file()
      && meta.uid() == unsafe { libc::geteuid() }
      && meta.nlink() == 1
      && meta.permissions().mode() & 0o7777 == 0o600
      && meta.len() <= 8192,
    "unsafe stop record"
  );
  let file = OpenOptions::new()
    .read(true)
    .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
    .open(path)?;
  let opened = file.metadata()?;
  ensure!(
    meta.ino() == opened.ino() && meta.dev() == opened.dev(),
    "stop record changed"
  );
  let mut bytes = Vec::new();
  file.take(8193).read_to_end(&mut bytes)?;
  ensure!(bytes.len() <= 8192, "stop record too large");
  let record: Record = serde_json::from_slice(&bytes)?;
  validate(&record, deployment, run_id)?;
  Ok(record)
}

fn write(path: &Path, record: &Record, create: bool) -> Result<()> {
  checked_parent(path)?;
  if !create {
    let old = read(path, &record.deployment, &record.run_id)?;
    ensure!(
      matches!(
        (old.phase.as_str(), record.phase.as_str()),
        ("running", "stopping")
          | ("stopping", "confirmed")
          | ("running", "recovered")
          | ("stopping", "recovered")
      ) && old.selected_processes == record.selected_processes,
      "invalid stop transition"
    );
  }
  static NEXT: AtomicU64 = AtomicU64::new(1);
  let sequence = NEXT.fetch_add(1, Ordering::Relaxed);
  let temporary = path.with_extension(format!("{}.{}.tmp", std::process::id(), sequence));
  let mut file = OpenOptions::new()
    .write(true)
    .create_new(true)
    .mode(0o600)
    .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
    .open(&temporary)?;
  let result = (|| -> Result<()> {
    file.write_all(&serde_json::to_vec(record)?)?;
    file.sync_all()?;
    if create {
      ensure!(
        fs::symlink_metadata(path).is_err_and(|error| error.kind() == std::io::ErrorKind::NotFound),
        "stop record already exists"
      );
    }
    fs::rename(&temporary, path)?;
    OpenOptions::new()
      .read(true)
      .open(path.parent().unwrap())?
      .sync_all()?;
    Ok(())
  })();
  if result.is_err() {
    let _ = fs::remove_file(&temporary);
  }
  result
}

pub fn create_running(paths: &Paths, deployment: &str, run_id: &str, ids: &[String]) -> Result<()> {
  let path = record_path(paths, deployment, run_id)?;
  let record = Record {
    schema_version: 1,
    deployment: deployment.into(),
    run_id: run_id.into(),
    phase: "running".into(),
    outcome: None,
    selected_processes: ids.to_vec(),
    recovery_method: None,
  };
  validate(&record, deployment, run_id)?;
  write(&path, &record, true)
}

pub fn publish_unknown(
  paths: &Paths,
  deployment: &str,
  run_id: &str,
  ids: &[String],
) -> Result<()> {
  let path = record_path(paths, deployment, run_id)?;
  let record = Record {
    schema_version: 1,
    deployment: deployment.into(),
    run_id: run_id.into(),
    phase: "stopping".into(),
    outcome: Some("unknown".into()),
    selected_processes: ids.to_vec(),
    recovery_method: None,
  };
  validate(&record, deployment, run_id)?;
  write(&path, &record, false)
}

pub fn publish_confirmed(paths: &Paths, deployment: &str, terminal: &Terminal) -> Result<()> {
  ensure!(
    terminal.outcome == "confirmed"
      && terminal.reason_code.is_none()
      && terminal.recovery_method.is_none()
      && terminal
        .processes
        .iter()
        .all(|process| process.outcome == "confirmed" && process.reason_code.is_none()),
    "only a complete live stop may be persisted as confirmed"
  );
  let path = record_path(paths, deployment, &terminal.run_id)?;
  let old = read(&path, deployment, &terminal.run_id)?;
  ensure!(
    old.phase == "stopping"
      && old.selected_processes
        == terminal
          .processes
          .iter()
          .map(|process| process.id.clone())
          .collect::<Vec<_>>(),
    "confirmed stop does not match admitted process selection"
  );
  let record = Record {
    schema_version: 1,
    deployment: deployment.into(),
    run_id: terminal.run_id.clone(),
    phase: "confirmed".into(),
    outcome: Some("confirmed".into()),
    selected_processes: old.selected_processes,
    recovery_method: None,
  };
  write(&path, &record, false)
}

pub fn publish_operator_recovery(paths: &Paths, deployment: &str, run_id: &str) -> Result<()> {
  let path = record_path(paths, deployment, run_id)?;
  let old = read(&path, deployment, run_id)?;
  ensure!(
    matches!(old.phase.as_str(), "running" | "stopping"),
    "run is already finalized"
  );
  let record = Record {
    schema_version: 1,
    deployment: deployment.into(),
    run_id: run_id.into(),
    phase: "recovered".into(),
    outcome: Some("confirmed".into()),
    selected_processes: old.selected_processes,
    recovery_method: Some("operator_attestation".into()),
  };
  write(&path, &record, false)
}

pub fn recover(paths: &Paths, deployment: &str, run_id: &str) -> Terminal {
  match record_path(paths, deployment, run_id).and_then(|path| read(&path, deployment, run_id)) {
    Ok(record) if matches!(record.phase.as_str(), "confirmed" | "recovered") => Terminal {
      run_id: run_id.into(),
      outcome: "confirmed",
      reason_code: None,
      recovery_method: (record.phase == "recovered").then_some("operator_attestation"),
      processes: record
        .selected_processes
        .into_iter()
        .map(|id| ChildOutcome {
          id,
          outcome: "confirmed",
          reason_code: None,
        })
        .collect(),
    },
    Ok(record) => Terminal::unknown(
      run_id.into(),
      "stop_interrupted",
      &record.selected_processes,
    ),
    Err(_) => Terminal::unknown(run_id.into(), "stop_result_persist_failed", &[]),
  }
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct ReceiptPart {
  node_path: String,
  kind: String,
  outcome: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Receipt {
  schema_version: u32,
  #[serde(rename = "type")]
  kind: String,
  run_id: String,
  eos_pid: u32,
  active_revision: String,
  backend_node: String,
  backend_pid: Option<u32>,
  backend_pgid: Option<u32>,
  generation_id: Option<String>,
  stop_epoch: Option<String>,
  admission_closed: bool,
  parts: Vec<ReceiptPart>,
  backend_group_drained: bool,
  backend_reaped: bool,
  sockets_removed: bool,
  outcome: String,
  reason_code: Option<String>,
}

pub struct ReceiptExpectation<'a> {
  pub run_id: &'a str,
  pub eos_pid: u32,
  pub revision: &'a str,
  pub backend_node: &'a str,
  pub parts: &'a [(String, String)],
}

/// A strict selected-pipe check. Duplicate object keys are rejected by the
/// typed deserializer, including nested part keys.
pub fn verify_receipt(line: &[u8], expected: &ReceiptExpectation<'_>) -> Result<bool> {
  ensure!(
    line.len() <= 4096 && line.last() == Some(&b'\n'),
    "invalid EOS receipt framing"
  );
  let receipt: Receipt = serde_json::from_slice(&line[..line.len() - 1])?;
  ensure!(
    receipt.schema_version == 1
      && receipt.kind == "eos_terminal_stop"
      && receipt.run_id == expected.run_id
      && receipt.eos_pid == expected.eos_pid
      && receipt.active_revision == expected.revision
      && receipt.backend_node == expected.backend_node,
    "invalid EOS receipt identity"
  );
  ensure!(
    receipt.backend_pid == receipt.backend_pgid && receipt.backend_pid.is_none_or(|pid| pid > 0),
    "invalid EOS backend identity"
  );
  ensure!(
    receipt.backend_pid.is_some()
      || (receipt.generation_id.is_none() && receipt.outcome == "unknown"),
    "invalid EOS backend rollback identity"
  );
  ensure!(
    receipt
      .generation_id
      .as_deref()
      .is_none_or(|value| value.len() == 32
        && value
          .bytes()
          .all(|b| b.is_ascii_hexdigit() && !b.is_ascii_uppercase())),
    "invalid EOS generation"
  );
  ensure!(
    receipt
      .stop_epoch
      .as_deref()
      .is_none_or(|value| !value.is_empty()
        && (value == "0" || !value.starts_with('0'))
        && value.bytes().all(|b| b.is_ascii_digit())),
    "invalid EOS stop epoch"
  );
  ensure!(
    receipt.parts.len() <= 64 && receipt.parts.len() == expected.parts.len(),
    "invalid EOS receipt part count"
  );
  for (part, (path, kind)) in receipt.parts.iter().zip(expected.parts) {
    ensure!(
      &part.node_path == path
        && &part.kind == kind
        && matches!(part.outcome.as_str(), "stopped" | "unknown"),
      "invalid EOS receipt part"
    );
  }
  ensure!(
    matches!(receipt.outcome.as_str(), "confirmed" | "unknown"),
    "invalid EOS receipt outcome"
  );
  let known_reason = matches!(
    receipt.reason_code.as_deref(),
    Some(
      "stop_timeout"
        | "stop_socket_unavailable"
        | "stop_response_invalid"
        | "stop_part_unknown"
        | "backend_exited"
        | "backend_forced_kill"
        | "backend_group_unreaped"
        | "backend_reap_failed"
        | "socket_cleanup_failed"
    )
  );
  ensure!(
    (receipt.outcome == "confirmed" && receipt.reason_code.is_none())
      || (receipt.outcome == "unknown" && known_reason),
    "invalid EOS receipt reason"
  );
  if receipt.outcome == "confirmed" {
    ensure!(
      receipt.backend_pid.is_some()
        && receipt.generation_id.is_some()
        && receipt.stop_epoch.is_some()
        && receipt.admission_closed
        && receipt.backend_group_drained
        && receipt.backend_reaped
        && receipt.sockets_removed
        && receipt.parts.iter().all(|part| part.outcome == "stopped"),
      "inconsistent confirmed EOS receipt"
    );
  }
  Ok(receipt.outcome == "confirmed")
}

#[cfg(test)]
mod tests {
  use super::*;
  #[test]
  fn durable_state_never_restores_confirmation() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let ids = vec!["egia".into(), "eos".into()];
    create_running(&paths, "test", "r42", &ids).unwrap();
    assert_eq!(
      recover(&paths, "test", "r42").reason_code,
      Some("stop_interrupted")
    );
    publish_unknown(&paths, "test", "r42", &ids).unwrap();
    assert_eq!(recover(&paths, "test", "r42").processes.len(), 2);
    assert!(publish_unknown(&paths, "test", "r42", &ids).is_err());
  }

  #[test]
  fn complete_live_stop_is_durable_and_unknown_stays_unknown() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let ids = vec!["eos".into()];
    create_running(&paths, "test", "r42", &ids).unwrap();
    publish_unknown(&paths, "test", "r42", &ids).unwrap();
    let confirmed = Terminal {
      run_id: "r42".into(),
      outcome: "confirmed",
      reason_code: None,
      recovery_method: None,
      processes: vec![ChildOutcome {
        id: "eos".into(),
        outcome: "confirmed",
        reason_code: None,
      }],
    };
    publish_confirmed(&paths, "test", &confirmed).unwrap();
    assert_eq!(recover(&paths, "test", "r42"), confirmed);
    assert!(publish_unknown(&paths, "test", "r42", &ids).is_err());
    assert!(publish_confirmed(&paths, "test", &confirmed).is_err());

    create_running(&paths, "test", "r43", &ids).unwrap();
    publish_unknown(&paths, "test", "r43", &ids).unwrap();
    let wrong = Terminal {
      run_id: "r43".into(),
      processes: vec![ChildOutcome {
        id: "other".into(),
        ..confirmed.processes[0].clone()
      }],
      ..confirmed
    };
    assert!(publish_confirmed(&paths, "test", &wrong).is_err());
    assert_eq!(recover(&paths, "test", "r43").outcome, "unknown");
    publish_operator_recovery(&paths, "test", "r43").unwrap();
    assert_eq!(recover(&paths, "test", "r43").outcome, "confirmed");
    assert_eq!(
      recover(&paths, "test", "r43").recovery_method,
      Some("operator_attestation")
    );
    assert!(publish_operator_recovery(&paths, "test", "r43").is_err());
  }

  #[test]
  fn selected_receipt_requires_exact_identity_and_facts() {
    let source = include_str!("../../../tests/fixtures/dependent_stop/eos_receipt_confirmed.json");
    let mut line = source.trim_end().as_bytes().to_vec();
    line.push(b'\n');
    let parts = vec![
      ("eos/hardware/base/base-0".into(), "base".into()),
      ("eos/hardware/manipulator/arm-0".into(), "arm".into()),
    ];
    let expectation = ReceiptExpectation {
      run_id: "r42",
      eos_pid: 4201,
      revision: "7a8daee6b7db9105f5483506ffc1743625b63089710dfbf936622329c471016b",
      backend_node: "eos/process/worker/sim",
      parts: &parts,
    };
    assert!(verify_receipt(&line, &expectation).unwrap());
    let wrong = String::from_utf8(line.clone())
      .unwrap()
      .replace("\"run_id\":\"r42\"", "\"run_id\":\"r43\"");
    assert!(verify_receipt(wrong.as_bytes(), &expectation).is_err());
    let duplicate = String::from_utf8(line.clone()).unwrap().replacen(
      "\"schema_version\":1,",
      "\"schema_version\":1,\"schema_version\":1,",
      1,
    );
    assert!(verify_receipt(duplicate.as_bytes(), &expectation).is_err());
    let unknown = String::from_utf8(line.clone()).unwrap().replacen(
      "\"schema_version\":1,",
      "\"schema_version\":1,\"extra\":true,",
      1,
    );
    assert!(verify_receipt(unknown.as_bytes(), &expectation).is_err());
    assert!(verify_receipt(&line[..line.len() - 1], &expectation).is_err());
  }
}
