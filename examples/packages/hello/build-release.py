#!/usr/bin/env python3
"""Build a local install-only example release candidate with no external SDK."""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parent
def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, help="SemVer x.y.z")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source-url", required=True, help="public GitHub repository that will host this example's source and release")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version):
        parser.error("--version must be x.y.z")
    segment = r"[A-Za-z0-9](?:[A-Za-z0-9_.-]*[A-Za-z0-9])?"
    if not re.fullmatch(fr"https://github\.com/{segment}/{segment}", args.source_url):
        parser.error("--source-url must be a GitHub repository URL without a trailing slash")
    arch = {"AMD64": "x86_64", "arm64": "aarch64"}.get(platform.machine(), platform.machine())
    if platform.system() != "Linux" or arch not in ("x86_64", "aarch64"):
        parser.error("example supports native Linux x86_64 or aarch64 only")
    args.output.mkdir(parents=True, exist_ok=True)
    name = f"island-hello-v{args.version}-linux-{arch}.tar.gz"
    archive = args.output / name
    with tempfile.TemporaryDirectory(prefix="island-hello-") as temporary:
        stage = Path(temporary)
        (stage / "bin").mkdir()
        shutil.copy2(ROOT / "hello.sh", stage / "bin/island-hello")
        (stage / "bin/island-hello").chmod(0o755)
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(stage / "bin", arcname="bin", recursive=False)
            tar.add(stage / "bin/island-hello", arcname="bin/island-hello")
    with archive.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    manifest = {
        "schema_version": 1,
        "id": "island/hello",
        "kind": "example",
        "version": args.version,
        "description": "Install-only package example that prints a greeting",
        "source_url": args.source_url,
        "release_url": f"{args.source_url}/releases/tag/v{args.version}",
        "license": "Apache-2.0",
        "assets": [{
            "name": name,
            "url": f"{args.source_url}/releases/download/v{args.version}/{name}",
            "sha256": digest,
            "size_bytes": archive.stat().st_size,
            "os": "linux",
            "arch": arch,
            "glibc_min": "2.17",
            "eos_contract": "",
            "required_libraries": [],
            "tested_ubuntu": [],
        }],
        "config_inputs": [],
    }
    draft = args.output / f"{name}.manifest.draft.json"
    draft.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Built {archive}\nSHA-256 {digest}\nDraft {draft}")
    print("Install-only example: it does not implement the EOS simulator contract.")
    print("Reviewer must verify that source and release assets are publicly accessible before approval.")


if __name__ == "__main__":
    main()
