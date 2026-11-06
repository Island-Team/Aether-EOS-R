"""Live model-stream wire contract — one evolving exchange per planner step.

Declaration only, per the interface-layer purity rule: the payload served on
the ``/api/aether/egia/model_io`` WebSocket. An *exchange* is the full request
(the prompt exactly as sent), provider-visible reasoning, and the
model's answer accumulated so far, so an operator console can inspect the
decision layer while inference is running. The
wire spec lives in ``docs/egia/northbound.md``; update both together.
"""

from __future__ import annotations

from datetime import datetime
from typing import Any, Literal

from pydantic import BaseModel, Field


class ModelMessage(BaseModel):
  """One chat message as sent to the completion endpoint."""

  role: Literal["system", "user", "assistant"]
  content: str = Field(description="message text; multimodal parts are flattened")


class ModelExchange(BaseModel):
  """One request/response pair from the planner's step loop."""

  exchange_id: int = Field(ge=0, description="monotonic exchange counter")
  agent_id: str = Field(
    default="planner",
    description="stable logical model lane, e.g. controller, reasoner, or sentinel",
  )
  mission_id: str = Field(description="mission this exchange belongs to")
  step: int = Field(ge=1, description="planner step that issued the request")
  status: Literal["streaming", "complete", "interrupted", "error"] = Field(
    default="complete", description="current lifecycle state of this request"
  )
  model: str = Field(default="", description="model name reported by the provider")
  messages: list[ModelMessage] = Field(description="the request, exactly as sent")
  reply: str = Field(description="the model's reply text accumulated so far")
  thinking: str | None = Field(
    default=None,
    description=(
      "provider-visible reasoning text accumulated so far, when returned "
      "separately from the answer"
    ),
  )
  reasoning_effort: str | None = Field(
    default=None,
    description="configured reasoning mode used for this completion",
  )
  finish_reason: str = Field(default="", description="provider finish reason")
  error: str | None = Field(default=None, description="transport error for error state")
  prompt_tokens: int = Field(default=0, ge=0)
  completion_tokens: int = Field(default=0, ge=0)
  duration_s: float = Field(default=0.0, ge=0.0, description="round-trip seconds")
  timestamp: datetime = Field(description="reply time, UTC")


class ModelActivity(BaseModel):
  """One primitive execution activity in a model lane."""

  activity_id: int = Field(ge=0, description="monotonic counter within the lane")
  agent_id: str = Field(default="planner", description="logical model lane")
  mission_id: str | None = Field(default=None, description="owning mission, if any")
  step: int | None = Field(default=None, ge=1, description="owning planner step")
  name: str = Field(description="primitive name")
  arguments: dict[str, Any] = Field(default_factory=dict)
  status: Literal["running", "complete", "error"] = "running"
  result: Any | None = None
  error: str | None = None
  started_at: datetime
  finished_at: datetime | None = None


class ModelAgentStatus(BaseModel):
  """Current observable state of one logical model/agent lane."""

  agent_id: str
  thinking: bool = Field(
    description="true while this lane has an in-flight model completion"
  )
  status: Literal["thinking", "working", "idle", "error"]
  exchange: ModelExchange | None = None
  activity: ModelActivity | None = None


class ModelIOFrame(BaseModel):
  """Frame served on the model I/O stream.

  ``exchange`` is ``None`` before the first request; ``total`` is the count
  of model requests since launch (the operator's request counter).
  """

  exchange: ModelExchange | None = None
  total: int = Field(default=0, ge=0, description="model requests since launch")
  agents: list[ModelAgentStatus] = Field(
    default_factory=list,
    description="current state of every model lane seen since launch",
  )
