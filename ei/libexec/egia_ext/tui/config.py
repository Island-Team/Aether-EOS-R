"""Config for the TUI: read what it needs from ``etc/egia/egia.toml``.

The pane-level log threshold mirrors ``[logging.console] level`` from
``etc/egia/egia.toml`` — the operator pane shows the events the pipeline itself
would print, and turning the console up or down moves both together. It may
be overridden with ``--log-level``. The WebSocket endpoints default to ``[server]``
host/port from the same file. Deliberately plain ``tomllib`` parsing: the
TUI is an operator interface, not a pipeline consumer of ``LaunchConfig``.
"""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from egia.lib.config import SandboxRuntimeConfig

LEVELS = ("DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL")


@dataclass(frozen=True)
class TuiConfig:
  """Everything the console needs to find its data sources."""

  host: str = "127.0.0.1"
  port: int = 16060
  log_level: str = "INFO"
  state_dir: Path = Path("var/lib/egia")
  cache_dir: Path = Path("var/cache/egia")
  log_dir: Path = Path("var/log/egia")
  run_dir: Path = Path("var/run")
  tmp_dir: Path = Path("var/tmp")
  etc_dir: Path = Path("etc")
  sandbox_enabled: bool = False
  sandbox: SandboxRuntimeConfig = field(default_factory=SandboxRuntimeConfig)
  extra: dict = field(default_factory=dict)

  @property
  def replay_path(self) -> Path:
    """Return the merged event stream inside durable component state."""
    return self.state_dir / "egia.jsonl"


def _level_at_least(name: str) -> str:
  """Clamp an arbitrary level name onto the known ladder."""
  upper = name.upper()
  return upper if upper in LEVELS else "WARNING"


def _deep_merge(base: dict, override: dict) -> dict:
  """Recursively layer a deployment document over factory values."""
  merged = dict(base)
  for key, value in override.items():
    if isinstance(value, dict) and isinstance(merged.get(key), dict):
      merged[key] = _deep_merge(merged[key], value)
    else:
      merged[key] = value
  return merged


def load_tui_config(
  etc: Path | str = "etc",
  var: Path | str | None = None,
  *,
  defaults: Path | str | None = None,
  state: Path | str | None = None,
  cache: Path | str | None = None,
  log: Path | str | None = None,
  run: Path | str | None = None,
  tmp: Path | str | None = None,
  log_level: str | None = None,
) -> TuiConfig:
  """Resolve host/port/log threshold from ``<etc>/egia.toml``.

  Factory values are read before the deployment overlay. Mutable roots are
  injected by the console composition root. ``var`` remains a compatibility
  input and splits into five roles when explicit roots are absent.
  """
  etc_dir = Path(etc)
  legacy = Path(var) if var is not None else None
  state_dir = (
    Path(state)
    if state is not None
    else (legacy / "lib/egia" if legacy is not None else TuiConfig.state_dir)
  )
  cache_dir = (
    Path(cache)
    if cache is not None
    else (legacy / "cache/egia" if legacy is not None else TuiConfig.cache_dir)
  )
  log_dir = (
    Path(log)
    if log is not None
    else (legacy / "log/egia" if legacy is not None else TuiConfig.log_dir)
  )
  run_dir = (
    Path(run)
    if run is not None
    else (legacy / "run" if legacy is not None else TuiConfig.run_dir)
  )
  tmp_dir = (
    Path(tmp)
    if tmp is not None
    else (legacy / "tmp" if legacy is not None else TuiConfig.tmp_dir)
  )
  host, port, level = TuiConfig.host, TuiConfig.port, TuiConfig.log_level
  doc: dict = {}
  if defaults is not None:
    factory_file = Path(defaults) / "egia.toml"
    if factory_file.is_file():
      with factory_file.open("rb") as fh:
        doc = tomllib.load(fh)
  egia_file = etc_dir / "egia.toml"
  if egia_file.is_file():
    with egia_file.open("rb") as fh:
      overlay = tomllib.load(fh)
    doc = _deep_merge(doc, overlay)
  server = doc.get("server", {})
  if isinstance(server, dict):
    host = str(server.get("host", host))
    port = int(server.get("port", port))
  logging_console = doc.get("logging", {})
  if isinstance(logging_console, dict):
    sink = logging_console.get("console", {})
    if isinstance(sink, dict) and sink.get("level") is not None:
      level = _level_at_least(str(sink["level"]))
  if log_level is not None:
    level = _level_at_least(log_level)
  launch_doc = doc.get("launch", {})
  sandbox_enabled = bool(
    launch_doc.get("sandbox", False) if isinstance(launch_doc, dict) else False
  )
  sandbox_doc: dict = {}
  if defaults is not None:
    factory_sandbox = Path(defaults) / "opt/sandbox.toml"
    if factory_sandbox.is_file():
      with factory_sandbox.open("rb") as fh:
        sandbox_doc = tomllib.load(fh)
  deployment_sandbox = etc_dir / "opt/sandbox.toml"
  if deployment_sandbox.is_file():
    with deployment_sandbox.open("rb") as fh:
      sandbox_doc = _deep_merge(sandbox_doc, tomllib.load(fh))
  devices = sandbox_doc.get("devices", {})
  device_doc = devices if isinstance(devices, dict) else {}
  sandbox = SandboxRuntimeConfig.model_validate(
    {
      "gateway": sandbox_doc.get("gateway", {}),
      "websocket": sandbox_doc.get("websocket", {}),
      "fallback_timeout_s": sandbox_doc.get("fallback_timeout_s", 5.0),
      "backend_retry_interval_s": sandbox_doc.get("backend_retry_interval_s", 5.0),
      "mic": device_doc.get("mic", {}),
      "camera": device_doc.get("camera", {}),
      "speaker": device_doc.get("speaker", {}),
    }
  )
  return TuiConfig(
    host=host,
    port=port,
    log_level=level,
    state_dir=state_dir,
    cache_dir=cache_dir,
    log_dir=log_dir,
    run_dir=run_dir,
    tmp_dir=tmp_dir,
    etc_dir=etc_dir,
    sandbox_enabled=sandbox_enabled,
    sandbox=sandbox,
  )


def level_value(name: str) -> int:
  """Numeric rank for a level name; unknown names sort as ``WARNING``."""
  return LEVELS.index(_level_at_least(name))
