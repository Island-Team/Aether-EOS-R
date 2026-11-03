//! Generated published EOS protobuf identities and Rust gRPC adapter.

pub mod v1 {
  tonic::include_proto!("island.eos.v1");
}

pub mod v2 {
  tonic::include_proto!("island.eos.v2");
}

/// Descriptor generated from the committed public protobuf sources.
pub const DESCRIPTOR_SET: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/eos-descriptor.bin"));

use std::collections::VecDeque;
use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpListener};
use std::collections::BTreeMap;
use std::thread::JoinHandle;
use tonic::{Request, Response, Status};
use aether_dispatcher::{
  AdmissionContext, AdmissionIngress, AuthorityRole, CommandRequest, DispatchResult,
  GatewayLookupError, GatewayOperationInfo, GatewayOperationKind, GatewayRejectionCode, Movement,
  OutcomeStatus, StopDispatchOutcome, StopKind, StopRequest,
};
use aether_hal::{
  BaseMovement, CameraFrame, CommandMeta, EndEffectorPose, ManipulatorMovement, NodeKind,
  VisionFrame,
};
use aether_common::time::{Clock, SystemClock};
use prost::Message;

use crate::Runtime;
use crate::host_skills::Catalog;
use crate::grpc_health::{HealthSnapshots, HealthView};
use crate::managed_admission::{self, GrantRole, ManagedAdmission};

#[path = "grpc/skill_service.rs"]
mod skill_service;
use skill_service::SkillManager as HostSkillManager;

pub fn validate_host_catalog_readiness(
  profile: &aether_hal::Profile,
  managed: bool,
  catalog: &crate::host_skills::Catalog,
) -> Result<(), &'static str> {
  skill_service::validate_catalog_readiness(profile, managed, catalog)
}

/// An admitted, immutable identity projection. Health is overlaid at read time.
#[derive(Clone)]
pub struct Projection {
  runtime: Arc<Runtime>,
  discover: v1::DiscoverResponse,
  bindings: BTreeMap<String, VelocityBinding>,
  converted_bindings: BTreeMap<String, ConvertedBinding>,
  stop_bindings: BTreeMap<String, StopBinding>,
  unavailable_mujoco_motion_stops: std::collections::BTreeSet<String>,
  cameras: BTreeMap<String, String>,
  camera_cache: Arc<CameraCache>,
  health: Arc<HealthSnapshots>,
  frame_encodes: Arc<tokio::sync::Semaphore>,
  #[cfg(test)]
  encode_test_gate: Option<Arc<(std::sync::Barrier, std::sync::Barrier)>>,
  quiescing: Arc<AtomicBool>,
  lifecycle: Arc<tokio::sync::watch::Sender<ServingPhase>>,
  resources: Arc<ResourceStore>,
  resource_watchers: Arc<AtomicUsize>,
  started_at: String,
  managed: Option<Arc<ManagedAdmission>>,
  projection_sha256: Option<String>,
  // In-process R02 compatibility tests use the old standalone Dispatcher.
  // The deployed binary always calls with_camera_cache_managed, which clears
  // this flag even when no IsOS verifier is present.
  legacy_unmanaged_writes: bool,
  host_skills: Option<Arc<HostSkillManager>>,
}

#[derive(Default)]
pub struct CameraCache {
  frames: Mutex<BTreeMap<String, VecDeque<CachedCameraFrame>>>,
}

#[derive(Clone)]
struct CachedCameraFrame {
  frame: Arc<CameraFrame>,
  received: std::time::Instant,
  captured_at: String,
}

impl CameraCache {
  /// Publish one admitted camera sample. The producer never waits for a gRPC encode.
  pub fn push(&self, node_path: &str, frame: Arc<CameraFrame>) {
    if let Ok(mut all) = self.frames.lock() {
      let queue = all.entry(node_path.into()).or_default();
      if queue
        .back()
        .is_some_and(|previous| same_camera_capture(&previous.frame, &frame))
      {
        return;
      }
      queue.push_back(CachedCameraFrame {
        frame,
        received: std::time::Instant::now(),
        captured_at: chrono::Utc::now().to_rfc3339(),
      });
      while queue.len() > 2 {
        queue.pop_front();
      }
    }
  }

  fn snapshot(&self, node_path: &str, count: usize) -> Option<Vec<CachedCameraFrame>> {
    let all = self.frames.lock().ok()?;
    let queue = all.get(node_path)?;
    if queue.len() < count {
      return None;
    }
    let entries: Vec<_> = queue.iter().rev().take(count).cloned().collect();
    if entries
      .iter()
      .any(|entry| entry.received.elapsed() > std::time::Duration::from_secs(1))
    {
      return None;
    }
    Some(entries.into_iter().rev().collect())
  }
}

fn same_camera_capture(previous: &CameraFrame, current: &CameraFrame) -> bool {
  match (previous.meta.sequence, current.meta.sequence) {
    (Some(left), Some(right)) => left == right,
    _ => match (
      previous.meta.source_timestamp_ns,
      current.meta.source_timestamp_ns,
    ) {
      (Some(left), Some(right)) if previous.meta.clock == current.meta.clock => left == right,
      _ => previous.meta.received_monotonic_ns == current.meta.received_monotonic_ns,
    },
  }
}

fn with_sample_age<T>(body: T, age_ms: Option<u64>) -> Response<T> {
  let mut response = Response::new(body);
  if let Some(age_ms) = age_ms {
    if let Ok(value) = age_ms.to_string().parse() {
      response.metadata_mut().insert("eos-sample-age-ms", value);
    }
  }
  response
}

/// Typed products from an admitted producer. The deployed runtime currently
/// supplies an empty registry; tests inject a producer to exercise this wire.
pub struct ResourceStore {
  slots: Mutex<BTreeMap<String, ResourceSlot>>,
}

/// A watch terminates with an explicit gap even when its small data queue is full.
pub struct ResourceWatchStream {
  updates: tokio_stream::wrappers::ReceiverStream<Result<v1::ResourceChanged, Status>>,
  lagged: Arc<AtomicBool>,
  ended: bool,
}

impl tokio_stream::Stream for ResourceWatchStream {
  type Item = Result<v1::ResourceChanged, Status>;

  fn poll_next(
    mut self: std::pin::Pin<&mut Self>,
    context: &mut std::task::Context<'_>,
  ) -> std::task::Poll<Option<Self::Item>> {
    if self.ended {
      return std::task::Poll::Ready(None);
    }
    if self.lagged.swap(false, Ordering::AcqRel) {
      self.ended = true;
      return std::task::Poll::Ready(Some(Err(Status::resource_exhausted(
        "resource watch lagged",
      ))));
    }
    std::pin::Pin::new(&mut self.updates).poll_next(context)
  }
}

struct ResourceSlot {
  class: String,
  product: Option<(prost_types::Struct, std::time::Instant)>,
  changed: tokio::sync::broadcast::Sender<()>,
}

impl ResourceStore {
  pub fn empty() -> Self {
    Self {
      slots: Mutex::new(BTreeMap::new()),
    }
  }

  pub fn admitted(robot_name: &str, descriptors: &[(String, String)]) -> Result<Self, String> {
    let prefix = format!("robots/{robot_name}/");
    let mut slots = BTreeMap::new();
    for (uri, class) in descriptors {
      if uri.len() > 256
        || !uri.starts_with(&prefix)
        || uri.len() == prefix.len()
        || !uri
          .bytes()
          .all(|byte| byte.is_ascii() && !byte.is_ascii_control())
        || !matches!(class.as_str(), "state" | "event" | "detection" | "asr")
      {
        return Err("invalid admitted resource descriptor".into());
      }
      let (changed, _) = tokio::sync::broadcast::channel(16);
      if slots
        .insert(
          uri.clone(),
          ResourceSlot {
            class: class.clone(),
            product: None,
            changed,
          },
        )
        .is_some()
      {
        return Err("duplicate admitted resource URI".into());
      }
    }
    Ok(Self {
      slots: Mutex::new(slots),
    })
  }

  pub fn publish(&self, uri: &str, product: prost_types::Struct) -> Result<(), String> {
    if product.encoded_len() > 64 * 1024 {
      return Err("resource product exceeds 64 KiB".into());
    }
    let mut slots = self.slots.lock().map_err(|_| "resource store poisoned")?;
    let slot = slots.get_mut(uri).ok_or("resource URI is not admitted")?;
    if let Some((previous, observed)) = slot.product.as_mut() {
      if previous == &product {
        *observed = std::time::Instant::now();
        return Ok(());
      }
    }
    slot.product = Some((product, std::time::Instant::now()));
    let _ = slot.changed.send(());
    Ok(())
  }

  fn list(&self) -> prost_types::ListValue {
    let slots = self.slots.lock().expect("resource store poisoned");
    prost_types::ListValue {
      values: slots
        .iter()
        .map(|(uri, slot)| {
          object_value([
            ("uri".into(), string_value(uri)),
            ("class".into(), string_value(&slot.class)),
          ])
        })
        .collect(),
    }
  }

  fn read(&self, uri: &str) -> Result<(prost_types::Struct, u64), Status> {
    let slots = self
      .slots
      .lock()
      .map_err(|_| Status::internal("resource store poisoned"))?;
    let slot = slots
      .get(uri)
      .ok_or_else(|| Status::not_found("resource is not listed"))?;
    let (product, observed) = slot
      .product
      .as_ref()
      .ok_or_else(|| Status::unavailable("resource is cold"))?;
    let age = observed.elapsed();
    if age > std::time::Duration::from_secs(1) {
      return Err(Status::unavailable("resource is stale"));
    }
    Ok((product.clone(), age.as_millis() as u64))
  }

  fn subscribe(&self, uri: &str) -> Result<tokio::sync::broadcast::Receiver<()>, Status> {
    let slots = self
      .slots
      .lock()
      .map_err(|_| Status::internal("resource store poisoned"))?;
    let slot = slots
      .get(uri)
      .ok_or_else(|| Status::not_found("resource is not listed"))?;
    Ok(slot.changed.subscribe())
  }
}

#[derive(Clone)]
struct VelocityBinding {
  node_path: String,
  binding_path: Option<String>,
  device_id: String,
  max_linear: f64,
  max_angular: f64,
  max_lease: u64,
}

#[derive(Clone)]
struct ConvertedBinding {
  node_path: String,
  binding_path: Option<String>,
  device_id: String,
  primitive: &'static str,
  conversion: Conversion,
}

#[derive(Clone)]
enum Conversion {
  RelativeBase {
    max_translation: f64,
    max_rotation: f64,
    frame_id: String,
  },
  AbsoluteJoints {
    names: Vec<String>,
    max_position: f64,
    max_speed: f64,
  },
  AbsoluteTool {
    frame_id: String,
    max_translation: f64,
    max_speed: f64,
  },
}

#[derive(Clone)]
struct StopBinding {
  kind: StopKind,
  device_id: String,
  primitive: &'static str,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ServingPhase {
  Serving,
  Transitioning,
  NotServing,
}

impl Projection {
  fn authenticated_role<T>(&self, request: &Request<T>) -> Result<AuthorityRole, Status> {
    let managed = self
      .managed
      .as_ref()
      .ok_or_else(|| Status::unavailable("managed actuator admission unavailable"))?;
    let bearer = request
      .metadata()
      .get("authorization")
      .and_then(|value| value.to_str().ok())
      .and_then(|value| value.strip_prefix("Bearer "))
      .ok_or_else(|| Status::unauthenticated("control grant required"))?;
    match managed.grant_role(bearer) {
      Some(GrantRole::EgiaControl) => Ok(AuthorityRole::EgiaControl),
      Some(GrantRole::OperatorControl) => Ok(AuthorityRole::OperatorControl),
      None => Err(Status::unauthenticated("invalid control grant")),
    }
  }

  fn expected_revision<T>(
    &self,
    request: &Request<T>,
    emergency: bool,
  ) -> Result<Option<String>, Status> {
    let managed = self
      .managed
      .as_ref()
      .ok_or_else(|| Status::unavailable("managed actuator admission unavailable"))?;
    let value = request
      .metadata()
      .get("x-eos-active-revision")
      .map(|value| {
        value
          .to_str()
          .map_err(|_| Status::invalid_argument("invalid active revision"))
      })
      .transpose()?;
    let Some(value) = value else {
      return if emergency {
        Ok(None)
      } else {
        Err(Status::invalid_argument("active revision required"))
      };
    };
    if !aether_common::events::valid_digest(value) {
      return Err(Status::invalid_argument("invalid active revision"));
    }
    if emergency {
      return Ok(None);
    }
    if value != managed.active_revision {
      return Err(Status::failed_precondition("stale active revision"));
    }
    Ok(Some(value.into()))
  }

  fn generation_metadata<T>(&self, mut response: Response<T>) -> Response<T> {
    if let Some(managed) = &self.managed {
      response.metadata_mut().insert(
        "eos-active-revision",
        managed.active_revision.parse().expect("validated revision"),
      );
    }
    if let Some(digest) = &self.projection_sha256 {
      response.metadata_mut().insert(
        "eos-projection-sha256",
        digest.parse().expect("validated projection digest"),
      );
    }
    response
  }

  fn authorized_operation(
    &self,
    id: &str,
    role: AuthorityRole,
  ) -> Result<GatewayOperationInfo, Status> {
    let managed = self
      .managed
      .as_ref()
      .ok_or_else(|| Status::unavailable("managed actuator admission unavailable"))?;
    self
      .runtime
      .dispatcher()
      .gateway_operation_authorized(id, &managed.run_id, role)
      .map_err(lookup_status)?
      .ok_or_else(|| Status::not_found("unknown operation"))
  }

  async fn ensure_serving(&self) -> Result<(), Status> {
    self
      .ensure_serving_with(std::time::Duration::from_secs(5))
      .await
  }

  async fn ensure_serving_with(&self, budget: std::time::Duration) -> Result<(), Status> {
    let mut lifecycle = self.lifecycle.subscribe();
    let wait = async {
      loop {
        let phase = *lifecycle.borrow_and_update();
        match phase {
          ServingPhase::Serving => {
            if !self.quiescing.load(Ordering::Acquire) {
              return Ok(());
            }
            if *lifecycle.borrow() != ServingPhase::Transitioning {
              return Err(Status::unavailable("runtime is quiescing"));
            }
          }
          ServingPhase::NotServing => {
            return Err(Status::unavailable("runtime is quiescing"));
          }
          ServingPhase::Transitioning => {}
        }
        lifecycle
          .changed()
          .await
          .map_err(|_| Status::unavailable("runtime listener ended"))?;
      }
    };
    tokio::time::timeout(budget, wait)
      .await
      .map_err(|_| Status::deadline_exceeded("shutdown transition exceeded caller deadline"))?
  }

  pub fn new(runtime: Arc<Runtime>) -> Result<Self, String> {
    Self::with_sources(
      runtime,
      Arc::new(CameraCache::default()),
      Arc::new(ResourceStore::empty()),
    )
  }

  pub fn with_camera_cache(
    runtime: Arc<Runtime>,
    camera_cache: Arc<CameraCache>,
  ) -> Result<Self, String> {
    Self::with_sources(runtime, camera_cache, Arc::new(ResourceStore::empty()))
  }

  pub fn with_camera_cache_managed(
    runtime: Arc<Runtime>,
    camera_cache: Arc<CameraCache>,
    managed: Option<Arc<ManagedAdmission>>,
    projection_sha256: String,
  ) -> Result<Self, String> {
    let mut projection = Self::with_camera_cache(runtime, camera_cache)?;
    projection.managed = managed;
    projection.projection_sha256 = Some(projection_sha256);
    projection.legacy_unmanaged_writes = false;
    Ok(projection)
  }

  /// Install the complete, pre-admitted host catalog before public readiness.
  pub fn with_host_skills(
    mut self,
    catalog: Arc<Catalog>,
    executable: std::path::PathBuf,
    events: Option<
      Arc<aether_common::events::EventSink<aether_common::events::NonblockingStderr, SystemClock>>,
    >,
  ) -> Result<Self, String> {
    self.host_skills = Some(HostSkillManager::new(&self, catalog, executable, events)?);
    Ok(self)
  }

  /// Called only after the physical Dispatcher stop and while the simulator
  /// child is still alive.
  pub fn shutdown_host_skills(&self) -> Result<(), String> {
    if let Some(manager) = &self.host_skills {
      manager.shutdown();
      manager.wait_stopped()?;
    }
    Ok(())
  }

  pub fn with_sources(
    runtime: Arc<Runtime>,
    camera_cache: Arc<CameraCache>,
    resources: Arc<ResourceStore>,
  ) -> Result<Self, String> {
    let profile = runtime.profile();
    let mut devices = Vec::new();
    let mut bindings = BTreeMap::new();
    let mut converted_bindings = BTreeMap::new();
    let mut stop_bindings = BTreeMap::new();
    let mut unavailable_mujoco_motion_stops = std::collections::BTreeSet::new();
    let mut cameras = BTreeMap::new();
    let mut actuator_count = 0usize;
    for descriptor in runtime.discover() {
      let node = profile
        .node(&descriptor.path)
        .ok_or("discovered node missing from profile")?;
      let mut primitives = Vec::new();
      if matches!(descriptor.kind, NodeKind::Base | NodeKind::Manipulator)
        && node.movement.is_some()
      {
        actuator_count += 1;
        if actuator_count > 64 {
          return Err("more than 64 admitted stop targets".into());
        }
      }
      if descriptor.kind == NodeKind::Camera
        && cameras
          .insert(node.id.clone(), node.path().into())
          .is_some()
      {
        return Err("duplicate camera device ID".into());
      }
      if descriptor.kind == NodeKind::Base
        && runtime.dispatcher().has_terminal_stop_fence(node.path())
        && node
          .movement
          .as_ref()
          .is_some_and(|movement| movement.modes.iter().any(|mode| mode == "body_velocity"))
        && has_exact_dispatcher_edge(profile, node, "body_velocity")
      {
        let limits = &node.movement.as_ref().expect("mode checked").limits;
        if let (Some(max_linear), Some(max_angular), Some(max_lease)) = (
          limits.max_linear_m_s,
          limits.max_angular_rad_s,
          limits.max_velocity_lease_ms,
        ) {
          let name = format!(
            "hardware/{}/base/{}/base.velocity@1.0.0",
            profile.robot.id, node.id
          );
          let inserted = bindings.insert(
            name,
            VelocityBinding {
              node_path: node.path().into(),
              binding_path: managed_admission::binding_for(
                profile,
                node,
                "body_velocity",
                "base.velocity",
              )
              .map(|binding| binding.binding_path),
              device_id: node.id.clone(),
              max_linear,
              max_angular,
              max_lease,
            },
          );
          if inserted.is_some() {
            return Err("duplicate capability name".into());
          }
          primitives.push(v1::PrimitiveBinding {
            name: "base.velocity".into(),
            version: "1.0.0".into(),
            kind: "direct".into(),
            references: Vec::new(),
            limits: Some(velocity_limits(max_linear, max_angular, max_lease)),
            health: "ready".into(),
            dependencies: Vec::new(),
            accepts: vec!["body_velocity".into()],
            absolute_frame: String::new(),
            description: "Bounded body-frame velocity lease".into(),
            parameters: Some(prost_types::Struct {
              fields: BTreeMap::new(),
            }),
          });
        }
      }
      if runtime.dispatcher().has_terminal_stop_fence(node.path()) {
        if let Some(movement) = &node.movement {
          let limits = &movement.limits;
          let conversion = if descriptor.kind == NodeKind::Base
            && movement.modes.iter().any(|mode| mode == "relative_pose")
            && has_exact_dispatcher_edge(profile, node, "relative_pose")
          {
            limits
              .max_translation_m
              .zip(limits.max_rotation_rad)
              .map(|(translation, rotation)| {
                (
                  "base.navigate",
                  Conversion::RelativeBase {
                    max_translation: translation,
                    max_rotation: rotation,
                    frame_id: node.frame_id.clone(),
                  },
                  vec!["relative".into()],
                  String::new(),
                  prost_types::Struct {
                    fields: [
                      ("x_m".into(), range(-translation, translation)),
                      ("y_m".into(), range(-translation, translation)),
                      ("yaw_rad".into(), range(-rotation, rotation)),
                    ]
                    .into(),
                  },
                )
              })
          } else if descriptor.kind == NodeKind::Manipulator
            && movement.modes.iter().any(|mode| mode == "joint_positions")
            && has_exact_dispatcher_edge(profile, node, "joint_positions")
          {
            limits
              .max_joint_position_rad
              .zip(limits.max_joint_speed_rad_s)
              .and_then(|(position, speed)| {
                node.joint_names.as_ref().map(|names| {
                  (
                    "arm.movej",
                    Conversion::AbsoluteJoints {
                      names: names.clone(),
                      max_position: position,
                      max_speed: speed,
                    },
                    vec!["absolute".into()],
                    String::new(),
                    prost_types::Struct {
                      fields: [
                        ("joint_positions_rad".into(), range(-position, position)),
                        ("speed_rad_s".into(), positive_range(speed)),
                      ]
                      .into(),
                    },
                  )
                })
              })
          } else if descriptor.kind == NodeKind::Manipulator
            && movement
              .modes
              .iter()
              .any(|mode| mode == "end_effector_pose")
            && has_exact_dispatcher_edge(profile, node, "end_effector_pose")
          {
            limits
              .max_end_effector_translation_m
              .zip(limits.max_end_effector_speed_m_s)
              .map(|(translation, speed)| {
                (
                  "arm.movel",
                  Conversion::AbsoluteTool {
                    frame_id: node.frame_id.clone(),
                    max_translation: translation,
                    max_speed: speed,
                  },
                  vec!["absolute".into()],
                  node.frame_id.clone(),
                  prost_types::Struct {
                    fields: [("speed_m_s".into(), positive_range(speed))].into(),
                  },
                )
              })
          } else {
            None
          };
          if let Some((primitive, conversion, references, absolute_frame, limits)) = conversion {
            let name = format!(
              "hardware/{}/{}/{}/{}@1.0.0",
              profile.robot.id, descriptor.kind, node.id, primitive
            );
            if converted_bindings
              .insert(
                name,
                ConvertedBinding {
                  node_path: node.path().into(),
                  binding_path: managed_admission::binding_for(
                    profile,
                    node,
                    match primitive {
                      "base.navigate" => "relative_pose",
                      "arm.movej" => "joint_positions",
                      "arm.movel" => "end_effector_pose",
                      _ => "",
                    },
                    primitive,
                  )
                  .map(|binding| binding.binding_path),
                  device_id: node.id.clone(),
                  primitive,
                  conversion,
                },
              )
              .is_some()
            {
              return Err("duplicate converted capability name".into());
            }
            primitives.push(v1::PrimitiveBinding {
              name: primitive.into(),
              version: "1.0.0".into(),
              kind: "direct".into(),
              references,
              limits: Some(limits),
              health: "ready".into(),
              dependencies: Vec::new(),
              accepts: Vec::new(),
              absolute_frame,
              description: format!("Bounded {primitive} through Dispatcher"),
              parameters: Some(prost_types::Struct::default()),
            });
          }
          if descriptor.kind == NodeKind::Manipulator
            && movement.modes.iter().any(|mode| mode == "joint_positions")
            && movement
              .modes
              .iter()
              .any(|mode| mode == "end_effector_pose")
            && has_exact_dispatcher_edge(profile, node, "end_effector_pose")
          {
            if let Some((translation, speed)) = limits
              .max_end_effector_translation_m
              .zip(limits.max_end_effector_speed_m_s)
            {
              let name = format!(
                "hardware/{}/manipulator/{}/arm.movel@1.0.0",
                profile.robot.id, node.id
              );
              if converted_bindings
                .insert(
                  name,
                  ConvertedBinding {
                    node_path: node.path().into(),
                    binding_path: managed_admission::binding_for(
                      profile,
                      node,
                      "end_effector_pose",
                      "arm.movel",
                    )
                    .map(|binding| binding.binding_path),
                    device_id: node.id.clone(),
                    primitive: "arm.movel",
                    conversion: Conversion::AbsoluteTool {
                      frame_id: node.frame_id.clone(),
                      max_translation: translation,
                      max_speed: speed,
                    },
                  },
                )
                .is_some()
              {
                return Err("duplicate converted capability name".into());
              }
              primitives.push(v1::PrimitiveBinding {
                name: "arm.movel".into(),
                version: "1.0.0".into(),
                kind: "direct".into(),
                references: vec!["absolute".into()],
                limits: Some(prost_types::Struct {
                  fields: [("speed_m_s".into(), positive_range(speed))].into(),
                }),
                health: "ready".into(),
                dependencies: Vec::new(),
                accepts: Vec::new(),
                absolute_frame: node.frame_id.clone(),
                description: "Bounded arm.movel through Dispatcher".into(),
                parameters: Some(prost_types::Struct::default()),
              });
            }
          }
        }
      }
      let motion_stop_name = format!(
        "hardware/{}/{}/{}/motion.stop@1.0.0",
        profile.robot.id, descriptor.kind, node.id
      );
      if runtime.dispatcher().supports_motion_stop(node.path()) {
        let name = motion_stop_name;
        if stop_bindings
          .insert(
            name,
            StopBinding {
              kind: StopKind::MotionStop {
                node_path: node.path().into(),
              },
              device_id: node.id.clone(),
              primitive: "motion.stop",
            },
          )
          .is_some()
        {
          return Err("duplicate stop capability name".into());
        }
        primitives.push(stop_primitive("motion.stop"));
      } else if node.adapter.adapter_type == "mujoco-v2" && node.movement.is_some() {
        unavailable_mujoco_motion_stops.insert(motion_stop_name);
      }
      devices.push(v1::DeviceDescriptor {
        id: descriptor.node_id,
        r#class: descriptor.kind.to_string(),
        part: node.id.clone(),
        state: "idle".into(),
        health: "ready".into(),
        primitives,
        description: node.description.clone(),
        state_uri: String::new(),
      });
    }
    devices.sort_by(|a, b| a.id.cmp(&b.id));
    let mut robot_primitives = Vec::new();
    if runtime.dispatcher().supports_non_latching_robot_stop() {
      stop_bindings.insert(
        format!("runtime/{}/robot.stop@1.0.0", profile.robot.id),
        StopBinding {
          kind: StopKind::RobotStop {
            robot_id: profile.robot.id.clone(),
          },
          device_id: String::new(),
          primitive: "robot.stop",
        },
      );
      robot_primitives.push(stop_primitive("robot.stop"));
    }
    if runtime.dispatcher().supports_safety_estop() {
      stop_bindings.insert(
        format!("runtime/{}/safety.estop@1.0.0", profile.robot.id),
        StopBinding {
          kind: StopKind::SafetyEstop {
            robot_id: profile.robot.id.clone(),
          },
          device_id: String::new(),
          primitive: "safety.estop",
        },
      );
      robot_primitives.push(stop_primitive("safety.estop"));
    }
    let started_at = chrono::Utc::now().to_rfc3339();
    let health = HealthSnapshots::new(&runtime);
    Ok(Self {
      runtime: runtime.clone(),
      discover: v1::DiscoverResponse {
        robot: Some(v1::RobotDescriptor {
          id: profile.robot.id.clone(),
          name: profile.robot.name.clone(),
          model: "eos-runtime".into(),
          state: "idle".into(),
          health: "ready".into(),
          estop: false,
          power_pct: None,
          description: format!("EOS graph runtime {}", profile.robot.name),
          primitives: robot_primitives,
        }),
        devices,
      },
      bindings,
      converted_bindings,
      stop_bindings,
      unavailable_mujoco_motion_stops,
      cameras,
      camera_cache,
      health,
      frame_encodes: Arc::new(tokio::sync::Semaphore::new(8)),
      #[cfg(test)]
      encode_test_gate: None,
      quiescing: Arc::new(AtomicBool::new(false)),
      lifecycle: Arc::new(tokio::sync::watch::channel(ServingPhase::Serving).0),
      resources,
      resource_watchers: Arc::new(AtomicUsize::new(0)),
      started_at,
      managed: None,
      projection_sha256: None,
      legacy_unmanaged_writes: true,
      host_skills: None,
    })
  }

  pub fn discover(&self) -> v1::DiscoverResponse {
    self.discover_from(&self.health.view())
  }

  fn discover_from(&self, health: &HealthView) -> v1::DiscoverResponse {
    let mut response = self.discover.clone();
    let write_ready = self.managed.is_some() || self.legacy_unmanaged_writes;
    for device in &mut response.devices {
      if let Some(descriptor) = self
        .runtime
        .discover()
        .into_iter()
        .find(|d| d.node_id == device.id)
      {
        let (healthy, ready) = health.read(&descriptor.path);
        device.health = if healthy { "ready" } else { "degraded" }.into();
        device.state = if ready { "idle" } else { "unavailable" }.into();
        for primitive in &mut device.primitives {
          let exact_binding = self.bindings.values().any(|binding| {
            binding.node_path == descriptor.path
              && primitive.name == "base.velocity"
              && binding.binding_path.is_some()
          }) || self.converted_bindings.values().any(|binding| {
            binding.node_path == descriptor.path
              && primitive.name == binding.primitive
              && binding.binding_path.is_some()
          });
          primitive.health = if write_ready
            && (self.legacy_unmanaged_writes || primitive.name == "motion.stop" || exact_binding)
            && (primitive.name == "motion.stop"
              || (healthy
                && ready
                && !self.runtime.dispatcher().movement_inhibited()
                && !self.runtime.dispatcher().emergency_stopped()))
          {
            "ready"
          } else {
            "unavailable"
          }
          .into();
        }
      }
    }
    if let Some(robot) = &mut response.robot {
      if response.devices.iter().any(|d| d.health != "ready")
        || self.runtime.dispatcher().movement_inhibited()
        || self.runtime.dispatcher().emergency_stopped()
        || self.runtime.dispatcher().has_legacy_journal_records()
      {
        robot.health = "degraded".into();
      }
      robot.estop = self.runtime.dispatcher().emergency_stopped();
      for primitive in &mut robot.primitives {
        primitive.health = if write_ready { "ready" } else { "unavailable" }.into();
      }
    }
    response
  }

  pub fn status(&self) -> prost_types::Struct {
    self.status_from(&self.health.view())
  }

  fn status_from(&self, view: &HealthView) -> prost_types::Struct {
    let robot = self.discover.robot.as_ref().expect("admitted robot");
    let health = self
      .discover_from(view)
      .robot
      .expect("admitted robot")
      .health;
    prost_types::Struct {
      fields: [
        ("robot_id".into(), string_value(&robot.id)),
        ("robot_name".into(), string_value(&robot.name)),
        (
          "status".into(),
          string_value(if health == "ready" {
            "ready"
          } else {
            "degraded"
          }),
        ),
        ("version".into(), string_value(env!("CARGO_PKG_VERSION"))),
        ("started_at".into(), string_value(&self.started_at)),
      ]
      .into(),
    }
  }

  fn capabilities(&self, health: &HealthView) -> v2::ListCapabilitiesResponse {
    let write_ready = self.managed.is_some() || self.legacy_unmanaged_writes;
    let mut capabilities: Vec<_> = self
      .bindings
      .iter()
      .map(|(name, binding)| {
        let (healthy, ready) = health.read(&binding.node_path);
        v2::CapabilityDescriptor {
          name: name.clone(),
          available: write_ready
            && (self.legacy_unmanaged_writes || binding.binding_path.is_some())
            && healthy
            && ready
            && !self.runtime.dispatcher().movement_inhibited()
            && !self.runtime.dispatcher().emergency_stopped(),
          primitive: "base.velocity".into(),
          version: "1.0.0".into(),
          device_id: binding.device_id.clone(),
        }
      })
      .collect();
    capabilities.extend(self.stop_bindings.iter().map(|(name, binding)| {
      v2::CapabilityDescriptor {
        name: name.clone(),
        available: write_ready,
        primitive: binding.primitive.into(),
        version: "1.0.0".into(),
        device_id: binding.device_id.clone(),
      }
    }));
    capabilities.extend(self.converted_bindings.iter().map(|(name, binding)| {
      let (healthy, ready) = health.read(&binding.node_path);
      v2::CapabilityDescriptor {
        name: name.clone(),
        available: write_ready
          && (self.legacy_unmanaged_writes || binding.binding_path.is_some())
          && healthy
          && ready
          && !self.runtime.dispatcher().movement_inhibited()
          && !self.runtime.dispatcher().emergency_stopped(),
        primitive: binding.primitive.into(),
        version: "1.0.0".into(),
        device_id: binding.device_id.clone(),
      }
    }));
    capabilities.sort_by(|a, b| a.name.cmp(&b.name));
    v2::ListCapabilitiesResponse { capabilities }
  }
}

fn lookup_status(error: GatewayLookupError) -> Status {
  match error {
    GatewayLookupError::LegacyRecord
    | GatewayLookupError::Unauthorized
    | GatewayLookupError::WrongRun
    | GatewayLookupError::NotFound => Status::not_found("unknown operation"),
    GatewayLookupError::Internal => Status::internal("operation journal unavailable"),
  }
}

fn legacy_lookup_status(error: GatewayLookupError) -> Status {
  match error {
    GatewayLookupError::LegacyRecord => {
      Status::data_loss("legacy operation lacks admission identity")
    }
    GatewayLookupError::Unauthorized => {
      Status::permission_denied("operation belongs to another role")
    }
    GatewayLookupError::WrongRun => Status::failed_precondition("operation belongs to another run"),
    GatewayLookupError::NotFound => Status::not_found("unknown operation"),
    GatewayLookupError::Internal => Status::internal("operation journal unavailable"),
  }
}

fn has_exact_dispatcher_edge(
  profile: &aether_hal::Profile,
  node: &aether_hal::NodeProfile,
  movement_type: &str,
) -> bool {
  node.inputs.get("command").is_some_and(|sources| {
    let sources = sources.values();
    if sources.len() != 1 {
      return false;
    }
    let source = &sources[0];
    let Some(control) = profile.node(&source.node) else {
      return false;
    };
    match control.node_type.as_str() {
      "eos.dispatcher.control@v1.0" => {
        source.output
          == if node.kind == NodeKind::Base {
            "base_command"
          } else {
            "manipulator_command"
          }
      }
      "eos.dispatcher.primitive_base@v0.1" | "eos.dispatcher.primitive_base@v1.0" => {
        source.output == "command"
          && node.kind == NodeKind::Base
          && control
            .settings
            .get("movement_type")
            .and_then(|v| v.as_str())
            == Some(movement_type)
      }
      "eos.dispatcher.primitive_manipulator@v0.1" | "eos.dispatcher.primitive_manipulator@v1.0" => {
        source.output == "command"
          && node.kind == NodeKind::Manipulator
          && control
            .settings
            .get("movement_type")
            .and_then(|v| v.as_str())
            == Some(movement_type)
      }
      _ => false,
    }
  })
}

fn range(minimum: f64, maximum: f64) -> prost_types::Value {
  object_value([
    ("minimum".into(), number_value(minimum)),
    ("maximum".into(), number_value(maximum)),
  ])
}

fn positive_range(maximum: f64) -> prost_types::Value {
  object_value([
    ("minimum".into(), number_value(0.0)),
    ("minimum_exclusive".into(), bool_value(true)),
    ("maximum".into(), number_value(maximum)),
  ])
}

fn velocity_limits(linear: f64, angular: f64, lease: u64) -> prost_types::Struct {
  prost_types::Struct {
    fields: [
      ("linear_x_m_s".into(), range(-linear, linear)),
      ("linear_y_m_s".into(), range(-linear, linear)),
      ("angular_z_rad_s".into(), range(-angular, angular)),
      ("valid_for_ms".into(), range(1.0, lease as f64)),
    ]
    .into(),
  }
}

fn stop_primitive(name: &str) -> v1::PrimitiveBinding {
  v1::PrimitiveBinding {
    name: name.into(),
    version: "1.0.0".into(),
    kind: "direct".into(),
    references: Vec::new(),
    limits: None,
    health: "ready".into(),
    dependencies: Vec::new(),
    accepts: Vec::new(),
    absolute_frame: String::new(),
    description: "Terminal stop through Dispatcher".into(),
    parameters: Some(prost_types::Struct {
      fields: BTreeMap::new(),
    }),
  }
}

fn numeric_arg(arguments: &prost_types::Struct, name: &str) -> Result<f64, Status> {
  let value = arguments
    .fields
    .get(name)
    .ok_or_else(|| Status::invalid_argument(format!("missing {name}")))?;
  let Some(prost_types::value::Kind::NumberValue(number)) = value.kind else {
    return Err(Status::invalid_argument(format!("{name} must be numeric")));
  };
  if !number.is_finite() {
    return Err(Status::invalid_argument(format!("{name} must be finite")));
  }
  Ok(number)
}

fn parse_velocity(
  arguments: &prost_types::Struct,
  binding: &VelocityBinding,
) -> Result<BaseMovement, Status> {
  const FIELDS: [&str; 4] = [
    "linear_x_m_s",
    "linear_y_m_s",
    "angular_z_rad_s",
    "valid_for_ms",
  ];
  if arguments.fields.len() != FIELDS.len()
    || arguments
      .fields
      .keys()
      .any(|key| !FIELDS.contains(&key.as_str()))
  {
    return Err(Status::invalid_argument(
      "base.velocity requires exactly four fields",
    ));
  }
  let x = numeric_arg(arguments, FIELDS[0])?;
  let y = numeric_arg(arguments, FIELDS[1])?;
  let yaw = numeric_arg(arguments, FIELDS[2])?;
  let lease = numeric_arg(arguments, FIELDS[3])?;
  if x.abs() > binding.max_linear
    || y.abs() > binding.max_linear
    || yaw.abs() > binding.max_angular
    || lease.fract() != 0.0
    || lease < 1.0
    || lease > binding.max_lease as f64
  {
    return Err(Status::invalid_argument(
      "base.velocity exceeds admitted limits",
    ));
  }
  Ok(BaseMovement::BodyVelocity {
    x_m_s: x,
    y_m_s: y,
    yaw_rad_s: yaw,
    valid_for_ms: lease as u64,
  })
}

fn string_arg<'a>(arguments: &'a prost_types::Struct, name: &str) -> Result<&'a str, Status> {
  match arguments
    .fields
    .get(name)
    .and_then(|value| value.kind.as_ref())
  {
    Some(prost_types::value::Kind::StringValue(value)) => Ok(value),
    _ => Err(Status::invalid_argument(format!("{name} must be a string"))),
  }
}

fn numeric_array(arguments: &prost_types::Struct, name: &str) -> Result<Vec<f64>, Status> {
  let Some(prost_types::value::Kind::ListValue(values)) = arguments
    .fields
    .get(name)
    .and_then(|value| value.kind.as_ref())
  else {
    return Err(Status::invalid_argument(format!("{name} must be an array")));
  };
  values
    .values
    .iter()
    .map(|value| match value.kind {
      Some(prost_types::value::Kind::NumberValue(number)) if number.is_finite() => Ok(number),
      _ => Err(Status::invalid_argument(format!(
        "{name} requires finite numbers"
      ))),
    })
    .collect()
}

fn exact_argument_keys(
  arguments: &prost_types::Struct,
  required: &[&str],
  optional: &[&str],
) -> Result<(), Status> {
  if required
    .iter()
    .any(|name| !arguments.fields.contains_key(*name))
    || arguments
      .fields
      .keys()
      .any(|name| !required.contains(&name.as_str()) && !optional.contains(&name.as_str()))
  {
    return Err(Status::invalid_argument(
      "arguments do not match admitted primitive",
    ));
  }
  Ok(())
}

fn optional_number(arguments: &prost_types::Struct, name: &str) -> Result<Option<f64>, Status> {
  arguments
    .fields
    .get(name)
    .map(|_| numeric_arg(arguments, name))
    .transpose()
}

fn parse_converted(
  arguments: &prost_types::Struct,
  binding: &ConvertedBinding,
) -> Result<Movement, Status> {
  match &binding.conversion {
    Conversion::RelativeBase {
      max_translation,
      max_rotation,
      frame_id,
    } => {
      exact_argument_keys(arguments, &["reference", "x_m", "y_m", "yaw_rad"], &[])?;
      if string_arg(arguments, "reference")? != "relative" {
        return Err(Status::invalid_argument(
          "base.navigate requires relative reference",
        ));
      }
      let x_m = numeric_arg(arguments, "x_m")?;
      let y_m = numeric_arg(arguments, "y_m")?;
      let yaw_rad = numeric_arg(arguments, "yaw_rad")?;
      if x_m.abs() > *max_translation
        || y_m.abs() > *max_translation
        || yaw_rad.abs() > *max_rotation
      {
        return Err(Status::invalid_argument(
          "base.navigate exceeds admitted limits",
        ));
      }
      Ok(Movement::Base(BaseMovement::RelativePose {
        x_m,
        y_m,
        yaw_rad,
        frame_id: frame_id.clone(),
      }))
    }
    Conversion::AbsoluteJoints {
      names,
      max_position,
      max_speed,
    } => {
      exact_argument_keys(
        arguments,
        &["reference", "joint_positions_rad"],
        &["speed_rad_s"],
      )?;
      if string_arg(arguments, "reference")? != "absolute" {
        return Err(Status::invalid_argument(
          "arm.movej requires absolute reference",
        ));
      }
      let positions = numeric_array(arguments, "joint_positions_rad")?;
      let speed = optional_number(arguments, "speed_rad_s")?;
      if positions.len() != names.len()
        || positions.iter().any(|value| value.abs() > *max_position)
        || speed.is_some_and(|value| value <= 0.0 || value > *max_speed)
      {
        return Err(Status::invalid_argument(
          "arm.movej exceeds admitted limits",
        ));
      }
      Ok(Movement::Manipulator(ManipulatorMovement::JointPositions {
        joint_names: names.clone(),
        joint_positions_rad: positions,
        max_speed_rad_s: speed,
      }))
    }
    Conversion::AbsoluteTool {
      frame_id,
      max_translation,
      max_speed,
    } => {
      exact_argument_keys(
        arguments,
        &[
          "reference",
          "frame_id",
          "position_m",
          "orientation_quat_xyzw",
        ],
        &["speed_m_s"],
      )?;
      if string_arg(arguments, "reference")? != "absolute"
        || string_arg(arguments, "frame_id")? != frame_id
      {
        return Err(Status::invalid_argument(
          "arm.movel requires admitted absolute frame",
        ));
      }
      let position: [f64; 3] = numeric_array(arguments, "position_m")?
        .try_into()
        .map_err(|_| Status::invalid_argument("position_m requires three numbers"))?;
      let orientation: [f64; 4] = numeric_array(arguments, "orientation_quat_xyzw")?
        .try_into()
        .map_err(|_| Status::invalid_argument("orientation_quat_xyzw requires four numbers"))?;
      let speed = optional_number(arguments, "speed_m_s")?;
      let qnorm: f64 = orientation.iter().map(|value| value * value).sum();
      let distance: f64 = position
        .iter()
        .map(|value| value * value)
        .sum::<f64>()
        .sqrt();
      if !(0.999..=1.001).contains(&qnorm)
        || distance > *max_translation
        || speed.is_some_and(|value| value <= 0.0 || value > *max_speed)
      {
        return Err(Status::invalid_argument(
          "arm.movel exceeds admitted limits",
        ));
      }
      Ok(Movement::Manipulator(
        ManipulatorMovement::EndEffectorPose {
          pose: EndEffectorPose {
            position_m: position,
            orientation_xyzw: orientation,
            frame_id: frame_id.clone(),
          },
          max_speed_m_s: speed,
        },
      ))
    }
  }
}

fn string_value(s: &str) -> prost_types::Value {
  prost_types::Value {
    kind: Some(prost_types::value::Kind::StringValue(s.into())),
  }
}

fn object_value(
  fields: impl IntoIterator<Item = (String, prost_types::Value)>,
) -> prost_types::Value {
  prost_types::Value {
    kind: Some(prost_types::value::Kind::StructValue(prost_types::Struct {
      fields: fields.into_iter().collect(),
    })),
  }
}

fn bool_value(value: bool) -> prost_types::Value {
  prost_types::Value {
    kind: Some(prost_types::value::Kind::BoolValue(value)),
  }
}

fn number_value(value: f64) -> prost_types::Value {
  prost_types::Value {
    kind: Some(prost_types::value::Kind::NumberValue(value)),
  }
}

fn null_value() -> prost_types::Value {
  prost_types::Value {
    kind: Some(prost_types::value::Kind::NullValue(0)),
  }
}

fn request_budget<T>(request: &Request<T>) -> Result<std::time::Duration, Status> {
  let Some(raw) = request.metadata().get("grpc-timeout") else {
    return Ok(std::time::Duration::from_secs(5));
  };
  let raw = raw
    .to_str()
    .map_err(|_| Status::invalid_argument("invalid grpc-timeout"))?;
  if raw.len() < 2 || raw.len() > 9 {
    return Err(Status::invalid_argument("invalid grpc-timeout"));
  }
  let (digits, unit) = raw.split_at(raw.len() - 1);
  if digits.len() > 8 || !digits.bytes().all(|byte| byte.is_ascii_digit()) {
    return Err(Status::invalid_argument("invalid grpc-timeout"));
  }
  let value: u64 = digits
    .parse()
    .map_err(|_| Status::invalid_argument("invalid grpc-timeout"))?;
  let nanos = match unit {
    "H" => value.saturating_mul(3_600_000_000_000),
    "M" => value.saturating_mul(60_000_000_000),
    "S" => value.saturating_mul(1_000_000_000),
    "m" => value.saturating_mul(1_000_000),
    "u" => value.saturating_mul(1_000),
    "n" => value,
    _ => return Err(Status::invalid_argument("invalid grpc-timeout")),
  };
  if nanos == 0 {
    return Err(Status::deadline_exceeded("caller deadline expired"));
  }
  Ok(std::time::Duration::from_nanos(nanos.min(5_000_000_000)))
}

fn checked_stop_result(outcome: StopDispatchOutcome) -> Result<DispatchResult, Status> {
  match outcome {
    StopDispatchOutcome::Operation(result) => Ok(result),
    StopDispatchOutcome::PreReleaseCapacity(_) => Err(Status::resource_exhausted(
      "stop reservation exceeds durable capacity",
    )),
    StopDispatchOutcome::PreReleaseDurability(_) => {
      Err(Status::internal("stop reservation could not be persisted"))
    }
  }
}

fn operation(info: GatewayOperationInfo) -> v2::Operation {
  let (status, result_status) = match info.status {
    OutcomeStatus::Completed => (v2::OperationStatus::Succeeded, "succeeded"),
    OutcomeStatus::Failed => (v2::OperationStatus::Failed, "failed"),
    OutcomeStatus::Cancelled => (v2::OperationStatus::Cancelled, "cancelled"),
    OutcomeStatus::Rejected => (v2::OperationStatus::Rejected, "rejected"),
    OutcomeStatus::InProgress => (v2::OperationStatus::Pending, "pending"),
    OutcomeStatus::OutcomeUnknown => (v2::OperationStatus::OutcomeUnknown, "outcome_unknown"),
  };
  let rejection = info
    .rejection_code
    .unwrap_or(GatewayRejectionCode::InvalidState);
  let rejection_reason = match rejection {
    GatewayRejectionCode::Unauthorized => "local recovery authority required",
    GatewayRejectionCode::InvalidState => "operation rejected by current state",
    GatewayRejectionCode::Internal => "operation unavailable",
  };
  let mut result_fields = BTreeMap::from([("status".into(), string_value(result_status))]);
  if info.kind == GatewayOperationKind::RobotStop {
    result_fields.insert(
      "parts".into(),
      prost_types::Value {
        kind: Some(prost_types::value::Kind::ListValue(
          prost_types::ListValue {
            values: info
              .parts
              .iter()
              .map(|part| {
                let state = match part.status {
                  OutcomeStatus::Completed => "succeeded",
                  OutcomeStatus::Failed => "failed",
                  OutcomeStatus::Cancelled => "cancelled",
                  OutcomeStatus::Rejected => "rejected",
                  OutcomeStatus::InProgress => "pending",
                  OutcomeStatus::OutcomeUnknown => "outcome_unknown",
                };
                object_value([
                  ("node_path".into(), string_value(&part.node_path)),
                  ("status".into(), string_value(state)),
                  (
                    "reason".into(),
                    part.reason.as_deref().map_or_else(null_value, string_value),
                  ),
                ])
              })
              .collect(),
          },
        )),
      },
    );
  }
  let acknowledgement = info.accepted.map(|accepted| prost_types::Struct {
    fields: [
      ("type".into(), string_value("primitive.ack")),
      ("primitive".into(), string_value(&info.primitive)),
      ("primitive_version".into(), string_value(&info.version)),
      ("accepted".into(), bool_value(accepted)),
      (
        "reason".into(),
        if accepted {
          null_value()
        } else {
          string_value(rejection_reason)
        },
      ),
      (
        "reason_code".into(),
        if accepted {
          null_value()
        } else {
          string_value(rejection.as_str())
        },
      ),
      ("command_id".into(), string_value(&info.command_id)),
      ("sequence".into(), number_value(1.0)),
      ("result".into(), object_value(result_fields)),
    ]
    .into(),
  });
  v2::Operation {
    operation_id: info.command_id,
    status: status as i32,
    accepted: info.accepted,
    reason_code: if info.status == OutcomeStatus::Rejected {
      rejection.as_str().into()
    } else {
      String::new()
    },
    acknowledgement,
  }
}

enum AdmittedOperation {
  Movement(AdmissionContext, CommandRequest),
  Stop(AdmissionContext, StopRequest),
}

enum LegacyOperation {
  Movement(CommandRequest, &'static str),
  Stop(StopRequest),
}

#[derive(Clone)]
pub struct Gateway(pub Projection);

#[tonic::async_trait]
impl v1::gateway_service_server::GatewayService for Gateway {
  async fn discover(&self, _: Request<()>) -> Result<Response<v1::DiscoverResponse>, Status> {
    self.0.ensure_serving().await?;
    let view = self.0.health.view();
    Ok(self.0.generation_metadata(with_sample_age(
      self.0.discover_from(&view),
      view.max_age_ms(),
    )))
  }
  async fn get_status(&self, _: Request<()>) -> Result<Response<prost_types::Struct>, Status> {
    self.0.ensure_serving().await?;
    let view = self.0.health.view();
    Ok(self.0.generation_metadata(with_sample_age(
      self.0.status_from(&view),
      view.max_age_ms(),
    )))
  }
  async fn list_resources(
    &self,
    _: Request<()>,
  ) -> Result<Response<prost_types::ListValue>, Status> {
    self.0.ensure_serving().await?;
    Ok(Response::new(self.0.resources.list()))
  }
  async fn read_resource(
    &self,
    request: Request<v1::ResourceRequest>,
  ) -> Result<Response<prost_types::Struct>, Status> {
    self.0.ensure_serving().await?;
    let uri = request.into_inner().uri;
    let (product, age_ms) = self.0.resources.read(&uri)?;
    Ok(with_sample_age(product, Some(age_ms)))
  }
  type WatchResourceStream = ResourceWatchStream;
  async fn watch_resource(
    &self,
    request: Request<v1::ResourceRequest>,
  ) -> Result<Response<Self::WatchResourceStream>, Status> {
    self.0.ensure_serving().await?;
    let uri = request.into_inner().uri;
    let mut subscription = self.0.resources.subscribe(&uri)?;
    let previous = self.0.resource_watchers.fetch_add(1, Ordering::AcqRel);
    if previous >= 16 {
      self.0.resource_watchers.fetch_sub(1, Ordering::AcqRel);
      return Err(Status::resource_exhausted("resource watch capacity"));
    }
    let (send, receive) = tokio::sync::mpsc::channel(2);
    let lagged = Arc::new(AtomicBool::new(false));
    let lagged_producer = lagged.clone();
    let watchers = self.0.resource_watchers.clone();
    let quiescing = self.0.quiescing.clone();
    tokio::spawn(async move {
      loop {
        tokio::select! {
          _ = tokio::time::sleep(std::time::Duration::from_millis(100)) => {
            if quiescing.load(Ordering::Acquire) { break; }
          }
          _ = send.closed() => break,
          event = subscription.recv() => match event {
            Ok(()) => {
              if send.try_send(Ok(v1::ResourceChanged { uri: uri.clone(), resource: None })).is_err() {
                lagged_producer.store(true, Ordering::Release);
                break;
              }
            }
            Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => {
              lagged_producer.store(true, Ordering::Release);
              break;
            }
            Err(tokio::sync::broadcast::error::RecvError::Closed) => break,
          }
        }
      }
      watchers.fetch_sub(1, Ordering::AcqRel);
    });
    Ok(Response::new(ResourceWatchStream {
      updates: tokio_stream::wrappers::ReceiverStream::new(receive),
      lagged,
      ended: false,
    }))
  }
  async fn capture_frames(
    &self,
    request: Request<v1::CaptureFramesRequest>,
  ) -> Result<Response<v1::CaptureFramesResponse>, Status> {
    self.0.ensure_serving().await?;
    let budget = request_budget(&request)?;
    let started = std::time::Instant::now();
    let request = request.into_inner();
    if !(1..=2).contains(&request.frame_count) {
      return Err(Status::invalid_argument("frame_count must be one or two"));
    }
    let robot = self.0.discover.robot.as_ref().expect("admitted robot");
    if request.robot_name != robot.name {
      return Err(Status::not_found("unknown robot"));
    }
    let path = self
      .0
      .cameras
      .get(&request.device_id)
      .ok_or_else(|| Status::not_found("unknown camera"))?
      .clone();
    let (healthy, ready) = self.0.health.read(&path);
    if !healthy || !ready {
      return Err(Status::unavailable("camera unavailable"));
    }
    let frames = self
      .0
      .camera_cache
      .snapshot(&path, request.frame_count as usize)
      .ok_or_else(|| Status::unavailable("camera frame cache is cold or stale"))?;
    let oldest_frame = frames.iter().map(|entry| entry.received).min();
    let permit = self
      .0
      .frame_encodes
      .clone()
      .try_acquire_owned()
      .map_err(|_| Status::resource_exhausted("camera encode capacity"))?;
    let device_id = request.device_id;
    #[cfg(test)]
    let encode_test_gate = self.0.encode_test_gate.clone();
    let task = tokio::task::spawn_blocking(move || {
      let _permit = permit;
      #[cfg(test)]
      if let Some(gate) = encode_test_gate {
        gate.0.wait();
        gate.1.wait();
      }
      let mut encoded = Vec::new();
      let mut total = 0usize;
      for entry in frames {
        if entry
          .frame
          .preview_jpeg
          .as_ref()
          .is_some_and(|jpeg| jpeg.bytes.len() > 8 * 1024 * 1024)
        {
          return Err(Status::resource_exhausted("camera JPEG exceeds 8 MiB"));
        }
        let frame =
          VisionFrame::encode_jpeg(&path, &entry.frame, 1.0, entry.frame.width_px.min(4096), 85)
            .map_err(|error| match error {
              aether_hal::HalError::InvalidData(ref reason)
                if reason.contains("bound") || reason.contains("exceeds") =>
              {
                Status::resource_exhausted("camera JPEG exceeds bound")
              }
              _ => Status::unavailable("camera JPEG unavailable"),
            })?;
        if frame.jpeg.len() > 8 * 1024 * 1024 {
          return Err(Status::resource_exhausted("camera JPEG exceeds 8 MiB"));
        }
        total = total.saturating_add(frame.jpeg.len());
        if total > 16 * 1024 * 1024 {
          return Err(Status::resource_exhausted("camera response exceeds 16 MiB"));
        }
        encoded.push(v1::CameraFrame {
          jpeg: frame.jpeg,
          width_px: frame.width_px,
          height_px: frame.height_px,
          captured_at: entry.captured_at,
        });
      }
      let response = v1::CaptureFramesResponse {
        r#type: "vision.snapshot".into(),
        schema_version: "1.0".into(),
        device_id,
        frames: encoded,
      };
      if response.encoded_len() > 16 * 1024 * 1024 {
        return Err(Status::resource_exhausted("camera response exceeds 16 MiB"));
      }
      Ok(response)
    });
    let response = tokio::time::timeout(budget.saturating_sub(started.elapsed()), task)
      .await
      .map_err(|_| Status::deadline_exceeded("camera encode deadline"))?
      .map_err(|_| Status::internal("camera encode worker failed"))?;
    self.0.ensure_serving().await?;
    let response = response?;
    let age = oldest_frame
      .map(|received| received.elapsed())
      .ok_or_else(|| Status::unavailable("camera frame cache is cold"))?;
    if age > std::time::Duration::from_secs(1) {
      return Err(Status::unavailable("camera frame cache is stale"));
    }
    Ok(with_sample_age(response, Some(age.as_millis() as u64)))
  }
  async fn get_model_status(
    &self,
    _: Request<v1::ModelStatusRequest>,
  ) -> Result<Response<v1::ModelStatus>, Status> {
    Err(Status::unimplemented("model status"))
  }
  type WatchModelStatusStream = tokio_stream::Empty<Result<v1::ModelStatus, Status>>;
  async fn watch_model_status(
    &self,
    _: Request<v1::ModelStatusRequest>,
  ) -> Result<Response<Self::WatchModelStatusStream>, Status> {
    Err(Status::unimplemented("model status"))
  }
  async fn list_navigation_points(
    &self,
    _: Request<v1::ListNavigationPointsRequest>,
  ) -> Result<Response<v1::ListNavigationPointsResponse>, Status> {
    Err(Status::unimplemented("navigation points"))
  }
  async fn get_navigation_point(
    &self,
    _: Request<v1::GetNavigationPointRequest>,
  ) -> Result<Response<v1::NavigationPoint>, Status> {
    Err(Status::unimplemented("navigation points"))
  }
  async fn put_navigation_point(
    &self,
    _: Request<v1::PutNavigationPointRequest>,
  ) -> Result<Response<v1::NavigationPoint>, Status> {
    Err(Status::unimplemented("navigation points"))
  }
  async fn delete_navigation_point(
    &self,
    _: Request<v1::GetNavigationPointRequest>,
  ) -> Result<Response<()>, Status> {
    Err(Status::unimplemented("navigation points"))
  }
  async fn reload_navigation_points(
    &self,
    _: Request<()>,
  ) -> Result<Response<v1::ReloadNavigationPointsResponse>, Status> {
    Err(Status::unimplemented("navigation points"))
  }
  async fn query_resource_history(
    &self,
    _: Request<v1::QueryResourceHistoryRequest>,
  ) -> Result<Response<v1::QueryResourceHistoryResponse>, Status> {
    Err(Status::unimplemented("resource history"))
  }
  async fn observe_objects(
    &self,
    _: Request<v1::ObserveObjectsRequest>,
  ) -> Result<Response<v1::ObserveObjectsResponse>, Status> {
    Err(Status::unimplemented("objects"))
  }
  async fn infer_vad(
    &self,
    _: Request<v1::InferVadRequest>,
  ) -> Result<Response<v1::InferVadResponse>, Status> {
    Err(Status::unimplemented("vad"))
  }
  async fn detect_humans(
    &self,
    _: Request<v1::DetectHumansRequest>,
  ) -> Result<Response<v1::DetectHumansResponse>, Status> {
    Err(Status::unimplemented("humans"))
  }
  async fn estimate_human_pose(
    &self,
    _: Request<v1::EstimateHumanPoseRequest>,
  ) -> Result<Response<v1::EstimateHumanPoseResponse>, Status> {
    Err(Status::unimplemented("pose"))
  }
  async fn detect_faces(
    &self,
    _: Request<v1::DetectFacesRequest>,
  ) -> Result<Response<v1::DetectFacesResponse>, Status> {
    Err(Status::unimplemented("faces"))
  }
  async fn get_perception_roles(
    &self,
    _: Request<()>,
  ) -> Result<Response<v1::PerceptionRolesResponse>, Status> {
    Err(Status::unimplemented("roles"))
  }
  type WatchMonitorStream = tokio_stream::Empty<Result<v1::MonitorEvent, Status>>;
  async fn watch_monitor(
    &self,
    _: Request<v1::MonitorRequest>,
  ) -> Result<Response<Self::WatchMonitorStream>, Status> {
    Err(Status::unimplemented("monitor"))
  }
}

#[derive(Clone)]
pub struct Capabilities {
  projection: Projection,
  watchers: Arc<AtomicUsize>,
  movement_submissions: Arc<tokio::sync::Semaphore>,
  controlled_stop_submissions: Arc<tokio::sync::Semaphore>,
  emergency_submissions: Arc<tokio::sync::Semaphore>,
}

impl Capabilities {
  pub fn new(projection: Projection) -> Self {
    Self {
      projection,
      watchers: Arc::new(AtomicUsize::new(0)),
      movement_submissions: Arc::new(tokio::sync::Semaphore::new(16)),
      controlled_stop_submissions: Arc::new(tokio::sync::Semaphore::new(1)),
      emergency_submissions: Arc::new(tokio::sync::Semaphore::new(1)),
    }
  }

  async fn submit_legacy(
    &self,
    request: Request<v2::SubmitOperationRequest>,
  ) -> Result<Response<v2::Operation>, Status> {
    let budget = request_budget(&request)?;
    let started = std::time::Instant::now();
    self.projection.ensure_serving_with(budget).await?;
    let deadline_monotonic_ns = SystemClock
      .monotonic_ns()
      .saturating_add(budget.as_nanos().min(u64::MAX as u128) as u64);
    let request = request.into_inner();
    if request.operation_id.is_empty() || request.operation_id.len() > 128 {
      return Err(Status::invalid_argument("invalid operation ID"));
    }
    let arguments = request
      .arguments
      .ok_or_else(|| Status::invalid_argument("arguments object required"))?;
    let command = if let Some(binding) = self.projection.bindings.get(&request.capability_name) {
      LegacyOperation::Movement(
        CommandRequest {
          node_path: binding.node_path.clone(),
          meta: CommandMeta {
            command_id: request.operation_id.clone(),
            deadline_monotonic_ns,
          },
          movement: Movement::Base(parse_velocity(&arguments, binding)?),
        },
        "base.velocity",
      )
    } else if let Some(binding) = self
      .projection
      .converted_bindings
      .get(&request.capability_name)
    {
      LegacyOperation::Movement(
        CommandRequest {
          node_path: binding.node_path.clone(),
          meta: CommandMeta {
            command_id: request.operation_id.clone(),
            deadline_monotonic_ns,
          },
          movement: parse_converted(&arguments, binding)?,
        },
        binding.primitive,
      )
    } else if let Some(binding) = self.projection.stop_bindings.get(&request.capability_name) {
      if !arguments.fields.is_empty() {
        return Err(Status::invalid_argument("stop arguments must be empty"));
      }
      LegacyOperation::Stop(StopRequest {
        command_id: request.operation_id.clone(),
        deadline_monotonic_ns,
        kind: binding.kind.clone(),
        authorized_recovery: false,
      })
    } else {
      return Err(Status::not_found("unknown executable capability"));
    };
    if started.elapsed() >= budget {
      return Err(Status::deadline_exceeded("caller deadline expired"));
    }
    let lane = match &command {
      LegacyOperation::Movement(..) => &self.movement_submissions,
      LegacyOperation::Stop(StopRequest {
        kind: StopKind::SafetyEstop { .. },
        ..
      }) => &self.emergency_submissions,
      LegacyOperation::Stop(..) => &self.controlled_stop_submissions,
    };
    let permit = lane
      .clone()
      .try_acquire_owned()
      .map_err(|_| Status::resource_exhausted("operation submission capacity"))?;
    self
      .projection
      .ensure_serving_with(budget.saturating_sub(started.elapsed()))
      .await?;
    let runtime = self.projection.runtime.clone();
    let id = request.operation_id;
    let task = tokio::task::spawn_blocking(move || {
      let _permit = permit;
      match command {
        LegacyOperation::Movement(command, primitive) => StopDispatchOutcome::Operation(
          runtime
            .dispatcher()
            .dispatch_gateway_movement(command, primitive),
        ),
        LegacyOperation::Stop(command) => {
          runtime.dispatcher().dispatch_gateway_stop_checked(command)
        }
      }
    });
    let outcome = tokio::time::timeout(budget.saturating_sub(started.elapsed()), task)
      .await
      .map_err(|_| Status::deadline_exceeded("operation still in progress"))?
      .map_err(|_| Status::internal("operation worker failed"))?;
    let result = checked_stop_result(outcome)?;
    if result.reason.as_deref() == Some("conflicting command ID reuse") {
      return Err(Status::already_exists("conflicting operation ID reuse"));
    }
    if result.status == OutcomeStatus::Rejected
      && self
        .projection
        .runtime
        .dispatcher()
        .gateway_operation(&id)
        .is_ok_and(|entry| entry.is_none())
    {
      self
        .projection
        .ensure_serving_with(budget.saturating_sub(started.elapsed()))
        .await?;
      return Err(Status::internal("operation reservation was not persisted"));
    }
    match self
      .projection
      .runtime
      .dispatcher()
      .gateway_operation(&id)
      .map_err(legacy_lookup_status)?
    {
      Some(info) => Ok(Response::new(operation(info))),
      None => Err(Status::not_found("unknown operation")),
    }
  }
}

#[tonic::async_trait]
impl v2::capability_service_server::CapabilityService for Capabilities {
  async fn list_capabilities(
    &self,
    _: Request<v2::ListCapabilitiesRequest>,
  ) -> Result<Response<v2::ListCapabilitiesResponse>, Status> {
    self.projection.ensure_serving().await?;
    let view = self.projection.health.view();
    Ok(self.projection.generation_metadata(with_sample_age(
      self.projection.capabilities(&view),
      view.max_age_ms(),
    )))
  }
  async fn submit_operation(
    &self,
    request: Request<v2::SubmitOperationRequest>,
  ) -> Result<Response<v2::Operation>, Status> {
    if self.projection.legacy_unmanaged_writes {
      return self.submit_legacy(request).await;
    }
    let budget = request_budget(&request)?;
    let started = std::time::Instant::now();
    self.projection.ensure_serving_with(budget).await?;
    let role = self.projection.authenticated_role(&request)?;
    let managed = self
      .projection
      .managed
      .as_ref()
      .expect("authenticated managed generation");
    let emergency = self
      .projection
      .stop_bindings
      .get(&request.get_ref().capability_name)
      .is_some_and(|binding| matches!(binding.kind, StopKind::SafetyEstop { .. }));
    let revision = self.projection.expected_revision(&request, emergency)?;
    let recovery = match request.metadata().get("x-eos-stop-recovery") {
      None => false,
      Some(value) if value.to_str().ok() == Some("true") => true,
      _ => return Err(Status::permission_denied("invalid stop recovery marker")),
    };
    let context = |binding_path: String, primitive: &str| AdmissionContext {
      run_id: managed.run_id.clone(),
      active_revision: revision.clone(),
      authority_role: role,
      ingress: AdmissionIngress::Grpc,
      binding_path,
      primitive: primitive.into(),
      primitive_version: "1.0.0".into(),
    };
    let deadline_monotonic_ns = SystemClock
      .monotonic_ns()
      .saturating_add(budget.as_nanos().min(u64::MAX as u128) as u64);
    let request = request.into_inner();
    if request.operation_id.is_empty() || request.operation_id.len() > 128 {
      return Err(Status::invalid_argument("invalid operation ID"));
    }
    let arguments = request
      .arguments
      .ok_or_else(|| Status::invalid_argument("arguments object required"))?;
    let command = if let Some(binding) = self.projection.bindings.get(&request.capability_name) {
      if role != AuthorityRole::EgiaControl || recovery {
        return Err(Status::permission_denied(
          "movement role or recovery marker denied",
        ));
      }
      let binding_path = binding
        .binding_path
        .clone()
        .ok_or_else(|| Status::failed_precondition("no active primitive binding"))?;
      let velocity = parse_velocity(&arguments, binding)?;
      AdmittedOperation::Movement(
        context(binding_path, "base.velocity"),
        CommandRequest {
          node_path: binding.node_path.clone(),
          meta: CommandMeta {
            command_id: request.operation_id.clone(),
            deadline_monotonic_ns,
          },
          movement: Movement::Base(velocity),
        },
      )
    } else if let Some(binding) = self
      .projection
      .converted_bindings
      .get(&request.capability_name)
    {
      if role != AuthorityRole::EgiaControl || recovery {
        return Err(Status::permission_denied(
          "movement role or recovery marker denied",
        ));
      }
      let binding_path = binding
        .binding_path
        .clone()
        .ok_or_else(|| Status::failed_precondition("no active primitive binding"))?;
      let movement = parse_converted(&arguments, binding)?;
      AdmittedOperation::Movement(
        context(binding_path, binding.primitive),
        CommandRequest {
          node_path: binding.node_path.clone(),
          meta: CommandMeta {
            command_id: request.operation_id.clone(),
            deadline_monotonic_ns,
          },
          movement,
        },
      )
    } else if let Some(binding) = self.projection.stop_bindings.get(&request.capability_name) {
      if !arguments.fields.is_empty() {
        return Err(Status::invalid_argument("stop arguments must be empty"));
      }
      if recovery
        && (role != AuthorityRole::OperatorControl
          || !matches!(binding.kind, StopKind::RobotStop { .. }))
      {
        return Err(Status::permission_denied(
          "stop recovery role or kind denied",
        ));
      }
      let binding_path = match &binding.kind {
        StopKind::MotionStop { node_path } => node_path.clone(),
        _ => String::new(),
      };
      AdmittedOperation::Stop(
        context(binding_path, binding.primitive),
        StopRequest {
          command_id: request.operation_id.clone(),
          deadline_monotonic_ns,
          kind: binding.kind.clone(),
          authorized_recovery: recovery,
        },
      )
    } else if self
      .projection
      .unavailable_mujoco_motion_stops
      .contains(&request.capability_name)
    {
      if !arguments.fields.is_empty() {
        return Err(Status::invalid_argument("stop arguments must be empty"));
      }
      return Err(Status::failed_precondition(
        "terminal_device_stop_unavailable",
      ));
    } else {
      return Err(Status::not_found("unknown executable capability"));
    };
    if started.elapsed() >= budget {
      return Err(Status::deadline_exceeded("caller deadline expired"));
    }
    let lane = match &command {
      AdmittedOperation::Movement(..) => &self.movement_submissions,
      AdmittedOperation::Stop(
        _,
        StopRequest {
          kind: StopKind::SafetyEstop { .. },
          ..
        },
      ) => &self.emergency_submissions,
      AdmittedOperation::Stop(..) => &self.controlled_stop_submissions,
    };
    let permit = lane
      .clone()
      .try_acquire_owned()
      .map_err(|_| Status::resource_exhausted("operation submission capacity"))?;
    self
      .projection
      .ensure_serving_with(budget.saturating_sub(started.elapsed()))
      .await?;
    let runtime = self.projection.runtime.clone();
    let id = request.operation_id;
    let task = tokio::task::spawn_blocking(move || {
      let _permit = permit;
      match command {
        AdmittedOperation::Movement(admission, command) => StopDispatchOutcome::Operation(
          runtime
            .dispatcher()
            .dispatch_managed_movement(admission, command),
        ),
        AdmittedOperation::Stop(admission, command) => runtime
          .dispatcher()
          .dispatch_managed_stop_checked(admission, command),
      }
    });
    let outcome = tokio::time::timeout(budget.saturating_sub(started.elapsed()), task)
      .await
      .map_err(|_| Status::deadline_exceeded("operation still in progress"))?
      .map_err(|_| Status::internal("operation worker failed"))?;
    let result = checked_stop_result(outcome)?;
    if result.reason.as_deref() == Some("conflicting command ID reuse") {
      return Err(Status::already_exists("conflicting operation ID reuse"));
    }
    if result.status == OutcomeStatus::Rejected
      && self
        .projection
        .runtime
        .dispatcher()
        .gateway_operation_authorized(&id, &managed.run_id, role)
        .is_ok_and(|operation| operation.is_none())
    {
      // A worker handed off just before the shutdown transition may be
      // rejected by the Dispatcher after its release gate closes. Hold that
      // response until health publishes NOT_SERVING; do not report a write
      // rejection while a watcher can still observe SERVING.
      self
        .projection
        .ensure_serving_with(budget.saturating_sub(started.elapsed()))
        .await?;
      return Err(Status::internal("operation reservation was not persisted"));
    }
    Ok(Response::new(operation(
      self.projection.authorized_operation(&id, role)?,
    )))
  }
  async fn get_operation(
    &self,
    request: Request<v2::OperationRequest>,
  ) -> Result<Response<v2::Operation>, Status> {
    self.projection.ensure_serving().await?;
    let role = if self.projection.legacy_unmanaged_writes {
      None
    } else {
      Some(self.projection.authenticated_role(&request)?)
    };
    let id = request.into_inner().operation_id;
    if id.is_empty() || id.len() > 128 {
      return Err(Status::invalid_argument("invalid operation ID"));
    }
    let info = match role {
      Some(role) => self.projection.authorized_operation(&id, role)?,
      None => self
        .projection
        .runtime
        .dispatcher()
        .gateway_operation(&id)
        .map_err(legacy_lookup_status)?
        .ok_or_else(|| Status::not_found("unknown operation"))?,
    };
    Ok(Response::new(operation(info)))
  }
  type WatchOperationStream = tokio_stream::wrappers::ReceiverStream<Result<v2::Operation, Status>>;
  async fn watch_operation(
    &self,
    request: Request<v2::OperationRequest>,
  ) -> Result<Response<Self::WatchOperationStream>, Status> {
    self.projection.ensure_serving().await?;
    let budget = request_budget(&request)?;
    let role = if self.projection.legacy_unmanaged_writes {
      None
    } else {
      Some(self.projection.authenticated_role(&request)?)
    };
    let id = request.into_inner().operation_id;
    if id.is_empty() || id.len() > 128 {
      return Err(Status::invalid_argument("invalid operation ID"));
    }
    let initial = match role {
      Some(role) => self.projection.authorized_operation(&id, role)?,
      None => self
        .projection
        .runtime
        .dispatcher()
        .gateway_operation(&id)
        .map_err(legacy_lookup_status)?
        .ok_or_else(|| Status::not_found("unknown operation"))?,
    };
    let previous = self.watchers.fetch_add(1, Ordering::AcqRel);
    if previous >= 16 {
      self.watchers.fetch_sub(1, Ordering::AcqRel);
      return Err(Status::resource_exhausted("operation watch capacity"));
    }
    let (send, receive) = tokio::sync::mpsc::channel(2);
    let runtime = self.projection.runtime.clone();
    let run_id = self
      .projection
      .managed
      .as_ref()
      .map(|managed| managed.run_id.clone());
    let watchers = self.watchers.clone();
    let quiescing = self.projection.quiescing.clone();
    tokio::spawn(async move {
      let _ = send.send(Ok(operation(initial.clone()))).await;
      if initial.status == OutcomeStatus::InProgress {
        let deadline = tokio::time::Instant::now() + budget;
        loop {
          if send.is_closed() {
            break;
          }
          if quiescing.load(Ordering::Acquire) {
            let _ = send.try_send(Err(Status::unavailable("runtime is quiescing")));
            break;
          }
          if tokio::time::Instant::now() >= deadline {
            let _ = send.try_send(Err(Status::deadline_exceeded("operation watch deadline")));
            break;
          }
          tokio::time::sleep(std::time::Duration::from_millis(20)).await;
          let found = match (&run_id, role) {
            (Some(run_id), Some(role)) => runtime
              .dispatcher()
              .gateway_operation_authorized(&id, run_id, role),
            _ => runtime.dispatcher().gateway_operation(&id),
          };
          match found {
            Ok(Some(current)) if current.status != OutcomeStatus::InProgress => {
              let _ = send.send(Ok(operation(current))).await;
              break;
            }
            Err(error) => {
              let status = if role.is_some() {
                lookup_status(error)
              } else {
                legacy_lookup_status(error)
              };
              let _ = send.send(Err(status)).await;
              break;
            }
            _ => {}
          }
        }
      }
      watchers.fetch_sub(1, Ordering::AcqRel);
    });
    Ok(Response::new(tokio_stream::wrappers::ReceiverStream::new(
      receive,
    )))
  }
  async fn cancel_operation(
    &self,
    request: Request<v2::OperationRequest>,
  ) -> Result<Response<v2::Operation>, Status> {
    let budget = request_budget(&request)?;
    self.projection.ensure_serving_with(budget).await?;
    let role = if self.projection.legacy_unmanaged_writes {
      None
    } else {
      Some(self.projection.authenticated_role(&request)?)
    };
    let id = request.into_inner().operation_id;
    if id.is_empty() || id.len() > 128 {
      return Err(Status::invalid_argument("invalid operation ID"));
    }
    let info = match role {
      Some(role) => self.projection.authorized_operation(&id, role)?,
      None => self
        .projection
        .runtime
        .dispatcher()
        .gateway_operation(&id)
        .map_err(legacy_lookup_status)?
        .ok_or_else(|| Status::not_found("unknown operation"))?,
    };
    if info.status != OutcomeStatus::InProgress {
      return Ok(Response::new(operation(info)));
    }
    if info.kind != GatewayOperationKind::Movement {
      return Err(Status::failed_precondition(
        "stop operation is not cancellable",
      ));
    }
    match role {
      Some(role) => {
        let run_id = &self
          .projection
          .managed
          .as_ref()
          .expect("authenticated managed generation")
          .run_id;
        self
          .projection
          .runtime
          .dispatcher()
          .cancel_authorized(&id, run_id, role)
          .map_err(lookup_status)?;
        Ok(Response::new(operation(
          self.projection.authorized_operation(&id, role)?,
        )))
      }
      None => {
        self
          .projection
          .runtime
          .dispatcher()
          .cancel(&id)
          .map_err(|_| Status::failed_precondition("operation cannot be cancelled"))?;
        let info = self
          .projection
          .runtime
          .dispatcher()
          .gateway_operation(&id)
          .map_err(legacy_lookup_status)?
          .ok_or_else(|| Status::not_found("unknown operation"))?;
        Ok(Response::new(operation(info)))
      }
    }
  }
}

#[derive(Clone)]
pub struct Skills(Projection);

impl Skills {
  fn manager(&self) -> Result<&Arc<HostSkillManager>, Status> {
    self
      .0
      .host_skills
      .as_ref()
      .ok_or_else(|| Status::unavailable("Rust skill catalog is unavailable"))
  }

  fn authorize<T>(&self, request: &Request<T>, write: bool) -> Result<(), Status> {
    let role = self.0.authenticated_role(request)?;
    if role != AuthorityRole::EgiaControl {
      return Err(if write {
        Status::permission_denied("EGIA control grant required")
      } else {
        Status::not_found("execution not found")
      });
    }
    if write {
      self.0.expected_revision(request, false)?;
    }
    Ok(())
  }
}

#[tonic::async_trait]
impl v1::skill_service_server::SkillService for Skills {
  async fn list_skills(
    &self,
    _: Request<v1::ListSkillsRequest>,
  ) -> Result<Response<v1::ListSkillsResponse>, Status> {
    self.0.ensure_serving().await?;
    Ok(
      self
        .0
        .generation_metadata(Response::new(self.manager()?.catalog()?)),
    )
  }
  async fn get_skill(
    &self,
    request: Request<v1::GetSkillRequest>,
  ) -> Result<Response<v1::SkillDescriptor>, Status> {
    self.0.ensure_serving().await?;
    let reference = request
      .into_inner()
      .reference
      .ok_or_else(|| Status::invalid_argument("skill reference required"))?;
    Ok(Response::new(
      self
        .manager()?
        .get_skill(&reference.name, &reference.version)?,
    ))
  }
  async fn start_execution(
    &self,
    request: Request<v1::StartExecutionRequest>,
  ) -> Result<Response<v1::SkillExecution>, Status> {
    self.0.ensure_serving().await?;
    self.authorize(&request, true)?;
    Ok(Response::new(self.manager()?.start(request.into_inner())?))
  }
  async fn get_execution(
    &self,
    request: Request<v1::GetExecutionRequest>,
  ) -> Result<Response<v1::SkillExecution>, Status> {
    self.0.ensure_serving().await?;
    self.authorize(&request, false)?;
    Ok(Response::new(
      self.manager()?.get(&request.into_inner().execution_id)?,
    ))
  }
  type WatchExecutionStream = skill_service::SkillWatchStream;
  async fn watch_execution(
    &self,
    request: Request<v1::WatchExecutionRequest>,
  ) -> Result<Response<Self::WatchExecutionStream>, Status> {
    self.0.ensure_serving().await?;
    self.authorize(&request, false)?;
    let request = request.into_inner();
    Ok(Response::new(
      self
        .manager()?
        .watch(&request.execution_id, request.after_sequence)?,
    ))
  }
  async fn get_execution_state(
    &self,
    _: Request<v1::GetExecutionStateRequest>,
  ) -> Result<Response<v1::SkillExecutionState>, Status> {
    Err(Status::unimplemented("skills"))
  }
  type WatchExecutionStateStream = tokio_stream::Empty<Result<v1::SkillExecutionState, Status>>;
  async fn watch_execution_state(
    &self,
    _: Request<v1::WatchExecutionStateRequest>,
  ) -> Result<Response<Self::WatchExecutionStateStream>, Status> {
    Err(Status::unimplemented("skills"))
  }
  async fn terminate_execution(
    &self,
    request: Request<v1::TerminateExecutionRequest>,
  ) -> Result<Response<v1::SkillExecution>, Status> {
    self.0.ensure_serving().await?;
    self.authorize(&request, true)?;
    Ok(Response::new(
      self
        .manager()?
        .terminate(&request.into_inner().execution_id)?,
    ))
  }
  async fn terminate_executions(
    &self,
    _: Request<v1::TerminateExecutionsRequest>,
  ) -> Result<Response<v1::TerminateExecutionsResponse>, Status> {
    Err(Status::unimplemented("skills"))
  }
  async fn list_executions(
    &self,
    request: Request<v1::ListExecutionsRequest>,
  ) -> Result<Response<v1::ListExecutionsResponse>, Status> {
    self.0.ensure_serving().await?;
    self.authorize(&request, false)?;
    Ok(Response::new(self.manager()?.list()?))
  }
  async fn signal_execution(
    &self,
    _: Request<v1::SignalExecutionRequest>,
  ) -> Result<Response<v1::SkillSignalReceipt>, Status> {
    Err(Status::unimplemented("skills"))
  }
  type FollowExecutionJournalStream = tokio_stream::Empty<Result<v1::SkillJournalEntry, Status>>;
  async fn follow_execution_journal(
    &self,
    _: Request<v1::FollowExecutionJournalRequest>,
  ) -> Result<Response<Self::FollowExecutionJournalStream>, Status> {
    Err(Status::unimplemented("skills"))
  }
  type MonitorExecutionsStream = tokio_stream::Empty<Result<v1::SkillMonitorUpdate, Status>>;
  async fn monitor_executions(
    &self,
    _: Request<v1::MonitorExecutionsRequest>,
  ) -> Result<Response<Self::MonitorExecutionsStream>, Status> {
    Err(Status::unimplemented("skills"))
  }
}

/// Owns a loopback listener and its shutdown signal. The socket binds before
/// the owner publishes readiness, so a collision cannot produce a ready event.
pub struct ServerHandle {
  address: SocketAddr,
  stop: Option<tokio::sync::oneshot::Sender<()>>,
  shutdown_reported: Option<std::sync::mpsc::Receiver<()>>,
  thread: Option<JoinHandle<()>>,
  quiescing: Arc<AtomicBool>,
  lifecycle: Arc<tokio::sync::watch::Sender<ServingPhase>>,
  health: Arc<HealthSnapshots>,
}

async fn publish_not_serving(
  projection: &Projection,
  publish_health: impl std::future::Future<Output = ()>,
) {
  // During the reporter's async publication, new calls wait instead of
  // seeing a serving health state with a closed Dispatcher release gate.
  projection
    .lifecycle
    .send_replace(ServingPhase::Transitioning);
  projection.health.stop();
  projection.runtime.dispatcher().quiesce();
  publish_health.await;
  projection.quiescing.store(true, Ordering::Release);
  projection.lifecycle.send_replace(ServingPhase::NotServing);
}

impl ServerHandle {
  pub fn start(projection: Projection, port: u16) -> Result<Self, String> {
    let quiescing = projection.quiescing.clone();
    let lifecycle = projection.lifecycle.clone();
    let health_snapshots = projection.health.clone();
    let listener = TcpListener::bind(SocketAddr::new(IpAddr::V4(Ipv4Addr::LOCALHOST), port))
      .map_err(|e| format!("gRPC listener bind: {e}"))?;
    let address = listener.local_addr().map_err(|e| e.to_string())?;
    listener.set_nonblocking(true).map_err(|e| e.to_string())?;
    let (stop, stopped) = tokio::sync::oneshot::channel();
    let (started, ready) = std::sync::mpsc::sync_channel(1);
    let (shutdown_ready, shutdown_reported) = std::sync::mpsc::sync_channel(1);
    let shutdown_projection = projection.clone();
    let thread = std::thread::spawn(move || {
      let executor = match tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()
      {
        Ok(executor) => executor,
        Err(error) => {
          let _ = started.send(Err(error.to_string()));
          return;
        }
      };
      executor.block_on(async move {
        let listener = match tokio::net::TcpListener::from_std(listener) {
          Ok(listener) => listener,
          Err(error) => {
            let _ = started.send(Err(error.to_string()));
            return;
          }
        };
        let (reporter, health) = tonic_health::server::health_reporter();
        reporter
          .set_service_status("", tonic_health::ServingStatus::Serving)
          .await;
        reporter
          .set_service_status(
            "island.eos.v1.GatewayService",
            tonic_health::ServingStatus::Serving,
          )
          .await;
        reporter
          .set_service_status(
            "island.eos.v2.CapabilityService",
            tonic_health::ServingStatus::Serving,
          )
          .await;
        reporter
          .set_service_status(
            "island.eos.v1.SkillService",
            if projection.host_skills.is_some() {
              tonic_health::ServingStatus::Serving
            } else {
              tonic_health::ServingStatus::NotServing
            },
          )
          .await;
        reporter
          .set_service_status(
            "island.eos.v1.WorkflowService",
            tonic_health::ServingStatus::NotServing,
          )
          .await;
        let gateway =
          v1::gateway_service_server::GatewayServiceServer::new(Gateway(projection.clone()))
            .max_decoding_message_size(64 * 1024)
            .max_encoding_message_size(16 * 1024 * 1024);
        let capabilities = v2::capability_service_server::CapabilityServiceServer::new(
          Capabilities::new(projection.clone()),
        )
        .max_decoding_message_size(64 * 1024);
        let skills = v1::skill_service_server::SkillServiceServer::new(Skills(projection))
          .max_decoding_message_size(2 * 1024 * 1024)
          .max_encoding_message_size(4 * 1024 * 1024);
        let serving = tokio::spawn(async move {
          tonic::transport::Server::builder()
            .add_service(health)
            .add_service(gateway)
            .add_service(capabilities)
            .add_service(skills)
            .serve_with_incoming_shutdown(
              tokio_stream::wrappers::TcpListenerStream::new(listener),
              async {
                let _ = stopped.await;
                publish_not_serving(
                  &shutdown_projection,
                  reporter.set_service_status("", tonic_health::ServingStatus::NotServing),
                )
                .await;
                let _ = shutdown_ready.send(());
              },
            )
            .await
        });
        // The serve future must be polled before the owner publishes ready.
        tokio::task::yield_now().await;
        if serving.is_finished() {
          let error = match serving.await {
            Ok(Ok(())) => "gRPC listener exited before readiness".to_owned(),
            Ok(Err(error)) => error.to_string(),
            Err(error) => error.to_string(),
          };
          let _ = started.send(Err(error));
          return;
        }
        let _ = started.send(Ok(()));
        match serving.await {
          Ok(Ok(())) => {}
          Ok(Err(error)) => eprintln!("aether-runtime: gRPC listener failed: {error}"),
          Err(error) => eprintln!("aether-runtime: gRPC listener task failed: {error}"),
        }
      });
    });
    ready.recv().map_err(|e| e.to_string())??;
    Ok(Self {
      address,
      stop: Some(stop),
      shutdown_reported: Some(shutdown_reported),
      thread: Some(thread),
      quiescing,
      lifecycle,
      health: health_snapshots,
    })
  }

  pub fn address(&self) -> SocketAddr {
    self.address
  }

  pub fn shutdown(&mut self) {
    self.begin_shutdown();
    if let Some(thread) = self.thread.take() {
      let deadline = std::time::Instant::now() + std::time::Duration::from_secs(1);
      while !thread.is_finished() && std::time::Instant::now() < deadline {
        std::thread::sleep(std::time::Duration::from_millis(5));
      }
      if thread.is_finished() {
        let _ = thread.join();
      }
      // A blocked adapter call cannot delay the Dispatcher safety stop or
      // process teardown. The OS ends the detached listener at process exit.
    }
  }

  /// Stop accepting RPCs before safety shutdown; join only after Dispatcher stop.
  pub fn begin_shutdown(&mut self) {
    if let Some(stop) = self.stop.take() {
      self.health.stop();
      let _ = stop.send(());
      if let Some(reported) = self.shutdown_reported.take() {
        let _ = reported.recv_timeout(std::time::Duration::from_millis(500));
      }
      // A failed listener has no observable health reporter; direct service
      // callers still fail closed. An active reporter remains Transitioning
      // until NOT_SERVING is published, including after the bounded wait.
      if self
        .thread
        .as_ref()
        .is_none_or(|thread| thread.is_finished())
      {
        self.quiescing.store(true, Ordering::Release);
        self.lifecycle.send_replace(ServingPhase::NotServing);
      }
    }
  }
}

impl Drop for ServerHandle {
  fn drop(&mut self) {
    self.shutdown();
  }
}

#[cfg(test)]
mod wire_tests {
  use prost::Message;

  #[test]
  fn submit_waits_for_health_transition_without_releasing_adapter() {
    use std::sync::Arc;

    use aether_common::time::{Clock, SystemClock};
    use aether_dispatcher::AdapterRegistration;
    use aether_hal::{
      Profile,
      cxx_bridge::{CxxMockBase, CxxMockCamera, CxxMockManipulator},
    };
    use super::v2::capability_service_server::CapabilityService;

    let profile = Profile::from_toml(include_str!(concat!(
      env!("CARGO_MANIFEST_DIR"),
      "/../../etc/eos/backends/mock-cxx.toml"
    )))
    .unwrap();
    let clock: Arc<dyn Clock> = Arc::new(SystemClock);
    let base = Arc::new(
      CxxMockBase::from_profile(
        profile
          .node("cores/eos/layers/hardware/groups/base/nodes/drive")
          .unwrap(),
        clock.clone(),
      )
      .unwrap(),
    );
    let camera = Arc::new(
      CxxMockCamera::from_profile(
        profile
          .node("cores/eos/layers/hardware/groups/camera/nodes/front")
          .unwrap(),
        clock.clone(),
      )
      .unwrap(),
    );
    let arm = Arc::new(
      CxxMockManipulator::from_profile(
        profile
          .node("cores/eos/layers/hardware/groups/manipulator/nodes/arm")
          .unwrap(),
        clock.clone(),
      )
      .unwrap(),
    );
    let runtime = Arc::new(
      crate::Runtime::new(
        profile,
        vec![
          AdapterRegistration::Base {
            node_id: "drive".into(),
            adapter: base.clone(),
          },
          AdapterRegistration::Camera {
            node_id: "front".into(),
            adapter: camera,
          },
          AdapterRegistration::Manipulator {
            node_id: "arm".into(),
            adapter: arm,
          },
        ],
        clock,
      )
      .unwrap(),
    );
    let projection = super::Projection::new(runtime.clone()).unwrap();
    let capabilities = super::Capabilities::new(projection.clone());
    tokio::runtime::Builder::new_multi_thread()
      .worker_threads(2)
      .enable_all()
      .build()
      .unwrap()
      .block_on(async {
        let (reporter, _health) = tonic_health::server::health_reporter();
        reporter
          .set_service_status("", tonic_health::ServingStatus::Serving)
          .await;
        let (entered_send, entered_recv) = tokio::sync::oneshot::channel();
        let (release_send, release_recv) = tokio::sync::oneshot::channel::<()>();
        let transitioning = projection.clone();
        let shutdown = tokio::spawn(async move {
          super::publish_not_serving(&transitioning, async move {
            let _ = entered_send.send(());
            let _ = release_recv.await;
            reporter
              .set_service_status("", tonic_health::ServingStatus::NotServing)
              .await;
          })
          .await;
        });
        entered_recv.await.unwrap();
        let number = |value| prost_types::Value {
          kind: Some(prost_types::value::Kind::NumberValue(value)),
        };
        let request = super::v2::SubmitOperationRequest {
          capability_name: "hardware/demo-0/base/drive/base.velocity@1.0.0".into(),
          arguments: Some(prost_types::Struct {
            fields: [
              ("linear_x_m_s".into(), number(0.1)),
              ("linear_y_m_s".into(), number(0.0)),
              ("angular_z_rad_s".into(), number(0.0)),
              ("valid_for_ms".into(), number(100.0)),
            ]
            .into(),
          }),
          operation_id: "during-health-transition".into(),
        };
        let submit = tokio::spawn(async move {
          capabilities
            .submit_operation(tonic::Request::new(request))
            .await
        });
        tokio::time::sleep(std::time::Duration::from_millis(20)).await;
        assert!(
          !submit.is_finished(),
          "write rejected while health was SERVING"
        );
        assert!(!base.mock_submit_started());
        assert!(runtime
          .dispatcher()
          .gateway_operation("during-health-transition")
          .unwrap()
          .is_none());
        release_send.send(()).unwrap();
        shutdown.await.unwrap();
        assert_eq!(
          submit.await.unwrap().unwrap_err().code(),
          tonic::Code::Unavailable
        );
        assert!(!base.mock_submit_started());
        assert!(runtime
          .dispatcher()
          .gateway_operation("during-health-transition")
          .unwrap()
          .is_none());
      });
  }

  #[test]
  fn typed_pre_release_stop_failures_keep_distinct_transport_statuses() {
    use aether_dispatcher::{DispatchResult, OutcomeStatus, StopDispatchOutcome};

    let rejected = DispatchResult {
      command_id: "capacity".into(),
      status: OutcomeStatus::Rejected,
      reason: None,
    };
    assert_eq!(
      super::checked_stop_result(StopDispatchOutcome::PreReleaseCapacity(rejected.clone()))
        .unwrap_err()
        .code(),
      tonic::Code::ResourceExhausted
    );
    assert_eq!(
      super::checked_stop_result(StopDispatchOutcome::PreReleaseDurability(rejected.clone()))
        .unwrap_err()
        .code(),
      tonic::Code::Internal
    );
    assert_eq!(
      super::checked_stop_result(StopDispatchOutcome::Operation(rejected.clone())).unwrap(),
      rejected
    );
  }

  #[test]
  fn encode_completed_after_shutdown_cannot_return_a_frame() {
    use std::sync::{Arc, Barrier};
    use aether_common::time::SystemClock;
    use aether_hal::Profile;
    use crate::Runtime;
    use super::v1::gateway_service_server::GatewayService;

    let profile = Profile::from_toml(include_str!(concat!(
      env!("CARGO_MANIFEST_DIR"),
      "/../../etc/eos/backends/mock-cxx.toml"
    )))
    .unwrap();
    let robot_name = profile.robot.name.clone();
    let path = profile
      .node("cores/eos/layers/hardware/groups/camera/nodes/front")
      .unwrap()
      .path()
      .to_owned();
    let runtime = Arc::new(Runtime::from_cxx_mock_profile(profile, Arc::new(SystemClock)).unwrap());
    let frame = runtime.dispatcher().capture_camera(&path).unwrap();
    let cache = Arc::new(super::CameraCache::default());
    cache.push(&path, Arc::new(frame));
    let mut projection = super::Projection::with_camera_cache(runtime, cache).unwrap();
    let gate = Arc::new((Barrier::new(2), Barrier::new(2)));
    projection.encode_test_gate = Some(gate.clone());
    let quiescing = projection.quiescing.clone();
    let worker = std::thread::spawn(move || {
      tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap()
        .block_on(async {
          super::Gateway(projection)
            .capture_frames(tonic::Request::new(super::v1::CaptureFramesRequest {
              robot_name,
              device_id: "front".into(),
              frame_count: 1,
            }))
            .await
            .unwrap_err()
            .code()
        })
    });
    gate.0.wait();
    quiescing.store(true, std::sync::atomic::Ordering::Release);
    gate.1.wait();
    assert_eq!(worker.join().unwrap(), tonic::Code::Unavailable);
  }

  #[test]
  fn cancelled_capture_releases_bounded_encode_permit() {
    use std::sync::{Arc, Barrier};
    use aether_common::time::SystemClock;
    use aether_hal::Profile;
    use crate::Runtime;
    use super::v1::gateway_service_server::GatewayService;

    let profile = Profile::from_toml(include_str!(concat!(
      env!("CARGO_MANIFEST_DIR"),
      "/../../etc/eos/backends/mock-cxx.toml"
    )))
    .unwrap();
    let robot_name = profile.robot.name.clone();
    let path = profile
      .node("cores/eos/layers/hardware/groups/camera/nodes/front")
      .unwrap()
      .path()
      .to_owned();
    let runtime = Arc::new(Runtime::from_cxx_mock_profile(profile, Arc::new(SystemClock)).unwrap());
    let cache = Arc::new(super::CameraCache::default());
    cache.push(
      &path,
      Arc::new(runtime.dispatcher().capture_camera(&path).unwrap()),
    );
    let mut projection = super::Projection::with_camera_cache(runtime, cache).unwrap();
    let permits = projection.frame_encodes.clone();
    let gate = Arc::new((Barrier::new(2), Barrier::new(2)));
    projection.encode_test_gate = Some(gate.clone());
    let executor = tokio::runtime::Builder::new_multi_thread()
      .worker_threads(2)
      .enable_all()
      .build()
      .unwrap();
    executor.block_on(async {
      let task = tokio::spawn(async move {
        super::Gateway(projection)
          .capture_frames(tonic::Request::new(super::v1::CaptureFramesRequest {
            robot_name,
            device_id: "front".into(),
            frame_count: 1,
          }))
          .await
      });
      gate.0.wait();
      assert_eq!(permits.available_permits(), 7);
      task.abort();
      gate.1.wait();
      let deadline = tokio::time::Instant::now() + std::time::Duration::from_secs(1);
      while permits.available_permits() != 8 {
        assert!(tokio::time::Instant::now() < deadline);
        tokio::time::sleep(std::time::Duration::from_millis(10)).await;
      }
      assert!(task.await.unwrap_err().is_cancelled());
    });
  }

  #[test]
  fn caller_timeout_is_bounded_and_expired_timeout_never_dispatches() {
    let mut request = tonic::Request::new(());
    assert_eq!(
      super::request_budget(&request).unwrap(),
      std::time::Duration::from_secs(5)
    );
    request
      .metadata_mut()
      .insert("grpc-timeout", "2m".parse().unwrap());
    assert_eq!(
      super::request_budget(&request).unwrap(),
      std::time::Duration::from_millis(2)
    );
    request
      .metadata_mut()
      .insert("grpc-timeout", "9H".parse().unwrap());
    assert_eq!(
      super::request_budget(&request).unwrap(),
      std::time::Duration::from_secs(5)
    );
    request
      .metadata_mut()
      .insert("grpc-timeout", "0n".parse().unwrap());
    assert_eq!(
      super::request_budget(&request).unwrap_err().code(),
      tonic::Code::DeadlineExceeded
    );
    request
      .metadata_mut()
      .insert("grpc-timeout", "2x".parse().unwrap());
    assert_eq!(
      super::request_budget(&request).unwrap_err().code(),
      tonic::Code::InvalidArgument
    );
  }

  #[test]
  fn published_service_and_operation_tags_are_stable() {
    let set = prost_types::FileDescriptorSet::decode(super::DESCRIPTOR_SET).unwrap();
    let gateway = set
      .file
      .iter()
      .find(|f| f.name.as_deref() == Some("eos/gateway.proto"))
      .unwrap();
    assert_eq!(gateway.package.as_deref(), Some("island.eos.v1"));
    let methods: Vec<_> = gateway
      .service
      .iter()
      .find(|s| s.name.as_deref() == Some("GatewayService"))
      .unwrap()
      .method
      .iter()
      .filter_map(|m| m.name.as_deref())
      .collect();
    for method in [
      "Discover",
      "ListResources",
      "GetStatus",
      "ReadResource",
      "WatchResource",
      "CaptureFrames",
    ] {
      assert!(
        methods.contains(&method),
        "missing published method {method}"
      );
    }
    let caps = set
      .file
      .iter()
      .find(|f| f.name.as_deref() == Some("eos/capabilities.proto"))
      .unwrap();
    assert_eq!(caps.package.as_deref(), Some("island.eos.v2"));
    let operation = caps
      .message_type
      .iter()
      .find(|m| m.name.as_deref() == Some("Operation"))
      .unwrap();
    let tags: Vec<_> = operation
      .field
      .iter()
      .map(|f| (f.name.as_deref().unwrap(), f.number.unwrap()))
      .collect();
    assert!(tags.contains(&("operation_id", 1)));
    assert!(tags.contains(&("status", 2)));
    assert!(tags.contains(&("acknowledgement", 5)));
  }
}
