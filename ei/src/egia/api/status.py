"""EGIA server status contract — the wire model for the health check.

Declaration only, per the interface-layer purity rule: this module holds the
Pydantic v2 model that ``egia.io`` serializes on the
``/api/aether/egia/health`` WebSocket and that northbound clients (WebUI) parse.
It performs no I/O and holds no state. The wire spec lives in
``docs/egia/northbound.md``; update both in the same PR.
"""

from __future__ import annotations

from datetime import datetime
from enum import StrEnum

from pydantic import BaseModel, Field


class HealthState(StrEnum):
  """Liveness states reported by the health route.

  ``STARTING`` covers the window between process start and pipeline readiness;
  ``READY`` means the decision pipeline accepts work; ``DEGRADED`` means the
  server is live but a non-fatal subsystem (e.g. an ``opt/`` package) is down;
  ``STOPPING`` is reported during graceful shutdown.
  """

  STARTING = "starting"
  READY = "ready"
  DEGRADED = "degraded"
  STOPPING = "stopping"


class SubsystemStatus(BaseModel):
  """Per-subsystem health projected from the core supervisor.

  ``state`` reuses :class:`HealthState` literals: ``starting`` until the
  subsystem signals readiness, ``ready`` while its loop runs, ``degraded``
  when its task failed and awaits restart, ``stopping`` during shutdown.
  """

  name: str
  state: HealthState
  uptime_seconds: float = Field(
    ge=0, description="seconds since the subsystem became ready"
  )
  restarts: int = Field(
    ge=0, description="times the supervisor restarted this subsystem"
  )
  last_error: str | None = Field(
    default=None, description="most recent failure, if any"
  )


class SafetyStatus(BaseModel):
  """Gate verdict counters projected into the health stream.

  ``admitted`` / ``rejected`` count workload invocations the safety gate has
  passed or refused since launch; ``estop_engaged`` is true while the
  emergency stop holds (all workload egress rejected).
  """

  admitted: int = Field(ge=0, description="workload calls the gate admitted")
  rejected: int = Field(ge=0, description="workload calls the gate rejected")
  estop_engaged: bool = Field(description="emergency stop currently holding")


class BackendConnection(StrEnum):
  """Southbound EOS connectivity as the monitor sees it.

  ``UP`` — the last heartbeat probe succeeded. ``DEGRADED`` — probes are
  failing but below the escalation threshold. ``DOWN`` — failures reached
  the threshold and the fault escalated. ``UNKNOWN`` — no probe has run yet
  (pipeline starting, or no monitor wired).
  """

  UP = "up"
  DEGRADED = "degraded"
  DOWN = "down"
  UNKNOWN = "unknown"


class BackendDeviceStatus(BaseModel):
  """One EOS device advertised by the current runtime process."""

  device_id: str
  device_class: str
  part_id: str
  description: str
  state: str
  health: str
  primitives: list[str] = Field(default_factory=list)


class BackendStatus(BaseModel):
  """Backend link snapshot served on the ``/backend`` stream.

  ``backend`` is the configured endpoint id; ``connection`` derives from the
  monitor's heartbeat results; ``consecutive_failures`` is the raw probe
  streak so clients can apply their own thresholds. ``name`` /
  ``description`` are the endpoint's robot profile (``[[backends]]`` name /
  intro): additive and null when the deployment profiles nothing.
  """

  backend: str = Field(default="default", description="configured backend endpoint id")
  connection: BackendConnection
  endpoint: str = Field(description="EOS plaintext gRPC endpoint")
  eos_robot_id: str | None = Field(
    default=None, description="stable robot identity advertised by EOS"
  )
  robot_name: str | None = Field(
    default=None, description="robot name advertised by EOS"
  )
  eos_started_at: str | None = Field(
    default=None, description="EOS process generation timestamp"
  )
  eos_version: str | None = None
  robot_model: str | None = None
  robot_description: str | None = None
  robot_state: str | None = None
  robot_health: str | None = None
  estop: bool | None = None
  power_pct: float | None = None
  devices: list[BackendDeviceStatus] = Field(default_factory=list)
  name: str | None = Field(
    default=None, description="robot profile name from the endpoint config"
  )
  description: str | None = Field(
    default=None, description="robot profile intro from the endpoint config"
  )
  last_heartbeat_at: datetime | None = Field(
    default=None, description="last successful probe time, UTC; None before the first"
  )
  consecutive_failures: int = Field(
    default=0, ge=0, description="consecutive failed heartbeats at the last probe"
  )


class BackendStatusFrame(BaseModel):
  """One ``/backend`` stream frame: one status per wired backend endpoint."""

  backends: list[BackendStatus] = Field(
    default_factory=list,
    description="one BackendStatus per configured backend, in config order",
  )


class ServerStatus(BaseModel):
  """Health-check payload served under ``/api/aether/egia/*``.

  Timestamps are RFC3339/ISO-8601 UTC strings on the wire. ``uptime_seconds``
  is measured from pipeline launch, not process start.
  """

  status: HealthState
  version: str = Field(description="EGIA package version, e.g. from importlib.metadata")
  started_at: datetime = Field(description="pipeline launch time, UTC")
  uptime_seconds: float = Field(ge=0, description="seconds since pipeline launch")
  correlation_id: str | None = Field(
    default=None,
    description="correlation ID of the health request, for log cross-reference",
  )
  subsystems: list[SubsystemStatus] = Field(
    default_factory=list,
    description="per-subsystem health reported by the core supervisor",
  )
  safety: SafetyStatus | None = Field(
    default=None,
    description="gate verdict counters; None when no gate is wired",
  )
