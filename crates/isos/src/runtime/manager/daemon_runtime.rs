use super::*;

pub(super) fn poll_deployments(paths: &config::Paths, managers: &mut Deployments) {
  for (name, active) in managers.iter_mut() {
    // A completed external Stop intentionally removes every child.  Its
    // live terminal result is authoritative for the attached run loop;
    // empty process snapshots must not be reclassified as provider loss.
    if active.supervisor.terminal().is_some() {
      continue;
    }
    let failure = if let Err(error) = active.supervisor.poll() {
      Some(error.to_string())
    } else if active.supervisor.status_snapshot().len() < active.expected_processes {
      active.supervisor.mark_unexpected_exit();
      Some("a managed process exited".to_owned())
    } else {
      None
    };
    if let Some(error) = failure {
      if active.last_error.is_none() {
        log_deployment(
          paths,
          name,
          active.supervisor.run_id(),
          "deployment_failed",
          "error",
        );
      }
      let _ = stop_active(paths, name, active, false);
      active.last_error = Some(error.clone());
      tracing::error!(
        event_code = "deployment_supervisor_failed",
        "process supervisor: {error}"
      );
      eprintln!("process supervisor: {error}");
    }
  }
}

pub(crate) fn daemon(
  paths: &config::Paths,
  deployment: &str,
  listener: UnixListener,
) -> Result<()> {
  // A manager session boundary is explicit; one-shot CLI reads must not
  // silently rotate the pre-run scope between commands.
  log_store::LogStore::new(paths, deployment)?.begin_manager_session()?;
  let mut managers = Deployments::new();
  let mut service_manager = services::ServiceManager::new(paths, deployment)?;
  println!("IsOS API listening on {}", paths.socket().display());
  while !INTERRUPTED.load(Ordering::SeqCst) {
    poll_deployments(paths, &mut managers);
    if let Err(error) = service_manager.poll() {
      tracing::error!(
        event_code = "service_supervisor_failed",
        "service supervisor: {error:#}"
      );
      eprintln!("service supervisor: {error:#}");
    }
    match listener.accept() {
      Ok((client, _)) => {
        if let Err(error) = handle_client(paths, &mut managers, &mut service_manager, client) {
          tracing::warn!(
            event_code = "manager_client_failed",
            "API client: {error:#}"
          );
          eprintln!("API client: {error}");
        }
      }
      Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
        std::thread::sleep(Duration::from_millis(50));
      }
      Err(error) => {
        tracing::error!(
          event_code = "manager_accept_failed",
          "API accept: {error:#}"
        );
        eprintln!("API accept: {error}");
      }
    }
  }
  let mut unknown = false;
  for (deployment, active) in managers.iter_mut() {
    let (_, terminal) = stop_active(paths, deployment, active, false);
    unknown |= terminal.outcome == "unknown";
  }
  service_manager.stop_all();
  tracing::info!(event_code = "manager_stopped", "IsOS manager stopped");
  fs::remove_file(paths.socket())?;
  ensure!(
    !unknown,
    "dependent stop unconfirmed during manager shutdown"
  );
  Ok(())
}
