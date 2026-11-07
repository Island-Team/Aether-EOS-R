//! IsOS-owned durable root for a selected managed EGIA child.

use crate::{app::config::Paths, runtime::process};
use anyhow::{Result, ensure};
use serde::Serialize;
use serde_json::Value;
use std::{
  fs::{self, File},
  os::unix::fs::{DirBuilderExt, MetadataExt, PermissionsExt},
  path::{Path, PathBuf},
};

#[derive(Debug, Serialize)]
pub struct Diagnostic {
  pub code: &'static str,
  pub stage: &'static str,
  pub path: String,
  pub message: &'static str,
}

impl std::fmt::Display for Diagnostic {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(f, "{}: {}", self.code, self.message)
  }
}
impl std::error::Error for Diagnostic {}

fn unsafe_path() -> anyhow::Error {
  Diagnostic {
    code: "egia_state_unsafe_path",
    stage: "spawn",
    path: String::new(),
    message: "Repair the private EGIA state directory and retry",
  }
  .into()
}

fn reserved(path: String) -> anyhow::Error {
  Diagnostic {
    code: "egia_state_reserved_arg",
    stage: "graph",
    path,
    message: "Remove the owner-reserved EGIA state override and retry",
  }
  .into()
}

const Q01_STALE_TEST_ENV: &str = "EGIA_Q01_STALE_TEST";
const EGIA_SETTINGS: &str = "/cores/isos/layers/process/groups/component/nodes/egia/settings";

fn test_env_reserved(path: String) -> anyhow::Error {
  Diagnostic {
    code: "egia_test_reserved_env",
    stage: "graph",
    path,
    message: "Remove graph-authored EGIA Q01 test enablement and retry",
  }
  .into()
}

pub fn reject_test_env_graph(graph: &Value) -> Result<()> {
  let Some(settings) = graph.pointer(EGIA_SETTINGS) else {
    return Ok(());
  };
  for field in ["env", "env_from"] {
    if settings
      .pointer(&format!("/{field}/{Q01_STALE_TEST_ENV}"))
      .is_some()
    {
      return Err(test_env_reserved(format!(
        "{EGIA_SETTINGS}/{field}/{Q01_STALE_TEST_ENV}"
      )));
    }
  }
  Ok(())
}

pub fn reject_test_env_spec(spec: &process::ProcessSpec) -> Result<()> {
  if spec.id != "egia" {
    return Ok(());
  }
  if spec.settings.env.contains_key(Q01_STALE_TEST_ENV) {
    return Err(test_env_reserved(format!(
      "{EGIA_SETTINGS}/env/{Q01_STALE_TEST_ENV}"
    )));
  }
  if spec.settings.env_from.contains_key(Q01_STALE_TEST_ENV) {
    return Err(test_env_reserved(format!(
      "{EGIA_SETTINGS}/env_from/{Q01_STALE_TEST_ENV}"
    )));
  }
  Ok(())
}

pub fn validate_authored(specs: &[process::ProcessSpec]) -> Result<()> {
  let Some(spec) = specs.iter().find(|spec| spec.id == "egia") else {
    return Ok(());
  };
  reject_test_env_spec(spec)?;
  let base = "/cores/isos/layers/process/groups/component/nodes/egia/settings";
  for (index, arg) in spec.settings.args.iter().enumerate() {
    if arg == "--state-root" || arg.starts_with("--state-root=") {
      return Err(reserved(format!("{base}/args/{index}")));
    }
  }
  if spec.settings.env.contains_key("ISLAND_STATE") {
    return Err(reserved(format!("{base}/env/ISLAND_STATE")));
  }
  if spec.settings.env_from.contains_key("ISLAND_STATE") {
    return Err(reserved(format!("{base}/env_from/ISLAND_STATE")));
  }
  Ok(())
}

fn inspect_private(path: &Path) -> Result<()> {
  let metadata = fs::symlink_metadata(path).map_err(|_| unsafe_path())?;
  ensure!(
    metadata.file_type().is_dir()
      && metadata.uid() == unsafe { libc::geteuid() }
      && metadata.permissions().mode() & 0o7777 == 0o700,
    unsafe_path()
  );
  Ok(())
}

fn inspect_ancestors(path: &Path) -> Result<()> {
  for ancestor in path.ancestors() {
    let metadata = fs::symlink_metadata(ancestor).map_err(|_| unsafe_path())?;
    ensure!(metadata.file_type().is_dir(), unsafe_path());
  }
  Ok(())
}

fn ensure_private(parent: &Path, name: &str) -> Result<PathBuf> {
  inspect_private(parent)?;
  let child = parent.join(name);
  match fs::symlink_metadata(&child) {
    Ok(_) => inspect_private(&child)?,
    Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
      fs::DirBuilder::new()
        .mode(0o700)
        .create(&child)
        .map_err(|_| unsafe_path())?;
      inspect_private(&child)?;
      File::open(parent)
        .and_then(|file| file.sync_all())
        .map_err(|_| unsafe_path())?;
    }
    Err(_) => return Err(unsafe_path()),
  }
  Ok(child)
}

pub fn prepare(paths: &Paths, deployment: &str, specs: &mut [process::ProcessSpec]) -> Result<()> {
  let Some(spec) = specs.iter_mut().find(|spec| spec.id == "egia") else {
    return Ok(());
  };
  process::validate_deployment_name(deployment)?;
  ensure!(paths.data.is_absolute(), unsafe_path());
  inspect_ancestors(&paths.data)?;
  inspect_private(&paths.data)?;
  let state = ensure_private(&paths.data, "state")?;
  let root = ensure_private(&state, deployment)?;
  for child in ["egia", "egia/safety", "egia/operations", "egia/missions"] {
    let child = root.join(child);
    match fs::symlink_metadata(&child) {
      Ok(_) => inspect_private(&child)?,
      Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
      Err(_) => return Err(unsafe_path()),
    }
  }
  let root = root.to_str().ok_or_else(unsafe_path)?.to_owned();
  spec.settings.command.extend(["--state-root".into(), root]);
  Ok(())
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::runtime::process::{ProcessSettings, ProcessSpec};
  use serde_json::json;
  use std::os::unix::fs::symlink;

  fn spec() -> ProcessSpec {
    ProcessSpec {
      id: "egia".into(),
      path: "isos/process/component/egia".into(),
      settings: ProcessSettings {
        command: vec!["/bin/true".into()],
        role: Some("egia".into()),
        args: vec![],
        cwd: None,
        env: Default::default(),
        env_from: Default::default(),
        skill_libraries: vec![],
        ready_tcp: None,
        ready_unix: None,
        start_timeout_ms: 30_000,
        restart: Default::default(),
        max_restarts: 0,
        depends_on: vec![],
      },
      package: None,
      fingerprint: None,
    }
  }

  fn paths() -> (tempfile::TempDir, Paths) {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    (temp, paths)
  }

  fn diagnostic(error: anyhow::Error) -> Diagnostic {
    error.downcast::<Diagnostic>().unwrap()
  }

  #[test]
  fn owner_root_is_stable_and_private_per_deployment() {
    let (_temp, paths) = paths();
    let mut first = [spec()];
    prepare(&paths, "alpha", &mut first).unwrap();
    let root = paths.data.join("state/alpha");
    assert_eq!(
      first[0].settings.command[1..],
      ["--state-root", root.to_str().unwrap()]
    );
    assert_eq!(
      fs::symlink_metadata(&root).unwrap().permissions().mode() & 0o7777,
      0o700
    );
    let mut again = [spec()];
    prepare(&paths, "alpha", &mut again).unwrap();
    assert_eq!(first[0].settings.command, again[0].settings.command);
    let mut second = [spec()];
    prepare(&paths, "beta", &mut second).unwrap();
    assert_ne!(second[0].settings.command, first[0].settings.command);
  }

  #[test]
  fn authored_overrides_report_exact_paths() {
    for (kind, expected) in [
      ("split", "/args/0"),
      ("equals", "/args/0"),
      ("env", "/env/ISLAND_STATE"),
      ("env_from", "/env_from/ISLAND_STATE"),
    ] {
      let mut spec = spec();
      match kind {
        "split" => spec.settings.args = vec!["--state-root".into(), "/tmp/x".into()],
        "equals" => spec.settings.args = vec!["--state-root=/tmp/x".into()],
        "env" => {
          spec
            .settings
            .env
            .insert("ISLAND_STATE".into(), "/tmp/x".into());
        }
        _ => {
          spec
            .settings
            .env_from
            .insert("ISLAND_STATE".into(), "PARENT_STATE".into());
        }
      }
      let error = diagnostic(validate_authored(&[spec]).unwrap_err());
      assert_eq!(error.code, "egia_state_reserved_arg");
      assert_eq!(error.stage, "graph");
      assert_eq!(
        error.path,
        format!("/cores/isos/layers/process/groups/component/nodes/egia/settings{expected}")
      );
    }
  }

  #[test]
  fn q01_test_flag_is_forbidden_only_as_egia_graph_target() {
    let fixture: Value = serde_json::from_str(include_str!(
      "../../../tests/fixtures/q01_egia_stale_env/cases.json"
    ))
    .unwrap();
    for case in fixture["cases"].as_array().unwrap() {
      let mut graph = json!({});
      graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["egia"]["settings"] =
        case["settings"].clone();
      let error = reject_test_env_graph(&graph).unwrap_err();
      let diagnostic = error.downcast_ref::<Diagnostic>().unwrap();
      assert_eq!(diagnostic.code, fixture["code"].as_str().unwrap());
      assert_eq!(diagnostic.stage, fixture["stage"].as_str().unwrap());
      assert_eq!(diagnostic.path, case["path"].as_str().unwrap());
      let mut spec = spec();
      if case["field"] == "env" {
        spec
          .settings
          .env
          .insert(Q01_STALE_TEST_ENV.into(), "1".into());
      } else {
        spec
          .settings
          .env_from
          .insert(Q01_STALE_TEST_ENV.into(), "INHERITED_TEST_FLAG".into());
      }
      let error = reject_test_env_spec(&spec).unwrap_err();
      assert_eq!(
        error.downcast_ref::<Diagnostic>().unwrap().path,
        case["path"].as_str().unwrap()
      );
      graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["other"] =
        json!({"settings":case["settings"]});
      graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["egia"]["settings"] =
        json!({});
      reject_test_env_graph(&graph).unwrap();
    }
    let mut spec = spec();
    spec
      .settings
      .env_from
      .insert("UNRELATED".into(), Q01_STALE_TEST_ENV.into());
    reject_test_env_spec(&spec).unwrap();
  }

  #[test]
  fn unsafe_existing_paths_fail_without_repair() {
    for kind in [
      "symlink",
      "file",
      "broad",
      "deployment_symlink",
      "egia_broad",
    ] {
      let (_temp, paths) = paths();
      let state = paths.data.join("state");
      match kind {
        "symlink" => symlink("/tmp", &state).unwrap(),
        "file" => fs::write(&state, "x").unwrap(),
        "broad" => {
          fs::create_dir(&state).unwrap();
          fs::set_permissions(&state, fs::Permissions::from_mode(0o755)).unwrap();
        }
        "deployment_symlink" => {
          fs::create_dir(&state).unwrap();
          symlink("/tmp", state.join("alpha")).unwrap();
        }
        _ => {
          fs::create_dir(&state).unwrap();
          let root = state.join("alpha");
          fs::create_dir(&root).unwrap();
          fs::create_dir(root.join("egia")).unwrap();
          fs::set_permissions(root.join("egia"), fs::Permissions::from_mode(0o755)).unwrap();
        }
      }
      let error = diagnostic(prepare(&paths, "alpha", &mut [spec()]).unwrap_err());
      assert_eq!(error.code, "egia_state_unsafe_path");
      if matches!(kind, "symlink" | "file" | "broad") {
        assert!(!state.join("alpha").exists());
      }
    }
  }

  #[test]
  fn invalid_deployment_cannot_create_state_path() {
    let (_temp, paths) = paths();
    assert!(prepare(&paths, "../escape", &mut [spec()]).is_err());
    assert!(!paths.data.join("state").exists());
  }
}
