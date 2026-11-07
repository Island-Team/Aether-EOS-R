//! Bound package process handoff sent to a selected owner.

use crate::domain::launch::{PackageIdentity, ResourceSelection};
use serde::Serialize;
use std::{collections::BTreeMap, path::PathBuf};

#[derive(Serialize)]
pub(crate) struct ProcessHandoff<'a> {
  pub(crate) contract: &'static str,
  pub(crate) schema_version: &'static str,
  pub(crate) owner_core: &'a str,
  pub(crate) run_id: &'a str,
  pub(crate) graph_revision: &'a str,
  pub(crate) node_path: &'a str,
  pub(crate) node_type: &'a str,
  pub(crate) package: &'a PackageIdentity,
  pub(crate) entrypoint: &'a PathBuf,
  pub(crate) resources: &'a BTreeMap<String, ResourceSelection>,
  pub(crate) lifecycle_adapter: &'a str,
  pub(crate) release_sha256: &'a str,
  pub(crate) payload_sha256: &'a str,
}
