"""Structural skill-execution ports shared by decision and the EOS link."""

from __future__ import annotations

from collections.abc import AsyncIterator
from dataclasses import dataclass
from typing import Any, Protocol


@dataclass(frozen=True)
class SkillExecution:
  """One validated snapshot from EOS's durable skill execution ledger."""

  execution_id: str
  request_id: str
  name: str
  version: str
  status: str
  sequence: int
  cancellation_requested: bool
  reason_code: str | None
  message: str | None
  result: dict[str, Any]

  @property
  def terminal(self) -> bool:
    """Whether EOS promises no later state transition for this execution."""
    return self.status in {"succeeded", "rejected", "cancelled", "timed_out", "failed"}


class SkillClient(Protocol):
  """Decision-facing subset of one EOS client's skill operations."""

  @property
  def skills_available(self) -> bool:
    """Whether the skill catalog is currently usable."""
    ...

  def skill(self, name: str, version: str) -> SkillBinding | None: ...
  async def start_skill(
    self,
    name: str,
    version: str,
    arguments: dict[str, Any],
    *,
    request_id: str,
    timeout_s: float,
  ) -> SkillExecution: ...
  async def get_skill_execution(self, execution_id: str) -> SkillExecution: ...
  def watch_skill_execution(
    self, execution_id: str, *, after_sequence: int = 0
  ) -> AsyncIterator[SkillExecution]: ...
  async def cancel_skill_execution(self, execution_id: str) -> SkillExecution: ...


class SkillClients(Protocol):
  """Resolve one skill client by stable robot alias."""

  def client(self, backend_id: str | None = None) -> SkillClient: ...


class SkillBinding(Protocol):
  """Metadata needed to govern a planner-origin skill invocation."""

  @property
  def primitive_refs(self) -> tuple[tuple[str, str], ...]: ...

  @property
  def parameters(self) -> dict[str, Any] | None: ...

  @property
  def returns(self) -> dict[str, Any] | None: ...
