"""Cancellation-safe change signal shared by latest-snapshot stores."""

from __future__ import annotations

import asyncio

__all__ = ["WaitSet"]


class WaitSet:
  """One-to-many change notification without locks.

  ``asyncio.Condition`` is unsafe for snapshot exchanges: a waiter cancelled
  inside ``Condition.wait`` can leave the lock held, deadlocking the next
  publisher. This class keeps a set of waiter futures instead — notify
  resolves them all, cancelled waiters simply drop out. Owners live in
  ``egia.io`` (``PartStore``, ``ContextStore``) and ``core/perception``
  (``SceneStore``), which is why it sits in the neutral ``internal`` layer.
  """

  def __init__(self) -> None:
    self._waiters: set[asyncio.Future[None]] = set()

  def notify_all(self) -> None:
    """Resolve every pending waiter."""
    for waiter in self._waiters:
      if not waiter.done():
        waiter.set_result(None)
    self._waiters.clear()

  async def wait(self, timeout_s: float) -> bool:
    """Block until :meth:`notify_all` or ``timeout_s``; True when notified."""
    waiter: asyncio.Future[None] = asyncio.get_running_loop().create_future()
    self._waiters.add(waiter)
    try:
      await asyncio.wait_for(waiter, timeout_s)
      return True
    except TimeoutError:
      return False
    finally:
      self._waiters.discard(waiter)
