#!/usr/bin/env python3
"""Generate public EOS JSON Schemas from annotated protobuf descriptors."""

from __future__ import annotations

import argparse
import importlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

# Ubuntu 22.04 ships protoc 3.12. Its generated Python modules require the
# pure-Python descriptor implementation with newer protobuf runtimes.
os.environ.setdefault("PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION", "python")

try:
  from google.protobuf import descriptor_pb2
except ImportError as error:
  raise SystemExit(
      "generate_eos_contracts: Python package 'protobuf' is required "
      "(install it in the build Python environment)"
  ) from error

from contract_manifest import ContractError, build_manifest, render_manifest


ROOT = Path(__file__).resolve().parents[1]
SECTIONS = ("actions", "body", "world")


def load_descriptors(proto_root: Path):
  """Return the descriptor set and decoded custom message options."""
  sources = sorted(
      str(path.relative_to(proto_root))
      for section in SECTIONS
      for path in (proto_root / "eos" / section).glob("*.proto")
  )
  if not sources:
    raise ContractError(f"{proto_root}: no public EOS schema proto files")
  with tempfile.TemporaryDirectory(prefix="eos-contract-descriptors-") as work:
    work_path = Path(work)
    descriptor_path = work_path / "contracts.pb"
    commands = (
        ["protoc", "--experimental_allow_proto3_optional", f"-I{proto_root}",
         f"--descriptor_set_out={descriptor_path}",
         "--include_imports", *sources],
        ["protoc", "--experimental_allow_proto3_optional", f"-I{proto_root}",
         f"--python_out={work_path}",
         "eos/options/schema.proto"],
    )
    for command in commands:
      try:
        subprocess.run(command, cwd=proto_root, check=True, capture_output=True, text=True)
      except FileNotFoundError as error:
        raise ContractError("protoc is required to generate EOS contracts") from error
      except subprocess.CalledProcessError as error:
        raise ContractError(error.stderr.strip() or "protoc failed") from error
    sys.path.insert(0, str(work_path))
    try:
      options = importlib.import_module("eos.options.schema_pb2")
      descriptors = descriptor_pb2.FileDescriptorSet.FromString(descriptor_path.read_bytes())
      # Descriptors keep their extension values even after the temporary module
      # is unloaded; only the option identifiers are needed by callers.
      return descriptors, options.json_schema, options.json_schema_path
    finally:
      sys.path.pop(0)


def schema_entries(proto_root: Path):
  descriptors, schema_option, path_option = load_descriptors(proto_root)
  entries = {}
  for file in descriptors.file:
    for message in file.message_type:
      if not message.options.HasExtension(path_option):
        continue
      if not message.options.HasExtension(schema_option):
        raise ContractError(f"{file.name}:{message.name}: missing JSON Schema option")
      relative = Path(message.options.Extensions[path_option])
      if len(relative.parts) != 2 or relative.parts[0] not in SECTIONS or not relative.name.endswith(".json"):
        raise ContractError(f"{file.name}:{message.name}: invalid generated path {relative}")
      if relative in entries:
        raise ContractError(f"duplicate generated schema path: {relative}")
      payload = message.options.Extensions[schema_option]
      schema = json.loads(payload)
      declared_fields = {field.json_name or field.name for field in message.field}
      schema_fields = set(schema.get("properties", {}))
      if declared_fields != schema_fields:
        raise ContractError(
            f"{file.name}:{message.name}: proto fields differ from JSON properties: "
            f"{sorted(declared_fields ^ schema_fields)}"
        )
      entries[relative] = payload.encode("utf-8")
  if not entries:
    raise ContractError("descriptors contain no annotated EOS schemas")
  return entries


def generate(proto_root: Path, output: Path, check: bool) -> None:
  entries = schema_entries(proto_root)
  with tempfile.TemporaryDirectory(prefix="eos-contract-output-") as work:
    staged = Path(work)
    for relative, payload in entries.items():
      destination = staged / relative
      destination.parent.mkdir(parents=True, exist_ok=True)
      destination.write_bytes(payload)
    manifest = render_manifest(build_manifest(staged)).encode("utf-8")
    entries[Path("contracts.manifest.json")] = manifest
    existing = {
        path.relative_to(output)
        for section in SECTIONS
        for path in (output / section).glob("*.json")
    }
    if (output / "contracts.manifest.json").exists():
      existing.add(Path("contracts.manifest.json"))
    stale = sorted(existing - entries.keys())
    if check:
      mismatches = [relative for relative, payload in entries.items()
                    if not (output / relative).is_file() or (output / relative).read_bytes() != payload]
      if mismatches or stale:
        names = ", ".join(str(path) for path in sorted([*mismatches, *stale]))
        raise ContractError(f"generated EOS contracts are stale: {names}")
      return
    for relative in stale:
      (output / relative).unlink()
    for relative, payload in entries.items():
      destination = output / relative
      destination.parent.mkdir(parents=True, exist_ok=True)
      destination.write_bytes(payload)


def main() -> int:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--proto-root", type=Path, default=ROOT / "share/contracts")
  parser.add_argument("--output", type=Path, default=ROOT / "share/ext/eos")
  parser.add_argument("--check", action="store_true")
  args = parser.parse_args()
  try:
    generate(args.proto_root.resolve(), args.output.resolve(), args.check)
  except (ContractError, OSError) as error:
    print(f"generate_eos_contracts: {error}", file=sys.stderr)
    return 1
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
