//! Operator inspection and owner-controlled stop without PID signaling.

use crate::{
  app::cli::PhysicalStopCommand, app::config::Paths, runtime::owners::dependent_stop,
  runtime::logs::LogStore, runtime::manager::manager_call,
};
use anyhow::{Context, Result, bail};
use serde::Serialize;
use serde_json::{Value, json};
use std::{
  fs,
  os::unix::net::UnixStream,
  path::Path,
  thread,
  time::{Duration, Instant},
};

const NAMES: &[&str] = &[
  "isos",
  "aether-runtime",
  "aether-http-bridge",
  "aether-config-server",
  "egia-runtime",
  "egia",
];

#[derive(Clone, Debug, Serialize)]
struct Candidate {
  pid: u32,
  parent_pid: u32,
  name: String,
  executable: String,
  ownership: &'static str,
}

fn candidates_at(root: &Path, self_pid: u32) -> Result<Vec<Candidate>> {
  let mut found = Vec::new();
  for entry in fs::read_dir(root)? {
    let entry = entry?;
    let Some(pid) = entry
      .file_name()
      .to_str()
      .and_then(|name| name.parse::<u32>().ok())
    else {
      continue;
    };
    if pid == self_pid {
      continue;
    }
    let path = entry.path();
    let Ok(executable) = fs::read_link(path.join("exe")) else {
      continue;
    };
    let display = executable.to_string_lossy();
    let name = display
      .strip_suffix(" (deleted)")
      .unwrap_or(&display)
      .rsplit('/')
      .next()
      .unwrap_or("");
    if !NAMES.contains(&name) {
      continue;
    }
    let Ok(stat) = fs::read_to_string(path.join("stat")) else {
      continue;
    };
    let Some(parent_pid) = stat
      .rsplit_once(") ")
      .and_then(|(_, fields)| fields.split_whitespace().nth(1))
      .and_then(|value| value.parse::<u32>().ok())
    else {
      continue;
    };
    found.push(Candidate {
      pid,
      parent_pid,
      name: name.to_owned(),
      executable: display.into_owned(),
      ownership: "unverified",
    });
  }
  found.sort_by_key(|candidate| candidate.pid);
  Ok(found)
}

fn candidates() -> Result<Vec<Candidate>> {
  candidates_at(Path::new("/proc"), std::process::id())
}

pub(crate) fn offline_status(paths: &Paths, deployment: &str) -> Value {
  let (run_id, run_id_state) = LogStore::new(paths, deployment)
    .map(|store| store.run_identity())
    .unwrap_or((None, "unavailable"));
  let terminal = if run_id_state == "known" {
    run_id
      .as_deref()
      .map(|id| dependent_stop::recover(paths, deployment, id))
  } else {
    None
  };
  let unknown = terminal
    .as_ref()
    .is_some_and(|value| value.outcome == "unknown");
  let state = if run_id_state == "unavailable" || unknown {
    "degraded"
  } else {
    "stopped"
  };
  let last_error = if unknown {
    Some(
      "Previous run's stop proof is unavailable after manager restart; owner-reviewed physical-stop recovery is required before launch",
    )
  } else if run_id_state == "unavailable" {
    Some("Previous run identity is unavailable; owner review is required before launch")
  } else {
    None
  };
  json!({
      "schema_version": "1.0.0",
      "state": state,
      "last_error": last_error,
      "active_revision": null,
      "processes": [],
      "run_id": run_id,
      "run_id_state": run_id_state,
      "terminal": terminal,
      "manager": {"state": "stopped"},
      "services": []
  })
}

fn status(paths: &Paths, deployment: &str) -> Result<Value> {
  if UnixStream::connect(paths.socket()).is_err() {
    return Ok(offline_status(paths, deployment));
  }
  let mut value = manager_call(paths, deployment, "deployment_status")?;
  value["manager"] = manager_call(paths, deployment, "manager_identity")?;
  Ok(value)
}

fn stop_result_state(result: &Value, before: &Value) -> &'static str {
  if before["terminal"]["outcome"] == "unknown" {
    return "unknown";
  }
  let Some(items) = result["terminal"].as_array() else {
    return "unknown";
  };
  if items.iter().any(|item| item["outcome"] != "confirmed") {
    return "unknown";
  }
  if before["state"] != "running" {
    return "not_requested";
  }
  let Some(run_id) = before["run_id"].as_str() else {
    return "unknown";
  };
  if items.iter().filter(|item| item["run_id"] == run_id).count() == 1 {
    "confirmed"
  } else {
    "unknown"
  }
}

fn validate_recovery_request(
  before: &Value,
  run_id: &str,
  attest_safe: bool,
  remaining: &[Candidate],
) -> Result<()> {
  if !attest_safe {
    bail!("operator must attest the backend is safe before recovery");
  }
  if before["manager"]["state"] != "stopped" {
    bail!("stop the IsOS manager before operator recovery");
  }
  if before["run_id_state"] != "known"
    || before["run_id"].as_str() != Some(run_id)
    || before["terminal"]["outcome"] != "unknown"
  {
    bail!("run ID is not the current interrupted stop");
  }
  if !remaining.is_empty() {
    bail!("possible processes remain; identify them before recovery");
  }
  Ok(())
}

pub(crate) fn run(paths: &Paths, deployment: &str, command: PhysicalStopCommand) -> Result<()> {
  let before = status(paths, deployment).context("cannot verify IsOS manager status")?;
  match command {
    PhysicalStopCommand::Check => {
      println!(
        "{}",
        serde_json::to_string_pretty(&json!({
            "status": before,
            "candidates": candidates()?,
            "note": "Candidate names do not prove ownership or physical safety"
        }))?
      );
    }
    PhysicalStopCommand::Stop => {
      if before["manager"]["state"] == "stopped" {
        let remaining = candidates()?;
        let unknown =
          before["terminal"]["outcome"] == "unknown" || before["run_id_state"] == "unavailable";
        println!(
          "{}",
          serde_json::to_string_pretty(&json!({
              "status": before,
              "stop": null,
              "physical_stop": if unknown { "unknown" } else { "not_requested" },
              "candidates": remaining,
              "note": "Manager is stopped; identify candidates before any manual signal"
          }))?
        );
        if unknown {
          bail!("previous run's physical stop proof is unavailable");
        }
        if !remaining.is_empty() {
          bail!("possible processes remain with unverified ownership");
        }
        return Ok(());
      }
      if before["manager"]["pid"].as_u64().is_none()
        || before["manager"]["start_ticks"].as_str().is_none()
      {
        bail!("manager identity is incomplete; no stop was requested");
      }
      let result = manager_call(paths, deployment, "manager_shutdown")
        .context("owner-controlled stop failed")?;
      let deadline = Instant::now() + Duration::from_secs(2);
      let remaining = loop {
        let found = candidates()?;
        if found.is_empty() || Instant::now() >= deadline {
          break found;
        }
        thread::sleep(Duration::from_millis(50));
      };
      let physical_stop = stop_result_state(&result, &before);
      println!(
        "{}",
        serde_json::to_string_pretty(&json!({
            "before": before,
            "stop": result,
            "physical_stop": physical_stop,
            "candidates": remaining,
            "note": "Identify remaining candidates before any manual signal"
        }))?
      );
      if physical_stop == "unknown" {
        bail!("physical stop is unconfirmed");
      }
      if !remaining.is_empty() {
        bail!("possible processes remain with unverified ownership");
      }
    }
    PhysicalStopCommand::Recover {
      run_id,
      attest_safe,
    } => {
      let remaining = candidates()?;
      if let Err(error) = validate_recovery_request(&before, &run_id, attest_safe, &remaining) {
        println!(
          "{}",
          serde_json::to_string_pretty(&json!({"candidates":remaining}))?
        );
        return Err(error);
      }
      dependent_stop::publish_operator_recovery(paths, deployment, &run_id)
        .context("could not persist operator recovery")?;
      println!(
        "{}",
        serde_json::to_string_pretty(&json!({
            "status": offline_status(paths, deployment),
            "recovery_method": "operator_attestation",
            "note": "Operator attested backend safety; no process was signaled"
        }))?
      );
    }
  }
  Ok(())
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::os::unix::fs::symlink;

  #[test]
  fn candidate_scan_excludes_self_and_requires_exact_name() {
    let root = tempfile::tempdir().unwrap();
    for (pid, name) in [(42, "aether-runtime"), (43, "other"), (44, "isos")] {
      let path = root.path().join(pid.to_string());
      fs::create_dir(&path).unwrap();
      symlink(format!("/opt/bin/{name}"), path.join("exe")).unwrap();
      fs::write(path.join("stat"), format!("{pid} ({name}) S 1 0 0")).unwrap();
    }
    let found = candidates_at(root.path(), 44).unwrap();
    assert_eq!(found.len(), 1);
    assert_eq!(found[0].pid, 42);
    assert_eq!(found[0].parent_pid, 1);
  }

  #[test]
  fn only_the_selected_running_deployment_receipt_confirms_stop() {
    let running = json!({"state":"running","run_id":"r42","terminal":null});
    assert_eq!(
      stop_result_state(
        &json!({"terminal":[{"run_id":"r42","outcome":"confirmed"}]}),
        &running
      ),
      "confirmed"
    );
    assert_eq!(
      stop_result_state(
        &json!({"terminal":[{"run_id":"other","outcome":"confirmed"}]}),
        &running
      ),
      "unknown"
    );
    assert_eq!(
      stop_result_state(&json!({"terminal":[]}), &running),
      "unknown"
    );
    assert_eq!(
      stop_result_state(
        &json!({"terminal":[{"run_id":"r42","outcome":"unknown"}]}),
        &running
      ),
      "unknown"
    );
    let inactive = json!({"state":"stopped","run_id":null,"terminal":null});
    assert_eq!(
      stop_result_state(&json!({"terminal":[]}), &inactive),
      "not_requested"
    );
    assert_eq!(
      stop_result_state(
        &json!({"terminal":[{"run_id":"other","outcome":"confirmed"}]}),
        &inactive
      ),
      "not_requested"
    );
  }

  #[test]
  fn recovery_requires_exact_run_attestation_and_no_candidates() {
    let status = json!({"manager":{"state":"stopped"},"run_id_state":"known","run_id":"r42","terminal":{"outcome":"unknown"}});
    assert!(validate_recovery_request(&status, "r42", true, &[]).is_ok());
    assert!(validate_recovery_request(&status, "r43", true, &[]).is_err());
    assert!(validate_recovery_request(&status, "r42", false, &[]).is_err());
    let candidate = Candidate {
      pid: 42,
      parent_pid: 1,
      name: "aether-runtime".into(),
      executable: "/opt/bin/aether-runtime".into(),
      ownership: "unverified",
    };
    assert!(validate_recovery_request(&status, "r42", true, &[candidate]).is_err());
  }

  #[test]
  fn offline_status_preserves_unknown_and_durable_confirmation() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let logs = LogStore::new(&paths, "test").unwrap();
    logs.publish_run("r42").unwrap();
    let ids = vec!["eos".into()];
    dependent_stop::create_running(&paths, "test", "r42", &ids).unwrap();
    dependent_stop::publish_unknown(&paths, "test", "r42", &ids).unwrap();
    let before = offline_status(&paths, "test");
    assert_eq!(before["state"], "degraded");
    assert_eq!(before["terminal"]["reason_code"], "stop_interrupted");
    dependent_stop::publish_operator_recovery(&paths, "test", "r42").unwrap();
    let after = offline_status(&paths, "test");
    assert_eq!(after["state"], "stopped");
    assert_eq!(after["terminal"]["outcome"], "confirmed");
    assert_eq!(after["terminal"]["recovery_method"], "operator_attestation");
  }
}
