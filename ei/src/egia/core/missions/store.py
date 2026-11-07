"""Versioned, filesystem-backed checkpoints for resumable missions.

The store owns durable mission execution state, not historical recall. One
atomic JSON file contains the mission lifecycle, its operator document, and the
latest planner record. Files live below the resolved EGIA state root and remain
available when the optional memory subsystem is disabled.
"""

from __future__ import annotations

import os
import tempfile
from datetime import UTC, datetime
from enum import StrEnum
from pathlib import Path
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, ValidationError, model_validator

from egia.api.mission import MissionRecord, MissionState
from egia.api.plan import PlanRecord, PlanStepStatus
from egia.core.missions.document import MissionDocument, MissionRevision

__all__ = [
  "MissionCheckpoint",
  "MissionCheckpointPhase",
  "MissionStore",
  "MissionStoreError",
]


class MissionStoreError(ValueError):
  """A mission checkpoint could not be read or validated safely."""


class MissionCheckpointPhase(StrEnum):
  """The durable execution boundary reached by a mission."""

  PENDING = "pending"
  PLANNING = "planning"
  EXECUTING = "executing"
  TERMINAL = "terminal"


class MissionCheckpoint(BaseModel):
  """One complete, versioned mission checkpoint."""

  model_config = ConfigDict(extra="forbid")

  schema_version: Literal[3] = 3
  saved_at: datetime = Field(default_factory=lambda: datetime.now(UTC))
  phase: MissionCheckpointPhase
  mission: MissionRecord
  document: MissionDocument
  revisions: list[MissionRevision] = Field(min_length=1)
  plan: PlanRecord | None = None

  @model_validator(mode="before")
  @classmethod
  def _migrate(cls, data: Any) -> Any:
    if isinstance(data, dict) and data.get("schema_version") in (1, 2):
      return {**data, "schema_version": 3}
    return data

  @model_validator(mode="after")
  def _validate_identity(self) -> MissionCheckpoint:
    if self.document.mission_id != self.mission.id:
      raise ValueError("document mission_id does not match mission record")
    if self.document.order != self.mission.order:
      raise ValueError("document order does not match mission record")
    if self.document.priority_band is not self.mission.priority_band:
      raise ValueError("document priority band does not match mission record")
    if self.plan is not None and self.plan.mission_id != self.mission.id:
      raise ValueError("plan mission_id does not match mission record")
    if self.plan is not None and self.plan.order != self.mission.order:
      raise ValueError("plan order does not match mission record")
    if self.plan is not None and self.plan.source != self.mission.source:
      raise ValueError("plan source does not match mission record")
    revision_numbers: list[int] = []
    for revision in self.revisions:
      if revision.document.mission_id != self.mission.id:
        raise ValueError("revision mission_id does not match mission record")
      if revision.revision != revision.document.revision:
        raise ValueError("revision number does not match its document")
      revision_numbers.append(revision.revision)
    if revision_numbers != sorted(set(revision_numbers)):
      raise ValueError("mission revisions must be unique and ascending")
    if self.document.revision != revision_numbers[-1]:
      raise ValueError("current document revision does not match revision history")
    if self.revisions[-1].document.order != self.document.order:
      raise ValueError("current document order does not match latest revision")
    if self.revisions[-1].document.priority_band is not self.document.priority_band:
      raise ValueError("current document priority band does not match latest revision")
    if self.phase is MissionCheckpointPhase.PENDING:
      if self.mission.state is not MissionState.PENDING:
        raise ValueError("pending checkpoint requires a pending mission")
    elif self.phase in (
      MissionCheckpointPhase.PLANNING,
      MissionCheckpointPhase.EXECUTING,
    ):
      if self.mission.state is not MissionState.RUNNING:
        raise ValueError("active checkpoint requires a running mission")
      if self.phase is MissionCheckpointPhase.EXECUTING and (
        self.plan is None
        or not any(step.status is PlanStepStatus.RUNNING for step in self.plan.plan)
      ):
        raise ValueError("executing checkpoint requires a running plan step")
    elif self.mission.state not in (
      MissionState.SUCCEEDED,
      MissionState.FAILED,
      MissionState.CANCELLED,
    ):
      raise ValueError("terminal checkpoint requires a terminal mission")
    return self


class MissionStore:
  """Save and load atomic ``*.mission.json`` checkpoints below one directory."""

  def __init__(self, root: Path | str) -> None:
    self._root = Path(root)
    created = not self._root.exists()
    self._root.mkdir(parents=True, exist_ok=True)
    if created:
      parent = os.open(self._root.parent, os.O_RDONLY)
      try:
        os.fsync(parent)
      finally:
        os.close(parent)

  @property
  def root(self) -> Path:
    """Directory containing mission checkpoint files."""
    return self._root

  def _path(self, mission_id: str) -> Path:
    if (
      not mission_id
      or Path(mission_id).name != mission_id
      or "/" in mission_id
      or "\\" in mission_id
    ):
      raise MissionStoreError(f"invalid mission id for checkpoint path: {mission_id!r}")
    return self._root / f"{mission_id}.mission.json"

  def clear(self) -> None:
    """Remove every persisted mission checkpoint for a fresh launch."""
    removed = False
    for path in self._root.glob("*.mission.json"):
      path.unlink()
      removed = True
    if removed:
      directory = os.open(self._root, os.O_RDONLY)
      try:
        os.fsync(directory)
      finally:
        os.close(directory)

  def load(self, mission_id: str) -> MissionCheckpoint | None:
    """Load one checkpoint, or return ``None`` when it does not exist.

    Raises:
        MissionStoreError: If the file is unreadable, invalid, or has the wrong
          mission identity.
    """
    path = self._path(mission_id)
    if not path.exists():
      return None
    checkpoint = self._read(path)
    if checkpoint.mission.id != mission_id:
      raise MissionStoreError(
        f"mission checkpoint {path} contains id {checkpoint.mission.id!r}"
      )
    return checkpoint

  def load_active(self) -> list[MissionCheckpoint]:
    """Load every pending or running mission in submission order.

    Raises:
        MissionStoreError: If any checkpoint is unreadable or invalid. Startup
          fails closed rather than silently losing durable work.
    """
    checkpoints: list[MissionCheckpoint] = []
    for path in sorted(self._root.glob("*.mission.json")):
      checkpoint = self._read(path)
      expected = self._path(checkpoint.mission.id)
      if path != expected:
        raise MissionStoreError(
          f"mission checkpoint filename {path.name!r} does not match "
          f"mission id {checkpoint.mission.id!r}"
        )
      if checkpoint.mission.state in (MissionState.PENDING, MissionState.RUNNING):
        checkpoints.append(checkpoint)
    return sorted(checkpoints, key=lambda item: item.mission.submitted_at)

  def save_mission(
    self,
    mission: MissionRecord,
    document: MissionDocument,
    revisions: list[MissionRevision],
  ) -> MissionCheckpoint:
    """Persist a mission lifecycle or document mutation, preserving its plan."""
    current = self.load(mission.id)
    phase = self._phase_for_mission(mission, current)
    checkpoint = MissionCheckpoint(
      phase=phase,
      mission=mission.model_copy(deep=True),
      document=document.model_copy(deep=True),
      revisions=[revision.model_copy(deep=True) for revision in revisions],
      plan=(current.plan.model_copy(deep=True) if current and current.plan else None),
    )
    self._write(checkpoint)
    return checkpoint

  def save_plan(
    self,
    mission: MissionRecord,
    document: MissionDocument,
    revisions: list[MissionRevision],
    plan: PlanRecord,
    *,
    active: bool,
  ) -> MissionCheckpoint:
    """Persist the latest planner state and its execution boundary."""
    if mission.state in (
      MissionState.SUCCEEDED,
      MissionState.FAILED,
      MissionState.CANCELLED,
    ):
      phase = MissionCheckpointPhase.TERMINAL
    elif mission.state is MissionState.PENDING:
      phase = MissionCheckpointPhase.PENDING
    elif active and any(step.status is PlanStepStatus.RUNNING for step in plan.plan):
      phase = MissionCheckpointPhase.EXECUTING
    else:
      phase = MissionCheckpointPhase.PLANNING
    checkpoint = MissionCheckpoint(
      phase=phase,
      mission=mission.model_copy(deep=True),
      document=document.model_copy(deep=True),
      revisions=[revision.model_copy(deep=True) for revision in revisions],
      plan=plan.model_copy(deep=True),
    )
    self._write(checkpoint)
    return checkpoint

  def _phase_for_mission(
    self,
    mission: MissionRecord,
    current: MissionCheckpoint | None,
  ) -> MissionCheckpointPhase:
    if mission.state is MissionState.PENDING:
      return MissionCheckpointPhase.PENDING
    if mission.state in (
      MissionState.SUCCEEDED,
      MissionState.FAILED,
      MissionState.CANCELLED,
    ):
      return MissionCheckpointPhase.TERMINAL
    if current is not None and current.phase is MissionCheckpointPhase.EXECUTING:
      return MissionCheckpointPhase.EXECUTING
    return MissionCheckpointPhase.PLANNING

  def _read(self, path: Path) -> MissionCheckpoint:
    try:
      return MissionCheckpoint.model_validate_json(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValidationError, ValueError) as exc:
      raise MissionStoreError(f"invalid mission checkpoint {path}: {exc}") from exc

  def _write(self, checkpoint: MissionCheckpoint) -> None:
    path = self._path(checkpoint.mission.id)
    temporary: Path | None = None
    try:
      with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        dir=self._root,
        prefix=f".{checkpoint.mission.id}.",
        suffix=".tmp",
        delete=False,
      ) as stream:
        temporary = Path(stream.name)
        stream.write(checkpoint.model_dump_json(indent=2))
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
      os.replace(temporary, path)
      directory = os.open(self._root, os.O_RDONLY)
      try:
        os.fsync(directory)
      finally:
        os.close(directory)
    except OSError as exc:
      if temporary is not None:
        temporary.unlink(missing_ok=True)
      raise MissionStoreError(
        f"could not save mission checkpoint {path}: {exc}"
      ) from exc
