//! Fixed local baseline for command admission and RGB/depth sample transfer.
use std::hint::black_box;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::{Duration, Instant};

use aether_common::time::ManualClock;
use aether_dispatcher::{AdapterRegistration, CommandRequest, Dispatcher, Movement, OutcomeStatus};
use aether_hal::{
  BaseMovement, BaseNode, BaseObservation, CameraFrame, ClockKind, CommandMeta, CommandOutcome,
  DepthFrame, HalError, Health, Intrinsics, MockBase, MockCamera, NodeAdapter, NodeKind,
  NodeStatus, Profile, SampleMeta,
};
use serde::Serialize;

const COMMANDS: usize = 5_000;
const DURABLE_COMMANDS: usize = 100;
const SAMPLES: usize = 1_000;
const PROFILES: usize = 1_000;

// The generic HAL mock reports Accepted. This benchmark mock confirms its
// synchronous in-memory call so dispatcher repetitions remain independent.
struct CompletedMockBase(MockBase);

impl NodeAdapter for CompletedMockBase {
  fn kind(&self) -> NodeKind {
    self.0.kind()
  }
  fn adapter_type(&self) -> &str {
    self.0.adapter_type()
  }
  fn status(&self) -> NodeStatus {
    self.0.status()
  }
  fn health(&self) -> Health {
    self.0.health()
  }
  fn stop(&self) -> Result<(), HalError> {
    self.0.stop()
  }
}

impl BaseNode for CompletedMockBase {
  fn observe(&self) -> Result<BaseObservation, HalError> {
    self.0.observe()
  }
  fn move_base(
    &self,
    meta: CommandMeta,
    movement: BaseMovement,
  ) -> Result<CommandOutcome, HalError> {
    self.0.move_base(meta, movement)?;
    Ok(CommandOutcome::Completed)
  }
}

fn profile() -> Profile {
  Profile::from_toml(
    r#"
version = "1.0.0"
[robot]
id = "bench-robot"
name = "Benchmark robot"
[cores.eos.layers.hardware.groups.base.nodes.base0]
id = "base0"
node_type = "eos.hardware.base@v1.0"
label = "Base"
description = "Benchmark mock"
frame_id = "base_link"
capabilities = ["body_velocity"]
[cores.eos.layers.hardware.groups.base.nodes.base0.adapter]
type = "mock"
[cores.eos.layers.hardware.groups.base.nodes.base0.health_timing]
poll_interval_ms = 10
stale_after_ms = 100
[cores.eos.layers.hardware.groups.base.nodes.base0.observation]
max_rate_hz = 50.0
max_payload_bytes = 4096
[cores.eos.layers.hardware.groups.base.nodes.base0.movement]
modes = ["body_velocity"]
[cores.eos.layers.hardware.groups.base.nodes.base0.movement.limits]
max_linear_m_s = 1.0
max_angular_rad_s = 1.0
max_velocity_lease_ms = 500
[cores.eos.layers.hardware.groups.camera.nodes.camera0]
id = "camera0"
node_type = "eos.hardware.camera@v1.0"
label = "Camera"
description = "Benchmark mock"
frame_id = "camera_link"
capabilities = ["rgb8", "aligned_depth_u16"]
[cores.eos.layers.hardware.groups.camera.nodes.camera0.adapter]
type = "mock"
[cores.eos.layers.hardware.groups.camera.nodes.camera0.health_timing]
poll_interval_ms = 10
stale_after_ms = 100
[cores.eos.layers.hardware.groups.camera.nodes.camera0.observation]
max_rate_hz = 30.0
max_payload_bytes = 2000000
[cores.eos.layers.dispatcher.groups.control.nodes.command]
id = "command"
node_type = "eos.dispatcher.control@v1.0"
label = "Dispatcher"
description = "Benchmark command route"
[cores.eos.layers.hardware.groups.base.nodes.base0.inputs.command]
node = "cores/eos/layers/dispatcher/groups/control/nodes/command"
output = "base_command"
"#,
  )
  .unwrap()
}

fn sample(frame_id: &str) -> SampleMeta {
  SampleMeta {
    source_timestamp_ns: None,
    clock: ClockKind::Unknown,
    received_monotonic_ns: 0,
    sequence: None,
    frame_id: frame_id.into(),
  }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
enum CallOutcome {
  Success,
  Rejected,
  Error,
}

#[derive(Clone, Serialize)]
struct CallSample {
  duration_ns: u64,
  outcome: CallOutcome,
  timed_out: bool,
}

#[derive(Serialize)]
struct Workload {
  name: &'static str,
  calls: usize,
  wall_duration_ns: u64,
  success: usize,
  rejected: usize,
  error: usize,
  timeout: usize,
  throughput_ops_s: f64,
  median_us: f64,
  p99_us: f64,
  samples: Vec<CallSample>,
}

fn percentile_ns(sorted: &[u64], numerator: usize, denominator: usize) -> u64 {
  sorted[(sorted.len() * numerator).div_ceil(denominator) - 1]
}

fn median_ns(sorted: &[u64]) -> f64 {
  let count = sorted.len();
  if count.is_multiple_of(2) {
    (sorted[count / 2 - 1] as f64 + sorted[count / 2] as f64) / 2.0
  } else {
    sorted[count / 2] as f64
  }
}

fn measure(
  name: &'static str,
  mut op: impl FnMut(usize) -> CallOutcome,
  count: usize,
  timeout: Option<Duration>,
) -> Workload {
  assert!(count > 0);
  let mut samples = Vec::with_capacity(count);
  let overall = Instant::now();
  for i in 0..count {
    let start = Instant::now();
    let outcome = op(i);
    let duration = start.elapsed();
    samples.push(CallSample {
      duration_ns: duration.as_nanos().min(u64::MAX as u128) as u64,
      outcome,
      timed_out: timeout.is_some_and(|limit| duration > limit),
    });
  }
  let wall_duration = overall.elapsed();
  let mut durations: Vec<_> = samples.iter().map(|sample| sample.duration_ns).collect();
  durations.sort_unstable();
  Workload {
    name,
    calls: count,
    wall_duration_ns: wall_duration.as_nanos().min(u64::MAX as u128) as u64,
    success: samples
      .iter()
      .filter(|s| s.outcome == CallOutcome::Success)
      .count(),
    rejected: samples
      .iter()
      .filter(|s| s.outcome == CallOutcome::Rejected)
      .count(),
    error: samples
      .iter()
      .filter(|s| s.outcome == CallOutcome::Error)
      .count(),
    timeout: samples.iter().filter(|s| s.timed_out).count(),
    throughput_ops_s: count as f64 / wall_duration.as_secs_f64(),
    median_us: median_ns(&durations) / 1_000.0,
    p99_us: percentile_ns(&durations, 99, 100) as f64 / 1_000.0,
    samples,
  }
}

fn dispatch_outcome(status: OutcomeStatus) -> CallOutcome {
  match status {
    OutcomeStatus::Completed => CallOutcome::Success,
    OutcomeStatus::Rejected => CallOutcome::Rejected,
    OutcomeStatus::Failed
    | OutcomeStatus::Cancelled
    | OutcomeStatus::InProgress
    | OutcomeStatus::OutcomeUnknown => CallOutcome::Error,
  }
}

#[derive(Serialize)]
struct Repetition {
  index: usize,
  workloads: Vec<Workload>,
}

#[derive(Serialize)]
struct Report {
  schema_version: &'static str,
  measurement_clock: &'static str,
  median_method: &'static str,
  p99_method: &'static str,
  timeout_ms: Option<u64>,
  repetitions: Vec<Repetition>,
  aggregate: Vec<Workload>,
}

fn aggregate(runs: &[Repetition]) -> Vec<Workload> {
  (0..runs[0].workloads.len())
    .map(|position| {
      let name = runs[0].workloads[position].name;
      let samples: Vec<_> = runs
        .iter()
        .flat_map(|run| run.workloads[position].samples.iter().cloned())
        .collect();
      let mut durations: Vec<_> = samples.iter().map(|sample| sample.duration_ns).collect();
      durations.sort_unstable();
      let calls = samples.len();
      let wall_duration_ns: u64 = runs
        .iter()
        .map(|run| run.workloads[position].wall_duration_ns)
        .sum();
      Workload {
        name,
        calls,
        wall_duration_ns,
        success: samples
          .iter()
          .filter(|s| s.outcome == CallOutcome::Success)
          .count(),
        rejected: samples
          .iter()
          .filter(|s| s.outcome == CallOutcome::Rejected)
          .count(),
        error: samples
          .iter()
          .filter(|s| s.outcome == CallOutcome::Error)
          .count(),
        timeout: samples.iter().filter(|s| s.timed_out).count(),
        throughput_ops_s: calls as f64 / (wall_duration_ns as f64 / 1e9),
        median_us: median_ns(&durations) / 1_000.0,
        p99_us: percentile_ns(&durations, 99, 100) as f64 / 1_000.0,
        samples,
      }
    })
    .collect()
}

fn run(index: usize, timeout: Option<Duration>) -> Repetition {
  let mut workloads = Vec::new();
  workloads.push(measure(
    "legacy_profile_safe_read",
    |_| {
      black_box(profile());
      CallOutcome::Success
    },
    PROFILES,
    timeout,
  ));
  let base = Arc::new(CompletedMockBase(MockBase::new(BaseObservation {
    meta: sample("base_link"),
    valid: true,
    x_m: 0.0,
    y_m: 0.0,
    yaw_rad: 0.0,
    x_m_s: 0.0,
    y_m_s: 0.0,
    yaw_rad_s: 0.0,
  })));
  let width = 640_u32;
  let height = 480_u32;
  let camera = Arc::new(MockCamera::new(CameraFrame {
    meta: sample("camera_link"),
    width_px: width,
    height_px: height,
    stride_bytes: width * 3,
    rgb8: vec![127; (width * height * 3) as usize],
    depth: Some(DepthFrame {
      depth_u16: vec![1000; (width * height) as usize],
      depth_scale_m: 0.001,
      intrinsics: Intrinsics {
        fx: 500.0,
        fy: 500.0,
        cx: 320.0,
        cy: 240.0,
        distortion_model: "none".into(),
        distortion_coefficients: vec![],
      },
    }),
    preview_jpeg: None,
  }));
  let dispatcher = Dispatcher::new(
    &profile(),
    vec![
      AdapterRegistration::Base {
        node_id: "base0".into(),
        adapter: base.clone(),
      },
      AdapterRegistration::Camera {
        node_id: "camera0".into(),
        adapter: camera.clone(),
      },
    ],
    Arc::new(ManualClock::new(0, None)),
  )
  .unwrap();
  workloads.push(measure(
    "memory_dispatch",
    |i| {
      let request = CommandRequest {
        node_path: "cores/eos/layers/hardware/groups/base/nodes/base0".into(),
        meta: CommandMeta {
          command_id: format!("bench-{i}"),
          deadline_monotonic_ns: 1_000_000_000,
        },
        movement: Movement::Base(BaseMovement::BodyVelocity {
          x_m_s: 0.25,
          y_m_s: 0.0,
          yaw_rad_s: 0.0,
          valid_for_ms: 100,
        }),
      };
      let result = black_box(dispatcher.dispatch(request));
      dispatch_outcome(result.status)
    },
    COMMANDS,
    timeout,
  ));
  let journal_dir = std::env::temp_dir().join(format!(
    "eos-bench-journal-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .unwrap()
      .as_nanos()
  ));
  let durable = Dispatcher::new_durable(
    &profile(),
    vec![
      AdapterRegistration::Base {
        node_id: "base0".into(),
        adapter: base,
      },
      AdapterRegistration::Camera {
        node_id: "camera0".into(),
        adapter: camera.clone(),
      },
    ],
    Arc::new(ManualClock::new(0, None)),
    &journal_dir,
  )
  .unwrap();
  workloads.push(measure(
    "durable_dispatch",
    |i| {
      let result = black_box(durable.dispatch(CommandRequest {
        node_path: "cores/eos/layers/hardware/groups/base/nodes/base0".into(),
        meta: CommandMeta {
          command_id: format!("durable-{i}"),
          deadline_monotonic_ns: 1_000_000_000,
        },
        movement: Movement::Base(BaseMovement::BodyVelocity {
          x_m_s: 0.25,
          y_m_s: 0.0,
          yaw_rad_s: 0.0,
          valid_for_ms: 100,
        }),
      }));
      dispatch_outcome(result.status)
    },
    DURABLE_COMMANDS,
    timeout,
  ));
  drop(durable);
  std::fs::remove_dir_all(journal_dir).unwrap();
  workloads.push(measure(
    "mock_camera_rgb8_depth16_640x480",
    |_| match dispatcher.capture_camera("cores/eos/layers/hardware/groups/camera/nodes/camera0") {
      Ok(frame) => match frame.depth {
        Some(depth) => {
          black_box(frame.rgb8.len() + depth.depth_u16.len());
          CallOutcome::Success
        }
        None => CallOutcome::Error,
      },
      Err(_) => CallOutcome::Error,
    },
    SAMPLES,
    timeout,
  ));
  Repetition { index, workloads }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
  let mut args = std::env::args().skip(1);
  let mut repetitions = 1;
  let mut output: Option<PathBuf> = None;
  let mut timeout = None;
  while let Some(arg) = args.next() {
    match arg.as_str() {
      "--repetitions" => repetitions = args.next().ok_or("missing repetitions")?.parse()?,
      "--output" => output = Some(args.next().ok_or("missing output path")?.into()),
      "--timeout-ms" => {
        timeout = Some(Duration::from_millis(
          args.next().ok_or("missing timeout")?.parse()?,
        ))
      }
      _ => return Err(format!("unknown argument: {arg}").into()),
    }
  }
  if repetitions == 0 {
    return Err("repetitions must be positive".into());
  }
  let runs: Vec<_> = (1..=repetitions).map(|index| run(index, timeout)).collect();
  let report = Report {
    schema_version: "1.1.0",
    measurement_clock: "std::time::Instant (monotonic)",
    median_method: "arithmetic midpoint for even sample count",
    p99_method: "nearest rank",
    timeout_ms: timeout.map(|value| value.as_millis().min(u64::MAX as u128) as u64),
    aggregate: aggregate(&runs),
    repetitions: runs,
  };
  for run in &report.repetitions {
    for workload in &run.workloads {
      println!("repeat {} {}: {}/{} success, {} rejected, {} error, {} timeout; {:.0} ops/s, median {:.2} us, nearest-rank p99 {:.2} us", run.index, workload.name, workload.success, workload.calls, workload.rejected, workload.error, workload.timeout, workload.throughput_ops_s, workload.median_us, workload.p99_us);
    }
  }
  for workload in &report.aggregate {
    println!("aggregate {}: {}/{} success, {} rejected, {} error, {} timeout; {:.0} ops/s, median {:.2} us, nearest-rank p99 {:.2} us", workload.name, workload.success, workload.calls, workload.rejected, workload.error, workload.timeout, workload.throughput_ops_s, workload.median_us, workload.p99_us);
  }
  if let Some(path) = output {
    std::fs::write(path, serde_json::to_vec_pretty(&report)?)?;
  }
  if report
    .aggregate
    .iter()
    .any(|workload| workload.success != workload.calls || workload.timeout != 0)
  {
    return Err("benchmark contained non-successful calls".into());
  }
  Ok(())
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn test_percentiles_even_count_use_midpoint_and_nearest_rank() {
    assert_eq!(median_ns(&[10, 20, 30, 40]), 25.0);
    assert_eq!(median_ns(&[10, 20, 30]), 20.0);
    assert_eq!(percentile_ns(&[10, 20, 30, 40], 99, 100), 40);
    assert_eq!(percentile_ns(&[10, 20, 30, 40], 50, 100), 20);
  }

  #[test]
  fn test_failure_and_rejection_are_counted() {
    let workload = measure(
      "test",
      |i| match i {
        0 => CallOutcome::Success,
        1 => dispatch_outcome(OutcomeStatus::Rejected),
        _ => dispatch_outcome(OutcomeStatus::OutcomeUnknown),
      },
      3,
      None,
    );
    assert_eq!(
      (workload.success, workload.rejected, workload.error),
      (1, 1, 1)
    );
    assert_eq!(workload.samples.len(), workload.calls);
  }

  #[test]
  fn test_slow_success_rejection_and_error_keep_outcomes_and_timeouts() {
    let workload = measure(
      "test",
      |i| {
        std::thread::sleep(Duration::from_millis(2));
        match i {
          0 => CallOutcome::Success,
          1 => CallOutcome::Rejected,
          _ => CallOutcome::Error,
        }
      },
      3,
      Some(Duration::from_millis(1)),
    );
    assert_eq!(
      (
        workload.success,
        workload.rejected,
        workload.error,
        workload.timeout
      ),
      (1, 1, 1, 3)
    );
    assert!(workload.samples.iter().all(|sample| sample.timed_out));
  }
}
