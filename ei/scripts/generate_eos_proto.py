#!/usr/bin/env python3
"""Generate Python bindings from the unified repository's EOS contracts."""

from __future__ import annotations

import re
import shutil
import sys
import tempfile
from pathlib import Path, PurePosixPath

from grpc_tools import protoc

from island.share import repository_resources

EOS_CONTRACT_SOURCE = "share/eos/contracts"
PROTO_IMPORT = re.compile(r'^\s*import\s+"([^"]+)"\s*;', re.MULTILINE)


def _inputs(contract_root: Path) -> tuple[Path, ...]:
  """Return the transitive source closure of the three consumed EOS protos."""
  pending = [
    PurePosixPath("eos/gateway.proto"),
    PurePosixPath("eos/capabilities.proto"),
    PurePosixPath("eos/skills.proto"),
  ]
  found: set[PurePosixPath] = set()
  while pending:
    relative = pending.pop()
    if relative in found:
      continue
    source = contract_root / relative
    if not source.is_file():
      # Well-known and google API imports come from grpcio-tools/common protos.
      continue
    found.add(relative)
    pending.extend(
      PurePosixPath(imported)
      for imported in PROTO_IMPORT.findall(source.read_text(encoding="utf-8"))
    )
  return tuple(contract_root / item for item in sorted(found))


def main() -> int:
  root = Path(__file__).resolve().parents[1]
  resources = repository_resources(root)
  gateway = resources.require("eos/contracts/eos/gateway.proto")
  contract_root = gateway.parents[1]
  output = root / "src/island/generated"
  protos = _inputs(contract_root)
  if gateway not in protos:
    raise RuntimeError("EOS gateway contract is not in the generated source closure")
  try:
    import grpc_tools

    bundled = Path(grpc_tools.__file__).resolve().parent / "_proto"
  except (ImportError, TypeError) as exc:
    raise RuntimeError("grpcio-tools is required to generate EOS bindings") from exc

  with tempfile.TemporaryDirectory() as temporary_output:
    result = protoc.main(
      [
        "grpc_tools.protoc",
        f"-I{contract_root}",
        f"-I{bundled}",
        f"--python_out={temporary_output}",
        f"--pyi_out={temporary_output}",
        f"--grpc_python_out={temporary_output}",
        *(str(proto) for proto in protos),
      ]
    )
    if result != 0:
      return result

    generated = Path(temporary_output) / "eos"
    if not generated.is_dir():
      raise RuntimeError("protoc did not generate the EOS contract package")
    destination = output / "eos"
    if destination.exists():
      shutil.rmtree(destination)
    shutil.copytree(generated, destination)
    for path in (*destination.rglob("*.py"), *destination.rglob("*.pyi")):
      content = path.read_text(encoding="utf-8")
      content = content.replace("from eos.", "from island.generated.eos.")
      content = content.replace("from eos import", "from island.generated.eos import")
      content = content.replace("import eos.", "import island.generated.eos.")
      content = content.replace(
        "eos/gateway_pb2_grpc.py", "island/generated/eos/gateway_pb2_grpc.py"
      )
      content = content.replace(
        "eos/skills_pb2_grpc.py", "island/generated/eos/skills_pb2_grpc.py"
      )
      content = content.replace(
        "eos/capabilities_pb2_grpc.py", "island/generated/eos/capabilities_pb2_grpc.py"
      )
      content = f"# Generated from in-repository {EOS_CONTRACT_SOURCE}.\n" + content
      path.write_text(content, encoding="utf-8")
    for directory in (
      destination,
      *[path for path in destination.rglob("*") if path.is_dir()],
    ):
      (directory / "__init__.py").touch()
  return 0


if __name__ == "__main__":
  sys.exit(main())
