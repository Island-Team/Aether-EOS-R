use super::*;

pub(crate) fn api_call(
  paths: &config::Paths,
  managers: &mut Deployments,
  service_manager: &mut services::ServiceManager,
  command: WireCommand,
) -> Result<Value> {
  let name = config::deployment_name(command.deployment().map(str::to_owned));
  if command.op().starts_with("deployment_") {
    process::validate_deployment_name(&name)?;
  }
  match command {
    WireCommand::ManagerIdentity => manager_identity(paths),
    WireCommand::LogAppend(value) => {
      process::validate_deployment_name(&name)?;
      let event = value.log;
      ensure!(
        !matches!(
          event.source.as_str(),
          "all" | "events" | "services" | "isos" | "browser"
        ),
        "reserved log source"
      );
      ensure!(log_store::valid_source(&event.source), "invalid log source");
      ensure!(
        !event.message.is_empty() && event.message.len() <= 8192,
        "log message must be 1..8192 bytes"
      );
      let code = event.event_code.as_deref().unwrap_or("external_event");
      ensure!(
        !code.is_empty()
          && code.len() <= 64
          && code
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'_'),
        "event_code must be 1..64 lowercase letters, digits, or underscores"
      );
      ensure!(
        log_store::level_rank(&event.level).is_some(),
        "log level must be trace, debug, info, warn, or error"
      );
      let correlation = next_correlation_id();
      log_store::LogStore::new(paths, &name)?.append_event_context(
        "legacy_external",
        &event.level,
        "legacy_external_event",
        "Legacy external event received",
        log_store::LogContext {
          run_id: Some(manager_session_id()),
          correlation_id: Some(&correlation),
          node_path: None,
          fields: json!({}),
        },
      )?;
      Ok(json!({"accepted":true}))
    }
    WireCommand::ManagerShutdown => {
      tracing::info!(
        event_code = "manager_shutdown",
        "IsOS manager shutdown requested"
      );
      let mut stopped = Vec::new();
      let mut terminals = Vec::new();
      for (deployment, active) in managers.iter_mut() {
        let (ids, terminal) = stop_active(paths, deployment, active, false);
        stopped.extend(ids);
        terminals.push(terminal);
      }
      let services = service_manager.stop_all();
      INTERRUPTED.store(true, Ordering::SeqCst);
      Ok(json!({"stopped":stopped,"terminal":terminals,"services":services,"manager":"stopping"}))
    }
    WireCommand::ServicesList => Ok(json!({"services":service_manager.list()?})),
    WireCommand::ServicesValidate => Ok(json!({"services":service_manager.validate()?})),
    WireCommand::ServicesStatus => Ok(json!({"services":service_manager.status()?})),
    WireCommand::ServicesStart(value) => {
      Ok(json!({"started":service_manager.start(&value.service_id)?}))
    }
    WireCommand::ServicesStop(value) => {
      Ok(json!({"stopped":service_manager.stop(&value.service_id)?}))
    }
    WireCommand::ServicesRestart(value) => {
      Ok(json!({"started":service_manager.restart(&value.service_id)?}))
    }
    WireCommand::ServicesLogs(value) => service_manager.logs(
      &value.service_id,
      value.cursor.as_deref(),
      value.limit.unwrap_or(100),
    ),
    WireCommand::DeploymentActivePackages(_) => {
      let packages: Vec<_> = managers
        .values()
        .flat_map(|active| active.supervisor.active_packages())
        .collect();
      Ok(json!({"packages":packages}))
    }
    WireCommand::DeploymentRestart(value) => {
      let started_at = Instant::now();
      let mut phase_at = started_at;
      check_replacement_admission(paths, managers, &name)?;
      let path = paths.node_graph_path(&name);
      let source = fs::read_to_string(&path)?;
      let revision = graph_compat::revision(source.as_bytes());
      if let Some(expected) = value.revision.as_deref() {
        ensure!(
          expected == revision,
          "saved graph revision changed; reload before restart"
        );
      }
      phase_timing("restart", "saved_graph", started_at, &mut phase_at);
      verify_selected_before_reader(paths, &name, &source, &revision)?;
      phase_timing("restart", "selected_verify", started_at, &mut phase_at);
      validate_saved_graph(paths, &name, &source)?;
      phase_timing("restart", "graph_validate", started_at, &mut phase_at);
      let specs = preflight_packages(paths, &name, &source, &revision)?;
      phase_timing("restart", "package_preflight", started_at, &mut phase_at);
      core_setup::prepare_run(paths, &name, &revision)?;
      phase_timing("restart", "core_prepare", started_at, &mut phase_at);
      ensure!(
        starter::revision(&path)? == revision,
        "saved graph revision changed during core setup; reload before restart"
      );
      let managed =
        graph_compat::uses_revisioned_generation(&serde_json::from_str::<Value>(&source)?);
      let launch = admitted_plan(revision.clone(), specs, managed)?;
      let started = restart_admitted_processes(paths, managers, &name, &launch)?;
      phase_timing(
        "restart",
        "candidate_probe_and_start",
        started_at,
        &mut phase_at,
      );
      Ok(json!({"started":started,"active_revision":revision}))
    }
    WireCommand::DeploymentStart(value) => {
      let started_at = Instant::now();
      let mut phase_at = started_at;
      check_replacement_admission(paths, managers, &name)?;
      let path = paths.node_graph_path(&name);
      let source = fs::read_to_string(&path)?;
      let revision = graph_compat::revision(source.as_bytes());
      if let Some(expected) = value.revision.as_deref() {
        ensure!(
          expected == revision,
          "saved graph revision changed; reload before run"
        );
      }
      phase_timing("start", "saved_graph", started_at, &mut phase_at);
      verify_selected_before_reader(paths, &name, &source, &revision)?;
      phase_timing("start", "selected_verify", started_at, &mut phase_at);
      validate_saved_graph(paths, &name, &source)?;
      phase_timing("start", "graph_validate", started_at, &mut phase_at);
      let specs = preflight_packages(paths, &name, &source, &revision)?;
      phase_timing("start", "package_preflight", started_at, &mut phase_at);
      core_setup::prepare_run(paths, &name, &revision)?;
      phase_timing("start", "core_prepare", started_at, &mut phase_at);
      ensure!(
        starter::revision(&path)? == revision,
        "saved graph revision changed during core setup; reload before run"
      );
      let managed =
        graph_compat::uses_revisioned_generation(&serde_json::from_str::<Value>(&source)?);
      let launch = admitted_plan(revision.clone(), specs, managed)?;
      let (started, run_id) = start_admitted_processes(paths, managers, &name, &launch)?;
      phase_timing(
        "start",
        "candidate_probe_and_start",
        started_at,
        &mut phase_at,
      );
      Ok(json!({"started":started,"active_revision":revision,"run_id":run_id}))
    }
    WireCommand::DeploymentStop(value) => {
      if let Some(expected) = value.revision.as_deref() {
        ensure!(
          starter::revision(&paths.node_graph_path(&name))? == expected,
          "saved graph revision changed; reload before stop"
        );
      }
      let (stopped, terminal) = if let Some(active) = managers.get_mut(&name) {
        let (ids, terminal) = stop_active(paths, &name, active, false);
        (ids, Some(terminal))
      } else {
        let identity = log_store::LogStore::new(paths, &name)
          .ok()
          .map(|logs| logs.run_identity());
        let terminal = identity.and_then(|(run_id, state)| {
          if state == "known" {
            run_id.map(|run_id| dependent_stop::recover(paths, &name, &run_id))
          } else {
            None
          }
        });
        (Vec::new(), terminal)
      };
      Ok(json!({"stopped": stopped,"terminal":terminal}))
    }
    WireCommand::DeploymentStatus(_) => {
      let started_at = Instant::now();
      let mut phase_at = started_at;
      let saved_revision = starter::revision(&paths.node_graph_path(&name)).ok();
      let pinned_packages: Vec<Value> = fs::read(paths.node_graph_path(&name))
        .ok()
        .and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok())
        .and_then(|graph| crate::domain::packages::collect_pins(&graph).ok())
        .unwrap_or_default()
        .into_iter()
        .map(|pin| json!({"id":pin.id,"version":pin.version,"path":pin.path}))
        .collect();
      phase_timing("status", "saved_graph_snapshot", started_at, &mut phase_at);
      let active = managers.get_mut(&name);
      let processes = if let Some(active) = active {
        match active.supervisor.status() {
          Ok(processes) => {
            if active.supervisor.terminal().is_none() && processes.len() < active.expected_processes
            {
              active.supervisor.mark_unexpected_exit();
              if active.last_error.is_none() {
                log_deployment(
                  paths,
                  &name,
                  active.supervisor.run_id(),
                  "deployment_failed",
                  "error",
                );
              }
              let _ = stop_active(paths, &name, active, false);
              active.last_error = Some("a managed process exited".into());
            }
            active.supervisor.status_snapshot()
          }
          Err(error) => {
            active.last_error = Some(error.to_string());
            let _ = stop_active(paths, &name, active, false);
            active.supervisor.status_snapshot()
          }
        }
      } else {
        Vec::new()
      };
      phase_timing("status", "supervisor_snapshot", started_at, &mut phase_at);
      let active = managers.get(&name);
      let active_revision = active.map(|a| a.active_revision.clone());
      let supervisor_failed = active.is_some_and(|a| a.last_error.is_some());
      let last_error = active
        .and_then(|a| a.last_error.clone())
        .or_else(|| fs::read_to_string(paths.runtime.join(format!("{name}.last-error"))).ok());
      let (run_id, run_id_state) = match log_store::LogStore::new(paths, &name) {
        Ok(logs) => logs.run_identity(),
        Err(_) => (None, "unavailable"),
      };
      let terminal = active
        .and_then(|active| active.supervisor.terminal())
        .or_else(|| {
          if active.is_none() && run_id_state == "known" {
            run_id
              .as_deref()
              .map(|id| dependent_stop::recover(paths, &name, id))
          } else {
            None
          }
        });
      let last_error = if active.is_none()
        && terminal
          .as_ref()
          .is_some_and(|value| value.outcome == "confirmed")
        && last_error.as_deref().is_some_and(|value| {
          value.starts_with("deployment_stop_unconfirmed:")
            || value == "dependent stop unconfirmed; owner recovery required"
        }) {
        None
      } else {
        last_error
      };
      let state = if run_id_state == "unavailable"
        || supervisor_failed
        || terminal
          .as_ref()
          .is_some_and(|value| value.outcome == "unknown")
      {
        "degraded"
      } else if active.is_none() || terminal.is_some() {
        "stopped"
      } else {
        "running"
      };
      let last_error = if active.is_none()
                && terminal
                    .as_ref()
                    .is_some_and(|value| value.reason_code == Some("stop_interrupted"))
            {
                Some("Previous run's stop proof is unavailable after manager restart; owner-reviewed physical-stop recovery is required before launch".into())
            } else {
                last_error
            }
            .or_else(|| {
                if state == "degraded" {
                    Some("dependent stop unconfirmed; owner recovery required".into())
                } else {
                    None
                }
            });
      let vision_evidence = current_vision_projection(active, run_id.as_deref(), state);
      let (log_ingest_rejected, log_write_failed) = log_store::counters();
      phase_timing("status", "scope_and_projection", started_at, &mut phase_at);
      Ok(
        json!({"schema_version":"0.1.0","saved_revision":saved_revision,"active_revision":active_revision,"state":state,"last_error":last_error,"packages":pinned_packages,"processes":processes,"run_id":run_id,"run_id_state":run_id_state,"terminal":terminal,"vision_evidence":vision_evidence,"log_ingest_rejected":log_ingest_rejected,"log_ingest_rejected_by_reason":log_store::rejection_reasons(),"log_write_failed":log_write_failed}),
      )
    }
  }
}
