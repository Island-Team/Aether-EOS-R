"""Minimal, no-start capability reply for the selected EGIA interpreter."""

from __future__ import annotations

import json
from importlib.metadata import PackageNotFoundError, version

SETUP_FEATURES = (
  "setup-v2",
  "catalog",
  "graph-0.1-reader",
  "graph-0.2-ei-projection",
  "egia-evidence-routing-v1",
)
RUNTIME_FEATURES = (
  "managed-config-v1",
  "eos-gateway-client-v2",
  "egia-evidence-scheduler-v1",
  "vision-evidence-ingress-v1",
  "vision-inspection-uds-v1",
)


def product_version() -> str:
  """Report this package's product release without importing the runtime."""
  try:
    return version("ei")
  except PackageNotFoundError:
    # Source checkouts can be probed before editable installation.
    return "0.2.1"


def main() -> int:
  """Write one bounded JSON reply; never load config or start a service."""
  print(
    json.dumps(
      {
        "binary": "ei" if __package__ == "ei" else "egia",
        "product_version": product_version(),
        "features": list(RUNTIME_FEATURES),
      },
      separators=(",", ":"),
    )
  )
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
