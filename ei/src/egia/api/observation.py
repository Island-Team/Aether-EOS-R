"""Observation wire models — timestamped records admitted through the state queue."""

from __future__ import annotations

from datetime import datetime

from pydantic import BaseModel, Field


class ObservationRecord(BaseModel):
  """One timestamped observation in the state queue.

  ``kind`` selects the prompt section (e.g. ``hearing``, ``vision``).
  ``source`` identifies the ingress path (e.g. ``webui.chat``).
  """

  id: str = Field(description="server-assigned stable identifier")
  kind: str = Field(description="modality / event class for prompt routing")
  source: str = Field(description="ingress path that produced this record")
  timestamp: datetime = Field(description="server admission time, UTC")
  text: str = Field(description="primary payload for text modalities")
  metadata: dict[str, str] = Field(
    default_factory=dict,
    description="optional key/value tags for future routing",
  )
