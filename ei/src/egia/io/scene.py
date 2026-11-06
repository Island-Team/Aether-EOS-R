"""Scene exchange — io's read model for the bundled scene telemetry.

The bundled ``scene/state`` resource is presentation: the parts family polls
or streams it from the backend and publishes each validated ``SceneState``
frame here, bypassing admission entirely. The northbound ``/perception/world/
scene`` route serves the latest frame on connect and each newer sequence.

Perception must not own this store — io projects it, so io holds it (the
inverse dependency would break the pipeline layer order).
"""

from __future__ import annotations

import asyncio

from pydantic import BaseModel

from egia.contracts.perception.world.scene import SceneState
from egia.internal.notify import WaitSet
from egia.lib.logger import get_logger


class SceneStore:
  """In-memory exchange for replaceable ``SceneState`` snapshots."""

  def __init__(self) -> None:
    self._latest: SceneState | None = None
    self._changed = WaitSet()
    self._log = get_logger("egia.io")

  def latest(self) -> SceneState | None:
    """Return the most recently published snapshot, if any."""
    return self._latest

  async def publish(self, state: BaseModel) -> None:
    """Validate and retain ``state`` as a ``SceneState``, then notify waiters.

    ``state`` is typed ``BaseModel`` so the store satisfies the parts
    family's publisher protocol; the round-trip through ``model_validate``
    rejects anything that is not a valid scene frame.
    """
    self._latest = SceneState.model_validate(state.model_dump())
    self._log.debug(
      "scene_built",
      sequence=self._latest.sequence,
      object_count=len(self._latest.objects or ()),
    )
    self._changed.notify_all()

  async def wait_next(
    self, after_sequence: int, stop: asyncio.Event | None = None
  ) -> SceneState | None:
    """Block until a snapshot with ``sequence > after_sequence`` arrives."""
    while stop is None or not stop.is_set():
      current = self._latest
      if current is not None and current.sequence > after_sequence:
        return current
      await self._changed.wait(timeout_s=0.5)
    return None
