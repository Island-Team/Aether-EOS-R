#!/usr/bin/env python3
"""Exercise local systemd launch ownership with disposable user units."""

from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import uuid


def main() -> None:
    root = Path(sys.argv[1])
    available = subprocess.run(["systemctl", "--user", "show", "--property=Version"],
                               capture_output=True)
    if available.returncode:
        print("SKIP: systemd user manager unavailable")
        return
    with tempfile.TemporaryDirectory(prefix="island-launch-test-") as temporary:
        base = Path(temporary)
        suffix = uuid.uuid4().hex[:8]
        driver = f"test-driver-{suffix}"
        worker = f"test-worker-{suffix}"
        (base / "eos.toml").write_text("[gateway]\nhost = '127.0.0.1'\n")
        (base / "stack.toml").write_text(
            "version = 1\neos_config = 'eos.toml'\nmodules = []\n"
            f"[[service]]\nname = '{driver}'\nrole = 'driver'\n"
            "command = ['/usr/bin/sleep', '300']\n"
            f"[[service]]\nname = '{worker}'\nrole = 'managed'\n"
            f"after = ['{driver}']\n"
            "command = ['/usr/bin/bash', '-c', 'sleep 300 & wait']\n"
        )
        environment = {**os.environ, "ISLAND_RUN": str(base / "run")}

        def invoke(*args: str) -> dict:
            result = subprocess.run([str(root / "bin/island-launch"), *args, "--json"],
                                    env=environment, text=True, capture_output=True, timeout=20)
            assert result.returncode == 0, result.stderr
            return json.loads(result.stdout)

        try:
            invoke("profile", "use", str(base))
            started = invoke("eos", "start")
            assert started["units"][driver]["ActiveState"] == "active"
            assert started["units"][worker]["ActiveState"] == "active"
            driver_pid = started["units"][driver]["MainPID"]
            # A fresh CLI invocation reattaches to units by stable scoped names.
            assert invoke("eos", "start")["units"][worker]["ActiveState"] == "active"
            eos_restarted = invoke("eos", "restart")
            assert eos_restarted["units"][driver]["MainPID"] == driver_pid
            stopped = invoke("eos", "stop")
            assert stopped["units"][driver]["ActiveState"] == "active"
            assert stopped["units"][worker]["ActiveState"] != "active"
            restarted = invoke("eos", "start")
            assert restarted["units"][driver]["ActiveState"] == "active"
            assert restarted["units"][worker]["ActiveState"] == "active"
            backend_restarted = invoke("backend", "restart")
            assert backend_restarted["units"][driver]["ActiveState"] == "active"
            assert backend_restarted["units"][driver]["MainPID"] != driver_pid
            manifest_path = base / "stack.toml"
            manifest_path.rename(base / "stack.saved.toml")
            try:
                assert invoke("eos", "status")["units"][worker]["ActiveState"] == "active"
            finally:
                (base / "stack.saved.toml").rename(manifest_path)
            cleared = invoke("backend", "stop")
            assert all(cleared["units"][name]["ActiveState"] != "active"
                       for name in (driver, worker))
            assert cleared["active"] is None
            # A listening endpoint without the scoped unit must never be adopted.
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                listener.listen()
                port = listener.getsockname()[1]
                with (base / "stack.toml").open("a") as stream:
                    stream.write(f"[[service]]\nname = 'test-conflict-{suffix}'\n"
                                 "role = 'managed'\ncommand = ['/usr/bin/sleep', '300']\n"
                                 f"ready_tcp = '127.0.0.1:{port}'\ntimeout_ms = 250\n")
                result = subprocess.run([str(root / "bin/island-launch"), "eos", "start"],
                                        env=environment, text=True, capture_output=True, timeout=20)
                assert result.returncode != 0 and "unmanaged process" in result.stderr, result.stderr
            result = subprocess.run([str(root / "bin/island-launch"), "eos", "start"],
                                    env=environment, text=True, capture_output=True, timeout=20)
            assert result.returncode != 0 and "did not become ready" in result.stderr, result.stderr
            failed = invoke("eos", "status")
            assert failed["units"][worker]["ActiveState"] != "active"
            assert failed["units"][driver]["ActiveState"] == "active"
            recovered = invoke("backend", "stop")
            assert recovered["units"][driver]["ActiveState"] != "active"
            result = subprocess.run([str(root / "bin/island-launch"), "backend", "start"],
                                    env=environment, text=True, capture_output=True, timeout=20)
            assert result.returncode != 0 and "did not become ready" in result.stderr, result.stderr
            assert invoke("backend", "status")["units"][driver]["ActiveState"] != "active"
        finally:
            subprocess.run([str(root / "bin/island-launch"), "backend", "stop"],
                           env=environment, capture_output=True, timeout=20)


if __name__ == "__main__":
    main()
