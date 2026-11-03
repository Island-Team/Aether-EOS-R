//! R04 native skill facade. The worker owns guest code; this manager owns every
//! public execution identity, broker decision, and lifecycle transition.

use super::*;
use std::collections::{BTreeSet, VecDeque};
use std::path::PathBuf;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use sha2::{Digest, Sha256};
use crate::host_skills::{Catalog, SkillDescriptor as HostDescriptor, WorkerCancelHandle, WorkerEvent};
use aether_common::events::{Event, EventSink, NonblockingStderr};

const MAX_RECORDS: usize = 1024;
const MAX_ACTIVE: usize = 8;
const MAX_HISTORY: usize = 64;
const MAX_WATCHERS: usize = 64;
// EGIA uses the standard 4 MiB gRPC receive limit. Keep framing headroom.
const MAX_LIST_WIRE: usize = 3 * 1024 * 1024;

pub(super) struct SkillManager {
  catalog: Arc<Catalog>,
  executable: PathBuf,
  runtime: Arc<Runtime>,
  managed: Option<Arc<ManagedAdmission>>,
  bindings: BTreeMap<String, VelocityBinding>,
  converted_bindings: BTreeMap<String, ConvertedBinding>,
  inner: Mutex<Inner>,
  next_id: AtomicUsize,
  events: Option<Arc<EventSink<NonblockingStderr, SystemClock>>>,
  event_failed: AtomicBool,
}

#[derive(Default)]
struct Inner {
  records: BTreeMap<String, Record>,
  by_request: BTreeMap<String, String>,
  order: VecDeque<String>,
  queue: VecDeque<String>,
  active: usize,
  cancel_active: usize,
  active_nonreentrant: BTreeSet<(String, String)>,
  watchers: usize,
  stopping: bool,
  reap_failed: bool,
}

struct Record {
  snapshot: v1::SkillExecution,
  signature: String,
  descriptor: HostDescriptor,
  arguments_json: String,
  deadline: Option<Instant>,
  timer: Option<tokio::task::JoinHandle<()>>,
  history: VecDeque<v1::SkillExecution>,
  updates: tokio::sync::broadcast::Sender<v1::SkillExecution>,
  cancel: Arc<AtomicBool>,
  broker_gate: Arc<Mutex<()>>,
  worker: Option<WorkerCancelHandle>,
  command_ids: Vec<String>,
  uncertain: bool,
  release_latched: bool,
  cancel_started: bool,
  cancel_pending: bool,
}

impl SkillManager {
  pub(super) fn new(
    projection: &Projection,
    catalog: Arc<Catalog>,
    executable: PathBuf,
    events: Option<Arc<EventSink<NonblockingStderr, SystemClock>>>,
  ) -> Result<Arc<Self>, String> {
    validate_catalog_readiness(
      projection.runtime.profile(),
      projection.managed.is_some(),
      &catalog,
    )
    .map_err(str::to_owned)?;
    for descriptor in catalog.descriptors() {
      for primitive in &descriptor.primitives {
        let live = match primitive.name.as_str() {
          "base.velocity" => projection
            .bindings
            .values()
            .any(|binding| binding.binding_path.is_some()),
          "arm.movej" => projection
            .converted_bindings
            .values()
            .any(|binding| binding.primitive == "arm.movej" && binding.binding_path.is_some()),
          _ => false,
        };
        if !live {
          return Err("host skill primitive has no live deployment binding".into());
        }
      }
    }
    Ok(Arc::new(Self {
      catalog,
      executable,
      runtime: projection.runtime.clone(),
      managed: projection.managed.clone(),
      bindings: projection.bindings.clone(),
      converted_bindings: projection.converted_bindings.clone(),
      inner: Mutex::new(Inner::default()),
      next_id: AtomicUsize::new(1),
      events,
      event_failed: AtomicBool::new(false),
    }))
  }

  pub(super) fn catalog(&self) -> Result<v1::ListSkillsResponse, Status> {
    Ok(v1::ListSkillsResponse {
      skills: self
        .catalog
        .descriptors()
        .iter()
        .map(public_descriptor)
        .collect::<Result<_, _>>()
        .map_err(Status::internal)?,
    })
  }

  pub(super) fn get_skill(&self, name: &str, version: &str) -> Result<v1::SkillDescriptor, Status> {
    self
      .catalog
      .descriptors()
      .iter()
      .find(|item| item.name == name && item.version == version)
      .ok_or_else(|| Status::not_found("skill identity not admitted"))
      .and_then(|item| public_descriptor(item).map_err(Status::internal))
  }

  pub(super) fn start(
    self: &Arc<Self>,
    request: v1::StartExecutionRequest,
  ) -> Result<v1::SkillExecution, Status> {
    let reference = request
      .reference
      .ok_or_else(|| Status::invalid_argument("skill reference required"))?;
    if request.request_id.is_empty() || request.request_id.len() > 96 {
      return Err(Status::invalid_argument(
        "request ID must be 1–96 UTF-8 bytes",
      ));
    }
    let arguments = request
      .arguments
      .ok_or_else(|| Status::invalid_argument("arguments object required"))?;
    let arguments_value = struct_to_json(&arguments)?;
    let arguments_json = serde_json::to_string(&arguments_value)
      .map_err(|_| Status::invalid_argument("arguments invalid"))?;
    if arguments_json.len() > 1024 * 1024 {
      return Err(Status::invalid_argument("arguments exceed 1 MiB"));
    }
    let timeout = parse_timeout(request.timeout.as_ref())?;
    let signature = serde_json::to_string(&(
      &reference.name,
      &reference.version,
      &arguments_value,
      timeout.map(|value| value.as_nanos()),
    ))
    .map_err(|_| Status::invalid_argument("arguments invalid"))?;
    {
      let inner = self
        .inner
        .lock()
        .map_err(|_| Status::internal("skill manager unavailable"))?;
      if let Some(existing_id) = inner.by_request.get(&request.request_id) {
        let existing = inner
          .records
          .get(existing_id)
          .expect("request index consistent");
        return if existing.signature == signature {
          Ok(existing.snapshot.clone())
        } else {
          Err(Status::already_exists("request ID has different content"))
        };
      }
    }
    let descriptor = self
      .catalog
      .descriptors()
      .iter()
      .find(|item| item.name == reference.name && item.version == reference.version)
      .ok_or_else(|| Status::not_found("skill identity not admitted"))?
      .clone();
    if !jsonschema::validator_for(&descriptor.parameters_schema)
      .map_err(|_| Status::internal("admitted schema unavailable"))?
      .is_valid(&arguments_value)
    {
      return Err(Status::invalid_argument("arguments fail admitted schema"));
    }
    let mut inner = self
      .inner
      .lock()
      .map_err(|_| Status::internal("skill manager unavailable"))?;
    if inner.stopping {
      return Err(Status::unavailable("runtime is quiescing"));
    }
    if let Some(existing_id) = inner.by_request.get(&request.request_id) {
      let existing = inner
        .records
        .get(existing_id)
        .expect("request index consistent");
      return if existing.signature == signature {
        Ok(existing.snapshot.clone())
      } else {
        Err(Status::already_exists("request ID has different content"))
      };
    }
    while inner.records.len() >= MAX_RECORDS {
      if !evict_oldest_terminal(&mut inner, None) {
        return Err(Status::resource_exhausted("execution records full"));
      }
    }
    let sequence = self.next_id.fetch_add(1, Ordering::Relaxed);
    let id = format!(
      "ex-{:x}",
      Sha256::digest(
        format!(
          "{}:{}:{}:{}",
          self.managed.as_ref().map_or("local", |m| m.run_id.as_str()),
          std::process::id(),
          now_nanos(),
          sequence
        )
        .as_bytes()
      )
    );
    let created_at = now_timestamp();
    let deadline = timeout.map(|value| Instant::now() + value);
    let snapshot = v1::SkillExecution {
      execution_id: id.clone(),
      request_id: request.request_id.clone(),
      reference: Some(reference),
      status: v1::ExecutionStatus::Queued as i32,
      sequence: 1,
      created_at: Some(created_at),
      deadline_at: timeout.map(|value| add_duration(&created_at, value)),
      runtime_id: self
        .managed
        .as_ref()
        .map_or(String::new(), |m| m.run_id.clone()),
      ..Default::default()
    };
    let (updates, _) = tokio::sync::broadcast::channel(MAX_HISTORY);
    if !make_list_space(&mut inner, snapshot.encoded_len() + 16, None) {
      return Err(Status::resource_exhausted(
        "execution list byte budget full",
      ));
    }
    inner.records.insert(
      id.clone(),
      Record {
        snapshot: snapshot.clone(),
        signature,
        descriptor,
        arguments_json,
        deadline,
        timer: None,
        history: VecDeque::from([snapshot.clone()]),
        updates,
        cancel: Arc::new(AtomicBool::new(false)),
        broker_gate: Arc::new(Mutex::new(())),
        worker: None,
        command_ids: Vec::new(),
        uncertain: false,
        release_latched: false,
        cancel_started: false,
        cancel_pending: false,
      },
    );
    inner.by_request.insert(request.request_id, id.clone());
    inner.order.push_back(id.clone());
    inner.queue.push_back(id.clone());
    drop(inner);
    self.emit_event(&id, "queued", "worker.queued", None, None);
    if let Some(delay) = timeout {
      let manager = self.clone();
      let timer_id = id.clone();
      let timer = tokio::spawn(async move {
        tokio::time::sleep(delay).await;
        manager.expire(&timer_id);
      });
      let mut inner = self.inner.lock().expect("skill manager lock");
      if let Some(record) = inner
        .records
        .get_mut(&id)
        .filter(|record| !terminal(record.snapshot.status))
      {
        record.timer = Some(timer);
      } else {
        timer.abort();
      }
    }
    self.schedule();
    Ok(snapshot)
  }

  pub(super) fn get(&self, id: &str) -> Result<v1::SkillExecution, Status> {
    self
      .inner
      .lock()
      .map_err(|_| Status::internal("skill manager unavailable"))?
      .records
      .get(id)
      .map(|record| record.snapshot.clone())
      .ok_or_else(|| Status::not_found("execution not found"))
  }

  pub(super) fn list(&self) -> Result<v1::ListExecutionsResponse, Status> {
    let inner = self
      .inner
      .lock()
      .map_err(|_| Status::internal("skill manager unavailable"))?;
    Ok(v1::ListExecutionsResponse {
      executions: inner
        .order
        .iter()
        .filter_map(|id| inner.records.get(id).map(|record| record.snapshot.clone()))
        .collect(),
    })
  }

  pub(super) fn watch(self: &Arc<Self>, id: &str, after: u64) -> Result<SkillWatchStream, Status> {
    let mut inner = self
      .inner
      .lock()
      .map_err(|_| Status::internal("skill manager unavailable"))?;
    if inner.watchers >= MAX_WATCHERS {
      return Err(Status::resource_exhausted("skill watchers full"));
    }
    let record = inner
      .records
      .get(id)
      .ok_or_else(|| Status::not_found("execution not found"))?;
    if after > record.snapshot.sequence {
      return Err(Status::invalid_argument("cursor ahead of execution"));
    }
    let first = record.history.front().map_or(0, |item| item.sequence);
    if after.saturating_add(1) < first {
      return Err(Status::out_of_range("lifecycle replay gap"));
    }
    let replay: Vec<_> = record
      .history
      .iter()
      .filter(|item| item.sequence > after)
      .cloned()
      .collect();
    let receiver = record.updates.subscribe();
    let terminal_now = terminal(record.snapshot.status);
    inner.watchers += 1;
    Ok(SkillWatchStream::new(
      self.clone(),
      replay,
      receiver,
      terminal_now,
      after,
    ))
  }

  pub(super) fn terminate(self: &Arc<Self>, id: &str) -> Result<v1::SkillExecution, Status> {
    let mut inner = self
      .inner
      .lock()
      .map_err(|_| Status::internal("skill manager unavailable"))?;
    let record = inner
      .records
      .get_mut(id)
      .ok_or_else(|| Status::not_found("execution not found"))?;
    if terminal(record.snapshot.status) {
      return Ok(record.snapshot.clone());
    }
    record.cancel.store(true, Ordering::Release);
    record.snapshot.cancellation_requested = true;
    record.snapshot.cancellation_requested_at = Some(now_timestamp());
    let handle = record.worker.clone();
    let broker_gate = record.broker_gate.clone();
    let queued = record.snapshot.status == v1::ExecutionStatus::Queued as i32;
    let launch_cancel = !queued && !record.cancel_started;
    if launch_cancel {
      record.cancel_started = true;
      record.cancel_pending = true;
    }
    if queued {
      transition(
        record,
        v1::ExecutionStatus::Cancelled,
        "worker.cancelled",
        None,
      );
    }
    let snapshot = record.snapshot.clone();
    drop(inner);
    // The broker holds this gate only while registering an ID. Dispatcher
    // checks the same revocation token at its durable release boundary.
    let _gate = broker_gate
      .lock()
      .map_err(|_| Status::internal("broker gate unavailable"))?;
    let ids = self
      .inner
      .lock()
      .map_err(|_| Status::internal("skill manager unavailable"))?
      .records
      .get(id)
      .map(|record| record.command_ids.clone())
      .unwrap_or_default();
    drop(_gate);
    if launch_cancel {
      self.launch_cancel(id, ids);
    }
    if let Some(handle) = handle {
      let _ = handle.cancel();
    }
    if queued {
      self.emit_event(id, "failed", "worker.cancelled", None, None);
      self.schedule();
    }
    Ok(snapshot)
  }

  pub(super) fn shutdown(self: &Arc<Self>) {
    let (handles, ids, queued_ids) = {
      let mut inner = self.inner.lock().expect("skill manager lock");
      inner.stopping = true;
      let mut handles = Vec::new();
      let mut ids = Vec::new();
      let mut queued_ids = Vec::new();
      for record in inner.records.values_mut() {
        if terminal(record.snapshot.status) {
          continue;
        }
        record.cancel.store(true, Ordering::Release);
        if record.snapshot.status == v1::ExecutionStatus::Running as i32 && !record.cancel_started {
          record.cancel_started = true;
          record.cancel_pending = true;
          ids.push((
            record.snapshot.execution_id.clone(),
            record.broker_gate.clone(),
          ));
        }
        if let Some(handle) = &record.worker {
          handles.push(handle.clone());
        }
        if record.snapshot.status == v1::ExecutionStatus::Queued as i32 {
          queued_ids.push(record.snapshot.execution_id.clone());
          transition(
            record,
            v1::ExecutionStatus::Cancelled,
            "worker.shutdown",
            None,
          );
        }
      }
      (handles, ids, queued_ids)
    };
    for (execution_id, gate) in ids {
      let _gate = gate.lock().expect("broker gate");
      let commands = self
        .inner
        .lock()
        .expect("skill manager lock")
        .records
        .get(&execution_id)
        .map(|record| record.command_ids.clone())
        .unwrap_or_default();
      drop(_gate);
      self.launch_cancel(&execution_id, commands);
    }
    for execution_id in queued_ids {
      self.emit_event(&execution_id, "failed", "worker.shutdown", None, None);
    }
    for handle in handles {
      let _ = handle.cancel();
    }
  }

  pub(super) fn wait_stopped(&self) -> Result<(), String> {
    let until = Instant::now() + Duration::from_secs(4);
    loop {
      let inner = self.inner.lock().map_err(|_| "skill manager unavailable")?;
      if inner.active == 0 && inner.cancel_active == 0 {
        return if inner.reap_failed {
          Err("skill worker reap unconfirmed".into())
        } else if self.event_failed.load(Ordering::Acquire) {
          Err("skill lifecycle event unavailable".into())
        } else {
          Ok(())
        };
      }
      if Instant::now() >= until {
        return Err("skill worker stop deadline exceeded".into());
      }
      drop(inner);
      std::thread::sleep(Duration::from_millis(10));
    }
  }

  fn launch_cancel(self: &Arc<Self>, execution_id: &str, ids: Vec<String>) {
    let mut inner = self.inner.lock().expect("skill manager lock");
    if ids.is_empty() {
      if let Some(record) = inner.records.get_mut(execution_id) {
        record.cancel_pending = false;
      }
      return;
    }
    inner.cancel_active += 1;
    drop(inner);
    let manager = self.clone();
    let execution_id = execution_id.to_owned();
    std::thread::spawn(move || manager.cancel_commands(&execution_id, &ids));
  }

  fn cancel_commands(self: Arc<Self>, execution_id: &str, ids: &[String]) {
    let mut uncertain = false;
    if let Some(managed) = &self.managed {
      for id in ids {
        match self.runtime.dispatcher().cancel_authorized(
          id,
          &managed.run_id,
          AuthorityRole::EgiaControl,
        ) {
          Ok(result)
            if matches!(
              result.status,
              OutcomeStatus::OutcomeUnknown | OutcomeStatus::InProgress
            ) =>
          {
            uncertain = true
          }
          Ok(_) | Err(GatewayLookupError::NotFound) => {}
          Err(_) => uncertain = true,
        }
      }
    }
    if let Ok(mut inner) = self.inner.lock() {
      if let Some(record) = inner.records.get_mut(execution_id) {
        record.uncertain |= uncertain;
        record.cancel_pending = false;
      }
      inner.cancel_active = inner.cancel_active.saturating_sub(1);
    }
    self.schedule();
  }

  fn expire(self: &Arc<Self>, id: &str) {
    let mut inner = self.inner.lock().expect("skill manager lock");
    let Some(record) = inner.records.get_mut(id) else {
      return;
    };
    if terminal(record.snapshot.status) {
      return;
    }
    record.cancel.store(true, Ordering::Release);
    let handle = record.worker.clone();
    let broker_gate = record.broker_gate.clone();
    let queued = record.snapshot.status == v1::ExecutionStatus::Queued as i32;
    let launch_cancel = !queued && !record.cancel_started;
    if launch_cancel {
      record.cancel_started = true;
      record.cancel_pending = true;
    }
    if queued {
      transition(
        record,
        v1::ExecutionStatus::TimedOut,
        "worker.deadline",
        None,
      );
    }
    drop(inner);
    let _gate = broker_gate.lock().expect("broker gate");
    let ids = self
      .inner
      .lock()
      .expect("skill manager lock")
      .records
      .get(id)
      .map(|record| record.command_ids.clone())
      .unwrap_or_default();
    drop(_gate);
    if launch_cancel {
      self.launch_cancel(id, ids);
    }
    if let Some(handle) = handle {
      let _ = handle.cancel();
    }
    if queued {
      self.emit_event(id, "failed", "worker.deadline", None, None);
      self.schedule();
    }
  }

  fn schedule(self: &Arc<Self>) {
    let mut launches = Vec::new();
    {
      let mut inner = self.inner.lock().expect("skill manager lock");
      if inner.stopping {
        return;
      }
      let mut retained = VecDeque::new();
      let mut blocked = BTreeSet::new();
      while let Some(id) = inner.queue.pop_front() {
        let Some(record) = inner.records.get(&id) else {
          continue;
        };
        if record.snapshot.status != v1::ExecutionStatus::Queued as i32 {
          continue;
        }
        let identity = (
          record.descriptor.name.clone(),
          record.descriptor.version.clone(),
        );
        let nonreentrant = !record.descriptor.reentrant;
        if inner.active.saturating_add(inner.cancel_active) >= MAX_ACTIVE
          || (nonreentrant
            && (inner.active_nonreentrant.contains(&identity) || blocked.contains(&identity)))
        {
          if nonreentrant {
            blocked.insert(identity);
          }
          retained.push_back(id);
          continue;
        }
        inner.active += 1;
        if nonreentrant {
          inner.active_nonreentrant.insert(identity);
        }
        let record = inner.records.get_mut(&id).expect("queued record");
        transition(record, v1::ExecutionStatus::Running, "worker.running", None);
        launches.push(id);
      }
      inner.queue = retained;
    }
    for id in launches {
      self.emit_event(&id, "running", "worker.running", None, None);
      let manager = self.clone();
      std::thread::spawn(move || manager.run_one(id));
    }
  }

  fn run_one(self: Arc<Self>, id: String) {
    let (descriptor, args, request_id, deadline, cancel) = {
      let inner = self.inner.lock().expect("skill manager lock");
      let record = inner.records.get(&id).expect("scheduled record");
      (
        record.descriptor.clone(),
        record.arguments_json.clone(),
        record.snapshot.request_id.clone(),
        record.deadline,
        record.cancel.clone(),
      )
    };
    let mut terminal_status = v1::ExecutionStatus::Failed;
    let reason: String;
    let mut result = None;
    if cancel.load(Ordering::Acquire) {
      self.finish(
        &id,
        &descriptor,
        v1::ExecutionStatus::Cancelled,
        "worker.cancelled",
        None,
        None,
        None,
      );
      return;
    }
    let mut session = match self.catalog.start_worker(
      &self.executable,
      &descriptor.name,
      &descriptor.version,
      &id,
      &args,
    ) {
      Ok(session) => session,
      Err(error) => {
        reason = "worker.failed".into();
        self.finish(
          &id,
          &descriptor,
          terminal_status,
          &reason,
          result,
          error.worker_reaped,
          None,
        );
        return;
      }
    };
    {
      let mut inner = self.inner.lock().expect("skill manager lock");
      if let Some(record) = inner.records.get_mut(&id) {
        record.worker = Some(session.cancellation_handle());
      }
    }
    let mut call_index = 0u64;
    loop {
      if cancel.load(Ordering::Acquire) {
        terminal_status = if deadline.is_some_and(|end| Instant::now() >= end) {
          v1::ExecutionStatus::TimedOut
        } else {
          v1::ExecutionStatus::Cancelled
        };
        reason = if terminal_status == v1::ExecutionStatus::TimedOut {
          "worker.deadline"
        } else {
          "worker.cancelled"
        }
        .into();
        break;
      }
      match session.recv(Duration::from_millis(100)) {
        Err(error) if error.code == "worker_timeout" => continue,
        Err(_error) => {
          reason = "worker.failed".into();
          break;
        }
        Ok(WorkerEvent::PrimitiveCall {
          name,
          version,
          device_id,
          arguments_json,
        }) => {
          let (status, acknowledgement) = self.broker(
            &id,
            &descriptor,
            &request_id,
            call_index,
            &name,
            &version,
            &device_id,
            &arguments_json,
            deadline,
            &cancel,
          );
          call_index = call_index.saturating_add(1);
          if session.reply_primitive(status, &acknowledgement).is_err() {
            reason = "worker.protocol_invalid".into();
            break;
          }
        }
        Ok(WorkerEvent::Completion {
          accepted,
          reason_code: _,
          result_json,
          message: _,
          status: _,
        }) => {
          let parsed = (result_json.len() <= 1024 * 1024)
            .then(|| serde_json::from_str::<serde_json::Value>(&result_json).ok())
            .flatten();
          if let Some(value) = parsed.filter(|value| {
            value.is_object()
              && jsonschema::validator_for(&descriptor.returns_schema)
                .is_ok_and(|schema| schema.is_valid(value))
          }) {
            result = json_to_struct(&value).ok();
            terminal_status = if accepted {
              v1::ExecutionStatus::Succeeded
            } else {
              v1::ExecutionStatus::Rejected
            };
            reason = if accepted {
              "worker.succeeded"
            } else {
              "worker.rejected"
            }
            .into();
          } else {
            reason = "worker.result_invalid".into();
          }
          break;
        }
        Ok(WorkerEvent::Failure { code: _ }) => {
          reason = "worker.failed".into();
          break;
        }
        Ok(WorkerEvent::Catalog { .. }) => {
          reason = "worker.protocol_invalid".into();
          break;
        }
      }
    }
    let reaped = session.terminate_reap().unwrap_or(false);
    let exit_code = session.exit_status().and_then(|status| status.code());
    self.finish(
      &id,
      &descriptor,
      terminal_status,
      &reason,
      result,
      Some(reaped),
      exit_code,
    );
  }

  #[allow(clippy::too_many_arguments)]
  fn finish(
    self: &Arc<Self>,
    id: &str,
    descriptor: &HostDescriptor,
    mut status: v1::ExecutionStatus,
    reason: &str,
    mut result: Option<prost_types::Struct>,
    reaped: Option<bool>,
    exit_code: Option<i32>,
  ) {
    let cancel_deadline = Instant::now() + Duration::from_secs(1);
    loop {
      let pending = self
        .inner
        .lock()
        .expect("skill manager lock")
        .records
        .get(id)
        .is_some_and(|record| record.cancel_pending);
      if !pending {
        break;
      }
      if Instant::now() >= cancel_deadline {
        if let Some(record) = self
          .inner
          .lock()
          .expect("skill manager lock")
          .records
          .get_mut(id)
        {
          record.uncertain = true;
        }
        break;
      }
      std::thread::sleep(Duration::from_millis(10));
    }
    let mut final_reason = reason.to_owned();
    {
      let mut inner = self.inner.lock().expect("skill manager lock");
      if reaped == Some(false) {
        inner.reap_failed = true;
        // A live child still occupies its worker slot and nonreentrant lane.
        status = v1::ExecutionStatus::Failed;
        final_reason = "worker.reap_failed".into();
        if let Some(record) = inner.records.get_mut(id) {
          transition(record, status, &final_reason, None);
        }
      } else {
        inner.active = inner.active.saturating_sub(1);
        if !descriptor.reentrant {
          inner
            .active_nonreentrant
            .remove(&(descriptor.name.clone(), descriptor.version.clone()));
        }
        let extra = result.as_ref().map_or(0, |value| value.encoded_len() + 64);
        let result_fits = make_list_space(&mut inner, extra, Some(id));
        if let Some(record) = inner.records.get_mut(id) {
          record.worker = None;
          final_reason = if record.uncertain {
            status = v1::ExecutionStatus::Failed;
            result = None;
            "worker.outcome_unknown"
          } else if record.cancel.load(Ordering::Acquire) {
            result = None;
            if status == v1::ExecutionStatus::TimedOut {
              "worker.deadline"
            } else {
              status = v1::ExecutionStatus::Cancelled;
              "worker.cancelled"
            }
          } else if record.release_latched {
            status = v1::ExecutionStatus::Failed;
            "worker.primitive_denied"
          } else if !result_fits {
            status = v1::ExecutionStatus::Failed;
            "worker.result_capacity"
          } else {
            reason
          }
          .into();
          transition(
            record,
            status,
            &final_reason,
            if result_fits { result } else { None },
          );
        }
      }
    }
    self.emit_event(
      id,
      if status == v1::ExecutionStatus::Succeeded {
        "completed"
      } else {
        "failed"
      },
      &final_reason,
      reaped,
      exit_code,
    );
    if reaped != Some(false) {
      self.schedule();
    }
  }

  fn emit_event(
    &self,
    id: &str,
    stage: &str,
    reason: &str,
    reaped: Option<bool>,
    exit_code: Option<i32>,
  ) {
    let Some(sink) = &self.events else {
      return;
    };
    let fields = self.inner.lock().ok().and_then(|inner| {
      inner.records.get(id).map(|record| {
        (
          record.descriptor.package_id.clone(),
          record.descriptor.package_version.clone(),
          record.snapshot.request_id.clone(),
        )
      })
    });
    let Some((package_id, package_version, request_id)) = fields else {
      return;
    };
    let correlation = format!("skill_{:x}", Sha256::digest(request_id.as_bytes()));
    if sink
      .emit_correlated(
        Event::SkillExecution {
          execution_id: id,
          package_id: &package_id,
          package_version: &package_version,
          stage,
          reason_code: reason,
          worker_reaped: reaped,
          worker_exit_code: exit_code,
        },
        None,
        Some(&correlation[..38]),
      )
      .is_err()
    {
      self.event_failed.store(true, Ordering::Release);
    }
  }

  fn deny_broker_call(&self, id: &str) -> (u32, String) {
    if let Ok(mut inner) = self.inner.lock() {
      if let Some(record) = inner.records.get_mut(id) {
        record.release_latched = true;
      }
    }
    (1, "{}".into())
  }

  #[allow(clippy::too_many_arguments)]
  fn broker(
    &self,
    id: &str,
    descriptor: &HostDescriptor,
    request_id: &str,
    index: u64,
    name: &str,
    version: &str,
    device_id: &str,
    arguments_json: &str,
    deadline: Option<Instant>,
    cancel: &AtomicBool,
  ) -> (u32, String) {
    let gate = {
      let inner = self.inner.lock().expect("skill manager lock");
      let Some(record) = inner.records.get(id) else {
        return (1, "{}".into());
      };
      record.broker_gate.clone()
    };
    // Terminate/timeout takes this short gate after revocation and queries
    // every registered ID. Dispatcher checks revocation again under journal lock.
    let _gate = gate.lock().expect("broker gate");
    if cancel.load(Ordering::Acquire) || self.inner.lock().expect("skill manager lock").stopping {
      return self.deny_broker_call(id);
    }
    if self
      .inner
      .lock()
      .expect("skill manager lock")
      .records
      .get(id)
      .is_some_and(|record| record.release_latched)
    {
      return self.deny_broker_call(id);
    }
    if !descriptor
      .primitives
      .iter()
      .any(|item| item.name == name && item.version == version)
      || !matches!(
        (name, version),
        ("base.velocity", "1.0.0") | ("arm.movej", "1.0.0")
      )
    {
      return self.deny_broker_call(id);
    }
    let Some(managed) = &self.managed else {
      return self.deny_broker_call(id);
    };
    let remaining = deadline.map_or(Duration::from_secs(5), |end| {
      end
        .saturating_duration_since(Instant::now())
        .min(Duration::from_secs(5))
    });
    if remaining.is_zero() {
      return self.deny_broker_call(id);
    }
    if arguments_json.len() > 1024 * 1024 {
      return self.deny_broker_call(id);
    }
    let Ok(value) = serde_json::from_str::<serde_json::Value>(arguments_json) else {
      return self.deny_broker_call(id);
    };
    let Ok(arguments) = json_to_struct(&value) else {
      return self.deny_broker_call(id);
    };
    let key = format!(
      "hardware/{}/{}/{}/{}@1.0.0",
      self.runtime.profile().robot.id,
      if name == "base.velocity" {
        "base"
      } else {
        "manipulator"
      },
      device_id,
      name
    );
    let parsed = if name == "base.velocity" {
      self
        .bindings
        .get(&key)
        .filter(|binding| binding.device_id == device_id)
        .and_then(|binding| {
          let path = binding.binding_path.clone()?;
          let movement = parse_velocity(&arguments, binding).ok()?;
          Some((path, binding.node_path.clone(), Movement::Base(movement)))
        })
    } else {
      self
        .converted_bindings
        .get(&key)
        .filter(|binding| binding.device_id == device_id && binding.primitive == "arm.movej")
        .and_then(|binding| {
          let path = binding.binding_path.clone()?;
          let movement = parse_converted(&arguments, binding).ok()?;
          Some((path, binding.node_path.clone(), movement))
        })
    };
    let Some((binding_path, node_path, movement)) = parsed else {
      return self.deny_broker_call(id);
    };
    let command_id = derived_command_id(
      &managed.run_id,
      &descriptor.name,
      &descriptor.version,
      request_id,
      index,
    );
    {
      let mut inner = self.inner.lock().expect("skill manager lock");
      if inner.stopping {
        return (1, "{}".into());
      }
      let Some(record) = inner.records.get_mut(id) else {
        return (1, "{}".into());
      };
      if record.cancel.load(Ordering::Acquire) || record.release_latched {
        record.release_latched = true;
        return (1, "{}".into());
      }
      record.command_ids.push(command_id.clone());
      if let Some(accounting) = record.snapshot.accounting.as_mut() {
        accounting.primitive_calls += 1;
      } else {
        record.snapshot.accounting = Some(v1::SkillExecutionAccounting {
          primitive_calls: 1,
          ..Default::default()
        });
      }
    }
    if cancel.load(Ordering::Acquire) {
      return self.deny_broker_call(id);
    }
    let admission = AdmissionContext {
      run_id: managed.run_id.clone(),
      active_revision: Some(managed.active_revision.clone()),
      authority_role: AuthorityRole::EgiaControl,
      ingress: AdmissionIngress::SkillWorker,
      binding_path,
      primitive: name.into(),
      primitive_version: version.into(),
    };
    let command = CommandRequest {
      node_path,
      meta: CommandMeta {
        command_id: command_id.clone(),
        deadline_monotonic_ns: SystemClock
          .monotonic_ns()
          .saturating_add(remaining.as_nanos().min(u64::MAX as u128) as u64),
      },
      movement,
    };
    drop(_gate);
    let outcome = self
      .runtime
      .dispatcher()
      .dispatch_skill_movement(admission, command, cancel);
    if outcome.status == OutcomeStatus::OutcomeUnknown {
      if let Ok(mut inner) = self.inner.lock() {
        if let Some(record) = inner.records.get_mut(id) {
          record.uncertain = true;
        }
      }
    }
    if outcome.status != OutcomeStatus::Completed {
      if let Ok(mut inner) = self.inner.lock() {
        if let Some(record) = inner.records.get_mut(id) {
          record.release_latched = true;
        }
      }
    }
    let journal = self
      .runtime
      .dispatcher()
      .gateway_operation_authorized(&command_id, &managed.run_id, AuthorityRole::EgiaControl)
      .ok()
      .flatten();
    let accepted = journal
      .as_ref()
      .and_then(|item| item.accepted)
      .unwrap_or(false);
    let status = match outcome.status {
      OutcomeStatus::Completed => "succeeded",
      OutcomeStatus::Failed => "failed",
      OutcomeStatus::Cancelled => "cancelled",
      OutcomeStatus::Rejected => "rejected",
      OutcomeStatus::InProgress => "pending",
      OutcomeStatus::OutcomeUnknown => "outcome_unknown",
    };
    let acknowledgement = serde_json::json!({
      "type":"primitive.ack", "primitive":name, "primitive_version":version,
      "accepted":accepted, "reason":outcome.reason,
      "reason_code":journal.as_ref().and_then(|item| item.rejection_code).map(|code| code.as_str()),
      "command_id":command_id, "sequence":1, "result":{"status":status}
    })
    .to_string();
    let host_status = if outcome.status == OutcomeStatus::Completed {
      0
    } else if matches!(
      outcome.status,
      OutcomeStatus::OutcomeUnknown | OutcomeStatus::InProgress
    ) {
      4
    } else {
      1
    };
    (host_status, acknowledgement)
  }
}

/// Pure pre-ready admission shared by private candidate inspection and launch.
pub(super) fn validate_catalog_readiness(
  profile: &aether_hal::Profile,
  managed: bool,
  catalog: &Catalog,
) -> Result<(), &'static str> {
  catalog
    .validate_broker_coverage()
    .map_err(|_| "catalog_broker_unavailable")?;
  let bindings = crate::managed_admission::admitted_bindings(profile);
  for descriptor in catalog.descriptors() {
    for primitive in &descriptor.primitives {
      let eligible = bindings.iter().any(|binding| {
        if binding.primitive != primitive.name || binding.primitive_version != primitive.version {
          return false;
        }
        let Some(node) = profile.node(&binding.node_path) else {
          return false;
        };
        let Some(movement) = &node.movement else {
          return false;
        };
        match primitive.name.as_str() {
          "base.velocity" => {
            node.kind == aether_hal::NodeKind::Base
              && movement.modes.iter().any(|mode| mode == "body_velocity")
              && movement.limits.max_linear_m_s.is_some()
              && movement.limits.max_angular_rad_s.is_some()
              && movement.limits.max_velocity_lease_ms.is_some()
          }
          "arm.movej" => {
            node.kind == aether_hal::NodeKind::Manipulator
              && movement.modes.iter().any(|mode| mode == "joint_positions")
              && movement.limits.max_joint_position_rad.is_some()
              && movement.limits.max_joint_speed_rad_s.is_some()
              && node.joint_names.is_some()
          }
          _ => false,
        }
      });
      if !managed || !eligible {
        return Err("catalog_binding_unavailable");
      }
    }
  }
  let wire = v1::ListSkillsResponse {
    skills: catalog
      .descriptors()
      .iter()
      .map(public_descriptor)
      .collect::<Result<_, _>>()
      .map_err(|_| "abi_descriptor_invalid")?,
  };
  if wire.encoded_len() > MAX_LIST_WIRE {
    return Err("catalog_list_oversize");
  }
  Ok(())
}

fn derived_command_id(
  run_id: &str,
  name: &str,
  version: &str,
  request_id: &str,
  index: u64,
) -> String {
  let mut digest = Sha256::new();
  digest.update(b"eos-skill-primitive-v1\0");
  for value in [run_id, name, version, request_id] {
    digest.update((value.len() as u32).to_le_bytes());
    digest.update(value.as_bytes());
  }
  digest.update(index.to_le_bytes());
  format!("sk-{:x}", digest.finalize())
}

fn list_wire_len(inner: &Inner) -> usize {
  v1::ListExecutionsResponse {
    executions: inner
      .order
      .iter()
      .filter_map(|id| inner.records.get(id).map(|record| record.snapshot.clone()))
      .collect(),
  }
  .encoded_len()
}

fn make_list_space(inner: &mut Inner, added: usize, protected: Option<&str>) -> bool {
  while list_wire_len(inner).saturating_add(added) > MAX_LIST_WIRE {
    if !evict_oldest_terminal(inner, protected) {
      return false;
    }
  }
  true
}

fn evict_oldest_terminal(inner: &mut Inner, protected: Option<&str>) -> bool {
  let Some(index) = inner.order.iter().position(|id| {
    Some(id.as_str()) != protected
      && inner
        .records
        .get(id)
        .is_some_and(|record| terminal(record.snapshot.status))
  }) else {
    return false;
  };
  let id = inner.order.remove(index).expect("selected retained record");
  if let Some(mut record) = inner.records.remove(&id) {
    if let Some(timer) = record.timer.take() {
      timer.abort();
    }
    inner.by_request.remove(&record.snapshot.request_id);
  }
  true
}

fn transition(
  record: &mut Record,
  status: v1::ExecutionStatus,
  reason: &str,
  result: Option<prost_types::Struct>,
) {
  record.snapshot.status = status as i32;
  record.snapshot.sequence += 1;
  record.snapshot.reason_code = reason.into();
  record.snapshot.result = result;
  if status == v1::ExecutionStatus::Running {
    record.snapshot.started_at = Some(now_timestamp());
  }
  if terminal(status as i32) {
    record.snapshot.finished_at = Some(now_timestamp());
    if let Some(timer) = record.timer.take() {
      timer.abort();
    }
  }
  if record.history.len() >= MAX_HISTORY {
    record.history.pop_front();
  }
  record.history.push_back(record.snapshot.clone());
  let _ = record.updates.send(record.snapshot.clone());
}

fn terminal(status: i32) -> bool {
  status >= v1::ExecutionStatus::Succeeded as i32
}

fn now_nanos() -> u128 {
  SystemTime::now()
    .duration_since(UNIX_EPOCH)
    .unwrap_or_default()
    .as_nanos()
}
fn now_timestamp() -> prost_types::Timestamp {
  let now = SystemTime::now()
    .duration_since(UNIX_EPOCH)
    .unwrap_or_default();
  prost_types::Timestamp {
    seconds: now.as_secs() as i64,
    nanos: now.subsec_nanos() as i32,
  }
}
fn add_duration(time: &prost_types::Timestamp, duration: Duration) -> prost_types::Timestamp {
  let nanos = time.nanos as u64 + u64::from(duration.subsec_nanos());
  prost_types::Timestamp {
    seconds: time.seconds + duration.as_secs() as i64 + (nanos / 1_000_000_000) as i64,
    nanos: (nanos % 1_000_000_000) as i32,
  }
}
fn parse_timeout(value: Option<&prost_types::Duration>) -> Result<Option<Duration>, Status> {
  let Some(value) = value else {
    return Ok(None);
  };
  if value.seconds < 0
    || value.seconds > 3600
    || value.nanos < 0
    || value.nanos >= 1_000_000_000
    || (value.seconds == 0 && value.nanos == 0)
    || (value.seconds == 3600 && value.nanos > 0)
  {
    return Err(Status::invalid_argument(
      "timeout must be positive and at most one hour",
    ));
  }
  Ok(Some(Duration::new(
    value.seconds as u64,
    value.nanos as u32,
  )))
}

fn public_descriptor(descriptor: &HostDescriptor) -> Result<v1::SkillDescriptor, String> {
  Ok(v1::SkillDescriptor {
    reference: Some(v1::SkillReference {
      name: descriptor.name.clone(),
      version: descriptor.version.clone(),
    }),
    description: if descriptor.description.is_empty() {
      "Native skill".into()
    } else {
      descriptor.description.clone()
    },
    parameters: Some(json_to_struct(&descriptor.parameters_schema).map_err(|e| e.to_string())?),
    returns: Some(json_to_struct(&descriptor.returns_schema).map_err(|e| e.to_string())?),
    primitives: descriptor
      .primitives
      .iter()
      .map(|primitive| v1::SkillPrimitiveReference {
        name: primitive.name.clone(),
        version: primitive.version.clone(),
      })
      .collect(),
    reentrant: descriptor.reentrant,
    ..Default::default()
  })
}

fn struct_to_json(value: &prost_types::Struct) -> Result<serde_json::Value, Status> {
  let mut fields = serde_json::Map::new();
  for (key, value) in &value.fields {
    fields.insert(key.clone(), value_to_json(value)?);
  }
  Ok(serde_json::Value::Object(fields))
}
fn value_to_json(value: &prost_types::Value) -> Result<serde_json::Value, Status> {
  use prost_types::value::Kind;
  Ok(
    match value
      .kind
      .as_ref()
      .ok_or_else(|| Status::invalid_argument("value kind required"))?
    {
      Kind::NullValue(_) => serde_json::Value::Null,
      Kind::BoolValue(value) => (*value).into(),
      Kind::NumberValue(value) => serde_json::Number::from_f64(*value)
        .ok_or_else(|| Status::invalid_argument("nonfinite number"))?
        .into(),
      Kind::StringValue(value) => value.clone().into(),
      Kind::StructValue(value) => struct_to_json(value)?,
      Kind::ListValue(values) => serde_json::Value::Array(
        values
          .values
          .iter()
          .map(value_to_json)
          .collect::<Result<_, _>>()?,
      ),
    },
  )
}
fn json_to_struct(value: &serde_json::Value) -> Result<prost_types::Struct, Status> {
  let fields = value
    .as_object()
    .ok_or_else(|| Status::invalid_argument("object required"))?;
  Ok(prost_types::Struct {
    fields: fields
      .iter()
      .map(|(key, value)| Ok((key.clone(), json_to_value(value)?)))
      .collect::<Result<_, Status>>()?,
  })
}
fn json_to_value(value: &serde_json::Value) -> Result<prost_types::Value, Status> {
  use prost_types::value::Kind;
  let kind = match value {
    serde_json::Value::Null => Kind::NullValue(0),
    serde_json::Value::Bool(value) => Kind::BoolValue(*value),
    serde_json::Value::Number(value) => Kind::NumberValue(
      value
        .as_f64()
        .ok_or_else(|| Status::invalid_argument("invalid number"))?,
    ),
    serde_json::Value::String(value) => Kind::StringValue(value.clone()),
    serde_json::Value::Array(values) => Kind::ListValue(prost_types::ListValue {
      values: values.iter().map(json_to_value).collect::<Result<_, _>>()?,
    }),
    serde_json::Value::Object(_) => Kind::StructValue(json_to_struct(value)?),
  };
  Ok(prost_types::Value { kind: Some(kind) })
}

pub struct SkillWatchStream {
  manager: Arc<SkillManager>,
  replay: VecDeque<v1::SkillExecution>,
  live: tokio_stream::wrappers::BroadcastStream<v1::SkillExecution>,
  terminal_now: bool,
  ended: bool,
  last_sequence: u64,
}
impl SkillWatchStream {
  fn new(
    manager: Arc<SkillManager>,
    replay: Vec<v1::SkillExecution>,
    live: tokio::sync::broadcast::Receiver<v1::SkillExecution>,
    terminal_now: bool,
    after: u64,
  ) -> Self {
    Self {
      manager,
      replay: replay.into(),
      live: tokio_stream::wrappers::BroadcastStream::new(live),
      terminal_now,
      ended: false,
      last_sequence: after,
    }
  }
}
impl Drop for SkillWatchStream {
  fn drop(&mut self) {
    if let Ok(mut inner) = self.manager.inner.lock() {
      inner.watchers = inner.watchers.saturating_sub(1);
    }
  }
}
impl tokio_stream::Stream for SkillWatchStream {
  type Item = Result<v1::SkillExecution, Status>;
  fn poll_next(
    mut self: std::pin::Pin<&mut Self>,
    cx: &mut std::task::Context<'_>,
  ) -> std::task::Poll<Option<Self::Item>> {
    if self.ended {
      return std::task::Poll::Ready(None);
    }
    if let Some(item) = self.replay.pop_front() {
      self.last_sequence = item.sequence;
      if terminal(item.status) {
        self.ended = true;
      }
      return std::task::Poll::Ready(Some(Ok(item)));
    }
    if self.terminal_now {
      self.ended = true;
      return std::task::Poll::Ready(None);
    }
    loop {
      match std::pin::Pin::new(&mut self.live).poll_next(cx) {
        std::task::Poll::Ready(Some(Ok(item))) if item.sequence <= self.last_sequence => continue,
        std::task::Poll::Ready(Some(Ok(item))) => {
          if self.last_sequence != 0 && item.sequence != self.last_sequence + 1 {
            self.ended = true;
            return std::task::Poll::Ready(Some(Err(Status::out_of_range("lifecycle replay gap"))));
          }
          self.last_sequence = item.sequence;
          if terminal(item.status) {
            self.ended = true;
          }
          return std::task::Poll::Ready(Some(Ok(item)));
        }
        std::task::Poll::Ready(Some(Err(_))) => {
          self.ended = true;
          return std::task::Poll::Ready(Some(Err(Status::out_of_range(
            "lifecycle watcher lagged",
          ))));
        }
        std::task::Poll::Ready(None) => {
          self.ended = true;
          return std::task::Poll::Ready(None);
        }
        std::task::Poll::Pending => return std::task::Poll::Pending,
      }
    }
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  fn profile() -> aether_hal::Profile {
    aether_hal::Profile::from_toml(include_str!(concat!(
      env!("CARGO_MANIFEST_DIR"),
      "/../../etc/eos/backends/mock-cxx.toml"
    )))
    .unwrap()
  }

  fn descriptor(description: &str) -> HostDescriptor {
    HostDescriptor {
      name: "example.observe".into(),
      version: "1.0.0".into(),
      package_id: "example/skill".into(),
      package_version: "1.0.0".into(),
      description: description.into(),
      parameters_schema: serde_json::json!({"type":"object"}),
      returns_schema: serde_json::json!({"type":"object"}),
      primitives: Vec::new(),
      reentrant: false,
      library_index: 0,
      descriptor_index: 0,
    }
  }

  fn empty_manager() -> Arc<SkillManager> {
    let profile = profile();
    let runtime = Arc::new(Runtime::from_cxx_mock_profile(profile, Arc::new(SystemClock)).unwrap());
    let projection =
      Projection::with_camera_cache(runtime, Arc::new(CameraCache::default())).unwrap();
    SkillManager::new(
      &projection,
      Arc::new(Catalog::empty()),
      PathBuf::new(),
      None,
    )
    .unwrap()
  }

  #[test]
  fn test_readiness_rejects_unbound_base_and_arm_primitives() {
    for primitive in ["base.velocity", "arm.movej"] {
      let mut skill = descriptor("bounded");
      skill
        .primitives
        .push(crate::host_skills::PrimitiveReference {
          name: primitive.into(),
          version: "1.0.0".into(),
        });
      let catalog = Catalog::from_descriptors_for_test(vec![skill]);
      assert_eq!(
        validate_catalog_readiness(&profile(), true, &catalog),
        Err("catalog_binding_unavailable")
      );
    }
  }

  #[test]
  fn test_readiness_accepts_exact_list_limit_and_rejects_one_byte_over() {
    let mut skills = Vec::new();
    for index in 0..4 {
      let mut skill = descriptor("bounded");
      skill.name = format!("example.skill{index}");
      skill.parameters_schema =
        serde_json::json!({"type":"object","description":"x".repeat(780_000)});
      skills.push(skill);
    }
    let encoded = |skills: &[HostDescriptor]| -> usize {
      v1::ListSkillsResponse {
        skills: skills
          .iter()
          .map(public_descriptor)
          .collect::<Result<_, _>>()
          .unwrap(),
      }
      .encoded_len()
    };
    let remainder = MAX_LIST_WIRE - encoded(&skills);
    let last = skills.last_mut().unwrap();
    last.parameters_schema =
      serde_json::json!({"type":"object","description":"x".repeat(780_000 + remainder)});
    assert_eq!(encoded(&skills), MAX_LIST_WIRE);
    let catalog = Catalog::from_descriptors_for_test(skills.clone());
    assert_eq!(
      validate_catalog_readiness(&profile(), true, &catalog),
      Ok(())
    );
    skills.last_mut().unwrap().parameters_schema =
      serde_json::json!({"type":"object","description":"x".repeat(780_001 + remainder)});
    let catalog = Catalog::from_descriptors_for_test(skills);
    assert_eq!(
      validate_catalog_readiness(&profile(), true, &catalog),
      Err("catalog_list_oversize")
    );
  }

  #[test]
  fn lifecycle_event_failure_prevents_clean_host_stop() {
    let manager = empty_manager();
    assert!(manager.wait_stopped().is_ok());
    manager.event_failed.store(true, Ordering::Release);
    assert_eq!(
      manager.wait_stopped().unwrap_err(),
      "skill lifecycle event unavailable"
    );
  }

  #[test]
  fn unreaped_post_spawn_failure_keeps_worker_slot_and_fails_stop() {
    let manager = empty_manager();
    let descriptor = descriptor("Native skill");
    {
      let mut inner = manager.inner.lock().unwrap();
      inner.active = 1;
      inner
        .active_nonreentrant
        .insert((descriptor.name.clone(), descriptor.version.clone()));
      inner.records.insert(
        "ex-stuck".into(),
        record("ex-stuck", v1::ExecutionStatus::Running, 0),
      );
    }
    manager.finish(
      "ex-stuck",
      &descriptor,
      v1::ExecutionStatus::Succeeded,
      "worker.succeeded",
      Some(prost_types::Struct::default()),
      Some(false),
      None,
    );
    let inner = manager.inner.lock().unwrap();
    assert_eq!(inner.active, 1);
    assert!(inner.reap_failed);
    let stuck = inner.records.get("ex-stuck").unwrap();
    assert_eq!(stuck.snapshot.status, v1::ExecutionStatus::Failed as i32);
    assert_eq!(stuck.snapshot.reason_code, "worker.reap_failed");
    assert!(stuck.snapshot.result.is_none());
    assert!(inner
      .active_nonreentrant
      .contains(&(descriptor.name, descriptor.version)));
    drop(inner);
    assert_eq!(
      manager.wait_stopped().unwrap_err(),
      "skill worker stop deadline exceeded"
    );
  }

  fn record(id: &str, status: v1::ExecutionStatus, payload_len: usize) -> Record {
    let (updates, _) = tokio::sync::broadcast::channel(MAX_HISTORY);
    let result = if payload_len == 0 {
      None
    } else {
      Some(prost_types::Struct {
        fields: [(
          "payload".into(),
          prost_types::Value {
            kind: Some(prost_types::value::Kind::StringValue(
              "x".repeat(payload_len),
            )),
          },
        )]
        .into(),
      })
    };
    let snapshot = v1::SkillExecution {
      execution_id: id.into(),
      request_id: id.into(),
      status: status as i32,
      sequence: 1,
      result,
      ..Default::default()
    };
    Record {
      snapshot: snapshot.clone(),
      signature: String::new(),
      descriptor: descriptor("Native skill"),
      arguments_json: "{}".into(),
      deadline: None,
      timer: None,
      history: VecDeque::from([snapshot]),
      updates,
      cancel: Arc::new(AtomicBool::new(false)),
      broker_gate: Arc::new(Mutex::new(())),
      worker: None,
      command_ids: Vec::new(),
      uncertain: false,
      release_latched: false,
      cancel_started: false,
      cancel_pending: false,
    }
  }

  #[tokio::test]
  async fn terminal_transition_aborts_long_deadline_timer() {
    let mut record = record("ex-timer", v1::ExecutionStatus::Running, 0);
    let timer = tokio::spawn(async { std::future::pending::<()>().await });
    let observer = timer.abort_handle();
    record.timer = Some(timer);
    transition(
      &mut record,
      v1::ExecutionStatus::Succeeded,
      "worker.succeeded",
      None,
    );
    tokio::task::yield_now().await;
    assert!(record.timer.is_none());
    assert!(observer.is_finished());
  }

  #[test]
  fn public_projection_accepts_old_prefix_empty_description() {
    let public = public_descriptor(&descriptor("")).unwrap();
    assert_eq!(public.description, "Native skill");
    assert_eq!(
      public
        .parameters
        .unwrap()
        .fields
        .get("type")
        .and_then(|value| value.kind.as_ref()),
      Some(&prost_types::value::Kind::StringValue("object".into()))
    );
  }

  #[test]
  fn utf8_request_identity_is_length_prefixed_and_run_scoped() {
    let first = derived_command_id("run_1", "example.observe", "1.0.0", "请求-一", 0);
    assert_eq!(first.len(), 67);
    assert!(first.starts_with("sk-"));
    assert_eq!(
      first,
      derived_command_id("run_1", "example.observe", "1.0.0", "请求-一", 0)
    );
    assert_ne!(
      first,
      derived_command_id("run_2", "example.observe", "1.0.0", "请求-一", 0)
    );
    assert_ne!(
      first,
      derived_command_id("run_1", "example.observe", "1.0.0", "请求-一", 1)
    );
  }

  #[test]
  fn timeout_and_object_boundaries_fail_closed() {
    assert!(parse_timeout(None).unwrap().is_none());
    assert!(parse_timeout(Some(&prost_types::Duration {
      seconds: 0,
      nanos: 1
    }))
    .is_ok());
    assert!(parse_timeout(Some(&prost_types::Duration {
      seconds: 3600,
      nanos: 0
    }))
    .is_ok());
    for value in [
      prost_types::Duration {
        seconds: 0,
        nanos: 0,
      },
      prost_types::Duration {
        seconds: 3600,
        nanos: 1,
      },
      prost_types::Duration {
        seconds: -1,
        nanos: 0,
      },
    ] {
      assert!(parse_timeout(Some(&value)).is_err());
    }
    assert!(json_to_struct(&serde_json::json!([])).is_err());
    let object = serde_json::json!({"b":[true,1.5,null,{"x":"y"}]});
    assert_eq!(
      struct_to_json(&json_to_struct(&object).unwrap()).unwrap(),
      object
    );
  }

  #[test]
  fn complete_list_evicts_terminal_only_and_rejects_full_active_budget() {
    let mut inner = Inner::default();
    inner.order.extend(["old".into(), "active".into()]);
    inner.records.insert(
      "old".into(),
      record("old", v1::ExecutionStatus::Succeeded, 2_000_000),
    );
    inner.records.insert(
      "active".into(),
      record("active", v1::ExecutionStatus::Running, 1_000_000),
    );
    inner.by_request.insert("old".into(), "old".into());
    inner.by_request.insert("active".into(), "active".into());
    assert!(make_list_space(&mut inner, 1_000_000, Some("active")));
    assert!(!inner.records.contains_key("old"));
    assert!(inner.records.contains_key("active"));
    assert!(!make_list_space(&mut inner, MAX_LIST_WIRE, Some("active")));
  }

  #[test]
  fn broker_gate_serializes_revocation_with_release_decision() {
    let gate = Arc::new(Mutex::new(()));
    let revoked = Arc::new(AtomicBool::new(false));
    let release = Arc::new(AtomicUsize::new(0));
    let held = gate.lock().unwrap();
    let child_gate = gate.clone();
    let child_revoked = revoked.clone();
    let child_release = release.clone();
    let worker = std::thread::spawn(move || {
      let _gate = child_gate.lock().unwrap();
      if !child_revoked.load(Ordering::Acquire) {
        child_release.fetch_add(1, Ordering::SeqCst);
      }
    });
    revoked.store(true, Ordering::Release);
    drop(held);
    worker.join().unwrap();
    assert_eq!(release.load(Ordering::SeqCst), 0);
  }
}
