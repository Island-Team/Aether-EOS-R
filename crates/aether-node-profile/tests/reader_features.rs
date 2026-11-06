use aether_node_profile::builtin_reader_node_types;
use serde_json::Value;

#[test]
fn built_in_reader_ids_match_frozen_contract_without_package_types() {
  let fixture: Value = serde_json::from_str(include_str!(
    "../../../tests/fixtures/reader_features/advertised.json"
  ))
  .unwrap();
  let ids = builtin_reader_node_types();
  assert_eq!(ids.iter().filter(|id| id.starts_with("eos.")).count(), 19);
  assert!(ids.iter().any(|id| id.starts_with("isos.eos_")));
  assert!(ids.windows(2).all(|pair| pair[0] < pair[1]));
  let expected = fixture["probes"]["aether-profile"]["supported_node_types"]
    .as_array()
    .unwrap()
    .iter()
    .map(|id| id.as_str().unwrap().to_owned())
    .collect::<Vec<_>>();
  assert_eq!(ids, expected);
  assert!(!ids.iter().any(|id| id.starts_with("eos.process.mujoco@")));
}
