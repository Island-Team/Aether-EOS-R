//! Typed schema-2 core setup reply admitted from a selected owner.

use serde::Deserialize;
use serde_json::Value;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct SetupDiagnostic {
  pub(crate) path: String,
  pub(crate) code: String,
  pub(crate) message: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct SetupFile {
  pub(crate) path: String,
  pub(crate) content: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct SetupReply {
  pub(crate) schema_version: u32,
  pub(crate) ok: bool,
  pub(crate) diagnostics: Vec<SetupDiagnostic>,
  pub(crate) files: Vec<SetupFile>,
  #[serde(default)]
  pub(crate) identity: Option<Value>,
  #[serde(default)]
  pub(crate) catalog: Option<Value>,
}
