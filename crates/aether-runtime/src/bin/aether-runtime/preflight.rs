//! Private IsOS candidate inspection. No live runtime object is constructed.

use std::env;
use std::path::PathBuf;

use aether_common::events::valid_run_id;
use aether_common::io::read_bounded;
use aether_hal::{NodeTypeRegistry, Profile};
use aether_runtime::{MAX_PROFILE_BYTES, grpc, host_skills, managed_admission};
use serde_json::{Value, json};

struct Input {
  profile: PathBuf,
  run_id: String,
  node_types: Vec<PathBuf>,
}

#[derive(Clone, Copy)]
struct Failure {
  stage: &'static str,
  code: &'static str,
}

fn failure(stage: &'static str, code: &'static str) -> Failure {
  Failure { stage, code }
}

fn parse(args: &[String]) -> Result<Input, Failure> {
  let invalid = || failure("input", "preflight_arguments_invalid");
  let mut profile = None;
  let mut run_id = None;
  let mut node_types = Vec::new();
  let mut args = args.iter();
  while let Some(option) = args.next() {
    let value = args.next().ok_or_else(invalid)?;
    match option.as_str() {
      "--profile" if profile.is_none() => profile = Some(PathBuf::from(value)),
      "--run-id" if run_id.is_none() && valid_run_id(value) => run_id = Some(value.clone()),
      "--node-types" if node_types.len() < 32 => node_types.push(PathBuf::from(value)),
      _ => return Err(invalid()),
    }
  }
  Ok(Input {
    profile: profile.ok_or_else(invalid)?,
    run_id: run_id.ok_or_else(invalid)?,
    node_types,
  })
}

fn catalog_failure(code: &'static str) -> Failure {
  match code {
    "worker_reap_failed" => failure("worker", "worker_reap_failed"),
    "worker_timeout" => failure("worker", "worker_timeout"),
    "worker_spawn_failed" => failure("worker", "worker_spawn_failed"),
    "worker_sandbox_failed" => failure("worker", "worker_sandbox_failed"),
    "worker_inspection_failed" | "stage_unavailable" => {
      failure("worker", "worker_inspection_failed")
    }
    "abi_schema_invalid" => failure("abi", "abi_schema_invalid"),
    "abi_capability_unowned" => failure("abi", "abi_capability_unowned"),
    "abi_library_invalid" => failure("abi", "abi_library_invalid"),
    "abi_descriptor_invalid" | "abi_primitive_invalid" => failure("abi", "abi_descriptor_invalid"),
    "catalog_duplicate_identity" => failure("catalog", "catalog_duplicate_identity"),
    "catalog_capacity" => failure("catalog", "catalog_capacity"),
    "selection_revision_mismatch" => failure("selection", "selection_revision_mismatch"),
    "selection_manifest_invalid" | "selection_path_invalid" => {
      failure("selection", "selection_manifest_invalid")
    }
    "selection_digest_mismatch" => failure("selection", "selection_digest_mismatch"),
    "selection_handoff_incomplete" | "selection_without_managed_generation" => {
      failure("selection", "selection_handoff_incomplete")
    }
    "selection_invalid" | "selection_order_invalid" | "private_file_oversize" => {
      failure("selection", "selection_invalid")
    }
    "library_changed" => failure("package", "library_changed"),
    "library_not_elf" => failure("elf", "library_not_elf"),
    "library_dependency_unsafe" => failure("elf", "library_dependency_unsafe"),
    "package_file_unsafe"
    | "package_path_unsafe"
    | "package_tree_unsafe"
    | "private_file_unsafe" => failure("package", "package_unsafe"),
    "package_file_changed"
    | "package_tree_changed"
    | "payload_changed"
    | "receipt_changed"
    | "release_changed" => failure("package", "package_changed"),
    _ => failure("package", "package_invalid"),
  }
}

fn run(args: &[String]) -> Result<Value, Failure> {
  let input = parse(args)?;
  let verifier = env::var_os("EOS_CONTROL_GRANTS_FILE")
    .map(PathBuf::from)
    .ok_or(failure("input", "preflight_grants_invalid"))?;
  let selection = env::var_os("EOS_SKILL_SELECTION_FILE")
    .map(PathBuf::from)
    .ok_or(failure("selection", "selection_handoff_incomplete"))?;
  let profile_bytes = read_bounded(&input.profile, MAX_PROFILE_BYTES)
    .map_err(|_| failure("input", "preflight_profile_invalid"))?;
  let managed = managed_admission::load(
    &input.profile,
    &profile_bytes,
    Some(&verifier),
    &input.run_id,
  )
  .map_err(|_| failure("input", "preflight_grants_invalid"))?
  .ok_or(failure("input", "preflight_grants_invalid"))?;
  let registry = NodeTypeRegistry::with_contribution_files(&input.node_types)
    .map_err(|_| failure("input", "preflight_profile_invalid"))?;
  let source = std::str::from_utf8(&profile_bytes)
    .map_err(|_| failure("input", "preflight_profile_invalid"))?;
  let profile = if source.trim_start().starts_with('{') {
    Profile::from_json_with_registry(source, &registry)
  } else {
    Profile::from_toml_with_registry(source, &registry)
  }
  .map_err(|_| failure("input", "preflight_profile_invalid"))?;
  let catalog = host_skills::Catalog::admit(
    &input.profile,
    Some(&managed),
    Some(&selection),
    &env::current_exe().map_err(|_| failure("input", "preflight_arguments_invalid"))?,
  )
  .map_err(|error| catalog_failure(error.code))?;
  let metadata = catalog
    .preflight()
    .filter(|metadata| !metadata.libraries.is_empty())
    .ok_or(failure("selection", "selection_invalid"))?;
  if metadata
    .libraries
    .iter()
    .any(|library| library.skill_count == 0 || library.skill_count > 256)
  {
    return Err(failure("abi", "abi_library_invalid"));
  }
  grpc::validate_host_catalog_readiness(&profile, true, &catalog).map_err(|code| {
    if code == "abi_descriptor_invalid" {
      failure("abi", code)
    } else {
      failure("catalog", code)
    }
  })?;
  let metadata = catalog
    .finish_preflight()
    .map_err(|error| catalog_failure(error.code))?;
  let result = json!({
    "schema_version": 1,
    "ok": true,
    "active_revision": metadata.active_revision,
    "generation": metadata.generation,
    "run_id": managed.run_id,
    "selection_sha256": metadata.selection_sha256,
    "libraries": metadata.libraries,
  });
  Ok(result)
}

pub(super) fn main(args: &[String]) -> i32 {
  // IsOS applies its own global deadline and checks process-group cleanup.
  unsafe { libc::alarm(90) };
  let (result, exit) = match run(args) {
    Ok(value) => (value, 0),
    Err(error) => (
      json!({"schema_version":1,"ok":false,"stage":error.stage,"code":error.code}),
      1,
    ),
  };
  println!("{result}");
  exit
}
