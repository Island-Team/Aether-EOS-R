"""Verify a retained selected-pin browser command and owner log boundary."""

import argparse
import hashlib
import json
from pathlib import Path

from verify_q01 import correlated_log_pages


def read(root: Path, name: str):
    return json.loads((root / name).read_text())


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(root: Path, eos: str, isos: str, egia: str) -> None:
    manifest = read(root, "evidence.sha256.json")
    actual = {path.relative_to(root).as_posix(): sha(path) for path in root.rglob("*")
              if path.is_file() and path != root / "evidence.sha256.json"}
    assert manifest == actual, "archive member missing, added, or changed"
    pins = read(root, "selected-inputs.json")
    assert (pins["eos"], pins["isos"], pins["egia"]) == (eos, isos, egia)
    eos_build = read(root, "q01-eos-build.json")
    isos_build = read(root, "q01-isos-build.json")
    assert eos_build["source_commit"] == eos_build["source_head"] == eos
    assert isos_build["source_commit"] == isos_build["source_head"] == isos
    assert eos_build["binary_sha256"]["aether-http-bridge"] == sha(
        root / "selected-binaries/aether-http-bridge")
    assert isos_build["binary_sha256"] == pins["isos_bin_sha256"] == sha(root / "bin/isos")
    ui_build = read(root, "q01-ui-rebuild.json")
    assert ui_build["source_commit"] == pins["ui"]
    assert ui_build["files_sha256"] == {
        path.relative_to(root / "ui-dist").as_posix(): sha(path)
        for path in (root / "ui-dist").rglob("*") if path.is_file()
    }
    assert pins["ui_index_sha256"] == sha(root / "ui-dist/index.html")

    ready = read(root, "browser-ready.json")
    dom = read(root, "browser-dom-observation.json")
    result = read(root, "browser-result.json")
    running = read(root, "status-running.json")
    assert dom["run_id"] == ready["run_id"] == running["run_id"]
    assert dom["active_revision"] == ready["revision"] == running["active_revision"]
    assert dom["connection_before_click"] == "running"
    assert dom["invoke_enabled_before_click"] is True
    assert dom["binding"] == "eos/dispatcher/base/base-velocity"
    assert dom["target"] == "eos/hardware/base/drive"
    assert dom["primitive"] == "base.velocity@1.0.0"
    assert dom["arguments_visible_before_click"] == {
        "x_m_per_s": 0.1, "y_m_per_s": 0, "yaw_rad_per_s": 0, "lease_ms": 250
    }
    command_id = dom["command_id"]
    assert dom["owner_status_sequence_observed"] in (
        ["Sending (browser)", "Completed"],
        ["Sending (browser)", "InProgress", "Completed"],
    )
    assert dom["owner_status_after_click"] == "Completed"
    after_stop = dom["after_stop_observation"]
    assert after_stop["connection"].startswith("Cannot reach IsOS at http://127.0.0.1:")
    assert after_stop["invoke_enabled"] is False
    assert (after_stop["prior_command_display"], after_stop["reason"]) in {
        ("Unavailable", "IsOS run identity unavailable."),
        ("Unavailable for this run", "Old deployment run or revision."),
    }
    assert result["http_status"] == 200 and result["ok"] is True
    assert result["request"] == {
        "op": "result", "command_id": command_id,
        "active_revision": dom["active_revision"],
    }
    assert result["result"] == {"command_id": command_id, "reason": None, "status": "Completed"}

    journal = [json.loads(path.read_text()) for path in (root / "journal").glob("*.rec")]
    matches = [entry for entry in journal if entry["body"].get("command_id") == command_id]
    assert len(matches) == 1
    entry = matches[0]
    record = entry["body"]
    body_bytes = json.dumps(record, separators=(",", ":"), ensure_ascii=False).encode()
    checksum = 0xcbf29ce484222325
    for byte in body_bytes:
        checksum = ((checksum ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    assert entry["checksum"] == checksum
    assert record["phase"] == "terminal" and record["result"]["status"] == "Completed"
    assert record["result"]["command_id"] == command_id
    assert record["node_path"] == dom["target"]
    admission = record["operation"]["identity"]["admission"]
    assert admission["run_id"] == dom["run_id"]
    assert admission["active_revision"] == dom["active_revision"]
    assert admission["binding_path"] == dom["binding"]
    assert admission["ingress"] == "private_bridge"
    assert admission["authority_role"] == "operator_control"
    assert record["operation"]["identity"]["target"] == {"hardware": {"node_path": dom["target"]}}
    assert json.loads(record["signature"])[2]["Base"]["BodyVelocity"] == {
        "x_m_s": 0.1, "y_m_s": 0.0, "yaw_rad_s": 0.0, "valid_for_ms": 250
    }

    pages = correlated_log_pages(root, "browser", command_id, dom["run_id"],
                                 dom["active_revision"])
    lines = [line for page in pages for line in page["lines"]]
    assert [(line["source"], line["event_code"], line["correlation_id"], line["fields"])
            for line in lines] == [
        ("bridge", "bridge_primitive_result", command_id,
         {"phase": "submit", "status": "InProgress"}),
        ("bridge", "bridge_primitive_result", command_id,
         {"phase": "followup", "status": "Completed"}),
    ]
    assert read(root, "q01-log-correlation-gaps.json") == []
    assert read(root, "reap-proof.json")["all_absent"] is True
    assert read(root, "stop-response.json")["terminal"]["outcome"] == "confirmed"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--eos-commit", required=True)
    parser.add_argument("--isos-commit", required=True)
    parser.add_argument("--egia-commit", required=True)
    args = parser.parse_args()
    verify(args.archive, args.eos_commit, args.isos_commit, args.egia_commit)
    print("Q01_BROWSER_BOUNDARY_PASS")
