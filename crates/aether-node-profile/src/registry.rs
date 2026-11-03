use std::collections::{BTreeMap, BTreeSet};

use serde::{Deserialize, Deserializer, Serialize, Serializer};

use crate::{valid_segment, NodeKind, NodeProfile, PackageRef, ProfileError};

#[derive(Clone, Copy, Debug, Eq, PartialEq, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Lifecycle {
  Internal,
  Process,
  Adapter,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum PortDataType {
  CameraFrame,
  BaseObservation,
  ManipulatorObservation,
  BaseMovement,
  ManipulatorMovement,
  ControlRequest,
  CommandOutcome,
  SkillOutcome,
  Health,
  Status,
  ObservationRef,
  ContextSnapshot,
  PlanProposal,
  VisionFrame,
  Custom(String),
}

impl PortDataType {
  pub fn id(&self) -> &str {
    match self {
      Self::CameraFrame => "camera_frame",
      Self::BaseObservation => "base_observation",
      Self::ManipulatorObservation => "manipulator_observation",
      Self::BaseMovement => "base_movement",
      Self::ManipulatorMovement => "manipulator_movement",
      Self::ControlRequest => "control_request",
      Self::CommandOutcome => "command_outcome",
      Self::SkillOutcome => "skill_outcome",
      Self::Health => "health",
      Self::Status => "status",
      Self::ObservationRef => "observation_ref",
      Self::ContextSnapshot => "context_snapshot",
      Self::PlanProposal => "plan_proposal",
      Self::VisionFrame => "vision_frame",
      Self::Custom(id) => id,
    }
  }
}

impl Serialize for PortDataType {
  fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
    serializer.serialize_str(self.id())
  }
}

impl<'de> Deserialize<'de> for PortDataType {
  fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
    let id = String::deserialize(deserializer)?;
    Ok(match id.as_str() {
      "camera_frame" => Self::CameraFrame,
      "base_observation" => Self::BaseObservation,
      "manipulator_observation" => Self::ManipulatorObservation,
      "base_movement" => Self::BaseMovement,
      "manipulator_movement" => Self::ManipulatorMovement,
      "control_request" => Self::ControlRequest,
      "command_outcome" => Self::CommandOutcome,
      "skill_outcome" => Self::SkillOutcome,
      "health" => Self::Health,
      "status" => Self::Status,
      "observation_ref" => Self::ObservationRef,
      "context_snapshot" => Self::ContextSnapshot,
      "plan_proposal" => Self::PlanProposal,
      "vision_frame" => Self::VisionFrame,
      _ => Self::Custom(id),
    })
  }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum PortTransport {
  InProcess,
  UnixSocket,
  Websocket,
  Ros2,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum DataClass {
  Image,
  Observation,
  Command,
  Event,
  State,
  Opaque,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum PortCardinality {
  #[default]
  One,
  Many,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Port {
  pub name: String,
  pub data_type: PortDataType,
  pub required: bool,
  #[serde(default)]
  pub cardinality: PortCardinality,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub description: Option<String>,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub expected_rate_hz: Option<f64>,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub transport: Option<PortTransport>,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub data_class: Option<DataClass>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct DataTypeDescriptor {
  pub id: String,
  pub version: String,
  pub purpose: String,
  pub media_type: String,
  pub codec: String,
  pub schema: serde_json::Value,
  pub max_payload_bytes: u64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum SettingType {
  String,
  PositiveInteger,
  NonnegativeInteger,
  StringArray,
  StringTable,
  ObjectArray,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Setting {
  pub name: String,
  pub data_type: SettingType,
  pub required: bool,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub description: Option<String>,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub unit: Option<String>,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub min: Option<i64>,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub max: Option<i64>,
  #[serde(default, skip_serializing_if = "Vec::is_empty")]
  pub choices: Vec<String>,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub selector: Option<SettingSelector>,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub items_schema: Option<serde_json::Value>,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub min_items: Option<usize>,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub max_items: Option<usize>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum SelectorProvider {
  Package,
  Registry,
  Graph,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Deserialize, Serialize)]
pub enum PackageSource {
  #[serde(rename = "node.package")]
  NodePackage,
  #[serde(rename = "node.adapter.package")]
  NodeAdapterPackage,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct SettingSelector {
  pub provider: SelectorProvider,
  pub resource: String,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub package_from: Option<PackageSource>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct NodeType {
  pub id: String,
  /// Operator-facing type description. Old sidecars omit this field.
  #[serde(default)]
  pub description: String,
  pub core: String,
  pub layer: String,
  pub group: String,
  pub kind: NodeKind,
  pub lifecycle: Lifecycle,
  pub capabilities: Vec<String>,
  pub settings: Vec<Setting>,
  pub inputs: Vec<Port>,
  pub outputs: Vec<Port>,
  /// Required for registered actuator types beyond the built-in EOS base and arm.
  #[serde(default)]
  pub safety_adapter_type: Option<String>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct SafetyAdapterType {
  pub id: String,
  pub kind: NodeKind,
  pub movement_modes: Vec<String>,
  pub supports_stop: bool,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct AdapterFamilyDescriptor {
  pub id: String,
  pub device_kinds: Vec<NodeKind>,
  pub transport: PortTransport,
  pub description: String,
  #[serde(default, skip_serializing_if = "Option::is_none")]
  pub settings_schema: Option<serde_json::Value>,
  #[serde(default)]
  pub supported_primitives: Vec<String>,
}

#[derive(Clone, Debug, Serialize)]
pub struct CatalogAdapterFamily {
  #[serde(flatten)]
  pub descriptor: AdapterFamilyDescriptor,
  #[serde(skip_serializing_if = "Option::is_none")]
  pub package: Option<PackageRef>,
  pub compiled: bool,
  /// Package families require a verified shared library at EOS launch.
  pub loadable: bool,
  #[serde(skip_serializing_if = "Option::is_none")]
  pub reason: Option<String>,
}

#[derive(Clone, Debug, Serialize)]
pub struct PrimitiveSpec {
  pub name: &'static str,
  pub version: &'static str,
  pub device_class: &'static str,
  pub movement_type: &'static str,
  pub description: &'static str,
}

pub const PRIMITIVE_SPECS: &[PrimitiveSpec] = &[
  PrimitiveSpec {
    name: "base.velocity",
    version: "1.0.0",
    device_class: "base",
    movement_type: "body_velocity",
    description: "Body-frame base velocity",
  },
  PrimitiveSpec {
    name: "arm.movej",
    version: "1.0.0",
    device_class: "manipulator",
    movement_type: "joint_positions",
    description: "Full joint-position target",
  },
];

#[derive(Clone, Debug, Default)]
pub struct NodeTypeRegistry {
  types: BTreeMap<String, NodeType>,
  safety_adapters: BTreeMap<String, SafetyAdapterType>,
  packages: BTreeMap<String, PackageRef>,
  data_types: BTreeMap<String, DataTypeDescriptor>,
  data_packages: BTreeMap<String, PackageRef>,
  codecs: BTreeSet<String>,
  adapter_families: BTreeMap<String, AdapterFamilyDescriptor>,
  family_packages: BTreeMap<String, PackageRef>,
}

/// Verified package contribution passed by IsOS, separate from graph instances.
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct NodeTypeContribution {
  pub package: PackageRef,
  #[serde(default)]
  pub node_types: Vec<NodeType>,
  #[serde(default)]
  pub safety_adapters: Vec<SafetyAdapterType>,
  #[serde(default)]
  pub data_types: Vec<DataTypeDescriptor>,
  #[serde(default)]
  pub adapter_families: Vec<AdapterFamilyDescriptor>,
}

impl NodeTypeRegistry {
  pub fn with_contribution_files(paths: &[std::path::PathBuf]) -> Result<Self, ProfileError> {
    let mut registry = Self::builtin();
    for path in paths {
      let bytes =
        std::fs::read(path).map_err(|e| ProfileError(format!("{}: {e}", path.display())))?;
      if bytes.len() > 1024 * 1024 {
        return Err(ProfileError(format!(
          "{}: node type contribution exceeds 1 MiB",
          path.display()
        )));
      }
      let contribution: NodeTypeContribution = serde_json::from_slice(&bytes)
        .map_err(|e| ProfileError(format!("{}: {e}", path.display())))?;
      registry.register_contribution(contribution)?;
    }
    Ok(registry)
  }
  pub fn builtin() -> Self {
    let mut registry = Self::default();
    registry.adapter_families.insert(
      "mock-cxx".into(),
      AdapterFamilyDescriptor {
        id: "mock-cxx".into(),
        device_kinds: vec![NodeKind::Camera, NodeKind::Base, NodeKind::Manipulator],
        transport: PortTransport::InProcess,
        description: "Compiled CXX test adapter".into(),
        settings_schema: None,
        supported_primitives: vec!["base.velocity@1.0.0".into(), "arm.movej@1.0.0".into()],
      },
    );
    registry
      .codecs
      .extend(["json_v1".into(), "vision_frame_jpeg_v1".into()]);
    for id in [
      "camera_frame",
      "base_observation",
      "manipulator_observation",
      "base_movement",
      "manipulator_movement",
      "control_request",
      "command_outcome",
      "skill_outcome",
      "health",
      "status",
      "observation_ref",
      "context_snapshot",
      "plan_proposal",
      "vision_frame",
    ] {
      let (media_type, codec, maximum) = if id == "vision_frame" {
        ("image/jpeg", "vision_frame_jpeg_v1", 8 * 1024 * 1024)
      } else {
        ("application/json", "json_v1", 8 * 1024 * 1024)
      };
      registry
        .register_builtin_data_type(DataTypeDescriptor {
          id: id.into(),
          version: "1.0".into(),
          purpose: builtin_data_purpose(id).into(),
          media_type: media_type.into(),
          codec: codec.into(),
          schema: builtin_data_schema(id),
          max_payload_bytes: maximum,
        })
        .expect("valid built-in data type");
    }
    let legacy_types = builtin_types();
    let target_types = target_types(&legacy_types);
    for definition in legacy_types.into_iter().chain(target_types) {
      registry
        .register_builtin(definition)
        .expect("valid built-in node type");
    }
    registry
  }

  pub fn get(&self, id: &str) -> Option<&NodeType> {
    self.types.get(id)
  }

  pub fn all(&self) -> impl Iterator<Item = &NodeType> {
    self.types.values()
  }

  pub fn definitions(&self) -> impl Iterator<Item = &NodeType> {
    self.all()
  }

  pub fn package_for_type(&self, id: &str) -> Option<&PackageRef> {
    self.packages.get(id)
  }

  pub fn data_type(&self, id: &str) -> Option<&DataTypeDescriptor> {
    self.data_types.get(id)
  }

  pub fn data_types(&self) -> impl Iterator<Item = &DataTypeDescriptor> {
    self.data_types.values()
  }

  pub fn package_for_data_type(&self, id: &str) -> Option<&PackageRef> {
    self.data_packages.get(id)
  }

  pub fn adapter_family(&self, id: &str) -> Option<&AdapterFamilyDescriptor> {
    self.adapter_families.get(id)
  }

  pub fn package_for_adapter_family(&self, id: &str) -> Option<&PackageRef> {
    self.family_packages.get(id)
  }

  pub fn catalog_adapter_families(&self) -> Vec<CatalogAdapterFamily> {
    self
      .adapter_families
      .values()
      .map(|descriptor| {
        let package = self.family_packages.get(&descriptor.id).cloned();
        let compiled = matches!(
          (
            descriptor.id.as_str(),
            package
              .as_ref()
              .map(|p| (p.id.as_str(), p.version.as_str()))
          ),
          ("mock-cxx", None) | ("mujoco-v2", Some(("island/mujoco-sim", "0.1.1")))
        );
        let loadable = !compiled
          && descriptor.transport == PortTransport::InProcess
          && self.family_packages.contains_key(&descriptor.id);
        CatalogAdapterFamily {
          descriptor: descriptor.clone(),
          package,
          compiled,
          loadable,
          reason: if loadable {
            Some("requires verified adapter library".into())
          } else if !compiled {
            Some("no EOS adapter implementation".into())
          } else {
            None
          },
        }
      })
      .collect()
  }

  pub fn register_codec(&mut self, id: &str) -> Result<(), ProfileError> {
    if !valid_segment(id) {
      return Err(ProfileError(format!("invalid codec ID: {id}")));
    }
    self.codecs.insert(id.into());
    Ok(())
  }

  pub fn register_contribution(
    &mut self,
    contribution: NodeTypeContribution,
  ) -> Result<(), ProfileError> {
    contribution.package.validate()?;
    let mut next = self.clone();
    for descriptor in contribution.data_types {
      let id = descriptor.id.clone();
      next.register_data_type(descriptor)?;
      next.data_packages.insert(id, contribution.package.clone());
    }
    for family in contribution.adapter_families {
      let mut kinds = BTreeSet::new();
      let mut primitives = BTreeSet::new();
      if !valid_segment(&family.id)
        || family.device_kinds.is_empty()
        || family.device_kinds.iter().any(|kind| {
          !matches!(
            kind,
            NodeKind::Camera | NodeKind::Base | NodeKind::Manipulator
          ) || !kinds.insert(*kind)
        })
        || family.description.trim().is_empty()
        || family
          .settings_schema
          .as_ref()
          .is_some_and(|schema| !schema.is_object())
        || family.supported_primitives.iter().any(|primitive| {
          !primitives.insert(primitive)
            || !PRIMITIVE_SPECS
              .iter()
              .any(|spec| format!("{}@{}", spec.name, spec.version) == *primitive)
        })
        || next.adapter_families.contains_key(&family.id)
      {
        return Err(ProfileError(format!(
          "invalid or duplicate adapter family: {}",
          family.id
        )));
      }
      next
        .family_packages
        .insert(family.id.clone(), contribution.package.clone());
      next.adapter_families.insert(family.id.clone(), family);
    }
    for adapter in contribution.safety_adapters {
      next.register_safety_adapter(adapter)?;
    }
    for definition in contribution.node_types {
      if matches!(
        definition.id.as_str(),
        "eos.hardware.mujoco_camera@v0.1"
          | "eos.hardware.mujoco_base@v0.1"
          | "eos.hardware.mujoco_manipulator@v0.1"
      ) {
        return Err(ProfileError(format!(
          "MuJoCo hardware uses generic built-in types: {}",
          definition.id
        )));
      }
      if let Some((layer, kind, lifecycle)) = match definition.id.as_str() {
        "eos.process.mujoco@v1.0" | "eos.process.mujoco@v0.1" => {
          Some(("process", NodeKind::Software, Lifecycle::Process))
        }
        "eos.hardware.mujoco_camera@v1.0" => {
          Some(("hardware", NodeKind::Camera, Lifecycle::Adapter))
        }
        "eos.hardware.mujoco_base@v1.0" => Some(("hardware", NodeKind::Base, Lifecycle::Adapter)),
        "eos.hardware.mujoco_manipulator@v1.0" => {
          Some(("hardware", NodeKind::Manipulator, Lifecycle::Adapter))
        }
        _ => None,
      } {
        if contribution.package.id != "island/mujoco-sim"
          || contribution.package.version != "0.1.1"
          || definition.core != "eos"
          || definition.layer != layer
          || definition.group != "mujoco"
          || definition.kind != kind
          || definition.lifecycle != lifecycle
        {
          return Err(ProfileError(format!(
            "unsupported MuJoCo type definition: {}",
            definition.id
          )));
        }
      }
      let id = definition.id.clone();
      next.register(definition)?;
      next.packages.insert(id, contribution.package.clone());
    }
    *self = next;
    Ok(())
  }

  pub fn register(&mut self, definition: NodeType) -> Result<(), ProfileError> {
    for port in definition.inputs.iter().chain(&definition.outputs) {
      let descriptor = self.data_types.get(port.data_type.id()).ok_or_else(|| {
        ProfileError(format!(
          "node type {} uses unknown data type {}",
          definition.id,
          port.data_type.id()
        ))
      })?;
      if !self.codecs.contains(&descriptor.codec) {
        return Err(ProfileError(format!(
          "node type {} requires uncompiled codec {}",
          definition.id, descriptor.codec
        )));
      }
    }
    if matches!(definition.kind, NodeKind::Base | NodeKind::Manipulator) {
      let safety = definition
        .safety_adapter_type
        .as_deref()
        .and_then(|id| self.safety_adapters.get(id))
        .ok_or_else(|| {
          ProfileError(format!(
            "custom actuator type {} requires a registered safety adapter",
            definition.id
          ))
        })?;
      if safety.kind != definition.kind
        || !safety.supports_stop
        || !definition
          .capabilities
          .iter()
          .all(|mode| safety.movement_modes.contains(mode))
      {
        return Err(ProfileError(format!(
          "custom actuator type {} has incompatible safety adapter",
          definition.id
        )));
      }
    }
    self.register_builtin(definition)
  }

  pub fn register_data_type(&mut self, descriptor: DataTypeDescriptor) -> Result<(), ProfileError> {
    if !valid_contributed_data_id(&descriptor.id)
      || !descriptor
        .id
        .ends_with(&format!("@v{}", descriptor.version))
    {
      return Err(ProfileError(format!(
        "invalid contributed data type ID: {}",
        descriptor.id
      )));
    }
    self.register_builtin_data_type(descriptor)
  }

  fn register_builtin_data_type(
    &mut self,
    descriptor: DataTypeDescriptor,
  ) -> Result<(), ProfileError> {
    if descriptor.media_type.trim().is_empty()
      || descriptor.version.trim().is_empty()
      || descriptor.purpose.trim().is_empty()
      || descriptor.codec.trim().is_empty()
      || descriptor.max_payload_bytes == 0
      || !descriptor.schema.is_object()
      || self.data_types.contains_key(&descriptor.id)
    {
      return Err(ProfileError(format!(
        "invalid or duplicate data type: {}",
        descriptor.id
      )));
    }
    self.data_types.insert(descriptor.id.clone(), descriptor);
    Ok(())
  }

  pub fn register_safety_adapter(
    &mut self,
    definition: SafetyAdapterType,
  ) -> Result<(), ProfileError> {
    if !valid_segment(&definition.id)
      || !matches!(definition.kind, NodeKind::Base | NodeKind::Manipulator)
      || !definition.supports_stop
      || definition.movement_modes.is_empty()
      || !definition
        .movement_modes
        .iter()
        .all(|mode| valid_segment(mode))
    {
      return Err(ProfileError(format!(
        "invalid safety adapter type: {}",
        definition.id
      )));
    }
    if self.safety_adapters.contains_key(&definition.id) {
      return Err(ProfileError(format!(
        "duplicate safety adapter type: {}",
        definition.id
      )));
    }
    self
      .safety_adapters
      .insert(definition.id.clone(), definition);
    Ok(())
  }

  fn register_builtin(&mut self, mut definition: NodeType) -> Result<(), ProfileError> {
    if definition.description.trim().is_empty() {
      if definition.id.ends_with("@v0.1") {
        return Err(ProfileError(format!(
          "node type {} requires a nonblank description",
          definition.id
        )));
      }
      definition.description = format!("Legacy {} node type", definition.id);
    }
    let expected_prefix = format!("{}.{}.", definition.core, definition.layer);
    let type_name = definition
      .id
      .strip_prefix(&expected_prefix)
      .and_then(|name| name.split_once('@'));
    if !type_name.is_some_and(|(name, version)| valid_segment(name) && valid_type_version(version))
      || ![&definition.core, &definition.layer, &definition.group]
        .iter()
        .all(|s| valid_segment(s))
      || definition
        .inputs
        .iter()
        .chain(&definition.outputs)
        .any(|p| {
          !valid_segment(&p.name)
            || p.description.as_ref().is_some_and(|d| d.trim().is_empty())
            || p
              .expected_rate_hz
              .is_some_and(|r| !r.is_finite() || r <= 0.0)
            || !self.data_types.contains_key(p.data_type.id())
            || (definition.id.ends_with("@v0.1")
              && (p.description.is_none() || p.transport.is_none() || p.data_class.is_none()))
        })
      || definition.settings.iter().any(|s| !valid_segment(&s.name))
      || definition.capabilities.iter().any(|c| !valid_segment(c))
      || (definition.kind != NodeKind::Software
        && (definition.core != "eos"
          || definition.layer != "hardware"
          || definition.lifecycle != Lifecycle::Adapter))
      || (definition.core == "eos"
        && definition.layer == "hardware"
        && definition.kind == NodeKind::Software)
      || (definition.kind == NodeKind::Software && definition.lifecycle == Lifecycle::Adapter)
    {
      return Err(ProfileError(format!(
        "invalid node type definition: {}",
        definition.id
      )));
    }
    let mut names = BTreeSet::new();
    if !definition.inputs.iter().all(|p| names.insert(&p.name)) {
      return Err(ProfileError(format!(
        "duplicate input on {}",
        definition.id
      )));
    }
    names.clear();
    if !definition.outputs.iter().all(|p| names.insert(&p.name)) {
      return Err(ProfileError(format!(
        "duplicate output on {}",
        definition.id
      )));
    }
    names.clear();
    if !definition.settings.iter().all(|s| names.insert(&s.name)) {
      return Err(ProfileError(format!(
        "duplicate setting on {}",
        definition.id
      )));
    }
    for setting in &mut definition.settings {
      if !valid_setting_descriptor(setting, definition.id.ends_with("@v0.1")) {
        return Err(ProfileError(format!(
          "invalid setting {} on {}",
          setting.name, definition.id
        )));
      }
      if setting.description.is_none() {
        setting.description = Some(setting.name.replace('_', " "));
      }
    }
    if self.types.contains_key(&definition.id) {
      return Err(ProfileError("duplicate node type registration".into()));
    }
    self.types.insert(definition.id.clone(), definition);
    Ok(())
  }

  pub(crate) fn validate_instance(
    &self,
    node: &NodeProfile,
    definition: &NodeType,
  ) -> Result<(), ProfileError> {
    for key in node.settings.keys() {
      if !definition.settings.iter().any(|s| &s.name == key) {
        return Err(ProfileError(format!(
          "node {} has unknown setting {key}",
          node.id
        )));
      }
    }
    for setting in &definition.settings {
      match node.settings.get(&setting.name) {
        None if setting.required => {
          return Err(ProfileError(format!(
            "node {} requires setting {}",
            node.id, setting.name
          )))
        }
        None => continue,
        Some(value)
          if matches!(setting.name.as_str(), "depends_on" | "args")
            && value.as_array().is_some_and(Vec::is_empty) => {}
        Some(value)
          if valid_setting(value, setting.data_type) && valid_setting_value(value, setting) => {}
        Some(_) => {
          return Err(ProfileError(format!(
            "node {} setting {} has invalid type or value",
            node.id, setting.name
          )))
        }
      }
    }
    if !node
      .capabilities
      .iter()
      .all(|c| definition.capabilities.contains(c))
    {
      return Err(ProfileError(format!(
        "node {} advertises capability outside its type",
        node.id
      )));
    }
    if matches!(
      definition.id.as_str(),
      "isos.process.component@v1.0" | "isos.process.component@v1.1"
    ) || is_eos_backend_type(&definition.id)
    {
      if node.settings.contains_key("ready_tcp") && node.settings.contains_key("ready_unix") {
        return Err(ProfileError(format!(
          "node {} cannot combine ready_tcp and ready_unix",
          node.id
        )));
      }
      if let Some(path) = node
        .settings
        .get(if is_eos_backend_type(&definition.id) {
          "socket"
        } else {
          "ready_unix"
        })
        .and_then(toml::Value::as_str)
      {
        if !path.starts_with('/') || path.len() >= 108 {
          return Err(ProfileError(format!(
            "node {} has invalid ready_unix path",
            node.id
          )));
        }
      }
      if let Some(restart) = node.settings.get("restart").and_then(toml::Value::as_str) {
        if !["never", "on_failure", "always"].contains(&restart) {
          return Err(ProfileError(format!(
            "node {} has invalid restart policy",
            node.id
          )));
        }
      }
    }
    if definition.id == "isos.process.component@v1.1"
      && !matches!(
        node.settings.get("role").and_then(toml::Value::as_str),
        Some("eos" | "egia" | "isosui")
      )
    {
      return Err(ProfileError(format!(
        "node {} has invalid process role",
        node.id
      )));
    }
    if definition.id == "eos.process.backend@v1.0" {
      let package = node.settings.get("package").and_then(toml::Value::as_str);
      let version = node.settings.get("version").and_then(toml::Value::as_str);
      if package.is_none_or(|value| {
        value
          .split_once('/')
          .is_none_or(|(owner, name)| !valid_segment(owner) || !valid_segment(name))
      }) || version != Some("0.1.1")
      {
        return Err(ProfileError(format!(
          "node {} requires a valid supported backend package identity",
          node.id
        )));
      }
    }
    if matches!(
      definition.id.as_str(),
      "eos.process.mujoco@v1.0" | "eos.process.mujoco@v0.1"
    ) && node
      .package
      .as_ref()
      .is_none_or(|package| package.id != "island/mujoco-sim" || package.version != "0.1.1")
    {
      return Err(ProfileError(format!(
        "node {} requires island/mujoco-sim@0.1.1",
        node.id
      )));
    }
    if matches!(
      definition.id.as_str(),
      "eos.process.mujoco@v1.0" | "eos.process.mujoco@v0.1"
    ) && (node
      .settings
      .get("scene")
      .and_then(toml::Value::as_str)
      .is_none()
      || node
        .settings
        .get("socket")
        .and_then(toml::Value::as_str)
        .is_none())
    {
      return Err(ProfileError(format!(
        "node {} requires scene and socket",
        node.id
      )));
    }
    Ok(())
  }
}

pub fn is_eos_backend_type(id: &str) -> bool {
  matches!(
    id,
    "eos.process.backend@v1.0" | "eos.process.mujoco@v1.0" | "eos.process.mujoco@v0.1"
  )
}

fn valid_type_version(version: &str) -> bool {
  let Some((major, minor)) = version.strip_prefix('v').and_then(|v| v.split_once('.')) else {
    return false;
  };
  !major.is_empty()
    && !minor.is_empty()
    && major.bytes().all(|b| b.is_ascii_digit())
    && minor.bytes().all(|b| b.is_ascii_digit())
}

fn valid_contributed_data_id(id: &str) -> bool {
  let Some((name, version)) = id.rsplit_once('@') else {
    return false;
  };
  let mut segments = name.split('.');
  segments.clone().count() >= 2 && segments.all(valid_segment) && valid_type_version(version)
}

fn valid_setting(value: &toml::Value, kind: SettingType) -> bool {
  match kind {
    SettingType::String => value.as_str().is_some_and(|s| !s.trim().is_empty()),
    SettingType::PositiveInteger => value.as_integer().is_some_and(|n| n > 0),
    SettingType::NonnegativeInteger => value.as_integer().is_some_and(|n| n >= 0),
    SettingType::StringArray => value.as_array().is_some_and(|values| {
      !values.is_empty()
        && values
          .iter()
          .all(|v| v.as_str().is_some_and(|s| !s.trim().is_empty()))
    }),
    SettingType::StringTable => value.as_table().is_some_and(|table| {
      table
        .iter()
        .all(|(k, v)| valid_segment(k) && v.as_str().is_some())
    }),
    SettingType::ObjectArray => value
      .as_array()
      .is_some_and(|values| values.iter().all(|value| value.as_table().is_some())),
  }
}

fn valid_item_schema(schema: &serde_json::Value, depth: usize) -> bool {
  let Some(object) = schema.as_object() else {
    return false;
  };
  let kind = schema.get("type").and_then(serde_json::Value::as_str);
  if depth > 3 || !matches!(kind, Some("object" | "string")) {
    return false;
  }
  let allowed = [
    "type",
    "properties",
    "required",
    "additionalProperties",
    "minLength",
    "maxLength",
    "pattern",
  ];
  if object.keys().any(|key| !allowed.contains(&key.as_str())) {
    return false;
  }
  match kind {
    Some("object") => {
      if object.contains_key("minLength")
        || object.contains_key("maxLength")
        || object.contains_key("pattern")
      {
        return false;
      }
      let Some(properties) = object
        .get("properties")
        .and_then(serde_json::Value::as_object)
      else {
        return false;
      };
      if properties.len() > 64
        || properties
          .iter()
          .any(|(name, child)| name.is_empty() || !valid_item_schema(child, depth + 1))
      {
        return false;
      }
      if object
        .get("additionalProperties")
        .is_some_and(|value| !value.is_boolean())
      {
        return false;
      }
      let Some(required) = object.get("required") else {
        return true;
      };
      let Some(required) = required.as_array() else {
        return false;
      };
      let mut seen = BTreeSet::new();
      required.iter().all(|value| {
        value
          .as_str()
          .is_some_and(|name| properties.contains_key(name) && seen.insert(name))
      })
    }
    Some("string") => {
      if object.contains_key("properties")
        || object.contains_key("required")
        || object.contains_key("additionalProperties")
      {
        return false;
      }
      let min = object
        .get("minLength")
        .map(serde_json::Value::as_u64)
        .unwrap_or(Some(0));
      let max = object
        .get("maxLength")
        .map(serde_json::Value::as_u64)
        .unwrap_or(Some(4096));
      let Some((min, max)) = min.zip(max) else {
        return false;
      };
      if min > max || max > 4096 {
        return false;
      }
      object.get("pattern").is_none_or(|value| {
        value
          .as_str()
          .is_some_and(|pattern| pattern.len() <= 256 && regex::Regex::new(pattern).is_ok())
      })
    }
    _ => false,
  }
}

fn item_matches_schema(value: &serde_json::Value, schema: &serde_json::Value) -> bool {
  match schema["type"].as_str() {
    Some("object") => {
      let Some(object) = value.as_object() else {
        return false;
      };
      let properties = schema["properties"].as_object().expect("validated schema");
      if schema["required"].as_array().is_some_and(|required| {
        required
          .iter()
          .any(|key| key.as_str().is_none_or(|name| !object.contains_key(name)))
      }) {
        return false;
      }
      object.iter().all(|(name, child)| {
        properties.get(name).map_or(
          schema["additionalProperties"].as_bool() != Some(false),
          |child_schema| item_matches_schema(child, child_schema),
        )
      })
    }
    Some("string") => value.as_str().is_some_and(|text| {
      let length = text.chars().count() as u64;
      length >= schema["minLength"].as_u64().unwrap_or(0)
        && length <= schema["maxLength"].as_u64().unwrap_or(4096)
        && schema["pattern"].as_str().is_none_or(|pattern| {
          regex::Regex::new(pattern).is_ok_and(|pattern| pattern.is_match(text))
        })
    }),
    _ => false,
  }
}

fn valid_setting_descriptor(setting: &Setting, current: bool) -> bool {
  if setting
    .description
    .as_ref()
    .is_some_and(|value| value.trim().is_empty())
    || (current && setting.description.is_none())
    || setting
      .unit
      .as_ref()
      .is_some_and(|value| value.trim().is_empty())
  {
    return false;
  }
  let numeric = matches!(
    setting.data_type,
    SettingType::PositiveInteger | SettingType::NonnegativeInteger
  );
  if (setting.min.is_some() || setting.max.is_some()) && !numeric {
    return false;
  }
  let object_array = setting.data_type == SettingType::ObjectArray;
  if object_array {
    if setting.items_schema.as_ref().is_none_or(|schema| {
      serde_json::to_vec(schema).map_or(true, |bytes| bytes.len() > 8192)
        || schema["type"].as_str() != Some("object")
        || !valid_item_schema(schema, 0)
    }) || setting
      .min_items
      .zip(setting.max_items)
      .is_some_and(|(min, max)| min > max)
      || setting.max_items.is_none_or(|max| max > 256)
    {
      return false;
    }
  } else if setting.items_schema.is_some()
    || setting.min_items.is_some()
    || setting.max_items.is_some()
  {
    return false;
  }
  if setting
    .min
    .into_iter()
    .chain(setting.max)
    .any(|value| value < 0)
    || setting
      .min
      .zip(setting.max)
      .is_some_and(|(min, max)| min > max)
  {
    return false;
  }
  let mut choices = BTreeSet::new();
  if !setting.choices.is_empty()
    && (!matches!(setting.data_type, SettingType::String)
      || setting.selector.is_some()
      || setting
        .choices
        .iter()
        .any(|value| value.trim().is_empty() || !choices.insert(value)))
  {
    return false;
  }
  setting.selector.as_ref().is_none_or(|selector| {
    matches!(setting.data_type, SettingType::String)
      && matches!(
        (
          selector.provider,
          selector.resource.as_str(),
          selector.package_from
        ),
        (SelectorProvider::Package, "scene" | "device", Some(_))
          | (SelectorProvider::Registry, "adapter_family", None)
          | (SelectorProvider::Graph, "node", None)
      )
  })
}

fn valid_setting_value(value: &toml::Value, setting: &Setting) -> bool {
  if setting.data_type == SettingType::ObjectArray {
    let Some(items) = value.as_array() else {
      return false;
    };
    return setting.min_items.is_none_or(|min| items.len() >= min)
      && setting.max_items.is_none_or(|max| items.len() <= max)
      && items.iter().all(|item| {
        serde_json::to_value(item).is_ok_and(|json| {
          setting
            .items_schema
            .as_ref()
            .is_some_and(|schema| item_matches_schema(&json, schema))
        })
      });
  }
  if let Some(integer) = value.as_integer() {
    if setting.min.is_some_and(|min| integer < min) || setting.max.is_some_and(|max| integer > max)
    {
      return false;
    }
  }
  setting.choices.is_empty()
    || value
      .as_str()
      .is_some_and(|selected| setting.choices.iter().any(|choice| choice == selected))
}

fn port(name: &str, data_type: PortDataType, required: bool) -> Port {
  Port {
    name: name.into(),
    data_type,
    required,
    cardinality: PortCardinality::One,
    description: None,
    expected_rate_hz: None,
    transport: None,
    data_class: None,
  }
}

fn setting(name: &str, data_type: SettingType, required: bool) -> Setting {
  Setting {
    name: name.into(),
    data_type,
    required,
    description: None,
    unit: None,
    min: None,
    max: None,
    choices: Vec::new(),
    selector: None,
    items_schema: None,
    min_items: None,
    max_items: None,
  }
}

fn node_type(
  id: &str,
  placement: (&str, &str, &str),
  kind: NodeKind,
  lifecycle: Lifecycle,
  capabilities: &[&str],
  ports: (Vec<Port>, Vec<Port>),
) -> NodeType {
  let (core, layer, group) = placement;
  let (inputs, outputs) = ports;
  NodeType {
        id: id.into(),
        description: match id {
            "eos.hardware.camera@v1.0" => "Captures camera frames and publishes health and status.",
            "eos.hardware.base@v1.0" => "Receives admitted base commands and publishes pose, outcome, health, and status.",
            "eos.hardware.manipulator@v1.0" => "Receives admitted arm commands and publishes joint observations, outcome, health, and status.",
            "eos.dispatcher.control@v1.0" => "Admits control requests and emits typed base or arm commands and status.",
            "eos.control.manual@v1.0" => "Accepts operator control with device status and emits a governed request and outcome.",
            "eos.control.primitive@v1.0" => "Forms a typed primitive request from device state and publishes its outcome.",
            "eos.dispatcher.primitive_base@v1.0" => "Binds a declared base primitive to a typed base command output.",
            "eos.dispatcher.primitive_manipulator@v1.0" => "Binds a declared arm primitive to a typed arm command output.",
            "eos.control.skill@v1.0" => "Describes a historical skill sequence and its declared outcome port.",
            "egia.common.source@v1.0" => "Publishes a typed agent source observation for downstream stages.",
            "egia.common.context@v1.0" => "Combines source evidence into a context snapshot for planning.",
            "egia.common.planner@v1.0" => "Consumes context and emits an agent plan proposal.",
            "egia.common.guard@v1.0" => "Checks a plan proposal before guarded agent output.",
            "isos.process.component@v1.0" => "Describes a legacy managed component process and its readiness.",
            "eos.process.backend@v1.0" => "Describes a legacy EOS backend child process and its readiness.",
            "eos.interface.camera_gate@v1.0" => "Gates camera frames into a read-only vision stream.",
            "isos.ui.vision@v1.0" | "isosui.system.vision@v1.0" => "Displays a read-only vision frame in the operator UI.",
            _ => "",
        }
        .into(),
        core: core.into(),
        layer: layer.into(),
        group: group.into(),
        kind,
        lifecycle,
        capabilities: capabilities.iter().map(|c| (*c).into()).collect(),
        settings: Vec::new(),
        inputs,
        outputs,
        safety_adapter_type: None,
    }
}

fn builtin_types() -> Vec<NodeType> {
  use PortDataType::*;
  let camera = node_type(
    "eos.hardware.camera@v1.0",
    ("eos", "hardware", "camera"),
    NodeKind::Camera,
    Lifecycle::Adapter,
    &["rgb8", "aligned_depth_u16"],
    (
      vec![],
      vec![
        port("frame", CameraFrame, false),
        port("health", Health, false),
        port("status", Status, false),
      ],
    ),
  );
  let mut base = node_type(
    "eos.hardware.base@v1.0",
    ("eos", "hardware", "base"),
    NodeKind::Base,
    Lifecycle::Adapter,
    &["body_velocity", "relative_pose"],
    (
      vec![port("command", BaseMovement, true)],
      vec![
        port("observation", BaseObservation, false),
        port("outcome", CommandOutcome, false),
        port("health", Health, false),
        port("status", Status, false),
      ],
    ),
  );
  base.inputs[0].cardinality = PortCardinality::Many;
  let mut manipulator = node_type(
    "eos.hardware.manipulator@v1.0",
    ("eos", "hardware", "manipulator"),
    NodeKind::Manipulator,
    Lifecycle::Adapter,
    &["joint_positions", "end_effector_pose"],
    (
      vec![port("command", ManipulatorMovement, true)],
      vec![
        port("observation", ManipulatorObservation, false),
        port("outcome", CommandOutcome, false),
        port("health", Health, false),
        port("status", Status, false),
      ],
    ),
  );
  manipulator.inputs[0].cardinality = PortCardinality::Many;
  let dispatcher = node_type(
    "eos.dispatcher.control@v1.0",
    ("eos", "dispatcher", "control"),
    NodeKind::Software,
    Lifecycle::Internal,
    &["admission", "status"],
    (
      vec![port("request", ControlRequest, false)],
      vec![
        port("base_command", BaseMovement, false),
        port("manipulator_command", ManipulatorMovement, false),
        port("status", Status, false),
      ],
    ),
  );
  let mut manual = node_type(
    "eos.control.manual@v1.0",
    ("eos", "control", "manual"),
    NodeKind::Software,
    Lifecycle::Internal,
    &["manual_control"],
    (
      vec![
        port("status", Status, true),
        port("base_observation", BaseObservation, false),
        port("manipulator_observation", ManipulatorObservation, false),
      ],
      vec![
        port("request", ControlRequest, false),
        port("outcome", CommandOutcome, false),
      ],
    ),
  );
  manual.settings = vec![setting("target", SettingType::String, true)];
  let mut primitive = node_type(
    "eos.control.primitive@v1.0",
    ("eos", "control", "primitive"),
    NodeKind::Software,
    Lifecycle::Internal,
    &["primitive_control"],
    (
      vec![
        port("status", Status, true),
        port("base_observation", BaseObservation, false),
        port("manipulator_observation", ManipulatorObservation, false),
      ],
      vec![
        port("request", ControlRequest, false),
        port("outcome", CommandOutcome, false),
      ],
    ),
  );
  primitive.settings = vec![
    setting("target", SettingType::String, true),
    setting("movement_type", SettingType::String, true),
  ];
  let mut primitive_base = node_type(
    "eos.dispatcher.primitive_base@v1.0",
    ("eos", "dispatcher", "base"),
    NodeKind::Software,
    Lifecycle::Internal,
    &["primitive_binding"],
    (vec![], vec![port("command", BaseMovement, false)]),
  );
  primitive_base.settings = vec![
    setting("primitive_name", SettingType::String, true),
    setting("primitive_version", SettingType::String, true),
    setting("movement_type", SettingType::String, true),
  ];
  let mut primitive_manipulator = node_type(
    "eos.dispatcher.primitive_manipulator@v1.0",
    ("eos", "dispatcher", "manipulator"),
    NodeKind::Software,
    Lifecycle::Internal,
    &["primitive_binding"],
    (vec![], vec![port("command", ManipulatorMovement, false)]),
  );
  primitive_manipulator.settings = primitive_base.settings.clone();
  let mut skill = node_type(
    "eos.control.skill@v1.0",
    ("eos", "control", "skill"),
    NodeKind::Software,
    Lifecycle::Internal,
    &["skill_sequence"],
    (
      vec![port("status", Status, true)],
      vec![port("outcome", SkillOutcome, false)],
    ),
  );
  skill.settings = vec![setting("steps", SettingType::StringArray, true)];
  let source = node_type(
    "egia.common.source@v1.0",
    ("egia", "common", "data"),
    NodeKind::Software,
    Lifecycle::Internal,
    &["observation_ref"],
    (vec![], vec![port("observation", ObservationRef, false)]),
  );
  let context = node_type(
    "egia.common.context@v1.0",
    ("egia", "common", "data"),
    NodeKind::Software,
    Lifecycle::Internal,
    &["context"],
    (
      vec![port("observation", ObservationRef, false)],
      vec![port("context", ContextSnapshot, false)],
    ),
  );
  let planner = node_type(
    "egia.common.planner@v1.0",
    ("egia", "common", "process"),
    NodeKind::Software,
    Lifecycle::Internal,
    &["planning"],
    (
      vec![port("context", ContextSnapshot, false)],
      vec![port("proposal", PlanProposal, false)],
    ),
  );
  let guard = node_type(
    "egia.common.guard@v1.0",
    ("egia", "common", "process"),
    NodeKind::Software,
    Lifecycle::Internal,
    &["request_guard"],
    (
      vec![port("proposal", PlanProposal, false)],
      vec![port("request", ControlRequest, false)],
    ),
  );
  let mut process = node_type(
    "isos.process.component@v1.0",
    ("isos", "process", "component"),
    NodeKind::Software,
    Lifecycle::Process,
    &["process"],
    (
      vec![],
      vec![port("status", Status, false), port("health", Health, false)],
    ),
  );
  process.settings = vec![
    setting("command", SettingType::StringArray, true),
    setting("cwd", SettingType::String, false),
    setting("env", SettingType::StringTable, false),
    setting("ready_tcp", SettingType::String, false),
    setting("ready_unix", SettingType::String, false),
    setting("start_timeout_ms", SettingType::PositiveInteger, false),
    setting("restart", SettingType::String, false),
    setting("max_restarts", SettingType::NonnegativeInteger, false),
    setting("depends_on", SettingType::StringArray, false),
  ];
  let mut process_v1_1 = process.clone();
  process_v1_1.id = "isos.process.component@v1.1".into();
  process_v1_1.description =
    "Declares a role-selected managed process and publishes readiness and health.".into();
  process_v1_1
    .settings
    .retain(|setting| setting.name != "command");
  process_v1_1
    .settings
    .push(setting("role", SettingType::String, true));
  process_v1_1
    .settings
    .push(setting("args", SettingType::StringArray, false));
  let mut backend = node_type(
    "eos.process.backend@v1.0",
    ("eos", "process", "backend"),
    NodeKind::Software,
    Lifecycle::Process,
    &["process"],
    (
      vec![],
      vec![port("status", Status, false), port("health", Health, false)],
    ),
  );
  backend.settings = vec![
    setting("package", SettingType::String, true),
    setting("version", SettingType::String, true),
    setting("scene", SettingType::String, true),
    setting("socket", SettingType::String, true),
    setting("env", SettingType::StringTable, false),
    setting("start_timeout_ms", SettingType::PositiveInteger, false),
  ];
  let mut gate = node_type(
    "eos.interface.camera_gate@v1.0",
    ("eos", "interface", "gate"),
    NodeKind::Software,
    Lifecycle::Internal,
    &["vision_preview"],
    (
      vec![port("frame", CameraFrame, true)],
      vec![port("preview", VisionFrame, false)],
    ),
  );
  gate.inputs[0].data_class = Some(DataClass::Image);
  gate.inputs[0].transport = Some(PortTransport::InProcess);
  gate.outputs[0].data_class = Some(DataClass::Image);
  gate.outputs[0].transport = Some(PortTransport::Websocket);
  let mut vision = node_type(
    "isos.ui.vision@v1.0",
    ("isos", "ui", "panel"),
    NodeKind::Software,
    Lifecycle::Internal,
    &["vision_display"],
    (vec![port("preview", VisionFrame, false)], vec![]),
  );
  vision.settings = vec![
    setting("target_fps", SettingType::PositiveInteger, false),
    setting("max_width_px", SettingType::PositiveInteger, false),
    setting("jpeg_quality", SettingType::PositiveInteger, false),
  ];
  vision.inputs[0].data_class = Some(DataClass::Image);
  vision.inputs[0].transport = Some(PortTransport::Websocket);
  let mut legacy_vision = node_type(
    "isosui.system.vision@v1.0",
    ("isosui", "system", "ui"),
    NodeKind::Software,
    Lifecycle::Internal,
    &["vision_display"],
    (vec![port("preview", VisionFrame, false)], vec![]),
  );
  legacy_vision.settings = vec![
    setting("target_fps", SettingType::PositiveInteger, false),
    setting("max_width_px", SettingType::PositiveInteger, false),
    setting("jpeg_quality", SettingType::PositiveInteger, false),
  ];
  legacy_vision.inputs[0].data_class = Some(DataClass::Image);
  legacy_vision.inputs[0].transport = Some(PortTransport::Websocket);
  vec![
    camera,
    base,
    manipulator,
    dispatcher,
    manual,
    primitive,
    primitive_base,
    primitive_manipulator,
    skill,
    source,
    context,
    planner,
    guard,
    process,
    process_v1_1,
    backend,
    gate,
    vision,
    legacy_vision,
  ]
}

fn target_types(legacy_types: &[NodeType]) -> Vec<NodeType> {
  legacy_types
    .iter()
    .filter_map(|legacy| {
      let id = match legacy.id.as_str() {
        "eos.hardware.camera@v1.0" => "eos.hardware.camera@v0.1",
        "eos.hardware.base@v1.0" => "eos.hardware.base@v0.1",
        "eos.hardware.manipulator@v1.0" => "eos.hardware.manipulator@v0.1",
        "eos.dispatcher.control@v1.0" => "eos.dispatcher.control@v0.1",
        "eos.dispatcher.primitive_base@v1.0" => "eos.dispatcher.primitive_base@v0.1",
        "eos.dispatcher.primitive_manipulator@v1.0" => "eos.dispatcher.primitive_manipulator@v0.1",
        "eos.interface.camera_gate@v1.0" => "eos.interface.camera_gate@v0.1",
        _ => return None,
      };
      let mut target = legacy.clone();
      target.id = id.into();
      for setting in &mut target.settings {
        setting.description = Some(
          match setting.name.as_str() {
            "primitive_name" => "Registered primitive operation name",
            "primitive_version" => "Exact primitive operation version",
            "movement_type" => "Movement mode supported by the selected device",
            _ => unreachable!("target built-in setting"),
          }
          .into(),
        );
        setting.choices = vec![match (target.id.as_str(), setting.name.as_str()) {
          ("eos.dispatcher.primitive_base@v0.1", "primitive_name") => "base.velocity",
          ("eos.dispatcher.primitive_manipulator@v0.1", "primitive_name") => "arm.movej",
          (_, "primitive_version") => "1.0.0",
          ("eos.dispatcher.primitive_base@v0.1", "movement_type") => "body_velocity",
          ("eos.dispatcher.primitive_manipulator@v0.1", "movement_type") => "joint_positions",
          _ => unreachable!("target built-in choice"),
        }
        .into()];
      }
      for port in target.inputs.iter_mut().chain(&mut target.outputs) {
        port.description = Some(builtin_data_purpose(port.data_type.id()).into());
        port.transport = Some(
          if target.id == "eos.interface.camera_gate@v0.1" && port.name == "preview" {
            PortTransport::Websocket
          } else {
            PortTransport::InProcess
          },
        );
        port.data_class = Some(match port.data_type {
          PortDataType::CameraFrame | PortDataType::VisionFrame => DataClass::Image,
          PortDataType::BaseObservation | PortDataType::ManipulatorObservation => {
            DataClass::Observation
          }
          PortDataType::BaseMovement
          | PortDataType::ManipulatorMovement
          | PortDataType::ControlRequest => DataClass::Command,
          PortDataType::CommandOutcome | PortDataType::SkillOutcome => DataClass::Event,
          PortDataType::Health | PortDataType::Status => DataClass::State,
          _ => DataClass::Opaque,
        });
      }
      Some(target)
    })
    .collect()
}

fn builtin_data_purpose(id: &str) -> &'static str {
  match id {
    "camera_frame" => "RGB-D camera sample with source provenance and calibration",
    "vision_frame" => "JPEG preview derived from a camera gate",
    "base_observation" => "Mobile base pose and velocity sample",
    "manipulator_observation" => "Joint and end effector sample",
    "base_movement" => "Admitted mobile base movement request",
    "manipulator_movement" => "Admitted manipulator movement request",
    "control_request" => "Typed request for the EOS command dispatcher",
    "command_outcome" => "Result of a dispatched hardware command",
    "skill_outcome" => "Result of a composed skill execution",
    "health" => "Adapter health and last successful observation",
    "status" => "Live node lifecycle and active command",
    "observation_ref" => "Reference to a source observation",
    "context_snapshot" => "Bounded EGIA context snapshot",
    "plan_proposal" => "EGIA proposed plan requiring admission",
    _ => unreachable!("built-in data type"),
  }
}

fn builtin_data_schema(id: &str) -> serde_json::Value {
  use serde_json::json;
  let properties = match id {
    "camera_frame" => json!({
        "meta":{"type":"object","description":"Source timestamp, clock, receive time, sequence and frame ID"},
        "width_px":{"type":"integer","minimum":1,"x-unit":"px","description":"RGB width"},
        "height_px":{"type":"integer","minimum":1,"x-unit":"px","description":"RGB height"},
        "stride_bytes":{"type":"integer","minimum":1,"x-unit":"bytes/row","description":"RGB row stride"},
        "rgb8":{"type":"array","description":"Interleaved 8-bit RGB pixels"},
        "depth":{"type":["object","null"],"description":"Optional aligned u16 depth, metres per unit and intrinsics"}
    }),
    "vision_frame" => json!({
        "gate_path":{"type":"string","description":"Canonical EOS camera gate path"},
        "sequence":{"type":["integer","null"],"minimum":0,"description":"Source camera sequence"},
        "timestamp_unix_ms":{"type":["integer","null"],"x-unit":"ms","description":"Source Unix timestamp when available"},
        "width_px":{"type":"integer","minimum":1,"x-unit":"px","description":"JPEG pixel width"},
        "height_px":{"type":"integer","minimum":1,"x-unit":"px","description":"JPEG pixel height"},
        "source_rate_hz":{"type":"number","exclusiveMinimum":0,"x-unit":"Hz","description":"Declared source camera rate"},
        "jpeg":{"type":"string","contentEncoding":"binary","description":"JPEG image bytes, at most 8 MiB"}
    }),
    "base_observation" => json!({
        "meta":{"type":"object","description":"Source sample metadata"},
        "valid":{"type":"boolean","description":"Whether pose and velocity are valid"},
        "x_m":{"type":"number","x-unit":"m","description":"Base x position"},
        "y_m":{"type":"number","x-unit":"m","description":"Base y position"},
        "yaw_rad":{"type":"number","x-unit":"rad","description":"Base heading"},
        "x_m_s":{"type":"number","x-unit":"m/s","description":"Longitudinal velocity"},
        "y_m_s":{"type":"number","x-unit":"m/s","description":"Lateral velocity"},
        "yaw_rad_s":{"type":"number","x-unit":"rad/s","description":"Angular velocity"}
    }),
    "manipulator_observation" => json!({
        "meta":{"type":"object","description":"Source sample metadata"},
        "joint_names":{"type":"array","items":{"type":"string"},"description":"Ordered joint names"},
        "joint_positions_rad":{"type":"array","items":{"type":"number"},"x-unit":"rad","description":"Ordered joint angles"},
        "joint_velocities_rad_s":{"type":"array","items":{"type":"number"},"x-unit":"rad/s","description":"Ordered joint velocities"},
        "joints_valid":{"type":"boolean","description":"Whether joint values are valid"},
        "end_effector_pose":{"type":["object","null"],"description":"Optional Cartesian position, orientation and frame"},
        "end_effector_valid":{"type":"boolean","description":"Whether end effector pose is valid"}
    }),
    "base_movement" => json!({
        "body_velocity":{"type":"object","description":"x and y velocity in m/s, yaw rate in rad/s, lease in ms"},
        "relative_pose":{"type":"object","description":"Relative x/y metres, yaw radians and frame ID"}
    }),
    "manipulator_movement" => json!({
        "joint_positions":{"type":"object","description":"Joint names, target angles in rad, optional speed rad/s"},
        "end_effector_pose":{"type":"object","description":"Target Cartesian pose and optional speed m/s"}
    }),
    "health" => json!({
        "state":{"type":"string","enum":["healthy","degraded","unavailable"],"description":"Current adapter health"},
        "code":{"type":["string","null"],"description":"Optional machine code"},
        "message":{"type":["string","null"],"description":"Optional operator explanation"},
        "last_ok_monotonic_ns":{"type":["integer","null"],"minimum":0,"x-unit":"ns","description":"Last successful local sample time"}
    }),
    "status" => json!({
        "state":{"type":"string","enum":["starting","ready","stopped","faulted"],"description":"Node lifecycle state"},
        "active_command_id":{"type":["string","null"],"description":"Current admitted command ID"}
    }),
    "control_request" => {
      json!({"node_path":{"type":"string","description":"Target graph path"},"command_id":{"type":"string","description":"Idempotency key"},"deadline_monotonic_ns":{"type":"integer","minimum":0,"x-unit":"ns","description":"Local admission deadline"}})
    }
    "command_outcome" | "skill_outcome" => {
      json!({"state":{"type":"string","description":"Accepted, completed, cancelled or uncertain result"},"message":{"type":["string","null"],"description":"Optional result detail"}})
    }
    "observation_ref" => {
      json!({"source_node":{"type":"string","description":"Source graph path"},"port":{"type":"string","description":"Source output port"},"sequence":{"type":["integer","null"],"description":"Optional source sequence"}})
    }
    "context_snapshot" => {
      json!({"observations":{"type":"array","description":"Bounded referenced observations"},"created_monotonic_ns":{"type":"integer","minimum":0,"x-unit":"ns","description":"Snapshot creation time"}})
    }
    "plan_proposal" => {
      json!({"steps":{"type":"array","description":"Proposed typed steps"},"source_context":{"type":"string","description":"Context snapshot reference"}})
    }
    _ => unreachable!("built-in data type"),
  };
  json!({"type":"object","description":builtin_data_purpose(id),"properties":properties})
}

#[cfg(test)]
mod object_array_tests {
  use super::*;

  #[test]
  fn nested_items_are_checked_without_coercion_or_extra_fields() {
    let mut setting = setting("skill_libraries", SettingType::ObjectArray, false);
    setting.description = Some("Exact package libraries".into());
    setting.items_schema = Some(serde_json::json!({
      "type":"object","additionalProperties":false,
      "required":["package","library"],
      "properties":{
        "package":{"type":"object","additionalProperties":false,
          "required":["id","version"],
          "properties":{"id":{"type":"string","minLength":1},
            "version":{"type":"string","minLength":1}}},
        "library":{"type":"string","minLength":1,"maxLength":256}
      }
    }));
    setting.min_items = Some(0);
    setting.max_items = Some(16);
    assert!(valid_setting_descriptor(&setting, true));
    let good = toml::Value::try_from(serde_json::json!([{
      "package":{"id":"island/example","version":"0.1.0"},"library":"lib/a.so"
    }]))
    .unwrap();
    assert!(valid_setting(&good, setting.data_type));
    assert!(valid_setting_value(&good, &setting));
    for invalid in [
      serde_json::json!([{"package":{"id":"island/example","version":"0.1.0"}}]),
      serde_json::json!([{"package":{"id":"island/example","version":"0.1.0"},"library":8}]),
      serde_json::json!([{"package":{"id":"island/example","version":"0.1.0","extra":"x"},"library":"lib/a.so"}]),
      serde_json::json!([{"package":{"id":"","version":"0.1.0"},"library":"lib/a.so"}]),
    ] {
      let value = toml::Value::try_from(invalid).unwrap();
      assert!(!valid_setting_value(&value, &setting));
    }
    let seventeen = toml::Value::Array(vec![good.as_array().unwrap()[0].clone(); 17]);
    assert!(!valid_setting_value(&seventeen, &setting));
    setting.items_schema = Some(serde_json::json!({"type":"string"}));
    assert!(!valid_setting_descriptor(&setting, true));
  }
}
