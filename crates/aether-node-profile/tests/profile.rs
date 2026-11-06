use aether_node_profile::{
  AdapterFamilyDescriptor, DataClass, DataTypeDescriptor, Lifecycle, NodeKind, NodeType,
  NodeTypeContribution, NodeTypeRegistry, PackageRef, PortDataType, PortTransport, Profile,
  SafetyAdapterType, Setting, SettingType,
};

fn new_eos_graph() -> serde_json::Value {
  let mut graph = serde_json::to_value(Profile::from_toml(DEMO).unwrap()).unwrap();
  graph["version"] = "0.1.0".into();
  graph["cores"].as_object_mut().unwrap().remove("egia");
  let eos = &mut graph["cores"]["eos"]["layers"];
  eos["dispatcher"]["groups"]["control"]["nodes"]["command"]["inputs"] = serde_json::json!({});
  for layer in eos.as_object_mut().unwrap().values_mut() {
    for group in layer["groups"].as_object_mut().unwrap().values_mut() {
      for node in group["nodes"].as_object_mut().unwrap().values_mut() {
        let old = node["node_type"].as_str().unwrap();
        node["node_type"] = old.replace("@v1.0", "@v0.1").into();
      }
    }
  }
  graph
}

fn single_core_graph() -> serde_json::Value {
  fn rewrite(value: &mut serde_json::Value) {
    match value {
      serde_json::Value::Object(fields) => {
        for value in fields.values_mut() {
          rewrite(value);
        }
      }
      serde_json::Value::Array(items) => {
        for value in items {
          rewrite(value);
        }
      }
      serde_json::Value::String(text) if text.starts_with("eos/") => {
        let parts = text.split('/').collect::<Vec<_>>();
        if parts.len() == 4 {
          *text = format!("isos/eos_{}/{}/{}", parts[1], parts[2], parts[3]);
        }
      }
      serde_json::Value::String(text) if text.starts_with("eos.") && text.ends_with("@v0.1") => {
        let versionless = text.strip_suffix("@v0.1").unwrap();
        let parts = versionless.split('.').collect::<Vec<_>>();
        if parts.len() == 3 {
          *text = format!("isos.eos_{}.{}@v0.2", parts[1], parts[2]);
        }
      }
      _ => {}
    }
  }
  let mut graph = new_eos_graph();
  graph["version"] = "0.2.0".into();
  let eos = graph["cores"]
    .as_object_mut()
    .unwrap()
    .remove("eos")
    .unwrap();
  let mut layers = serde_json::Map::new();
  for (name, layer) in eos["layers"].as_object().unwrap() {
    layers.insert(format!("eos_{name}"), layer.clone());
  }
  graph["cores"] = serde_json::json!({"isos":{"layers":layers}});
  rewrite(&mut graph);
  graph
}

#[test]
fn single_core_eos_projection_keeps_hardware_and_admission() {
  let graph = single_core_graph();
  let profile = Profile::from_json(&graph.to_string()).unwrap();
  assert_eq!(profile.hardware_nodes().count(), 3);
  assert!(profile.node("isos/eos_hardware/camera/front").is_some());
  let mut extra = graph;
  extra["cores"]["egia"] = serde_json::json!({"layers":{}});
  assert!(Profile::from_json(&extra.to_string()).is_err());
}

fn mujoco_registry() -> NodeTypeRegistry {
  let mut registry = NodeTypeRegistry::builtin();
  let mut process = registry.get("eos.process.backend@v1.0").unwrap().clone();
  process.id = "eos.process.mujoco@v0.1".into();
  process.group = "mujoco".into();
  process.description = "Runs the verified MuJoCo simulator and publishes its state".into();
  process
    .settings
    .retain(|s| !matches!(s.name.as_str(), "package" | "version"));
  for setting in &mut process.settings {
    setting.description = Some(format!("Verified {} setting", setting.name));
  }
  for port in process.inputs.iter_mut().chain(process.outputs.iter_mut()) {
    port.description = Some(format!("Verified {} port", port.name));
    port.transport = Some(PortTransport::InProcess);
    port.data_class = Some(DataClass::Event);
  }
  for port in &mut process.outputs {
    port.description = Some(format!("Live {} from simulator", port.name));
    port.transport = Some(PortTransport::InProcess);
    port.data_class = Some(DataClass::State);
  }
  registry
    .register_contribution(NodeTypeContribution {
      package: PackageRef {
        id: "island/mujoco-sim".into(),
        version: "0.1.1".into(),
      },
      node_types: vec![process],
      safety_adapters: vec![],
      data_types: vec![],
      adapter_families: vec![AdapterFamilyDescriptor {
        id: "mujoco-v2".into(),
        device_kinds: vec![NodeKind::Camera, NodeKind::Base, NodeKind::Manipulator],
        transport: PortTransport::UnixSocket,
        description: "Verified MuJoCo device socket".into(),
        settings_schema: None,
        supported_primitives: vec!["base.velocity@1.0.0".into(), "arm.movej@1.0.0".into()],
      }],
    })
    .unwrap();
  registry
}

#[test]
fn verified_mujoco_contribution_maps_to_single_core_without_losing_pin() {
  let registry = mujoco_registry();
  let original = registry.get("eos.process.mujoco@v0.1").unwrap();
  let mapped = registry.get("isos.eos_process.mujoco@v0.2").unwrap();
  assert_eq!(mapped.core, "isos");
  assert_eq!(mapped.layer, "eos_process");
  assert_eq!(mapped.group, original.group);
  assert_eq!(mapped.settings.len(), original.settings.len());
  assert_eq!(
    registry.package_for_type(&mapped.id),
    registry.package_for_type(&original.id)
  );
}

#[test]
fn new_eos_catalog_has_complete_descriptors_and_legacy_compatibility() {
  let registry = NodeTypeRegistry::builtin();
  for (name, core, layer, group, kind, lifecycle) in [
    (
      "eos.hardware.camera",
      "eos",
      "hardware",
      "camera",
      NodeKind::Camera,
      Lifecycle::Adapter,
    ),
    (
      "eos.hardware.base",
      "eos",
      "hardware",
      "base",
      NodeKind::Base,
      Lifecycle::Adapter,
    ),
    (
      "eos.hardware.manipulator",
      "eos",
      "hardware",
      "manipulator",
      NodeKind::Manipulator,
      Lifecycle::Adapter,
    ),
    (
      "eos.dispatcher.control",
      "eos",
      "dispatcher",
      "control",
      NodeKind::Software,
      Lifecycle::Internal,
    ),
    (
      "eos.dispatcher.primitive_base",
      "eos",
      "dispatcher",
      "base",
      NodeKind::Software,
      Lifecycle::Internal,
    ),
    (
      "eos.dispatcher.primitive_manipulator",
      "eos",
      "dispatcher",
      "manipulator",
      NodeKind::Software,
      Lifecycle::Internal,
    ),
    (
      "eos.interface.camera_gate",
      "eos",
      "interface",
      "gate",
      NodeKind::Software,
      Lifecycle::Internal,
    ),
  ] {
    let current = registry.get(&format!("{name}@v0.1")).unwrap();
    let legacy = registry.get(&format!("{name}@v1.0")).unwrap();
    assert_eq!(
      (&current.core, &current.layer, &current.group),
      (&core.to_string(), &layer.to_string(), &group.to_string())
    );
    assert_eq!((current.kind, current.lifecycle), (kind, lifecycle));
    assert_eq!(current.capabilities, legacy.capabilities);
    assert!(!current.description.trim().is_empty());
    for setting in &current.settings {
      assert!(setting
        .description
        .as_ref()
        .is_some_and(|d| !d.trim().is_empty()));
    }
    for port in current.inputs.iter().chain(&current.outputs) {
      assert!(port
        .description
        .as_ref()
        .is_some_and(|d| !d.trim().is_empty()));
      assert!(port.transport.is_some());
      assert!(port.data_class.is_some());
      assert!(port.expected_rate_hz.is_none());
    }
    assert!(serde_json::to_value(current).unwrap()["id"]
      .as_str()
      .unwrap()
      .ends_with("@v0.1"));
  }
  for legacy_only in [
    "eos.control.manual",
    "eos.control.primitive",
    "eos.control.skill",
    "eos.process.backend",
  ] {
    assert!(registry.get(&format!("{legacy_only}@v0.1")).is_none());
    assert!(registry.get(&format!("{legacy_only}@v1.0")).is_some());
  }
}

#[test]
fn new_graph_requires_new_types_and_preserves_governed_bindings() {
  let mut graph = new_eos_graph();
  Profile::from_json(&graph.to_string()).unwrap();
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["camera"]["nodes"]["front"]["node_type"] =
    "eos.hardware.camera@v1.0".into();
  assert!(Profile::from_json(&graph.to_string()).is_err());
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["camera"]["nodes"]["front"]["node_type"] =
    "eos.hardware.camera@v0.1".into();
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["camera"]["nodes"]["front"]
    ["description"] = "".into();
  assert!(Profile::from_json(&graph.to_string()).is_err());
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["camera"]["nodes"]["front"]
    ["description"] = "Front camera".into();
  graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["base"] = serde_json::json!({"nodes":{
    "velocity":{"id":"velocity","node_type":"eos.dispatcher.primitive_base@v0.1",
      "label":"Base velocity","description":"Bounded velocity binding",
      "observation":null,"movement":null,
      "settings":{"primitive_name":"base.velocity","primitive_version":"1.0.0","movement_type":"body_velocity"}}
  }});
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["base"]["nodes"]["drive"]["inputs"]
    ["command"] = serde_json::json!([{"node":"eos/dispatcher/base/velocity","output":"command"}]);
  graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["manipulator"] = serde_json::json!({"nodes":{
    "movej":{"id":"movej","node_type":"eos.dispatcher.primitive_manipulator@v0.1",
      "label":"Arm movej","description":"Joint position binding",
      "observation":null,"movement":null,
      "settings":{"primitive_name":"arm.movej","primitive_version":"1.0.0","movement_type":"joint_positions"}}
  }});
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["manipulator"]["nodes"]["arm"]["inputs"]
    ["command"] =
    serde_json::json!([{"node":"eos/dispatcher/manipulator/movej","output":"command"}]);
  graph["cores"]["eos"]["layers"]["interface"] = serde_json::json!({"groups":{"gate":{"nodes":{
    "front_preview":{"id":"front_preview","node_type":"eos.interface.camera_gate@v0.1",
      "label":"Front preview","description":"Read-only camera preview","observation":null,"movement":null,
      "inputs":{"frame":{"node":"eos/hardware/camera/front","output":"frame"}}}
  }}}});
  Profile::from_json(&graph.to_string()).unwrap();
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["manipulator"]["nodes"]["arm"]["inputs"]
    ["command"] = serde_json::json!([{"node":"eos/dispatcher/base/velocity","output":"command"}]);
  assert!(Profile::from_json(&graph.to_string()).is_err());
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["manipulator"]["nodes"]["arm"]["inputs"]
    ["command"] =
    serde_json::json!([{"node":"eos/dispatcher/manipulator/movej","output":"command"}]);
  graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["base"]["nodes"]["velocity"]
    ["settings"]["primitive_name"] = "arm.movej".into();
  assert!(Profile::from_json(&graph.to_string()).is_err());
}

#[test]
fn setting_metadata_rejects_bad_schema_and_values() {
  let mut registry = NodeTypeRegistry::builtin();
  let mut camera = registry.get("eos.hardware.camera@v0.1").unwrap().clone();
  camera.id = "eos.hardware.bounded_camera@v0.1".into();
  camera.settings.push(Setting {
    name: "exposure_ms".into(),
    data_type: SettingType::PositiveInteger,
    required: true,
    description: Some("Camera exposure duration".into()),
    unit: Some("ms".into()),
    min: Some(1),
    max: Some(10),
    choices: vec![],
    selector: None,
    items_schema: None,
    min_items: None,
    max_items: None,
  });
  registry.register(camera.clone()).unwrap();
  let mut graph = new_eos_graph();
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["camera"]["nodes"]["front"]["node_type"] =
    camera.id.clone().into();
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["camera"]["nodes"]["front"]["settings"] =
    serde_json::json!({"exposure_ms":5});
  Profile::from_json_with_registry(&graph.to_string(), &registry).unwrap();
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["camera"]["nodes"]["front"]["settings"] =
    serde_json::json!({"exposure_ms":11});
  assert!(Profile::from_json_with_registry(&graph.to_string(), &registry).is_err());
  let mut bad = camera.clone();
  bad.id = "eos.hardware.bad_camera@v0.1".into();
  bad.settings[0].min = Some(-1);
  assert!(registry.register(bad.clone()).is_err());
  bad.settings[0].min = Some(11);
  assert!(registry.register(bad.clone()).is_err());
  bad.settings[0].min = None;
  bad.settings[0].description = None;
  assert!(registry.register(bad).is_err());

  let mut choice = camera.clone();
  choice.id = "eos.hardware.choice_camera@v0.1".into();
  choice.settings[0].data_type = SettingType::String;
  choice.settings[0].min = None;
  choice.settings[0].max = None;
  choice.settings[0].choices = vec!["rgb".into(), "rgb".into()];
  assert!(registry.register(choice.clone()).is_err());
  choice.settings[0].choices.clear();
  choice.settings[0].selector = Some(
    serde_json::from_value(serde_json::json!({
      "provider":"package", "resource":"unknown", "package_from":"node.package"
    }))
    .unwrap(),
  );
  assert!(registry.register(choice.clone()).is_err());
  choice.settings[0].selector = Some(
    serde_json::from_value(serde_json::json!({
      "provider":"package", "resource":"scene", "package_from":"node.adapter.package"
    }))
    .unwrap(),
  );
  registry.register(choice).unwrap();

  let mut large = camera;
  large.id = "eos.hardware.large_camera@v0.1".into();
  large.settings[0].min = Some(9_007_199_254_740_992);
  large.settings[0].max = Some(9_007_199_254_740_992);
  registry.register(large.clone()).unwrap();
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["camera"]["nodes"]["front"]["node_type"] =
    large.id.into();
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["camera"]["nodes"]["front"]["settings"] =
    serde_json::json!({"exposure_ms":9_007_199_254_740_992_i64});
  Profile::from_json_with_registry(&graph.to_string(), &registry).unwrap();
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["camera"]["nodes"]["front"]["settings"] =
    serde_json::json!({"exposure_ms":9_007_199_254_740_993_i64});
  assert!(Profile::from_json_with_registry(&graph.to_string(), &registry).is_err());
}

#[test]
fn object_array_setting_preserves_metadata_and_checks_each_item() {
  let mut registry = NodeTypeRegistry::builtin();
  let mut process = registry.get("isos.process.component@v1.1").unwrap().clone();
  process.id = "isos.process.component@v0.1".into();
  process.description = "Checked package skill selection".into();
  for setting in &mut process.settings {
    setting.description = Some(format!("Verified {} setting", setting.name));
  }
  for port in process.inputs.iter_mut().chain(&mut process.outputs) {
    port.description = Some(format!("Live {} from managed process", port.name));
    port.transport = Some(PortTransport::InProcess);
    port.data_class = Some(DataClass::State);
  }
  process.settings.push(Setting {
    name: "skill_libraries".into(),
    data_type: SettingType::ObjectArray,
    required: false,
    description: Some("Exact package skill libraries".into()),
    unit: Some("entries".into()),
    min: None,
    max: None,
    choices: vec![],
    selector: None,
    items_schema: Some(serde_json::json!({
      "type":"object", "additionalProperties":false,
      "required":["package","library"],
      "properties":{
        "package":{"type":"object", "additionalProperties":false,
          "required":["id","version"],
          "properties":{"id":{"type":"string","minLength":1},
            "version":{"type":"string","minLength":1}}},
        "library":{"type":"string","minLength":1,"maxLength":256}
      }
    })),
    min_items: Some(0),
    max_items: Some(16),
  });
  registry.register(process.clone()).unwrap();
  let encoded = serde_json::to_value(&process).unwrap();
  assert_eq!(
    encoded["settings"].as_array().unwrap().last().unwrap()["data_type"],
    "object_array"
  );
  let decoded: NodeType = serde_json::from_value(encoded).unwrap();
  let selected = decoded.settings.last().unwrap();
  assert_eq!(selected.min_items, Some(0));
  assert_eq!(selected.max_items, Some(16));
  assert!(selected.items_schema.as_ref().unwrap().is_object());

  let legacy = Profile::from_toml(
    r#"
version = "1.0.0"
[robot]
id = "robot-1"
name = "Test"
[cores.isos.layers.process.groups.component.nodes.eos]
id = "eos"
node_type = "isos.process.component@v1.1"
label = "EOS"
description = "EOS runtime"
capabilities = ["process"]
[cores.isos.layers.process.groups.component.nodes.eos.settings]
role = "eos"
args = []
ready_unix = "/tmp/eos-v2.sock"
"#,
  )
  .unwrap();
  let mut graph = serde_json::to_value(legacy).unwrap();
  graph["version"] = "0.1.0".into();
  let node =
    &mut graph["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["eos"];
  node["node_type"] = "isos.process.component@v0.1".into();
  let selected = serde_json::json!({
    "package":{"id":"island/example-skills","version":"0.1.0"},
    "library":"lib/example.skill.so"
  });
  node["settings"]["skill_libraries"] = serde_json::json!([selected]);
  let check =
    |graph: &serde_json::Value| Profile::from_json_with_registry(&graph.to_string(), &registry);
  assert!(check(&graph).is_ok());
  let mut missing = graph.clone();
  missing["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["eos"]["settings"]
    ["skill_libraries"][0]
    .as_object_mut()
    .unwrap()
    .remove("library");
  assert!(check(&missing).is_err());
  let mut extra = graph.clone();
  extra["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["eos"]["settings"]
    ["skill_libraries"][0]["unexpected"] = true.into();
  assert!(check(&extra).is_err());
  let mut nested = graph.clone();
  nested["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["eos"]["settings"]
    ["skill_libraries"][0]["package"]["id"] = 7.into();
  assert!(check(&nested).is_err());
  let mut too_many = graph.clone();
  too_many["cores"]["isos"]["layers"]["process"]["groups"]["component"]["nodes"]["eos"]
    ["settings"]["skill_libraries"] = serde_json::Value::Array(vec![selected; 17]);
  assert!(check(&too_many).is_err());
  let mut malformed = process;
  malformed.settings.last_mut().unwrap().items_schema =
    Some(serde_json::json!({"type":"object","properties":"oops"}));
  assert!(NodeTypeRegistry::builtin().register(malformed).is_err());
}

#[test]
fn mujoco_process_is_contributed_and_new_hardware_is_generic() {
  let package = PackageRef {
    id: "island/mujoco-sim".into(),
    version: "0.1.1".into(),
  };
  let mut registry = NodeTypeRegistry::builtin();
  let mut process = registry.get("eos.process.backend@v1.0").unwrap().clone();
  process.id = "eos.process.mujoco@v0.1".into();
  process.group = "mujoco".into();
  process.description = "Runs the verified MuJoCo simulator and publishes its state".into();
  process
    .settings
    .retain(|s| !matches!(s.name.as_str(), "package" | "version"));
  for setting in &mut process.settings {
    setting.description = Some(format!("Verified {} setting", setting.name));
  }
  for port in &mut process.outputs {
    port.description = Some(format!("Live {} from simulator", port.name));
    port.transport = Some(PortTransport::InProcess);
    port.data_class = Some(DataClass::State);
  }
  registry
    .register_contribution(NodeTypeContribution {
      package: package.clone(),
      node_types: vec![process],
      safety_adapters: vec![],
      data_types: vec![],
      adapter_families: vec![],
    })
    .unwrap();
  assert_eq!(
    registry.package_for_type("eos.process.mujoco@v0.1"),
    Some(&package)
  );
  let mut hardware = registry.get("eos.hardware.camera@v0.1").unwrap().clone();
  hardware.id = "eos.hardware.mujoco_camera@v0.1".into();
  hardware.group = "mujoco".into();
  assert!(registry
    .register_contribution(NodeTypeContribution {
      package,
      node_types: vec![hardware],
      safety_adapters: vec![],
      data_types: vec![],
      adapter_families: vec![],
    })
    .is_err());
  assert!(registry.get("eos.hardware.mujoco_camera@v0.1").is_none());
}

#[test]
fn new_graph_binds_generic_mujoco_devices_to_exact_package_process() {
  let registry = mujoco_registry();
  let mut graph = new_eos_graph();
  let package = serde_json::json!({"id":"island/mujoco-sim","version":"0.1.1"});
  let backend = "eos/process/mujoco/simulator";
  graph["cores"]["eos"]["layers"]["process"] = serde_json::json!({"groups":{"mujoco":{"nodes":{
    "simulator":{"id":"simulator","node_type":"eos.process.mujoco@v0.1",
      "package":package,"label":"MuJoCo simulator","description":"Verified simulation child",
      "capabilities":["process"],"settings":{"scene":"default","socket":"/tmp/mujoco-v2.sock"}}
  }}}});
  for (group, id, device) in [
    ("camera", "front", "cam-0"),
    ("base", "drive", "base-0"),
    ("manipulator", "arm", "arm-0"),
  ] {
    let node = &mut graph["cores"]["eos"]["layers"]["hardware"]["groups"][group]["nodes"][id];
    node["adapter"] = serde_json::json!({"type":"mujoco-v2","package":package,
      "settings":{"backend":backend,"socket_path":"/tmp/mujoco-v2.sock",
        "simulator_node":device,"device_id":device}});
    node["references"] = serde_json::json!([backend]);
  }
  let profile = Profile::from_json_with_registry(&graph.to_string(), &registry).unwrap();
  assert_eq!(profile.hardware_nodes().count(), 3);
  let base = &mut graph["cores"]["eos"]["layers"]["hardware"]["groups"]["base"]["nodes"]["drive"];
  base["adapter"]["package"]["version"] = "0.1.2".into();
  assert!(
    Profile::from_json_with_registry(&graph.to_string(), &registry)
      .unwrap_err()
      .to_string()
      .contains("provenance")
  );
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["base"]["nodes"]["drive"]["adapter"]
    ["package"]["version"] = "0.1.1".into();
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["camera"]["nodes"]["front"]["node_type"] =
    "eos.hardware.base@v0.1".into();
  assert!(
    Profile::from_json_with_registry(&graph.to_string(), &registry)
      .unwrap_err()
      .to_string()
      .contains("placed outside")
  );
}

#[test]
fn custom_data_port_requires_registered_compiled_codec() {
  let mut registry = NodeTypeRegistry::builtin();
  let mut gate = registry
    .get("eos.interface.camera_gate@v1.0")
    .unwrap()
    .clone();
  gate.id = "eos.interface.custom_gate@v1.0".into();
  gate.outputs[0].data_type = PortDataType::Custom("vendor.camera.preview@v1.0".into());
  assert!(registry.register(gate.clone()).is_err());
  registry.register_data_type(DataTypeDescriptor {
        id: "vendor.camera.preview@v1.0".into(),
        version: "1.0".into(),
        purpose: "JPEG preview from vendor camera".into(),
        media_type: "image/jpeg".into(),
        codec: "custom_jpeg_v1".into(),
        schema: serde_json::json!({"type":"object","properties":{"width_px":{"type":"integer","x-unit":"px","description":"Image width"}}}),
        max_payload_bytes: 1024,
    }).unwrap();
  assert!(registry.register(gate.clone()).is_err());
  registry.register_codec("custom_jpeg_v1").unwrap();
  registry.register(gate).unwrap();
}

const DEMO: &str = include_str!("../../../etc/eos/backends/mock-cxx.toml");

#[test]
fn one_profile_owns_hardware_and_distinct_software_cores() {
  let profile = Profile::from_toml(DEMO).unwrap();
  assert_eq!(profile.version, "1.0.0");
  assert_eq!(profile.hardware_nodes().count(), 3);
  assert_eq!(profile.all_nodes().count(), 5);
  let guard = profile
    .node("cores/egia/layers/common/groups/process/nodes/guard")
    .unwrap();
  assert_eq!(guard.kind, NodeKind::Software);
  assert!(guard.adapter.adapter_type.is_empty());
  assert!(guard.frame_id.is_empty());
  assert_eq!(guard.references, ["eos/dispatcher/control/command"]);
  assert!(!profile.hardware_nodes().any(|node| node.id == "guard"));
}

#[test]
fn legacy_paths_migrate_atomically_to_short_paths() {
  let profile = Profile::from_toml(DEMO).unwrap();
  assert_eq!(
    profile
      .node("cores/eos/layers/hardware/groups/base/nodes/drive")
      .unwrap()
      .path(),
    "eos/hardware/base/drive"
  );
  let encoded = serde_json::to_string(&profile).unwrap();
  assert!(!encoded.contains("cores/eos/layers/"));
  let mixed = DEMO.replacen(
    "cores/eos/layers/hardware/groups/base/nodes/drive",
    "eos/hardware/base/drive",
    1,
  );
  assert!(Profile::from_toml(&mixed).is_err());
}

#[test]
fn legacy_vision_type_imports_to_isos_owner() {
  let mut graph = serde_json::to_value(Profile::from_toml(DEMO).unwrap()).unwrap();
  graph["cores"]["isosui"]["layers"]["system"]["groups"]["ui"] = serde_json::json!({"nodes":{"viewer":{
      "id":"viewer","node_type":"isosui.system.vision@v1.0",
      "label":"Viewer","description":"Legacy viewer"
  }}});
  let profile = Profile::from_json(&graph.to_string()).unwrap();
  let viewer = profile.node("isos/ui/panel/viewer").unwrap();
  assert_eq!(viewer.node_type, "isos.ui.vision@v1.0");
  assert!(profile.node("isosui/system/ui/viewer").is_none());
  let encoded = serde_json::to_string(&profile).unwrap();
  assert!(!encoded.contains("isosui.system.vision"));
}

#[test]
fn primitive_binding_command_array_targets_one_device_through_dispatcher() {
  let mut graph = serde_json::to_value(Profile::from_toml(DEMO).unwrap()).unwrap();
  let binding = serde_json::json!({
      "id":"velocity", "node_type":"eos.dispatcher.primitive_base@v1.0",
      "label":"Base velocity", "description":"Primitive binding", "observation":null,
      "settings":{"primitive_name":"base.velocity","primitive_version":"1.0.0","movement_type":"body_velocity"}
  });
  graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["base"] =
    serde_json::json!({"nodes":{"velocity":binding}});
  let edge = serde_json::json!([{"node":"eos/dispatcher/base/velocity","output":"command"}]);
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["base"]["nodes"]["drive"]["inputs"]
    ["command"] = edge;
  let source = serde_json::to_string(&graph).unwrap();
  let valid = Profile::from_json(&source).unwrap();
  assert_eq!(
    valid.node("eos/hardware/base/drive").unwrap().inputs["command"]
      .values()
      .len(),
    1
  );
  graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["base"]["nodes"]["velocity"]
    ["settings"]["primitive_version"] = serde_json::json!("9.0.0");
  assert!(Profile::from_json(&graph.to_string()).is_err());
  graph["cores"]["eos"]["layers"]["dispatcher"]["groups"]["base"]["nodes"]["velocity"]
    ["settings"]["primitive_version"] = serde_json::json!("1.0.0");
  graph["cores"]["eos"]["layers"]["hardware"]["groups"]["base"]["nodes"]["drive"]["inputs"]
    ["command"] = serde_json::json!([]);
  assert!(Profile::from_json(&graph.to_string()).is_err());
}

#[test]
fn rejects_bad_version_unknown_fields_and_broken_references() {
  assert!(Profile::from_toml(&DEMO.replace("version = \"1.0.0\"", "version = \"2.0.0\"")).is_err());
  assert!(Profile::from_toml(&DEMO.replace(
    "name = \"EOS v2 demo\"",
    "name = \"EOS v2 demo\"\nunknown = 1"
  ))
  .is_err());
  assert!(Profile::from_toml(&DEMO.replace(
    "cores/eos/layers/dispatcher/groups/control/nodes/command",
    "cores/eos/layers/dispatcher/groups/control/nodes/missing"
  ))
  .is_err());
  assert!(Profile::from_toml(&DEMO.replace(
    "cores/eos/layers/hardware/groups/base/nodes/drive",
    "cores/egia/layers/hardware/groups/base/nodes/drive"
  ))
  .is_err());
}

#[test]
fn rejects_egia_hardware_and_implicit_software_hardware_fields() {
  assert!(Profile::from_toml(&DEMO.replace(
    "[cores.eos.layers.hardware.groups.camera.nodes.front]",
    "[cores.egia.layers.hardware.groups.camera.nodes.front]"
  ))
  .is_err());
  assert!(Profile::from_toml(&DEMO.replace(
    "capabilities = [\"request_guard\"]",
    "capabilities = [\"request_guard\"]\nframe_id = \"base\""
  ))
  .is_err());
}

#[test]
fn test_node_type_wrong_version_or_placement_rejected() {
  assert!(Profile::from_toml(
    &DEMO.replace("eos.hardware.camera@v1.0", "eos.hardware.camera@v2.0")
  )
  .is_err());
  assert!(Profile::from_toml(&DEMO.replace(
    "node_type = \"eos.hardware.camera@v1.0\"",
    "node_type = \"eos.hardware.base@v1.0\""
  ))
  .is_err());
  assert!(
    Profile::from_toml(&DEMO.replace("node_type = \"eos.hardware.camera@v1.0\"\n", "")).is_err()
  );
}

#[test]
fn test_typed_connection_mismatch_and_unknown_port_rejected() {
  assert!(Profile::from_toml(&DEMO.replace(
        "[cores.eos.layers.hardware.groups.base.nodes.drive.inputs.command]\nnode = \"cores/eos/layers/dispatcher/groups/control/nodes/command\"\noutput = \"base_command\"\n",
        ""
    )).is_err());
  assert!(Profile::from_toml(&DEMO.replace(
    "output = \"base_command\"",
    "output = \"manipulator_command\""
  ))
  .is_err());
  assert!(
    Profile::from_toml(&DEMO.replace("output = \"base_command\"", "output = \"missing\"")).is_err()
  );
  assert!(Profile::from_toml(&DEMO.replace(
    "[cores.eos.layers.hardware.groups.base.nodes.drive.inputs.command]",
    "[cores.eos.layers.hardware.groups.base.nodes.drive.inputs.other]"
  ))
  .is_err());
}

#[test]
fn test_json_roundtrip_preserves_authored_graph() {
  let profile = Profile::from_toml(DEMO).unwrap();
  let json = serde_json::to_string(&profile).unwrap();
  let parsed = Profile::from_json(&json).unwrap();
  assert_eq!(parsed.all_nodes().count(), 5);
  assert_eq!(parsed.hardware_nodes().count(), 3);
  let registry = NodeTypeRegistry::builtin();
  let catalog = serde_json::to_value(registry.all().collect::<Vec<_>>()).unwrap();
  assert!(catalog
    .as_array()
    .unwrap()
    .iter()
    .any(|entry| entry["id"] == "isos.process.component@v1.0"));
}

#[test]
fn test_process_settings_dependencies_and_cycles() {
  let first = "cores/isos/layers/process/groups/component/nodes/eos";
  let second = "cores/isos/layers/process/groups/component/nodes/egia";
  let process_graph = format!(
    r#"
version = "1.0.0"
[robot]
id = "robot-1"
name = "Test"
[cores.isos.layers.process.groups.component.nodes.eos]
id = "eos"
node_type = "isos.process.component@v1.0"
label = "EOS"
description = "EOS runtime"
capabilities = ["process"]
[cores.isos.layers.process.groups.component.nodes.eos.settings]
command = ["/bin/aether-runtime"]
restart = "on_failure"
depends_on = []
ready_unix = "/tmp/eos-v2.sock"
[cores.isos.layers.process.groups.component.nodes.egia]
id = "egia"
node_type = "isos.process.component@v1.0"
label = "EGIA"
description = "EGIA runtime"
capabilities = ["process"]
[cores.isos.layers.process.groups.component.nodes.egia.settings]
command = ["/bin/egia"]
depends_on = ["{first}"]
"#
  );
  assert_eq!(
    Profile::from_toml(&process_graph)
      .unwrap()
      .all_nodes()
      .count(),
    2
  );
  assert!(Profile::from_toml(
    &process_graph.replace("restart = \"on_failure\"", "restart = \"sometimes\"")
  )
  .is_err());
  assert!(
    Profile::from_toml(&process_graph.replace("command = [\"/bin/egia\"]", "command = []"))
      .is_err()
  );
  assert!(Profile::from_toml(&process_graph.replace(
    "ready_unix = \"/tmp/eos-v2.sock\"",
    "ready_unix = \"relative.sock\""
  ))
  .is_err());
  assert!(Profile::from_toml(&process_graph.replace(
    "ready_unix = \"/tmp/eos-v2.sock\"",
    "ready_unix = \"/tmp/eos-v2.sock\"\nready_tcp = \"127.0.0.1:1234\""
  ))
  .is_err());
  assert!(Profile::from_toml(&process_graph.replace(
    &format!("depends_on = [\"{first}\"]"),
    &format!("depends_on = [\"{first}\", \"{first}\"]")
  ))
  .is_err());
  let cyclic = process_graph.replace(
    "restart = \"on_failure\"",
    &format!("restart = \"on_failure\"\ndepends_on = [\"{second}\"]"),
  );
  assert!(Profile::from_toml(&cyclic).is_err());
}

#[test]
fn process_v1_1_uses_core_role_and_arguments() {
  let graph = r#"
version = "1.0.0"
[robot]
id = "robot-1"
name = "Test"
[cores.isos.layers.process.groups.component.nodes.eos]
id = "eos"
node_type = "isos.process.component@v1.1"
label = "EOS"
description = "EOS runtime"
capabilities = ["process"]
[cores.isos.layers.process.groups.component.nodes.eos.settings]
role = "eos"
args = []
ready_unix = "/tmp/eos-v2.sock"
"#;
  assert!(Profile::from_toml(graph).is_ok());
  assert!(Profile::from_toml(&graph.replace("role = \"eos\"", "role = \"unknown\"")).is_err());
  assert!(Profile::from_toml(&graph.replace("args = []", "command = [\"/bin/eos\"]")).is_err());
}

#[test]
fn test_custom_actuator_requires_registered_safety_adapter() {
  let mut registry = NodeTypeRegistry::builtin();
  let custom = NodeType {
    id: "eos.hardware.custom-base@v1.0".into(),
    description: "Custom base adapter".into(),
    core: "eos".into(),
    layer: "hardware".into(),
    group: "base".into(),
    kind: NodeKind::Base,
    lifecycle: Lifecycle::Adapter,
    capabilities: vec!["body_velocity".into()],
    settings: vec![],
    inputs: vec![],
    outputs: vec![],
    bridge: None,
    safety_adapter_type: None,
  };
  assert!(registry.register(custom.clone()).is_err());
  let mut with_safety = custom;
  with_safety.safety_adapter_type = Some("registered-stop-v1".into());
  assert!(registry.register(with_safety.clone()).is_err());
  registry
    .register_safety_adapter(SafetyAdapterType {
      id: "registered-stop-v1".into(),
      kind: NodeKind::Base,
      movement_modes: vec!["body_velocity".into()],
      supports_stop: true,
    })
    .unwrap();
  assert!(registry.register(with_safety).is_ok());
}

#[test]
fn node_type_description_is_required_for_new_types_and_defaulted_for_legacy_sidecars() {
  let mut registry = NodeTypeRegistry::builtin();
  assert!(registry
    .get("eos.hardware.camera@v1.0")
    .unwrap()
    .description
    .contains("Captures camera frames"));
  assert!(registry
    .get("isos.process.component@v1.1")
    .unwrap()
    .description
    .contains("role-selected"));
  assert!(registry
    .all()
    .all(|node_type| !node_type.description.trim().is_empty()));
  let mut legacy = registry.get("egia.common.source@v1.0").unwrap().clone();
  legacy.id = "egia.common.legacy-source@v1.0".into();
  let mut old_sidecar = serde_json::to_value(legacy).unwrap();
  old_sidecar.as_object_mut().unwrap().remove("description");
  let legacy: NodeType = serde_json::from_value(old_sidecar).unwrap();
  registry.register(legacy).unwrap();
  assert!(registry
    .get("egia.common.legacy-source@v1.0")
    .unwrap()
    .description
    .contains("Legacy"));

  let mut current = registry.get("egia.common.source@v1.0").unwrap().clone();
  current.id = "egia.common.current-source@v0.1".into();
  current.description.clear();
  assert!(registry.register(current.clone()).is_err());
  current.description = "Publishes agent evidence".into();
  current.outputs[0].description = Some("Agent source observation".into());
  current.outputs[0].transport = Some(PortTransport::InProcess);
  current.outputs[0].data_class = Some(DataClass::Observation);
  registry.register(current).unwrap();
  assert_eq!(
    registry
      .get("egia.common.current-source@v0.1")
      .unwrap()
      .description,
    "Publishes agent evidence"
  );
}

#[test]
fn package_backend_binds_every_mujoco_adapter() {
  let graph = include_str!("../../../examples/v2/mujoco-isos.toml");
  let profile = Profile::from_toml(graph).unwrap();
  let backend = profile
    .node("cores/eos/layers/process/groups/backend/nodes/simulator")
    .unwrap();
  assert_eq!(backend.node_type, "eos.process.backend@v1.0");
  assert_eq!(profile.hardware_nodes().count(), 3);
  assert!(Profile::from_toml(&graph.replace(
    "socket = \"/tmp/mujoco-v2.sock\"",
    "socket = \"/tmp/other.sock\""
  ))
  .is_err());
  assert!(
    Profile::from_toml(&graph.replace("device_id = \"base-0\"", "device_id = \"arm-0\"")).is_err()
  );
  assert!(Profile::from_toml(
    &graph.replace("version = \"0.1.1\"\nscene", "version = \"1.0.0\"\nscene")
  )
  .is_err());
  assert!(Profile::from_toml(&graph.replace(
    "backend = \"cores/eos/layers/process/groups/backend/nodes/simulator\"",
    "backend = \"cores/isos/layers/process/groups/backend/nodes/missing\""
  ))
  .is_err());
}

#[test]
fn egia_common_ports_are_specific_and_tensor_is_reserved() {
  let registry = NodeTypeRegistry::builtin();
  let ids: Vec<_> = registry.all().map(|entry| entry.id.as_str()).collect();
  for id in [
    "egia.common.source@v1.0",
    "egia.common.context@v1.0",
    "egia.common.planner@v1.0",
    "egia.common.guard@v1.0",
  ] {
    assert!(ids.contains(&id));
  }
  assert!(!ids.iter().any(|id| id.starts_with("egia.tensor.")));
  assert!(registry.get("egia.reasoning.planner@v1.0").is_none());
}

#[test]
fn control_nodes_require_exact_graph_targets_and_bounded_skill_steps() {
  let graph = include_str!("../../../examples/v2/mujoco-isos.toml");
  assert!(Profile::from_toml(graph).is_ok());
  assert!(Profile::from_toml(&graph.replace(
    "movement_type = \"body_velocity\"",
    "movement_type = \"joint_positions\""
  ))
  .is_err());
  let dispatcher = "cores/eos/layers/dispatcher/groups/control/nodes/command";
  let target = "cores/eos/layers/hardware/groups/base/nodes/drive";
  assert!(Profile::from_toml(&graph.replace(
    &format!("references = [\"{dispatcher}\", \"{target}\"]"),
    &format!("references = [\"{dispatcher}\"]")
  ))
  .is_err());
  assert!(Profile::from_toml(&graph.replace(
    "[cores.eos.layers.control.groups.manual.nodes.manual_0.inputs.base_observation]",
    "[cores.eos.layers.control.groups.manual.nodes.manual_0.inputs.manipulator_observation]"
  ))
  .is_err());
  let primitive = "cores/eos/layers/control/groups/primitive/nodes/base_velocity_0";
  let arm = "cores/eos/layers/control/groups/primitive/nodes/arm_joint_0";
  let too_many = (0..17)
    .map(|_| format!("\"{primitive}\""))
    .collect::<Vec<_>>()
    .join(", ");
  assert!(Profile::from_toml(&graph.replace(
    &format!("steps = [\"{primitive}\", \"{arm}\"]"),
    &format!("steps = [{too_many}]")
  ))
  .is_err());
}

#[test]
fn package_contribution_binds_types_to_exact_package_and_placement() {
  let mut registry = NodeTypeRegistry::builtin();
  let mut camera = registry.get("eos.hardware.camera@v1.0").unwrap().clone();
  camera.id = "eos.hardware.mujoco_camera@v1.0".into();
  camera.group = "mujoco".into();
  let package = PackageRef {
    id: "island/mujoco-sim".into(),
    version: "0.1.1".into(),
  };
  registry
    .register_contribution(NodeTypeContribution {
      package: package.clone(),
      node_types: vec![camera],
      safety_adapters: vec![],
      data_types: vec![],
      adapter_families: vec![],
    })
    .unwrap();
  let mut graph: serde_json::Value =
    serde_json::from_str(&serde_json::to_string(&Profile::from_toml(DEMO).unwrap()).unwrap())
      .unwrap();
  let hardware = graph["cores"]["eos"]["layers"]["hardware"]["groups"]
    .as_object_mut()
    .unwrap();
  let mut camera_group = hardware.remove("camera").unwrap();
  let front = &mut camera_group["nodes"]["front"];
  front["node_type"] = "eos.hardware.mujoco_camera@v1.0".into();
  front["package"] = serde_json::to_value(&package).unwrap();
  hardware.insert("mujoco".into(), camera_group);
  let text = serde_json::to_string(&graph)
    .unwrap()
    .replace("groups/camera/nodes/front", "groups/mujoco/nodes/front");
  let profile = Profile::from_json_with_registry(&text, &registry).unwrap();
  assert_eq!(
    profile
      .node("cores/eos/layers/hardware/groups/mujoco/nodes/front")
      .unwrap()
      .kind,
    NodeKind::Camera
  );
  assert!(Profile::from_json(&text).is_err());
  assert!(Profile::from_json_with_registry(
    &text.replace("island/mujoco-sim", "other/mujoco-sim"),
    &registry
  )
  .is_err());
}

#[test]
fn contribution_rejects_unknown_fields_and_keeps_registry_atomic() {
  let mut registry = NodeTypeRegistry::builtin();
  let mut process = registry.get("eos.process.backend@v1.0").unwrap().clone();
  process.id = "eos.process.mujoco@v1.0".into();
  process.group = "mujoco".into();
  let mut duplicate = process.clone();
  duplicate.id = process.id.clone();
  let contribution = NodeTypeContribution {
    package: PackageRef {
      id: "island/mujoco-sim".into(),
      version: "0.1.1".into(),
    },
    node_types: vec![process, duplicate],
    safety_adapters: vec![],
    data_types: vec![],
    adapter_families: vec![],
  };
  assert!(registry.register_contribution(contribution).is_err());
  assert!(registry.get("eos.process.mujoco@v1.0").is_none());
  let bad =
    r#"{"package":{"id":"island/mujoco-sim","version":"0.1.1"},"node_types":[],"unexpected":true}"#;
  assert!(serde_json::from_str::<NodeTypeContribution>(bad).is_err());
}

#[test]
fn adapter_family_catalog_compilation_is_derived_from_exact_package() {
  let contribution = serde_json::json!({
      "package":{"id":"island/mujoco-sim","version":"0.1.1"},
      "adapter_families":[{"id":"mujoco-v2","device_kinds":["camera","base","manipulator"],
          "transport":"unix_socket","description":"MuJoCo socket device",
          "supported_primitives":["base.velocity@1.0.0","arm.movej@1.0.0"]}]
  });
  let mut registry = NodeTypeRegistry::builtin();
  registry
    .register_contribution(serde_json::from_value(contribution.clone()).unwrap())
    .unwrap();
  let family = registry
    .catalog_adapter_families()
    .into_iter()
    .find(|family| family.descriptor.id == "mujoco-v2")
    .unwrap();
  assert!(family.compiled);
  assert_eq!(family.package.unwrap().version, "0.1.1");
  let unpinned_generic = DEMO.replace("type = \"mock-cxx\"", "type = \"mujoco-v2\"");
  assert!(Profile::from_toml_with_registry(&unpinned_generic, &registry).is_err());
  let mut forged = contribution.clone();
  forged["adapter_families"][0]["compiled"] = serde_json::json!(true);
  assert!(serde_json::from_value::<NodeTypeContribution>(forged).is_err());
  let mut software_family = contribution.clone();
  software_family["adapter_families"][0]["device_kinds"] = serde_json::json!(["software"]);
  assert!(NodeTypeRegistry::builtin()
    .register_contribution(serde_json::from_value(software_family).unwrap())
    .is_err());
  let mut wrong_package = contribution;
  wrong_package["package"]["version"] = serde_json::json!("2.0.1");
  let mut registry = NodeTypeRegistry::builtin();
  registry
    .register_contribution(serde_json::from_value(wrong_package).unwrap())
    .unwrap();
  let family = registry
    .catalog_adapter_families()
    .into_iter()
    .find(|family| family.descriptor.id == "mujoco-v2")
    .unwrap();
  assert!(!family.compiled);
}
