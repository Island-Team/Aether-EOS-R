//! Private per-run grant verifier wire record.

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct Verifier {
  pub(crate) schema_version: u32,
  pub(crate) run_id: String,
  pub(crate) active_revision: String,
  pub(crate) egia_control_sha256: String,
  pub(crate) operator_control_sha256: String,
}
