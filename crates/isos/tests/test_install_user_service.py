"""Exercise installer service opt-in without touching the user's systemd session."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE_ROOT = Path(__file__).resolve().parents[1]


class InstallUserServiceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.checkout = self.root / "checkout"
        (self.checkout / "tools").mkdir(parents=True)
        (self.checkout / "target/release").mkdir(parents=True)
        for source in ("install.sh", "tools/user-service.sh"):
            destination = self.checkout / source
            shutil.copy2(SOURCE_ROOT / source, destination)
        (self.checkout / "config").mkdir()
        shutil.copy2(SOURCE_ROOT / "config/isos.toml", self.checkout / "config/isos.toml")
        self.write_executable(self.checkout / "build.sh", "#!/bin/sh\nexit 0\n")
        self.write_executable(
            self.checkout / "target/release/isos",
            '#!/bin/sh\nif [ "$1" = --version ]; then echo "isos 1.0.0"; fi\n',
        )
        fake_bin = self.root / "fake-bin"
        fake_bin.mkdir()
        self.write_executable(
            fake_bin / "cargo",
            '#!/bin/sh\n'
            'while [ "$#" -gt 0 ]; do\n'
            '  if [ "$1" = --root ]; then shift; root="$1"; fi\n'
            '  shift\n'
            'done\n'
            'mkdir -p "$root/bin"\n'
            'cp "$ISOS_TEST_BINARY" "$root/bin/isos"\n',
        )
        self.write_executable(
            fake_bin / "systemctl",
            '#!/bin/sh\n'
            'printf "%s\\n" "$*" >> "$ISOS_TEST_SYSTEMCTL_LOG"\n'
            'case "$*" in *is-active*) exit 3;; esac\n',
        )
        home = self.root / "home"
        (home / ".config/isos").mkdir(parents=True)
        (home / ".config/isos/mode").write_text("product\n")
        self.environment = dict(os.environ)
        self.environment.update(
            HOME=str(home),
            PATH=f"{fake_bin}:{os.environ['PATH']}",
            ISOS_INSTALL_ROOT=str(self.root / "installed"),
            ISOS_TEST_BINARY=str(self.checkout / "target/release/isos"),
            ISOS_TEST_SYSTEMCTL_LOG=str(self.root / "systemctl.log"),
        )

    @staticmethod
    def write_executable(path, content):
        path.write_text(content)
        path.chmod(0o755)

    def install(self, *arguments):
        return subprocess.run(
            [str(self.checkout / "install.sh"), *arguments],
            env=self.environment,
            text=True,
            capture_output=True,
            check=False,
        )

    def systemctl_calls(self):
        log = self.root / "systemctl.log"
        return log.read_text().splitlines() if log.exists() else []

    def test_default_install_does_not_enable_or_start_service(self):
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.root / "installed/bin/isos").exists())
        self.assertEqual(self.systemctl_calls(), ["--user is-active --quiet isos-server.service"])
        self.assertFalse((self.root / "home/.config/systemd/user/isos.service").exists())

    def test_flag_installs_and_starts_user_service(self):
        result = self.install("--install-user-service")
        self.assertEqual(result.returncode, 0, result.stderr)
        unit = self.root / "home/.config/systemd/user/isos.service"
        self.assertIn(f"ExecStart={self.root}/installed/bin/isos serve", unit.read_text())
        self.assertEqual(
            self.systemctl_calls(),
            [
                "--user is-active --quiet isos-server.service",
                "--user daemon-reload",
                "--user enable --now isos.service",
            ],
        )

    def test_unknown_flag_exits_before_install(self):
        result = self.install("--unknown")
        self.assertEqual(result.returncode, 2)
        self.assertFalse((self.root / "installed").exists())
        self.assertEqual(self.systemctl_calls(), [])

    def test_activate_env_selects_private_home_without_stopping_old_manager(self):
        selected = self.root / "selected"
        source = self.root / "setup.env"
        source.write_text(
            f"export ISOS_HOME={selected}\n"
            "export ISOS_HTTP_PORT=37125\n"
            "export ISOSUI_PORT=37126\n"
            f"export ISOS_AETHER_BIN_DIR={selected}/eos/bin\n"
        )
        installed = self.root / "installed/bin/isos"
        installed.parent.mkdir(parents=True)
        self.write_executable(installed, "#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$ISOS_TEST_OLD_CLI_LOG\"\n")
        self.environment["ISOS_TEST_OLD_CLI_LOG"] = str(self.root / "old-cli.log")
        result = self.install("--activate-env", str(source))
        self.assertEqual(result.returncode, 0, result.stderr)
        active = self.root / "home/.config/isos/active.env"
        self.assertEqual(active.read_text(), source.read_text() + f"export ISOS_CONFIG={selected}/config/isos.toml\n")
        self.assertEqual(active.stat().st_mode & 0o777, 0o600)
        self.assertEqual((selected / "config/isos.toml").stat().st_mode & 0o777, 0o600)
        self.assertNotIn("[server]", (selected / "config/isos.toml").read_text())
        self.assertFalse((self.root / "old-cli.log").exists())
        self.assertEqual((self.root / "home/.config/isos/mode").read_text(), "product\n")

    def test_invalid_activation_rejected_before_install(self):
        source = self.root / "bad.env"
        source.write_text("export ISOS_HOME=/home/test\nexport UNEXPECTED_KEY=value\n")
        result = self.install("--activate-env", str(source))
        self.assertEqual(result.returncode, 2)
        self.assertFalse((self.root / "home/.config/isos/active.env").exists())
        self.assertFalse((self.root / "installed").exists())

    def test_package_specific_activation_key_is_rejected(self):
        source = self.root / "bad.env"
        source.write_text("export ISOS_HOME=/home/test\nexport MUJOCO_ROOT=/tmp/mujoco\n")
        result = self.install("--activate-env", str(source))
        self.assertEqual(result.returncode, 2)
        self.assertIn("unsupported active env key MUJOCO_ROOT", result.stderr)

    def test_failed_build_preserves_previous_activation(self):
        active = self.root / "home/.config/isos/active.env"
        active.write_text("export ISOS_HOME=/home/old\n")
        source = self.root / "setup.env"
        source.write_text("export ISOS_HOME=/home/new\nexport ISOS_HTTP_PORT=37125\nexport ISOSUI_PORT=37126\n")
        self.write_executable(self.checkout / "build.sh", "#!/bin/sh\nexit 7\n")
        result = self.install("--activate-env", str(source))
        self.assertEqual(result.returncode, 7)
        self.assertEqual(active.read_text(), "export ISOS_HOME=/home/old\n")

    def test_active_obsolete_service_blocks_install_without_stopping_it(self):
        self.write_executable(
            self.root / "fake-bin/systemctl",
            '#!/bin/sh\nprintf "%s\\n" "$*" >> "$ISOS_TEST_SYSTEMCTL_LOG"\n'
            'case "$*" in *is-active*) exit 0;; esac\n',
        )
        result = self.install()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("systemctl --user stop isos-server.service", result.stderr)
        self.assertEqual(self.systemctl_calls(), ["--user is-active --quiet isos-server.service"])
        self.assertFalse((self.root / "installed/bin/isos").exists())


if __name__ == "__main__":
    unittest.main()
