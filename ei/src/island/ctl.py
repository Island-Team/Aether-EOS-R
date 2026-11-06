"""Read-only Island layout and product-configuration inspection command."""

from __future__ import annotations

import argparse
import sys

from island.boot import (
  BootError,
  components,
  config_path,
  factory_config_path,
  preflight,
)
from island.cfg import ConfigError, load
from island.path import Layout
from island.share import SharedResources


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
  """Parse one narrow read-only administration command."""
  parser = argparse.ArgumentParser(
    prog="islandctl",
    description="Inspect or validate Island-egia layout and config",
  )
  parser.add_argument("--config", default=None, help="product config path")
  parser.add_argument("command", choices=("paths", "config", "validate"))
  return parser.parse_args(argv)


def _paths(layout: Layout) -> None:
  """Print the resolved immutable and mutable role roots."""
  values = (
    ("mode", layout.mode.value),
    ("prefix", layout.prefix),
    ("etc", layout.etc),
    ("share", layout.share),
    ("libexec", layout.libexec),
    ("state", layout.state),
    ("cache", layout.cache),
    ("log", layout.log),
    ("run", layout.run),
    ("tmp", layout.tmp),
  )
  for name, value in values:
    print(f"{name}={value}")


def _config(layout: Layout, value: str | None, *, validate: bool) -> None:
  """Print config selection and optionally require enabled commands."""
  path = config_path(layout, value)
  product = load(path, defaults=factory_config_path(layout))
  resources = SharedResources.resolve(product, layout) if validate else None
  items = components(product, layout, shared_resources=resources)
  if validate:
    preflight(items)
  print(f"config={path}")
  print(f"name={product.name}")
  print(f"profile={product.profile}")
  for root in product.share:
    print(f"share={root}")
  for item in items:
    print(f"{item.name}.enabled={str(item.enabled).lower()}")
    print(f"{item.name}.command={item.path}")
    print(f"{item.name}.available={str(item.available).lower()}")


def main(argv: list[str] | None = None, *, layout: Layout | None = None) -> int:
  """Run a read-only inspection or validation command."""
  args = parse_args(argv)
  resolved = layout or Layout.resolve()
  try:
    if args.command == "paths":
      _paths(resolved)
    else:
      _config(resolved, args.config, validate=args.command == "validate")
  except (BootError, ConfigError, ValueError) as exc:
    print(f"islandctl: {exc}", file=sys.stderr)
    return 2
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
