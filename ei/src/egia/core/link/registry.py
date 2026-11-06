"""Backend client registry — maps ``backend_id`` to its ``EosClient``.

One client per backend endpoint, never more than one session for the same
endpoint (see the "One session per endpoint" section in
``docs/egia/core/link.md``). The registry is the composition root's
one-stop surface for routing resources and primitive calls by backend:
resolve by ``backend_id``, or iterate all clients for status aggregation.
Pure in-memory state; performs no I/O.
"""

from __future__ import annotations

from egia.core.link.client import EosClient

__all__ = ["ClientRegistry"]

DEFAULT_BACKEND_ID = "default"


class ClientRegistry:
  """Register a ``EosClient`` per backend id.

  A backend endpoint (robot, sim instance) is registered under exactly one id;
  registering the same id twice is a configuration error. ``client(None)``
  resolves to the sole registered client, or to the ``"default"`` id when
  several are registered — mirroring the pre-multi-backend call sites that
  worked with exactly one client.
  """

  def __init__(self) -> None:
    self._clients: dict[str, EosClient] = {}
    self._order: list[str] = []

  def register(self, backend_id: str, client: EosClient) -> None:
    """Register ``client`` under ``backend_id`` (duplicate id -> ValueError)."""
    if backend_id in self._clients:
      raise ValueError(f"backend already registered: {backend_id!r}")
    self._clients[backend_id] = client
    self._order.append(backend_id)

  def client(self, backend_id: str | None = None) -> EosClient:
    """Resolve the client for ``backend_id``.

    ``None`` resolves to the sole registered client when there is exactly one,
    otherwise to the ``"default"`` id when present. An unknown id, an
    ambiguous ``None`` (several clients, no ``"default"``), or an empty
    registry raises ``LookupError``.
    """
    if backend_id is not None:
      try:
        return self._clients[backend_id]
      except KeyError as exc:
        raise LookupError(f"unknown backend: {backend_id!r}") from exc
    if len(self._clients) == 1:
      return next(iter(self._clients.values()))
    if DEFAULT_BACKEND_ID in self._clients:
      return self._clients[DEFAULT_BACKEND_ID]
    raise LookupError("ambiguous backend resolution: multiple backends, no 'default'")

  def ids(self) -> list[str]:
    """Registered backend ids in registration order."""
    return list(self._order)

  def all(self) -> dict[str, EosClient]:
    """Snapshot of ``backend_id -> client`` in registration order."""
    return {backend_id: self._clients[backend_id] for backend_id in self._order}
