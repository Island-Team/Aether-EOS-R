#!/usr/bin/env python3
"""Print the descriptor-derived EOS RPC catalog and generated JSON contracts."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

from generate_north_http_openapi import collect_types, descriptor_set, ref, render_types


ROOT = Path(__file__).resolve().parents[1]
SERVICE_FILES = ("eos/gateway.proto", "eos/skills.proto", "eos/workflow.proto")


def contracts_root(explicit: str | None) -> Path:
  candidates = []
  if explicit:
    candidates.append(Path(explicit))
  else:
    for key in ("EOS_CONTRACTS", "ISLAND_SHARE"):
      if os.environ.get(key):
        candidates.append(Path(os.environ[key]))
    candidates.append(ROOT / "share")
  for candidate in candidates:
    if not candidate.is_absolute():
      raise ValueError(f"contracts root must be absolute: {candidate}")
    if candidate.is_dir():
      return candidate
  raise ValueError("cannot resolve contracts root")


def generated_root(root: Path) -> Path:
  if (root / "ext/eos/contracts.manifest.json").is_file():
    return root / "ext/eos"
  if (root / "contracts.manifest.json").is_file():
    return root
  raise ValueError(f"no generated EOS contracts under {root}")


def load_contracts(root: Path) -> dict:
  manifest_path = root / "contracts.manifest.json"
  manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
  groups = {"primitives": [], "bodyDevices": [], "resources": [], "acknowledgement": None}
  role_groups = {"primitive": "primitives", "body-device": "bodyDevices",
                 "world-resource": "resources"}
  for item in manifest["schemas"]:
    relative = Path(item["path"])
    path = root / relative
    schema = json.loads(path.read_text(encoding="utf-8"))
    entry = {"name": schema.get("properties", {}).get("type", {}).get("const")
             or schema.get("properties", {}).get("device_class", {}).get("const")
             or schema["$id"],
             "contract": relative.as_posix(), "schema": schema}
    if item["role"] == "primitive":
      entry["version"] = item["version"]
    if item["role"] == "acknowledgement":
      groups["acknowledgement"] = entry
    else:
      groups[role_groups[item["role"]]].append(entry)
  groups["primitives"].sort(key=lambda item: (item["name"], item["version"]))
  groups["bodyDevices"].sort(key=lambda item: item["name"])
  groups["resources"].sort(key=lambda item: item["name"])
  return groups


def render(root: Path) -> dict:
  descriptor, http_option, _ = descriptor_set(ROOT / "share/contracts")
  messages, enums = collect_types(descriptor)
  services = []
  for file in descriptor.file:
    if file.name not in SERVICE_FILES:
      continue
    for service in file.service:
      methods = []
      for rpc in service.method:
        binding = rpc.options.Extensions[http_option] if rpc.options.HasExtension(http_option) else None
        http = None
        if binding is not None:
          verb = binding.WhichOneof("pattern")
          if verb in {"get", "post", "put", "patch", "delete"}:
            http = {"verb": verb.upper(), "path": getattr(binding, verb)}
            if binding.body:
              http["body"] = binding.body
        methods.append({"name": rpc.name,
                        "requestType": rpc.input_type.lstrip(".").split(".")[-1],
                        "responseType": rpc.output_type.lstrip(".").split(".")[-1],
                        "requestSchema": ref(rpc.input_type),
                        "responseSchema": ref(rpc.output_type),
                        "serverStreaming": rpc.server_streaming,
                        "http": http})
      services.append({"fullName": file.package + "." + service.name, "methods": methods})
  order = {name: index for index, name in enumerate(SERVICE_FILES)}
  services.sort(key=lambda service: order[next(file.name for file in descriptor.file
                                                if any(file.package + "." + entry.name == service["fullName"]
                                                       for entry in file.service))])
  return {"schemaVersion": "5.0", "services": services,
          "types": render_types(messages, enums), "contracts": load_contracts(generated_root(root))}


def main() -> int:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--contracts", help="absolute share or generated EOS contract root")
  parser.add_argument("--compact", action="store_true")
  args = parser.parse_args()
  try:
    payload = render(contracts_root(args.contracts))
  except (ValueError, KeyError, OSError, json.JSONDecodeError) as error:
    print(f"print_contract_interfaces: {error}", file=sys.stderr)
    return 2
  print(json.dumps(payload, indent=None if args.compact else 2, ensure_ascii=False, sort_keys=True))
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
