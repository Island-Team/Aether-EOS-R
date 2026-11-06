"""Resolve collision-free shared-resource roots as one logical directory."""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from island.cfg import ConfigError, ProductConfig
from island.path import Layout


@dataclass(frozen=True, slots=True)
class SharedResources:
  """Indexed resources contributed by distinct physical roots."""

  roots: tuple[Path, ...]
  files: dict[PurePosixPath, Path]

  @classmethod
  def resolve(cls, config: ProductConfig, layout: Layout) -> SharedResources:
    """Resolve configured roots and reject missing roots or file collisions."""
    roots = tuple((layout.prefix / entry).resolve() for entry in config.share)
    files: dict[PurePosixPath, Path] = {}
    for root in roots:
      if not root.is_dir():
        raise ConfigError(f"shared resource root not found: {root}")
      for path in sorted(item for item in root.rglob("*") if item.is_file()):
        relative = PurePosixPath(path.relative_to(root).as_posix())
        previous = files.get(relative)
        if previous is not None:
          raise ConfigError(
            f"duplicate shared resource {relative}: {previous} and {path}"
          )
        files[relative] = path
    return cls(roots=roots, files=files)

  def require(self, relative: str | PurePosixPath) -> Path:
    """Return one logical resource path or raise an actionable error."""
    key = PurePosixPath(relative)
    try:
      return self.files[key]
    except KeyError as exc:
      raise ConfigError(f"shared resource not found: {key}") from exc

  def directory(self, relative: str | PurePosixPath) -> Path:
    """Return a logical directory when exactly one root contributes it."""
    key = Path(relative)
    matches = tuple(root / key for root in self.roots if (root / key).is_dir())
    if not matches:
      raise ConfigError(f"shared resource directory not found: {key}")
    if len(matches) > 1:
      raise ConfigError(f"shared resource directory has multiple owners: {key}")
    return next(iter(matches))

  def under(
    self, relative: str | PurePosixPath
  ) -> tuple[tuple[PurePosixPath, Path], ...]:
    """Return logical files below a directory in deterministic order."""
    prefix = PurePosixPath(relative)
    return tuple(
      (path, source)
      for path, source in sorted(
        self.files.items(), key=lambda item: item[0].as_posix()
      )
      if path == prefix or prefix in path.parents
    )


def require_eos_contracts(resources: SharedResources) -> tuple[Path, Path, Path, Path]:
  """Require EOS 2 discovery, capability, skill, and acknowledgement contracts."""
  return (
    resources.require("eos/contracts/eos/gateway.proto"),
    resources.require("eos/contracts/eos/capabilities.proto"),
    resources.require("eos/contracts/eos/skills.proto"),
    resources.require("eos/schemas/eos/actions/ack.v1.json"),
  )


def configured_resources(
  layout: Layout, config: Path | str | None = None
) -> SharedResources:
  """Load and index the product's configured shared-resource roots."""
  from island.cfg import load

  selected_config = config or os.environ.get("ISLAND_CONFIG")
  if selected_config is None:
    config_path = layout.etc / "island.toml"
  else:
    selected = Path(selected_config).expanduser()
    config_path = (
      selected.resolve()
      if selected.is_absolute()
      else (layout.prefix / selected).resolve()
    )
  product = load(
    config_path,
    defaults=layout.share / "defaults/island.toml",
  )
  return SharedResources.resolve(product, layout)


def repository_resources(root: Path) -> SharedResources:
  """Load the shared-resource index declared by a source checkout."""
  layout = Layout.resolve(mode="source", prefix=root, environ={})
  return configured_resources(layout, root / "etc/island.toml")
