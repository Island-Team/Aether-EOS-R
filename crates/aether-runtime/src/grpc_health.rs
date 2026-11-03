//! Bounded EOS-owned health sampling for the gRPC projection.
//!
//! Adapter health and status can perform socket I/O. Never call them on a
//! tonic request worker: one background sampler owns all outstanding probes,
//! and readers consume only the last fresh result.

use std::collections::BTreeMap;
use std::sync::{
  Arc, Condvar, Mutex, Weak,
  atomic::{AtomicBool, Ordering},
};
use std::time::{Duration, Instant};

use aether_hal::{HealthState, NodeState};

use crate::Runtime;

struct NodeSpec {
  path: String,
  poll_interval: Duration,
  stale_after: Duration,
  next_poll: Instant,
}

#[derive(Clone, Copy)]
struct Sample {
  healthy: bool,
  ready: bool,
  measured_at: Instant,
}

/// One sampler thread per admitted projection, regardless of request volume.
pub(crate) struct HealthSnapshots {
  samples: Mutex<BTreeMap<String, Sample>>,
  changed: Condvar,
  stale_after: BTreeMap<String, Duration>,
  stopped: AtomicBool,
}

/// One immutable view of all samples and their age at a read boundary.
pub(crate) struct HealthView {
  samples: BTreeMap<String, Sample>,
  stale_after: BTreeMap<String, Duration>,
  stopped: bool,
  at: Instant,
}

impl HealthSnapshots {
  pub(crate) fn new(runtime: &Arc<Runtime>) -> Arc<Self> {
    let now = Instant::now();
    let specs: Vec<_> = runtime
      .discover()
      .into_iter()
      .filter_map(|descriptor| {
        let node = runtime.profile().node(&descriptor.path)?;
        Some(NodeSpec {
          path: descriptor.path,
          poll_interval: Duration::from_millis(node.health_timing.poll_interval_ms.max(1)),
          stale_after: Duration::from_millis(node.health_timing.stale_after_ms)
            .min(Duration::from_secs(1)),
          next_poll: now,
        })
      })
      .collect();
    let snapshots = Arc::new(Self {
      samples: Mutex::new(BTreeMap::new()),
      changed: Condvar::new(),
      stale_after: specs
        .iter()
        .map(|spec| (spec.path.clone(), spec.stale_after))
        .collect(),
      stopped: AtomicBool::new(false),
    });
    if specs.is_empty() {
      return snapshots;
    }
    let weak_snapshots = Arc::downgrade(&snapshots);
    let weak_runtime = Arc::downgrade(runtime);
    std::thread::Builder::new()
      .name("eos-grpc-health".into())
      .spawn(move || sample_loop(weak_snapshots, weak_runtime, specs))
      .expect("health sampler thread");
    // Fast local adapters have a complete initial snapshot before the first
    // listener read. A stalled socket cannot hold startup beyond this bound.
    snapshots.wait_for_seed(Duration::from_millis(200));
    snapshots
  }

  pub(crate) fn read(&self, path: &str) -> (bool, bool) {
    self.view().read(path)
  }

  pub(crate) fn view(&self) -> HealthView {
    let samples = self
      .samples
      .lock()
      .map(|guard| guard.clone())
      .unwrap_or_default();
    HealthView {
      samples,
      stale_after: self.stale_after.clone(),
      stopped: self.stopped.load(Ordering::Acquire),
      at: Instant::now(),
    }
  }

  pub(crate) fn stop(&self) {
    self.stopped.store(true, Ordering::Release);
    self.changed.notify_all();
  }

  fn wait_for_seed(&self, timeout: Duration) {
    let deadline = Instant::now() + timeout;
    let Ok(mut samples) = self.samples.lock() else {
      return;
    };
    while samples.len() < self.stale_after.len() {
      let remaining = deadline.saturating_duration_since(Instant::now());
      if remaining.is_zero() {
        break;
      }
      let Ok((next, _)) = self.changed.wait_timeout(samples, remaining) else {
        break;
      };
      samples = next;
    }
  }
}

impl HealthView {
  pub(crate) fn read(&self, path: &str) -> (bool, bool) {
    let Some(stale_after) = self.stale_after.get(path) else {
      return (false, false);
    };
    let Some(sample) = self.samples.get(path) else {
      return (false, false);
    };
    if self.stopped || self.at.duration_since(sample.measured_at) > *stale_after {
      return (false, false);
    }
    (sample.healthy, sample.ready)
  }

  /// The oldest complete health/status sample used by a projection read.
  /// Missing or stale data is represented by absent metadata, never age zero.
  pub(crate) fn max_age_ms(&self) -> Option<u64> {
    if self.stopped || self.stale_after.is_empty() {
      return None;
    }
    self
      .stale_after
      .iter()
      .try_fold(0, |oldest, (path, stale_after)| {
        let age = self.at.duration_since(self.samples.get(path)?.measured_at);
        (age <= *stale_after).then_some(oldest.max(age.as_millis().min(u64::MAX as u128) as u64))
      })
  }
}

fn sample_loop(snapshots: Weak<HealthSnapshots>, runtime: Weak<Runtime>, mut specs: Vec<NodeSpec>) {
  while let Some(store) = snapshots.upgrade() {
    if store.stopped.load(Ordering::Acquire) {
      break;
    }
    drop(store);
    let now = Instant::now();
    for spec in &mut specs {
      if now < spec.next_poll {
        continue;
      }
      let Some(owner) = runtime.upgrade() else {
        return;
      };
      // Freshness starts before either potentially blocking adapter call.
      // A late status response must not make an earlier health result new.
      let probe_started = Instant::now();
      let healthy = owner
        .dispatcher()
        .health(&spec.path)
        .is_ok_and(|health| health.state == HealthState::Healthy);
      let ready = owner
        .dispatcher()
        .status(&spec.path)
        .is_ok_and(|status| status.state == NodeState::Ready);
      drop(owner);
      let Some(store) = snapshots.upgrade() else {
        return;
      };
      if store.stopped.load(Ordering::Acquire) {
        return;
      }
      if let Ok(mut samples) = store.samples.lock() {
        samples.insert(
          spec.path.clone(),
          Sample {
            healthy,
            ready,
            measured_at: probe_started,
          },
        );
        store.changed.notify_all();
      }
      spec.next_poll = Instant::now() + spec.poll_interval;
    }
    std::thread::sleep(Duration::from_millis(5));
  }
}
