//! Bounded, owner-only per-deployment logs and cursor reads.

pub(crate) mod manager_log;
use crate::app::config::Paths;
use crate::runtime::owners::event_policy;
mod cursor;
use anyhow::{Context, Result, ensure};
use chrono::{SecondsFormat, Utc};
pub use cursor::{CurrentError, CurrentQuery};
use serde_json::{Value, json};
use std::{
  collections::{BTreeMap, BTreeSet, VecDeque},
  fs::{self, File, OpenOptions},
  io::{BufRead, BufReader, Read, Seek, SeekFrom, Write},
  os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt},
  path::{Path, PathBuf},
  sync::{
    Arc, Mutex, OnceLock,
    atomic::{AtomicU64, Ordering},
    mpsc::{self, Receiver},
  },
  time::{Duration, Instant},
};

const DEFAULT_MAX_BYTES: u64 = 262_144;
const DEFAULT_BACKUPS: u8 = 2;
const DEFAULT_MAX_SOURCES: u16 = 32;
const MAX_READ_BYTES: u64 = 65_536;
static INGEST_REJECTED: AtomicU64 = AtomicU64::new(0);
static WRITE_FAILED: AtomicU64 = AtomicU64::new(0);
static REJECTION_REASONS: OnceLock<Mutex<BTreeMap<&'static str, u64>>> = OnceLock::new();
static DEPLOYMENT_LOSSES: OnceLock<Mutex<BTreeMap<PathBuf, Arc<AtomicU64>>>> = OnceLock::new();

fn saturating_increment(counter: &AtomicU64) {
  let mut current = counter.load(Ordering::Relaxed);
  loop {
    let next = current.saturating_add(1);
    match counter.compare_exchange_weak(current, next, Ordering::Relaxed, Ordering::Relaxed) {
      Ok(_) => return,
      Err(observed) => current = observed,
    }
  }
}

fn reject(store: &LogStore, reason: &'static str) {
  saturating_increment(&INGEST_REJECTED);
  saturating_increment(&store.losses);
  let mut reasons = REJECTION_REASONS
    .get_or_init(|| Mutex::new(BTreeMap::new()))
    .lock()
    .unwrap();
  let count = reasons.entry(reason).or_default();
  *count = count.saturating_add(1);
}

pub fn rejection_reasons() -> Value {
  let reasons = REJECTION_REASONS
    .get_or_init(|| Mutex::new(BTreeMap::new()))
    .lock()
    .unwrap();
  json!(&*reasons)
}

pub fn counters() -> (u64, u64) {
  (
    INGEST_REJECTED.load(Ordering::Relaxed),
    WRITE_FAILED.load(Ordering::Relaxed),
  )
}

/// Account for a stop event that could not enter the bounded log queue.
/// This must remain free of filesystem calls on the stop response path.
pub fn record_stop_event_loss() {
  saturating_increment(&WRITE_FAILED);
}

pub struct LogContext<'a> {
  pub run_id: Option<&'a str>,
  pub correlation_id: Option<&'a str>,
  pub node_path: Option<&'a str>,
  pub fields: Value,
}

#[derive(Clone)]
pub struct LogStore {
  root: PathBuf,
  max_bytes: u64,
  backups: u8,
  max_sources: u16,
  level: u8,
  lock: Arc<Mutex<()>>,
  suppression: Arc<Mutex<BTreeMap<String, (Instant, u32)>>>,
  losses: Arc<AtomicU64>,
}

fn token_valid(value: &str, max: usize) -> bool {
  !value.is_empty()
    && value.len() <= max
    && value
      .bytes()
      .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
}

fn source_valid(source: &str) -> bool {
  token_valid(source, 48)
}

fn event_code_valid(code: &str) -> bool {
  !code.is_empty()
    && code.len() <= 64
    && code
      .bytes()
      .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
}

pub fn valid_source(source: &str) -> bool {
  source_valid(source) && !matches!(source, "all" | "services" | "events")
}

fn shared_lock() -> Arc<Mutex<()>> {
  static LOCK: OnceLock<Arc<Mutex<()>>> = OnceLock::new();
  LOCK.get_or_init(|| Arc::new(Mutex::new(()))).clone()
}

fn shared_losses(root: &Path) -> Arc<AtomicU64> {
  DEPLOYMENT_LOSSES
    .get_or_init(|| Mutex::new(BTreeMap::new()))
    .lock()
    .unwrap()
    .entry(root.to_path_buf())
    .or_insert_with(|| Arc::new(AtomicU64::new(0)))
    .clone()
}

pub fn level_rank(level: &str) -> Option<u8> {
  match level {
    "trace" => Some(0),
    "debug" => Some(1),
    "info" => Some(2),
    "warn" => Some(3),
    "error" => Some(4),
    _ => None,
  }
}

fn private_dir(path: &Path) -> Result<()> {
  fs::create_dir_all(path)?;
  fs::set_permissions(path, fs::Permissions::from_mode(0o700))?;
  Ok(())
}

impl LogStore {
  pub fn new(paths: &Paths, deployment: &str) -> Result<Self> {
    Self::try_new(paths, deployment).inspect_err(|_| saturating_increment(&WRITE_FAILED))
  }

  fn try_new(paths: &Paths, deployment: &str) -> Result<Self> {
    ensure!(source_valid(deployment), "invalid deployment for logs");
    let max_bytes = std::env::var("ISOS_LOG_MAX_BYTES")
      .ok()
      .map(|v| v.parse())
      .transpose()?
      .unwrap_or(DEFAULT_MAX_BYTES);
    let backups = std::env::var("ISOS_LOG_BACKUPS")
      .ok()
      .map(|v| v.parse())
      .transpose()?
      .unwrap_or(DEFAULT_BACKUPS);
    let max_sources = std::env::var("ISOS_LOG_MAX_SOURCES")
      .ok()
      .map(|v| v.parse())
      .transpose()?
      .unwrap_or(DEFAULT_MAX_SOURCES);
    let level = std::env::var("ISOS_LOG_LEVEL").unwrap_or_else(|_| "info".into());
    let level =
      level_rank(&level).context("ISOS_LOG_LEVEL must be trace, debug, info, warn, or error")?;
    ensure!(
      (65_536..=100_000_000).contains(&max_bytes),
      "ISOS_LOG_MAX_BYTES must be 65536..100000000"
    );
    ensure!(
      (1..=10).contains(&backups),
      "ISOS_LOG_BACKUPS must be 1..10"
    );
    ensure!(
      (10..=256).contains(&max_sources),
      "ISOS_LOG_MAX_SOURCES must be 10..256"
    );
    let root = paths.data.join("logs").join(deployment);
    let fresh_root = !root.exists();
    private_dir(&root)?;
    for source in [
      "events", "isos", "packages", "graph", "ui", "eos", "egia", "bridge", "config", "services",
    ] {
      let file = OpenOptions::new()
        .create(true)
        .append(true)
        .mode(0o600)
        .open(root.join(format!("{source}.log")))?;
      file.set_permissions(fs::Permissions::from_mode(0o600))?;
    }
    let store = Self {
      root,
      max_bytes,
      backups,
      max_sources,
      level,
      lock: shared_lock(),
      suppression: Arc::new(Mutex::new(BTreeMap::new())),
      losses: shared_losses(&paths.data.join("logs").join(deployment)),
    };
    if cursor::initialize(&store, fresh_root).is_err() {
      saturating_increment(&WRITE_FAILED);
      saturating_increment(&store.losses);
    }
    Ok(store)
  }

  pub fn root(&self) -> &Path {
    &self.root
  }

  pub fn publish_run(&self, run_id: &str) -> Result<()> {
    cursor::publish_run(self, run_id)
  }

  pub fn run_identity(&self) -> (Option<String>, &'static str) {
    cursor::status(self)
  }

  pub fn manager_scope_id(&self) -> Option<String> {
    cursor::manager_scope_id(self)
  }

  pub fn begin_manager_session(&self) -> Result<()> {
    cursor::begin_manager_session(self)
  }

  pub fn read_current(&self, query: CurrentQuery<'_>) -> std::result::Result<Value, CurrentError> {
    cursor::read_current(self, query)
  }

  fn path(&self, source: &str) -> Result<PathBuf> {
    ensure!(source_valid(source), "invalid log source");
    Ok(self.root.join(format!(
      "{}.log",
      if matches!(source, "all" | "services") {
        "events"
      } else {
        source
      }
    )))
  }

  fn append_file(&self, path: &Path, line: &[u8]) -> Result<()> {
    let old_len = fs::metadata(path).map(|m| m.len()).unwrap_or(0);
    if old_len.saturating_add(line.len() as u64) > self.max_bytes {
      for index in (1..=self.backups).rev() {
        let dest = path.with_extension(format!("log.{index}"));
        let src = if index == 1 {
          path.to_path_buf()
        } else {
          path.with_extension(format!("log.{}", index - 1))
        };
        if dest.exists() {
          fs::remove_file(&dest)?;
        }
        if src.exists() {
          fs::rename(src, dest)?;
        }
      }
    }
    let mut file = OpenOptions::new()
      .create(true)
      .append(true)
      .mode(0o600)
      .open(path)?;
    file.set_permissions(fs::Permissions::from_mode(0o600))?;
    file.write_all(line)?;
    Ok(())
  }

  fn source_file_available(&self, path: &Path) -> Result<bool> {
    if path.exists() {
      return Ok(true);
    }
    let source_count = fs::read_dir(&self.root)?
      .filter_map(|entry| entry.ok())
      .filter(|entry| {
        entry
          .path()
          .extension()
          .is_some_and(|extension| extension == "log")
      })
      .count();
    Ok(source_count < usize::from(self.max_sources))
  }

  pub fn append(&self, source: &str, level: &str, message: &str) -> Result<()> {
    self.append_event(source, level, "message", message)
  }

  pub fn append_event(
    &self,
    source: &str,
    level: &str,
    event_code: &str,
    message: &str,
  ) -> Result<()> {
    self.append_event_context(
      source,
      level,
      event_code,
      message,
      LogContext {
        run_id: None,
        correlation_id: None,
        node_path: None,
        fields: json!({}),
      },
    )
  }

  pub fn append_event_context(
    &self,
    source: &str,
    level: &str,
    event_code: &str,
    message: &str,
    context: LogContext<'_>,
  ) -> Result<()> {
    self.append_event_context_marked(source, level, event_code, message, context, false)
  }

  pub fn append_apply_failure_context(
    &self,
    source: &str,
    level: &str,
    event_code: &str,
    message: &str,
    context: LogContext<'_>,
  ) -> Result<()> {
    self.append_event_context_marked(source, level, event_code, message, context, true)
  }

  fn append_event_context_marked(
    &self,
    source: &str,
    level: &str,
    event_code: &str,
    message: &str,
    context: LogContext<'_>,
    apply_failure: bool,
  ) -> Result<()> {
    let LogContext {
      run_id,
      correlation_id,
      node_path,
      fields,
    } = context;
    ensure!(valid_source(source), "invalid log source");
    ensure!(event_code_valid(event_code), "invalid log event code");
    for id in [run_id, correlation_id].into_iter().flatten() {
      ensure!(token_valid(id, 64), "invalid log correlation ID");
    }
    ensure!(
      node_path.is_none_or(|path| ["isos", "eos", "egia"]
        .into_iter()
        .any(|core| event_policy::node_pointer(path, core))),
      "invalid log node path"
    );
    let object = fields.as_object().context("log fields must be an object")?;
    ensure!(object.len() <= 12, "too many log fields");
    ensure!(
      serde_json::to_vec(&fields)?.len() <= 4096,
      "log fields too large"
    );
    for (key, value) in object {
      ensure!(
        source_valid(key) && key.len() <= 48,
        "invalid log field name"
      );
      ensure!(
        value.is_null()
          || value.is_boolean()
          || value.is_number()
          || value
            .as_str()
            .is_some_and(|s| s.len() <= 256 && !s.chars().any(char::is_control)),
        "invalid log field value"
      );
    }
    let rank = level_rank(level).context("invalid log level")?;
    if rank < self.level
      && !matches!(
        event_code,
        "deployment_starting"
          | "deployment_stopping"
          | "deployment_ready"
          | "deployment_failed"
          | "deployment_stopped"
          | "child_restart_attempt"
          | "child_restart_ready"
          | "package_load_failed"
          | "package_verified"
          | "events_suppressed"
      )
    {
      return Ok(());
    }
    let _guard = self.lock.lock().unwrap();
    let path = self.path(source)?;
    let message = if message.len() > 8192 {
      &message[..message.floor_char_boundary(8192)]
    } else {
      message
    };
    let mut safe_message = message.to_owned();
    let value = loop {
      let candidate = json!({"timestamp":Utc::now().to_rfc3339_opts(SecondsFormat::Millis,true),"level":level,"source":source,
                "core":if source == "eos" || source == "bridge" || source == "config" { "eos" } else if source == "egia" { "egia" } else { "isos" },
                "node_path":node_path,"event_code":event_code,"message":safe_message,"pid":std::process::id(),
                "run_id":run_id.unwrap_or(crate::runtime::manager::manager_session_id()),"correlation_id":correlation_id,"fields":fields});
      if serde_json::to_vec(&candidate)?.len() < event_policy::MAX_STORED_LINE {
        break candidate;
      }
      ensure!(
        !safe_message.is_empty(),
        "log event exceeds stored line limit"
      );
      safe_message.pop();
    };
    self.append_record(&path, source, &value, apply_failure)
  }

  fn append_record(
    &self,
    path: &Path,
    source: &str,
    value: &Value,
    apply_failure: bool,
  ) -> Result<()> {
    let mut line = serde_json::to_vec(value)?;
    line.push(b'\n');
    ensure!(
      line.len() <= event_policy::MAX_STORED_LINE,
      "log event exceeds stored line limit"
    );
    let result = (|| {
      if self.source_file_available(path)? {
        self.append_file(path, &line)?;
      }
      if source != "events" {
        self.append_file(&self.root.join("events.log"), &line)?;
      }
      Ok(())
    })();
    if result.is_err() || cursor::record_accept(self, value, apply_failure).is_err() {
      saturating_increment(&WRITE_FAILED);
      saturating_increment(&self.losses);
    }
    result
  }

  pub fn append_selected(&self, value: &Value) -> Result<()> {
    let source = value["source"].as_str().context("selected event source")?;
    ensure!(
      matches!(source, "eos" | "bridge" | "config" | "egia"),
      "invalid selected source"
    );
    level_rank(value["level"].as_str().context("selected event level")?)
      .context("selected event level")?;
    let code = value["event_code"]
      .as_str()
      .context("selected event code")?;
    let run_id = value["run_id"].as_str().context("selected event run")?;
    let eligible = matches!(code, "egia_discovery_lost" | "egia_safety_rejected");
    let terminal = code.ends_with("_stopped")
      || code.ends_with("_start_failed")
      || code.ends_with("_terminal")
      || code == "egia_operation_unknown";
    // Serialize admission and the append. A failed first write must not
    // become the representative event for the next sixty seconds.
    let _guard = self.lock.lock().unwrap();
    let mut summaries = Vec::new();
    let mut suppress = false;
    let mut pending_key = None;
    {
      let mut table = self.suppression.lock().unwrap();
      let now = Instant::now();
      if eligible {
        let key = format!(
          "{run_id}|{source}|{}|{code}|{}|{}",
          value["node_path"].as_str().unwrap_or(""),
          value["correlation_id"].as_str().unwrap_or(""),
          value["fields"]["reason_code"].as_str().unwrap_or("")
        );
        if let Some((at, count)) = table.get_mut(&key) {
          if now.duration_since(*at) < Duration::from_secs(60) {
            *count = count.saturating_add(1);
            suppress = true;
          } else {
            if *count > 0 {
              summaries.push((source.to_owned(), code.to_owned(), *count));
            }
            pending_key = Some((key, now));
          }
        } else if table.len() < 256 {
          pending_key = Some((key, now));
        }
      }
      if terminal {
        for (key, (_, count)) in table.iter_mut() {
          if key.starts_with(&format!("{run_id}|")) && *count > 0 {
            let parts: Vec<_> = key.split('|').collect();
            summaries.push((parts[1].to_owned(), parts[3].to_owned(), *count));
            *count = 0;
          }
        }
      }
    }
    if suppress {
      return Ok(());
    }
    for (suppressed_source, suppressed_event_code, count) in summaries {
      let summary = json!({"timestamp":Utc::now().to_rfc3339_opts(SecondsFormat::Millis,true),
                "level":"info","source":"isos","core":"isos","node_path":null,
                "event_code":"events_suppressed","run_id":run_id,"correlation_id":null,
                "message":"Repeated child events suppressed","pid":std::process::id(),
                "fields":{"suppressed_source":suppressed_source,"suppressed_event_code":suppressed_event_code,"count":count}});
      let _ = self.append_record(&self.path("isos")?, "isos", &summary, false);
    }
    let result = self.append_record(&self.path(source)?, source, value, false);
    if result.is_ok()
      && let Some((key, now)) = pending_key
    {
      self.suppression.lock().unwrap().insert(key, (now, 0));
    }
    result
  }

  fn append_raw(&self, path: &Path, raw: &[u8]) {
    let _guard = self.lock.lock().unwrap();
    if self.append_file(path, raw).is_err() {
      saturating_increment(&WRITE_FAILED);
      saturating_increment(&self.losses);
    }
  }

  pub fn read(&self, source: &str, cursor: Option<&str>, limit: usize) -> Result<Value> {
    self.read_filtered(source, cursor, limit, None)
  }

  pub fn read_filtered(
    &self,
    source: &str,
    cursor: Option<&str>,
    limit: usize,
    min_level: Option<&str>,
  ) -> Result<Value> {
    ensure!((1..=200).contains(&limit), "log limit must be 1..200");
    let min_rank = min_level
      .map(|level| level_rank(level).context("invalid log level"))
      .transpose()?;
    let source_path = self.path(source)?;
    let unified_fallback = !source_path.exists() && !matches!(source, "all" | "services");
    let path = if unified_fallback {
      self.path("all")?
    } else {
      source_path
    };
    let mut file =
      File::open(&path).with_context(|| format!("log source {source} is unavailable"))?;
    let meta = file.metadata()?;
    // Inodes can be reused during rapid rotation. Birth time is stable
    // while a file is appended and changes when a replacement is created.
    let birth_ns = meta
      .created()
      .ok()
      .and_then(|created| created.duration_since(std::time::UNIX_EPOCH).ok())
      .map_or(0, |duration| duration.as_nanos());
    let identity = format!("{:x}-{:x}-{birth_ns:x}", meta.dev(), meta.ino());
    let mut offset = 0_u64;
    let mut rotated = false;
    if let Some(cursor) = cursor {
      let (id, position) = cursor.rsplit_once(':').context("invalid log cursor")?;
      let position = position
        .parse::<u64>()
        .context("invalid log cursor offset")?;
      if id == identity && position <= meta.len() {
        offset = position;
      } else {
        rotated = true;
      }
    }
    file.seek(SeekFrom::Start(offset))?;
    let mut reader = BufReader::new(file.take(MAX_READ_BYTES));
    let mut lines = Vec::new();
    let mut consumed = 0_u64;
    while lines.len() < limit {
      let mut raw = Vec::new();
      let size = reader.read_until(b'\n', &mut raw)?;
      if size == 0 {
        break;
      }
      if raw.last() != Some(&b'\n') && raw.len() == 8193 {
        break;
      }
      consumed += size as u64;
      if let Ok(value) = serde_json::from_slice::<Value>(&raw)
        && (!unified_fallback || value["source"].as_str() == Some(source))
        && (source != "services"
          || value["source"]
            .as_str()
            .is_some_and(|name| name.starts_with("service-")))
        && min_rank.is_none_or(|rank| {
          value["level"]
            .as_str()
            .and_then(level_rank)
            .is_some_and(|level| level >= rank)
        })
      {
        lines.push(value);
      }
    }
    let next_cursor = format!("{identity}:{}", offset + consumed);
    Ok(json!({"lines":lines,"next_cursor":next_cursor,"rotated":rotated}))
  }

  pub fn tail_filtered(
    &self,
    source: &str,
    limit: usize,
    min_level: Option<&str>,
  ) -> Result<Vec<Value>> {
    ensure!((1..=1000).contains(&limit), "log limit must be 1..1000");
    let min_rank = min_level
      .map(|level| level_rank(level).context("invalid log level"))
      .transpose()?;
    let source_path = self.path(source)?;
    let unified_fallback = !source_path.exists() && !matches!(source, "all" | "services");
    let path = if unified_fallback {
      self.path("all")?
    } else {
      source_path
    };
    let file = File::open(&path).with_context(|| format!("log source {source} is unavailable"))?;
    let mut lines = VecDeque::with_capacity(limit);
    for raw in BufReader::new(file).split(b'\n') {
      let raw = raw?;
      if let Ok(value) = serde_json::from_slice::<Value>(&raw)
        && (!unified_fallback || value["source"].as_str() == Some(source))
        && (source != "services"
          || value["source"]
            .as_str()
            .is_some_and(|name| name.starts_with("service-")))
        && min_rank.is_none_or(|rank| {
          value["level"]
            .as_str()
            .and_then(level_rank)
            .is_some_and(|level| level >= rank)
        })
      {
        if lines.len() == limit {
          lines.pop_front();
        }
        lines.push_back(value);
      }
    }
    Ok(lines.into_iter().collect())
  }
}

pub struct ChildAdmission {
  pub nodes: Option<Arc<BTreeSet<String>>>,
  pub skill_pins: BTreeSet<(String, String)>,
}

pub fn capture<R: std::io::Read + Send + 'static>(
  reader: R,
  store: LogStore,
  source: String,
  stream: &'static str,
  run_id: String,
  pid: u32,
  admission: Option<Arc<ChildAdmission>>,
) {
  std::thread::spawn(move || {
    capture_stream(
      reader,
      store,
      source,
      stream,
      &run_id,
      pid,
      admission.as_deref(),
    )
  });
}

/// The selected EOS stdout is inspected as raw bytes before any P06 log processing.
/// The channel receives one bounded transcript only after EOF; a descendant-held
/// writer therefore cannot accidentally satisfy the owner proof.
pub struct EosStdout {
  pub frame: Option<Vec<u8>>,
  pub at: Instant,
  pub eof: bool,
  pub invalid: bool,
}

pub fn capture_eos_stdout<R: Read + Send + 'static>(
  reader: R,
  store: LogStore,
  run_id: String,
  pid: u32,
) -> Receiver<EosStdout> {
  let (sender, receiver) = mpsc::sync_channel(4);
  std::thread::spawn(move || {
    let raw_dir = store.root.join("owner/eos");
    let raw_path = raw_dir.join("stdout.log");
    let raw_enabled = private_dir(&store.root.join("owner"))
      .and_then(|_| private_dir(&raw_dir))
      .is_ok();
    let mut reader = BufReader::new(reader);
    let mut count = 0;
    loop {
      let mut line = Vec::new();
      let result = Read::by_ref(&mut reader)
        .take(4097)
        .read_until(b'\n', &mut line);
      match result {
        Ok(0) => break,
        Err(_) => {
          let _ = sender.try_send(EosStdout {
            frame: None,
            at: Instant::now(),
            eof: false,
            invalid: true,
          });
          return;
        }
        Ok(_) => {
          let mut candidate = line
            .windows(b"eos_terminal_stop".len())
            .any(|bytes| bytes == b"eos_terminal_stop");
          let mut invalid = line.len() > 4096 || line.last() != Some(&b'\n');
          if line.last() != Some(&b'\n') {
            loop {
              let mut rest = Vec::new();
              match Read::by_ref(&mut reader)
                .take(4097)
                .read_until(b'\n', &mut rest)
              {
                Ok(0) | Err(_) => {
                  invalid = true;
                  break;
                }
                Ok(_) => {
                  if raw_enabled {
                    store.append_raw(&raw_path, &rest);
                  }
                  candidate |= rest
                    .windows(b"eos_terminal_stop".len())
                    .any(|bytes| bytes == b"eos_terminal_stop");
                  if rest.last() == Some(&b'\n') {
                    break;
                  }
                }
              }
            }
          }
          if candidate {
            count += 1;
            let _ = sender.try_send(EosStdout {
              frame: if !invalid && count <= 2 {
                Some(line.clone())
              } else {
                None
              },
              at: Instant::now(),
              eof: false,
              invalid: invalid || count > 2,
            });
          }
          if raw_enabled {
            store.append_raw(&raw_path, &line);
          }
        }
      }
    }
    let _ = (run_id, pid); // Pipe ownership is bound by its creating spawn.
    let _ = sender.try_send(EosStdout {
      frame: None,
      at: Instant::now(),
      eof: true,
      invalid: false,
    });
  });
  receiver
}

fn capture_stream<R: std::io::Read>(
  reader: R,
  store: LogStore,
  source: String,
  stream: &str,
  run_id: &str,
  pid: u32,
  admission: Option<&ChildAdmission>,
) {
  let mut reader = BufReader::new(reader);
  let raw_dir = store.root.join("owner").join(&source);
  let raw_path = raw_dir.join(format!("{stream}.log"));
  let raw_enabled = private_dir(&store.root.join("owner"))
    .and_then(|_| private_dir(&raw_dir))
    .is_ok();
  let empty_pins = BTreeSet::new();
  loop {
    let mut raw = Vec::new();
    // Bound a child that writes a very long line without a newline.
    match Read::by_ref(&mut reader)
      .take((event_policy::MAX_RAW_LINE + 1) as u64)
      .read_until(b'\n', &mut raw)
    {
      Ok(0) | Err(_) => break,
      Ok(_) => {
        if raw_enabled {
          store.append_raw(&raw_path, &raw);
        }
        if raw.last() != Some(&b'\n') {
          // Drain the rest of an oversized line without buffering it.
          loop {
            let mut discard = Vec::new();
            match Read::by_ref(&mut reader)
              .take((event_policy::MAX_RAW_LINE + 1) as u64)
              .read_until(b'\n', &mut discard)
            {
              Ok(0) | Err(_) => break,
              Ok(_) if discard.last() == Some(&b'\n') => {
                if raw_enabled {
                  store.append_raw(&raw_path, &discard);
                }
                break;
              }
              Ok(_) => {
                if raw_enabled {
                  store.append_raw(&raw_path, &discard);
                }
              }
            }
          }
          reject(&store, "raw_oversize");
          continue;
        }
        if stream == "stderr" && matches!(source.as_str(), "eos" | "bridge" | "config" | "egia") {
          match event_policy::parse_child(&raw, &source, run_id, pid) {
            Ok(event) => {
              let node_admitted = event["node_path"].as_str().is_none_or(|path| {
                admission.is_none_or(|policy| {
                  policy
                    .nodes
                    .as_ref()
                    .is_none_or(|nodes| nodes.contains(path))
                })
              });
              if !node_admitted {
                reject(&store, "node_owner");
              } else if !event_policy::skill_pin_admitted(
                &event,
                admission.map_or(&empty_pins, |policy| &policy.skill_pins),
              ) {
                reject(&store, "skill_pin_mismatch");
              } else {
                let _ = store.append_selected(&event);
              }
            }
            Err(error) => {
              let message = error.to_string();
              let reason = if message.contains("run mismatch") {
                "run_mismatch"
              } else if message.contains("source role") {
                "source_role"
              } else if message.contains("unregistered") {
                "event_code"
              } else if message.contains("node pointer") {
                "node_path"
              } else if message.contains("field") {
                "fields"
              } else if message.contains("message") {
                "message"
              } else if message.contains("line") {
                "line_limit"
              } else {
                "invalid_envelope"
              };
              reject(&store, reason);
            }
          }
        }
      }
    }
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::{
    io::Write,
    os::unix::fs::PermissionsExt,
    process::{Command, Stdio},
  };

  #[test]
  fn loss_counter_increment_saturates_under_contention() {
    let counter = Arc::new(AtomicU64::new(0));
    let workers: Vec<_> = (0..8)
      .map(|_| {
        let counter = Arc::clone(&counter);
        std::thread::spawn(move || {
          for _ in 0..1_000 {
            saturating_increment(&counter);
          }
        })
      })
      .collect();
    for worker in workers {
      worker.join().unwrap();
    }
    assert_eq!(counter.load(Ordering::Relaxed), 8_000);
    counter.store(u64::MAX - 1, Ordering::Relaxed);
    saturating_increment(&counter);
    saturating_increment(&counter);
    assert_eq!(counter.load(Ordering::Relaxed), u64::MAX);
  }

  #[test]
  fn selected_stdout_parser_reports_frame_then_eof_and_duplicate() {
    let (_temp, store) = store();
    let bytes =
      b"ordinary output\n{\"type\":\"eos_terminal_stop\"}\n{\"type\":\"eos_terminal_stop\"}\n";
    let receiver = capture_eos_stdout(
      std::io::Cursor::new(bytes.to_vec()),
      store,
      "r42".into(),
      42,
    );
    let first = receiver.recv_timeout(Duration::from_secs(1)).unwrap();
    assert!(first.frame.is_some() && !first.eof && !first.invalid);
    let second = receiver.recv_timeout(Duration::from_secs(1)).unwrap();
    assert!(second.frame.is_some());
    let eof = receiver.recv_timeout(Duration::from_secs(1)).unwrap();
    assert!(eof.eof);
  }

  fn store() -> (tempfile::TempDir, LogStore) {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    let store = LogStore::new(&paths, "default").unwrap();
    (temp, store)
  }

  fn skill_page(
    store: &LogStore,
    source: &str,
    correlation_id: Option<&str>,
    cursor: Option<&str>,
  ) -> Value {
    store
      .read_current(CurrentQuery {
        source,
        level: "info",
        node_path: None,
        correlation_id,
        cursor,
        limit: 100,
        active_revision: None,
      })
      .unwrap()
  }

  fn skill_pipe(store: &LogStore, events: &[Value], pins: &BTreeSet<(String, String)>) -> u32 {
    let mut child = Command::new("/bin/sh")
      .args(["-c", "exec cat >&2"])
      .stdin(Stdio::piped())
      .stdout(Stdio::null())
      .stderr(Stdio::piped())
      .spawn()
      .unwrap();
    let pid = child.id();
    let mut writer = child.stdin.take().unwrap();
    let admission = ChildAdmission {
      nodes: None,
      skill_pins: pins.clone(),
    };
    for event in events {
      serde_json::to_writer(&mut writer, event).unwrap();
      writer.write_all(b"\n").unwrap();
    }
    drop(writer);
    capture_stream(
      child.stderr.take().unwrap(),
      store.clone(),
      "eos".into(),
      "stderr",
      "r04-run",
      pid,
      Some(&admission),
    );
    assert!(child.wait().unwrap().success());
    pid
  }

  #[test]
  fn r04_skill_pipe_and_current_cursor_replay() {
    let fixture: Value = serde_json::from_str(include_str!(
      "../../../tests/fixtures/r04_skill_events/cases.json"
    ))
    .unwrap();
    assert_eq!(fixture["cursor_cases"].as_array().unwrap().len(), 10);
    let (_temp, mut store) = store();
    store.max_bytes = 65_536;
    store.backups = 1;
    store.publish_run("r04-run").unwrap();
    let pins = BTreeSet::from([("island/fixture-skills".into(), "0.1.0+Build.1".into())]);
    let queued = fixture["base_execution"].clone();
    let mut running = queued.clone();
    running["fields"]["stage"] = json!("running");
    running["fields"]["reason_code"] = json!("worker.running");
    let queued_pid = skill_pipe(&store, &[queued, running], &pins);
    assert_eq!(
      skill_page(&store, "all", None, None)["anchor"]["state"],
      "pending"
    );

    let mut rejected = fixture["base_execution"].clone();
    rejected["level"] = json!("error");
    rejected["fields"]["stage"] = json!("failed");
    rejected["fields"]["reason_code"] = json!("worker.guest_text");
    let mut failed = rejected.clone();
    failed["fields"]["reason_code"] = json!("worker.outcome_unknown");
    failed["fields"]["worker_reaped"] = json!(true);
    failed["fields"]["worker_exit_code"] = json!(1);
    let failure_pid = skill_pipe(&store, &[rejected, failed.clone()], &pins);
    let page = skill_page(&store, "all", None, None);
    assert_eq!(page["anchor"]["event"]["event_code"], "eos_skill_execution");
    assert_eq!(page["anchor"]["event"]["pid"], failure_pid);
    assert_eq!(page["diagnostics"][0]["code"], "log_collector_gap");
    let filtered = skill_page(&store, "egia", Some("other"), None);
    assert_eq!(filtered["anchor"]["event"], page["anchor"]["event"]);
    assert!(filtered["lines"].as_array().unwrap().is_empty());
    let source = store.read("eos", None, 100).unwrap();
    let all = store.read("all", None, 100).unwrap();
    assert_eq!(source["lines"], all["lines"]);
    assert_eq!(source["lines"].as_array().unwrap().len(), 3);
    assert_eq!(source["lines"][0]["pid"], queued_pid);

    let old = page["next_cursor"].as_str().unwrap().to_owned();
    skill_pipe(&store, &[failed, fixture["base_stop"].clone()], &pins);
    let later = skill_page(&store, "all", None, Some(&old));
    assert_eq!(later["anchor"]["event"], page["anchor"]["event"]);
    assert_eq!(later["lines"][0]["event_code"], "eos_skill_execution");
    assert_eq!(
      later["lines"][1]["event_code"],
      "eos_skill_stop_unconfirmed"
    );
    assert_eq!(store.run_identity(), (Some("r04-run".into()), "known"));

    let old = later["next_cursor"].as_str().unwrap().to_owned();
    for _ in 0..17 {
      store
        .append_event_context(
          "isos",
          "info",
          "deployment_starting",
          &"x".repeat(5_000),
          LogContext {
            run_id: Some("r04-run"),
            correlation_id: None,
            node_path: None,
            fields: json!({}),
          },
        )
        .unwrap();
    }
    let rotated = skill_page(&store, "all", None, Some(&old));
    assert_eq!(rotated["rotated"], true);
    assert_eq!(rotated["anchor"]["event"], page["anchor"]["event"]);
    assert!(
      !rotated["diagnostics"]
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
            run_id: Some("r04-run"),
            correlation_id: None,
            node_path: None,
            fields: json!({}),
          },
        )
        .unwrap();
    }
    let expired = skill_page(&store, "all", None, Some(&old));
    assert_eq!(expired["rotated"], true);
    assert_eq!(expired["diagnostics"][0]["code"], "log_cursor_expired");
    assert_eq!(expired["anchor"]["event"], page["anchor"]["event"]);
  }

  #[test]
  fn r04_skill_stop_first_anchor_and_write_gap() {
    let fixture: Value = serde_json::from_str(include_str!(
      "../../../tests/fixtures/r04_skill_events/cases.json"
    ))
    .unwrap();
    let (_temp, store) = store();
    store.publish_run("r04-run").unwrap();
    let pid = skill_pipe(&store, &[fixture["base_stop"].clone()], &BTreeSet::new());
    let first = skill_page(&store, "all", None, None);
    assert_eq!(
      first["anchor"]["event"]["event_code"],
      "eos_skill_stop_unconfirmed"
    );
    assert_eq!(first["anchor"]["event"]["pid"], pid);
    assert_eq!(first["scope"]["run_id"], "r04-run");
    saturating_increment(&store.losses);
    let after_gap = skill_page(
      &store,
      "all",
      None,
      Some(first["next_cursor"].as_str().unwrap()),
    );
    assert_eq!(after_gap["anchor"]["event"], first["anchor"]["event"]);
    assert_eq!(after_gap["diagnostics"][0]["code"], "log_collector_gap");
    assert_eq!(after_gap["diagnostics"][0]["count"], 1);
  }

  #[test]
  fn r04_selected_pin_mismatch_has_own_rejection_reason() {
    let fixture: Value = serde_json::from_str(include_str!(
      "../../../tests/fixtures/r04_skill_events/cases.json"
    ))
    .unwrap();
    let (_temp, store) = store();
    store.publish_run("r04-run").unwrap();
    let mut event = fixture["base_execution"].clone();
    event["fields"]["package_version"] = json!("0..1");
    let old_pins = BTreeSet::from([("island/fixture-skills".into(), "0.1.0+Build.1".into())]);
    let before = rejection_reasons()["skill_pin_mismatch"]
      .as_u64()
      .unwrap_or(0);
    skill_pipe(&store, &[event.clone()], &old_pins);
    assert!(
      store.read("eos", None, 10).unwrap()["lines"]
        .as_array()
        .unwrap()
        .is_empty()
    );
    assert!(
      store.read("all", None, 10).unwrap()["lines"]
        .as_array()
        .unwrap()
        .is_empty()
    );
    assert!(
      rejection_reasons()["skill_pin_mismatch"]
        .as_u64()
        .unwrap_or(0)
        > before
    );

    let selected = BTreeSet::from([("island/fixture-skills".into(), "0..1".into())]);
    let pid = skill_pipe(&store, &[event], &selected);
    let source = store.read("eos", None, 10).unwrap();
    assert_eq!(source["lines"][0]["fields"]["package_version"], "0..1");
    assert_eq!(source["lines"][0]["pid"], pid);
  }

  #[test]
  fn failed_store_initialization_is_visible_in_write_health() {
    let temp = tempfile::tempdir().unwrap();
    let file = temp.path().join("not-a-directory");
    fs::write(&file, b"occupied").unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: file,
      runtime: temp.path().join("run"),
    };
    let before = counters().1;
    assert!(LogStore::new(&paths, "default").is_err());
    assert!(counters().1 > before);
  }

  #[test]
  fn cursor_is_bounded_and_reports_rotation() {
    let (_temp, mut store) = store();
    store.max_bytes = 500;
    store.append("eos", "info", "first").unwrap();
    store.append("eos", "error", "second").unwrap();
    let first = store.read("eos", None, 1).unwrap();
    assert_eq!(first["lines"].as_array().unwrap().len(), 1);
    assert_eq!(first["lines"][0]["message"], "first");
    let second = store.read("eos", first["next_cursor"].as_str(), 1).unwrap();
    assert_eq!(second["lines"][0]["message"], "second");
    for _ in 0..10 {
      store.append("eos", "info", &"x".repeat(150)).unwrap();
    }
    let rotated = store
      .read("eos", first["next_cursor"].as_str(), 10)
      .unwrap();
    assert_eq!(rotated["rotated"], true);
  }

  #[test]
  fn logs_are_private_and_sources_are_checked() {
    let (_temp, store) = store();
    store.append("config", "error", "failure").unwrap();
    assert_eq!(
      fs::metadata(&store.root).unwrap().permissions().mode() & 0o777,
      0o700
    );
    assert_eq!(
      fs::metadata(store.root.join("config.log"))
        .unwrap()
        .permissions()
        .mode()
        & 0o777,
      0o600
    );
    assert_eq!(
      store.read("config", None, 10).unwrap()["lines"][0]["message"],
      "failure"
    );
    assert!(store.read("../secret", None, 1).is_err());
    assert!(store.read("config", None, 201).is_err());
    assert!(store.read("config", Some("bad"), 1).is_err());
    assert!(
      !store
        .root
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .join("process-logs")
        .exists()
    );
  }

  #[test]
  fn severity_threshold_filters_child_events() {
    let (_temp, mut store) = store();
    store.level = level_rank("warn").unwrap();
    store.append("eos", "info", "routine stdout").unwrap();
    store.append("eos", "error", "important stderr").unwrap();
    let lines = store.read("eos", None, 10).unwrap();
    assert_eq!(lines["lines"].as_array().unwrap().len(), 1);
    assert_eq!(lines["lines"][0]["message"], "important stderr");
    assert!(store.append("isos", "verbose", "unknown").is_err());
    assert!(store.append("all", "warn", "bad alias").is_err());
  }

  #[test]
  fn unified_stream_keeps_isos_and_service_events() {
    let (_temp, store) = store();
    store
      .append_event("isos", "warn", "admission_failed", "stale receipt")
      .unwrap();
    store
      .append_event("service-model", "error", "child_exited", "exit 1")
      .unwrap();
    let all = store.read("all", None, 10).unwrap();
    assert_eq!(all["lines"].as_array().unwrap().len(), 2);
    assert_eq!(all["lines"][0]["event_code"], "admission_failed");
    assert_eq!(all["lines"][1]["source"], "service-model");
    assert!(all["lines"][0]["pid"].is_number());
    let services = store.read("services", None, 10).unwrap();
    assert_eq!(services["lines"].as_array().unwrap().len(), 1);
    assert_eq!(services["lines"][0]["event_code"], "child_exited");
  }

  #[test]
  fn context_fields_are_bounded_and_preserve_correlation() {
    let (_temp, store) = store();
    store
      .append_event_context(
        "packages",
        "error",
        "package_preflight_failed",
        "Admission failed",
        LogContext {
          run_id: Some("manager-1"),
          correlation_id: Some("request-2"),
          node_path: None,
          fields: json!({"package_id":"sample/worker","package_version":"0.1.1"}),
        },
      )
      .unwrap();
    let line = &store.read("packages", None, 1).unwrap()["lines"][0];
    assert_eq!(line["run_id"], "manager-1");
    assert_eq!(line["correlation_id"], "request-2");
    assert_eq!(line["fields"]["package_id"], "sample/worker");
    assert!(
      store
        .append_event_context(
          "packages",
          "info",
          "bad",
          "bad",
          LogContext {
            run_id: None,
            correlation_id: None,
            node_path: None,
            fields: json!({"secret":"line\nwith newline"})
          }
        )
        .is_err()
    );
    assert!(
      store
        .append_event_context(
          "packages",
          "info",
          "bad",
          "bad",
          LogContext {
            run_id: Some("bad/id"),
            correlation_id: None,
            node_path: None,
            fields: json!({})
          }
        )
        .is_err()
    );
  }

  #[test]
  fn source_limit_keeps_unified_events_without_growing_source_files() {
    let (_temp, mut store) = store();
    store.max_sources = 10;
    store.append("isos", "info", "ready").unwrap();
    store.append("service-extra", "warn", "bounded").unwrap();
    assert!(!store.root.join("service-extra.log").exists());
    let events = store.read("all", None, 10).unwrap();
    assert_eq!(events["lines"][1]["source"], "service-extra");
    let source = store.read("service-extra", None, 10).unwrap();
    assert_eq!(source["lines"][0]["message"], "bounded");
    assert_eq!(
      fs::read_dir(&store.root)
        .unwrap()
        .filter_map(|entry| entry.ok())
        .filter(|entry| entry.path().extension().is_some_and(|ext| ext == "log"))
        .count(),
      10
    );
  }

  #[test]
  fn capture_only_records_selected_structured_events() {
    let (_temp, store) = store();
    let mut input = b"routine progress\n{\"level\":\"warn\",\"message\":\"selected\",\"source\":\"forged\",\"event_code\":\"child_warning\"}\n".to_vec();
    input.extend_from_slice(&[b'x'; 9000]);
    input.extend_from_slice(b"\n{\"timestamp\":\"2026-11-01T10:00:01.000Z\",\"level\":\"info\",\"source\":\"eos\",\"core\":\"eos\",\"event_code\":\"eos_ready\",\"run_id\":\"r1\",\"message\":\"EOS runtime ready\",\"fields\":{\"state\":\"ready\",\"node_count\":1}}\n");
    input.extend_from_slice(&[0xff, b'\n']);
    capture_stream(
      &input[..],
      store.clone(),
      "eos".into(),
      "stderr",
      "r1",
      99,
      None,
    );
    let lines = store.read("eos", None, 10).unwrap();
    assert_eq!(lines["lines"].as_array().unwrap().len(), 1);
    assert_eq!(lines["lines"][0]["source"], "eos");
    assert_eq!(lines["lines"][0]["event_code"], "eos_ready");
    assert_eq!(lines["lines"][0]["pid"], 99);
  }

  #[test]
  fn repeated_noncritical_child_event_has_bounded_terminal_summary() {
    let (_temp, store) = store();
    let warning = json!({"timestamp":"2026-11-01T10:00:01.000Z","level":"warn","source":"egia","core":"egia",
            "node_path":null,"event_code":"egia_discovery_lost","run_id":"r1","correlation_id":null,
            "message":"EOS discovery unavailable","fields":{"reason_code":"eos_unavailable"},"pid":42});
    store.append_selected(&warning).unwrap();
    store.append_selected(&warning).unwrap();
    store.append_selected(&warning).unwrap();
    let terminal = json!({"timestamp":"2026-11-01T10:00:02.000Z","level":"info","source":"egia","core":"egia",
            "node_path":null,"event_code":"egia_stopped","run_id":"r1","correlation_id":null,
            "message":"EGIA stopped","fields":{"state":"stopped"},"pid":42});
    store.append_selected(&terminal).unwrap();
    let lines = store.read("all", None, 10).unwrap();
    let lines = lines["lines"].as_array().unwrap();
    assert_eq!(lines.len(), 3);
    assert_eq!(lines[0]["event_code"], "egia_discovery_lost");
    assert_eq!(lines[1]["event_code"], "events_suppressed");
    assert_eq!(lines[1]["fields"]["count"], 2);
    assert_eq!(lines[2]["event_code"], "egia_stopped");
  }

  #[test]
  fn distinct_safety_rejection_correlation_and_reason_are_retained() {
    let (_temp, store) = store();
    let mut rejection = json!({"timestamp":"2026-11-01T10:00:01.000Z","level":"warn","source":"egia","core":"egia",
            "node_path":null,"event_code":"egia_safety_rejected","run_id":"r1","correlation_id":"request_1",
            "message":"Safety gate rejected request","fields":{"reason_code":"policy_denied"},"pid":42});
    store.append_selected(&rejection).unwrap();
    store.append_selected(&rejection).unwrap();
    rejection["correlation_id"] = json!("request_2");
    store.append_selected(&rejection).unwrap();
    rejection["fields"]["reason_code"] = json!("estop_latched");
    store.append_selected(&rejection).unwrap();
    let lines = store.read("all", None, 10).unwrap();
    let lines = lines["lines"].as_array().unwrap();
    assert_eq!(lines.len(), 3);
    assert_eq!(lines[0]["correlation_id"], "request_1");
    assert_eq!(lines[1]["correlation_id"], "request_2");
    assert_eq!(lines[2]["fields"]["reason_code"], "estop_latched");
  }

  #[test]
  fn failed_first_selected_append_can_retry_as_first_event() {
    let (_temp, store) = store();
    let event = json!({"timestamp":"2026-11-01T10:00:01.000Z","level":"warn","source":"egia","core":"egia",
            "node_path":null,"event_code":"egia_safety_rejected","run_id":"r1","correlation_id":"request_1",
            "message":"Safety gate rejected request","fields":{"reason_code":"policy_denied"},"pid":42});
    let blocked = store.root.join("egia.log");
    fs::remove_file(&blocked).unwrap();
    fs::create_dir(&blocked).unwrap();
    assert!(store.append_selected(&event).is_err());
    fs::remove_dir(&blocked).unwrap();
    store.append_selected(&event).unwrap();
    let lines = store.read("all", None, 10).unwrap();
    assert_eq!(lines["lines"].as_array().unwrap().len(), 1);
    assert_eq!(lines["lines"][0]["correlation_id"], "request_1");
  }

  #[test]
  fn a03_recovery_failure_anchor_survives_rotation_and_new_run_scope() {
    let (_temp, mut store) = store();
    store.max_bytes = 500;
    store.publish_run("ra03").unwrap();
    let failure = json!({"timestamp":"2026-11-01T10:00:01.000Z","level":"error","source":"egia","core":"egia",
            "node_path":null,"event_code":"egia_safety_reset_refused","run_id":"ra03","correlation_id":"reset_1",
            "message":"Safety latch reset refused","fields":{"reason_code":"revision_mismatch"},"pid":42});
    store.append_selected(&failure).unwrap();
    let first = skill_page(&store, "all", None, None);
    assert_eq!(
      first["anchor"]["event"]["event_code"],
      "egia_safety_reset_refused"
    );
    for _ in 0..8 {
      store.append("isos", "info", &"x".repeat(150)).unwrap();
    }
    let rotated = skill_page(
      &store,
      "all",
      None,
      Some(first["next_cursor"].as_str().unwrap()),
    );
    assert_eq!(rotated["anchor"]["event"], first["anchor"]["event"]);
    store.publish_run("ra03next").unwrap();
    let next = skill_page(&store, "all", None, None);
    assert_eq!(next["scope"]["run_id"], "ra03next");
    assert_eq!(next["anchor"]["state"], "pending");
  }

  #[test]
  fn selected_bridge_result_is_filtered_in_current_run_through_rotation() {
    let (_temp, mut store) = store();
    store.max_bytes = 1_024;
    store.backups = 3;
    store.publish_run("r_browser_1").unwrap();
    store
      .append_event_context(
        "isos",
        "info",
        "deployment_ready",
        "Deployment ready",
        LogContext {
          run_id: Some("r_browser_1"),
          correlation_id: None,
          node_path: None,
          fields: json!({}),
        },
      )
      .unwrap();
    let event = json!({"timestamp":"2026-11-02T08:00:00.100Z","level":"info","source":"bridge","core":"eos",
            "node_path":null,"event_code":"bridge_primitive_result","run_id":"r_browser_1",
            "correlation_id":"ui-018f","message":"EOS browser primitive result",
            "fields":{"status":"Completed","phase":"followup"}});
    let mut raw = serde_json::to_vec(&event).unwrap();
    raw.push(b'\n');
    capture_stream(
      &raw[..],
      store.clone(),
      "bridge".into(),
      "stderr",
      "r_browser_1",
      4242,
      None,
    );
    let page = skill_page(&store, "bridge", Some("ui-018f"), None);
    assert_eq!(page["anchor"]["event"]["event_code"], "deployment_ready");
    assert_eq!(page["lines"][0]["event_code"], "bridge_primitive_result");
    assert_eq!(page["lines"][0]["pid"], 4242);
    assert_eq!(page["lines"][0]["correlation_id"], "ui-018f");

    let token = format!("h_{}", "a".repeat(62));
    let mut derived = event.clone();
    derived["correlation_id"] = json!(token);
    derived["fields"]["status"] = json!("Rejected");
    derived["level"] = json!("warn");
    let mut raw = serde_json::to_vec(&derived).unwrap();
    raw.push(b'\n');
    capture_stream(
      &raw[..],
      store.clone(),
      "bridge".into(),
      "stderr",
      "r_browser_1",
      4242,
      None,
    );
    let derived_page = skill_page(&store, "bridge", Some(&token), None);
    assert_eq!(derived_page["lines"][0]["correlation_id"], token);
    assert_eq!(derived_page["lines"][0]["fields"]["status"], "Rejected");

    for _ in 0..2 {
      store.append("isos", "info", &"x".repeat(550)).unwrap();
    }
    let retained = skill_page(&store, "bridge", Some("ui-018f"), None);
    assert_eq!(retained["anchor"]["event"], page["anchor"]["event"]);
    assert_eq!(retained["lines"][0]["correlation_id"], "ui-018f");
    let derived_retained = skill_page(&store, "bridge", Some(&token), None);
    assert_eq!(derived_retained["lines"][0]["correlation_id"], token);
    let old_cursor = retained["next_cursor"].as_str().unwrap().to_owned();
    store.publish_run("r_browser_2").unwrap();
    let changed = skill_page(&store, "bridge", Some("ui-018f"), Some(&old_cursor));
    assert_eq!(changed["scope"]["run_id"], "r_browser_2");
    assert!(changed["lines"].as_array().unwrap().is_empty());
    assert_eq!(changed["diagnostics"][0]["code"], "log_run_changed");
  }

  #[test]
  fn selected_child_cannot_claim_an_unadmitted_node() {
    let (_temp, store) = store();
    let event = b"{\"timestamp\":\"2026-11-01T10:00:01.000Z\",\"level\":\"info\",\"source\":\"eos\",\"core\":\"eos\",\"node_path\":\"/cores/eos/layers/process/groups/worker/nodes/sim\",\"event_code\":\"eos_backend_ready\",\"run_id\":\"r1\",\"message\":\"EOS backend child ready\",\"fields\":{\"state\":\"ready\"}}\n";
    let admitted = BTreeSet::from(["/cores/eos/layers/process/groups/worker/nodes/other".into()]);
    let admission = ChildAdmission {
      nodes: Some(Arc::new(admitted)),
      skill_pins: BTreeSet::new(),
    };
    capture_stream(
      &event[..],
      store.clone(),
      "eos".into(),
      "stderr",
      "r1",
      42,
      Some(&admission),
    );
    assert!(
      store.read("eos", None, 10).unwrap()["lines"]
        .as_array()
        .unwrap()
        .is_empty()
    );
    assert!(counters().0 > 0);
    assert!(rejection_reasons()["node_owner"].as_u64().unwrap_or(0) > 0);
  }

  #[test]
  fn stored_event_trims_utf8_at_whole_line_bound() {
    let (_temp, store) = store();
    store
      .append_event("isos", "info", "message", &"界".repeat(4_000))
      .unwrap();
    let raw = fs::read(store.root().join("events.log")).unwrap();
    assert!(raw.len() <= event_policy::MAX_STORED_LINE);
    let text = std::str::from_utf8(&raw).unwrap();
    assert!(text.ends_with('\n'));
    assert_eq!(
      serde_json::from_slice::<Value>(&raw).unwrap()["core"],
      "isos"
    );
  }

  #[test]
  fn private_raw_child_log_rotates_without_entering_shared_stream() {
    let (_temp, mut store) = store();
    store.max_bytes = 100;
    let input = format!(
      "{}\n{}\n{}\n",
      "x".repeat(80),
      "y".repeat(80),
      "z".repeat(80)
    );
    capture_stream(
      input.as_bytes(),
      store.clone(),
      "ui".into(),
      "stdout",
      "m1",
      42,
      None,
    );
    assert!(store.root().join("owner/ui/stdout.log.1").is_file());
    assert!(
      store.read("all", None, 10).unwrap()["lines"]
        .as_array()
        .unwrap()
        .is_empty()
    );
    assert_eq!(
      fs::metadata(store.root().join("owner/ui/stdout.log"))
        .unwrap()
        .permissions()
        .mode()
        & 0o777,
      0o600
    );
  }

  #[test]
  fn filtered_reads_and_tail_keep_only_matching_severity() {
    let (_temp, store) = store();
    for (level, message) in [
      ("info", "one"),
      ("warn", "two"),
      ("info", "three"),
      ("error", "four"),
    ] {
      store.append("eos", level, message).unwrap();
    }
    let filtered = store.read_filtered("eos", None, 2, Some("warn")).unwrap();
    assert_eq!(filtered["lines"].as_array().unwrap().len(), 2);
    assert_eq!(filtered["lines"][0]["message"], "two");
    assert_eq!(filtered["lines"][1]["message"], "four");
    let tail = store.tail_filtered("eos", 1, Some("warn")).unwrap();
    assert_eq!(tail[0]["message"], "four");
    assert!(
      store
        .read_filtered("eos", None, 1, Some("verbose"))
        .is_err()
    );
    assert!(
      store
        .append_event("eos", "trace", "../bad", "invalid")
        .is_err()
    );
  }
}
