//! Process-local monotonic time and optional wall-clock provenance.

use std::sync::{Mutex, OnceLock};
use std::time::{Instant, SystemTime, UNIX_EPOCH};

/// A clock that can be replaced in tests without sleeping.
pub trait Clock: Send + Sync {
  /// Nanoseconds since one process-wide origin; never compare across processes.
  fn monotonic_ns(&self) -> u64;

  /// Unix nanoseconds when a wall clock is available.
  fn unix_timestamp_ns(&self) -> Option<i64>;
}

#[derive(Debug, Default, Clone, Copy)]
pub struct SystemClock;

impl Clock for SystemClock {
  fn monotonic_ns(&self) -> u64 {
    static ORIGIN: OnceLock<Instant> = OnceLock::new();
    let origin = ORIGIN.get_or_init(Instant::now);
    u64::try_from(origin.elapsed().as_nanos()).unwrap_or(u64::MAX)
  }

  fn unix_timestamp_ns(&self) -> Option<i64> {
    let elapsed = SystemTime::now().duration_since(UNIX_EPOCH).ok()?;
    i64::try_from(elapsed.as_nanos()).ok()
  }
}

#[derive(Debug)]
pub struct ManualClock {
  state: Mutex<ManualTime>,
}

#[derive(Debug, Clone, Copy)]
struct ManualTime {
  monotonic_ns: u64,
  unix_timestamp_ns: Option<i64>,
}

impl ManualClock {
  pub fn new(monotonic_ns: u64, unix_timestamp_ns: Option<i64>) -> Self {
    Self {
      state: Mutex::new(ManualTime {
        monotonic_ns,
        unix_timestamp_ns,
      }),
    }
  }

  pub fn set_monotonic_ns(&self, value: u64) {
    self
      .state
      .lock()
      .expect("manual clock poisoned")
      .monotonic_ns = value;
  }

  pub fn advance_ns(&self, delta: u64) {
    let mut state = self.state.lock().expect("manual clock poisoned");
    state.monotonic_ns = state.monotonic_ns.saturating_add(delta);
    if let Some(unix) = state.unix_timestamp_ns {
      state.unix_timestamp_ns = Some(unix.saturating_add(i64::try_from(delta).unwrap_or(i64::MAX)));
    }
  }

  pub fn set_unix_timestamp_ns(&self, value: Option<i64>) {
    self
      .state
      .lock()
      .expect("manual clock poisoned")
      .unix_timestamp_ns = value;
  }
}

impl Clock for ManualClock {
  fn monotonic_ns(&self) -> u64 {
    self
      .state
      .lock()
      .expect("manual clock poisoned")
      .monotonic_ns
  }

  fn unix_timestamp_ns(&self) -> Option<i64> {
    self
      .state
      .lock()
      .expect("manual clock poisoned")
      .unix_timestamp_ns
  }
}

/// True only while `now` is strictly before a process-local deadline.
pub fn before_deadline(now_ns: u64, deadline_ns: u64) -> bool {
  now_ns < deadline_ns
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn manual_clock_advances_both_domains_without_sleep() {
    let clock = ManualClock::new(5, Some(100));
    clock.advance_ns(7);
    assert_eq!(clock.monotonic_ns(), 12);
    assert_eq!(clock.unix_timestamp_ns(), Some(107));
    clock.set_unix_timestamp_ns(None);
    assert_eq!(clock.unix_timestamp_ns(), None);
  }

  #[test]
  fn deadline_is_exclusive() {
    assert!(before_deadline(9, 10));
    assert!(!before_deadline(10, 10));
    assert!(!before_deadline(11, 10));
  }
}
