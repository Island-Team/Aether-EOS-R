//! Inspect a unified v2 node profile without opening any device.

use std::env;
use std::error::Error;
use std::path::PathBuf;

use aether_common::io::read_bounded;
use aether_hal::{NodeTypeRegistry, Profile, PRIMITIVE_SPECS};
use aether_runtime::MAX_PROFILE_BYTES;

#[path = "aether_profile/setup.rs"]
mod setup;

const PROFILE_FEATURES: [&str; 8] = [
  "short-paths",
  "node-types",
  "data-idl",
  "primitive-binding",
  "catalog",
  "setup-v1",
  "setup-v2",
  "graph-0.1-reader",
];

fn main() -> Result<(), Box<dyn Error>> {
  if env::args().nth(1).as_deref() == Some("--capabilities") {
    println!(
      "{}",
      serde_json::json!({"product_version":env!("CARGO_PKG_VERSION"),"graph_version":aether_hal::GRAPH_VERSION,
            "node_type_version":"1.0","supported_graph_versions":["1.0.0","0.1.0","0.2.0"],
            "supported_node_type_versions":["1.0","0.1","0.2"],
            "supported_node_types":aether_hal::builtin_reader_node_types(),
            "binary":"aether-profile","features":PROFILE_FEATURES})
    );
    return Ok(());
  }
  let mut args = env::args();
  let program = args.next().unwrap_or_else(|| "aether-profile".into());
  let raw: Vec<String> = args.collect();
  let mut input = Vec::new();
  let mut node_types = Vec::<PathBuf>::new();
  let mut index = 0;
  while index < raw.len() {
    if raw[index] == "--node-types" {
      index += 1;
      node_types.push(PathBuf::from(
        raw.get(index).ok_or("--node-types needs a path")?,
      ));
    } else {
      input.push(raw[index].clone());
    }
    index += 1;
  }
  let registry = NodeTypeRegistry::with_contribution_files(&node_types)?;
  if input == ["setup", "--json"] {
    setup::run(&registry)?;
    return Ok(());
  }
  let (validate_only, path) = match input.as_slice() {
    [verb] if verb == "catalog" => {
      let types = registry.all().collect::<Vec<_>>();
      let data_types = registry.data_types().collect::<Vec<_>>();
      let adapter_families = registry.catalog_adapter_families();
      println!(
        "{}",
        serde_json::json!({"types":types,"data_types":data_types,
                    "adapter_families":adapter_families,"primitive_specs":PRIMITIVE_SPECS})
      );
      return Ok(());
    }
    [path] => (false, path.as_str()),
    [verb, flag, path] if verb == "validate" && flag == "--file" => (true, path.as_str()),
    _ => {
      eprintln!(
                "usage: {program} <profile.toml> | validate --file <profile.toml|json> | catalog | setup --json [--node-types <verified-json>]..."
            );
      std::process::exit(2);
    }
  };
  let bytes = read_bounded(path, MAX_PROFILE_BYTES)?;
  let source = std::str::from_utf8(&bytes)?;
  let profile = if source.trim_start().starts_with('{') {
    Profile::from_json_with_registry(source, &registry)?
  } else {
    Profile::from_toml_with_registry(source, &registry)?
  };
  if validate_only {
    println!(
      "{}",
      serde_json::json!({"ok":true,"version":profile.version})
    );
    return Ok(());
  }
  println!("robot {} ({})", profile.robot.id, profile.robot.name);
  for node in profile.all_nodes() {
    println!(
      "{}\t{}\t{}\t{}",
      node.path(),
      node.label,
      if node.adapter.adapter_type.is_empty() {
        "software"
      } else {
        &node.adapter.adapter_type
      },
      node.capabilities.join(",")
    );
  }
  Ok(())
}
