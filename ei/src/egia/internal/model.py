"""Substitution-seam Protocol for the decision model.

Per ``docs/egia/egia-2607.md`` §3.1: ``core/decision`` talks to the model through
this Protocol, never through a concrete SDK. A new provider — Anthropic,
OpenAI, or a local server speaking the same API — is a new implementation;
nothing above the seam changes.

The interface layer is pure: declarations and data only, no I/O, no state.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Literal, Protocol, runtime_checkable

from egia.lib.vlm import ChatMessage, ModelResponse, ModelStreamDelta

__all__ = ["DecisionModel"]


@runtime_checkable
class DecisionModel(Protocol):
  """One completion request, one response. Retry policy lives in the caller."""

  async def complete(
    self,
    messages: list[ChatMessage],
    *,
    max_tokens: int | None = None,
    temperature: float | None = None,
    reasoning_effort: Literal["none", "low", "medium", "high"] | None = None,
    on_delta: Callable[[ModelStreamDelta], None] | None = None,
  ) -> ModelResponse: ...
