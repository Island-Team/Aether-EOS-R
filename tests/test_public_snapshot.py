"""Check that the public snapshot receipt binds only tracked source bytes and modes."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/check_public_snapshot.py"


def git(root: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(root), *args], check=True, capture_output=True)


class PublicSnapshotTest(unittest.TestCase):
    def test_tracked_contents_and_mode_must_match(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            private, public = root / "private", root / "public"
            for checkout in (private, public):
                checkout.mkdir()
                git(checkout, "init", "-q")
                (checkout / "source.txt").write_text("reviewed\n")
                (checkout / "ignored.log").write_text("different local state\n")
                (checkout / ".gitignore").write_text("ignored.log\n")
                git(checkout, "add", "source.txt", ".gitignore")
            (public / "ignored.log").write_text("another local state\n")
            receipt = root / "receipt.json"

            def compare() -> subprocess.CompletedProcess[str]:
                return subprocess.run([sys.executable, str(SCRIPT), str(private), str(public),
                                       "--output", str(receipt)], capture_output=True, text=True)

            self.assertEqual(compare().returncode, 0)
            result = json.loads(receipt.read_text())
            self.assertTrue(result["matched"])
            self.assertEqual(list(result["private"]), [".gitignore", "source.txt"])
            (public / "source.txt").write_text("changed\n")
            self.assertEqual(compare().returncode, 1)
            self.assertEqual(json.loads(receipt.read_text())["changed"], ["source.txt"])
            allowed = subprocess.run([sys.executable, str(SCRIPT), str(private), str(public),
                                      "--output", str(receipt), "--allow-changed", "source.txt"],
                                     capture_output=True, text=True)
            self.assertEqual(allowed.returncode, 0)
            self.assertEqual(json.loads(receipt.read_text())["allowed_public_overlays"], ["source.txt"])
            (public / "source.txt").write_text("reviewed\n")
            os.chmod(public / "source.txt", 0o755)
            self.assertNotEqual(compare().returncode, 0)


    def test_only_declared_private_example_may_be_omitted(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            private, public = root / "private", root / "public"
            for checkout in (private, public):
                checkout.mkdir()
                git(checkout, "init", "-q")
                (checkout / "source.txt").write_text("same\n")
                git(checkout, "add", "source.txt")
            example = Path("private-only/inspection.cpp")
            (private / example).parent.mkdir(parents=True)
            (private / example).write_text("private skill\n")
            git(private, "add", str(example))
            receipt = root / "receipt.json"

            def compare() -> subprocess.CompletedProcess[str]:
                return subprocess.run([sys.executable, str(SCRIPT), str(private), str(public),
                                       "--output", str(receipt),
                                       "--exclude-private-prefix", "private-only/",
                                       "--reject-public-text", "private skill"],
                                      capture_output=True, text=True)

            self.assertEqual(compare().returncode, 0)
            self.assertEqual(json.loads(receipt.read_text())["excluded_private_paths"],
                             [example.as_posix()])
            (public / example).parent.mkdir(parents=True)
            (public / example).write_text("private skill\n")
            git(public, "add", str(example))
            self.assertNotEqual(compare().returncode, 0)
            git(public, "rm", "-q", "-f", str(example))
            (public / "untracked.txt").write_text("copied but not staged\n")
            failed = compare()
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn("stage or remove untracked files", failed.stderr)


if __name__ == "__main__":
    unittest.main()
