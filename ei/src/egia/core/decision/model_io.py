"""In-memory exchange for the live planner model stream — mirrors ``ContextStore``.

The planner publishes the request, evolving provider-visible reasoning and
answer text, then the completed response. The northbound ``/model_io`` handler
emits the latest state on connect plus each update. Only the latest exchange
is retained — a slow consumer coalesces by construction — while ``total``
counts distinct requests since launch.
"""

from __future__ import annotations

import asyncio
import threading

from egia.api.model_io import (
  ModelActivity,
  ModelAgentStatus,
  ModelExchange,
  ModelIOFrame,
)
from egia.internal.notify import WaitSet

__all__ = ["ModelIOStore"]


class ModelIOStore:
  """Latest model exchange + total request count, with change notification."""

  def __init__(self) -> None:
    self._latest: ModelExchange | None = None
    self._exchanges: dict[str, ModelExchange] = {}
    self._activities: dict[str, ModelActivity] = {}
    self._seen: set[tuple[str, str, int]] = set()
    self._total = 0
    self._version = 0
    self._changed = WaitSet()
    self._lock = threading.Lock()

  @property
  def version(self) -> int:
    """Monotonic publish counter; bumps on every :meth:`publish`."""
    with self._lock:
      return self._version

  @property
  def total(self) -> int:
    """Total model requests published since launch."""
    with self._lock:
      return self._total

  def latest(self) -> ModelExchange | None:
    with self._lock:
      return self._latest

  def publish(self, exchange: ModelExchange) -> None:
    """Publish a new request or an in-place streaming update.

    Repeated frames for one ``exchange_id`` bump ``version`` so live clients
    repaint, but count as one model request in ``total``.
    """
    with self._lock:
      identity = (exchange.agent_id, exchange.mission_id, exchange.exchange_id)
      is_new = identity not in self._seen
      self._latest = exchange
      self._exchanges[exchange.agent_id] = exchange
      if is_new:
        self._seen.add(identity)
        self._total += 1
      self._version += 1
    self._changed.notify_all()

  def publish_activity(self, activity: ModelActivity) -> None:
    """Publish a primitive lifecycle update for one agent lane."""
    with self._lock:
      self._activities[activity.agent_id] = activity
      self._version += 1
    self._changed.notify_all()

  def _frame_locked(self) -> ModelIOFrame:
    agent_ids = sorted(self._exchanges.keys() | self._activities.keys())
    agents: list[ModelAgentStatus] = []
    for agent_id in agent_ids:
      exchange = self._exchanges.get(agent_id)
      activity = self._activities.get(agent_id)
      thinking = exchange is not None and exchange.status == "streaming"
      if thinking:
        status = "thinking"
      elif activity is not None and activity.status == "running":
        status = "working"
      elif (exchange is not None and exchange.status == "error") or (
        activity is not None and activity.status == "error"
      ):
        status = "error"
      else:
        status = "idle"
      agents.append(
        ModelAgentStatus(
          agent_id=agent_id,
          thinking=thinking,
          status=status,
          exchange=exchange,
          activity=activity,
        )
      )
    return ModelIOFrame(exchange=self._latest, total=self._total, agents=agents)

  def frame(self) -> ModelIOFrame:
    """Current frame: latest exchange (if any) plus the request count."""
    with self._lock:
      return self._frame_locked()

  async def wait_next(
    self, after_version: int, stop: asyncio.Event | None = None
  ) -> ModelIOFrame | None:
    """Block until a publish past ``after_version``; ``None`` on ``stop``."""
    while stop is None or not stop.is_set():
      with self._lock:
        if self._version > after_version:
          return self._frame_locked()
      await self._changed.wait(timeout_s=0.5)
    return None
