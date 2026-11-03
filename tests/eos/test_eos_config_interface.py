#!/usr/bin/env python3
"""Contract checks for the side-effect-free EOS configuration executable."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile


def call(executable: str, command: str, values: dict | None = None) -> dict:
    request = "" if values is None else json.dumps({"schema_version": 1, "values": values})
    result = subprocess.run(
        [executable, command, "--json"], input=request, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    response = json.loads(result.stdout)
    assert response["schema_version"] == 1
    assert isinstance(response["diagnostics"], list)
    assert result.returncode == (0 if response["ok"] else 1), result.stderr
    return response


def main() -> None:
    executable = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="eos-config-contract-") as tmp:
        before = set(Path(tmp).iterdir())
        description = call(executable, "describe")["data"]
        assert description["component"] == "eos"
        assert description["version"]
        assert {x["name"] for x in description["provides"]} >= {
            "island.eos.v1.GatewayService", "island.eos.v1.SkillService",
            "island.eos.v2.CapabilityService",
        }
        defaults = call(executable, "defaults")["data"]
        assert defaults["grpc_port"] == 16200
        assert defaults["north_http_port"] == 16201
        assert defaults["binding_backend"] == "bindings/isos-demo.toml"
        assert defaults["binding_profile"] == "isos-demo"
        profiles = call(executable, "profiles")["data"]["profiles"]
        assert {profile["id"] for profile in profiles} == {"eos.native", "eos.uds"}
        native = next(p for p in profiles if p["id"] == "eos.native")
        uds = next(p for p in profiles if p["id"] == "eos.uds")
        assert uds["template"] == "endpoint"
        assert [field["name"] for field in native["fields"]] == [
            "host", "grpc_port", "grpc_uds_path", "grpc_uds_mode",
            "north_http_port", "binding_backend", "binding_profile",
        ]
        for field in native["fields"] + uds["fields"]:
            assert field["label"] and field["description"]
        assert native["fields"][1]["min"] == 1
        assert native["fields"][1]["max"] == 65535
        assert native["fields"][3]["default"] == defaults["grpc_uds_mode"]
        assert [field["name"] for field in uds["fields"]] == ["socket_path"]
        assert uds["fields"][0]["required"]
        assert uds["readiness"] == {"kind": "uds", "field": "socket_path"}
        values = {"grpc_uds_path": f"{tmp}/eos.sock"}
        assert call(executable, "validate", values)["data"]["valid"]
        files = call(executable, "render", values)["data"]["files"]
        assert {f["path"] for f in files} == {
            "eos/eos.toml", "ext/north-http/config.toml",
            "eos/bindings/isos-demo.toml",
        }
        assert "grpc_uds_path" in files[0]["content"]
        assert f"unix:{tmp}/eos.sock" in files[1]["content"]
        assert "[profile.isos-demo.robot]" in files[2]["content"]
        assert call(executable, "render", {"socket_path": f"{tmp}/eos.sock"})["data"]["files"] == files
        conflict = call(executable, "validate", {
            "socket_path": f"{tmp}/one.sock", "grpc_uds_path": f"{tmp}/two.sock",
        })
        assert not conflict["ok"]
        assert conflict["diagnostics"][0]["path"] == "values.socket_path"
        bad = call(executable, "validate", {"grpc_uds_path": "relative.sock"})
        assert not bad["ok"]
        assert bad["diagnostics"][0]["path"] == "values.grpc_uds_path"
        assert set(Path(tmp).iterdir()) == before


if __name__ == "__main__":
    main()
