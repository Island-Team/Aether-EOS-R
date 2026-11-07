"""Typed compact automatic-speech-recognition snapshot."""

from datetime import datetime
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field


class AsrSnapshot(BaseModel):
  """One complete recognized-speech result without audio or token timing."""

  model_config = ConfigDict(extra="forbid")
  type: Literal["asr.snapshot"] = "asr.snapshot"
  schema_version: Literal["1.0"] = "1.0"
  sequence: int = Field(ge=0)
  timestamp: datetime
  text: str
  language: str | None = Field(default=None, min_length=2, max_length=35)
  confidence: float | None = Field(default=None, ge=0, le=1)
