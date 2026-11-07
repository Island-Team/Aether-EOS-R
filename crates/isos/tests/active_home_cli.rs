use std::{fs, os::unix::fs::PermissionsExt, path::PathBuf, process::Command};

fn fixture() -> (PathBuf, PathBuf, PathBuf) {
  let nonce = std::time::SystemTime::now()
    .duration_since(std::time::UNIX_EPOCH)
    .unwrap()
    .as_nanos();
  let root = std::env::temp_dir().join(format!("isos-active-home-{}-{nonce}", std::process::id()));
  let xdg = root.join("xdg");
  let old = xdg.join("isos/deployments/default.nodes.json");
  fs::create_dir_all(old.parent().unwrap()).unwrap();
  fs::write(&old, b"old graph must not change").unwrap();
  let selected = root.join("selected");
  fs::create_dir_all(selected.join("config")).unwrap();
  fs::write(
    selected.join("config/isos.toml"),
    b"[server]\nhttp_port=37125\nui_port=37126\n",
  )
  .unwrap();
  (root, xdg, selected)
}

fn check(xdg: &PathBuf) -> std::process::Output {
  Command::new(env!("CARGO_BIN_EXE_isos"))
    .args(["version", "check", "--json"])
    .env("XDG_CONFIG_HOME", xdg)
    .env_remove("ISOS_HOME")
    .env_remove("ISOS_CONFIG")
    .env_remove("ISOS_HTTP_PORT")
    .env_remove("ISOSUI_PORT")
    .output()
    .unwrap()
}

#[test]
fn installed_activation_selects_new_home_without_touching_old_graph() {
  let (root, xdg, selected) = fixture();
  let active = xdg.join("isos/active.env");
  fs::write(
        &active,
        format!(
            "export ISOS_HOME={}\nexport ISOS_CONFIG={}\nexport ISOS_HTTP_PORT=37125\nexport ISOSUI_PORT=37126\n",
            selected.display(),
            selected.join("config/isos.toml").display()
        ),
    )
    .unwrap();
  fs::set_permissions(&active, fs::Permissions::from_mode(0o600)).unwrap();
  let output = check(&xdg);
  assert!(
    output.status.success(),
    "{}",
    String::from_utf8_lossy(&output.stderr)
  );
  let report: serde_json::Value = serde_json::from_slice(&output.stdout).unwrap();
  assert_eq!(
    report["graph"]["path"],
    selected
      .join("config/deployments/default.nodes.json")
      .to_string_lossy()
      .as_ref()
  );
  assert_eq!(
    fs::read(xdg.join("isos/deployments/default.nodes.json")).unwrap(),
    b"old graph must not change"
  );
  fs::remove_dir_all(root).unwrap();
}

#[test]
fn unsafe_activation_fails_before_graph_access() {
  let (root, xdg, selected) = fixture();
  let active = xdg.join("isos/active.env");
  fs::write(
    &active,
    format!("export ISOS_HOME={}\n", selected.display()),
  )
  .unwrap();
  fs::set_permissions(&active, fs::Permissions::from_mode(0o644)).unwrap();
  let output = check(&xdg);
  assert!(!output.status.success());
  assert!(String::from_utf8_lossy(&output.stderr).contains("active.env must be owner-only"));
  assert_eq!(
    fs::read(xdg.join("isos/deployments/default.nodes.json")).unwrap(),
    b"old graph must not change"
  );
  fs::remove_dir_all(root).unwrap();
}

#[test]
fn unsupported_activation_key_names_key_and_configuration_guide() {
  let (root, xdg, selected) = fixture();
  let active = xdg.join("isos/active.env");
  fs::write(
    &active,
    format!(
      "export ISOS_HOME={}\nexport ISOS_HTTP_PORT=37125\nexport ISOSUI_PORT=37126\nexport MUJOCO_ROOT=/tmp/example\n",
      selected.display()
    ),
  )
  .unwrap();
  fs::set_permissions(&active, fs::Permissions::from_mode(0o600)).unwrap();
  let output = check(&xdg);
  assert!(!output.status.success());
  let stderr = String::from_utf8_lossy(&output.stderr);
  assert!(stderr.contains("unsupported key MUJOCO_ROOT"), "{stderr}");
  assert!(
    stderr.contains("docs/isos/usage/configuration.md"),
    "{stderr}"
  );
  fs::remove_dir_all(root).unwrap();
}
