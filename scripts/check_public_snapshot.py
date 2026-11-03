#!/usr/bin/env python3
"""Compare tracked source bytes and modes before an independent public snapshot PR."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess


def tracked_manifest(root: Path) -> dict[str, dict[str, str]]:
    untracked = subprocess.check_output(["git", "-C", str(root), "ls-files",
                                         "--others", "--exclude-standard", "-z"])
    if untracked:
        names = [os.fsdecode(name) for name in untracked.split(b"\0") if name]
        raise ValueError(f"stage or remove untracked files before comparison: {names[:10]}")
    listing = subprocess.check_output(["git", "-C", str(root), "ls-files", "--stage", "-z"])
    result: dict[str, dict[str, str]] = {}
    for entry in listing.split(b"\0"):
        if not entry:
            continue
        header, path_bytes = entry.split(b"\t", 1)
        mode, _, stage = header.decode("ascii").split(" ")
        if stage != "0" or mode not in {"100644", "100755", "120000"}:
            raise ValueError(f"unsupported tracked entry: {os.fsdecode(path_bytes)} ({mode}, stage {stage})")
        relative = os.fsdecode(path_bytes)
        path = root / relative
        if mode == "120000":
            if not path.is_symlink():
                raise ValueError(f"tracked symlink changed type: {relative}")
            payload = os.fsencode(os.readlink(path))
        else:
            if not path.is_file() or path.is_symlink():
                raise ValueError(f"tracked file changed type: {relative}")
            actual_mode = "100755" if path.stat().st_mode & stat.S_IXUSR else "100644"
            if actual_mode != mode:
                raise ValueError(f"tracked file mode differs from Git index: {relative}")
            payload = path.read_bytes()
        result[relative] = {"mode": mode, "sha256": hashlib.sha256(payload).hexdigest()}
    return dict(sorted(result.items()))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("private", type=Path, help="reviewed private checkout")
    parser.add_argument("public", type=Path, help="independent-history public snapshot checkout")
    parser.add_argument("--output", type=Path, required=True, help="JSON receipt outside both source trees")
    parser.add_argument("--exclude-private-prefix", action="append", default=[],
                        help="reviewed private-only directory prefix, including trailing slash")
    parser.add_argument("--allow-changed", action="append", default=[],
                        help="reviewed public-only overlay path; both byte digests enter receipt")
    parser.add_argument("--reject-public-text", action="append", default=[],
                        help="literal token forbidden in public tracked paths and text")
    args = parser.parse_args()
    private_root = args.private.resolve(strict=True)
    public_root = args.public.resolve(strict=True)
    private = tracked_manifest(private_root)
    public = tracked_manifest(public_root)
    prefixes = tuple(args.exclude_private_prefix)
    if any(not prefix.endswith("/") or prefix.startswith("/") or ".." in Path(prefix).parts
           for prefix in prefixes):
        parser.error("excluded prefixes must be relative directories ending in /")
    excluded = sorted(name for name in private if name.startswith(prefixes))
    if prefixes and any(not any(name.startswith(prefix) for name in excluded) for prefix in prefixes):
        parser.error("each excluded prefix must select at least one private tracked path")
    forbidden_paths = sorted(name for name in public if name.startswith(prefixes))
    expected = {name: data for name, data in private.items() if name not in excluded}
    missing = sorted(expected.keys() - public.keys())
    extra = sorted(public.keys() - expected.keys())
    changed = sorted(name for name in expected.keys() & public.keys() if expected[name] != public[name])
    allowed = sorted(set(args.allow_changed))
    if any(name not in changed for name in allowed):
        parser.error("each allowed overlay must name an actual changed tracked path")
    disallowed = sorted(set(changed) - set(allowed))
    forbidden_text: dict[str, list[str]] = {}
    for token in args.reject_public_text:
        encoded = token.encode("utf-8")
        matches = [name for name in public if encoded in os.fsencode(name)
                   or encoded in (os.fsencode(os.readlink(public_root / name))
                                  if public[name]["mode"] == "120000"
                                  else (public_root / name).read_bytes())]
        if matches:
            forbidden_text[token] = matches
    matched = not (missing or extra or disallowed or forbidden_paths or forbidden_text)
    receipt = {"private": private, "public": public, "excluded_private_paths": excluded,
               "allowed_public_overlays": allowed, "missing": missing, "extra": extra,
               "changed": changed, "disallowed_changed": disallowed,
               "forbidden_public_paths": forbidden_paths, "forbidden_public_text": forbidden_text,
               "matched": matched}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"matched": matched, "private_files": len(private),
                      "public_files": len(public), "excluded_private_paths": excluded,
                      "allowed_public_overlays": allowed, "missing": missing, "extra": extra,
                      "disallowed_changed": disallowed, "forbidden_public_paths": forbidden_paths,
                      "forbidden_public_text": forbidden_text}, sort_keys=True))
    if not matched:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
