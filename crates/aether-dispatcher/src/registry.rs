//! EOS hardware nodes, indexed by their profile path and stable node ID.

use std::collections::{BTreeMap, BTreeSet};
use std::sync::{Arc, Mutex};

use aether_hal::{
  BaseNode, CameraNode, Health, HealthState, ManipulatorNode, NodeKind, NodeProfile, Profile,
};

use crate::DispatchError;

/// One adapter instance belongs to exactly one configured node.
#[derive(Clone)]
pub enum AdapterRegistration {
  Camera {
    node_id: String,
    adapter: Arc<dyn CameraNode>,
  },
  Base {
    node_id: String,
    adapter: Arc<dyn BaseNode>,
  },
  Manipulator {
    node_id: String,
    adapter: Arc<dyn ManipulatorNode>,
  },
}

impl AdapterRegistration {
  pub fn node_id(&self) -> &str {
    match self {
      Self::Camera { node_id, .. }
      | Self::Base { node_id, .. }
      | Self::Manipulator { node_id, .. } => node_id,
    }
  }

  pub fn adapter_type(&self) -> &str {
    match self {
      Self::Camera { adapter, .. } => adapter.adapter_type(),
      Self::Base { adapter, .. } => adapter.adapter_type(),
      Self::Manipulator { adapter, .. } => adapter.adapter_type(),
    }
  }

  pub fn reported_kind(&self) -> NodeKind {
    match self {
      Self::Camera { adapter, .. } => adapter.kind(),
      Self::Base { adapter, .. } => adapter.kind(),
      Self::Manipulator { adapter, .. } => adapter.kind(),
    }
  }

  pub fn kind(&self) -> NodeKind {
    match self {
      Self::Camera { .. } => NodeKind::Camera,
      Self::Base { .. } => NodeKind::Base,
      Self::Manipulator { .. } => NodeKind::Manipulator,
    }
  }
}

/// The interfaces visible beneath a node's path.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum NodeFacet {
  Status,
  Health,
  Observation,
  Movement,
  Safety,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct NodeDescriptor {
  pub path: String,
  pub node_id: String,
  pub kind: NodeKind,
  pub label: String,
  pub facets: Vec<NodeFacet>,
  pub capabilities: Vec<String>,
}

pub(crate) struct NodeEntry {
  pub profile: NodeProfile,
  pub adapter: AdapterRegistration,
  pub operation_lock: Mutex<()>,
  read_fault: Mutex<Option<String>>,
}

impl NodeEntry {
  pub fn health(&self) -> Health {
    let mut reported = match &self.adapter {
      AdapterRegistration::Camera { adapter, .. } => adapter.health(),
      AdapterRegistration::Base { adapter, .. } => adapter.health(),
      AdapterRegistration::Manipulator { adapter, .. } => adapter.health(),
    };
    if let Some(reason) = self
      .read_fault
      .lock()
      .expect("health lock poisoned")
      .as_ref()
    {
      reported.state = HealthState::Degraded;
      reported.code = Some("observation_failure".into());
      reported.message = Some(reason.clone());
    }
    reported
  }

  pub fn record_read_fault(&self, reason: String) {
    *self.read_fault.lock().expect("health lock poisoned") = Some(reason);
  }

  pub fn clear_read_fault(&self) {
    *self.read_fault.lock().expect("health lock poisoned") = None;
  }
}

pub(crate) struct Registry {
  pub nodes: BTreeMap<String, NodeEntry>,
}

impl Registry {
  pub fn new(
    profile: &Profile,
    registrations: Vec<AdapterRegistration>,
  ) -> Result<Self, DispatchError> {
    let configured: BTreeMap<String, NodeProfile> = profile
      .hardware_nodes()
      .map(|node| (node.id.clone(), node.clone()))
      .collect();
    let mut nodes = BTreeMap::new();
    let mut seen = BTreeSet::new();
    for adapter in registrations {
      let id = adapter.node_id().to_owned();
      if !seen.insert(id.clone()) {
        return Err(DispatchError::Configuration(format!(
          "duplicate adapter: {id}"
        )));
      }
      let node = configured.get(&id).ok_or_else(|| {
        DispatchError::Configuration(format!("adapter for unconfigured node: {id}"))
      })?;
      if node.kind != adapter.kind() || adapter.reported_kind() != adapter.kind() {
        return Err(DispatchError::Configuration(format!(
          "adapter kind disagrees with profile for node: {id}"
        )));
      }
      if node.adapter.adapter_type != adapter.adapter_type() {
        return Err(DispatchError::Configuration(format!(
          "adapter type disagrees with profile for node: {id}"
        )));
      }
      nodes.insert(
        node.path().to_owned(),
        NodeEntry {
          profile: node.clone(),
          adapter,
          operation_lock: Mutex::new(()),
          read_fault: Mutex::new(None),
        },
      );
    }
    for id in configured.keys() {
      if !seen.contains(id) {
        return Err(DispatchError::Configuration(format!(
          "missing adapter for node: {id}"
        )));
      }
    }
    Ok(Self { nodes })
  }

  pub fn discover(&self) -> Vec<NodeDescriptor> {
    self
      .nodes
      .values()
      .map(|entry| {
        let mut facets = vec![NodeFacet::Status, NodeFacet::Health];
        if entry.profile.observation.is_some() {
          facets.push(NodeFacet::Observation);
        }
        if entry.profile.movement.is_some() {
          facets.extend([NodeFacet::Movement, NodeFacet::Safety]);
        }
        NodeDescriptor {
          path: entry.profile.path().to_owned(),
          node_id: entry.profile.id.clone(),
          kind: entry.profile.kind,
          label: entry.profile.label.clone(),
          facets,
          capabilities: entry.profile.capabilities.clone(),
        }
      })
      .collect()
  }
}
