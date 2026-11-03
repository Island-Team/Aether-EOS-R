use std::any::Any;
use std::collections::BTreeMap;
use std::marker::PhantomData;
use std::sync::{Arc, RwLock};

use crate::{HalError, NodeDataEnvelope, NodePayload, NodeProfile, NodeTypeRegistry};

/// Retains only the newest typed value for each registered output port.
#[derive(Default)]
pub struct TypedDataHub {
  latest: RwLock<BTreeMap<(String, String), HubSlot>>,
}

struct HubSlot {
  generation: u64,
  value: Arc<dyn Any + Send + Sync>,
}

/// A cursor into a single registered output. Slow consumers observe its newest value only.
pub struct HubSubscription<T> {
  hub: Arc<TypedDataHub>,
  node_path: String,
  port: String,
  generation: u64,
  payload: PhantomData<T>,
}

impl<T: NodePayload + Send + Sync + 'static> HubSubscription<T> {
  pub fn poll_latest(&mut self) -> Result<Option<Arc<NodeDataEnvelope<T>>>, HalError> {
    let latest = self
      .hub
      .latest
      .read()
      .map_err(|_| HalError::InvalidData("data hub lock poisoned".into()))?;
    let Some(slot) = latest.get(&(self.node_path.clone(), self.port.clone())) else {
      return Ok(None);
    };
    if slot.generation == self.generation {
      return Ok(None);
    }
    self.generation = slot.generation;
    slot
      .value
      .clone()
      .downcast::<NodeDataEnvelope<T>>()
      .map(Some)
      .map_err(|_| HalError::InvalidData("data hub payload type does not match port".into()))
  }
}

impl TypedDataHub {
  pub fn publish<T: NodePayload + Send + Sync + 'static>(
    &self,
    node: &NodeProfile,
    registry: &NodeTypeRegistry,
    port: &str,
    payload: T,
  ) -> Result<(), HalError> {
    let envelope = Arc::new(NodeDataEnvelope::output(node, registry, port, payload)?);
    let mut latest = self
      .latest
      .write()
      .map_err(|_| HalError::InvalidData("data hub lock poisoned".into()))?;
    let key = (node.path().into(), port.into());
    let generation = latest
      .get(&key)
      .map_or(1, |slot| slot.generation.wrapping_add(1).max(1));
    latest.insert(
      key,
      HubSlot {
        generation,
        value: envelope,
      },
    );
    Ok(())
  }

  pub fn subscribe<T: NodePayload + Send + Sync + 'static>(
    self: &Arc<Self>,
    node: &NodeProfile,
    registry: &NodeTypeRegistry,
    port: &str,
  ) -> Result<HubSubscription<T>, HalError> {
    let definition = registry
      .get(&node.node_type)
      .ok_or_else(|| HalError::InvalidData("unregistered node type".into()))?;
    if node.path().is_empty()
      || !definition
        .outputs
        .iter()
        .any(|p| p.name == port && p.data_type == T::DATA_TYPE)
    {
      return Err(HalError::InvalidData(
        "subscription port or payload type mismatch".into(),
      ));
    }
    Ok(HubSubscription {
      hub: self.clone(),
      node_path: node.path().into(),
      port: port.into(),
      generation: 0,
      payload: PhantomData,
    })
  }

  pub fn latest<T: NodePayload + Send + Sync + 'static>(
    &self,
    node_path: &str,
    port: &str,
  ) -> Result<Option<Arc<NodeDataEnvelope<T>>>, HalError> {
    let value = self
      .latest
      .read()
      .map_err(|_| HalError::InvalidData("data hub lock poisoned".into()))?
      .get(&(node_path.into(), port.into()))
      .map(|slot| slot.value.clone());
    value
      .map(|value| {
        value
          .downcast::<NodeDataEnvelope<T>>()
          .map_err(|_| HalError::InvalidData("data hub payload type does not match port".into()))
      })
      .transpose()
  }
}
