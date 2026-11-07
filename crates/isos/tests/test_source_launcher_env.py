"""Check source-launcher configuration precedence without running IsOS."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE_ROOT = Path(__file__).resolve().parents[1]


class SourceLauncherEnvTest(unittest.TestCase):
    def test_local_env_precedes_example_and_example_fills_missing_value(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "bin").mkdir()
            (root / "target/release").mkdir(parents=True)
            shutil.copy2(SOURCE_ROOT / "bin/isos", root / "bin/isos")
            shutil.copy2(SOURCE_ROOT / ".env.example", root / ".env.example")
            executable = root / "target/release/isos"
            executable.write_text("#!/bin/sh\nprintf '%s\\n' \"$ISOS_AETHER_BIN_DIR\"\n")
            executable.chmod(0o755)
            environment = dict(os.environ)
            environment.pop("ISOS_AETHER_BIN_DIR", None)

            (root / ".env").write_text("ISOS_AETHER_BIN_DIR=/local/core/bin\n")
            result = subprocess.run(
                [str(root / "bin/isos")],
                env=environment,
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertEqual(result.stdout.strip(), "/local/core/bin")

            (root / ".env").unlink()
            result = subprocess.run(
                [str(root / "bin/isos")],
                env=environment,
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertEqual(
                result.stdout.strip(),
                f"{root}/../../target/release",
            )
