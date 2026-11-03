"""Focused path binding checks for relocated R06/A03 evidence archives."""

import tempfile
import unittest
from pathlib import Path

from verify_q01_r06_a03 import recorded_case_root


class RecordedCaseRootTests(unittest.TestCase):
    def test_original_and_relocated_archive_share_recorded_launch_root(self):
        with tempfile.TemporaryDirectory() as directory:
            launch = Path(directory) / "original" / "case"
            copied = Path(directory) / "matrix" / "cases" / "r06-a03"
            argv = ["replay.py", "--output", str(launch)]
            env = {
                "ISOS_AETHER_BIN_DIR": str(launch / "selected-binaries"),
                "ISOSUI_DIST_DIR": str(launch / "selected-ui-dist"),
            }
            self.assertEqual(recorded_case_root(argv, env), launch.resolve())
            self.assertNotEqual(copied, launch)
            self.assertEqual(recorded_case_root(argv, env), launch.resolve())

    def test_rejects_tampered_recorded_output_or_bin_parent(self):
        with tempfile.TemporaryDirectory() as directory:
            launch = Path(directory) / "case"
            argv = ["replay.py", "--output", str(launch)]
            env = {
                "ISOS_AETHER_BIN_DIR": str(launch / "selected-binaries"),
                "ISOSUI_DIST_DIR": str(launch / "selected-ui-dist"),
            }
            for forged_argv, forged_env in (
                (["replay.py", "--output", str(launch.parent / "case-sibling")], env),
                (argv, dict(env, ISOS_AETHER_BIN_DIR=str(
                    launch.parent / "case-sibling" / "selected-binaries"))),
                (argv, dict(env, ISOSUI_DIST_DIR=str(
                    launch.parent / "case-sibling" / "selected-ui-dist"))),
            ):
                with self.subTest(argv=forged_argv, env=forged_env):
                    with self.assertRaises(AssertionError):
                        recorded_case_root(forged_argv, forged_env)


if __name__ == "__main__":
    unittest.main()
