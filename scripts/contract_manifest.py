#!/usr/bin/env python3
"""Validate the canonical JSON Schema tree and emit its deterministic manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


SCHEMA_DIRS = (Path("actions"), Path("body"), Path("world"))
FORMAT_VERSION = 3
ROLES = frozenset({"primitive", "acknowledgement", "body-device", "world-resource"})
PRIMITIVE_VERSION = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
SCHEMA_FILENAME = re.compile(r"^.+\.v[1-9][0-9]*\.json$")


class ContractError(ValueError):
  """A canonical contract is malformed or internally inconsistent."""


def _resolve_pointer(document: Any, pointer: str, source: Path, reference: str) -> None:
  if not pointer:
    return
  if not pointer.startswith("/"):
    raise ContractError(f"{source}: unsupported JSON pointer in $ref {reference!r}")
  value = document
  try:
    for token in pointer[1:].split("/"):
      token = token.replace("~1", "/").replace("~0", "~")
      value = value[int(token)] if isinstance(value, list) else value[token]
  except (KeyError, IndexError, ValueError, TypeError) as error:
    raise ContractError(f"{source}: unresolved $ref {reference!r}") from error


def _references(value: Any):
  if isinstance(value, dict):
    reference = value.get("$ref")
    if isinstance(reference, str):
      yield reference
    for child in value.values():
      yield from _references(child)
  elif isinstance(value, list):
    for child in value:
      yield from _references(child)


def _role(relative: Path, schema: dict[str, Any]) -> str:
  discriminator = schema.get("properties", {}).get("type", {}).get("const")
  if relative.parts[:1] == ("actions",):
    return "acknowledgement" if discriminator == "primitive.ack" else "primitive"
  if relative.parts[:1] == ("body",):
    return "body-device"
  if relative.parts[:1] == ("world",):
    return "world-resource"
  raise ContractError(f"{relative}: no manifest v2 role for schema path")


def build_manifest(root: Path) -> dict[str, Any]:
  root = root.resolve()
  paths = sorted(
      path
      for directory in SCHEMA_DIRS
      for path in (root / directory).rglob("*.json")
      if SCHEMA_FILENAME.fullmatch(path.name)
  )
  if not paths:
    raise ContractError(f"{root}: no versioned contract schemas found")

  loaded: list[tuple[Path, bytes, dict[str, Any]]] = []
  by_id: dict[str, tuple[Path, dict[str, Any]]] = {}
  discriminators: dict[tuple[str, str], Path] = {}
  primitive_identities: dict[tuple[str, str], Path] = {}
  for path in paths:
    relative = path.relative_to(root)
    raw = path.read_bytes()
    try:
      schema = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
      raise ContractError(f"{relative}: invalid JSON: {error}") from error
    if not isinstance(schema, dict):
      raise ContractError(f"{relative}: schema root must be an object")
    schema_id = schema.get("$id")
    if not isinstance(schema_id, str) or not schema_id:
      raise ContractError(f"{relative}: schema must have a non-empty string $id")
    if schema_id in by_id:
      raise ContractError(f"{relative}: duplicate $id {schema_id!r} also used by {by_id[schema_id][0]}")
    by_id[schema_id] = (relative, schema)
    if schema.get("$schema") != "https://json-schema.org/draft/2020-12/schema":
      raise ContractError(f"{relative}: $schema must select JSON Schema draft 2020-12")

    role = _role(relative, schema)
    if role not in ROLES:
      raise ContractError(f"{relative}: unknown manifest role {role!r}")
    discriminator = schema.get("properties", {}).get("type", {}).get("const")
    if role == "body-device":
      device_class = schema.get("properties", {}).get("device_class", {}).get("const")
      expected_class = relative.name.removesuffix(".v1.json")
      if expected_class == "mic":
        expected_class = "microphone"
      if device_class != expected_class:
        raise ContractError(
            f"{relative}: body-device device_class must be {expected_class!r}"
        )
      if discriminator is not None:
        raise ContractError(f"{relative}: body-device must not declare a resource discriminator")
    elif not isinstance(discriminator, str) or not discriminator:
      raise ContractError(f"{relative}: {role} must declare properties.type.const")
    if role == "primitive":
      version = schema.get("x-island-primitive-version")
      if not isinstance(version, str) or not PRIMITIVE_VERSION.fullmatch(version):
        raise ContractError(
            f"{relative}: primitive must declare a semantic x-island-primitive-version"
        )
      identity = (discriminator, version)
      if identity in primitive_identities:
        raise ContractError(
            f"{relative}: duplicate primitive identity {discriminator!r} version {version!r} "
            f"also used by {primitive_identities[identity]}"
        )
      primitive_identities[identity] = relative
    elif discriminator is not None:
      if not isinstance(discriminator, str) or not discriminator:
        raise ContractError(f"{relative}: properties.type.const must be a non-empty string")
      family = "actions" if role == "acknowledgement" else "resources"
      key = (family, discriminator)
      if key in discriminators:
        raise ContractError(
            f"{relative}: duplicate {family} discriminator {discriminator!r} "
            f"also used by {discriminators[key]}"
        )
      discriminators[key] = relative
    loaded.append((relative, raw, schema))

  for relative, _, schema in loaded:
    for reference in _references(schema):
      target_id, separator, pointer = reference.partition("#")
      target = schema if not target_id else by_id.get(target_id, (None, None))[1]
      if target is None:
        raise ContractError(f"{relative}: unresolved external $ref {reference!r}")
      if separator:
        _resolve_pointer(target, pointer, relative, reference)

  acknowledgements = [
      path
      for (family, discriminator), path in discriminators.items()
      if family == "actions" and discriminator == "primitive.ack"
  ]
  if len(acknowledgements) != 1:
    raise ContractError("contract set must contain exactly one primitive.ack schema")

  entries = []
  for relative, raw, schema in loaded:
    role = _role(relative, schema)
    entry = {
          "id": schema["$id"],
          "path": relative.as_posix(),
          "role": role,
          "sha256": hashlib.sha256(raw).hexdigest(),
      }
    if role == "primitive":
      entry["name"] = schema["properties"]["type"]["const"]
      entry["version"] = schema["x-island-primitive-version"]
    entries.append(entry)
  set_digest = hashlib.sha256()
  for entry in entries:
    set_digest.update(entry["path"].encode())
    set_digest.update(b"\0")
    set_digest.update(entry["sha256"].encode())
    set_digest.update(b"\n")
  return {
      "format_version": FORMAT_VERSION,
      "schema_count": len(entries),
      "set_sha256": set_digest.hexdigest(),
      "schemas": entries,
  }


def render_manifest(manifest: dict[str, Any]) -> str:
  return json.dumps(manifest, indent=2, sort_keys=True) + "\n"


def main() -> int:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--root", type=Path, required=True, help="Island share directory")
  parser.add_argument("--output", type=Path, required=True, help="manifest destination")
  parser.add_argument("--check", action="store_true", help="fail unless output is current")
  args = parser.parse_args()
  try:
    rendered = render_manifest(build_manifest(args.root))
    if args.check:
      existing = args.output.read_text(encoding="utf-8") if args.output.is_file() else ""
      if existing != rendered:
        raise ContractError(f"{args.output}: stale; regenerate with {Path(__file__).name}")
    else:
      args.output.parent.mkdir(parents=True, exist_ok=True)
      args.output.write_text(rendered, encoding="utf-8")
  except (ContractError, OSError) as error:
    print(f"contract manifest: {error}", file=sys.stderr)
    return 1
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
