#!/usr/bin/env python3
"""Verify the exact GitHub Actions run and bytes selected for CLI publication."""

import argparse
import hashlib
import json
import re
import tarfile
from pathlib import Path


class ReleaseError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ReleaseError(message)


def verify(run: dict, jobs: dict, workflow: dict, directory: Path, repository: str, head: str,
           version: str, run_id: int) -> str:
    require(re.fullmatch(r"[0-9a-f]{40}", head) is not None, "invalid selected master SHA")
    require(re.fullmatch(r"0\.[0-9]+\.[0-9]+", version) is not None, "invalid release version")
    require(run.get("id") == run_id, "prepare run ID mismatch")
    require(run.get("repository", {}).get("full_name") == repository, "prepare repository mismatch")
    workflow_path = ".github/workflows/cli-release.yml"
    require(workflow.get("path") == workflow_path and workflow.get("state") == "active",
            "selected workflow identity mismatch")
    require(type(workflow.get("id")) is int and workflow["id"] > 0,
            "invalid selected workflow ID")
    require(type(run.get("workflow_id")) is int and run["workflow_id"] == workflow["id"]
            and run.get("path") == workflow_path,
            "prepare workflow identity mismatch")
    require(run.get("event") == "workflow_dispatch", "prepare event mismatch")
    require(run.get("head_branch") == "master", "prepare branch mismatch")
    require(run.get("status") == "completed" and run.get("conclusion") == "success",
            "prepare run unsuccessful")
    require(run.get("head_sha") == head, "prepare head differs from selected master")
    listed = jobs.get("jobs", [])
    require(jobs.get("total_count") == len(listed) and 1 <= len(listed) <= 2,
            "unexpected prepare job count")
    require({job.get("name") for job in listed} in ({"prepare"}, {"prepare", "publish"}),
            "unexpected prepare job set")
    require(all(job.get("run_id") == run_id for job in listed), "job run ID mismatch")
    require(all(job.get("conclusion") == "skipped" for job in listed if job.get("name") == "publish"),
            "publish phase job unexpectedly ran")
    job = next(job for job in listed if job.get("name") == "prepare")
    require(job.get("run_id") == run_id and job.get("name") == "prepare"
            and job.get("status") == "completed" and job.get("conclusion") == "success",
            "prepare phase job not successful")

    name = f"isos-v{version}-linux-x86_64.tar.gz"
    require({p.name for p in directory.iterdir()} ==
            {name, "SHA256SUMS", "VERSION", "SOURCE_COMMIT", "PREPARE.json"},
            "unexpected release artifact files")
    require((directory / "VERSION").read_text() == version + "\n", "prepared version mismatch")
    require((directory / "SOURCE_COMMIT").read_text() == head + "\n",
            "prepared source commit mismatch")
    prepare = json.loads((directory / "PREPARE.json").read_text())
    require(prepare == {"repository": repository, "workflow": ".github/workflows/cli-release.yml",
                        "event": "workflow_dispatch", "phase": "prepare", "run_id": run_id,
                        "source_commit": head}, "prepared metadata mismatch")
    archive = directory / name
    require(archive.is_file() and not archive.is_symlink(), "archive is not a regular file")
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    require((directory / "SHA256SUMS").read_text() == f"{digest}  {name}\n",
            "archive SHA256SUMS mismatch")
    with tarfile.open(archive, "r:gz") as contents:
        members = contents.getmembers()
        require({"" if m.name in (".", "./") else m.name.removeprefix("./") for m in members} ==
                {"", "isos", "LICENSE", "README.md"} and len(members) == 4,
                "unexpected archive contents")
        require(all(m.isdir() if m.name in (".", "./") else m.isfile() for m in members),
                "archive contains nonregular entry")
        binary = next(m for m in members if m.name.removeprefix("./") == "isos")
        require(bool(binary.mode & 0o111) and binary.size > 0, "CLI binary is empty or not executable")
    return digest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-json", type=Path, required=True)
    parser.add_argument("--jobs-json", type=Path, required=True)
    parser.add_argument("--workflow-json", type=Path, required=True)
    parser.add_argument("--release-dir", type=Path, required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--head-sha", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--run-id", type=int, required=True)
    args = parser.parse_args()
    digest = verify(json.loads(args.run_json.read_text()), json.loads(args.jobs_json.read_text()),
                    json.loads(args.workflow_json.read_text()),
                    args.release_dir, args.repository, args.head_sha, args.version, args.run_id)
    print(f"verified prepare run {args.run_id}, source {args.head_sha}, archive sha256 {digest}")


if __name__ == "__main__":
    main()
