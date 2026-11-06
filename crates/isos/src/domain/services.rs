//! Independent, owner-managed service processes. Service files are never graph nodes.

use crate::{app::config::Paths, runtime::logs::LogStore};
use anyhow::{Context, Result, bail, ensure};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
  collections::{BTreeMap, BTreeSet},
  fs,
  io::Read,
  net::{TcpStream, ToSocketAddrs},
  os::unix::{fs::PermissionsExt, process::CommandExt},
  path::{Path, PathBuf},
  process::{Child, Command, Stdio},
  thread,
  time::{Duration, Instant},
};

pub const CONTRACT_VERSION: &str = "1.0.0";
const MAX_CONFIG_BYTES: u64 = 65_536;

fn valid_id(id: &str) -> bool {
  !id.is_empty()
    && id.len() <= 32
    && id
      .bytes()
      .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Artifact {
  /// Absolute executable or bundle file whose SHA-256 is pinned.
  pub path: PathBuf,
  pub sha256: String,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Probe {
  /// `tcp`, `unix`, or `exec`; target is an address, socket path, or executable path.
  pub kind: ProbeKind,
  pub target: String,
  #[serde(default)]
  pub args: Vec<String>,
}

#[derive(Debug, Clone, Copy, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ProbeKind {
  Tcp,
  Unix,
  Exec,
}

#[derive(Debug, Clone, Copy, Default, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum RestartPolicy {
  #[default]
  Never,
  OnFailure,
  Always,
}

fn default_timeout() -> u64 {
  30_000
}
fn default_stop_timeout() -> u64 {
  5_000
}

/// One file at `<config>/services/<id>.toml`. Environment values are never stored here.
#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ServiceSpec {
  pub version: String,
  pub id: String,
  pub artifact: Artifact,
  #[serde(default)]
  pub adapter: Option<Adapter>,
  pub command: Vec<String>,
  pub cwd: PathBuf,
  #[serde(default)]
  pub env_from: BTreeMap<String, String>,
  #[serde(default)]
  pub preflight: Option<Probe>,
  #[serde(default)]
  pub readiness: Option<Probe>,
  #[serde(default)]
  pub health: Option<Probe>,
  #[serde(default)]
  pub depends_on: Vec<String>,
  #[serde(default)]
  pub restart: RestartPolicy,
  #[serde(default)]
  pub max_restarts: u32,
  #[serde(default = "default_timeout")]
  pub start_timeout_ms: u64,
  #[serde(default = "default_stop_timeout")]
  pub stop_timeout_ms: u64,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
pub enum Adapter {
  IslandModels { repo: PathBuf, alias: String },
}

fn verify_artifact(artifact: &Artifact) -> Result<()> {
  ensure!(
    artifact.path.is_absolute(),
    "artifact path must be absolute"
  );
  ensure!(
    artifact.sha256.len() == 64 && artifact.sha256.bytes().all(|b| b.is_ascii_hexdigit()),
    "artifact sha256 must be 64 hexadecimal characters"
  );
  let mut file = fs::File::open(&artifact.path)
    .with_context(|| format!("open artifact {}", artifact.path.display()))?;
  let mut hash = Sha256::new();
  let mut buffer = [0u8; 65_536];
  loop {
    let count = file.read(&mut buffer)?;
    if count == 0 {
      break;
    }
    hash.update(&buffer[..count]);
  }
  ensure!(
    hex::encode(hash.finalize()) == artifact.sha256.to_ascii_lowercase(),
    "artifact digest mismatch: {}",
    artifact.path.display()
  );
  Ok(())
}

fn validate_probe(probe: &Probe) -> Result<()> {
  ensure!(
    !probe.target.is_empty() && probe.target.len() <= 4096,
    "invalid probe target"
  );
  ensure!(
    probe.args.len() <= 32
      && probe
        .args
        .iter()
        .all(|arg| !arg.is_empty() && arg.len() <= 4096),
    "invalid probe arguments"
  );
  match probe.kind {
    ProbeKind::Tcp => {
      ensure!(probe.args.is_empty(), "TCP probe cannot have args");
      ensure!(
        probe.target.to_socket_addrs()?.next().is_some(),
        "TCP probe has no address"
      );
    }
    ProbeKind::Unix => {
      ensure!(
        probe.args.is_empty() && Path::new(&probe.target).is_absolute(),
        "Unix probe requires an absolute socket path"
      );
    }
    ProbeKind::Exec => ensure!(
      Path::new(&probe.target).is_absolute(),
      "exec probe requires an absolute executable path"
    ),
  }
  Ok(())
}

impl ServiceSpec {
  pub fn validate(&self) -> Result<()> {
    ensure!(
      self.version == CONTRACT_VERSION,
      "unsupported service contract {}",
      self.version
    );
    ensure!(valid_id(&self.id), "invalid service id");
    ensure!(
      !self.command.is_empty()
        && self.command.len() <= 64
        && self
          .command
          .iter()
          .all(|arg| !arg.is_empty() && arg.len() <= 4096),
      "invalid service command"
    );
    ensure!(
      Path::new(&self.command[0]).is_absolute(),
      "service executable must be absolute"
    );
    ensure!(
      Path::new(&self.command[0]) == self.artifact.path,
      "service executable must match the pinned artifact"
    );
    ensure!(
      self.cwd.is_absolute() && self.cwd.is_dir(),
      "service cwd must be an existing absolute directory"
    );
    ensure!(
      (1..=120_000).contains(&self.start_timeout_ms),
      "start_timeout_ms out of range"
    );
    ensure!(
      (1..=60_000).contains(&self.stop_timeout_ms),
      "stop_timeout_ms out of range"
    );
    ensure!(self.max_restarts <= 10, "max_restarts exceeds 10");
    ensure!(
      self
        .depends_on
        .iter()
        .all(|id| valid_id(id) && id != &self.id),
      "invalid service dependency"
    );
    for (target, source) in &self.env_from {
      ensure!(
        valid_env(target) && valid_env(source),
        "invalid environment reference"
      );
      ensure!(
        std::env::var_os(source).is_some(),
        "required environment variable {source} is unset"
      );
    }
    for probe in [&self.preflight, &self.readiness, &self.health]
      .into_iter()
      .flatten()
    {
      validate_probe(probe)?;
    }
    verify_artifact(&self.artifact)?;
    if let Some(Adapter::IslandModels { repo, alias }) = &self.adapter {
      ensure!(
        self.command == island_models_adapter(repo, alias)?,
        "Island-models managed command differs from the catalog"
      );
      ensure!(
        self.cwd == *repo,
        "Island-models cwd must be its repository root"
      );
    }
    ensure!(
      fs::metadata(&self.command[0])?.permissions().mode() & 0o111 != 0,
      "service executable is not executable"
    );
    Ok(())
  }
}

fn valid_env(name: &str) -> bool {
  let mut bytes = name.bytes();
  bytes
    .next()
    .is_some_and(|b| b.is_ascii_alphabetic() || b == b'_')
    && bytes.all(|b| b.is_ascii_alphanumeric() || b == b'_')
    && name.len() <= 128
}

pub fn service_dir(paths: &Paths) -> PathBuf {
  paths.config.join("services")
}

pub fn load_specs(paths: &Paths) -> Result<BTreeMap<String, ServiceSpec>> {
  let directory = service_dir(paths);
  if !directory.exists() {
    return Ok(BTreeMap::new());
  }
  let mut result = BTreeMap::new();
  for entry in fs::read_dir(&directory)? {
    let entry = entry?;
    let path = entry.path();
    if path.extension().is_none_or(|ext| ext != "toml") {
      continue;
    }
    let meta = fs::symlink_metadata(&path)?;
    ensure!(
      meta.is_file() && meta.len() <= MAX_CONFIG_BYTES,
      "invalid service file {}",
      path.display()
    );
    let spec: ServiceSpec = toml::from_str(&fs::read_to_string(&path)?)
      .with_context(|| format!("parse {}", path.display()))?;
    ensure!(
      path
        .file_stem()
        .is_some_and(|stem| stem == spec.id.as_str()),
      "service file name must match its id"
    );
    ensure!(valid_id(&spec.id), "invalid service id");
    ensure!(
      result.insert(spec.id.clone(), spec).is_none(),
      "duplicate service id"
    );
  }
  Ok(result)
}

fn ordered(specs: &BTreeMap<String, ServiceSpec>, id: &str) -> Result<Vec<String>> {
  fn visit(
    id: &str,
    specs: &BTreeMap<String, ServiceSpec>,
    temporary: &mut BTreeSet<String>,
    done: &mut BTreeSet<String>,
    out: &mut Vec<String>,
  ) -> Result<()> {
    if done.contains(id) {
      return Ok(());
    }
    ensure!(
      temporary.insert(id.to_owned()),
      "service dependency cycle at {id}"
    );
    let spec = specs
      .get(id)
      .with_context(|| format!("unknown service {id}"))?;
    for dep in &spec.depends_on {
      visit(dep, specs, temporary, done, out)?;
    }
    temporary.remove(id);
    done.insert(id.to_owned());
    out.push(id.to_owned());
    Ok(())
  }
  let mut out = Vec::new();
  visit(
    id,
    specs,
    &mut BTreeSet::new(),
    &mut BTreeSet::new(),
    &mut out,
  )?;
  Ok(out)
}

pub fn validate_all(paths: &Paths) -> Result<Vec<String>> {
  let specs = load_specs(paths)?;
  for (id, spec) in &specs {
    spec
      .validate()
      .with_context(|| format!("validate service {id}"))?;
    ordered(&specs, id)?;
  }
  Ok(specs.keys().cloned().collect())
}

fn probe_ready(probe: &Probe, cwd: &Path) -> bool {
  match probe.kind {
    ProbeKind::Tcp => probe.target.to_socket_addrs().ok().is_some_and(|addrs| {
      addrs
        .into_iter()
        .any(|addr| TcpStream::connect_timeout(&addr, Duration::from_millis(200)).is_ok())
    }),
    ProbeKind::Unix => std::os::unix::net::UnixStream::connect(&probe.target).is_ok(),
    ProbeKind::Exec => {
      let Ok(mut child) = Command::new(&probe.target)
        .args(&probe.args)
        .current_dir(cwd)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
      else {
        return false;
      };
      let deadline = Instant::now() + Duration::from_secs(10);
      loop {
        match child.try_wait() {
          Ok(Some(status)) => return status.success(),
          Err(_) => return false,
          Ok(None) if Instant::now() >= deadline => {
            let _ = child.kill();
            let _ = child.wait();
            return false;
          }
          Ok(None) => thread::sleep(Duration::from_millis(25)),
        }
      }
    }
  }
}

struct Running {
  spec: ServiceSpec,
  child: Child,
  restarts: u32,
}

#[derive(Debug, Clone, Serialize)]
pub struct ServiceStatus {
  pub id: String,
  pub state: &'static str,
  pub pid: Option<u32>,
  pub restarts: u32,
  pub healthy: Option<bool>,
  pub artifact_sha256: Option<String>,
}

pub struct ServiceManager {
  paths: Paths,
  running: BTreeMap<String, Running>,
  order: Vec<String>,
  logs: LogStore,
}

impl ServiceManager {
  fn dependents(&self, id: &str) -> Vec<String> {
    let mut affected = BTreeSet::from([id.to_owned()]);
    loop {
      let before = affected.len();
      for (name, running) in &self.running {
        if running
          .spec
          .depends_on
          .iter()
          .any(|dep| affected.contains(dep))
        {
          affected.insert(name.clone());
        }
      }
      if affected.len() == before {
        break;
      }
    }
    self
      .order
      .iter()
      .filter(|name| *name != id && affected.contains(*name))
      .cloned()
      .collect()
  }
  pub fn new(paths: &Paths, deployment: &str) -> Result<Self> {
    Ok(Self {
      paths: paths.clone(),
      running: BTreeMap::new(),
      order: Vec::new(),
      logs: LogStore::new(paths, deployment)?,
    })
  }
  pub fn list(&self) -> Result<Vec<String>> {
    Ok(load_specs(&self.paths)?.into_keys().collect())
  }
  pub fn validate(&self) -> Result<Vec<String>> {
    validate_all(&self.paths)
  }
  pub fn status(&self) -> Result<Vec<ServiceStatus>> {
    Ok(
      self
        .list()?
        .into_iter()
        .map(|id| {
          if let Some(running) = self.running.get(&id) {
            ServiceStatus {
              id,
              state: "running",
              pid: Some(running.child.id()),
              restarts: running.restarts,
              healthy: running
                .spec
                .health
                .as_ref()
                .map(|probe| probe_ready(probe, &running.spec.cwd)),
              artifact_sha256: Some(running.spec.artifact.sha256.clone()),
            }
          } else {
            ServiceStatus {
              id,
              state: "stopped",
              pid: None,
              restarts: 0,
              healthy: None,
              artifact_sha256: None,
            }
          }
        })
        .collect(),
    )
  }
  fn spawn(&self, spec: &ServiceSpec) -> Result<Child> {
    let mut command = Command::new(&spec.command[0]);
    command
      .args(&spec.command[1..])
      .current_dir(&spec.cwd)
      .stdin(Stdio::null())
      .stdout(Stdio::piped())
      .stderr(Stdio::piped());
    for (target, source) in &spec.env_from {
      command.env(
        target,
        std::env::var_os(source).context("environment reference disappeared")?,
      );
    }
    let parent = std::process::id();
    unsafe {
      command.pre_exec(move || {
        if libc::setsid() < 0 {
          return Err(std::io::Error::last_os_error());
        }
        if libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM) < 0 {
          return Err(std::io::Error::last_os_error());
        }
        if libc::getppid() as u32 != parent {
          return Err(std::io::Error::from_raw_os_error(libc::ESRCH));
        }
        Ok(())
      });
    }
    let mut child = command
      .spawn()
      .with_context(|| format!("spawn service {}", spec.id))?;
    let pid = child.id();
    let source = format!("service-{}", spec.id);
    if let Some(stdout) = child.stdout.take() {
      crate::runtime::logs::capture(
        stdout,
        self.logs.clone(),
        source.clone(),
        "stdout",
        crate::runtime::manager::manager_session_id().into(),
        pid,
        None,
      );
    }
    if let Some(stderr) = child.stderr.take() {
      crate::runtime::logs::capture(
        stderr,
        self.logs.clone(),
        source,
        "stderr",
        crate::runtime::manager::manager_session_id().into(),
        pid,
        None,
      );
    }
    Ok(child)
  }
  fn start_one(&mut self, spec: ServiceSpec) -> Result<()> {
    if let Some(probe) = &spec.preflight {
      ensure!(
        probe_ready(probe, &spec.cwd),
        "{} preflight failed",
        spec.id
      );
    }
    let mut child = self.spawn(&spec)?;
    let deadline = Instant::now() + Duration::from_millis(spec.start_timeout_ms);
    let no_probe_grace = Instant::now() + Duration::from_millis(50);
    loop {
      let exited = match child.try_wait() {
        Ok(status) => status,
        Err(error) => {
          terminate(&mut child, spec.stop_timeout_ms);
          return Err(error.into());
        }
      };
      if let Some(exit) = exited {
        terminate(&mut child, spec.stop_timeout_ms);
        bail!("{} exited before readiness: {exit}", spec.id);
      }
      if spec
        .readiness
        .as_ref()
        .map_or(Instant::now() >= no_probe_grace, |probe| {
          probe_ready(probe, &spec.cwd)
        })
      {
        break;
      }
      if Instant::now() >= deadline {
        terminate(&mut child, spec.stop_timeout_ms);
        bail!("{} readiness timed out", spec.id);
      }
      thread::sleep(Duration::from_millis(50));
    }
    if let Err(error) = self.logs.append(
      "isos",
      "info",
      &format!("service {} started pid {}", spec.id, child.id()),
    ) {
      terminate(&mut child, spec.stop_timeout_ms);
      return Err(error);
    }
    self.order.push(spec.id.clone());
    self.running.insert(
      spec.id.clone(),
      Running {
        spec,
        child,
        restarts: 0,
      },
    );
    Ok(())
  }
  pub fn start(&mut self, id: &str) -> Result<Vec<String>> {
    let specs = load_specs(&self.paths)?;
    let order = ordered(&specs, id)?;
    for name in &order {
      specs[name].validate()?;
    }
    let mut added: Vec<String> = Vec::new();
    for name in order {
      if self.running.contains_key(&name) {
        continue;
      }
      if let Err(error) = self.start_one(specs[&name].clone()) {
        for started in added.iter().rev() {
          self.stop_one(started);
        }
        return Err(error);
      }
      added.push(name);
    }
    Ok(added)
  }
  fn stop_one(&mut self, id: &str) -> bool {
    if let Some(mut running) = self.running.remove(id) {
      terminate(&mut running.child, running.spec.stop_timeout_ms);
      self.order.retain(|candidate| candidate != id);
      let _ = self
        .logs
        .append("isos", "info", &format!("service {id} stopped"));
      true
    } else {
      false
    }
  }
  pub fn stop(&mut self, id: &str) -> Result<Vec<String>> {
    ensure!(valid_id(id), "invalid service id");
    let mut affected = BTreeSet::from([id.to_owned()]);
    loop {
      let before = affected.len();
      for (name, running) in &self.running {
        if running
          .spec
          .depends_on
          .iter()
          .any(|dep| affected.contains(dep))
        {
          affected.insert(name.clone());
        }
      }
      if affected.len() == before {
        break;
      }
    }
    let mut stopped = Vec::new();
    for name in self.order.clone().iter().rev() {
      if affected.contains(name) && self.stop_one(name) {
        stopped.push(name.clone());
      }
    }
    Ok(stopped)
  }
  pub fn stop_all(&mut self) -> Vec<String> {
    let mut stopped = Vec::new();
    for id in self.order.clone().iter().rev() {
      if self.stop_one(id) {
        stopped.push(id.clone());
      }
    }
    stopped
  }
  pub fn restart(&mut self, id: &str) -> Result<Vec<String>> {
    let specs = load_specs(&self.paths)?;
    for name in ordered(&specs, id)? {
      specs[&name].validate()?;
    }
    let dependents = self.dependents(id);
    self.stop(id)?;
    let mut started = self.start(id)?;
    for dependent in dependents {
      started.extend(self.start(&dependent)?);
    }
    Ok(started)
  }
  pub fn poll(&mut self) -> Result<()> {
    let ids = self.order.clone();
    for id in ids {
      let Some(running) = self.running.get_mut(&id) else {
        continue;
      };
      let exited = running.child.try_wait()?;
      let unhealthy = exited.is_none()
        && running
          .spec
          .health
          .as_ref()
          .is_some_and(|probe| !probe_ready(probe, &running.spec.cwd));
      if exited.is_none() && !unhealthy {
        continue;
      }
      let mut process = self.running.remove(&id).expect("checked running");
      let failure = exited.is_some_and(|status| !status.success()) || unhealthy;
      let _ = self.logs.append(
        "isos",
        "warn",
        &format!("service {id} exited or became unhealthy"),
      );
      let dependents = self.dependents(&id);
      for dep in dependents.iter().rev() {
        self.stop_one(dep);
      }
      self.order.retain(|candidate| candidate != &id);
      terminate(&mut process.child, process.spec.stop_timeout_ms);
      if (process.spec.restart == RestartPolicy::Always
        || (process.spec.restart == RestartPolicy::OnFailure && failure))
        && process.restarts < process.spec.max_restarts
      {
        let delay = 100u64 * (1u64 << process.restarts.min(5));
        thread::sleep(Duration::from_millis(delay));
        let next = process.restarts + 1;
        self.start_one(process.spec)?;
        self.running.get_mut(&id).expect("started").restarts = next;
        for dep in dependents {
          self.start(&dep)?;
        }
      }
    }
    Ok(())
  }
  pub fn logs(&self, id: &str, cursor: Option<&str>, limit: usize) -> Result<serde_json::Value> {
    ensure!(valid_id(id), "invalid service id");
    ensure!((1..=200).contains(&limit), "log limit must be 1..200");
    ensure!(
      load_specs(&self.paths)?.contains_key(id),
      "unknown service {id}"
    );
    self.logs.read(&format!("service-{id}"), cursor, limit)
  }
}

impl Drop for ServiceManager {
  fn drop(&mut self) {
    self.stop_all();
  }
}

fn terminate(child: &mut Child, timeout_ms: u64) {
  let pgid = child.id() as i32;
  unsafe {
    libc::kill(-pgid, libc::SIGTERM);
  }
  let deadline = Instant::now() + Duration::from_millis(timeout_ms);
  while Instant::now() < deadline {
    let _ = child.try_wait();
    if unsafe { libc::kill(-pgid, 0) } < 0
      && std::io::Error::last_os_error().raw_os_error() == Some(libc::ESRCH)
    {
      return;
    }
    thread::sleep(Duration::from_millis(25));
  }
  unsafe {
    libc::kill(-pgid, libc::SIGKILL);
  }
  let _ = child.wait();
}

#[derive(Debug, Deserialize)]
struct ModelsCatalog {
  schema_version: u32,
  services: Vec<ModelEntry>,
}
#[derive(Debug, Deserialize)]
struct ModelEntry {
  alias: String,
  managed_command: Vec<String>,
}
#[derive(Debug, Deserialize)]
struct ModelsPreflight {
  schema_version: u32,
  services: Vec<ModelReadiness>,
}
#[derive(Debug, Deserialize)]
struct ModelReadiness {
  alias: String,
  ready: bool,
  missing_artifacts: Vec<String>,
  errors: Vec<String>,
}

/// Validate the current Island-models CLI contract without altering that repository.
pub fn island_models_adapter(repo: &Path, alias: &str) -> Result<Vec<String>> {
  ensure!(
    repo.is_absolute() && valid_id(alias),
    "invalid Island-models repository or alias"
  );
  let launcher = repo.join("launch.sh");
  ensure!(launcher.is_file(), "Island-models launch.sh is missing");
  let catalog = Command::new(&launcher)
    .arg("--catalog-json")
    .current_dir(repo)
    .output()?;
  ensure!(catalog.status.success(), "Island-models catalog failed");
  let catalog: ModelsCatalog = serde_json::from_slice(&catalog.stdout)?;
  ensure!(
    catalog.schema_version == 1,
    "unsupported Island-models catalog schema"
  );
  let entry = catalog
    .services
    .iter()
    .find(|entry| entry.alias == alias)
    .context("model alias not in catalog")?;
  ensure!(
    entry.managed_command.len() == 4
      && entry.managed_command[0] == "launch.sh"
      && entry.managed_command[1..] == ["--managed", "--only", alias],
    "unexpected managed command"
  );
  let check = Command::new(&launcher)
    .args(["--only", alias, "--preflight-json"])
    .current_dir(repo)
    .output()?;
  ensure!(
    check.status.success(),
    "Island-models preflight command failed"
  );
  let report: ModelsPreflight = serde_json::from_slice(&check.stdout)?;
  ensure!(
    report.schema_version == 1,
    "unsupported Island-models preflight schema"
  );
  let service = report
    .services
    .iter()
    .find(|service| service.alias == alias)
    .context("model alias missing from preflight")?;
  ensure!(
    service.ready,
    "model {alias} is not ready: missing {:?}; errors {:?}",
    service.missing_artifacts,
    service.errors
  );
  Ok(vec![
    launcher.to_string_lossy().into_owned(),
    "--managed".into(),
    "--only".into(),
    alias.into(),
  ])
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::os::unix::fs::PermissionsExt;

  fn fixture() -> (tempfile::TempDir, Paths, ServiceSpec) {
    let dir = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: dir.path().join("config"),
      data: dir.path().join("data"),
      runtime: dir.path().join("run"),
    };
    fs::create_dir_all(service_dir(&paths)).unwrap();
    let executable = PathBuf::from("/bin/sh");
    let hash = hex::encode(Sha256::digest(fs::read(&executable).unwrap()));
    let spec = ServiceSpec {
      version: CONTRACT_VERSION.into(),
      id: "stub".into(),
      artifact: Artifact {
        path: executable.clone(),
        sha256: hash,
      },
      command: vec![
        executable.to_string_lossy().into_owned(),
        "-c".into(),
        "sleep 30".into(),
      ],
      cwd: dir.path().to_path_buf(),
      adapter: None,
      env_from: BTreeMap::new(),
      preflight: None,
      readiness: None,
      health: None,
      depends_on: vec![],
      restart: RestartPolicy::Never,
      max_restarts: 0,
      start_timeout_ms: 1000,
      stop_timeout_ms: 1000,
    };
    (dir, paths, spec)
  }

  fn save(paths: &Paths, spec: &ServiceSpec) {
    fs::write(
      service_dir(paths).join(format!("{}.toml", spec.id)),
      toml::to_string(spec).unwrap(),
    )
    .unwrap();
  }

  #[test]
  fn exact_artifact_and_contract_required() {
    let (_dir, paths, mut spec) = fixture();
    save(&paths, &spec);
    assert_eq!(validate_all(&paths).unwrap(), ["stub"]);
    spec.artifact.sha256 = "0".repeat(64);
    save(&paths, &spec);
    assert!(
      validate_all(&paths)
        .unwrap_err()
        .to_string()
        .contains("validate service")
    );
    spec.version = "1.0.0".into();
    assert!(spec.validate().is_err());
  }

  #[test]
  fn cycle_and_missing_dependency_rejected() {
    let (_dir, paths, mut spec) = fixture();
    spec.depends_on.push("other".into());
    save(&paths, &spec);
    assert!(validate_all(&paths).is_err());
    let mut other = spec.clone();
    other.id = "other".into();
    other.depends_on = vec!["stub".into()];
    save(&paths, &other);
    assert!(
      validate_all(&paths)
        .unwrap_err()
        .to_string()
        .contains("cycle")
    );
  }

  #[test]
  fn start_stop_and_idempotent_stop() {
    let (_dir, paths, spec) = fixture();
    save(&paths, &spec);
    let mut manager = ServiceManager::new(&paths, "default").unwrap();
    assert_eq!(manager.start("stub").unwrap(), ["stub"]);
    assert_eq!(manager.start("stub").unwrap(), Vec::<String>::new());
    assert_eq!(manager.status().unwrap()[0].state, "running");
    assert_eq!(manager.stop("stub").unwrap(), ["stub"]);
    assert!(manager.stop("stub").unwrap().is_empty());
  }

  #[test]
  fn startup_failure_rolls_back_new_dependencies() {
    let (_dir, paths, base) = fixture();
    save(&paths, &base);
    let mut child = base.clone();
    child.id = "failing".into();
    child.command = vec!["/bin/sh".into(), "-c".into(), "exit 7".into()];
    child.depends_on = vec!["stub".into()];
    save(&paths, &child);
    let mut manager = ServiceManager::new(&paths, "default").unwrap();
    assert!(manager.start("failing").is_err());
    assert!(
      manager
        .status()
        .unwrap()
        .iter()
        .all(|service| service.state == "stopped")
    );
  }

  #[test]
  fn failed_child_restarts_only_within_budget() {
    let (_dir, paths, mut spec) = fixture();
    spec.command = vec!["/bin/sh".into(), "-c".into(), "sleep 0.2; exit 7".into()];
    spec.restart = RestartPolicy::OnFailure;
    spec.max_restarts = 1;
    save(&paths, &spec);
    let mut manager = ServiceManager::new(&paths, "default").unwrap();
    manager.start("stub").unwrap();
    thread::sleep(Duration::from_millis(250));
    manager.poll().unwrap();
    assert_eq!(manager.status().unwrap()[0].restarts, 1);
    thread::sleep(Duration::from_millis(250));
    manager.poll().unwrap();
    assert_eq!(manager.status().unwrap()[0].state, "stopped");
  }

  #[test]
  fn provider_stop_stops_dependents_first() {
    let (_dir, paths, provider) = fixture();
    save(&paths, &provider);
    let mut dependent = provider.clone();
    dependent.id = "dependent".into();
    dependent.depends_on = vec!["stub".into()];
    save(&paths, &dependent);
    let mut manager = ServiceManager::new(&paths, "default").unwrap();
    assert_eq!(manager.start("dependent").unwrap(), ["stub", "dependent"]);
    assert_eq!(manager.stop("stub").unwrap(), ["dependent", "stub"]);
  }

  #[test]
  fn island_models_adapter_checks_catalog_and_preflight() {
    let dir = tempfile::tempdir().unwrap();
    let launcher = dir.path().join("launch.sh");
    fs::write(&launcher, "#!/bin/sh\nif [ \"$1\" = --catalog-json ]; then echo '{\"schema_version\":1,\"services\":[{\"alias\":\"demo\",\"managed_command\":[\"launch.sh\",\"--managed\",\"--only\",\"demo\"]}]}'; else echo '{\"schema_version\":1,\"services\":[{\"alias\":\"demo\",\"ready\":true,\"missing_artifacts\":[],\"errors\":[]}]}'; fi\n").unwrap();
    fs::set_permissions(&launcher, fs::Permissions::from_mode(0o755)).unwrap();
    assert_eq!(
      island_models_adapter(dir.path(), "demo").unwrap(),
      vec![
        launcher.to_string_lossy().into_owned(),
        "--managed".into(),
        "--only".into(),
        "demo".into()
      ]
    );
    let (_service_dir, _paths, mut spec) = fixture();
    spec.id = "demo".into();
    spec.artifact = Artifact {
      path: launcher.clone(),
      sha256: hex::encode(Sha256::digest(fs::read(&launcher).unwrap())),
    };
    spec.command = island_models_adapter(dir.path(), "demo").unwrap();
    spec.cwd = dir.path().to_path_buf();
    spec.adapter = Some(Adapter::IslandModels {
      repo: dir.path().to_path_buf(),
      alias: "demo".into(),
    });
    spec.validate().unwrap();
    spec.command[3] = "other".into();
    assert!(spec.validate().is_err());
  }
}
