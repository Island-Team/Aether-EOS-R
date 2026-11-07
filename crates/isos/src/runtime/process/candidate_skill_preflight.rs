//! Closed private EOS catalog probe before a managed replacement stops its old run.
use crate::{
  app::config::Paths, domain::graph::graph_compat, runtime::process,
  runtime::process::skill_selection,
};
use anyhow::{Context, Result, ensure};
use serde::Deserialize;
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::{
  collections::{BTreeMap, BTreeSet},
  ffi::OsString,
  fs,
  io::{self, Read},
  os::{
    fd::AsRawFd,
    unix::process::{CommandExt, ExitStatusExt},
  },
  path::{Path, PathBuf},
  process::{Child, Command, ExitStatus, Stdio},
  time::{Duration, Instant},
};

const LIMIT: usize = 16 * 1024;
const DEADLINE: Duration = Duration::from_secs(90);

#[derive(Debug, Clone)]
pub struct Diagnostic {
  pub code: &'static str,
  pub stage: &'static str,
  pub message: &'static str,
  pub eos_stage: Option<&'static str>,
  pub eos_code: Option<&'static str>,
  pub meta: Box<SafeMeta>,
}

#[derive(Debug, Clone)]
pub struct SafeMeta {
  pub run_id: String,
  pub revision: String,
  pub generation: String,
  pub package_id: Option<String>,
  pub package_version: Option<String>,
}

impl std::fmt::Display for Diagnostic {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(f, "{}: repair selected skill catalog and retry", self.code)
  }
}
impl std::error::Error for Diagnostic {}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Inputs {
  executable: PathBuf,
  executable_sha256: String,
  profile: PathBuf,
  profile_sha256: String,
  selection: PathBuf,
  selection_sha256: String,
  generation: String,
  revision: String,
  libraries: Vec<skill_selection::SelectedLibrary>,
  node_types: Vec<String>,
  specs_sha256: String,
  selectors: BTreeMap<String, String>,
}

const ASSET_SELECTORS: [&str; 2] = ["ISOS_GPU", "ISOS_ROS"];

fn effective_selectors(
  spec: &process::ProcessSpec,
  secrets: &BTreeMap<String, OsString>,
  secret_sources: &BTreeSet<String>,
) -> BTreeMap<String, String> {
  ASSET_SELECTORS
    .iter()
    .filter_map(|name| {
      // Match Supervisor::spawn: inherited values, authored env, source removal,
      // then the already resolved env_from values.
      let value = if let Some(secret) = secrets.get(*name) {
        secret.to_str().map(str::to_owned)
      } else if secret_sources.contains(*name) {
        None
      } else {
        spec
          .settings
          .env
          .get(*name)
          .cloned()
          .or_else(|| std::env::var(name).ok())
      };
      value.map(|value| ((*name).to_owned(), value))
    })
    .collect()
}

fn all_secret_sources(specs: &[process::ProcessSpec]) -> BTreeSet<String> {
  specs
    .iter()
    .flat_map(|spec| spec.settings.env_from.values().cloned())
    .collect()
}

fn sha(bytes: &[u8]) -> String {
  hex::encode(Sha256::digest(bytes))
}

fn required_option(command: &[String], flag: &str) -> Result<String> {
  let mut matches = command.iter().enumerate().filter(|(_, arg)| *arg == flag);
  let index = matches.next().context("selected EOS option missing")?.0;
  ensure!(matches.next().is_none(), "duplicate selected EOS option");
  let value = command
    .get(index + 1)
    .context("selected EOS option value missing")?;
  ensure!(!value.starts_with('-'), "invalid selected EOS option value");
  Ok(value.clone())
}

impl Inputs {
  #[cfg(test)]
  pub(crate) fn rejection_stub(executable: PathBuf) -> Self {
    Self {
      executable,
      executable_sha256: String::new(),
      profile: PathBuf::from("/tmp/rejection-profile"),
      profile_sha256: String::new(),
      selection: PathBuf::from("/tmp/rejection-selection"),
      selection_sha256: "a".repeat(64),
      generation: "b".repeat(64),
      revision: "c".repeat(64),
      libraries: vec![skill_selection::SelectedLibrary {
        package_id: "test/verified".into(),
        package_version: "0.1.0".into(),
        package_root: String::new(),
        payload_root: String::new(),
        receipt_sha256: String::new(),
        release_sha256: String::new(),
        payload_sha256: String::new(),
        library: "lib/x.skill.so".into(),
        library_sha256: "d".repeat(64),
      }],
      node_types: Vec::new(),
      specs_sha256: String::new(),
      selectors: BTreeMap::new(),
    }
  }
  pub fn collect(
    paths: &Paths,
    name: &str,
    revision: &str,
    specs: &[process::ProcessSpec],
    plan: &crate::domain::graph::generated_v2::SecretPlan,
  ) -> Result<Option<Self>> {
    let spec = specs
      .iter()
      .find(|spec| spec.id == "eos" && spec.settings.role.as_deref() == Some("eos"));
    let Some(spec) = spec else { return Ok(None) };
    let Some(selected) = spec.settings.env.get(skill_selection::ENV) else {
      return Ok(None);
    };
    let saved_bytes = fs::read(paths.node_graph_path(name))?;
    ensure!(
      graph_compat::revision(&saved_bytes) == revision,
      "saved graph revision changed"
    );
    let graph: Value = serde_json::from_slice(&saved_bytes)?;
    let root = crate::domain::graph::generated_v2::selected_v2_root(paths, name, &graph, revision)?;
    let actual = skill_selection::compare_selected(paths, &graph, revision, &root)?;
    ensure!(
      actual.as_deref() == Some(Path::new(selected)),
      "selected skill handoff changed"
    );
    let selection_bytes = fs::read(selected)?;
    let selected_inventory: skill_selection::Selection = serde_json::from_slice(&selection_bytes)?;
    ensure!(
      selected_inventory.schema_version == 1
        && selected_inventory.active_revision == revision
        && !selected_inventory.libraries.is_empty(),
      "invalid selected skill inventory"
    );
    let profile = PathBuf::from(required_option(&spec.settings.command, "--profile")?);
    ensure!(
      profile == root.join("eos/graph.json"),
      "selected EOS profile changed"
    );
    let executable = PathBuf::from(&spec.settings.command[0]);
    ensure!(
      executable.is_absolute(),
      "selected EOS executable is not absolute"
    );
    let generation = root
      .file_name()
      .context("selected generation missing")?
      .to_string_lossy()
      .into_owned();
    ensure!(
      generation.len() == 64
        && generation
          .bytes()
          .all(|b| b.is_ascii_hexdigit() && !b.is_ascii_uppercase()),
      "selected generation invalid"
    );
    let mut node_types = Vec::new();
    for (index, argument) in spec.settings.command.iter().enumerate() {
      if argument == "--node-types" {
        let value = spec
          .settings
          .command
          .get(index + 1)
          .context("selected node types value missing")?;
        ensure!(!value.starts_with('-'), "invalid selected node types value");
        node_types.extend([argument.clone(), value.clone()]);
      }
    }
    let empty = BTreeMap::new();
    let selectors = effective_selectors(
      spec,
      plan.get("eos").unwrap_or(&empty),
      &all_secret_sources(specs),
    );
    Ok(Some(Self {
      executable_sha256: sha(&fs::read(&executable)?),
      profile_sha256: sha(&fs::read(&profile)?),
      selection_sha256: sha(&selection_bytes),
      executable,
      profile,
      selection: PathBuf::from(selected),
      generation,
      revision: revision.to_owned(),
      libraries: selected_inventory.libraries,
      node_types,
      specs_sha256: sha(format!("{specs:?}").as_bytes()),
      selectors,
    }))
  }

  pub fn recheck(
    &self,
    paths: &Paths,
    name: &str,
    specs: &[process::ProcessSpec],
    plan: &crate::domain::graph::generated_v2::SecretPlan,
  ) -> Result<()> {
    self.launch_recheck(paths, name)?;
    let spec = specs
      .iter()
      .find(|spec| spec.id == "eos")
      .context("selected EOS process missing")?;
    let empty = BTreeMap::new();
    ensure!(
      effective_selectors(
        spec,
        plan.get("eos").unwrap_or(&empty),
        &all_secret_sources(specs)
      ) == self.selectors,
      "selected EOS asset selectors changed"
    );
    ensure!(
      sha(format!("{specs:?}").as_bytes()) == self.specs_sha256,
      "selected process specs changed"
    );
    Ok(())
  }

  pub fn launch_recheck(&self, paths: &Paths, name: &str) -> Result<()> {
    let saved_bytes = fs::read(paths.node_graph_path(name))?;
    ensure!(
      graph_compat::revision(&saved_bytes) == self.revision,
      "saved graph revision changed"
    );
    let graph: Value = serde_json::from_slice(&saved_bytes)?;
    let root =
      crate::domain::graph::generated_v2::selected_v2_root(paths, name, &graph, &self.revision)?;
    ensure!(
      root
        .file_name()
        .is_some_and(|name| name == self.generation.as_str()),
      "selected generation changed"
    );
    let fresh = skill_selection::compare_selected(paths, &graph, &self.revision, &root)?;
    ensure!(
      fresh.as_deref() == Some(self.selection.as_path()),
      "selected skill file changed"
    );
    ensure!(
      sha(&fs::read(&self.executable)?) == self.executable_sha256,
      "selected executable changed"
    );
    ensure!(
      sha(&fs::read(&self.profile)?) == self.profile_sha256,
      "selected EOS profile changed"
    );
    ensure!(
      sha(&fs::read(&self.selection)?) == self.selection_sha256,
      "selected skill file changed"
    );
    Ok(())
  }

  pub fn verify_launch_selectors(
    &self,
    spec: &process::ProcessSpec,
    secrets: &BTreeMap<String, OsString>,
    sources: &BTreeSet<String>,
  ) -> Result<()> {
    ensure!(
      effective_selectors(spec, secrets, sources) == self.selectors,
      "selected EOS asset selectors changed at launch"
    );
    Ok(())
  }

  pub fn diagnostic(
    &self,
    run_id: &str,
    code: &'static str,
    eos: Option<(&'static str, &'static str)>,
  ) -> Diagnostic {
    Diagnostic {
      code,
      stage: "preflight",
      message: "Repair selected skill catalog and retry",
      eos_stage: eos.map(|pair| pair.0),
      eos_code: eos.map(|pair| pair.1),
      meta: Box::new(SafeMeta {
        run_id: run_id.to_owned(),
        revision: self.revision.clone(),
        generation: self.generation.clone(),
        package_id: self.libraries.first().map(|lib| lib.package_id.clone()),
        package_version: self
          .libraries
          .first()
          .map(|lib| lib.package_version.clone()),
      }),
    }
  }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Success {
  schema_version: u32,
  ok: bool,
  active_revision: String,
  generation: String,
  run_id: String,
  selection_sha256: String,
  libraries: Vec<LibraryEcho>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct LibraryEcho {
  package_id: String,
  package_version: String,
  library: String,
  library_sha256: String,
  abi_version: u32,
  skill_count: u32,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Rejection {
  schema_version: u32,
  ok: bool,
  stage: String,
  code: String,
}

fn closed_pair(stage: &str, code: &str) -> Option<(&'static str, &'static str)> {
  let pairs: &[(&str, &[&str])] = &[
    (
      "input",
      &[
        "preflight_arguments_invalid",
        "preflight_grants_invalid",
        "preflight_profile_invalid",
      ],
    ),
    (
      "selection",
      &[
        "selection_invalid",
        "selection_revision_mismatch",
        "selection_manifest_invalid",
        "selection_digest_mismatch",
        "selection_handoff_incomplete",
      ],
    ),
    (
      "package",
      &[
        "package_invalid",
        "package_changed",
        "package_unsafe",
        "library_changed",
      ],
    ),
    ("elf", &["library_not_elf", "library_dependency_unsafe"]),
    (
      "worker",
      &[
        "worker_spawn_failed",
        "worker_sandbox_failed",
        "worker_inspection_failed",
        "worker_timeout",
        "worker_reap_failed",
      ],
    ),
    (
      "abi",
      &[
        "abi_library_invalid",
        "abi_descriptor_invalid",
        "abi_schema_invalid",
        "abi_capability_unowned",
      ],
    ),
    (
      "catalog",
      &[
        "catalog_duplicate_identity",
        "catalog_capacity",
        "catalog_broker_unavailable",
        "catalog_binding_unavailable",
        "catalog_list_oversize",
      ],
    ),
  ];
  pairs
    .iter()
    .find(|(s, _)| *s == stage)
    .and_then(|(s, codes)| codes.iter().find(|c| **c == code).map(|c| (*s, *c)))
}

fn parse(
  status: ExitStatus,
  output: &[u8],
  inputs: &Inputs,
  run_id: &str,
) -> std::result::Result<(), Diagnostic> {
  let invalid = || inputs.diagnostic(run_id, "skill_catalog_preflight_invalid_output", None);
  if status.signal().is_some() {
    return Err(inputs.diagnostic(run_id, "skill_catalog_preflight_timeout", None));
  }
  if output.last() != Some(&b'\n')
    || output[..output.len() - 1].contains(&b'\n')
    || output.contains(&b'\r')
  {
    return Err(invalid());
  }
  let line = std::str::from_utf8(&output[..output.len() - 1]).map_err(|_| invalid())?;
  // serde_json ordinarily accepts duplicate keys. Value's visitor below rejects them.
  let value = strict_json(line).ok_or_else(invalid)?;
  if status.success() {
    let result: Success = serde_json::from_value(value).map_err(|_| invalid())?;
    if result.schema_version != 1
      || !result.ok
      || result.active_revision != inputs.revision
      || result.generation != inputs.generation
      || result.run_id != run_id
      || result.selection_sha256 != inputs.selection_sha256
      || result.libraries.len() != inputs.libraries.len()
    {
      return Err(invalid());
    }
    for (got, expected) in result.libraries.iter().zip(&inputs.libraries) {
      if got.package_id != expected.package_id
        || got.package_version != expected.package_version
        || got.library != expected.library
        || got.library_sha256 != expected.library_sha256
        || got.abi_version != 2
        || !(1..=256).contains(&got.skill_count)
      {
        return Err(invalid());
      }
    }
    Ok(())
  } else {
    let result: Rejection = serde_json::from_value(value).map_err(|_| invalid())?;
    let pair = closed_pair(&result.stage, &result.code).ok_or_else(invalid)?;
    if result.schema_version != 1 || result.ok {
      return Err(invalid());
    }
    Err(inputs.diagnostic(run_id, "skill_catalog_preflight_rejected", Some(pair)))
  }
}

fn strict_json(source: &str) -> Option<Value> {
  use serde::de::{Deserializer, MapAccess, SeqAccess, Visitor};
  struct Strict;
  impl<'de> serde::Deserialize<'de> for Strict {
    fn deserialize<D: Deserializer<'de>>(d: D) -> std::result::Result<Self, D::Error> {
      d.deserialize_any(Strict)
    }
  }
  impl<'de> Visitor<'de> for Strict {
    type Value = Strict;
    fn expecting(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
      f.write_str("unique-key JSON")
    }
    fn visit_bool<E: serde::de::Error>(self, _: bool) -> std::result::Result<Self::Value, E> {
      Ok(Strict)
    }
    fn visit_i64<E: serde::de::Error>(self, _: i64) -> std::result::Result<Self::Value, E> {
      Ok(Strict)
    }
    fn visit_u64<E: serde::de::Error>(self, _: u64) -> std::result::Result<Self::Value, E> {
      Ok(Strict)
    }
    fn visit_f64<E: serde::de::Error>(self, _: f64) -> std::result::Result<Self::Value, E> {
      Ok(Strict)
    }
    fn visit_str<E: serde::de::Error>(self, _: &str) -> std::result::Result<Self::Value, E> {
      Ok(Strict)
    }
    fn visit_string<E: serde::de::Error>(self, _: String) -> std::result::Result<Self::Value, E> {
      Ok(Strict)
    }
    fn visit_none<E: serde::de::Error>(self) -> std::result::Result<Self::Value, E> {
      Ok(Strict)
    }
    fn visit_unit<E: serde::de::Error>(self) -> std::result::Result<Self::Value, E> {
      Ok(Strict)
    }
    fn visit_some<D: Deserializer<'de>>(self, d: D) -> std::result::Result<Self::Value, D::Error> {
      serde::Deserialize::deserialize(d)
    }
    fn visit_seq<A: SeqAccess<'de>>(self, mut a: A) -> std::result::Result<Self::Value, A::Error> {
      while a.next_element::<Strict>()?.is_some() {}
      Ok(Strict)
    }
    fn visit_map<A: MapAccess<'de>>(self, mut a: A) -> std::result::Result<Self::Value, A::Error> {
      let mut keys = std::collections::HashSet::new();
      while let Some(key) = a.next_key::<String>()? {
        if !keys.insert(key) {
          return Err(serde::de::Error::custom("duplicate key"));
        }
        a.next_value::<Strict>()?;
      }
      Ok(Strict)
    }
  }
  let mut d = serde_json::Deserializer::from_str(source);
  let _: Strict = serde::Deserialize::deserialize(&mut d).ok()?;
  d.end().ok()?;
  serde_json::from_str(source).ok()
}

fn nonblocking(child: &Child) -> io::Result<()> {
  for fd in [
    child.stdout.as_ref().unwrap().as_raw_fd(),
    child.stderr.as_ref().unwrap().as_raw_fd(),
  ] {
    let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
    if flags < 0 || unsafe { libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK) } < 0 {
      return Err(io::Error::last_os_error());
    }
  }
  Ok(())
}

fn drain(reader: &mut impl Read, bytes: &mut Vec<u8>) -> io::Result<bool> {
  let mut buffer = [0_u8; 4096];
  loop {
    match reader.read(&mut buffer) {
      Ok(0) => return Ok(true),
      Ok(count) => {
        if bytes.len() + count > LIMIT {
          return Err(io::Error::other("probe output limit"));
        }
        bytes.extend_from_slice(&buffer[..count]);
      }
      Err(error) if error.kind() == io::ErrorKind::WouldBlock => return Ok(false),
      Err(error) => return Err(error),
    }
  }
}

fn cleanup(child: &mut Child) -> bool {
  let pid = child.id() as i32;
  let signal = |sig| unsafe { libc::kill(-pid, sig) };
  if signal(libc::SIGTERM) != 0 && io::Error::last_os_error().raw_os_error() != Some(libc::ESRCH) {
    return false;
  }
  let deadline = Instant::now() + Duration::from_secs(2);
  loop {
    let child_done = child.try_wait().ok().flatten().is_some();
    let group_gone =
      signal(0) != 0 && io::Error::last_os_error().raw_os_error() == Some(libc::ESRCH);
    if child_done && group_gone {
      return true;
    }
    if Instant::now() >= deadline {
      break;
    }
    std::thread::sleep(Duration::from_millis(10));
  }
  let _ = signal(libc::SIGKILL);
  let deadline = Instant::now() + Duration::from_secs(2);
  loop {
    let child_done = child.try_wait().ok().flatten().is_some();
    let group_gone =
      signal(0) != 0 && io::Error::last_os_error().raw_os_error() == Some(libc::ESRCH);
    if child_done && group_gone {
      return true;
    }
    if Instant::now() >= deadline {
      return false;
    }
    std::thread::sleep(Duration::from_millis(10));
  }
}

pub fn run(inputs: &Inputs, run_id: &str, verifier: &Path) -> std::result::Result<(), Diagnostic> {
  run_with_deadline(inputs, run_id, verifier, DEADLINE)
}

fn run_with_deadline(
  inputs: &Inputs,
  run_id: &str,
  verifier: &Path,
  deadline: Duration,
) -> std::result::Result<(), Diagnostic> {
  let invalid = || inputs.diagnostic(run_id, "skill_catalog_preflight_invalid_output", None);
  let mut command = Command::new(&inputs.executable);
  command
    .arg("--skill-catalog-preflight")
    .arg("--profile")
    .arg(&inputs.profile)
    .arg("--run-id")
    .arg(run_id)
    .args(&inputs.node_types)
    .env_clear()
    .env("EOS_CONTROL_GRANTS_FILE", verifier)
    .env(skill_selection::ENV, &inputs.selection)
    .envs(&inputs.selectors)
    .stdin(Stdio::null())
    .stdout(Stdio::piped())
    .stderr(Stdio::piped());
  let parent_pid = std::process::id();
  unsafe {
    command.pre_exec(move || {
      if libc::setsid() < 0 || libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM) < 0 {
        return Err(io::Error::last_os_error());
      }
      if libc::getppid() as u32 != parent_pid {
        return Err(io::Error::from_raw_os_error(libc::ESRCH));
      }
      Ok(())
    });
  }
  let start = Instant::now();
  let mut child = command.spawn().map_err(|_| invalid())?;
  let mut stdout = Vec::new();
  let mut stderr = Vec::new();
  let mut output_error = nonblocking(&child).is_err();
  let mut status = None;
  let mut eof = (false, false);
  while !output_error && start.elapsed() < deadline {
    eof.0 = match drain(child.stdout.as_mut().unwrap(), &mut stdout) {
      Ok(done) => done,
      Err(_) => {
        output_error = true;
        break;
      }
    };
    eof.1 = match drain(child.stderr.as_mut().unwrap(), &mut stderr) {
      Ok(done) => done,
      Err(_) => {
        output_error = true;
        break;
      }
    };
    status = match child.try_wait() {
      Ok(status) => status,
      Err(_) => {
        output_error = true;
        break;
      }
    };
    if status.is_some() && eof.0 && eof.1 {
      break;
    }
    std::thread::sleep(Duration::from_millis(10));
  }
  let timed_out = start.elapsed() >= deadline;
  let cleaned = cleanup(&mut child);
  if !cleaned {
    return Err(inputs.diagnostic(run_id, "skill_catalog_preflight_cleanup_unconfirmed", None));
  }
  if timed_out {
    return Err(inputs.diagnostic(run_id, "skill_catalog_preflight_timeout", None));
  }
  if output_error || !eof.0 || !eof.1 {
    return Err(invalid());
  }
  parse(status.ok_or_else(invalid)?, &stdout, inputs, run_id)
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::os::unix::{fs::PermissionsExt, process::ExitStatusExt};

  fn fixture() -> (Inputs, Value) {
    let cases: Value = serde_json::from_str(include_str!(
      "../../../tests/fixtures/skill_selection/preflight_cases.json"
    ))
    .unwrap();
    let success = &cases["success_result"];
    let entry = &success["libraries"][0];
    (
      Inputs {
        executable: PathBuf::from("/bin/true"),
        executable_sha256: String::new(),
        profile: PathBuf::from("/tmp/fixture-profile"),
        profile_sha256: String::new(),
        selection: PathBuf::from("/tmp/fixture-selection"),
        selection_sha256: success["selection_sha256"].as_str().unwrap().into(),
        generation: success["generation"].as_str().unwrap().into(),
        revision: success["active_revision"].as_str().unwrap().into(),
        libraries: vec![skill_selection::SelectedLibrary {
          package_id: entry["package_id"].as_str().unwrap().into(),
          package_version: entry["package_version"].as_str().unwrap().into(),
          library: entry["library"].as_str().unwrap().into(),
          library_sha256: entry["library_sha256"].as_str().unwrap().into(),
          package_root: String::new(),
          payload_root: String::new(),
          receipt_sha256: String::new(),
          release_sha256: String::new(),
          payload_sha256: String::new(),
        }],
        node_types: Vec::new(),
        specs_sha256: String::new(),
        selectors: BTreeMap::new(),
      },
      cases,
    )
  }

  #[test]
  fn frozen_closed_protocol_and_exact_echo() {
    let (inputs, cases) = fixture();
    let success = format!("{}\n", cases["success_result"]);
    assert!(
      parse(
        ExitStatus::from_raw(0),
        success.as_bytes(),
        &inputs,
        "candidate-1"
      )
      .is_ok()
    );
    for case in cases["cases"].as_array().unwrap() {
      if let (Some(stage), Some(code)) = (case["eos_stage"].as_str(), case["eos_code"].as_str()) {
        let line = format!(
          "{{\"schema_version\":1,\"ok\":false,\"stage\":\"{stage}\",\"code\":\"{code}\"}}\n"
        );
        let error = parse(
          ExitStatus::from_raw(256),
          line.as_bytes(),
          &inputs,
          "candidate-1",
        )
        .unwrap_err();
        assert_eq!(
          error.code, "skill_catalog_preflight_rejected",
          "{}",
          case["id"]
        );
        assert_eq!(error.meta.package_version, Some("0.1.0".into()));
      }
    }
    let bad = [
      "",
      "{}\n",
      "{}\n{}\n",
      "{\"schema_version\":1,\"schema_version\":1}\n",
      "{\"schema_version\":1,\"ok\":false,\"stage\":\"guest\",\"code\":\"bad\"}\n",
    ];
    for line in bad {
      assert_eq!(
        parse(
          ExitStatus::from_raw(0),
          line.as_bytes(),
          &inputs,
          "candidate-1"
        )
        .unwrap_err()
        .code,
        "skill_catalog_preflight_invalid_output"
      );
    }
    let mut mismatch = cases["success_result"].clone();
    mismatch["libraries"][0]["library_sha256"] = Value::String("0".repeat(64));
    assert_eq!(
      parse(
        ExitStatus::from_raw(0),
        format!("{mismatch}\n").as_bytes(),
        &inputs,
        "candidate-1"
      )
      .unwrap_err()
      .code,
      "skill_catalog_preflight_invalid_output"
    );
    assert_eq!(
      parse(ExitStatus::from_raw(15), b"", &inputs, "candidate-1")
        .unwrap_err()
        .code,
      "skill_catalog_preflight_timeout"
    );
  }

  fn script(body: &str) -> (tempfile::TempDir, Inputs) {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("probe");
    fs::write(&path, format!("#!/bin/sh\n{body}\n")).unwrap();
    fs::set_permissions(&path, fs::Permissions::from_mode(0o700)).unwrap();
    let (mut inputs, _) = fixture();
    inputs.executable = path;
    (dir, inputs)
  }

  #[test]
  fn probe_clears_environment_and_reaps_rejection() {
    let (_dir, inputs) = script(
      "test -z \"$HOME\" || exit 2\ntest -n \"$EOS_CONTROL_GRANTS_FILE\" || exit 2\ntest -n \"$EOS_SKILL_SELECTION_FILE\" || exit 2\nprintf '%s\\n' '{\"schema_version\":1,\"ok\":false,\"stage\":\"selection\",\"code\":\"selection_invalid\"}'\nexit 1",
    );
    let error = run_with_deadline(
      &inputs,
      "candidate-1",
      Path::new("/tmp/verifier"),
      Duration::from_secs(1),
    )
    .unwrap_err();
    assert_eq!(error.code, "skill_catalog_preflight_rejected");
    assert_eq!(error.eos_code, Some("selection_invalid"));
  }

  #[test]
  fn asset_selectors_match_authored_and_resolved_launch_environment() {
    let spec = process::ProcessSpec {
      id: "eos".into(),
      path: "isos/process/component/eos".into(),
      settings: serde_json::from_value(serde_json::json!({
          "command":["/bin/true"],
          "env":{"ISOS_GPU":"nvidia"},
          "env_from":{"ISOS_ROS":"PRIVATE_ROS_SOURCE"}
      }))
      .unwrap(),
      package: None,
      fingerprint: None,
    };
    let sources = BTreeSet::from(["ISOS_ROS".to_owned()]);
    let empty = BTreeMap::new();
    let selected = effective_selectors(&spec, &empty, &sources);
    assert_eq!(selected.get("ISOS_GPU").map(String::as_str), Some("nvidia"));
    assert!(!selected.contains_key("ISOS_ROS"));
    let secrets = BTreeMap::from([("ISOS_ROS".to_owned(), OsString::from("jazzy"))]);
    let selected = effective_selectors(&spec, &secrets, &sources);
    assert_eq!(selected.get("ISOS_ROS").map(String::as_str), Some("jazzy"));
    let (_dir, mut inputs) = script(
      "test \"$ISOS_GPU\" = nvidia || exit 2\ntest \"$ISOS_ROS\" = jazzy || exit 2\ntest -z \"$HOME\" || exit 2\nprintf '%s\\n' '{\"schema_version\":1,\"ok\":false,\"stage\":\"selection\",\"code\":\"selection_invalid\"}'\nexit 1",
    );
    inputs.selectors = selected;
    inputs
      .verify_launch_selectors(&spec, &secrets, &sources)
      .unwrap();
    let error = run_with_deadline(
      &inputs,
      "candidate-1",
      Path::new("/tmp/verifier"),
      Duration::from_secs(1),
    )
    .unwrap_err();
    assert_eq!(error.code, "skill_catalog_preflight_rejected");
    assert!(
      inputs
        .verify_launch_selectors(&spec, &empty, &sources)
        .is_err()
    );
  }

  #[test]
  fn probe_timeout_reaps_group() {
    let (_dir, inputs) = script("exec sleep 5");
    let start = Instant::now();
    let error = run_with_deadline(
      &inputs,
      "candidate-1",
      Path::new("/tmp/verifier"),
      Duration::from_millis(100),
    )
    .unwrap_err();
    assert_eq!(error.code, "skill_catalog_preflight_timeout");
    assert!(start.elapsed() < Duration::from_secs(3));
  }
}
