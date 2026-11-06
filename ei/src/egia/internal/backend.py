"""Composition-root seam for replacing one backend session with another.

The handoff contains only interface-layer mission and plan records.  It lets the
composition root retain admitted work while core tears down backend-owned
clients, watchers, and registries and constructs their replacements.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from egia.api.mission import MissionRecord
from egia.api.observation import ObservationRecord
from egia.api.plan import PlanRecord


@dataclass
class BackendSessionHandoff:
  """Mutable, process-local state transferred across a backend rebind."""

  revision: int = 0
  _missions: list[MissionRecord] = field(default_factory=list)
  _plans: dict[str, PlanRecord] = field(default_factory=dict)
  _observations: list[ObservationRecord] = field(default_factory=list)
  _observation_history: list[ObservationRecord] = field(default_factory=list)

  def capture(
    self,
    missions: list[MissionRecord],
    active_plan: PlanRecord | None = None,
    *,
    observations: list[ObservationRecord] | None = None,
    observation_history: list[ObservationRecord] | None = None,
  ) -> None:
    """Atomically replace the pending handoff and advance its revision."""
    self._missions = [mission.model_copy(deep=True) for mission in missions]
    self._plans = (
      {active_plan.mission_id: active_plan.model_copy(deep=True)}
      if active_plan is not None
      else {}
    )
    self._observations = [
      observation.model_copy(deep=True) for observation in observations or []
    ]
    self._observation_history = [
      observation.model_copy(deep=True) for observation in observation_history or []
    ]
    self.revision += 1

  def consume(self) -> tuple[list[MissionRecord], dict[str, PlanRecord]]:
    """Take one isolated snapshot, clearing it for a later session change."""
    missions = [mission.model_copy(deep=True) for mission in self._missions]
    plans = {key: value.model_copy(deep=True) for key, value in self._plans.items()}
    self._missions.clear()
    self._plans.clear()
    return missions, plans

  def consume_observations(
    self,
  ) -> tuple[list[ObservationRecord], list[ObservationRecord]]:
    """Take the bounded admission state retained by the previous session."""
    observations = [item.model_copy(deep=True) for item in self._observations]
    history = [item.model_copy(deep=True) for item in self._observation_history]
    self._observations.clear()
    self._observation_history.clear()
    return observations, history


__all__ = ["BackendSessionHandoff"]
