//! Exact selected-reader admission for the authored graph and projected children.
use crate::{
  app::config::Paths, runtime::process::core_bins, runtime::owners::core_setup,
  domain::graph::graph_compat::Diagnostic, runtime::process::ProcessSpec,
};
use serde::Deserialize;
use serde::de::{self, MapAccess, SeqAccess, Visitor};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
  collections::{BTreeMap, BTreeSet},
  fs,
  io::Read,
  os::unix::process::CommandExt,
  path::{Path, PathBuf},
  process::{Command, Stdio},
  sync::mpsc,
  thread,
  time::{Duration, Instant},
};

const LIMIT: u64 = 64 * 1024;
const TIMEOUT: Duration = Duration::from_secs(5);
struct Unique(Value);

impl<'de> Deserialize<'de> for Unique {
  fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
    struct UniqueVisitor;
    impl<'de> Visitor<'de> for UniqueVisitor {
      type Value = Unique;
      fn expecting(&self, formatter: &mut std::fmt::Formatter) -> std::fmt::Result {
        formatter.write_str("JSON without duplicate object fields")
      }
      fn visit_bool<E: de::Error>(self, value: bool) -> Result<Unique, E> {
        Ok(Unique(json!(value)))
      }
      fn visit_i64<E: de::Error>(self, value: i64) -> Result<Unique, E> {
        Ok(Unique(json!(value)))
      }
      fn visit_u64<E: de::Error>(self, value: u64) -> Result<Unique, E> {
        Ok(Unique(json!(value)))
      }
      fn visit_f64<E: de::Error>(self, value: f64) -> Result<Unique, E> {
        serde_json::Number::from_f64(value)
          .map(|number| Unique(Value::Number(number)))
          .ok_or_else(|| E::custom("invalid JSON number"))
      }
      fn visit_str<E: de::Error>(self, value: &str) -> Result<Unique, E> {
        Ok(Unique(json!(value)))
      }
      fn visit_string<E: de::Error>(self, value: String) -> Result<Unique, E> {
        Ok(Unique(Value::String(value)))
      }
      fn visit_none<E: de::Error>(self) -> Result<Unique, E> {
        Ok(Unique(Value::Null))
      }
      fn visit_unit<E: de::Error>(self) -> Result<Unique, E> {
        Ok(Unique(Value::Null))
      }
      fn visit_seq<A: SeqAccess<'de>>(self, mut seq: A) -> Result<Unique, A::Error> {
        let mut items = Vec::new();
        while let Some(item) = seq.next_element::<Unique>()? {
          items.push(item.0);
        }
        Ok(Unique(Value::Array(items)))
      }
      fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<Unique, A::Error> {
        let mut items = serde_json::Map::new();
        while let Some((key, value)) = map.next_entry::<String, Unique>()? {
          if items.insert(key.clone(), value.0).is_some() {
            return Err(de::Error::custom(format!("duplicate JSON field {key}")));
          }
        }
        Ok(Unique(Value::Object(items)))
      }
    }
    deserializer.deserialize_any(UniqueVisitor)
  }
}

pub fn parse_unique_json(bytes: &[u8]) -> serde_json::Result<Value> {
  serde_json::from_slice::<Unique>(bytes).map(|value| value.0)
}

#[derive(Clone, Debug)]
struct Claims {
  core: &'static str,
  binary: &'static str,
  hash: Option<String>,
  graph_versions: BTreeSet<String>,
  type_versions: BTreeSet<String>,
  node_types: BTreeSet<String>,
  features: BTreeSet<String>,
  scalar_fallback: bool,
}

impl Claims {
  fn empty(core: &'static str, binary: &'static str, path: &Path) -> Self {
    Self {
      core,
      binary,
      hash: fs::read(path)
        .ok()
        .map(|bytes| hex::encode(Sha256::digest(bytes))),
      graph_versions: BTreeSet::new(),
      type_versions: BTreeSet::new(),
      node_types: BTreeSet::new(),
      features: BTreeSet::new(),
      scalar_fallback: false,
    }
  }

  // The four optional requested fields mirror the published diagnostic wire shape.
  #[allow(clippy::too_many_arguments)]
  fn diagnostic(
    &self,
    code: &'static str,
    path: impl Into<String>,
    stage: &'static str,
    graph: Option<&str>,
    node_type: Option<&str>,
    feature: Option<&str>,
    package: Option<&Value>,
  ) -> Diagnostic {
    let mut context = BTreeMap::new();
    context.insert("stage".into(), json!(stage));
    context.insert(
            "reader".into(),
            json!({
                "core":self.core,
                "binary":self.binary,
                "binary_sha256":self.hash,
                "requested":{"graph_version":graph,"node_type":node_type,"feature":feature,"package":package},
                "supported":{
                    "graph_versions":self.graph_versions,
                    "node_type_versions":self.type_versions,
                    "node_types":self.node_types,
                    "features":self.features
                }
            }),
        );
    let repair = match code {
      "reader_binary_mismatch" | "reader_identity_invalid" => {
        "restore the selected build and Apply again"
      }
      "reader_package_pin_mismatch" => "restore the exact verified package pin and Apply again",
      _ => "select a verified compatible build and Apply again",
    };
    let subject = feature.or(node_type).or(graph).unwrap_or("identity");
    Diagnostic {
      code,
      path: path.into(),
      message: format!("{} lacks or rejects {subject}; {repair}", self.binary),
      context,
    }
  }
}

fn member_set(value: &Value, key: &str, nonempty: bool) -> Option<BTreeSet<String>> {
  let array = value.get(key)?.as_array()?;
  if nonempty && array.is_empty() {
    return None;
  }
  let mut values = BTreeSet::new();
  for item in array {
    let item = item.as_str()?;
    if item.is_empty() || item.len() > 256 || !item.is_ascii() || !values.insert(item.into()) {
      return None;
    }
  }
  Some(values)
}

fn parse_claims(
  mut claims: Claims,
  reply: &Value,
  graph_reader: bool,
  setup: bool,
) -> Result<Claims, Diagnostic> {
  let identity_path = if setup { "" } else { "/version" };
  let identity_matches = if setup {
    reply["core"] == claims.core
  } else {
    reply["binary"] == claims.binary
  };
  if !identity_matches {
    return Err(claims.diagnostic(
      "reader_binary_mismatch",
      identity_path,
      "reader",
      None,
      None,
      None,
      None,
    ));
  }
  if !reply["product_version"]
    .as_str()
    .is_some_and(|v| !v.is_empty())
    || setup && reply["setup_schema_version"] != 2
  {
    return Err(claims.diagnostic(
      "reader_identity_invalid",
      identity_path,
      "reader",
      None,
      None,
      None,
      None,
    ));
  }
  claims.features = if reply.get("features").is_none() {
    BTreeSet::new()
  } else {
    member_set(reply, "features", false).ok_or_else(|| {
      claims.diagnostic(
        "reader_identity_invalid",
        identity_path,
        "reader",
        None,
        None,
        None,
        None,
      )
    })?
  };
  if !graph_reader {
    if claims.core == "egia"
      && [
        "supported_graph_versions",
        "supported_node_type_versions",
        "supported_node_types",
      ]
      .iter()
      .any(|key| reply.get(*key).is_some())
    {
      return Err(claims.diagnostic(
        "reader_identity_invalid",
        identity_path,
        "reader",
        None,
        None,
        None,
        None,
      ));
    }
    return Ok(claims);
  }
  let keys = [
    "supported_graph_versions",
    "supported_node_type_versions",
    "supported_node_types",
  ];
  let present = keys.iter().filter(|key| reply.get(**key).is_some()).count();
  if present == 0 {
    if reply["graph_version"] != "1.0.0" || reply["node_type_version"] != "1.0" {
      return Err(claims.diagnostic(
        "reader_identity_invalid",
        identity_path,
        "reader",
        None,
        None,
        None,
        None,
      ));
    }
    claims.graph_versions.insert("1.0.0".into());
    claims.type_versions.insert("1.0".into());
    claims.scalar_fallback = true;
    return Ok(claims);
  }
  if present != keys.len() {
    return Err(claims.diagnostic(
      "reader_identity_invalid",
      identity_path,
      "reader",
      None,
      None,
      None,
      None,
    ));
  }
  claims.graph_versions = member_set(reply, keys[0], true).ok_or_else(|| {
    claims.diagnostic(
      "reader_identity_invalid",
      identity_path,
      "reader",
      None,
      None,
      None,
      None,
    )
  })?;
  claims.type_versions = member_set(reply, keys[1], true).ok_or_else(|| {
    claims.diagnostic(
      "reader_identity_invalid",
      identity_path,
      "reader",
      None,
      None,
      None,
      None,
    )
  })?;
  claims.node_types = member_set(reply, keys[2], false).ok_or_else(|| {
    claims.diagnostic(
      "reader_identity_invalid",
      identity_path,
      "reader",
      None,
      None,
      None,
      None,
    )
  })?;
  if claims.node_types.iter().any(|id| {
    id.rsplit_once("@v")
      .is_none_or(|(_, suffix)| !claims.type_versions.contains(suffix))
  }) {
    return Err(claims.diagnostic(
      "reader_identity_invalid",
      identity_path,
      "reader",
      None,
      None,
      None,
      None,
    ));
  }
  Ok(claims)
}

fn probe(
  path: &Path,
  binary: &'static str,
  core: &'static str,
  graph_reader: bool,
  environment: Option<&BTreeMap<String, String>>,
) -> Result<Claims, Diagnostic> {
  let claims = Claims::empty(core, binary, path);
  if claims.hash.is_none() {
    return Err(claims.diagnostic(
      "reader_binary_mismatch",
      "",
      "reader",
      None,
      None,
      None,
      None,
    ));
  }
  let mut command = Command::new(path);
  command
    .arg("--capabilities")
    .stdin(Stdio::null())
    .stdout(Stdio::piped())
    .stderr(Stdio::null());
  if let Some(environment) = environment {
    command.envs(environment);
  }
  command.env_remove(crate::runtime::process::skill_selection::ENV);
  // Keep a helper's descendants inside the same deadline as the selected probe.
  unsafe {
    command.pre_exec(|| {
      if libc::setsid() < 0 {
        return Err(std::io::Error::last_os_error());
      }
      Ok(())
    });
  }
  let mut child = command.spawn().map_err(|_| {
    claims.diagnostic(
      "reader_binary_mismatch",
      "",
      "reader",
      None,
      None,
      None,
      None,
    )
  })?;
  let group = child.id() as i32;
  let stdout = child.stdout.take().expect("piped stdout");
  let (sender, receiver) = mpsc::channel();
  thread::spawn(move || {
    let mut bytes = Vec::new();
    let result = stdout
      .take(LIMIT + 1)
      .read_to_end(&mut bytes)
      .map(|_| bytes);
    let _ = sender.send(result);
  });
  let started = Instant::now();
  let mut status = None;
  let mut output = None;
  loop {
    if status.is_none() {
      status = child.try_wait().ok().flatten();
    }
    if output.is_none() {
      output = receiver.try_recv().ok();
    }
    if status.is_some() && output.is_some() {
      break;
    }
    if started.elapsed() >= TIMEOUT {
      unsafe { libc::kill(-group, libc::SIGKILL) };
      let _ = child.kill();
      let _ = child.wait();
      return Err(claims.diagnostic(
        "reader_identity_invalid",
        "",
        "reader",
        None,
        None,
        None,
        None,
      ));
    }
    thread::sleep(Duration::from_millis(10));
  }
  let output = output.and_then(Result::ok);
  if !status.is_some_and(|s| s.success()) || output.as_ref().is_none_or(|v| v.len() as u64 > LIMIT)
  {
    return Err(claims.diagnostic(
      "reader_identity_invalid",
      "",
      "reader",
      None,
      None,
      None,
      None,
    ));
  }
  let value: Value = parse_unique_json(output.as_ref().unwrap()).map_err(|_| {
    claims.diagnostic(
      "reader_identity_invalid",
      "",
      "reader",
      None,
      None,
      None,
      None,
    )
  })?;
  parse_claims(claims, &value, graph_reader, false)
}

fn setup(paths: &Paths, core: &'static str, graph: &Value) -> Result<Claims, Diagnostic> {
  let binary = if core == "eos" {
    "aether-profile"
  } else {
    "egia-config"
  };
  let path = if core == "eos" {
    core_bins::resolve(binary)
  } else {
    core_setup::adapter_path(paths, core)
  };
  let path = path.unwrap_or_else(|_| PathBuf::new());
  let mut claims = Claims::empty(core, binary, &path);
  if core == "egia" && crate::domain::graph::generated_v2::egia_launch_env(graph).is_err() {
    claims.hash = None;
    let pointer = "/cores/isos/layers/process/groups/component/nodes/egia";
    let role_path = if graph.pointer(&format!("{pointer}/settings/role")).is_some() {
      format!("{pointer}/settings/role")
    } else {
      format!("{pointer}/node_type")
    };
    return Err(claims.diagnostic(
      "reader_binary_mismatch",
      role_path,
      "reader",
      None,
      None,
      None,
      None,
    ));
  }
  if core == "egia" {
    claims.hash = crate::domain::graph::generated_v2::egia_chain_digest(graph).ok();
  }
  let advertised = if core == "eos" {
    Some(probe(&path, "aether-profile", "eos", true, None)?)
  } else {
    None
  };
  let reply = core_setup::identity_for_graph(paths, core, graph).map_err(|_| {
    claims.diagnostic(
      "reader_identity_invalid",
      "",
      "reader",
      None,
      None,
      None,
      None,
    )
  })?;
  let identity = parse_claims(claims, &reply, true, true)?;
  if let Some(advertised) = advertised
    && (identity.graph_versions != advertised.graph_versions
      || identity.type_versions != advertised.type_versions
      || identity.node_types != advertised.node_types
      || identity.features != advertised.features)
  {
    return Err(identity.diagnostic(
      "reader_identity_invalid",
      "",
      "reader",
      None,
      None,
      None,
      None,
    ));
  }
  Ok(identity)
}

/// Whether the selected EGIA setup identity attests the exact A04 route table.
pub fn egia_evidence_route_feature(paths: &Paths, graph: &Value) -> Result<bool, Diagnostic> {
  Ok(
    setup(paths, "egia", graph)?
      .features
      .contains("egia-evidence-routing-v1"),
  )
}

fn nodes(graph: &Value, core: &str) -> Vec<(String, String, Option<Value>)> {
  let mut found = Vec::new();
  let unified = graph["version"] == "0.2.0";
  let graph_core = if unified { "isos" } else { core };
  let Some(layers) = graph
    .pointer(&format!("/cores/{graph_core}/layers"))
    .and_then(Value::as_object)
  else {
    return found;
  };
  for (layer_id, layer) in layers {
    if unified && !layer_id.starts_with(if core == "eos" { "eos_" } else { "ei_" }) {
      continue;
    }
    let Some(groups) = layer["groups"].as_object() else {
      continue;
    };
    for (group_id, group) in groups {
      let Some(entries) = group["nodes"].as_object() else {
        continue;
      };
      for (key, node) in entries {
        if node["enabled"] == false {
          continue;
        }
        if let Some(id) = node["node_type"].as_str() {
          found.push((
            format!("/cores/{graph_core}/layers/{layer_id}/groups/{group_id}/nodes/{key}"),
            id.into(),
            node.get("package").cloned(),
          ));
        }
      }
    }
  }
  found.sort_by(|a, b| a.0.cmp(&b.0));
  found
}

fn check_reader(
  claims: &Claims,
  graph: &Value,
  core: &str,
  catalog: &Value,
) -> Result<(), Diagnostic> {
  let version = graph["version"].as_str().unwrap_or_default();
  if !claims.graph_versions.contains(version) {
    return Err(claims.diagnostic(
      "reader_graph_unsupported",
      "/version",
      "graph",
      Some(version),
      None,
      None,
      None,
    ));
  }
  let builtins = &claims.node_types;
  let registered: BTreeMap<&str, &Value> = catalog["types"]
    .as_array()
    .into_iter()
    .flatten()
    .filter_map(|item| item["id"].as_str().map(|id| (id, item)))
    .collect();
  for (path, id, pin) in nodes(graph, core) {
    let Some((_, suffix)) = id.rsplit_once("@v") else {
      continue;
    };
    let contributed = registered
      .get(id.as_str())
      .and_then(|item| item.get("package"));
    if contributed.is_some() && pin.as_ref() != contributed {
      return Err(claims.diagnostic(
        "reader_package_pin_mismatch",
        format!("{path}/package"),
        "package",
        Some(version),
        Some(&id),
        None,
        pin.as_ref(),
      ));
    }
    if !claims.type_versions.contains(suffix)
      || (!builtins.contains(&id) && !claims.scalar_fallback && contributed.is_none())
      || (claims.scalar_fallback
        && !(version == "1.0.0" && id.ends_with("@v1.0") && registered.contains_key(id.as_str())))
    {
      return Err(claims.diagnostic(
        "reader_type_unsupported",
        format!("{path}/node_type"),
        "reader",
        Some(version),
        Some(&id),
        None,
        pin.as_ref(),
      ));
    }
  }
  Ok(())
}

fn feature(
  claims: &Claims,
  graph: &Value,
  path: &str,
  id: Option<&str>,
  token: &str,
) -> Result<(), Diagnostic> {
  if claims.features.contains(token) {
    return Ok(());
  }
  Err(claims.diagnostic(
    "reader_feature_unsupported",
    path,
    "feature",
    graph["version"].as_str(),
    id,
    Some(token),
    None,
  ))
}

fn check_inner(
  paths: &Paths,
  graph: &Value,
  specs: &[ProcessSpec],
  catalog: &Value,
  only_core: Option<&str>,
) -> Result<(), Diagnostic> {
  let version = graph["version"].as_str().unwrap_or_default();
  let vision_selected = crate::runtime::process::vision_binding::selected_gate_for_graph(graph)
    .map_err(|error| Diagnostic {
      code: error.code,
      path: error.path,
      message: error.message.into(),
      context: BTreeMap::new(),
    })?
    .is_some();
  if vision_selected && !crate::domain::graph::graph_compat::uses_revisioned_generation(graph) {
    return Err(Diagnostic {
      code: "vision_graph_stale",
      path: "/version".into(),
      message: "Vision evidence requires an admitted revisioned graph".into(),
      context: BTreeMap::new(),
    });
  }
  let egia_route_path = if crate::domain::graph::graph_compat::uses_revisioned_generation(graph) {
    crate::domain::graph::edge_admission::admit(graph, catalog)
      .ok()
      .and_then(|edges| {
        edges
          .into_iter()
          .find(|edge| {
            edge.route_class
              == (if version == "0.2.0" {
                "ei_local"
              } else {
                "egia_local"
              })
              && edge.routed
          })
          .map(|edge| edge.path)
      })
  } else {
    None
  };
  if only_core != Some("egia") {
    let profile = setup(paths, "eos", graph)?;
    check_reader(&profile, graph, "eos", catalog)?;
    if version == "0.1.0" {
      for token in [
        "graph-0.1-reader",
        "node-types",
        "data-idl",
        "setup-v2",
        "catalog",
      ] {
        feature(&profile, graph, "/version", None, token)?;
      }
    }
  }
  let egia_enabled = specs.iter().any(|spec| spec.id == "egia");
  if let Some(path) = &egia_route_path
    && !egia_enabled
  {
    return Err(
      Claims::empty("egia", "egia", Path::new("/missing")).diagnostic(
        "reader_feature_unsupported",
        path,
        "feature",
        Some(version),
        None,
        Some("egia-evidence-scheduler-v1"),
        None,
      ),
    );
  }
  if only_core != Some("eos") && egia_enabled {
    let egia = setup(paths, "egia", graph)?;
    check_reader(&egia, graph, "egia", catalog)?;
    if version == "0.1.0" {
      for token in ["graph-0.1-reader", "setup-v2", "catalog"] {
        feature(&egia, graph, "/version", None, token)?;
      }
      if let Some(path) = &egia_route_path {
        feature(&egia, graph, path, None, "egia-evidence-routing-v1")?;
      }
    }
  }
  for (id, binary, graph_reader) in [
    ("eos", "aether-runtime", true),
    ("config", "aether-config-server", true),
    ("bridge", "aether-http-bridge", false),
    ("egia", "egia", false),
  ] {
    if only_core.is_some_and(|owner| owner != if id == "egia" { "egia" } else { "eos" }) {
      continue;
    }
    let Some(spec) = specs.iter().find(|spec| spec.id == id) else {
      continue;
    };
    let path = Path::new(&spec.settings.command[0]);
    let mut claims = Claims::empty(if id == "egia" { "egia" } else { "eos" }, binary, path);
    if id == "egia" {
      claims.hash = crate::domain::graph::generated_v2::egia_chain_digest(graph).ok();
    }
    let pointer = format!("/cores/isos/layers/process/groups/component/nodes/{id}");
    let role_path = if graph.pointer(&format!("{pointer}/settings/role")).is_some() {
      format!("{pointer}/settings/role")
    } else {
      format!("{pointer}/node_type")
    };
    if claims.hash.is_none() {
      return Err(claims.diagnostic(
        "reader_binary_mismatch",
        role_path,
        "reader",
        None,
        None,
        None,
        None,
      ));
    }
    claims = probe(
      path,
      binary,
      claims.core,
      graph_reader,
      (id == "egia").then_some(&spec.settings.env),
    )
    .map_err(|mut error| {
      error.path = role_path.clone();
      if id == "egia" {
        error.context.get_mut("reader").unwrap()["binary_sha256"] = json!(claims.hash);
      }
      error
    })?;
    if id == "egia" {
      claims.hash = crate::domain::graph::generated_v2::egia_chain_digest(graph).ok();
    }
    if graph_reader {
      check_reader(&claims, graph, "eos", catalog)?;
    }
    if graph_reader && version == "0.1.0" {
      for token in ["graph-0.1-reader", "node-types", "data-idl"] {
        feature(&claims, graph, "/version", None, token)?;
      }
      if id == "eos" {
        feature(
          &claims,
          graph,
          &format!("{pointer}/node_type"),
          None,
          "managed-control-grants",
        )?;
        if !spec.settings.skill_libraries.is_empty() {
          feature(
            &claims,
            graph,
            &format!("{pointer}/node_type"),
            None,
            "host-skill-abi-v2",
          )?;
        }
      }
    }
    if id == "bridge" {
      feature(
        &claims,
        graph,
        &format!("{pointer}/node_type"),
        None,
        "vision-ws",
      )?;
    }
    if vision_selected {
      let tokens: &[&str] = match id {
        "eos" => &["vision-provenance-v1"],
        "bridge" => &["vision-owner-uds-v1"],
        "egia" => &["vision-evidence-ingress-v1", "vision-inspection-uds-v1"],
        _ => &[],
      };
      for token in tokens {
        feature(&claims, graph, &format!("{pointer}/node_type"), None, token)?;
      }
    }
    if id == "egia" {
      for token in ["managed-config-v1", "eos-gateway-client-v2"] {
        feature(&claims, graph, &format!("{pointer}/node_type"), None, token)?;
      }
      if let Some(path) = &egia_route_path {
        feature(&claims, graph, path, None, "egia-evidence-scheduler-v1")?;
      }
    }
    if id == "eos" {
      if spec.package.is_some() || spec.settings.command.iter().any(|arg| arg == "--log-dir") {
        feature(&claims, graph, "/version", None, "log-dir")?;
      }
      if spec.package.is_some() {
        feature(&claims, graph, "/version", None, "package-process-v1")?;
      }
      for (node_path, node_type, _) in nodes(graph, "eos") {
        let required = if node_type.starts_with("eos.dispatcher.primitive_") {
          Some("primitive-binding")
        } else if node_type.starts_with("eos.interface.camera_gate") {
          Some("vision-gate")
        } else {
          None
        };
        if let Some(token) = required {
          feature(
            &claims,
            graph,
            &format!("{node_path}/node_type"),
            Some(&node_type),
            token,
          )?;
        }
      }
    }
  }
  Ok(())
}

pub fn check(
  paths: &Paths,
  graph: &Value,
  specs: &[ProcessSpec],
  catalog: &Value,
) -> Result<(), Diagnostic> {
  check_inner(paths, graph, specs, catalog, None)
}

pub fn check_core(
  paths: &Paths,
  graph: &Value,
  specs: &[ProcessSpec],
  catalog: &Value,
  core: &str,
) -> Result<(), Diagnostic> {
  check_inner(paths, graph, specs, catalog, Some(core))
}

/// Probe owner setup identities before invoking their package-aware catalogs.
/// This preserves a typed selected-binary failure instead of a catalog stderr error.
pub fn check_setup_basics(paths: &Paths, graph: &Value) -> Result<(), Diagnostic> {
  let version = graph["version"].as_str().unwrap_or_default();
  for core in ["eos", "egia"] {
    let selected = if version == "0.2.0" {
      graph
        .pointer("/cores/isos/layers")
        .and_then(Value::as_object)
        .is_some_and(|layers| {
          layers
            .keys()
            .any(|layer| layer.starts_with(if core == "eos" { "eos_" } else { "ei_" }))
        })
    } else {
      graph.pointer(&format!("/cores/{core}")).is_some()
    };
    if !selected {
      continue;
    }
    let claims = setup(paths, core, graph)?;
    if !claims.graph_versions.contains(version) {
      return Err(claims.diagnostic(
        "reader_graph_unsupported",
        "/version",
        "graph",
        Some(version),
        None,
        None,
        None,
      ));
    }
    if core == "egia" {
      for (path, id, _) in nodes(graph, "egia") {
        let suffix = id.rsplit_once("@v").map(|(_, suffix)| suffix).unwrap_or("");
        if !claims.type_versions.contains(suffix)
          || (!claims.node_types.contains(&id)
            && !(claims.scalar_fallback && version == "1.0.0" && id.ends_with("@v1.0")))
        {
          return Err(claims.diagnostic(
            "reader_type_unsupported",
            format!("{path}/node_type"),
            "reader",
            Some(version),
            Some(&id),
            None,
            None,
          ));
        }
      }
    }
    if version == "0.1.0" {
      for token in if core == "eos" {
        &[
          "graph-0.1-reader",
          "node-types",
          "data-idl",
          "setup-v2",
          "catalog",
        ][..]
      } else {
        &["graph-0.1-reader", "setup-v2", "catalog"][..]
      } {
        feature(&claims, graph, "/version", None, token)?;
      }
    }
  }
  Ok(())
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::os::unix::fs::PermissionsExt;

  #[test]
  fn unified_owner_nodes_are_selected_by_layer_prefix() {
    let graph = json!({"version":"0.2.0","cores":{"isos":{"layers":{
      "eos_hardware":{"groups":{"camera":{"nodes":{"front":{"node_type":"isos.eos_hardware.camera@v0.2"}}}}},
      "ei_io":{"groups":{"link":{"nodes":{"gateway":{"node_type":"isos.ei_io.gateway@v0.2"}}}}},
      "process":{"groups":{"component":{"nodes":{"eos":{"node_type":"isos.process.component@v0.2"}}}}}
    }}}});
    let eos = nodes(&graph, "eos");
    let ei = nodes(&graph, "egia");
    assert_eq!(eos.len(), 1);
    assert_eq!(ei.len(), 1);
    assert!(eos[0].0.contains("/eos_hardware/"));
    assert!(ei[0].0.contains("/ei_io/"));
  }

  fn claims() -> Claims {
    Claims::empty("eos", "aether-runtime", Path::new("/missing"))
  }

  fn reply() -> Value {
    json!({"binary":"aether-runtime","product_version":"fixture",
            "graph_version":"1.0.0","node_type_version":"1.0",
            "supported_graph_versions":["0.1.0","1.0.0"],
            "supported_node_type_versions":["0.1","1.0"],
            "supported_node_types":["eos.hardware.camera@v0.1"],
            "features":["graph-0.1-reader","vision-gate"]})
  }

  #[test]
  fn exact_arrays_and_features_admit_only_claimed_types() {
    let selected = parse_claims(claims(), &reply(), true, false).unwrap();
    let graph = json!({"version":"0.1.0","cores":{"eos":{"layers":{"hardware":{"groups":{"camera":{"nodes":{"front":{"node_type":"eos.hardware.camera@v0.1"}}}}}}}}});
    let catalog = json!({"types":[{"id":"eos.hardware.camera@v0.1"}]});
    check_reader(&selected, &graph, "eos", &catalog).unwrap();
    feature(&selected, &graph, "/version", None, "graph-0.1-reader").unwrap();
    let error = feature(&selected, &graph, "/version", None, "data-idl").unwrap_err();
    assert_eq!(error.code, "reader_feature_unsupported");
    assert_eq!(error.context["reader"]["requested"]["feature"], "data-idl");
    let mut no_type = reply();
    no_type["supported_node_types"] = json!([]);
    let selected = parse_claims(claims(), &no_type, true, false).unwrap();
    let error = check_reader(&selected, &graph, "eos", &catalog).unwrap_err();
    assert_eq!(error.code, "reader_type_unsupported");
    assert_eq!(
      error.path,
      "/cores/eos/layers/hardware/groups/camera/nodes/front/node_type"
    );
  }

  #[test]
  fn old_eos_runtime_without_managed_grant_feature_is_rejected_at_its_process_node() {
    let selected = parse_claims(claims(), &reply(), true, false).unwrap();
    let graph = json!({"version":"0.1.0"});
    let path = "/cores/isos/layers/process/groups/component/nodes/eos/node_type";
    let error = feature(&selected, &graph, path, None, "managed-control-grants").unwrap_err();
    assert_eq!(error.code, "reader_feature_unsupported");
    assert_eq!(error.path, path);
    assert_eq!(
      error.context["reader"]["requested"]["feature"],
      "managed-control-grants"
    );
  }

  #[test]
  fn partial_duplicate_and_malformed_arrays_never_use_scalar_fallback() {
    let mut partial = reply();
    partial
      .as_object_mut()
      .unwrap()
      .remove("supported_node_types");
    assert_eq!(
      parse_claims(claims(), &partial, true, false)
        .unwrap_err()
        .code,
      "reader_identity_invalid"
    );
    let mut duplicate = reply();
    duplicate["supported_graph_versions"] = json!(["0.1.0", "0.1.0"]);
    assert_eq!(
      parse_claims(claims(), &duplicate, true, false)
        .unwrap_err()
        .code,
      "reader_identity_invalid"
    );
    let mut wrong = reply();
    wrong["binary"] = json!("aether-profile");
    assert_eq!(
      parse_claims(claims(), &wrong, true, false)
        .unwrap_err()
        .code,
      "reader_binary_mismatch"
    );
  }

  #[test]
  fn scalar_fallback_is_legacy_only_and_runtime_features_still_apply() {
    let old = json!({"binary":"aether-runtime","product_version":"fixture",
            "graph_version":"1.0.0","node_type_version":"1.0","features":[]});
    let selected = parse_claims(claims(), &old, true, false).unwrap();
    assert!(selected.scalar_fallback);
    assert!(selected.graph_versions.contains("1.0.0"));
    let graph = json!({"version":"1.0.0","cores":{}});
    let error = feature(&selected, &graph, "/version", None, "package-process-v1").unwrap_err();
    assert_eq!(error.code, "reader_feature_unsupported");
    let target = json!({"version":"0.1.0","cores":{}});
    assert_eq!(
      check_reader(&selected, &target, "eos", &json!({"types":[]}))
        .unwrap_err()
        .code,
      "reader_graph_unsupported"
    );
  }

  #[test]
  fn contributed_type_needs_exact_catalog_pin() {
    let mut reply = reply();
    reply["supported_node_types"] = json!(["eos.process.worker@v0.1"]);
    let selected = parse_claims(claims(), &reply, true, false).unwrap();
    let graph = json!({"version":"0.1.0","cores":{"eos":{"layers":{"process":{"groups":{"worker":{"nodes":{"sim":{"node_type":"eos.process.worker@v0.1","package":{"id":"sample/worker","version":"0.1.1"}}}}}}}}}});
    let catalog = json!({"types":[{"id":"eos.process.worker@v0.1","package":{"id":"sample/worker","version":"0.1.1"}}]});
    check_reader(&selected, &graph, "eos", &catalog).unwrap();
    let wrong = json!({"types":[{"id":"eos.process.worker@v0.1","package":{"id":"sample/worker","version":"0.1.0"}}]});
    let error = check_reader(&selected, &graph, "eos", &wrong).unwrap_err();
    assert_eq!(error.code, "reader_package_pin_mismatch");
    assert!(error.path.ends_with("/package"));
  }

  #[test]
  fn egia_runtime_must_not_claim_graph_arrays() {
    let claims = Claims::empty("egia", "egia", Path::new("/missing"));
    let normal = json!({"binary":"egia","product_version":"fixture","features":["managed-config-v1","eos-gateway-client-v2"]});
    parse_claims(claims.clone(), &normal, false, false).unwrap();
    let mut invalid = normal;
    invalid["supported_graph_versions"] = json!(["0.1.0"]);
    assert_eq!(
      parse_claims(claims, &invalid, false, false)
        .unwrap_err()
        .code,
      "reader_identity_invalid"
    );
  }

  #[test]
  fn duplicate_json_fields_fail_even_when_nested() {
    assert!(parse_unique_json(br#"{"binary":"egia","binary":"eos"}"#).is_err());
    assert!(parse_unique_json(br#"{"reader":{"features":[],"features":[]}}"#).is_err());
    assert!(parse_unique_json(br#"{"reader":{"features":[]}}"#).is_ok());
  }

  #[test]
  fn probe_rejects_descendant_pipe_hold_at_deadline() {
    let root = tempfile::tempdir().unwrap();
    let binary = root.path().join("aether-runtime");
    fs::write(
            &binary,
            "#!/bin/sh\nprintf '%s\\n' '{\"binary\":\"aether-runtime\",\"product_version\":\"x\",\"features\":[]}'\nsleep 30 &\n",
        )
        .unwrap();
    fs::set_permissions(&binary, fs::Permissions::from_mode(0o700)).unwrap();
    let started = Instant::now();
    let error = probe(&binary, "aether-runtime", "eos", false, None).unwrap_err();
    assert_eq!(error.code, "reader_identity_invalid");
    assert!(started.elapsed() < Duration::from_secs(8));
  }

  #[test]
  fn capabilities_probe_does_not_receive_skill_selection_handoff() {
    let root = tempfile::tempdir().unwrap();
    let binary = root.path().join("aether-runtime");
    fs::write(&binary, "#!/bin/sh\nif [ -n \"${EOS_SKILL_SELECTION_FILE+x}\" ]; then exit 77; fi\nprintf '%s\\n' '{\"binary\":\"aether-runtime\",\"product_version\":\"x\",\"features\":[]}'\n").unwrap();
    fs::set_permissions(&binary, fs::Permissions::from_mode(0o700)).unwrap();
    let mut environment = BTreeMap::new();
    environment.insert(
      crate::runtime::process::skill_selection::ENV.to_string(),
      "/tmp/forgery".to_string(),
    );
    probe(&binary, "aether-runtime", "eos", false, Some(&environment)).unwrap();
  }
}
