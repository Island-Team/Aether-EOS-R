use std::fs;
use std::collections::HashMap;
use std::io::{Read, Write};
use std::os::unix::fs::{FileTypeExt, MetadataExt, OpenOptionsExt, PermissionsExt};
use std::os::unix::net::UnixStream;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use aether_common::time::SystemClock;
use aether_hal::{is_eos_backend_type, NodeKind, Profile};
use aether_mujoco::{Endpoint, ManagedSession};

pub struct BackendLaunch {
  pub node: String,
  pub executable: PathBuf,
  pub scene: PathBuf,
  pub log_path: Option<PathBuf>,
}

pub struct BackendChild {
  child: Child,
  socket: PathBuf,
  stop_socket: PathBuf,
  session: Arc<ManagedSession>,
  ready_generation: Option<String>,
  stopped: bool,
  identity: Option<ProcIdentity>,
  baseline_children: HashMap<u32, u64>,
  log_workers: Vec<thread::JoinHandle<()>>,
}

#[derive(Clone, Copy)]
struct ProcIdentity {
  pid: u32,
  pgid: i32,
  sid: i32,
  start_ticks: u64,
}

struct ProcStat {
  state: char,
  ppid: i32,
  pgid: i32,
  sid: i32,
  start_ticks: u64,
}

fn proc_stat(pid: u32) -> std::io::Result<ProcStat> {
  let text = fs::read_to_string(format!("/proc/{pid}/stat"))?;
  let tail = text
    .rsplit_once(") ")
    .ok_or_else(|| std::io::Error::new(std::io::ErrorKind::InvalidData, "bad proc stat"))?
    .1;
  let fields: Vec<_> = tail.split_whitespace().collect();
  if fields.len() <= 19 {
    return Err(std::io::Error::new(
      std::io::ErrorKind::InvalidData,
      "short proc stat",
    ));
  }
  let parse = |index: usize| {
    fields[index]
      .parse::<i32>()
      .map_err(|_| std::io::Error::new(std::io::ErrorKind::InvalidData, "bad proc identity"))
  };
  Ok(ProcStat {
    state: fields[0].chars().next().unwrap_or('?'),
    ppid: parse(1)?,
    pgid: parse(2)?,
    sid: parse(3)?,
    start_ticks: fields[19]
      .parse()
      .map_err(|_| std::io::Error::new(std::io::ErrorKind::InvalidData, "bad proc start ticks"))?,
  })
}

pub struct BackendStopReport {
  pub backend_pid: u32,
  pub backend_pgid: i32,
  pub group_drained: bool,
  pub reaped: bool,
  pub sockets_removed: bool,
  pub forced_kill: bool,
  pub reason: Option<&'static str>,
}

fn group_drained(identity: ProcIdentity) -> std::io::Result<bool> {
  let leader = proc_stat(identity.pid)?;
  if leader.start_ticks != identity.start_ticks
    || leader.pgid != identity.pgid
    || leader.sid != identity.sid
  {
    return Err(std::io::Error::new(
      std::io::ErrorKind::InvalidData,
      "backend leader identity changed",
    ));
  }
  let mut drained = leader.state == 'Z' || leader.state == 'X';
  for item in fs::read_dir("/proc")? {
    let item = item?;
    let Some(pid) = item.file_name().to_string_lossy().parse::<u32>().ok() else {
      continue;
    };
    if pid == identity.pid {
      continue;
    }
    let stat = match proc_stat(pid) {
      Ok(stat) => stat,
      Err(error) if error.kind() == std::io::ErrorKind::NotFound => continue,
      Err(error) => return Err(error),
    };
    if stat.pgid == identity.pgid || stat.sid == identity.sid {
      if stat.state == 'Z' {
        // EOS is a subreaper. An adopted descendant can be reaped by PID;
        // another parent's zombie remains an unproved group member.
        unsafe {
          libc::waitpid(pid as i32, std::ptr::null_mut(), libc::WNOHANG);
        }
      }
      drained = false;
    }
  }
  Ok(drained)
}

fn owned_descendants_drained(
  identity: ProcIdentity,
  known: &mut HashMap<u32, u64>,
  baseline_children: &HashMap<u32, u64>,
) -> std::io::Result<DrainObservation> {
  let mut snapshot = HashMap::new();
  for item in fs::read_dir("/proc")? {
    let item = item?;
    let Some(pid) = item.file_name().to_string_lossy().parse::<u32>().ok() else {
      continue;
    };
    match proc_stat(pid) {
      Ok(stat) => {
        snapshot.insert(pid, stat);
      }
      Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
      Err(error) => return Err(error),
    }
  }
  if snapshot
    .get(&identity.pid)
    .is_none_or(|stat| stat.start_ticks != identity.start_ticks)
  {
    return Err(std::io::Error::new(
      std::io::ErrorKind::InvalidData,
      "backend leader identity lost",
    ));
  }
  // The held leader pins this session and process group identity. A member
  // with either identity belongs to this backend, even after subreaper
  // adoption. Keep identities of descendants that subsequently escape both.
  known.entry(identity.pid).or_insert(identity.start_ticks);
  for (&pid, stat) in &snapshot {
    if pid != identity.pid && (stat.pgid == identity.pgid || stat.sid == identity.sid) {
      known.insert(pid, stat.start_ticks);
    }
  }
  loop {
    let mut added = false;
    for (&pid, stat) in &snapshot {
      if known.contains_key(&pid) {
        continue;
      }
      let parent = stat.ppid as u32;
      if known.get(&parent).is_some_and(|ticks| {
        snapshot
          .get(&parent)
          .is_some_and(|entry| entry.start_ticks == *ticks)
      }) {
        known.insert(pid, stat.start_ticks);
        added = true;
      }
    }
    if !added {
      break;
    }
  }
  let mut drained = true;
  for (&pid, &ticks) in known.iter() {
    if pid == identity.pid {
      continue;
    }
    let Some(stat) = snapshot.get(&pid) else {
      continue;
    };
    if stat.start_ticks != ticks {
      continue;
    }
    if stat.state == 'Z' || stat.state == 'X' {
      unsafe {
        libc::waitpid(pid as i32, std::ptr::null_mut(), libc::WNOHANG);
      }
    }
    drained = false;
  }
  // An adopted child that escaped the backend session before observation is
  // indistinguishable from an unrelated child launched after the baseline.
  // Never signal or reap it; also never confirm a complete backend stop.
  let owner_pid = unsafe { libc::getpid() };
  let ambiguous_adoption = snapshot.iter().any(|(&pid, stat)| {
    stat.ppid == owner_pid
      && pid != identity.pid
      && baseline_children.get(&pid) != Some(&stat.start_ticks)
      && known.get(&pid) != Some(&stat.start_ticks)
  });
  Ok(DrainObservation {
    owned_drained: drained && group_drained(identity)?,
    ambiguous_adoption,
  })
}

struct DrainObservation {
  owned_drained: bool,
  ambiguous_adoption: bool,
}

fn direct_children() -> std::io::Result<HashMap<u32, u64>> {
  let owner_pid = unsafe { libc::getpid() };
  let mut children = HashMap::new();
  for item in fs::read_dir("/proc")? {
    let item = item?;
    let Some(pid) = item.file_name().to_string_lossy().parse::<u32>().ok() else {
      continue;
    };
    match proc_stat(pid) {
      Ok(stat) if stat.ppid == owner_pid => {
        children.insert(pid, stat.start_ticks);
      }
      Ok(_) => {}
      Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
      Err(error) => return Err(error),
    }
  }
  Ok(children)
}

fn signal_escaped_descendants(
  identity: ProcIdentity,
  known: &HashMap<u32, u64>,
  signal: i32,
) -> bool {
  let mut found = false;
  for (&pid, &ticks) in known {
    if pid == identity.pid {
      continue;
    }
    let Ok(stat) = proc_stat(pid) else {
      continue;
    };
    if stat.start_ticks == ticks
      && stat.pgid != identity.pgid
      && stat.sid != identity.sid
      && stat.state != 'Z'
      && stat.state != 'X'
    {
      found = true;
      unsafe {
        libc::kill(pid as i32, signal);
      }
    }
  }
  found
}

impl BackendChild {
  pub fn start(
    profile: &Profile,
    launch: &BackendLaunch,
    require_stop_proof: bool,
  ) -> Result<Self, String> {
    let backend = profile
      .node(&launch.node)
      .ok_or_else(|| format!("backend node {} is missing", launch.node))?;
    if !is_eos_backend_type(&backend.node_type) {
      return Err(format!("{} is not an EOS backend", launch.node));
    }
    if profile
      .all_nodes()
      .filter(|node| is_eos_backend_type(&node.node_type))
      .count()
      != 1
    {
      return Err("exactly one EOS backend is supported".into());
    }
    let identity = if matches!(
      backend.node_type.as_str(),
      "eos.process.mujoco@v1.0" | "eos.process.mujoco@v0.1"
    ) {
      backend
        .package
        .as_ref()
        .map(|p| (p.id.as_str(), p.version.as_str()))
    } else {
      backend
        .settings
        .get("package")
        .and_then(|v| v.as_str())
        .zip(backend.settings.get("version").and_then(|v| v.as_str()))
    };
    if identity != Some(("island/mujoco-sim", "0.1.1")) {
      return Err("unsupported backend package or version".into());
    }
    for (kind, path) in [("executable", &launch.executable), ("scene", &launch.scene)] {
      if !path.is_absolute() || !path.is_file() {
        return Err(format!("backend {kind} must be an absolute regular file"));
      }
    }
    if fs::metadata(&launch.executable)
      .map_err(|error| error.to_string())?
      .permissions()
      .mode()
      & 0o111
      == 0
    {
      return Err("backend executable is not executable".into());
    }
    let socket = PathBuf::from(
      backend
        .settings
        .get("socket")
        .and_then(|value| value.as_str())
        .ok_or("backend socket is missing")?,
    );
    let devices = validate_bindings(profile, launch, &socket)?;
    let stop_socket = PathBuf::from(format!("{}.stop", socket.display()));
    if stop_socket.as_os_str().as_encoded_bytes().len() >= 108 {
      return Err("backend stop socket must fit the Unix path limit".into());
    }
    clear_stale_socket(&socket)?;
    clear_stale_socket(&stop_socket)?;
    let session = Endpoint::new(
      socket.clone(),
      "base-0".into(),
      String::new(),
      Vec::new(),
      Arc::new(SystemClock),
    )
    .session();
    let parent = socket.parent().ok_or("backend socket has no parent")?;
    if !parent.is_dir() {
      return Err(format!(
        "backend socket parent does not exist: {}",
        parent.display()
      ));
    }
    let mut command = Command::new(&launch.executable);
    command
      .arg("--managed-strict-socket")
      .arg("--v2-socket")
      .arg(&socket)
      .arg("--scene")
      .arg(&launch.scene)
      .current_dir(parent);
    if let Some(path) = &launch.log_path {
      let directory = path.parent().ok_or("backend log path has no parent")?;
      fs::create_dir_all(directory).map_err(|error| error.to_string())?;
      fs::set_permissions(directory, fs::Permissions::from_mode(0o700))
        .map_err(|error| error.to_string())?;
      fs::OpenOptions::new()
        .create(true)
        .append(true)
        .mode(0o600)
        .open(path)
        .map_err(|error| error.to_string())?;
      command.stdout(Stdio::piped()).stderr(Stdio::piped());
    } else {
      // Standalone launches must not let raw simulator diagnostics enter the
      // parent's JSONL stderr stream when no private capture was requested.
      command.stdout(Stdio::null()).stderr(Stdio::null());
    }
    // A separate session lets EOS stop any descendants the simulator spawns.
    unsafe {
      let parent_pid = libc::getpid();
      command.pre_exec(move || {
        if libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM) < 0 || libc::getppid() != parent_pid {
          return Err(std::io::Error::last_os_error());
        }
        if libc::setsid() < 0 {
          return Err(std::io::Error::last_os_error());
        }
        Ok(())
      });
    }
    if unsafe { libc::prctl(libc::PR_SET_CHILD_SUBREAPER, 1) } < 0 {
      return Err(format!(
        "backend subreaper setup failed: {}",
        std::io::Error::last_os_error()
      ));
    }
    let baseline_children = direct_children().map_err(|error| error.to_string())?;
    let mut child = command.spawn().map_err(|error| error.to_string())?;
    let pid = child.id();
    let stat = match proc_stat(pid) {
      Ok(stat) if stat.pgid == pid as i32 && stat.sid == pid as i32 => stat,
      result => {
        // pre_exec completed setsid before spawn returned. Holding the
        // unreaped leader pins its PGID while startup rollback signals it.
        unsafe {
          libc::kill(-(pid as i32), libc::SIGKILL);
        }
        let _ = child.kill();
        let _ = child.wait();
        return Err(format!(
          "backend identity unavailable after spawn: {:?}",
          result.err()
        ));
      }
    };
    let identity = ProcIdentity {
      pid,
      pgid: stat.pgid,
      sid: stat.sid,
      start_ticks: stat.start_ticks,
    };
    let mut log_workers = Vec::new();
    if let Some(path) = &launch.log_path {
      let lock = Arc::new(Mutex::new(()));
      for (level, pipe) in [
        (
          "info",
          child
            .stdout
            .take()
            .map(|p| Box::new(p) as Box<dyn Read + Send>),
        ),
        (
          "error",
          child
            .stderr
            .take()
            .map(|p| Box::new(p) as Box<dyn Read + Send>),
        ),
      ] {
        if let Some(pipe) = pipe {
          let path = path.clone();
          let lock = lock.clone();
          log_workers.push(thread::spawn(move || log_stream(pipe, &path, level, lock)));
        }
      }
    }
    let mut backend = Self {
      child,
      socket,
      stop_socket,
      session,
      ready_generation: None,
      stopped: false,
      identity: Some(identity),
      baseline_children,
      log_workers,
    };
    let timeout = backend
      .timeout(profile, &launch.node)
      .unwrap_or(Duration::from_secs(30));
    backend.wait_ready(&devices, timeout, require_stop_proof)?;
    Ok(backend)
  }

  fn timeout(&self, profile: &Profile, node: &str) -> Option<Duration> {
    let milliseconds = profile
      .node(node)?
      .settings
      .get("start_timeout_ms")?
      .as_integer()?;
    Some(Duration::from_millis(milliseconds.clamp(1, 120_000) as u64))
  }

  fn wait_ready(
    &mut self,
    devices: &[String],
    timeout: Duration,
    require_stop_proof: bool,
  ) -> Result<(), String> {
    let deadline = Instant::now() + timeout;
    let mut last_transient = None;
    loop {
      if proc_stat(self.child.id())
        .map(|stat| stat.state == 'Z' || stat.state == 'X')
        .unwrap_or(true)
      {
        return Err("backend exited before ready".into());
      }
      if self.socket.exists() {
        let mut complete = true;
        for device in devices {
          let remaining = deadline.saturating_duration_since(Instant::now());
          if remaining.is_zero() {
            complete = false;
            break;
          }
          let endpoint = Endpoint::new(
            self.socket.clone(),
            device.clone(),
            String::new(),
            Vec::new(),
            Arc::new(SystemClock),
          );
          match endpoint.strict_health(remaining.min(Duration::from_millis(500))) {
            Ok(()) => {}
            Err(error) if error.is_transient() => {
              last_transient = Some(format!("strict health for {device}: {error}"));
              complete = false;
              break;
            }
            Err(error) => return Err(format!("strict health for {device}: {error}")),
          }
        }
        if complete && Instant::now() < deadline {
          let mut generation: Option<String> = None;
          let mut stop_ready = true;
          for device in devices.iter().filter(|device| device.as_str() != "cam-0") {
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
              stop_ready = false;
              break;
            }
            let endpoint = Endpoint::new(
              self.socket.clone(),
              device.clone(),
              String::new(),
              Vec::new(),
              Arc::new(SystemClock),
            );
            match endpoint.strict_stop_health(Duration::from_millis(500).min(remaining)) {
              Ok(current) if generation.as_ref().is_none_or(|prior| prior == &current) => {
                generation = Some(current);
              }
              Ok(_) if require_stop_proof => {
                return Err(format!("strict stop generation mismatch for {device}"));
              }
              Ok(_) => {
                stop_ready = false;
                break;
              }
              Err(error) if error.is_transient() => {
                last_transient = Some(format!("strict stop health for {device}: {error}"));
                stop_ready = false;
                break;
              }
              Err(error) if require_stop_proof => {
                return Err(format!("strict stop health for {device}: {error}"));
              }
              Err(_) => {
                stop_ready = false;
                break;
              }
            }
          }
          if stop_ready {
            if let Some(generation) = generation {
              self
                .session
                .install_generation(&generation)
                .map_err(|error| error.to_string())?;
              self.ready_generation = Some(generation);
            }
            return Ok(());
          }
          if !require_stop_proof {
            return Ok(());
          }
        }
      }
      if Instant::now() >= deadline {
        return Err(format!(
          "backend socket {} did not pass strict health and stop proof before startup deadline{}",
          self.socket.display(),
          last_transient.map_or_else(String::new, |error| format!(": {error}"))
        ));
      }
      thread::sleep(
        Duration::from_millis(20).min(deadline.saturating_duration_since(Instant::now())),
      );
    }
  }

  pub fn check_alive(&mut self) -> Result<(), String> {
    match proc_stat(self.child.id()) {
      Ok(stat) if stat.state != 'Z' && stat.state != 'X' => Ok(()),
      Ok(_) => Err("backend child exited".into()),
      Err(error) => Err(format!("backend identity unavailable: {error}")),
    }
  }

  pub fn session(&self) -> Arc<ManagedSession> {
    self.session.clone()
  }

  pub fn ready_generation(&self) -> Option<&str> {
    self.ready_generation.as_deref()
  }

  pub fn stop_managed(&mut self, deadline: Instant) -> BackendStopReport {
    let pid = self.child.id();
    let mut report = BackendStopReport {
      backend_pid: pid,
      backend_pgid: pid as i32,
      group_drained: false,
      reaped: false,
      sockets_removed: false,
      forced_kill: false,
      reason: None,
    };
    if self.stopped {
      report.reason = Some("backend_reap_failed");
      return report;
    }
    self.stopped = true;
    let Some(identity) = self.identity else {
      report.reason = Some("backend_exited");
      // pre_exec setsid before a successful spawn returns. The held child
      // handle keeps that PID unavailable for reuse until the wait below.
      unsafe {
        libc::kill(-(pid as i32), libc::SIGKILL);
      }
      let _ = self.child.kill();
      report.reaped = self.child.wait().is_ok();
      self.cleanup_sockets(&mut report);
      return report;
    };
    let original_stat = proc_stat(pid).ok().filter(|stat| {
      stat.start_ticks == identity.start_ticks
        && stat.pgid == identity.pgid
        && stat.sid == identity.sid
    });
    let original = original_stat.is_some();
    if !original {
      report.reason = Some("backend_exited");
    } else {
      if original_stat.is_some_and(|stat| stat.state == 'Z' || stat.state == 'X') {
        report.reason = Some("backend_exited");
      }
      let mut known = HashMap::new();
      if owned_descendants_drained(identity, &mut known, &self.baseline_children).is_err() {
        report.reason = Some("backend_group_unreaped");
      }
      unsafe {
        libc::kill(-identity.pgid, libc::SIGTERM);
      }
      if signal_escaped_descendants(identity, &known, libc::SIGTERM) {
        report.reason = Some("backend_group_unreaped");
      }
      let now = Instant::now();
      let remaining = deadline.saturating_duration_since(now);
      let kill_reserve = Duration::from_millis(500).min(remaining / 2);
      let term_end = (now + Duration::from_secs(3)).min(deadline - kill_reserve);
      let mut safe_to_reap = false;
      while Instant::now() < term_end {
        match owned_descendants_drained(identity, &mut known, &self.baseline_children) {
          Ok(observation) if observation.owned_drained => {
            safe_to_reap = true;
            report.group_drained = !observation.ambiguous_adoption;
            if observation.ambiguous_adoption {
              report.reason = Some("backend_group_unreaped");
            }
            break;
          }
          Err(_) => {
            report.reason = Some("backend_group_unreaped");
            break;
          }
          Ok(_) => thread::sleep(Duration::from_millis(20)),
        }
      }
      if !safe_to_reap {
        // The leader is still held unreaped, so the original PGID cannot be
        // reused for an unrelated process before the final group signal.
        if proc_stat(pid).ok().is_some_and(|stat| {
          stat.start_ticks == identity.start_ticks
            && stat.pgid == identity.pgid
            && stat.sid == identity.sid
        }) {
          report.forced_kill = true;
          report.reason = Some("backend_forced_kill");
          unsafe {
            libc::kill(-identity.pgid, libc::SIGKILL);
          }
          signal_escaped_descendants(identity, &known, libc::SIGKILL);
          while Instant::now() < deadline {
            match owned_descendants_drained(identity, &mut known, &self.baseline_children) {
              Ok(observation) if observation.owned_drained => {
                safe_to_reap = true;
                report.group_drained = !observation.ambiguous_adoption;
                if observation.ambiguous_adoption && report.reason != Some("backend_forced_kill") {
                  report.reason = Some("backend_group_unreaped");
                }
                break;
              }
              Err(_) => {
                report.reason = Some("backend_group_unreaped");
                break;
              }
              Ok(_) => thread::sleep(Duration::from_millis(20)),
            }
          }
        }
      }
      if safe_to_reap {
        report.reaped = self.child.wait().is_ok();
        if !report.reaped {
          report.reason = Some("backend_reap_failed");
        }
      }
    }
    if !report.group_drained {
      report.reason.get_or_insert("backend_group_unreaped");
    }
    self.cleanup_sockets(&mut report);
    report
  }

  fn cleanup_sockets(&mut self, report: &mut BackendStopReport) {
    let remove = |path: &Path| match fs::remove_file(path) {
      Ok(()) => true,
      Err(error) if error.kind() == std::io::ErrorKind::NotFound => true,
      Err(_) => false,
    };
    let main_removed = remove(&self.socket);
    let stop_removed = remove(&self.stop_socket);
    report.sockets_removed = main_removed && stop_removed;
    if !report.sockets_removed {
      report.reason = Some("socket_cleanup_failed");
    }
    for worker in self.log_workers.drain(..) {
      if worker.is_finished() {
        let _ = worker.join();
      }
      // A stuck pipe reader cannot extend the physical stop deadline.
    }
  }
}

impl Drop for BackendChild {
  fn drop(&mut self) {
    if !self.stopped {
      let _ = self.stop_managed(Instant::now() + Duration::from_secs(2));
    }
  }
}

fn validate_bindings(
  profile: &Profile,
  launch: &BackendLaunch,
  socket: &Path,
) -> Result<Vec<String>, String> {
  let backend = profile
    .node(&launch.node)
    .ok_or("backend node is missing")?;
  let scene = backend
    .settings
    .get("scene")
    .and_then(|value| value.as_str())
    .ok_or("backend scene selector is missing")?;
  if !matches!(scene, "pick-place" | "navigation") {
    return Err(format!("unsupported backend scene selector: {scene}"));
  }
  let expected_scene_tail = if scene == "navigation" {
    Path::new("assets/scenes/navigation/scene.xml")
  } else {
    Path::new("assets/scene.xml")
  };
  if !launch.scene.ends_with(expected_scene_tail) {
    return Err(format!(
      "backend scene path disagrees with selected {scene}"
    ));
  }
  if !socket.is_absolute() || socket.as_os_str().as_encoded_bytes().len() >= 108 {
    return Err("backend socket must be an absolute Unix socket path under 108 bytes".into());
  }
  let parent = socket.parent().ok_or("backend socket has no parent")?;
  let metadata = fs::symlink_metadata(parent).map_err(|error| error.to_string())?;
  if !metadata.is_dir()
    || metadata.file_type().is_symlink()
    || metadata.permissions().mode() & 0o777 != 0o700
    || metadata.uid() != unsafe { libc::geteuid() }
  {
    return Err("backend socket parent must be a private directory".into());
  }
  let mut devices = Vec::new();
  for node in profile
    .hardware_nodes()
    .filter(|node| node.adapter.adapter_type == "mujoco-v2")
  {
    let expected = match node.kind {
      NodeKind::Camera => "cam-0",
      NodeKind::Base => "base-0",
      NodeKind::Manipulator => "arm-0",
      NodeKind::Software => return Err(format!("{} is not hardware", node.path())),
    };
    let setting = |key: &str| {
      node
        .adapter
        .settings
        .get(key)
        .and_then(|value| value.as_str())
        .ok_or_else(|| format!("{}: missing adapter setting {key}", node.path()))
    };
    let simulator_node = setting("simulator_node")?;
    if simulator_node != expected || setting("device_id")? != expected {
      return Err(format!(
        "{}: wrong MuJoCo device identity or kind",
        node.path()
      ));
    }
    if setting("backend")? != backend.path()
      || node
        .references
        .iter()
        .filter(|path| *path == backend.path())
        .count()
        != 1
    {
      return Err(format!("{}: MuJoCo backend binding disagrees", node.path()));
    }
    if Path::new(setting("socket_path")?) != socket {
      return Err(format!("{}: MuJoCo socket binding disagrees", node.path()));
    }
    if devices.iter().any(|device| device == expected) {
      return Err(format!("{}: duplicate MuJoCo device binding", node.path()));
    }
    devices.push(expected.to_owned());
  }
  if devices.is_empty() {
    return Err("backend has no declared MuJoCo devices".into());
  }
  Ok(devices)
}

fn log_stream(mut reader: Box<dyn Read + Send>, path: &Path, level: &str, lock: Arc<Mutex<()>>) {
  let mut chunk = [0u8; 4096];
  let mut line = Vec::with_capacity(8192);
  while let Ok(count) = reader.read(&mut chunk) {
    if count == 0 {
      if !line.is_empty() {
        let _ = write_log_line(&line, path, level, &lock);
      }
      break;
    }
    for piece in chunk[..count].split_inclusive(|byte| *byte == b'\n') {
      let keep = 8192usize.saturating_sub(line.len()).min(piece.len());
      line.extend_from_slice(&piece[..keep]);
      if piece.last() == Some(&b'\n') {
        if !write_log_line(&line, path, level, &lock) {
          return;
        }
        line.clear();
      }
    }
  }
}

fn write_log_line(bytes: &[u8], path: &Path, level: &str, lock: &Arc<Mutex<()>>) -> bool {
  let threshold = std::env::var("ISOS_LOG_LEVEL").unwrap_or_else(|_| "info".into());
  if level == "info" && matches!(threshold.as_str(), "warn" | "error") {
    return true;
  }
  let message = String::from_utf8_lossy(bytes);
  let message = message.trim_end_matches(['\r', '\n']);
  let mut end = message.len().min(8192);
  while !message.is_char_boundary(end) {
    end -= 1;
  }
  let timestamp = utc_timestamp();
  let entry = serde_json::json!({
      "timestamp": timestamp, "level": level, "source": "mujoco",
      "message": &message[..end]
  })
  .to_string()
    + "\n";
  let Ok(_guard) = lock.lock() else {
    return false;
  };
  if rotate_if_needed(path, entry.len()).is_err() {
    return false;
  }
  let Ok(mut file) = fs::OpenOptions::new()
    .create(true)
    .append(true)
    .mode(0o600)
    .open(path)
  else {
    return false;
  };
  file.write_all(entry.as_bytes()).is_ok()
}

fn rotate_if_needed(path: &Path, incoming: usize) -> std::io::Result<()> {
  let max_bytes = std::env::var("ISOS_LOG_MAX_BYTES")
    .ok()
    .and_then(|v| v.parse::<u64>().ok())
    .filter(|v| (65_536..=100_000_000).contains(v))
    .unwrap_or(1_048_576);
  let backups = std::env::var("ISOS_LOG_BACKUPS")
    .ok()
    .and_then(|v| v.parse::<usize>().ok())
    .filter(|v| (1..=10).contains(v))
    .unwrap_or(3);
  let old = fs::metadata(path).map(|m| m.len()).unwrap_or(0);
  if old + incoming as u64 <= max_bytes {
    return Ok(());
  }
  for index in (1..=backups).rev() {
    let source = if index == 1 {
      path.to_path_buf()
    } else {
      PathBuf::from(format!("{}.{}", path.display(), index - 1))
    };
    let target = PathBuf::from(format!("{}.{}", path.display(), index));
    if source.exists() {
      fs::rename(source, target)?;
    }
  }
  Ok(())
}

fn utc_timestamp() -> String {
  let now = std::time::SystemTime::now()
    .duration_since(std::time::UNIX_EPOCH)
    .unwrap_or_default();
  let seconds = now.as_secs() as libc::time_t;
  let mut tm = unsafe { std::mem::zeroed::<libc::tm>() };
  unsafe {
    libc::gmtime_r(&seconds, &mut tm);
  }
  format!(
    "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
    tm.tm_year + 1900,
    tm.tm_mon + 1,
    tm.tm_mday,
    tm.tm_hour,
    tm.tm_min,
    tm.tm_sec,
    now.subsec_millis()
  )
}

fn clear_stale_socket(socket: &Path) -> Result<(), String> {
  match fs::symlink_metadata(socket) {
    Ok(metadata) if !metadata.file_type().is_socket() => Err(format!(
      "backend socket path is not a Unix socket: {}",
      socket.display()
    )),
    Ok(_) if UnixStream::connect(socket).is_ok() => Err(format!(
      "backend socket is already served: {}",
      socket.display()
    )),
    Ok(_) => fs::remove_file(socket).map_err(|error| error.to_string()),
    Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
    Err(error) => Err(error.to_string()),
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  static CHILD_TEST_LOCK: Mutex<()> = Mutex::new(());

  const GRAPH: &str = include_str!("../../../../../examples/v2/mujoco-isos.toml");
  const NODE: &str = "cores/eos/layers/process/groups/backend/nodes/simulator";
  const STRICT_SCRIPT: &str = r#"#!/usr/bin/python3
import json, socket, struct, sys, time
assert sys.argv[1] == '--managed-strict-socket'
assert sys.argv[2] == '--v2-socket'
assert sys.argv[4] == '--scene'
assert len(sys.argv) == 6
server = socket.socket(socket.AF_UNIX)
server.bind(sys.argv[3])
server.listen(4)
for expected in ['base-0', 'cam-0', 'arm-0']:
    conn, _ = server.accept()
    with conn:
        lengths = conn.recv(8)
        metadata_len, payload_len = struct.unpack('<II', lengths)
        assert payload_len == 0
        data = b''
        while len(data) < metadata_len:
            data += conn.recv(metadata_len-len(data))
        request = json.loads(data)
        assert request['op'] == 'health'
        assert request['node'] == expected
        reply = json.dumps({'version':'2.0.0','id':request['id'],'ok':True,
                            'accepted':True,'features':['strict-socket-v1']}).encode()
        conn.sendall(struct.pack('<II',len(reply),0)+reply)
while True: time.sleep(0.1)
"#;

  fn directory() -> PathBuf {
    let path = std::env::temp_dir().join(format!(
      "aether-backend-child-{}-{}",
      std::process::id(),
      std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos()
    ));
    fs::create_dir(&path).unwrap();
    fs::set_permissions(&path, fs::Permissions::from_mode(0o700)).unwrap();
    path
  }

  fn fixture(script: &str) -> (PathBuf, Profile, BackendLaunch, PathBuf) {
    let dir = directory();
    let socket = dir.join("sim.sock");
    let source = GRAPH
      .replace("/tmp/mujoco-v2.sock", socket.to_str().unwrap())
      .replace("scene = \"default\"", "scene = \"pick-place\"");
    let profile = Profile::from_toml(&source).unwrap();
    let executable = dir.join("sim.py");
    fs::write(&executable, script).unwrap();
    fs::set_permissions(&executable, fs::Permissions::from_mode(0o700)).unwrap();
    let scene = dir.join("assets/scene.xml");
    fs::create_dir_all(scene.parent().unwrap()).unwrap();
    fs::write(&scene, "<mujoco/>").unwrap();
    let launch = BackendLaunch {
      node: NODE.into(),
      executable,
      scene,
      log_path: None,
    };
    (dir, profile, launch, socket)
  }

  #[test]
  fn starts_and_reaps_direct_child_after_strict_health() {
    let _child_test_guard = CHILD_TEST_LOCK
      .lock()
      .unwrap_or_else(|poison| poison.into_inner());
    let (dir, profile, launch, socket) = fixture(STRICT_SCRIPT);
    let mut child = BackendChild::start(&profile, &launch, false).unwrap();
    assert!(socket.exists());
    child.check_alive().unwrap();
    let pid = child.child.id();
    drop(child);
    assert!(!socket.exists());
    assert_eq!(unsafe { libc::kill(pid as libc::pid_t, 0) }, -1);
    fs::remove_dir_all(dir).unwrap();
  }

  #[test]
  fn bind_before_listen_and_initial_timeout_retry_until_strict_health() {
    let _child_test_guard = CHILD_TEST_LOCK
      .lock()
      .unwrap_or_else(|poison| poison.into_inner());
    for (script, minimum) in [
      (
        STRICT_SCRIPT.replace("server.listen(4)", "time.sleep(0.10)\nserver.listen(4)"),
        Duration::from_millis(80),
      ),
      (
        STRICT_SCRIPT.replace(
          "for expected in ['base-0', 'cam-0', 'arm-0']:",
          "conn,_=server.accept()\ntime.sleep(0.60)\nconn.close()\nfor expected in ['base-0', 'cam-0', 'arm-0']:",
        ),
        Duration::from_millis(500),
      ),
    ] {
      let (dir, profile, launch, socket) = fixture(&script);
      let started = Instant::now();
      let mut child = BackendChild::start(&profile, &launch, false).unwrap();
      assert!(started.elapsed() >= minimum);
      child.check_alive().unwrap();
      drop(child);
      assert!(!socket.exists());
      fs::remove_dir_all(dir).unwrap();
    }
  }

  #[test]
  fn child_output_is_structured_and_kept_in_mujoco_log() {
    let _child_test_guard = CHILD_TEST_LOCK
      .lock()
      .unwrap_or_else(|poison| poison.into_inner());
    let script = STRICT_SCRIPT.replace("server = socket.socket", "print('simulator ready', flush=True)\nprint('simulator warning', file=sys.stderr, flush=True)\nserver = socket.socket");
    let (dir, profile, mut launch, _) = fixture(&script);
    let log = dir.join("logs/mujoco.log");
    launch.log_path = Some(log.clone());
    let child = BackendChild::start(&profile, &launch, false).unwrap();
    drop(child);
    let lines = fs::read_to_string(&log).unwrap();
    let entries: Vec<serde_json::Value> = lines
      .lines()
      .map(|line| serde_json::from_str(line).unwrap())
      .collect();
    assert!(entries
      .iter()
      .any(|entry| entry["level"] == "info" && entry["message"] == "simulator ready"));
    assert!(entries
      .iter()
      .any(|entry| entry["level"] == "error" && entry["message"] == "simulator warning"));
    assert!(entries.iter().all(
      |entry| entry["source"] == "mujoco" && entry["timestamp"].as_str().unwrap().ends_with('Z')
    ));
    let _ = fs::remove_dir_all(dir);
  }

  #[test]
  fn long_unterminated_child_line_is_drained_and_following_line_is_captured() {
    let _child_test_guard = CHILD_TEST_LOCK
      .lock()
      .unwrap_or_else(|poison| poison.into_inner());
    let dir = std::env::temp_dir().join(format!(
      "aether-backend-log-bound-{}-{}",
      std::process::id(),
      std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos()
    ));
    fs::create_dir_all(&dir).unwrap();
    let log = dir.join("mujoco.log");
    let mut output = vec![b'X'; 2_000_000];
    output.extend_from_slice(b"\nafter\n");
    log_stream(
      Box::new(std::io::Cursor::new(output)),
      &log,
      "error",
      Arc::new(Mutex::new(())),
    );
    let entries: Vec<serde_json::Value> = fs::read_to_string(&log)
      .unwrap()
      .lines()
      .map(|line| serde_json::from_str(line).unwrap())
      .collect();
    assert_eq!(entries.len(), 2);
    assert_eq!(entries[0]["message"].as_str().unwrap().len(), 8192);
    assert_eq!(entries[1]["message"], "after");
    fs::remove_dir_all(dir).unwrap();
  }

  #[test]
  fn startup_failure_reaps_child_and_rejects_wrong_identity() {
    let _child_test_guard = CHILD_TEST_LOCK
      .lock()
      .unwrap_or_else(|poison| poison.into_inner());
    let (dir, profile, mut launch, socket) = fixture("#!/bin/sh\nexit 7\n");
    assert!(BackendChild::start(&profile, &launch, false).is_err());
    assert!(!socket.exists());
    launch.node = "cores/isos/layers/process/groups/component/nodes/eos".into();
    assert!(BackendChild::start(&profile, &launch, false).is_err());
    fs::remove_dir_all(dir).unwrap();
  }

  #[test]
  fn pre_ready_leader_exit_drains_its_live_descendant() {
    let _child_test_guard = CHILD_TEST_LOCK
      .lock()
      .unwrap_or_else(|poison| poison.into_inner());
    let script = "#!/usr/bin/python3\nimport os,sys,time\npid=os.fork()\nif pid==0:\n time.sleep(10)\nelse:\n open(sys.argv[0]+'.descendant','w').write(str(pid))\n os._exit(7)\n";
    let (dir, profile, launch, socket) = fixture(script);
    assert!(BackendChild::start(&profile, &launch, false).is_err());
    let descendant: i32 = fs::read_to_string(format!("{}.descendant", launch.executable.display()))
      .unwrap()
      .parse()
      .unwrap();
    assert_eq!(unsafe { libc::kill(descendant, 0) }, -1);
    assert!(!socket.exists());
    fs::remove_dir_all(dir).unwrap();
  }

  #[test]
  fn profile_binding_failures_precede_child_spawn() {
    let _child_test_guard = CHILD_TEST_LOCK
      .lock()
      .unwrap_or_else(|poison| poison.into_inner());
    let (dir, profile, launch, _) = fixture(STRICT_SCRIPT);
    let backend = profile.node(&launch.node).unwrap();
    assert_eq!(
      backend.settings.get("scene").and_then(|v| v.as_str()),
      Some("pick-place")
    );
    let socket = PathBuf::from(backend.settings["socket"].as_str().unwrap());
    for (old, new) in [
      ("scene = \"pick-place\"", "scene = \"unknown\""),
      ("version = \"0.1.1\"", "version = \"0.1.2\""),
      ("simulator_node = \"base-0\"", "simulator_node = \"cam-0\""),
      ("device_id = \"base-0\"", "device_id = \"arm-0\""),
    ] {
      let source = GRAPH
        .replace("/tmp/mujoco-v2.sock", socket.to_str().unwrap())
        .replace("\nscene = \"default\"\n", "\nscene = \"pick-place\"\n")
        .replace(old, new);
      assert!(source.contains(new), "mutation missing: {old}");
      if let Ok(profile) = Profile::from_toml(&source) {
        assert!(
          BackendChild::start(&profile, &launch, false).is_err(),
          "{old}"
        );
      }
      assert!(!launch.executable.with_extension("py.pid").exists());
    }
    fs::remove_dir_all(dir).unwrap();
  }

  #[test]
  fn forged_or_legacy_health_never_makes_backend_ready() {
    let _child_test_guard = CHILD_TEST_LOCK
      .lock()
      .unwrap_or_else(|poison| poison.into_inner());
    let invalid = [
      "{'ok':True,'accepted':True}",
      "{'ok':True,'accepted':True,'features':'strict-socket-v1'}",
      "{'ok':True,'accepted':True,'features':['strict-socket-v2']}",
      "{'ok':True,'accepted':False,'features':['strict-socket-v1']}",
      "{'ok':False,'accepted':True,'features':['strict-socket-v1']}",
      "{'ok':True,'accepted':True,'features':['strict-socket-v1'],'version':'2.1.0'}",
      "{'ok':True,'accepted':True,'features':['strict-socket-v1'],'id':'wrong'}",
    ];
    for fields in invalid {
      let script = format!(
        "#!/usr/bin/python3\nimport json, os, socket, struct, sys, time\n\
         open(sys.argv[0]+'.pid','w').write(str(os.getpid()))\n\
         server=socket.socket(socket.AF_UNIX)\nserver.bind(sys.argv[3])\nserver.listen(1)\n\
         conn,_=server.accept()\nheader=conn.recv(8)\nn,_=struct.unpack('<II',header)\n\
         data=b''\nwhile len(data)<n: data+=conn.recv(n-len(data))\n\
         request=json.loads(data)\nreply={{'version':'2.0.0','id':request['id'],**{fields}}}\n\
         body=json.dumps(reply).encode()\nconn.sendall(struct.pack('<II',len(body),0)+body)\n\
         while True: time.sleep(0.1)\n"
      );
      let (dir, profile, launch, socket) = fixture(&script);
      let error = BackendChild::start(&profile, &launch, false).err().unwrap();
      assert!(error.contains("strict health"), "{error}");
      let pid: libc::pid_t = fs::read_to_string(format!("{}.pid", launch.executable.display()))
        .unwrap()
        .parse()
        .unwrap();
      assert_eq!(
        unsafe { libc::kill(pid, 0) },
        -1,
        "unreaped child for {fields}"
      );
      assert!(!socket.exists());
      fs::remove_dir_all(dir).unwrap();
    }
  }

  #[test]
  fn connect_only_and_malformed_health_fail_bounded_and_reap() {
    let _child_test_guard = CHILD_TEST_LOCK
      .lock()
      .unwrap_or_else(|poison| poison.into_inner());
    for response in ["timeout", "malformed", "drip"] {
      let script = r#"#!/usr/bin/python3
import os, socket, struct, sys, time
open(sys.argv[0]+'.pid','w').write(str(os.getpid()))
server=socket.socket(socket.AF_UNIX)
server.bind(sys.argv[3])
server.listen(1)
conn,_=server.accept()
if 'RESPONSE' == 'malformed':
    header=conn.recv(8)
    size,_=struct.unpack('<II',header)
    conn.recv(size)
    conn.sendall(struct.pack('<II',1,0)+b'{')
elif 'RESPONSE' == 'drip':
    for byte in struct.pack('<II',64,0):
        try: conn.sendall(bytes([byte]))
        except BrokenPipeError: break
        time.sleep(0.04)
while True: time.sleep(0.1)
"#
      .replace("RESPONSE", response);
      let (dir, _, launch, socket) = fixture(&script);
      let source = GRAPH
        .replace("/tmp/mujoco-v2.sock", socket.to_str().unwrap())
        .replace(
          "\nscene = \"default\"\nsocket =",
          "\nscene = \"pick-place\"\nstart_timeout_ms = 150\nsocket =",
        );
      let profile = Profile::from_toml(&source).unwrap();
      let started = Instant::now();
      assert!(BackendChild::start(&profile, &launch, false).is_err());
      assert!(started.elapsed() < Duration::from_secs(1), "{response}");
      let pid: libc::pid_t = fs::read_to_string(format!("{}.pid", launch.executable.display()))
        .unwrap()
        .parse()
        .unwrap();
      assert_eq!(unsafe { libc::kill(pid, 0) }, -1);
      assert!(!socket.exists());
      fs::remove_dir_all(dir).unwrap();
    }
  }

  #[test]
  fn real_strict_child_proves_both_catalog_scenes_when_selected() {
    let _child_test_guard = CHILD_TEST_LOCK
      .lock()
      .unwrap_or_else(|poison| poison.into_inner());
    let Ok(executable) = std::env::var("MUJOCO_R05_EXECUTABLE") else {
      return;
    };
    let root = PathBuf::from(std::env::var("MUJOCO_R05_PACKAGE_ROOT").unwrap());
    for (scene_id, scene_relative) in [
      ("pick-place", "assets/scene.xml"),
      ("navigation", "assets/scenes/navigation/scene.xml"),
    ] {
      let (dir, profile, mut launch, socket) = fixture(STRICT_SCRIPT);
      let profile = if scene_id == "pick-place" {
        profile
      } else {
        let source = GRAPH
          .replace("/tmp/mujoco-v2.sock", socket.to_str().unwrap())
          .replace("scene = \"default\"", "scene = \"navigation\"");
        Profile::from_toml(&source).unwrap()
      };
      launch.executable = PathBuf::from(&executable);
      launch.scene = root.join(scene_relative);
      let mut child = BackendChild::start(&profile, &launch, false).unwrap();
      child.check_alive().unwrap();
      drop(child);
      assert!(!socket.exists(), "{scene_id} socket remains after reap");
      fs::remove_dir_all(dir).unwrap();
    }
  }

  #[test]
  fn leader_exit_does_not_hide_live_descendant_from_final_group_scan() {
    let _child_test_guard = CHILD_TEST_LOCK
      .lock()
      .unwrap_or_else(|poison| poison.into_inner());
    unsafe {
      libc::prctl(libc::PR_SET_CHILD_SUBREAPER, 1);
    }
    let directory = directory();
    let mut command = Command::new("/bin/sh");
    command
      .arg("-c")
      .arg("sleep 10 & exit 0")
      .stdout(Stdio::null())
      .stderr(Stdio::null());
    unsafe {
      command.pre_exec(|| {
        if libc::setsid() < 0 {
          Err(std::io::Error::last_os_error())
        } else {
          Ok(())
        }
      });
    }
    let child = command.spawn().unwrap();
    let pid = child.id();
    let stat = proc_stat(pid).unwrap();
    let identity = ProcIdentity {
      pid,
      pgid: stat.pgid,
      sid: stat.sid,
      start_ticks: stat.start_ticks,
    };
    std::thread::sleep(Duration::from_millis(40));
    assert!(
      !group_drained(identity).unwrap(),
      "live descendant must remain visible after leader exits"
    );
    let socket = directory.join("sim.sock");
    let session = Endpoint::new(
      socket.clone(),
      "base-0".into(),
      String::new(),
      Vec::new(),
      Arc::new(SystemClock),
    )
    .session();
    let mut backend = BackendChild {
      child,
      socket: socket.clone(),
      stop_socket: directory.join("sim.sock.stop"),
      session,
      ready_generation: None,
      stopped: false,
      identity: Some(identity),
      baseline_children: HashMap::new(),
      log_workers: Vec::new(),
    };
    let report = backend.stop_managed(Instant::now() + Duration::from_secs(2));
    assert!(
      report.group_drained && report.reaped,
      "group did not drain/reap"
    );
    fs::remove_dir_all(directory).unwrap();
  }

  #[test]
  fn term_resistant_leader_needs_kill_and_can_never_confirm() {
    let _child_test_guard = CHILD_TEST_LOCK
      .lock()
      .unwrap_or_else(|poison| poison.into_inner());
    let directory = directory();
    let mut command = Command::new("/bin/sh");
    command
      .arg("-c")
      .arg("trap '' TERM; while :; do sleep 1; done")
      .stdout(Stdio::null())
      .stderr(Stdio::null());
    unsafe {
      command.pre_exec(|| {
        if libc::setsid() < 0 {
          Err(std::io::Error::last_os_error())
        } else {
          Ok(())
        }
      });
    }
    let child = command.spawn().unwrap();
    let pid = child.id();
    let stat = proc_stat(pid).unwrap();
    let identity = ProcIdentity {
      pid,
      pgid: stat.pgid,
      sid: stat.sid,
      start_ticks: stat.start_ticks,
    };
    std::thread::sleep(Duration::from_millis(40));
    let socket = directory.join("sim.sock");
    let session = Endpoint::new(
      socket.clone(),
      "base-0".into(),
      String::new(),
      Vec::new(),
      Arc::new(SystemClock),
    )
    .session();
    let mut backend = BackendChild {
      child,
      socket,
      stop_socket: directory.join("sim.sock.stop"),
      session,
      ready_generation: None,
      stopped: false,
      identity: Some(identity),
      baseline_children: HashMap::new(),
      log_workers: Vec::new(),
    };
    let report = backend.stop_managed(Instant::now() + Duration::from_millis(400));
    assert!(report.forced_kill);
    assert_eq!(report.reason, Some("backend_forced_kill"));
    assert!(report.reaped);
    fs::remove_dir_all(directory).unwrap();
  }

  #[test]
  fn escaped_adopted_descendant_cannot_confirm_group_drain() {
    let _child_test_guard = CHILD_TEST_LOCK
      .lock()
      .unwrap_or_else(|poison| poison.into_inner());
    let directory = directory();
    unsafe {
      libc::prctl(libc::PR_SET_CHILD_SUBREAPER, 1);
    }
    let baseline_children = direct_children().unwrap();
    let pid_file = directory.join("escaped.pid");
    let mut command = Command::new("/usr/bin/python3");
    command
      .arg("-c")
      .arg("import os,sys,time\npid=os.fork()\nif pid==0:\n os.setsid()\n open(sys.argv[1],'w').write(str(os.getpid()))\n time.sleep(10)\nelse:\n time.sleep(0.05)")
      .arg(&pid_file)
      .stdout(Stdio::null())
      .stderr(Stdio::null());
    unsafe {
      command.pre_exec(|| {
        if libc::setsid() < 0 {
          Err(std::io::Error::last_os_error())
        } else {
          Ok(())
        }
      });
    }
    let child = command.spawn().unwrap();
    let pid = child.id();
    let stat = proc_stat(pid).unwrap();
    let identity = ProcIdentity {
      pid,
      pgid: stat.pgid,
      sid: stat.sid,
      start_ticks: stat.start_ticks,
    };
    for _ in 0..100 {
      if pid_file.exists() {
        break;
      }
      thread::sleep(Duration::from_millis(10));
    }
    let escaped: i32 = fs::read_to_string(&pid_file).unwrap().parse().unwrap();
    for _ in 0..100 {
      if proc_stat(pid).unwrap().state == 'Z'
        && proc_stat(escaped as u32).unwrap().ppid == unsafe { libc::getpid() }
      {
        break;
      }
      thread::sleep(Duration::from_millis(10));
    }
    assert_eq!(proc_stat(pid).unwrap().state, 'Z');
    assert_eq!(proc_stat(escaped as u32).unwrap().ppid, unsafe {
      libc::getpid()
    });
    let mut known = HashMap::new();
    let observation = owned_descendants_drained(identity, &mut known, &baseline_children).unwrap();
    assert!(observation.owned_drained && observation.ambiguous_adoption);
    assert!(
      !known.contains_key(&(escaped as u32)),
      "an unobserved escaped child has no proven backend identity"
    );
    let socket = directory.join("sim.sock");
    let session = Endpoint::new(
      socket.clone(),
      "base-0".into(),
      String::new(),
      Vec::new(),
      Arc::new(SystemClock),
    )
    .session();
    let mut backend = BackendChild {
      child,
      socket,
      stop_socket: directory.join("sim.sock.stop"),
      session,
      ready_generation: None,
      stopped: false,
      identity: Some(identity),
      baseline_children,
      log_workers: Vec::new(),
    };
    let report = backend.stop_managed(Instant::now() + Duration::from_millis(400));
    assert!(
      !report.group_drained && report.reason.is_some(),
      "unobserved escaped descendant must keep stop unconfirmed"
    );
    assert_eq!(report.reason, Some("backend_group_unreaped"));
    assert!(report.reaped);
    assert!(!report.forced_kill);
    assert_eq!(unsafe { libc::kill(escaped, 0) }, 0);
    unsafe {
      libc::kill(escaped, libc::SIGKILL);
      libc::waitpid(escaped, std::ptr::null_mut(), 0);
      libc::waitpid(pid as i32, std::ptr::null_mut(), 0);
    }
    fs::remove_dir_all(directory).unwrap();
  }

  #[test]
  fn unrelated_post_baseline_child_is_neither_signalled_nor_reaped() {
    let _child_test_guard = CHILD_TEST_LOCK
      .lock()
      .unwrap_or_else(|poison| poison.into_inner());
    unsafe {
      libc::prctl(libc::PR_SET_CHILD_SUBREAPER, 1);
    }
    let directory = directory();
    let baseline_children = direct_children().unwrap();
    let mut backend_command = Command::new("/bin/sleep");
    backend_command.arg("10");
    unsafe {
      backend_command.pre_exec(|| {
        if libc::setsid() < 0 {
          Err(std::io::Error::last_os_error())
        } else {
          Ok(())
        }
      });
    }
    let child = backend_command.spawn().unwrap();
    let pid = child.id();
    let stat = proc_stat(pid).unwrap();
    let identity = ProcIdentity {
      pid,
      pgid: stat.pgid,
      sid: stat.sid,
      start_ticks: stat.start_ticks,
    };
    let mut unrelated_command = Command::new("/bin/sleep");
    unrelated_command.arg("10");
    unsafe {
      unrelated_command.pre_exec(|| {
        if libc::setsid() < 0 {
          Err(std::io::Error::last_os_error())
        } else {
          Ok(())
        }
      });
    }
    let mut unrelated = unrelated_command.spawn().unwrap();
    let unrelated_pid = unrelated.id();
    assert_ne!(proc_stat(unrelated_pid).unwrap().sid, identity.sid);
    let socket = directory.join("sim.sock");
    let session = Endpoint::new(
      socket.clone(),
      "base-0".into(),
      String::new(),
      Vec::new(),
      Arc::new(SystemClock),
    )
    .session();
    let mut backend = BackendChild {
      child,
      socket,
      stop_socket: directory.join("sim.sock.stop"),
      session,
      ready_generation: None,
      stopped: false,
      identity: Some(identity),
      baseline_children,
      log_workers: Vec::new(),
    };
    let report = backend.stop_managed(Instant::now() + Duration::from_millis(400));
    assert!(
      !report.group_drained,
      "ambiguous adoption must keep stop unconfirmed"
    );
    assert_eq!(report.reason, Some("backend_group_unreaped"));
    assert!(report.reaped);
    assert!(!report.forced_kill);
    assert_eq!(unsafe { libc::kill(unrelated_pid as i32, 0) }, 0);
    assert!(
      unrelated.try_wait().unwrap().is_none(),
      "unrelated child was reaped"
    );
    unrelated.kill().unwrap();
    unrelated.wait().unwrap();
    fs::remove_dir_all(directory).unwrap();
  }

  #[test]
  fn observed_escaped_descendant_is_signalled_by_identity() {
    let _child_test_guard = CHILD_TEST_LOCK
      .lock()
      .unwrap_or_else(|poison| poison.into_inner());
    unsafe {
      libc::prctl(libc::PR_SET_CHILD_SUBREAPER, 1);
    }
    let directory = directory();
    let baseline_children = direct_children().unwrap();
    let pid_file = directory.join("escaped.pid");
    let mut command = Command::new("/usr/bin/python3");
    command
      .arg("-c")
      .arg("import os,sys,time\npid=os.fork()\nif pid==0:\n os.setsid()\n open(sys.argv[1],'w').write(str(os.getpid()))\n time.sleep(10)\nelse:\n time.sleep(10)")
      .arg(&pid_file)
      .stdout(Stdio::null())
      .stderr(Stdio::null());
    unsafe {
      command.pre_exec(|| {
        if libc::setsid() < 0 {
          Err(std::io::Error::last_os_error())
        } else {
          Ok(())
        }
      });
    }
    let child = command.spawn().unwrap();
    let pid = child.id();
    let stat = proc_stat(pid).unwrap();
    let identity = ProcIdentity {
      pid,
      pgid: stat.pgid,
      sid: stat.sid,
      start_ticks: stat.start_ticks,
    };
    for _ in 0..100 {
      if pid_file.exists() {
        break;
      }
      thread::sleep(Duration::from_millis(10));
    }
    let escaped: i32 = fs::read_to_string(&pid_file).unwrap().parse().unwrap();
    assert_eq!(proc_stat(escaped as u32).unwrap().ppid, pid as i32);
    let socket = directory.join("sim.sock");
    let session = Endpoint::new(
      socket.clone(),
      "base-0".into(),
      String::new(),
      Vec::new(),
      Arc::new(SystemClock),
    )
    .session();
    let mut backend = BackendChild {
      child,
      socket,
      stop_socket: directory.join("sim.sock.stop"),
      session,
      ready_generation: None,
      stopped: false,
      identity: Some(identity),
      baseline_children,
      log_workers: Vec::new(),
    };
    let report = backend.stop_managed(Instant::now() + Duration::from_secs(2));
    assert!(report.reaped, "backend leader must be reaped");
    assert!(
      proc_stat(escaped as u32).is_err(),
      "proven escaped child survived"
    );
    fs::remove_dir_all(directory).unwrap();
  }
}
