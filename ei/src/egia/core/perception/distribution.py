"""The first distribution gate — typed read ports over the perception state.

The gate exposes one sink per consumer — the context port (admission records
+ frame exchanges), the safety port (raw frames + fault stream),
the monitor port (raw frames at high frequency), and the E2E port (future).
It is a light structural seam only: no buffering, no copying — consumers
pull from the shared exchanges (the admission store, the per-backend
per-part exchanges) through the port named for them, so
later phases can attach bound checks and routing policy at one place
without touching every consumer.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from typing import NamedTuple, Protocol

from egia.contracts.endpoints import ResourceKind
from egia.core.perception.admission import Admission


class PartExchange(Protocol):
  """Structural stand-in for ``core.io.PartStore``.

  The layer contract puts ``perception`` below ``io``, so this module cannot
  name the real class; the gate only hands the exchanges through, so a
  structural protocol keeps the ports typed without the import.
  """


__all__ = [
  "ContextPort",
  "DistributionGate",
  "MonitorPort",
  "SafetyPort",
]


@dataclass(frozen=True)
class ContextPort:
  """What the main chain (context/decision) reads: admission + exchanges."""

  admission: Admission
  part_stores: Mapping[ResourceKind, PartExchange]


@dataclass(frozen=True)
class SafetyPort:
  """Perception state for safety, including admitted faults."""

  admission: Admission


@dataclass(frozen=True)
class MonitorPort:
  """Raw part-frame exchanges for the monitor's high-frequency checks."""

  part_stores: Mapping[ResourceKind, PartExchange]


class _E2EPort(NamedTuple):
  """Reserved for phase 7+: the end-to-end loops arrive behind this gate."""


class DistributionGate:
  """One typed port per consumer over the shared perception exchanges."""

  def __init__(
    self,
    admission: Admission,
    part_stores: Mapping[ResourceKind, PartExchange],
  ) -> None:
    self._admission = admission
    self._part_stores = part_stores

  def context_port(self) -> ContextPort:
    """Admission plus part stores, as the main chain reads them."""
    return ContextPort(self._admission, self._part_stores)

  def monitor_port(self) -> MonitorPort:
    """Raw part-frame exchanges, ready for phase 7's bound checks."""
    return MonitorPort(self._part_stores)

  def safety_port(self) -> SafetyPort:
    """Perception state incl. faults for the safety chain."""
    return SafetyPort(self._admission)

  def e2e_port(self) -> _E2EPort | None:
    """Reserved: the e2e loops come later, behind this same gate."""
    return None
