#!/usr/bin/env python3
"""Isolated CLI integration for the independent IsOS service manager."""
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
ISOS = Path(os.environ.get("ISOS_BIN", str(ROOT / "target/debug/isos")))


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def command(env, *args, success=True):
    proc = subprocess.run([str(ISOS), *args], env=env, capture_output=True,
                          text=True, timeout=20)
    if success and proc.returncode:
        raise AssertionError(f"isos {' '.join(args)}: {proc.stderr}\n{proc.stdout}")
    return proc


def ready(check, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            result = check()
            if result:
                return result
        except (OSError, ValueError, AssertionError):
            pass
        time.sleep(0.05)
    raise AssertionError("manager did not become ready")


def main():
    assert ISOS.is_file(), f"build IsOS first: {ISOS}"
    with tempfile.TemporaryDirectory(prefix="isos-service-v2-") as temp:
        root = Path(temp)
        home = root / "home"
        ui = root / "ui"
        (ui / "node_modules").mkdir(parents=True)
        (ui / "en/isos").mkdir(parents=True)
        (ui / "en/isos/index.html").write_text("<html>test</html>")
        (ui / "package.json").write_text(json.dumps({
            "name": "isos-ui-stub", "version": "2.0.0",
            "scripts": {"start": "python3 -m http.server ${ISOSUI_PORT} --bind 127.0.0.1"}}))
        service_port = port()
        script = root / "server.py"
        script.write_text("#!/usr/bin/env python3\n"
                          "import http.server,sys\n"
                          "print('service online',flush=True)\n"
                          "http.server.HTTPServer(('127.0.0.1',int(sys.argv[1])),http.server.BaseHTTPRequestHandler).serve_forever()\n")
        script.chmod(0o700)
        config = home / "config/services"
        config.mkdir(parents=True)
        (config / "stub.toml").write_text(
            f'version = "1.0.0"\nid = "stub"\ncommand = ["{script}", "{service_port}"]\n'
            f'cwd = "{root}"\nrestart = "never"\nmax_restarts = 0\n'
            f'[artifact]\npath = "{script}"\nsha256 = "{hashlib.sha256(script.read_bytes()).hexdigest()}"\n'
            f'[readiness]\nkind = "tcp"\ntarget = "127.0.0.1:{service_port}"\n')
        env = dict(os.environ, ISOS_HOME=str(home), ISOS_HTTP_PORT=str(port()),
                   ISOSUI_PORT=str(port()), ISOSUI_REPO_PATH=str(ui))
        command(env, "init", "--mode", "dev")
        assert "stub" in command(env, "services", "validate").stdout
        log = (root / "serve.log").open("w+")
        serve = subprocess.Popen([str(ISOS), "serve", "--ui-live"], env=env,
                                 stdout=log, stderr=subprocess.STDOUT, text=True)
        try:
            ready(lambda: (home / "run/manager.sock").exists() or serve.poll() is not None)
            assert serve.poll() is None, (root / "serve.log").read_text()
            started = json.loads(command(env, "services", "start", "stub").stdout)
            assert started["started"] == ["stub"]
            assert json.loads(command(env, "services", "start", "stub").stdout)["started"] == []
            status = json.loads(command(env, "services", "status", "stub").stdout)["services"][0]
            assert status["state"] == "running" and status["pid"] > 0
            assert status["artifact_sha256"] == hashlib.sha256(script.read_bytes()).hexdigest()
            assert json.loads(command(env, "services", "stop", "stub").stdout)["stopped"] == ["stub"]
            assert json.loads(command(env, "services", "stop", "stub").stdout)["stopped"] == []
            command(env, "services", "start", "stub")
            result = json.loads(command(env, "stop").stdout)
            assert "stub" in result["services"]
            assert serve.wait(timeout=10) == 0
            assert not (home / "run/manager.sock").exists()
            print("PASS independent service start, idempotence, status, scoped and root stop")
        finally:
            if serve.poll() is None:
                serve.send_signal(signal.SIGINT)
                serve.wait(timeout=10)
            log.close()


if __name__ == "__main__":
    main()
