"""Subsystem registration types for the pipeline supervisor."""

from __future__ import annotations

import asyncio
from collections.abc import Callable, Coroutine
from dataclasses import dataclass, field
from datetime import datetime

from egia.api.status import HealthState

READY_TIMEOUT_S = 30.0
RESTART_BACKOFF_S = (0.5, 1.0, 2.0, 5.0, 10.0)


@dataclass
class SubsystemSpec:
  """A pipeline module managed by the supervisor.

  ``entry`` receives ``ready`` and ``stop`` events. It must complete its
  initialization, set ``ready``, then run its loop until ``stop`` is set.
  Setting ``ready`` is the contract that no module starts serving work
  before the whole pipeline is up.
  """

  name: str
  entry: Callable[[asyncio.Event, asyncio.Event], Coroutine[None, None, None]]


@dataclass
class _Runtime:
  spec: SubsystemSpec
  task: asyncio.Task | None = None
  state: HealthState = HealthState.STARTING
  ready: asyncio.Event = field(default_factory=asyncio.Event)
  ready_at: datetime | None = None
  restarts: int = 0
  failures: int = 0
  last_error: str | None = None
