//! In-process EOS v2 composition boundary.
//!
//! A deployment-specific factory constructs one adapter per profile node.
//! This crate keeps profile I/O, node admission, and dispatch composition in
//! one place while the deployed C++ process is migrated in later stages.

use std::fs::{File, OpenOptions};
use std::path::Path;
use std::sync::Arc;

use aether_common::io::read_bounded;
use aether_common::time::Clock;
use aether_dispatcher::{
  AdapterRegistration, AdmissionGeneration, CommandRequest, DispatchResult, Dispatcher, Movement,
  NodeDescriptor,
};
use aether_hal::{
  cxx_bridge, BaseMovement, BaseNode, CameraNode, ManipulatorMovement, ManipulatorNode, NodeKind,
  NodeProfile, Profile,
};
use aether_mujoco::{Endpoint, MujocoBase, MujocoCamera, MujocoManipulator};
use fs2::FileExt;
use thiserror::Error;

pub mod adapter_host;
pub mod adapter_library;
pub mod grpc;
pub mod managed_admission;
pub mod host_skills;
mod grpc_health;
use adapter_host::HostedAdapter;
use adapter_library::{Libraries, LibrarySpec};

/// Maximum profile size before parsing; adapter payload limits are separate.
pub const MAX_PROFILE_BYTES: usize = 1024 * 1024;

#[derive(Debug, Error)]
pub enum RuntimeError {
  #[error("profile I/O: {0}")]
  Io(#[from] std::io::Error),
  #[error("profile is not UTF-8: {0}")]
  Utf8(#[from] std::str::Utf8Error),
  #[error("profile validation: {0}")]
  Profile(#[from] aether_hal::ProfileError),
  #[error("hardware adapter: {0}")]
  Hardware(#[from] aether_hal::HalError),
  #[error("node admission: {0}")]
  Admission(#[from] aether_dispatcher::DispatchError),
}

pub struct Runtime {
  profile: Profile,
  dispatcher: Arc<Dispatcher>,
  _host_start_lock: Option<File>,
}

impl Runtime {
  /// Admit a parsed profile only after every node has exactly one adapter.
  pub fn new(
    profile: Profile,
    adapters: Vec<AdapterRegistration>,
    clock: Arc<dyn Clock>,
  ) -> Result<Self, RuntimeError> {
    let dispatcher = Dispatcher::new(&profile, adapters, clock)?;
    Ok(Self {
      profile,
      dispatcher: Arc::new(dispatcher),
      _host_start_lock: None,
    })
  }

  /// Construct a runtime whose command IDs and outcomes survive process restarts.
  /// The journal directory is exclusive to this runtime instance.
  pub fn new_durable(
    profile: Profile,
    adapters: Vec<AdapterRegistration>,
    clock: Arc<dyn Clock>,
    journal_directory: impl AsRef<Path>,
  ) -> Result<Self, RuntimeError> {
    let dispatcher = Dispatcher::new_durable(&profile, adapters, clock, journal_directory)?;
    Ok(Self {
      profile,
      dispatcher: Arc::new(dispatcher),
      _host_start_lock: None,
    })
  }

  /// Admit an IsOS-verified saved generation before any actuator adapter can
  /// receive a command. The generation remains immutable for this process.
  pub fn new_durable_managed(
    profile: Profile,
    adapters: Vec<AdapterRegistration>,
    clock: Arc<dyn Clock>,
    journal_directory: impl AsRef<Path>,
    generation: AdmissionGeneration,
  ) -> Result<Self, RuntimeError> {
    let dispatcher =
      Dispatcher::new_durable_managed(&profile, adapters, clock, journal_directory, generation)?;
    Ok(Self {
      profile,
      dispatcher: Arc::new(dispatcher),
      _host_start_lock: None,
    })
  }

  /// Read a bounded TOML or JSON graph and construct one process-local runtime.
  pub fn from_file(
    path: impl AsRef<Path>,
    adapters: Vec<AdapterRegistration>,
    clock: Arc<dyn Clock>,
  ) -> Result<Self, RuntimeError> {
    let bytes = read_bounded(path, MAX_PROFILE_BYTES)?;
    let source = std::str::from_utf8(&bytes)?;
    let profile = if source.trim_start().starts_with('{') {
      let mut profile: Profile = serde_json::from_str(source)
        .map_err(|error| RuntimeError::Profile(aether_hal::ProfileError(error.to_string())))?;
      profile.validate()?;
      profile
    } else {
      Profile::from_toml(source)?
    };
    Self::new(profile, adapters, clock)
  }

  /// Construct supported adapters from a validated profile. Each device gets
  /// one adapter, and MuJoCo remains a separate process behind its socket.
  pub fn from_profile_adapters(
    profile: Profile,
    clock: Arc<dyn Clock>,
  ) -> Result<Self, RuntimeError> {
    let adapters = Self::profile_adapters(&profile, &clock)?;
    Self::new(profile, adapters, clock)
  }

  /// Construct supported adapters with a durable command journal.
  pub fn from_profile_adapters_durable(
    profile: Profile,
    clock: Arc<dyn Clock>,
    journal_directory: impl AsRef<Path>,
  ) -> Result<Self, RuntimeError> {
    let adapters = Self::profile_adapters(&profile, &clock)?;
    Self::new_durable(profile, adapters, clock, journal_directory)
  }

  /// Admit package-contributed adapters only from exact verified libraries.
  pub fn from_profile_adapters_durable_with_libraries(
    profile: Profile,
    clock: Arc<dyn Clock>,
    journal_directory: impl AsRef<Path>,
    library_specs: &[LibrarySpec],
  ) -> Result<Self, RuntimeError> {
    let libraries = Libraries::open(library_specs)?;
    let adapters = Self::profile_adapters_with_libraries(&profile, &clock, Some(&libraries), None)?;
    Self::new_durable(profile, adapters, clock, journal_directory)
  }

  /// The deployed path: one verified package library in one host per hardware node.
  pub fn from_profile_adapters_durable_with_hosts(
    profile: Profile,
    clock: Arc<dyn Clock>,
    journal_directory: impl AsRef<Path>,
    library_specs: &[LibrarySpec],
    host_executable: &Path,
  ) -> Result<Self, RuntimeError> {
    Self::from_profile_adapters_durable_with_hosts_and_generation(
      profile,
      clock,
      journal_directory,
      library_specs,
      host_executable,
      None,
    )
  }

  /// The managed deployed path binds the IsOS generation to the Dispatcher.
  pub fn from_profile_adapters_durable_managed_with_hosts(
    profile: Profile,
    clock: Arc<dyn Clock>,
    journal_directory: impl AsRef<Path>,
    library_specs: &[LibrarySpec],
    host_executable: &Path,
    generation: AdmissionGeneration,
  ) -> Result<Self, RuntimeError> {
    Self::from_profile_adapters_durable_with_hosts_and_generation(
      profile,
      clock,
      journal_directory,
      library_specs,
      host_executable,
      Some(generation),
    )
  }

  fn from_profile_adapters_durable_with_hosts_and_generation(
    profile: Profile,
    clock: Arc<dyn Clock>,
    journal_directory: impl AsRef<Path>,
    library_specs: &[LibrarySpec],
    host_executable: &Path,
    generation: Option<AdmissionGeneration>,
  ) -> Result<Self, RuntimeError> {
    let journal_directory = journal_directory.as_ref();
    let mut families = std::collections::BTreeSet::new();
    for spec in library_specs {
      if !families.insert(&spec.family) {
        return Err(RuntimeError::Profile(aether_hal::ProfileError(format!(
          "duplicate adapter library {}",
          spec.family
        ))));
      }
    }
    let lock = if library_specs.is_empty() {
      None
    } else {
      std::fs::create_dir_all(journal_directory)?;
      let file = OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .open(journal_directory.join(".adapter-host.lock"))?;
      file.try_lock_exclusive()?;
      Some(file)
    };
    let adapters = Self::profile_adapters_with_libraries(
      &profile,
      &clock,
      None,
      Some((library_specs, host_executable)),
    )?;
    let mut runtime = match generation {
      Some(generation) => {
        Self::new_durable_managed(profile, adapters, clock, journal_directory, generation)?
      }
      None => Self::new_durable(profile, adapters, clock, journal_directory)?,
    };
    runtime._host_start_lock = lock;
    Ok(runtime)
  }

  /// Construct the repository's CXX mock devices from a validated profile.
  /// This is a development composition path; it never opens physical hardware.
  pub fn from_cxx_mock_profile(
    profile: Profile,
    clock: Arc<dyn Clock>,
  ) -> Result<Self, RuntimeError> {
    Self::from_profile_adapters(profile, clock)
  }

  /// Build an exclusive, durable process runtime from CXX mock adapters.
  pub fn from_cxx_mock_profile_durable(
    profile: Profile,
    clock: Arc<dyn Clock>,
    journal_directory: impl AsRef<Path>,
  ) -> Result<Self, RuntimeError> {
    Self::from_profile_adapters_durable(profile, clock, journal_directory)
  }

  fn profile_adapters(
    profile: &Profile,
    clock: &Arc<dyn Clock>,
  ) -> Result<Vec<AdapterRegistration>, RuntimeError> {
    Self::profile_adapters_with_libraries(profile, clock, None, None)
  }

  fn profile_adapters_with_libraries(
    profile: &Profile,
    clock: &Arc<dyn Clock>,
    libraries: Option<&Libraries>,
    hosts: Option<(&[LibrarySpec], &Path)>,
  ) -> Result<Vec<AdapterRegistration>, RuntimeError> {
    let mut adapters = Vec::new();
    for node in profile.hardware_nodes() {
      let adapter_package = node.adapter.package.as_ref().or(node.package.as_ref());
      let compiled_mujoco = matches!(
        adapter_package.map(|p| (p.id.as_str(), p.version.as_str())),
        Some(("island/mujoco-sim", "0.1.1"))
      ) && matches!(
        (node.node_type.as_str(), node.kind),
        (
          "eos.hardware.camera@v1.0"
            | "eos.hardware.camera@v0.1"
            | "eos.hardware.mujoco_camera@v1.0",
          NodeKind::Camera
        ) | (
          "eos.hardware.base@v1.0" | "eos.hardware.base@v0.1" | "eos.hardware.mujoco_base@v1.0",
          NodeKind::Base
        ) | (
          "eos.hardware.manipulator@v1.0"
            | "eos.hardware.manipulator@v0.1"
            | "eos.hardware.mujoco_manipulator@v1.0",
          NodeKind::Manipulator
        )
      );
      let legacy_unpinned_mujoco = adapter_package.is_none()
        && matches!(
          node.node_type.as_str(),
          "eos.hardware.camera@v1.0" | "eos.hardware.base@v1.0" | "eos.hardware.manipulator@v1.0"
        )
        && profile.all_nodes().any(|backend| {
          backend.node_type == "eos.process.backend@v1.0"
            && backend.settings.get("package").and_then(|v| v.as_str()) == Some("island/mujoco-sim")
            && backend.settings.get("version").and_then(|v| v.as_str()) == Some("0.1.1")
        });
      if (node.adapter.adapter_type == "mujoco-v2" && !compiled_mujoco && !legacy_unpinned_mujoco)
        || (node.adapter.adapter_type == "mock-cxx" && adapter_package.is_some())
      {
        return Err(RuntimeError::Profile(aether_hal::ProfileError(format!(
          "{}: no compiled package adapter and stop implementation",
          node.path()
        ))));
      }
      let registration = if node.adapter.adapter_type == "mujoco-v2" {
        let endpoint = Endpoint::from_profile(node, clock.clone())?;
        match node.kind {
          NodeKind::Camera => AdapterRegistration::Camera {
            node_id: node.id.clone(),
            adapter: Arc::new(MujocoCamera(endpoint)),
          },
          NodeKind::Base => AdapterRegistration::Base {
            node_id: node.id.clone(),
            adapter: Arc::new(MujocoBase(endpoint)),
          },
          NodeKind::Manipulator => AdapterRegistration::Manipulator {
            node_id: node.id.clone(),
            adapter: Arc::new(MujocoManipulator(endpoint)),
          },
          NodeKind::Software => unreachable!("hardware_nodes contains only hardware"),
        }
      } else if node.adapter.adapter_type == "mock-cxx" {
        match node.kind {
          NodeKind::Camera => AdapterRegistration::Camera {
            node_id: node.id.clone(),
            adapter: Arc::new(cxx_bridge::CxxMockCamera::from_profile(
              node,
              clock.clone(),
            )?),
          },
          NodeKind::Base => AdapterRegistration::Base {
            node_id: node.id.clone(),
            adapter: Arc::new({
              let adapter = cxx_bridge::CxxMockBase::from_profile(node, clock.clone())?;
              #[cfg(feature = "test-hooks")]
              if let Some(delay_ms) = mock_submit_delay_ms() {
                adapter.set_mock_submit_delay_ms(delay_ms);
              }
              adapter
            }),
          },
          NodeKind::Manipulator => AdapterRegistration::Manipulator {
            node_id: node.id.clone(),
            adapter: Arc::new({
              let adapter = cxx_bridge::CxxMockManipulator::from_profile(node, clock.clone())?;
              #[cfg(feature = "test-hooks")]
              if let Some(delay_ms) = mock_submit_delay_ms() {
                adapter.set_mock_submit_delay_ms(delay_ms);
              }
              adapter
            }),
          },
          NodeKind::Software => unreachable!("hardware_nodes contains only hardware"),
        }
      } else if adapter_package.is_some() {
        if let Some((specs, executable)) = hosts {
          let spec = specs
            .iter()
            .find(|spec| spec.family == node.adapter.adapter_type)
            .ok_or_else(|| {
              RuntimeError::Profile(aether_hal::ProfileError(format!(
                "{}: no verified adapter library",
                node.path()
              )))
            })?;
          registration_from_adapter(node, HostedAdapter::start(node, spec, executable)?)
        } else {
          let library = libraries.ok_or_else(|| {
            RuntimeError::Profile(aether_hal::ProfileError(format!(
              "{}: no verified adapter library",
              node.path()
            )))
          })?;
          registration_from_adapter(node, library.create(node)?)
        }
      } else {
        return Err(RuntimeError::Profile(aether_hal::ProfileError(format!(
          "{}: unsupported adapter type {}",
          node.path(),
          node.adapter.adapter_type
        ))));
      };
      adapters.push(registration);
    }
    Ok(adapters)
  }

  pub fn profile(&self) -> &Profile {
    &self.profile
  }

  pub fn discover(&self) -> Vec<NodeDescriptor> {
    self.dispatcher.discover()
  }

  pub fn dispatch(&self, request: CommandRequest) -> DispatchResult {
    self.dispatcher.dispatch(request)
  }

  /// Route a graph-admitted manual or primitive request through the sole dispatcher.
  pub fn invoke_control(
    &self,
    control_path: &str,
    mut request: CommandRequest,
  ) -> Result<DispatchResult, String> {
    request.node_path = self.control_target_for_movement(control_path, &request.movement)?;
    Ok(self.dispatcher.dispatch(request))
  }

  pub fn control_target_for_movement(
    &self,
    control_path: &str,
    movement: &Movement,
  ) -> Result<String, String> {
    let control = self
      .profile
      .node(control_path)
      .ok_or_else(|| format!("unknown control node: {control_path}"))?;
    if matches!(
      control.node_type.as_str(),
      "eos.dispatcher.primitive_base@v1.0"
        | "eos.dispatcher.primitive_manipulator@v1.0"
        | "eos.dispatcher.primitive_base@v0.1"
        | "eos.dispatcher.primitive_manipulator@v0.1"
    ) {
      if control.settings["movement_type"].as_str() != Some(movement_type(movement)) {
        return Err("movement type differs from primitive binding".into());
      }
      let targets: Vec<_> = self
        .profile
        .hardware_nodes()
        .filter(|node| {
          node.inputs.get("command").is_some_and(|sources| {
            sources
              .values()
              .iter()
              .any(|source| source.node == control.path() && source.output == "command")
          })
        })
        .collect();
      return match targets.as_slice() {
        [target] => Ok(target.path().into()),
        _ => Err("primitive binding must target exactly one hardware node".into()),
      };
    }
    if !matches!(
      control.node_type.as_str(),
      "eos.control.manual@v1.0" | "eos.control.primitive@v1.0"
    ) {
      return Err(format!("node {control_path} cannot invoke movement"));
    }
    let target = control.settings["target"]
      .as_str()
      .expect("validated control target");
    if control.node_type == "eos.control.primitive@v1.0"
      && control.settings["movement_type"].as_str() != Some(movement_type(movement))
    {
      return Err("movement type differs from primitive definition".into());
    }
    Ok(target.into())
  }

  /// Return the exact authored primitive sequence for one admitted skill node.
  pub fn skill_steps(&self, path: &str) -> Result<Vec<String>, String> {
    let node = self
      .profile
      .node(path)
      .ok_or_else(|| format!("unknown skill node: {path}"))?;
    if node.node_type != "eos.control.skill@v1.0" {
      return Err(format!("node {path} is not a skill"));
    }
    Ok(
      node.settings["steps"]
        .as_array()
        .expect("validated skill steps")
        .iter()
        .map(|step| step.as_str().expect("validated step path").to_owned())
        .collect(),
    )
  }

  pub fn control_target(&self, path: &str) -> Result<String, String> {
    let node = self
      .profile
      .node(path)
      .ok_or_else(|| format!("unknown control node: {path}"))?;
    if matches!(
      node.node_type.as_str(),
      "eos.dispatcher.primitive_base@v1.0"
        | "eos.dispatcher.primitive_manipulator@v1.0"
        | "eos.dispatcher.primitive_base@v0.1"
        | "eos.dispatcher.primitive_manipulator@v0.1"
    ) {
      return self
        .profile
        .hardware_nodes()
        .find(|device| {
          device.inputs.get("command").is_some_and(|sources| {
            sources
              .values()
              .iter()
              .any(|source| source.node == node.path() && source.output == "command")
          })
        })
        .map(|device| device.path().to_owned())
        .ok_or_else(|| format!("binding {path} has no target"));
    }
    if node.node_type != "eos.control.primitive@v1.0" {
      return Err(format!("node {path} is not a primitive"));
    }
    Ok(
      node.settings["target"]
        .as_str()
        .expect("validated primitive target")
        .to_owned(),
    )
  }

  pub fn manual_stop(&self, path: &str) -> Result<(), String> {
    let node = self
      .profile
      .node(path)
      .ok_or_else(|| format!("unknown manual node: {path}"))?;
    if node.node_type != "eos.control.manual@v1.0" {
      return Err(format!("node {path} is not a manual control"));
    }
    self.dispatcher.stop_node(
      node.settings["target"]
        .as_str()
        .expect("validated manual target"),
    )
  }

  pub fn dispatcher(&self) -> &Dispatcher {
    &self.dispatcher
  }

  pub fn dispatcher_shared(&self) -> Arc<Dispatcher> {
    self.dispatcher.clone()
  }
}

/// Process fixture only. Production builds never read this environment key.
#[cfg(feature = "test-hooks")]
fn mock_submit_delay_ms() -> Option<u32> {
  std::env::var("EOS_TEST_MOCK_SUBMIT_DELAY_MS")
    .ok()?
    .parse::<u32>()
    .ok()
    .filter(|delay_ms| (1..=1000).contains(delay_ms))
}

fn movement_type(movement: &Movement) -> &'static str {
  match movement {
    Movement::Base(BaseMovement::BodyVelocity { .. }) => "body_velocity",
    Movement::Base(BaseMovement::RelativePose { .. }) => "relative_pose",
    Movement::Manipulator(ManipulatorMovement::JointPositions { .. }) => "joint_positions",
    Movement::Manipulator(ManipulatorMovement::EndEffectorPose { .. }) => "end_effector_pose",
  }
}

fn registration_from_adapter<A>(node: &NodeProfile, adapter: Arc<A>) -> AdapterRegistration
where
  A: CameraNode + BaseNode + ManipulatorNode + 'static,
{
  match node.kind {
    NodeKind::Camera => AdapterRegistration::Camera {
      node_id: node.id.clone(),
      adapter,
    },
    NodeKind::Base => AdapterRegistration::Base {
      node_id: node.id.clone(),
      adapter,
    },
    NodeKind::Manipulator => AdapterRegistration::Manipulator {
      node_id: node.id.clone(),
      adapter,
    },
    NodeKind::Software => unreachable!("hardware_nodes contains only hardware"),
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use aether_common::time::ManualClock;
  use aether_dispatcher::{CommandRequest, Movement, OutcomeStatus};
  use aether_hal::{BaseMovement, BaseObservation, ClockKind, CommandMeta, MockBase, SampleMeta};

  const PROFILE: &str = r#"
version = "1.0.0"
[robot]
id = "robot-1"
name = "Test robot"
[cores.eos.layers.hardware.groups.base.nodes.drive]
id = "drive"
node_type = "eos.hardware.base@v1.0"
label = "Drive"
description = "Stub base"
frame_id = "base_link"
capabilities = ["body_velocity"]
[cores.eos.layers.hardware.groups.base.nodes.drive.adapter]
type = "mock"
[cores.eos.layers.hardware.groups.base.nodes.drive.health_timing]
poll_interval_ms = 10
stale_after_ms = 100
[cores.eos.layers.hardware.groups.base.nodes.drive.observation]
max_rate_hz = 10.0
max_payload_bytes = 4096
[cores.eos.layers.hardware.groups.base.nodes.drive.movement]
modes = ["body_velocity"]
[cores.eos.layers.hardware.groups.base.nodes.drive.movement.limits]
max_linear_m_s = 1.0
max_angular_rad_s = 1.0
max_velocity_lease_ms = 500
[cores.eos.layers.dispatcher.groups.control.nodes.command]
id = "command"
node_type = "eos.dispatcher.control@v1.0"
label = "Dispatcher"
description = "Test command route"
[cores.eos.layers.hardware.groups.base.nodes.drive.inputs.command]
node = "cores/eos/layers/dispatcher/groups/control/nodes/command"
output = "base_command"
"#;

  #[test]
  fn composes_profile_node_and_dispatch_without_external_services() {
    let profile = Profile::from_toml(PROFILE).unwrap();
    let base = Arc::new(MockBase::new(BaseObservation {
      meta: SampleMeta {
        source_timestamp_ns: None,
        clock: ClockKind::Unknown,
        received_monotonic_ns: 0,
        sequence: Some(1),
        frame_id: "base_link".into(),
      },
      valid: true,
      x_m: 0.0,
      y_m: 0.0,
      yaw_rad: 0.0,
      x_m_s: 0.0,
      y_m_s: 0.0,
      yaw_rad_s: 0.0,
    }));
    let runtime = Runtime::new(
      profile,
      vec![AdapterRegistration::Base {
        node_id: "drive".into(),
        adapter: base.clone(),
      }],
      Arc::new(ManualClock::new(0, None)),
    )
    .unwrap();
    assert_eq!(runtime.discover()[0].path, "eos/hardware/base/drive");
    let result = runtime.dispatch(CommandRequest {
      node_path: "cores/eos/layers/hardware/groups/base/nodes/drive".into(),
      meta: CommandMeta {
        command_id: "v2-test-1".into(),
        deadline_monotonic_ns: 200_000_000,
      },
      movement: Movement::Base(BaseMovement::BodyVelocity {
        x_m_s: 0.2,
        y_m_s: 0.0,
        yaw_rad_s: 0.0,
        valid_for_ms: 100,
      }),
    });
    assert_eq!(result.status, OutcomeStatus::OutcomeUnknown);
    assert_eq!(base.commands().len(), 1);
  }

  #[test]
  fn control_node_resolves_target_and_rejects_wrong_primitive_mode() {
    let dispatcher = "cores/eos/layers/dispatcher/groups/control/nodes/command";
    let target = "cores/eos/layers/hardware/groups/base/nodes/drive";
    let control = format!(
      r#"
[cores.eos.layers.control.groups.primitive.nodes.velocity_0]
id = "velocity_0"
node_type = "eos.control.primitive@v1.0"
label = "Velocity"
description = "Bounded velocity"
capabilities = ["primitive_control"]
references = ["{dispatcher}", "{target}"]
[cores.eos.layers.control.groups.primitive.nodes.velocity_0.settings]
target = "{target}"
movement_type = "body_velocity"
[cores.eos.layers.control.groups.primitive.nodes.velocity_0.inputs.status]
node = "{dispatcher}"
output = "status"
[cores.eos.layers.control.groups.primitive.nodes.velocity_0.inputs.base_observation]
node = "{target}"
output = "observation"
"#
    );
    let profile = Profile::from_toml(&format!("{PROFILE}\n{control}")).unwrap();
    let base = Arc::new(MockBase::new(BaseObservation {
      meta: SampleMeta {
        source_timestamp_ns: None,
        clock: ClockKind::Unknown,
        received_monotonic_ns: 0,
        sequence: Some(1),
        frame_id: "base_link".into(),
      },
      valid: true,
      x_m: 0.0,
      y_m: 0.0,
      yaw_rad: 0.0,
      x_m_s: 0.0,
      y_m_s: 0.0,
      yaw_rad_s: 0.0,
    }));
    let runtime = Runtime::new(
      profile,
      vec![AdapterRegistration::Base {
        node_id: "drive".into(),
        adapter: base.clone(),
      }],
      Arc::new(ManualClock::new(0, None)),
    )
    .unwrap();
    let path = "cores/eos/layers/control/groups/primitive/nodes/velocity_0";
    let mut request = CommandRequest {
      node_path: "cores/eos/layers/hardware/groups/base/nodes/forged".into(),
      meta: CommandMeta {
        command_id: "via-primitive".into(),
        deadline_monotonic_ns: 200_000_000,
      },
      movement: Movement::Base(BaseMovement::BodyVelocity {
        x_m_s: 0.2,
        y_m_s: 0.0,
        yaw_rad_s: 0.0,
        valid_for_ms: 100,
      }),
    };
    assert_eq!(
      runtime
        .invoke_control(path, request.clone())
        .unwrap()
        .status,
      OutcomeStatus::OutcomeUnknown
    );
    assert_eq!(base.commands().len(), 1);
    request.movement = Movement::Base(BaseMovement::RelativePose {
      x_m: 0.1,
      y_m: 0.0,
      yaw_rad: 0.0,
      frame_id: "base_link".into(),
    });
    assert!(runtime.invoke_control(path, request).is_err());
    assert_eq!(base.commands().len(), 1);
  }

  #[test]
  fn requires_exactly_one_adapter_for_every_node() {
    let profile = Profile::from_toml(PROFILE).unwrap();
    assert!(Runtime::new(profile, vec![], Arc::new(ManualClock::new(0, None))).is_err());
  }

  #[test]
  fn composed_cxx_base_crosses_the_dispatch_boundary_once() {
    let cxx_profile = PROFILE.replacen("type = \"mock\"", "type = \"mock-cxx\"", 1);
    let profile = Profile::from_toml(&cxx_profile).unwrap();
    let clock = Arc::new(ManualClock::new(20_000_000, None));
    let base = Arc::new(aether_hal::cxx_bridge::CxxMockBase::with_clock(
      clock.clone(),
      "base_link".into(),
    ));
    let runtime = Runtime::new(
      profile,
      vec![AdapterRegistration::Base {
        node_id: "drive".into(),
        adapter: base.clone(),
      }],
      clock,
    )
    .unwrap();
    let request = CommandRequest {
      node_path: "cores/eos/layers/hardware/groups/base/nodes/drive".into(),
      meta: CommandMeta {
        command_id: "cxx-1".into(),
        deadline_monotonic_ns: 200_000_000,
      },
      movement: Movement::Base(BaseMovement::BodyVelocity {
        x_m_s: 0.2,
        y_m_s: 0.0,
        yaw_rad_s: 0.0,
        valid_for_ms: 100,
      }),
    };
    let first = runtime.dispatch(request.clone());
    let second = runtime.dispatch(request);
    assert_eq!(first.status, OutcomeStatus::OutcomeUnknown);
    assert_eq!(second.status, first.status);
    let observed = aether_hal::BaseNode::observe(base.as_ref()).unwrap();
    assert!((observed.x_m_s - 0.2).abs() < f64::EPSILON);
  }

  #[test]
  fn demo_profile_mounts_three_isolated_cxx_nodes() {
    let source = include_str!(concat!(
      env!("CARGO_MANIFEST_DIR"),
      "/../../etc/eos/backends/mock-cxx.toml"
    ));
    let profile = Profile::from_toml(source).unwrap();
    let runtime =
      Runtime::from_cxx_mock_profile(profile, Arc::new(ManualClock::new(0, None))).unwrap();
    assert_eq!(runtime.discover().len(), 3);
    let frame = runtime
      .dispatcher()
      .capture_camera("cores/eos/layers/hardware/groups/camera/nodes/front")
      .unwrap();
    assert_eq!(frame.depth.unwrap().depth_u16.len(), 4);
    assert_eq!(runtime.dispatcher().emergency_stop().len(), 2);
  }
}
