//! Bounded, EOS-local evidence delivery. No command payload enters this module.

use std::collections::VecDeque;
use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use aether_hal::DataClass;
use serde_json::{json, Value};

pub const MAX_ROUTES: usize = 256;
const GLOBAL_BYTES: usize = 64 * 1024 * 1024;
const FIFO_BYTES: usize = 1024 * 1024;
const FIFO_RECORDS: usize = 64;
const CREDIT_MAX: u32 = 1024;
const COST_UNIT: usize = 64 * 1024;
const VISITS_PER_SLICE: usize = 64;
const ATTEMPTS_PER_SLICE: usize = 64;
const ACCEPT_DEADLINE: Duration = Duration::from_millis(100);

#[derive(Clone)]
pub struct RouteConfig {
  pub source_node: String,
  pub source_output: String,
  pub target_node: String,
  pub target_input: String,
  pub data_type: String,
  pub class: DataClass,
  pub priority: i64,
  pub frequency_hz: Option<f64>,
}

impl RouteConfig {
  pub fn key(&self) -> (&str, &str, &str, &str) {
    (
      &self.source_node,
      &self.source_output,
      &self.target_node,
      &self.target_input,
    )
  }
}

struct Item<T> {
  value: T,
  bytes: usize,
}

struct Route<T> {
  config: RouteConfig,
  queue: VecDeque<Item<T>>,
  bytes: usize,
  credit: u32,
  last_token_ns: Option<u64>,
  in_flight: bool,
  in_flight_cancelled: bool,
  delivered_count: u64,
  cap_wait_count: u64,
  replaced_count: u64,
  overflow_count: u64,
  consumer_error_count: u64,
  cancelled_count: u64,
  gap_count: u64,
  last_delivery_ns: Option<u64>,
}

impl<T> Route<T> {
  fn new(config: RouteConfig) -> Self {
    Self {
      config,
      queue: VecDeque::new(),
      bytes: 0,
      credit: 0,
      last_token_ns: None,
      in_flight: false,
      in_flight_cancelled: false,
      delivered_count: 0,
      cap_wait_count: 0,
      replaced_count: 0,
      overflow_count: 0,
      consumer_error_count: 0,
      cancelled_count: 0,
      gap_count: 0,
      last_delivery_ns: None,
    }
  }

  fn token_ready(&self, now_ns: u64) -> bool {
    let Some(rate) = self.config.frequency_hz else {
      return true;
    };
    let Some(last) = self.last_token_ns else {
      return true;
    };
    let Some(elapsed) = now_ns.checked_sub(last).filter(|elapsed| *elapsed > 0) else {
      return false;
    };
    rate > 1e9 || (elapsed as f64) * rate >= 1e9
  }

  fn cost(&self) -> u32 {
    self.queue.front().map_or(0, |item| {
      item.bytes.div_ceil(COST_UNIT).clamp(1, CREDIT_MAX as usize) as u32
    })
  }

  fn status(&self) -> Value {
    json!({
      "source_node":self.config.source_node,"source_output":self.config.source_output,
      "target_node":self.config.target_node,"target_input":self.config.target_input,
      "data_type":self.config.data_type,"delivery_state":"active","routed":true,
      "priority":self.config.priority,"frequency_hz":self.config.frequency_hz,
      "weight":20-self.config.priority,
      "queue_class":if matches!(self.config.class, DataClass::Image | DataClass::Observation) {"latest"} else {"fifo"},
      "queue_depth":self.queue.len(),"delivered_count":self.delivered_count,
      "cap_wait_count":self.cap_wait_count,"replaced_count":self.replaced_count,
      "overflow_count":self.overflow_count,"consumer_error_count":self.consumer_error_count,
      "cancelled_count":self.cancelled_count,"gap_count":self.gap_count,
      "last_delivery_monotonic_ns":self.last_delivery_ns,
    })
  }
}

pub struct Scheduler<T> {
  routes: Vec<Route<T>>,
  cursor: usize,
  total_bytes: usize,
  closed: bool,
  drained: bool,
}

impl<T> Scheduler<T> {
  pub fn new(mut configs: Vec<RouteConfig>) -> Result<Self, String> {
    if configs.len() > MAX_ROUTES {
      return Err("too many live evidence routes".into());
    }
    configs.sort_by(|a, b| a.key().cmp(&b.key()));
    for (index, config) in configs.iter().enumerate() {
      if index > 0 && configs[index - 1].key() == config.key() {
        return Err("duplicate live evidence route".into());
      }
      if !(-20..=19).contains(&config.priority)
        || config
          .frequency_hz
          .is_some_and(|rate| !rate.is_finite() || rate <= 0.0)
        || !matches!(
          config.class,
          DataClass::Image | DataClass::Observation | DataClass::State | DataClass::Event
        )
      {
        return Err("invalid live evidence route".into());
      }
    }
    Ok(Self {
      routes: configs.into_iter().map(Route::new).collect(),
      cursor: 0,
      total_bytes: 0,
      closed: false,
      drained: false,
    })
  }

  pub fn len(&self) -> usize {
    self.routes.len()
  }

  pub fn configs(&self) -> Vec<RouteConfig> {
    self
      .routes
      .iter()
      .map(|route| route.config.clone())
      .collect()
  }

  pub fn enqueue(&mut self, index: usize, value: T, bytes: usize) -> Result<(), String> {
    if self.closed {
      return Err("evidence admission is closed".into());
    }
    let route = self.routes.get_mut(index).ok_or("unknown evidence route")?;
    let latest = matches!(
      route.config.class,
      DataClass::Image | DataClass::Observation
    );
    let old_bytes = if latest {
      route.queue.front().map_or(0, |item| item.bytes)
    } else {
      0
    };
    let next_route_bytes = route.bytes.saturating_sub(old_bytes).saturating_add(bytes);
    let next_global_bytes = self
      .total_bytes
      .saturating_sub(old_bytes)
      .saturating_add(bytes);
    let overflow = next_global_bytes > GLOBAL_BYTES
      || (!latest && (route.queue.len() >= FIFO_RECORDS || next_route_bytes > FIFO_BYTES));
    if overflow {
      route.overflow_count = route.overflow_count.saturating_add(1);
      route.gap_count = route.gap_count.saturating_add(1);
      return Ok(());
    }
    if latest && route.queue.pop_front().is_some() {
      route.replaced_count = route.replaced_count.saturating_add(1);
      route.gap_count = route.gap_count.saturating_add(1);
      self.total_bytes -= old_bytes;
      route.bytes -= old_bytes;
    }
    route.queue.push_back(Item { value, bytes });
    route.bytes += bytes;
    self.total_bytes += bytes;
    Ok(())
  }

  /// Seal admission and delivery without walking the queues. The owner calls
  /// this before Dispatcher emergency_stop, then drains after that stop call.
  pub fn seal(&mut self) {
    self.closed = true;
  }

  pub fn close(&mut self) {
    if self.drained {
      return;
    }
    self.seal();
    self.drained = true;
    for route in &mut self.routes {
      let cancelled =
        route.queue.len() as u64 + u64::from(route.in_flight && !route.in_flight_cancelled);
      route.cancelled_count = route.cancelled_count.saturating_add(cancelled);
      route.gap_count = route.gap_count.saturating_add(cancelled);
      route.in_flight_cancelled = route.in_flight;
      route.queue.clear();
      route.bytes = 0;
      route.credit = 0;
    }
    self.total_bytes = 0;
  }

  /// Discard one source's queued evidence after camera health loss. An item
  /// already in preparation is fenced from commit by its cancelled flag.
  pub fn clear_route(&mut self, index: usize) {
    let route = &mut self.routes[index];
    let cancelled =
      route.queue.len() as u64 + u64::from(route.in_flight && !route.in_flight_cancelled);
    route.cancelled_count = route.cancelled_count.saturating_add(cancelled);
    route.gap_count = route.gap_count.saturating_add(cancelled);
    route.in_flight_cancelled = route.in_flight;
    self.total_bytes = self.total_bytes.saturating_sub(route.bytes);
    route.queue.clear();
    route.bytes = 0;
    route.credit = 0;
  }

  pub fn status(&self) -> Vec<Value> {
    self.routes.iter().map(Route::status).collect()
  }
}

/// Fan out one immutable producer payload while the scheduler lock is held.
/// Rejected routes drop only an Arc handle, never clone the frame bytes.
pub fn fanout_shared_locked<T>(
  scheduler: &mut Scheduler<Arc<T>>,
  indices: &[usize],
  payload: &Arc<T>,
  bytes: usize,
) {
  for index in indices {
    let _ = scheduler.enqueue(*index, Arc::clone(payload), bytes);
  }
}

/// One bounded service slice. Preparation runs without the scheduler lock;
/// publication and final accounting share the admission/stop fence.
pub fn service_slice<T, F, C>(
  scheduler: &Arc<Mutex<Scheduler<T>>>,
  now_ns: u64,
  cancelled: &AtomicBool,
  mut prepare: F,
  mut commit: C,
) where
  F: FnMut(usize, T, &AtomicBool, Instant) -> Result<T, ()>,
  C: FnMut(usize, T) -> Result<u64, ()>,
{
  let count = scheduler.lock().expect("route lock poisoned").len();
  if count == 0 {
    return;
  }
  let mut visits = 0;
  let mut attempts = 0;
  while visits < VISITS_PER_SLICE.min(count) && attempts < ATTEMPTS_PER_SLICE {
    let index = {
      let mut state = scheduler.lock().expect("route lock poisoned");
      if state.closed {
        return;
      }
      let index = state.cursor;
      let route = &mut state.routes[index];
      route.credit = route
        .credit
        .saturating_add((20 - route.config.priority) as u32)
        .min(CREDIT_MAX);
      index
    };
    visits += 1;
    loop {
      if attempts >= ATTEMPTS_PER_SLICE {
        break;
      }
      let next = {
        let mut state = scheduler.lock().expect("route lock poisoned");
        if state.closed {
          return;
        }
        let route = &mut state.routes[index];
        if route.queue.is_empty() {
          route.credit = 0;
          None
        } else if route.in_flight {
          None
        } else if !route.token_ready(now_ns) {
          route.cap_wait_count = route.cap_wait_count.saturating_add(1);
          None
        } else if route.credit < route.cost() {
          None
        } else {
          let item = route.queue.pop_front().expect("checked queue");
          route.credit -= item.bytes.div_ceil(COST_UNIT).clamp(1, CREDIT_MAX as usize) as u32;
          route.bytes -= item.bytes;
          route.in_flight = true;
          route.in_flight_cancelled = false;
          if route.config.frequency_hz.is_some() {
            route.last_token_ns = Some(now_ns);
          }
          state.total_bytes -= item.bytes;
          Some(item.value)
        }
      };
      let Some(value) = next else {
        break;
      };
      attempts += 1;
      let deadline = Instant::now() + ACCEPT_DEADLINE;
      let staged = prepare(index, value, cancelled, deadline);
      let mut state = scheduler.lock().expect("route lock poisoned");
      let closed = state.closed;
      let route = &mut state.routes[index];
      if !route.in_flight_cancelled {
        if closed || cancelled.load(Ordering::Acquire) || Instant::now() >= deadline {
          route.cancelled_count = route.cancelled_count.saturating_add(1);
          route.gap_count = route.gap_count.saturating_add(1);
        } else if let Ok(value) = staged {
          if let Ok(commit_ns) = commit(index, value) {
            route.delivered_count = route.delivered_count.saturating_add(1);
            route.last_delivery_ns = Some(commit_ns);
          } else {
            route.consumer_error_count = route.consumer_error_count.saturating_add(1);
            route.gap_count = route.gap_count.saturating_add(1);
          }
        } else {
          if Instant::now() >= deadline {
            route.cancelled_count = route.cancelled_count.saturating_add(1);
          } else {
            route.consumer_error_count = route.consumer_error_count.saturating_add(1);
          }
          route.gap_count = route.gap_count.saturating_add(1);
        }
      }
      route.in_flight = false;
      if closed {
        return;
      }
    }
    let mut state = scheduler.lock().expect("route lock poisoned");
    state.cursor = (index + 1) % count;
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  fn test_slice<T: Copy, F>(
    scheduler: &Arc<Mutex<Scheduler<T>>>,
    now_ns: u64,
    cancelled: &AtomicBool,
    mut accept: F,
  ) where
    F: FnMut(usize, T, &AtomicBool, Instant) -> Result<(), ()>,
  {
    service_slice(
      scheduler,
      now_ns,
      cancelled,
      |index, value, cancelled, deadline| {
        accept(index, value, cancelled, deadline)?;
        Ok(value)
      },
      |_, _| Ok(now_ns),
    );
  }

  fn config(class: DataClass, priority: i64, rate: Option<f64>, id: usize) -> RouteConfig {
    RouteConfig {
      source_node: format!("eos/hardware/camera/{id}"),
      source_output: "frame".into(),
      target_node: format!("eos/interface/gate/{id}"),
      target_input: "frame".into(),
      data_type: "camera_frame".into(),
      class,
      priority,
      frequency_hz: rate,
    }
  }

  #[test]
  fn health_loss_clears_only_affected_gate_queue_and_counts_gap() {
    let mut routes = Scheduler::new(vec![
      config(DataClass::Image, 0, None, 1),
      config(DataClass::Image, 0, None, 2),
    ])
    .unwrap();
    routes.enqueue(0, 11u8, 5).unwrap();
    routes.enqueue(1, 22u8, 5).unwrap();
    routes.clear_route(0);
    let status = routes.status();
    assert_eq!(status[0]["queue_depth"], 0);
    assert_eq!(status[0]["gap_count"], 1);
    assert_eq!(status[1]["queue_depth"], 1);
    assert_eq!(status[1]["gap_count"], 0);
  }

  #[test]
  fn cap_handles_extreme_rates_and_backward_time() {
    let mut slow = Route::<u8>::new(config(DataClass::Image, 0, Some(f64::from_bits(1)), 0));
    slow.last_token_ns = Some(0);
    assert!(!slow.token_ready(u64::MAX));
    let mut fast = Route::<u8>::new(config(DataClass::Image, 0, Some(f64::MAX), 0));
    fast.last_token_ns = Some(100);
    assert!(!fast.token_ready(99));
    assert!(!fast.token_ready(100));
    assert!(fast.token_ready(101));
  }

  #[test]
  fn admission_rejects_excess_routes_and_non_evidence_class() {
    assert!(Scheduler::<u8>::new(
      (0..257)
        .map(|i| config(DataClass::Image, 0, None, i))
        .collect()
    )
    .is_err());
    assert!(Scheduler::<u8>::new(vec![config(DataClass::Command, 0, None, 0)]).is_err());
    let default = Scheduler::<u8>::new(vec![config(DataClass::Image, 0, None, 0)]).unwrap();
    assert_eq!(default.status()[0]["weight"], 20);
  }

  #[test]
  fn latest_replaces_and_fifo_rejects_newest() {
    let mut latest = Scheduler::new(vec![config(DataClass::Image, 0, None, 0)]).unwrap();
    for n in 0..3 {
      latest.enqueue(0, n, 10).unwrap();
    }
    assert_eq!(latest.routes[0].queue.front().unwrap().value, 2);
    assert_eq!(latest.routes[0].replaced_count, 2);
    assert_eq!(latest.routes[0].gap_count, 2);
    let mut fifo = Scheduler::new(vec![config(DataClass::Event, 0, None, 0)]).unwrap();
    for n in 0..65 {
      fifo.enqueue(0, n, 1).unwrap();
    }
    assert_eq!(fifo.routes[0].queue.len(), 64);
    assert_eq!(fifo.routes[0].queue.front().unwrap().value, 0);
    assert_eq!(fifo.routes[0].overflow_count, 1);
  }

  #[test]
  fn cursor_reaches_every_route_with_256_ready() {
    let scheduler = Arc::new(Mutex::new(
      Scheduler::new(
        (0..256)
          .map(|i| config(DataClass::Event, 19, None, i))
          .collect(),
      )
      .unwrap(),
    ));
    for i in 0..256 {
      scheduler.lock().unwrap().enqueue(i, i, 65536).unwrap();
    }
    let cancel = AtomicBool::new(false);
    let mut delivered = Vec::new();
    for slice in 0..4 {
      test_slice(&scheduler, slice, &cancel, |_, v, _, _| {
        delivered.push(v);
        Ok(())
      });
    }
    delivered.sort();
    assert_eq!(delivered, (0..256).collect::<Vec<_>>());
  }

  #[test]
  fn cursor_reaches_sixty_fifth_route_on_second_slice() {
    let scheduler = Arc::new(Mutex::new(
      Scheduler::new(
        (0..65)
          .map(|i| config(DataClass::Event, 19, None, i))
          .collect(),
      )
      .unwrap(),
    ));
    for i in 0..65 {
      scheduler.lock().unwrap().enqueue(i, i, 1).unwrap();
    }
    let cancel = AtomicBool::new(false);
    let mut delivered = Vec::new();
    test_slice(&scheduler, 0, &cancel, |_, value, _, _| {
      delivered.push(value);
      Ok(())
    });
    assert_eq!(delivered.len(), 64);
    assert!(!delivered.contains(&64));
    test_slice(&scheduler, 1, &cancel, |_, value, _, _| {
      delivered.push(value);
      Ok(())
    });
    assert!(delivered.contains(&64));
  }

  #[test]
  fn mixed_weight_cursor_visits_all_continuously_ready_routes() {
    let configs = (0..256)
      .map(|i| config(DataClass::Event, if i % 2 == 0 { -20 } else { 19 }, None, i))
      .collect();
    let scheduler = Arc::new(Mutex::new(Scheduler::new(configs).unwrap()));
    for index in 0..256 {
      for value in 0..40 {
        scheduler.lock().unwrap().enqueue(index, value, 1).unwrap();
      }
    }
    let cancel = AtomicBool::new(false);
    for slice in 0..256 {
      test_slice(&scheduler, slice, &cancel, |_, _, _, _| Ok(()));
      if scheduler
        .lock()
        .unwrap()
        .routes
        .iter()
        .all(|route| route.delivered_count > 0)
      {
        return;
      }
    }
    panic!("some continuously ready routes were starved by mixed weights");
  }

  #[test]
  fn saturated_shared_frame_fanout_does_not_hold_stop_seal_behind_payload_copies() {
    let scheduler = Arc::new(Mutex::new(
      Scheduler::new(
        (0..256)
          .map(|i| config(DataClass::Image, 0, None, i))
          .collect(),
      )
      .unwrap(),
    ));
    let frame = Arc::new(vec![0u8; 8 * 1024 * 1024]);
    let (producer_locked_tx, producer_locked_rx) = std::sync::mpsc::channel();
    let (producer_release_tx, producer_release_rx) = std::sync::mpsc::channel();
    let producer = {
      let scheduler = scheduler.clone();
      let frame = frame.clone();
      std::thread::spawn(move || {
        let mut state = scheduler.lock().unwrap();
        producer_locked_tx.send(()).unwrap();
        producer_release_rx.recv().unwrap();
        let start = Instant::now();
        fanout_shared_locked(
          &mut state,
          &(0..256).collect::<Vec<_>>(),
          &frame,
          8 * 1024 * 1024,
        );
        start.elapsed()
      })
    };
    producer_locked_rx
      .recv_timeout(Duration::from_secs(1))
      .unwrap();
    let (seal_waiting_tx, seal_waiting_rx) = std::sync::mpsc::channel();
    let seal = {
      let scheduler = scheduler.clone();
      std::thread::spawn(move || {
        assert!(
          scheduler.try_lock().is_err(),
          "producer must hold the route lock"
        );
        seal_waiting_tx.send(()).unwrap();
        let start = Instant::now();
        scheduler.lock().unwrap().seal();
        start.elapsed()
      })
    };
    seal_waiting_rx
      .recv_timeout(Duration::from_secs(1))
      .unwrap();
    producer_release_tx.send(()).unwrap();
    assert!(producer.join().unwrap() < Duration::from_millis(100));
    assert!(seal.join().unwrap() < Duration::from_millis(250));
    let guard = scheduler.lock().unwrap();
    assert_eq!(
      guard
        .routes
        .iter()
        .map(|route| route.queue.len())
        .sum::<usize>(),
      8
    );
    assert_eq!(
      guard
        .routes
        .iter()
        .map(|route| route.overflow_count)
        .sum::<u64>(),
      248
    );
    assert_eq!(Arc::strong_count(&frame), 9);
    assert!(guard
      .routes
      .iter()
      .flat_map(|route| route.queue.iter())
      .all(|item| Arc::ptr_eq(&item.value, &frame)));
  }

  #[test]
  fn stop_cancels_inflight_once_and_fences_late_accept() {
    let scheduler = Arc::new(Mutex::new(
      Scheduler::new(vec![config(DataClass::Image, 0, None, 0)]).unwrap(),
    ));
    scheduler.lock().unwrap().enqueue(0, 7, 1).unwrap();
    let cancelled = Arc::new(AtomicBool::new(false));
    let worker = {
      let scheduler = scheduler.clone();
      let cancelled = cancelled.clone();
      std::thread::spawn(move || {
        test_slice(&scheduler, 0, &cancelled, |_, _, cancelled, _| {
          while !cancelled.load(Ordering::Acquire) {
            std::thread::yield_now();
          }
          Err(())
        })
      })
    };
    while !scheduler.lock().unwrap().routes[0].in_flight {
      std::thread::yield_now();
    }
    cancelled.store(true, Ordering::Release);
    scheduler.lock().unwrap().close();
    worker.join().unwrap();
    let route = &scheduler.lock().unwrap().routes[0];
    assert_eq!(
      (
        route.delivered_count,
        route.cancelled_count,
        route.gap_count
      ),
      (0, 1, 1)
    );
  }

  #[test]
  fn accepted_before_cancel_is_counted_as_delivery() {
    let scheduler = Arc::new(Mutex::new(
      Scheduler::new(vec![config(DataClass::Image, 0, None, 0)]).unwrap(),
    ));
    scheduler.lock().unwrap().enqueue(0, 1, 1).unwrap();
    let cancelled = AtomicBool::new(false);
    test_slice(&scheduler, 0, &cancelled, |_, _, _, _| Ok(()));
    scheduler.lock().unwrap().close();
    let guard = scheduler.lock().unwrap();
    assert_eq!(guard.routes[0].delivered_count, 1);
    assert_eq!(guard.routes[0].cancelled_count, 0);
    assert_eq!(guard.routes[0].gap_count, 0);
  }

  #[test]
  fn stop_cancels_backlog_and_rejects_new_admission() {
    let mut scheduler = Scheduler::new(vec![config(DataClass::Event, 0, None, 0)]).unwrap();
    for value in 0..3 {
      scheduler.enqueue(0, value, 1).unwrap();
    }
    scheduler.close();
    assert_eq!(scheduler.routes[0].cancelled_count, 3);
    assert_eq!(scheduler.routes[0].gap_count, 3);
    assert_eq!(scheduler.routes[0].queue.len(), 0);
    assert!(scheduler.enqueue(0, 4, 1).is_err());
  }

  #[test]
  fn acceptance_after_deadline_is_cancelled_once() {
    let scheduler = Arc::new(Mutex::new(
      Scheduler::new(vec![config(DataClass::Image, 0, None, 0)]).unwrap(),
    ));
    scheduler.lock().unwrap().enqueue(0, 1, 1).unwrap();
    let cancelled = AtomicBool::new(false);
    test_slice(&scheduler, 0, &cancelled, |_, _, _, deadline| {
      while Instant::now() <= deadline {
        std::thread::yield_now();
      }
      Ok(())
    });
    let guard = scheduler.lock().unwrap();
    assert_eq!(guard.routes[0].delivered_count, 0);
    assert_eq!(guard.routes[0].cancelled_count, 1);
    assert_eq!(guard.routes[0].gap_count, 1);
  }

  #[test]
  fn timed_out_preparation_never_publishes_a_gate_frame() {
    let scheduler = Arc::new(Mutex::new(
      Scheduler::new(vec![config(DataClass::Image, 0, None, 0)]).unwrap(),
    ));
    scheduler.lock().unwrap().enqueue(0, 7u8, 1).unwrap();
    let delivered = Arc::new(Mutex::new(None));
    let cancelled = AtomicBool::new(false);
    service_slice(
      &scheduler,
      0,
      &cancelled,
      |_, value, _, deadline| {
        while Instant::now() < deadline {
          std::thread::yield_now();
        }
        Ok(value)
      },
      {
        let delivered = delivered.clone();
        move |_, value| {
          *delivered.lock().unwrap() = Some(value);
          Ok(1)
        }
      },
    );
    assert_eq!(*delivered.lock().unwrap(), None);
    let guard = scheduler.lock().unwrap();
    assert_eq!(
      (
        guard.routes[0].delivered_count,
        guard.routes[0].cancelled_count,
        guard.routes[0].gap_count
      ),
      (0, 1, 1)
    );
  }

  #[test]
  fn sealed_scheduler_fences_paused_consumer_publication_and_producer_admission() {
    let scheduler = Arc::new(Mutex::new(
      Scheduler::new(vec![config(DataClass::Image, 0, None, 0)]).unwrap(),
    ));
    scheduler.lock().unwrap().enqueue(0, 7u8, 1).unwrap();
    let delivered = Arc::new(Mutex::new(None));
    let cancelled = Arc::new(AtomicBool::new(false));
    let (entered_tx, entered_rx) = std::sync::mpsc::channel();
    let (release_tx, release_rx) = std::sync::mpsc::channel();
    let worker = {
      let scheduler = scheduler.clone();
      let delivered = delivered.clone();
      let cancelled = cancelled.clone();
      std::thread::spawn(move || {
        service_slice(
          &scheduler,
          0,
          &cancelled,
          |_, value, _, _| {
            entered_tx.send(()).unwrap();
            release_rx.recv().unwrap();
            Ok(value)
          },
          |_, value| {
            *delivered.lock().unwrap() = Some(value);
            Ok(1)
          },
        )
      })
    };
    entered_rx.recv_timeout(Duration::from_secs(1)).unwrap();
    let (producer_ready_tx, producer_ready_rx) = std::sync::mpsc::channel();
    let (producer_release_tx, producer_release_rx) = std::sync::mpsc::channel();
    let producer = {
      let scheduler = scheduler.clone();
      std::thread::spawn(move || {
        producer_ready_tx.send(()).unwrap();
        producer_release_rx.recv().unwrap();
        scheduler.lock().unwrap().enqueue(0, 8, 1)
      })
    };
    producer_ready_rx
      .recv_timeout(Duration::from_secs(1))
      .unwrap();
    // The same seal is used before Dispatcher emergency_stop. Queue draining
    // happens only after it; neither paused producer nor consumer can publish.
    scheduler.lock().unwrap().seal();
    producer_release_tx.send(()).unwrap();
    assert!(producer.join().unwrap().is_err());
    release_tx.send(()).unwrap();
    worker.join().unwrap();
    scheduler.lock().unwrap().close();
    assert_eq!(*delivered.lock().unwrap(), None);
    let guard = scheduler.lock().unwrap();
    assert_eq!(
      (
        guard.routes[0].delivered_count,
        guard.routes[0].cancelled_count,
        guard.routes[0].gap_count
      ),
      (0, 1, 1)
    );
  }

  #[test]
  fn delivery_timestamp_is_sampled_at_acceptance_commit() {
    let scheduler = Arc::new(Mutex::new(
      Scheduler::new(vec![config(DataClass::Image, 0, None, 0)]).unwrap(),
    ));
    scheduler.lock().unwrap().enqueue(0, 1u8, 1).unwrap();
    let commit_clock = std::sync::atomic::AtomicU64::new(10);
    let cancelled = AtomicBool::new(false);
    service_slice(
      &scheduler,
      10,
      &cancelled,
      |_, value, _, _| {
        commit_clock.store(25, Ordering::Release);
        Ok(value)
      },
      |_, _| Ok(commit_clock.load(Ordering::Acquire)),
    );
    let guard = scheduler.lock().unwrap();
    assert_eq!(guard.routes[0].delivered_count, 1);
    assert_eq!(guard.routes[0].last_delivery_ns, Some(25));
  }

  #[test]
  fn delivery_cap_waits_without_gap_and_consumes_failed_attempt() {
    let scheduler = Arc::new(Mutex::new(
      Scheduler::new(vec![config(DataClass::Event, -20, Some(2.0), 0)]).unwrap(),
    ));
    let cancelled = AtomicBool::new(false);
    scheduler.lock().unwrap().enqueue(0, 1, 1).unwrap();
    test_slice(&scheduler, 0, &cancelled, |_, _, _, _| Err(()));
    scheduler.lock().unwrap().enqueue(0, 2, 1).unwrap();
    test_slice(&scheduler, 1, &cancelled, |_, _, _, _| Ok(()));
    {
      let guard = scheduler.lock().unwrap();
      assert_eq!(guard.routes[0].consumer_error_count, 1);
      assert_eq!(guard.routes[0].cap_wait_count, 1);
      assert_eq!(guard.routes[0].gap_count, 1);
      assert_eq!(guard.routes[0].queue.len(), 1);
    }
    test_slice(&scheduler, 500_000_000, &cancelled, |_, _, _, _| Ok(()));
    assert_eq!(scheduler.lock().unwrap().routes[0].delivered_count, 1);
  }

  #[test]
  fn byte_limits_preserve_older_fifo_and_latest_on_pressure() {
    let mut fifo = Scheduler::new(vec![config(DataClass::State, 0, None, 0)]).unwrap();
    fifo.enqueue(0, 1, 700_000).unwrap();
    fifo.enqueue(0, 2, 400_000).unwrap();
    assert_eq!(fifo.routes[0].queue.len(), 1);
    assert_eq!(fifo.routes[0].queue.front().unwrap().value, 1);
    assert_eq!(fifo.routes[0].overflow_count, 1);
    let mut latest = Scheduler::new(vec![config(DataClass::Image, 0, None, 0)]).unwrap();
    latest.enqueue(0, 1, 10).unwrap();
    latest.enqueue(0, 2, GLOBAL_BYTES + 1).unwrap();
    assert_eq!(latest.routes[0].queue.front().unwrap().value, 1);
    assert_eq!(latest.routes[0].replaced_count, 0);
    assert_eq!(latest.routes[0].overflow_count, 1);
    let mut global = Scheduler::new(vec![
      config(DataClass::Image, 0, None, 0),
      config(DataClass::Image, 0, None, 1),
    ])
    .unwrap();
    global.enqueue(0, 1, GLOBAL_BYTES).unwrap();
    global.enqueue(1, 2, 1).unwrap();
    assert_eq!(global.routes[1].queue.len(), 0);
    assert_eq!(global.routes[1].overflow_count, 1);
    assert_eq!(global.routes[1].gap_count, 1);
  }

  #[test]
  fn priority_weight_and_large_payload_credit_are_bounded() {
    let scheduler = Arc::new(Mutex::new(
      Scheduler::new(vec![
        config(DataClass::Image, 19, None, 0),
        config(DataClass::Event, -20, None, 1),
      ])
      .unwrap(),
    ));
    scheduler.lock().unwrap().enqueue(1, 2, 1).unwrap();
    scheduler
      .lock()
      .unwrap()
      .enqueue(0, 1, GLOBAL_BYTES - 1)
      .unwrap();
    let cancelled = AtomicBool::new(false);
    for tick in 0..1023 {
      test_slice(&scheduler, tick, &cancelled, |_, _, _, _| Ok(()));
    }
    assert_eq!(scheduler.lock().unwrap().routes[0].delivered_count, 0);
    test_slice(&scheduler, 1024, &cancelled, |_, _, _, _| Ok(()));
    let guard = scheduler.lock().unwrap();
    assert_eq!(guard.routes[0].delivered_count, 1);
    assert_eq!(guard.routes[1].delivered_count, 1);
    assert_eq!(guard.routes[0].status()["weight"], 1);
    assert_eq!(guard.routes[1].status()["weight"], 40);
  }
}
