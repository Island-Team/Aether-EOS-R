//! Resolve package-contributed processes from verified declarations and stable resource IDs.

pub mod plan;

use crate::{
  app::config::Paths,
  domain::packages,
  domain::registry::release::{ProcessDeclaration, Release},
};
use anyhow::{Context, Result, ensure};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
  collections::{BTreeMap, BTreeSet},
  fs,
  path::PathBuf,
};

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct ResourceCatalog {
  contract: String,
  schema_version: String,
  resources: Vec<Resource>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Resource {
  kind: String,
  id: String,
  label: String,
  description: String,
  #[serde(default)]
  path: Option<String>,
  #[serde(default)]
  sha256: Option<String>,
  #[serde(default)]
  metadata: Option<Value>,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct PackageIdentity {
  pub id: String,
  pub version: String,
}

pub struct ResolvedProcess {
  pub package: PackageIdentity,
  pub launch: PackageLaunch,
  pub launch_bytes: Vec<u8>,
  pub environment: BTreeMap<String, String>,
  pub fingerprint: String,
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct PackageLaunch {
  pub schema_version: u32,
  pub node_path: String,
  pub node_type: String,
  pub package: PackageIdentity,
  pub entrypoint: PathBuf,
  pub resources: BTreeMap<String, ResourceSelection>,
  pub lifecycle_adapter: String,
  pub release_sha256: Option<String>,
  pub payload_sha256: Option<String>,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ResourceSelection {
  pub kind: String,
  pub id: String,
  pub path: Option<PathBuf>,
  pub sha256: Option<String>,
  pub metadata: Option<Value>,
}

/// Bind a verified process declaration to the exact deployment generation at spawn time.
pub fn bind_launch(
  launch: &PackageLaunch,
  run_id: &str,
  graph_revision: &str,
  verified_root: &std::path::Path,
) -> Result<Vec<u8>> {
  ensure!(launch.schema_version == 1, "invalid unbound package launch");
  let owner = if launch.node_type == "isos.eos_process.mujoco@v0.2" {
    "eos"
  } else {
    launch.node_path.split('/').next().unwrap_or_default()
  };
  supported_owner(owner, &launch.node_path, &launch.node_type, &launch.package)?;
  ensure!(
    !run_id.is_empty()
      && run_id.len() <= 64
      && run_id
        .bytes()
        .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-')),
    "invalid package launch run identity"
  );
  ensure!(
    graph_revision.len() == 64
      && graph_revision
        .bytes()
        .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte)),
    "invalid package launch graph revision"
  );
  ensure!(
    launch.entrypoint.starts_with(verified_root)
      && launch.resources.values().all(|resource| resource
        .path
        .as_ref()
        .is_none_or(|path| path.starts_with(verified_root))),
    "package launch path escapes verified store"
  );
  let release_sha256 = launch
    .release_sha256
    .as_deref()
    .context("package launch release digest missing")?;
  let payload_sha256 = launch
    .payload_sha256
    .as_deref()
    .context("package launch payload digest missing")?;
  let bound = crate::interfaces::handoff::ProcessHandoff {
    contract: "isos.package-process@v1",
    schema_version: "0.1.0",
    owner_core: owner,
    run_id,
    graph_revision,
    node_path: &launch.node_path,
    node_type: &launch.node_type,
    package: &launch.package,
    entrypoint: &launch.entrypoint,
    resources: &launch.resources,
    lifecycle_adapter: &launch.lifecycle_adapter,
    release_sha256,
    payload_sha256,
  };
  let bytes = serde_json::to_vec(&bound)?;
  ensure!(bytes.len() <= 65_536, "package launch exceeds 64 KiB");
  Ok(bytes)
}

fn declared_process<'a>(
  release: &'a crate::domain::registry::release::Release,
  node_type: &str,
) -> Option<(&'a ProcessDeclaration, &'a str)> {
  let contributions = release.node_contributions.as_ref()?;
  if node_type == "isos.eos_process.mujoco@v0.2" && packages::is_published_mujoco_legacy(release) {
    let process = contributions
      .processes
      .iter()
      .find(|process| process.node_type == "eos.process.mujoco@v0.1")?;
    return Some((process, "eos"));
  }
  let process = contributions
    .processes
    .iter()
    .find(|process| process.node_type == node_type)?;
  let owner = contributions
    .node_types
    .iter()
    .find(|registered| registered["id"] == node_type)?
    .get("core")?
    .as_str()?;
  Some((process, owner))
}

fn admission_failure(package: &PackageIdentity, code: &'static str, path: &str) -> anyhow::Error {
  packages::PackageFailure {
    code,
    stage: "admission",
    id: package.id.clone(),
    version: package.version.clone(),
    path: path.to_owned(),
  }
  .into()
}

fn supported_owner(
  owner: &str,
  node_path: &str,
  node_type: &str,
  package: &PackageIdentity,
) -> Result<()> {
  let unified_simulator = owner == "eos"
    && node_type == "isos.eos_process.mujoco@v0.2"
    && package.id == "island/mujoco-sim"
    && package.version == "0.1.1"
    && node_path.split('/').take(3).collect::<Vec<_>>() == ["isos", "eos_process", "mujoco"];
  if !unified_simulator
    && (node_path.split('/').next() != Some(owner)
      || node_path.split('/').nth(1) != Some("process"))
  {
    return Err(admission_failure(
      package,
      "package_owner_mismatch",
      node_path,
    ));
  }
  if owner != "eos" {
    return Err(admission_failure(
      package,
      "package_owner_unavailable",
      node_path,
    ));
  }
  Ok(())
}

fn catalog(root: &std::path::Path, path: &str) -> Result<ResourceCatalog> {
  let file = packages::checked_file(root, path, false)?;
  let bytes = fs::read(file)?;
  ensure!(
    bytes.len() <= 262_144,
    "package resource catalog exceeds 256 KiB"
  );
  let catalog: ResourceCatalog = serde_json::from_slice(&bytes)?;
  ensure!(
    catalog.contract == "isos.package-resources@v1"
      && catalog.schema_version == "0.1.0"
      && catalog.resources.len() <= 256,
    "unsupported package resource catalog"
  );
  let mut ids = BTreeSet::new();
  let stable = |value: &str| {
    (1..=128).contains(&value.len())
      && value.split('-').all(|part| {
        !part.is_empty()
          && part
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit())
      })
  };
  for resource in &catalog.resources {
    ensure!(
      stable(&resource.kind)
        && stable(&resource.id)
        && (1..=256).contains(&resource.label.trim().len())
        && (1..=1024).contains(&resource.description.trim().len())
        && resource
          .metadata
          .as_ref()
          .is_none_or(|value| serde_json::to_vec(value).is_ok_and(|bytes| bytes.len() <= 16_384))
        && ids.insert((&resource.kind, &resource.id)),
      "invalid or duplicate package resource"
    );
    if let Some(path) = &resource.path {
      let file = packages::checked_file(root, path, false)?;
      let digest = hex::encode(Sha256::digest(fs::read(file)?));
      ensure!(
        resource.sha256.as_deref() == Some(digest.as_str()),
        "package resource digest mismatch"
      );
    } else {
      ensure!(
        resource.sha256.is_none(),
        "logical resource cannot have file digest"
      );
    }
  }
  Ok(catalog)
}

fn process_catalog(
  root: &std::path::Path,
  release: &Release,
  path: &str,
) -> Result<ResourceCatalog> {
  if path != "@isos-published-mujoco-scenes" {
    return catalog(root, path);
  }
  ensure!(
    packages::is_published_mujoco_legacy(release),
    "unsupported package resource catalog"
  );
  let mut resources = Vec::new();
  for (id, label, scene) in [
    ("pick-place", "Pick and place", "assets/scene.xml"),
    (
      "navigation",
      "Navigation",
      "assets/scenes/navigation/scene.xml",
    ),
  ] {
    let file = packages::checked_file(root, scene, false)?;
    let sha256 = hex::encode(Sha256::digest(fs::read(file)?));
    resources.push(Resource {
      kind: "scene".into(),
      id: id.into(),
      label: label.into(),
      description: format!("Verified MuJoCo {label} scene"),
      path: Some(scene.into()),
      sha256: Some(sha256),
      metadata: None,
    });
  }
  Ok(ResourceCatalog {
    contract: "isos.package-resources@v1".into(),
    schema_version: "0.1.0".into(),
    resources,
  })
}

pub fn validate_payload(
  root: &std::path::Path,
  release: &Release,
  process: &ProcessDeclaration,
) -> Result<()> {
  packages::checked_file(root, &process.entrypoint, true)?;
  for resource in &process.resources {
    process_catalog(root, release, &resource.descriptor)?;
  }
  Ok(())
}

pub fn option_resources(
  root: &std::path::Path,
  release: &Release,
  process: &ProcessDeclaration,
) -> Result<Vec<Value>> {
  let mut rows = Vec::new();
  for declaration in &process.resources {
    for resource in process_catalog(root, release, &declaration.descriptor)?.resources {
      rows.push(json!({
          "kind":resource.kind,
          "id":resource.id,
          "label":resource.label,
          "description":resource.description,
          "metadata":resource.metadata,
      }));
    }
  }
  Ok(rows)
}

pub fn resolve(
  paths: &Paths,
  package: &PackageIdentity,
  node_type: &str,
  node_path: &str,
  settings: &Value,
) -> Result<Option<ResolvedProcess>> {
  let verified = packages::verify(paths, &package.id, &package.version)?;
  let Some((process, owner)) = declared_process(&verified.release, node_type) else {
    return Ok(None);
  };
  supported_owner(owner, node_path, node_type, package)?;
  validate_payload(&verified.root, &verified.release, process)?;
  let entrypoint = packages::checked_file(&verified.root, &process.entrypoint, true)?;
  let mut resources = BTreeMap::new();
  for declaration in &process.resources {
    let selected = settings
      .get(&declaration.setting)
      .and_then(Value::as_str)
      .with_context(|| format!("missing package resource setting {}", declaration.setting))?;
    let index = process_catalog(&verified.root, &verified.release, &declaration.descriptor)?;
    let item = index
      .resources
      .iter()
      .find(|r| r.kind == declaration.kind && r.id == selected)
      .with_context(|| format!("package resource {selected} is unavailable"))?;
    let path = item
      .path
      .as_deref()
      .map(|path| packages::checked_file(&verified.root, path, false))
      .transpose()?;
    resources.insert(
      declaration.setting.clone(),
      ResourceSelection {
        kind: item.kind.clone(),
        id: item.id.clone(),
        path,
        sha256: item.sha256.clone(),
        metadata: item.metadata.clone(),
      },
    );
  }
  let mut environment = BTreeMap::new();
  for input in &process.host_inputs {
    let value = settings
      .get("env")
      .and_then(|env| env.get(&input.name))
      .and_then(Value::as_str)
      .map(str::to_owned)
      .or_else(|| std::env::var(&input.name).ok());
    if input.required {
      ensure!(
        value.is_some(),
        "required package host input {} is missing",
        input.name
      );
    }
    if let Some(value) = value {
      let directory = PathBuf::from(&value);
      ensure!(
        directory.is_absolute() && directory.is_dir(),
        "package host input must be an absolute directory"
      );
      environment.insert(input.name.clone(), value);
    }
  }
  let launch = PackageLaunch {
    schema_version: 1,
    node_path: node_path.to_owned(),
    node_type: node_type.to_owned(),
    package: package.clone(),
    entrypoint,
    resources,
    lifecycle_adapter: process.lifecycle_adapter.clone(),
    release_sha256: verified.receipt.release_sha256.clone(),
    payload_sha256: verified.receipt.payload_sha256.clone(),
  };
  let launch_bytes = serde_json::to_vec(&launch)?;
  let fingerprint = hex::encode(Sha256::digest(&launch_bytes));
  Ok(Some(ResolvedProcess {
    package: package.clone(),
    launch,
    launch_bytes,
    environment,
    fingerprint,
  }))
}

fn graph_nodes(graph: &Value) -> Vec<(String, &Value)> {
  let mut result = Vec::new();
  if let Some(cores) = graph.get("cores").and_then(Value::as_object) {
    for (core_id, core) in cores {
      if let Some(layers) = core.get("layers").and_then(Value::as_object) {
        for (layer_id, layer) in layers {
          if let Some(groups) = layer.get("groups").and_then(Value::as_object) {
            for (group_id, group) in groups {
              if let Some(nodes) = group.get("nodes").and_then(Value::as_object) {
                for (node_id, node) in nodes {
                  result.push((format!("{core_id}/{layer_id}/{group_id}/{node_id}"), node));
                }
              }
            }
          }
        }
      }
    }
  }
  result
}

pub fn validate_graph(paths: &Paths, graph: &Value) -> Result<()> {
  let nodes = graph_nodes(graph);
  let mut processes = BTreeMap::new();
  for (path, node) in &nodes {
    let Some(node_type) = node.get("node_type").and_then(Value::as_str) else {
      continue;
    };
    let Some(pin) = node.get("package") else {
      continue;
    };
    let package: PackageIdentity = serde_json::from_value(pin.clone())?;
    let verified = packages::verify(paths, &package.id, &package.version)?;
    if declared_process(&verified.release, node_type).is_none() {
      if matches!(path.split('/').nth(1), Some("process" | "eos_process")) {
        return Err(admission_failure(
          &package,
          "package_process_unsupported_contract",
          path,
        ));
      }
      continue;
    }
    if let Some(resolved) = resolve(paths, &package, node_type, path, &node["settings"])? {
      processes.insert(path.clone(), (resolved, &node["settings"]));
    }
  }
  if !processes.is_empty() {
    let catalog = crate::runtime::process::catalog_for_option_graph(paths, graph)?;
    for (process, _) in processes.values() {
      let family = &process.launch.lifecycle_adapter;
      if !catalog["adapter_families"]
        .as_array()
        .is_some_and(|families| {
          families
            .iter()
            .any(|entry| entry["id"] == *family && entry["compiled"] == true)
        })
      {
        return Err(admission_failure(
          &process.package,
          "package_lifecycle_adapter_unavailable",
          &process.launch.node_path,
        ));
      }
    }
  }
  Ok(())
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn unrelated_resource_families_share_digest_and_containment_checks() {
    for (kind, id, filename) in [
      ("calibration", "camera-a", "calibration.dat"),
      ("preset", "voice-b", "preset.dat"),
    ] {
      let root = tempfile::tempdir().unwrap();
      fs::write(root.path().join(filename), b"verified").unwrap();
      let digest = hex::encode(Sha256::digest(b"verified"));
      let mut declaration = json!({"contract":"isos.package-resources@v1",
        "schema_version":"0.1.0","resources":[{"kind":kind,"id":id,
        "label":"Selected resource","description":"Owner validated resource",
        "path":filename,"sha256":digest}]});
      let descriptor = root.path().join("resources.json");
      fs::write(&descriptor, serde_json::to_vec(&declaration).unwrap()).unwrap();
      assert_eq!(
        catalog(root.path(), "resources.json").unwrap().resources[0].id,
        id
      );

      fs::write(root.path().join(filename), b"changed").unwrap();
      assert!(
        catalog(root.path(), "resources.json").is_err(),
        "{kind} changed file"
      );
      declaration["resources"][0]["path"] = json!("../escape.dat");
      fs::write(&descriptor, serde_json::to_vec(&declaration).unwrap()).unwrap();
      assert!(
        catalog(root.path(), "resources.json").is_err(),
        "{kind} unsafe path"
      );
      declaration["resources"][0]["path"] = json!(filename);
      declaration["resources"][0]["metadata"] = json!({"opaque":"x".repeat(20_000)});
      fs::write(&descriptor, serde_json::to_vec(&declaration).unwrap()).unwrap();
      assert!(
        catalog(root.path(), "resources.json").is_err(),
        "{kind} oversized metadata"
      );
    }
  }
  #[test]
  fn unknown_or_mismatched_process_owner_is_typed() {
    let package = PackageIdentity {
      id: "vendor/demo".into(),
      version: "1.0.0".into(),
    };
    for (owner, path, code) in [
      (
        "vendor",
        "vendor/process/demo/one",
        "package_owner_unavailable",
      ),
      ("eos", "vendor/process/demo/one", "package_owner_mismatch"),
      ("eos", "eos/hardware/demo/one", "package_owner_mismatch"),
    ] {
      let error = supported_owner(owner, path, "eos.process.mujoco@v0.1", &package).unwrap_err();
      let failure = error.downcast_ref::<packages::PackageFailure>().unwrap();
      assert_eq!(failure.code, code);
      assert_eq!(failure.path, path);
    }
  }
  #[test]
  fn pinned_mujoco_v02_owner_maps_once_and_rejects_unknown_paths() {
    let root = tempfile::tempdir().unwrap();
    let manifest = root.path().join("release.json");
    fs::write(
      &manifest,
      include_bytes!("../../../tests/fixtures/packages/mujoco_published_0_1_1.json"),
    )
    .unwrap();
    let release = packages::read_manifest(&manifest).unwrap();
    assert_eq!(
      declared_process(&release, "isos.eos_process.mujoco@v0.2")
        .unwrap()
        .0
        .entrypoint,
      "bin/eos-mujoco-sim"
    );
    assert!(declared_process(&release, "isos.eos_process.unknown@v0.2").is_none());
    let package = PackageIdentity {
      id: release.id.clone(),
      version: release.version.clone(),
    };
    assert!(
      supported_owner(
        "eos",
        "isos/eos_process/mujoco/sim",
        "isos.eos_process.mujoco@v0.2",
        &package
      )
      .is_ok()
    );
    for (path, node_type) in [
      ("isos/eos_process/other/sim", "isos.eos_process.mujoco@v0.2"),
      (
        "isos/eos_hardware/mujoco/sim",
        "isos.eos_process.mujoco@v0.2",
      ),
      (
        "isos/eos_process/mujoco/sim",
        "isos.eos_process.unknown@v0.2",
      ),
    ] {
      assert!(supported_owner("eos", path, node_type, &package).is_err());
    }
  }
  #[test]
  fn pinned_mujoco_v02_launch_uses_verified_scene_and_eos_owner() {
    use std::os::unix::fs::PermissionsExt;
    let root = tempfile::tempdir().unwrap();
    let source = root.path().join("bundle");
    fs::create_dir_all(source.join("bin")).unwrap();
    fs::create_dir_all(source.join("assets/scenes/navigation")).unwrap();
    fs::write(
      source.join("release.json"),
      include_bytes!("../../../tests/fixtures/packages/mujoco_published_0_1_1.json"),
    )
    .unwrap();
    fs::write(
      source.join("assets/scene.xml"),
      b"<mujoco model='pick-place'/>",
    )
    .unwrap();
    fs::write(
      source.join("assets/scenes/navigation/scene.xml"),
      b"<mujoco model='navigation'/>",
    )
    .unwrap();
    let binary = source.join("bin/eos-mujoco-sim");
    fs::write(&binary, b"#!/bin/sh\nexit 0\n").unwrap();
    fs::set_permissions(&binary, fs::Permissions::from_mode(0o700)).unwrap();
    let paths = Paths {
      config: root.path().join("config"),
      data: root.path().join("data"),
      runtime: root.path().join("run"),
    };
    paths.prepare().unwrap();
    packages::link_with_manifest(&paths, "island/mujoco-sim", "0.1.1", &source, None).unwrap();
    let package = PackageIdentity {
      id: "island/mujoco-sim".into(),
      version: "0.1.1".into(),
    };
    let resolved = resolve(&paths, &package, "isos.eos_process.mujoco@v0.2", "isos/eos_process/mujoco/simulator",
      &json!({"scene":"pick-place","socket":"/tmp/mujoco-test.sock","env":{"MUJOCO_ROOT":root.path()}})).unwrap().unwrap();
    assert_eq!(resolved.launch.resources["scene"].id, "pick-place");
    assert!(
      resolved.launch.resources["scene"]
        .path
        .as_ref()
        .unwrap()
        .ends_with("assets/scene.xml")
    );
    let bytes = bind_launch(
      &resolved.launch,
      "run1",
      &"a".repeat(64),
      resolved
        .launch
        .entrypoint
        .parent()
        .unwrap()
        .parent()
        .unwrap(),
    )
    .unwrap();
    let handoff: Value = serde_json::from_slice(&bytes).unwrap();
    assert_eq!(handoff["owner_core"], "eos");
    assert_eq!(handoff["node_path"], "isos/eos_process/mujoco/simulator");
  }
  #[test]
  fn generic_catalog_binds_file_and_logical_resources() {
    let root = tempfile::tempdir().unwrap();
    fs::write(root.path().join("model.bin"), b"model").unwrap();
    let digest = hex::encode(Sha256::digest(b"model"));
    let catalog_data = json!({"contract":"isos.package-resources@v1","schema_version":"0.1.0","resources":[
      {"kind":"model","id":"small","label":"Small","description":"small model","path":"model.bin","sha256":digest},
      {"kind":"mode","id":"fast","label":"Fast","description":"fast mode","metadata":{"hint":"owner validated"}}
    ]});
    fs::write(
      root.path().join("resources.json"),
      serde_json::to_vec(&catalog_data).unwrap(),
    )
    .unwrap();
    assert_eq!(
      catalog(root.path(), "resources.json")
        .unwrap()
        .resources
        .len(),
      2
    );
    fs::write(root.path().join("model.bin"), b"changed").unwrap();
    assert!(catalog(root.path(), "resources.json").is_err());
    let mut old = catalog_data;
    old["schema_version"] = json!(1);
    fs::write(
      root.path().join("resources.json"),
      serde_json::to_vec(&old).unwrap(),
    )
    .unwrap();
    assert!(catalog(root.path(), "resources.json").is_err());
  }

  #[test]
  fn bind_launch_requires_current_fence_and_contained_paths() {
    let root = std::path::Path::new("/bundle");
    let launch = PackageLaunch {
      schema_version: 1,
      node_path: "eos/process/sim/one".into(),
      node_type: "demo.process@v1".into(),
      package: PackageIdentity {
        id: "vendor/sim".into(),
        version: "1.0.0".into(),
      },
      entrypoint: root.join("bin/start"),
      resources: BTreeMap::from([(
        "model".into(),
        ResourceSelection {
          kind: "model".into(),
          id: "small".into(),
          path: Some(root.join("model.bin")),
          sha256: Some("a".repeat(64)),
          metadata: None,
        },
      )]),
      lifecycle_adapter: "process".into(),
      release_sha256: Some("a".repeat(64)),
      payload_sha256: Some("b".repeat(64)),
    };
    let fence = "c".repeat(64);
    let bytes = bind_launch(&launch, "run_1", &fence, root).unwrap();
    let value: Value = serde_json::from_slice(&bytes).unwrap();
    assert_eq!(value["contract"], "isos.package-process@v1");
    assert_eq!(value["schema_version"], "0.1.0");
    assert_eq!(value["resources"]["model"]["id"], "small");
    assert!(bind_launch(&launch, "run_1", "bad", root).is_err());
    assert!(bind_launch(&launch, "run_1", &fence, std::path::Path::new("/other")).is_err());
  }
}
