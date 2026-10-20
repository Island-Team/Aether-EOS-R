"""Stable protobuf declaration fingerprint, independent of source file layout."""

from __future__ import annotations

import hashlib
import subprocess
import tempfile
from pathlib import Path

from google.protobuf import descriptor_pb2


SERVICES = ("gateway", "skills", "workflow")


def wire_surface(proto_root: Path, prefix: str = "eos") -> dict[str, dict[str, str]]:
  with tempfile.TemporaryDirectory(prefix="eos-wire-") as directory:
    output = Path(directory) / "services.pb"
    subprocess.run(
      ["protoc", f"-I{proto_root}", f"--descriptor_set_out={output}",
       "--include_imports", *(f"{prefix}/{name}.proto" for name in SERVICES)],
      cwd=proto_root, check=True, capture_output=True, text=True,
    )
    descriptor = descriptor_pb2.FileDescriptorSet.FromString(output.read_bytes())

  surface: dict[str, dict[str, str]] = {"messages": {}, "enums": {}, "services": {}}
  for file in descriptor.file:
    if file.package != "island.eos.v1":
      continue
    for key, declarations in (("messages", file.message_type),
                              ("enums", file.enum_type),
                              ("services", file.service)):
      for declaration in declarations:
        name = f"{file.package}.{declaration.name}"
        assert name not in surface[key], name
        surface[key][name] = hashlib.sha256(
          declaration.SerializeToString(deterministic=True)
        ).hexdigest()
  return {key: dict(sorted(values.items())) for key, values in surface.items()}
