//! Bounded, one-shot graph setup for the IsOS owner process.

use std::error::Error;
use std::io::{Read, Write};
use std::path::Path;

use aether_common::io::read_bounded;
use aether_hal::{NodeTypeContribution, NodeTypeRegistry, Profile, PRIMITIVE_SPECS};
use serde_json::{json, Value};

const MAX_REQUEST_BYTES: u64 = 1024 * 1024;
const MAX_RESPONSE_BYTES: usize = 4 * 1024 * 1024;

pub fn run(registry: &NodeTypeRegistry) -> Result<(), Box<dyn Error>> {
  let mut input = Vec::new();
  std::io::stdin()
    .take(MAX_REQUEST_BYTES + 1)
    .read_to_end(&mut input)?;
  let response = if input.len() as u64 > MAX_REQUEST_BYTES {
    failure(
      "request",
      "request_too_large",
      "setup request exceeds 1 MiB",
    )
  } else {
    match serde_json::from_slice::<Value>(&input) {
      Ok(request) => handle(&request, registry),
      Err(error) => failure("request", "invalid_json", &error.to_string()),
    }
  };
  let output = serde_json::to_vec(&response)?;
  if output.len() > MAX_RESPONSE_BYTES {
    return write_response(&failure(
      "response",
      "response_too_large",
      "setup response exceeds 4 MiB",
    ));
  }
  std::io::stdout().write_all(&output)?;
  std::io::stdout().write_all(b"\n")?;
  Ok(())
}

fn write_response(response: &Value) -> Result<(), Box<dyn Error>> {
  serde_json::to_writer(std::io::stdout(), response)?;
  std::io::stdout().write_all(b"\n")?;
  Ok(())
}

fn failure(path: &str, code: &str, message: &str) -> Value {
  json!({"schema_version":1,"ok":false,"diagnostics":[{
        "path":path,"code":code,"message":message
    }],"files":[]})
}

fn registry_for_packages(
  packages: &Value,
  base: &NodeTypeRegistry,
) -> Result<NodeTypeRegistry, String> {
  let mut registry = base.clone();
  let entries = packages.as_array().ok_or("expected packages array")?;
  for (index, entry) in entries.iter().enumerate() {
    let id = entry
      .get("id")
      .and_then(Value::as_str)
      .ok_or_else(|| format!("packages[{index}] has no package ID"))?;
    let version = entry
      .get("version")
      .and_then(Value::as_str)
      .ok_or_else(|| format!("packages[{index}] has no version"))?;
    let directory = entry
      .get("path")
      .and_then(Value::as_str)
      .ok_or_else(|| format!("packages[{index}] has no package path"))?;
    let root = Path::new(directory);
    if !root.is_absolute() {
      return Err(format!("packages[{index}] path must be absolute"));
    }
    let root = root
      .canonicalize()
      .map_err(|error| format!("packages[{index}] path: {error}"))?;
    let sidecar = root.join("node-types.json");
    if !sidecar.exists() {
      continue;
    }
    let canonical = sidecar
      .canonicalize()
      .map_err(|error| format!("packages[{index}] sidecar: {error}"))?;
    if !canonical.starts_with(&root) {
      return Err(format!("packages[{index}] sidecar escapes package"));
    }
    let bytes = read_bounded(&canonical, MAX_REQUEST_BYTES as usize)
      .map_err(|error| format!("packages[{index}] sidecar: {error}"))?;
    let contribution: NodeTypeContribution = serde_json::from_slice(&bytes)
      .map_err(|error| format!("packages[{index}] sidecar: {error}"))?;
    if contribution.package.id != id || contribution.package.version != version {
      return Err(format!("packages[{index}] sidecar identity differs"));
    }
    registry
      .register_contribution(contribution)
      .map_err(|error| format!("packages[{index}] sidecar: {error}"))?;
  }
  Ok(registry)
}

fn handle(request: &Value, registry: &NodeTypeRegistry) -> Value {
  let mut reply = handle_inner(request, registry);
  if request.get("schema_version").and_then(Value::as_u64) == Some(2) {
    reply["schema_version"] = json!(2);
    if let Some(object) = reply.as_object_mut() {
      object.remove("revision");
    }
    if reply.get("identity").is_some() {
      reply["identity"]["setup_schema_version"] = json!(2);
    }
  }
  reply
}

fn handle_inner(request: &Value, registry: &NodeTypeRegistry) -> Value {
  let Some(object) = request.as_object() else {
    return failure(
      "request",
      "invalid_request",
      "setup request must be an object",
    );
  };
  if !matches!(
    object.get("schema_version").and_then(Value::as_u64),
    Some(1 | 2)
  ) {
    return failure(
      "schema_version",
      "unsupported_version",
      "expected setup schema version 1 or 2",
    );
  }
  if object.get("core").and_then(Value::as_str) != Some("eos") {
    return failure("core", "invalid_core", "expected eos core");
  }
  let Some(op) = object.get("op").and_then(Value::as_str) else {
    return failure("op", "invalid_operation", "missing setup operation");
  };
  if !matches!(op, "identity" | "catalog" | "validate" | "render") {
    return failure("op", "invalid_operation", "unknown setup operation");
  }
  if op == "identity" {
    return json!({"schema_version":1,"ok":true,"diagnostics":[],"files":[],
            "identity":{"core":"eos","product_version":env!("CARGO_PKG_VERSION"),
                "graph_version":aether_hal::GRAPH_VERSION,"node_type_version":"1.0",
                "supported_graph_versions":["1.0.0","0.1.0","0.2.0"],
                "supported_node_type_versions":["1.0","0.1","0.2"],
                "supported_node_types":aether_hal::builtin_reader_node_types(),
                "features":super::PROFILE_FEATURES,
                "setup_schema_version":1}});
  }
  if op == "catalog" {
    return json!({"schema_version":1,"ok":true,"diagnostics":[],"files":[],
            "catalog":{"types":registry.all().collect::<Vec<_>>(),
            "data_types":registry.data_types().collect::<Vec<_>>(),
            "adapter_families":registry.catalog_adapter_families(),
            "primitive_specs":PRIMITIVE_SPECS}});
  }
  let Some(revision) = object.get("revision").and_then(Value::as_str) else {
    return failure("revision", "invalid_revision", "missing graph revision");
  };
  if revision.is_empty() || revision.len() > 128 || !revision.is_ascii() {
    return failure(
      "revision",
      "invalid_revision",
      "graph revision must be 1–128 ASCII bytes",
    );
  }
  if !object.get("endpoints").is_some_and(Value::is_object) {
    return failure(
      "endpoints",
      "invalid_endpoints",
      "expected endpoints object",
    );
  }
  if !object.get("packages").is_some_and(Value::is_array) {
    return failure("packages", "invalid_packages", "expected packages array");
  }
  let registry = match registry_for_packages(&object["packages"], registry) {
    Ok(registry) => registry,
    Err(error) => return failure("packages", "invalid_packages", &error),
  };
  let Some(graph) = object.get("graph") else {
    return failure("graph", "missing_graph", "missing graph");
  };
  let graph = if graph.get("version").and_then(Value::as_str) == Some("0.2.0") {
    if graph["cores"]
      .as_object()
      .is_none_or(|cores| cores.len() != 1)
    {
      return failure(
        "graph/cores",
        "invalid_graph",
        "0.2.0 requires one isos core",
      );
    }
    let Some(layers) = graph
      .pointer("/cores/isos/layers")
      .and_then(Value::as_object)
    else {
      return failure(
        "graph/cores/isos/layers",
        "invalid_graph",
        "0.2.0 requires isos layers",
      );
    };
    let eos_layers = layers
      .iter()
      .filter(|(name, _)| name.starts_with("eos_"))
      .map(|(name, value)| (name.clone(), value.clone()))
      .collect::<serde_json::Map<_, _>>();
    if eos_layers.is_empty() {
      return failure(
        "graph/cores/isos/layers",
        "invalid_graph",
        "0.2.0 requires EOS layers",
      );
    }
    json!({"version":"0.2.0","robot":graph.get("robot"),"cores":{"isos":{"layers":eos_layers}}})
  } else if graph.get("version").and_then(Value::as_str) == Some("0.1.0") {
    let Some(eos) = graph.pointer("/cores/eos") else {
      return failure(
        "graph/cores/eos",
        "invalid_graph",
        "0.1.0 EOS setup requires an eos core",
      );
    };
    json!({"version":"0.1.0","robot":graph.get("robot"),"cores":{"eos":eos}})
  } else {
    graph.clone()
  };
  let source = match serde_json::to_string(&graph) {
    Ok(source) => source,
    Err(error) => return failure("graph", "invalid_graph", &error.to_string()),
  };
  let profile = match Profile::from_json_with_registry(&source, &registry) {
    Ok(profile) => profile,
    Err(error) => return failure("graph", "invalid_graph", &error.to_string()),
  };
  if op == "validate" {
    return json!({"schema_version":1,"ok":true,"revision":revision,
            "diagnostics":[],"files":[]});
  }
  let content = match serde_json::to_string(&profile) {
    Ok(content) => content,
    Err(error) => return failure("graph", "render_failed", &error.to_string()),
  };
  json!({"schema_version":1,"ok":true,"revision":revision,
        "diagnostics":[],"files":[{"path":"eos/graph.json","content":content}]})
}

#[cfg(test)]
mod tests {
  use super::*;

  fn request(op: &str, graph: Value) -> Value {
    json!({"schema_version":1,"op":op,"core":"eos","revision":"abc",
            "graph":graph,"endpoints":{},"packages":[]})
  }

  #[test]
  fn test_setup_render_emits_validated_graph_without_starting_runtime() {
    let graph = json!({"version":"1.0.0","robot":{"id":"robot","name":"Robot"},"cores":{}});
    let registry = NodeTypeRegistry::with_contribution_files(&[]).unwrap();
    let result = handle(&request("render", graph), &registry);
    assert_eq!(result["ok"], true);
    assert_eq!(result["files"][0]["path"], "eos/graph.json");
    let rendered = result["files"][0]["content"].as_str().unwrap();
    assert!(Profile::from_json(rendered).is_ok());
  }

  #[test]
  fn test_setup_rejects_invalid_graph_without_files() {
    let result = handle(&request("render", json!({})), &NodeTypeRegistry::default());
    assert_eq!(result["ok"], false);
    assert_eq!(result["files"], json!([]));
    assert_eq!(result["diagnostics"][0]["code"], "invalid_graph");
  }

  #[test]
  fn test_setup_catalog_exposes_types() {
    let result = handle(
      &json!({"schema_version":1,"op":"catalog","core":"eos"}),
      &NodeTypeRegistry::with_contribution_files(&[]).unwrap(),
    );
    assert_eq!(result["ok"], true);
    assert!(result["catalog"]["types"]
      .as_array()
      .is_some_and(|v| !v.is_empty()));
    let types = result["catalog"]["types"].as_array().unwrap();
    assert!(types.iter().any(|item| {
      item["id"] == "eos.dispatcher.control@v0.1"
        && item["description"]
          .as_str()
          .is_some_and(|value| !value.is_empty())
    }));
  }

  #[test]
  fn test_setup_identity_needs_no_graph_or_revision() {
    let result = handle(
      &json!({"schema_version":1,"op":"identity","core":"eos"}),
      &NodeTypeRegistry::default(),
    );
    assert_eq!(result["ok"], true);
    assert_eq!(result["files"], json!([]));
    assert_eq!(result["identity"]["product_version"], "0.2.1");
    assert_eq!(result["identity"]["graph_version"], "1.0.0");
    assert_eq!(
      result["identity"]["supported_graph_versions"],
      json!(["1.0.0", "0.1.0", "0.2.0"])
    );
    assert_eq!(
      result["identity"]["supported_node_type_versions"],
      json!(["1.0", "0.1", "0.2"])
    );
    assert_eq!(
      result["identity"]["supported_node_types"],
      json!(aether_hal::builtin_reader_node_types())
    );
    assert_eq!(
      result["identity"]["features"],
      json!(crate::PROFILE_FEATURES)
    );
    assert_eq!(result["identity"]["setup_schema_version"], 1);
  }

  #[test]
  fn setup_schema_two_preserves_selected_reply_identity() {
    let result = handle(
      &json!({"schema_version":2,"op":"identity","core":"eos"}),
      &NodeTypeRegistry::default(),
    );
    assert_eq!(result["ok"], true);
    assert_eq!(result["schema_version"], 2);
    assert_eq!(result["identity"]["setup_schema_version"], 2);
  }

  #[test]
  fn setup_schema_two_validate_reply_has_no_legacy_revision_field() {
    let graph = json!({"version":"1.0.0","robot":{"id":"robot","name":"Robot"},"cores":{"eos":{"layers":{}}}});
    let mut selected = request("validate", graph);
    selected["schema_version"] = json!(2);
    let result = handle(&selected, &NodeTypeRegistry::default());
    assert!(result.get("revision").is_none());
    assert_eq!(result["schema_version"], 2);
  }

  #[test]
  fn test_setup_projects_new_graph_to_eos_owner_before_runtime_handoff() {
    let graph = json!({
      "version":"0.1.0","robot":{"id":"robot","name":"Robot"},
      "cores":{
        "eos":{"layers":{"dispatcher":{"groups":{"control":{"nodes":{
          "command":{"id":"command","node_type":"eos.dispatcher.control@v0.1",
            "label":"Dispatcher","description":"Command admission","observation":null,"movement":null}
        }}}}}},
        "isos":{"layers":{"process":{"groups":{"component":{"nodes":{
          "eos":{"id":"eos","node_type":"isos.process.component@v0.1",
            "label":"EOS process","description":"IsOS-owned runtime process"}
        }}}}}}
      }
    });
    let registry = NodeTypeRegistry::with_contribution_files(&[]).unwrap();
    let result = handle(&request("render", graph), &registry);
    assert_eq!(result["ok"], true, "{result}");
    let rendered = result["files"][0]["content"].as_str().unwrap();
    let projected: Value = serde_json::from_str(rendered).unwrap();
    assert_eq!(projected["version"], "0.1.0");
    assert!(projected["cores"].get("eos").is_some());
    assert!(projected["cores"].get("isos").is_none());
    Profile::from_json_with_registry(rendered, &registry).unwrap();
  }

  #[test]
  fn test_setup_projects_single_core_eos_layers_without_manager_nodes() {
    let graph = json!({
      "version":"0.2.0","robot":{"id":"robot","name":"Robot"},
      "cores":{"isos":{"layers":{
        "eos_dispatcher":{"groups":{"control":{"nodes":{
          "command":{"id":"command","node_type":"isos.eos_dispatcher.control@v0.2",
            "label":"Dispatcher","description":"Command admission","observation":null,"movement":null}
        }}}},
        "process":{"groups":{"component":{"nodes":{"eos":{"id":"eos"}}}}}
      }}}
    });
    let registry = NodeTypeRegistry::builtin();
    let result = handle(&request("render", graph), &registry);
    assert_eq!(result["ok"], true, "{result}");
    let rendered = result["files"][0]["content"].as_str().unwrap();
    let projected: Value = serde_json::from_str(rendered).unwrap();
    assert_eq!(projected["version"], "0.2.0");
    assert!(projected["cores"]["isos"]["layers"]
      .get("eos_dispatcher")
      .is_some());
    assert!(projected["cores"]["isos"]["layers"]
      .get("process")
      .is_none());
    Profile::from_json_with_registry(rendered, &registry).unwrap();
  }

  #[test]
  fn test_setup_rejects_unknown_eos_type_in_new_graph() {
    let graph = json!({"version":"0.1.0","robot":{"id":"robot","name":"Robot"},
    "cores":{"eos":{"layers":{"dispatcher":{"groups":{"control":{"nodes":{
      "command":{"id":"command","node_type":"eos.dispatcher.future@v0.1",
        "label":"Unknown","description":"Unknown","observation":null,"movement":null}
    }}}}}}}});
    let result = handle(&request("render", graph), &NodeTypeRegistry::default());
    assert_eq!(result["ok"], false);
    assert_eq!(result["files"], json!([]));
    assert_eq!(result["diagnostics"][0]["code"], "invalid_graph");
  }

  #[test]
  fn test_setup_rejects_unknown_schema_and_graph_versions() {
    let graph = json!({"version":"1.0.0","robot":{"id":"robot","name":"Robot"},"cores":{}});
    let mut request = request("render", graph);
    request["schema_version"] = json!(3);
    let wrong_schema = handle(&request, &NodeTypeRegistry::default());
    assert_eq!(
      wrong_schema["diagnostics"][0]["code"],
      "unsupported_version"
    );
    assert_eq!(wrong_schema["files"], json!([]));

    request["schema_version"] = json!(1);
    request["graph"]["version"] = json!("2.0.0");
    let wrong_graph = handle(&request, &NodeTypeRegistry::default());
    assert_eq!(wrong_graph["diagnostics"][0]["code"], "invalid_graph");
    assert_eq!(wrong_graph["files"], json!([]));
  }

  #[test]
  fn test_setup_renders_demo_eos_nodes_for_runtime() {
    let source = include_str!(concat!(
      env!("CARGO_MANIFEST_DIR"),
      "/../../etc/eos/backends/mock-cxx.toml"
    ));
    let registry = NodeTypeRegistry::with_contribution_files(&[]).unwrap();
    let profile = Profile::from_toml_with_registry(source, &registry).unwrap();
    let graph = serde_json::to_value(profile).unwrap();
    let result = handle(&request("render", graph), &registry);
    assert_eq!(result["ok"], true, "{result}");
    let rendered = result["files"][0]["content"].as_str().unwrap();
    let admitted = Profile::from_json_with_registry(rendered, &registry).unwrap();
    assert!(admitted.cores.contains_key("eos"));
    assert!(admitted.all_nodes().count() > 1);
  }

  #[test]
  fn test_setup_loads_verified_package_sidecar_for_render_and_validate() {
    let directory = std::env::temp_dir().join(format!(
      "eos-setup-sidecar-{}-{}",
      std::process::id(),
      std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos()
    ));
    std::fs::create_dir(&directory).unwrap();
    let package = json!({"id":"vendor/base","version":"2.0.0"});
    let contribution = json!({"package":package,"node_types":[],"safety_adapters":[],
            "data_types":[],"adapter_families":[{"id":"vendor-base","device_kinds":["base"],
                "transport":"in_process","description":"Test package base","supported_primitives":["base.velocity@1.0.0"]}]});
    std::fs::write(directory.join("node-types.json"), contribution.to_string()).unwrap();
    let source = include_str!(concat!(
      env!("CARGO_MANIFEST_DIR"),
      "/../../etc/eos/backends/mock-cxx.toml"
    ));
    let profile = Profile::from_toml(source).unwrap();
    let mut graph = serde_json::to_value(profile).unwrap();
    let adapter = &mut graph["cores"]["eos"]["layers"]["hardware"]["groups"]["base"]["nodes"]
      ["drive"]["adapter"];
    adapter["type"] = json!("vendor-base");
    adapter["package"] = package.clone();
    let mut request = request("render", graph);
    request["packages"] = json!([{"id":"vendor/base","version":"2.0.0","path":directory}]);
    let base = NodeTypeRegistry::builtin();
    let rendered = handle(&request, &base);
    assert_eq!(rendered["ok"], true, "{rendered}");
    assert_eq!(rendered["files"][0]["path"], "eos/graph.json");
    request["op"] = json!("validate");
    let validated = handle(&request, &base);
    assert_eq!(validated["ok"], true, "{validated}");
    assert_eq!(validated["files"], json!([]));
    request["packages"][0]["version"] = json!("2.0.1");
    let mismatched = handle(&request, &base);
    assert_eq!(mismatched["ok"], false);
    assert_eq!(mismatched["diagnostics"][0]["code"], "invalid_packages");
    std::fs::remove_dir_all(directory).unwrap();
  }
}
