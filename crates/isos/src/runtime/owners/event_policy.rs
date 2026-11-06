//! P06 selected-child event admission. The child pipe supplies bytes; the
//! supervisor supplies the role, run identity and observed PID.

use anyhow::{Context, Result, ensure};
use chrono::{DateTime, SecondsFormat};
use crate::interfaces::events::{RawEvent, StrictStoredEvent};
use serde_json::{Value, json};
#[cfg(test)]
use serde_json::Map;
use std::{collections::BTreeSet, sync::OnceLock};

pub const MAX_RAW_LINE: usize = 8_064;
pub const MAX_STORED_LINE: usize = 8_192;

fn token(value: &str, max: usize) -> bool {
  !value.is_empty()
    && value.len() <= max
    && value
      .bytes()
      .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
}

fn browser_result_correlation(value: &str) -> bool {
  if !token(value, 64) {
    return false;
  }
  match value.strip_prefix("h_") {
    Some(digest) => {
      digest.len() == 62
        && digest
          .bytes()
          .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    }
    None => true,
  }
}

fn code(value: &str) -> bool {
  !value.is_empty()
    && value.len() <= 64
    && value
      .bytes()
      .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
}

pub(crate) fn node_pointer(value: &str, core: &str) -> bool {
  if value.len() > 512 {
    return false;
  }
  let parts: Vec<_> = value.split('/').collect();
  parts.len() == 9
    && parts[0].is_empty()
    && parts[1] == "cores"
    && parts[2] == core
    && parts[3] == "layers"
    && parts[5] == "groups"
    && parts[7] == "nodes"
    && [parts[2], parts[4], parts[6], parts[8]]
      .iter()
      .all(|part| token(part, 48))
}

/// Closed, release-pinned role/code policy. Unknown codes cannot be authorized
/// by graph data or a child's own capability response.
fn allowed(role: &str, event_code: &str) -> bool {
  match role {
    "eos" => matches!(
      event_code,
      "eos_starting"
        | "eos_ready"
        | "eos_start_failed"
        | "eos_stopping"
        | "eos_stopped"
        | "eos_package_admitted"
        | "eos_package_provenance_unavailable"
        | "eos_backend_ready"
        | "eos_backend_lost"
        | "eos_stop_unconfirmed"
        | "eos_events_suppressed"
        | "eos_skill_execution"
        | "eos_skill_stop_unconfirmed"
    ),
    "bridge" => matches!(
      event_code,
      "bridge_starting"
        | "bridge_ready"
        | "bridge_start_failed"
        | "bridge_stopping"
        | "bridge_stopped"
        | "bridge_primitive_result"
    ),
    "config" => matches!(
      event_code,
      "config_starting"
        | "config_ready"
        | "config_start_failed"
        | "config_stopping"
        | "config_stopped"
    ),
    "egia" => egia_policy()["codes"].get(event_code).is_some(),
    _ => false,
  }
}

fn egia_policy() -> &'static Value {
  static POLICY: OnceLock<Value> = OnceLock::new();
  POLICY.get_or_init(|| {
    serde_json::from_str(include_str!(
      "../../../tests/fixtures/jsonl_events/egia-role-policy.json"
    ))
    .expect("reviewed EGIA role policy fixture")
  })
}

fn enum_value(value: &Value, choices: &Value) -> bool {
  value.as_str().is_some_and(|s| {
    choices
      .as_array()
      .is_some_and(|values| values.iter().any(|v| v.as_str() == Some(s)))
  })
}

fn validate_egia(event: &RawEvent) -> Result<()> {
  let policy = egia_policy();
  let rule = &policy["codes"][&event.event_code];
  ensure!(rule.is_object(), "unregistered EGIA event code");
  ensure!(
    rule["message"].as_str() == Some(event.message.as_str()),
    "EGIA message differs from owner template"
  );
  let node_rule = rule["node_path"].as_str().context("EGIA node rule")?;
  ensure!(
    node_rule != "null" || event.node_path.is_none(),
    "EGIA node path forbidden for code"
  );
  if let Some(level) = rule["level"].as_str() {
    ensure!(event.level == level, "EGIA severity mismatch");
  } else if rule.get("level_by_state").is_some() {
    let state = event
      .fields
      .get("state")
      .and_then(Value::as_str)
      .context("EGIA state missing")?;
    ensure!(
      rule["level_by_state"][state].as_str() == Some(event.level.as_str()),
      "EGIA state severity mismatch"
    );
  } else if rule.get("level_by_outcome").is_some() {
    let outcome = event
      .fields
      .get("outcome")
      .and_then(Value::as_str)
      .context("EGIA outcome missing")?;
    ensure!(
      rule["level_by_outcome"][outcome].as_str() == Some(event.level.as_str()),
      "EGIA outcome severity mismatch"
    );
  }
  let allowed_fields = rule["fields"].as_object().context("EGIA field policy")?;
  for (key, kind) in allowed_fields {
    if !key.ends_with('?') {
      ensure!(
        event.fields.contains_key(key),
        "EGIA required field missing"
      );
    }
    let actual_key = key.trim_end_matches('?');
    let Some(value) = event.fields.get(actual_key) else {
      continue;
    };
    let kind = kind.as_str().context("EGIA field kind")?;
    let valid = if kind == "positive_integer" {
      value
        .as_u64()
        .is_some_and(|n| (1..=u32::MAX as u64).contains(&n))
    } else if kind == "opaque_id" {
      value.as_str().is_some_and(|s| token(s, 64))
    } else {
      enum_value(value, &policy["types"][kind])
    };
    ensure!(valid, "EGIA typed field invalid");
  }
  for key in event.fields.keys() {
    ensure!(
      allowed_fields.contains_key(key) || allowed_fields.contains_key(&format!("{key}?")),
      "EGIA field not allowed"
    );
  }
  Ok(())
}

fn validate_eos(event: &RawEvent) -> Result<()> {
  let code = event.event_code.as_str();
  if code == "bridge_primitive_result" {
    return validate_bridge_primitive_result(event);
  }
  if matches!(code, "eos_skill_execution" | "eos_skill_stop_unconfirmed") {
    return validate_skill(event);
  }
  let prefix = event.source.as_str();
  let (level, message, required, optional, node_allowed) = match code {
    "eos_starting" | "bridge_starting" | "config_starting" => (
      "info",
      format!("EOS {prefix} starting"),
      vec![("state", "starting")],
      vec![],
      false,
    ),
    "eos_ready" => (
      "info",
      "EOS runtime ready".into(),
      vec![("state", "ready")],
      vec!["node_count"],
      false,
    ),
    "bridge_ready" | "config_ready" => (
      "info",
      format!("EOS {prefix} ready"),
      vec![("state", "ready")],
      vec![],
      false,
    ),
    "eos_start_failed" | "bridge_start_failed" | "config_start_failed" => (
      "error",
      format!("EOS {prefix} failed to start"),
      vec![("state", "failed")],
      vec!["stage", "reason_code"],
      false,
    ),
    "eos_stopping" | "bridge_stopping" | "config_stopping" => (
      "info",
      format!("EOS {prefix} stopping"),
      vec![("state", "stopping")],
      vec![],
      false,
    ),
    "eos_stopped" | "bridge_stopped" | "config_stopped" => (
      "info",
      format!("EOS {prefix} stopped"),
      vec![("state", "stopped")],
      vec![],
      false,
    ),
    "eos_package_admitted" => (
      "info",
      "EOS backend package admitted".into(),
      vec![],
      vec![
        "package_id",
        "package_version",
        "release_sha256",
        "payload_sha256",
      ],
      true,
    ),
    "eos_package_provenance_unavailable" => (
      "warn",
      "EOS backend package provenance unavailable".into(),
      vec![],
      vec!["package_id", "package_version"],
      true,
    ),
    "eos_backend_ready" => (
      "info",
      "EOS backend child ready".into(),
      vec![("state", "ready")],
      vec![],
      true,
    ),
    "eos_backend_lost" => (
      "error",
      "EOS backend child exited".into(),
      vec![("state", "failed"), ("reason_code", "child_exited")],
      vec![],
      true,
    ),
    "eos_stop_unconfirmed" => (
      "error",
      "EOS actuator stop unconfirmed".into(),
      vec![("state", "failed"), ("reason_code", "stop_unconfirmed")],
      vec![],
      true,
    ),
    "eos_events_suppressed" => (
      "warn",
      "EOS repeated events suppressed".into(),
      vec![],
      vec!["suppressed_count"],
      false,
    ),
    _ => anyhow::bail!("unregistered EOS event code"),
  };
  ensure!(
    event.level == level && event.message == message,
    "EOS envelope differs from owner template"
  );
  ensure!(
    node_allowed || event.node_path.is_none(),
    "EOS node path forbidden for code"
  );
  if node_allowed {
    ensure!(event.node_path.is_some(), "EOS backend node path missing");
  }
  for (key, expected) in &required {
    ensure!(
      event.fields.get(*key).and_then(Value::as_str) == Some(*expected),
      "EOS required field mismatch"
    );
  }
  for key in event.fields.keys() {
    ensure!(
      required.iter().any(|(name, _)| *name == key) || optional.contains(&key.as_str()),
      "EOS field not allowed for code"
    );
  }
  for (key, value) in &event.fields {
    let valid = match key.as_str() {
      "state" => matches!(
        value.as_str(),
        Some("starting" | "ready" | "failed" | "stopping" | "stopped")
      ),
      "stage" => matches!(
        value.as_str(),
        Some(
          "options"
            | "profile"
            | "registry"
            | "adapter"
            | "journal"
            | "backend"
            | "listener"
            | "stop"
            | "sink"
        )
      ),
      "reason_code" => matches!(
        value.as_str(),
        Some(
          "invalid_run_id"
            | "profile_invalid"
            | "registry_invalid"
            | "adapter_unavailable"
            | "journal_unavailable"
            | "backend_start_failed"
            | "child_exited"
            | "listener_bind_failed"
            | "stop_unconfirmed"
            | "event_sink_failed"
            | "other_failure"
        )
      ),
      "package_id" => value.as_str().is_some_and(package_id),
      "package_version" => value.as_str().is_some_and(package_version),
      "release_sha256" | "payload_sha256" => value.as_str().is_some_and(|s| {
        s.len() == 64
          && s
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
      }),
      "node_count" | "suppressed_count" => value.as_u64().is_some_and(|n| n <= u32::MAX as u64),
      _ => false,
    };
    ensure!(valid, "EOS typed field invalid");
  }
  if code == "eos_package_admitted" {
    ensure!(
      [
        "package_id",
        "package_version",
        "release_sha256",
        "payload_sha256"
      ]
      .iter()
      .all(|k| event.fields.contains_key(*k)),
      "EOS package provenance incomplete"
    );
  }
  if code.ends_with("_start_failed") {
    ensure!(
      event.fields.contains_key("stage") && event.fields.contains_key("reason_code"),
      "EOS startup failure fields incomplete"
    );
  }
  if code == "eos_ready" {
    ensure!(
      event.fields.contains_key("node_count"),
      "EOS admitted node count missing"
    );
  }
  if code == "eos_package_provenance_unavailable" {
    ensure!(
      ["package_id", "package_version"]
        .iter()
        .all(|k| event.fields.contains_key(*k)),
      "EOS package pin incomplete"
    );
  }
  if code == "eos_events_suppressed" {
    ensure!(
      event
        .fields
        .get("suppressed_count")
        .and_then(Value::as_u64)
        .is_some_and(|n| n > 0),
      "EOS suppression count missing"
    );
  }
  Ok(())
}

fn validate_bridge_primitive_result(event: &RawEvent) -> Result<()> {
  ensure!(
    event.node_path.is_none(),
    "bridge result node path forbidden"
  );
  ensure!(
    event.message == "EOS browser primitive result",
    "bridge result message mismatch"
  );
  ensure!(
    event
      .correlation_id
      .as_deref()
      .is_some_and(browser_result_correlation),
    "bridge result correlation missing or invalid"
  );
  ensure!(
    event.fields.len() == 2
      && event.fields.contains_key("status")
      && event.fields.contains_key("phase"),
    "bridge result fields mismatch"
  );
  let status = event.fields["status"]
    .as_str()
    .context("bridge result status")?;
  let level = match status {
    "Failed" | "Rejected" | "OutcomeUnknown" => "warn",
    "Completed" | "Cancelled" | "InProgress" => "info",
    _ => anyhow::bail!("bridge result status invalid"),
  };
  ensure!(event.level == level, "bridge result severity mismatch");
  ensure!(
    matches!(event.fields["phase"].as_str(), Some("submit" | "followup")),
    "bridge result phase invalid"
  );
  Ok(())
}

fn package_id(value: &str) -> bool {
  if value.len() > 96 {
    return false;
  }
  let Some((namespace, name)) = value.split_once('/') else {
    return false;
  };
  [namespace, name].into_iter().all(|part| {
    !part.is_empty()
      && part
        .bytes()
        .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-')
  })
}

fn package_version(value: &str) -> bool {
  value.len() <= 64
    && value.as_bytes().first().is_some_and(u8::is_ascii_digit)
    && value.split('.').count() >= 3
    && value
      .bytes()
      .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'+'))
}

fn validate_skill(event: &RawEvent) -> Result<()> {
  ensure!(event.node_path.is_none(), "skill node path forbidden");
  let fields = &event.fields;
  if event.event_code == "eos_skill_stop_unconfirmed" {
    ensure!(
      event.level == "error" && event.message == "EOS native skill stop unconfirmed",
      "skill stop envelope mismatch"
    );
    ensure!(
      fields.len() == 2
        && fields.get("state").and_then(Value::as_str) == Some("failed")
        && fields.get("reason_code").and_then(Value::as_str) == Some("skill_stop_unconfirmed"),
      "skill stop fields mismatch"
    );
    return Ok(());
  }
  const KEYS: [&str; 7] = [
    "execution_id",
    "package_id",
    "package_version",
    "stage",
    "reason_code",
    "worker_reaped",
    "worker_exit_code",
  ];
  ensure!(
    fields.len() == KEYS.len() && KEYS.iter().all(|key| fields.contains_key(*key)),
    "skill execution fields mismatch"
  );
  ensure!(
    event.message == "EOS native skill execution lifecycle",
    "skill execution message mismatch"
  );
  let correlation = event
    .correlation_id
    .as_deref()
    .context("skill correlation missing")?;
  ensure!(
    correlation.len() == 38
      && correlation.starts_with("skill_")
      && correlation[6..]
        .bytes()
        .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte)),
    "skill correlation invalid"
  );
  let execution_id = fields["execution_id"]
    .as_str()
    .context("skill execution ID")?;
  ensure!(
    execution_id.starts_with("ex-")
      && execution_id.len() <= 80
      && execution_id.bytes().all(|byte| {
        byte.is_ascii_lowercase() || byte.is_ascii_digit() || matches!(byte, b'.' | b'_' | b'-')
      }),
    "skill execution ID invalid"
  );
  ensure!(
    fields["package_id"].as_str().is_some_and(package_id)
      && fields["package_version"]
        .as_str()
        .is_some_and(package_version),
    "skill package pin invalid"
  );
  let stage = fields["stage"].as_str().context("skill stage")?;
  let reason = fields["reason_code"].as_str().context("skill reason")?;
  let pairing = match stage {
    "queued" => event.level == "info" && reason == "worker.queued",
    "running" => event.level == "info" && reason == "worker.running",
    "completed" => event.level == "info" && reason == "worker.succeeded",
    "failed" => {
      event.level == "error"
        && matches!(
          reason,
          "worker.cancelled"
            | "worker.deadline"
            | "worker.failed"
            | "worker.outcome_unknown"
            | "worker.primitive_denied"
            | "worker.protocol_invalid"
            | "worker.reap_failed"
            | "worker.rejected"
            | "worker.result_capacity"
            | "worker.result_invalid"
            | "worker.shutdown"
        )
    }
    _ => false,
  };
  ensure!(pairing, "skill stage, reason or severity mismatch");
  let reaped = &fields["worker_reaped"];
  let exit = &fields["worker_exit_code"];
  ensure!(
    reaped.is_null() || reaped.is_boolean(),
    "skill worker reap type invalid"
  );
  ensure!(
    exit.is_null()
      || exit
        .as_i64()
        .is_some_and(|code| i32::try_from(code).is_ok()),
    "skill worker exit type invalid"
  );
  ensure!(
    !matches!(stage, "queued" | "running") || (reaped.is_null() && exit.is_null()),
    "nonterminal skill worker fact"
  );
  ensure!(
    stage != "completed" || reaped.as_bool() == Some(true),
    "completed skill worker was not reaped"
  );
  Ok(())
}

pub(crate) fn skill_pin_admitted(value: &Value, pins: &BTreeSet<(String, String)>) -> bool {
  value["event_code"] != "eos_skill_execution"
    || value["fields"]["package_id"]
      .as_str()
      .zip(value["fields"]["package_version"].as_str())
      .is_some_and(|(id, version)| pins.contains(&(id.to_owned(), version.to_owned())))
}

pub fn parse_child(raw: &[u8], role: &str, run_id: &str, pid: u32) -> Result<Value> {
  ensure!(
    raw.len() <= MAX_RAW_LINE && raw.last() == Some(&b'\n'),
    "raw line limit or terminator"
  );
  let event: RawEvent = serde_json::from_slice(raw).context("invalid selected event JSON")?;
  if matches!(
    event.event_code.as_str(),
    "eos_skill_execution" | "eos_skill_stop_unconfirmed" | "bridge_primitive_result"
  ) {
    let envelope: Value = serde_json::from_slice(raw)?;
    let object = envelope.as_object().context("selected envelope object")?;
    const KEYS: [&str; 10] = [
      "timestamp",
      "level",
      "source",
      "core",
      "node_path",
      "event_code",
      "run_id",
      "correlation_id",
      "message",
      "fields",
    ];
    ensure!(
      object.len() == KEYS.len() && KEYS.iter().all(|key| object.contains_key(*key)),
      "selected envelope fields mismatch"
    );
  }
  let (source, core) = match role {
    "eos" | "bridge" | "config" => (role, "eos"),
    "egia" => ("egia", "egia"),
    _ => anyhow::bail!("unselected process role"),
  };
  ensure!(
    event.source == source && event.core == core,
    "source role mismatch"
  );
  ensure!(
    event.run_id == run_id && token(run_id, 64) && run_id.starts_with('r'),
    "run mismatch"
  );
  ensure!(
    code(&event.event_code) && allowed(role, &event.event_code),
    "unregistered event code"
  );
  ensure!(
    matches!(
      event.level.as_str(),
      "trace" | "debug" | "info" | "warn" | "error"
    ),
    "invalid severity"
  );
  ensure!(
    event
      .correlation_id
      .as_deref()
      .is_none_or(|id| token(id, 64)),
    "invalid correlation"
  );
  ensure!(
    event
      .node_path
      .as_deref()
      .is_none_or(|path| node_pointer(path, core)),
    "invalid node pointer"
  );
  let stamp = DateTime::parse_from_rfc3339(&event.timestamp).context("invalid event time")?;
  ensure!(
    event.timestamp.ends_with('Z')
      && event.timestamp.len() >= 24
      && stamp.to_rfc3339_opts(SecondsFormat::Millis, true) == event.timestamp,
    "event time must be UTC milliseconds"
  );
  ensure!(
    event.fields.len() <= 12 && serde_json::to_vec(&event.fields)?.len() <= 4096,
    "event fields too large"
  );
  for (key, value) in &event.fields {
    ensure!(token(key, 48), "invalid event field key");
    ensure!(
      value.is_null()
        || value.is_boolean()
        || value.is_i64()
        || value.is_u64()
        || value
          .as_str()
          .is_some_and(|s| s.len() <= 256 && !s.chars().any(char::is_control)),
      "unsafe event field value"
    );
  }
  ensure!(
    !event.message.is_empty()
      && event.message.len() <= 8192
      && !event.message.chars().any(char::is_control),
    "unsafe event message"
  );
  if role == "egia" {
    validate_egia(&event)?;
  } else {
    validate_eos(&event)?;
  }
  let normalized = json!({"timestamp":event.timestamp,"level":event.level,"source":source,"core":core,
        "node_path":event.node_path,"event_code":event.event_code,"run_id":run_id,
        "correlation_id":event.correlation_id,"message":event.message,"fields":event.fields,"pid":pid});
  ensure!(
    serde_json::to_vec(&normalized)?.len() < MAX_STORED_LINE,
    "normalized line too large"
  );
  Ok(normalized)
}

/// Check an already normalized P06 record before it can enter a P07 page or
/// become a persisted anchor. The source log is owner-only, but it can be
/// truncated or modified independently of the cursor state.
pub(crate) fn validate_stored(value: &Value) -> Result<()> {
  let object = value
    .as_object()
    .context("stored event must be an object")?;
  const KEYS: [&str; 11] = [
    "timestamp",
    "level",
    "source",
    "core",
    "node_path",
    "event_code",
    "run_id",
    "correlation_id",
    "message",
    "fields",
    "pid",
  ];
  ensure!(
    object.len() == KEYS.len() && KEYS.iter().all(|key| object.contains_key(*key)),
    "stored event envelope mismatch"
  );
  ensure!(
    serde_json::to_vec(value)?.len() < MAX_STORED_LINE,
    "stored event exceeds line bound"
  );
  let timestamp = value["timestamp"]
    .as_str()
    .context("stored event timestamp")?;
  let stamp = DateTime::parse_from_rfc3339(timestamp)?;
  ensure!(
    timestamp.ends_with('Z') && stamp.to_rfc3339_opts(SecondsFormat::Millis, true) == timestamp,
    "stored event time must be UTC milliseconds"
  );
  let level = value["level"].as_str().context("stored event level")?;
  ensure!(
    matches!(level, "trace" | "debug" | "info" | "warn" | "error"),
    "stored event severity"
  );
  let source = value["source"].as_str().context("stored event source")?;
  ensure!(
    token(source, 48) && !matches!(source, "all" | "services" | "events"),
    "stored event source"
  );
  let core = value["core"].as_str().context("stored event core")?;
  let selected = matches!(source, "eos" | "bridge" | "config" | "egia");
  ensure!(
    core
      == if source == "egia" {
        "egia"
      } else if selected {
        "eos"
      } else {
        "isos"
      },
    "stored event core/source mismatch"
  );
  let run = value["run_id"].as_str().context("stored event run")?;
  ensure!(
    token(run, 64) && (run.starts_with('m') || run.starts_with('r')),
    "stored event run"
  );
  ensure!(
    value["correlation_id"].is_null()
      || value["correlation_id"]
        .as_str()
        .is_some_and(|id| token(id, 64)),
    "stored event correlation"
  );
  ensure!(
    value["node_path"].is_null()
      || value["node_path"].as_str().is_some_and(|path| node_pointer(
        path,
        if selected { core } else { "isos" }
      ) || (!selected
        && ["eos", "egia"]
          .into_iter()
          .any(|owner| node_pointer(path, owner)))),
    "stored event node pointer"
  );
  let code_value = value["event_code"].as_str().context("stored event code")?;
  ensure!(code(code_value), "stored event code");
  let message = value["message"].as_str().context("stored event message")?;
  ensure!(
    !message.is_empty() && message.len() <= 8192 && !message.chars().any(char::is_control),
    "stored event message"
  );
  ensure!(
    value["pid"].is_null()
      || value["pid"]
        .as_u64()
        .is_some_and(|pid| (1..=u32::MAX as u64).contains(&pid)),
    "stored event pid"
  );
  let fields = value["fields"].as_object().context("stored event fields")?;
  ensure!(
    fields.len() <= 12 && serde_json::to_vec(fields)?.len() <= 4096,
    "stored event fields bound"
  );
  for (key, field) in fields {
    ensure!(token(key, 48), "stored event field key");
    ensure!(
      field.is_null()
        || field.is_boolean()
        || (field.is_number() && (!selected || field.is_i64() || field.is_u64()))
        || field
          .as_str()
          .is_some_and(|s| s.len() <= 256 && !s.chars().any(char::is_control)),
      "stored event field value"
    );
  }
  if selected {
    ensure!(run.starts_with('r'), "selected event run");
    ensure!(
      value["pid"]
        .as_u64()
        .is_some_and(|pid| (1..=u32::MAX as u64).contains(&pid)),
      "selected event pid"
    );
    ensure!(allowed(source, code_value), "selected stored event code");
    let mut raw = value.clone();
    raw.as_object_mut().expect("checked object").remove("pid");
    let event: RawEvent = serde_json::from_value(raw)?;
    if source == "egia" {
      validate_egia(&event)?;
    } else {
      validate_eos(&event)?;
    }
  }
  Ok(())
}

pub(crate) fn parse_stored(raw: &[u8]) -> Result<Value> {
  ensure!(
    raw.len() <= MAX_STORED_LINE && raw.last() == Some(&b'\n'),
    "stored event line bound"
  );
  let _: StrictStoredEvent = serde_json::from_slice(raw)?;
  let value: Value = serde_json::from_slice(raw)?;
  validate_stored(&value)?;
  Ok(value)
}

#[cfg(test)]
mod tests {
  use super::*;
  fn line(value: &Value) -> Vec<u8> {
    let mut raw = serde_json::to_vec(value).unwrap();
    raw.push(b'\n');
    raw
  }

  #[test]
  fn reviewed_bridge_result_policy_is_closed() {
    let fixture: Value = serde_json::from_str(include_str!(
      "../../../tests/fixtures/jsonl_events/bridge-primitive-result.json"
    ))
    .unwrap();
    let base = &fixture["base"];
    assert_eq!(fixture["schema"], "isos-bridge-primitive-result-q01-v1");
    for case in fixture["accepted"].as_array().unwrap() {
      let mut value = base.clone();
      value["fields"]["status"] = case["status"].clone();
      value["fields"]["phase"] = case["phase"].clone();
      value["level"] = case["level"].clone();
      let stored = parse_child(&line(&value), "bridge", "r_demo", 4242).unwrap();
      assert_eq!(stored["pid"], 4242);
      validate_stored(&stored).unwrap();
      assert!(parse_child(&line(&value), "eos", "r_demo", 4242).is_err());
      assert!(parse_child(&line(&value), "config", "r_demo", 4242).is_err());
      assert!(parse_child(&line(&value), "egia", "r_demo", 4242).is_err());
    }
    for case in fixture["rejected"].as_array().unwrap() {
      let mut value = base.clone();
      let path = case["path"].as_str().unwrap();
      let bad = case.get("repeat").map_or_else(
        || case["value"].clone(),
        |repeat| {
          json!(format!(
            "{}{}",
            case["prefix"].as_str().unwrap(),
            repeat
              .as_str()
              .unwrap()
              .repeat(case["count"].as_u64().unwrap() as usize)
          ))
        },
      );
      if let Some(key) = path.strip_prefix("fields.") {
        value["fields"][key] = bad;
      } else {
        value[path] = bad;
      }
      assert!(
        parse_child(&line(&value), "bridge", "r_demo", 4242).is_err(),
        "{path}"
      );
      if path == "correlation_id" {
        let mut stored = parse_child(&line(base), "bridge", "r_demo", 4242).unwrap();
        stored["correlation_id"] = value["correlation_id"].clone();
        assert!(validate_stored(&stored).is_err(), "stored {path}");
        assert!(parse_stored(&line(&stored)).is_err(), "stored bytes {path}");
      }
    }
    for key in ["node_path", "correlation_id", "fields"] {
      let mut value = base.clone();
      value.as_object_mut().unwrap().remove(key);
      assert!(
        parse_child(&line(&value), "bridge", "r_demo", 4242).is_err(),
        "{key}"
      );
    }
    let mut derived = base.clone();
    derived["correlation_id"] = json!(format!("h_{}", "a".repeat(62)));
    assert!(parse_child(&line(&derived), "bridge", "r_demo", 4242).is_ok());
    let raw = String::from_utf8(line(base)).unwrap();
    let duplicate = raw.replace(
      "\"status\":\"Completed\"",
      "\"status\":\"Completed\",\"status\":\"Failed\"",
    );
    assert!(parse_child(duplicate.as_bytes(), "bridge", "r_demo", 4242).is_err());
    let oversized = format!("{}\n", "x".repeat(MAX_RAW_LINE));
    assert!(parse_child(oversized.as_bytes(), "bridge", "r_demo", 4242).is_err());
  }

  #[test]
  fn reviewed_r04_skill_child_matrix() {
    let fixture: Value = serde_json::from_str(include_str!(
      "../../../tests/fixtures/r04_skill_events/cases.json"
    ))
    .unwrap();
    let pins = BTreeSet::from([(
      "island/fixture-skills".to_owned(),
      "0.1.0+Build.1".to_owned(),
    )]);
    let no_pins = BTreeSet::new();
    assert_eq!(fixture["child_cases"].as_array().unwrap().len(), 31);
    for case in fixture["child_cases"].as_array().unwrap() {
      let id = case["id"].as_str().unwrap();
      let mut event = fixture[if case["base"] == "execution" {
        "base_execution"
      } else {
        "base_stop"
      }]
      .clone();
      if let Some(removed) = case["remove"].as_array() {
        for path in removed {
          let path = path.as_str().unwrap();
          let (parent, key) = path.split_once('.').unwrap();
          event[parent].as_object_mut().unwrap().remove(key);
        }
      }
      for (path, value) in case["set"].as_object().unwrap() {
        if let Some(key) = path.strip_prefix("fields.") {
          event["fields"][key] = value.clone();
        } else {
          event[path] = value.clone();
        }
      }
      let role = case["role"].as_str().unwrap_or("eos");
      let selected = case["selected"].as_bool().unwrap_or(true);
      let alternate_pins = case["selected_pin"].as_object().map(|pin| {
        BTreeSet::from([(
          pin["id"].as_str().unwrap().to_owned(),
          pin["version"].as_str().unwrap().to_owned(),
        )])
      });
      let selected_pins = if selected {
        alternate_pins.as_ref().unwrap_or(&pins)
      } else {
        &no_pins
      };
      let admitted = parse_child(&line(&event), role, "r04-run", 4242).and_then(|stored| {
        ensure!(
          skill_pin_admitted(&stored, selected_pins),
          "skill pin not selected"
        );
        validate_stored(&stored)?;
        Ok(stored)
      });
      assert_eq!(admitted.is_ok(), case["expect"] == "accept", "{id}");
      if let Ok(stored) = admitted {
        assert_eq!(stored["pid"], 4242, "{id}");
      }
    }
  }

  #[test]
  fn reviewed_egia_catalog_is_exact_and_typed() {
    let codes = egia_policy()["codes"].as_object().unwrap();
    assert_eq!(egia_policy()["schema"], "egia-jsonl-role-policy-a03-v2");
    assert_eq!(codes.len(), 24);
    for code in codes.keys() {
      assert!(allowed("egia", code));
    }
    assert!(!allowed("egia", "egia_unreviewed"));
    let mut ready = json!({"timestamp":"2026-11-01T10:00:01.000Z","level":"info","source":"egia","core":"egia",
            "event_code":"egia_ready","run_id":"r1","message":"EGIA ready","fields":{"generation":3}});
    assert_eq!(
      parse_child(&line(&ready), "egia", "r1", 4).unwrap()["pid"],
      4
    );
    ready["fields"]["generation"] = json!(0);
    assert!(parse_child(&line(&ready), "egia", "r1", 4).is_err());
    let mut stopped = json!({"timestamp":"2026-11-01T10:00:01.000Z","level":"error","source":"egia","core":"egia",
            "event_code":"egia_stopped","run_id":"r1","message":"EGIA stopped","fields":{"state":"stop_unconfirmed"}});
    assert!(parse_child(&line(&stopped), "egia", "r1", 4).is_ok());
    stopped["level"] = json!("info");
    assert!(parse_child(&line(&stopped), "egia", "r1", 4).is_err());

    let mut recovery = json!({"timestamp":"2026-11-01T10:00:01.000Z","level":"warn","source":"egia","core":"egia",
            "event_code":"egia_mission_restored","run_id":"r1","message":"Mission checkpoint restored",
            "fields":{"mission_id":"mission_1","outcome":"pending"}});
    assert!(parse_child(&line(&recovery), "egia", "r1", 4).is_ok());
    recovery["fields"]["outcome"] = json!("failed_closed");
    assert!(parse_child(&line(&recovery), "egia", "r1", 4).is_err());
    recovery["level"] = json!("error");
    assert!(parse_child(&line(&recovery), "egia", "r1", 4).is_ok());
    recovery["fields"]["secret"] = json!("unsafe");
    assert!(parse_child(&line(&recovery), "egia", "r1", 4).is_err());
  }

  #[test]
  fn all_a03_egia_codes_keep_closed_templates_and_typed_fields() {
    let policy = egia_policy();
    for (code, rule) in policy["codes"].as_object().unwrap() {
      let mut fields = Map::new();
      for (name, kind) in rule["fields"].as_object().unwrap() {
        if name.ends_with('?') {
          continue;
        }
        let kind = kind.as_str().unwrap();
        let sample = match kind {
          "positive_integer" => json!(1),
          "opaque_id" => json!("id_1"),
          _ => policy["types"][kind][0].clone(),
        };
        fields.insert(name.clone(), sample);
      }
      let level = if let Some(level) = rule["level"].as_str() {
        level
      } else if let Some(state) = fields.get("state").and_then(Value::as_str) {
        rule["level_by_state"][state].as_str().unwrap()
      } else {
        let outcome = fields.get("outcome").and_then(Value::as_str).unwrap();
        rule["level_by_outcome"][outcome].as_str().unwrap()
      };
      let mut event = json!({"timestamp":"2026-11-01T10:00:01.000Z","level":level,"source":"egia","core":"egia",
                "node_path":null,"event_code":code,"run_id":"r1","correlation_id":"request_1",
                "message":rule["message"],"fields":fields});
      assert!(
        parse_child(&line(&event), "egia", "r1", 42).is_ok(),
        "{code}"
      );
      event["message"] = json!("unreviewed child detail");
      assert!(
        parse_child(&line(&event), "egia", "r1", 42).is_err(),
        "{code}"
      );
      event["message"] = rule["message"].clone();
      event["fields"]["unexpected"] = json!("value");
      assert!(
        parse_child(&line(&event), "egia", "r1", 42).is_err(),
        "{code}"
      );
    }
  }

  #[test]
  fn selected_envelope_rejects_spoofing_and_unsafe_shapes() {
    let base = json!({"timestamp":"2026-11-01T10:00:01.000Z","level":"info","source":"eos","core":"eos",
            "event_code":"eos_ready","run_id":"r1","message":"EOS runtime ready","fields":{"state":"ready","node_count":3}});
    for (key, bad) in [
      ("source", json!("egia")),
      ("core", json!("egia")),
      ("run_id", json!("r2")),
      ("event_code", json!("eos_custom")),
      ("message", json!("password=secret")),
      ("timestamp", json!("2026-11-01T10:00:01+00:00")),
      (
        "node_path",
        json!("/cores/eos/layers/process/groups/worker/nodes/sim/package"),
      ),
      ("pid", json!(42)),
      ("payload", json!("frame")),
    ] {
      let mut value = base.clone();
      value[key] = bad;
      assert!(
        parse_child(&line(&value), "eos", "r1", 42).is_err(),
        "{key}"
      );
    }
    let mut value = base.clone();
    value["fields"]["secret"] = json!("value");
    assert!(parse_child(&line(&value), "eos", "r1", 42).is_err());
    assert!(parse_child(&vec![b'x'; MAX_RAW_LINE + 1], "eos", "r1", 42).is_err());
  }

  #[test]
  fn selected_eos_binary_failure_stream_matches_collector_policy() {
    let Some(binary) = std::env::var_os("EOS_P06_RUNTIME_BIN") else {
      return;
    };
    let temp = tempfile::tempdir().unwrap();
    let child = std::process::Command::new(binary)
      .arg("--profile")
      .arg(temp.path().join("missing.toml"))
      .arg("--journal-dir")
      .arg(temp.path().join("journal"))
      .arg("--socket")
      .arg(temp.path().join("eos.sock"))
      .arg("--run-id")
      .arg("r-isos-test")
      .stdout(std::process::Stdio::piped())
      .stderr(std::process::Stdio::piped())
      .spawn()
      .unwrap();
    let pid = child.id();
    let output = child.wait_with_output().unwrap();
    assert!(!output.status.success());
    let records: Vec<_> = output
      .stderr
      .split_inclusive(|byte| *byte == b'\n')
      .filter(|line| !line.is_empty())
      .map(|line| parse_child(line, "eos", "r-isos-test", pid).unwrap())
      .collect();
    assert_eq!(records.len(), 2);
    assert_eq!(records[0]["event_code"], "eos_starting");
    assert_eq!(records[1]["event_code"], "eos_start_failed");
    let paths = crate::app::config::Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    let logs = crate::runtime::logs::LogStore::new(&paths, "default").unwrap();
    logs.publish_run("r-isos-test").unwrap();
    for record in &records {
      logs.append_selected(record).unwrap();
    }
    let source = logs.read("eos", None, 10).unwrap();
    let all = logs.read("all", None, 10).unwrap();
    assert_eq!(source["lines"], all["lines"]);
    let current = logs
      .read_current(crate::runtime::logs::CurrentQuery {
        source: "all",
        level: "info",
        node_path: None,
        correlation_id: None,
        cursor: None,
        limit: 10,
        active_revision: None,
      })
      .unwrap();
    assert_eq!(current["scope"]["run_id"], "r-isos-test");
    assert_eq!(current["anchor"]["event"]["event_code"], "eos_start_failed");
  }

  #[test]
  fn selected_egia_launcher_stream_matches_collector_policy() {
    let Some(launcher) = std::env::var_os("EGIA_P06_LAUNCHER") else {
      return;
    };
    let python = std::env::var_os("EGIA_P06_PYTHON").expect("selected EGIA Python required");
    let temp = tempfile::tempdir().unwrap();
    let mut child = std::process::Command::new(launcher)
      .args(["--managed", "--no-tui", "--var"])
      .arg(temp.path().join("var"))
      .env("EGIA_PYTHON", python)
      .env("ISOS_RUN_ID", "r-isos-test")
      .env_remove("PYTHONPATH")
      .stdout(std::process::Stdio::piped())
      .stderr(std::process::Stdio::piped())
      .spawn()
      .unwrap();
    let pid = child.id();
    std::thread::sleep(std::time::Duration::from_secs(4));
    let _ = child.kill();
    let output = child.wait_with_output().unwrap();
    let paths = crate::app::config::Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    let logs = crate::runtime::logs::LogStore::new(&paths, "default").unwrap();
    logs.publish_run("r-isos-test").unwrap();
    let records: Vec<_> = output
      .stderr
      .split_inclusive(|byte| *byte == b'\n')
      .filter(|line| !line.is_empty())
      .map(|line| parse_child(line, "egia", "r-isos-test", pid).unwrap())
      .collect();
    assert!(!records.is_empty(), "selected EGIA emitted no event");
    assert_eq!(records[0]["event_code"], "egia_starting");
    for record in &records {
      logs.append_selected(record).unwrap();
    }
    let source = logs.read("egia", None, 10).unwrap();
    let all = logs.read("all", None, 10).unwrap();
    assert_eq!(source["lines"], all["lines"]);
    let current = logs
      .read_current(crate::runtime::logs::CurrentQuery {
        source: "all",
        level: "info",
        node_path: None,
        correlation_id: None,
        cursor: None,
        limit: 10,
        active_revision: None,
      })
      .unwrap();
    assert_eq!(current["scope"]["run_id"], "r-isos-test");
    if let Some(first) = records.iter().find(|event| event["level"] == "error") {
      assert_eq!(
        current["anchor"]["event"]["event_code"],
        first["event_code"]
      );
    }
  }
  #[test]
  fn strict_role_and_duplicate_admission() {
    let raw = b"{\"timestamp\":\"2026-11-01T10:00:01.000Z\",\"level\":\"info\",\"source\":\"eos\",\"core\":\"eos\",\"event_code\":\"eos_ready\",\"run_id\":\"r1\",\"message\":\"EOS runtime ready\",\"fields\":{\"state\":\"ready\",\"node_count\":1}}\n";
    assert_eq!(parse_child(raw, "eos", "r1", 42).unwrap()["pid"], 42);
    assert!(parse_child(raw, "bridge", "r1", 42).is_err());
    assert!(parse_child(raw, "eos", "r2", 42).is_err());
    assert!(
      parse_child(
        std::str::from_utf8(raw)
          .unwrap()
          .replace("\"state\":\"ready\"", "\"state\":1,\"state\":2")
          .as_bytes(),
        "eos",
        "r1",
        42
      )
      .is_err()
    );
  }
}
