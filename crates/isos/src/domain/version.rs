//! Build-bundled version identities; runtime compatibility does not depend on GitHub.

use anyhow::{Result, ensure};
use serde::{Deserialize, Serialize};
use std::collections::BTreeSet;

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct ExternalPackage {
  pub id: String,
  pub version: String,
  pub manifest_sha256: String,
  pub asset_sha256: String,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct VersionSupport {
  pub schema: String,
  pub product_version: String,
  pub target_graph_format: String,
  pub isos_http_schema: String,
  pub package_contract: String,
  pub package_schema: String,
  pub external_packages: Vec<ExternalPackage>,
}

const BUNDLED: &str = include_str!("../../../../config/version-support.toml");

pub(crate) fn bundled() -> Result<VersionSupport> {
  let map: VersionSupport = toml::from_str(BUNDLED)?;
  ensure!(
    map.schema == "isos.version-support@v1",
    "unsupported version support map"
  );
  ensure!(
    map.product_version == env!("CARGO_PKG_VERSION"),
    "version support product mismatch"
  );
  ensure!(
    map.target_graph_format == "0.2.0",
    "version support graph target mismatch"
  );
  ensure!(
    map.isos_http_schema == "0.1.0",
    "version support HTTP schema mismatch"
  );
  ensure!(
    map.package_contract == "isos.package@v1" && map.package_schema == "0.1.0",
    "version support package contract mismatch"
  );
  let mut ids = BTreeSet::new();
  for package in &map.external_packages {
    ensure!(
      ids.insert((&package.id, &package.version)),
      "duplicate external package version"
    );
    ensure!(
      package.manifest_sha256.len() == 64
        && package
          .manifest_sha256
          .bytes()
          .all(|byte| byte.is_ascii_hexdigit()),
      "invalid external package manifest digest"
    );
    ensure!(
      package.asset_sha256.len() == 64
        && package
          .asset_sha256
          .bytes()
          .all(|byte| byte.is_ascii_hexdigit()),
      "invalid external package asset digest"
    );
  }
  Ok(map)
}

impl VersionSupport {
  pub(crate) fn external_package(&self, id: &str, version: &str) -> Option<&ExternalPackage> {
    self
      .external_packages
      .iter()
      .find(|package| package.id == id && package.version == version)
  }

  pub(crate) fn graph_status(&self, version: Option<&str>) -> &'static str {
    match version {
      None => "not_configured",
      Some(value) if value == self.target_graph_format => "compatible",
      Some("0.1.0") => "migration_required",
      Some(_) => "unsupported",
    }
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn bundled_map_matches_build_and_known_contracts() {
    let map = bundled().unwrap();
    assert_eq!(map.product_version, "0.2.1");
    assert_eq!(map.external_packages.len(), 1);
    assert_eq!(map.external_packages[0].id, "island/mujoco-sim");
  }

  #[test]
  fn map_rejects_unknown_fields() {
    let invalid = format!("{BUNDLED}\nunexpected = true\n");
    assert!(toml::from_str::<VersionSupport>(&invalid).is_err());
  }

  #[test]
  fn graph_status_is_exact_and_offline() {
    let map = bundled().unwrap();
    assert_eq!(map.graph_status(Some("0.2.0")), "compatible");
    assert_eq!(map.graph_status(Some("0.1.0")), "migration_required");
    assert_eq!(map.graph_status(Some("0.2.1")), "unsupported");
    assert_eq!(map.graph_status(None), "not_configured");
  }

  #[test]
  fn external_pin_lookup_requires_exact_identity_and_version() {
    let map = bundled().unwrap();
    assert!(map.external_package("island/mujoco-sim", "0.1.1").is_some());
    assert!(map.external_package("island/mujoco-sim", "0.1.2").is_none());
  }
}
