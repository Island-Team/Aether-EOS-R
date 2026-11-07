"""Shared pipeline status projected to the northbound interface."""

from __future__ import annotations

from datetime import UTC, datetime

from egia.api.status import HealthState
from egia.core.supervisor import Supervisor


class ServerState:
  """Mutable pipeline status shared between core and the WebSocket interface."""

  def __init__(self, version: str) -> None:
    self.status = HealthState.STARTING
    self.version = version
    self.started_at = datetime.now(UTC)
    self.supervisor: Supervisor | None = None
