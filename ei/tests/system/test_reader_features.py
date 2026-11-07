"""The selected EGIA launcher reports capabilities without starting EGIA."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path


def _launcher() -> Path:
  return Path(__file__).resolve().parents[2] / "bin/egia"


def _probe(
  prefix: Path, *, python: str | None = None
) -> subprocess.CompletedProcess[str]:
  env = dict(os.environ)
  env["ISLAND_PREFIX"] = str(prefix)
  env.pop("ISLAND_PYTHON", None)
  env.pop("PYTHONPATH", None)
  if python is None:
    env.pop("EGIA_PYTHON", None)
  else:
    env["EGIA_PYTHON"] = python
  return subprocess.run(
    [str(_launcher()), "--capabilities"],
    cwd=prefix,
    env=env,
    capture_output=True,
    text=True,
    timeout=10,
    check=False,
  )


def test_real_selected_launcher_and_setup_identity_agree(tmp_path: Path) -> None:
  root = Path(__file__).resolve().parents[2]
  runtime = _probe(root, python=sys.executable)
  setup = subprocess.run(
    [str(root / "bin/egia-config"), "setup", "--json"],
    input='{"schema_version":2,"op":"identity","core":"egia"}',
    cwd=tmp_path,
    env={**os.environ, "EGIA_PYTHON": sys.executable},
    capture_output=True,
    text=True,
    timeout=10,
    check=True,
  )
  assert runtime.returncode == 0
  assert runtime.stderr == ""
  runtime_reply = json.loads(runtime.stdout)
  setup_reply = json.loads(setup.stdout)
  fixture = json.loads(
    (root / "tests/fixtures/reader_features/advertised.json").read_text(
      encoding="utf-8"
    )
  )
  for field, expected in fixture["setup"].items():
    actual = setup_reply["identity"][field]
    if field in {
      "supported_graph_versions",
      "supported_node_type_versions",
      "supported_node_types",
      "features",
    }:
      # P05's frozen fixture is the historical floor; A01 adds exact stages.
      assert set(expected) <= set(actual)
    elif field in {"product_version", "graph_version", "node_type_version"}:
      assert (
        actual
        == {
          "product_version": "0.2.1",
          "graph_version": "0.2.0",
          "node_type_version": "0.2",
        }[field]
      )
    else:
      assert actual == expected
  assert set(fixture["runtime"]["features"]) <= set(runtime_reply["features"])
  assert runtime_reply == {
    "binary": "egia",
    "product_version": setup_reply["identity"]["product_version"],
    "features": [
      "managed-config-v1",
      "eos-gateway-client-v2",
      "egia-evidence-scheduler-v1",
      "vision-evidence-ingress-v1",
      "vision-inspection-uds-v1",
    ],
  }
  assert len(setup_reply["identity"]["supported_node_types"]) == 22
  assert setup_reply["identity"]["supported_node_types"] == sorted(
    setup_reply["identity"]["supported_node_types"]
  )
  assert setup_reply["identity"]["features"] == [
    "setup-v2",
    "catalog",
    "graph-0.1-reader",
    "graph-0.2-ei-projection",
    "egia-evidence-routing-v1",
  ]
  assert not any(key.startswith("supported_") for key in runtime_reply)


def test_probe_module_does_not_import_runtime_or_transport(tmp_path: Path) -> None:
  root = Path(__file__).resolve().parents[2]
  script = """import egia.reader_features as probe
import sys
probe.main()
assert 'egia.main' not in sys.modules
assert 'egia.core' not in sys.modules
assert 'grpc' not in sys.modules
"""
  result = subprocess.run(
    [
      sys.executable,
      "-B",
      "-c",
      script,
    ],
    cwd=tmp_path,
    env={**os.environ, "PYTHONPATH": str(root / "src")},
    capture_output=True,
    text=True,
    timeout=10,
    check=False,
  )
  assert result.returncode == 0, result.stderr
  assert json.loads(result.stdout)["binary"] == "egia"
  assert list(tmp_path.iterdir()) == []


def test_missing_default_interpreter_never_bootstraps(tmp_path: Path) -> None:
  source = tmp_path / "source"
  (source / "src/egia").mkdir(parents=True)
  (source / "pyproject.toml").write_text("[project]\nname='egia'\n")
  fake_bin = tmp_path / "bin"
  fake_bin.mkdir()
  uv_called = tmp_path / "uv-called"
  uv = fake_bin / "uv"
  uv.write_text(f"#!/bin/sh\ntouch '{uv_called}'\nexit 99\n")
  uv.chmod(0o755)
  env = dict(os.environ)
  env["ISLAND_PREFIX"] = str(source)
  env["PATH"] = f"{fake_bin}:/usr/bin:/bin"
  for name in ("EGIA_PYTHON", "ISLAND_PYTHON", "PYTHONPATH"):
    env.pop(name, None)
  result = subprocess.run(
    [str(_launcher()), "--capabilities"],
    cwd=source,
    env=env,
    capture_output=True,
    text=True,
    timeout=10,
    check=False,
  )
  assert result.returncode == 127
  assert result.stdout == ""
  assert "interpreter" in result.stderr
  assert not uv_called.exists()
  assert not (source / ".venv").exists()


def test_probe_uses_selected_interpreter_and_installed_module_tree(
  tmp_path: Path,
) -> None:
  prefix = tmp_path / "portable"
  module = prefix / "lib/island/egia/reader_features.py"
  module.parent.mkdir(parents=True)
  (prefix / "share/contracts").mkdir(parents=True)
  (prefix / "libexec").mkdir()
  (module.parent / "__init__.py").write_text("")
  module.write_text('print("selected-module-v1")\n')
  interpreter = tmp_path / "selected-python"
  called = tmp_path / "interpreter-called"
  interpreter.write_text(
    f"#!/bin/sh\nprintf '%s\\n' \"$*\" > '{called}'\nexec '{sys.executable}' \"$@\"\n"
  )
  interpreter.chmod(0o755)

  first = _probe(prefix, python=str(interpreter))
  assert first.returncode == 0
  assert first.stdout.strip() == "selected-module-v1"
  assert called.read_text().strip() == "-B -m egia.reader_features"
  assert not list(prefix.rglob("__pycache__"))

  module.write_text('print("selected-module-v2-updated")\n')
  second = _probe(prefix, python=str(interpreter))
  assert second.returncode == 0
  assert second.stdout.strip() == "selected-module-v2-updated"
  assert not list(prefix.rglob("__pycache__"))


def test_setup_and_runtime_use_same_selected_installed_module_root(
  tmp_path: Path,
) -> None:
  prefix = tmp_path / "portable"
  (prefix / "share/contracts").mkdir(parents=True)
  (prefix / "libexec").mkdir()
  selected_lib = tmp_path / "selected-lib"
  package = selected_lib / "egia"
  package.mkdir(parents=True)
  (package / "__init__.py").write_text("")
  (package / "reader_features.py").write_text('print("runtime-selected")\n')
  (package / "config_cli.py").write_text('print("setup-selected")\n')
  env = dict(os.environ)
  env.update(
    {
      "ISLAND_PREFIX": str(prefix),
      "ISLAND_LIB": str(selected_lib),
      "EGIA_PYTHON": sys.executable,
    }
  )
  env.pop("PYTHONPATH", None)
  env.pop("ISLAND_PYTHON", None)
  for command, expected in (
    ([_launcher(), "--capabilities"], "runtime-selected"),
    ([_launcher().with_name("egia-config"), "setup", "--json"], "setup-selected"),
  ):
    result = subprocess.run(
      [str(part) for part in command],
      cwd=prefix,
      env=env,
      capture_output=True,
      text=True,
      timeout=10,
      check=False,
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == expected
  assert not list(selected_lib.rglob("__pycache__"))


def test_missing_explicit_interpreter_fails_without_side_effects(
  tmp_path: Path,
) -> None:
  result = _probe(tmp_path, python=str(tmp_path / "missing-python"))
  assert result.returncode == 127
  assert result.stdout == ""
  assert list(tmp_path.iterdir()) == []


def test_capabilities_rejects_launch_arguments_before_bootstrap(
  tmp_path: Path,
) -> None:
  env = {**os.environ, "ISLAND_PREFIX": str(tmp_path)}
  env.pop("EGIA_PYTHON", None)
  result = subprocess.run(
    [str(_launcher()), "--managed", "--capabilities"],
    cwd=tmp_path,
    env=env,
    capture_output=True,
    text=True,
    timeout=10,
    check=False,
  )
  assert result.returncode == 2
  assert result.stdout == ""
  assert list(tmp_path.iterdir()) == []
