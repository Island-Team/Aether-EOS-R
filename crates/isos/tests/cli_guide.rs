use std::process::Command;

fn isos(home: &std::path::Path, args: &[&str]) -> std::process::Output {
  Command::new(env!("CARGO_BIN_EXE_isos"))
    .args(args)
    .env("ISOS_HOME", home)
    .env_remove("ISOS_CONFIG")
    .env_remove("ISOS_MODE")
    .output()
    .expect("run IsOS CLI")
}

#[test]
fn unmapped_legacy_cli_validate_and_apply_fail_at_exact_pointer_without_write() {
  let temp = tempfile::tempdir().unwrap();
  let home = temp.path().join("home");
  let source = include_bytes!("fixtures/graph_compat/legacy_unmapped_1_0_0.json");
  let path = temp.path().join("legacy.json");
  std::fs::write(&path, source).unwrap();
  let name = path.to_str().unwrap();
  let expected = "/cores/eos/layers/control/groups/manual/nodes/manual_0/node_type";
  for args in [
    vec!["config", "validate", name],
    vec!["config", "apply", name],
  ] {
    let output = isos(&home, &args);
    assert!(!output.status.success(), "{args:?}");
    let stderr = String::from_utf8(output.stderr).unwrap();
    assert!(stderr.contains(expected), "{stderr}");
    assert!(stderr.contains("unmapped_node_type"), "{stderr}");
  }
  assert_eq!(std::fs::read(&path).unwrap(), source);
  assert!(!home.join("config/deployments/default.nodes.json").exists());
}

#[test]
fn skill_selection_cli_validate_rejects_exact_field_before_reader_launch() {
  let temp = tempfile::tempdir().unwrap();
  let home = temp.path().join("home");
  let path = temp.path().join("selection.json");
  let mut graph = serde_json::json!({"version":"0.1.0","robot":{},"cores":{"isos":{"layers":{"process":{"groups":{"component":{"nodes":{
      "eos":{"id":"eos","node_type":"isos.process.component@v0.1","settings":{"role":"eos","skill_libraries":[
          {"package":{"id":"test/skill","version":"0.1.0"},"library":"x.skill.so"},
          {"package":{"id":"test/skill","version":"0.1.0"},"library":"x.skill.so"}
      ]}}
  }}}}}}}});
  std::fs::write(&path, serde_json::to_vec(&graph).unwrap()).unwrap();
  let output = isos(&home, &["config", "validate", path.to_str().unwrap()]);
  assert!(!output.status.success());
  let stderr = String::from_utf8(output.stderr).unwrap();
  assert!(stderr.contains("skill_selection_invalid"), "{stderr}");
  assert!(
    stderr
      .contains("/cores/isos/layers/process/groups/component/nodes/eos/settings/skill_libraries/1"),
    "{stderr}"
  );
  graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["eos"]["settings"]
    ["skill_libraries"] = serde_json::json!([{"package":{"id":"test/skill","version":"0.1.0"},"library":"../escape.skill.so"}]);
  std::fs::write(&path, serde_json::to_vec(&graph).unwrap()).unwrap();
  let output = isos(&home, &["config", "validate", path.to_str().unwrap()]);
  assert!(!output.status.success());
  let stderr = String::from_utf8(output.stderr).unwrap();
  assert!(stderr.contains("skill_library_unsafe"), "{stderr}");
  assert!(
    stderr.contains(
      "/cores/isos/layers/process/groups/component/nodes/eos/settings/skill_libraries/0/library"
    ),
    "{stderr}"
  );
  assert!(!home.join("config/deployments/default.nodes.json").exists());
}

#[test]
fn failed_cli_apply_is_the_manager_scope_anchor_but_validate_is_not() {
  let temp = tempfile::tempdir().unwrap();
  let home = temp.path().join("home");
  let source = include_bytes!("fixtures/graph_compat/legacy_unmapped_1_0_0.json");
  let path = temp.path().join("legacy.json");
  std::fs::write(&path, source).unwrap();
  let name = path.to_str().unwrap();
  assert!(!isos(&home, &["config", "validate", name]).status.success());
  let before = isos(&home, &["logs", "all", "--current"]);
  assert!(before.status.success());
  assert!(before.stdout.is_empty());
  assert!(!isos(&home, &["config", "apply", name]).status.success());
  let after = isos(&home, &["logs", "eos", "--current", "--level", "error"]);
  assert!(
    after.status.success(),
    "{}",
    String::from_utf8_lossy(&after.stderr)
  );
  let anchor: serde_json::Value = serde_json::from_slice(after.stdout.trim_ascii()).unwrap();
  assert_eq!(anchor["event_code"], "deployment_preflight_failed");
  assert_eq!(anchor["source"], "isos");
  let status = isos(&home, &["status", "--json"]);
  assert!(status.status.success());
  let body: serde_json::Value = serde_json::from_slice(&status.stdout).unwrap();
  assert_eq!(body["run_id_state"], "none");
}

#[test]
fn help_prints_quick_start_without_creating_home() {
  let temp = tempfile::tempdir().unwrap();
  let home = temp.path().join("isos-home");
  let output = isos(&home, &["--help"]);
  assert!(output.status.success());
  let stdout = String::from_utf8(output.stdout).unwrap();
  assert!(stdout.contains("workbench"));
  assert!(stdout.contains("dev"));
  assert!(stdout.contains("product"));
  assert!(!home.exists());
}

#[test]
fn init_prints_next_steps_without_starting_manager() {
  let temp = tempfile::tempdir().unwrap();
  let home = temp.path().join("isos-home");
  let output = isos(&home, &["init", "--mode", "dev"]);
  assert!(output.status.success());
  let stdout = String::from_utf8(output.stdout).unwrap();
  assert!(stdout.contains("editable graph:"));
  assert!(stdout.contains("Next steps:"));
  assert!(stdout.contains("isos version check"));
  assert!(stdout.contains("isos                               # open the workbench"));
  assert!(home.join("config/deployments/default.nodes.json").exists());
  assert!(!home.join("run/manager.sock").exists());
}

#[test]
fn product_init_mentions_configured_builds_without_starting_manager() {
  let temp = tempfile::tempdir().unwrap();
  let home = temp.path().join("isos-home");
  let output = isos(&home, &["init", "--mode", "product"]);
  assert!(output.status.success());
  let stdout = String::from_utf8(output.stdout).unwrap();
  assert!(stdout.contains("ISOS_AETHER_BIN_DIR"));
  assert!(stdout.contains("ISOSUI_DIST_DIR"));
  assert!(stdout.contains("isos version check"));
  assert!(!home.join("run/manager.sock").exists());
}

#[test]
fn version_check_defaults_to_readable_report_and_supports_json() {
  let temp = tempfile::tempdir().unwrap();
  let home = temp.path().join("isos-home");
  let plain = isos(&home, &["version", "check"]);
  assert!(plain.status.success());
  let text = String::from_utf8(plain.stdout).unwrap();
  assert!(text.contains("Graph configuration:"));
  assert!(text.contains("Running:"));
  assert!(text.contains("Local version support: not_configured | target graph 0.2.0"));
  let machine = isos(&home, &["version", "check", "--json"]);
  assert!(machine.status.success());
  let report: serde_json::Value = serde_json::from_slice(&machine.stdout).unwrap();
  assert_eq!(report["cores"][0]["core"], "eos");
  assert!(report["cores"][0]["ready"].is_boolean());
  assert_eq!(report["cores"][0]["enabled"], false);
  assert_eq!(report["version_support"]["status"], "not_configured");
  assert_eq!(report["version_support"]["map"]["product_version"], "0.2.1");
  assert_eq!(
    report["version_support"]["map"]["target_graph_format"],
    "0.2.0"
  );
}
