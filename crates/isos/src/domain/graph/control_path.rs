//! Resolve one registered UI event path from an admitted graph.
//!
//! The browser supplies only the producer path. Movement values, binding and
//! owner endpoint come from the saved graph, never from an HTTP event body.

use anyhow::{Context, Result, ensure};
use serde_json::Value;
use std::net::SocketAddr;

#[derive(Clone)]
pub struct ControlPath {
  pub eos_gateway: String,
  pub binding: String,
  pub hardware: String,
  pub endpoint: SocketAddr,
  pub x_m_s: f64,
  pub y_m_s: f64,
  pub yaw_rad_s: f64,
  pub valid_for_ms: u64,
}

fn node<'a>(graph: &'a Value, path: &str) -> Option<&'a Value> {
  let mut parts = path.split('/');
  let (Some(core), Some(layer), Some(group), Some(id), None) = (
    parts.next(),
    parts.next(),
    parts.next(),
    parts.next(),
    parts.next(),
  ) else {
    return None;
  };
  if [core, layer, group, id]
    .iter()
    .any(|part| part.is_empty() || part.contains('~'))
  {
    return None;
  }
  graph
    .get("cores")?
    .get(core)?
    .get("layers")?
    .get(layer)?
    .get("groups")?
    .get(group)?
    .get("nodes")?
    .get(id)
}

fn consumers(graph: &Value, source: &str, output: &str) -> Vec<(String, String)> {
  let mut result = Vec::new();
  let Some(cores) = graph["cores"].as_object() else {
    return result;
  };
  for (core_id, core) in cores {
    let Some(layers) = core["layers"].as_object() else {
      continue;
    };
    for (layer_id, layer) in layers {
      let Some(groups) = layer["groups"].as_object() else {
        continue;
      };
      for (group_id, group) in groups {
        let Some(nodes) = group["nodes"].as_object() else {
          continue;
        };
        for (node_id, value) in nodes {
          let Some(inputs) = value["inputs"].as_object() else {
            continue;
          };
          for (input, sources) in inputs {
            let sources: Vec<&Value> = sources
              .as_array()
              .map_or_else(|| vec![sources], |array| array.iter().collect());
            if sources
              .iter()
              .any(|entry| entry["node"] == source && entry["output"] == output)
            {
              result.push((
                format!("{core_id}/{layer_id}/{group_id}/{node_id}"),
                input.clone(),
              ));
            }
          }
        }
      }
    }
  }
  result
}

fn exactly_one(
  graph: &Value,
  source: &str,
  output: &str,
  input: &str,
  expected_type: &str,
) -> Result<String> {
  let matches: Vec<_> = consumers(graph, source, output)
    .into_iter()
    .filter(|(path, port)| {
      port == input && node(graph, path).is_some_and(|entry| entry["node_type"] == expected_type)
    })
    .collect();
  ensure!(
    matches.len() == 1,
    "control path requires one declared {expected_type} connection"
  );
  Ok(matches[0].0.clone())
}

pub fn resolve(graph: &Value, source: &str) -> Result<ControlPath> {
  ensure!(
    node(graph, source).is_some_and(|entry| entry["node_type"] == "isosui.input.keyboard@v0.1"),
    "source is not a registered keyboard producer"
  );
  let gateway = exactly_one(
    graph,
    source,
    "state",
    "state",
    "isos.interface.http_gateway@v0.1",
  )?;
  let gateway_node = node(graph, &gateway).context("IsOS gateway disappeared")?;
  let eos_gateway = exactly_one(
    graph,
    &gateway,
    "movement",
    "movement",
    "eos.interface.http_gateway@v0.1",
  )?;
  let binding = exactly_one(
    graph,
    &eos_gateway,
    "movement",
    "request",
    "eos.dispatcher.primitive_base@v0.1",
  )?;
  ensure!(
    gateway_node["settings"]["target"] == binding,
    "IsOS gateway target does not match the declared primitive edge"
  );
  let hardware = exactly_one(
    graph,
    &binding,
    "command",
    "command",
    "eos.hardware.base@v0.1",
  )?;
  let settings = &gateway_node["settings"];
  let number = |name: &str| -> Result<f64> {
    let value = settings[name]
      .as_f64()
      .with_context(|| format!("gateway {name} must be a number"))?;
    ensure!(value.is_finite(), "gateway {name} must be finite");
    Ok(value)
  };
  let valid_for_ms = settings["valid_for_ms"]
    .as_u64()
    .context("gateway valid_for_ms must be an integer")?;
  ensure!(
    (50..=500).contains(&valid_for_ms),
    "gateway lease must be 50..500 ms"
  );
  let bridge = graph
    .pointer("/cores/isos/layers/process/groups/component/nodes/bridge")
    .context("EOS bridge process is unavailable")?;
  ensure!(
    bridge["node_type"] == "isos.process.component@v0.1" && bridge["settings"]["role"] == "eos",
    "EOS bridge process declaration is invalid"
  );
  let listener = bridge["settings"]["ready_tcp"]
    .as_str()
    .context("EOS bridge listener is unavailable")?;
  let args = bridge["settings"]["args"]
    .as_array()
    .context("EOS bridge arguments are unavailable")?;
  ensure!(
    args
      .windows(2)
      .any(|pair| pair[0] == "--listen" && pair[1] == listener),
    "EOS bridge readiness and listener differ"
  );
  let endpoint: SocketAddr = listener.parse().context("EOS bridge listener is invalid")?;
  ensure!(
    endpoint.ip().is_loopback(),
    "EOS bridge listener must be loopback"
  );
  Ok(ControlPath {
    eos_gateway,
    binding,
    hardware,
    endpoint,
    x_m_s: number("x_m_s")?,
    y_m_s: number("y_m_s")?,
    yaw_rad_s: number("yaw_rad_s")?,
    valid_for_ms,
  })
}

#[cfg(test)]
mod tests {
  use super::*;
  use serde_json::json;

  #[test]
  fn path_resolves_only_declared_edges_and_fixed_settings() {
    let graph = json!({"cores":{
      "isosui":{"layers":{"input":{"groups":{"keyboard":{"nodes":{"drive":{"node_type":"isosui.input.keyboard@v0.1"}}}}}}},
      "isos":{"layers":{"interface":{"groups":{"gateway":{"nodes":{"key_to_base":{
        "node_type":"isos.interface.http_gateway@v0.1",
        "settings":{"target":"eos/dispatcher/base/velocity","x_m_s":0.2,"y_m_s":0.0,"yaw_rad_s":0.0,"valid_for_ms":150},
        "inputs":{"state":{"node":"isosui/input/keyboard/drive","output":"state"}}
      }}}}},"process":{"groups":{"component":{"nodes":{"bridge":{"node_type":"isos.process.component@v0.1","settings":{"role":"eos","args":["--listen","127.0.0.1:16020"],"ready_tcp":"127.0.0.1:16020"}}}}}}}},
      "eos":{"layers":{"interface":{"groups":{"gateway":{"nodes":{"control":{
        "node_type":"eos.interface.http_gateway@v0.1",
        "inputs":{"movement":{"node":"isos/interface/gateway/key_to_base","output":"movement"}}
      }}}}},"dispatcher":{"groups":{"base":{"nodes":{"velocity":{
        "node_type":"eos.dispatcher.primitive_base@v0.1",
        "inputs":{"request":{"node":"eos/interface/gateway/control","output":"movement"}}
      }}}}},"hardware":{"groups":{"base":{"nodes":{"drive":{
        "node_type":"eos.hardware.base@v0.1",
        "inputs":{"command":{"node":"eos/dispatcher/base/velocity","output":"command"}}
      }}}}}}}
    }});
    let route = resolve(&graph, "isosui/input/keyboard/drive").unwrap();
    assert_eq!(route.binding, "eos/dispatcher/base/velocity");
    assert_eq!(route.hardware, "eos/hardware/base/drive");
    assert_eq!(route.valid_for_ms, 150);
    let mut detached = graph.clone();
    detached["cores"]["eos"]["layers"]["interface"]["groups"]["gateway"]["nodes"]["control"]["inputs"] =
      json!({});
    assert!(resolve(&detached, "isosui/input/keyboard/drive").is_err());
  }
}
