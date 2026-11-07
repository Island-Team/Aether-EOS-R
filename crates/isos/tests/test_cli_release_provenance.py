"""Offline publish gate tests; no GitHub release is created."""

import copy
import hashlib
import io
import json
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from verify_cli_release import ReleaseError, verify  # noqa: E402


REPOSITORY = "Island-Team/isos"
HEAD = "a" * 40
RUN_ID = 1234
VERSION = "0.1.0"


class ReleaseProvenanceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.run = {
            "id": RUN_ID, "repository": {"full_name": REPOSITORY},
            "workflow_id": 400604125, "path": ".github/workflows/cli-release.yml",
            "event": "workflow_dispatch", "head_branch": "master", "head_sha": HEAD,
            "status": "completed", "conclusion": "success",
        }
        self.workflow = {"id": 400604125, "path": ".github/workflows/cli-release.yml",
                         "state": "active"}
        self.jobs = {"total_count": 1, "jobs": [{
            "run_id": RUN_ID, "name": "prepare", "status": "completed", "conclusion": "success",
        }]}
        name = f"isos-v{VERSION}-linux-x86_64.tar.gz"
        archive = self.directory / name
        with tarfile.open(archive, "w:gz") as tar:
            root = tarfile.TarInfo("./")
            root.type = tarfile.DIRTYPE
            tar.addfile(root)
            for filename, mode in (("isos", 0o755), ("LICENSE", 0o644), ("README.md", 0o644)):
                payload = filename.encode()
                member = tarfile.TarInfo("./" + filename)
                member.size = len(payload)
                member.mode = mode
                tar.addfile(member, io.BytesIO(payload))
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        (self.directory / "SHA256SUMS").write_text(f"{digest}  {name}\n")
        (self.directory / "VERSION").write_text(VERSION + "\n")
        (self.directory / "SOURCE_COMMIT").write_text(HEAD + "\n")
        self.prepare = {"repository": REPOSITORY, "workflow": ".github/workflows/cli-release.yml",
                        "event": "workflow_dispatch", "phase": "prepare", "run_id": RUN_ID,
                        "source_commit": HEAD}
        self.save_prepare()

    def save_prepare(self):
        (self.directory / "PREPARE.json").write_text(json.dumps(self.prepare) + "\n")

    def check(self):
        return verify(self.run, self.jobs, self.workflow, self.directory,
                      REPOSITORY, HEAD, VERSION, RUN_ID)

    def test_valid_prepare(self):
        self.assertRegex(self.check(), r"^[0-9a-f]{64}$")
        self.jobs = {"total_count": 2, "jobs": [self.jobs["jobs"][0], {
            "run_id": RUN_ID, "name": "publish", "status": "completed", "conclusion": "skipped",
        }]}
        self.assertRegex(self.check(), r"^[0-9a-f]{64}$")

    def test_reject_publish_job_executed(self):
        self.jobs = {"total_count": 2, "jobs": [self.jobs["jobs"][0], {
            "run_id": RUN_ID, "name": "publish", "status": "completed", "conclusion": "success",
        }]}
        with self.assertRaises(ReleaseError):
            self.check()

    def test_reject_wrong_run_identity_and_phase(self):
        for field, value in (("id", 55), ("repository", {"full_name": "other/isos"}),
                             ("workflow_id", 99), ("workflow_id", True),
                             ("path", ".github/workflows/other.yml"),
                             ("event", "push"), ("head_branch", "topic"),
                             ("head_sha", "b" * 40), ("status", "in_progress"),
                             ("conclusion", "failure")):
            with self.subTest(field=field):
                original = copy.deepcopy(self.run[field])
                self.run[field] = value
                with self.assertRaises(ReleaseError):
                    self.check()
                self.run[field] = original
        for field, value in (("id", 99), ("id", True), ("path", ".github/workflows/other.yml"),
                             ("state", "disabled_manually")):
            with self.subTest(workflow=field, value=value):
                original = self.workflow[field]
                self.workflow[field] = value
                with self.assertRaises(ReleaseError):
                    self.check()
                self.workflow[field] = original
        for field, value in (("name", "publish"), ("conclusion", "failure"), ("run_id", 55)):
            with self.subTest(job=field):
                old = self.jobs["jobs"][0][field]
                self.jobs["jobs"][0][field] = value
                with self.assertRaises(ReleaseError):
                    self.check()
                self.jobs["jobs"][0][field] = old

    def test_reject_artifact_metadata_and_bytes(self):
        for field, value in (("phase", "publish"), ("run_id", 55), ("source_commit", "b" * 40)):
            with self.subTest(field=field):
                old = self.prepare[field]
                self.prepare[field] = value
                self.save_prepare()
                with self.assertRaises(ReleaseError):
                    self.check()
                self.prepare[field] = old
                self.save_prepare()
        old = (self.directory / "SOURCE_COMMIT").read_text()
        (self.directory / "SOURCE_COMMIT").write_text("b" * 40 + "\n")
        with self.assertRaises(ReleaseError):
            self.check()
        (self.directory / "SOURCE_COMMIT").write_text(old)
        old = (self.directory / "SHA256SUMS").read_text()
        (self.directory / "SHA256SUMS").write_text("0" * 64 + old[64:])
        with self.assertRaises(ReleaseError):
            self.check()

    def test_reject_version_extra_file_and_archive_tampering(self):
        (self.directory / "VERSION").write_text("0.1.1\n")
        with self.assertRaises(ReleaseError):
            self.check()
        (self.directory / "VERSION").write_text(VERSION + "\n")
        (self.directory / "unexpected").write_text("extra")
        with self.assertRaises(ReleaseError):
            self.check()
        (self.directory / "unexpected").unlink()
        staging = self.directory / "package"
        staging.mkdir()
        (staging / "isos").write_text("extra upload")
        with self.assertRaises(ReleaseError):
            self.check()
        (staging / "isos").unlink()
        staging.rmdir()
        archive = self.directory / f"isos-v{VERSION}-linux-x86_64.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            member = tarfile.TarInfo("../escape")
            member.size = 1
            tar.addfile(member, io.BytesIO(b"x"))
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        (self.directory / "SHA256SUMS").write_text(f"{digest}  {archive.name}\n")
        with self.assertRaises(ReleaseError):
            self.check()

    def test_prepare_upload_removes_staging(self):
        script = (Path(__file__).resolve().parents[1] / ".github/workflows/cli-release.yml").read_text()
        self.assertLess(script.index('tar -C release/package -czf'),
                        script.index('rm -r release/package'))
        self.assertLess(script.index('rm -r release/package'),
                        script.index('echo "name=${name}" >> "$GITHUB_OUTPUT"'))
        self.assertLess(script.index('rm -r release/package'),
                        script.index('- uses: actions/upload-artifact@v4'))

    def test_remote_master_is_rechecked_immediately_before_publish(self):
        script = (Path(__file__).resolve().parents[1] / ".github/workflows/cli-release.yml").read_text()
        gate = 'bash scripts/require_release_master.sh "$GITHUB_SHA"'
        self.assertEqual(script.count(gate), 2)
        self.assertIn(
            "[[ -z \"$tag_refs\" ]] || { echo 'Release tag appeared during verification' >&2; exit 1; }\n"
            f"          {gate}\n          gh release create",
            script,
        )
        mock_dir = self.directory / "mock-bin"
        mock_dir.mkdir()
        git = mock_dir / "git"
        git.write_text("#!/bin/sh\nprintf '%s\\trefs/heads/master\\n' \"$MOCK_MASTER\"\n")
        git.chmod(0o755)
        helper = Path(__file__).resolve().parents[1] / "scripts/require_release_master.sh"
        for selected, remote, accepted in ((HEAD, HEAD, True), (HEAD, "b" * 40, False)):
            with self.subTest(remote=remote):
                env = dict(os.environ, PATH=f"{mock_dir}:{os.environ['PATH']}", MOCK_MASTER=remote)
                result = subprocess.run(["bash", str(helper), selected], env=env,
                                        capture_output=True, text=True, check=False)
                self.assertEqual(result.returncode == 0, accepted)


if __name__ == "__main__":
    unittest.main()
