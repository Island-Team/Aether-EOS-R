"""Validate the frozen design-only Q01 contract case matrix."""

import json
from pathlib import Path


def main() -> None:
  path = Path(__file__).with_name("cases.json")
  payload = json.loads(path.read_text(encoding="utf-8"))
  assert payload["schema"] == "q01-eos-vision-provenance-v1"
  assert payload["status"] == "target_not_implemented"
  cases = payload["cases"]
  ids = [case["id"] for case in cases]
  assert len(ids) == len(set(ids)) and len(ids) >= 8
  assert any(case["expect"]["outcome"] == "reject" for case in cases)
  assert any(case["expect"]["outcome"] in ("frame", "render", "accept", "display", "subscribed") for case in cases)
  for case in cases:
    assert isinstance(case["given"], dict)
    assert isinstance(case["expect"], dict)
    assert case["id"] and case["expect"]["outcome"]
    if case["expect"]["outcome"] == "reject":
      assert case["expect"].get("reason_code")
  print(f"{len(cases)} q01-eos-vision-provenance-v1 design cases valid")


if __name__ == "__main__":
  main()
