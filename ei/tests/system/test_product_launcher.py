"""Product launcher and read-only control command behavior."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path
from typing import Self

import pytest

from island import boot
from island.cfg import ConfigError, load
from island.path import Layout, Mode
from island.share import SharedResources, require_eos_contracts


def test_repository_product_config_declares_shared_resource_path() -> None:
  root = Path(__file__).resolve().parents[2]

  with (root / "etc/island.toml").open("rb") as stream:
    product = tomllib.load(stream)

  assert product["share"] == ["share", "../share"]
  assert product["components"]["eos"] == {
    "enabled": True,
    "launch": "../bin/eosd",
    "ready": "127.0.0.1:16003",
    "ready_timeout_s": 120,
  }


def test_shared_roots_reject_duplicate_relative_files(tmp_path: Path) -> None:
  first = tmp_path / "first"
  second = tmp_path / "second"
  (first / "contracts").mkdir(parents=True)
  (second / "contracts").mkdir(parents=True)
  (first / "contracts/example.json").write_text("one", encoding="utf-8")
  (second / "contracts/example.json").write_text("two", encoding="utf-8")
  config_path = _config(tmp_path / "island.toml")
  config_path.write_text(
    config_path.read_text(encoding="utf-8").replace(
      'share = ["share"]', 'share = ["first", "second"]'
    ),
    encoding="utf-8",
  )
  product = load(config_path)
  layout = Layout.resolve(mode=Mode.SOURCE, prefix=tmp_path, environ={})

  with pytest.raises(ConfigError, match="duplicate shared resource contracts"):
    SharedResources.resolve(product, layout)


def test_shared_roots_merge_disjoint_files_in_one_logical_directory(
  tmp_path: Path,
) -> None:
  first = tmp_path / "first"
  second = tmp_path / "second"
  (first / "contracts/island").mkdir(parents=True)
  (second / "contracts/google").mkdir(parents=True)
  (first / "contracts/island/gateway.proto").write_text("gateway", encoding="utf-8")
  (second / "contracts/google/http.proto").write_text("http", encoding="utf-8")
  config_path = _config(tmp_path / "island.toml")
  config_path.write_text(
    config_path.read_text(encoding="utf-8").replace(
      'share = ["share"]', 'share = ["first", "second"]'
    ),
    encoding="utf-8",
  )
  resources = SharedResources.resolve(
    load(config_path),
    Layout.resolve(mode=Mode.SOURCE, prefix=tmp_path, environ={}),
  )

  assert [path.as_posix() for path, _source in resources.under("contracts")] == [
    "contracts/google/http.proto",
    "contracts/island/gateway.proto",
  ]


def test_current_eos_capability_contract_is_admitted_before_launch(
  tmp_path: Path,
) -> None:
  (tmp_path / "eos/contracts/eos").mkdir(parents=True)
  (tmp_path / "eos/schemas/eos/actions").mkdir(parents=True)
  for filename in ("gateway.proto", "capabilities.proto", "skills.proto"):
    (tmp_path / f"eos/contracts/eos/{filename}").write_text(
      'syntax = "proto3";', encoding="utf-8"
    )
  (tmp_path / "eos/schemas/eos/actions/ack.v1.json").write_text("{}", encoding="utf-8")
  config_path = _config(tmp_path / "island.toml", eos=True)
  config_path.write_text(
    config_path.read_text(encoding="utf-8").replace(
      'share = ["share"]', 'share = ["."]'
    ),
    encoding="utf-8",
  )
  resources = SharedResources.resolve(
    load(config_path), Layout.resolve(mode=Mode.SOURCE, prefix=tmp_path, environ={})
  )

  contracts = require_eos_contracts(resources)
  assert [path.name for path in contracts] == [
    "gateway.proto",
    "capabilities.proto",
    "skills.proto",
    "ack.v1.json",
  ]


def test_enabled_eos_requires_gateway_and_skill_contracts_in_one_root(
  tmp_path: Path,
) -> None:
  first = tmp_path / "first"
  second = tmp_path / "second"
  (first / "eos/contracts/eos").mkdir(parents=True)
  (first / "eos/schemas/eos/actions").mkdir(parents=True)
  (second / "eos/contracts/eos").mkdir(parents=True)
  (first / "eos/contracts/eos/gateway.proto").write_text("gateway", encoding="utf-8")
  (first / "eos/contracts/eos/capabilities.proto").write_text(
    "capabilities", encoding="utf-8"
  )
  (first / "eos/schemas/eos/actions/ack.v1.json").write_text("{}", encoding="utf-8")
  (second / "eos/contracts/eos/skills.proto").write_text("skills", encoding="utf-8")
  (tmp_path / "bin").mkdir()
  eos = tmp_path / "bin/eosd"
  eos.write_text("#!/bin/sh\n", encoding="utf-8")
  eos.chmod(0o755)
  config_path = _config(tmp_path / "island.toml", egia=False, eos=True)
  config_path.write_text(
    config_path.read_text(encoding="utf-8").replace(
      'share = ["share"]', 'share = ["first", "second"]'
    ),
    encoding="utf-8",
  )
  layout = Layout.resolve(mode=Mode.SOURCE, prefix=tmp_path, environ={})
  product = load(config_path)
  resources = SharedResources.resolve(product, layout)

  with pytest.raises(ConfigError, match="one configured shared root"):
    boot.components(product, layout, shared_resources=resources)


def _config(path: Path, *, egia: bool = True, eos: bool = False) -> Path:
  """Write one minimal product config for command tests."""
  path.write_text(
    'name = "Island-egia"\n'
    'profile = "sandbox"\n'
    'share = ["share"]\n\n'
    "[components.egia]\n"
    f"enabled = {str(egia).lower()}\n"
    'launch = "bin/egia"\n\n'
    "[components.eos]\n"
    f"enabled = {str(eos).lower()}\n"
    'launch = "bin/eosd"\n',
    encoding="utf-8",
  )
  return path


class _FakeChild:
  """Deterministic ``Popen`` substitute for supervision tests."""

  def __init__(
    self,
    poll_results: list[int | None],
    *,
    timeout_on_wait: bool = False,
  ) -> None:
    self.pid = 4242
    self._poll_results = poll_results
    self._poll_index = 0
    self._timeout_on_wait = timeout_on_wait
    self.poll_calls = 0
    self.terminate_calls = 0
    self.kill_calls = 0
    self.wait_calls: list[float | None] = []

  def poll(self) -> int | None:
    self.poll_calls += 1
    result = self._poll_results[self._poll_index]
    if self._poll_index < len(self._poll_results) - 1:
      self._poll_index += 1
    return result

  def terminate(self) -> None:
    self.terminate_calls += 1

  def kill(self) -> None:
    self.kill_calls += 1

  def wait(self, timeout: float | None = None) -> int:
    self.wait_calls.append(timeout)
    if self._timeout_on_wait and timeout is not None and self.kill_calls == 0:
      raise boot.subprocess.TimeoutExpired("fake-component", timeout)
    return -9 if self.kill_calls else 0


def _component(name: str) -> boot.Component:
  """Build one enabled component without consulting the filesystem."""
  path = Path("/components") / name
  return boot.Component(name=name, enabled=True, path=path, argv=(str(path),))


def test_boot_layers_factory_config_and_supervises_egia_checkout_command(
  tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
  root = Path(__file__).resolve().parents[2]
  layout = Layout.resolve(mode=Mode.SOURCE, prefix=root, environ={})
  product = tmp_path / "island.toml"
  product.write_text("[components.egia]\nenabled = true\n", encoding="utf-8")
  called: list[tuple[tuple[str, ...], dict[str, str]]] = []

  class FakeCollector:
    """No-op session collector for command construction."""

    def __init__(self, *_args: object) -> None:
      pass

    def __enter__(self) -> Self:
      return self

    def __exit__(self, *_args: object) -> None:
      pass

    def event(self, *_args: object, **_kwargs: object) -> None:
      pass

    def environment(self, component: str) -> tuple[tuple[str, str], ...]:
      return (("ISLAND_COMPONENT", component),)

    def register(self, _pid: int, _component: str) -> None:
      pass

  def fake_popen(argv: tuple[str, ...], **kwargs: object) -> _FakeChild:
    called.append((argv, kwargs["env"]))  # type: ignore[arg-type]
    return _FakeChild([0])

  monkeypatch.setattr(boot, "SessionLogCollector", FakeCollector)
  monkeypatch.setattr(boot.subprocess, "Popen", fake_popen)
  assert (
    boot.main(
      ["--config", str(product), "--", "--no-tui"],
      layout=layout,
    )
    == 0
  )

  command = str(root / "bin/egia")
  assert called[0][0] == (
    command,
    "--no-tui",
    "--island-config",
    str(product),
    "--mode",
    "live",
  )
  assert called[0][1]["ISLAND_COMPONENT"] == "egia"


def test_sibling_eos_launcher_receives_its_own_source_prefix() -> None:
  root = Path(__file__).resolve().parents[2]
  layout = Layout.resolve(mode=Mode.SOURCE, prefix=root, environ={})
  product = load(root / "etc/island.toml", defaults=root / "share/defaults/island.toml")

  eos = next(item for item in boot.components(product, layout) if item.name == "eos")

  assert [item.name for item in boot.components(product, layout)] == [
    "eos",
    "egia",
  ]
  assert eos.ready == "127.0.0.1:16003"
  assert eos.ready_timeout_s == 120
  assert dict(eos.environment) == {
    "ISLAND_PREFIX": str((root / "..").resolve()),
    "ISLAND_MODE": "source",
    "ISLAND_STATE": str(layout.state),
    "ISLAND_CACHE": str(layout.cache),
    "ISLAND_LOG": str(layout.log),
    "ISLAND_RUN": str(layout.run),
    "ISLAND_TMP": str(layout.tmp),
  }


def test_all_components_receive_resolved_mutable_product_roots(tmp_path: Path) -> None:
  layout = Layout.resolve(mode=Mode.SOURCE, prefix=tmp_path, environ={})
  product_path = _config(tmp_path / "island.toml", eos=True)
  product = load(product_path)

  items = boot.components(product, layout)

  expected = {
    "ISLAND_STATE": str(layout.state),
    "ISLAND_CACHE": str(layout.cache),
    "ISLAND_LOG": str(layout.log),
    "ISLAND_RUN": str(layout.run),
    "ISLAND_TMP": str(layout.tmp),
  }
  for item in items:
    assert expected.items() <= dict(item.environment).items()


def test_product_config_recursively_layers_partial_deployment_overlay(
  tmp_path: Path,
) -> None:
  root = Path(__file__).resolve().parents[2]
  overlay = tmp_path / "island.toml"
  overlay.write_text(
    'profile = "fleet"\n[components.eos]\nenabled = true\n',
    encoding="utf-8",
  )

  product = load(overlay, defaults=root / "share/defaults/island.toml")

  assert product.name == "IsOS"
  assert product.profile == "fleet"
  assert product.share == ("share",)
  assert product.components.egia.enabled is True
  assert product.components.eos.enabled is True


@pytest.mark.parametrize(
  ("overlay", "message"),
  (
    ("profile = 7\n", "profile must be a non-empty string"),
    (
      '[components.egia]\nenabled = "yes"\n',
      "components.egia.enabled must be a boolean",
    ),
    ("[components]\nplanner = true\n", "unknown components: planner"),
  ),
)
def test_product_config_validates_deployment_overlay_after_layering(
  tmp_path: Path, overlay: str, message: str
) -> None:
  root = Path(__file__).resolve().parents[2]
  deployment = tmp_path / "island.toml"
  deployment.write_text(overlay, encoding="utf-8")

  with pytest.raises(ConfigError, match=message):
    load(deployment, defaults=root / "share/defaults/island.toml")


def test_islandctl_layers_factory_config_below_partial_overlay(
  tmp_path: Path,
) -> None:
  root = Path(__file__).resolve().parents[2]
  product = tmp_path / "island.toml"
  product.write_text('profile = "fleet"\n', encoding="utf-8")

  result = subprocess.run(
    [str(root / "bin/islandctl"), "--config", str(product), "config"],
    cwd=tmp_path,
    check=True,
    capture_output=True,
    text=True,
  )

  assert "name=IsOS" in result.stdout
  assert "profile=fleet" in result.stdout
  assert "egia.enabled=true" in result.stdout
  assert "eos.enabled=false" in result.stdout


def test_multi_component_exit_does_not_stop_other_failure_domain(
  monkeypatch: pytest.MonkeyPatch,
) -> None:
  failed = _FakeChild([7])
  healthy = _FakeChild([None, 0])
  pending = iter((failed, healthy))
  launched: list[tuple[str, ...]] = []
  sleeps: list[float] = []

  def fake_popen(argv: tuple[str, ...], **_kwargs) -> _FakeChild:
    launched.append(argv)
    return next(pending)

  monkeypatch.setattr(boot.subprocess, "Popen", fake_popen)
  monkeypatch.setattr(boot.time, "sleep", sleeps.append)

  code = boot.launch((_component("egia"), _component("eos")))

  assert code == 7
  assert launched == [("/components/egia",), ("/components/eos",)]
  assert healthy.poll_calls == 2
  assert sleeps == [0.1]
  assert failed.terminate_calls == healthy.terminate_calls == 0
  assert failed.kill_calls == healthy.kill_calls == 0
  assert failed.wait_calls == healthy.wait_calls == []


def test_clean_egia_exit_stops_composed_eos(
  monkeypatch: pytest.MonkeyPatch,
) -> None:
  egia = _FakeChild([0])
  eos = _FakeChild([None])
  pending = iter((egia, eos))

  def fake_popen(_argv: tuple[str, ...], **_kwargs) -> _FakeChild:
    return next(pending)

  monkeypatch.setattr(boot.subprocess, "Popen", fake_popen)

  code = boot.launch((_component("egia"), _component("eos")))

  assert code == 0
  assert egia.terminate_calls == 0
  assert eos.terminate_calls == 1
  assert egia.wait_calls == [5]
  assert eos.wait_calls == [5]


def test_composed_launch_waits_for_eos_listener_before_starting_egia(
  monkeypatch: pytest.MonkeyPatch,
) -> None:
  eos = boot.Component(
    name="eos",
    enabled=True,
    path=Path("components/eos"),
    argv=("components/eos",),
    ready="127.0.0.1:16003",
  )
  egia = boot.Component(
    name="egia",
    enabled=True,
    path=Path("components/egia"),
    argv=("components/egia",),
  )
  eos_child = _FakeChild([None])
  egia_child = _FakeChild([0])
  pending = iter((eos_child, egia_child))
  events: list[str] = []

  def fake_popen(argv: tuple[str, ...], **_kwargs) -> _FakeChild:
    events.append(f"start:{argv[0]}")
    return next(pending)

  def fake_wait(component: boot.Component, _child: _FakeChild) -> None:
    events.append(f"ready:{component.name}")

  monkeypatch.setattr(boot.subprocess, "Popen", fake_popen)
  monkeypatch.setattr(boot, "_wait_until_ready", fake_wait)

  assert boot.launch((eos, egia)) == 0
  assert events == [
    "start:components/eos",
    "ready:eos",
    "start:components/egia",
  ]


def test_keyboard_interrupt_stops_only_remaining_component_children(
  monkeypatch: pytest.MonkeyPatch,
) -> None:
  exited = _FakeChild([0])
  running = _FakeChild([None], timeout_on_wait=True)
  pending = iter((exited, running))

  def fake_popen(_argv: tuple[str, ...], **_kwargs) -> _FakeChild:
    return next(pending)

  def interrupt(_seconds: float) -> None:
    raise KeyboardInterrupt

  monkeypatch.setattr(boot.subprocess, "Popen", fake_popen)
  monkeypatch.setattr(boot.time, "sleep", interrupt)

  code = boot.launch((_component("worker"), _component("eos")))

  assert code == 130
  assert exited.terminate_calls == 0
  assert running.terminate_calls == 1
  assert exited.kill_calls == 0
  assert running.kill_calls == 1
  assert exited.wait_calls == [5]
  assert running.wait_calls == [5, None]


def test_island_legacy_config_points_to_import(
  tmp_path: Path,
) -> None:
  root = Path(__file__).resolve().parents[2]
  product = _config(tmp_path / "island.toml", eos=True)

  result = subprocess.run(
    [str(root / "bin/island"), "--config", str(product)],
    cwd=tmp_path,
    check=False,
    capture_output=True,
    text=True,
    timeout=5,
  )

  assert result.returncode == 2
  assert "old combined launcher" in result.stderr
  assert "isos config init" in result.stderr


def test_islandctl_rejects_malformed_component_flag(tmp_path: Path) -> None:
  root = Path(__file__).resolve().parents[2]
  product = tmp_path / "island.toml"
  product.write_text(
    'name = "Island-egia"\nprofile = "sandbox"\nshare = ["share"]\n'
    '[components.egia]\nenabled = "yes"\nlaunch = "bin/egia"\n'
    '[components.eos]\nenabled = false\nlaunch = "bin/eosd"\n',
    encoding="utf-8",
  )

  result = subprocess.run(
    [str(root / "bin/islandctl"), "--config", str(product), "validate"],
    cwd=tmp_path,
    check=False,
    capture_output=True,
    text=True,
  )

  assert result.returncode == 2
  assert "components.egia.enabled must be a boolean" in result.stderr


def test_wrappers_load_installed_python_tree_from_system_prefix(
  tmp_path: Path,
) -> None:
  root = Path(__file__).resolve().parents[2]
  prefix = tmp_path / "prefix"
  commands = prefix / "bin"
  package_root = prefix / "lib/island"
  commands.mkdir(parents=True)
  package_root.mkdir(parents=True)
  shutil.copy2(root / "bin/island", commands / "island")
  shutil.copy2(root / "bin/islandctl", commands / "islandctl")
  shutil.copytree(root / "src/island", package_root / "island")
  elsewhere = tmp_path / "elsewhere"
  elsewhere.mkdir()
  env = dict(os.environ)
  env.pop("PYTHONPATH", None)
  env.pop("ISLAND_MODE", None)
  env["ISLAND_PYTHON"] = sys.executable

  ctl = subprocess.run(
    [str(commands / "islandctl"), "paths"],
    cwd=elsewhere,
    env=env,
    check=True,
    capture_output=True,
    text=True,
  )
  launch_help = subprocess.run(
    [str(commands / "island"), "--help"],
    cwd=elsewhere,
    env=env,
    check=True,
    capture_output=True,
    text=True,
  )

  assert "mode=system" in ctl.stdout
  assert f"prefix={prefix}" in ctl.stdout
  assert f"libexec={prefix / 'libexec/island'}" in ctl.stdout
  assert "Start the selected IsOS deployment" in launch_help.stdout


def test_control_wrapper_detects_portable_role_tree(tmp_path: Path) -> None:
  root = Path(__file__).resolve().parents[2]
  prefix = tmp_path / "bundle"
  commands = prefix / "bin"
  package_root = prefix / "lib/island"
  commands.mkdir(parents=True)
  package_root.mkdir(parents=True)
  (prefix / "share/contracts").mkdir(parents=True)
  (prefix / "libexec").mkdir()
  shutil.copy2(root / "bin/islandctl", commands / "islandctl")
  shutil.copytree(root / "src/island", package_root / "island")
  elsewhere = tmp_path / "elsewhere"
  elsewhere.mkdir()
  env = dict(os.environ)
  env.pop("PYTHONPATH", None)
  env.pop("ISLAND_MODE", None)
  env["ISLAND_PYTHON"] = sys.executable

  result = subprocess.run(
    [str(commands / "islandctl"), "paths"],
    cwd=elsewhere,
    env=env,
    check=True,
    capture_output=True,
    text=True,
  )

  assert "mode=portable" in result.stdout
  assert f"share={prefix / 'share'}" in result.stdout
  assert f"state={prefix / 'var/lib'}" in result.stdout
