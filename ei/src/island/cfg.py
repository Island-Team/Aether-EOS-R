"""Load the small product-level configuration shared by composition roots."""

from __future__ import annotations

import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Any


class ConfigError(ValueError):
  """Raised when ``island.toml`` is missing or malformed."""


@dataclass(frozen=True, slots=True)
class ComponentConfig:
  """Validated activation and prefix-relative launcher for one component."""

  enabled: bool
  launch: str
  ready: str | None = None
  ready_timeout_s: float = 30.0


@dataclass(frozen=True, slots=True)
class Components:
  """Product components selected for one Island launch."""

  egia: ComponentConfig
  eos: ComponentConfig


@dataclass(frozen=True, slots=True)
class LoggingConfig:
  """Validated Island session-log retention and rotation policy."""

  max_bytes: int = 32 * 1024 * 1024
  backup_count: int = 5
  retained_sessions: int = 20


@dataclass(frozen=True, slots=True)
class ProductConfig:
  """Validated product identity, profile, and component selection."""

  name: str
  profile: str
  share: tuple[str, ...]
  components: Components
  logging: LoggingConfig


def load(path: Path, *, defaults: Path | None = None) -> ProductConfig:
  """Layer factory defaults beneath deployment config and validate the result.

  Args:
    path: Required deployment overlay, which may omit factory-provided fields.
    defaults: Optional complete factory configuration loaded first.

  Raises:
    ConfigError: If either file cannot be read or the merged shape is invalid.
  """
  document = _read(defaults, "product defaults") if defaults is not None else {}
  document = _deep_merge(document, _read(path, "product config"))

  name = _text(document, "name")
  profile = _text(document, "profile")
  share = _paths(document, "share")
  raw_components = document.get("components")
  if not isinstance(raw_components, dict):
    raise ConfigError("[components] must be a table")
  unknown = sorted(set(raw_components) - {"egia", "eos"})
  if unknown:
    raise ConfigError(f"unknown components: {', '.join(unknown)}")
  logging = _logging_config(document.get("logging", {}))
  return ProductConfig(
    name=name,
    profile=profile,
    share=share,
    components=Components(
      egia=_component_config(raw_components, "egia"),
      eos=_component_config(raw_components, "eos"),
    ),
    logging=logging,
  )


def _read(path: Path, label: str) -> dict[str, Any]:
  """Read one TOML document with a role-specific error message."""
  try:
    with path.open("rb") as stream:
      return tomllib.load(stream)
  except FileNotFoundError as exc:
    raise ConfigError(f"{label} not found: {path}") from exc
  except (OSError, tomllib.TOMLDecodeError) as exc:
    raise ConfigError(f"cannot read {label} {path}: {exc}") from exc


def _deep_merge(base: dict[str, Any], overlay: dict[str, Any]) -> dict[str, Any]:
  """Recursively merge tables while deployment scalars replace factory data."""
  merged = dict(base)
  for key, value in overlay.items():
    current = merged.get(key)
    if isinstance(current, dict) and isinstance(value, dict):
      merged[key] = _deep_merge(current, value)
    else:
      merged[key] = value
  return merged


def _text(document: dict[str, Any], key: str) -> str:
  """Return one required non-empty string field."""
  value = document.get(key)
  if not isinstance(value, str) or not value.strip():
    raise ConfigError(f"{key} must be a non-empty string")
  return value.strip()


def _paths(document: dict[str, Any], key: str) -> tuple[str, ...]:
  """Return a required non-empty list of distinct relative paths."""
  value = document.get(key)
  if not isinstance(value, list) or not value:
    raise ConfigError(f"{key} must be a non-empty list of paths")
  paths: list[str] = []
  for item in value:
    if not isinstance(item, str) or not item.strip():
      raise ConfigError(f"{key} entries must be non-empty strings")
    path = Path(item.strip())
    if path.is_absolute():
      raise ConfigError(f"{key} entries must be prefix-relative paths: {item}")
    normalized = path.as_posix()
    if normalized in paths:
      raise ConfigError(f"duplicate {key} path: {normalized}")
    paths.append(normalized)
  return tuple(paths)


def _component_config(document: dict[str, Any], key: str) -> ComponentConfig:
  """Return one strict component table."""
  value = document.get(key)
  if not isinstance(value, dict):
    raise ConfigError(f"components.{key} must be a table")
  unknown = sorted(set(value) - {"enabled", "launch", "ready", "ready_timeout_s"})
  if unknown:
    raise ConfigError(f"unknown components.{key} settings: {', '.join(unknown)}")
  enabled = value.get("enabled")
  if not isinstance(enabled, bool):
    raise ConfigError(f"components.{key}.enabled must be a boolean")
  launch = value.get("launch")
  if not isinstance(launch, str) or not launch.strip():
    raise ConfigError(f"components.{key}.launch must be a non-empty string")
  if Path(launch).is_absolute():
    raise ConfigError(f"components.{key}.launch must be prefix-relative")
  ready = value.get("ready")
  if ready is not None and (not isinstance(ready, str) or not ready.strip()):
    raise ConfigError(f"components.{key}.ready must be a non-empty host:port string")
  ready_timeout_s = value.get("ready_timeout_s", 30.0)
  if (
    isinstance(ready_timeout_s, bool)
    or not isinstance(ready_timeout_s, int | float)
    or ready_timeout_s <= 0
  ):
    raise ConfigError(f"components.{key}.ready_timeout_s must be a positive number")
  return ComponentConfig(
    enabled=enabled,
    launch=launch.strip(),
    ready=ready.strip() if ready is not None else None,
    ready_timeout_s=float(ready_timeout_s),
  )


def _logging_config(value: object) -> LoggingConfig:
  """Return the strict, always-enabled product session-log policy."""
  if not isinstance(value, dict):
    raise ConfigError("[logging] must be a table")
  unknown = sorted(set(value) - {"max_bytes", "backup_count", "retained_sessions"})
  if unknown:
    raise ConfigError(f"unknown logging settings: {', '.join(unknown)}")

  def positive_int(key: str, default: int) -> int:
    raw = value.get(key, default)
    if isinstance(raw, bool) or not isinstance(raw, int) or raw <= 0:
      raise ConfigError(f"logging.{key} must be a positive integer")
    return raw

  return LoggingConfig(
    max_bytes=positive_int("max_bytes", 32 * 1024 * 1024),
    backup_count=positive_int("backup_count", 5),
    retained_sessions=positive_int("retained_sessions", 20),
  )
