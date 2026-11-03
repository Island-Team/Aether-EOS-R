use std::collections::BTreeMap;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use aether_hal::{
  CameraFrame, CameraJpegOptions, CameraJpegTransformer, DataTransformer, HealthState, NodeState,
  DataClass, EosEdgeClass, NodeTypeRegistry, TypedDataHub, VisionFrame,
};
use aether_runtime::Runtime;
use aether_runtime::grpc::CameraCache;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use uuid::Uuid;
use super::routes::{self, RouteConfig, Scheduler};

#[derive(Default)]
struct Delivered {
  generation: u64,
  frame: Option<Arc<SourceFrame>>,
  canonical: Option<VisionFrame>,
  canonical_sha256: Option<String>,
}

struct SourceFrame {
  frame: Arc<CameraFrame>,
  source_frame_id: String,
  health_epoch: u64,
  health_fence: Arc<AtomicU64>,
}

fn commit_delivery(
  slot: &Arc<Mutex<Delivered>>,
  frame: Arc<SourceFrame>,
  canonical: VisionFrame,
  digest: String,
) -> Result<(), ()> {
  let mut delivered = slot.try_lock().map_err(|_| ())?;
  if delivered.generation == u64::MAX
    || frame.health_fence.load(Ordering::Acquire) != frame.health_epoch
  {
    return Err(());
  }
  delivered.frame = Some(frame);
  delivered.canonical = Some(canonical);
  delivered.canonical_sha256 = Some(digest);
  delivered.generation += 1;
  Ok(())
}

struct Gate {
  label: String,
  source_path: String,
  source_rate_hz: f64,
  generation: Arc<AtomicU64>,
  health_epoch: Arc<AtomicU64>,
  delivered: Option<Arc<Mutex<Delivered>>>,
}

fn capture_interval(rate_hz: f64) -> Duration {
  let seconds = 1.0 / rate_hz.min(60.0);
  if seconds.is_finite() && seconds < u64::MAX as f64 {
    Duration::from_secs_f64(seconds)
  } else {
    Duration::MAX
  }
}

pub struct VisionService {
  revision: String,
  graph_revision: Option<String>,
  run_id: Option<String>,
  runtime_stream_id: String,
  gates: BTreeMap<String, Gate>,
  hub: Arc<TypedDataHub>,
  scheduler: Arc<Mutex<Scheduler<Arc<SourceFrame>>>>,
  rows: Vec<Value>,
  stopped: Arc<AtomicBool>,
  workers: Mutex<Vec<JoinHandle<()>>>,
}

pub struct CapturedVision {
  pub frame: VisionFrame,
  pub generation: u64,
  pub metadata: Value,
}

impl VisionService {
  pub fn start(
    runtime: Arc<Runtime>,
    registry: Arc<NodeTypeRegistry>,
    revision: String,
    managed: Option<(String, String)>,
    boot: Instant,
    camera_cache: Arc<CameraCache>,
  ) -> Result<Self, String> {
    let hub = Arc::new(TypedDataHub::default());
    let stopped = Arc::new(AtomicBool::new(false));
    let mut configs = Vec::new();
    let mut rows = Vec::new();
    for edge in runtime
      .profile()
      .eos_edge_contract(&registry)
      .map_err(|e| e.to_string())?
      .into_iter()
      .filter(|e| e.source_node.starts_with("eos/") && e.target_node.starts_with("eos/"))
    {
      if edge.class == EosEdgeClass::EosLocal {
        let source = runtime
          .profile()
          .node(&edge.source_node)
          .ok_or("missing route source")?;
        let target = runtime
          .profile()
          .node(&edge.target_node)
          .ok_or("missing route target")?;
        if source.node_type != "eos.hardware.camera@v0.1"
          || target.node_type != "eos.interface.camera_gate@v0.1"
          || edge.source_output != "frame"
          || edge.target_input != "frame"
          || edge.data_type != "camera_frame"
        {
          return Err("missing compiled EOS-local route endpoint".into());
        }
        configs.push(RouteConfig {
          source_node: edge.source_node,
          source_output: edge.source_output,
          target_node: edge.target_node,
          target_input: edge.target_input,
          data_type: edge.data_type,
          class: DataClass::Image,
          priority: edge.priority,
          frequency_hz: edge.frequency_hz,
        });
      } else {
        rows.push(json!({"source_node":edge.source_node,"source_output":edge.source_output,
          "target_node":edge.target_node,"target_input":edge.target_input,"data_type":edge.data_type,
          "delivery_state":if edge.class==EosEdgeClass::FixedSafety {"fixed_not_scheduled"} else {"not_routed"},
          "routed":false,"priority":null,"frequency_hz":null,"weight":null,"queue_class":null,
          "queue_depth":null,"delivered_count":null,"cap_wait_count":null,"replaced_count":null,
          "overflow_count":null,"consumer_error_count":null,"cancelled_count":null,"gap_count":null,
          "last_delivery_monotonic_ns":null}));
      }
    }
    let scheduler = Arc::new(Mutex::new(Scheduler::new(configs)?));
    let active = scheduler
      .lock()
      .map_err(|_| "route lock poisoned")?
      .configs();
    let mut routes_by_source: BTreeMap<String, Vec<usize>> = BTreeMap::new();
    let mut delivered_by_gate = BTreeMap::new();
    let mut delivered_by_index = Vec::new();
    for (index, config) in active.iter().enumerate() {
      routes_by_source
        .entry(config.source_node.clone())
        .or_default()
        .push(index);
      let slot = Arc::new(Mutex::new(Delivered::default()));
      delivered_by_gate.insert(config.target_node.clone(), slot.clone());
      delivered_by_index.push(slot);
    }
    let mut gates = BTreeMap::new();
    let mut sources = BTreeMap::new();
    let mut health_epochs = BTreeMap::new();
    for node in runtime.profile().all_nodes().filter(|n| {
      matches!(
        n.node_type.as_str(),
        "eos.interface.camera_gate@v1.0" | "eos.interface.camera_gate@v0.1"
      )
    }) {
      let source_path = &node.inputs["frame"]
        .one()
        .expect("validated frame input")
        .node;
      let source = runtime
        .profile()
        .node(source_path)
        .expect("validated camera source");
      let source_rate_hz = source
        .observation
        .as_ref()
        .expect("camera observation")
        .max_rate_hz;
      let generation = sources
        .entry(source_path.clone())
        .or_insert_with(|| Arc::new(AtomicU64::new(0)))
        .clone();
      let health_epoch = health_epochs
        .entry(source_path.clone())
        .or_insert_with(|| Arc::new(AtomicU64::new(0)))
        .clone();
      let delivered = if node.node_type == "eos.interface.camera_gate@v0.1" {
        Some(
          delivered_by_gate
            .get(node.path())
            .ok_or("camera gate route absent")?
            .clone(),
        )
      } else {
        None
      };
      gates.insert(
        node.path().into(),
        Gate {
          label: node.label.clone(),
          source_path: source_path.clone(),
          source_rate_hz,
          generation,
          health_epoch,
          delivered,
        },
      );
    }
    // gRPC reads the same validated source publications as the owner vision
    // path. Cameras without a declared gate still use one bounded worker.
    for node in runtime.profile().all_nodes() {
      if matches!(
        node.node_type.as_str(),
        "eos.hardware.camera@v1.0" | "eos.hardware.camera@v0.1"
      ) {
        sources
          .entry(node.path().into())
          .or_insert_with(|| Arc::new(AtomicU64::new(0)));
      }
    }
    let runtime_stream_id = Uuid::new_v4().to_string();
    let mut workers: Vec<JoinHandle<()>> = sources
      .into_iter()
      .map(|(source_path, generation)| {
        let health_epoch = health_epochs
          .entry(source_path.clone())
          .or_insert_with(|| Arc::new(AtomicU64::new(0)))
          .clone();
        let runtime = runtime.clone();
        let registry = registry.clone();
        let hub = hub.clone();
        let stopped = stopped.clone();
        let scheduler = scheduler.clone();
        let camera_cache = camera_cache.clone();
        let indices = routes_by_source.remove(&source_path).unwrap_or_default();
        let slots = indices
          .iter()
          .map(|index| delivered_by_index[*index].clone())
          .collect::<Vec<_>>();
        let source = runtime
          .profile()
          .node(&source_path)
          .expect("validated camera")
          .clone();
        let rate = source
          .observation
          .as_ref()
          .expect("camera observation")
          .max_rate_hz
          .min(60.0);
        thread::spawn(move || {
          let interval = capture_interval(rate);
          let invalidate = || {
            health_epoch.fetch_add(1, Ordering::AcqRel);
            if let Ok(mut routes) = scheduler.lock() {
              for index in &indices {
                routes.clear_route(*index);
              }
            }
            for slot in &slots {
              if let Ok(mut delivered) = slot.lock() {
                delivered.frame = None;
                delivered.canonical = None;
                delivered.canonical_sha256 = None;
              }
            }
          };
          while !stopped.load(Ordering::Relaxed) {
            let capture_epoch = health_epoch.load(Ordering::Acquire);
            let captured = runtime.dispatcher().capture_camera(&source_path);
            let healthy = runtime
              .dispatcher()
              .status(&source_path)
              .is_ok_and(|status| status.state == NodeState::Ready)
              && runtime
                .dispatcher()
                .health(&source_path)
                .is_ok_and(|health| health.state == HealthState::Healthy);
            if let (Ok(frame), true) = (captured, healthy) {
              if !stopped.load(Ordering::Acquire)
                && health_epoch.load(Ordering::Acquire) == capture_epoch
                && hub
                  .publish(&source, &registry, "frame", frame.clone())
                  .is_ok()
              {
                let bytes = serde_json::to_vec(&frame).map_or(usize::MAX, |v| v.len());
                let Ok(source_sequence) =
                  generation.fetch_update(Ordering::AcqRel, Ordering::Acquire, |value| {
                    value.checked_add(1)
                  })
                else {
                  break;
                };
                let source_frame_id = format!("src-{}", source_sequence + 1);
                let shared = Arc::new(SourceFrame {
                  frame: Arc::new(frame),
                  source_frame_id,
                  health_epoch: capture_epoch,
                  health_fence: health_epoch.clone(),
                });
                camera_cache.push(&source_path, shared.frame.clone());
                if let Ok(mut routes) = scheduler.lock() {
                  routes::fanout_shared_locked(&mut routes, &indices, &shared, bytes);
                }
              } else {
                invalidate();
              }
            } else {
              invalidate();
            }
            let mut remaining = interval;
            while !stopped.load(Ordering::Relaxed) && !remaining.is_zero() {
              let pause = remaining.min(Duration::from_millis(100));
              thread::sleep(pause);
              remaining = remaining.saturating_sub(pause);
            }
          }
        })
      })
      .collect();
    if !delivered_by_index.is_empty() {
      let scheduler = scheduler.clone();
      let stopped = stopped.clone();
      let active = active.clone();
      workers.push(thread::spawn(move || {
        let staged = std::cell::RefCell::new(vec![None; active.len()]);
        while !stopped.load(Ordering::Acquire) {
          let now = boot.elapsed().as_nanos().min(u64::MAX as u128) as u64;
          routes::service_slice(
            &scheduler,
            now,
            &stopped,
            |index, frame, cancelled, deadline| {
              let slot = &delivered_by_index[index];
              loop {
                if cancelled.load(Ordering::Acquire) || Instant::now() >= deadline {
                  return Err(());
                }
                if slot.try_lock().is_ok() {
                  let config = &active[index];
                  let source_rate_hz = runtime
                    .profile()
                    .node(&config.source_node)
                    .and_then(|source| source.observation.as_ref())
                    .map(|observation| observation.max_rate_hz)
                    .ok_or(())?;
                  let canonical = CameraJpegTransformer
                    .transform(
                      &frame.frame,
                      &CameraJpegOptions {
                        gate_path: config.target_node.clone(),
                        source_rate_hz,
                        max_width_px: 1280,
                        quality: 80,
                      },
                    )
                    .map_err(|_| ())?;
                  let digest = format!("{:x}", Sha256::digest(&canonical.jpeg));
                  if cancelled.load(Ordering::Acquire) || Instant::now() >= deadline {
                    return Err(());
                  }
                  if !runtime
                    .dispatcher()
                    .status(&config.source_node)
                    .is_ok_and(|status| status.state == NodeState::Ready)
                    || !runtime
                      .dispatcher()
                      .health(&config.source_node)
                      .is_ok_and(|health| health.state == HealthState::Healthy)
                  {
                    return Err(());
                  }
                  staged.borrow_mut()[index] = Some((canonical, digest));
                  return Ok(frame);
                }
                thread::sleep(Duration::from_millis(1));
              }
            },
            |index, frame| {
              let (canonical, digest) = staged.borrow_mut()[index].take().ok_or(())?;
              let committed_ns = boot.elapsed().as_nanos().min(u64::MAX as u128) as u64;
              commit_delivery(&delivered_by_index[index], frame, canonical, digest)?;
              Ok(committed_ns)
            },
          );
          thread::sleep(Duration::from_millis(1));
        }
      }));
    }
    Ok(Self {
      revision,
      graph_revision: managed.as_ref().map(|(_, revision)| revision.clone()),
      run_id: managed.map(|(run_id, _)| run_id),
      runtime_stream_id,
      gates,
      hub,
      scheduler,
      rows,
      stopped,
      workers: Mutex::new(workers),
    })
  }

  pub fn route_status(&self) -> Value {
    if self.stopped.load(Ordering::Acquire) {
      return json!({"ok":false,"error":"runtime is stopping"});
    }
    let mut rows = self.rows.clone();
    match self.scheduler.lock() {
      Ok(routes) => rows.extend(routes.status()),
      Err(_) => return json!({"ok":false,"error":"route status unavailable"}),
    }
    rows.sort_by(|a, b| {
      let key = |row: &Value| {
        [
          "source_node",
          "source_output",
          "target_node",
          "target_input",
        ]
        .map(|field| row[field].as_str().unwrap_or("").to_owned())
      };
      key(a).cmp(&key(b))
    });
    if self.stopped.load(Ordering::Acquire) {
      return json!({"ok":false,"error":"runtime is stopping"});
    }
    json!({"ok":true,"projection_sha256":self.revision,"routes":rows})
  }

  pub fn active_gates(&self, runtime: &Runtime) -> Value {
    let gates = self
      .gates
      .iter()
      .filter(|(_, gate)| self.is_active(runtime, gate))
      .map(|(path, gate)| {
        json!({"path":path,"label":gate.label,"output_port":"preview",
                "source_path":gate.source_path,"source_rate_hz":gate.source_rate_hz})
      })
      .collect::<Vec<_>>();
    json!({"revision":self.revision,"gates":gates})
  }

  fn is_active(&self, runtime: &Runtime, gate: &Gate) -> bool {
    let active = !self.stopped.load(Ordering::Acquire)
      && runtime
        .dispatcher()
        .status(&gate.source_path)
        .is_ok_and(|status| status.state == NodeState::Ready)
      && runtime
        .dispatcher()
        .health(&gate.source_path)
        .is_ok_and(|health| health.state == HealthState::Healthy);
    if !active {
      gate.health_epoch.fetch_add(1, Ordering::AcqRel);
      if let Some(slot) = &gate.delivered {
        if let Ok(mut delivered) = slot.lock() {
          delivered.frame = None;
          delivered.canonical = None;
          delivered.canonical_sha256 = None;
        }
      }
    }
    active
  }

  pub fn attest(&self, runtime: &Runtime, gate_path: &str) -> Result<Value, String> {
    let run_id = self.run_id.as_deref().ok_or("vision_unavailable")?;
    let graph_revision = self.graph_revision.as_deref().ok_or("vision_unavailable")?;
    let gate = self.gates.get(gate_path).ok_or("gate_inactive")?;
    let slot = gate.delivered.as_ref().ok_or("gate_inactive")?;
    if !self.is_active(runtime, gate) {
      return Err("gate_inactive".into());
    }
    let generation = slot.lock().map_err(|_| "vision_unavailable")?.generation;
    if generation == u64::MAX {
      return Err("generation_exhausted".into());
    }
    Ok(
      json!({"ok":true,"ready":true,"run_id":run_id,"active_revision":graph_revision,
      "projection_revision":self.revision,"runtime_stream_id":self.runtime_stream_id,
      "source_path":gate.source_path,"gate_path":gate_path,"gate_generation":generation}),
    )
  }

  pub fn capture(
    &self,
    runtime: &Runtime,
    gate_path: &str,
    revision: &str,
    after_generation: u64,
    max_width_px: u32,
    jpeg_quality: u8,
  ) -> Result<Option<CapturedVision>, String> {
    if revision != self.revision {
      return Err("graph revision changed".into());
    }
    let gate = self.gates.get(gate_path).ok_or("unknown camera gate")?;
    if !self.is_active(runtime, gate) {
      return Err("camera gate is inactive".into());
    }
    let (generation, frame, canonical, canonical_digest, source_frame_id) =
      if let Some(delivered) = &gate.delivered {
        let slot = delivered.lock().map_err(|_| "camera gate slot poisoned")?;
        (
          slot.generation,
          slot.frame.as_ref().map(|item| item.frame.clone()),
          slot.canonical.clone(),
          slot.canonical_sha256.clone(),
          slot.frame.as_ref().map(|item| item.source_frame_id.clone()),
        )
      } else {
        let generation = gate.generation.load(Ordering::Acquire);
        let frame = self
          .hub
          .latest::<CameraFrame>(&gate.source_path, "frame")
          .map_err(|e| e.to_string())?
          .map(|envelope| Arc::new(envelope.payload.clone()));
        (generation, frame, None, None, None)
      };
    if generation == u64::MAX && after_generation >= generation {
      return Err("generation_exhausted".into());
    }
    if generation == 0 || generation <= after_generation {
      return Ok(None);
    }
    let Some(frame) = frame else {
      return Ok(None);
    };
    let frame = if max_width_px == 1280 && jpeg_quality == 80 {
      match canonical {
        Some(canonical) => canonical,
        None => CameraJpegTransformer
          .transform(
            &frame,
            &CameraJpegOptions {
              gate_path: gate_path.into(),
              source_rate_hz: gate.source_rate_hz,
              max_width_px,
              quality: jpeg_quality,
            },
          )
          .map_err(|e| e.to_string())?,
      }
    } else {
      CameraJpegTransformer
        .transform(
          &frame,
          &CameraJpegOptions {
            gate_path: gate_path.into(),
            source_rate_hz: gate.source_rate_hz,
            max_width_px,
            quality: jpeg_quality,
          },
        )
        .map_err(|e| e.to_string())?
    };
    if self.stopped.load(Ordering::Acquire) {
      return Err("stopped".into());
    }
    if !self.is_active(runtime, gate) {
      return Err("gate_inactive".into());
    }
    let mut metadata = json!({"gate_path":frame.gate_path,"width_px":frame.width_px,
      "height_px":frame.height_px,"source_rate_hz":frame.source_rate_hz});
    if let Some(sequence) = frame.sequence {
      metadata["sequence"] = sequence.into();
    }
    if let Some(timestamp) = frame.timestamp_unix_ms {
      metadata["timestamp_unix_ms"] = timestamp.into();
    }
    if let (Some(graph_revision), Some(source_frame_id)) = (&self.graph_revision, source_frame_id) {
      metadata["graph_revision"] = json!(graph_revision);
      metadata["projection_revision"] = json!(self.revision);
      metadata["runtime_stream_id"] = json!(self.runtime_stream_id);
      metadata["source_path"] = json!(gate.source_path);
      metadata["source_frame_id"] = json!(source_frame_id);
      metadata["gate_generation"] = json!(generation);
      metadata["render"] = json!({"max_width_px":max_width_px,"jpeg_quality":jpeg_quality});
      let digest = if max_width_px == 1280 && jpeg_quality == 80 {
        canonical_digest.unwrap_or_else(|| format!("{:x}", Sha256::digest(&frame.jpeg)))
      } else {
        format!("{:x}", Sha256::digest(&frame.jpeg))
      };
      metadata["jpeg_sha256"] = json!(digest);
    }
    if serde_json::to_vec(&metadata)
      .map_err(|e| e.to_string())?
      .len()
      > 4096
    {
      return Err("invalid_metadata".into());
    }
    Ok(Some(CapturedVision {
      frame,
      generation,
      metadata,
    }))
  }

  pub fn shutdown(&self) -> Result<(), String> {
    self.begin_shutdown()?;
    self
      .scheduler
      .lock()
      .map_err(|_| "route lock poisoned")?
      .close();
    let deadline = Instant::now() + Duration::from_secs(1);
    let mut workers = self.workers.lock().map_err(|_| "worker lock poisoned")?;
    while workers.iter().any(|worker| !worker.is_finished()) && Instant::now() < deadline {
      thread::sleep(Duration::from_millis(5));
    }
    let timed_out = workers.iter().any(|worker| !worker.is_finished());
    for worker in workers.drain(..) {
      if worker.is_finished() {
        let _ = worker.join();
      }
      // A blocked adapter call cannot be killed; dropping its handle detaches it.
    }
    if timed_out {
      Err("evidence worker join deadline exceeded".into())
    } else {
      Ok(())
    }
  }

  pub fn begin_shutdown(&self) -> Result<(), String> {
    let mut routes = self.scheduler.lock().map_err(|_| {
      self.stopped.store(true, Ordering::Release);
      "route lock poisoned"
    })?;
    routes.seal();
    self.stopped.store(true, Ordering::Release);
    for gate in self.gates.values() {
      if let Some(slot) = &gate.delivered {
        if let Ok(mut delivered) = slot.lock() {
          delivered.frame = None;
          delivered.canonical = None;
          delivered.canonical_sha256 = None;
        }
      }
    }
    Ok(())
  }
}

impl Drop for VisionService {
  fn drop(&mut self) {
    let _ = self.shutdown();
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use aether_common::time::SystemClock;
  use aether_hal::Profile;

  #[test]
  fn managed_attestation_before_first_frame_has_generation_zero() {
    let bytes = include_bytes!(concat!(
      env!("CARGO_MANIFEST_DIR"),
      "/../../tests/fixtures/edge_contract/valid_evidence.json"
    ));
    let mut profile: Profile = serde_json::from_slice(bytes).unwrap();
    profile.validate().unwrap();
    let runtime = Runtime::from_cxx_mock_profile(profile, Arc::new(SystemClock)).unwrap();
    let gate_path = "eos/interface/gate/front";
    let mut gates = BTreeMap::new();
    gates.insert(
      gate_path.into(),
      Gate {
        label: "Front".into(),
        source_path: "eos/hardware/camera/front".into(),
        source_rate_hz: 30.0,
        generation: Arc::new(AtomicU64::new(0)),
        health_epoch: Arc::new(AtomicU64::new(0)),
        delivered: Some(Arc::new(Mutex::new(Delivered::default()))),
      },
    );
    let vision = VisionService {
      revision: "b".repeat(64),
      graph_revision: Some("a".repeat(64)),
      run_id: Some("run-one".into()),
      runtime_stream_id: Uuid::new_v4().to_string(),
      gates,
      hub: Arc::new(TypedDataHub::default()),
      scheduler: Arc::new(Mutex::new(Scheduler::new(Vec::new()).unwrap())),
      rows: Vec::new(),
      stopped: Arc::new(AtomicBool::new(false)),
      workers: Mutex::new(Vec::new()),
    };
    let attest = vision.attest(&runtime, gate_path).unwrap();
    assert_eq!(attest["ready"], true);
    assert_eq!(attest["gate_generation"], 0);
    assert_eq!(attest["active_revision"], "a".repeat(64));
    assert_eq!(attest["projection_revision"], "b".repeat(64));
    assert!(vision
      .capture(&runtime, gate_path, &"b".repeat(64), 0, 1280, 80)
      .unwrap()
      .is_none());
  }

  #[test]
  fn blocked_capture_worker_reports_join_timeout_without_waiting_forever() {
    let (release, blocked) = std::sync::mpsc::channel::<()>();
    let worker = thread::spawn(move || {
      let _ = blocked.recv();
    });
    let vision = VisionService {
      revision: "test".into(),
      graph_revision: None,
      run_id: None,
      runtime_stream_id: Uuid::new_v4().to_string(),
      gates: BTreeMap::new(),
      hub: Arc::new(TypedDataHub::default()),
      scheduler: Arc::new(Mutex::new(Scheduler::new(Vec::new()).unwrap())),
      rows: Vec::new(),
      stopped: Arc::new(AtomicBool::new(false)),
      workers: Mutex::new(vec![worker]),
    };
    let start = Instant::now();
    assert_eq!(
      vision.shutdown().unwrap_err(),
      "evidence worker join deadline exceeded"
    );
    assert!(start.elapsed() < Duration::from_secs(2));
    assert!(vision.stopped.load(Ordering::Acquire));
    assert!(vision.route_status()["ok"] == false);
    release.send(()).unwrap();
  }

  #[test]
  fn tiny_positive_camera_rate_has_safe_saturated_cadence() {
    assert_eq!(capture_interval(f64::from_bits(1)), Duration::MAX);
    assert_eq!(capture_interval(1.0), Duration::from_secs(1));
    assert!(capture_interval(f64::MAX) > Duration::from_millis(16));
  }

  #[test]
  fn invalidation_between_preparation_and_commit_rejects_old_frame() {
    use aether_hal::{ClockKind, SampleMeta};
    let fence = Arc::new(AtomicU64::new(0));
    let slot = Arc::new(Mutex::new(Delivered::default()));
    let frame = Arc::new(SourceFrame {
      frame: Arc::new(CameraFrame {
        meta: SampleMeta {
          source_timestamp_ns: None,
          clock: ClockKind::Unknown,
          received_monotonic_ns: 0,
          sequence: None,
          frame_id: "front".into(),
        },
        width_px: 1,
        height_px: 1,
        stride_bytes: 3,
        rgb8: vec![1, 2, 3],
        depth: None,
        preview_jpeg: None,
      }),
      source_frame_id: "src-1".into(),
      health_epoch: 0,
      health_fence: fence.clone(),
    });
    let canonical = || VisionFrame {
      gate_path: "gate".into(),
      sequence: None,
      timestamp_unix_ms: None,
      width_px: 1,
      height_px: 1,
      source_rate_hz: 30.0,
      jpeg: vec![0xff, 0xd8, 0xff, 0xd9],
    };
    let (prepared_tx, prepared_rx) = std::sync::mpsc::channel();
    let (release_tx, release_rx) = std::sync::mpsc::channel();
    let queued = frame.clone();
    let worker_slot = slot.clone();
    let worker = thread::spawn(move || {
      let prepared = canonical();
      prepared_tx.send(()).unwrap();
      release_rx.recv().unwrap();
      commit_delivery(&worker_slot, queued, prepared, "digest".into())
    });
    prepared_rx.recv().unwrap();
    fence.fetch_add(1, Ordering::AcqRel);
    {
      let mut delivered = slot.lock().unwrap();
      delivered.frame = None;
      delivered.canonical = None;
    }
    release_tx.send(()).unwrap();
    assert!(worker.join().unwrap().is_err());
    assert!(slot.lock().unwrap().frame.is_none());
    assert_eq!(slot.lock().unwrap().generation, 0);
    let fresh = Arc::new(SourceFrame {
      frame: frame.frame.clone(),
      source_frame_id: "src-2".into(),
      health_epoch: 1,
      health_fence: fence,
    });
    assert!(commit_delivery(
      &slot,
      fresh,
      VisionFrame {
        gate_path: "gate".into(),
        sequence: None,
        timestamp_unix_ms: None,
        width_px: 1,
        height_px: 1,
        source_rate_hz: 30.0,
        jpeg: vec![0xff, 0xd8, 0xff, 0xd9]
      },
      "digest".into()
    )
    .is_ok());
    assert_eq!(slot.lock().unwrap().generation, 1);
  }
}
