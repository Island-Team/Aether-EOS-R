#!/usr/bin/env python3
"""Check the frozen P05 design fixture without a runtime implementation."""

import json
from pathlib import Path


def main() -> None:
    root = Path(__file__).resolve().parents[3]
    fixture = json.loads((Path(__file__).parent / "cases.json").read_text())
    assert fixture["schema_version"] == 1
    assert fixture["slice"] == "P05"
    assert (root / fixture["baseline_graph"]).is_file()
    assert fixture["baseline_package"] == {
        "id": "island/mujoco-sim",
        "version": "0.1.1",
    }
    cases = fixture["cases"]
    assert len(cases) == 45
    ids = [case["id"] for case in cases]
    assert len(ids) == len(set(ids))
    by_id = {case["id"]: case for case in cases}
    operations = {"validate", "apply", "run", "restart", "version_check", "validate_apply_run", "reader_preflight"}
    outcomes = {"admit", "admit_read_only", "admit_reader_only", "reject", "suggest", "blocked", "first_failure"}
    codes = {
        "unsupported_graph_version", "unsupported_node_type", "migration_required", "package_invalid_sidecar",
        "package_changed", "generated_manifest_mismatch", "reader_identity_invalid",
        "reader_graph_unsupported", "reader_type_unsupported", "reader_feature_unsupported",
        "reader_binary_mismatch", "reader_package_pin_mismatch",
    }
    stages = {"graph", "package", "reader", "feature", "sidecar", "snapshot", "manifest"}
    for case in cases:
        assert case["id"] and case["operation"] in operations and case["mutation"]
        expected = case["expected"]
        assert expected["result"] in outcomes
        if expected["result"] == "reject":
            assert expected["code"] in codes and expected["stage"] in stages
            assert expected["path"] == "" or expected["path"].startswith("/")
            assert "/settings/command" not in expected["path"]
            assert expected["saved_bytes_unchanged"] is True
            assert expected["stop_calls"] == expected["spawn_calls"] == 0
            if case["operation"] == "restart":
                assert expected["active_pids_unchanged"] is True
            if expected["code"] == "reader_feature_unsupported":
                assert expected["requested_feature"]
        else:
            assert "code" not in expected
        if expected["result"] == "admit_reader_only":
            assert case["operation"] == "reader_preflight"
            assert expected["scope"] == "isolated_p05_preflight"
            assert expected["runtime_supported_arrays_absent"] is True
            assert expected["public_run_admitted"] is False
            assert expected["runtime_gateway_ready"] is False
    assert by_id["probe_order_deterministic"]["expected"]["first_path"] == "/version"
    assert by_id["legacy_backend_preempted_by_migration_required"]["expected"]["code"] == "migration_required"
    assert by_id["eos_capabilities_sidecar_free"]["expected"]["capabilities_argv"] == ["--capabilities"]
    assert by_id["egia_runtime_non_graph_reader_shape"]["expected"]["runtime_supported_arrays_absent"] is True
    assert by_id["egia_legacy_types_in_target_envelope"]["mutation"].startswith("egia_setup_only_")
    for case_id in ("egia_missing_managed_config_feature", "egia_missing_gateway_client_feature"):
        assert by_id[case_id]["expected"]["path"].endswith("/node_type")
    assert by_id["legacy_scalar_runtime_still_requires_feature"]["expected"]["requested_feature"] == "mujoco-child-v2"
    print(f"P05 fixture valid: {len(cases)} unique cases")


if __name__ == "__main__":
    main()
