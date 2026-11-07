//! Bounded event records parsed from owner pipes and stored logs.

use serde::{
  Deserialize, Deserializer,
  de::{MapAccess, Visitor},
};
use serde_json::{Map, Value};
use std::fmt;

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct RawEvent {
  pub(crate) timestamp: String,
  pub(crate) level: String,
  pub(crate) source: String,
  pub(crate) core: String,
  #[serde(default)]
  pub(crate) node_path: Option<String>,
  pub(crate) event_code: String,
  pub(crate) run_id: String,
  #[serde(default)]
  pub(crate) correlation_id: Option<String>,
  pub(crate) message: String,
  #[serde(default, deserialize_with = "unique_fields")]
  pub(crate) fields: Map<String, Value>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
#[allow(dead_code)] // Used to reject duplicate and unknown stored keys during deserialization.
pub(crate) struct StrictStoredEvent {
  timestamp: String,
  level: String,
  source: String,
  core: String,
  node_path: Option<String>,
  event_code: String,
  run_id: String,
  correlation_id: Option<String>,
  message: String,
  #[serde(deserialize_with = "unique_fields")]
  fields: Map<String, Value>,
  pid: Option<u32>,
}

fn unique_fields<'de, D: Deserializer<'de>>(
  deserializer: D,
) -> std::result::Result<Map<String, Value>, D::Error> {
  struct UniqueFields;
  impl<'de> Visitor<'de> for UniqueFields {
    type Value = Map<String, Value>;
    fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
      f.write_str("an object without duplicate fields")
    }
    fn visit_map<M: MapAccess<'de>>(
      self,
      mut access: M,
    ) -> std::result::Result<Self::Value, M::Error> {
      let mut fields = Map::new();
      while let Some((key, value)) = access.next_entry::<String, Value>()? {
        if fields.insert(key, value).is_some() {
          return Err(serde::de::Error::custom("duplicate event field"));
        }
      }
      Ok(fields)
    }
  }
  deserializer.deserialize_map(UniqueFields)
}
