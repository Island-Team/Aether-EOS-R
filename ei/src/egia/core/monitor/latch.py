"""Fleet emergency latch over EOS privileged primitives."""

from __future__ import annotations

from pathlib import Path

from egia.contracts.endpoints import PrimitiveKind
from egia.core.link import ClientRegistry, EosClient
from egia.lib import shared_events
from egia.lib.logger import get_logger
from egia.lib.safety_state import ResetAuthorization, SafetyState, SafetyStateError

__all__ = ["EstopLatch"]


def _single_registry(client: EosClient) -> ClientRegistry:
  registry = ClientRegistry()
  registry.register(getattr(client, "backend", "default") or "default", client)
  return registry


class EstopLatch:
  """Fleet latch whose durable state survives both EOS and EGIA restarts."""

  def __init__(
    self,
    client: EosClient | ClientRegistry,
    state_root: Path | None = None,
    *,
    graph_revision: str | None = None,
  ) -> None:
    self._clients = (
      client if isinstance(client, ClientRegistry) else _single_registry(client)
    )
    self._state = SafetyState(state_root / "safety") if state_root is not None else None
    self._graph_revision = graph_revision or "0" * 64
    self._engaged = bool(self._state.aliases()) if self._state is not None else False
    self._log = get_logger("egia.core.monitor")

  @property
  def engaged(self) -> bool:
    return self._engaged

  async def engage(self, reason: str) -> None:
    if self._engaged:
      return
    if self._state is not None:
      # Durable owner state precedes every outbound emergency operation.
      identities: dict[str, str] = {}
      for alias, client in self._clients.all().items():
        try:
          identities[alias] = client.discovery.robot_id
        except (AttributeError, RuntimeError):
          identities[alias] = ""
      self._state.engage(identities, self._graph_revision)
    self._engaged = True
    self._log.error("estop_engaged", reason=reason)
    reason_code = (
      "audit_fault"
      if reason.startswith(("audit:", "model sentinel:"))
      else "operator_estop"
      if reason.startswith("operator:")
      else "eos_unknown"
    )
    shared_events.emit("egia_safety_latched", fields={"reason_code": reason_code})
    shared_events.emit("egia_monitor_episode", fields={"outcome": "stop_required"})
    for alias, client in self._clients.all().items():
      try:
        await client.initialize()
        ack = await client.call_primitive(PrimitiveKind.SAFETY_ESTOP.value, "", {})
        if not ack.succeeded:
          raise RuntimeError(
            f"EOS rejected safety.estop: {ack.reason_code}: {ack.reason}"
          )
      except Exception as exc:  # noqa: BLE001 - emergency egress never raises
        self._log.error(
          "emergency_egress_failed",
          robot=alias,
          primitive=PrimitiveKind.SAFETY_ESTOP.value,
          reason=str(exc),
        )

  async def motion_stop(self) -> None:
    self._log.info("motion_stopped")
    for alias, client in self._clients.all().items():
      try:
        await client.initialize()
        for device_id in client.motion_device_ids():
          try:
            ack = await client.call_primitive(
              PrimitiveKind.MOTION_STOP.value, device_id, {}
            )
            if not ack.succeeded:
              raise RuntimeError(
                f"EOS rejected motion.stop: {ack.reason_code}: {ack.reason}"
              )
          except Exception as exc:  # noqa: BLE001 - continue across devices
            self._log.error(
              "emergency_egress_failed",
              robot=alias,
              device_id=device_id,
              primitive=PrimitiveKind.MOTION_STOP.value,
              reason=str(exc),
            )
      except Exception as exc:  # noqa: BLE001 - continue across robots
        self._log.error(
          "emergency_egress_failed",
          robot=alias,
          primitive=PrimitiveKind.MOTION_STOP.value,
          reason=str(exc),
        )

  def reconcile_restarts(self) -> bool:
    """Compatibility probe: runtime rediscovery cannot authorize reset."""
    return False

  async def consume_authorized_reset(
    self,
    authorization: ResetAuthorization | None,
    *,
    unresolved_operations: bool = False,
  ) -> bool:
    """Consume one stopped-process request and recheck all aliases twice."""
    if self._state is None:
      return False
    if authorization is None:
      return False
    shared_events.emit("egia_safety_reset_requested")
    if unresolved_operations:
      shared_events.emit(
        "egia_safety_reset_refused", fields={"reason_code": "lookup_unresolved"}
      )
      return False
    persisted = self._state.aliases()
    if (
      frozenset(authorization.identities) != persisted
      or authorization.identities != self._state.identities()
      or self._state.episode()
      != (authorization.episode_id, authorization.graph_revision)
      or authorization.graph_revision != self._graph_revision
      or persisted != frozenset(self._clients.all())
    ):
      self._log.error("estop_reset_refused", reason="alias_coverage")
      shared_events.emit(
        "egia_safety_reset_refused",
        fields={
          "reason_code": "revision_mismatch"
          if authorization.graph_revision != self._graph_revision
          else "alias_coverage"
        },
      )
      return False
    try:
      first = await self._verify_reset_safety(persisted)
      second = await self._verify_reset_safety(persisted)
      if first != second:
        raise SafetyStateError("EOS generation changed during reset")
      self._state.clear(authorization)
    except Exception as exc:  # noqa: BLE001 - any failed owner check keeps the latch
      self._log.error("estop_reset_refused", reason=str(exc))
      shared_events.emit(
        "egia_safety_reset_refused", fields={"reason_code": "eos_unsafe"}
      )
      return False
    self._engaged = False
    self._log.info("estop_reset_completed", aliases=sorted(persisted))
    shared_events.emit("egia_safety_reset_completed")
    return True

  async def _verify_reset_safety(
    self, aliases: frozenset[str]
  ) -> dict[str, tuple[str, str, str, str]]:
    generations: dict[str, tuple[str, str, str, str]] = {}
    for alias in sorted(aliases):
      client = self._clients.client(alias)
      discovery = await client.initialize(force=True)
      persisted_robot_id = (
        self._state.identities()[alias] if self._state is not None else ""
      )
      if not persisted_robot_id or discovery.robot_id != persisted_robot_id:
        raise SafetyStateError("EOS robot identity changed or was unknown")
      if (
        discovery.estop
        or discovery.health != "ready"
        or discovery.state not in {"idle", "ready"}
      ):
        raise SafetyStateError("EOS estop or runtime state is unsafe")
      if (
        client.active_revision != self._graph_revision
        or client.projection_sha256 is None
      ):
        raise SafetyStateError("EOS selected revision is unavailable")
      status = await client.refresh_status()
      if status.get("status") != "ready" or client.discovery.estop:
        raise SafetyStateError("EOS live status is unsafe")
      if (
        client.active_revision != self._graph_revision
        or client.projection_sha256 is None
        or client.discovery.robot_id != persisted_robot_id
      ):
        raise SafetyStateError("EOS selected generation changed")
      revision = client.active_revision
      projection = client.projection_sha256
      if revision is None or projection is None:
        raise SafetyStateError("EOS selected generation changed")
      generations[alias] = (
        revision,
        projection,
        client.discovery.robot_id,
        client.discovery.started_at,
      )
    return generations
