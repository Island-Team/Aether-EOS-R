#!/usr/bin/env python3
"""Resolve collision-free shared roots for build-system consumption."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def main(argv: list[str] | None = None) -> int:
  """Print a CMake fragment containing the configured physical roots."""
  parser = argparse.ArgumentParser()
  parser.add_argument("--prefix", type=Path, required=True)
  parser.add_argument("--config", type=Path, required=True)
  args = parser.parse_args(argv)
  prefix = args.prefix.resolve()
  sys.path.insert(0, str(prefix / "src"))

  from island.path import Layout
  from island.share import configured_resources

  layout = Layout.resolve(mode="source", prefix=prefix, environ={})
  resources = configured_resources(layout, args.config)
  print("set(ISLAND_CONFIGURED_SHARED_ROOTS")
  for root in resources.roots:
    print(f"  [==[{root}]==]")
  print(")")
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
