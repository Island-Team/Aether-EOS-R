use std::collections::{BTreeMap, BTreeSet};

use serde::{Deserialize, Serialize};
use thiserror::Error;

mod registry;
mod edge_contract;
pub const GRAPH_VERSION: &str = "1.0.0";
pub const NEW_GRAPH_VERSION: &str = "0.1.0";
pub const SINGLE_CORE_GRAPH_VERSION: &str = "0.2.0";
pub use registry::{
  is_eos_backend_type, AdapterFamilyDescriptor, BridgeDeclaration, CatalogAdapterFamily, DataClass,
  DeliveryMode, DataTypeDescriptor, Lifecycle, NodeType, NodeTypeContribution, NodeTypeRegistry,
  Port, PortCardinality, PortDataType, PortTransport, PrimitiveSpec, SafetyAdapterType, Setting,
  SettingSelector, SettingType, SelectorProvider, PackageSource, PRIMITIVE_SPECS,
};
pub use edge_contract::{EosEdgeClass, EosEdgeContract};

/// Exact built-in EOS type IDs readable without a package contribution.
pub fn builtin_reader_node_types() -> Vec<String> {
  NodeTypeRegistry::builtin()
    .all()
    .filter(|definition| {
      definition.id.starts_with("eos.") || definition.id.starts_with("isos.eos_")
    })
    .map(|definition| definition.id.clone())
    .collect()
}

#[derive(Debug, Error)]
#[error("profile: {0}")]
pub struct ProfileError(pub String);

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, Ord, PartialOrd, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum NodeKind {
  #[default]
  Camera,
  Base,
  Manipulator,
  Software,
}

impl std::fmt::Display for NodeKind {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    f.write_str(match self {
      Self::Camera => "camera",
      Self::Base => "base",
      Self::Manipulator => "manipulator",
      Self::Software => "software",
    })
  }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Profile {
  pub version: String,
  pub robot: RobotProfile,
  pub cores: BTreeMap<String, CoreProfile>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct RobotProfile {
  pub id: String,
  pub name: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct CoreProfile {
  pub layers: BTreeMap<String, LayerProfile>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct LayerProfile {
  pub groups: BTreeMap<String, NodeGroup>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct NodeGroup {
  pub nodes: BTreeMap<String, NodeProfile>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct NodeProfile {
  pub id: String,
  pub node_type: String,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub package: Option<PackageRef>,
  pub label: String,
  pub description: String,
  #[serde(default)]
  pub frame_id: String,
  #[serde(default)]
  pub joint_names: Option<Vec<String>>,
  #[serde(default)]
  pub adapter: AdapterProfile,
  #[serde(default)]
  pub capabilities: Vec<String>,
  #[serde(default)]
  pub health_timing: HealthTiming,
  pub observation: Option<ObservationProfile>,
  pub movement: Option<MovementProfile>,
  #[serde(default)]
  pub references: Vec<String>,
  #[serde(default)]
  pub settings: BTreeMap<String, toml::Value>,
  #[serde(default)]
  pub inputs: BTreeMap<String, PortSources>,
  #[serde(skip)]
  pub kind: NodeKind,
  #[serde(skip)]
  pub path: String,
}

#[derive(Clone, Debug, Eq, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct PackageRef {
  pub id: String,
  pub version: String,
}

impl PackageRef {
  pub fn validate(&self) -> Result<(), ProfileError> {
    let valid_id = self
      .id
      .split_once('/')
      .is_some_and(|(owner, name)| valid_segment(owner) && valid_segment(name));
    let valid_version = self.version.split('.').count() == 3
      && self
        .version
        .split('.')
        .all(|part| !part.is_empty() && part.bytes().all(|b| b.is_ascii_digit()));
    if !valid_id || !valid_version {
      return Err(ProfileError(format!(
        "invalid package reference: {}@{}",
        self.id, self.version
      )));
    }
    Ok(())
  }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct PortSource {
  pub node: String,
  pub output: String,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub priority: Option<i64>,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub frequency_hz: Option<f64>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(untagged)]
pub enum PortSources {
  One(PortSource),
  Many(Vec<PortSource>),
}

impl PortSources {
  pub fn values(&self) -> &[PortSource] {
    match self {
      Self::One(source) => std::slice::from_ref(source),
      Self::Many(sources) => sources,
    }
  }

  pub fn one(&self) -> Option<&PortSource> {
    let values = self.values();
    (values.len() == 1).then(|| &values[0])
  }

  fn values_mut(&mut self) -> &mut [PortSource] {
    match self {
      Self::One(source) => std::slice::from_mut(source),
      Self::Many(sources) => sources,
    }
  }
}

/// Canonical v2 address; legacy graph addresses are imported to this form.
pub fn canonical_node_path(path: &str) -> Option<String> {
  let parts: Vec<_> = path.split('/').collect();
  let fields = match parts.as_slice() {
    [core, layer, group, node] => [*core, *layer, *group, *node],
    ["cores", core, "layers", layer, "groups", group, "nodes", node] => {
      [*core, *layer, *group, *node]
    }
    _ => return None,
  };
  fields
    .iter()
    .all(|part| valid_segment(part))
    .then(|| fields.join("/"))
}

impl NodeProfile {
  pub fn path(&self) -> &str {
    &self.path
  }
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct AdapterProfile {
  #[serde(rename = "type")]
  #[serde(default)]
  pub adapter_type: String,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub package: Option<PackageRef>,
  #[serde(default)]
  pub settings: BTreeMap<String, toml::Value>,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct HealthTiming {
  pub poll_interval_ms: u64,
  pub stale_after_ms: u64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ObservationProfile {
  pub max_rate_hz: f64,
  pub max_payload_bytes: u64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct MovementProfile {
  pub modes: Vec<String>,
  pub limits: MovementLimits,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct MovementLimits {
  pub max_linear_m_s: Option<f64>,
  pub max_angular_rad_s: Option<f64>,
  pub max_velocity_lease_ms: Option<u64>,
  pub max_translation_m: Option<f64>,
  pub max_rotation_rad: Option<f64>,
  pub max_joint_position_rad: Option<f64>,
  pub max_joint_speed_rad_s: Option<f64>,
  pub max_end_effector_speed_m_s: Option<f64>,
  pub max_end_effector_translation_m: Option<f64>,
}

impl Profile {
  pub fn from_json(source: &str) -> Result<Self, ProfileError> {
    Self::from_json_with_registry(source, &NodeTypeRegistry::builtin())
  }

  pub fn from_json_with_registry(
    source: &str,
    registry: &NodeTypeRegistry,
  ) -> Result<Self, ProfileError> {
    let mut profile: Profile =
      serde_json::from_str(source).map_err(|e| ProfileError(e.to_string()))?;
    profile.validate_with_registry(registry)?;
    Ok(profile)
  }

  pub fn from_toml(source: &str) -> Result<Self, ProfileError> {
    Self::from_toml_with_registry(source, &NodeTypeRegistry::builtin())
  }

  pub fn from_toml_with_registry(
    source: &str,
    registry: &NodeTypeRegistry,
  ) -> Result<Self, ProfileError> {
    let mut profile: Profile = toml::from_str(source).map_err(|e| ProfileError(e.to_string()))?;
    profile.validate_with_registry(registry)?;
    Ok(profile)
  }

  /// Hardware nodes owned by EOS and eligible for adapter registration.
  pub fn hardware_nodes(&self) -> impl Iterator<Item = &NodeProfile> {
    let (core, layer) = if self.version == SINGLE_CORE_GRAPH_VERSION {
      ("isos", "eos_hardware")
    } else {
      ("eos", "hardware")
    };
    self
      .cores
      .get(core)
      .into_iter()
      .flat_map(move |core| core.layers.get(layer))
      .flat_map(|layer| layer.groups.values())
      .flat_map(|group| group.nodes.values())
  }

  pub fn all_nodes(&self) -> impl Iterator<Item = &NodeProfile> {
    self
      .cores
      .values()
      .flat_map(|core| core.layers.values())
      .flat_map(|layer| layer.groups.values())
      .flat_map(|group| group.nodes.values())
  }

  pub fn node(&self, path: &str) -> Option<&NodeProfile> {
    let canonical = canonical_node_path(path)?;
    self.all_nodes().find(|node| node.path == canonical)
  }

  pub fn validate(&mut self) -> Result<(), ProfileError> {
    self.validate_with_registry(&NodeTypeRegistry::builtin())
  }

  pub fn validate_with_registry(
    &mut self,
    registry: &NodeTypeRegistry,
  ) -> Result<(), ProfileError> {
    let mut next = self.clone();
    next.normalize_paths()?;
    next.validate_normalized_with_registry(registry)?;
    *self = next;
    Ok(())
  }

  fn normalize_paths(&mut self) -> Result<(), ProfileError> {
    self.migrate_legacy_vision()?;
    let mut forms = BTreeSet::new();
    for node in self
      .cores
      .values_mut()
      .flat_map(|core| core.layers.values_mut())
      .flat_map(|layer| layer.groups.values_mut())
      .flat_map(|group| group.nodes.values_mut())
    {
      for path in &mut node.references {
        normalize_reference(path, &mut forms)?;
      }
      for sources in node.inputs.values_mut() {
        for source in sources.values_mut() {
          normalize_reference(&mut source.node, &mut forms)?;
        }
      }
      for key in ["target", "backend"] {
        if let Some(value) = node.settings.get_mut(key) {
          if let Some(path) = value.as_str() {
            let mut path = path.to_owned();
            normalize_reference(&mut path, &mut forms)?;
            *value = toml::Value::String(path);
          }
        }
        if let Some(value) = node.adapter.settings.get_mut(key) {
          if let Some(path) = value.as_str() {
            let mut path = path.to_owned();
            normalize_reference(&mut path, &mut forms)?;
            *value = toml::Value::String(path);
          }
        }
      }
      for key in ["steps", "depends_on"] {
        if let Some(values) = node
          .settings
          .get_mut(key)
          .and_then(toml::Value::as_array_mut)
        {
          for value in values {
            if let Some(path) = value.as_str() {
              let mut path = path.to_owned();
              normalize_reference(&mut path, &mut forms)?;
              *value = toml::Value::String(path);
            }
          }
        }
      }
      if let Some(sources) = node.inputs.remove("command") {
        node.inputs.insert(
          "command".into(),
          PortSources::Many(sources.values().to_vec()),
        );
      }
    }
    if forms.len() > 1 {
      return Err(ProfileError(
        "mixed legacy and short node paths are ambiguous".into(),
      ));
    }
    Ok(())
  }

  fn migrate_legacy_vision(&mut self) -> Result<(), ProfileError> {
    let Some(mut old_core) = self.cores.remove("isosui") else {
      return Ok(());
    };
    if old_core.layers.len() != 1 || !old_core.layers.contains_key("system") {
      return Err(ProfileError("unsupported isosui legacy core".into()));
    }
    let mut layer = old_core
      .layers
      .remove("system")
      .expect("checked system layer");
    if layer.groups.len() != 1 || !layer.groups.contains_key("ui") {
      return Err(ProfileError("unsupported isosui legacy group".into()));
    }
    let group = layer.groups.remove("ui").expect("checked ui group");
    let target = &mut self
      .cores
      .entry("isos".into())
      .or_insert_with(|| CoreProfile {
        layers: BTreeMap::new(),
      })
      .layers
      .entry("ui".into())
      .or_insert_with(|| LayerProfile {
        groups: BTreeMap::new(),
      })
      .groups
      .entry("panel".into())
      .or_insert_with(|| NodeGroup {
        nodes: BTreeMap::new(),
      })
      .nodes;
    for (id, mut node) in group.nodes {
      if node.node_type != "isosui.system.vision@v1.0" || target.contains_key(&id) {
        return Err(ProfileError(format!(
          "unsupported or duplicate legacy Vision node: {id}"
        )));
      }
      node.node_type = "isos.ui.vision@v1.0".into();
      target.insert(id, node);
    }
    Ok(())
  }

  fn validate_normalized_with_registry(
    &mut self,
    registry: &NodeTypeRegistry,
  ) -> Result<(), ProfileError> {
    if self.version != GRAPH_VERSION
      && self.version != NEW_GRAPH_VERSION
      && self.version != SINGLE_CORE_GRAPH_VERSION
    {
      return Err(ProfileError(format!(
        "profile version must be {GRAPH_VERSION}, {NEW_GRAPH_VERSION}, or {SINGLE_CORE_GRAPH_VERSION}"
      )));
    }
    if self.version == SINGLE_CORE_GRAPH_VERSION
      && (self.cores.len() != 1 || !self.cores.contains_key("isos"))
    {
      return Err(ProfileError(
        "0.2.0 profile requires only the isos core".into(),
      ));
    }
    if !valid_segment(&self.robot.id) || self.robot.name.trim().is_empty() {
      return Err(ProfileError("robot id and name are required".into()));
    }
    let mut hardware_ids = BTreeSet::new();
    let mut paths = BTreeSet::new();
    let legacy_mujoco_backend = self.all_nodes().any(|node| {
      node.node_type == "eos.process.backend@v1.0"
        && node.settings.get("package").and_then(toml::Value::as_str) == Some("island/mujoco-sim")
        && node.settings.get("version").and_then(toml::Value::as_str) == Some("0.1.1")
    });
    for (core_id, core) in &mut self.cores {
      if !valid_segment(core_id) || core.layers.is_empty() {
        return Err(ProfileError(format!("invalid or empty core: {core_id}")));
      }
      for (layer_id, layer) in &mut core.layers {
        if !valid_segment(layer_id) || layer.groups.is_empty() {
          return Err(ProfileError(format!("invalid or empty layer: {layer_id}")));
        }
        for (group_id, group) in &mut layer.groups {
          if !valid_segment(group_id) || group.nodes.is_empty() {
            return Err(ProfileError(format!("invalid or empty group: {group_id}")));
          }
          for (key, node) in &mut group.nodes {
            let definition = registry
              .get(&node.node_type)
              .ok_or_else(|| ProfileError(format!("unknown node type: {}", node.node_type)))?;
            let expected_type_version = match self.version.as_str() {
              SINGLE_CORE_GRAPH_VERSION => "@v0.2",
              NEW_GRAPH_VERSION => "@v0.1",
              _ => "@v1.",
            };
            if !node.node_type.contains(expected_type_version) {
              return Err(ProfileError(format!(
                "node {key} type {} is incompatible with graph version {}",
                node.node_type, self.version
              )));
            }
            if node.package.as_ref() != registry.package_for_type(&node.node_type) {
              return Err(ProfileError(format!(
                "node {key} package does not match registered type {}",
                node.node_type
              )));
            }
            if let Some(package) = &node.package {
              package.validate()?;
            }
            if let Some(package) = &node.adapter.package {
              package.validate()?;
            }
            let kind = definition.kind;
            let hardware = kind != NodeKind::Software;
            if hardware {
              if let Some(family) = registry.adapter_family(&node.adapter.adapter_type) {
                let legacy_unpinned_mujoco = family.id == "mujoco-v2"
                  && node.adapter.package.is_none()
                  && node.package.is_none()
                  && legacy_mujoco_backend;
                let family_package = if family.id == "mujoco-v2" {
                  node.adapter.package.as_ref().or(node.package.as_ref())
                } else {
                  node.adapter.package.as_ref()
                };
                if !family.device_kinds.contains(&kind)
                  || (!legacy_unpinned_mujoco
                    && family_package != registry.package_for_adapter_family(&family.id))
                {
                  return Err(ProfileError(format!(
                    "node {key} adapter family provenance or kind mismatch"
                  )));
                }
              } else if node.adapter.package.is_some()
                || (node.adapter.adapter_type == "mujoco-v2"
                  && !legacy_mujoco_backend
                  && !matches!(
                    node.node_type.as_str(),
                    "eos.hardware.mujoco_camera@v1.0"
                      | "eos.hardware.mujoco_base@v1.0"
                      | "eos.hardware.mujoco_manipulator@v1.0"
                  ))
              {
                return Err(ProfileError(format!(
                  "node {key} has unregistered adapter family"
                )));
              }
            }
            if definition.core != *core_id
              || definition.layer != *layer_id
              || definition.group != *group_id
            {
              return Err(ProfileError(format!(
                "node {key} type {} is placed outside {}/{}/{}",
                node.node_type, definition.core, definition.layer, definition.group
              )));
            }
            registry.validate_instance(node, definition)?;
            if node.id != *key || !valid_segment(key) {
              return Err(ProfileError(format!("invalid node id: {key}")));
            }
            let path = format!("{core_id}/{layer_id}/{group_id}/{key}");
            if !paths.insert(path.clone()) {
              return Err(ProfileError(format!("duplicate node path: {path}")));
            }
            if hardware && !hardware_ids.insert(node.id.clone()) {
              return Err(ProfileError(format!("duplicate hardware node id: {key}")));
            }
            if node.label.trim().is_empty()
              || ((self.version == NEW_GRAPH_VERSION || self.version == SINGLE_CORE_GRAPH_VERSION)
                && node.description.trim().is_empty())
              || (hardware
                && (node.adapter.adapter_type.trim().is_empty() || node.frame_id.trim().is_empty()))
            {
              return Err(ProfileError(format!(
                "node {key} requires label and hardware adapter/frame fields"
              )));
            }
            if hardware
              && (node.health_timing.poll_interval_ms == 0
                || node.health_timing.stale_after_ms < node.health_timing.poll_interval_ms)
            {
              return Err(ProfileError(format!(
                "node {key} has invalid health timing"
              )));
            }
            let mut capabilities = BTreeSet::new();
            if !node
              .capabilities
              .iter()
              .all(|c| valid_segment(c) && capabilities.insert(c))
            {
              return Err(ProfileError(format!("node {key} has invalid capabilities")));
            }
            node.kind = kind;
            node.path = path;
            if !hardware {
              if node.joint_names.is_some()
                || node.movement.is_some()
                || node.observation.is_some()
                || !node.frame_id.is_empty()
                || !node.adapter.adapter_type.is_empty()
                || !node.adapter.settings.is_empty()
                || node.health_timing.poll_interval_ms != 0
                || node.health_timing.stale_after_ms != 0
              {
                return Err(ProfileError(format!(
                  "software node {key} cannot declare hardware interfaces"
                )));
              }
              continue;
            }
            match (&kind, &node.joint_names) {
              (NodeKind::Manipulator, Some(names)) if !names.is_empty() => {
                let mut unique = BTreeSet::new();
                if !names
                  .iter()
                  .all(|name| valid_segment(name) && unique.insert(name))
                {
                  return Err(ProfileError(format!("node {key} has invalid joint names")));
                }
              }
              (NodeKind::Manipulator, _) => {
                return Err(ProfileError(format!("node {key} requires joint_names")))
              }
              (_, Some(_)) => {
                return Err(ProfileError(format!(
                  "node {key} cannot declare joint_names"
                )))
              }
              (_, None) => {}
            }
            if let Some(obs) = &node.observation {
              if !obs.max_rate_hz.is_finite()
                || obs.max_rate_hz <= 0.0
                || obs.max_payload_bytes == 0
              {
                return Err(ProfileError(format!(
                  "node {key} has invalid observation bounds"
                )));
              }
            }
            if node.observation.is_none() {
              return Err(ProfileError(format!(
                "node {key} requires observation settings"
              )));
            }
            if kind != NodeKind::Camera && node.movement.is_none() {
              return Err(ProfileError(format!(
                "actuator {key} requires movement settings"
              )));
            }
            if kind == NodeKind::Camera && node.movement.is_some() {
              return Err(ProfileError(format!(
                "camera {key} cannot advertise movement"
              )));
            }
            let allowed_capabilities: &[&str] = match kind {
              NodeKind::Software => unreachable!(),
              NodeKind::Camera => &["rgb8", "aligned_depth_u16"],
              NodeKind::Base => &["body_velocity", "relative_pose"],
              NodeKind::Manipulator => &["joint_positions", "end_effector_pose"],
            };
            if node.capabilities.is_empty()
              || !node
                .capabilities
                .iter()
                .all(|c| allowed_capabilities.contains(&c.as_str()))
              || (kind == NodeKind::Camera && !capabilities.contains(&"rgb8".to_string()))
            {
              return Err(ProfileError(format!("node {key} has invalid capabilities")));
            }
            if let Some(movement) = &node.movement {
              let allowed = match kind {
                NodeKind::Software => unreachable!(),
                NodeKind::Base => ["body_velocity", "relative_pose"].as_slice(),
                NodeKind::Manipulator => ["joint_positions", "end_effector_pose"].as_slice(),
                NodeKind::Camera => &[],
              };
              let mut modes = BTreeSet::new();
              if movement.modes.is_empty()
                || !movement
                  .modes
                  .iter()
                  .all(|m| allowed.contains(&m.as_str()) && modes.insert(m))
              {
                return Err(ProfileError(format!(
                  "node {key} has invalid movement modes"
                )));
              }
              for mode in &movement.modes {
                let limits = &movement.limits;
                let bounded = match mode.as_str() {
                  "body_velocity" => {
                    limits.max_linear_m_s.is_some()
                      && limits.max_angular_rad_s.is_some()
                      && limits.max_velocity_lease_ms.is_some_and(|ms| ms > 0)
                  }
                  "relative_pose" => {
                    limits.max_translation_m.is_some() && limits.max_rotation_rad.is_some()
                  }
                  "joint_positions" => {
                    limits.max_joint_position_rad.is_some()
                      && limits.max_joint_speed_rad_s.is_some()
                  }
                  "end_effector_pose" => {
                    limits.max_end_effector_translation_m.is_some()
                      && limits.max_end_effector_speed_m_s.is_some()
                  }
                  _ => false,
                };
                if !bounded {
                  return Err(ProfileError(format!(
                    "node {key} has missing limits for {mode}"
                  )));
                }
                if !capabilities.contains(mode) {
                  return Err(ProfileError(format!(
                    "node {key} capability missing movement mode {mode}"
                  )));
                }
              }
              if node.capabilities.iter().any(|c| {
                [
                  "body_velocity",
                  "relative_pose",
                  "joint_positions",
                  "end_effector_pose",
                ]
                .contains(&c.as_str())
                  && !modes.contains(c)
              }) {
                return Err(ProfileError(format!(
                  "node {key} advertises unsupported movement capability"
                )));
              }
              for value in [
                movement.limits.max_linear_m_s,
                movement.limits.max_angular_rad_s,
                movement.limits.max_translation_m,
                movement.limits.max_rotation_rad,
                movement.limits.max_joint_position_rad,
                movement.limits.max_joint_speed_rad_s,
                movement.limits.max_end_effector_speed_m_s,
                movement.limits.max_end_effector_translation_m,
              ]
              .into_iter()
              .flatten()
              {
                if !value.is_finite() || value <= 0.0 {
                  return Err(ProfileError(format!(
                    "node {key} has invalid movement limit"
                  )));
                }
              }
            }
          }
        }
      }
    }
    for node in self.all_nodes() {
      let mut seen = BTreeSet::new();
      for reference in &node.references {
        if reference == node.path() || !seen.insert(reference) || !paths.contains(reference) {
          return Err(ProfileError(format!(
            "node {} has invalid reference: {reference}",
            node.path()
          )));
        }
      }
      let definition = registry.get(&node.node_type).expect("validated node type");
      for (input_name, sources) in &node.inputs {
        let input = definition
          .inputs
          .iter()
          .find(|port| &port.name == input_name)
          .ok_or_else(|| {
            ProfileError(format!(
              "node {} has unknown input {input_name}",
              node.path()
            ))
          })?;
        if sources.values().is_empty()
          || (input.cardinality != PortCardinality::Many && sources.values().len() != 1)
        {
          return Err(ProfileError(format!(
            "node {} input {input_name} has invalid cardinality",
            node.path()
          )));
        }
        let mut unique = BTreeSet::new();
        for source in sources.values() {
          if !unique.insert((&source.node, &source.output)) {
            return Err(ProfileError(format!(
              "node {} input {input_name} repeats a source",
              node.path()
            )));
          }
          if self.node(&source.node).is_none()
            && external_bridge_input(node, definition, input, input_name, &source.node)
          {
            // IsOS admits the complete cross-core edge before handing EOS its
            // owner projection. EOS retains the binding for run-fenced ingress.
            continue;
          }
          let source_node = self.node(&source.node).ok_or_else(|| {
            ProfileError(format!(
              "node {} input {input_name} references missing node {}",
              node.path(),
              source.node
            ))
          })?;
          let source_type = registry
            .get(&source_node.node_type)
            .expect("validated source type");
          let output = source_type
            .outputs
            .iter()
            .find(|port| port.name == source.output)
            .ok_or_else(|| {
              ProfileError(format!(
                "node {} has unknown source output {}",
                node.path(),
                source.output
              ))
            })?;
          if source.node == node.path || input.data_type != output.data_type {
            return Err(ProfileError(format!(
              "node {} input {input_name} has incompatible source",
              node.path()
            )));
          }
          if matches!(
            node.node_type.as_str(),
            "eos.interface.camera_gate@v1.0" | "eos.interface.camera_gate@v0.1"
          ) && (source_node.kind != NodeKind::Camera
            || input_name != "frame"
            || source.output != "frame")
          {
            return Err(ProfileError(format!(
              "camera gate {} must read a camera frame output",
              node.path()
            )));
          }
          if matches!(
            node.node_type.as_str(),
            "isosui.system.vision@v1.0" | "isos.ui.vision@v1.0"
          ) && (!matches!(
            source_node.node_type.as_str(),
            "eos.interface.camera_gate@v1.0" | "eos.interface.camera_gate@v0.1"
          ) || input_name != "preview"
            || source.output != "preview")
          {
            return Err(ProfileError(format!(
              "Vision node {} must read a camera gate preview",
              node.path()
            )));
          }
        }
      }
      for input in definition.inputs.iter().filter(|port| port.required) {
        if !node.inputs.contains_key(&input.name) {
          return Err(ProfileError(format!(
            "node {} requires input {}",
            node.path(),
            input.name
          )));
        }
      }
    }
    edge_contract::validate(self, registry)?;
    self.validate_control_nodes()?;
    // EOS owns package-backed simulator children. A backend declaration
    // binds every MuJoCo adapter to its one typed socket endpoint.
    if self
      .all_nodes()
      .any(|node| is_eos_backend_type(&node.node_type))
    {
      for node in self
        .hardware_nodes()
        .filter(|node| node.adapter.adapter_type == "mujoco-v2")
      {
        let backend_path = node
          .adapter
          .settings
          .get("backend")
          .and_then(toml::Value::as_str)
          .ok_or_else(|| ProfileError(format!("node {} requires backend binding", node.path())))?;
        let backend = self.node(backend_path).ok_or_else(|| {
          ProfileError(format!(
            "node {} references missing backend {backend_path}",
            node.path()
          ))
        })?;
        if !is_eos_backend_type(&backend.node_type)
          || !node.references.iter().any(|path| path == backend_path)
        {
          return Err(ProfileError(format!(
            "node {} must reference an EOS backend",
            node.path()
          )));
        }
        let socket = node
          .adapter
          .settings
          .get("socket_path")
          .and_then(toml::Value::as_str);
        let backend_socket = backend.settings.get("socket").and_then(toml::Value::as_str);
        let device = node
          .adapter
          .settings
          .get("simulator_node")
          .and_then(toml::Value::as_str);
        let device_alias = node
          .adapter
          .settings
          .get("device_id")
          .and_then(toml::Value::as_str);
        if socket.is_none()
          || socket != backend_socket
          || device.is_none()
          || device_alias != device
          || (node
            .adapter
            .package
            .as_ref()
            .or(node.package.as_ref())
            .is_some()
            && node.adapter.package.as_ref().or(node.package.as_ref()) != backend.package.as_ref())
        {
          return Err(ProfileError(format!(
            "node {} has a mismatched backend socket or device ID",
            node.path()
          )));
        }
      }
    }
    let mut dependencies: BTreeMap<String, Vec<String>> = BTreeMap::new();
    for node in self.all_nodes().filter(|node| {
      registry
        .get(&node.node_type)
        .is_some_and(|kind| kind.lifecycle == Lifecycle::Process)
    }) {
      let mut seen = BTreeSet::new();
      let mut predecessors = Vec::new();
      if let Some(values) = node
        .settings
        .get("depends_on")
        .and_then(toml::Value::as_array)
      {
        for value in values {
          let path = value.as_str().expect("validated string array");
          let predecessor = self.node(path).ok_or_else(|| {
            ProfileError(format!(
              "node {} depends on missing node {path}",
              node.path()
            ))
          })?;
          if registry
            .get(&predecessor.node_type)
            .is_none_or(|kind| kind.lifecycle != Lifecycle::Process)
            || path == node.path()
            || !seen.insert(path)
          {
            return Err(ProfileError(format!(
              "node {} has invalid process dependency {path}",
              node.path()
            )));
          }
          predecessors.push(path.to_owned());
        }
      }
      dependencies.insert(node.path().to_owned(), predecessors);
    }
    let mut visited = BTreeSet::new();
    let mut active = BTreeSet::new();
    for path in dependencies.keys() {
      visit_dependency(path, &dependencies, &mut active, &mut visited)?;
    }
    // One graph-owned route reaches each actuator; the map makes a second
    // source for the same input impossible before runtime admission.
    let mut binding_targets: BTreeMap<&str, &str> = BTreeMap::new();
    for node in self
      .hardware_nodes()
      .filter(|node| node.kind != NodeKind::Camera)
    {
      let sources = node
        .inputs
        .get("command")
        .ok_or_else(|| ProfileError(format!("actuator {} requires a command edge", node.path())))?;
      let legacy_output = match node.kind {
        NodeKind::Base => "base_command",
        NodeKind::Manipulator => "manipulator_command",
        _ => unreachable!(),
      };
      let mut legacy = false;
      for source in sources.values() {
        let sender = self
          .node(&source.node)
          .expect("validated connection source");
        if matches!(
          sender.node_type.as_str(),
          "eos.dispatcher.control@v1.0"
            | "eos.dispatcher.control@v0.1"
            | "isos.eos_dispatcher.control@v0.2"
        ) && source.output == legacy_output
        {
          legacy = true;
        } else if is_primitive_binding(&sender.node_type, node.kind) && source.output == "command" {
          if binding_targets.insert(sender.path(), node.path()).is_some() {
            return Err(ProfileError(format!(
              "primitive binding {} targets multiple devices",
              sender.path()
            )));
          }
          validate_primitive_binding(sender, node)?;
        } else {
          return Err(ProfileError(format!(
            "actuator {} has an unadmitted command source",
            node.path()
          )));
        }
      }
      if legacy && sources.values().len() != 1 {
        return Err(ProfileError(format!(
          "actuator {} mixes legacy and binding command sources",
          node.path()
        )));
      }
    }
    for binding in self
      .all_nodes()
      .filter(|node| is_any_primitive_binding(&node.node_type))
    {
      if !binding_targets.contains_key(binding.path()) {
        return Err(ProfileError(format!(
          "primitive binding {} requires exactly one hardware target",
          binding.path()
        )));
      }
    }
    Ok(())
  }

  fn validate_control_nodes(&self) -> Result<(), ProfileError> {
    const DISPATCHER: &str = "eos/dispatcher/control/command";
    for node in self.all_nodes() {
      let mut expected = BTreeSet::new();
      match node.node_type.as_str() {
        "eos.control.manual@v1.0" | "eos.control.primitive@v1.0" => {
          let target_path = node.settings["target"].as_str().expect("validated target");
          let target = self.node(target_path).ok_or_else(|| {
            ProfileError(format!(
              "control {} has missing target {target_path}",
              node.path()
            ))
          })?;
          if !matches!(target.kind, NodeKind::Base | NodeKind::Manipulator) {
            return Err(ProfileError(format!(
              "control {} target is not an actuator",
              node.path()
            )));
          }
          if node.node_type == "eos.control.primitive@v1.0" {
            let mode = node.settings["movement_type"]
              .as_str()
              .expect("validated mode");
            if !target
              .movement
              .as_ref()
              .is_some_and(|movement| movement.modes.iter().any(|value| value == mode))
            {
              return Err(ProfileError(format!(
                "primitive {} mode is not supported by its target",
                node.path()
              )));
            }
          }
          let (expected_port, other_port) = if target.kind == NodeKind::Base {
            ("base_observation", "manipulator_observation")
          } else {
            ("manipulator_observation", "base_observation")
          };
          if node
            .inputs
            .get(expected_port)
            .and_then(PortSources::one)
            .is_none_or(|input| input.node != target_path || input.output != "observation")
            || node.inputs.contains_key(other_port)
          {
            return Err(ProfileError(format!(
              "control {} must connect typed observation from its target",
              node.path()
            )));
          }
          expected.insert(target_path);
          expected.insert(DISPATCHER);
        }
        "eos.control.skill@v1.0" => {
          let steps = node.settings["steps"].as_array().expect("validated steps");
          if steps.len() > 16 {
            return Err(ProfileError(format!(
              "skill {} exceeds 16 steps",
              node.path()
            )));
          }
          for value in steps {
            let path = value.as_str().expect("validated step");
            if self
              .node(path)
              .is_none_or(|step| step.node_type != "eos.control.primitive@v1.0")
            {
              return Err(ProfileError(format!(
                "skill {} contains invalid primitive {path}",
                node.path()
              )));
            }
            expected.insert(path);
          }
          expected.insert(DISPATCHER);
        }
        _ => continue,
      }
      if self
        .node(DISPATCHER)
        .is_none_or(|dispatcher| dispatcher.node_type != "eos.dispatcher.control@v1.0")
        || node
          .inputs
          .get("status")
          .and_then(PortSources::one)
          .is_none_or(|input| input.node != DISPATCHER || input.output != "status")
        || node
          .references
          .iter()
          .map(String::as_str)
          .collect::<BTreeSet<_>>()
          != expected
      {
        return Err(ProfileError(format!(
          "control {} must reference its targets and EOS dispatcher exactly",
          node.path()
        )));
      }
    }
    Ok(())
  }
}

pub(crate) fn external_bridge_input(
  node: &NodeProfile,
  definition: &NodeType,
  input: &Port,
  input_name: &str,
  source_node: &str,
) -> bool {
  node.path().starts_with("eos/")
    && !source_node.starts_with("eos/")
    && source_node != node.path()
    && input.transport == Some(PortTransport::Http)
    && definition.bridge.as_ref().is_some_and(|bridge| {
      bridge.adapter == "http" && bridge.inputs.iter().any(|name| name == input_name)
    })
}

fn visit_dependency(
  path: &str,
  dependencies: &BTreeMap<String, Vec<String>>,
  active: &mut BTreeSet<String>,
  visited: &mut BTreeSet<String>,
) -> Result<(), ProfileError> {
  if visited.contains(path) {
    return Ok(());
  }
  if !active.insert(path.to_owned()) {
    return Err(ProfileError(format!("process dependency cycle at {path}")));
  }
  for predecessor in &dependencies[path] {
    visit_dependency(predecessor, dependencies, active, visited)?;
  }
  active.remove(path);
  visited.insert(path.to_owned());
  Ok(())
}

fn valid_segment(segment: &str) -> bool {
  !segment.is_empty()
    && segment
      .bytes()
      .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
}

fn normalize_reference(
  path: &mut String,
  forms: &mut BTreeSet<&'static str>,
) -> Result<(), ProfileError> {
  let form = if path.starts_with("cores/") {
    "legacy"
  } else {
    "short"
  };
  let canonical =
    canonical_node_path(path).ok_or_else(|| ProfileError(format!("invalid node path: {path}")))?;
  forms.insert(form);
  *path = canonical
    .strip_prefix("isosui/system/ui/")
    .map_or(canonical.clone(), |id| format!("isos/ui/panel/{id}"));
  Ok(())
}

fn is_any_primitive_binding(node_type: &str) -> bool {
  matches!(
    node_type,
    "eos.dispatcher.primitive_base@v1.0"
      | "eos.dispatcher.primitive_manipulator@v1.0"
      | "eos.dispatcher.primitive_base@v0.1"
      | "eos.dispatcher.primitive_manipulator@v0.1"
      | "isos.eos_dispatcher.primitive_base@v0.2"
      | "isos.eos_dispatcher.primitive_manipulator@v0.2"
  )
}

fn is_primitive_binding(node_type: &str, kind: NodeKind) -> bool {
  matches!(
    (node_type, kind),
    (
      "eos.dispatcher.primitive_base@v1.0"
        | "eos.dispatcher.primitive_base@v0.1"
        | "isos.eos_dispatcher.primitive_base@v0.2",
      NodeKind::Base
    ) | (
      "eos.dispatcher.primitive_manipulator@v1.0"
        | "eos.dispatcher.primitive_manipulator@v0.1"
        | "isos.eos_dispatcher.primitive_manipulator@v0.2",
      NodeKind::Manipulator
    )
  )
}

fn validate_primitive_binding(
  binding: &NodeProfile,
  device: &NodeProfile,
) -> Result<(), ProfileError> {
  let name = binding.settings["primitive_name"]
    .as_str()
    .expect("validated primitive name");
  let version = binding.settings["primitive_version"]
    .as_str()
    .expect("validated primitive version");
  let mode = binding.settings["movement_type"]
    .as_str()
    .expect("validated movement type");
  let supported = matches!(
    (name, version, mode, device.kind),
    ("base.velocity", "1.0.0", "body_velocity", NodeKind::Base)
      | (
        "arm.movej",
        "1.0.0",
        "joint_positions",
        NodeKind::Manipulator
      )
  );
  if !supported
    || !device
      .movement
      .as_ref()
      .is_some_and(|movement| movement.modes.iter().any(|m| m == mode))
  {
    return Err(ProfileError(format!(
      "primitive binding {} is incompatible with {}",
      binding.path(),
      device.path()
    )));
  }
  Ok(())
}
