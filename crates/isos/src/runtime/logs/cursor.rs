//! Durable current-run scope and authenticated, rotation-aware log cursors.

use super::{LogStore, event_policy, level_rank, source_valid, token_valid};
use anyhow::{Context, Result, ensure};
use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
  fs::{self, File, OpenOptions},
  io::{BufRead, BufReader, Read, Seek, SeekFrom, Write},
  os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt},
  path::{Path, PathBuf},
  sync::atomic::{AtomicU64, Ordering},
};

const STATE_MAX: u64 = 32_768;
const SCAN_MAX: usize = 65_536;
static NEXT_TEMP: AtomicU64 = AtomicU64::new(1);

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Anchor {
  event: Value,
  segment: String,
  after_offset: u64,
  seal: String,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct State {
  schema_version: u8,
  deployment: String,
  manager_session: String,
  never_run: bool,
  last_run_id: Option<String>,
  manager_anchor: Option<Anchor>,
  run_anchor: Option<Anchor>,
  run_anchor_state: String,
  loss_baseline: u64,
  loss_uncertain: bool,
  scope_seal: String,
}

struct LoadedState {
  state: State,
  anchor_proven: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct SessionIntent {
  schema_version: u8,
  deployment: String,
  session: String,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Token {
  d: String,
  k: String,
  r: String,
  f: String,
  s: String,
  o: u64,
  p: bool,
  l: u64,
}

#[derive(Debug)]
pub struct CurrentError {
  pub code: &'static str,
  pub path: &'static str,
  pub message: &'static str,
  pub status: u16,
}

impl CurrentError {
  fn new(code: &'static str, path: &'static str, message: &'static str, status: u16) -> Self {
    Self {
      code,
      path,
      message,
      status,
    }
  }

  pub fn diagnostic(&self) -> Value {
    json!({"path":self.path,"code":self.code,"message":self.message})
  }

  pub fn public_error(&self) -> &'static str {
    match self.code {
      "log_scope_unavailable" => "log scope unavailable",
      "log_cursor_invalid" | "log_cursor_filter_mismatch" => "log cursor rejected",
      _ => "log query rejected",
    }
  }
}

fn scope_error() -> CurrentError {
  CurrentError::new(
    "log_scope_unavailable",
    "/scope",
    "Current log scope unavailable",
    503,
  )
}

fn cursor_invalid() -> CurrentError {
  CurrentError::new(
    "log_cursor_invalid",
    "/query/cursor",
    "Log cursor invalid",
    400,
  )
}

fn private_write(path: &Path, bytes: &[u8]) -> Result<()> {
  let parent = path.parent().context("cursor state parent")?;
  fs::create_dir_all(parent)?;
  fs::set_permissions(parent, fs::Permissions::from_mode(0o700))?;
  let temporary = parent.join(format!(
    ".cursor-{}-{}.tmp",
    std::process::id(),
    NEXT_TEMP.fetch_add(1, Ordering::Relaxed)
  ));
  let mut file = OpenOptions::new()
    .write(true)
    .create_new(true)
    .mode(0o600)
    .open(&temporary)?;
  let result = (|| {
    file.write_all(bytes)?;
    file.sync_all()?;
    fs::rename(&temporary, path)?;
    File::open(parent)?.sync_all()?;
    Ok::<_, anyhow::Error>(())
  })();
  if result.is_err() {
    let _ = fs::remove_file(&temporary);
  }
  result
}

fn private_read(path: &Path, max: u64) -> Result<Vec<u8>> {
  let meta = fs::symlink_metadata(path)?;
  ensure!(meta.file_type().is_file(), "cursor file is not regular");
  ensure!(
    meta.permissions().mode() & 0o077 == 0,
    "cursor file is not private"
  );
  ensure!(meta.len() <= max, "cursor file exceeds bound");
  let mut file = File::open(path)?;
  let mut bytes = Vec::new();
  file.read_to_end(&mut bytes)?;
  ensure!(bytes.len() as u64 <= max, "cursor file changed while read");
  Ok(bytes)
}

fn identity(path: &Path) -> Result<String> {
  let meta = fs::metadata(path)?;
  let birth = meta
    .created()
    .ok()
    .and_then(|time| time.duration_since(std::time::UNIX_EPOCH).ok())
    .map_or(0, |duration| duration.as_nanos());
  Ok(format!("{:x}-{:x}-{birth:x}", meta.dev(), meta.ino()))
}

fn deployment(store: &LogStore) -> Result<String> {
  Ok(
    store
      .root
      .file_name()
      .context("deployment log root")?
      .to_string_lossy()
      .into_owned(),
  )
}

fn state_path(store: &LogStore) -> PathBuf {
  store.root.join("cursor-state.json")
}

fn scope_key_path(store: &LogStore) -> PathBuf {
  store.root.join("scope-key")
}

fn scope_key(store: &LogStore) -> Result<[u8; 32]> {
  let bytes = private_read(&scope_key_path(store), 32)?;
  ensure!(bytes.len() == 32, "scope key length");
  let mut result = [0_u8; 32];
  result.copy_from_slice(&bytes);
  Ok(result)
}

fn create_scope_key(store: &LogStore) -> Result<()> {
  let path = scope_key_path(store);
  if path.exists() {
    scope_key(store)?;
    return Ok(());
  }
  let mut random = [0_u8; 32];
  File::open("/dev/urandom")?.read_exact(&mut random)?;
  let temporary = store.root.join(format!(
    ".scope-key-{}-{}.tmp",
    std::process::id(),
    NEXT_TEMP.fetch_add(1, Ordering::Relaxed)
  ));
  let mut file = OpenOptions::new()
    .write(true)
    .create_new(true)
    .mode(0o600)
    .open(&temporary)?;
  let result = (|| {
    file.write_all(&random)?;
    file.sync_all()?;
    match fs::hard_link(&temporary, &path) {
      Ok(()) => File::open(&store.root)?.sync_all()?,
      Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => {
        scope_key(store)?;
      }
      Err(error) => return Err(error.into()),
    }
    Ok::<_, anyhow::Error>(())
  })();
  let _ = fs::remove_file(temporary);
  result
}

fn scope_seal(store: &LogStore, state: &State) -> Result<String> {
  let mut unsigned = state.clone();
  unsigned.scope_seal.clear();
  let body = serde_json::to_vec(&unsigned)?;
  Ok(URL_SAFE_NO_PAD.encode(mac(&scope_key(store)?, &body)))
}

fn intent_path(store: &LogStore) -> PathBuf {
  store.root.join("cursor-session-intent.json")
}

fn read_intent(store: &LogStore, name: &str) -> Result<Option<String>> {
  let path = intent_path(store);
  match fs::symlink_metadata(&path) {
    Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
    Err(error) => return Err(error.into()),
    Ok(_) => {}
  }
  let intent: SessionIntent = serde_json::from_slice(&private_read(&path, 1024)?)?;
  ensure!(
    intent.schema_version == 1
      && intent.deployment == name
      && intent.session.starts_with('m')
      && token_valid(&intent.session, 64),
    "manager session intent invalid"
  );
  Ok(Some(intent.session))
}

fn write_intent(store: &LogStore, name: &str, session: &str) -> Result<()> {
  let intent = SessionIntent {
    schema_version: 1,
    deployment: name.to_owned(),
    session: session.to_owned(),
  };
  private_write(&intent_path(store), &serde_json::to_vec(&intent)?)
}

fn clear_intent(store: &LogStore) -> Result<()> {
  let path = intent_path(store);
  if path.exists() {
    fs::remove_file(path)?;
    File::open(&store.root)?.sync_all()?;
  }
  Ok(())
}

fn loss_total(store: &LogStore) -> u64 {
  store.losses.load(Ordering::Relaxed)
}

fn anchor_record_matches(store: &LogStore, anchor: &Anchor) -> Result<()> {
  let Some(segment) = segments(store)?
    .into_iter()
    .find(|segment| segment.id == anchor.segment)
  else {
    // The sanitized copy outlives ordinary rotation. Its original
    // segment may no longer be retained.
    return Ok(());
  };
  ensure!(
    anchor.after_offset <= segment.len,
    "anchor position exceeds segment"
  );
  let start = anchor
    .after_offset
    .saturating_sub(event_policy::MAX_STORED_LINE as u64);
  let mut file = File::open(&segment.path)?;
  file.seek(SeekFrom::Start(start))?;
  let mut bytes = vec![0; (anchor.after_offset - start) as usize];
  file.read_exact(&mut bytes)?;
  ensure!(
    bytes.last() == Some(&b'\n'),
    "anchor position is not a record boundary"
  );
  let begin = bytes[..bytes.len() - 1]
    .iter()
    .rposition(|byte| *byte == b'\n')
    .map_or(0, |index| index + 1);
  let accepted = event_policy::parse_stored(&bytes[begin..])?;
  ensure!(
    accepted == anchor.event,
    "anchor copy differs from accepted record"
  );
  Ok(())
}

fn anchor_seal(
  store: &LogStore,
  event: &Value,
  segment: &str,
  after_offset: u64,
) -> Result<String> {
  let body = serde_json::to_vec(&json!([event, segment, after_offset]))?;
  Ok(URL_SAFE_NO_PAD.encode(mac(&key(store)?, &body)))
}

fn typed_package_failure(code: &str) -> bool {
  matches!(
    code,
    "package_load_failed"
      | "package_unsafe_path"
      | "package_missing_file"
      | "package_invalid_descriptor"
      | "package_invalid_manifest"
      | "package_changed"
      | "package_platform_mismatch"
      | "package_invalid_sidecar"
      | "package_version_exists"
      | "package_digest_mismatch"
  )
}

fn validate_anchor(store: &LogStore, anchor: &Anchor, run: &str, manager: bool) -> Result<bool> {
  ensure!(
    anchor.segment.len() <= 96
      && anchor.segment.split('-').count() == 3
      && anchor
        .segment
        .split('-')
        .all(|part| !part.is_empty() && part.bytes().all(|byte| byte.is_ascii_hexdigit())),
    "anchor segment identity"
  );
  ensure!(
    anchor.after_offset > 0 && anchor.after_offset <= 100_000_000,
    "anchor accepted position"
  );
  event_policy::validate_stored(&anchor.event)?;
  ensure!(
    anchor.event["run_id"].as_str() == Some(run),
    "anchor scope mismatch"
  );
  let source = anchor.event["source"].as_str().context("anchor source")?;
  let code = anchor.event["event_code"].as_str().context("anchor code")?;
  let level = anchor.event["level"].as_str().context("anchor level")?;
  let eligible = if manager {
    level == "error"
      && ((source == "packages" && typed_package_failure(code))
        || (source == "isos" && code == "deployment_preflight_failed"))
  } else {
    (level == "info"
      && matches!(
        (source, code),
        ("isos", "deployment_ready")
          | ("eos", "eos_ready")
          | ("bridge", "bridge_ready")
          | ("config", "config_ready")
          | ("egia", "egia_ready")
      ))
      || (level == "error"
        && (matches!(source, "eos" | "bridge" | "config" | "egia")
          || (source == "isos" && code == "deployment_failed")))
  };
  ensure!(eligible, "anchor event is not eligible for scope");
  // A rotated or unreadable cursor key invalidates the anchor proof and
  // old page tokens, but it does not revoke the separately durable run ID.
  if !anchor_seal(store, &anchor.event, &anchor.segment, anchor.after_offset)
    .is_ok_and(|expected| anchor.seal == expected)
  {
    return Ok(false);
  }
  anchor_record_matches(store, anchor)?;
  Ok(true)
}

fn validate_state(store: &LogStore, state: &State, name: &str) -> Result<bool> {
  ensure!(
    state.schema_version == 1 && state.deployment == name,
    "cursor state identity"
  );
  ensure!(
    state.manager_session.starts_with('m') && token_valid(&state.manager_session, 64),
    "cursor manager identity"
  );
  ensure!(
    state
      .last_run_id
      .as_deref()
      .is_none_or(|id| id.starts_with('r') && token_valid(id, 64)),
    "cursor run identity"
  );
  ensure!(
    state.never_run == state.last_run_id.is_none(),
    "cursor run marker mismatch"
  );
  ensure!(
    matches!(
      state.run_anchor_state.as_str(),
      "pending" | "found" | "unavailable"
    ),
    "cursor anchor state"
  );
  if state.run_anchor_state == "found" {
    ensure!(state.run_anchor.is_some(), "cursor anchor missing");
  } else {
    ensure!(state.run_anchor.is_none(), "cursor anchor state mismatch");
  }
  let mut anchor_proven = true;
  if let Some(anchor) = state.manager_anchor.as_ref() {
    ensure!(
      state.last_run_id.is_none(),
      "manager anchor after run admission"
    );
    anchor_proven &= validate_anchor(store, anchor, &state.manager_session, true)?;
  }
  if let Some(anchor) = state.run_anchor.as_ref() {
    anchor_proven &= validate_anchor(
      store,
      anchor,
      state
        .last_run_id
        .as_deref()
        .context("run anchor without identity")?,
      false,
    )?;
  }
  Ok(anchor_proven)
}

fn save_state(store: &LogStore, state: &State) -> Result<()> {
  #[cfg(test)]
  if state.deployment == "p07-anchor-fail" && state.run_anchor_state == "found" {
    anyhow::bail!("injected anchor publication failure");
  }
  #[cfg(test)]
  if state.deployment == "p07-session-fail"
    && intent_path(store).exists()
    && state.manager_session == crate::runtime::manager::manager_session_id()
  {
    anyhow::bail!("injected manager-session state publication failure");
  }
  let mut sealed = state.clone();
  sealed.scope_seal = scope_seal(store, &sealed)?;
  private_write(&state_path(store), &serde_json::to_vec(&sealed)?)
}

pub(super) fn initialize(store: &LogStore, fresh_root: bool) -> Result<()> {
  if !fresh_root {
    return Ok(());
  }
  create_scope_key(store)?;
  let name = deployment(store)?;
  let state = State {
    schema_version: 1,
    deployment: name,
    manager_session: crate::runtime::manager::manager_session_id().to_owned(),
    never_run: true,
    last_run_id: None,
    manager_anchor: None,
    run_anchor: None,
    run_anchor_state: "pending".into(),
    loss_baseline: loss_total(store),
    loss_uncertain: false,
    scope_seal: String::new(),
  };
  save_state(store, &state)?;
  let _ = key(store)?;
  Ok(())
}

fn segments(store: &LogStore) -> Result<Vec<Segment>> {
  let mut result = Vec::new();
  for backup in (1..=store.backups).rev() {
    let path = store.root.join(format!("events.log.{backup}"));
    if path.exists() {
      result.push(Segment::at(path)?);
    }
  }
  result.push(Segment::at(store.root.join("events.log"))?);
  Ok(result)
}

#[derive(Clone)]
struct Segment {
  path: PathBuf,
  id: String,
  len: u64,
}

impl Segment {
  fn at(path: PathBuf) -> Result<Self> {
    let meta = fs::symlink_metadata(&path)?;
    ensure!(meta.file_type().is_file(), "log segment is not regular");
    Ok(Self {
      id: identity(&path)?,
      len: meta.len(),
      path,
    })
  }
}

fn load_state(store: &LogStore) -> Result<LoadedState> {
  let name = deployment(store)?;
  let bytes = private_read(&state_path(store), STATE_MAX)?;
  let state: State = serde_json::from_slice(&bytes)?;
  ensure!(
    state.scope_seal == scope_seal(store, &state)?,
    "cursor scope proof mismatch"
  );
  let anchor_proven = validate_state(store, &state, &name)?;
  if let Some(intent) = read_intent(store, &name)? {
    ensure!(
      intent == state.manager_session,
      "manager session publication pending"
    );
  }
  Ok(LoadedState {
    state,
    anchor_proven,
  })
}

pub(super) fn manager_scope_id(store: &LogStore) -> Option<String> {
  let _guard = store.lock.lock().unwrap();
  load_state(store)
    .ok()
    .map(|loaded| loaded.state.manager_session)
}

pub(super) fn begin_manager_session(store: &LogStore) -> Result<()> {
  let _guard = store.lock.lock().unwrap();
  let Ok(loaded) = load_state(store) else {
    // An upgraded P06 log directory has no proven prior identity. Keep
    // reads closed while allowing this manager to admit a new run.
    return Ok(());
  };
  let mut state = loaded.state;
  if state.manager_session != crate::runtime::manager::manager_session_id() {
    write_intent(
      store,
      &state.deployment,
      crate::runtime::manager::manager_session_id(),
    )?;
    state.manager_session = crate::runtime::manager::manager_session_id().to_owned();
    state.manager_anchor = None;
    if state.last_run_id.is_none() {
      state.loss_baseline = loss_total(store);
    } else if state.run_anchor_state == "pending" || !loaded.anchor_proven {
      // A prior manager published an admitted run but never durably
      // collected its first eligible result. A restart cannot keep
      // such a page pending forever.
      state.run_anchor_state = "unavailable".into();
      state.run_anchor = None;
    }
    if state.last_run_id.is_some() {
      // P06's process counters reset with the manager. Without an
      // accepted durable loss total, a zero after restart is unknown.
      state.loss_uncertain = true;
    }
    save_state(store, &state)?;
    clear_intent(store)?;
  }
  Ok(())
}

pub(super) fn status(store: &LogStore) -> (Option<String>, &'static str) {
  let _guard = store.lock.lock().unwrap();
  match load_state(store) {
    Ok(loaded) => match loaded.state.last_run_id {
      Some(run) => (Some(run), "known"),
      None => (None, "none"),
    },
    Err(_) => (None, "unavailable"),
  }
}

pub(super) fn publish_run(store: &LogStore, run_id: &str) -> Result<()> {
  ensure!(
    run_id.starts_with('r') && token_valid(run_id, 64),
    "invalid deployment run ID"
  );
  let _guard = store.lock.lock().unwrap();
  let mut state = match load_state(store) {
    Ok(loaded) => loaded.state,
    Err(_error)
      if fs::symlink_metadata(state_path(store))
        .is_err_and(|cause| cause.kind() == std::io::ErrorKind::NotFound)
        && fs::symlink_metadata(scope_key_path(store))
          .is_err_and(|cause| cause.kind() == std::io::ErrorKind::NotFound) =>
    {
      // An older P06 log directory has no P07 never-run marker. It
      // cannot be read as manager scope, but a newly admitted run can
      // establish a durable identity and a new history proof epoch.
      create_scope_key(store)?;
      let name = deployment(store)?;
      write_intent(store, &name, crate::runtime::manager::manager_session_id())?;
      State {
        schema_version: 1,
        deployment: name,
        manager_session: crate::runtime::manager::manager_session_id().to_owned(),
        never_run: false,
        last_run_id: Some(run_id.to_owned()),
        manager_anchor: None,
        run_anchor: None,
        run_anchor_state: "pending".into(),
        loss_baseline: loss_total(store),
        loss_uncertain: false,
        scope_seal: String::new(),
      }
    }
    Err(error) => return Err(error),
  };
  state.never_run = false;
  state.last_run_id = Some(run_id.to_owned());
  state.manager_anchor = None;
  state.run_anchor = None;
  state.run_anchor_state = "pending".into();
  state.loss_baseline = loss_total(store);
  #[cfg(test)]
  if state.deployment == "p07-publish-fail" {
    anyhow::bail!("injected cursor-state publication failure");
  }
  let saved = save_state(store, &state);
  #[cfg(test)]
  let saved = if saved.is_ok() && state.deployment == "p07-publish-uncertain" {
    Err(anyhow::anyhow!(
      "injected post-rename publication uncertainty"
    ))
  } else {
    saved
  };
  let saved = saved.and_then(|()| clear_intent(store));
  if saved.is_err()
    && let Ok(bytes) = private_read(&state_path(store), STATE_MAX)
    && let Ok(mut survivor) = serde_json::from_slice::<State>(&bytes)
    && survivor.last_run_id.as_deref() == Some(run_id)
  {
    survivor.run_anchor_state = "unavailable".into();
    let _ = save_state(store, &survivor);
  }
  saved
}

pub(super) fn record_accept(store: &LogStore, event: &Value, apply_failure: bool) -> Result<()> {
  let run_id = event["run_id"].as_str().unwrap_or("");
  let code = event["event_code"].as_str().unwrap_or("");
  let source = event["source"].as_str().unwrap_or("");
  let level = event["level"].as_str().unwrap_or("");
  let ready = level == "info"
    && matches!(
      (source, code),
      ("isos", "deployment_ready")
        | ("eos", "eos_ready")
        | ("bridge", "bridge_ready")
        | ("config", "config_ready")
        | ("egia", "egia_ready")
    );
  let run_failure = level == "error"
    && (matches!(source, "eos" | "bridge" | "config" | "egia")
      || (source == "isos" && code == "deployment_failed"));
  if !(level == "error"
    && (source == "packages" || matches!(source, "isos" | "eos" | "bridge" | "config" | "egia")))
    && !ready
    && !apply_failure
  {
    return Ok(());
  }
  let loaded = load_state(store)?;
  // A later event cannot replace an unprovable copy of the first event.
  if !loaded.anchor_proven {
    return Ok(());
  }
  let mut state = loaded.state;
  let manager_eligible = state.last_run_id.is_none()
    && state.manager_anchor.is_none()
    && run_id == state.manager_session
    && level == "error"
    && ((source == "packages" && typed_package_failure(code))
      || (source == "isos" && apply_failure && code == "deployment_preflight_failed"));
  let run_eligible = state.last_run_id.as_deref() == Some(run_id)
    && state.run_anchor_state == "pending"
    && (ready || run_failure);
  if !manager_eligible && !run_eligible {
    return Ok(());
  }
  event_policy::validate_stored(event)?;
  let path = store.root.join("events.log");
  let segment = identity(&path)?;
  let after_offset = fs::metadata(&path)?.len();
  let anchor = Anchor {
    event: event.clone(),
    seal: anchor_seal(store, event, &segment, after_offset)?,
    segment,
    after_offset,
  };
  if manager_eligible {
    state.manager_anchor = Some(anchor);
  } else {
    state.run_anchor = Some(anchor);
    state.run_anchor_state = "found".into();
  }
  save_state(store, &state)
}

fn key(store: &LogStore) -> Result<[u8; 32]> {
  let path = store.root.join("cursor-key");
  if let Ok(bytes) = private_read(&path, 32)
    && bytes.len() == 32
  {
    let mut result = [0_u8; 32];
    result.copy_from_slice(&bytes);
    return Ok(result);
  }
  // A missing, malformed or unreadable owner-only key rotates the token
  // namespace. The state reader keeps its durable scope ID and treats old
  // anchor seals as unprovable. Replacement is atomic and private.
  let mut random = [0_u8; 32];
  File::open("/dev/urandom")?.read_exact(&mut random)?;
  private_write(&path, &random)?;
  Ok(random)
}

fn mac(key: &[u8; 32], data: &[u8]) -> [u8; 32] {
  let mut inner_pad = [0x36_u8; 64];
  let mut outer_pad = [0x5c_u8; 64];
  for (index, byte) in key.iter().enumerate() {
    inner_pad[index] ^= byte;
    outer_pad[index] ^= byte;
  }
  let mut inner = Sha256::new();
  inner.update(inner_pad);
  inner.update(data);
  let inner = inner.finalize();
  let mut outer = Sha256::new();
  outer.update(outer_pad);
  outer.update(inner);
  outer.finalize().into()
}

fn encode_token(store: &LogStore, token: &Token) -> Result<String> {
  let body = URL_SAFE_NO_PAD.encode(serde_json::to_vec(token)?);
  let signature = URL_SAFE_NO_PAD.encode(mac(&key(store)?, body.as_bytes()));
  let encoded = format!("lc1.{body}.{signature}");
  ensure!(encoded.len() <= 512, "log cursor exceeds bound");
  Ok(encoded)
}

fn decode_token(store: &LogStore, raw: &str) -> Result<Token, CurrentError> {
  if raw.len() > 512 || !raw.is_ascii() {
    return Err(cursor_invalid());
  }
  let parts: Vec<_> = raw.split('.').collect();
  if parts.len() != 3 || parts[0] != "lc1" {
    return Err(cursor_invalid());
  }
  let expected = mac(
    &key(store).map_err(|_| cursor_invalid())?,
    parts[1].as_bytes(),
  );
  let actual = URL_SAFE_NO_PAD
    .decode(parts[2])
    .map_err(|_| cursor_invalid())?;
  if actual.len() != 32
    || URL_SAFE_NO_PAD.encode(&actual) != parts[2]
    || actual
      .iter()
      .zip(expected)
      .fold(0_u8, |acc, (a, b)| acc | (a ^ b))
      != 0
  {
    return Err(cursor_invalid());
  }
  let bytes = URL_SAFE_NO_PAD
    .decode(parts[1])
    .map_err(|_| cursor_invalid())?;
  if URL_SAFE_NO_PAD.encode(&bytes) != parts[1] {
    return Err(cursor_invalid());
  }
  serde_json::from_slice(&bytes).map_err(|_| cursor_invalid())
}

fn filter_hash(source: &str, level: &str, node: Option<&str>, correlation: Option<&str>) -> String {
  let bytes = serde_json::to_vec(&json!([source, level, node, correlation])).unwrap_or_default();
  hex::encode(Sha256::digest(bytes))
}

fn gap(code: &str, count: Option<u64>) -> Value {
  json!({"code":code,"count":count})
}

pub struct CurrentQuery<'a> {
  pub source: &'a str,
  pub level: &'a str,
  pub node_path: Option<&'a str>,
  pub correlation_id: Option<&'a str>,
  pub cursor: Option<&'a str>,
  pub limit: usize,
  pub active_revision: Option<&'a str>,
}

pub(super) fn read_current(
  store: &LogStore,
  query: CurrentQuery<'_>,
) -> Result<Value, CurrentError> {
  if query.source != "all" && !source_valid(query.source) {
    return Err(CurrentError::new(
      "log_source_invalid",
      "/query/source",
      "Log source invalid",
      400,
    ));
  }
  let rank = level_rank(query.level).ok_or_else(|| {
    CurrentError::new(
      "log_level_invalid",
      "/query/level",
      "Log level invalid",
      400,
    )
  })?;
  if !(1..=200).contains(&query.limit) {
    return Err(CurrentError::new(
      "log_limit_invalid",
      "/query/limit",
      "Log limit invalid",
      400,
    ));
  }
  if let Some(path) = query.node_path
    && !["isos", "eos", "egia"]
      .into_iter()
      .any(|core| event_policy::node_pointer(path, core))
  {
    return Err(CurrentError::new(
      "log_node_path_invalid",
      "/query/node_path",
      "Log node path invalid",
      400,
    ));
  }
  if let Some(id) = query.correlation_id
    && !token_valid(id, 64)
  {
    return Err(CurrentError::new(
      "log_correlation_id_invalid",
      "/query/correlation_id",
      "Log correlation ID invalid",
      400,
    ));
  }
  let filters = filter_hash(
    query.source,
    query.level,
    query.node_path,
    query.correlation_id,
  );
  let _guard = store.lock.lock().unwrap();
  let loaded = load_state(store).map_err(|_| scope_error())?;
  let state = loaded.state;
  let anchor_proven = loaded.anchor_proven;
  let (kind, run_id, anchor_kind, anchor) = if let Some(run) = state.last_run_id.as_deref() {
    (
      "deployment_run",
      run,
      "first_ready_or_failure",
      state.run_anchor.as_ref().filter(|_| anchor_proven),
    )
  } else {
    (
      "manager_session",
      state.manager_session.as_str(),
      "first_prelaunch_failure",
      state.manager_anchor.as_ref().filter(|_| anchor_proven),
    )
  };
  let name = deployment(store).map_err(|_| scope_error())?;
  let segments = segments(store).map_err(|_| scope_error())?;
  let current = segments.last().ok_or_else(scope_error)?;
  let mut run_changed = false;
  let mut expired = false;
  let mut rotated = false;
  let mut old = None;
  if let Some(raw) = query.cursor {
    let parsed = decode_token(store, raw)?;
    if parsed.d != name {
      return Err(cursor_invalid());
    }
    if parsed.f != filters {
      return Err(CurrentError::new(
        "log_cursor_filter_mismatch",
        "/query/cursor",
        "Log cursor filters changed",
        400,
      ));
    }
    if parsed.k != kind || parsed.r != run_id {
      run_changed = true;
    } else {
      old = Some(parsed);
    }
  }
  let loss_now = loss_total(store);
  let mut gap_count = if state.loss_uncertain || !anchor_proven {
    None
  } else {
    old.as_ref().map_or_else(
      || loss_now.checked_sub(state.loss_baseline),
      |token| loss_now.checked_sub(token.l),
    )
  };
  let mut collector_gap = gap_count.is_none() || gap_count.is_some_and(|count| count > 0);
  let mut anchor_state = if !anchor_proven {
    "unavailable"
  } else if anchor.is_some() {
    "found"
  } else if state.last_run_id.is_some() {
    state.run_anchor_state.as_str()
  } else {
    "pending"
  };
  if anchor_state == "pending" && collector_gap {
    anchor_state = "unavailable";
  }
  let mut lines = Vec::new();
  let mut position = (current.id.clone(), current.len);
  if anchor_state == "found" {
    let pinned = anchor.expect("found anchor exists");
    let (start_segment, start_offset) = if let Some(token) = old.as_ref().filter(|token| !token.p) {
      (token.s.as_str(), token.o)
    } else {
      (pinned.segment.as_str(), pinned.after_offset)
    };
    let (start_index, mut offset) = if let Some(index) = segments
      .iter()
      .position(|segment| segment.id == start_segment && start_offset <= segment.len)
    {
      (index, start_offset)
    } else {
      if old.is_some() {
        expired = true;
        rotated = true;
      }
      (0, 0)
    };
    let mut remaining = SCAN_MAX;
    position = (segments[start_index].id.clone(), offset);
    for (index, segment) in segments.iter().enumerate().skip(start_index) {
      if index > start_index {
        offset = 0;
        rotated = true;
      }
      if remaining == 0 || lines.len() >= query.limit {
        break;
      }
      let mut file = File::open(&segment.path).map_err(|_| scope_error())?;
      file
        .seek(SeekFrom::Start(offset))
        .map_err(|_| scope_error())?;
      let mut reader = BufReader::new(file.take(remaining as u64));
      loop {
        if remaining == 0 || lines.len() >= query.limit {
          break;
        }
        let mut raw = Vec::new();
        let read = reader
          .read_until(b'\n', &mut raw)
          .map_err(|_| scope_error())?;
        if read == 0 {
          break;
        }
        remaining -= read;
        if raw.last() != Some(&b'\n') {
          if raw.len() >= event_policy::MAX_STORED_LINE {
            // An unterminated prefix this large cannot be a
            // valid P06 line. Advance across it over bounded
            // pages instead of trapping the cursor forever.
            collector_gap = true;
            gap_count = None;
            offset += read as u64;
            position = (segment.id.clone(), offset);
          }
          break;
        }
        offset += read as u64;
        position = (segment.id.clone(), offset);
        let value = match event_policy::parse_stored(&raw) {
          Ok(value) => value,
          _ => {
            collector_gap = true;
            gap_count = None;
            continue;
          }
        };
        if value["run_id"].as_str() != Some(run_id) {
          continue;
        }
        if query.source != "all" && value["source"].as_str() != Some(query.source) {
          continue;
        }
        if !value["level"]
          .as_str()
          .and_then(level_rank)
          .is_some_and(|level| level >= rank)
        {
          continue;
        }
        if query.node_path.is_some() && value["node_path"].as_str() != query.node_path {
          continue;
        }
        if query.correlation_id.is_some()
          && value["correlation_id"].as_str() != query.correlation_id
        {
          continue;
        }
        lines.push(value);
      }
    }
  } else if anchor_state == "pending" {
    position = (current.id.clone(), current.len);
  }
  let token = Token {
    d: name,
    k: kind.into(),
    r: run_id.into(),
    f: filters,
    s: position.0,
    o: position.1,
    p: anchor_state == "pending",
    l: loss_now,
  };
  let next_cursor = encode_token(store, &token).map_err(|_| scope_error())?;
  let mut diagnostics = Vec::new();
  if run_changed {
    diagnostics.push(gap("log_run_changed", None));
  }
  if expired {
    diagnostics.push(gap("log_cursor_expired", None));
  }
  if collector_gap {
    diagnostics.push(gap("log_collector_gap", gap_count));
  }
  if anchor_state == "unavailable" {
    diagnostics.push(gap("log_anchor_unavailable", None));
  }
  Ok(json!({
      "scope":{"kind":kind,"run_id":run_id,"active_revision":query.active_revision},
      "anchor":{"kind":anchor_kind,"state":anchor_state,"event":anchor.map(|pinned| &pinned.event)},
      "lines":lines,
      "next_cursor":next_cursor,
      "rotated":rotated,
      "diagnostics":diagnostics,
  }))
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::{app::config::Paths, runtime::logs::LogContext};
  use std::os::unix::fs::PermissionsExt;

  fn store(name: &str) -> (tempfile::TempDir, LogStore) {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    let store = LogStore::new(&paths, name).unwrap();
    (temp, store)
  }

  fn page(store: &LogStore, cursor: Option<&str>, source: &str) -> Value {
    store
      .read_current(CurrentQuery {
        source,
        level: "info",
        node_path: None,
        correlation_id: None,
        cursor,
        limit: 100,
        active_revision: None,
      })
      .unwrap()
  }

  fn append(store: &LogStore, run: &str, code: &str, level: &str) {
    store
      .append_event_context(
        "isos",
        level,
        code,
        "test",
        LogContext {
          run_id: Some(run),
          correlation_id: None,
          node_path: None,
          fields: json!({}),
        },
      )
      .unwrap();
  }

  #[test]
  fn manager_anchor_distinguishes_validate_from_apply_and_bypasses_filters() {
    let (_temp, store) = store("p07-manager-anchor");
    let manager = crate::runtime::manager::manager_session_id();
    store
      .append_event_context(
        "isos",
        "error",
        "deployment_preflight_failed",
        "validate",
        LogContext {
          run_id: Some(manager),
          correlation_id: None,
          node_path: None,
          fields: json!({}),
        },
      )
      .unwrap();
    assert_eq!(page(&store, None, "eos")["anchor"]["state"], "pending");
    store
      .append_apply_failure_context(
        "isos",
        "error",
        "deployment_preflight_failed",
        "apply",
        LogContext {
          run_id: Some(manager),
          correlation_id: None,
          node_path: None,
          fields: json!({}),
        },
      )
      .unwrap();
    let current = page(&store, None, "eos");
    assert_eq!(current["scope"]["kind"], "manager_session");
    assert_eq!(current["anchor"]["event"]["message"], "apply");
    assert_eq!(current["lines"].as_array().unwrap().len(), 0);
  }

  #[test]
  fn cursor_identity_and_key_are_private_files() {
    let (_temp, store) = store("p07-private");
    for name in ["cursor-state.json", "cursor-key", "scope-key"] {
      let metadata = fs::symlink_metadata(store.root.join(name)).unwrap();
      assert!(metadata.file_type().is_file());
      assert_eq!(metadata.permissions().mode() & 0o077, 0);
    }
    assert_eq!(
      fs::metadata(&store.root).unwrap().permissions().mode() & 0o077,
      0
    );
  }

  #[test]
  fn run_anchor_is_first_accepted_event_and_same_code_later_is_a_line() {
    let (_temp, store) = store("p07-run-anchor");
    let run = "r101";
    store.publish_run(run).unwrap();
    assert_eq!(page(&store, None, "all")["anchor"]["state"], "pending");
    append(&store, run, "deployment_ready", "info");
    append(&store, run, "deployment_ready", "info");
    let current = page(&store, None, "all");
    assert_eq!(current["anchor"]["event"]["event_code"], "deployment_ready");
    assert_eq!(current["lines"].as_array().unwrap().len(), 1);
    assert_eq!(current["lines"][0]["event_code"], "deployment_ready");
    let next = current["next_cursor"].as_str().unwrap();
    assert_eq!(
      page(&store, Some(next), "all")["lines"]
        .as_array()
        .unwrap()
        .len(),
      0
    );
  }

  #[test]
  fn stopped_run_keeps_identity_anchor_and_terminal_line() {
    let (_temp, store) = store("p07-stopped");
    let run = "r115";
    store.publish_run(run).unwrap();
    append(&store, run, "deployment_ready", "info");
    append(&store, run, "deployment_stopped", "info");
    assert_eq!(store.run_identity(), (Some(run.into()), "known"));
    let current = page(&store, None, "all");
    assert_eq!(current["anchor"]["event"]["event_code"], "deployment_ready");
    assert_eq!(current["lines"][0]["event_code"], "deployment_stopped");
  }

  #[test]
  fn cursor_authentication_filter_binding_and_run_change() {
    let (_temp, store) = store("p07-token");
    store.publish_run("r201").unwrap();
    append(&store, "r201", "deployment_ready", "info");
    let token = page(&store, None, "all")["next_cursor"]
      .as_str()
      .unwrap()
      .to_owned();
    let invalid = store
      .read_current(CurrentQuery {
        source: "all",
        level: "info",
        node_path: None,
        correlation_id: None,
        cursor: Some("123:456:789"),
        limit: 100,
        active_revision: None,
      })
      .unwrap_err();
    assert_eq!(invalid.code, "log_cursor_invalid");
    let mismatch = store
      .read_current(CurrentQuery {
        source: "eos",
        level: "info",
        node_path: None,
        correlation_id: None,
        cursor: Some(&token),
        limit: 100,
        active_revision: None,
      })
      .unwrap_err();
    assert_eq!(mismatch.code, "log_cursor_filter_mismatch");
    let mut tampered = token.clone().into_bytes();
    let signature_start = token.rfind('.').unwrap() + 1;
    tampered[signature_start] = if tampered[signature_start] == b'A' {
      b'B'
    } else {
      b'A'
    };
    let tampered = String::from_utf8(tampered).unwrap();
    assert_eq!(
      store
        .read_current(CurrentQuery {
          source: "all",
          level: "info",
          node_path: None,
          correlation_id: None,
          cursor: Some(&tampered),
          limit: 100,
          active_revision: None,
        })
        .unwrap_err()
        .code,
      "log_cursor_invalid"
    );
    store.publish_run("r202").unwrap();
    let changed = page(&store, Some(&token), "all");
    assert_eq!(changed["scope"]["run_id"], "r202");
    assert_eq!(changed["diagnostics"][0]["code"], "log_run_changed");
  }

  #[test]
  fn corrupt_state_never_guesses_from_two_retained_starting_events() {
    let (_temp, store) = store("p07-recovery");
    for run in ["r301", "r302"] {
      store.publish_run(run).unwrap();
      append(&store, run, "deployment_starting", "info");
      append(&store, run, "deployment_ready", "info");
    }
    fs::write(state_path(&store), b"broken").unwrap();
    assert_eq!(store.run_identity(), (None, "unavailable"));
    assert_eq!(
      store
        .read_current(CurrentQuery {
          source: "all",
          level: "info",
          node_path: None,
          correlation_id: None,
          cursor: None,
          limit: 100,
          active_revision: None,
        })
        .unwrap_err()
        .code,
      "log_scope_unavailable"
    );
  }

  #[test]
  fn admitted_second_run_without_start_marker_stays_unavailable_after_restart() {
    let (temp, store) = store("p07-missing-start");
    store.publish_run("r801").unwrap();
    append(&store, "r801", "deployment_starting", "info");
    append(&store, "r801", "deployment_ready", "info");
    store.publish_run("r802").unwrap();
    // Simulate the P06 starting append failing after durable admission.
    fs::write(state_path(&store), b"broken").unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    let restarted = LogStore::new(&paths, "p07-missing-start").unwrap();
    restarted.begin_manager_session().unwrap();
    assert_eq!(restarted.run_identity(), (None, "unavailable"));
    assert_eq!(
      restarted
        .read_current(CurrentQuery {
          source: "all",
          level: "info",
          node_path: None,
          correlation_id: None,
          cursor: None,
          limit: 100,
          active_revision: None,
        })
        .unwrap_err()
        .code,
      "log_scope_unavailable"
    );
  }

  #[test]
  fn semantically_corrupt_pinned_anchor_fails_closed() {
    let (_temp, store) = store("p07-bad-anchor");
    store.publish_run("r811").unwrap();
    append(&store, "r811", "deployment_ready", "info");
    let original: State =
      serde_json::from_slice(&private_read(&state_path(&store), STATE_MAX).unwrap()).unwrap();
    let mut bad = original.clone();
    bad.run_anchor.as_mut().unwrap().event["event_code"] = json!("legacy_external_event");
    let anchor = bad.run_anchor.as_mut().unwrap();
    anchor.seal = anchor_seal(&store, &anchor.event, &anchor.segment, anchor.after_offset).unwrap();
    save_state(&store, &bad).unwrap();
    assert_eq!(store.run_identity(), (None, "unavailable"));
    let mut bad = original.clone();
    bad.run_anchor.as_mut().unwrap().after_offset += 1;
    let anchor = bad.run_anchor.as_mut().unwrap();
    anchor.seal = anchor_seal(&store, &anchor.event, &anchor.segment, anchor.after_offset).unwrap();
    save_state(&store, &bad).unwrap();
    assert_eq!(store.run_identity(), (None, "unavailable"));
    let mut bad = original;
    bad.run_anchor.as_mut().unwrap().event["pid"] = json!("forged");
    let anchor = bad.run_anchor.as_mut().unwrap();
    anchor.seal = anchor_seal(&store, &anchor.event, &anchor.segment, anchor.after_offset).unwrap();
    save_state(&store, &bad).unwrap();
    assert_eq!(store.run_identity(), (None, "unavailable"));
  }

  #[test]
  fn cursor_key_loss_keeps_durable_run_but_invalidates_old_pages() {
    for (name, damage) in [
      ("p07-key-replaced", "replaced"),
      ("p07-key-deleted", "deleted"),
      ("p07-key-unreadable", "unreadable"),
    ] {
      let (temp, store) = store(name);
      let run = "r812";
      store.publish_run(run).unwrap();
      append(&store, run, "deployment_ready", "info");
      append(&store, run, "deployment_stopped", "info");
      let old = page(&store, None, "all")["next_cursor"]
        .as_str()
        .unwrap()
        .to_owned();
      let path = store.root.join("cursor-key");
      match damage {
        "replaced" => fs::write(&path, [17_u8; 32]).unwrap(),
        "deleted" => fs::remove_file(&path).unwrap(),
        "unreadable" => fs::set_permissions(&path, fs::Permissions::from_mode(0o000)).unwrap(),
        _ => unreachable!(),
      }

      let paths = Paths {
        config: temp.path().join("config"),
        data: temp.path().join("data"),
        runtime: temp.path().join("run"),
      };
      let restarted = LogStore::new(&paths, name).unwrap();
      restarted.begin_manager_session().unwrap();
      assert_eq!(restarted.run_identity(), (Some(run.into()), "known"));
      assert_eq!(
        restarted
          .read_current(CurrentQuery {
            source: "all",
            level: "info",
            node_path: None,
            correlation_id: None,
            cursor: Some(&old),
            limit: 100,
            active_revision: None,
          })
          .unwrap_err()
          .code,
        "log_cursor_invalid"
      );
      let fresh = page(&restarted, None, "all");
      assert_eq!(fresh["scope"]["run_id"], run);
      assert_eq!(fresh["anchor"]["state"], "unavailable");
      assert!(fresh["anchor"]["event"].is_null());
      assert!(fresh["lines"].as_array().unwrap().is_empty());
      assert_eq!(fresh["diagnostics"][0]["code"], "log_collector_gap");
      assert!(fresh["diagnostics"][0]["count"].is_null());
      assert_eq!(fresh["diagnostics"][1]["code"], "log_anchor_unavailable");
      assert!(fresh["next_cursor"].as_str().unwrap().starts_with("lc1."));
    }
  }

  #[test]
  fn tampered_run_and_anchor_cannot_claim_fabricated_scope() {
    let (temp, store) = store("p07-scope-tampered");
    store.publish_run("r812").unwrap();
    append(&store, "r812", "deployment_ready", "info");
    let mut state: State =
      serde_json::from_slice(&private_read(&state_path(&store), STATE_MAX).unwrap()).unwrap();
    state.last_run_id = Some("r999".into());
    state.run_anchor.as_mut().unwrap().event["run_id"] = json!("r999");
    fs::write(state_path(&store), serde_json::to_vec(&state).unwrap()).unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    let restarted = LogStore::new(&paths, "p07-scope-tampered").unwrap();
    restarted.begin_manager_session().unwrap();
    assert_eq!(restarted.run_identity(), (None, "unavailable"));
    assert_eq!(
      restarted
        .read_current(CurrentQuery {
          source: "all",
          level: "info",
          node_path: None,
          correlation_id: None,
          cursor: None,
          limit: 100,
          active_revision: None,
        })
        .unwrap_err()
        .code,
      "log_scope_unavailable"
    );
    assert!(restarted.publish_run("r1000").is_err());
  }

  #[test]
  fn missing_stable_scope_key_never_reissues_scope_proof() {
    let (_temp, store) = store("p07-scope-key-missing");
    store.publish_run("r813").unwrap();
    append(&store, "r813", "deployment_ready", "info");
    fs::remove_file(scope_key_path(&store)).unwrap();
    assert_eq!(store.run_identity(), (None, "unavailable"));
    assert!(!scope_key_path(&store).exists());
    assert!(store.publish_run("r814").is_err());
    assert!(!scope_key_path(&store).exists());
  }

  #[test]
  fn missing_state_with_existing_scope_key_is_not_p06_upgrade() {
    let (_temp, store) = store("p07-state-missing");
    store.publish_run("r815").unwrap();
    fs::remove_file(state_path(&store)).unwrap();
    assert_eq!(store.run_identity(), (None, "unavailable"));
    assert!(store.publish_run("r816").is_err());
    assert!(!state_path(&store).exists());
  }

  #[test]
  fn signed_but_ineligible_manager_package_anchor_fails_closed() {
    let (_temp, store) = store("p07-bad-package-anchor");
    let manager = crate::runtime::manager::manager_session_id();
    store
      .append_event_context(
        "packages",
        "error",
        "package_load_failed",
        "package load failed",
        LogContext {
          run_id: Some(manager),
          correlation_id: None,
          node_path: None,
          fields: json!({}),
        },
      )
      .unwrap();
    let mut state: State =
      serde_json::from_slice(&private_read(&state_path(&store), STATE_MAX).unwrap()).unwrap();
    let anchor = state.manager_anchor.as_mut().unwrap();
    anchor.event["event_code"] = json!("package_verified");
    anchor.seal = anchor_seal(&store, &anchor.event, &anchor.segment, anchor.after_offset).unwrap();
    save_state(&store, &state).unwrap();
    assert_eq!(store.run_identity(), (None, "unavailable"));
  }

  #[test]
  fn malformed_matching_json_line_is_skipped_with_collector_gap() {
    let (_temp, store) = store("p07-bad-line");
    let run = "r821";
    store.publish_run(run).unwrap();
    append(&store, run, "deployment_ready", "info");
    let malformed = json!({"run_id":run,"source":"isos","level":"info",
            "event_code":"deployment_stopped","message":"forged without P06 envelope"});
    let mut line = serde_json::to_vec(&malformed).unwrap();
    line.push(b'\n');
    OpenOptions::new()
      .append(true)
      .open(store.root.join("events.log"))
      .unwrap()
      .write_all(&line)
      .unwrap();
    let mut duplicate = store.read("all", None, 1).unwrap()["lines"][0].clone();
    duplicate["event_code"] = json!("deployment_stopped");
    let mut duplicate = serde_json::to_string(&duplicate).unwrap();
    duplicate.pop();
    duplicate.push_str(",\"run_id\":\"r821\"}\n");
    OpenOptions::new()
      .append(true)
      .open(store.root.join("events.log"))
      .unwrap()
      .write_all(duplicate.as_bytes())
      .unwrap();
    append(&store, run, "deployment_stopped", "info");
    let current = page(&store, None, "all");
    assert_eq!(current["lines"].as_array().unwrap().len(), 1);
    assert_eq!(current["lines"][0]["event_code"], "deployment_stopped");
    assert_eq!(current["diagnostics"][0]["code"], "log_collector_gap");
    assert!(current["diagnostics"][0]["count"].is_null());
  }

  #[test]
  fn overlong_corrupt_line_advances_across_bounded_pages() {
    let (_temp, store) = store("p07-oversize-line");
    let run = "r831";
    store.publish_run(run).unwrap();
    append(&store, run, "deployment_ready", "info");
    let mut raw = vec![b'x'; 70_000];
    raw.push(b'\n');
    OpenOptions::new()
      .append(true)
      .open(store.root.join("events.log"))
      .unwrap()
      .write_all(&raw)
      .unwrap();
    append(&store, run, "deployment_stopped", "info");
    let first = page(&store, None, "all");
    assert!(first["lines"].as_array().unwrap().is_empty());
    assert_eq!(first["diagnostics"][0]["code"], "log_collector_gap");
    let second = page(&store, first["next_cursor"].as_str(), "all");
    assert_eq!(second["lines"][0]["event_code"], "deployment_stopped");
  }

  #[test]
  fn publication_failure_keeps_never_run_scope() {
    let (_temp, store) = store("p07-publish-fail");
    assert!(store.publish_run("r401").is_err());
    assert_eq!(store.run_identity(), (None, "none"));
  }

  #[test]
  fn older_log_directory_is_unavailable_until_a_new_run_is_published() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    let root = paths.data.join("logs").join("p07-upgrade");
    fs::create_dir_all(&root).unwrap();
    fs::write(root.join("events.log"), b"").unwrap();
    let store = LogStore::new(&paths, "p07-upgrade").unwrap();
    assert_eq!(store.run_identity(), (None, "unavailable"));
    store.publish_run("r451").unwrap();
    assert!(scope_key_path(&store).is_file());
    assert_eq!(store.run_identity(), (Some("r451".into()), "known"));
    assert_eq!(page(&store, None, "all")["anchor"]["state"], "pending");
  }

  #[test]
  fn uncertain_publication_keeps_surviving_new_run_and_unavailable_anchor() {
    let (_temp, store) = store("p07-publish-uncertain");
    assert!(store.publish_run("r475").is_err());
    assert_eq!(store.run_identity(), (Some("r475".into()), "known"));
    let current = page(&store, None, "all");
    assert_eq!(current["anchor"]["state"], "unavailable");
    assert_eq!(current["diagnostics"][0]["code"], "log_anchor_unavailable");
  }

  #[test]
  fn manager_restart_after_publication_does_not_leave_anchor_pending() {
    let (_temp, store) = store("p07-restart-pending");
    store.publish_run("r476").unwrap();
    let mut state: State =
      serde_json::from_slice(&private_read(&state_path(&store), STATE_MAX).unwrap()).unwrap();
    state.manager_session = "m-old-manager".into();
    save_state(&store, &state).unwrap();
    begin_manager_session(&store).unwrap();
    let current = page(&store, None, "all");
    assert_eq!(current["scope"]["run_id"], "r476");
    assert_eq!(current["anchor"]["state"], "unavailable");
    assert_eq!(current["diagnostics"][0]["code"], "log_collector_gap");
    assert_eq!(current["diagnostics"][1]["code"], "log_anchor_unavailable");
  }

  #[test]
  fn failed_manager_session_publication_latches_stale_scope_unavailable() {
    let (temp, store) = store("p07-session-fail");
    let mut state: State =
      serde_json::from_slice(&private_read(&state_path(&store), STATE_MAX).unwrap()).unwrap();
    state.manager_session = "m-old-manager".into();
    save_state(&store, &state).unwrap();
    assert!(store.begin_manager_session().is_err());
    assert!(intent_path(&store).is_file());
    assert_eq!(store.run_identity(), (None, "unavailable"));
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    let later = LogStore::new(&paths, "p07-session-fail").unwrap();
    assert_eq!(
      later
        .read_current(CurrentQuery {
          source: "all",
          level: "info",
          node_path: None,
          correlation_id: None,
          cursor: None,
          limit: 100,
          active_revision: None
        })
        .unwrap_err()
        .code,
      "log_scope_unavailable"
    );
  }

  #[test]
  fn anchor_write_failure_reports_gap_and_does_not_invent_ready() {
    let (_temp, store) = store("p07-anchor-fail");
    store.publish_run("r501").unwrap();
    append(&store, "r501", "deployment_ready", "info");
    let current = page(&store, None, "all");
    assert_eq!(current["anchor"]["state"], "unavailable");
    assert!(current["anchor"]["event"].is_null());
    assert_eq!(current["lines"].as_array().unwrap().len(), 0);
    assert_eq!(current["diagnostics"][0]["code"], "log_collector_gap");
    assert_eq!(current["diagnostics"][1]["code"], "log_anchor_unavailable");
  }

  #[test]
  fn retained_rotation_continues_and_expired_position_reports_gap() {
    let (_temp, mut store) = store("p07-rotation");
    store.max_bytes = 65_536;
    store.backups = 1;
    let run = "r601";
    store.publish_run(run).unwrap();
    append(&store, run, "deployment_ready", "info");
    let first = page(&store, None, "all");
    let old = first["next_cursor"].as_str().unwrap().to_owned();
    for _ in 0..17 {
      store
        .append_event_context(
          "isos",
          "info",
          "deployment_starting",
          &"x".repeat(5_000),
          LogContext {
            run_id: Some(run),
            correlation_id: None,
            node_path: None,
            fields: json!({}),
          },
        )
        .unwrap();
    }
    let retained = page(&store, Some(&old), "all");
    assert_eq!(retained["rotated"], true);
    assert!(!retained["lines"].as_array().unwrap().is_empty());
    assert!(retained["lines"].as_array().unwrap().len() < 17);
    assert!(
      !retained["diagnostics"]
        .as_array()
        .unwrap()
        .iter()
        .any(|item| item["code"] == "log_cursor_expired")
    );
    for _ in 0..32 {
      store
        .append_event_context(
          "isos",
          "info",
          "deployment_starting",
          &"y".repeat(5_000),
          LogContext {
            run_id: Some(run),
            correlation_id: None,
            node_path: None,
            fields: json!({}),
          },
        )
        .unwrap();
    }
    let expired = page(&store, Some(&old), "all");
    assert_eq!(expired["rotated"], true);
    assert_eq!(expired["diagnostics"][0]["code"], "log_cursor_expired");
  }

  #[test]
  fn filtered_empty_page_advances_after_anchor_within_scan_bound() {
    let (_temp, store) = store("p07-filter-progress");
    let run = "r701";
    store.publish_run(run).unwrap();
    append(&store, run, "deployment_ready", "info");
    for _ in 0..20 {
      store
        .append_event_context(
          "isos",
          "info",
          "deployment_starting",
          &"z".repeat(5_000),
          LogContext {
            run_id: Some(run),
            correlation_id: None,
            node_path: None,
            fields: json!({}),
          },
        )
        .unwrap();
    }
    let first = page(&store, None, "eos");
    assert_eq!(first["anchor"]["event"]["event_code"], "deployment_ready");
    assert!(first["lines"].as_array().unwrap().is_empty());
    let first_token = first["next_cursor"].as_str().unwrap();
    let second = page(&store, Some(first_token), "eos");
    assert!(second["lines"].as_array().unwrap().is_empty());
    assert_ne!(second["next_cursor"].as_str().unwrap(), first_token);
  }
}
