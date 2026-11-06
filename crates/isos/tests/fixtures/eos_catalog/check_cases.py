#!/usr/bin/env python3
"""Check the frozen owner catalog fixture before IsOS release unit tests."""

import hashlib
import json
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent
PROFILE = ROOT / "aether-profile"
CATALOG = ROOT / "catalog.json"
EXPECTED = "eda80c78e9b414249a8aca89756ae30b142ad03f3f61e7a792f9d438c0bd78d4"


def invoke(*args: str) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run([str(PROFILE), *args], capture_output=True, check=False)


def main() -> None:
    data = CATALOG.read_bytes()
    assert hashlib.sha256(data).hexdigest() == EXPECTED
    value = json.loads(data)
    assert any(item["id"] == "isos.process.component@v1.1" for item in value["types"])
    accepted = invoke("catalog")
    assert accepted.returncode == 0 and accepted.stdout == data
    assert invoke("validate").returncode != 0
    with tempfile.TemporaryDirectory() as home:
        sidecar = Path(home) / "node-types.json"
        sidecar.write_text(json.dumps({"node_types": [], "data_types": [],
                                       "adapter_families": [], "safety_adapters": []}))
        accepted = invoke("catalog", "--node-types", str(sidecar))
        assert accepted.returncode == 0 and accepted.stdout == data
        sidecar.write_text(json.dumps({"node_types": [{"id": "unverified"}]}))
        assert invoke("catalog", "--node-types", str(sidecar)).returncode != 0
        sidecar.unlink()
        assert invoke("catalog", "--node-types", str(sidecar)).returncode != 0
    print("EOS_CATALOG_TEST_FIXTURE_PASS")


if __name__ == "__main__":
    main()
