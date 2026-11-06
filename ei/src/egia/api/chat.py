"""Hearing ingress wire models — text translated from WebUI sound."""

from __future__ import annotations

from datetime import datetime
from typing import Literal

from pydantic import BaseModel, Field


class ChatRequest(BaseModel):
  """One translated utterance sent as a hearing perception."""

  sentence: str = Field(min_length=1, description="utterance text to enqueue")
  correlation_id: str | None = Field(
    default=None,
    description="optional client ID for log cross-reference",
  )


class ChatAck(BaseModel):
  """Successful admission of a hearing perception into the state queue."""

  ok: Literal[True] = True
  observation_id: str
  timestamp: datetime
  correlation_id: str | None = None


class ChatError(BaseModel):
  """Rejected chat request — validation or capacity failure."""

  ok: Literal[False] = False
  error: str
  correlation_id: str | None = None
