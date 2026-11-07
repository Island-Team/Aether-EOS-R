use std::process::Command;

#[test]
fn obsolete_commands_are_not_exposed_or_executed() {
  let temporary = tempfile::tempdir().unwrap();
  let home = temporary.path().join("isos-home");
  for command in [
    vec!["build"],
    vec!["version", "select", "eos", "abc"],
    vec!["eos", "start"],
    vec!["egia", "start"],
    vec!["v2", "run"],
  ] {
    let output = Command::new(env!("CARGO_BIN_EXE_isos"))
      .args(&command)
      .env("ISOS_HOME", &home)
      .env_remove("ISOS_CONFIG")
      .output()
      .expect("run IsOS CLI");
    assert!(
      !output.status.success(),
      "{command:?} unexpectedly succeeded"
    );
    let stderr = String::from_utf8(output.stderr).unwrap();
    assert!(
      stderr.contains("unrecognized subcommand"),
      "{command:?}: {stderr}"
    );
  }
  assert!(!home.join("data/builds").exists());
}
