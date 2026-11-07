//! Human-facing readiness for configured core executables.

use crate::{
  app::config::Paths, runtime::process::core_bins, runtime::owners::core_setup,
  domain::graph::generated_v2, domain::graph::graph_compat, app::isosui, runtime::process,
  runtime::process::reader_features,
};
use serde_json::{Value, json};
use std::collections::BTreeMap;

fn process_present(graph: &Value, id: &str) -> bool {
  graph
    .pointer(&format!(
      "/cores/isos/layers/process/groups/component/nodes/{id}"
    ))
    .is_some_and(|node| node["enabled"] != false)
}

pub fn report(paths: &Paths, name: &str, active: Option<&Value>) -> Value {
  let graph_path = paths.node_graph_path(name);
  let saved = graph_compat::read(&graph_path).ok();
  let graph_revision = saved.as_ref().map(|saved| saved.revision.clone());
  let graph: Value = saved.map_or(Value::Null, |saved| saved.value);
  let graph_admission_issue = if graph.is_object() {
    graph_compat::legacy_mapping_diagnostic(&graph, process::catalog().ok().as_ref())
      .map(|diagnostic| format!("saved graph is incompatible: {diagnostic}"))
  } else {
    None
  };
  let projected = if graph.is_object() {
    process::project_with_packages(paths, &graph.to_string()).ok()
  } else {
    None
  };
  let mut reader_issues = BTreeMap::new();
  if graph.is_object() && graph_admission_issue.is_none() {
    if let (Some(specs), Ok(catalog)) = (&projected, process::catalog_for_graph(paths, &graph)) {
      for core in ["eos", "egia"] {
        if let Err(issue) = reader_features::check_core(paths, &graph, specs, &catalog, core) {
          reader_issues.insert(core, issue);
        }
      }
    } else if let Err(issue) = reader_features::check_setup_basics(paths, &graph) {
      let core = if issue.context["reader"]["core"] == "egia" {
        "egia"
      } else {
        "eos"
      };
      reader_issues.insert(core, issue);
    }
  }
  let endpoints = graph
    .is_object()
    .then(|| core_setup::endpoints(paths, name, &graph))
    .transpose();
  let cores: Vec<Value> = ["eos", "egia"]
        .into_iter()
        .map(|core| {
            let explicit = match core {
                "eos" => std::env::var_os("ISOS_AETHER_BIN_DIR").is_some(),
                _ => std::env::var_os("ISOS_EI_BIN_DIR").is_some() || std::env::var_os("ISOS_EGIA_BIN_DIR").is_some(),
            };
            let path = if core == "eos" {
                core_bins::resolve("aether-profile")
            } else {
                core_setup::adapter_path(paths, core)
            };
            let enabled = process_present(&graph, core);
            let generated = if graph["version"] == "0.1.0" {
                graph_revision.as_deref().and_then(|revision| {
                    generated_v2::selected_root(paths, name, &graph, revision)
                        .ok()
                        .map(|root| root.join("core-setup/launch-inputs.json"))
                })
            } else {
                Some(paths.generated(name).join("core-setup/launch-inputs.json"))
            };
            let graph_configured = enabled && generated.is_some_and(|path| path.is_file());
            let running = active
                .and_then(|value| value.get("processes"))
                .and_then(Value::as_array)
                .is_some_and(|processes| {
                    processes
                        .iter()
                        .any(|process| process["id"] == core && process["state"] == "running")
                });
            let running_changed = running
                && projected.as_ref().is_some_and(|specs| {
                    specs
                        .iter()
                        .find(|spec| spec.id == core)
                        .is_some_and(|spec| {
                            active
                                .and_then(|value| value.get("processes"))
                                .and_then(Value::as_array)
                                .and_then(|processes| {
                                    processes.iter().find(|process| process["id"] == core)
                                })
                                .is_some_and(|process| {
                                    process["fingerprint"] != json!(spec.fingerprint)
                                })
                        })
                });
            let (path, version, compatible, reason) = match path {
                Ok(path) => match if core == "egia" && graph.is_object() {
                    core_setup::identity_for_graph(paths, core, &graph)
                } else {
                    core_setup::identity(paths, core)
                } {
                    Ok(identity) => {
                        let supports_graph = graph["version"] != "0.1.0"
                            || identity["supported_graph_versions"]
                                .as_array()
                                .is_some_and(|versions| versions.iter().any(|v| v == "0.1.0"));
                        (
                            Some(path.to_string_lossy().into_owned()),
                            identity["product_version"].as_str().map(str::to_owned),
                            supports_graph,
                            (!supports_graph).then(|| format!(
                                "{core} setup does not advertise graph 0.1.0"
                            )),
                        )
                    },
                    Err(error) => (
                        Some(path.to_string_lossy().into_owned()),
                        None,
                        false,
                        Some(error.to_string()),
                    ),
                },
                Err(error) => (None, None, false, Some(error.to_string())),
            };
            let inputs_issue = if graph_configured {
                core_setup::verify_launch_inputs(paths, name)
                    .err()
                    .map(|e| e.to_string())
            } else {
                None
            };
            let endpoint_issue = endpoints.as_ref().err().map(ToString::to_string);
            let selected_issue = reader_issues.get(core);
            let ready = compatible
                && graph_configured
                && graph_admission_issue.is_none()
                && selected_issue.is_none()
                && inputs_issue.is_none()
                && endpoint_issue.is_none()
                && !running_changed;
            let reason = if enabled {
                graph_admission_issue
                    .clone()
                    .or_else(|| selected_issue.map(ToString::to_string))
                    .or(reason)
                    .or(inputs_issue)
                    .or(endpoint_issue)
                    .or_else(|| {
                        running_changed.then(|| {
                            "running from an earlier configured build; restart to use the saved build"
                                .into()
                        })
                    })
            } else {
                None
            };
            let configured_endpoint = endpoints.as_ref().ok().and_then(|endpoints| {
                endpoints.as_ref().and_then(|value| match core {
                    "eos" => value.get("eos_control"),
                    "egia" => value.get("egia_northbound"),
                    _ => None,
                })
            });
            let action = if !enabled {
                "Core is not enabled in the saved graph".into()
            } else if graph_admission_issue.is_some() {
                "Use a supported graph version or explicitly migrate the saved graph".into()
            } else if !compatible {
                format!("Build {core} in this workspace and configure its binary directory")
            } else if !graph_configured {
                format!("Add the {core} process to the graph, then save it")
            } else if running_changed {
                "Restart the deployment to use the saved build".into()
            } else if reason.is_some() {
                if graph["version"] == "0.1.0" {
                    "Restore the pinned binary or create a new graph revision and Apply".into()
                } else {
                    "Run `isos init` to refresh the configured build, then restart".into()
                }
            } else if running {
                "Running from the configured build".into()
            } else {
                "Run the saved graph".into()
            };
            json!({"core":core,"path":path,"version":version,"compatible":compatible,
                "configured":explicit || path.is_some(),"enabled":enabled,"graph_configured":graph_configured,
                "running":running,"ready":ready,"reason":reason,"action":action,
                "configured_endpoint":configured_endpoint,
                "diagnostics":selected_issue.into_iter().collect::<Vec<_>>(),
                "compatible_upgrade":null})
        })
        .collect();
  let ui = match isosui::bundle_for_serve(paths) {
    Ok(path) => json!({"path":path,"ready":true}),
    Err(error) => json!({"path":null,"ready":false,"reason":error.to_string(),
            "action":"Build IsosUI under ui/ and configure ISOSUI_DIST_DIR"}),
  };
  json!({"cores":cores,"graph":{"path":graph_path,"configured":graph.is_object(),"version":graph["version"]},
        "isos":{"version":env!("CARGO_PKG_VERSION"),"path":std::env::current_exe().ok()},
        "isosui":ui})
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn disabled_process_is_not_an_enabled_core() {
    let graph = json!({"cores":{"isos":{"layers":{"process":{"groups":{"component":{"nodes":{
        "egia":{"enabled":false}
    }}}}}}}});
    assert!(!process_present(&graph, "egia"));
  }

  #[test]
  fn unmapped_legacy_graph_is_not_ready_even_with_generated_inputs() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    crate::app::config::atomic_write(
      &paths.node_graph_path("default"),
      include_bytes!("../../../tests/fixtures/graph_compat/legacy_unmapped_1_0_0.json"),
    )
    .unwrap();
    crate::app::config::atomic_write(
      &paths
        .generated("default")
        .join("core-setup/launch-inputs.json"),
      b"[]",
    )
    .unwrap();
    let status = report(&paths, "default", None);
    let eos = &status["cores"][0];
    assert_eq!(eos["ready"], false);
    let reason = eos["reason"].as_str().unwrap();
    assert!(reason.contains("/cores/eos/layers/control/groups/manual/nodes/manual_0/node_type"));
    assert!(reason.contains("unmapped_node_type"));
  }

  #[test]
  fn old_graph_version_takes_priority_over_changed_binary_guidance() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    crate::app::config::atomic_write(
            &paths.node_graph_path("default"),
            br#"{"version":"2.0.0","robot":{},"cores":{"isos":{"layers":{"process":{"groups":{"component":{"nodes":{"eos":{"id":"eos","node_type":"isos.process.component@v1.1","settings":{"role":"eos","args":[]}}}}}}}}}}"#,
        )
        .unwrap();
    let status = report(&paths, "default", None);
    let eos = &status["cores"][0];
    assert!(eos["reason"].as_str().unwrap().contains("incompatible"));
    assert!(
      eos["action"]
        .as_str()
        .unwrap()
        .contains("explicitly migrate")
    );
    assert!(status["cores"][1]["reason"].is_null());
  }

  #[test]
  fn new_graph_is_not_reported_as_an_unknown_version() {
    let temp = tempfile::tempdir().unwrap();
    let paths = Paths {
      config: temp.path().join("config"),
      data: temp.path().join("data"),
      runtime: temp.path().join("run"),
    };
    paths.prepare().unwrap();
    let graph = crate::domain::graph::starter::draft(&paths, "default").unwrap();
    crate::app::config::atomic_write(
      &paths.node_graph_path("default"),
      &serde_json::to_vec_pretty(&graph).unwrap(),
    )
    .unwrap();
    let status = report(&paths, "default", None);
    assert_eq!(status["graph"]["configured"], true);
    assert!(
      !status["cores"][0]["reason"]
        .as_str()
        .unwrap_or("")
        .contains("saved 0.1.0 graph requires")
    );
  }
}
