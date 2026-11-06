"""Foreground launcher for the external EOS diagnostic REPL.

The TUI depends only on EOS's public ``eos-repl`` wrapper. It does not import
EOS code or attempt to reproduce the wrapper's source/installed layout logic.
"""

from __future__ import annotations

import asyncio
import os
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class ReplEndpoint:
  """Parsed EOS gRPC endpoint passed to the diagnostic client."""

  host: str
  port: int


@dataclass(frozen=True)
class ReplLaunchResult:
  """Outcome of attempting to run one foreground EOS REPL session."""

  argv: tuple[str, ...]
  exit_code: int | None = None
  error: str | None = None

  @property
  def launched(self) -> bool:
    """Return whether the REPL process was successfully started."""
    return self.exit_code is not None

  @property
  def succeeded(self) -> bool:
    """Return whether the REPL started and exited successfully."""
    return self.exit_code == 0 and self.error is None


def resolve_repl_wrapper(eos_launcher: Path | str) -> Path:
  """Resolve the public ``eos-repl`` wrapper beside an EOS launcher.

  The configured launcher may belong to either a source checkout or an
  installed prefix; both layouts place public command wrappers together.

  Raises:
      ValueError: If the sibling wrapper is missing or not executable.
  """
  launcher = Path(eos_launcher).expanduser()
  wrapper = launcher.parent / "eos-repl"
  if not wrapper.is_file():
    raise ValueError(f"EOS REPL wrapper not found: {wrapper}")
  if not os.access(wrapper, os.X_OK):
    raise ValueError(f"EOS REPL wrapper is not executable: {wrapper}")
  return wrapper.resolve()


def parse_repl_endpoint(endpoint: str) -> ReplEndpoint:
  """Parse ``host:port`` or ``[IPv6]:port`` into REPL arguments.

  Raises:
      ValueError: If the endpoint has no valid host or TCP port.
  """
  value = endpoint.strip()
  host: str
  port_text: str
  if value.startswith("["):
    closing = value.find("]")
    if closing < 0 or closing + 1 >= len(value) or value[closing + 1] != ":":
      raise ValueError(f"invalid EOS endpoint {endpoint!r}; expected [host]:port")
    host = value[1:closing]
    port_text = value[closing + 2 :]
  else:
    if value.count(":") != 1:
      raise ValueError(f"invalid EOS endpoint {endpoint!r}; expected host:port")
    host, port_text = value.rsplit(":", 1)
  if not host:
    raise ValueError(f"invalid EOS endpoint {endpoint!r}; host is empty")
  try:
    port = int(port_text)
  except ValueError as exc:
    raise ValueError(
      f"invalid EOS endpoint {endpoint!r}; port is not an integer"
    ) from exc
  if not 1 <= port <= 65535:
    raise ValueError(f"invalid EOS endpoint {endpoint!r}; port must be 1..65535")
  return ReplEndpoint(host=host, port=port)


async def launch_repl(
  eos_launcher: Path | str,
  endpoint: str,
) -> ReplLaunchResult:
  """Run EOS's REPL in the foreground with the caller's terminal streams.

  Resolution, validation, and process-start failures are returned rather than
  raised so the console can restore curses and present actionable feedback.
  """
  try:
    wrapper = resolve_repl_wrapper(eos_launcher)
    target = parse_repl_endpoint(endpoint)
  except (OSError, ValueError) as exc:
    return ReplLaunchResult(argv=(), error=str(exc))

  argv = (
    str(wrapper),
    "--host",
    target.host,
    "--port",
    str(target.port),
  )
  # The product launcher exports EGIA's prefix to the TUI. EOS's public
  # wrapper also consumes ISLAND_PREFIX, so inheriting that value makes a
  # sibling EOS checkout search EGIA's libexec tree and exit immediately.
  # Anchor the wrapper to the configured EOS launcher's own prefix instead.
  eos_prefix = Path(eos_launcher).expanduser().resolve().parent.parent
  environment = {**os.environ, "ISLAND_PREFIX": str(eos_prefix)}
  if (eos_prefix / "adapters/eos-cxx").is_dir():
    environment["ISLAND_MODE"] = "source"
  try:
    process = await asyncio.create_subprocess_exec(*argv, env=environment)
  except OSError as exc:
    return ReplLaunchResult(argv=argv, error=f"failed to start EOS REPL: {exc}")
  return ReplLaunchResult(argv=argv, exit_code=await process.wait())
