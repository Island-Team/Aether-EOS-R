"""Bidder profiles — executor cost models compiled from EOS discovery.

A bidder is one addressable executor instance: a (backend, part) pair such
as ``("line", "arm-2")``. The profile carries what §3.5's bid price needs —
primitive availability and tags, committed load, current zone, last primitive,
and health. Feasibility
is decided here, by the bidder: :meth:`BidderProfile.bid` returns ``None``
when the bidder cannot run the step at all, so no infeasible assignment can
be constructed even transiently — the auctioneer never sees one.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from egia.core.decision.profiles import ProfileStep
from egia.core.link import EosDiscovery

__all__ = ["BidderProfile", "bidder_profiles"]


@dataclass
class BidderProfile:
  """One executor's cost model; mutated only by award/completion bookkeeping."""

  backend: str
  part_id: str
  primitives: set[str]
  tags: set[str] = field(default_factory=set)
  # Stations this executor serves; empty means "serves any" (unknown — the
  # backend's admission decides). A step naming a station bids only on
  # executors that serve it: reachability is abstention, not auction math.
  stations: set[str] = field(default_factory=set)
  healthy: bool = True
  load_s: float = 0.0
  zone: str | None = None
  last_primitive: str | None = None
  # Cost knobs (§3.5): fixed transition price between zones, fixed primitive
  # switch price, congestion price per second of committed load.
  zone_change_cost_s: float = 1.0
  primitive_switch_cost_s: float = 0.5

  @property
  def id(self) -> str:
    return f"{self.backend}:{self.part_id}"

  def bid(self, step: ProfileStep, *, congestion: float = 0.1) -> float | None:
    """The §3.5 bid price for one step, or ``None`` to abstain.

    $$b_{ij} = c^{\\mathrm{exec}}_{ij} + c^{\\mathrm{trans}}_{ij}
    + c^{\\mathrm{switch}}_{ij} + \\lambda L_j$$

    Abstention (unhealthy, or the primitive is not served by this part) is the
    feasibility decision — the auctioneer constructs assignments only from
    received bids.
    """
    if not self.healthy or step.primitive not in self.primitives:
      return None
    if self.stations and step.station is not None and step.station not in self.stations:
      return None
    exec_cost = step.est_duration_s
    transition = (
      0.0 if step.zone is None or step.zone == self.zone else self.zone_change_cost_s
    )
    switch = (
      0.0
      if self.last_primitive is None or self.last_primitive == step.primitive
      else self.primitive_switch_cost_s
    )
    return exec_cost + transition + switch + congestion * self.load_s

  def commit(self, step: ProfileStep) -> None:
    """Record one awarded step: load, zone, and primitive-switch state."""
    self.load_s += step.est_duration_s
    self.zone = step.zone or self.zone
    self.last_primitive = step.primitive

  def settle(self, step: ProfileStep) -> None:
    """Record one completed step: the load commitment retires."""
    self.load_s = max(0.0, self.load_s - step.est_duration_s)


def bidder_profiles(backend: str, discovery: EosDiscovery) -> list[BidderProfile]:
  """Compile one bidder per EOS device that advertises primitives.

  EOS deliberately does not define station or tag reachability. Those sets
  therefore remain empty and no reachability is inferred from device class,
  part id, or backend metadata.
  """
  profiles: list[BidderProfile] = []
  for device in discovery.devices:
    primitives = set(device.supported_primitives)
    if not primitives:
      continue
    profiles.append(
      BidderProfile(
        backend=backend,
        part_id=device.id,
        primitives=primitives,
        healthy=device.alive and device.admitted,
      )
    )
  return profiles
