"""Shared envelope for normalized EOS world snapshots."""

from datetime import datetime

from pydantic import BaseModel, ConfigDict, Field


class WorldSnapshot(BaseModel):
  """Sequence and timing fields shared by all world snapshots."""

  model_config = ConfigDict(extra="forbid")
  schema_version: str = "1.0"
  sequence: int = Field(ge=0)
  timestamp: datetime
  source_timestamp_ns: int | None = Field(ge=0)
