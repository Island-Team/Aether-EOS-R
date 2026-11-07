//! Selected node and adapter catalogs from the verified graph and package pins.

use super::*;

pub fn catalog() -> Result<Value> {
  catalog_at(&Paths::discover()?)
}

pub fn catalog_at(paths: &Paths) -> Result<Value> {
  catalog_with_sidecars(packages::node_type_sidecars(paths)?)
}

/// Built-in EOS registry without installed package contributions. Option
/// projection resolves package selectors and selected EGIA stages from the draft.
pub fn catalog_base() -> Result<Value> {
  catalog_with_sidecars(Vec::new())
}

/// Resolve only the package process pins used by an editable draft. A
/// missing or changed pin is omitted so node-options can show a repair row.
/// Installed releases outside this graph cannot contribute selector metadata.
pub fn catalog_for_option_graph(paths: &Paths, graph: &Value) -> Result<Value> {
  if graph["version"] != "0.1.0" && graph["version"] != "0.2.0" {
    // Historical graphs expose only the three read-only collections from
    // EOS's built-in registry. Unrelated installed releases cannot change
    // those collections or make inspection fail on duplicate type IDs.
    return catalog_base();
  }
  let mut sidecars = BTreeSet::new();
  for node in graph["cores"]
    .as_object()
    .into_iter()
    .flat_map(|cores| cores.values())
    .flat_map(|core| {
      core["layers"]
        .as_object()
        .into_iter()
        .flat_map(|layers| layers.values())
    })
    .flat_map(|layer| {
      layer["groups"]
        .as_object()
        .into_iter()
        .flat_map(|groups| groups.values())
    })
    .flat_map(|group| {
      group["nodes"]
        .as_object()
        .into_iter()
        .flat_map(|nodes| nodes.values())
    })
  {
    if let (Some(id), Some(version)) = (
      node["package"]["id"].as_str(),
      node["package"]["version"].as_str(),
    ) && let Ok(verified) = packages::verify(paths, id, version)
      && let Some(sidecar) = verified.sidecar
    {
      sidecars.insert(sidecar);
    }
  }
  let catalog = match sidecars.is_empty() {
    false => catalog_with_sidecars(sidecars.into_iter().collect())?,
    true => catalog_base()?,
  };
  let selected_stages = (graph.pointer("/cores/egia").is_some()
    || (graph["version"] == "0.2.0"
      && graph
        .pointer("/cores/isos/layers")
        .and_then(Value::as_object)
        .is_some_and(|layers| layers.keys().any(|name| name.starts_with("ei_")))))
    && crate::runtime::owners::core_setup::identity_for_graph(paths, "egia", graph)
      .ok()
      .and_then(|identity| identity["supported_node_types"].as_array().cloned())
      .is_some_and(|types| {
        types.iter().any(|item| {
          item
            == if graph["version"] == "0.2.0" {
              "isos.ei_io.gateway@v0.2"
            } else {
              "egia.link.gateway@v0.1"
            }
        })
      });
  with_egia_stage_catalog(paths, graph, catalog, selected_stages, None)
}

pub fn catalog_for_graph(paths: &Paths, graph: &Value) -> Result<Value> {
  let verified = packages::verify_graph_packages(paths, graph)?;
  let sidecars = verified
    .iter()
    .filter_map(|package| package.sidecar.clone())
    .collect();
  let catalog = catalog_with_sidecars(sidecars)?;
  with_egia_stage_catalog(
    paths,
    graph,
    catalog,
    graph_compat::has_egia_stages(graph)
      || (graph["version"] == "0.2.0"
        && graph
          .pointer("/cores/isos/layers")
          .and_then(Value::as_object)
          .is_some_and(|layers| layers.keys().any(|name| name.starts_with("ei_")))),
    Some(&verified),
  )
}

fn with_egia_stage_catalog(
  paths: &Paths,
  graph: &Value,
  mut catalog: Value,
  include: bool,
  verified: Option<&[packages::VerifiedPackage]>,
) -> Result<Value> {
  if graph_compat::uses_revisioned_generation(graph) && include {
    let bytes = serde_json::to_vec(graph)?;
    let revision = graph_compat::revision(&bytes);
    let reply = if let Some(verified) = verified {
      crate::runtime::owners::core_setup::catalog_with_verified_packages(
        paths, "default", "egia", graph, &revision, verified,
      )?
    } else {
      crate::runtime::owners::core_setup::call(
        paths, "default", "egia", "catalog", graph, &revision,
      )?
    };
    merge_egia_stage_catalog(&mut catalog, &reply)?;
    let feature =
      crate::runtime::process::reader_features::egia_evidence_route_feature(paths, graph)?;
    let descriptors = catalog[if graph["version"] == "0.2.0" {
      "ei_routed_edges"
    } else {
      "egia_routed_edges"
    }]
    .as_array()
    .is_some_and(|routes| !routes.is_empty());
    ensure!(
      feature == descriptors,
      "selected EGIA evidence routing feature and catalog disagree"
    );
  }
  Ok(catalog)
}

pub(crate) fn merge_egia_stage_catalog(catalog: &mut Value, reply: &Value) -> Result<()> {
  ensure!(reply["ok"] == true, "EGIA owner catalog rejected graph");
  let stages = reply["catalog"]["stage_types"]
    .as_array()
    .context("EGIA owner catalog lacks stage_types")?;
  let data = reply["catalog"]["data_types"]
    .as_array()
    .context("EGIA owner catalog lacks data_types")?;
  let routes = match reply["catalog"].get("routed_edges") {
    None => None,
    Some(value) => Some(
      value
        .as_array()
        .context("EGIA owner catalog has invalid routed_edges")?,
    ),
  };
  let unified = stages.iter().any(|stage| {
    stage["id"]
      .as_str()
      .is_some_and(|id| id.starts_with("isos.ei_"))
  });
  let expected_routes: BTreeSet<(&str, &str, &str, &str)> = if unified {
    [
      (
        "isos.ei_io.gateway@v0.2",
        "resource",
        "isos.ei_evidence.admission@v0.2",
        "resource",
      ),
      (
        "isos.ei_evidence.admission@v0.2",
        "observations",
        "isos.ei_cognition.builder@v0.2",
        "observations",
      ),
    ]
  } else {
    [
      (
        "egia.link.gateway@v0.1",
        "resource",
        "egia.perception.admission@v0.1",
        "resource",
      ),
      (
        "egia.perception.admission@v0.1",
        "observations",
        "egia.context.builder@v0.1",
        "observations",
      ),
    ]
  }
  .into_iter()
  .collect();
  let mut actual_routes = BTreeSet::new();
  for route in routes.into_iter().flatten() {
    let pair = (
      route["source_type"]
        .as_str()
        .context("EGIA route lacks source_type")?,
      route["output"]
        .as_str()
        .context("EGIA route lacks output")?,
      route["target_type"]
        .as_str()
        .context("EGIA route lacks target_type")?,
      route["input"].as_str().context("EGIA route lacks input")?,
    );
    ensure!(
      route.as_object().is_some_and(|fields| fields.len() == 5),
      "EGIA route has unknown fields"
    );
    ensure!(
      route["controls"] == serde_json::json!(["priority", "frequency_hz"]),
      "EGIA route has invalid controls"
    );
    ensure!(
      expected_routes.contains(&pair),
      "EGIA route is not an approved evidence pair"
    );
    ensure!(
      actual_routes.insert(pair),
      "EGIA owner catalog repeats an evidence route"
    );
  }
  if routes.is_some() {
    ensure!(
      actual_routes == expected_routes,
      "EGIA owner catalog is missing evidence routes"
    );
  }
  let expected: BTreeSet<&str> = if unified {
    [
      "isos.ei_io.gateway@v0.2",
      "isos.ei_evidence.admission@v0.2",
      "isos.ei_cognition.builder@v0.2",
      "isos.ei_cognition.queue@v0.2",
      "isos.ei_cognition.planner@v0.2",
      "isos.ei_governance.gate@v0.2",
      "isos.ei_governance.health@v0.2",
    ]
  } else {
    [
      "egia.link.gateway@v0.1",
      "egia.perception.admission@v0.1",
      "egia.context.builder@v0.1",
      "egia.missions.queue@v0.1",
      "egia.decision.planner@v0.1",
      "egia.safety.gate@v0.1",
      "egia.monitor.health@v0.1",
    ]
  }
  .into_iter()
  .collect();
  let mut actual = BTreeSet::new();
  let existing: BTreeSet<&str> = catalog["types"]
    .as_array()
    .context("aggregate catalog lacks types")?
    .iter()
    .filter_map(|entry| entry["id"].as_str())
    .collect();
  let mut normalized_stages = Vec::with_capacity(stages.len());
  for stage in stages {
    let id = stage["id"].as_str().context("EGIA stage lacks id")?;
    ensure!(actual.insert(id), "EGIA owner catalog repeats stage {id}");
    let placement = if unified {
      match id {
        "isos.ei_io.gateway@v0.2" => Some(("isos", "ei_io", "link")),
        "isos.ei_evidence.admission@v0.2" => Some(("isos", "ei_evidence", "perception")),
        "isos.ei_cognition.builder@v0.2" => Some(("isos", "ei_cognition", "context")),
        "isos.ei_cognition.queue@v0.2" => Some(("isos", "ei_cognition", "missions")),
        "isos.ei_cognition.planner@v0.2" => Some(("isos", "ei_cognition", "decision")),
        "isos.ei_governance.gate@v0.2" => Some(("isos", "ei_governance", "safety")),
        "isos.ei_governance.health@v0.2" => Some(("isos", "ei_governance", "monitor")),
        _ => None,
      }
    } else {
      graph_compat::target_placement(id)
    };
    ensure!(
      expected.contains(id),
      "EGIA owner catalog has unapproved stage {id}"
    );
    let (core, layer, group) = placement.context("EGIA stage placement is unavailable")?;
    ensure!(
      stage["core"] == core && stage["layer"] == layer && stage["group"] == group,
      "EGIA owner catalog has invalid placement for {id}"
    );
    ensure!(
      !existing.contains(id),
      "EGIA stage {id} conflicts with another owner"
    );
    ensure!(
      stage["description"]
        .as_str()
        .is_some_and(|text| !text.trim().is_empty()),
      "EGIA stage {id} lacks description"
    );
    ensure!(
      stage["inputs"].is_array() && stage["outputs"].is_array(),
      "EGIA stage {id} lacks port arrays"
    );
    let fields = stage["settings_fields"]
      .as_array()
      .with_context(|| format!("EGIA stage {id} lacks settings_fields"))?;
    ensure!(
      stage.get("settings").is_none(),
      "EGIA stage {id} has conflicting settings metadata"
    );
    let mut names = BTreeSet::new();
    for field in fields {
      let name = field["name"]
        .as_str()
        .filter(|name| !name.trim().is_empty())
        .with_context(|| format!("EGIA stage {id} has an unnamed setting"))?;
      ensure!(names.insert(name), "EGIA stage {id} repeats setting {name}");
      ensure!(
        field["type"].as_str().is_some_and(|kind| !kind.is_empty())
          && field["label"]
            .as_str()
            .is_some_and(|label| !label.trim().is_empty())
          && field["description"]
            .as_str()
            .is_some_and(|description| !description.trim().is_empty())
          && field["required"].is_boolean(),
        "EGIA stage {id} has invalid setting {name}"
      );
    }
    let mut normalized = stage.clone();
    let mut ui_fields = fields.clone();
    for field in &mut ui_fields {
      // Keep the owner's original schema and translate only the shared
      // editor discriminator. Unsupported future shapes fail closed.
      field["data_type"] = Value::String(stage_editor_type(field)?.into());
    }
    normalized["settings"] = Value::Array(ui_fields);
    normalized_stages.push(normalized);
  }
  ensure!(
    actual == expected,
    "EGIA owner catalog is missing registered stages"
  );
  let existing_data: BTreeSet<&str> = catalog["data_types"]
    .as_array()
    .context("aggregate catalog lacks data_types")?
    .iter()
    .filter_map(|entry| entry["id"].as_str())
    .collect();
  let mut data_ids = BTreeSet::new();
  for entry in data {
    let id = entry["id"].as_str().context("EGIA data type lacks id")?;
    ensure!(
      data_ids.insert(id),
      "EGIA owner catalog repeats data type {id}"
    );
    ensure!(
      !existing_data.contains(id),
      "EGIA data type {id} conflicts with another owner"
    );
    ensure!(
      entry["schema"].is_object(),
      "EGIA data type {id} lacks schema"
    );
  }
  for stage in stages {
    for direction in ["inputs", "outputs"] {
      for port in stage[direction].as_array().unwrap() {
        let id = port["data_type"]
          .as_str()
          .context("EGIA stage port lacks data type")?;
        ensure!(
          data_ids.contains(id) || existing_data.contains(id),
          "EGIA stage port references unknown data type {id}"
        );
      }
    }
  }
  catalog["types"]
    .as_array_mut()
    .unwrap()
    .extend(normalized_stages);
  catalog["data_types"]
    .as_array_mut()
    .unwrap()
    .extend(data.iter().cloned());
  catalog[if unified {
    "ei_routed_edges"
  } else {
    "egia_routed_edges"
  }] = Value::Array(routes.cloned().unwrap_or_default());
  Ok(())
}

fn stage_editor_type(field: &Value) -> Result<&'static str> {
  let name = field["name"].as_str().unwrap_or("unnamed");
  match field["type"].as_str() {
    Some("string") => Ok("string"),
    Some("endpoint") => Ok("endpoint"),
    Some("integer") if field["min"].as_i64().is_some_and(|min| min >= 1) => Ok("positive_integer"),
    Some("number") if field["exclusive_min"].as_f64() == Some(0.0) => Ok("positive_number"),
    _ => anyhow::bail!("EGIA stage setting {name} has no supported editor type"),
  }
}

fn catalog_with_sidecars(sidecars: Vec<PathBuf>) -> Result<Value> {
  let program = catalog_program()?;
  let mut command = Command::new(&program);
  command.arg("catalog");
  let output = command
    .output()
    .with_context(|| format!("run EOS type catalog {}", Path::new(&program).display()))?;
  ensure!(
    output.status.success(),
    "EOS catalog failed: {}",
    String::from_utf8_lossy(&output.stderr).trim()
  );
  ensure!(
    output.stdout.len() <= MAX_GRAPH_BYTES as usize,
    "EOS catalog exceeds 1 MiB"
  );
  let mut catalog: Value = serde_json::from_slice(&output.stdout)?;
  ensure!(
    catalog.get("types").and_then(Value::as_array).is_some(),
    "EOS catalog has no types array"
  );
  let mut contributed = BTreeMap::new();
  let mut contributed_data = BTreeMap::new();
  let mut contributed_adapters = BTreeMap::new();
  for sidecar in sidecars {
    let value: Value = serde_json::from_slice(&fs::read(&sidecar)?)?;
    let admitted = Command::new(&program)
      .arg("catalog")
      .arg("--node-types")
      .arg(&sidecar)
      .output()
      .with_context(|| format!("catalog package sidecar {}", sidecar.display()))?;
    ensure!(
      admitted.status.success(),
      "EOS rejected package sidecar {}: {}",
      sidecar.display(),
      String::from_utf8_lossy(&admitted.stderr).trim()
    );
    let admitted: Value = serde_json::from_slice(&admitted.stdout)?;
    if let Some(types) = value["node_types"].as_array() {
      for node_type in types {
        let id = node_type["id"]
          .as_str()
          .context("contributed node type lacks id")?;
        let mut node_type = node_type.clone();
        node_type["package"] = value["package"].clone();
        ensure!(
          contributed.insert(id.to_owned(), node_type).is_none(),
          "installed packages contribute duplicate node type {id}; remove one package version"
        );
      }
    }
    if value["package"]["id"] == "island/mujoco-sim"
      && value["package"]["version"] == "0.1.1"
      && value["node_types"].as_array().is_some_and(|types| {
        types
          .iter()
          .any(|node| node["id"] == "eos.process.mujoco@v0.1")
      })
    {
      let id = "isos.eos_process.mujoco@v0.2";
      let mut alias = admitted["types"]
        .as_array()
        .and_then(|types| types.iter().find(|entry| entry["id"] == id))
        .cloned()
        .context("EOS omitted pinned MuJoCo single-core type")?;
      alias["package"] = value["package"].clone();
      ensure!(
        contributed.insert(id.into(), alias).is_none(),
        "duplicate pinned MuJoCo single-core type"
      );
    }
    if let Some(data_types) = value["data_types"].as_array() {
      for data_type in data_types {
        let id = data_type["id"]
          .as_str()
          .context("contributed data type lacks id")?;
        let mut data_type = data_type.clone();
        data_type["package"] = value["package"].clone();
        ensure!(
          contributed_data.insert(id.to_owned(), data_type).is_none(),
          "installed packages contribute duplicate data type {id}; remove one package version"
        );
      }
    }
    if let Some(adapters) = admitted["adapter_families"].as_array() {
      for adapter in adapters {
        let id = adapter["id"]
          .as_str()
          .context("contributed adapter family lacks id")?;
        if !value["adapter_families"]
          .as_array()
          .is_some_and(|families| families.iter().any(|family| family["id"] == id))
        {
          continue;
        }
        let mut adapter = adapter.clone();
        adapter["package"] = value["package"].clone();
        ensure!(
          contributed_adapters
            .insert(id.to_owned(), adapter)
            .is_none(),
          "installed packages contribute duplicate adapter family {id}"
        );
      }
    }
  }
  let types = catalog["types"].as_array_mut().unwrap();
  {
    types.retain(|item| item["id"] != "isos.process.component@v0.1");
    let mut component = types
      .iter()
      .find(|item| item["id"] == "isos.process.component@v1.1")
      .context("EOS catalog lacks IsOS legacy component definition")?
      .clone();
    component["id"] = Value::String("isos.process.component@v0.1".into());
    component["description"] =
      Value::String("IsOS-owned role-selected child with bounded arguments and readiness".into());
    if let Some(settings) = component["settings"].as_array_mut() {
      settings.push(serde_json::json!({
          "name":"env_from","data_type":"string_table","required":false,
          "description":"Child environment targets and manager environment source names"
      }));
      settings.push(crate::runtime::process::skill_selection::setting_descriptor());
    }
    types.push(component.clone());
    component["id"] = Value::String("isos.process.component@v0.2".into());
    types.push(component);
  }
  // These are ordinary registered types. The browser owns the producer and
  // IsOS owns the HTTP bridge; neither grants a package permission to execute.
  types.push(serde_json::json!({
    "id":"isosui.input.keyboard@v0.1",
    "description":"Publishes the selected keyboard state while the workbench is armed for one run.",
    "core":"isosui","layer":"input","group":"keyboard","kind":"software",
    "lifecycle":"internal","capabilities":["user_input"],
    "settings":[
      {"name":"key","data_type":"string","required":true,"description":"KeyboardEvent.code selected by the operator",
        "selector":{"provider":"ui","resource":"keys"}},
      {"name":"modifiers","data_type":"string_array","required":false,"description":"Required Control, Shift, Alt, or Meta modifiers"}
    ],
    "inputs":[],
    "outputs":[{"name":"state","data_type":"key_state","required":false,
      "cardinality":"one","description":"Run-bound keyboard state event",
      "transport":"http","delivery_mode":"event","data_class":"event"}]
  }));
  types.push(serde_json::json!({
    "id":"isos.interface.http_gateway@v0.1",
    "description":"Converts a run-bound input event to a configured typed movement request.",
    "core":"isos","layer":"interface","group":"gateway","kind":"software",
    "lifecycle":"internal","capabilities":["bridge"],
    "bridge":{"adapter":"http","inputs":["state"],"outputs":["movement"]},
    "settings":[
      {"name":"target","data_type":"string","required":true,
        "description":"Exact Dispatcher primitive binding node path",
        "selector":{"provider":"graph","resource":"node","filter":{
          "core":"eos","capability":"primitive_binding",
          "output_data_type":"base_movement","output_transport":"in_process"}}},
      {"name":"x_m_s","data_type":"number","required":true,"unit":"m/s","description":"Body-frame forward velocity"},
      {"name":"y_m_s","data_type":"number","required":true,"unit":"m/s","description":"Body-frame lateral velocity"},
      {"name":"yaw_rad_s","data_type":"number","required":true,"unit":"rad/s","description":"Body-frame angular velocity"},
      {"name":"valid_for_ms","data_type":"positive_integer","required":true,"unit":"ms",
        "min":50,"max":500,"description":"Maximum movement lease for one refresh"}
    ],
    "inputs":[{"name":"state","data_type":"key_state","required":true,
      "cardinality":"one","description":"Keyboard state from a registered producer",
      "transport":"http","delivery_mode":"event","data_class":"event"}],
    "outputs":[{"name":"movement","data_type":"base_movement","required":false,
      "cardinality":"one","description":"Typed movement request to the selected owner gateway",
      "transport":"http","delivery_mode":"stream","data_class":"command"}]
  }));
  let keyboard = types
    .iter()
    .find(|entry| entry["id"] == "isosui.input.keyboard@v0.1")
    .context("browser keyboard type missing")?
    .clone();
  let mut unified_keyboard = keyboard;
  unified_keyboard["id"] = Value::String("isos.ui.keyboard@v0.2".into());
  unified_keyboard["core"] = Value::String("isos".into());
  unified_keyboard["layer"] = Value::String("ui".into());
  types.push(unified_keyboard);
  let gateway = types
    .iter()
    .find(|entry| entry["id"] == "isos.interface.http_gateway@v0.1")
    .context("IsOS gateway type missing")?
    .clone();
  let mut unified_gateway = gateway;
  unified_gateway["id"] = Value::String("isos.interface.http_gateway@v0.2".into());
  if let Some(settings) = unified_gateway["settings"].as_array_mut()
    && let Some(target) = settings
      .iter_mut()
      .find(|setting| setting["name"] == "target")
  {
    target["selector"]["filter"]["core"] = Value::String("isos".into());
  }
  types.push(unified_gateway);
  for id in [
    "isosui.input.keyboard@v0.1",
    "isos.interface.http_gateway@v0.1",
    "isos.ui.keyboard@v0.2",
    "isos.interface.http_gateway@v0.2",
  ] {
    let definition = types
      .iter()
      .find(|entry| entry["id"] == id)
      .context("built-in node type missing after registration")?;
    crate::domain::registry::node_schema::validate_type(definition)?;
  }
  let builtin: BTreeSet<_> = types
    .iter()
    .filter_map(|value| value["id"].as_str().map(str::to_owned))
    .collect();
  for (id, node_type) in contributed {
    ensure!(
      !builtin.contains(&id),
      "package node type {id} conflicts with built-in type"
    );
    types.push(node_type);
  }
  catalog["bridge_adapters"] = serde_json::json!([
    {"core":"isos","adapter":"http","node_type":"isos.interface.http_gateway@v0.1"},
    {"core":"eos","adapter":"http","node_type":"eos.interface.http_gateway@v0.1"}
  ]);
  if !catalog["data_types"].is_array() {
    catalog["data_types"] = serde_json::json!([]);
  }
  let data_types = catalog["data_types"].as_array_mut().unwrap();
  data_types.push(serde_json::json!({
    "id":"key_state","version":"0.1.0","purpose":"Bounded operator key state",
    "media_type":"application/json","codec":"json_v1","max_payload_bytes":512,
    "schema":{"type":"object","additionalProperties":false,
      "properties":{"state":{"type":"string","enum":["pressed","refresh","released"]},
        "activation_id":{"type":"string","minLength":1,"maxLength":64},
        "sequence":{"type":"integer","minimum":1}},
      "required":["state","activation_id","sequence"]}
  }));
  let key_state = data_types
    .iter()
    .find(|entry| entry["id"] == "key_state")
    .context("built-in key_state data type missing after registration")?;
  crate::domain::registry::node_schema::validate_data_type(key_state)?;
  let builtin_data: BTreeSet<_> = data_types
    .iter()
    .filter_map(|value| value["id"].as_str().map(str::to_owned))
    .collect();
  for (id, data_type) in contributed_data {
    ensure!(
      !builtin_data.contains(&id),
      "package data type {id} conflicts with built-in type"
    );
    data_types.push(data_type);
  }
  if !catalog["adapter_families"].is_array() {
    catalog["adapter_families"] = serde_json::json!([]);
  }
  let families = catalog["adapter_families"].as_array_mut().unwrap();
  let builtin_families: BTreeSet<_> = families
    .iter()
    .filter_map(|value| value["id"].as_str().map(str::to_owned))
    .collect();
  for (id, family) in contributed_adapters {
    ensure!(
      !builtin_families.contains(&id),
      "package adapter family {id} conflicts with built-in family"
    );
    families.push(family);
  }
  Ok(catalog)
}

fn catalog_program() -> Result<PathBuf> {
  #[cfg(test)]
  if let Some(path) = std::env::var_os("ISOS_TEST_CATALOG_PROFILE") {
    return checked_test_catalog_program(PathBuf::from(path));
  }
  core_bins::resolve("aether-profile")
}

#[cfg(test)]
pub(super) fn checked_test_catalog_program(path: PathBuf) -> Result<PathBuf> {
  ensure!(
    path.is_absolute(),
    "test EOS catalog profile must be absolute"
  );
  ensure!(
    fs::metadata(&path)
      .is_ok_and(|metadata| { metadata.is_file() && metadata.permissions().mode() & 0o111 != 0 }),
    "test EOS catalog profile is missing or not executable"
  );
  Ok(path)
}

#[cfg(test)]
mod egia_stage_catalog_tests {
  use super::*;
  use serde_json::json;

  #[test]
  fn unified_builtin_catalog_types_are_registered() {
    let catalog = catalog_base().unwrap();
    for id in [
      "isos.process.component@v0.2",
      "isos.ui.keyboard@v0.2",
      "isos.interface.http_gateway@v0.2",
    ] {
      assert!(
        catalog["types"]
          .as_array()
          .unwrap()
          .iter()
          .any(|entry| entry["id"] == id),
        "{id}"
      );
    }
  }

  #[test]
  fn unified_ei_catalog_requires_exact_stage_placements() {
    let entries = [
      ("isos.ei_io.gateway@v0.2", "ei_io", "link"),
      (
        "isos.ei_evidence.admission@v0.2",
        "ei_evidence",
        "perception",
      ),
      ("isos.ei_cognition.builder@v0.2", "ei_cognition", "context"),
      ("isos.ei_cognition.queue@v0.2", "ei_cognition", "missions"),
      ("isos.ei_cognition.planner@v0.2", "ei_cognition", "decision"),
      ("isos.ei_governance.gate@v0.2", "ei_governance", "safety"),
      ("isos.ei_governance.health@v0.2", "ei_governance", "monitor"),
    ];
    let stages: Vec<_> = entries
      .iter()
      .map(|(id, layer, group)| {
        json!({
          "id":id,"core":"isos","layer":layer,"group":group,
          "description":"EI owner stage","inputs":[],"outputs":[],"settings_fields":[]
        })
      })
      .collect();
    let reply = json!({"ok":true,"catalog":{"stage_types":stages,"data_types":[],
    "routed_edges":[
      {"source_type":"isos.ei_io.gateway@v0.2","output":"resource",
        "target_type":"isos.ei_evidence.admission@v0.2","input":"resource",
        "controls":["priority","frequency_hz"]},
      {"source_type":"isos.ei_evidence.admission@v0.2","output":"observations",
        "target_type":"isos.ei_cognition.builder@v0.2","input":"observations",
        "controls":["priority","frequency_hz"]}
    ]}});
    let mut catalog = json!({"types":[],"data_types":[]});
    merge_egia_stage_catalog(&mut catalog, &reply).unwrap();
    assert_eq!(catalog["types"].as_array().unwrap().len(), 7);
    assert_eq!(catalog["ei_routed_edges"], reply["catalog"]["routed_edges"]);
    assert!(catalog.get("egia_routed_edges").is_none());
    let mut wrong = reply;
    wrong["catalog"]["stage_types"][0]["group"] = json!("unsafe");
    assert!(merge_egia_stage_catalog(&mut json!({"types":[],"data_types":[]}), &wrong).is_err());
  }

  #[test]
  fn owner_setting_types_translate_only_when_editor_can_preserve_values() {
    assert_eq!(
      stage_editor_type(&json!({"name":"capacity","type":"integer","min":1})).unwrap(),
      "positive_integer"
    );
    assert_eq!(
      stage_editor_type(&json!({"name":"step_timeout_s","type":"number","exclusive_min":0}))
        .unwrap(),
      "positive_number"
    );
    assert_eq!(
      stage_editor_type(&json!({"name":"gateway","type":"endpoint"})).unwrap(),
      "endpoint"
    );
    assert!(stage_editor_type(&json!({"name":"capacity","type":"integer"})).is_err());
    assert!(stage_editor_type(&json!({"name":"ratio","type":"number"})).is_err());
    assert!(stage_editor_type(&json!({"name":"flag","type":"boolean"})).is_err());
  }

  #[test]
  fn owner_stage_descriptors_and_schemas_are_copied_without_substitution() {
    let mut catalog = json!({"types":[],"data_types":[]});
    let mut stages = Vec::new();
    for id in [
      "egia.link.gateway@v0.1",
      "egia.perception.admission@v0.1",
      "egia.context.builder@v0.1",
      "egia.missions.queue@v0.1",
      "egia.decision.planner@v0.1",
      "egia.safety.gate@v0.1",
      "egia.monitor.health@v0.1",
    ] {
      let (core, layer, group) = graph_compat::target_placement(id).unwrap();
      stages.push(json!({"id":id,"core":core,"layer":layer,"group":group,
                "description":"owner stage","inputs":[{"name":"in","data_type":"egia.stage.test@v0.1"}],
                "outputs":[],"settings_fields":[{"name":"controller.model","label":"Controller model",
                    "description":"Owner model binding","required":false,"type":"string"}]}));
    }
    let data = json!([{"id":"egia.stage.test@v0.1",
            "schema":{"type":"object","properties":{"value":{"type":"integer"}}}}]);
    let routes = json!([
        {"source_type":"egia.link.gateway@v0.1","output":"resource","target_type":"egia.perception.admission@v0.1","input":"resource","controls":["priority","frequency_hz"]},
        {"source_type":"egia.perception.admission@v0.1","output":"observations","target_type":"egia.context.builder@v0.1","input":"observations","controls":["priority","frequency_hz"]}
    ]);
    let reply =
      json!({"ok":true,"catalog":{"stage_types":stages,"data_types":data,"routed_edges":routes}});
    merge_egia_stage_catalog(&mut catalog, &reply).unwrap();
    assert_eq!(catalog["types"][0]["settings"][0]["data_type"], "string");
    assert_eq!(
      catalog["types"][0]["settings_fields"],
      reply["catalog"]["stage_types"][0]["settings_fields"]
    );
    assert_eq!(catalog["data_types"], reply["catalog"]["data_types"]);
    assert_eq!(
      catalog["egia_routed_edges"],
      reply["catalog"]["routed_edges"]
    );
    let mut a01 = reply.clone();
    a01["catalog"]
      .as_object_mut()
      .unwrap()
      .remove("routed_edges");
    let mut a01_catalog = json!({"types":[],"data_types":[]});
    merge_egia_stage_catalog(&mut a01_catalog, &a01).unwrap();
    assert_eq!(a01_catalog["egia_routed_edges"], json!([]));
    for routes in [
      json!([]),
      json!([reply["catalog"]["routed_edges"][0]]),
      json!([
        reply["catalog"]["routed_edges"][0],
        reply["catalog"]["routed_edges"][0]
      ]),
      json!([reply["catalog"]["routed_edges"][0], reply["catalog"]["routed_edges"][1],
                {"source_type":"egia.context.builder@v0.1","output":"snapshot","target_type":"egia.decision.planner@v0.1","input":"snapshot","controls":["priority","frequency_hz"]}]),
    ] {
      let mut wrong = reply.clone();
      wrong["catalog"]["routed_edges"] = routes;
      assert!(merge_egia_stage_catalog(&mut json!({"types":[],"data_types":[]}), &wrong).is_err());
    }
    let mut wrong_route = reply.clone();
    wrong_route["catalog"]["routed_edges"][0]["controls"] = json!(["frequency_hz", "priority"]);
    assert!(
      merge_egia_stage_catalog(&mut json!({"types":[],"data_types":[]}), &wrong_route).is_err()
    );
    let mut wrong = reply.clone();
    wrong["catalog"]["stage_types"][0]["group"] = json!("unsafe");
    assert!(merge_egia_stage_catalog(&mut json!({"types":[],"data_types":[]}), &wrong).is_err());
    let mut wrong = reply.clone();
    wrong["catalog"]["stage_types"][0]["settings"] = json!([]);
    assert!(merge_egia_stage_catalog(&mut json!({"types":[],"data_types":[]}), &wrong).is_err());
    let mut wrong = reply.clone();
    let duplicate = wrong["catalog"]["stage_types"][0]["settings_fields"][0].clone();
    wrong["catalog"]["stage_types"][0]["settings_fields"]
      .as_array_mut()
      .unwrap()
      .push(duplicate);
    assert!(merge_egia_stage_catalog(&mut json!({"types":[],"data_types":[]}), &wrong).is_err());
    wrong = reply;
    wrong["catalog"]["data_types"] = json!([]);
    assert!(merge_egia_stage_catalog(&mut json!({"types":[],"data_types":[]}), &wrong).is_err());
  }
}
