"""Latest-snapshot exchanges for standalone resources.

Each northbound replaceable-snapshot WebSocket (``/base``, ``/gimbal``, ...,
``/context``) streams state produced elsewhere in the pipeline. These stores
are the shared handoff: a producer publishes a validated snapshot and the
WebSocket handler blocks for the next version, then serializes it.
:class:`PartStore` mirrors ``SceneStore`` but is generic over any part
snapshot that carries a monotonic ``sequence`` field; :class:`ContextStore`
is keyed by an opaque version counter because prompt snapshots carry no
sequence.

The stores own no domain logic and validate nothing — producers must hand
them already-validated models (contracts from ``egia.contracts.perception`` for
parts, ``egia.api.context.ContextSnapshot`` for context,
``egia.api.prompts.PromptsSnapshot`` for the assembled system prompt). Like the
rest of ``egia.io`` they project state other modules own; the producers live in
``core/perception`` and ``core/context``.
"""

from __future__ import annotations

import asyncio
from typing import Protocol

from pydantic import BaseModel

from egia.internal.notify import WaitSet


class PartSnapshot(Protocol):
  """Structural shape every standalone part snapshot exposes to ``io``.

  Every contract in ``egia.contracts.perception`` carries a monotonic ``sequence``
  (the envelope tick) and serializes through Pydantic's ``model_dump_json``.
  The store and the WebSocket handler depend only on those two members, so a
  new part needs no store-side change — only a new route binding it to a path.
  """

  sequence: int

  def model_dump(self, **kwargs) -> dict: ...

  def model_dump_json(self) -> str: ...


class PartStore:
  """In-memory exchange for replaceable part snapshots.

  A producer calls :meth:`publish` with the latest contract snapshot; the
  WebSocket consumer blocks in :meth:`wait_next` until a snapshot with a higher
  ``sequence`` arrives. At most one snapshot (the latest) is retained, so a
  slow consumer coalesces by construction and never accumulates a backlog.
  """

  def __init__(self) -> None:
    self._latest: PartSnapshot | None = None
    self._latest_by_key: dict[str, PartSnapshot] = {}
    self._changed = WaitSet()

  def latest(self) -> PartSnapshot | None:
    """Return the most recently published snapshot, if any."""
    return self._latest

  def all_latest(self) -> dict[str, PartSnapshot]:
    """Return each robot/device snapshot retained by this family store."""
    return dict(self._latest_by_key)

  async def publish(self, state: BaseModel, *, key: str | None = None) -> None:
    """Retain ``state`` (re-reading its ``sequence``) and notify waiters.

    ``state`` is typed ``BaseModel`` for caller convenience; it is stored
    against the :class:`PartSnapshot` protocol, so only ``sequence`` and
    ``model_dump_json`` are ever read back.
    """
    self._latest = state  # type: ignore[assignment]
    if key is not None:
      self._latest_by_key[key] = state  # type: ignore[assignment]
    self._changed.notify_all()

  async def wait_next(
    self, after_sequence: int, stop: asyncio.Event | None = None
  ) -> PartSnapshot | None:
    """Block until a snapshot with ``sequence > after_sequence`` is published.

    Polls on a short timeout so a ``stop`` event is observed promptly; returns
    ``None`` only when ``stop`` is set before any newer snapshot arrives.
    """
    while stop is None or not stop.is_set():
      current = self._latest
      if current is not None and current.sequence > after_sequence:
        return current
      await self._changed.wait(timeout_s=0.5)
    return None


class ContextStore:
  """In-memory exchange for the latest prompt snapshot (:class:`ContextSnapshot`).

  Mirrors :class:`PartStore` but versions snapshots with an opaque monotonic
  counter, because a prompt bundle has no ``sequence`` field. A producer
  (``core/context``) publishes after each rebuild; the northbound ``/context``
  handler emits the latest on connect plus each newer version.
  """

  def __init__(self) -> None:
    self._latest: BaseModel | None = None
    self._version = 0
    self._changed = WaitSet()

  def latest(self) -> BaseModel | None:
    """Return the most recently published snapshot, if any."""
    return self._latest

  @property
  def version(self) -> int:
    """Monotonic publish counter; bumps on every :meth:`publish`."""
    return self._version

  async def publish(self, state: BaseModel) -> None:
    """Retain ``state`` and notify waiters."""
    self._latest = state
    self._version += 1
    self._changed.notify_all()

  async def wait_next(
    self, after_version: int, stop: asyncio.Event | None = None
  ) -> BaseModel | None:
    """Block until a snapshot published after ``after_version`` arrives.

    Returns ``None`` only when ``stop`` is set before any newer snapshot.
    """
    while stop is None or not stop.is_set():
      if self._version > after_version and self._latest is not None:
        return self._latest
      await self._changed.wait(timeout_s=0.5)
    return None


class CatalogStore:
  """In-memory exchange for a replaceable public catalog snapshot."""

  def __init__(self) -> None:
    self._latest: BaseModel | None = None
    self._version = 0
    self._changed = WaitSet()

  def latest(self) -> BaseModel | None:
    return self._latest

  @property
  def version(self) -> int:
    return self._version

  async def publish(self, state: BaseModel) -> None:
    self._latest = state
    self._version += 1
    self._changed.notify_all()

  async def wait_next(
    self, after_version: int, stop: asyncio.Event | None = None
  ) -> BaseModel | None:
    while stop is None or not stop.is_set():
      if self._version > after_version and self._latest is not None:
        return self._latest
      await self._changed.wait(timeout_s=0.5)
    return None


class PrimitivesStore(CatalogStore):
  """Published primitive catalog."""


class SkillsStore(CatalogStore):
  """Published native-skill catalog."""


class PromptsStore:
  """In-memory exchange for the raw planner system prompt (:class:`PromptsSnapshot`).

  Published at launch once the decision instruction is known. The ``/prompts``
  handler emits the latest on connect.
  """

  def __init__(self) -> None:
    self._latest: BaseModel | None = None
    self._version = 0
    self._changed = WaitSet()

  def latest(self) -> BaseModel | None:
    return self._latest

  @property
  def version(self) -> int:
    return self._version

  async def publish(self, state: BaseModel) -> None:
    self._latest = state
    self._version += 1
    self._changed.notify_all()

  async def wait_next(
    self, after_version: int, stop: asyncio.Event | None = None
  ) -> BaseModel | None:
    while stop is None or not stop.is_set():
      if self._version > after_version and self._latest is not None:
        return self._latest
      await self._changed.wait(timeout_s=0.5)
    return None
