"""EOS WatchResource source for one discovered resource URI."""

from __future__ import annotations

import asyncio

from pydantic import BaseModel

from egia.contracts.endpoints import ResourceKind
from egia.core.link import EosClient
from egia.lib.logger import get_logger, log_once

__all__ = ["EosResourceSource"]

_RECONNECT_S = 1.0


class EosResourceSource:
  name = "eos"

  def __init__(
    self,
    client: EosClient,
    kind: ResourceKind,
    uri: str,
    model: type[BaseModel],
    poll_interval_s: float = 1.0,
  ) -> None:
    if poll_interval_s <= 0:
      raise ValueError("poll_interval_s must be positive")
    self._client = client
    self.kind = kind
    self._uri = uri
    self._model = model
    self._poll_interval_s = poll_interval_s
    self._log = get_logger("egia.core.perception")

  async def run(self, publish, stop: asyncio.Event) -> None:
    announced = False
    while not stop.is_set():
      try:
        async for snapshot in self._client.stream_resource(
          self._uri,
          self._model,
          kind=self.kind,
          poll_interval_s=self._poll_interval_s,
        ):
          if not announced:
            announced = True
            log_once(
              self._log,
              "info",
              "eos_resource_watch_started",
              key=self._uri,
              uri=self._uri,
            )
          await publish(snapshot)
          if stop.is_set():
            return
      except (OSError, ValueError) as exc:
        self._log.warning(
          "eos_resource_watch_reconnecting", uri=self._uri, error=str(exc)
        )
      try:
        await asyncio.wait_for(stop.wait(), timeout=_RECONNECT_S)
      except TimeoutError:
        pass
