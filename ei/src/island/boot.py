"""Compose independently executable Island components from product config."""

from __future__ import annotations

import argparse
import os
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from island.cfg import ConfigError, ProductConfig, load
from island.path import Layout
from island.session_log import SessionLogCollector
from island.share import SharedResources, require_eos_contracts


class BootError(RuntimeError):
  """Raised when an enabled component cannot be launched safely."""


@dataclass(frozen=True, slots=True)
class Component:
  """One configured component command and its availability state."""

  name: str
  enabled: bool
  path: Path
  argv: tuple[str, ...]
  environment: tuple[tuple[str, str], ...] = ()
  ready: str | None = None
  ready_timeout_s: float = 30.0

  @property
  def available(self) -> bool:
    """Whether the command exists as an executable regular file."""
    return self.path.is_file() and os.access(self.path, os.X_OK)


def config_path(layout: Layout, value: str | None) -> Path:
  """Resolve product config without consulting the current directory."""
  if value is None:
    return layout.etc / "island.toml"
  path = Path(value).expanduser()
  return path.resolve() if path.is_absolute() else (layout.prefix / path).resolve()


def factory_config_path(layout: Layout) -> Path:
  """Return the product factory config below the injected share root."""
  return layout.share / "defaults/island.toml"


def components(
  config: ProductConfig,
  layout: Layout,
  egia_args: tuple[str, ...] = (),
  *,
  product_config: Path | None = None,
  shared_resources: SharedResources | None = None,
) -> tuple[Component, ...]:
  """Describe EGIA and EOS commands without starting either process."""
  egia_path = (layout.prefix / config.components.egia.launch).resolve()
  eos_path = (layout.prefix / config.components.eos.launch).resolve()
  eos_prefix = eos_path.parent.parent
  mutable_environment = (
    ("ISLAND_STATE", str(layout.state)),
    ("ISLAND_CACHE", str(layout.cache)),
    ("ISLAND_LOG", str(layout.log)),
    ("ISLAND_RUN", str(layout.run)),
    ("ISLAND_TMP", str(layout.tmp)),
  )
  eos_environment: tuple[tuple[str, str], ...] = mutable_environment
  if (
    shared_resources is not None
    and config.components.eos.enabled
    and eos_path.is_file()
  ):
    gateway, capabilities, skills, acknowledgement = require_eos_contracts(
      shared_resources
    )
    resource_root = next(
      root for root in shared_resources.roots if gateway.is_relative_to(root)
    )
    if any(
      not path.is_relative_to(resource_root)
      for path in (capabilities, skills, acknowledgement)
    ):
      raise ConfigError(
        "enabled EOS requires gateway, capability, skill, and acknowledgement "
        "contracts in one configured shared root"
      )
    eos_environment = (
      *eos_environment,
      ("ISLAND_SHARE", str(resource_root)),
      ("EOS_CONTRACTS", str(resource_root)),
    )
  if eos_prefix != layout.prefix and (eos_prefix / "adapters/eos-cxx").is_dir():
    eos_environment = (
      ("ISLAND_PREFIX", str(eos_prefix)),
      ("ISLAND_MODE", "source"),
      *eos_environment,
    )
  return (
    Component(
      name="eos",
      enabled=config.components.eos.enabled,
      path=eos_path,
      argv=(str(eos_path),),
      environment=eos_environment,
      ready=config.components.eos.ready,
      ready_timeout_s=config.components.eos.ready_timeout_s,
    ),
    Component(
      name="egia",
      enabled=config.components.egia.enabled,
      path=egia_path,
      argv=(
        str(egia_path),
        *egia_args,
        *(("--island-config", str(product_config)) if product_config else ()),
        "--mode",
        config.profile,
      ),
      environment=mutable_environment,
    ),
  )


def preflight(items: tuple[Component, ...]) -> tuple[Component, ...]:
  """Return enabled commands only after every one is available."""
  enabled = tuple(item for item in items if item.enabled)
  if not enabled:
    raise BootError("no components are enabled in island.toml")
  missing = next((item for item in enabled if not item.available), None)
  if missing is not None:
    raise BootError(
      f"enabled component {missing.name!r} is unavailable: "
      f"expected executable {missing.path}"
    )
  return enabled


def launch(
  items: tuple[Component, ...], *, collector: SessionLogCollector | None = None
) -> int:
  """Supervise all configured component processes.

  A clean EGIA exit represents an operator closing its foreground TUI, so the
  composed launcher also stops the remaining owned children. Component
  failures remain isolated so the surviving process stays available for
  diagnostics.
  """
  children: list[subprocess.Popen[bytes]] = []

  def stop_running() -> None:
    """Terminate and reap every child that has not already exited."""
    for child in children:
      if child.poll() is None:
        if collector is not None:
          collector.event("component_terminate", pid=getattr(child, "pid", 0))
        child.terminate()
    for child in children:
      try:
        child.wait(timeout=5)
      except subprocess.TimeoutExpired:
        if collector is not None:
          collector.event(
            "component_kill", pid=getattr(child, "pid", 0), level="warning"
          )
        child.kill()
        child.wait()

  try:
    for item in items:
      environment = dict(item.environment)
      if collector is not None:
        environment.update(collector.environment(item.name))
      child = subprocess.Popen(item.argv, env={**os.environ, **environment})
      children.append(child)
      if collector is not None:
        collector.register(child.pid, item.name)
        collector.event("component_spawned", component_name=item.name, pid=child.pid)
      if item.ready is not None:
        _wait_until_ready(item, child)
        if collector is not None:
          collector.event("component_ready", component_name=item.name, pid=child.pid)
    reported: set[int] = set()
    while True:
      statuses = tuple(child.poll() for child in children)
      if collector is not None:
        for index, status in enumerate(statuses):
          if status is not None and index not in reported:
            reported.add(index)
            collector.event(
              "component_exited",
              component_name=items[index].name,
              pid=children[index].pid,
              exit_code=status,
              level="info" if status == 0 else "error",
            )
      completed = tuple(code for code in statuses if code is not None)
      if len(completed) == len(children):
        return next((code for code in completed if code != 0), 0)
      egia_index = next(
        (index for index, item in enumerate(items) if item.name == "egia"), None
      )
      if egia_index is not None and statuses[egia_index] == 0:
        stop_running()
        return 0
      time.sleep(0.1)
  except KeyboardInterrupt:
    if collector is not None:
      collector.event("session_interrupted", level="warning")
    stop_running()
    return 130
  except BootError:
    if collector is not None:
      collector.event("session_launch_failed", level="error")
    stop_running()
    raise


def _wait_until_ready(component: Component, child: subprocess.Popen[bytes]) -> None:
  """Wait for one owned component's configured TCP listener to accept connections."""
  host, separator, raw_port = (
    component.ready.rpartition(":") if component.ready else ("", "", "")
  )
  if not separator or not host or not raw_port.isdecimal():
    raise BootError(
      f"component {component.name!r} has invalid readiness endpoint {component.ready!r}; "
      "expected host:port"
    )
  port = int(raw_port)
  if not 1 <= port <= 65535:
    raise BootError(
      f"component {component.name!r} has invalid readiness endpoint {component.ready!r}; "
      "port must be between 1 and 65535"
    )
  deadline = time.monotonic() + component.ready_timeout_s
  while True:
    if child.poll() is not None:
      raise BootError(
        f"component {component.name!r} exited before its listener {component.ready} became ready"
      )
    try:
      with socket.create_connection((host, port), timeout=0.2):
        return
    except OSError:
      if time.monotonic() >= deadline:
        raise BootError(
          f"component {component.name!r} did not make listener {component.ready} ready within "
          f"{component.ready_timeout_s:g}s"
        )
      time.sleep(0.1)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
  """Parse product launch options and EGIA pass-through arguments."""
  parser = argparse.ArgumentParser(
    prog="island",
    description="Launch enabled Island-egia components",
  )
  parser.add_argument("--config", default=None, help="product config path")
  parser.add_argument(
    "args",
    nargs=argparse.REMAINDER,
    help="arguments after -- are forwarded to the EGIA checkout command",
  )
  return parser.parse_args(argv)


def main(argv: list[str] | None = None, *, layout: Layout | None = None) -> int:
  """Load the product plan, refuse incomplete stacks, and launch."""
  args = parse_args(argv)
  resolved = layout or Layout.resolve()
  forwarded = tuple(args.args[1:] if args.args[:1] == ["--"] else args.args)
  try:
    selected_config = config_path(resolved, args.config)
    config = load(
      selected_config,
      defaults=factory_config_path(resolved),
    )
    shared = SharedResources.resolve(config, resolved)
    plan = preflight(
      components(
        config,
        resolved,
        forwarded,
        product_config=selected_config,
        shared_resources=shared,
      )
    )
    try:
      with SessionLogCollector(resolved, config.logging) as collector:
        collector.event("session_started", components=[item.name for item in plan])
        result = launch(plan, collector=collector)
        collector.event("session_finished", exit_code=result)
        return result
    except RuntimeError as exc:
      raise BootError(str(exc)) from exc
  except (BootError, ConfigError, ValueError) as exc:
    print(f"island: {exc}", file=sys.stderr)
    return 2


if __name__ == "__main__":
  raise SystemExit(main())
