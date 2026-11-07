use super::*;

#[derive(Debug)]
pub(super) struct RetirementDiagnostic {
  pub(super) code: &'static str,
}

impl std::fmt::Display for RetirementDiagnostic {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    if self.code == "deployment_stop_unconfirmed" {
      write!(
        f,
        "{}: previous run's stop proof is unavailable; inspect deployment status and complete owner-reviewed physical-stop recovery before another launch",
        self.code
      )
    } else {
      write!(f, "{}: owner recovery required", self.code)
    }
  }
}
impl std::error::Error for RetirementDiagnostic {}

#[cfg(test)]
pub(super) fn stop_log_gate() -> &'static (std::sync::Barrier, std::sync::Barrier) {
  static GATE: OnceLock<(std::sync::Barrier, std::sync::Barrier)> = OnceLock::new();
  GATE.get_or_init(|| (std::sync::Barrier::new(2), std::sync::Barrier::new(2)))
}

#[cfg(test)]
pub(super) fn stalled_stop_log_gate() -> &'static (std::sync::Barrier, std::sync::Barrier) {
  static GATE: OnceLock<(std::sync::Barrier, std::sync::Barrier)> = OnceLock::new();
  GATE.get_or_init(|| (std::sync::Barrier::new(2), std::sync::Barrier::new(2)))
}

struct StopLogJob {
  paths: config::Paths,
  deployment: String,
  run_id: String,
  correlation: String,
  terminal: Receiver<(dependent_stop::Terminal, bool)>,
}

fn stop_log_queue() -> Option<&'static SyncSender<StopLogJob>> {
  static QUEUE: OnceLock<Option<SyncSender<StopLogJob>>> = OnceLock::new();
  QUEUE
    .get_or_init(|| {
      let (sender, receiver) = mpsc::sync_channel::<StopLogJob>(32);
      std::thread::Builder::new()
        .name("isos-stop-events".into())
        .spawn(move || {
          while let Ok(job) = receiver.recv() {
            write_stop_log_job(job);
          }
        })
        .ok()
        .map(|_| sender)
    })
    .as_ref()
}

fn write_stop_log_job(job: StopLogJob) {
  let logs = log_store::LogStore::new(&job.paths, &job.deployment);
  if let Ok(logs) = &logs {
    let _ = logs.append_event_context(
      "isos",
      "info",
      "deployment_stopping",
      "Deployment stopping",
      log_store::LogContext {
        run_id: Some(&job.run_id),
        correlation_id: Some(&job.correlation),
        node_path: None,
        fields: json!({}),
      },
    );
  }
  let Ok((terminal, failed_candidate)) = job.terminal.recv_timeout(Duration::from_secs(125)) else {
    log_store::record_stop_event_loss();
    return;
  };
  let (code, level, message) = if terminal.outcome == "unknown" {
    (
      "deployment_stop_unconfirmed",
      "error",
      "Deployment stop unconfirmed",
    )
  } else if failed_candidate {
    ("deployment_failed", "error", "Deployment failed")
  } else {
    ("deployment_stopped", "info", "Deployment stopped")
  };
  #[cfg(test)]
  if job.deployment == "p06-stop-order" && code == "deployment_stopped" {
    let gate = stop_log_gate();
    gate.0.wait();
    gate.1.wait();
  }
  #[cfg(test)]
  if job.deployment == "r06-stop-log-stall" {
    let gate = stalled_stop_log_gate();
    gate.0.wait();
    gate.1.wait();
  }
  if let Ok(logs) = &logs {
    let _ = logs.append_event_context(
      "isos",
      level,
      code,
      message,
      log_store::LogContext {
        run_id: Some(&terminal.run_id),
        correlation_id: Some(&job.correlation),
        node_path: None,
        fields: json!({"outcome":terminal.outcome,"reason_code":terminal.reason_code}),
      },
    );
  } else {
    log_store::record_stop_event_loss();
  }
}

fn queue_stop_events(
  paths: &config::Paths,
  deployment: &str,
  run_id: &str,
  correlation: String,
) -> Option<SyncSender<(dependent_stop::Terminal, bool)>> {
  let (sender, receiver) = mpsc::sync_channel(1);
  let Some(queue) = stop_log_queue() else {
    log_store::record_stop_event_loss();
    log_store::record_stop_event_loss();
    return None;
  };
  let job = StopLogJob {
    paths: paths.clone(),
    deployment: deployment.into(),
    run_id: run_id.into(),
    correlation,
    terminal: receiver,
  };
  if queue.try_send(job).is_err() {
    log_store::record_stop_event_loss();
    log_store::record_stop_event_loss();
    None
  } else {
    Some(sender)
  }
}

pub(super) fn log_deployment(
  paths: &config::Paths,
  name: &str,
  run_id: &str,
  code: &str,
  level: &str,
) {
  #[cfg(test)]
  if name == "p06-stop-order" && code == "deployment_stopped" {
    let gate = stop_log_gate();
    gate.0.wait();
    gate.1.wait();
  }
  if code == "deployment_starting" {
    RUN_STARTED_IN_REQUEST.with(|started| started.set(true));
  }
  if let Ok(logs) = log_store::LogStore::new(paths, name) {
    let correlation = next_correlation_id();
    let message = match code {
      "deployment_starting" => "Deployment starting",
      "deployment_ready" => "Deployment ready",
      "deployment_failed" => "Deployment failed",
      "deployment_stopped" => "Deployment stopped",
      _ => "Deployment state changed",
    };
    let _ = logs.append_event_context(
      "isos",
      level,
      code,
      message,
      log_store::LogContext {
        run_id: Some(run_id),
        correlation_id: Some(&correlation),
        node_path: None,
        fields: json!({}),
      },
    );
  }
}

pub(super) fn stop_active(
  paths: &config::Paths,
  name: &str,
  active: &mut ActiveDeployment,
  failed_candidate: bool,
) -> (Vec<String>, dependent_stop::Terminal) {
  stop_active_with_correlation(paths, name, active, failed_candidate, None)
}

pub(super) fn stop_active_with_correlation(
  paths: &config::Paths,
  name: &str,
  active: &mut ActiveDeployment,
  failed_candidate: bool,
  request_correlation: Option<&str>,
) -> (Vec<String>, dependent_stop::Terminal) {
  let already = active.supervisor.terminal().is_some();
  let correlation = request_correlation
    .map(str::to_owned)
    .unwrap_or_else(next_correlation_id);
  let pending_event = if already {
    None
  } else {
    queue_stop_events(paths, name, active.supervisor.run_id(), correlation)
  };
  let (stopped, terminal) = active.supervisor.stop_with_terminal();
  if let Some(sender) = pending_event
    && sender
      .try_send((terminal.clone(), failed_candidate))
      .is_err()
  {
    log_store::record_stop_event_loss();
  }
  if terminal.outcome == "unknown" {
    active.last_error = Some("dependent stop unconfirmed; owner recovery required".into());
  }
  (stopped, terminal)
}

pub(super) fn log_prelaunch_failure(
  paths: &config::Paths,
  name: &str,
  error: &anyhow::Error,
  correlation: &str,
) {
  let Some(logs) = log_store::LogStore::new(paths, name).ok() else {
    return;
  };
  let diagnostic = error.chain().find_map(|cause| {
    cause
      .downcast_ref::<graph_compat::Diagnostic>()
      .map(|d| (d.code, d.path.as_str()))
      .or_else(|| {
        cause
          .downcast_ref::<crate::domain::packages::PackageFailure>()
          .map(|d| (d.code, d.path.as_str()))
      })
      .or_else(|| {
        cause
          .downcast_ref::<crate::domain::graph::generated_v2::Diagnostic>()
          .map(|d| (d.code, d.path.as_str()))
      })
      .or_else(|| {
        cause
          .downcast_ref::<config::StopBudgetDiagnostic>()
          .map(|d| (d.code, d.path))
      })
      .or_else(|| {
        cause
          .downcast_ref::<crate::runtime::process::skill_selection::Diagnostic>()
          .map(|d| (d.code, d.path.as_str()))
      })
      .or_else(|| {
        cause
          .downcast_ref::<crate::runtime::owners::control_grants::Diagnostic>()
          .map(|d| (d.code, d.path.as_str()))
      })
      .or_else(|| {
        cause
          .downcast_ref::<crate::runtime::process::vision_binding::Diagnostic>()
          .map(|d| (d.code, d.path.as_str()))
      })
      .or_else(|| {
        cause
          .downcast_ref::<egia_state::Diagnostic>()
          .map(|d| (d.code, d.path.as_str()))
      })
      .or_else(|| {
        cause
          .downcast_ref::<crate::runtime::process::candidate_skill_preflight::Diagnostic>()
          .map(|d| (d.code, ""))
      })
      .or_else(|| {
        cause
          .downcast_ref::<RetirementDiagnostic>()
          .map(|d| (d.code, ""))
      })
  });
  let mut fields = serde_json::Map::new();
  if let Some((code, path)) = diagnostic {
    fields.insert("diagnostic_code".into(), json!(code));
    if path.len() <= 256 && !path.chars().any(char::is_control) {
      fields.insert("diagnostic_path".into(), json!(path));
    }
  }
  if let Some(probe) = error.chain().find_map(|cause| {
    cause.downcast_ref::<crate::runtime::process::candidate_skill_preflight::Diagnostic>()
  }) {
    fields.insert("active_revision".into(), json!(probe.meta.revision));
    fields.insert("generation".into(), json!(probe.meta.generation));
    fields.insert("candidate_run_id".into(), json!(probe.meta.run_id));
    if let Some(stage) = probe.eos_stage {
      fields.insert("eos_stage".into(), json!(stage));
    }
    if let Some(code) = probe.eos_code {
      fields.insert("eos_code".into(), json!(code));
    }
    if let Some(id) = &probe.meta.package_id {
      fields.insert("package_id".into(), json!(id));
    }
    if let Some(version) = &probe.meta.package_version {
      fields.insert("package_version".into(), json!(version));
    }
  }
  let _ = logs.append_event_context(
    "isos",
    "error",
    "deployment_preflight_failed",
    "Deployment preflight failed before launch",
    log_store::LogContext {
      run_id: Some(manager_session_id()),
      correlation_id: Some(correlation),
      node_path: None,
      fields: Value::Object(fields),
    },
  );
}

pub(super) fn log_package_load_failure(
  paths: &config::Paths,
  name: &str,
  error: &anyhow::Error,
  correlation: &str,
) {
  let Some(failure) = error
    .chain()
    .find_map(|cause| cause.downcast_ref::<process::PackageLoadFailure>())
  else {
    return;
  };
  if let Ok(logs) = log_store::LogStore::new(paths, name) {
    let _ = logs.append_event_context(
            "packages", "error", "package_load_failed",
            "Deployment package load failed before launch",
            log_store::LogContext {
                run_id: Some(manager_session_id()),
                correlation_id: Some(correlation),
                node_path: failure.node_path.as_deref(),
                fields: json!({"package_id":failure.id,"package_version":failure.version,"stage":failure.stage,
                    "source_kind":null,"source_commit":null,"asset_sha256":null,"release_sha256":null,"payload_sha256":null}),
            },
        );
  }
}

pub(super) fn log_request_preflight_failure(
  paths: &config::Paths,
  name: &str,
  error: &anyhow::Error,
  correlation: &str,
) {
  log_package_load_failure(paths, name, error, correlation);
  log_prelaunch_failure(paths, name, error, correlation);
}
