use std::collections::BTreeMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};

use aether_common::time::Clock;
use aether_dispatcher::{CommandRequest, DispatchResult, Movement, OutcomeStatus};
use aether_hal::CommandMeta;
use aether_runtime::Runtime;
use serde_json::{json, Value};

const MAX_EXECUTIONS: usize = 128;
const MAX_ACTIVE: usize = 8;
const STEP_DEADLINE_NS: u64 = 5_000_000_000;

struct Execution {
  status: &'static str,
  results: Vec<DispatchResult>,
  active: Option<(String, String)>,
  cancelled: bool,
  reason: Option<String>,
  sequence: u64,
}

impl Execution {
  fn snapshot(&self, id: &str) -> Value {
    json!({"ok":true,"execution":{"execution_id":id,"status":self.status,
            "results":self.results,"reason":self.reason}})
  }
}

pub struct SkillManager {
  runtime: Arc<Runtime>,
  clock: Arc<dyn Clock>,
  executions: Mutex<BTreeMap<String, Execution>>,
  workers: Mutex<Vec<JoinHandle<()>>>,
  next_sequence: AtomicU64,
}

impl SkillManager {
  pub fn new(runtime: Arc<Runtime>, clock: Arc<dyn Clock>) -> Arc<Self> {
    Arc::new(Self {
      runtime,
      clock,
      executions: Mutex::new(BTreeMap::new()),
      workers: Mutex::new(Vec::new()),
      next_sequence: AtomicU64::new(0),
    })
  }

  pub fn execute(
    self: &Arc<Self>,
    path: &str,
    id: &str,
    movements: Vec<Movement>,
  ) -> Result<Value, String> {
    if id.is_empty()
      || id.len() > 64
      || !id
        .bytes()
        .all(|byte| byte.is_ascii_alphanumeric() || b"._-".contains(&byte))
    {
      return Err(
        "execution_id must be 1..64 ASCII letters, digits, dot, dash, or underscore".into(),
      );
    }
    let steps = self.runtime.skill_steps(path)?;
    if steps.len() != movements.len() {
      return Err("skill step count differs from graph definition".into());
    }
    let mut targets = Vec::with_capacity(steps.len());
    for (step, movement) in steps.iter().zip(&movements) {
      targets.push(self.runtime.control_target_for_movement(step, movement)?);
    }
    {
      let mut executions = self.executions.lock().expect("skill state poisoned");
      if executions.contains_key(id) {
        return Err("execution_id already exists".into());
      }
      if executions.len() >= MAX_EXECUTIONS {
        evict_oldest_terminal(&mut executions)?;
      }
      if executions
        .values()
        .filter(|entry| matches!(entry.status, "queued" | "running"))
        .count()
        >= MAX_ACTIVE
      {
        return Err("too many active skills".into());
      }
      executions.insert(
        id.into(),
        Execution {
          status: "queued",
          results: Vec::new(),
          active: None,
          cancelled: false,
          reason: None,
          sequence: self.next_sequence.fetch_add(1, Ordering::Relaxed),
        },
      );
    }
    let manager = self.clone();
    let id_owned = id.to_owned();
    let worker = thread::spawn(move || manager.run(&id_owned, steps, targets, movements));
    let mut workers = self.workers.lock().expect("skill workers poisoned");
    workers.retain(|worker| !worker.is_finished());
    workers.push(worker);
    Ok(self.result(id).expect("new execution missing"))
  }

  fn run(&self, id: &str, steps: Vec<String>, targets: Vec<String>, movements: Vec<Movement>) {
    for (index, ((step, target), movement)) in
      steps.into_iter().zip(targets).zip(movements).enumerate()
    {
      let command_id = format!("{id}:{index}");
      {
        let mut entries = self.executions.lock().expect("skill state poisoned");
        let entry = entries.get_mut(id).expect("running execution missing");
        if entry.cancelled {
          entry.status = "cancelled";
          return;
        }
        entry.status = "running";
        entry.active = Some((command_id.clone(), target.clone()));
      }
      let request = CommandRequest {
        node_path: String::new(),
        meta: CommandMeta {
          command_id,
          deadline_monotonic_ns: self.clock.monotonic_ns().saturating_add(STEP_DEADLINE_NS),
        },
        movement,
      };
      let outcome = self.runtime.invoke_control(&step, request);
      let mut entries = self.executions.lock().expect("skill state poisoned");
      let entry = entries.get_mut(id).expect("running execution missing");
      entry.active = None;
      if entry.cancelled {
        if let Ok(result) = outcome {
          entry.results.push(result);
        }
        entry.status = "cancelled";
        return;
      }
      match outcome {
        Ok(result) if result.status == OutcomeStatus::Completed => {
          entry.results.push(result);
        }
        Ok(result) => {
          entry.reason = Some(format!("step {index} returned {:?}", result.status));
          entry.results.push(result);
          drop(entries);
          self.stop_failed_step(id, &target);
          return;
        }
        Err(error) => {
          entry.reason = Some(error);
          drop(entries);
          self.stop_failed_step(id, &target);
          return;
        }
      }
    }
    if let Some(entry) = self
      .executions
      .lock()
      .expect("skill state poisoned")
      .get_mut(id)
    {
      entry.status = "completed";
    }
  }

  fn stop_failed_step(&self, id: &str, target: &str) {
    let stop = self.runtime.dispatcher().stop_node(target);
    let mut entries = self.executions.lock().expect("skill state poisoned");
    let entry = entries.get_mut(id).expect("failed execution missing");
    if let Err(error) = stop {
      entry.reason = Some(format!(
        "{}; stop failed: {error}",
        entry.reason.as_deref().unwrap_or("step failed")
      ));
    }
    entry.status = if entry.cancelled {
      "cancelled"
    } else {
      "failed"
    };
  }

  pub fn result(&self, id: &str) -> Option<Value> {
    self
      .executions
      .lock()
      .expect("skill state poisoned")
      .get(id)
      .map(|entry| entry.snapshot(id))
  }

  pub fn cancel(&self, id: &str) -> Result<Value, String> {
    let active = {
      let mut entries = self.executions.lock().expect("skill state poisoned");
      let entry = entries.get_mut(id).ok_or("unknown execution_id")?;
      if matches!(entry.status, "completed" | "cancelled" | "failed") {
        return Ok(entry.snapshot(id));
      }
      entry.cancelled = true;
      entry.active.clone()
    };
    if let Some((command_id, target)) = active {
      let _ = self.runtime.dispatcher().cancel(&command_id);
      let _ = self.runtime.dispatcher().stop_node(&target);
    }
    Ok(self.result(id).expect("cancelled execution missing"))
  }

  pub fn shutdown(&self) {
    let ids: Vec<String> = self
      .executions
      .lock()
      .expect("skill state poisoned")
      .iter()
      .filter(|(_, entry)| matches!(entry.status, "queued" | "running"))
      .map(|(id, _)| id.clone())
      .collect();
    for id in ids {
      let _ = self.cancel(&id);
    }
    for worker in self
      .workers
      .lock()
      .expect("skill workers poisoned")
      .drain(..)
    {
      let _ = worker.join();
    }
  }
}

fn evict_oldest_terminal(executions: &mut BTreeMap<String, Execution>) -> Result<(), String> {
  let oldest = executions
    .iter()
    .filter(|(_, entry)| matches!(entry.status, "completed" | "failed" | "cancelled"))
    .min_by_key(|(_, entry)| entry.sequence)
    .map(|(id, _)| id.clone())
    .ok_or("skill result capacity reached")?;
  executions.remove(&oldest);
  Ok(())
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn retention_evicts_oldest_terminal_but_preserves_running() {
    let mut entries = BTreeMap::new();
    for (id, status, sequence) in [
      ("running", "running", 0),
      ("old", "completed", 1),
      ("new", "failed", 2),
    ] {
      entries.insert(
        id.into(),
        Execution {
          status,
          results: Vec::new(),
          active: None,
          cancelled: false,
          reason: None,
          sequence,
        },
      );
    }
    evict_oldest_terminal(&mut entries).unwrap();
    assert!(entries.contains_key("running"));
    assert!(entries.contains_key("new"));
    assert!(!entries.contains_key("old"));
  }
}
