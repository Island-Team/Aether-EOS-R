"""Load EOS-owned contracts from Island's configured shared-resource roots."""

from __future__ import annotations

import json
from functools import lru_cache
from pathlib import PurePosixPath
from typing import Any

from jsonschema import Draft202012Validator
from jsonschema.exceptions import SchemaError
from referencing import Registry, Resource
from referencing.exceptions import CannotDetermineSpecification

from island.path import Layout
from island.share import SharedResources, configured_resources

_resources: SharedResources | None = None


def configure(resources: SharedResources) -> None:
  """Select the collision-checked logical resource tree for this process."""
  global _resources
  _resources = resources
  schema_documents.cache_clear()
  schema_registry.cache_clear()


def resources() -> SharedResources:
  """Return configured resources, lazily loading the default product config."""
  global _resources
  if _resources is None:
    _resources = configured_resources(Layout.resolve())
  return _resources


def load_json(relative: str | PurePosixPath) -> dict[str, Any]:
  """Load one JSON object by its logical shared-resource path."""
  path = resources().require(relative)
  try:
    value = json.loads(path.read_text(encoding="utf-8"))
  except (OSError, json.JSONDecodeError) as exc:
    raise ValueError(f"cannot load shared contract {relative}: {exc}") from exc
  if not isinstance(value, dict):
    raise TypeError(f"shared contract must contain a JSON object: {relative}")
  return value


@lru_cache(maxsize=1)
def schema_documents() -> tuple[dict[str, Any], ...]:
  """Load every JSON Schema contributed below contracts and primitives."""
  documents: list[dict[str, Any]] = []
  identifiers: dict[str, PurePosixPath] = {}
  for prefix in (PurePosixPath("eos/schemas/eos"),):
    for relative, _source in resources().under(prefix):
      if relative.suffix != ".json":
        continue
      document = load_json(relative)
      if relative.name == "contracts.manifest.json":
        continue
      identifier = document.get("$id")
      if not isinstance(identifier, str) or not identifier:
        raise ValueError(f"shared JSON Schema has no $id: {relative}")
      previous = identifiers.get(identifier)
      if previous is not None:
        raise ValueError(
          f"duplicate shared JSON Schema $id {identifier!r}: {previous} and {relative}"
        )
      identifiers[identifier] = relative
      try:
        Draft202012Validator.check_schema(document)
      except SchemaError as exc:
        raise ValueError(
          f"invalid shared JSON Schema {relative}: {exc.message}"
        ) from exc
      documents.append(document)
  return tuple(documents)


@lru_cache(maxsize=1)
def schema_registry() -> Registry:
  """Build the reference registry for every configured JSON Schema $id."""
  try:
    return Registry().with_resources(
      (document["$id"], Resource.from_contents(document))
      for document in schema_documents()
    )
  except CannotDetermineSpecification as exc:
    raise ValueError(f"invalid configured JSON Schema: {exc}") from exc
