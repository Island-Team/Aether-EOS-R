//! EOS-owned graph edge admission. IsOS validates the complete cross-core graph.

use std::collections::{BTreeMap, BTreeSet};

use crate::{
  DataClass, NodeProfile, NodeTypeRegistry, PortSource, PortSources, PortTransport, Profile,
  ProfileError,
};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EosEdgeClass {
  /// Candidate for the R01 in-process evidence route; this does not deliver data yet.
  EosLocal,
  /// Fixed Dispatcher-governed command or state path; never user-scheduled.
  FixedSafety,
  /// Historical or registered descriptive edge with no EOS delivery promise.
  DesignOnly,
}

#[derive(Clone, Debug, PartialEq)]
pub struct EosEdgeContract {
  pub source_node: String,
  pub source_output: String,
  pub target_node: String,
  pub target_input: String,
  pub data_type: String,
  pub transport: PortTransport,
  pub class: EosEdgeClass,
  pub priority: i64,
  pub frequency_hz: Option<f64>,
}

fn pointer(node: &NodeProfile, input: &str, index: Option<usize>) -> String {
  let parts: Vec<_> = node.path().split('/').collect();
  let mut path = format!(
    "/cores/{}/layers/{}/groups/{}/nodes/{}/inputs/{input}",
    parts[0], parts[1], parts[2], parts[3]
  );
  if let Some(index) = index {
    path.push_str(&format!("/{index}"));
  }
  path
}

fn is_fixed_data(data_type: &str) -> bool {
  matches!(
    data_type,
    "base_movement"
      | "manipulator_movement"
      | "control_request"
      | "command_outcome"
      | "skill_outcome"
      | "health"
      | "status"
  )
}

fn class_for(
  source: &NodeProfile,
  output: &str,
  target: &NodeProfile,
  input: &str,
  data_type: &str,
  class: Option<DataClass>,
) -> EosEdgeClass {
  if is_fixed_data(data_type) || class == Some(DataClass::Command) {
    return EosEdgeClass::FixedSafety;
  }
  if matches!(
    source.node_type.as_str(),
    "eos.hardware.camera@v0.1" | "isos.eos_hardware.camera@v0.2"
  ) && output == "frame"
    && matches!(
      target.node_type.as_str(),
      "eos.interface.camera_gate@v0.1" | "isos.eos_interface.camera_gate@v0.2"
    )
    && input == "frame"
    && class == Some(DataClass::Image)
  {
    return EosEdgeClass::EosLocal;
  }
  EosEdgeClass::DesignOnly
}

fn validate_qos(source: &PortSource, class: EosEdgeClass, path: &str) -> Result<(), ProfileError> {
  if let Some(priority) = source.priority {
    if !(-20..=19).contains(&priority) {
      return Err(ProfileError(format!(
        "{path}/priority: priority must be -20..19"
      )));
    }
    if class != EosEdgeClass::EosLocal {
      return Err(ProfileError(format!(
        "{path}/priority: user priority is forbidden on a fixed or unrouted edge"
      )));
    }
  }
  if let Some(rate) = source.frequency_hz {
    if !rate.is_finite() || rate <= 0.0 {
      return Err(ProfileError(format!(
        "{path}/frequency_hz: frequency must be finite and positive"
      )));
    }
    if class != EosEdgeClass::EosLocal {
      return Err(ProfileError(format!(
        "{path}/frequency_hz: user frequency cap is forbidden on a fixed or unrouted edge"
      )));
    }
  }
  Ok(())
}

fn visit(
  node: &str,
  successors: &BTreeMap<String, Vec<String>>,
  active: &mut BTreeSet<String>,
  done: &mut BTreeSet<String>,
) -> Result<(), ProfileError> {
  if done.contains(node) {
    return Ok(());
  }
  if !active.insert(node.to_owned()) {
    return Err(ProfileError(format!("EOS edge cycle reaches {node}")));
  }
  for next in successors.get(node).into_iter().flatten() {
    visit(next, successors, active, done)?;
  }
  active.remove(node);
  done.insert(node.to_owned());
  Ok(())
}

pub(crate) fn validate(
  profile: &Profile,
  registry: &NodeTypeRegistry,
) -> Result<Vec<EosEdgeContract>, ProfileError> {
  let mut edges = Vec::new();
  let mut successors: BTreeMap<String, Vec<String>> = BTreeMap::new();
  for target in profile
    .all_nodes()
    .filter(|node| node.path().starts_with("eos/") || node.path().starts_with("isos/eos_"))
  {
    let target_type = registry
      .get(&target.node_type)
      .ok_or_else(|| ProfileError(format!("node {} has unregistered EOS type", target.path())))?;
    for (input_name, sources) in &target.inputs {
      let input = target_type
        .inputs
        .iter()
        .find(|port| port.name == *input_name)
        .ok_or_else(|| {
          ProfileError(format!(
            "node {} has unknown input {input_name}",
            target.path()
          ))
        })?;
      for (index, source) in sources.values().iter().enumerate() {
        let path = pointer(
          target,
          input_name,
          match sources {
            PortSources::One(_) => None,
            PortSources::Many(_) => Some(index),
          },
        );
        if profile.node(&source.node).is_none()
          && crate::external_bridge_input(target, target_type, input, input_name, &source.node)
        {
          validate_qos(source, EosEdgeClass::FixedSafety, &path)?;
          continue;
        }
        let producer = profile
          .node(&source.node)
          .ok_or_else(|| ProfileError(format!("{path}: missing source node {}", source.node)))?;
        let producer_type = registry.get(&producer.node_type).ok_or_else(|| {
          ProfileError(format!(
            "{path}: unregistered source type {}",
            producer.node_type
          ))
        })?;
        let output = producer_type
          .outputs
          .iter()
          .find(|port| port.name == source.output)
          .ok_or_else(|| {
            ProfileError(format!("{path}: unknown source output {}", source.output))
          })?;
        let input_transport = input.transport.unwrap_or(PortTransport::InProcess);
        let output_transport = output.transport.unwrap_or(PortTransport::InProcess);
        if input_transport != output_transport {
          return Err(ProfileError(format!(
            "{path}: source and input transport differ"
          )));
        }
        if input.delivery_mode != output.delivery_mode {
          return Err(ProfileError(format!(
            "{path}: source and input delivery mode differ"
          )));
        }
        if input.data_class.is_some()
          && output.data_class.is_some()
          && input.data_class != output.data_class
        {
          return Err(ProfileError(format!(
            "{path}: source and input data class differ"
          )));
        }
        let class =
          if !producer.path().starts_with("eos/") && !producer.path().starts_with("isos/eos_") {
            if profile.version == crate::GRAPH_VERSION
              && producer.node_type == "egia.common.guard@v1.0"
              && target.node_type == "eos.dispatcher.control@v1.0"
              && input_name == "request"
              && source.output == "request"
            {
              EosEdgeClass::DesignOnly
            } else {
              return Err(ProfileError(format!(
                "{path}: cross-core edge lacks a declared bridge"
              )));
            }
          } else {
            if input_transport != PortTransport::InProcess {
              return Err(ProfileError(format!(
                "{path}: EOS-local transport is unavailable"
              )));
            }
            let class = class_for(
              producer,
              &source.output,
              target,
              input_name,
              input.data_type.id(),
              input.data_class.or(output.data_class),
            );
            successors
              .entry(source.node.clone())
              .or_default()
              .push(target.path().to_owned());
            class
          };
        validate_qos(source, class, &path)?;
        edges.push(EosEdgeContract {
          source_node: source.node.clone(),
          source_output: source.output.clone(),
          target_node: target.path().to_owned(),
          target_input: input_name.clone(),
          data_type: input.data_type.id().to_owned(),
          transport: input_transport,
          class,
          priority: source.priority.unwrap_or(0),
          frequency_hz: source.frequency_hz,
        });
      }
    }
  }
  let mut done = BTreeSet::new();
  for node in successors.keys() {
    visit(node, &successors, &mut BTreeSet::new(), &mut done)?;
  }
  Ok(edges)
}

impl Profile {
  /// Return the validated EOS edge contract for R01 route construction.
  pub fn eos_edge_contract(
    &self,
    registry: &NodeTypeRegistry,
  ) -> Result<Vec<EosEdgeContract>, ProfileError> {
    let mut checked = self.clone();
    checked.validate_with_registry(registry)?;
    validate(&checked, registry)
  }
}
