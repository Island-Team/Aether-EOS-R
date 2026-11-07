use anyhow::{Context, Result, ensure};
use serde::Deserialize;
use std::{
  collections::BTreeMap,
  fs,
  io::Write,
  os::unix::fs::{OpenOptionsExt, PermissionsExt},
  path::{Path, PathBuf},
};

#[derive(Debug, Clone)]
pub struct Paths {
  pub config: PathBuf,
  pub data: PathBuf,
  pub runtime: PathBuf,
}

#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct RuntimeConfig {
  #[serde(default)]
  server: ServerConfig,
  #[serde(default)]
  paths: RuntimePaths,
  #[serde(default)]
  deployment: DeploymentConfig,
  #[serde(default)]
  logging: LoggingConfig,
  #[serde(default)]
  vision_evidence: VisionEvidenceConfig,
  #[serde(default)]
  package_host_inputs: BTreeMap<String, PathBuf>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct VisionEvidenceConfig {
  gate_path: Option<String>,
}

#[cfg(test)]
thread_local! {
    static TEST_VISION_CONFIG_PATH: std::cell::RefCell<Option<PathBuf>> = const { std::cell::RefCell::new(None) };
}

#[cfg(test)]
pub(crate) fn test_vision_config_path(path: Option<PathBuf>) {
  TEST_VISION_CONFIG_PATH.with(|selected| *selected.borrow_mut() = path);
}

/// The selection is read from the selected source, never from an inherited child environment.
pub fn vision_gate_selection() -> Result<(Option<String>, String)> {
  use sha2::{Digest, Sha256};
  let path = runtime_config_path(
    Path::new(env!("CARGO_MANIFEST_DIR")),
    std::env::var_os("ISOS_CONFIG").as_deref(),
  )?;
  #[cfg(test)]
  let path = TEST_VISION_CONFIG_PATH
    .with(|selected| selected.borrow().clone())
    .unwrap_or(path);
  let source = merged_config_source(&path, std::env::var_os("ISOS_CONFIG").is_some())?;
  let config: RuntimeConfig = toml::from_str(&source)?;
  let gate = config
    .vision_evidence
    .gate_path
    .filter(|value| !value.is_empty());
  Ok((gate, hex::encode(Sha256::digest(source.as_bytes()))))
}

#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct ServerConfig {
  http_port: Option<u16>,
  ui_port: Option<u16>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct RuntimePaths {
  ui_repo: Option<PathBuf>,
  ui_dist: Option<PathBuf>,
  aether_bin_dir: Option<PathBuf>,
  ei_bin_dir: Option<PathBuf>,
  ei_defaults_dir: Option<PathBuf>,
  ei_factory_defaults_dir: Option<PathBuf>,
  egia_bin_dir: Option<PathBuf>,
  egia_defaults_dir: Option<PathBuf>,
  egia_factory_defaults_dir: Option<PathBuf>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct DeploymentConfig {
  name: Option<String>,
  stop_timeout_ms: Option<u64>,
  eos_stop_grace_ms: Option<u64>,
}

#[derive(Debug, Clone, Copy)]
pub struct StopBudget {
  pub timeout_ms: u64,
  pub eos_grace_ms: u64,
}

#[derive(Debug, serde::Serialize)]
pub struct StopBudgetDiagnostic {
  pub code: &'static str,
  pub stage: &'static str,
  pub path: &'static str,
}

impl std::fmt::Display for StopBudgetDiagnostic {
  fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(formatter, "{}: {}", self.code, self.path)
  }
}

impl std::error::Error for StopBudgetDiagnostic {}

impl StopBudget {
  fn from_deployment(config: &DeploymentConfig) -> Result<Self> {
    let timeout_ms = config.stop_timeout_ms.unwrap_or(30_000);
    ensure!(
      (5_000..=120_000).contains(&timeout_ms),
      StopBudgetDiagnostic {
        code: "stop_budget_invalid",
        stage: "config",
        path: "/deployment/stop_timeout_ms"
      }
    );
    let eos_grace_ms = config.eos_stop_grace_ms.unwrap_or(20_000);
    ensure!(
      (1_000..=timeout_ms - 3_000).contains(&eos_grace_ms),
      StopBudgetDiagnostic {
        code: "stop_budget_invalid",
        stage: "config",
        path: "/deployment/eos_stop_grace_ms"
      }
    );
    Ok(Self {
      timeout_ms,
      eos_grace_ms,
    })
  }
}

pub fn stop_budget() -> Result<StopBudget> {
  let path = runtime_config_path(
    Path::new(env!("CARGO_MANIFEST_DIR")),
    std::env::var_os("ISOS_CONFIG").as_deref(),
  )?;
  let source = merged_config_source(&path, std::env::var_os("ISOS_CONFIG").is_some())?;
  let config: RuntimeConfig = toml::from_str(&source)?;
  StopBudget::from_deployment(&config.deployment)
}

#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct LoggingConfig {
  default_source: Option<String>,
  max_bytes: Option<u64>,
  backups: Option<u8>,
  max_sources: Option<u16>,
  level: Option<String>,
}

fn config_values(source: &str, repo_root: &Path) -> Result<BTreeMap<String, String>> {
  let config: RuntimeConfig = toml::from_str(source)?;
  for (ei, legacy, key) in [
    (
      &config.paths.ei_bin_dir,
      &config.paths.egia_bin_dir,
      "bin_dir",
    ),
    (
      &config.paths.ei_defaults_dir,
      &config.paths.egia_defaults_dir,
      "defaults_dir",
    ),
    (
      &config.paths.ei_factory_defaults_dir,
      &config.paths.egia_factory_defaults_dir,
      "factory_defaults_dir",
    ),
  ] {
    ensure!(
      ei.is_none() || legacy.is_none() || ei == legacy,
      "paths.ei_{key} conflicts with paths.egia_{key}"
    );
  }
  StopBudget::from_deployment(&config.deployment)?;
  let mut values = BTreeMap::new();
  for (port, key) in [
    (config.server.http_port, "ISOS_HTTP_PORT"),
    (config.server.ui_port, "ISOSUI_PORT"),
  ] {
    if let Some(port) = port {
      ensure!(port >= 1024, "{key} must be 1024..65535");
      values.insert(key.into(), port.to_string());
    }
  }
  for (path, key) in [
    (config.paths.ui_repo, "ISOSUI_REPO_PATH"),
    (config.paths.ui_dist, "ISOSUI_DIST_DIR"),
    (config.paths.aether_bin_dir, "ISOS_AETHER_BIN_DIR"),
    (
      config.paths.ei_bin_dir.or(config.paths.egia_bin_dir),
      "ISOS_EI_BIN_DIR",
    ),
    (
      config
        .paths
        .ei_defaults_dir
        .or(config.paths.egia_defaults_dir),
      "ISOS_EI_DEFAULTS_DIR",
    ),
    (
      config
        .paths
        .ei_factory_defaults_dir
        .or(config.paths.egia_factory_defaults_dir),
      "ISOS_EI_FACTORY_DEFAULTS_DIR",
    ),
  ] {
    if let Some(path) = path {
      let path = if path.is_absolute() {
        path
      } else {
        repo_root.join(path)
      };
      values.insert(key.into(), path.to_string_lossy().into_owned());
    }
  }
  if let Some(name) = config.deployment.name {
    crate::runtime::process::validate_deployment_name(&name)?;
    values.insert("ISOS_DEPLOYMENT".into(), name);
  }
  if let Some(source) = config.logging.default_source {
    ensure!(
      source == "all" || crate::runtime::logs::valid_source(&source),
      "logging.default_source must be all or a valid log source"
    );
    values.insert("ISOS_LOG_DEFAULT_SOURCE".into(), source);
  }
  if let Some(max_bytes) = config.logging.max_bytes {
    ensure!(
      (65_536..=100_000_000).contains(&max_bytes),
      "logging.max_bytes must be 65536..100000000"
    );
    values.insert("ISOS_LOG_MAX_BYTES".into(), max_bytes.to_string());
  }
  if let Some(backups) = config.logging.backups {
    ensure!((1..=10).contains(&backups), "logging.backups must be 1..10");
    values.insert("ISOS_LOG_BACKUPS".into(), backups.to_string());
  }
  if let Some(max_sources) = config.logging.max_sources {
    ensure!(
      (10..=256).contains(&max_sources),
      "logging.max_sources must be 10..256"
    );
    values.insert("ISOS_LOG_MAX_SOURCES".into(), max_sources.to_string());
  }
  if let Some(level) = config.logging.level {
    ensure!(
      crate::runtime::logs::level_rank(&level).is_some(),
      "logging.level must be trace, debug, info, warn, or error"
    );
    values.insert("ISOS_LOG_LEVEL".into(), level);
  }
  for (key, path) in config.package_host_inputs {
    ensure!(
      key.len() <= 64
        && key
          .bytes()
          .next()
          .is_some_and(|byte| byte.is_ascii_uppercase())
        && key
          .bytes()
          .all(|byte| byte.is_ascii_uppercase() || byte.is_ascii_digit() || byte == b'_')
        && !key.starts_with("ISOS_")
        && !key.starts_with("ISLAND_")
        && !key.starts_with("EGIA_"),
      "invalid package_host_inputs key {key}"
    );
    ensure!(
      path.is_absolute(),
      "package_host_inputs.{key} must be an absolute path"
    );
    ensure!(
      values
        .insert(key.clone(), path.to_string_lossy().into_owned())
        .is_none(),
      "package_host_inputs.{key} conflicts with a built-in setting"
    );
  }
  Ok(values)
}

fn config_without_env_overrides(
  values: BTreeMap<String, String>,
  has_env: impl Fn(&str) -> bool,
) -> BTreeMap<String, String> {
  values
    .into_iter()
    .filter(|(key, _)| !has_env(key))
    .collect()
}

fn runtime_config_path(
  repo_root: &Path,
  override_path: Option<&std::ffi::OsStr>,
) -> Result<PathBuf> {
  if let Some(path) = override_path {
    let path = PathBuf::from(path);
    ensure!(path.is_absolute(), "ISOS_CONFIG must be an absolute path");
    Ok(path)
  } else {
    Ok(repo_root.join("config/isos.toml"))
  }
}

fn fill_missing_toml(selected: &mut toml::Value, defaults: toml::Value) {
  if let (toml::Value::Table(base), toml::Value::Table(fallbacks)) = (selected, defaults) {
    for (key, value) in fallbacks {
      if let Some(existing) = base.get_mut(&key) {
        fill_missing_toml(existing, value);
      } else {
        base.insert(key, value);
      }
    }
  }
}

/// Read the selected file first, then fill its missing fields from the example.
/// TOML comments never become configuration values.
fn merged_config_source(path: &Path, required: bool) -> Result<String> {
  let selected = match fs::read_to_string(path) {
    Ok(source) => source,
    Err(error) if error.kind() == std::io::ErrorKind::NotFound && !required => String::new(),
    Err(error) => return Err(error).with_context(|| format!("read {}", path.display())),
  };
  let mut merged: toml::Value = if selected.trim().is_empty() {
    toml::Value::Table(toml::map::Map::new())
  } else {
    toml::from_str(&selected).with_context(|| format!("parse {}", path.display()))?
  };
  let defaults: toml::Value = toml::from_str(include_str!("../../config/isos.example.toml"))
    .context("parse config/isos.example.toml")?;
  fill_missing_toml(&mut merged, defaults);
  Ok(toml::to_string(&merged)?)
}

fn config_guide() -> String {
  format!(
    "file://{}/docs/isos/usage/configuration.md",
    env!("CARGO_MANIFEST_DIR")
  )
}

const ACTIVE_ENV_KEYS: &[&str] = &[
  "ISOS_HOME",
  "ISOS_CONFIG",
  "ISOS_HTTP_PORT",
  "ISOSUI_PORT",
  "ISOS_AETHER_BIN_DIR",
  "ISOSUI_DIST_DIR",
  "ISOS_EGIA_BIN_DIR",
  "ISOS_EI_BIN_DIR",
  "ISOS_EGIA_DEFAULTS_DIR",
  "ISOS_EI_DEFAULTS_DIR",
  "ISOS_EGIA_FACTORY_DEFAULTS_DIR",
  "ISOS_EI_FACTORY_DEFAULTS_DIR",
  "EGIA_PYTHON",
  "EI_PYTHON",
  "ISLAND_PREFIX",
  "ISLAND_MODE",
  "ISLAND_ETC",
  "ISLAND_SHARE",
  "ISLAND_CONFIG",
  "EGIA_ETC",
  "ISLAND_CACHE",
  "ISLAND_LOG",
  "ISLAND_RUN",
  "ISLAND_TMP",
];

fn active_env_path() -> Result<PathBuf> {
  let home = std::env::var_os("HOME").ok_or_else(|| anyhow::anyhow!("HOME is required"))?;
  let config = std::env::var_os("XDG_CONFIG_HOME")
    .map(PathBuf::from)
    .unwrap_or_else(|| PathBuf::from(home).join(".config"));
  ensure!(config.is_absolute(), "XDG_CONFIG_HOME must be absolute");
  Ok(config.join("isos/active.env"))
}

fn parse_active_env(source: &str) -> Result<BTreeMap<&'static str, String>> {
  let mut values = BTreeMap::new();
  for (index, line) in source.lines().enumerate() {
    let line = line.trim();
    if line.is_empty() || line.starts_with('#') {
      continue;
    }
    let entry = line.strip_prefix("export ").unwrap_or(line);
    let (key, value) = entry
      .split_once('=')
      .ok_or_else(|| anyhow::anyhow!("active.env line {} needs KEY=VALUE", index + 1))?;
    let key = key.trim();
    let value = value.trim();
    let allowed = ACTIVE_ENV_KEYS
      .iter()
      .find(|candidate| **candidate == key)
      .ok_or_else(|| anyhow::anyhow!("active.env line {} has unsupported key {key}", index + 1))?;
    ensure!(
      !value.is_empty() && !value.contains('\0'),
      "active.env line {} has an empty or invalid value",
      index + 1
    );
    ensure!(
      !values.contains_key(allowed),
      "active.env line {} repeats a key",
      index + 1
    );
    if key.ends_with("PORT") {
      let port: u16 = value
        .parse()
        .context("active.env port must be 1024..65535")?;
      ensure!(port >= 1024, "active.env port must be 1024..65535");
    } else if key == "ISLAND_MODE" {
      ensure!(
        matches!(value, "system" | "user" | "dev"),
        "active.env ISLAND_MODE is invalid"
      );
    } else {
      ensure!(
        Path::new(value).is_absolute(),
        "active.env {key} must be an absolute path"
      );
    }
    values.insert(*allowed, value.to_owned());
  }
  ensure!(
    values.contains_key("ISOS_HOME"),
    "active.env requires ISOS_HOME"
  );
  ensure!(
    values.contains_key("ISOS_HTTP_PORT") && values.contains_key("ISOSUI_PORT"),
    "active.env requires API and UI ports"
  );
  ensure!(
    values.get("ISOS_HTTP_PORT") != values.get("ISOSUI_PORT"),
    "active.env API and UI ports must differ"
  );
  Ok(values)
}

fn load_active_env() -> Result<()> {
  let path = active_env_path()?;
  load_active_env_inner(&path)
    .with_context(|| format!("Edit {}. Guide: {}", path.display(), config_guide()))
}

fn load_active_env_inner(path: &Path) -> Result<()> {
  use std::os::unix::fs::MetadataExt;
  let source = match fs::read_to_string(path) {
    Ok(source) => source,
    Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(()),
    Err(error) => return Err(error).with_context(|| format!("read {}", path.display())),
  };
  let metadata = fs::symlink_metadata(path)?;
  ensure!(
    metadata.file_type().is_file() && !metadata.file_type().is_symlink(),
    "active.env must be a regular file"
  );
  ensure!(
    metadata.mode() & 0o077 == 0,
    "active.env must be owner-only (0600)"
  );
  ensure!(
    metadata.uid() == unsafe { libc::geteuid() },
    "active.env must be owned by the current user"
  );
  for (key, value) in parse_active_env(&source)? {
    if std::env::var_os(key).is_none() {
      // Startup is single-threaded here, before CLI parsing and child creation.
      unsafe { std::env::set_var(key, value) };
    }
  }
  Ok(())
}

pub fn load_runtime_config() -> Result<()> {
  load_runtime_config_inner()
    .with_context(|| format!("Edit IsOS configuration: {}", config_guide()))
}

fn load_runtime_config_inner() -> Result<()> {
  load_active_env()?;
  let repo_root = Path::new(env!("CARGO_MANIFEST_DIR"));
  let override_path = std::env::var_os("ISOS_CONFIG");
  let path = runtime_config_path(repo_root, override_path.as_deref())?;
  let source = merged_config_source(&path, override_path.is_some())?;
  let values =
    config_values(&source, repo_root).with_context(|| format!("validate {}", path.display()))?;
  for (key, value) in config_without_env_overrides(values, |key| std::env::var_os(key).is_some()) {
    // This runs once before CLI parsing, threads, or child processes exist.
    unsafe {
      std::env::set_var(key, value);
    }
  }
  Ok(())
}

impl Paths {
  pub fn mode(&self) -> Result<&'static str> {
    let selected = std::env::var("ISOS_MODE").ok().or_else(|| {
      fs::read_to_string(self.config.join("mode"))
        .ok()
        .map(|value| value.trim().to_owned())
    });
    match selected.as_deref().unwrap_or("dev") {
      "dev" => Ok("dev"),
      "product" => Ok("product"),
      _ => anyhow::bail!("IsOS mode must be dev or product"),
    }
  }

  pub fn set_mode(&self, mode: &str) -> Result<()> {
    ensure!(
      matches!(mode, "dev" | "product"),
      "IsOS mode must be dev or product"
    );
    atomic_write(&self.config.join("mode"), mode.as_bytes())
  }

  pub fn discover() -> Result<Self> {
    if let Ok(base) = std::env::var("ISOS_HOME") {
      let base = PathBuf::from(base);
      ensure!(base.is_absolute(), "ISOS_HOME must be an absolute path");
      return Ok(Self {
        config: base.join("config"),
        data: base.join("data"),
        runtime: base.join("run"),
      });
    }
    let home = std::env::var_os("HOME")
      .map(PathBuf::from)
      .ok_or_else(|| anyhow::anyhow!("HOME is required"))?;
    let config = std::env::var_os("XDG_CONFIG_HOME")
      .map(PathBuf::from)
      .unwrap_or_else(|| home.join(".config"))
      .join("isos");
    let data = std::env::var_os("XDG_DATA_HOME")
      .map(PathBuf::from)
      .unwrap_or_else(|| home.join(".local/share"))
      .join("isos");
    let runtime = std::env::var_os("XDG_RUNTIME_DIR")
      .map(PathBuf::from)
      .unwrap_or_else(|| PathBuf::from(format!("/run/user/{}", unsafe { libc::getuid() })))
      .join("isos");
    Ok(Self {
      config,
      data,
      runtime,
    })
  }

  pub fn node_graph_path(&self, name: &str) -> PathBuf {
    self
      .config
      .join("deployments")
      .join(format!("{name}.nodes.json"))
  }
  pub fn generated(&self, name: &str) -> PathBuf {
    self.data.join("generated").join(name)
  }
  pub fn socket(&self) -> PathBuf {
    self.runtime.join("manager.sock")
  }

  pub fn prepare(&self) -> Result<()> {
    for dir in [&self.config, &self.data, &self.runtime] {
      fs::create_dir_all(dir).with_context(|| format!("create {}", dir.display()))?;
      fs::set_permissions(dir, fs::Permissions::from_mode(0o700))?;
    }
    Ok(())
  }
}

pub fn deployment_name(cli: Option<String>) -> String {
  cli
    .or_else(|| std::env::var("ISOS_DEPLOYMENT").ok())
    .unwrap_or_else(|| "default".into())
}

pub fn atomic_write(path: &Path, bytes: &[u8]) -> Result<()> {
  let parent = path
    .parent()
    .ok_or_else(|| anyhow::anyhow!("path has no parent"))?;
  fs::create_dir_all(parent)?;
  fs::set_permissions(parent, fs::Permissions::from_mode(0o700))?;
  let temporary = parent.join(format!(
    ".{}.{}.tmp",
    path.file_name().unwrap().to_string_lossy(),
    std::process::id()
  ));
  let mut file = fs::OpenOptions::new()
    .create_new(true)
    .write(true)
    .mode(0o600)
    .open(&temporary)
    .with_context(|| format!("create {}", temporary.display()))?;
  let result = (|| {
    file.write_all(bytes)?;
    file.sync_all()?;
    fs::rename(&temporary, path)?;
    Ok::<_, anyhow::Error>(())
  })();
  if result.is_err() {
    let _ = fs::remove_file(&temporary);
  }
  result
}

#[cfg(test)]
mod tests {
  use super::*;
  #[test]
  fn active_env_is_explicit_and_rejects_untrusted_entries() {
    let values = parse_active_env("export ISOS_HOME=/home/test/new\nexport ISOS_HTTP_PORT=37125\nexport ISOSUI_PORT=37126\nexport ISOS_AETHER_BIN_DIR=/home/test/eos/bin\n").unwrap();
    assert_eq!(values["ISOS_HOME"], "/home/test/new");
    assert_eq!(values["ISOS_HTTP_PORT"], "37125");
    for source in [
      "export ISOS_HTTP_PORT=37125\n",
      "export ISOS_HOME=relative\n",
      "export ISOS_HOME=/home/test\nexport SECRET=value\n",
      "export ISOS_HOME=/home/test\nexport ISOS_HOME=/home/other\n",
      "export ISOS_HOME=/home/test\nexport ISOS_HTTP_PORT=80\n",
      "export ISOS_HOME=/home/test\nexport ISOS_HTTP_PORT=37125\nexport ISOSUI_PORT=37125\n",
    ] {
      assert!(parse_active_env(source).is_err(), "accepted {source}");
    }
  }
  #[test]
  fn config_toml_maps_settings_and_rejects_invalid_values() {
    let root = Path::new("/tmp/isos-source");
    let values = config_values("[server]\nhttp_port=16000\n[paths]\nui_repo='../IsosUI'\n[logging]\ndefault_source='all'\nlevel='warn'\nmax_bytes=262144\nbackups=2\nmax_sources=32\n", root).unwrap();
    assert_eq!(values["ISOS_HTTP_PORT"], "16000");
    assert_eq!(values["ISOSUI_REPO_PATH"], "/tmp/isos-source/../IsosUI");
    assert_eq!(values["ISOS_LOG_DEFAULT_SOURCE"], "all");
    assert_eq!(values["ISOS_LOG_BACKUPS"], "2");
    assert_eq!(values["ISOS_LOG_MAX_SOURCES"], "32");
    assert_eq!(values["ISOS_LOG_LEVEL"], "warn");
    let selected = config_without_env_overrides(values, |key| key == "ISOS_LOG_LEVEL");
    assert!(!selected.contains_key("ISOS_LOG_LEVEL"));
    assert_eq!(selected["ISOS_HTTP_PORT"], "16000");
    assert!(config_values("[server]\nhttp_port=80\n", root).is_err());
    assert!(config_values("[unknown]\nvalue=1\n", root).is_err());
    assert!(config_values("[logging]\nlevel='verbose'\n", root).is_err());
    assert!(config_values("[logging]\ndefault_source='../bad'\n", root).is_err());
    assert!(config_values("[logging]\nmax_sources=9\n", root).is_err());
    let defaults = StopBudget::from_deployment(&DeploymentConfig::default()).unwrap();
    assert_eq!(
      (defaults.timeout_ms, defaults.eos_grace_ms),
      (30_000, 20_000)
    );
    let error = config_values("[deployment]\nstop_timeout_ms=4999\n", root).unwrap_err();
    let diagnostic = error.downcast_ref::<StopBudgetDiagnostic>().unwrap();
    assert_eq!(diagnostic.path, "/deployment/stop_timeout_ms");
    let error = config_values(
      "[deployment]\nstop_timeout_ms=5000\neos_stop_grace_ms=3000\n",
      root,
    )
    .unwrap_err();
    let diagnostic = error.downcast_ref::<StopBudgetDiagnostic>().unwrap();
    assert_eq!(diagnostic.path, "/deployment/eos_stop_grace_ms");
    assert_eq!(
      runtime_config_path(root, None).unwrap(),
      root.join("config/isos.toml")
    );
    assert_eq!(
      runtime_config_path(root, Some(std::ffi::OsStr::new("/tmp/custom.toml"))).unwrap(),
      Path::new("/tmp/custom.toml")
    );
    assert!(runtime_config_path(root, Some(std::ffi::OsStr::new("custom.toml"))).is_err());
    for source in [
      include_str!("../../config/isos.toml"),
      include_str!("../../config/isos.example.toml"),
    ] {
      let parsed = config_values(source, root).unwrap();
      for key in ["ISOSUI_REPO_PATH", "ISOS_AETHER_BIN_DIR"] {
        if let Some(path) = parsed.get(key) {
          assert!(Path::new(path).is_absolute());
        }
      }
    }
  }

  #[test]
  fn selected_config_overrides_example_without_copying_comments() {
    let dir = tempfile::tempdir().unwrap();
    let selected = dir.path().join("selected.toml");
    fs::write(&selected, "# local note\n[server]\nhttp_port = 17123\n").unwrap();
    let merged = merged_config_source(&selected, true).unwrap();
    let values = config_values(&merged, dir.path()).unwrap();
    assert_eq!(values["ISOS_HTTP_PORT"], "17123");
    assert_eq!(values["ISOSUI_PORT"], "16001");
    assert_eq!(values["ISOS_LOG_LEVEL"], "info");
    assert!(!merged.contains("local note"));
    fs::remove_file(&selected).unwrap();
    assert!(merged_config_source(&selected, true).is_err());
    assert!(merged_config_source(&selected, false).is_ok());
  }

  #[test]
  fn package_host_inputs_are_generic_absolute_paths() {
    let root = Path::new("/tmp/isos-source");
    let values = config_values("[package_host_inputs]\nSIM_ROOT='/opt/simulator'\n", root).unwrap();
    assert_eq!(values["SIM_ROOT"], "/opt/simulator");
    for key in [
      "ISOS_HOME",
      "ISLAND_PREFIX",
      "EGIA_PYTHON",
      "lower",
      "BAD-DASH",
    ] {
      let source = format!("[package_host_inputs]\n{key}='/opt/simulator'\n");
      assert!(config_values(&source, root).is_err(), "accepted {key}");
    }
    assert!(config_values("[package_host_inputs]\nSIM_ROOT='relative'\n", root).is_err());
  }

  #[test]
  fn ei_paths_are_preferred_and_legacy_paths_remain_readable() {
    let root = Path::new("/tmp/isos-source");
    let current = config_values(
      "[paths]\nei_bin_dir='bin/ei'\nei_defaults_dir='etc/ei'\n",
      root,
    )
    .unwrap();
    assert_eq!(current["ISOS_EI_BIN_DIR"], "/tmp/isos-source/bin/ei");
    assert_eq!(current["ISOS_EI_DEFAULTS_DIR"], "/tmp/isos-source/etc/ei");
    let legacy = config_values("[paths]\negia_bin_dir='bin/egia'\n", root).unwrap();
    assert_eq!(legacy["ISOS_EI_BIN_DIR"], "/tmp/isos-source/bin/egia");
    assert!(
      config_values(
        "[paths]\nei_bin_dir='bin/ei'\negia_bin_dir='bin/egia'\n",
        root
      )
      .is_err()
    );
  }
}
