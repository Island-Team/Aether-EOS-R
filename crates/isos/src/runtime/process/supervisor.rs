//! Direct-child lifecycle, readiness, and dependent stop supervision.

use super::*;

struct Running {
  spec: ProcessSpec,
  child: Child,
  restarts: u32,
  identity: Option<LeaderIdentity>,
  eos_stdout: Option<Receiver<log_store::EosStdout>>,
}

#[derive(Default)]
struct EosTranscript {
  frames: Vec<Vec<u8>>,
  invalid: bool,
  first_at: Option<Instant>,
  eof_at: Option<Instant>,
}

fn selected_eos_stop_parts(graph: &Value, backend: &str) -> Vec<(String, String)> {
  let mut parts = Vec::new();
  if backend.is_empty() {
    return parts;
  }
  let unified = graph["version"] == "0.2.0";
  let hardware = if unified {
    "/cores/isos/layers/eos_hardware/groups"
  } else {
    "/cores/eos/layers/hardware/groups"
  };
  let Some(groups) = graph.pointer(hardware).and_then(Value::as_object) else {
    return parts;
  };
  for (group, family) in groups {
    let Some(nodes) = family.get("nodes").and_then(Value::as_object) else {
      continue;
    };
    for (id, node) in nodes {
      if node["movement"].is_object()
        && node["adapter"]["settings"]["backend"] == backend
        && node["references"]
          .as_array()
          .is_some_and(|references| references.iter().any(|reference| reference == backend))
      {
        let prefix = if unified {
          "isos/eos_hardware"
        } else {
          "eos/hardware"
        };
        parts.push((format!("{prefix}/{group}/{id}"), group.clone()));
      }
    }
  }
  parts.sort_by(|a, b| a.0.cmp(&b.0));
  parts
}

impl EosTranscript {
  fn accept(&mut self, event: log_store::EosStdout) {
    self.invalid |= event.invalid;
    if let Some(frame) = event.frame {
      if self.first_at.is_none() {
        self.first_at = Some(event.at);
      }
      if self.frames.len() < 2 {
        self.frames.push(frame);
      } else {
        self.invalid = true;
      }
    }
    if event.eof {
      self.eof_at = Some(event.at);
    }
  }
}

#[derive(Clone, Copy)]
struct LeaderIdentity {
  pid: i32,
  pgid: i32,
  session: i32,
  start_ticks: u64,
}

fn leader_identity(pid: i32) -> Result<LeaderIdentity> {
  let stat = fs::read_to_string(format!("/proc/{pid}/stat"))?;
  let fields: Vec<_> = stat
    .rsplit_once(") ")
    .context("invalid leader stat")?
    .1
    .split_whitespace()
    .collect();
  ensure!(fields.len() > 19, "incomplete leader stat");
  Ok(LeaderIdentity {
    pid,
    pgid: fields[2].parse()?,
    session: fields[3].parse()?,
    start_ticks: fields[19].parse()?,
  })
}

fn observe_exit(child: &Child) -> Result<bool> {
  let mut info: libc::siginfo_t = unsafe { std::mem::zeroed() };
  let result = unsafe {
    libc::waitid(
      libc::P_PID,
      child.id(),
      &mut info,
      libc::WEXITED | libc::WNOHANG | libc::WNOWAIT,
    )
  };
  if result < 0 {
    return Err(std::io::Error::last_os_error().into());
  }
  Ok(unsafe { info.si_pid() } != 0)
}

fn identity_matches(original: LeaderIdentity) -> bool {
  leader_identity(original.pid).is_ok_and(|current| {
    current.pid == original.pid
      && current.pgid == original.pgid
      && current.session == original.session
      && current.start_ticks == original.start_ticks
  })
}

fn signal_group(original: LeaderIdentity, signal: i32) -> Result<()> {
  ensure!(
    original.pid > 0
      && original.pgid == original.pid
      && original.session == original.pid
      && identity_matches(original),
    "owned process-group identity is unavailable"
  );
  let result = unsafe { libc::kill(-original.pgid, signal) };
  if result < 0 {
    let error = std::io::Error::last_os_error();
    if error.raw_os_error() != Some(libc::ESRCH) {
      return Err(error.into());
    }
  }
  Ok(())
}

fn group_live(original: LeaderIdentity) -> Result<bool> {
  ensure!(
    identity_matches(original),
    "owned process-group identity is unavailable"
  );
  for entry in fs::read_dir("/proc")? {
    let entry = entry?;
    if let Ok(pid) = entry.file_name().to_string_lossy().parse::<i32>()
      && let Ok(stat) = fs::read_to_string(format!("/proc/{pid}/stat"))
      && let Some((_, rest)) = stat.rsplit_once(") ")
    {
      let fields: Vec<_> = rest.split_whitespace().collect();
      if fields.len() > 3
        && fields[2].parse::<i32>().ok() == Some(original.pgid)
        && fields[0] != "Z"
        && fields[0] != "X"
      {
        return Ok(true);
      }
    }
  }
  Ok(false)
}

fn stop_order(specs: &[ProcessSpec], launch: &[String]) -> Vec<String> {
  let mut remaining: BTreeSet<String> = specs.iter().map(|spec| spec.id.clone()).collect();
  let mut result = Vec::with_capacity(remaining.len());
  while !remaining.is_empty() {
    let next = specs
      .iter()
      .filter(|spec| remaining.contains(&spec.id))
      .filter(|spec| {
        !specs.iter().any(|other| {
          remaining.contains(&other.id)
            && other
              .settings
              .depends_on
              .iter()
              .any(|dependency| dependency.rsplit('/').next() == Some(spec.id.as_str()))
        })
      })
      .max_by_key(|spec| {
        launch
          .iter()
          .position(|id| id == &spec.id)
          .or_else(|| specs.iter().position(|candidate| candidate.id == spec.id))
          .unwrap_or(0)
      });
    let id = next
      .map(|spec| spec.id.clone())
      .unwrap_or_else(|| remaining.iter().next_back().unwrap().clone());
    remaining.remove(&id);
    result.push(id);
  }
  result
}

#[derive(Debug, Clone, Serialize)]
pub struct ProcessStatus {
  pub id: String,
  pub pid: u32,
  pub restarts: u32,
  pub state: &'static str,
  pub package: Option<package_process::PackageIdentity>,
  pub fingerprint: Option<String>,
}

/// The manager process is the parent of every child in this registry.
pub struct Supervisor {
  running: BTreeMap<String, Running>,
  order: Vec<String>,
  logs: log_store::LogStore,
  paths: Paths,
  deployment: String,
  run_id: String,
  control_grants: Option<control_grants::Bundle>,
  candidate_skill_inputs: Option<crate::runtime::process::candidate_skill_preflight::Inputs>,
  admitted_nodes: Arc<BTreeSet<String>>,
  stop_budget: config::StopBudget,
  terminal: Arc<Mutex<Option<dependent_stop::Terminal>>>,
  run_prepared: bool,
  selected: Vec<String>,
  active_revision: String,
  eos_backend: String,
  eos_parts: Vec<(String, String)>,
  unexpected_exit: bool,
  #[cfg(test)]
  retirement_gate: Option<Arc<(std::sync::Barrier, std::sync::Barrier)>>,
}

impl Supervisor {
  pub fn specs(&self) -> Vec<ProcessSpec> {
    self
      .order
      .iter()
      .filter_map(|id| self.running.get(id).map(|running| running.spec.clone()))
      .collect()
  }

  pub fn active_packages(&self) -> Vec<package_process::PackageIdentity> {
    self
      .running
      .values()
      .filter_map(|running| running.spec.package.clone())
      .collect()
  }
  pub fn verify_specs(&self, specs: &[ProcessSpec]) -> Result<()> {
    for spec in specs {
      if let Some(active) = self.running.get(&spec.id) {
        ensure!(
          active.spec.fingerprint == spec.fingerprint,
          "{}: linked package changed while active; restart after revalidation",
          spec.id
        );
        ensure!(
          active.spec.settings.command == spec.settings.command,
          "{}: active command differs from saved graph; restart",
          spec.id
        );
      }
    }
    Ok(())
  }
  pub fn new(paths: &Paths, deployment: &str) -> Result<Self> {
    validate_deployment_name(deployment)?;
    let mut admitted_nodes = BTreeSet::new();
    if let Ok(bytes) = fs::read(paths.node_graph_path(deployment))
      && let Ok(graph) = serde_json::from_slice::<Value>(&bytes)
    {
      for core in ["eos", "egia"] {
        if let Some(layers) = graph
          .pointer(&format!("/cores/{core}/layers"))
          .and_then(Value::as_object)
        {
          for (layer, entry) in layers {
            if let Some(groups) = entry.get("groups").and_then(Value::as_object) {
              for (group, entry) in groups {
                if let Some(nodes) = entry.get("nodes").and_then(Value::as_object) {
                  for node in nodes.keys() {
                    admitted_nodes.insert(format!(
                      "/cores/{core}/layers/{layer}/groups/{group}/nodes/{node}"
                    ));
                  }
                }
              }
            }
          }
        }
      }
    }
    Ok(Self {
      running: BTreeMap::new(),
      order: Vec::new(),
      logs: log_store::LogStore::new(paths, deployment)?,
      paths: paths.clone(),
      deployment: deployment.to_owned(),
      run_id: crate::runtime::manager::next_run_id(),
      control_grants: None,
      candidate_skill_inputs: None,
      admitted_nodes: Arc::new(admitted_nodes),
      stop_budget: config::stop_budget()?,
      terminal: Arc::new(Mutex::new(None)),
      run_prepared: false,
      selected: Vec::new(),
      active_revision: String::new(),
      eos_backend: String::new(),
      eos_parts: Vec::new(),
      unexpected_exit: false,
      #[cfg(test)]
      retirement_gate: None,
    })
  }

  pub fn run_id(&self) -> &str {
    &self.run_id
  }
  pub fn publish_run_scope(&self) -> Result<()> {
    self.logs.publish_run(&self.run_id)
  }
  pub fn set_run_id(&mut self, run_id: String) {
    self.run_id = run_id;
    self.terminal = Arc::new(Mutex::new(None));
    self.run_prepared = false;
    self.selected.clear();
    self.unexpected_exit = false;
  }

  pub fn terminal(&self) -> Option<dependent_stop::Terminal> {
    self.terminal.lock().unwrap().clone()
  }

  pub fn prepare_run(&mut self, specs: &[ProcessSpec], revision: &str) -> Result<()> {
    ensure!(!self.run_prepared, "run is already prepared");
    let ids = stop_order(specs, &[]);
    dependent_stop::create_running(&self.paths, &self.deployment, &self.run_id, &ids)?;
    self.selected = ids;
    self.active_revision = revision.to_owned();
    self.eos_backend.clear();
    self.eos_parts.clear();
    if let Ok(bytes) = fs::read(self.paths.node_graph_path(&self.deployment))
      && let Ok(graph) = serde_json::from_slice::<Value>(&bytes)
    {
      let unified = graph["version"] == "0.2.0";
      let process = if unified {
        "/cores/isos/layers/eos_process/groups"
      } else {
        "/cores/eos/layers/process/groups"
      };
      if let Some(groups) = graph.pointer(process).and_then(Value::as_object) {
        for (group, family) in groups {
          if let Some(nodes) = family.get("nodes").and_then(Value::as_object) {
            for (id, node) in nodes {
              if node.get("package").is_some() {
                let prefix = if unified {
                  "isos/eos_process"
                } else {
                  "eos/process"
                };
                self.eos_backend = format!("{prefix}/{group}/{id}");
              }
            }
          }
        }
      }
      self.eos_parts = selected_eos_stop_parts(&graph, &self.eos_backend);
    }
    self.run_prepared = true;
    Ok(())
  }
  pub fn prepare_control_grants(&mut self, revision: &str) -> Result<()> {
    ensure!(
      self.control_grants.is_none(),
      "control grant bundle is already attached"
    );
    self.control_grants = Some(control_grants::Bundle::issue(
      &self.paths,
      &self.deployment,
      &self.run_id,
      revision,
    )?);
    Ok(())
  }
  pub fn prepare_vision(&mut self, revision: &str) -> Result<()> {
    if crate::runtime::process::vision_binding::selected_gate()?.is_none()
      && !crate::domain::graph::graph_compat::read(&self.paths.node_graph_path(&self.deployment))
        .ok()
        .is_some_and(|saved| saved.value["version"] == "0.2.0")
    {
      return Ok(());
    }
    if let Some(template) = crate::runtime::process::vision_binding::selected_template(
      &self.paths,
      &self.deployment,
      revision,
    )? {
      self.attach_verified_vision_template(template)?;
    }
    Ok(())
  }
  pub fn attach_verified_vision_template(
    &mut self,
    template: crate::runtime::process::vision_binding::Template,
  ) -> Result<()> {
    self
      .control_grants
      .as_mut()
      .context("managed grant bundle missing")?
      .attach_vision(template)
  }
  pub fn admitted_vision_template(
    &self,
  ) -> Option<&crate::runtime::process::vision_binding::Template> {
    self
      .control_grants
      .as_ref()
      .and_then(crate::runtime::owners::control_grants::Bundle::vision_template)
  }
  pub fn verify_control_grants(&self, required: bool) -> Result<()> {
    if let Some(bundle) = &self.control_grants {
      bundle.verify()?;
    } else if required {
      return Err(anyhow::Error::new(control_grants::Diagnostic {
        code: "control_grant_invalid_file",
        stage: "spawn",
        path: String::new(),
        message: "Repair private control-grant state and retry launch",
      }));
    }
    Ok(())
  }

  /// Retire an unpublished candidate without hiding an uncertain grant cleanup.
  pub fn retire_candidate_grants(&mut self) -> Result<()> {
    ensure!(
      self.running.is_empty(),
      "candidate children are still running"
    );
    if let Some(bundle) = &self.control_grants {
      bundle.retire()?;
      self.control_grants = None;
    }
    Ok(())
  }

  /// Leave a failed candidate's on-disk directory for operator repair rather
  /// than letting Drop retry and erase the only cleanup breadcrumb.
  pub fn detach_candidate_grants(&mut self) {
    self.control_grants = None;
  }

  pub fn revoke_candidate_grants_keep_directory(&mut self) -> Result<()> {
    ensure!(
      self.running.is_empty(),
      "candidate children are still running"
    );
    if let Some(bundle) = self.control_grants.take() {
      bundle.revoke_keep_directory()?;
    }
    Ok(())
  }

  pub fn candidate_verifier(&self) -> Result<PathBuf> {
    let bundle = self
      .control_grants
      .as_ref()
      .context("candidate grants missing")?;
    bundle.verify()?;
    bundle
      .launch_env("eos")
      .map(|(_, path)| path)
      .context("candidate verifier missing")
  }

  pub fn set_candidate_skill_inputs(
    &mut self,
    inputs: crate::runtime::process::candidate_skill_preflight::Inputs,
  ) {
    self.candidate_skill_inputs = Some(inputs);
  }

  fn spawn(
    &self,
    spec: &ProcessSpec,
    secrets: &BTreeMap<String, std::ffi::OsString>,
    secret_sources: &BTreeSet<String>,
  ) -> Result<(Child, Option<Receiver<log_store::EosStdout>>)> {
    crate::runtime::owners::egia_state::reject_test_env_spec(spec)?;
    if spec.id == "eos" && spec.settings.role.as_deref() == Some("eos") {
      if let Some(inputs) = &self.candidate_skill_inputs {
        inputs.launch_recheck(&self.paths, &self.deployment)?;
        inputs.verify_launch_selectors(spec, secrets, secret_sources)?;
      }
      crate::runtime::process::skill_selection::verify_child_handoff(
        &self.paths,
        &self.deployment,
        spec,
      )?;
    }
    let mut command = Command::new(&spec.settings.command[0]);
    let mut arguments = spec.settings.command[1..].to_vec();
    if matches!(spec.id.as_str(), "eos" | "bridge" | "config")
      && spec.settings.role.as_deref() == Some("eos")
    {
      command.arg("--run-id").arg(&self.run_id);
      if spec.id == "eos"
        && let Some(package) = &spec.package
      {
        let verified = packages::verify(&self.paths, &package.id, &package.version)?;
        let release = verified
          .receipt
          .release_sha256
          .as_deref()
          .context("verified release digest missing")?;
        let payload = verified
          .receipt
          .payload_sha256
          .as_deref()
          .context("verified payload digest missing")?;
        if let Some(position) = spec
          .settings
          .command
          .iter()
          .position(|arg| arg == "--package-process")
        {
          let handoff_path = spec
            .settings
            .command
            .get(position + 1)
            .context("package process handoff path missing")?;
          let handoff_path = Path::new(handoff_path);
          ensure!(
            handoff_path.starts_with(&self.paths.runtime),
            "package handoff escapes runtime directory"
          );
          let bytes = fs::read(handoff_path)?;
          ensure!(bytes.len() <= 65_536, "package handoff exceeds 64 KiB");
          let digest = hex::encode(Sha256::digest(&bytes));
          ensure!(
            handoff_path.file_name().and_then(|name| name.to_str())
              == Some(format!("package-process-{digest}.json").as_str()),
            "package handoff changed before launch"
          );
          verify_admitted_handoff_fingerprint(
            Path::new(&spec.settings.command[0]),
            &digest,
            spec.fingerprint.as_deref(),
          )?;
          let handoff: Value = serde_json::from_slice(&bytes)?;
          ensure!(
            handoff["package"]["id"] == package.id
              && handoff["package"]["version"] == package.version
              && handoff["release_sha256"] == release
              && handoff["payload_sha256"] == payload,
            "package handoff differs from verified receipt"
          );
          let launch: package_process::PackageLaunch =
            serde_json::from_slice(&bytes).context("parse unbound package launch")?;
          let bound = package_process::bind_launch(
            &launch,
            &self.run_id,
            &self.active_revision,
            &verified.root,
          )?;
          let bound_digest = hex::encode(Sha256::digest(&bound));
          let bound_path = self
            .paths
            .runtime
            .join(format!("package-process-v1-{bound_digest}.json"));
          config::atomic_write(&bound_path, &bound)?;
          arguments[position] = bound_path.to_string_lossy().into_owned();
        }
        ensure!(
          release.len() == 64
            && release
              .bytes()
              .all(|b| b.is_ascii_hexdigit() && !b.is_ascii_uppercase()),
          "invalid release digest"
        );
        ensure!(
          payload.len() == 64
            && payload
              .bytes()
              .all(|b| b.is_ascii_hexdigit() && !b.is_ascii_uppercase()),
          "invalid payload digest"
        );
        let owner_dir = self.logs.root().join("owner/eos");
        fs::create_dir_all(&owner_dir)?;
        fs::set_permissions(
          self.logs.root().join("owner"),
          fs::Permissions::from_mode(0o700),
        )?;
        fs::set_permissions(&owner_dir, fs::Permissions::from_mode(0o700))?;
        command.arg("--log-dir").arg(owner_dir);
      }
    }
    command.args(&arguments);
    let managed_egia = spec.id == "egia"
      && spec.settings.role.as_deref() == Some("egia")
      && self.control_grants.is_some();
    crate::runtime::process::skill_selection::scrub_inherited(&mut command);
    if managed_egia {
      command.env_remove("ISLAND_STATE");
    }
    command.envs(&spec.settings.env);
    for reserved in control_grants::RESERVED_ENVS {
      command.env_remove(reserved);
    }
    for source in secret_sources {
      command.env_remove(source);
    }
    command.envs(secrets);
    if let Some(bundle) = &self.control_grants {
      bundle.verify()?;
      #[cfg(test)]
      if spec
        .settings
        .env
        .get("ISOS_R03_TEST_FAIL_DELIVERY")
        .is_some_and(|value| value == "1")
      {
        return Err(anyhow::Error::new(control_grants::Diagnostic {
          code: "control_grant_delivery_failed",
          stage: "spawn",
          path: String::new(),
          message: "Repair private control-grant delivery and retry",
        }));
      }
      if let Some((key, path)) = bundle.launch_env(&spec.id) {
        command.env(key, path);
      }
      for (key, path) in bundle.vision_env(&spec.id) {
        command.env(key, path);
      }
      if spec.id == "bridge"
        && let Some(socket) = bundle.vision_socket()
      {
        command.arg("--owner-vision-socket").arg(socket);
      }
    }
    if spec.id == "egia" && spec.settings.role.as_deref() == Some("egia") {
      command.env("ISOS_RUN_ID", &self.run_id);
    }
    if let Some(cwd) = &spec.settings.cwd {
      command.current_dir(cwd);
    }
    command
      .stdin(Stdio::null())
      .stdout(Stdio::piped())
      .stderr(Stdio::piped());
    // A dedicated process group lets stop terminate descendants as well.
    let parent_pid = std::process::id();
    unsafe {
      command.pre_exec(move || {
        if managed_egia {
          libc::umask(0o077);
        }
        if libc::setsid() < 0 {
          return Err(std::io::Error::last_os_error());
        }
        if libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM) < 0 {
          return Err(std::io::Error::last_os_error());
        }
        if libc::getppid() as u32 != parent_pid {
          return Err(std::io::Error::from_raw_os_error(libc::ESRCH));
        }
        Ok(())
      });
    }
    let mut child = command
      .spawn()
      .with_context(|| format!("spawn {}", spec.id))?;
    let pid = child.id();
    let source = if matches!(spec.settings.role.as_deref(), Some("eos" | "egia")) {
      spec.id.clone()
    } else {
      format!("unselected-{}", spec.id)
    };
    let skill_pins = spec
      .settings
      .skill_libraries
      .iter()
      .map(|entry| (entry.package.id.clone(), entry.package.version.clone()))
      .collect::<BTreeSet<_>>();
    let admission = Arc::new(log_store::ChildAdmission {
      nodes: Some(self.admitted_nodes.clone()),
      skill_pins,
    });
    let eos_stdout = if let Some(stdout) = child.stdout.take() {
      if spec.id == "eos" && spec.settings.role.as_deref() == Some("eos") {
        Some(log_store::capture_eos_stdout(
          stdout,
          self.logs.clone(),
          self.run_id.clone(),
          pid,
        ))
      } else {
        log_store::capture(
          stdout,
          self.logs.clone(),
          source.clone(),
          "stdout",
          self.run_id.clone(),
          pid,
          Some(admission.clone()),
        );
        None
      }
    } else {
      None
    };
    if let Some(stderr) = child.stderr.take() {
      log_store::capture(
        stderr,
        self.logs.clone(),
        source,
        "stderr",
        self.run_id.clone(),
        pid,
        Some(admission),
      );
    }
    Ok((child, eos_stdout))
  }

  fn unix_socket_ready(path: &Path) -> bool {
    let bytes = path.as_os_str().as_bytes();
    if bytes.is_empty() || bytes.len() > 107 || bytes.contains(&0) {
      return false;
    }
    // A nonblocking connect keeps a full listener backlog within the startup deadline.
    unsafe {
      let fd = libc::socket(
        libc::AF_UNIX,
        libc::SOCK_STREAM | libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC,
        0,
      );
      if fd < 0 {
        return false;
      }
      let mut address: libc::sockaddr_un = std::mem::zeroed();
      address.sun_family = libc::AF_UNIX as libc::sa_family_t;
      for (destination, byte) in address.sun_path.iter_mut().zip(bytes.iter()) {
        *destination = *byte as libc::c_char;
      }
      let length =
        (std::mem::offset_of!(libc::sockaddr_un, sun_path) + bytes.len() + 1) as libc::socklen_t;
      let connected = libc::connect(fd, (&raw const address).cast::<libc::sockaddr>(), length) == 0;
      libc::close(fd);
      connected
    }
  }

  fn wait_ready(&self, spec: &ProcessSpec, child: &mut Child) -> Result<()> {
    let vision = self
      .control_grants
      .as_ref()
      .and_then(|bundle| bundle.vision_template());
    let timeout = if vision.is_some() && spec.id == "eos" {
      spec.settings.start_timeout_ms.min(5_000)
    } else {
      spec.settings.start_timeout_ms
    };
    let deadline = Instant::now() + Duration::from_millis(timeout);
    let no_probe_grace = Instant::now() + Duration::from_millis(50);
    loop {
      if observe_exit(child)? {
        bail!("{} exited before ready", spec.id);
      }
      let ready = if let Some(endpoint) = &spec.settings.ready_tcp {
        endpoint.to_socket_addrs().ok().is_some_and(|addrs| {
          addrs
            .into_iter()
            .any(|addr| TcpStream::connect_timeout(&addr, Duration::from_millis(100)).is_ok())
        })
      } else if let Some(path) = &spec.settings.ready_unix {
        Self::unix_socket_ready(path)
      } else {
        Instant::now() >= no_probe_grace
      };
      let owner_ready = if matches!(spec.settings.role.as_deref(), Some("eos" | "egia")) {
        let expected = format!("{}_ready", spec.id);
        self
          .logs
          .tail_filtered(&spec.id, 100, None)
          .ok()
          .is_some_and(|events| {
            events.iter().any(|event| {
              event["run_id"].as_str() == Some(self.run_id.as_str())
                && event["pid"].as_u64() == Some(child.id() as u64)
                && event["event_code"].as_str() == Some(expected.as_str())
            })
          })
      } else {
        true
      };
      let vision_ready = if let Some(bundle) = &self.control_grants {
        match spec.id.as_str() {
          "eos" => bundle.vision_template().is_none_or(|template| {
            crate::runtime::process::vision_binding::attest_eos(spec, template, &self.run_id)
          }),
          "bridge" => bundle.vision_socket().as_deref().is_none_or(|socket| {
            crate::runtime::process::vision_binding::bridge_ready(spec, socket)
          }),
          _ => true,
        }
      } else {
        true
      };
      if ready && owner_ready && vision_ready {
        return Ok(());
      }
      ensure!(
        Instant::now() < deadline,
        "{} did not become ready",
        spec.id
      );
      thread::sleep(Duration::from_millis(50));
    }
  }

  #[cfg(test)]
  pub fn start(&mut self, specs: &[ProcessSpec]) -> Result<Vec<String>> {
    let plan = crate::domain::graph::generated_v2::secret_plan(specs)?;
    self.start_with_secrets(specs, &plan)
  }

  pub fn start_with_secrets(
    &mut self,
    specs: &[ProcessSpec],
    secrets: &crate::domain::graph::generated_v2::SecretPlan,
  ) -> Result<Vec<String>> {
    ensure!(
      self.terminal().is_none(),
      "stopped run cannot launch children"
    );
    if let Some(bundle) = &self.control_grants {
      bundle.verify()?;
    }
    let secret_sources: BTreeSet<String> = self
      .running
      .values()
      .map(|running| &running.spec)
      .chain(specs.iter())
      .flat_map(|spec| spec.settings.env_from.values().cloned())
      .collect();
    for spec in specs {
      ensure!(
        spec.settings.env_from.keys().all(|target| secrets
          .get(&spec.id)
          .is_some_and(|child| child.contains_key(target))),
        "{}: child secret plan is incomplete",
        spec.id
      );
    }
    let mut added = Vec::new();
    for spec in specs {
      if self.running.contains_key(&spec.id) {
        continue;
      }
      for dependency in &spec.settings.depends_on {
        let id = dependency.rsplit('/').next().unwrap_or(dependency);
        ensure!(
          self.running.contains_key(id),
          "{} needs process {id}",
          spec.id
        );
      }
      let empty = BTreeMap::new();
      let (mut child, eos_stdout) = self.spawn(
        spec,
        secrets.get(&spec.id).unwrap_or(&empty),
        &secret_sources,
      )?;
      let identity = leader_identity(child.id() as i32).ok();
      if let Err(error) = self.wait_ready(spec, &mut child) {
        self.running.insert(
          spec.id.clone(),
          Running {
            spec: spec.clone(),
            child,
            restarts: 0,
            identity,
            eos_stdout,
          },
        );
        self.order.push(spec.id.clone());
        return Err(error);
      }
      self.running.insert(
        spec.id.clone(),
        Running {
          spec: spec.clone(),
          child,
          restarts: 0,
          identity,
          eos_stdout,
        },
      );
      self.order.push(spec.id.clone());
      added.push(spec.id.clone());
    }
    Ok(added)
  }

  pub fn stop(&mut self) -> Vec<String> {
    self.stop_with_terminal().0
  }

  pub fn stop_with_terminal(&mut self) -> (Vec<String>, dependent_stop::Terminal) {
    if let Some(terminal) = self.terminal() {
      return (Vec::new(), terminal);
    }
    let start = Instant::now();
    let deadline = start + Duration::from_millis(self.stop_budget.timeout_ms);
    let cleanup_at = deadline - Duration::from_millis(2_000);
    let dependent_grace = self.stop_budget.timeout_ms - self.stop_budget.eos_grace_ms - 2_000;
    let dependent_until = start + Duration::from_millis(dependent_grace.min(8_000));
    let specs: Vec<_> = self
      .running
      .values()
      .map(|running| running.spec.clone())
      .collect();
    let ids = stop_order(&specs, &self.order);
    let record_ids = if self.run_prepared {
      self.selected.clone()
    } else {
      ids.clone()
    };
    let timer_fence = self.terminal.clone();
    let timer_run = self.run_id.clone();
    let timer_ids = record_ids.clone();
    thread::spawn(move || {
      thread::sleep(deadline.saturating_duration_since(Instant::now()));
      let mut fence = timer_fence.lock().unwrap();
      if fence.is_none() {
        *fence = Some(dependent_stop::Terminal::unknown(
          timer_run,
          "stop_deadline_exceeded",
          &timer_ids,
        ));
      }
    });
    let mut intent_ok = false;
    if self.run_prepared || !record_ids.is_empty() {
      if !self.run_prepared {
        let _ =
          dependent_stop::create_running(&self.paths, &self.deployment, &self.run_id, &record_ids);
      }
      let (sender, receiver) = mpsc::sync_channel(1);
      let paths = self.paths.clone();
      let deployment = self.deployment.clone();
      let run_id = self.run_id.clone();
      let record_ids_for_worker = record_ids.clone();
      thread::spawn(move || {
        let success =
          dependent_stop::publish_unknown(&paths, &deployment, &run_id, &record_ids_for_worker)
            .is_ok();
        let _ = sender.try_send(success);
      });
      intent_ok = receiver
        .recv_timeout(Duration::from_millis(500))
        .unwrap_or(false);
    }
    let mut stopped = Vec::new();
    let mut processes = Vec::new();
    let mut first_reason = if !intent_ok {
      Some("stop_result_persist_failed")
    } else if self.unexpected_exit {
      Some("child_exit_unexpected")
    } else {
      None
    };
    // Revoke both read authorities before any selected child is terminated.
    if let Some(bundle) = self.control_grants.as_mut()
      && bundle.revoke_vision_for_stop().is_err()
    {
      first_reason.get_or_insert("vision_grant_invalid");
    }
    for id in &ids {
      let Some(mut running) = self.running.remove(id) else {
        continue;
      };
      let is_eos = id == "eos"
        && running.spec.settings.role.as_deref() == Some("eos")
        && !self.eos_backend.is_empty();
      let observed_before = observe_exit(&running.child).unwrap_or(false);
      let mut reason = if self.unexpected_exit || observed_before {
        Some("child_exit_unexpected")
      } else {
        None
      };
      if !intent_ok {
        reason.get_or_insert("stop_result_persist_failed");
      }
      let identity = running.identity;
      if let Some(identity) = identity {
        if signal_group(
          identity,
          if intent_ok {
            libc::SIGTERM
          } else {
            libc::SIGKILL
          },
        )
        .is_err()
        {
          reason.get_or_insert("stop_group_unreaped");
        }
      } else {
        reason.get_or_insert("stop_group_unreaped");
      }
      let eos_until = Instant::now() + Duration::from_millis(self.stop_budget.eos_grace_ms);
      let graceful_until = if !intent_ok {
        Instant::now()
      } else if is_eos {
        eos_until.min(cleanup_at)
      } else {
        dependent_until.min(cleanup_at)
      };
      let mut receipt = EosTranscript::default();
      loop {
        if is_eos && let Some(receiver) = &running.eos_stdout {
          while let Ok(event) = receiver.try_recv() {
            receipt.accept(event);
          }
        }
        let exited = observe_exit(&running.child).unwrap_or(false);
        let drained = identity
          .and_then(|held| group_live(held).ok())
          .is_some_and(|live| !live);
        if exited && drained && (!is_eos || receipt.eof_at.is_some() || receipt.invalid) {
          break;
        }
        if Instant::now() >= graceful_until {
          break;
        }
        thread::sleep(Duration::from_millis(10));
      }
      let mut drained = identity
        .and_then(|held| group_live(held).ok())
        .is_some_and(|live| !live);
      if !drained {
        if let Some(identity) = identity {
          if signal_group(identity, libc::SIGKILL).is_err() {
            reason.get_or_insert("stop_group_unreaped");
          } else {
            reason.get_or_insert("stop_forced_kill");
          }
        } else {
          reason.get_or_insert("stop_group_unreaped");
        }
        let reap_until = if is_eos {
          (cleanup_at + Duration::from_millis(1_500)).min(deadline)
        } else {
          (Instant::now() + Duration::from_millis(200)).min(cleanup_at)
        };
        while Instant::now() < reap_until {
          drained = identity
            .and_then(|held| group_live(held).ok())
            .is_some_and(|live| !live);
          if drained && observe_exit(&running.child).unwrap_or(false) {
            break;
          }
          thread::sleep(Duration::from_millis(10));
        }
      }
      if !drained {
        reason.get_or_insert("stop_group_unreaped");
      }
      let exit = if observe_exit(&running.child).unwrap_or(false) {
        running.child.wait().ok()
      } else {
        None
      };
      if exit.is_some() {
        stopped.push(id.clone());
      } else {
        reason.get_or_insert("stop_group_unreaped");
      }
      if is_eos {
        let eos_reason = self.eos_receipt_reason(&running, &receipt, exit, eos_until, deadline);
        if let Some(eos_reason) = eos_reason {
          reason.get_or_insert(eos_reason);
        }
      }
      let outcome = if reason.is_some() {
        "unknown"
      } else {
        "confirmed"
      };
      if first_reason.is_none() {
        first_reason = reason;
      }
      processes.push(dependent_stop::ChildOutcome {
        id: id.clone(),
        outcome,
        reason_code: reason,
      });
      if exit.is_none() {
        self.running.insert(id.clone(), running);
      }
    }
    // A failed startup may have admitted the full plan but spawned only a
    // prefix.  The absent children have no process group to drain, yet the
    // terminal result must still account for every selected process.
    if self.run_prepared {
      let mut observed: BTreeMap<_, _> = processes
        .into_iter()
        .map(|outcome| (outcome.id.clone(), outcome))
        .collect();
      processes = self
        .selected
        .iter()
        .map(|id| {
          observed.remove(id).unwrap_or(dependent_stop::ChildOutcome {
            id: id.clone(),
            outcome: if intent_ok && !self.unexpected_exit {
              "confirmed"
            } else {
              "unknown"
            },
            reason_code: if !intent_ok {
              Some("stop_result_persist_failed")
            } else if self.unexpected_exit {
              Some("child_exit_unexpected")
            } else {
              None
            },
          })
        })
        .collect();
    }
    if let Some(bundle) = self.control_grants.take() {
      let (sender, receiver) = mpsc::sync_channel(1);
      #[cfg(test)]
      let retirement_gate = self.retirement_gate.clone();
      let launched = thread::Builder::new()
        .name("isos-grant-retire".into())
        .spawn(move || {
          #[cfg(test)]
          if let Some(gate) = retirement_gate {
            gate.0.wait();
            gate.1.wait();
          }
          let _ = sender.try_send(bundle.retire().is_ok());
        })
        .is_ok();
      let retirement_until = deadline - Duration::from_millis(500);
      let allowance = retirement_until
        .saturating_duration_since(Instant::now())
        .min(Duration::from_millis(500));
      if !launched || !receiver.recv_timeout(allowance).unwrap_or(false) {
        first_reason.get_or_insert("control_grant_retire_failed");
      }
    }
    if Instant::now() >= deadline {
      first_reason = Some("stop_deadline_exceeded");
    }
    if let Some(reason) = first_reason
      && processes.iter().all(|entry| entry.outcome == "confirmed")
      && let Some(last) = processes.last_mut()
    {
      last.outcome = "unknown";
      last.reason_code = Some(reason);
    }
    let mut terminal = dependent_stop::Terminal {
      run_id: self.run_id.clone(),
      outcome: if first_reason.is_some() {
        "unknown"
      } else {
        "confirmed"
      },
      reason_code: first_reason,
      recovery_method: None,
      processes,
    };
    if terminal.outcome == "confirmed" {
      let (sender, receiver) = mpsc::sync_channel(1);
      let paths = self.paths.clone();
      let deployment = self.deployment.clone();
      let proof = terminal.clone();
      let launched = thread::Builder::new()
        .name("isos-stop-confirm".into())
        .spawn(move || {
          let _ =
            sender.try_send(dependent_stop::publish_confirmed(&paths, &deployment, &proof).is_ok());
        })
        .is_ok();
      let allowance = deadline
        .saturating_duration_since(Instant::now())
        .saturating_sub(Duration::from_millis(100))
        .min(Duration::from_millis(400));
      if !launched || !receiver.recv_timeout(allowance).unwrap_or(false) {
        terminal = dependent_stop::Terminal::unknown(
          self.run_id.clone(),
          "stop_result_persist_failed",
          &record_ids,
        );
      }
    }
    let mut fence = self.terminal.lock().unwrap();
    if fence.is_none() {
      *fence = Some(if Instant::now() >= deadline {
        dependent_stop::Terminal::unknown(
          self.run_id.clone(),
          "stop_deadline_exceeded",
          &record_ids,
        )
      } else {
        terminal
      });
    }
    let terminal = fence.clone().expect("stop fence selected terminal");
    drop(fence);
    self.order.clear();
    (stopped, terminal)
  }

  fn eos_receipt_reason(
    &self,
    running: &Running,
    receipt: &EosTranscript,
    exit: Option<ExitStatus>,
    eos_until: Instant,
    deadline: Instant,
  ) -> Option<&'static str> {
    if Instant::now() >= deadline {
      return Some("stop_deadline_exceeded");
    }
    if receipt.first_at.is_some_and(|at| at >= eos_until)
      || receipt.eof_at.is_some_and(|at| at >= eos_until)
    {
      return Some("eos_receipt_late");
    }
    if receipt.invalid || (receipt.frames.len() == 1 && receipt.eof_at.is_none()) {
      return Some("eos_receipt_invalid");
    }
    if receipt.frames.is_empty() {
      return Some("eos_receipt_missing");
    }
    if receipt.frames.len() != 1 {
      return Some("eos_receipt_invalid");
    }
    let expected = dependent_stop::ReceiptExpectation {
      run_id: &self.run_id,
      eos_pid: running.child.id(),
      revision: &self.active_revision,
      backend_node: &self.eos_backend,
      parts: &self.eos_parts,
    };
    match dependent_stop::verify_receipt(&receipt.frames[0], &expected) {
      Err(_) => Some("eos_receipt_invalid"),
      Ok(false) => Some("eos_stop_unconfirmed"),
      Ok(true) if !exit.is_some_and(|status| status.success()) => Some("eos_stop_unconfirmed"),
      Ok(true) => None,
    }
  }

  pub fn poll(&mut self) -> Result<()> {
    if self.terminal().is_some() {
      return Ok(());
    }
    let exited = self
      .running
      .values()
      .any(|running| observe_exit(&running.child).unwrap_or(true));
    if exited {
      self.unexpected_exit = true;
      bail!("managed child exited unexpectedly; dependent stop is unconfirmed");
    }
    Ok(())
  }

  pub fn mark_unexpected_exit(&mut self) {
    self.unexpected_exit = true;
  }

  pub fn status(&mut self) -> Result<Vec<ProcessStatus>> {
    self.poll()?;
    Ok(self.status_snapshot())
  }

  pub fn status_snapshot(&self) -> Vec<ProcessStatus> {
    self
      .running
      .iter()
      .map(|(id, running)| ProcessStatus {
        id: id.clone(),
        pid: running.child.id(),
        restarts: running.restarts,
        state: if observe_exit(&running.child).unwrap_or(false) {
          "exited"
        } else {
          "running"
        },
        package: running.spec.package.clone(),
        fingerprint: running.spec.fingerprint.clone(),
      })
      .collect()
  }
}

impl Drop for Supervisor {
  fn drop(&mut self) {
    self.stop();
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use serde_json::json;
  use std::os::unix::fs::MetadataExt;
  use std::os::unix::net::UnixListener;
  use std::sync::mpsc;

  #[test]
  fn stop_parts_follow_declared_backend_and_hardware_groups() {
    let graph = json!({"cores":{"eos":{"layers":{"hardware":{"groups":{
      "base":{"nodes":{"drive":{"adapter":{"settings":{"backend":"eos/process/simulator/main"}},"references":["eos/process/simulator/main"],"movement":{}}}},
      "manipulator":{"nodes":{"arm":{"adapter":{"settings":{"backend":"eos/process/simulator/main"}},"references":["eos/process/simulator/main"],"movement":{}}}},
      "camera":{"nodes":{"front":{"adapter":{"settings":{"backend":"eos/process/simulator/main"}}}}},
      "other":{"nodes":{"elsewhere":{"adapter":{"settings":{"backend":"eos/process/other/main"}},"movement":{}}}}
    }}}}}});
    assert_eq!(
      selected_eos_stop_parts(&graph, "eos/process/simulator/main"),
      vec![
        ("eos/hardware/base/drive".into(), "base".into()),
        ("eos/hardware/manipulator/arm".into(), "manipulator".into())
      ]
    );
  }

  #[test]
  fn substituted_package_handoff_is_not_admitted() {
    let root = tempfile::tempdir().unwrap();
    let executable = root.path().join("eos");
    fs::write(&executable, b"eos binary").unwrap();
    let admitted_launch = hex::encode(Sha256::digest(b"admitted launch"));
    let mut fingerprint = Sha256::new();
    fingerprint.update(b"eos binary");
    fingerprint.update(admitted_launch.as_bytes());
    let admitted_fingerprint = hex::encode(fingerprint.finalize());
    assert!(
      verify_admitted_handoff_fingerprint(
        &executable,
        &admitted_launch,
        Some(&admitted_fingerprint)
      )
      .is_ok()
    );
    let alternate_launch = hex::encode(Sha256::digest(b"alternate launch from same package"));
    assert!(
      verify_admitted_handoff_fingerprint(
        &executable,
        &alternate_launch,
        Some(&admitted_fingerprint)
      )
      .is_err()
    );
  }

  #[test]
  fn test_catalog_override_rejects_missing_relative_and_nonexecutable_owner() {
    let temp = tempfile::tempdir().unwrap();
    let missing = temp.path().join("missing-profile");
    assert!(catalog::checked_test_catalog_program(missing).is_err());
    assert!(catalog::checked_test_catalog_program(PathBuf::from("relative-profile")).is_err());
    let file = temp.path().join("profile");
    fs::write(&file, b"#!/bin/sh\nexit 0\n").unwrap();
    assert!(catalog::checked_test_catalog_program(file.clone()).is_err());
    fs::set_permissions(&file, fs::Permissions::from_mode(0o755)).unwrap();
    assert_eq!(
      catalog::checked_test_catalog_program(file.clone()).unwrap(),
      file
    );
  }

  #[test]
  fn unreaped_leader_pins_group_for_descendant_cleanup() {
    let mut command = Command::new("/bin/sh");
    command.arg("-c").arg("sleep 10 & exit 0");
    unsafe {
      command.pre_exec(|| {
        if libc::setsid() < 0 {
          return Err(std::io::Error::last_os_error());
        }
        Ok(())
      });
    }
    let mut child = command.spawn().unwrap();
    let original = leader_identity(child.id() as i32).unwrap();
    let until = Instant::now() + Duration::from_secs(1);
    while !observe_exit(&child).unwrap() && Instant::now() < until {
      thread::sleep(Duration::from_millis(5));
    }
    assert!(observe_exit(&child).unwrap());
    let mut wrong = original;
    wrong.start_ticks += 1;
    assert!(signal_group(wrong, libc::SIGTERM).is_err());
    assert!(signal_group(original, libc::SIGKILL).is_ok());
    let until = Instant::now() + Duration::from_secs(1);
    while group_live(original).unwrap() && Instant::now() < until {
      thread::sleep(Duration::from_millis(5));
    }
    assert!(!group_live(original).unwrap());
    assert!(child.wait().unwrap().success());
  }

  #[test]
  fn selected_eos_raw_stdout_receipt_confirms_only_the_current_run() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    fs::create_dir_all(paths.config.join("deployments")).unwrap();
    fs::write(
      paths.node_graph_path("test"),
      serde_json::json!({
          "cores": {"eos": {"layers": {"process": {"groups": {"worker": {
              "nodes": {"sim": {"id": "sim", "package": {"id":"sample/worker","version":"0.1.0"}}}
          }}}}}}
      })
      .to_string(),
    )
    .unwrap();
    let revision = "a".repeat(64);
    let script = temp.path().join("stub-eos.py");
    let source = r#"#!/usr/bin/env python3
import json,os,signal,subprocess,sys,time
args=sys.argv[1:]
run=args[args.index('--run-id')+1]
print(json.dumps({'timestamp':'2026-11-01T10:00:01.000Z','level':'info','source':'eos','core':'eos','event_code':'eos_ready','run_id':run,'message':'EOS runtime ready','fields':{'state':'ready','node_count':1}}),file=sys.stderr,flush=True)
def stop(sig,frame):
    if os.getenv('R06_HOLD_EOF'):
        subprocess.Popen(['/bin/sleep','2'],start_new_session=True,stdout=sys.stdout,stderr=subprocess.DEVNULL)
    receipt={'schema_version':1,'type':'eos_terminal_stop','run_id':os.getenv('R06_RECEIPT_RUN',run),'eos_pid':os.getpid(),'active_revision':'__REV__','backend_node':'eos/process/worker/sim','backend_pid':1234,'backend_pgid':1234,'generation_id':'00112233445566778899aabbccddeeff','stop_epoch':'1','admission_closed':True,'parts':[],'backend_group_drained':True,'backend_reaped':True,'sockets_removed':True,'outcome':'confirmed','reason_code':None}
    print(json.dumps(receipt,separators=(',',':')),flush=True)
    sys.exit(0)
signal.signal(signal.SIGTERM,stop)
while True: time.sleep(.05)
"#.replace("__REV__", &revision);
    fs::write(&script, source).unwrap();
    fs::set_permissions(&script, fs::Permissions::from_mode(0o700)).unwrap();
    let spec = ProcessSpec {
      id: "eos".into(),
      path: "isos/process/component/eos".into(),
      settings: serde_json::from_value(serde_json::json!({"command":[script],"role":"eos",
                "start_timeout_ms":1000}))
      .unwrap(),
      package: None,
      fingerprint: None,
    };
    let mut supervisor = Supervisor::new(&paths, "test").unwrap();
    supervisor.stop_budget = config::StopBudget {
      timeout_ms: 5_000,
      eos_grace_ms: 1_000,
    };
    supervisor.publish_run_scope().unwrap();
    supervisor
      .prepare_run(std::slice::from_ref(&spec), &revision)
      .unwrap();
    supervisor.start(std::slice::from_ref(&spec)).unwrap();
    let (stopped, terminal) = supervisor.stop_with_terminal();
    assert_eq!(stopped, ["eos"]);
    assert_eq!(terminal.outcome, "confirmed");
    assert_eq!(terminal.reason_code, None);
    assert_eq!(supervisor.stop_with_terminal().0.len(), 0);
    assert_eq!(
      dependent_stop::recover(&paths, "test", supervisor.run_id()).outcome,
      "confirmed"
    );
    let mut wrong = spec.clone();
    wrong
      .settings
      .env
      .insert("R06_RECEIPT_RUN".into(), "r-stale".into());
    let mut second = Supervisor::new(&paths, "test").unwrap();
    second.stop_budget = config::StopBudget {
      timeout_ms: 5_000,
      eos_grace_ms: 1_000,
    };
    second.publish_run_scope().unwrap();
    second.prepare_run(&[wrong.clone()], &revision).unwrap();
    second.start(&[wrong]).unwrap();
    let (_, terminal) = second.stop_with_terminal();
    assert_eq!(terminal.outcome, "unknown");
    assert_eq!(terminal.reason_code, Some("eos_receipt_invalid"));
    let mut held = spec;
    held.settings.env.insert("R06_HOLD_EOF".into(), "1".into());
    let mut third = Supervisor::new(&paths, "test").unwrap();
    third.stop_budget = config::StopBudget {
      timeout_ms: 5_000,
      eos_grace_ms: 1_000,
    };
    third.publish_run_scope().unwrap();
    third.prepare_run(&[held.clone()], &revision).unwrap();
    third.start(&[held]).unwrap();
    let started = Instant::now();
    let (_, terminal) = third.stop_with_terminal();
    assert_eq!(terminal.reason_code, Some("eos_receipt_invalid"));
    assert!(started.elapsed() < Duration::from_secs(3));
  }

  #[test]
  fn unpublished_candidate_grants_have_checked_retirement() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let mut candidate = Supervisor::new(&paths, "test").unwrap();
    candidate.prepare_control_grants(&"a".repeat(64)).unwrap();
    let bundle = paths
      .runtime
      .join(format!("control-grants/test/{}", candidate.run_id()));
    let verifier = candidate.candidate_verifier().unwrap();
    fs::set_permissions(&verifier, fs::Permissions::from_mode(0o644)).unwrap();
    assert!(candidate.retire_candidate_grants().is_err());
    assert!(bundle.exists());
    fs::set_permissions(&verifier, fs::Permissions::from_mode(0o600)).unwrap();
    candidate.retire_candidate_grants().unwrap();
    assert!(!bundle.exists());
  }

  #[test]
  fn managed_grants_are_delivered_only_to_the_named_children_and_retired_after_stop() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let script = temp.path().join("capture-env.sh");
    fs::write(
      &script,
      r#"#!/bin/sh
set -eu
for name in EOS_CONTROL_GRANTS_FILE EGIA_EOS_CONTROL_GRANT_FILE AETHER_BRIDGE_CONTROL_GRANT_FILE; do
  eval 'value=${'"$name"'-}'
  if [ -n "$value" ]; then printf '%s\n' "$name" >> "$CAPTURE_PATH"; fi
done
sleep 10
"#,
    )
    .unwrap();
    fs::set_permissions(&script, fs::Permissions::from_mode(0o700)).unwrap();
    let mut supervisor = Supervisor::new(&paths, "test").unwrap();
    let run_id = supervisor.run_id().to_owned();
    supervisor.prepare_control_grants(&"a".repeat(64)).unwrap();
    let mut specs = Vec::new();
    for id in ["eos", "config", "egia", "bridge", "other"] {
      let capture = temp.path().join(format!("{id}.env"));
      let spec = ProcessSpec {
        id: id.into(),
        path: format!("isos/process/component/{id}"),
        settings: serde_json::from_value(serde_json::json!({
            "command":[script],
            "env":{
                "CAPTURE_PATH":capture,
                "EOS_CONTROL_GRANTS_FILE":"authored-fake",
                "EGIA_EOS_CONTROL_GRANT_FILE":"authored-fake",
                "AETHER_BRIDGE_CONTROL_GRANT_FILE":"authored-fake"
            },
            "start_timeout_ms":1000
        }))
        .unwrap(),
        package: None,
        fingerprint: None,
      };
      specs.push(spec);
    }
    supervisor.start(&specs).unwrap();
    for (id, expected) in [
      ("eos", "EOS_CONTROL_GRANTS_FILE\n"),
      ("config", ""),
      ("egia", "EGIA_EOS_CONTROL_GRANT_FILE\n"),
      ("bridge", "AETHER_BRIDGE_CONTROL_GRANT_FILE\n"),
      ("other", ""),
    ] {
      let capture = temp.path().join(format!("{id}.env"));
      let content = fs::read_to_string(capture).unwrap_or_default();
      assert_eq!(content, expected, "recipient {id}");
    }
    let bundle = paths.runtime.join(format!("control-grants/test/{run_id}"));
    assert!(bundle.exists());
    assert_eq!(supervisor.stop().len(), specs.len());
    assert!(!bundle.exists());
  }

  #[test]
  fn managed_egia_scrubs_inherited_state_and_uses_private_umask() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let capture = temp.path().join("egia-state-env");
    let script = temp.path().join("capture-state.sh");
    fs::write(
            &script,
            r#"#!/bin/sh
mkdir -p "$2/egia/missions"
printf '%s|%s\n' "${ISLAND_STATE-unset}" "$(umask)" > "$CAPTURE_PATH"
printf '{"timestamp":"2026-11-02T10:00:01.000Z","level":"info","source":"egia","core":"egia","event_code":"egia_ready","run_id":"%s","message":"EGIA ready","fields":{"generation":1}}\n' "$ISOS_RUN_ID" >&2
sleep 10
"#,
        )
        .unwrap();
    fs::set_permissions(&script, fs::Permissions::from_mode(0o700)).unwrap();
    let mut supervisor = Supervisor::new(&paths, "egia-state-test").unwrap();
    supervisor.prepare_control_grants(&"a".repeat(64)).unwrap();
    let spec = ProcessSpec {
      id: "egia".into(),
      path: "isos/process/component/egia".into(),
      settings: serde_json::from_value(serde_json::json!({
          "command":[script], "role":"egia", "env":{"CAPTURE_PATH":capture},
          "start_timeout_ms":1000
      }))
      .unwrap(),
      package: None,
      fingerprint: None,
    };
    let mut specs = [spec];
    crate::runtime::owners::egia_state::prepare(&paths, "egia-state-test", &mut specs).unwrap();
    supervisor.start(&specs).unwrap();
    let output = wait_for_file(&capture, 1);
    assert_eq!(output.trim(), "unset|0077");
    let mission_dir = paths.data.join("state/egia-state-test/egia/missions");
    assert_eq!(
      fs::symlink_metadata(&mission_dir)
        .unwrap()
        .permissions()
        .mode()
        & 0o7777,
      0o700
    );
    supervisor.stop();
    let mut next = [specs[0].clone()];
    next[0].settings.command.truncate(1);
    crate::runtime::owners::egia_state::prepare(&paths, "egia-state-test", &mut next).unwrap();
    assert_eq!(next[0].settings.command, specs[0].settings.command);
    let mut restarted = Supervisor::new(&paths, "egia-state-test").unwrap();
    restarted.prepare_control_grants(&"b".repeat(64)).unwrap();
    restarted.start(&next).unwrap();
    assert_eq!(wait_for_file(&capture, 1).trim(), "unset|0077");
    restarted.stop();
  }

  #[test]
  fn egia_spawn_rechecks_q01_test_env_targets() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let supervisor = Supervisor::new(&paths, "q01-env-guard").unwrap();
    for field in ["env", "env_from"] {
      let mut spec = ProcessSpec {
        id: "egia".into(),
        path: "isos/process/component/egia".into(),
        settings: serde_json::from_value(serde_json::json!({
            "command":["/bin/true"], "role":"egia"
        }))
        .unwrap(),
        package: None,
        fingerprint: None,
      };
      if field == "env" {
        spec
          .settings
          .env
          .insert("EGIA_Q01_STALE_TEST".into(), "1".into());
      } else {
        spec
          .settings
          .env_from
          .insert("EGIA_Q01_STALE_TEST".into(), "INHERITED_TEST_FLAG".into());
      }
      let error = supervisor
        .spawn(&spec, &BTreeMap::new(), &BTreeSet::new())
        .unwrap_err();
      let diagnostic = error
        .downcast_ref::<crate::runtime::owners::egia_state::Diagnostic>()
        .unwrap();
      assert_eq!(diagnostic.code, "egia_test_reserved_env");
      assert_eq!(
        diagnostic.path,
        format!(
          "/cores/isos/layers/process/groups/component/nodes/egia/settings/{field}/EGIA_Q01_STALE_TEST"
        )
      );
    }
  }

  #[test]
  fn automatic_child_restart_reuses_run_and_control_grant_bundle() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let sentinel = temp.path().join("first-start");
    let script = temp.path().join("restart-once.sh");
    fs::write(
      &script,
      format!(
        "#!/bin/sh\nif [ ! -e '{}' ]; then touch '{}'; sleep 0.3; exit 1; fi\nsleep 10\n",
        sentinel.display(),
        sentinel.display()
      ),
    )
    .unwrap();
    fs::set_permissions(&script, fs::Permissions::from_mode(0o700)).unwrap();
    let mut supervisor = Supervisor::new(&paths, "test").unwrap();
    let run_id = supervisor.run_id().to_owned();
    supervisor.prepare_control_grants(&"a".repeat(64)).unwrap();
    let verifier = paths
      .runtime
      .join(format!("control-grants/test/{run_id}/verifier.json"));
    let original_inode = fs::symlink_metadata(&verifier).unwrap().ino();
    let spec = ProcessSpec {
      id: "eos".into(),
      path: "isos/process/component/eos".into(),
      settings: serde_json::from_value(serde_json::json!({
          "command":[script], "restart":"on_failure", "max_restarts":1,
          "start_timeout_ms":1000
      }))
      .unwrap(),
      package: None,
      fingerprint: None,
    };
    supervisor.start(&[spec]).unwrap();
    thread::sleep(Duration::from_millis(400));
    assert!(supervisor.poll().is_err());
    assert_eq!(supervisor.run_id(), run_id);
    assert_eq!(supervisor.status_snapshot()[0].restarts, 0);
    assert_eq!(
      fs::symlink_metadata(&verifier).unwrap().ino(),
      original_inode
    );
    let (_, terminal) = supervisor.stop_with_terminal();
    assert_eq!(terminal.outcome, "unknown");
    assert!(!verifier.exists());
  }

  fn wait_for_file(path: &Path, expected_lines: usize) -> String {
    for _ in 0..200 {
      if let Ok(bytes) = fs::read_to_string(path)
        && bytes.lines().count() >= expected_lines
      {
        return bytes;
      }
      thread::sleep(Duration::from_millis(10));
    }
    panic!("child did not write the expected record");
  }

  #[test]
  fn backend_resolution_failure_carries_exact_package_identity() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let graph = serde_json::json!({"version":"1.0.0","robot":{"id":"r","name":"R"},"cores":{"eos":{"layers":{"process":{"groups":{"worker":{"nodes":{"sim":{"id":"sim","node_type":"eos.process.worker@v1","package":{"id":"sample/worker","version":"0.1.1"},"settings":{}}}}}}}}}}).to_string();
    let error = project_with_packages(&paths, &graph).unwrap_err();
    let failure = error.downcast_ref::<packages::PackageFailure>().unwrap();
    assert_eq!(failure.id, "sample/worker");
    assert_eq!(failure.version, "0.1.1");
    assert_eq!(failure.stage, "manifest");
  }

  fn graph(settings: &str) -> String {
    let settings: Value = serde_json::from_str(settings).unwrap();
    serde_json::json!({
        "version": "1.0.0",
        "robot": {"id": "r", "name": "R"},
        "cores": {"isos": {"layers": {"process": {"groups": {"component": {
            "nodes": {"eos": {
                "id": "eos", "node_type": "isos.process.component@v1.1",
                "settings": settings
            }}
        }}}}}}
    })
    .to_string()
  }

  #[test]
  fn projection_rejects_wrong_type_placement_and_unbounded_launch() {
    let source = graph(r#"{"command":["/bin/sleep","1"]}"#);
    assert_eq!(project(&source).unwrap()[0].id, "eos");
    assert!(project(&source.replace("v1.1", "v2.0")).is_err());
    assert!(project(&source.replace("\"process\"", "\"hardware\"")).is_err());
    assert!(project(&graph(r#"{"command":["sleep","1"]}"#)).is_err());
    let toml = r#"
version = "1.0.0"
[robot]
id = "r"
name = "R"
[cores.isos.layers.process.groups.component.nodes.eos]
id = "eos"
node_type = "isos.process.component@v1.1"
label = "EOS"
description = "runtime"
[cores.isos.layers.process.groups.component.nodes.eos.settings]
command = ["/bin/sleep", "1"]
"#;
    assert_eq!(project(toml).unwrap()[0].id, "eos");
  }

  #[test]
  fn unix_readiness_settings_are_bounded_and_exclusive() {
    assert!(
      project(&graph(
        r#"{"command":["/bin/sleep","1"],"ready_unix":"relative.sock"}"#
      ))
      .is_err()
    );
    assert!(
      project(&graph(
        &serde_json::json!({
            "command": ["/bin/sleep", "1"],
            "ready_unix": format!("/{}", "x".repeat(107))
        })
        .to_string()
      ))
      .is_err()
    );
    assert!(
      project(&graph(
        r#"{"command":["/bin/sleep","1"],"ready_unix":"/tmp/eos.sock","ready_tcp":"127.0.0.1:1"}"#
      ))
      .is_err()
    );
  }

  #[test]
  fn rejects_workbench_and_duplicate_listener_ports() {
    let mut spec = project(&graph(
      r#"{"command":["/bin/sleep","1"],"ready_tcp":"127.0.0.1:16020"}"#,
    ))
    .unwrap()
    .remove(0);
    let mut specs = BTreeMap::new();
    specs.insert(spec.id.clone(), spec.clone());
    assert!(validate_listener_ports(&specs).is_ok());

    let mut second = spec.clone();
    second.id = "bridge".into();
    second.path = "isos/process/component/bridge".into();
    specs.insert(second.id.clone(), second);
    assert!(validate_listener_ports(&specs).is_err());

    specs.remove("bridge");
    spec.settings.ready_tcp = Some("127.0.0.1:16000".into());
    specs.insert(spec.id.clone(), spec);
    assert!(validate_listener_ports(&specs).is_err());
  }

  #[test]
  fn selected_real_child_binds_run_pid_and_stderr_only() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    let script = temp.path().join("selected-eos");
    fs::write(&script, r#"#!/bin/sh
run=''
while [ "$#" -gt 0 ]; do
  if [ "$1" = '--run-id' ]; then run="$2"; shift 2; else shift; fi
done
printf '{"timestamp":"2026-11-01T10:00:01.000Z","level":"info","source":"eos","core":"eos","event_code":"eos_ready","run_id":"%s","message":"EOS runtime ready","fields":{"state":"ready","node_count":1}}\n' "$run" >&2
printf '{"timestamp":"2026-11-01T10:00:01.000Z","level":"info","source":"eos","core":"eos","event_code":"eos_ready","run_id":"%s","message":"EOS runtime ready","fields":{"state":"ready","node_count":1}}\n' "$run"
sleep 2
"#).unwrap();
    fs::set_permissions(&script, fs::Permissions::from_mode(0o700)).unwrap();
    let mut spec = project(&graph(&serde_json::json!({"command":[script]}).to_string()))
      .unwrap()
      .remove(0);
    spec.settings.role = Some("eos".into());
    let mut supervisor = Supervisor::new(&paths, "default").unwrap();
    let run_id = supervisor.run_id().to_owned();
    supervisor.start(&[spec]).unwrap();
    let pid = supervisor.status_snapshot()[0].pid;
    let logs = log_store::LogStore::new(&paths, "default").unwrap();
    let deadline = Instant::now() + Duration::from_secs(2);
    let line = loop {
      let lines = logs.read("eos", None, 10).unwrap();
      if let Some(line) = lines["lines"].as_array().unwrap().first() {
        break line.clone();
      }
      assert!(
        Instant::now() < deadline,
        "selected child event did not arrive"
      );
      thread::sleep(Duration::from_millis(10));
    };
    assert_eq!(line["run_id"], run_id);
    assert_eq!(line["pid"], pid);
    assert_eq!(
      logs.read("eos", None, 10).unwrap()["lines"]
        .as_array()
        .unwrap()
        .len(),
      1
    );
    assert!(logs.root().join("owner/eos/stdout.log").is_file());
    assert!(logs.root().join("owner/eos/stderr.log").is_file());
    fs::remove_file(logs.root().join("events.log")).unwrap();
    fs::create_dir(logs.root().join("events.log")).unwrap();
    let mut later = line.clone();
    later["event_code"] = serde_json::json!("eos_stopping");
    later["message"] = serde_json::json!("EOS eos stopping");
    later["fields"] = serde_json::json!({"state":"stopping"});
    assert!(logs.append_selected(&later).is_err());
    let stop_started = Instant::now();
    supervisor.stop();
    assert!(stop_started.elapsed() < Duration::from_secs(1));
    assert!(log_store::counters().1 > 0);
  }

  #[test]
  fn selected_egia_run_binding_overrides_authored_environment() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    let script = temp.path().join("selected-egia");
    fs::write(&script, r#"#!/bin/sh
printf '{"timestamp":"2026-11-01T10:00:01.000Z","level":"info","source":"egia","core":"egia","event_code":"egia_ready","run_id":"%s","message":"EGIA ready","fields":{"generation":1}}\n' "$ISOS_RUN_ID" >&2
sleep 2
"#).unwrap();
    fs::set_permissions(&script, fs::Permissions::from_mode(0o700)).unwrap();
    let mut spec = project(&graph(
      &serde_json::json!({"command":[script],"env":{"ISOS_RUN_ID":"r-spoof"}}).to_string(),
    ))
    .unwrap()
    .remove(0);
    spec.id = "egia".into();
    spec.path = "isos/process/component/egia".into();
    spec.settings.role = Some("egia".into());
    let mut supervisor = Supervisor::new(&paths, "default").unwrap();
    let run_id = supervisor.run_id().to_owned();
    supervisor.start(&[spec]).unwrap();
    let lines = log_store::LogStore::new(&paths, "default")
      .unwrap()
      .read("egia", None, 10)
      .unwrap();
    assert_eq!(lines["lines"][0]["run_id"], run_id);
    supervisor.stop();
  }

  #[test]
  fn waits_for_unix_listener_and_rolls_back_on_timeout() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    let socket = temp.path().join("ready.sock");
    let settings = serde_json::json!({
        "command": ["/bin/sleep", "30"],
        "ready_unix": socket,
        "start_timeout_ms": 1000
    });
    let specs = project(&graph(&settings.to_string())).unwrap();
    let listener_path = socket.clone();
    let (go_tx, go_rx) = mpsc::channel();
    let (done_tx, done_rx) = mpsc::channel();
    let listener = thread::spawn(move || {
      go_rx.recv().unwrap();
      thread::sleep(Duration::from_millis(150));
      let _listener = UnixListener::bind(listener_path).unwrap();
      let _ = done_rx.recv_timeout(Duration::from_secs(2));
    });
    let mut supervisor = Supervisor::new(&paths, "test").unwrap();
    let start = Instant::now();
    go_tx.send(()).unwrap();
    assert_eq!(supervisor.start(&specs).unwrap(), vec!["eos"]);
    assert!(start.elapsed() >= Duration::from_millis(100));
    assert_eq!(supervisor.stop(), vec!["eos"]);
    done_tx.send(()).unwrap();
    listener.join().unwrap();

    fs::remove_file(&socket).unwrap();
    let timeout = serde_json::json!({
        "command": ["/bin/sleep", "30"],
        "ready_unix": socket,
        "start_timeout_ms": 120
    });
    let mut supervisor = Supervisor::new(&paths, "test").unwrap();
    assert!(
      supervisor
        .start(&project(&graph(&timeout.to_string())).unwrap())
        .is_err()
    );
    let _ = supervisor.stop();
    assert!(supervisor.status().unwrap().is_empty());
  }

  #[test]
  fn direct_child_start_stop_and_failed_readiness() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    let mut supervisor = Supervisor::new(&paths, "test").unwrap();
    let specs = project(&graph(r#"{"command":["/bin/sleep","30"]}"#)).unwrap();
    assert_eq!(supervisor.start(&specs).unwrap(), vec!["eos"]);
    assert_eq!(supervisor.status().unwrap().len(), 1);
    assert_eq!(supervisor.stop(), vec!["eos"]);
    assert!(supervisor.status().unwrap().is_empty());

    let bad = project(&graph(
      r#"{"command":["/bin/false"],"start_timeout_ms":20}"#,
    ))
    .unwrap();
    let mut failed = Supervisor::new(&paths, "test").unwrap();
    assert!(failed.start(&bad).is_err());
    let (_, terminal) = failed.stop_with_terminal();
    assert_eq!(terminal.outcome, "unknown");
    assert!(failed.status().unwrap().is_empty());
  }

  #[test]
  fn full_graph_validator_fails_closed() {
    let temp = tempfile::tempdir().unwrap();
    let graph_path = temp.path().join("graph.json");
    fs::write(&graph_path, graph(r#"{"command":["/bin/sleep","1"]}"#)).unwrap();
    validate_with_program(&graph_path, Path::new("/bin/true")).unwrap();
    assert!(validate_with_program(&graph_path, Path::new("/bin/false")).is_err());
    assert!(validate_with_program(&graph_path, Path::new("/missing/validator")).is_err());
  }

  #[test]
  fn target_validation_rejects_cross_owner_reference_before_eos_projection() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let mut graph = crate::domain::graph::starter::draft(&paths, "default").unwrap();
    graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["control"]["nodes"]["command"]["references"] =
      serde_json::json!(["isos/process/component/eos"]);
    let error = validate_value_with_profile(&paths, &graph, Path::new("/bin/true"))
      .unwrap_err()
      .to_string();
    assert!(error.contains("cross_core_link_unavailable"), "{error}");
    assert!(error.contains("/references/0"), "{error}");
  }

  #[test]
  fn restarts_failing_child_with_bounded_attempts() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    let mut supervisor = Supervisor::new(&paths, "test").unwrap();
    let specs = project(&graph(
      r#"{"command":["/bin/sleep","0.1"],"restart":"always","max_restarts":1}"#,
    ))
    .unwrap();
    supervisor.start(&specs).unwrap();
    thread::sleep(Duration::from_millis(140));
    assert!(supervisor.status().is_err());
    let (_, terminal) = supervisor.stop_with_terminal();
    assert_eq!(terminal.outcome, "unknown");
    thread::sleep(Duration::from_millis(140));
    assert!(supervisor.status().unwrap().is_empty());
  }

  #[test]
  fn provider_exit_stops_dependent_process() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    let mut source: Value =
      serde_json::from_str(&graph(r#"{"command":["/bin/sleep","0.1"]}"#)).unwrap();
    source["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["egia"] = serde_json::json!({
        "id": "egia",
        "node_type": "isos.process.component@v1.1",
        "settings": {
            "command": ["/bin/sleep", "30"],
            "depends_on": ["cores/isos/layers/process/groups/component/nodes/eos"]
        }
    });
    let specs = project(&source.to_string()).unwrap();
    assert_eq!(
      specs
        .iter()
        .map(|spec| spec.id.as_str())
        .collect::<Vec<_>>(),
      ["eos", "egia"]
    );
    let mut supervisor = Supervisor::new(&paths, "test").unwrap();
    supervisor.start(&specs).unwrap();
    thread::sleep(Duration::from_millis(140));
    assert!(supervisor.status().is_err());
    let (_, terminal) = supervisor.stop_with_terminal();
    assert_eq!(terminal.outcome, "unknown");
    assert!(supervisor.status().unwrap().is_empty());
  }

  #[test]
  fn failed_startup_terminal_accounts_for_unspawned_selected_child() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let make = |id: &str, depends_on: Vec<&str>| ProcessSpec {
      id: id.into(),
      path: format!("isos/process/component/{id}"),
      settings: serde_json::from_value(serde_json::json!({
          "command": ["/bin/sleep", "30"],
          "depends_on": depends_on,
      }))
      .unwrap(),
      package: None,
      fingerprint: None,
    };
    let provider = make("provider", vec![]);
    let dependent = make("dependent", vec!["provider"]);
    let mut supervisor = Supervisor::new(&paths, "test").unwrap();
    supervisor.publish_run_scope().unwrap();
    supervisor
      .prepare_run(&[provider.clone(), dependent], &"a".repeat(64))
      .unwrap();
    supervisor.start(&[provider]).unwrap();
    let (stopped, terminal) = supervisor.stop_with_terminal();
    assert_eq!(stopped, ["provider"]);
    assert_eq!(terminal.outcome, "confirmed");
    assert_eq!(terminal.processes.len(), 2);
    assert_eq!(terminal.processes[0].id, "dependent");
    assert_eq!(terminal.processes[0].outcome, "confirmed");
    assert_eq!(terminal.processes[1].id, "provider");
  }

  #[test]
  fn stalled_grant_retirement_is_bounded_and_stays_unknown() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let mut supervisor = Supervisor::new(&paths, "retire-stall").unwrap();
    supervisor.publish_run_scope().unwrap();
    supervisor.prepare_control_grants(&"a".repeat(64)).unwrap();
    supervisor.prepare_run(&[], &"a".repeat(64)).unwrap();
    let gate = Arc::new((std::sync::Barrier::new(2), std::sync::Barrier::new(2)));
    supervisor.retirement_gate = Some(gate.clone());
    let started = Instant::now();
    let releaser = thread::spawn(move || {
      gate.0.wait();
      thread::sleep(Duration::from_millis(800));
      gate.1.wait();
    });
    let (_, terminal) = supervisor.stop_with_terminal();
    assert!(started.elapsed() < Duration::from_millis(700));
    assert_eq!(terminal.outcome, "unknown");
    assert_eq!(terminal.reason_code, Some("control_grant_retire_failed"));
    releaser.join().unwrap();
    assert_eq!(supervisor.stop_with_terminal().1, terminal);
  }

  #[test]
  fn env_from_value_reaches_only_its_named_child() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    let source = "ISOS_P04_ISOLATION_SOURCE_9A6D1";
    unsafe { std::env::set_var(source, "private-value") };
    let eos_file = temp.path().join("eos.txt");
    let egia_file = temp.path().join("egia.txt");
    let script = temp.path().join("record.sh");
    fs::write(
            &script,
            format!(
                "#!/bin/sh\nprintf '%s|%s\\n' \"${{{source}-unset}}\" \"${{EGIA_API_KEY-unset}}\" > \"$1\"\nsleep 30\n"
            ),
        )
        .unwrap();
    fs::set_permissions(&script, fs::Permissions::from_mode(0o700)).unwrap();
    let make = |id: &str, output: &Path, secret: bool| {
      ProcessSpec {
            id: id.into(),
            path: format!("isos/process/component/{id}"),
            settings: serde_json::from_value(serde_json::json!({
                "command": [script, output],
                "env_from": if secret { serde_json::json!({"EGIA_API_KEY":source}) } else { serde_json::json!({}) },
            }))
            .unwrap(),
            package: None,
            fingerprint: None,
        }
    };
    let specs = [
      make("eos", &eos_file, false),
      make("egia", &egia_file, true),
    ];
    let mut supervisor = Supervisor::new(&paths, "test").unwrap();
    assert_eq!(supervisor.start(&specs).unwrap(), ["eos", "egia"]);
    assert_eq!(wait_for_file(&eos_file, 1), "unset|unset\n");
    assert_eq!(wait_for_file(&egia_file, 1), "unset|private-value\n");
    supervisor.stop();
    unsafe { std::env::remove_var(source) };
  }

  #[test]
  fn dependent_auto_restart_uses_its_secret_plan_without_inheriting_source() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    let source = "ISOS_P04_RESTART_SOURCE_36B11";
    unsafe { std::env::set_var(source, "restart-value") };
    let record = temp.path().join("dependent.txt");
    let script = temp.path().join("dependent.sh");
    fs::write(
            &script,
            format!(
                "#!/bin/sh\nprintf '%s|%s\\n' \"${{{source}-unset}}\" \"${{EGIA_API_KEY-unset}}\" >> \"$1\"\nsleep 30\n"
            ),
        )
        .unwrap();
    fs::set_permissions(&script, fs::Permissions::from_mode(0o700)).unwrap();
    let provider = ProcessSpec {
      id: "eos".into(),
      path: "isos/process/component/eos".into(),
      settings: serde_json::from_value(serde_json::json!({
          "command": ["/bin/sh", "-c", "sleep 0.35; exit 1"],
          "restart": "always",
          "max_restarts": 1
      }))
      .unwrap(),
      package: None,
      fingerprint: None,
    };
    let dependent = ProcessSpec {
      id: "egia".into(),
      path: "isos/process/component/egia".into(),
      settings: serde_json::from_value(serde_json::json!({
          "command": [script, record],
          "depends_on": ["cores/isos/layers/process/groups/component/nodes/eos"],
          "env_from": {"EGIA_API_KEY": source}
      }))
      .unwrap(),
      package: None,
      fingerprint: None,
    };
    let mut supervisor = Supervisor::new(&paths, "test").unwrap();
    supervisor.start(&[provider, dependent]).unwrap();
    thread::sleep(Duration::from_millis(420));
    assert!(supervisor.status().is_err());
    assert_eq!(wait_for_file(&record, 1), "unset|restart-value\n");
    let (_, terminal) = supervisor.stop_with_terminal();
    assert_eq!(terminal.outcome, "unknown");
    unsafe { std::env::remove_var(source) };
  }
}
