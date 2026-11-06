use crate::app::{config, foreground::INTERRUPTED, server as http};
use crate::domain::{
  graph::{graph_compat, graph_migration, starter},
  services,
};
use crate::runtime::{
  logs as log_store,
  owners::{core_setup, dependent_stop, egia_state},
  process::{self, reader_features},
};
use crate::interfaces::manager::*;
use anyhow::{Context, Result, bail, ensure};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
  collections::{BTreeMap, BTreeSet},
  fs,
  io::{BufRead, BufReader, Read, Write},
  os::unix::{
    fs::PermissionsExt,
    net::{UnixListener, UnixStream},
  },
  sync::{
    Mutex, OnceLock,
    atomic::{AtomicU64, Ordering},
    mpsc::{self, Receiver, SyncSender},
  },
  time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

pub(crate) fn monotonic_millis() -> u64 {
  let mut clock = libc::timespec {
    tv_sec: 0,
    tv_nsec: 0,
  };
  // CLOCK_MONOTONIC is shared across local processes, so a CLI send and
  // manager accept can be compared without logging wall time or secrets.
  let ok = unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut clock) == 0 };
  if !ok {
    return 0;
  }
  (clock.tv_sec as u64)
    .saturating_mul(1000)
    .saturating_add((clock.tv_nsec as u64) / 1_000_000)
}

pub(crate) fn emit_phase_timing(op: &str, phase: &str, phase_ms: u64, elapsed_ms: u64) {
  if std::env::var_os("ISOS_PHASE_TIMING").is_none() {
    return;
  }
  eprintln!(
    "q03_phase_timing operation={op} phase={phase} phase_ms={phase_ms} elapsed_ms={elapsed_ms} monotonic_ms={}",
    monotonic_millis()
  );
}

fn phase_timing(op: &str, phase: &'static str, started: Instant, previous: &mut Instant) {
  let now = Instant::now();
  emit_phase_timing(
    op,
    phase,
    now.duration_since(*previous).as_millis() as u64,
    now.duration_since(started).as_millis() as u64,
  );
  *previous = now;
}

pub(crate) struct ActiveDeployment {
  active_revision: String,
  managed: bool,
  supervisor: process::Supervisor,
  last_error: Option<String>,
  expected_processes: usize,
  vision: Option<VisionSnapshot>,
}

#[derive(Clone)]
struct VisionSnapshot {
  run_id: String,
  active_revision: String,
  projection: Value,
}

struct RunAdmission {
  managed: bool,
}

type DeploymentPlan = crate::domain::launch::plan::LaunchPlan<
  process::ProcessSpec,
  crate::domain::graph::generated_v2::SecretPlan,
>;

fn admitted_plan(
  revision: String,
  specs: Vec<process::ProcessSpec>,
  managed: bool,
) -> Result<DeploymentPlan> {
  let secrets = crate::domain::graph::generated_v2::secret_plan(&specs)?;
  let ownership = if managed {
    crate::domain::launch::plan::Ownership::Managed
  } else {
    crate::domain::launch::plan::Ownership::Legacy
  };
  DeploymentPlan::new(revision, specs, secrets, ownership)
}

fn restart_admitted_processes(
  paths: &config::Paths,
  managers: &mut Deployments,
  name: &str,
  plan: &DeploymentPlan,
) -> Result<Vec<String>> {
  restart_processes_with_plan(
    paths,
    managers,
    name,
    plan.revision(),
    plan.specs(),
    plan.secrets(),
    RunAdmission {
      managed: plan.ownership() == crate::domain::launch::plan::Ownership::Managed,
    },
  )
}

fn start_admitted_processes(
  paths: &config::Paths,
  managers: &mut Deployments,
  name: &str,
  plan: &DeploymentPlan,
) -> Result<(Vec<String>, String)> {
  start_prepared_processes(
    paths,
    managers,
    name,
    plan.revision(),
    plan.specs(),
    plan.secrets(),
    RunAdmission {
      managed: plan.ownership() == crate::domain::launch::plan::Ownership::Managed,
    },
  )
}

fn vision_snapshot(
  run_id: &str,
  revision: &str,
  projection: Option<Value>,
) -> Option<VisionSnapshot> {
  projection.map(|projection| VisionSnapshot {
    run_id: run_id.to_owned(),
    active_revision: revision.to_owned(),
    projection,
  })
}

fn admitted_vision_snapshot(
  supervisor: &process::Supervisor,
  revision: &str,
) -> Option<VisionSnapshot> {
  vision_snapshot(
    supervisor.run_id(),
    revision,
    supervisor
      .admitted_vision_template()
      .map(crate::runtime::process::vision_binding::projection),
  )
}

fn recheck_admitted_vision_template(
  paths: &config::Paths,
  name: &str,
  revision: &str,
  supervisor: &process::Supervisor,
) -> Result<()> {
  #[cfg(test)]
  if TEST_EMPTY_SELECTED_TEMPLATE.with(std::cell::Cell::get) {
    return compare_admitted_vision_template(supervisor, || Ok(None));
  }
  compare_admitted_vision_template(supervisor, || {
    Ok(crate::runtime::process::vision_binding::selected_template(
      paths, name, revision,
    )?)
  })
}

#[cfg(test)]
thread_local! {
    static TEST_EMPTY_SELECTED_TEMPLATE: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

fn compare_admitted_vision_template(
  supervisor: &process::Supervisor,
  selected: impl FnOnce() -> Result<Option<crate::runtime::process::vision_binding::Template>>,
) -> Result<()> {
  ensure!(
    selected()?.as_ref() == supervisor.admitted_vision_template(),
    "selected Vision template changed during admission"
  );
  Ok(())
}

fn recheck_candidate_vision_or_retire(
  paths: &config::Paths,
  name: &str,
  revision: &str,
  managed: bool,
  supervisor: &mut process::Supervisor,
) -> Result<()> {
  if !managed {
    return Ok(());
  }
  if let Err(error) = recheck_admitted_vision_template(paths, name, revision, supervisor) {
    if supervisor.retire_candidate_grants().is_err() {
      let run_id = supervisor.run_id().to_owned();
      supervisor.detach_candidate_grants();
      block_candidate_cleanup(paths, name, &run_id);
      bail!("selected Vision template changed and candidate cleanup is unconfirmed");
    }
    return Err(error);
  }
  Ok(())
}

fn current_vision_projection(
  active: Option<&ActiveDeployment>,
  run_id: Option<&str>,
  state: &str,
) -> Option<Value> {
  if state != "running" {
    return None;
  }
  let active = active?;
  let vision = active.vision.as_ref()?;
  (run_id == Some(vision.run_id.as_str())
    && active.supervisor.run_id() == vision.run_id
    && active.active_revision == vision.active_revision)
    .then(|| vision.projection.clone())
}
pub(crate) type Deployments = BTreeMap<String, ActiveDeployment>;

#[cfg(test)]
pub(crate) fn test_set_vision_projection(
  managers: &mut Deployments,
  name: &str,
  projection: Value,
) -> String {
  let active = managers.get_mut(name).expect("test deployment exists");
  let run_id = active.supervisor.run_id().to_owned();
  active.vision = vision_snapshot(&run_id, &active.active_revision, Some(projection));
  run_id
}

#[cfg(test)]
pub(crate) fn test_admit_vision_template(
  managers: &mut Deployments,
  name: &str,
  template: crate::runtime::process::vision_binding::Template,
) -> String {
  let active = managers.get_mut(name).expect("test deployment exists");
  let run_id = active.supervisor.run_id().to_owned();
  assert_eq!(template.graph_revision, active.active_revision);
  active
    .supervisor
    .prepare_control_grants(&active.active_revision)
    .unwrap();
  active
    .supervisor
    .attach_verified_vision_template(template)
    .unwrap();
  active.vision = admitted_vision_snapshot(&active.supervisor, &active.active_revision);
  run_id
}

#[derive(Debug)]
pub(crate) struct ScopePublishFailure;

impl std::fmt::Display for ScopePublishFailure {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    f.write_str("log scope publication failed")
  }
}
impl std::error::Error for ScopePublishFailure {}

fn publish_run_scope(supervisor: &process::Supervisor) -> Result<()> {
  supervisor
    .publish_run_scope()
    .map_err(|_| anyhow::Error::new(ScopePublishFailure))
}

fn ensure_launch_history(paths: &config::Paths, name: &str) -> Result<()> {
  let (run_id, state) = log_store::LogStore::new(paths, name)?.run_identity();
  if state == "known" {
    let Some(run_id) = run_id else {
      return Err(anyhow::Error::new(RetirementDiagnostic {
        code: "deployment_stop_unconfirmed",
      }));
    };
    if dependent_stop::recover(paths, name, &run_id).outcome != "confirmed" {
      return Err(anyhow::Error::new(RetirementDiagnostic {
        code: "deployment_stop_unconfirmed",
      }));
    }
    return Ok(());
  }
  ensure!(state == "none", "previous run identity is unavailable");
  Ok(())
}

fn check_replacement_admission(
  paths: &config::Paths,
  managers: &Deployments,
  name: &str,
) -> Result<()> {
  check_candidate_cleanup_marker(
    paths,
    name,
    managers.get(name).map(|active| active.supervisor.run_id()),
  )?;
  crate::runtime::owners::control_grants::check_endpoint_orphans(
    paths,
    managers.iter().map(|(deployment, active)| {
      (
        deployment.as_str(),
        active.supervisor.run_id(),
        active.active_revision.as_str(),
      )
    }),
  )?;
  if managers
    .get(name)
    .and_then(|active| active.supervisor.terminal())
    .is_some_and(|terminal| terminal.outcome == "unknown")
  {
    return Err(anyhow::Error::new(RetirementDiagnostic {
      code: "deployment_stop_unconfirmed",
    }));
  }
  if !managers.contains_key(name) {
    ensure_launch_history(paths, name)?;
  }
  Ok(())
}

pub(crate) fn manager_session_id() -> &'static str {
  static SESSION: OnceLock<String> = OnceLock::new();
  SESSION.get_or_init(|| {
    format!(
      "m{}-{:x}",
      std::process::id(),
      SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos()
    )
  })
}

pub(crate) fn next_correlation_id() -> String {
  static NEXT_CORRELATION: AtomicU64 = AtomicU64::new(1);
  format!("p{}", NEXT_CORRELATION.fetch_add(1, Ordering::Relaxed))
}

pub(crate) fn next_run_id() -> String {
  static NEXT_RUN: AtomicU64 = AtomicU64::new(1);
  format!(
    "r{}-{}",
    manager_session_id().trim_start_matches('m'),
    NEXT_RUN.fetch_add(1, Ordering::Relaxed)
  )
}

std::thread_local! {
    static RUN_STARTED_IN_REQUEST: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
    static REQUEST_CORRELATION: std::cell::RefCell<Option<String>> = const { std::cell::RefCell::new(None) };
}

fn preflight_packages(
  paths: &config::Paths,
  name: &str,
  source: &str,
  revision: &str,
) -> Result<Vec<process::ProcessSpec>> {
  let graph: Value = serde_json::from_str(source)?;
  crate::runtime::process::skill_selection::check_reserved(&graph)?;
  let selected_libraries = crate::runtime::process::skill_selection::entries(&graph)?;
  if !selected_libraries.is_empty() {
    crate::domain::packages::verify_graph_packages(paths, &graph)?;
    crate::runtime::process::skill_selection::render(paths, &graph, revision)?;
  }
  let selected = if graph_compat::uses_revisioned_generation(&graph) {
    Some(crate::domain::graph::generated_v2::selected_v2_root(
      paths, name, &graph, revision,
    )?)
  } else {
    None
  };
  reader_features::check_setup_basics(paths, &graph)?;
  let mut specs = process::project_with_packages(paths, source)?;
  if graph_compat::uses_revisioned_generation(&graph) {
    egia_state::validate_authored(&specs)?;
  }
  core_setup::verify_launch_inputs(paths, name)?;
  if let Some(selected) = selected {
    crate::runtime::owners::control_grants::validate_plan(&specs)?;
    let generated = selected.join("eos/graph.json");
    process::use_generated_eos_profile(&mut specs, &generated)?;
    let ei_dir = if graph["version"] == "0.2.0" {
      "ei"
    } else {
      "egia"
    };
    process::use_generated_egia_config(&mut specs, &selected.join(ei_dir))?;
    egia_state::prepare(paths, name, &mut specs)?;
    let skill_file = crate::runtime::process::skill_selection::compare_selected(
      paths, &graph, revision, &selected,
    )?;
    crate::runtime::process::skill_selection::apply_env(&mut specs, skill_file.as_deref());
  }
  let catalog = process::catalog_for_graph(paths, &graph)?;
  reader_features::check(paths, &graph, &specs, &catalog)?;
  if graph_compat::uses_revisioned_generation(&graph)
    && let Some(eos) = specs.iter().find(|spec| spec.id == "eos")
  {
    let output = std::process::Command::new(&eos.settings.command[0])
      .arg("--capabilities")
      .output()?;
    ensure!(
      output.status.success() && output.stdout.len() <= 65_536,
      "selected EOS managed stop support is unavailable"
    );
    let capabilities: Value = serde_json::from_slice(&output.stdout)?;
    ensure!(
      capabilities["features"]
        .as_array()
        .is_some_and(|features| features
          .iter()
          .any(|value| value.as_str() == Some("managed-stop-v1"))),
      "selected EOS lacks managed-stop-v1"
    );
  }
  Ok(specs)
}

fn validate_saved_graph(paths: &config::Paths, name: &str, source: &str) -> Result<()> {
  let saved = graph_compat::parse(source.as_bytes())?;
  if graph_compat::uses_revisioned_generation(&saved.value) {
    let proposal = graph_migration::Proposal {
      graph: saved.value,
      bytes: source.as_bytes().to_vec(),
      revision: saved.revision,
      changes: Vec::new(),
    };
    graph_migration::prepare(paths, name, &proposal).map_err(anyhow::Error::new)?;
    Ok(())
  } else {
    crate::domain::packages::verify_graph_packages(paths, &saved.value)?;
    graph_compat::admit_legacy_mapped(&saved.value, process::catalog().ok().as_ref())?;
    process::validate_full_graph(&paths.node_graph_path(name))
  }
}

fn verify_selected_before_reader(
  paths: &config::Paths,
  name: &str,
  source: &str,
  revision: &str,
) -> Result<()> {
  let started_at = Instant::now();
  let mut phase_at = started_at;
  let saved = graph_compat::parse(source.as_bytes())?;
  if graph_compat::uses_revisioned_generation(&saved.value) {
    crate::runtime::process::skill_selection::check_reserved(&saved.value)?;
    // Keep the exact pin's path and payload diagnostic when a package has
    // changed after Apply. The generated snapshot reports only a global
    // missing input if it checks package inventory first.
    crate::domain::packages::verify_graph_packages(paths, &saved.value)?;
    phase_timing(
      "selected_verify",
      "package_bytes",
      started_at,
      &mut phase_at,
    );
    if !crate::runtime::process::skill_selection::entries(&saved.value)?.is_empty() {
      crate::runtime::process::skill_selection::render(paths, &saved.value, revision)?;
    }
    phase_timing(
      "selected_verify",
      "skill_selection",
      started_at,
      &mut phase_at,
    );
    crate::domain::graph::generated_v2::selected_v2_root(paths, name, &saved.value, revision)?;
    phase_timing(
      "selected_verify",
      "generated_snapshot",
      started_at,
      &mut phase_at,
    );
  }
  Ok(())
}

#[cfg(test)]
pub(crate) fn restart_processes(
  paths: &config::Paths,
  managers: &mut Deployments,
  name: &str,
  revision: &str,
  specs: &[process::ProcessSpec],
) -> Result<Vec<String>> {
  let plan = crate::domain::graph::generated_v2::secret_plan(specs)?;
  restart_processes_with_plan(
    paths,
    managers,
    name,
    revision,
    specs,
    &plan,
    RunAdmission { managed: false },
  )
}

fn restart_processes_with_plan(
  paths: &config::Paths,
  managers: &mut Deployments,
  name: &str,
  revision: &str,
  specs: &[process::ProcessSpec],
  plan: &crate::domain::graph::generated_v2::SecretPlan,
  admission: RunAdmission,
) -> Result<Vec<String>> {
  let RunAdmission { managed } = admission;
  let started_at = Instant::now();
  let mut phase_at = started_at;
  check_replacement_admission(paths, managers, name)?;
  if managed {
    crate::runtime::owners::control_grants::validate_plan(specs)?;
  }
  if let Some(active) = managers.get(name) {
    active
      .supervisor
      .verify_control_grants(active.managed && active.last_error.is_none())?;
  }
  let old_plan = managers
    .get(name)
    .map(|active| crate::domain::graph::generated_v2::secret_plan(&active.supervisor.specs()))
    .transpose()?;
  let candidate_inputs = if managed {
    crate::runtime::process::candidate_skill_preflight::Inputs::collect(
      paths, name, revision, specs, plan,
    )?
  } else {
    None
  };
  phase_timing("restart", "candidate_inputs", started_at, &mut phase_at);
  let selected_replacement = candidate_inputs.is_some();
  let mut supervisor = process::Supervisor::new(paths, name)?;
  if managed {
    supervisor.prepare_control_grants(revision)?;
    supervisor.prepare_vision(revision)?;
  }
  let candidate_run = supervisor.run_id().to_owned();
  probe_candidate(
    paths,
    name,
    revision,
    specs,
    plan,
    candidate_inputs,
    &mut supervisor,
  )?;
  recheck_candidate_vision_or_retire(paths, name, revision, managed, &mut supervisor)?;
  phase_timing("restart", "candidate_probe", started_at, &mut phase_at);
  let previous =
    stop_old_for_replacement(paths, managers, name, &mut supervisor, selected_replacement)?;
  // Replacing a run is ordered after the previous children have been reaped.
  // The new scope is durable before any replacement child can start.
  publish_run_scope(&supervisor)?;
  supervisor.prepare_run(specs, revision)?;
  phase_timing("restart", "candidate_prepare", started_at, &mut phase_at);
  log_deployment(paths, name, &candidate_run, "deployment_starting", "info");
  let attempt = (|| -> Result<Vec<String>> {
    let started = supervisor.start_with_secrets(specs, plan)?;
    std::thread::sleep(Duration::from_millis(100));
    let running = supervisor.status()?;
    ensure!(
      running.len() == specs.len(),
      "a managed process exited during restart; inspect categorized child logs"
    );
    Ok(started)
  })();
  let started = match attempt {
    Ok(started) => started,
    Err(error) => {
      let vision = admitted_vision_snapshot(&supervisor, revision);
      let mut failed = ActiveDeployment {
        active_revision: revision.to_owned(),
        managed,
        supervisor,
        last_error: None,
        expected_processes: specs.len(),
        vision,
      };
      let (_, rollback) = stop_active(paths, name, &mut failed, true);
      if rollback.outcome == "unknown" {
        managers.insert(name.to_owned(), failed);
        bail!("candidate stop unconfirmed; previous generation cannot be restored");
      }
      if let Some((mut active, old_specs, _)) = previous {
        let recovery_run = next_run_id();
        active.supervisor.set_run_id(recovery_run.clone());
        if let Err(recovery_error) = if active.managed {
          active
            .supervisor
            .prepare_control_grants(&active.active_revision)
            .and_then(|()| active.supervisor.prepare_vision(&active.active_revision))
        } else {
          Ok(())
        } {
          log_deployment(paths, name, &recovery_run, "deployment_failed", "error");
          active.last_error = Some(format!(
            "restart failed: {error:#}; recovery grant preparation failed: {recovery_error}"
          ));
          managers.insert(name.to_owned(), active);
          bail!("restart failed: {error:#}; recovery grant preparation failed: {recovery_error}");
        }
        active.vision = admitted_vision_snapshot(&active.supervisor, &active.active_revision);
        publish_run_scope(&active.supervisor)?;
        active
          .supervisor
          .prepare_run(&old_specs, &active.active_revision)?;
        log_deployment(paths, name, &recovery_run, "deployment_starting", "info");
        let empty = crate::domain::graph::generated_v2::SecretPlan::new();
        let restore = active
          .supervisor
          .start_with_secrets(&old_specs, old_plan.as_ref().unwrap_or(&empty))
          .and_then(|_| {
            std::thread::sleep(Duration::from_millis(100));
            ensure!(
              active.supervisor.status()?.len() == old_specs.len(),
              "a previous child exited during restoration"
            );
            Ok(())
          });
        match restore {
          Ok(_) => {
            active.last_error = None;
            log_deployment(paths, name, &recovery_run, "deployment_ready", "info");
            managers.insert(name.to_owned(), active);
            bail!("restart failed; previous revision restored: {error:#}");
          }
          Err(restore_error) => {
            log_deployment(paths, name, &recovery_run, "deployment_failed", "error");
            active.last_error = Some(format!(
              "restart failed: {error:#}; previous revision could not be restored: {restore_error:#}"
            ));
            managers.insert(name.to_owned(), active);
            bail!(
              "restart failed: {error:#}; previous revision could not be restored: {restore_error:#}"
            );
          }
        }
      }
      return Err(error);
    }
  };
  phase_timing("restart", "children_ready", started_at, &mut phase_at);
  log_deployment(paths, name, &candidate_run, "deployment_ready", "info");
  let vision = admitted_vision_snapshot(&supervisor, revision);
  managers.insert(
    name.to_owned(),
    ActiveDeployment {
      active_revision: revision.to_owned(),
      managed,
      supervisor,
      last_error: None,
      expected_processes: specs.len(),
      vision,
    },
  );
  Ok(started)
}

fn stop_old_for_replacement(
  paths: &config::Paths,
  managers: &mut Deployments,
  name: &str,
  candidate: &mut process::Supervisor,
  selected_replacement: bool,
) -> Result<
  Option<(
    ActiveDeployment,
    Vec<process::ProcessSpec>,
    dependent_stop::Terminal,
  )>,
> {
  let stopped = managers.get_mut(name).map(|active| {
    let old_specs = active.supervisor.specs();
    let correlation = selected_replacement
      .then(|| REQUEST_CORRELATION.with(|current| current.borrow().clone()))
      .flatten();
    let (_, terminal) =
      stop_active_with_correlation(paths, name, active, false, correlation.as_deref());
    (old_specs, terminal)
  });
  if stopped
    .as_ref()
    .is_some_and(|(_, terminal)| terminal.outcome == "unknown")
  {
    if candidate.retire_candidate_grants().is_err() {
      candidate.detach_candidate_grants();
      block_candidate_cleanup(paths, name, candidate.run_id());
      return Err(anyhow::Error::new(RetirementDiagnostic {
        code: "skill_catalog_preflight_cleanup_unconfirmed",
      }));
    }
    return Err(anyhow::Error::new(RetirementDiagnostic {
      code: "deployment_stop_unconfirmed",
    }));
  }
  Ok(
    managers
      .remove(name)
      .zip(stopped)
      .map(|(active, (old_specs, terminal))| (active, old_specs, terminal)),
  )
}

fn check_candidate_cleanup_marker(
  paths: &config::Paths,
  name: &str,
  active_run: Option<&str>,
) -> Result<()> {
  let cleanup_error = || {
    anyhow::Error::new(RetirementDiagnostic {
      code: "skill_catalog_preflight_cleanup_unconfirmed",
    })
  };
  let blocked = candidate_cleanup_blocks()
    .lock()
    .map_err(|_| cleanup_error())?;
  if blocked.contains(
    &paths
      .runtime
      .join(format!("{name}.skill-candidate-cleanup-unconfirmed")),
  ) || paths
    .runtime
    .join(format!("{name}.skill-candidate-cleanup-unconfirmed"))
    .exists()
    || paths
      .data
      .join(format!("{name}.skill-candidate-cleanup-unconfirmed"))
      .exists()
  {
    return Err(cleanup_error());
  }
  let bundle_root = paths.runtime.join("control-grants").join(name);
  let entries = fs::read_dir(&bundle_root);
  #[cfg(test)]
  let entries = if FAIL_CANDIDATE_INSPECTION.with(|fail| fail.get()) == 1 {
    Err(std::io::Error::other(
      "injected grant-root inspection failure",
    ))
  } else {
    entries
  };
  match entries {
    Ok(entries) => {
      for entry in entries {
        #[cfg(test)]
        let entry = if FAIL_CANDIDATE_INSPECTION.with(|fail| fail.get()) == 2 {
          Err(std::io::Error::other(
            "injected grant-entry inspection failure",
          ))
        } else {
          entry
        };
        let entry = entry.map_err(|_| cleanup_error())?;
        let file_type = entry.file_type();
        #[cfg(test)]
        let file_type = if FAIL_CANDIDATE_INSPECTION.with(|fail| fail.get()) == 3 {
          Err(std::io::Error::other(
            "injected grant-type inspection failure",
          ))
        } else {
          file_type
        };
        if entry.file_name().to_str() != active_run
          || !file_type.map_err(|_| cleanup_error())?.is_dir()
        {
          return Err(cleanup_error());
        }
      }
    }
    Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
    Err(_) => return Err(cleanup_error()),
  }
  Ok(())
}

fn candidate_cleanup_blocks() -> &'static Mutex<BTreeSet<std::path::PathBuf>> {
  static BLOCKED: OnceLock<Mutex<BTreeSet<std::path::PathBuf>>> = OnceLock::new();
  BLOCKED.get_or_init(|| Mutex::new(BTreeSet::new()))
}

#[cfg(test)]
thread_local! {
    static FAIL_CANDIDATE_MARKER_WRITE: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
    static FAIL_CANDIDATE_FALLBACK_WRITE: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
    static FORCE_PROBE_CLEANUP_UNCONFIRMED: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
    static FAIL_CANDIDATE_INSPECTION: std::cell::Cell<u8> = const { std::cell::Cell::new(0) };
}

fn persist_candidate_cleanup_marker(marker: &std::path::Path) -> Result<()> {
  #[cfg(test)]
  if FAIL_CANDIDATE_MARKER_WRITE.with(|fail| fail.get()) {
    bail!("candidate cleanup marker persistence failed");
  }
  crate::app::config::atomic_write(marker, b"candidate cleanup requires repair\n")
}

fn persist_candidate_fallback_marker(marker: &std::path::Path) -> Result<()> {
  #[cfg(test)]
  if FAIL_CANDIDATE_FALLBACK_WRITE.with(|fail| fail.get()) {
    bail!("candidate cleanup fallback marker persistence failed");
  }
  crate::app::config::atomic_write(marker, b"candidate cleanup requires repair\n")
}

fn block_candidate_cleanup(paths: &config::Paths, name: &str, run_id: &str) {
  let marker = paths
    .runtime
    .join(format!("{name}.skill-candidate-cleanup-unconfirmed"));
  if let Ok(mut blocked) = candidate_cleanup_blocks().lock() {
    blocked.insert(marker.clone());
  }
  if persist_candidate_cleanup_marker(&marker).is_err() {
    tracing::error!(
      event_code = "skill_catalog_preflight_cleanup_unconfirmed",
      "candidate cleanup marker persistence failed"
    );
    let fallback = paths
      .data
      .join(format!("{name}.skill-candidate-cleanup-unconfirmed"));
    if persist_candidate_fallback_marker(&fallback).is_err() {
      tracing::error!(
        event_code = "skill_catalog_preflight_cleanup_unconfirmed",
        "candidate cleanup fallback marker persistence failed"
      );
      let orphan = paths.runtime.join("control-grants").join(name).join(run_id);
      if !orphan.exists() {
        let _ = fs::create_dir(&orphan);
      }
    }
  }
}

fn probe_candidate(
  paths: &config::Paths,
  name: &str,
  revision: &str,
  specs: &[process::ProcessSpec],
  plan: &crate::domain::graph::generated_v2::SecretPlan,
  inputs: Option<crate::runtime::process::candidate_skill_preflight::Inputs>,
  supervisor: &mut process::Supervisor,
) -> Result<()> {
  let Some(inputs) = inputs else {
    return Ok(());
  };
  let candidate_run = supervisor.run_id().to_owned();
  let probe = supervisor.candidate_verifier().and_then(|verifier| {
    crate::runtime::process::candidate_skill_preflight::run(&inputs, &candidate_run, &verifier)
      .map_err(anyhow::Error::new)
  });
  let gate = probe.and_then(|()| {
    let fresh = preflight_packages(
      paths,
      name,
      &fs::read_to_string(paths.node_graph_path(name))?,
      revision,
    )?;
    inputs.recheck(paths, name, &fresh, plan)?;
    supervisor.verify_control_grants(true)?;
    ensure!(
      format!("{specs:?}") == format!("{fresh:?}"),
      "selected process specs changed"
    );
    Ok(())
  });
  if let Err(error) = gate {
    let unconfirmed = error.chain().any(|cause| {
      cause
        .downcast_ref::<crate::runtime::process::candidate_skill_preflight::Diagnostic>()
        .is_some_and(|d| d.code == "skill_catalog_preflight_cleanup_unconfirmed")
    });
    #[cfg(test)]
    let unconfirmed = unconfirmed || FORCE_PROBE_CLEANUP_UNCONFIRMED.with(|force| force.get());
    let retirement = if unconfirmed {
      supervisor.revoke_candidate_grants_keep_directory()
    } else {
      supervisor.retire_candidate_grants()
    };
    if retirement.is_err() || unconfirmed {
      supervisor.detach_candidate_grants();
      block_candidate_cleanup(paths, name, &candidate_run);
      return Err(anyhow::Error::new(inputs.diagnostic(
        &candidate_run,
        "skill_catalog_preflight_cleanup_unconfirmed",
        None,
      )));
    }
    return Err(error);
  }
  supervisor.set_candidate_skill_inputs(inputs);
  Ok(())
}

fn start_prepared_processes(
  paths: &config::Paths,
  managers: &mut Deployments,
  name: &str,
  revision: &str,
  specs: &[process::ProcessSpec],
  plan: &crate::domain::graph::generated_v2::SecretPlan,
  admission: RunAdmission,
) -> Result<(Vec<String>, String)> {
  let RunAdmission { managed } = admission;
  let started_at = Instant::now();
  let mut phase_at = started_at;
  check_replacement_admission(paths, managers, name)?;
  let live_confirmed = managers
    .get(name)
    .and_then(|active| active.supervisor.terminal())
    .is_some_and(|terminal| terminal.outcome == "confirmed");
  if managed {
    crate::runtime::owners::control_grants::validate_plan(specs)?;
  }
  if managers
    .get(name)
    .and_then(|active| active.supervisor.terminal())
    .is_some()
  {
    let previous = managers.remove(name).expect("terminal run exists");
    if previous
      .supervisor
      .terminal()
      .is_some_and(|terminal| terminal.outcome == "unknown")
    {
      managers.insert(name.to_owned(), previous);
      return Err(anyhow::Error::new(RetirementDiagnostic {
        code: "deployment_stop_unconfirmed",
      }));
    }
  }
  if let Some(active) = managers.get_mut(name) {
    ensure!(
      active.managed == managed,
      "saved graph management mode changed; restart required"
    );
    ensure!(
      active.active_revision == revision,
      "saved graph changed while running; stop before restart"
    );
    active.supervisor.verify_specs(specs)?;
  } else {
    if !live_confirmed {
      ensure_launch_history(paths, name)?;
    }
    let candidate_inputs = if managed {
      crate::runtime::process::candidate_skill_preflight::Inputs::collect(
        paths, name, revision, specs, plan,
      )?
    } else {
      None
    };
    phase_timing("start", "candidate_inputs", started_at, &mut phase_at);
    let supervisor = process::Supervisor::new(paths, name)?;
    let mut supervisor = supervisor;
    if managed {
      supervisor.prepare_control_grants(revision)?;
      supervisor.prepare_vision(revision)?;
    }
    probe_candidate(
      paths,
      name,
      revision,
      specs,
      plan,
      candidate_inputs,
      &mut supervisor,
    )?;
    recheck_candidate_vision_or_retire(paths, name, revision, managed, &mut supervisor)?;
    phase_timing("start", "candidate_probe", started_at, &mut phase_at);
    publish_run_scope(&supervisor)?;
    supervisor.prepare_run(specs, revision)?;
    phase_timing("start", "candidate_prepare", started_at, &mut phase_at);
    log_deployment(
      paths,
      name,
      supervisor.run_id(),
      "deployment_starting",
      "info",
    );
    let vision = admitted_vision_snapshot(&supervisor, revision);
    managers.insert(
      name.to_owned(),
      ActiveDeployment {
        active_revision: revision.to_owned(),
        managed,
        supervisor,
        last_error: None,
        expected_processes: specs.len(),
        vision,
      },
    );
  }
  let active = managers.get_mut(name).expect("start inserted active run");
  let attempt = active
    .supervisor
    .start_with_secrets(specs, plan)
    .and_then(|started| {
      std::thread::sleep(Duration::from_millis(100));
      let running = active.supervisor.status()?;
      ensure!(
        running.len() == specs.len(),
        "a managed process exited during startup; inspect categorized child logs"
      );
      Ok(started)
    });
  let started = match attempt {
    Ok(started) => started,
    Err(error) => {
      let mut failed = managers.remove(name).expect("failed run exists");
      let _ = stop_active(paths, name, &mut failed, true);
      managers.insert(name.to_owned(), failed);
      return Err(error);
    }
  };
  phase_timing("start", "children_ready", started_at, &mut phase_at);
  let active = managers.get_mut(name).expect("active run exists");
  active.last_error = None;
  if !started.is_empty() {
    log_deployment(
      paths,
      name,
      active.supervisor.run_id(),
      "deployment_ready",
      "info",
    );
  }
  Ok((started, active.supervisor.run_id().to_owned()))
}

pub(crate) fn manager_identity(paths: &config::Paths) -> Result<Value> {
  let executable = std::env::current_exe()?;
  let stat = fs::read_to_string("/proc/self/stat")?;
  let fields = stat
    .rsplit_once(") ")
    .context("read process start identity")?
    .1;
  let start_ticks = fields
    .split_whitespace()
    .nth(19)
    .context("process start identity is incomplete")?;
  Ok(json!({
      "pid": std::process::id(),
      "start_ticks": start_ticks,
      "mode": paths.mode()?,
      "executable": executable,
      "executable_sha256": hex::encode(Sha256::digest(fs::read(executable)?)),
      "isos_version": env!("CARGO_PKG_VERSION"),
      "api_schema_version": "0.1.0",
      "service_contract_version": "1.0.0",
      "api_origin": http::local_origin()?,
      "ui_url": http::ui_url()?,
  }))
}

mod lifecycle;
mod operations;
mod socket;
mod daemon_runtime;
mod client;

use lifecycle::{
  RetirementDiagnostic, log_deployment, log_request_preflight_failure, stop_active,
  stop_active_with_correlation,
};
#[cfg(test)]
use lifecycle::{log_package_load_failure, log_prelaunch_failure, stalled_stop_log_gate, stop_log_gate};
pub(crate) use operations::api_call;
pub(crate) use socket::{bind_daemon, handle_client};
pub(crate) use daemon_runtime::daemon;
pub(crate) use client::{
  RemoteDiagnosticError, manager_call, manager_call_revision, manager_status_for_http, service_call,
};

#[cfg(test)]
mod restart_tests;
#[cfg(test)]
use daemon_runtime::poll_deployments;
