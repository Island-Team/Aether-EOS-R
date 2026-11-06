"""Northbound wire model for the raw planner system prompt."""

from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, Field


class PromptSection(BaseModel):
  """One named section of a prompt, retained for UI and agent consumers."""

  name: str = Field(min_length=1, description="stable section name, e.g. role or rules")
  text: str = Field(min_length=1, description="section text as sent to the model")
  lines: list[str] = Field(
    default_factory=list,
    description="non-empty lines in the section, useful for line-oriented clients",
  )


class PromptDocument(BaseModel):
  """A prompt message with its role and named sections."""

  role: Literal["system", "user"]
  text: str = Field(min_length=1, description="complete prompt message text")
  sections: list[PromptSection] = Field(default_factory=list)


class PromptsSnapshot(BaseModel):
  """Raw LLM system prompt as the decision layer will send it.

  ``system`` is the resolved static decision instruction — the same text that becomes the
  ``role=system`` message on every planner step.
  Published at readiness so operator consoles can inspect it before the first
  model request.
  """

  system: str = Field(min_length=1, description="assembled planner system message")
  sections: list[PromptSection] = Field(
    default_factory=list,
    description="named sections of the system prompt in model order",
  )
