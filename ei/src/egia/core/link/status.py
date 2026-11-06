"""Backend link status tracker — the monitor's heartbeat ledger.

The monitor loop records each heartbeat probe result here; the northbound
``/backend`` stream and the context builder's ``backend`` prompt section
both read the latest snapshot. Pure state, no I/O: the tracker knows
nothing about WebSockets or prompts.
"""

from __future__ import annotations

import threading
from collections.abc import Callable
from datetime import UTC, datetime

from egia.api.status import BackendConnection, BackendDeviceStatus, BackendStatus

__all__ = [
  "BackendStatusTracker",
  "backend_section_renderer",
  "backend_section_renderers",
  "render_backend_status",
]


class BackendStatusTracker:
  """Thread-safe ledger of heartbeat results for one backend endpoint."""

  def __init__(
    self,
    endpoint: str,
    failure_threshold: int = 3,
    *,
    backend: str = "default",
    name: str | None = None,
    description: str | None = None,
  ) -> None:
    self._backend = backend
    self._endpoint = endpoint
    self._name = name
    self._description = description
    self._threshold = max(failure_threshold, 1)
    self._failures = 0
    self._last_success: datetime | None = None
    self._runtime: dict = {}
    self._lock = threading.Lock()

  @property
  def backend(self) -> str:
    """The configured backend id this tracker reports under."""
    return self._backend

  def record_success(self) -> None:
    """Record a successful heartbeat probe."""
    with self._lock:
      self._failures = 0
      self._last_success = datetime.now(UTC)

  def record_failure(self) -> None:
    """Record a failed heartbeat probe."""
    with self._lock:
      self._failures += 1

  def update_runtime(self, discovery) -> None:
    """Replace metadata after a successful EOS discovery or rediscovery."""
    devices = [
      BackendDeviceStatus(
        device_id=device.id,
        device_class=device.device_class,
        part_id=device.part_id,
        description=device.description,
        state=device.state,
        health=device.health,
        primitives=list(device.supported_primitives),
      )
      for device in discovery.devices
    ]
    with self._lock:
      self._runtime = {
        "eos_robot_id": discovery.robot_id,
        "robot_name": discovery.robot_name,
        "eos_started_at": discovery.started_at,
        "eos_version": discovery.version,
        "robot_model": discovery.robot_model,
        "robot_description": discovery.robot_description,
        "robot_state": discovery.state,
        "robot_health": discovery.health,
        "estop": discovery.estop,
        "power_pct": discovery.power_pct,
        "devices": devices,
      }

  def update_status(self, status: dict) -> None:
    """Apply live ``GetStatus`` health without inventing runtime identities.

    Current EOS status carries aggregate health and device counts. Discovery
    remains authoritative for per-device identity and health; the client
    rediscovery listener replaces those details when the aggregate changes.
    Per-device identity and health remain owned by atomic discovery snapshots.
    """
    with self._lock:
      state = status.get("status")
      if state in {"ready", "degraded", "unavailable"}:
        self._runtime["robot_health"] = state

  def snapshot(self) -> BackendStatus:
    """Return the current link status (safe from any thread/task)."""
    with self._lock:
      if self._failures == 0:
        connection = (
          BackendConnection.UP if self._last_success else BackendConnection.UNKNOWN
        )
      elif self._failures >= self._threshold:
        connection = BackendConnection.DOWN
      else:
        connection = BackendConnection.DEGRADED
      return BackendStatus(
        backend=self._backend,
        connection=connection,
        endpoint=self._endpoint,
        name=self._name,
        description=self._description,
        last_heartbeat_at=self._last_success,
        consecutive_failures=self._failures,
        **self._runtime,
      )


def render_backend_status(snapshot: BackendStatus) -> str:
  """One-line human/model rendering of a backend status snapshot.

  The default backend keeps the pre-multi-backend line shape; a named
  backend carries its id so multi-backend prompts attribute each line, and
  a profiled robot appends its name — the model can then address robots by
  name instead of endpoint id. The free-text intro stays off this line
  (prompts stay one line per robot); operators read it on the wire.
  """
  label = (
    f"Backend ({snapshot.endpoint})"
    if snapshot.backend == "default"
    else f"Backend {snapshot.backend} ({snapshot.endpoint})"
  )
  if snapshot.name:
    label = f"{label} — {snapshot.name}"
  if snapshot.connection is BackendConnection.UNKNOWN:
    lines = [f"{label}: connection unknown, no heartbeat yet"]
  else:
    detail = (
      "heartbeats healthy"
      if snapshot.consecutive_failures == 0
      else f"{snapshot.consecutive_failures} consecutive heartbeat failures"
    )
    lines = [f"{label}: {snapshot.connection.value} — {detail}"]
  if snapshot.robot_model:
    lines.append(f"  Robot: {snapshot.robot_model} ({snapshot.robot_health})")
  for device in snapshot.devices:
    lines.append(
      f"  {device.device_class} {device.device_id}: {device.health} ({device.state})"
    )
  return "\n".join(lines)


def backend_section_renderer(
  tracker: BackendStatusTracker,
) -> Callable[[], str]:
  """Return a zero-arg callable rendering the tracker's live status."""
  return lambda: render_backend_status(tracker.snapshot())


def backend_section_renderers(
  trackers: dict[str, BackendStatusTracker] | BackendStatusTracker,
) -> Callable[[], str]:
  """Aggregate renderer: one status line per backend tracker, joined by newline.

  A single tracker (or a one-entry mapping) renders exactly the
  single-backend text — the multi-backend prompt shape collapses to today's
  when the deployment wires one endpoint.
  """
  if isinstance(trackers, BackendStatusTracker):
    return backend_section_renderer(trackers)

  def _render() -> str:
    return "\n".join(
      render_backend_status(tracker.snapshot()) for tracker in trackers.values()
    )

  return _render
