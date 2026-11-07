"""The admission store — sole admission point for runtime observations.

Besides the live FIFO the admission keeps a bounded per-kind *history*:
every admission (coalescing admissions included) appends to its kind's
bucket, so the context builder can render perception as a chronological
stream instead of only the latest snapshot — the perception-history prompt
(§3.3). Formerly ``core/queue`` — the queue is the admission point now, see
``docs/egia/core/perception.md``.
"""

from __future__ import annotations

import asyncio
import uuid
from collections import deque
from collections.abc import Callable
from datetime import UTC, datetime

from egia.api.observation import ObservationRecord
from egia.lib.logger import get_logger


class Admission:
  """In-memory bounded FIFO for timestamped observation records.

  When at capacity, new admissions are rejected rather than evicting
  existing records. Independent consumers wait on :meth:`wait_for_change`
  with their own revision for updates.
  ``history_per_kind`` bounds how many past admissions of each kind the
  queue remembers for the perception-history prompt.
  """

  def __init__(self, capacity: int, history_per_kind: int = 8) -> None:
    if capacity < 1:
      raise ValueError("queue capacity must be at least 1")
    if history_per_kind < 1:
      raise ValueError("history_per_kind must be at least 1")
    self._capacity = capacity
    self._history_per_kind = history_per_kind
    self._records: deque[ObservationRecord] = deque()
    self._history: dict[str, deque[ObservationRecord]] = {}
    self._dropped: dict[str, int] = {}
    self._coalesced: dict[str, int] = {}
    self._revision = 0
    self._changed = asyncio.Event()
    self._listeners: list[Callable[[], None]] = []
    self._log = get_logger("egia.core.perception")

  @property
  def capacity(self) -> int:
    return self._capacity

  @property
  def size(self) -> int:
    return len(self._records)

  @property
  def revision(self) -> int:
    """Monotonic admission version for independent change subscribers."""
    return self._revision

  def is_full(self) -> bool:
    return len(self._records) >= self._capacity

  def records(self) -> list[ObservationRecord]:
    return list(self._records)

  def restore(
    self,
    records: list[ObservationRecord],
    history: list[ObservationRecord],
  ) -> None:
    """Restore bounded admission state retained across a backend rebind."""
    self._records = deque(
      item.model_copy(deep=True) for item in records[-self._capacity :]
    )
    self._history.clear()
    for source in history:
      self._remember(source.model_copy(deep=True))
    self._publish_change()

  def records_by_kind(self, kind: str) -> list[ObservationRecord]:
    """Return every current record of ``kind``, one per coalesced source."""
    return [record for record in self._records if record.kind == kind]

  def latest_by_kind(
    self, kind: str, *, source: str | None = None
  ) -> ObservationRecord | None:
    for record in reversed(self._records):
      if record.kind == kind and (source is None or record.source == source):
        return record
    return None

  def history(
    self,
    kinds: set[str] | frozenset[str] | None = None,
    limit: int | None = None,
  ) -> list[ObservationRecord]:
    """Recent admissions across ``kinds`` (or all), oldest first, bounded.

    The per-kind history keeps every admission — scene coalescing included —
    so the context builder can render perception as a chronological stream,
    not just the latest snapshot. ``limit`` caps the merged result; None
    returns the full remembered span.
    """
    records = [
      record
      for kind, bucket in self._history.items()
      if kinds is None or kind in kinds
      for record in bucket
    ]
    records.sort(key=lambda record: record.timestamp)
    if limit is not None and limit > 0:
      return records[-limit:]
    return records

  def dropped(self, kind: str) -> int:
    """How many admissions of ``kind`` the history has evicted so far."""
    return self._dropped.get(kind, 0)

  def coalesced(self, kind: str) -> int:
    """How many same-kind records coalescing admission has replaced so far.

    Counted O(1) when ``enqueue_latest`` replaces a pending same-kind record.
    """
    return self._coalesced.get(kind, 0)

  def _remember(self, record: ObservationRecord) -> None:
    """Append one admission to its kind's bounded history bucket."""
    bucket = self._history.setdefault(record.kind, deque())
    bucket.append(record)
    while len(bucket) > self._history_per_kind:
      bucket.popleft()
      self._dropped[record.kind] = self._dropped.get(record.kind, 0) + 1

  async def enqueue(
    self,
    *,
    kind: str,
    source: str,
    text: str,
    metadata: dict[str, str] | None = None,
  ) -> ObservationRecord | None:
    """Admit one observation. Returns ``None`` when the queue is full."""
    if self.is_full():
      self._log.warning("queue_full", capacity=self._capacity, kind=kind, source=source)
      return None
    record = ObservationRecord(
      id=str(uuid.uuid4()),
      kind=kind,
      source=source,
      timestamp=datetime.now(UTC),
      text=text,
      metadata=metadata or {},
    )
    self._records.append(record)
    self._remember(record)
    self._log.info(
      "observation_enqueued",
      observation_id=record.id,
      kind=record.kind,
      source=record.source,
    )
    self._publish_change()
    return record

  async def enqueue_latest(
    self,
    *,
    kind: str,
    source: str,
    text: str,
    metadata: dict[str, str] | None = None,
  ) -> ObservationRecord | None:
    """Admit one observation, replacing the same kind/source snapshot.

    Coalescing admission for high-rate streams: at most one record per
    ``(kind, source)`` sits in the queue. Independent backends therefore do
    not erase one another. Replacing a source cannot grow the queue; a first
    snapshot from a new source is rejected when the global capacity is full.
    """
    replaced = any(r.kind == kind and r.source == source for r in self._records)
    self._records = deque(
      r for r in self._records if not (r.kind == kind and r.source == source)
    )
    if replaced:
      self._coalesced[kind] = self._coalesced.get(kind, 0) + 1
    elif self.is_full():
      self._log.warning("queue_full", capacity=self._capacity, kind=kind, source=source)
      return None
    record = ObservationRecord(
      id=str(uuid.uuid4()),
      kind=kind,
      source=source,
      timestamp=datetime.now(UTC),
      text=text,
      metadata=metadata or {},
    )
    self._records.append(record)
    self._remember(record)
    log = self._log.debug if replaced else self._log.info
    log(
      "observation_enqueued",
      observation_id=record.id,
      kind=record.kind,
      source=record.source,
      coalesced=replaced,
    )
    self._publish_change()
    return record

  def _publish_change(self) -> None:
    """Advance the durable change token and wake current waiters."""
    self._revision += 1
    self._changed.set()
    for listener in tuple(self._listeners):
      listener()

  def add_listener(self, listener: Callable[[], None]) -> Callable[[], None]:
    """Observe an admitted change; rejected full-queue input emits nothing."""
    self._listeners.append(listener)

    def unsubscribe() -> None:
      self._listeners.remove(listener)

    return unsubscribe

  async def wait_for_change(
    self,
    after: int,
    stop: asyncio.Event | None = None,
  ) -> int | None:
    """Wait until an admission newer than ``after`` exists.

    Unlike :meth:`wait_changed`, this is safe for multiple independent
    subscribers. A context rebuild, proposer pass, or memory consolidator
    cannot consume another subscriber's notification: each compares its own
    last observed revision to the durable counter.
    """
    while True:
      if stop is not None and stop.is_set():
        return None
      if self._revision != after:
        return self._revision
      self._changed.clear()
      # An admission may have raced the clear; its revision remains durable.
      if self._revision != after:
        return self._revision
      if stop is None:
        await self._changed.wait()
        continue
      wait_changed = asyncio.create_task(self._changed.wait())
      wait_stop = asyncio.create_task(stop.wait())
      try:
        done, pending = await asyncio.wait(
          {wait_changed, wait_stop}, return_when=asyncio.FIRST_COMPLETED
        )
      except asyncio.CancelledError:
        wait_changed.cancel()
        wait_stop.cancel()
        raise
      for task in pending:
        task.cancel()
      if wait_stop in done:
        return None

  async def wait_changed(self, stop: asyncio.Event | None = None) -> bool:
    """Block until a new observation is admitted or ``stop`` is set."""
    if stop is not None and stop.is_set():
      return False
    # Check-then-clear instead of clear-then-wait: an admission racing the
    # caller between event loop steps must not be lost to an early clear.
    if self._changed.is_set():
      self._changed.clear()
      return True
    if stop is None:
      await self._changed.wait()
      self._changed.clear()
      return True
    wait_changed = asyncio.create_task(self._changed.wait())
    wait_stop = asyncio.create_task(stop.wait())
    try:
      done, pending = await asyncio.wait(
        {wait_changed, wait_stop},
        return_when=asyncio.FIRST_COMPLETED,
      )
    except asyncio.CancelledError:
      # A caller racing this wait against another event may cancel the
      # outer task; the inner waiters must not outlive it.
      wait_changed.cancel()
      wait_stop.cancel()
      raise
    for task in pending:
      task.cancel()
    return wait_changed in done and wait_changed.result()
