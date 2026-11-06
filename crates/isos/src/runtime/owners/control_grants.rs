//! Private, per-run credentials for EOS managed admission.

use crate::{app::config::Paths, runtime::process};
use anyhow::{Result, ensure};
use serde::Serialize;
use crate::interfaces::grants::Verifier;
use sha2::{Digest, Sha256};
use std::{
  fs::{self, File, OpenOptions},
  io::{Read, Write},
  os::unix::fs::{DirBuilderExt, FileTypeExt, MetadataExt, OpenOptionsExt, PermissionsExt},
  path::{Path, PathBuf},
};

const GRANT_NAMES: [&str; 3] = [
  "egia-control.grant",
  "operator-control.grant",
  "verifier.json",
];
pub const RESERVED_ENVS: [&str; 8] = [
  "EOS_CONTROL_GRANTS_FILE",
  "EGIA_EOS_CONTROL_GRANT_FILE",
  "AETHER_BRIDGE_CONTROL_GRANT_FILE",
  "AETHER_BRIDGE_VISION_VERIFIER_FILE",
  "EGIA_VISION_BINDING_FILE",
  "EGIA_EOS_VISION_GRANT_FILE",
  "EGIA_VISION_INSPECTION_SOCKET",
  "EGIA_VISION_INSPECTION_VERIFIER_FILE",
];
const VISION_FILES: [&str; 4] = [
  "egia-vision.grant",
  "vision-inspection.grant",
  "vision-verifier.json",
  "egia-vision-inspection-verifier.json",
];
const ENDPOINT_FILES: [&str; 4] = [
  "eos-vision-owner.sock",
  "egia-vision-inspection.sock",
  "vision-verifier.json",
  "egia-vision-inspection-verifier.json",
];

#[derive(Debug, Serialize)]
pub struct Diagnostic {
  pub code: &'static str,
  pub stage: &'static str,
  pub path: String,
  pub message: &'static str,
}

impl std::fmt::Display for Diagnostic {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(
      f,
      "{}: repair private control-grant state before launching",
      self.code
    )
  }
}
impl std::error::Error for Diagnostic {}

fn failure(code: &'static str) -> anyhow::Error {
  Diagnostic {
    code,
    stage: "spawn",
    path: String::new(),
    message: "Repair private control-grant state and retry launch",
  }
  .into()
}

#[derive(Debug, Clone)]
pub struct Bundle {
  root: PathBuf,
  runtime_root: PathBuf,
  endpoint_root: Option<PathBuf>,
  run_id: String,
  revision: String,
  vision: Option<crate::runtime::process::vision_binding::Template>,
}

fn valid_hash(value: &str) -> bool {
  value.len() == 64
    && value
      .bytes()
      .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
}

fn inspect_dir(path: &Path) -> Result<()> {
  let meta = fs::symlink_metadata(path).map_err(|_| failure("control_grant_unsafe_path"))?;
  ensure!(
    meta.file_type().is_dir()
      && meta.uid() == unsafe { libc::geteuid() }
      && meta.permissions().mode() & 0o7777 == 0o700,
    failure("control_grant_unsafe_path")
  );
  Ok(())
}

fn no_symlink_ancestors(path: &Path) -> Result<()> {
  ensure!(path.is_absolute(), failure("control_grant_unsafe_path"));
  for ancestor in path.ancestors() {
    let meta = fs::symlink_metadata(ancestor).map_err(|_| failure("control_grant_unsafe_path"))?;
    ensure!(
      meta.file_type().is_dir(),
      failure("control_grant_unsafe_path")
    );
  }
  Ok(())
}

fn create_dir(parent: &Path, name: &str) -> Result<PathBuf> {
  inspect_dir(parent)?;
  let path = parent.join(name);
  fs::DirBuilder::new()
    .mode(0o700)
    .create(&path)
    .map_err(|_| failure("control_grant_unsafe_path"))?;
  inspect_dir(&path)?;
  File::open(parent)
    .and_then(|f| f.sync_all())
    .map_err(|_| failure("control_grant_issue_failed"))?;
  Ok(path)
}

fn require_dir(parent: &Path, name: &str) -> Result<PathBuf> {
  inspect_dir(parent)?;
  let path = parent.join(name);
  match fs::symlink_metadata(&path) {
    Ok(_) => inspect_dir(&path)?,
    Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
      return create_dir(parent, name);
    }
    Err(_) => return Err(failure("control_grant_unsafe_path")),
  }
  Ok(path)
}

fn write_new(path: &Path, bytes: &[u8]) -> Result<()> {
  let mut file = OpenOptions::new()
    .write(true)
    .create_new(true)
    .mode(0o600)
    .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
    .open(path)
    .map_err(|_| failure("control_grant_issue_failed"))?;
  file
    .write_all(bytes)
    .and_then(|_| file.sync_all())
    .map_err(|_| failure("control_grant_issue_failed"))?;
  Ok(())
}

fn read_file(path: &Path, max: u64) -> Result<Vec<u8>> {
  let meta = fs::symlink_metadata(path).map_err(|_| failure("control_grant_invalid_file"))?;
  ensure!(
    meta.file_type().is_file()
      && meta.uid() == unsafe { libc::geteuid() }
      && meta.nlink() == 1
      && meta.permissions().mode() & 0o7777 == 0o600,
    failure("control_grant_unsafe_path")
  );
  ensure!(meta.len() <= max, failure("control_grant_invalid_file"));
  let file = OpenOptions::new()
    .read(true)
    .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
    .open(path)
    .map_err(|_| failure("control_grant_unsafe_path"))?;
  let descriptor_meta = file
    .metadata()
    .map_err(|_| failure("control_grant_unsafe_path"))?;
  ensure!(
    descriptor_meta.ino() == meta.ino() && descriptor_meta.dev() == meta.dev(),
    failure("control_grant_unsafe_path")
  );
  let mut bytes = Vec::new();
  file
    .take(max + 1)
    .read_to_end(&mut bytes)
    .map_err(|_| failure("control_grant_invalid_file"))?;
  ensure!(
    bytes.len() as u64 == meta.len(),
    failure("control_grant_invalid_file")
  );
  Ok(bytes)
}

fn random_grant() -> Result<[u8; 32]> {
  let mut value = [0u8; 32];
  let mut offset = 0;
  while offset < value.len() {
    let count =
      unsafe { libc::getrandom(value[offset..].as_mut_ptr().cast(), value.len() - offset, 0) };
    if count < 0 {
      if std::io::Error::last_os_error().kind() == std::io::ErrorKind::Interrupted {
        continue;
      }
      return Err(failure("control_grant_issue_failed"));
    }
    ensure!(count > 0, failure("control_grant_issue_failed"));
    offset += count as usize;
  }
  Ok(value)
}

fn decoded_grant(bytes: &[u8]) -> Result<Vec<u8>> {
  ensure!(
    bytes.len() == 65 && bytes[64] == b'\n',
    failure("control_grant_invalid_file")
  );
  let text =
    std::str::from_utf8(&bytes[..64]).map_err(|_| failure("control_grant_invalid_file"))?;
  ensure!(valid_hash(text), failure("control_grant_invalid_file"));
  hex::decode(text).map_err(|_| failure("control_grant_invalid_file"))
}

fn endpoint_name(deployment: &str, run_id: &str) -> String {
  let digest = Sha256::digest(format!("{deployment}\0{run_id}").as_bytes());
  hex::encode(&digest[..16])
}

fn socket_path(root: &Path, name: &str) -> Result<PathBuf> {
  let path = root.join(name);
  ensure!(
    path.as_os_str().as_encoded_bytes().len() <= 107,
    failure("vision_endpoint_unavailable")
  );
  Ok(path)
}

fn verify_endpoint_dir(root: &Path) -> Result<()> {
  no_symlink_ancestors(root)?;
  inspect_dir(root)?;
  inspect_dir(
    root
      .parent()
      .ok_or_else(|| failure("control_grant_unsafe_path"))?,
  )?;
  let actual = fs::read_dir(root)
    .map_err(|_| failure("vision_endpoint_unavailable"))?
    .map(|entry| {
      entry
        .map(|entry| entry.file_name())
        .map_err(|_| failure("vision_endpoint_unavailable"))
    })
    .collect::<Result<std::collections::BTreeSet<_>>>()?;
  let mut expected: std::collections::BTreeSet<_> = ENDPOINT_FILES[2..]
    .iter()
    .map(std::ffi::OsStr::new)
    .map(std::ffi::OsStr::to_os_string)
    .collect();
  for name in &ENDPOINT_FILES[..2] {
    let path = socket_path(root, name)?;
    if let Ok(metadata) = fs::symlink_metadata(&path) {
      ensure!(
        metadata.file_type().is_socket()
          && metadata.uid() == unsafe { libc::geteuid() }
          && metadata.permissions().mode() & 0o7777 == 0o600,
        failure("vision_endpoint_unavailable")
      );
      expected.insert(std::ffi::OsStr::new(name).to_os_string());
    }
  }
  ensure!(actual == expected, failure("vision_endpoint_unavailable"));
  Ok(())
}

pub fn check_endpoint_orphans<'a>(
  paths: &Paths,
  active_runs: impl IntoIterator<Item = (&'a str, &'a str, &'a str)>,
) -> Result<()> {
  let root = paths.runtime.join("v");
  match fs::symlink_metadata(&root) {
    Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(()),
    Err(_) => return Err(failure("vision_endpoint_unavailable")),
    Ok(_) => {}
  }
  no_symlink_ancestors(&root)?;
  inspect_dir(&root)?;
  let allowed: std::collections::BTreeMap<_, _> = active_runs
    .into_iter()
    .map(|(deployment, run_id, revision)| (endpoint_name(deployment, run_id), (run_id, revision)))
    .collect();
  for entry in fs::read_dir(&root).map_err(|_| failure("vision_endpoint_unavailable"))? {
    let entry = entry.map_err(|_| failure("vision_endpoint_unavailable"))?;
    let name = entry.file_name();
    let (run_id, revision) = name
      .to_str()
      .and_then(|name| allowed.get(name))
      .ok_or_else(|| failure("vision_endpoint_unavailable"))?;
    verify_endpoint_dir(&entry.path())?;
    for (verifier_name, revision_field) in [
      (VISION_FILES[2], "active_revision"),
      (VISION_FILES[3], "graph_revision"),
    ] {
      let value: serde_json::Value =
        serde_json::from_slice(&read_file(&entry.path().join(verifier_name), 4096)?)
          .map_err(|_| failure("vision_grant_invalid"))?;
      ensure!(
        value["run_id"] == **run_id && value[revision_field] == **revision,
        failure("vision_graph_stale")
      );
    }
  }
  Ok(())
}

impl Bundle {
  pub fn issue(paths: &Paths, deployment: &str, run_id: &str, revision: &str) -> Result<Self> {
    Self::issue_with(paths, deployment, run_id, revision, random_grant)
  }

  fn issue_with(
    paths: &Paths,
    deployment: &str,
    run_id: &str,
    revision: &str,
    mut draw: impl FnMut() -> Result<[u8; 32]>,
  ) -> Result<Self> {
    process::validate_deployment_name(deployment)?;
    ensure!(
      run_id.len() <= 64
        && run_id.starts_with('r')
        && run_id
          .bytes()
          .all(|b| b.is_ascii_alphanumeric() || b == b'-'),
      failure("control_grant_issue_failed")
    );
    ensure!(valid_hash(revision), failure("control_grant_mismatch"));
    no_symlink_ancestors(&paths.runtime)?;
    inspect_dir(&paths.runtime)?;
    let control_root = require_dir(&paths.runtime, "control-grants")?;
    let deployment_root = require_dir(&control_root, deployment)?;
    let root = create_dir(&deployment_root, run_id)?;
    let issue = (|| {
      let egia = draw()?;
      let operator = draw()?;
      ensure!(egia != operator, failure("control_grant_issue_failed"));
      write_new(
        &root.join(GRANT_NAMES[0]),
        format!("{}\n", hex::encode(egia)).as_bytes(),
      )?;
      write_new(
        &root.join(GRANT_NAMES[1]),
        format!("{}\n", hex::encode(operator)).as_bytes(),
      )?;
      let verifier = Verifier {
        schema_version: 1,
        run_id: run_id.to_owned(),
        active_revision: revision.to_owned(),
        egia_control_sha256: hex::encode(Sha256::digest(egia)),
        operator_control_sha256: hex::encode(Sha256::digest(operator)),
      };
      let mut bytes =
        serde_json::to_vec(&verifier).map_err(|_| failure("control_grant_issue_failed"))?;
      bytes.push(b'\n');
      write_new(&root.join(GRANT_NAMES[2]), &bytes)?;
      File::open(&root)
        .and_then(|f| f.sync_all())
        .map_err(|_| failure("control_grant_issue_failed"))?;
      Ok::<(), anyhow::Error>(())
    })();
    if let Err(error) = issue {
      for name in GRANT_NAMES {
        let _ = fs::remove_file(root.join(name));
      }
      let _ = fs::remove_dir(&root);
      return Err(error);
    }
    let bundle = Self {
      root,
      runtime_root: paths.runtime.clone(),
      endpoint_root: None,
      run_id: run_id.to_owned(),
      revision: revision.to_owned(),
      vision: None,
    };
    if let Err(error) = bundle.verify() {
      for name in GRANT_NAMES {
        let _ = fs::remove_file(bundle.root.join(name));
      }
      let _ = fs::remove_dir(&bundle.root);
      return Err(error);
    }
    Ok(bundle)
  }

  pub fn verify(&self) -> Result<()> {
    let run_dir = &self.root;
    no_symlink_ancestors(run_dir)?;
    inspect_dir(run_dir)?;
    inspect_dir(
      run_dir
        .parent()
        .ok_or_else(|| failure("control_grant_unsafe_path"))?,
    )?;
    inspect_dir(
      run_dir
        .parent()
        .and_then(Path::parent)
        .ok_or_else(|| failure("control_grant_unsafe_path"))?,
    )?;
    inspect_dir(
      run_dir
        .parent()
        .and_then(Path::parent)
        .and_then(Path::parent)
        .ok_or_else(|| failure("control_grant_unsafe_path"))?,
    )?;
    let actual = fs::read_dir(run_dir)
      .map_err(|_| failure("control_grant_unsafe_path"))?
      .map(|entry| {
        entry
          .map(|item| item.file_name())
          .map_err(|_| failure("control_grant_unsafe_path"))
      })
      .collect::<Result<std::collections::BTreeSet<_>>>()?;
    let mut expected: std::collections::BTreeSet<_> = GRANT_NAMES
      .iter()
      .map(std::ffi::OsStr::new)
      .map(std::ffi::OsStr::to_os_string)
      .collect();
    if self.vision.is_some() {
      expected.extend(
        VISION_FILES[..2]
          .iter()
          .map(std::ffi::OsStr::new)
          .map(std::ffi::OsStr::to_os_string),
      );
      expected.insert(std::ffi::OsStr::new("egia").to_os_string());
    }
    ensure!(actual == expected, failure("control_grant_unsafe_path"));
    let egia = decoded_grant(&read_file(&run_dir.join(GRANT_NAMES[0]), 65)?)?;
    let operator = decoded_grant(&read_file(&run_dir.join(GRANT_NAMES[1]), 65)?)?;
    let bytes = read_file(&run_dir.join(GRANT_NAMES[2]), 1024)?;
    let verifier: Verifier =
      serde_json::from_slice(&bytes).map_err(|_| failure("control_grant_invalid_file"))?;
    ensure!(
      verifier.schema_version == 1
        && verifier.run_id == self.run_id
        && verifier.active_revision == self.revision
        && verifier.egia_control_sha256 == hex::encode(Sha256::digest(&egia))
        && verifier.operator_control_sha256 == hex::encode(Sha256::digest(&operator)),
      failure("control_grant_mismatch")
    );
    if let Some(template) = &self.vision {
      let endpoint_root = self
        .endpoint_root
        .as_deref()
        .ok_or_else(|| failure("vision_endpoint_unavailable"))?;
      let deployment = run_dir
        .parent()
        .and_then(Path::file_name)
        .and_then(std::ffi::OsStr::to_str)
        .ok_or_else(|| failure("vision_endpoint_unavailable"))?;
      ensure!(
        endpoint_root
          == self
            .runtime_root
            .join("v")
            .join(endpoint_name(deployment, &self.run_id)),
        failure("vision_endpoint_unavailable")
      );
      verify_endpoint_dir(endpoint_root)?;
      inspect_dir(&run_dir.join("egia"))?;
      let egia_entries = fs::read_dir(run_dir.join("egia"))?
        .map(|entry| entry.map(|entry| entry.file_name()))
        .collect::<std::io::Result<std::collections::BTreeSet<_>>>()?;
      ensure!(
        egia_entries == [std::ffi::OsStr::new("vision-binding.json").to_os_string()].into(),
        failure("vision_grant_invalid")
      );
      let binding_bytes = read_file(&run_dir.join("egia/vision-binding.json"), 4096)?;
      let binding: serde_json::Value =
        serde_json::from_slice(&binding_bytes).map_err(|_| failure("vision_grant_invalid"))?;
      ensure!(
        binding
          == crate::runtime::process::vision_binding::binding(
            template,
            &self.run_id,
            run_dir,
            endpoint_root
          ),
        failure("vision_binding_changed")
      );
      let vision = decoded_grant(&read_file(&run_dir.join(VISION_FILES[0]), 65)?)?;
      let inspection = decoded_grant(&read_file(&run_dir.join(VISION_FILES[1]), 65)?)?;
      ensure!(
        vision != inspection
          && vision != egia
          && inspection != egia
          && vision != operator
          && inspection != operator,
        failure("vision_grant_invalid")
      );
      let bridge: serde_json::Value =
        serde_json::from_slice(&read_file(&endpoint_root.join(VISION_FILES[2]), 4096)?)
          .map_err(|_| failure("vision_grant_invalid"))?;
      ensure!(
        bridge
          == serde_json::json!({"schema_version":1,"run_id":self.run_id,
                "active_revision":self.revision,"projection_revision":template.projection_revision,
                "source_path":template.source_path,"gate_path":template.gate_path,
                "egia_vision_sha256":hex::encode(Sha256::digest(&vision))}),
        failure("vision_grant_invalid")
      );
      let inspect: serde_json::Value =
        serde_json::from_slice(&read_file(&endpoint_root.join(VISION_FILES[3]), 4096)?)
          .map_err(|_| failure("vision_grant_invalid"))?;
      ensure!(
        inspect
          == serde_json::json!({"schema_version":1,"run_id":self.run_id,
                "graph_revision":self.revision,"inspection_sha256":hex::encode(Sha256::digest(&inspection))}),
        failure("vision_grant_invalid")
      );
    }
    Ok(())
  }

  pub fn attach_vision(
    &mut self,
    template: crate::runtime::process::vision_binding::Template,
  ) -> Result<()> {
    ensure!(
      self.vision.is_none() && template.graph_revision == self.revision,
      failure("vision_binding_changed")
    );
    self.verify()?;
    let deployment = self
      .root
      .parent()
      .and_then(Path::file_name)
      .and_then(std::ffi::OsStr::to_str)
      .ok_or_else(|| failure("vision_endpoint_unavailable"))?;
    let endpoints = require_dir(&self.runtime_root, "v")?;
    let endpoint_root = endpoints.join(endpoint_name(deployment, &self.run_id));
    socket_path(&endpoint_root, &template.endpoint_name)?;
    socket_path(&endpoint_root, "egia-vision-inspection.sock")?;
    // Existing names are never reused, even if an orphan has valid-looking
    // verifier bytes. This also makes a truncated digest collision closed.
    let vision = random_grant()?;
    let inspection = random_grant()?;
    ensure!(vision != inspection, failure("vision_grant_invalid"));
    let endpoint_root = create_dir(&endpoints, &endpoint_name(deployment, &self.run_id))?;
    let egia_dir = match create_dir(&self.root, "egia") {
      Ok(dir) => dir,
      Err(error) => {
        let _ = fs::remove_dir(&endpoint_root);
        return Err(error);
      }
    };
    let issue = (|| {
      write_new(
        &self.root.join(VISION_FILES[0]),
        format!("{}\n", hex::encode(vision)).as_bytes(),
      )?;
      write_new(
        &self.root.join(VISION_FILES[1]),
        format!("{}\n", hex::encode(inspection)).as_bytes(),
      )?;
      let bridge = serde_json::json!({"schema_version":1,"run_id":self.run_id,
                "active_revision":self.revision,"projection_revision":template.projection_revision,
                "source_path":template.source_path,"gate_path":template.gate_path,
                "egia_vision_sha256":hex::encode(Sha256::digest(vision))});
      write_new(
        &endpoint_root.join(VISION_FILES[2]),
        &serde_json::to_vec(&bridge)?,
      )?;
      let inspect = serde_json::json!({"schema_version":1,"run_id":self.run_id,
                "graph_revision":self.revision,"inspection_sha256":hex::encode(Sha256::digest(inspection))});
      write_new(
        &endpoint_root.join(VISION_FILES[3]),
        &serde_json::to_vec(&inspect)?,
      )?;
      write_new(
        &egia_dir.join("vision-binding.json"),
        &serde_json::to_vec(&crate::runtime::process::vision_binding::binding(
          &template,
          &self.run_id,
          &self.root,
          &endpoint_root,
        ))?,
      )?;
      File::open(&self.root)?.sync_all()?;
      File::open(&endpoint_root)?.sync_all()?;
      Ok::<(), anyhow::Error>(())
    })();
    if issue.is_err() {
      for name in &VISION_FILES[..2] {
        let _ = fs::remove_file(self.root.join(name));
      }
      for name in &VISION_FILES[2..] {
        let _ = fs::remove_file(endpoint_root.join(name));
      }
      let _ = fs::remove_file(egia_dir.join("vision-binding.json"));
      let _ = fs::remove_dir(egia_dir);
      let _ = fs::remove_dir(&endpoint_root);
      return issue;
    }
    self.endpoint_root = Some(endpoint_root);
    self.vision = Some(template);
    self.verify()
  }

  pub fn vision_env(&self, id: &str) -> Vec<(&'static str, PathBuf)> {
    if self.vision.is_none() {
      return Vec::new();
    }
    match id {
      "bridge" => vec![(
        "AETHER_BRIDGE_VISION_VERIFIER_FILE",
        self.endpoint_root.as_ref().unwrap().join(VISION_FILES[2]),
      )],
      "egia" => vec![
        (
          "EGIA_VISION_BINDING_FILE",
          self.root.join("egia/vision-binding.json"),
        ),
        (
          "EGIA_EOS_VISION_GRANT_FILE",
          self.root.join(VISION_FILES[0]),
        ),
        (
          "EGIA_VISION_INSPECTION_SOCKET",
          self
            .endpoint_root
            .as_ref()
            .unwrap()
            .join("egia-vision-inspection.sock"),
        ),
        (
          "EGIA_VISION_INSPECTION_VERIFIER_FILE",
          self.endpoint_root.as_ref().unwrap().join(VISION_FILES[3]),
        ),
      ],
      _ => Vec::new(),
    }
  }
  pub fn vision_socket(&self) -> Option<PathBuf> {
    self.vision.as_ref().map(|template| {
      self
        .endpoint_root
        .as_ref()
        .unwrap()
        .join(&template.endpoint_name)
    })
  }
  pub fn vision_template(&self) -> Option<&crate::runtime::process::vision_binding::Template> {
    self.vision.as_ref()
  }

  pub fn launch_env(&self, id: &str) -> Option<(&'static str, PathBuf)> {
    match id {
      "eos" => Some((RESERVED_ENVS[0], self.root.join(GRANT_NAMES[2]))),
      "egia" => Some((RESERVED_ENVS[1], self.root.join(GRANT_NAMES[0]))),
      "bridge" => Some((RESERVED_ENVS[2], self.root.join(GRANT_NAMES[1]))),
      _ => None,
    }
  }

  pub fn retire(&self) -> Result<()> {
    self.verify()?;
    if self.vision.is_some() {
      self.revoke_vision()?;
    }
    self.remove_endpoint_dir()?;
    for name in GRANT_NAMES {
      fs::remove_file(self.root.join(name)).map_err(|_| failure("control_grant_unsafe_path"))?;
    }
    fs::remove_dir(&self.root).map_err(|_| failure("control_grant_unsafe_path"))?;
    File::open(
      self
        .root
        .parent()
        .ok_or_else(|| failure("control_grant_unsafe_path"))?,
    )?
    .sync_all()
    .map_err(|_| failure("control_grant_unsafe_path"))?;
    Ok(())
  }

  /// Revoke an uncertain candidate while retaining its run directory as a
  /// durable breadcrumb for the next manager's replacement check.
  pub fn revoke_keep_directory(&self) -> Result<()> {
    self.verify()?;
    if self.vision.is_some() {
      self.revoke_vision()?;
    }
    self.remove_endpoint_dir()?;
    for name in GRANT_NAMES {
      fs::remove_file(self.root.join(name)).map_err(|_| failure("control_grant_unsafe_path"))?;
    }
    ensure!(
      fs::read_dir(&self.root)?.next().is_none(),
      failure("control_grant_unsafe_path")
    );
    File::open(&self.root)?.sync_all()?;
    File::open(
      self
        .root
        .parent()
        .ok_or_else(|| failure("control_grant_unsafe_path"))?,
    )?
    .sync_all()?;
    Ok(())
  }

  fn revoke_vision(&self) -> Result<()> {
    for name in &VISION_FILES[..2] {
      fs::remove_file(self.root.join(name))?;
    }
    let endpoint_root = self
      .endpoint_root
      .as_deref()
      .ok_or_else(|| failure("vision_endpoint_unavailable"))?;
    for name in &VISION_FILES[2..] {
      fs::remove_file(endpoint_root.join(name))?;
    }
    fs::remove_file(self.root.join("egia/vision-binding.json"))?;
    fs::remove_dir(self.root.join("egia"))?;
    Ok(())
  }

  fn remove_endpoint_dir(&self) -> Result<()> {
    let Some(root) = &self.endpoint_root else {
      return Ok(());
    };
    no_symlink_ancestors(root)?;
    inspect_dir(root)?;
    for name in &ENDPOINT_FILES[..2] {
      let path = socket_path(root, name)?;
      match fs::symlink_metadata(&path) {
        Ok(meta) => {
          ensure!(
            meta.file_type().is_socket()
              && meta.uid() == unsafe { libc::geteuid() }
              && meta.permissions().mode() & 0o7777 == 0o600,
            failure("vision_endpoint_unavailable")
          );
          fs::remove_file(&path).map_err(|_| failure("vision_endpoint_unavailable"))?;
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(_) => return Err(failure("vision_endpoint_unavailable")),
      }
    }
    ensure!(
      fs::read_dir(root)?.next().is_none(),
      failure("vision_endpoint_unavailable")
    );
    fs::remove_dir(root).map_err(|_| failure("vision_endpoint_unavailable"))?;
    File::open(
      root
        .parent()
        .ok_or_else(|| failure("vision_endpoint_unavailable"))?,
    )?
    .sync_all()
    .map_err(|_| failure("vision_endpoint_unavailable"))?;
    Ok(())
  }

  pub fn revoke_vision_for_stop(&mut self) -> Result<()> {
    if self.vision.is_none() {
      return Ok(());
    }
    // A corrupt grant must never keep the other read authority live. Unlink
    // both verifier paths independently before touching bindings or children.
    let intact = self.verify().is_ok();
    let mut removed = true;
    let endpoint_root = self.endpoint_root.as_deref();
    for (path_root, name) in [
      (endpoint_root, VISION_FILES[2]),
      (endpoint_root, VISION_FILES[3]),
      (Some(self.root.as_path()), VISION_FILES[0]),
      (Some(self.root.as_path()), VISION_FILES[1]),
    ] {
      let Some(path_root) = path_root else {
        removed = false;
        continue;
      };
      match fs::remove_file(path_root.join(name)) {
        Ok(()) => {}
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(_) => removed = false,
      }
    }
    match fs::remove_file(self.root.join("egia/vision-binding.json")) {
      Ok(()) => {}
      Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
      Err(_) => removed = false,
    }
    match fs::remove_dir(self.root.join("egia")) {
      Ok(()) => {}
      Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
      Err(_) => removed = false,
    }
    if File::open(&self.root)
      .and_then(|file| file.sync_all())
      .is_err()
    {
      removed = false;
    }
    if removed {
      self.vision = None;
    }
    ensure!(intact && removed, failure("vision_grant_invalid"));
    Ok(())
  }
}

pub fn validate_plan(specs: &[process::ProcessSpec]) -> Result<()> {
  for (id, role) in [("eos", "eos"), ("config", "eos")] {
    ensure!(
      specs
        .iter()
        .any(|spec| spec.id == id && spec.settings.role.as_deref() == Some(role)),
      failure("control_grant_delivery_failed")
    );
  }
  for spec in specs {
    if matches!(spec.id.as_str(), "egia" | "bridge") {
      let role = if spec.id == "egia" { "egia" } else { "eos" };
      ensure!(
        spec.settings.role.as_deref() == Some(role),
        failure("control_grant_delivery_failed")
      );
    }
    ensure!(
      spec
        .settings
        .env_from
        .iter()
        .all(|(target, source)| !RESERVED_ENVS.contains(&target.as_str())
          && !RESERVED_ENVS.contains(&source.as_str())),
      failure("control_grant_delivery_failed")
    );
    if matches!(spec.id.as_str(), "eos" | "bridge" | "config") {
      for (index, arg) in spec.settings.args.iter().enumerate() {
        if [
          "--run-id",
          "--backend-release-sha256",
          "--backend-payload-sha256",
          "--log-dir",
          "--owner-vision-socket",
        ]
        .iter()
        .any(|flag| arg == flag || arg.starts_with(&format!("{flag}=")))
        {
          return Err(
            Diagnostic {
              code: "control_grant_reserved_arg",
              stage: "graph",
              path: format!(
                "/cores/isos/layers/process/groups/component/nodes/{}/settings/args/{index}",
                spec.id
              ),
              message: "Remove the owner-reserved launch argument and retry",
            }
            .into(),
          );
        }
      }
    }
  }
  Ok(())
}

#[cfg(test)]
mod tests {
  use super::*;
  use serde_json::json;
  use std::os::unix::fs::symlink;

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

  fn revision() -> String {
    "a".repeat(64)
  }

  fn vision_template() -> crate::runtime::process::vision_binding::Template {
    crate::runtime::process::vision_binding::Template {
      schema_version: 1,
      graph_revision: revision(),
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
    }
  }

  #[test]
  fn selected_vision_grants_are_private_distinct_and_revoked_before_retirement() {
    let (_temp, paths) = paths();
    let mut bundle = Bundle::issue(&paths, "default", "r-one", &revision()).unwrap();
    bundle.attach_vision(vision_template()).unwrap();
    bundle.verify().unwrap();
    let root = bundle.root.clone();
    let binding = root.join("egia/vision-binding.json");
    assert_eq!(
      fs::symlink_metadata(&binding).unwrap().permissions().mode() & 0o7777,
      0o600
    );
    let vision = fs::read(root.join("egia-vision.grant")).unwrap();
    let inspection = fs::read(root.join("vision-inspection.grant")).unwrap();
    assert_ne!(vision, inspection);
    let endpoint_root = bundle.endpoint_root.clone().unwrap();
    let inspection_verifier = endpoint_root.join("egia-vision-inspection-verifier.json");
    assert!(inspection_verifier.is_file());
    assert_eq!(
      bundle.vision_env("bridge"),
      vec![(
        "AETHER_BRIDGE_VISION_VERIFIER_FILE",
        endpoint_root.join("vision-verifier.json")
      )]
    );
    assert!(bundle.vision_env("egia").iter().any(|(name, path)| *name
      == "EGIA_VISION_INSPECTION_VERIFIER_FILE"
      && path == &inspection_verifier));
    assert!(
      !fs::read(&binding)
        .unwrap()
        .windows(vision.len())
        .any(|bytes| bytes == vision)
    );
    let socket = bundle.vision_socket().unwrap();
    let listener = std::os::unix::net::UnixListener::bind(&socket).unwrap();
    fs::set_permissions(&socket, fs::Permissions::from_mode(0o600)).unwrap();
    let established_client = std::os::unix::net::UnixStream::connect(&socket).unwrap();
    let (established_server, _) = listener.accept().unwrap();
    bundle.verify().unwrap();
    bundle.revoke_vision_for_stop().unwrap();
    assert!(!endpoint_root.join("vision-verifier.json").exists());
    assert!(!inspection_verifier.exists());
    assert!(!binding.exists());
    assert!(established_client.peer_addr().is_ok());
    drop(established_server);
    drop(established_client);
    drop(listener);
    fs::remove_file(socket).unwrap();
    bundle.retire().unwrap();
  }

  #[test]
  fn vision_grant_mode_or_verifier_tamper_fails_closed() {
    let (_temp, paths) = paths();
    let mut bundle = Bundle::issue(&paths, "default", "r-two", &revision()).unwrap();
    bundle.attach_vision(vision_template()).unwrap();
    let grant = bundle.root.join("egia-vision.grant");
    fs::set_permissions(&grant, fs::Permissions::from_mode(0o644)).unwrap();
    assert!(bundle.verify().is_err());
    fs::set_permissions(&grant, fs::Permissions::from_mode(0o600)).unwrap();
    fs::write(
      bundle
        .endpoint_root
        .as_ref()
        .unwrap()
        .join("vision-verifier.json"),
      b"{}\n",
    )
    .unwrap();
    assert!(bundle.verify().is_err());
  }

  #[test]
  fn stop_revokes_both_authorities_when_one_verifier_is_tampered_or_missing() {
    for missing in [false, true] {
      let (_temp, paths) = paths();
      let mut bundle = Bundle::issue(&paths, "default", "r-stop", &revision()).unwrap();
      bundle.attach_vision(vision_template()).unwrap();
      let endpoint_root = bundle.endpoint_root.as_ref().unwrap();
      let damaged = endpoint_root.join("egia-vision-inspection-verifier.json");
      if missing {
        fs::remove_file(&damaged).unwrap();
      } else {
        fs::write(&damaged, b"{}\n").unwrap();
      }
      let surviving = endpoint_root.join("vision-verifier.json");
      assert!(surviving.exists());
      let error = bundle.revoke_vision_for_stop().unwrap_err();
      assert_eq!(
        error.downcast_ref::<Diagnostic>().unwrap().code,
        "vision_grant_invalid"
      );
      assert!(!surviving.exists());
      assert!(!damaged.exists());
      assert!(!bundle.root.join("egia-vision.grant").exists());
      assert!(!bundle.root.join("vision-inspection.grant").exists());
      assert!(!bundle.root.join("egia/vision-binding.json").exists());
      bundle.retire().unwrap();
    }
  }

  #[test]
  fn stop_revokes_inspection_authority_when_owner_grant_is_missing() {
    let (_temp, paths) = paths();
    let mut bundle = Bundle::issue(&paths, "default", "r-stop", &revision()).unwrap();
    bundle.attach_vision(vision_template()).unwrap();
    fs::remove_file(bundle.root.join("egia-vision.grant")).unwrap();
    assert!(bundle.revoke_vision_for_stop().is_err());
    assert!(
      !bundle
        .endpoint_root
        .as_ref()
        .unwrap()
        .join("egia-vision-inspection-verifier.json")
        .exists()
    );
    bundle.retire().unwrap();
  }

  #[test]
  fn long_private_home_uses_short_distinct_endpoints_and_cleans_after_stop() {
    let temp = tempfile::tempdir().unwrap();
    let home = temp.path().join("long-private-home-1");
    let paths = Paths {
      config: home.join("config"),
      data: home.join("data"),
      runtime: home.join("run"),
    };
    paths.prepare().unwrap();
    let run = "r-long-deployment-run-12345";
    let mut first = Bundle::issue(&paths, "default", run, &revision()).unwrap();
    assert!(
      first
        .root
        .join("egia-vision-inspection.sock")
        .as_os_str()
        .as_encoded_bytes()
        .len()
        > 107
    );
    first.attach_vision(vision_template()).unwrap();
    let endpoint = first.endpoint_root.clone().unwrap();
    assert!(
      first
        .vision_socket()
        .unwrap()
        .as_os_str()
        .as_encoded_bytes()
        .len()
        <= 107
    );
    assert!(endpoint.starts_with(paths.runtime.join("v")));
    assert_eq!(
      fs::symlink_metadata(&endpoint)
        .unwrap()
        .permissions()
        .mode()
        & 0o7777,
      0o700
    );
    check_endpoint_orphans(&paths, [("default", run, revision().as_str())]).unwrap();
    assert!(check_endpoint_orphans(&paths, [("default", run, "b".repeat(64).as_str())]).is_err());
    assert!(check_endpoint_orphans(&paths, []).is_err());
    let binding: serde_json::Value =
      serde_json::from_slice(&fs::read(first.root.join("egia/vision-binding.json")).unwrap())
        .unwrap();
    assert_eq!(
      binding["eos_endpoint"]["path"],
      first.vision_socket().unwrap().to_str().unwrap()
    );
    first.revoke_vision_for_stop().unwrap();
    assert!(!endpoint.join(VISION_FILES[2]).exists());
    assert!(!endpoint.join(VISION_FILES[3]).exists());
    first.retire().unwrap();
    assert!(!endpoint.exists());
    check_endpoint_orphans(&paths, []).unwrap();
    let mut second = Bundle::issue(
      &paths,
      "default",
      "r-long-deployment-run-12346",
      &revision(),
    )
    .unwrap();
    second.attach_vision(vision_template()).unwrap();
    assert_ne!(second.endpoint_root.as_ref(), Some(&endpoint));
    second.revoke_vision_for_stop().unwrap();
    second.retire().unwrap();
  }

  #[test]
  fn endpoint_tamper_and_orphan_block_replacement() {
    let (_temp, paths) = paths();
    let mut bundle = Bundle::issue(&paths, "default", "r-one", &revision()).unwrap();
    bundle.attach_vision(vision_template()).unwrap();
    let endpoint = bundle.endpoint_root.clone().unwrap();
    let verifier = endpoint.join(VISION_FILES[2]);
    let bytes = fs::read(&verifier).unwrap();
    fs::remove_file(&verifier).unwrap();
    symlink(bundle.root.join(VISION_FILES[0]), &verifier).unwrap();
    assert_eq!(
      code(&bundle.verify().unwrap_err()),
      "control_grant_unsafe_path"
    );
    fs::remove_file(&verifier).unwrap();
    write_new(&verifier, &bytes).unwrap();
    bundle.verify().unwrap();
    bundle.revoke_vision_for_stop().unwrap();
    assert!(check_endpoint_orphans(&paths, []).is_err());
    bundle.retire().unwrap();
    check_endpoint_orphans(&paths, []).unwrap();
  }

  #[test]
  fn maximum_deployment_name_does_not_lengthen_endpoint_and_overlong_home_fails_preflight() {
    let temp = tempfile::tempdir().unwrap();
    let home = temp.path().join("private-home-1234567890");
    let paths = Paths {
      config: home.join("config"),
      data: home.join("data"),
      runtime: home.join("run"),
    };
    paths.prepare().unwrap();
    let deployment = "d".repeat(48);
    let mut bundle = Bundle::issue(&paths, &deployment, "r-max-name", &revision()).unwrap();
    assert!(
      bundle
        .root
        .join("egia-vision-inspection.sock")
        .as_os_str()
        .as_encoded_bytes()
        .len()
        > 107
    );
    bundle.attach_vision(vision_template()).unwrap();
    assert!(
      bundle
        .vision_env("egia")
        .iter()
        .find(|(name, _)| *name == "EGIA_VISION_INSPECTION_SOCKET")
        .unwrap()
        .1
        .as_os_str()
        .as_encoded_bytes()
        .len()
        <= 107
    );
    bundle.revoke_vision_for_stop().unwrap();
    bundle.retire().unwrap();

    let too_long_home = temp
      .path()
      .join("private-home-prefix-0123456789012345678901234567890123456789");
    let long_paths = Paths {
      config: too_long_home.join("config"),
      data: too_long_home.join("data"),
      runtime: too_long_home.join("run"),
    };
    long_paths.prepare().unwrap();
    let mut blocked = Bundle::issue(&long_paths, "default", "r-overlong", &revision()).unwrap();
    assert_eq!(
      code(&blocked.attach_vision(vision_template()).unwrap_err()),
      "vision_endpoint_unavailable"
    );
    assert!(
      fs::read_dir(long_paths.runtime.join("v"))
        .unwrap()
        .next()
        .is_none()
    );
    blocked.retire().unwrap();
  }

  fn spec(id: &str, role: &str, args: Vec<&str>) -> process::ProcessSpec {
    process::ProcessSpec {
      id: id.into(),
      path: format!("isos/process/component/{id}"),
      settings: serde_json::from_value(json!({
          "command": ["/bin/true"], "role": role, "args": args
      }))
      .unwrap(),
      package: None,
      fingerprint: None,
    }
  }

  fn two_specs() -> Vec<process::ProcessSpec> {
    vec![spec("eos", "eos", vec![]), spec("config", "eos", vec![])]
  }

  fn code(error: &anyhow::Error) -> &'static str {
    error.downcast_ref::<Diagnostic>().unwrap().code
  }

  #[test]
  fn issues_exact_private_files_and_distinct_grants_without_generated_state() {
    let (_temp, paths) = paths();
    let bundle = Bundle::issue_with(&paths, "test", "r-test-1", &revision(), {
      let mut values = [[0x11; 32], [0x22; 32]].into_iter();
      move || Ok(values.next().unwrap())
    })
    .unwrap();
    bundle.verify().unwrap();
    assert_eq!(
      fs::symlink_metadata(&bundle.root)
        .unwrap()
        .permissions()
        .mode()
        & 0o777,
      0o700
    );
    for name in GRANT_NAMES {
      assert_eq!(
        fs::symlink_metadata(bundle.root.join(name))
          .unwrap()
          .permissions()
          .mode()
          & 0o777,
        0o600
      );
    }
    let egia = fs::read(bundle.root.join(GRANT_NAMES[0])).unwrap();
    let operator = fs::read(bundle.root.join(GRANT_NAMES[1])).unwrap();
    assert_eq!(egia.len(), 65);
    assert_eq!(operator.len(), 65);
    assert_ne!(egia, operator);
    let verifier: Verifier =
      serde_json::from_slice(&fs::read(bundle.root.join(GRANT_NAMES[2])).unwrap()).unwrap();
    assert_eq!(verifier.run_id, "r-test-1");
    assert_eq!(verifier.active_revision, revision());
    assert_eq!(
      verifier.egia_control_sha256,
      hex::encode(Sha256::digest([0x11; 32]))
    );
    assert_eq!(
      verifier.operator_control_sha256,
      hex::encode(Sha256::digest([0x22; 32]))
    );
    assert!(!paths.generated("test").exists());
    assert_eq!(bundle.launch_env("config"), None);
    assert_eq!(bundle.launch_env("other"), None);
    bundle.retire().unwrap();
    assert!(!bundle.root.exists());
  }

  #[test]
  fn rotation_rejects_old_grant_and_duplicate_run_directory() {
    let (_temp, paths) = paths();
    let first = Bundle::issue(&paths, "test", "r-test-1", &revision()).unwrap();
    assert_eq!(
      code(&Bundle::issue(&paths, "test", "r-test-1", &revision()).unwrap_err()),
      "control_grant_unsafe_path"
    );
    let old = fs::read(first.root.join(GRANT_NAMES[0])).unwrap();
    let second = Bundle::issue(&paths, "test", "r-test-2", &revision()).unwrap();
    let new = fs::read(second.root.join(GRANT_NAMES[0])).unwrap();
    assert_ne!(old, new);
    first.retire().unwrap();
    second.retire().unwrap();
  }

  #[test]
  fn rejects_tamper_shape_mode_symlink_and_verifier_mismatch() {
    let (_temp, paths) = paths();
    let bundle = Bundle::issue(&paths, "test", "r-test-1", &revision()).unwrap();
    let grant = bundle.root.join(GRANT_NAMES[0]);
    let original = fs::read(&grant).unwrap();
    fs::write(&grant, &original[..64]).unwrap();
    assert_eq!(
      code(&bundle.verify().unwrap_err()),
      "control_grant_invalid_file"
    );
    fs::write(&grant, &original).unwrap();
    fs::set_permissions(&grant, fs::Permissions::from_mode(0o644)).unwrap();
    assert_eq!(
      code(&bundle.verify().unwrap_err()),
      "control_grant_unsafe_path"
    );
    fs::set_permissions(&grant, fs::Permissions::from_mode(0o600)).unwrap();
    let verifier = bundle.root.join(GRANT_NAMES[2]);
    let mut value: serde_json::Value =
      serde_json::from_slice(&fs::read(&verifier).unwrap()).unwrap();
    value["active_revision"] = json!("b".repeat(64));
    fs::write(&verifier, serde_json::to_vec(&value).unwrap()).unwrap();
    assert_eq!(
      code(&bundle.verify().unwrap_err()),
      "control_grant_mismatch"
    );
    fs::remove_file(&verifier).unwrap();
    symlink(&grant, &verifier).unwrap();
    assert_eq!(
      code(&bundle.verify().unwrap_err()),
      "control_grant_unsafe_path"
    );
  }

  #[test]
  fn injected_random_failure_does_not_leave_a_run_directory() {
    let (_temp, paths) = paths();
    let error = Bundle::issue_with(&paths, "test", "r-fail", &revision(), || {
      Err(failure("control_grant_issue_failed"))
    })
    .unwrap_err();
    assert_eq!(code(&error), "control_grant_issue_failed");
    assert!(!paths.runtime.join("control-grants/test/r-fail").exists());
  }

  #[test]
  fn plan_requires_eos_and_config_and_rejects_reserved_author_args() {
    let mut specs = two_specs();
    validate_plan(&specs).unwrap();
    validate_plan(&[specs.clone(), vec![spec("egia", "egia", vec![])]].concat()).unwrap();
    specs.remove(0);
    assert_eq!(
      code(&validate_plan(&specs).unwrap_err()),
      "control_grant_delivery_failed"
    );
    let mut specs = two_specs();
    specs[0].settings.args = vec![
      "--listen".into(),
      "127.0.0.1:16020".into(),
      "--run-id=spoof".into(),
    ];
    let error = validate_plan(&specs).unwrap_err();
    let diagnostic = error.downcast_ref::<Diagnostic>().unwrap();
    assert_eq!(diagnostic.code, "control_grant_reserved_arg");
    assert_eq!(diagnostic.stage, "graph");
    assert_eq!(
      diagnostic.path,
      "/cores/isos/layers/process/groups/component/nodes/eos/settings/args/2"
    );
    specs[0].settings.args = vec!["--run-id".into(), "spoof".into()];
    assert_eq!(
      validate_plan(&specs)
        .unwrap_err()
        .downcast_ref::<Diagnostic>()
        .unwrap()
        .path,
      "/cores/isos/layers/process/groups/component/nodes/eos/settings/args/0"
    );
    specs[0].settings.args.clear();
    specs[0]
      .settings
      .env_from
      .insert("EOS_CONTROL_GRANTS_FILE".into(), "MALICIOUS".into());
    assert_eq!(
      code(&validate_plan(&specs).unwrap_err()),
      "control_grant_delivery_failed"
    );
  }
}
