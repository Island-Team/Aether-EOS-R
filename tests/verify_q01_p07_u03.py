#!/usr/bin/env python3
"""Archive-only semantic verifier for the selected Q01 P07/U03 case."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path
import tarfile

ISOS_SOURCES = {
    "ac984bcf998e9252cc4e5c222620dca3ae6d3c37": "8304ab894ed97f92c5fd201ec233c9455bcf98fb",
    "00c1d7aa184de8b969e6e40472bf9b1fa55032ff": "90d7a87c5c98dacbdcca0c2eacbdcead3fb4774b",
}
ISOS_LOCK = "915c47050861921da13290aa1f574c3de54d70f97b02603d2536c88264d3eb5d"
UI_PIN = "920995788d8e29dd6ab594d31278033bb791cfeb"
UI_TREE = "b93db23ec5ad24453417dc3a83d03f93dd069deb"
UI_LOCK = "922c376224fb6814252b074f07e93b9b0d701e19fbafe262b9881f59992ac4e9"
OVERLAYS = {
    "q01_p07_u03_mounted.test.tsx": "e909b15a83d836cabd39847628d09d472429b0f65889234a328eebb91e8a0e41",
    "q01_p07_workspace_stop.test.tsx": "6904741cefc56e03f33d071d3d8c3e16e146b3b751f2a6de4352cbce5eadcad4",
}
UI_TESTS = {
    "src/components/isos/IsosLogsPanel.integration.test.tsx",
    "src/components/isos/WorkspacePanel.test.tsx",
    "src/components/isos/IsosHome.selected-catalog.test.tsx",
    "src/components/isos/NodeGraphManager.selected-catalog.test.tsx",
    "src/components/isos/log-cursor-state.test.ts",
    "src/components/isos/workspace-layout.test.ts",
}
SEGMENT_HELPER = "336d364ef42161567fb2ce6985df384c5642c808b2c980f15f845a60a5ac834f"
SENTINEL = b"synthetic_grant_prompt_q01_keep_private"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(root: Path, name: str):
    return json.loads((root / name).read_text())


def object_id(kind: str, data: bytes) -> bytes:
    return hashlib.sha1(kind.encode() + b" " + str(len(data)).encode() + b"\0" + data).digest()


def archive_tree(path: Path) -> tuple[str, dict[str, bytes]]:
    contents: dict[str, bytes] = {}
    tree: dict = {}
    with tarfile.open(path) as archive:
        for member in archive:
            if member.isdir():
                continue
            parts = member.name.split("/")
            assert parts and all(part not in ("", ".", "..") for part in parts), member.name
            assert not member.name.startswith("/"), member.name
            if member.isfile():
                body = archive.extractfile(member).read()
                mode = b"100755" if member.mode & 0o111 else b"100644"
            elif member.issym():
                body = member.linkname.encode()
                mode = b"120000"
            else:
                raise AssertionError(("unsupported tar entry", member.name))
            contents[member.name] = body
            node = tree
            for part in parts[:-1]:
                node = node.setdefault(part, {})
            assert parts[-1] not in node, ("duplicate tar entry", member.name)
            node[parts[-1]] = (mode, object_id("blob", body))

    def hash_tree(node: dict) -> bytes:
        entries = []
        for name, value in node.items():
            directory = isinstance(value, dict)
            mode, oid = (b"40000", hash_tree(value)) if directory else value
            entries.append((name.encode() + (b"/" if directory else b""), mode, name.encode(), oid))
        raw = b"".join(mode + b" " + name + b"\0" + oid
                       for _, mode, name, oid in sorted(entries, key=lambda item: item[0]))
        return object_id("tree", raw)

    return hash_tree(tree).hex(), contents


def data(root: Path, name: str) -> dict:
    reply = read(root, name)
    assert reply["status"] == 200 and reply["body"]["ok"] is True, name
    return reply["body"]["data"]


def error(root: Path, name: str, status: int, code: str, path: str) -> None:
    reply = read(root, name)
    assert reply["status"] == status and reply["body"]["ok"] is False, name
    diagnostic = reply["body"]["data"]["diagnostics"][0]
    assert (diagnostic["code"], diagnostic["path"]) == (code, path), name
    if "cursor" in name or name in ("mismatch", "tampered-reply"):
        wire = json.dumps(reply)
        token = read(root, "tampered-token.json")
        assert token["original"] not in wire and token["tampered"] not in wire, name


def verify(root: Path, selected_pins: dict | None = None,
           selection: dict | None = None) -> None:
    manifest = read(root, "evidence.sha256.json")
    files = {p.relative_to(root).as_posix() for p in root.rglob("*")
             if p.is_file() and p != root / "evidence.sha256.json"}
    assert set(manifest) == files, ("archive coverage", set(manifest) ^ files)
    for name, expected in manifest.items():
        assert not Path(name).is_absolute() and ".." not in Path(name).parts
        assert sha(root / name) == expected, (name, "digest mismatch")

    pins = read(root, "selected-pins.json")
    assert set(pins["ui_test_sources"]) == UI_TESTS
    if selection is None:
        isos_pin = pins["isos_source"]
        assert isos_pin in ISOS_SOURCES
        isos_tree = ISOS_SOURCES[isos_pin]
        isos_lock = ISOS_LOCK
        ui_pin, ui_tree, ui_lock = UI_PIN, UI_TREE, UI_LOCK
        overlays, segment_helper = OVERLAYS, SEGMENT_HELPER
    else:
        from q01_selection import canonical_bytes
        isos_owner, ui_owner = selection["owners"]["isos"], selection["owners"]["ui"]
        isos_pin, isos_tree = isos_owner["commit"], isos_owner["tree"]
        isos_lock = isos_owner["locks"]["Cargo.lock"]
        ui_pin, ui_tree = ui_owner["commit"], ui_owner["tree"]
        ui_lock = ui_owner["locks"]["package-lock.json"]
        overlays = {name: selection["harness"][f"tests/{name}"]
                    for name in OVERLAYS}
        segment_helper = selection["harness"]["tests/q01_p07_segment_id.rs"]
        assert pins["selection_sha256"] == hashlib.sha256(canonical_bytes(selection)).hexdigest()
        assert pins["ui_test_sources"] == {
            name: ui_owner["sources"][name] for name in pins["ui_test_sources"]}
    assert (pins["isos_source"], pins["isos_tree"], pins["isos_lock_sha256"]) == (isos_pin, isos_tree, isos_lock)
    assert (pins["ui_source"], pins["ui_tree"], pins["ui_lock_sha256"]) == (ui_pin, ui_tree, ui_lock)
    if selected_pins is not None:
        assert selected_pins["isos"] == isos_pin and selected_pins["ui"] == ui_pin
    for key, archive_name, tree_oid in (
        ("isos", "isos-selected-source.tar", isos_tree),
        ("ui", "ui-selected-source.tar", ui_tree),
    ):
        assert pins[f"{key}_archive_sha256"] == sha(root / archive_name)
        actual_tree, contents = archive_tree(root / archive_name)
        assert actual_tree == tree_oid, (key, "selected source tree")
        if key == "isos":
            assert contents["Cargo.lock"] == (root / "isos-Cargo.lock").read_bytes()
        else:
            assert contents["package-lock.json"] == (root / "ui-package-lock.json").read_bytes()
            for name, digest in pins["ui_test_sources"].items():
                assert hashlib.sha256(contents[name]).hexdigest() == digest
                assert digest == sha(root / "ui-source" / name)
    assert sha(root / "bin/isos") == pins["isos_binary_sha256"]
    receipt = read(root, "isos-build-receipt.json")
    assert receipt["source_commit"] == isos_pin and receipt["source_tree"] == isos_tree
    assert receipt["source_archive_sha256"] == pins["isos_archive_sha256"]
    assert receipt["cargo_lock_sha256"] == isos_lock
    assert Path(receipt["working_directory"]).is_absolute()
    assert (Path(receipt["command"][0]).name, receipt["command"][1:]) == \
        ("cargo", ["build", "--locked", "--bin", "isos"])
    assert receipt["cargo_version"].startswith("cargo ") and receipt["rustc_version"].startswith("rustc ")
    assert receipt["output_binary"] == "bin/isos" and receipt["output_binary_sha256"] == sha(root / "bin/isos")
    assert receipt["exit"] == 0
    assert "cargo build --locked --bin isos" in (root / "isos-build.log").read_text()
    assert "Finished" in (root / "isos-build.log").read_text() and "exit=0" in (root / "isos-build.log").read_text()
    assert sha(root / "source-segment-id.rs") == pins["segment_helper_source_sha256"] == segment_helper
    assert sha(root / "bin/segment-id") == pins["segment_helper_binary_sha256"]
    assert "rustc" in (root / "segment-id-build.log").read_text() and "exit=0" in (root / "segment-id-build.log").read_text()
    assert pins["ui_overlays_sha256"] == overlays
    for name, expected in overlays.items():
        assert sha(root / f"ui-{name}") == expected
    ui_receipt = read(root, "ui-build-receipt.json")
    assert ui_receipt["source_commit"] == ui_pin and ui_receipt["source_tree"] == ui_tree
    assert ui_receipt["source_archive_sha256"] == pins["ui_archive_sha256"]
    assert ui_receipt["package_lock_sha256"] == ui_lock
    ui_root = Path(pins["ui_working_directory"])
    assert ui_root.is_absolute()
    assert pins["ui_working_directory"] == str(ui_root)
    assert ui_receipt["working_directory"] == str(ui_root)
    assert ui_receipt["command"] == ["npm", "run", "build"] and ui_receipt["exit"] == 0
    assert ui_receipt["node_version"] == pins["environment"]["node"]
    assert ui_receipt["npm_version"] == pins["environment"]["npm"]
    assert ui_receipt["bundles"] == pins["ui_bundles"]
    for name, digest in pins["ui_bundles"].items():
        assert sha(root / "ui-dist" / name.removeprefix("dist/")) == digest
    for name, tokens in (
        ("ui-mounted-tests.log", (
            "pins a manager package failure outside source filters",
            "keeps the opaque same-run cursor while collapsed and rechecks status on reopen",
            "keeps retained rows on rotation, shows gap, then clears them on a new run",
            "retains rows through collector loss, backs off for 30 seconds, then resumes",
            "keeps a collapsed panel mounted, restores focus controls and supports keyboard resize",
            "keeps Stop callable while a Run request is pending",
            "leaves Stop enabled during a pending graph validation",
            "Test Files", "passed", "exit=0")),
        ("ui-q01-mounted-overlay.log", ("Q01 mounted logs preserve same-scope cursor and rows through outage and show an authenticated expiry gap", "1 passed", "exit=0")),
        ("ui-q01-workspace-stop.log", ("Q01 mounted workspace keeps owner Stop reachable when Logs is closed", "1 passed", "exit=0")),
        ("ui-build.log", ("typecheck", "build:local", "build:hosted", "Both IsosUI bundles and manifests are valid", "exit=0")),
    ):
        output = (root / name).read_text()
        assert all(token in output for token in tokens), name

    before = (root / "saved-before.raw.json").read_bytes()
    assert before == (root / "saved-after.raw.json").read_bytes(), "rejected Apply changed saved bytes"
    candidate = read(root, "invalid-candidate.json")
    assert candidate == {**json.loads(before), "version": "2.0.0"}
    error(root, "invalid-apply.json", 422, "unsupported_graph_version", "/version")
    initial_status = data(root, "status-before.json")
    final_status = data(root, "status-after.json")
    for status in (initial_status, final_status, data(root, "restart-status.json")):
        assert status["state"] == "stopped" and not status["processes"]
        assert status["run_id"] is None and status["run_id_state"] == "none"
        assert status["saved_revision"] == hashlib.sha256(before).hexdigest()
        assert status["active_revision"] is None

    pending = data(root, "pending.json")
    found = data(root, "found.json")
    filtered = data(root, "filtered.json")
    expired = data(root, "expired.json")
    scope = found["scope"]
    assert scope["kind"] == "manager_session" and scope["run_id"].startswith("m")
    assert pending["scope"] == scope and pending["anchor"] == {"kind": "first_prelaunch_failure", "state": "pending", "event": None}
    anchor = found["anchor"]
    assert anchor["kind"] == "first_prelaunch_failure" and anchor["state"] == "found"
    event = anchor["event"]
    assert event["event_code"] == "deployment_preflight_failed" and event["source"] == "isos"
    assert event["run_id"] == scope["run_id"] and event["fields"] == {"diagnostic_code": "unsupported_graph_version", "diagnostic_path": "/version"}
    assert filtered["scope"] == expired["scope"] == scope
    assert filtered["anchor"] == expired["anchor"] == anchor
    assert filtered["lines"] == []
    assert [d["code"] for d in expired["diagnostics"]] == ["log_cursor_expired"]
    assert expired["rotated"] is True
    token = found["next_cursor"]
    assert token == read(root, "tampered-token.json")["original"]
    assert token.startswith("lc1.") and len(token) <= 512
    body, mac = token.split(".")[1:]
    cursor = json.loads(base64.urlsafe_b64decode(body + "=" * (-len(body) % 4)))
    assert cursor == read(root, "decoded-cursor.json")
    assert cursor["d"] == "default" and cursor["k"] == "manager_session" and cursor["r"] == scope["run_id"]
    assert cursor["p"] is False and isinstance(cursor["o"], int) and cursor["o"] > 0
    changed = read(root, "tampered-token.json")["tampered"]
    assert changed.split(".")[:2] == ["lc1", body]
    first = base64.urlsafe_b64decode(mac + "=" * (-len(mac) % 4))
    altered = base64.urlsafe_b64decode(changed.split(".")[2] + "=" * (-len(changed.split(".")[2]) % 4))
    assert len(first) == len(altered) == 32 and first[0] ^ altered[0] == 1 and first[1:] == altered[1:]
    error(root, "mismatch.json", 400, "log_cursor_filter_mismatch", "/query/cursor")
    error(root, "tampered-reply.json", 400, "log_cursor_invalid", "/query/cursor")

    snapshots = read(root, "rotation-snapshots.json")
    assert len(snapshots) >= 4
    for index, state in enumerate(snapshots):
        assert "events.log" in state
        for name, meta in state.items():
            path = root / meta["file"]
            assert path == root / "segments" / f"snapshot-{index}" / name
            assert path.stat().st_size == meta["size"] and sha(path) == meta["sha256"]
            assert meta["segment_id"] == f"{meta['device']:x}-{meta['inode']:x}-{meta['birth_ns']:x}"
            assert meta["birth_ns"] > 0
            assert SENTINEL not in path.read_bytes()
            for line in path.read_bytes().splitlines():
                json.loads(line)
        if index:
            assert state["events.log"]["segment_id"] != snapshots[index - 1]["events.log"]["segment_id"]
            assert state["events.log.1"]["segment_id"] == snapshots[index - 1]["events.log"]["segment_id"]
    found_state = read(root, "cursor-state-found.raw.json")
    pruned_state = read(root, "cursor-state-pruned.raw.json")
    assert found_state["schema_version"] == pruned_state["schema_version"] == 1
    assert found_state["deployment"] == pruned_state["deployment"] == "default"
    assert found_state["manager_session"] == pruned_state["manager_session"] == scope["run_id"]
    assert found_state["never_run"] is pruned_state["never_run"] is True
    assert found_state["last_run_id"] is pruned_state["last_run_id"] is None
    assert found_state["run_anchor"] is pruned_state["run_anchor"] is None
    assert found_state["manager_anchor"] == pruned_state["manager_anchor"]
    anchor_state = found_state["manager_anchor"]
    assert anchor_state["event"] == event
    assert anchor_state["segment"] == cursor["s"] == snapshots[0]["events.log"]["segment_id"]
    assert anchor_state["after_offset"] == cursor["o"]
    assert anchor_state["seal"] and found_state["scope_seal"] and pruned_state["scope_seal"]
    first_events = [json.loads(line) for line in (root / snapshots[0]["events.log"]["file"]).read_bytes().splitlines()]
    assert event in first_events, "anchor does not match accepted first event"
    assert any(e["event_code"] == "deployment_preflight_failed" for e in first_events)
    original = (root / snapshots[0]["events.log"]["file"]).read_bytes()
    assert json.loads(original[:cursor["o"]].splitlines()[-1]) == event
    assert cursor["s"] in {meta["segment_id"] for meta in snapshots[1].values()}
    assert cursor["s"] not in {meta["segment_id"] for meta in snapshots[-1].values()}, "cursor segment still retained"
    assert anchor_state["segment"] not in {meta["segment_id"] for meta in snapshots[-1].values()}, "anchor segment still retained"
    for meta in snapshots[-1].values():
        assert b"deployment_preflight_failed" not in (root / meta["file"]).read_bytes(), "anchor still retained"
    assert snapshots[-1]["events.log"]["segment_id"] != snapshots[0]["events.log"]["segment_id"]
    for name in ("pending", "found", "filtered", "expired", "restart-fresh", "restart-old-cursor"):
        assert SENTINEL not in (root / f"{name}.json").read_bytes()

    restart = data(root, "restart-fresh.json")
    old = data(root, "restart-old-cursor.json")
    restarted_state = read(root, "cursor-state-restarted.raw.json")
    assert restarted_state["manager_session"] == restart["scope"]["run_id"]
    assert restarted_state["manager_anchor"] is None and restarted_state["last_run_id"] is None
    assert restarted_state["scope_seal"]
    assert restart["scope"]["kind"] == "manager_session"
    assert restart["scope"]["run_id"] != scope["run_id"]
    assert restart["anchor"] == {"kind": "first_prelaunch_failure", "state": "pending", "event": None}
    assert old["scope"] == restart["scope"] and old["anchor"] == restart["anchor"]
    assert old["lines"] == [] and [d["code"] for d in old["diagnostics"]] == ["log_run_changed"]
    for session in (1, 2):
        assert read(root, f"manager-exit-{session}.json")["exit"] == 0
    print("Q01_P07_U03_VERIFY_PASS", root)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    args = parser.parse_args()
    verify(args.archive.resolve())


if __name__ == "__main__":
    main()
