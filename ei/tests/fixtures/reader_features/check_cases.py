"""Check the frozen P05 EGIA design fixture without claiming runtime support."""

from __future__ import annotations

import json
from pathlib import Path


def main() -> None:
  root = Path(__file__).parent
  advertised = json.loads((root / "advertised.json").read_text(encoding="utf-8"))
  cases = json.loads((root / "cases.json").read_text(encoding="utf-8"))
  assert advertised["schema_version"] == cases["schema_version"] == 1
  assert advertised["slice"] == cases["slice"] == "P05"
  exact_ids = advertised["setup"]["supported_node_types"]
  assert len(exact_ids) == 4
  assert exact_ids == sorted(set(exact_ids)) == sorted(advertised["placements"])
  assert advertised["setup"]["supported_node_type_versions"] == ["1.0"]
  assert advertised["setup"]["features"] == ["setup-v2", "catalog", "graph-0.1-reader"]
  assert advertised["runtime"]["features"] == [
    "managed-config-v1",
    "eos-gateway-client-v2",
  ]
  assert advertised["runtime_graph_reader"] is False
  assert not any(key.startswith("supported_") for key in advertised["runtime"])
  case_ids = [case["id"] for case in cases["cases"]]
  assert len(case_ids) == len(set(case_ids)) == 22
  assert all("result" in case["expected"] for case in cases["cases"])
  print("EGIA P05 fixture valid: 22 unique cases, four exact IDs, nonreader runtime")


if __name__ == "__main__":
  main()
