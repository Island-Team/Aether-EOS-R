use super::*;

struct EmptySelectedTemplate;
impl EmptySelectedTemplate {
  fn new() -> Self {
    TEST_EMPTY_SELECTED_TEMPLATE.with(|flag| flag.set(true));
    Self
  }
}
impl Drop for EmptySelectedTemplate {
  fn drop(&mut self) {
    TEST_EMPTY_SELECTED_TEMPLATE.with(|flag| flag.set(false));
  }
}

#[test]
fn final_vision_fence_rejects_both_gate_selection_transitions() {
  struct ResetVisionConfig;
  impl Drop for ResetVisionConfig {
    fn drop(&mut self) {
      config::test_vision_config_path(None);
    }
  }
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  paths.prepare().unwrap();
  let config_path = paths.config.join("vision-test.toml");
  fs::write(&config_path, b"").unwrap();
  config::test_vision_config_path(Some(config_path.clone()));
  let _reset = ResetVisionConfig;
  let graph = json!({"version":"0.1.0","robot":{},"cores":{}});
  let bytes = serde_json::to_vec_pretty(&graph).unwrap();
  let revision = graph_compat::revision(&bytes);
  config::atomic_write(&paths.node_graph_path("test"), &bytes).unwrap();
  let base_files = vec![
    (
      std::path::PathBuf::from("eos/graph.json"),
      b"eos projection".to_vec(),
    ),
    (
      std::path::PathBuf::from("launch-inputs.json"),
      b"[]".to_vec(),
    ),
  ];
  crate::domain::graph::generated_v2::publish(&paths, "test", &graph, &revision, &base_files)
    .unwrap();
  let mut supervisor = process::Supervisor::new(&paths, "test").unwrap();
  supervisor.prepare_control_grants(&revision).unwrap();
  assert!(
    crate::runtime::process::vision_binding::selected_template(&paths, "test", &revision)
      .unwrap()
      .is_none()
  );
  recheck_admitted_vision_template(&paths, "test", &revision, &supervisor).unwrap();

  let selected = crate::runtime::process::vision_binding::Template {
    schema_version: 1,
    graph_revision: revision.clone(),
    projection_revision: "b".repeat(64),
    source_path: "eos/hardware/camera/front".into(),
    gate_path: "eos/interface/gate/front".into(),
    ingress_type: "egia.vision.ingress@v0.1.frame".into(),
    render: crate::runtime::process::vision_binding::Render {
      max_width_px: 1280,
      jpeg_quality: 80,
    },
    target_fps: 30,
    endpoint_kind: "uds".into(),
    endpoint_name: "eos-vision-owner.sock".into(),
  };
  fs::write(
    &config_path,
    b"[vision_evidence]\ngate_path = \"eos/interface/gate/front\"\n",
  )
  .unwrap();
  let mut selected_files = base_files.clone();
  selected_files.push((
    std::path::PathBuf::from(crate::runtime::process::vision_binding::TEMPLATE_FILE),
    serde_json::to_vec(&selected).unwrap(),
  ));
  crate::domain::graph::generated_v2::publish(&paths, "test", &graph, &revision, &selected_files)
    .unwrap();
  assert_eq!(
    crate::runtime::process::vision_binding::selected_template(&paths, "test", &revision).unwrap(),
    Some(selected.clone())
  );
  assert!(
    recheck_admitted_vision_template(&paths, "test", &revision, &supervisor)
      .unwrap_err()
      .to_string()
      .contains("selected Vision template changed")
  );
  supervisor.retire_candidate_grants().unwrap();

  let mut replacement = process::Supervisor::new(&paths, "test").unwrap();
  replacement.prepare_control_grants(&revision).unwrap();
  replacement
    .attach_verified_vision_template(selected)
    .unwrap();
  recheck_admitted_vision_template(&paths, "test", &revision, &replacement).unwrap();
  fs::write(&config_path, b"").unwrap();
  crate::domain::graph::generated_v2::publish(&paths, "test", &graph, &revision, &base_files)
    .unwrap();
  assert!(
    crate::runtime::process::vision_binding::selected_template(&paths, "test", &revision)
      .unwrap()
      .is_none()
  );
  assert!(
    recheck_admitted_vision_template(&paths, "test", &revision, &replacement)
      .unwrap_err()
      .to_string()
      .contains("selected Vision template changed")
  );
  replacement.retire_candidate_grants().unwrap();
}

#[test]
fn admitted_projection_uses_attached_grant_after_template_source_changes() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  paths.prepare().unwrap();
  let revision = "a".repeat(64);
  let mut supervisor = process::Supervisor::new(&paths, "test").unwrap();
  supervisor.prepare_control_grants(&revision).unwrap();
  let admitted = crate::runtime::process::vision_binding::Template {
    schema_version: 1,
    graph_revision: revision.clone(),
    projection_revision: "b".repeat(64),
    source_path: "eos/hardware/camera/front".into(),
    gate_path: "eos/interface/gate/front".into(),
    ingress_type: "egia.vision.ingress@v0.1.frame".into(),
    render: crate::runtime::process::vision_binding::Render {
      max_width_px: 1280,
      jpeg_quality: 80,
    },
    target_fps: 30,
    endpoint_kind: "uds".into(),
    endpoint_name: "eos-vision-owner.sock".into(),
  };
  supervisor
    .attach_verified_vision_template(admitted.clone())
    .unwrap();
  let mut changed_source = admitted.clone();
  changed_source.projection_revision = "c".repeat(64);
  changed_source.target_fps = 12;
  let snapshot = admitted_vision_snapshot(&supervisor, &revision).unwrap();
  assert_eq!(
    snapshot.projection,
    crate::runtime::process::vision_binding::projection(&admitted)
  );
  assert_ne!(
    snapshot.projection,
    crate::runtime::process::vision_binding::projection(&changed_source)
  );
  assert_eq!(supervisor.admitted_vision_template(), Some(&admitted));
  supervisor.retire_candidate_grants().unwrap();
}

#[test]
fn status_keeps_admitted_vision_pair_across_saved_edit_and_stop() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  paths.prepare().unwrap();
  let path = paths.node_graph_path("test");
  config::atomic_write(&path, b"old saved graph").unwrap();
  let old_saved = starter::revision(&path).unwrap();
  let spec = process::ProcessSpec {
    id: "stub".into(),
    path: "isos/process/component/stub".into(),
    settings: serde_json::from_value(json!({"command":["/bin/sleep","30"]})).unwrap(),
    package: None,
    fingerprint: None,
  };
  let mut managers = Deployments::new();
  restart_processes(&paths, &mut managers, "test", &old_saved, &[spec]).unwrap();
  let run_id = managers["test"].supervisor.run_id().to_owned();
  let projection = json!({"gate_path":"eos/interface/gate/camera","source_path":"eos/interface/camera/front","projection_revision":"pinned","render":{"max_width_px":1280,"jpeg_quality":80},"target_fps":30});
  managers.get_mut("test").unwrap().vision =
    vision_snapshot(&run_id, &old_saved, Some(projection.clone()));
  let mut services = services::ServiceManager::new(&paths, "test").unwrap();
  let request = |op: &str| WireCommand::deployment_call(op, "test", None).unwrap();
  // No selected generation or owner source is available in this home.
  // A deliberately slow selected source would exceed the status deadline
  // if status tried to verify it again.
  struct ResetSelectedDelay;
  impl Drop for ResetSelectedDelay {
    fn drop(&mut self) {
      crate::runtime::process::vision_binding::test_selected_template_delay(0);
    }
  }
  crate::runtime::process::vision_binding::test_selected_template_delay(3_000);
  let _reset_delay = ResetSelectedDelay;
  let started = Instant::now();
  for _ in 0..5 {
    let status = api_call(
      &paths,
      &mut managers,
      &mut services,
      request("deployment_status"),
    )
    .unwrap();
    assert_eq!(status["state"], "running");
    assert_eq!(status["run_id"], run_id);
    assert_eq!(status["active_revision"], old_saved);
    assert_eq!(status["vision_evidence"], projection);
  }
  assert!(started.elapsed() < Duration::from_secs(2));
  config::atomic_write(&path, b"new saved graph").unwrap();
  let new_saved = starter::revision(&path).unwrap();
  let status = api_call(
    &paths,
    &mut managers,
    &mut services,
    request("deployment_status"),
  )
  .unwrap();
  assert_eq!(status["saved_revision"], new_saved);
  assert_eq!(status["active_revision"], old_saved);
  assert_eq!(status["run_id"], run_id);
  assert_eq!(status["vision_evidence"], projection);

  // A failed or mismatched run identity cannot borrow the old evidence.
  managers
    .get_mut("test")
    .unwrap()
    .vision
    .as_mut()
    .unwrap()
    .run_id = "other".into();
  let status = api_call(
    &paths,
    &mut managers,
    &mut services,
    request("deployment_status"),
  )
  .unwrap();
  assert!(status["vision_evidence"].is_null());
  managers
    .get_mut("test")
    .unwrap()
    .vision
    .as_mut()
    .unwrap()
    .run_id = run_id.clone();
  let stopped = api_call(
    &paths,
    &mut managers,
    &mut services,
    request("deployment_stop"),
  )
  .unwrap();
  assert_eq!(stopped["terminal"]["outcome"], "confirmed");
  let status = api_call(
    &paths,
    &mut managers,
    &mut services,
    request("deployment_status"),
  )
  .unwrap();
  assert_eq!(status["state"], "stopped");
  assert!(status["vision_evidence"].is_null());
}
use std::net::Shutdown;
use std::os::unix::fs::PermissionsExt;

fn call_admission(paths: &config::Paths, managers: &mut Deployments, op: &str) -> Value {
  let mut services = services::ServiceManager::new(paths, "test").unwrap();
  let (mut client, server) = UnixStream::pair().unwrap();
  let request = json!({"schema_version":"0.1.0","op":op,"payload":{"deployment":"test"}});
  writeln!(client, "{request}").unwrap();
  client.shutdown(Shutdown::Write).unwrap();
  handle_client(paths, managers, &mut services, server).unwrap();
  let mut reply = String::new();
  client.read_to_string(&mut reply).unwrap();
  serde_json::from_str(&reply).unwrap()
}

fn assert_admission_code(reply: &Value, code: &str) {
  assert_eq!(reply["ok"], false);
  assert_eq!(reply["data"]["diagnostics"][0]["code"], code);
  assert_eq!(reply["data"]["diagnostics"][0]["path"], "");
}

fn stop_events(paths: &config::Paths) -> Vec<Value> {
  for _ in 0..100 {
    let lines = log_store::LogStore::new(paths, "test")
      .unwrap()
      .read("all", None, 100)
      .unwrap()["lines"]
      .as_array()
      .unwrap()
      .clone();
    if lines
      .iter()
      .any(|line| line["event_code"] == "deployment_stop_unconfirmed")
    {
      return lines;
    }
    std::thread::sleep(Duration::from_millis(10));
  }
  panic!("stop event was not committed");
}

fn retirement_fixture(
  unexpected: bool,
  eos: bool,
) -> (
  tempfile::TempDir,
  config::Paths,
  Deployments,
  process::Supervisor,
) {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  paths.prepare().unwrap();
  fs::create_dir_all(paths.node_graph_path("test").parent().unwrap()).unwrap();
  if eos {
    fs::write(
      paths.node_graph_path("test"),
      serde_json::to_vec(&json!({
          "cores":{"eos":{"layers":{"process":{"groups":{"simulator":{"nodes":{"sim":{"package":{"id":"test/owner","version":"0.1.0"}}}}}}}}}
      }))
      .unwrap(),
    )
    .unwrap();
  } else {
    fs::write(paths.node_graph_path("test"), b"saved graph bytes").unwrap();
  }
  let eos_script = temp.path().join("eos-no-receipt.sh");
  if eos {
    fs::write(&eos_script, "#!/bin/sh\nrun=''\nwhile [ \"$#\" -gt 0 ]; do\n  if [ \"$1\" = '--run-id' ]; then shift; run=\"$1\"; break; fi\n  shift\ndone\nprintf '{\"timestamp\":\"2026-11-01T10:00:01.000Z\",\"level\":\"info\",\"source\":\"eos\",\"core\":\"eos\",\"event_code\":\"eos_ready\",\"run_id\":\"%s\",\"message\":\"EOS runtime ready\",\"fields\":{\"state\":\"ready\",\"node_count\":1}}\\n' \"$run\" >&2\ntrap 'exit 0' TERM\nwhile :; do sleep 1; done\n").unwrap();
    fs::set_permissions(&eos_script, fs::Permissions::from_mode(0o700)).unwrap();
  }
  let spec = process::ProcessSpec {
    id: if eos { "eos" } else { "stub" }.into(),
    path: if eos {
      "eos/process/simulator/worker"
    } else {
      "isos/process/component/stub"
    }
    .into(),
    settings: serde_json::from_value(if eos {
      json!({"command":[eos_script],"role":"eos","start_timeout_ms":1000})
    } else {
      json!({"command":["/bin/sleep","30"]})
    })
    .unwrap(),
    package: None,
    fingerprint: None,
  };
  let mut old = process::Supervisor::new(&paths, "test").unwrap();
  old.prepare_control_grants(&"a".repeat(64)).unwrap();
  old.publish_run_scope().unwrap();
  if eos {
    old
      .prepare_run(std::slice::from_ref(&spec), &"a".repeat(64))
      .unwrap();
  }
  old.start(&[spec]).unwrap();
  if unexpected {
    old.mark_unexpected_exit();
  }
  let mut managers = Deployments::new();
  managers.insert(
    "test".into(),
    ActiveDeployment {
      active_revision: "a".repeat(64),
      managed: true,
      supervisor: old,
      last_error: None,
      expected_processes: 1,
      vision: None,
    },
  );
  let mut candidate = process::Supervisor::new(&paths, "test").unwrap();
  candidate.prepare_control_grants(&"b".repeat(64)).unwrap();
  let success = json!({
      "schema_version":1,"ok":true,"active_revision":"c".repeat(64),
      "generation":"b".repeat(64),"run_id":candidate.run_id(),
      "selection_sha256":"a".repeat(64),
      "libraries":[{"package_id":"test/verified","package_version":"0.1.0",
          "library":"lib/x.skill.so","library_sha256":"d".repeat(64),
          "abi_version":2,"skill_count":1}]
  });
  let script = temp.path().join("probe.sh");
  fs::write(
    &script,
    format!("#!/bin/sh\nprintf '%s\\n' '{}'\n", success),
  )
  .unwrap();
  fs::set_permissions(&script, fs::Permissions::from_mode(0o700)).unwrap();
  let inputs = crate::runtime::process::candidate_skill_preflight::Inputs::rejection_stub(script);
  crate::runtime::process::candidate_skill_preflight::run(
    &inputs,
    candidate.run_id(),
    &candidate.candidate_verifier().unwrap(),
  )
  .unwrap();
  (temp, paths, managers, candidate)
}

#[test]
fn unknown_old_stop_retires_unpublished_candidate_and_keeps_old_identity() {
  let (_temp, paths, mut managers, mut candidate) = retirement_fixture(true, false);
  let old_run = managers["test"].supervisor.run_id().to_owned();
  let candidate_dir = paths
    .runtime
    .join("control-grants/test")
    .join(candidate.run_id());
  let before = fs::read(paths.node_graph_path("test")).unwrap();
  REQUEST_CORRELATION.with(|current| *current.borrow_mut() = Some("restart-request-42".into()));
  let error = stop_old_for_replacement(&paths, &mut managers, "test", &mut candidate, true)
    .err()
    .unwrap();
  REQUEST_CORRELATION.with(|current| *current.borrow_mut() = None);
  assert_eq!(
    error.downcast_ref::<RetirementDiagnostic>().unwrap().code,
    "deployment_stop_unconfirmed"
  );
  assert!(!candidate_dir.exists());
  assert_eq!(managers["test"].supervisor.run_id(), old_run);
  assert_eq!(
    managers["test"].supervisor.terminal().unwrap().outcome,
    "unknown"
  );
  assert_eq!(fs::read(paths.node_graph_path("test")).unwrap(), before);
  assert_eq!(
    log_store::LogStore::new(&paths, "test")
      .unwrap()
      .run_identity()
      .0
      .as_deref(),
    Some(old_run.as_str())
  );
  assert!(check_candidate_cleanup_marker(&paths, "test", Some(&old_run)).is_ok());
  let lines = stop_events(&paths);
  let stops: Vec<_> = lines
    .iter()
    .filter(|line| line["event_code"] == "deployment_stop_unconfirmed")
    .collect();
  assert_eq!(stops.len(), 1);
  assert_eq!(stops[0]["run_id"], old_run);
  assert_eq!(stops[0]["correlation_id"], "restart-request-42");
  assert!(lines.iter().all(|line| {
    line["event_code"] != "deployment_starting" || line["run_id"] != candidate.run_id()
  }));
  let old_specs = managers["test"].supervisor.specs();
  let retry = restart_processes(&paths, &mut managers, "test", "new", &old_specs).unwrap_err();
  assert_eq!(
    retry.downcast_ref::<RetirementDiagnostic>().unwrap().code,
    "deployment_stop_unconfirmed"
  );
  assert_eq!(
    managers["test"].supervisor.terminal().unwrap().reason_code,
    Some("child_exit_unexpected")
  );
  let mut recovered = Deployments::new();
  let mut services = services::ServiceManager::new(&paths, "test").unwrap();
  let status = api_call(
    &paths,
    &mut recovered,
    &mut services,
    WireCommand::DeploymentStatus(DeploymentTarget {
      deployment: "test".into(),
    }),
  )
  .unwrap();
  assert_eq!(status["run_id"], old_run);
  assert_eq!(status["state"], "degraded");
  assert_eq!(status["terminal"]["reason_code"], "stop_interrupted");
  assert!(
    status["last_error"]
      .as_str()
      .unwrap()
      .contains("after manager restart")
  );
  let after_restart =
    restart_processes(&paths, &mut recovered, "test", "new", &old_specs).unwrap_err();
  assert_eq!(
    after_restart
      .downcast_ref::<RetirementDiagnostic>()
      .unwrap()
      .code,
    "deployment_stop_unconfirmed"
  );
  assert!(
    after_restart
      .to_string()
      .contains("stop proof is unavailable")
  );
}

#[test]
fn dual_cleanup_failure_keeps_candidate_orphan_and_old_terminal() {
  let (_temp, paths, mut managers, mut candidate) = retirement_fixture(true, false);
  let old_run = managers["test"].supervisor.run_id().to_owned();
  let orphan = paths
    .runtime
    .join("control-grants/test")
    .join(candidate.run_id());
  fs::set_permissions(
    candidate.candidate_verifier().unwrap(),
    fs::Permissions::from_mode(0o644),
  )
  .unwrap();
  FAIL_CANDIDATE_MARKER_WRITE.with(|fail| fail.set(true));
  FAIL_CANDIDATE_FALLBACK_WRITE.with(|fail| fail.set(true));
  REQUEST_CORRELATION.with(|current| *current.borrow_mut() = Some("restart-request-42".into()));
  let error = stop_old_for_replacement(&paths, &mut managers, "test", &mut candidate, true)
    .err()
    .unwrap();
  log_request_preflight_failure(&paths, "test", &error, "restart-request-42");
  REQUEST_CORRELATION.with(|current| *current.borrow_mut() = None);
  FAIL_CANDIDATE_MARKER_WRITE.with(|fail| fail.set(false));
  FAIL_CANDIDATE_FALLBACK_WRITE.with(|fail| fail.set(false));
  assert_eq!(
    error.downcast_ref::<RetirementDiagnostic>().unwrap().code,
    "skill_catalog_preflight_cleanup_unconfirmed"
  );
  assert_eq!(managers["test"].supervisor.run_id(), old_run);
  assert_eq!(
    managers["test"].supervisor.terminal().unwrap().outcome,
    "unknown"
  );
  assert!(orphan.is_dir());
  assert!(check_candidate_cleanup_marker(&paths, "test", Some(&old_run)).is_err());
  candidate_cleanup_blocks().lock().unwrap().clear();
  assert!(check_candidate_cleanup_marker(&paths, "test", None).is_err());
  let lines = stop_events(&paths);
  let old_stop = lines
    .iter()
    .find(|line| line["event_code"] == "deployment_stop_unconfirmed")
    .unwrap();
  let cleanup = lines
    .iter()
    .find(|line| line["event_code"] == "deployment_preflight_failed")
    .unwrap();
  assert_eq!(old_stop["run_id"], old_run);
  assert_eq!(old_stop["correlation_id"], "restart-request-42");
  assert_eq!(cleanup["run_id"], manager_session_id());
  assert_eq!(cleanup["correlation_id"], "restart-request-42");
  assert_eq!(
    cleanup["fields"]["diagnostic_code"],
    "skill_catalog_preflight_cleanup_unconfirmed"
  );
  let bearer = fs::read_to_string(orphan.join("operator-control.grant")).unwrap();
  assert!(
    !serde_json::to_string(&lines)
      .unwrap()
      .contains(bearer.trim())
  );
}

#[test]
fn api_retry_fences_unknown_old_run_before_missing_or_changed_graph() {
  let (_temp, paths, mut managers, mut candidate) = retirement_fixture(true, false);
  let old_run = managers["test"].supervisor.run_id().to_owned();
  let candidate_run = candidate.run_id().to_owned();
  stop_old_for_replacement(&paths, &mut managers, "test", &mut candidate, true)
    .err()
    .unwrap();
  fs::remove_file(paths.node_graph_path("test")).unwrap();
  for op in ["deployment_restart", "deployment_start"] {
    assert_admission_code(
      &call_admission(&paths, &mut managers, op),
      "deployment_stop_unconfirmed",
    );
  }
  fs::write(
    paths.node_graph_path("test"),
    b"changed invalid graph and package",
  )
  .unwrap();
  for op in ["deployment_restart", "deployment_start"] {
    assert_admission_code(
      &call_admission(&paths, &mut managers, op),
      "deployment_stop_unconfirmed",
    );
  }
  assert_eq!(managers["test"].supervisor.run_id(), old_run);
  assert_eq!(
    managers["test"].supervisor.terminal().unwrap().reason_code,
    Some("child_exit_unexpected")
  );
  assert_eq!(
    log_store::LogStore::new(&paths, "test")
      .unwrap()
      .run_identity()
      .0
      .as_deref(),
    Some(old_run.as_str())
  );
  assert!(
    !paths
      .runtime
      .join("control-grants/test")
      .join(candidate_run)
      .exists()
  );
  assert!(
    managers
      .get_mut("test")
      .unwrap()
      .supervisor
      .status()
      .unwrap()
      .is_empty()
  );
}

#[test]
fn api_retry_prefers_candidate_cleanup_before_graph_or_old_terminal() {
  let (_temp, paths, mut managers, mut candidate) = retirement_fixture(true, false);
  let old_run = managers["test"].supervisor.run_id().to_owned();
  let candidate_run = candidate.run_id().to_owned();
  fs::set_permissions(
    candidate.candidate_verifier().unwrap(),
    fs::Permissions::from_mode(0o644),
  )
  .unwrap();
  stop_old_for_replacement(&paths, &mut managers, "test", &mut candidate, true)
    .err()
    .unwrap();
  fs::remove_file(paths.node_graph_path("test")).unwrap();
  for op in ["deployment_restart", "deployment_start"] {
    assert_admission_code(
      &call_admission(&paths, &mut managers, op),
      "skill_catalog_preflight_cleanup_unconfirmed",
    );
  }
  fs::write(
    paths.node_graph_path("test"),
    b"changed invalid graph and package",
  )
  .unwrap();
  assert_admission_code(
    &call_admission(&paths, &mut managers, "deployment_restart"),
    "skill_catalog_preflight_cleanup_unconfirmed",
  );
  assert_eq!(managers["test"].supervisor.run_id(), old_run);
  assert_eq!(
    log_store::LogStore::new(&paths, "test")
      .unwrap()
      .run_identity()
      .0
      .as_deref(),
    Some(old_run.as_str())
  );
  assert!(
    paths
      .runtime
      .join("control-grants/test")
      .join(candidate_run)
      .is_dir()
  );
  assert!(
    managers
      .get_mut("test")
      .unwrap()
      .supervisor
      .status()
      .unwrap()
      .is_empty()
  );
}

#[test]
fn grant_directory_inspection_failures_are_typed_and_path_free() {
  let (_temp, paths, mut managers, _candidate) = retirement_fixture(false, false);
  for fault in [1, 2, 3] {
    FAIL_CANDIDATE_INSPECTION.with(|fail| fail.set(fault));
    let reply = call_admission(&paths, &mut managers, "deployment_restart");
    FAIL_CANDIDATE_INSPECTION.with(|fail| fail.set(0));
    assert_admission_code(&reply, "skill_catalog_preflight_cleanup_unconfirmed");
    assert!(!reply.to_string().contains(paths.runtime.to_str().unwrap()));
  }
  let root = paths.runtime.join("control-grants/test");
  let old = managers.remove("test").unwrap();
  drop(old);
  fs::remove_dir_all(&root).unwrap();
  fs::write(&root, b"unreadable grant root").unwrap();
  let reply = call_admission(&paths, &mut managers, "deployment_start");
  assert_admission_code(&reply, "skill_catalog_preflight_cleanup_unconfirmed");
  assert!(!reply.to_string().contains(paths.runtime.to_str().unwrap()));
}

#[test]
fn old_grant_retirement_failure_keeps_precise_terminal_reason() {
  let (_temp, paths, mut managers, mut candidate) = retirement_fixture(false, false);
  let old_run = managers["test"].supervisor.run_id().to_owned();
  let old_verifier = paths
    .runtime
    .join("control-grants/test")
    .join(&old_run)
    .join("verifier.json");
  fs::set_permissions(old_verifier, fs::Permissions::from_mode(0o644)).unwrap();
  let error = stop_old_for_replacement(&paths, &mut managers, "test", &mut candidate, true)
    .err()
    .unwrap();
  assert_eq!(
    error.downcast_ref::<RetirementDiagnostic>().unwrap().code,
    "deployment_stop_unconfirmed"
  );
  let terminal = managers["test"].supervisor.terminal().unwrap();
  assert_eq!(terminal.outcome, "unknown");
  let old_run = terminal.run_id.clone();
  assert_eq!(terminal.reason_code, Some("control_grant_retire_failed"));
  assert_eq!(terminal.run_id, old_run);
  assert!(
    !paths
      .runtime
      .join("control-grants/test")
      .join(candidate.run_id())
      .exists()
  );
}

#[test]
fn missing_eos_stop_receipt_blocks_selected_replacement() {
  let (_temp, paths, mut managers, mut candidate) = retirement_fixture(false, true);
  let old_run = managers["test"].supervisor.run_id().to_owned();
  let error = stop_old_for_replacement(&paths, &mut managers, "test", &mut candidate, true)
    .err()
    .unwrap();
  assert_eq!(
    error.downcast_ref::<RetirementDiagnostic>().unwrap().code,
    "deployment_stop_unconfirmed"
  );
  let terminal = managers["test"].supervisor.terminal().unwrap();
  assert_eq!(terminal.outcome, "unknown");
  assert_eq!(terminal.reason_code, Some("eos_receipt_missing"));
  assert_eq!(terminal.run_id, old_run);
  assert!(
    !paths
      .runtime
      .join("control-grants/test")
      .join(candidate.run_id())
      .exists()
  );
}

#[test]
fn primary_marker_failure_uses_fallback_after_unknown_old_stop() {
  let (_temp, paths, mut managers, mut candidate) = retirement_fixture(true, false);
  let old_run = managers["test"].supervisor.run_id().to_owned();
  fs::set_permissions(
    candidate.candidate_verifier().unwrap(),
    fs::Permissions::from_mode(0o644),
  )
  .unwrap();
  FAIL_CANDIDATE_MARKER_WRITE.with(|fail| fail.set(true));
  let error = stop_old_for_replacement(&paths, &mut managers, "test", &mut candidate, true)
    .err()
    .unwrap();
  FAIL_CANDIDATE_MARKER_WRITE.with(|fail| fail.set(false));
  assert_eq!(
    error.downcast_ref::<RetirementDiagnostic>().unwrap().code,
    "skill_catalog_preflight_cleanup_unconfirmed"
  );
  assert!(
    !paths
      .runtime
      .join("test.skill-candidate-cleanup-unconfirmed")
      .exists()
  );
  assert!(
    paths
      .data
      .join("test.skill-candidate-cleanup-unconfirmed")
      .exists()
  );
  candidate_cleanup_blocks().lock().unwrap().clear();
  let retry = check_candidate_cleanup_marker(&paths, "test", Some(&old_run)).unwrap_err();
  assert_eq!(
    retry.downcast_ref::<RetirementDiagnostic>().unwrap().code,
    "skill_catalog_preflight_cleanup_unconfirmed"
  );
}

#[test]
fn confirmed_old_stop_preserves_candidate_for_one_scope_publication() {
  let (_temp, paths, mut managers, mut candidate) = retirement_fixture(false, false);
  let old_run = managers["test"].supervisor.run_id().to_owned();
  let candidate_run = candidate.run_id().to_owned();
  let previous = stop_old_for_replacement(&paths, &mut managers, "test", &mut candidate, true)
    .unwrap()
    .unwrap();
  assert_eq!(previous.2.outcome, "confirmed");
  assert_eq!(previous.0.supervisor.run_id(), old_run);
  assert_eq!(
    log_store::LogStore::new(&paths, "test")
      .unwrap()
      .run_identity()
      .0
      .as_deref(),
    Some(old_run.as_str())
  );
  assert!(candidate.candidate_verifier().unwrap().exists());
  publish_run_scope(&candidate).unwrap();
  assert_eq!(
    log_store::LogStore::new(&paths, "test")
      .unwrap()
      .run_identity()
      .0
      .as_deref(),
    Some(candidate_run.as_str())
  );
}

#[test]
fn rejected_candidate_preserves_old_pid_grants_and_run_scope() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  paths.prepare().unwrap();
  let old_script = temp.path().join("old.sh");
  fs::write(&old_script, "#!/bin/sh\nexec sleep 10\n").unwrap();
  fs::set_permissions(&old_script, fs::Permissions::from_mode(0o700)).unwrap();
  let old_spec = process::ProcessSpec {
    id: "old".into(),
    path: "isos/process/component/old".into(),
    settings: serde_json::from_value(json!({"command":[old_script],"start_timeout_ms":1000}))
      .unwrap(),
    package: None,
    fingerprint: None,
  };
  let revision = "c".repeat(64);
  let mut old = process::Supervisor::new(&paths, "test").unwrap();
  old.prepare_control_grants(&revision).unwrap();
  old.publish_run_scope().unwrap();
  old.start(&[old_spec]).unwrap();
  let old_run = old.run_id().to_owned();
  let old_pid = old.status().unwrap()[0].pid;
  let old_grants = paths.runtime.join(format!("control-grants/test/{old_run}"));
  let mut managers = Deployments::new();
  managers.insert(
    "test".into(),
    ActiveDeployment {
      active_revision: revision.clone(),
      managed: true,
      supervisor: old,
      last_error: None,
      expected_processes: 1,
      vision: None,
    },
  );
  let probe_script = temp.path().join("reject.sh");
  fs::write(&probe_script, "#!/bin/sh\nprintf '%s\\n' '{\"schema_version\":1,\"ok\":false,\"stage\":\"selection\",\"code\":\"selection_invalid\"}'\nexit 1\n").unwrap();
  fs::set_permissions(&probe_script, fs::Permissions::from_mode(0o700)).unwrap();
  let inputs =
    crate::runtime::process::candidate_skill_preflight::Inputs::rejection_stub(probe_script);
  let mut candidate = process::Supervisor::new(&paths, "test").unwrap();
  candidate.prepare_control_grants(&revision).unwrap();
  let candidate_grants = paths
    .runtime
    .join(format!("control-grants/test/{}", candidate.run_id()));
  let error = probe_candidate(
    &paths,
    "test",
    &revision,
    &[],
    &crate::domain::graph::generated_v2::SecretPlan::new(),
    Some(inputs),
    &mut candidate,
  )
  .unwrap_err();
  let diagnostic = error
    .downcast_ref::<crate::runtime::process::candidate_skill_preflight::Diagnostic>()
    .unwrap();
  assert_eq!(diagnostic.code, "skill_catalog_preflight_rejected");
  assert_eq!(diagnostic.eos_code, Some("selection_invalid"));
  assert_eq!(diagnostic.meta.package_version.as_deref(), Some("0.1.0"));
  assert!(!candidate_grants.exists());
  assert!(old_grants.exists());
  assert_eq!(
    managers
      .get_mut("test")
      .unwrap()
      .supervisor
      .status()
      .unwrap()[0]
      .pid,
    old_pid
  );
  assert_eq!(managers["test"].supervisor.run_id(), old_run);
  assert_eq!(
    log_store::LogStore::new(&paths, "test")
      .unwrap()
      .run_identity()
      .0
      .as_deref(),
    Some(old_run.as_str())
  );
  managers.get_mut("test").unwrap().supervisor.stop();
}

#[test]
fn uncertain_candidate_grant_cleanup_blocks_another_replacement() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  paths.prepare().unwrap();
  let script = temp.path().join("tamper.sh");
  fs::write(&script, "#!/bin/sh\n/bin/chmod 644 \"$EOS_CONTROL_GRANTS_FILE\"\nprintf '%s\\n' '{\"schema_version\":1,\"ok\":false,\"stage\":\"selection\",\"code\":\"selection_invalid\"}'\nexit 1\n").unwrap();
  fs::set_permissions(&script, fs::Permissions::from_mode(0o700)).unwrap();
  let inputs = crate::runtime::process::candidate_skill_preflight::Inputs::rejection_stub(script);
  let mut candidate = process::Supervisor::new(&paths, "test").unwrap();
  candidate.prepare_control_grants(&"c".repeat(64)).unwrap();
  let verifier = candidate.candidate_verifier().unwrap();
  FAIL_CANDIDATE_MARKER_WRITE.with(|fail| fail.set(true));
  let error = probe_candidate(
    &paths,
    "test",
    &"c".repeat(64),
    &[],
    &crate::domain::graph::generated_v2::SecretPlan::new(),
    Some(inputs),
    &mut candidate,
  )
  .unwrap_err();
  FAIL_CANDIDATE_MARKER_WRITE.with(|fail| fail.set(false));
  assert_eq!(
    error
      .downcast_ref::<crate::runtime::process::candidate_skill_preflight::Diagnostic>()
      .unwrap()
      .code,
    "skill_catalog_preflight_cleanup_unconfirmed"
  );
  assert!(check_candidate_cleanup_marker(&paths, "test", None).is_err());
  assert!(
    !paths
      .runtime
      .join("test.skill-candidate-cleanup-unconfirmed")
      .exists()
  );
  // Simulate a manager restart: the orphan bundle still blocks replacement.
  candidate_cleanup_blocks().lock().unwrap().clear();
  assert!(check_candidate_cleanup_marker(&paths, "test", None).is_err());
  fs::set_permissions(&verifier, fs::Permissions::from_mode(0o600)).unwrap();
  let orphan = verifier.parent().unwrap();
  for name in [
    "egia-control.grant",
    "operator-control.grant",
    "verifier.json",
  ] {
    fs::remove_file(orphan.join(name)).unwrap();
  }
  fs::remove_dir(orphan).unwrap();
  assert!(check_candidate_cleanup_marker(&paths, "test", None).is_err());
  fs::remove_file(paths.data.join("test.skill-candidate-cleanup-unconfirmed")).unwrap();
  check_candidate_cleanup_marker(&paths, "test", None).unwrap();
}

#[test]
fn uncertain_probe_cleanup_retains_revoked_orphan_when_both_markers_fail() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  paths.prepare().unwrap();
  let script = temp.path().join("reject.sh");
  fs::write(&script, "#!/bin/sh\nprintf '%s\\n' '{\"schema_version\":1,\"ok\":false,\"stage\":\"selection\",\"code\":\"selection_invalid\"}'\nexit 1\n").unwrap();
  fs::set_permissions(&script, fs::Permissions::from_mode(0o700)).unwrap();
  let inputs = crate::runtime::process::candidate_skill_preflight::Inputs::rejection_stub(script);
  let mut candidate = process::Supervisor::new(&paths, "test").unwrap();
  candidate.prepare_control_grants(&"c".repeat(64)).unwrap();
  let orphan = paths
    .runtime
    .join(format!("control-grants/test/{}", candidate.run_id()));
  FAIL_CANDIDATE_MARKER_WRITE.with(|fail| fail.set(true));
  FAIL_CANDIDATE_FALLBACK_WRITE.with(|fail| fail.set(true));
  FORCE_PROBE_CLEANUP_UNCONFIRMED.with(|force| force.set(true));
  let error = probe_candidate(
    &paths,
    "test",
    &"c".repeat(64),
    &[],
    &crate::domain::graph::generated_v2::SecretPlan::new(),
    Some(inputs),
    &mut candidate,
  )
  .unwrap_err();
  FAIL_CANDIDATE_MARKER_WRITE.with(|fail| fail.set(false));
  FAIL_CANDIDATE_FALLBACK_WRITE.with(|fail| fail.set(false));
  FORCE_PROBE_CLEANUP_UNCONFIRMED.with(|force| force.set(false));
  assert_eq!(
    error
      .downcast_ref::<crate::runtime::process::candidate_skill_preflight::Diagnostic>()
      .unwrap()
      .code,
    "skill_catalog_preflight_cleanup_unconfirmed"
  );
  assert!(orphan.is_dir());
  assert!(fs::read_dir(&orphan).unwrap().next().is_none());
  assert!(
    !paths
      .runtime
      .join("test.skill-candidate-cleanup-unconfirmed")
      .exists()
  );
  assert!(
    !paths
      .data
      .join("test.skill-candidate-cleanup-unconfirmed")
      .exists()
  );
  candidate_cleanup_blocks().lock().unwrap().clear();
  assert!(check_candidate_cleanup_marker(&paths, "test", None).is_err());
  fs::remove_dir(&orphan).unwrap();
  check_candidate_cleanup_marker(&paths, "test", None).unwrap();
}

#[test]
fn writable_selected_skill_rejects_restart_before_old_process_stops() {
  let (_temp, paths, source) = crate::runtime::process::skill_selection::tests::fixture();
  let graph = crate::runtime::process::skill_selection::tests::graph(json!([
    crate::runtime::process::skill_selection::tests::pin("lib/island/skills/example.skill.so")
  ]));
  let bytes = serde_json::to_vec_pretty(&graph).unwrap();
  let revision = graph_compat::revision(&bytes);
  config::atomic_write(&paths.node_graph_path("test"), &bytes).unwrap();
  crate::domain::graph::generated_v2::publish(
    &paths,
    "test",
    &graph,
    &revision,
    &[
      (
        std::path::PathBuf::from("eos/graph.json"),
        b"projection".to_vec(),
      ),
      (
        std::path::PathBuf::from("launch-inputs.json"),
        b"[]".to_vec(),
      ),
    ],
  )
  .unwrap();
  let active_spec = process::ProcessSpec {
    id: "stub".into(),
    path: "isos/process/component/stub".into(),
    settings: serde_json::from_value(json!({
        "command":["/bin/sleep","30"],"start_timeout_ms":500
    }))
    .unwrap(),
    package: None,
    fingerprint: None,
  };
  let mut managers = Deployments::new();
  restart_processes(&paths, &mut managers, "test", "old", &[active_spec]).unwrap();
  let old_run = managers.get("test").unwrap().supervisor.run_id().to_owned();
  let old_pid = managers
    .get_mut("test")
    .unwrap()
    .supervisor
    .status()
    .unwrap()[0]
    .pid;
  fs::set_permissions(
    source.join("lib/island/skills/example.skill.so"),
    fs::Permissions::from_mode(0o775),
  )
  .unwrap();
  let mut services = services::ServiceManager::new(&paths, "test").unwrap();
  let error = api_call(
    &paths,
    &mut managers,
    &mut services,
    WireCommand::DeploymentRestart(DeploymentMutation {
      deployment: "test".into(),
      revision: Some(revision),
    }),
  )
  .unwrap_err();
  assert_eq!(
    error
      .downcast_ref::<crate::runtime::process::skill_selection::Diagnostic>()
      .unwrap()
      .code,
    "skill_library_unsafe"
  );
  let active = managers.get_mut("test").unwrap();
  assert_eq!(active.supervisor.run_id(), old_run);
  assert_eq!(active.supervisor.status().unwrap()[0].pid, old_pid);
  active.supervisor.stop();
}

#[test]
fn owner_stop_is_idempotent_and_recovered_status_is_confirmed() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  let spec = process::ProcessSpec {
    id: "stub".into(),
    path: "isos/process/component/stub".into(),
    settings: serde_json::from_value(json!({"command":["/bin/sleep","30"]})).unwrap(),
    package: None,
    fingerprint: None,
  };
  let mut managers = Deployments::new();
  restart_processes(&paths, &mut managers, "test", "a", &[spec]).unwrap();
  let mut services = services::ServiceManager::new(&paths, "test").unwrap();
  let request = |op: &str| WireCommand::deployment_call(op, "test", None).unwrap();
  let first = api_call(
    &paths,
    &mut managers,
    &mut services,
    request("deployment_stop"),
  )
  .unwrap();
  assert_eq!(first["stopped"], json!(["stub"]));
  assert_eq!(first["terminal"]["outcome"], "confirmed");
  // This is the daemon tick that an attached `isos run` can observe
  // immediately after another client stops its deployment.
  poll_deployments(&paths, &mut managers);
  assert!(managers["test"].last_error.is_none());
  let attached_status = api_call(
    &paths,
    &mut managers,
    &mut services,
    request("deployment_status"),
  )
  .unwrap();
  assert_eq!(attached_status["state"], "stopped");
  assert_eq!(attached_status["terminal"], first["terminal"]);
  let logs = log_store::LogStore::new(&paths, "test").unwrap();
  assert!(
    logs.read("all", None, 100).unwrap()["lines"]
      .as_array()
      .unwrap()
      .iter()
      .all(|line| line["event_code"] != "deployment_failed")
  );
  let second = api_call(
    &paths,
    &mut managers,
    &mut services,
    request("deployment_stop"),
  )
  .unwrap();
  assert_eq!(second["stopped"], json!([]));
  assert_eq!(second["terminal"], first["terminal"]);
  let status = api_call(
    &paths,
    &mut managers,
    &mut services,
    request("deployment_status"),
  )
  .unwrap();
  assert_eq!(status["state"], "stopped");
  assert_eq!(status["terminal"], first["terminal"]);
  let mut recovered = Deployments::new();
  let status = api_call(
    &paths,
    &mut recovered,
    &mut services,
    request("deployment_status"),
  )
  .unwrap();
  assert_eq!(status["state"], "stopped");
  assert_eq!(status["terminal"]["outcome"], "confirmed");
  assert_eq!(status["terminal"]["reason_code"], Value::Null);
  assert!(ensure_launch_history(&paths, "test").is_ok());
  config::atomic_write(
    &paths.runtime.join("test.last-error"),
    b"deployment_stop_unconfirmed: owner recovery required",
  )
  .unwrap();
  let status = api_call(
    &paths,
    &mut recovered,
    &mut services,
    request("deployment_status"),
  )
  .unwrap();
  assert_eq!(status["state"], "stopped");
  assert_eq!(status["last_error"], Value::Null);
}

#[test]
fn unexpected_provider_exit_still_fails_daemon_poll() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  let make = |id: &str, seconds: &str, depends_on: Vec<&str>| process::ProcessSpec {
    id: id.into(),
    path: format!("isos/process/component/{id}"),
    settings: serde_json::from_value(json!({
        "command": ["/bin/sleep", seconds],
        "depends_on": depends_on,
    }))
    .unwrap(),
    package: None,
    fingerprint: None,
  };
  let specs = [
    make("provider", "0.4", vec![]),
    make("bridge", "30", vec!["provider"]),
    make("egia", "30", vec!["bridge"]),
  ];
  let mut managers = Deployments::new();
  restart_processes(&paths, &mut managers, "provider-exit", "a", &specs).unwrap();
  let pids: Vec<_> = managers["provider-exit"]
    .supervisor
    .status_snapshot()
    .iter()
    .map(|process| process.pid)
    .collect();
  std::thread::sleep(Duration::from_millis(450));
  // The daemon tick must stop dependents without waiting for a status client.
  poll_deployments(&paths, &mut managers);
  assert!(managers["provider-exit"].last_error.is_some());
  let terminal = managers["provider-exit"].supervisor.terminal().unwrap();
  assert_eq!(terminal.outcome, "unknown");
  assert_eq!(terminal.reason_code, Some("child_exit_unexpected"));
  assert_eq!(
    terminal
      .processes
      .iter()
      .map(|entry| entry.id.as_str())
      .collect::<Vec<_>>(),
    ["egia", "bridge", "provider"]
  );
  assert!(
    managers["provider-exit"]
      .supervisor
      .status_snapshot()
      .is_empty()
  );
  for pid in pids {
    assert!(!std::path::Path::new(&format!("/proc/{pid}")).exists());
  }
  let logs = log_store::LogStore::new(&paths, "provider-exit").unwrap();
  assert!(
    logs.read("all", None, 100).unwrap()["lines"]
      .as_array()
      .unwrap()
      .iter()
      .any(|line| line["event_code"] == "deployment_failed")
  );
  let (stopped, repeated) = stop_active(
    &paths,
    "provider-exit",
    managers.get_mut("provider-exit").unwrap(),
    false,
  );
  assert!(stopped.is_empty());
  assert_eq!(repeated, terminal);
}

#[test]
fn short_daemon_snapshot_stops_remaining_dependents_without_a_status_request() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  let spec = process::ProcessSpec {
    id: "dependent".into(),
    path: "isos/process/component/dependent".into(),
    settings: serde_json::from_value(json!({"command":["/bin/sleep","30"]})).unwrap(),
    package: None,
    fingerprint: None,
  };
  let mut managers = Deployments::new();
  restart_processes(&paths, &mut managers, "short-snapshot", "a", &[spec]).unwrap();
  let pid = managers["short-snapshot"].supervisor.status_snapshot()[0].pid;
  managers
    .get_mut("short-snapshot")
    .unwrap()
    .expected_processes = 2;
  poll_deployments(&paths, &mut managers);
  let active = &managers["short-snapshot"];
  assert_eq!(active.supervisor.terminal().unwrap().outcome, "unknown");
  assert!(active.supervisor.status_snapshot().is_empty());
  assert!(!std::path::Path::new(&format!("/proc/{pid}")).exists());
}

#[test]
fn grant_plan_preflight_leaves_active_pid_and_current_run_scope_unchanged() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  paths.prepare().unwrap();
  let active_spec = process::ProcessSpec {
    id: "stub".into(),
    path: "isos/process/component/stub".into(),
    settings: serde_json::from_value(json!({"command":["/bin/sleep","30"],"start_timeout_ms":500}))
      .unwrap(),
    package: None,
    fingerprint: None,
  };
  let mut managers = Deployments::new();
  restart_processes(&paths, &mut managers, "test", "old", &[active_spec]).unwrap();
  let original = managers.get_mut("test").unwrap();
  let old_run = original.supervisor.run_id().to_owned();
  let old_pid = original.supervisor.status().unwrap()[0].pid;
  let mut candidate = process::ProcessSpec {
    id: "eos".into(),
    path: "isos/process/component/eos".into(),
    settings: serde_json::from_value(json!({"command":["/bin/true"],"role":"eos"})).unwrap(),
    package: None,
    fingerprint: None,
  };
  let revision = "a".repeat(64);
  let empty = crate::domain::graph::generated_v2::SecretPlan::new();
  let error = restart_processes_with_plan(
    &paths,
    &mut managers,
    "test",
    &revision,
    &[candidate.clone()],
    &empty,
    RunAdmission { managed: true },
  )
  .unwrap_err();
  assert_eq!(
    error
      .downcast_ref::<crate::runtime::owners::control_grants::Diagnostic>()
      .unwrap()
      .code,
    "control_grant_delivery_failed"
  );
  log_prelaunch_failure(&paths, "test", &error, "p-grant");
  let preflight = log_store::LogStore::new(&paths, "test")
    .unwrap()
    .read("isos", None, 20)
    .unwrap();
  let event = preflight["lines"]
    .as_array()
    .unwrap()
    .iter()
    .find(|line| line["event_code"] == "deployment_preflight_failed")
    .unwrap();
  assert_eq!(event["run_id"], manager_session_id());
  assert_eq!(event["correlation_id"], "p-grant");
  assert_eq!(
    event["fields"]["diagnostic_code"],
    "control_grant_delivery_failed"
  );
  let active = managers.get_mut("test").unwrap();
  assert_eq!(active.supervisor.status().unwrap()[0].pid, old_pid);
  assert_eq!(active.supervisor.run_id(), old_run);
  assert_eq!(active.active_revision, "old");
  assert_eq!(
    log_store::LogStore::new(&paths, "test")
      .unwrap()
      .run_identity()
      .0
      .as_deref(),
    Some(old_run.as_str())
  );
  candidate.settings.args = vec!["--run-id=spoof".into()];
  let mut config = candidate.clone();
  config.id = "config".into();
  config.path = "isos/process/component/config".into();
  config.settings.args.clear();
  let error = restart_processes_with_plan(
    &paths,
    &mut managers,
    "test",
    &revision,
    &[candidate, config],
    &empty,
    RunAdmission { managed: true },
  )
  .unwrap_err();
  let diagnostic = error
    .downcast_ref::<crate::runtime::owners::control_grants::Diagnostic>()
    .unwrap();
  assert_eq!(
    diagnostic.path,
    "/cores/isos/layers/process/groups/component/nodes/eos/settings/args/0"
  );
  assert_eq!(
    managers
      .get_mut("test")
      .unwrap()
      .supervisor
      .status()
      .unwrap()[0]
      .pid,
    old_pid
  );
  managers.get_mut("test").unwrap().supervisor.stop();
}

#[test]
fn failed_managed_replacement_rotates_grants_again_for_recovery() {
  let _selected = EmptySelectedTemplate::new();
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  paths.prepare().unwrap();
  let script = temp.path().join("owner-stub.sh");
  fs::write(
            &script,
            concat!(
                "#!/bin/sh\nset -eu\nrun=missing\n",
                "while [ \"$#\" -gt 0 ]; do\n",
                "  if [ \"$1\" = --run-id ]; then shift; run=\"$1\"; fi\n",
                "  shift\ndone\n",
                "if [ \"$STUB_FAIL\" = 1 ]; then exit 1; fi\n",
                "if [ \"$STUB_ID\" = eos ]; then fields='{\"state\":\"ready\",\"node_count\":1}'; message='EOS runtime ready'; ",
                "else fields='{\"state\":\"ready\"}'; message='EOS config ready'; fi\n",
                "printf '{\"timestamp\":\"2026-11-01T10:00:01.000Z\",\"level\":\"info\",",
                "\"source\":\"%s\",\"core\":\"eos\",\"event_code\":\"%s_ready\",",
                "\"run_id\":\"%s\",\"message\":\"%s\",\"fields\":%s}\\n' ",
                "\"$STUB_ID\" \"$STUB_ID\" \"$run\" \"$message\" \"$fields\" >&2\n",
                "sleep 10\n"
            ),
        )
        .unwrap();
  fs::set_permissions(&script, fs::Permissions::from_mode(0o700)).unwrap();
  let make = |id: &str, fail: bool| process::ProcessSpec {
    id: id.into(),
    path: format!("isos/process/component/{id}"),
    settings: serde_json::from_value(json!({
        "command":[script], "role":"eos", "start_timeout_ms":1000,
        "env":{"STUB_ID":id,"STUB_FAIL":if fail {"1"} else {"0"}}
    }))
    .unwrap(),
    package: None,
    fingerprint: None,
  };
  let old_specs = [make("eos", false), make("config", false)];
  let mut managers = Deployments::new();
  let revision = "a".repeat(64);
  let empty = crate::domain::graph::generated_v2::SecretPlan::new();
  let old_projection = json!({"gate_path":"eos/interface/gate/old","projection_revision":"old"});
  restart_processes_with_plan(
    &paths,
    &mut managers,
    "test",
    &revision,
    &old_specs,
    &empty,
    RunAdmission { managed: true },
  )
  .unwrap();
  let old_run = managers.get("test").unwrap().supervisor.run_id().to_owned();
  // Simulate a stale status projection without an attached Vision grant.
  // Recovery must derive its projection from the new grant, not carry this over.
  managers.get_mut("test").unwrap().vision =
    vision_snapshot(&old_run, &revision, Some(old_projection.clone()));
  assert_eq!(
    current_vision_projection(managers.get("test"), Some(&old_run), "running"),
    Some(old_projection.clone())
  );
  let old_bundle = paths.runtime.join(format!("control-grants/test/{old_run}"));
  assert!(old_bundle.is_dir());
  let old_pid = managers
    .get_mut("test")
    .unwrap()
    .supervisor
    .status()
    .unwrap()[0]
    .pid;
  let grant = old_bundle.join("egia-control.grant");
  fs::set_permissions(&grant, fs::Permissions::from_mode(0o644)).unwrap();
  let error = restart_processes_with_plan(
    &paths,
    &mut managers,
    "test",
    &revision,
    &old_specs,
    &empty,
    RunAdmission { managed: true },
  )
  .unwrap_err();
  assert_eq!(
    error
      .downcast_ref::<crate::runtime::owners::control_grants::Diagnostic>()
      .unwrap()
      .code,
    "control_grant_unsafe_path"
  );
  assert_eq!(
    managers
      .get_mut("test")
      .unwrap()
      .supervisor
      .status()
      .unwrap()[0]
      .pid,
    old_pid
  );
  assert_eq!(managers.get("test").unwrap().supervisor.run_id(), old_run);
  assert_eq!(
    current_vision_projection(managers.get("test"), Some(&old_run), "running"),
    Some(old_projection.clone())
  );
  fs::set_permissions(&grant, fs::Permissions::from_mode(0o600)).unwrap();
  let mut candidate_specs = [make("eos", false), make("config", false)];
  candidate_specs[1]
    .settings
    .env
    .insert("ISOS_R03_TEST_FAIL_DELIVERY".into(), "1".into());
  let error = restart_processes_with_plan(
    &paths,
    &mut managers,
    "test",
    &revision,
    &candidate_specs,
    &empty,
    RunAdmission { managed: true },
  )
  .unwrap_err();
  assert!(error.to_string().contains("previous revision restored"));
  assert!(error.to_string().contains("control_grant_delivery_failed"));
  let recovered = managers.get_mut("test").unwrap();
  let recovery_run = recovered.supervisor.run_id().to_owned();
  assert_ne!(recovery_run, old_run);
  assert_eq!(
    current_vision_projection(Some(recovered), Some(&recovery_run), "running"),
    None
  );
  assert!(current_vision_projection(Some(recovered), Some(&old_run), "running").is_none());
  assert_eq!(recovered.supervisor.status().unwrap().len(), 2);
  assert!(!old_bundle.exists());
  assert!(
    paths
      .runtime
      .join(format!("control-grants/test/{recovery_run}"))
      .exists()
  );
  let candidate_run = (0..100)
    .find_map(|_| {
      let events = log_store::LogStore::new(&paths, "test")
        .unwrap()
        .read("all", None, 100)
        .unwrap();
      let run = events["lines"]
        .as_array()
        .unwrap()
        .iter()
        .find(|line| line["event_code"] == "deployment_failed")
        .and_then(|line| line["run_id"].as_str())
        .map(str::to_owned);
      if run.is_none() {
        std::thread::sleep(Duration::from_millis(10));
      }
      run
    })
    .expect("candidate failure event was not committed");
  assert_ne!(candidate_run, old_run);
  assert_ne!(candidate_run, recovery_run);
  assert!(
    !paths
      .runtime
      .join(format!("control-grants/test/{candidate_run}"))
      .exists()
  );
  restart_processes_with_plan(
    &paths,
    &mut managers,
    "test",
    &"b".repeat(64),
    &old_specs,
    &empty,
    RunAdmission { managed: true },
  )
  .unwrap();
  let replacement = managers.get_mut("test").unwrap();
  let replacement_run = replacement.supervisor.run_id().to_owned();
  assert_ne!(replacement_run, recovery_run);
  assert_eq!(
    current_vision_projection(Some(replacement), Some(&replacement_run), "running"),
    None
  );
  assert!(current_vision_projection(Some(replacement), Some(&recovery_run), "running").is_none());
  replacement.supervisor.stop();
}

#[test]
fn rejected_start_is_logged_under_manager_session_before_reply() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  let mut managers = Deployments::new();
  let mut services = services::ServiceManager::new(&paths, "default").unwrap();
  let (mut client, server) = UnixStream::pair().unwrap();
  client
            .write_all(
                b"{\"schema_version\":\"0.1.0\",\"op\":\"deployment_start\",\"payload\":{\"deployment\":\"default\"}}\n",
            )
            .unwrap();
  client.shutdown(Shutdown::Write).unwrap();
  handle_client(&paths, &mut managers, &mut services, server).unwrap();
  let mut reply = String::new();
  client.read_to_string(&mut reply).unwrap();
  assert_eq!(serde_json::from_str::<Value>(&reply).unwrap()["ok"], false);
  assert!(managers.is_empty());
  let lines = log_store::LogStore::new(&paths, "default")
    .unwrap()
    .read("isos", None, 10)
    .unwrap();
  let event = lines["lines"]
    .as_array()
    .unwrap()
    .iter()
    .find(|line| line["event_code"] == "deployment_preflight_failed")
    .unwrap();
  assert_eq!(event["run_id"], manager_session_id());
  assert!(event["correlation_id"].as_str().is_some());
}

#[test]
fn hostile_diagnostic_path_is_omitted_without_losing_preflight_event() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  let error = anyhow::Error::new(graph_compat::Diagnostic {
    path: "/cores/bad\nkey".into(),
    code: "unsupported_core",
    message: "unsupported core".into(),
    context: BTreeMap::new(),
  });
  log_prelaunch_failure(&paths, "default", &error, "p-hostile");
  let lines = log_store::LogStore::new(&paths, "default")
    .unwrap()
    .read("isos", None, 10)
    .unwrap();
  assert_eq!(
    lines["lines"][0]["event_code"],
    "deployment_preflight_failed"
  );
  assert_eq!(
    lines["lines"][0]["fields"]["diagnostic_code"],
    "unsupported_core"
  );
  assert!(lines["lines"][0]["fields"]["diagnostic_path"].is_null());
}

#[test]
fn run_rejects_unowned_core_before_projecting_children() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  let mut graph: Value = serde_json::from_slice(include_bytes!(
    "../../../tests/fixtures/graph_compat/migration_expected_0_1_0.json"
  ))
  .unwrap();
  graph["cores"]["evil"] = json!({"layers":{}});
  let error = validate_saved_graph(&paths, "default", &graph.to_string())
    .unwrap_err()
    .to_string();
  assert!(error.contains("/cores/evil"), "{error}");
  assert!(error.contains("unsupported_core"), "{error}");
}

#[test]
fn typed_package_load_failure_records_exact_pin_and_session() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  let failure = anyhow::Error::new(process::PackageLoadFailure {
    id: "test/sample".into(),
    version: "0.1.1".into(),
    stage: "backend",
    node_path: Some("/cores/eos/layers/process/groups/simulator/nodes/worker".into()),
    source: anyhow::anyhow!("private path /tmp/secret"),
  });
  log_request_preflight_failure(&paths, "default", &failure, "p-request");
  log_package_load_failure(
    &paths,
    "default",
    &anyhow::anyhow!("unrelated EOS binary missing"),
    "p-other",
  );
  let lines = log_store::LogStore::new(&paths, "default")
    .unwrap()
    .read("packages", None, 10)
    .unwrap();
  assert_eq!(lines["lines"].as_array().unwrap().len(), 1);
  let line = &lines["lines"][0];
  assert_eq!(line["event_code"], "package_load_failed");
  assert_eq!(line["fields"]["package_id"], "test/sample");
  assert_eq!(line["fields"]["package_version"], "0.1.1");
  assert_eq!(line["fields"]["stage"], "backend");
  assert_eq!(
    line["node_path"],
    "/cores/eos/layers/process/groups/simulator/nodes/worker"
  );
  assert!(line["fields"]["release_sha256"].is_null());
  assert!(line["fields"]["payload_sha256"].is_null());
  assert_eq!(line["run_id"], manager_session_id());
  assert_eq!(line["correlation_id"], "p-request");
  assert!(!line.to_string().contains("/tmp/secret"));
  let preflight = log_store::LogStore::new(&paths, "default")
    .unwrap()
    .read("isos", None, 10)
    .unwrap();
  assert_eq!(preflight["lines"][0]["correlation_id"], "p-request");
}

#[test]
fn successful_graph_apply_clears_previous_setup_failure() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  fs::create_dir_all(&paths.runtime).unwrap();
  let stale = paths.runtime.join("default.last-error");
  fs::write(&stale, "old setup failure").unwrap();
  crate::app::cli::clear_saved_setup_error(&paths, "default").unwrap();
  assert!(!stale.exists());
  crate::app::cli::clear_saved_setup_error(&paths, "default").unwrap();
}

#[test]
fn external_log_operation_records_selected_event_and_rejects_reserved_source() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  let mut managers = Deployments::new();
  let mut services = services::ServiceManager::new(&paths, "default").unwrap();
  let make_request = |source: &str| {
    WireCommand::LogAppend(LogAppend {
      deployment: "default".into(),
      log: LogEventInput {
        source: source.into(),
        level: "warn".into(),
        message: "adapter unavailable".into(),
        event_code: Some("adapter_unavailable".into()),
      },
    })
  };
  assert_eq!(
    api_call(&paths, &mut managers, &mut services, make_request("eos")).unwrap()["accepted"],
    true
  );
  let stored = log_store::LogStore::new(&paths, "default")
    .unwrap()
    .read("legacy_external", None, 10)
    .unwrap();
  assert_eq!(stored["lines"][0]["event_code"], "legacy_external_event");
  assert_eq!(
    stored["lines"][0]["message"],
    "Legacy external event received"
  );
  assert!(api_call(&paths, &mut managers, &mut services, make_request("isos")).is_err());
}

#[test]
fn failed_restart_with_unexpected_candidate_exit_blocks_restoration() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  let spec = |program: &str| process::ProcessSpec {
    id: "stub".into(),
    path: "isos/process/component/stub".into(),
    settings: serde_json::from_value(json!({
        "command": [program, if program == "/bin/sleep" { "30" } else { "" }],
        "start_timeout_ms": 200
    }))
    .unwrap(),
    package: None,
    fingerprint: None,
  };
  let mut managers = Deployments::new();
  restart_processes(&paths, &mut managers, "test", "old", &[spec("/bin/sleep")]).unwrap();
  let old_run = managers["test"].supervisor.run_id().to_owned();
  let error =
    restart_processes(&paths, &mut managers, "test", "new", &[spec("/bin/false")]).unwrap_err();
  assert!(error.to_string().contains("candidate stop unconfirmed"));
  assert_eq!(
    managers["test"].supervisor.terminal().unwrap().outcome,
    "unknown"
  );
  let (run, state) = log_store::LogStore::new(&paths, "test")
    .unwrap()
    .run_identity();
  assert_eq!(state, "known");
  assert_ne!(run.unwrap(), old_run);
  assert!(restart_processes(&paths, &mut managers, "test", "old", &[spec("/bin/sleep")]).is_err());
}

#[test]
fn failed_initial_start_retains_unknown_and_blocks_retry() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  let spec = |program: &str| process::ProcessSpec {
    id: "stub".into(),
    path: "isos/process/component/stub".into(),
    settings: serde_json::from_value(json!({
        "command": [program, if program == "/bin/sleep" { "30" } else { "" }],
        "start_timeout_ms": 200
    }))
    .unwrap(),
    package: None,
    fingerprint: None,
  };
  let plan = crate::domain::graph::generated_v2::SecretPlan::new();
  let mut managers = Deployments::new();
  assert!(
    start_prepared_processes(
      &paths,
      &mut managers,
      "test",
      "revision",
      &[spec("/bin/false")],
      &plan,
      RunAdmission { managed: false },
    )
    .is_err()
  );
  let terminal = managers["test"].supervisor.terminal().unwrap();
  assert_eq!(terminal.outcome, "unknown");
  let old_run = terminal.run_id.clone();
  let mut services = services::ServiceManager::new(&paths, "test").unwrap();
  let status = api_call(
    &paths,
    &mut managers,
    &mut services,
    WireCommand::DeploymentStatus(DeploymentTarget {
      deployment: "test".into(),
    }),
  )
  .unwrap();
  assert_eq!(status["state"], "degraded");
  assert!(status["vision_evidence"].is_null());
  assert!(
    start_prepared_processes(
      &paths,
      &mut managers,
      "test",
      "revision",
      &[spec("/bin/sleep")],
      &plan,
      RunAdmission { managed: false },
    )
    .is_err()
  );
  assert_eq!(managers["test"].supervisor.run_id(), old_run);
}

#[test]
fn scope_publication_failure_blocks_child_spawn() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  let spec = process::ProcessSpec {
    id: "stub".into(),
    path: "isos/process/component/stub".into(),
    settings: serde_json::from_value(json!({
        "command": ["/bin/sleep", "30"], "start_timeout_ms": 200
    }))
    .unwrap(),
    package: None,
    fingerprint: None,
  };
  let mut managers = Deployments::new();
  let error = start_prepared_processes(
    &paths,
    &mut managers,
    "p07-publish-fail",
    "revision",
    &[spec],
    &crate::domain::graph::generated_v2::SecretPlan::new(),
    RunAdmission { managed: false },
  )
  .unwrap_err();
  assert!(error.downcast_ref::<ScopePublishFailure>().is_some());
  assert!(managers.is_empty());
  let store = log_store::LogStore::new(&paths, "p07-publish-fail").unwrap();
  assert_eq!(store.run_identity(), (None, "none"));
  assert_eq!(
    store.read("all", None, 10).unwrap()["lines"]
      .as_array()
      .unwrap()
      .len(),
    0
  );
}

#[test]
fn managed_scope_publication_failure_retires_prepared_grants() {
  let _selected = EmptySelectedTemplate::new();
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  paths.prepare().unwrap();
  let specs = ["eos", "config"].map(|id| process::ProcessSpec {
    id: id.into(),
    path: format!("isos/process/component/{id}"),
    settings: serde_json::from_value(json!({
        "command": ["/bin/sleep", "30"], "role": "eos", "start_timeout_ms": 200
    }))
    .unwrap(),
    package: None,
    fingerprint: None,
  });
  let mut managers = Deployments::new();
  let error = start_prepared_processes(
    &paths,
    &mut managers,
    "p07-publish-fail",
    &"a".repeat(64),
    &specs,
    &crate::domain::graph::generated_v2::SecretPlan::new(),
    RunAdmission { managed: true },
  )
  .unwrap_err();
  assert!(error.downcast_ref::<ScopePublishFailure>().is_some());
  assert!(managers.is_empty());
  assert_eq!(
    fs::read_dir(paths.runtime.join("control-grants/p07-publish-fail"))
      .unwrap()
      .count(),
    0
  );
}

#[test]
fn restart_reaps_old_child_before_stalled_stop_event() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  let spec = |program: &str| process::ProcessSpec {
    id: "stub".into(),
    path: "isos/process/component/stub".into(),
    settings: serde_json::from_value(json!({
        "command": [program, if program == "/bin/sleep" { "30" } else { "" }],
        "start_timeout_ms": 200
    }))
    .unwrap(),
    package: None,
    fingerprint: None,
  };
  let mut managers = Deployments::new();
  restart_processes(
    &paths,
    &mut managers,
    "p06-stop-order",
    "old",
    &[spec("/bin/sleep")],
  )
  .unwrap();
  let old_pid = managers["p06-stop-order"].supervisor.status_snapshot()[0].pid;
  let thread = std::thread::spawn(move || {
    let result = restart_processes(
      &paths,
      &mut managers,
      "p06-stop-order",
      "new",
      &[spec("/bin/false")],
    );
    if let Some(active) = managers.get_mut("p06-stop-order") {
      active.supervisor.stop();
    }
    result.is_err()
  });
  let gate = stop_log_gate();
  gate.0.wait();
  let reaped = unsafe { libc::kill(old_pid as i32, 0) } == -1
    && std::io::Error::last_os_error().raw_os_error() == Some(libc::ESRCH);
  gate.1.wait();
  assert!(thread.join().unwrap());
  assert!(
    reaped,
    "old child remained live while stop event was stalled"
  );
}

#[test]
fn stalled_terminal_log_does_not_hold_stop_response() {
  let temp = tempfile::tempdir().unwrap();
  let paths = config::Paths {
    config: temp.path().join("config"),
    data: temp.path().join("data"),
    runtime: temp.path().join("run"),
  };
  let spec = process::ProcessSpec {
    id: "stub".into(),
    path: "isos/process/component/stub".into(),
    settings: serde_json::from_value(json!({"command":["/bin/sleep","30"]})).unwrap(),
    package: None,
    fingerprint: None,
  };
  let mut managers = Deployments::new();
  restart_processes(&paths, &mut managers, "r06-stop-log-stall", "old", &[spec]).unwrap();
  let mut active = managers.remove("r06-stop-log-stall").unwrap();
  let run_id = active.supervisor.run_id().to_owned();
  let log_paths = paths.clone();
  let (sender, receiver) = mpsc::sync_channel(1);
  let worker = std::thread::spawn(move || {
    let started = std::time::Instant::now();
    let (_, terminal) = stop_active(&paths, "r06-stop-log-stall", &mut active, false);
    sender.send((terminal, started.elapsed())).unwrap();
  });
  let gate = stalled_stop_log_gate();
  gate.0.wait();
  let response = receiver.recv_timeout(Duration::from_secs(1));
  gate.1.wait();
  let (terminal, elapsed) = response.unwrap();
  assert_eq!(terminal.outcome, "confirmed");
  assert!(elapsed < Duration::from_secs(1));
  worker.join().unwrap();
  let logs = log_store::LogStore::new(&log_paths, "r06-stop-log-stall").unwrap();
  let until = std::time::Instant::now() + Duration::from_secs(2);
  loop {
    let lines = logs.read("all", None, 100).unwrap();
    let events: Vec<_> = lines["lines"]
      .as_array()
      .unwrap()
      .iter()
      .filter(|line| line["run_id"].as_str() == Some(run_id.as_str()))
      .filter(|line| {
        matches!(
          line["event_code"].as_str(),
          Some("deployment_stopping" | "deployment_stopped")
        )
      })
      .collect();
    if events.len() == 2 {
      assert_eq!(events[0]["event_code"], "deployment_stopping");
      assert_eq!(events[1]["event_code"], "deployment_stopped");
      assert_eq!(events[0]["correlation_id"], events[1]["correlation_id"]);
      break;
    }
    assert!(
      std::time::Instant::now() < until,
      "stop events were not written"
    );
    std::thread::sleep(Duration::from_millis(10));
  }
}
