"""Review wire models — the operator promotion route (§3.3).

Review is the separate write path into reference memory: the operator judges
a concluded episodic plan and promotes it — a human decision recorded with
the plan, never an automatic consequence of success. Pure data: the models
below are the request/ack/error frames of ``/api/aether/egia/review``; the
stores live in ``core/memory`` and the handler in ``egia.io``.
"""

from __future__ import annotations

from datetime import datetime
from typing import Literal

from pydantic import BaseModel, Field

REVIEW_OUTCOMES = ("effective", "ineffective")


class ReviewRequest(BaseModel):
  """One operator review of a concluded plan.

  ``outcome`` overrides the plan-level judgment; absent, the plan promotes
  with its recorded outcome. Only ``effective`` / ``ineffective`` are valid —
  the review path is the only writer of those judgments.
  """

  plan_id: str = Field(min_length=1, description="episodic plan id to promote")
  outcome: Literal["effective", "ineffective"] | None = Field(
    default=None,
    description="optional outcome override; only the review path sets these",
  )
  note: str | None = Field(
    default=None, description="operator note stored with the plan"
  )
  correlation_id: str | None = Field(
    default=None,
    description="optional client ID for log cross-reference",
  )


class ReviewAck(BaseModel):
  """Successful promotion into reference memory."""

  ok: Literal[True] = True
  plan_id: str
  outcome: str = Field(description="plan-level outcome after promotion")
  reviewed_at: datetime = Field(description="review time, UTC")
  correlation_id: str | None = None


class ReviewError(BaseModel):
  """Rejected review — validation failure or unknown plan."""

  ok: Literal[False] = False
  error: str
  correlation_id: str | None = None
