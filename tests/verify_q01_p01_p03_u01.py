#!/usr/bin/env python3
"""Offline semantic verifier for the selected-pin P01/P03/U01 Q01 archive."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import shutil
import stat
import subprocess
import tarfile
import tempfile
from pathlib import Path

COMMON_PINS = {"mujoco": "4e6b5449ccd25be53ad95b13baa2c89f7ed91e10",
               "ui": "920995788d8e29dd6ab594d31278033bb791cfeb"}
COMMON_TREES = {"mujoco": "0df44f54ffdbb8fac8e4b694b23cb19333f76241",
                "ui": "b93db23ec5ad24453417dc3a83d03f93dd069deb"}
KNOWN_ISOS = {
    "ac984bcf998e9252cc4e5c222620dca3ae6d3c37": "8304ab894ed97f92c5fd201ec233c9455bcf98fb",
    "00c1d7aa184de8b969e6e40472bf9b1fa55032ff": "90d7a87c5c98dacbdcca0c2eacbdcead3fb4774b",
}
PROCESS = "/cores/eos/layers/process/groups/mujoco/nodes/simulator"
CAMERA = "/cores/eos/layers/hardware/groups/camera/nodes/front"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read(root: Path, name: str) -> bytes:
    path = root / name
    assert path.is_file() and not path.is_symlink(), name
    return path.read_bytes()


def obj(root: Path, name: str):
    return json.loads(read(root, name))


def diagnostic(response, expected):
    items = response["body"]["data"]["diagnostics"]
    assert any((item.get("code"), item.get("path"), item.get("stage")) == expected
               for item in items), (expected, items)


def package_tree(root: Path) -> str:
    h = hashlib.sha256(b"isos-package-tree-v1\0")
    files = sorted(path for path in root.rglob("*") if path.name not in
                   {"receipt.json", "release.json", "node-types.json"})
    for path in files:
        assert not path.is_symlink()
        relative = path.relative_to(root).as_posix().encode()
        h.update(len(relative).to_bytes(4, "little"))
        h.update(relative)
        h.update(b"D" if path.is_dir() else b"F")
        h.update(b"1" if path.is_file() and path.stat().st_mode & 0o111 else b"0")
        data = path.read_bytes() if path.is_file() else b""
        h.update(len(data).to_bytes(8, "little"))
        h.update(hashlib.sha256(data).digest() if path.is_file() else b"\0" * 32)
    return h.hexdigest()


def inventory(root: Path) -> dict:
    result = {}
    for path in sorted(root.rglob("*")):
        assert not path.is_symlink()
        name = path.relative_to(root).as_posix()
        result[name] = {"kind": "file" if path.is_file() else "dir",
                        "mode": stat.S_IMODE(path.stat().st_mode),
                        "sha256": digest(path.read_bytes()) if path.is_file() else None}
    return result


def source_snapshot(root: Path, owner: str, pins: dict, trees: dict) -> str:
    source = f"sources/{owner}/"
    identity = obj(root, source + "identity.json")
    raw = read(root, source + "commit.raw")
    archive = read(root, source + "tracked.tar")
    assert identity["archive_sha256"] == digest(archive)
    assert identity["commit_sha256"] == digest(raw)
    git_oid = hashlib.sha1(b"commit " + str(len(raw)).encode() + b"\0" + raw).hexdigest()
    assert git_oid == identity["commit"] == pins[owner]
    assert raw.splitlines()[0] == f"tree {identity['tree']}".encode()
    with tempfile.TemporaryDirectory(prefix="q01-owner-source-") as temp_name:
        temp = Path(temp_name)
        with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as tar:
            for member in tar.getmembers():
                assert not member.name.startswith("/") and ".." not in Path(member.name).parts
            tar.extractall(temp, filter="data")
        subprocess.run(["git", "init", "-q", temp], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["git", "add", "-A"], cwd=temp, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        actual = subprocess.check_output(["git", "write-tree"], cwd=temp, text=True).strip()
        if owner == "isos":
            assert (temp / "Cargo.lock").read_bytes() == read(root, "isos-Cargo.lock")
        if owner == "ui":
            assert (temp / "package-lock.json").read_bytes() == read(root, "ui-package-lock.json")
    assert actual == identity["tree"] == trees[owner], owner
    return actual


def verify(root: Path, selected_pins: dict | None = None,
           selection: dict | None = None) -> None:
    seal = obj(root, "evidence.sha256.json")
    found = {path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file()
             and path != root / "evidence.sha256.json"}
    assert found == set(seal), (sorted(found - set(seal)), sorted(set(seal) - found))
    for name, expected in seal.items():
        assert digest(read(root, name)) == expected, name
    selected = obj(root, "selected-pins.json")
    pins = selected["pins"]
    if selected_pins is not None:
        assert all(selected_pins[owner] == commit for owner, commit in pins.items()), \
            "P01/P03/U01 case does not match the selected owner pins"
    if selection is None:
        assert pins["isos"] in KNOWN_ISOS
        expected_pins = {"isos": pins["isos"], **COMMON_PINS}
        trees = {"isos": KNOWN_ISOS[pins["isos"]], **COMMON_TREES}
        isos_lock = selected["isos_lock_sha256"]
        ui_lock = selected["ui_lock_sha256"]
    else:
        from q01_selection import canonical_bytes
        expected_pins = {name: selection["owners"][name]["commit"]
                         for name in ("isos", "mujoco", "ui")}
        trees = {name: selection["owners"][name]["tree"]
                 for name in expected_pins}
        isos_lock = selection["owners"]["isos"]["locks"]["Cargo.lock"]
        ui_lock = selection["owners"]["ui"]["locks"]["package-lock.json"]
        assert selected["selection_sha256"] == digest(canonical_bytes(selection))
        assert selected["release_sha256"] == selection["package"]["release_sha256"]
        assert selected["asset_sha256"] == selection["package"]["asset_sha256"]
    assert pins == expected_pins
    assert selected["source_trees"] == trees
    for owner in pins:
        assert source_snapshot(root, owner, pins, trees) == selected["source_trees"][owner]
        fetch_log = read(root, f"{owner}-remote-fetch.log").decode()
        if selection is None:
            assert "fetch origin master" in fetch_log and "fatal:" not in fetch_log
        else:
            selected_owner = selection["owners"][owner]
            assert ("git ls-remote " + selected_owner["repository"] + " " +
                    selected_owner["ref"]) in fetch_log
            assert selected_owner["commit"] + "\t" + selected_owner["ref"] in fetch_log
    assert selected["ui_source_commit"] == pins["ui"]
    assert selected["isos_lock_sha256"] == digest(read(root, "isos-Cargo.lock")) == isos_lock
    assert selected["ui_lock_sha256"] == digest(read(root, "ui-package-lock.json")) == ui_lock
    assert selected["isos_binary_sha256"] == digest(read(root, "bin/isos"))
    assert selected["release_sha256"] == digest(read(root, "package-input/release.json"))
    assert selected["asset_sha256"] == digest(read(root, "package-input/asset.tar.gz"))
    assert selected["installed_receipt_sha256"] == digest(read(root, "package-installed/receipt.json"))
    release = obj(root, "package-input/release.json")
    receipt = obj(root, "package-installed/receipt.json")
    assert release["source_commit"] == pins["mujoco"] and release["version"] == "0.1.1"
    assert receipt["schema_version"] == 2 and receipt["id"] == release["id"]
    assert receipt["version"] == release["version"] and receipt["source_kind"] == "local_candidate"
    assert receipt["asset_sha256"] == selected["asset_sha256"]
    assert receipt["release_sha256"] == digest(read(root, "package-installed/release.json"))
    assert receipt["sidecar_sha256"] == digest(read(root, "package-installed/node-types.json"))
    assert receipt["payload_sha256"] == package_tree(root / "package-installed")
    assert any(row["sha256"] == selected["asset_sha256"] for row in release["assets"])
    assert "$ " in read(root, "isos-build.log").decode() and "Finished" in read(root, "isos-build.log").decode()
    assert "Compiling isos " in read(root, "isos-build.log").decode()
    assert "$ " in read(root, "isos-clean.log").decode()
    assert "$ " in read(root, "install.log").decode()
    assert obj(root, "preview.json")["body"]["ok"] is True
    baseline = obj(root, "initial-graph.json")
    assert baseline["revision"] and len(baseline["revision"]) == 64
    original_inventory = inventory(root / "package-installed")
    expected_cases = {"payload-tamper": ("package_changed", PROCESS + "/package", "payload"),
                      "receipt-tamper": ("package_changed", PROCESS + "/package", "payload"),
                      "ghost-scene": ("mujoco_scene_unavailable", PROCESS + "/settings/scene", "admission"),
                      "ghost-device": ("mujoco_device_unavailable", CAMERA + "/adapter/settings/device_id", "admission")}
    for label, expected in expected_cases.items():
        case = label + "/"
        before = read(root, case + "saved-before.raw.json")
        after = read(root, case + "saved-after.raw.json")
        assert before == after and digest(before) == baseline["revision"], label
        graph_before, graph_after = obj(root, case + "graph-before.json"), obj(root, case + "graph-after.json")
        assert graph_before == graph_after and graph_before["revision"] == digest(before)
        assert obj(root, case + "status-before.json") == obj(root, case + "status-after.json")
        assert not obj(root, case + "status-after.json")["processes"]
        validation, applied = obj(root, case + "validate.json"), obj(root, case + "apply.json")
        assert validation["status"] == 200 and validation["body"]["data"]["valid"] is False
        assert applied["status"] == 422 and applied["body"]["ok"] is False
        diagnostic(validation, expected)
        diagnostic(applied, expected)
        original = read(root, case + "payload-before.raw")
        changed = read(root, case + "payload-after.raw")
        receipt_before = read(root, case + "receipt-before.raw.json")
        receipt_after = read(root, case + "receipt-after.raw.json")
        before_tree = root / case / "installed-before"
        after_tree = root / case / "installed-after"
        before_inventory = inventory(before_tree)
        after_inventory = inventory(after_tree)
        assert before_inventory == original_inventory, label
        assert set(before_inventory) == set(after_inventory), label
        assert read(root, case + "installed-before/receipt.json") == receipt_before
        assert read(root, case + "installed-after/receipt.json") == receipt_after
        assert read(root, case + "installed-before/LICENSE") == original
        assert read(root, case + "installed-after/LICENSE") == changed
        assert original == read(root, "package-installed/LICENSE")
        assert receipt_before == read(root, "package-installed/receipt.json")
        if label == "payload-tamper":
            assert len(original) == len(changed) and sum(a != b for a, b in zip(original, changed)) == 1
            assert receipt_before == receipt_after
            assert {name for name in before_inventory if before_inventory[name] != after_inventory[name]} == {"LICENSE"}
            assert package_tree(after_tree) != receipt["payload_sha256"]
        elif label == "receipt-tamper":
            assert original == changed
            assert {name for name in before_inventory if before_inventory[name] != after_inventory[name]} == {"receipt.json"}
            assert package_tree(after_tree) == receipt["payload_sha256"]
            parsed_before, parsed_after = json.loads(receipt_before), json.loads(receipt_after)
            assert set(parsed_before) == set(parsed_after)
            assert {key for key in parsed_before if parsed_before[key] != parsed_after[key]} == {"payload_sha256"}
            assert sum(a != b for a, b in zip(parsed_before["payload_sha256"], parsed_after["payload_sha256"])) == 1
        else:
            assert original == changed and receipt_before == receipt_after
            assert before_inventory == after_inventory
            node, setting, selected_id = (("eos/process/mujoco/simulator", "scene", "ghost-scene") if label == "ghost-scene" else
                                           ("eos/hardware/camera/front", "adapter.settings.device_id", "ghost-device"))
            options = obj(root, case + "options.json")
            assert options["status"] == 200 and options["body"]["ok"]
            groups = [row for row in options["body"]["data"]["setting_options"]
                      if (row["node_path"], row["setting"]) == (node, setting)]
            assert len(groups) == 1 and groups[0]["selected"] == selected_id
            rows = [row for row in groups[0]["options"] if row["value"] == selected_id]
            assert len(rows) == 1 and rows[0]["available"] is False
            assert isinstance(rows[0]["reason_code"], str) and rows[0]["reason_code"]
            assert isinstance(rows[0]["reason"], str) and rows[0]["reason"]
            candidate = obj(root, case + "candidate.json")
            selected_value = (candidate["cores"]["eos"]["layers"]["process"]["groups"]["mujoco"]["nodes"]["simulator"]["settings"]["scene"] if label == "ghost-scene" else
                              candidate["cores"]["eos"]["layers"]["hardware"]["groups"]["camera"]["nodes"]["front"]["adapter"]["settings"]["device_id"])
            assert selected_value == selected_id
    valid = "stale-revision/"
    before = read(root, valid + "saved-before.raw.json")
    after = read(root, valid + "saved-after.raw.json")
    old, new = digest(before), digest(after)
    assert old == baseline["revision"] and new != old
    assert obj(root, valid + "graph-before.json")["revision"] == old
    assert obj(root, valid + "graph-after.json")["revision"] == new
    assert obj(root, valid + "validate.json")["body"]["data"]["valid"] is True
    assert obj(root, valid + "apply.json")["status"] == 200
    assert obj(root, valid + "stale-apply.json")["status"] == 409
    status_before = obj(root, valid + "status-before.json")
    status_after = obj(root, valid + "status-after.json")
    assert status_before["saved_revision"] == old and status_after["saved_revision"] == new
    for key in ("active_revision", "run_id", "processes", "state"):
        assert status_before[key] == status_after[key], key
    ui = obj(root, "visible-editor.json")
    build = obj(root, "ui-build-receipt.json")
    assert build["source_commit"] == pins["ui"]
    assert build["source_tree"] == trees["ui"]
    assert build["proxy_harness_sha256"] == digest(read(root, "ui-proxy-harness.py"))
    if selection is not None:
        assert build["proxy_harness_sha256"] == \
            selection["harness"]["tests/q01_p01_p03_u01_replay.py"]
    assert build["files_sha256"] == {path.relative_to(root / "ui-dist").as_posix(): digest(path.read_bytes())
                                    for path in (root / "ui-dist").rglob("*") if path.is_file()}
    assert build["isos_api_origin"] == obj(root, "ui-served.json")["api_origin"]
    assert obj(root, "ui-served.json")["index_sha256"] == digest(read(root, "ui-dist/index.html"))
    assert obj(root, "ui-served.json")["served_sha256"] == build["files_sha256"]
    assert any(build["isos_api_origin"].encode() in path.read_bytes()
               for path in (root / "ui-dist/assets").glob("*.js"))
    assert obj(root, "ui-proxy-owner-options.json") == obj(root, "ui-owner-options.json")
    assert obj(root, "ui-served.json")["proxy_api_options_sha256"] == digest(read(root, "ui-proxy-owner-options.json"))
    ui_graph = obj(root, "ui-owner-graph.json")
    assert ui_graph["revision"] == digest(read(root, "ui-home-graph.raw.json"))
    assert ui_graph["graph"]["cores"]["eos"]["layers"]["hardware"]["groups"]["camera"]["nodes"]["front"]["adapter"]["settings"]["device_id"] == "ghost-device"
    consumed = obj(root, "ui-consumed-options.json")
    index = consumed["post_index"]
    prefix = f"ui-post-options/{index:06d}"
    post_meta = obj(root, prefix + "-meta.json")
    post_request = read(root, prefix + "-request.raw.json")
    post_response = read(root, prefix + "-response.raw.json")
    assert post_meta["method"] == "POST" and post_meta["path"] == "/api/isos/v1/node-options"
    assert post_meta["status"] == 200
    assert post_meta["request_sha256"] == consumed["request_sha256"] == digest(post_request)
    assert post_meta["response_sha256"] == consumed["response_sha256"] == digest(post_response)
    assert json.loads(post_request)["graph"] == ui_graph["graph"]
    assert consumed["graph_sha256"] == digest(read(root, "ui-home-graph.raw.json"))
    device_options = json.loads(post_response)["data"]["setting_options"]
    matching_rows = [row for row in device_options if (row["node_path"], row["setting"]) ==
                     ("eos/hardware/camera/front", "adapter.settings.device_id")]
    assert len(matching_rows) == 1
    row = matching_rows[0]
    matching_choices = [choice for choice in row["options"] if choice["value"] == "ghost-device"]
    assert len(matching_choices) == 1
    chosen = matching_choices[0]
    ax = ui["ax_state"]
    assert ui["selected_id"] == row["selected"] == "ghost-device"
    assert chosen["available"] is False
    assert chosen["reason_code"] == "mujoco_device_unavailable"
    assert isinstance(chosen["reason"], str) and chosen["reason"]
    assert f"alert: {chosen['reason_code']} {chosen['reason']}" in ax
    assert 'button "Save and Run" [disabled]' in ax
    assert 'option "ghost-device (ghost-device) · Unavailable" [disabled] [selected]' in ax
    assert 'alert: mujoco_device_unavailable Select a verified available value' in ax
    assert ui["apply_blocked"] is True
    assert ui["owner_options_sha256"] == digest(read(root, "ui-owner-options.json"))
    assert ui["served_index_sha256"] == digest(read(root, "ui-dist/index.html"))
    assert obj(root, "case-result.json")["status"] == "passed"
    print("Q01_P01_P03_U01_VERIFY_PASS", root)


def tamper_suite(root: Path, selection: dict | None = None) -> None:
    with tempfile.TemporaryDirectory(prefix="q01-p01-p03-u01-tamper-") as temp_name:
        copy = Path(temp_name) / "archive"
        shutil.copytree(root, copy)
        seal_path = copy / "evidence.sha256.json"
        original_seal = seal_path.read_bytes()
        cases = (
            ("unchanged-installed-member", "payload-tamper/installed-after/assets/MODELS.md"),
            ("ui-post-response-reason", "ui-post-options"),
            ("ui-post-response-availability", "ui-post-options"),
            ("ui-post-request-graph", "ui-post-options"),
            ("ui-lockfile-source-mismatch", "ui-package-lock.json"),
            ("ghost-scene-diagnostic", "ghost-scene/apply.json"),
            ("stale-revision", "stale-revision/stale-apply.json"),
        )
        for label, target in cases:
            changes = {}
            if label == "unchanged-installed-member":
                path = copy / target
                data = bytearray(path.read_bytes())
                assert data
                data[0] ^= 1
                changes[target] = bytes(data)
            elif label.startswith("ui-post-"):
                index = obj(copy, "ui-consumed-options.json")["post_index"]
                base = f"ui-post-options/{index:06d}"
                is_response = label in ("ui-post-response-reason", "ui-post-response-availability")
                name = base + ("-response.raw.json" if is_response else "-request.raw.json")
                data = obj(copy, name)
                if is_response:
                    row = next(row for row in data["data"]["setting_options"]
                               if (row["node_path"], row["setting"]) ==
                               ("eos/hardware/camera/front", "adapter.settings.device_id"))
                    chosen = next(item for item in row["options"] if item["value"] == "ghost-device")
                    if label.endswith("reason"):
                        chosen["reason"] = "Different verified reason"
                    else:
                        chosen["available"] = True
                else:
                    data["graph"]["robot"]["name"] = "wrong-selected-draft"
                changes[name] = (json.dumps(data, sort_keys=True) + "\n").encode()
                meta_name = base + "-meta.json"
                meta = obj(copy, meta_name)
                key = "response_sha256" if is_response else "request_sha256"
                meta[key] = digest(changes[name])
                changes[meta_name] = (json.dumps(meta, sort_keys=True) + "\n").encode()
                consumed = obj(copy, "ui-consumed-options.json")
                consumed[key] = digest(changes[name])
                changes["ui-consumed-options.json"] = (json.dumps(consumed, sort_keys=True) + "\n").encode()
            elif label == "ui-lockfile-source-mismatch":
                data = bytearray(read(copy, target))
                data[0] ^= 1
                changes[target] = bytes(data)
                selected = obj(copy, "selected-pins.json")
                selected["ui_lock_sha256"] = digest(changes[target])
                changes["selected-pins.json"] = (json.dumps(selected, sort_keys=True) + "\n").encode()
            elif label == "ghost-scene-diagnostic":
                data = obj(copy, target)
                data["body"]["data"]["diagnostics"][0]["path"] = "/version"
                changes[target] = (json.dumps(data, sort_keys=True) + "\n").encode()
            else:
                data = obj(copy, target)
                data["status"] = 200
                changes[target] = (json.dumps(data, sort_keys=True) + "\n").encode()
            originals = {name: read(copy, name) for name in changes}
            seal = json.loads(original_seal)
            for name, changed in changes.items():
                (copy / name).write_bytes(changed)
                seal[name] = digest(changed)
            seal_path.write_text(json.dumps(seal, sort_keys=True) + "\n")
            try:
                verify(copy, selection=selection)
            except (AssertionError, ValueError, KeyError, IndexError):
                print("Q01_P01_P03_U01_TAMPER_REJECTED", label)
            else:
                raise AssertionError(f"semantic tamper accepted: {label}")
            for name, original in originals.items():
                (copy / name).write_bytes(original)
            seal_path.write_bytes(original_seal)
        verify(copy, selection=selection)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--tamper-suite", action="store_true")
    parser.add_argument("--expect-selection", type=Path)
    parser.add_argument("--expect-selection-sha256")
    args = parser.parse_args()
    assert bool(args.expect_selection) == bool(args.expect_selection_sha256)
    if args.expect_selection:
        from q01_selection import load_selection
        selection = load_selection(args.expect_selection, args.expect_selection_sha256)
    else:
        selection = None
    root = args.archive.resolve()
    verify(root, selection=selection)
    if args.tamper_suite:
        tamper_suite(root, selection=selection)


if __name__ == "__main__":
    main()
