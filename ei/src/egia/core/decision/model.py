"""Binding from the decision-model Protocol to the configured VLM endpoint.

:class:`VLMModel` implements :class:`egia.internal.model.DecisionModel` over
the reusable :class:`egia.lib.vlm.VLMClient`; this is where token accounting
and per-request logging attach. Retries and prompt construction belong to the
planner — this module owns exactly one request per call.

Model selection is independent of EOS primitive and skill discovery.
"""

from __future__ import annotations

import asyncio
import contextlib
import time
from collections.abc import Callable
from typing import Literal

from egia.lib.config import ModelConfig
from egia.lib.logger import get_logger
from egia.lib.vlm import ChatMessage, ModelResponse, ModelStreamDelta, VLMClient


class ModelAvailabilityGate:
  """Cancel active completions and pause new ones while backends are unavailable."""

  def __init__(self, *, available: bool = True) -> None:
    self._available = asyncio.Event()
    self._unavailable = asyncio.Event()
    if available:
      self._available.set()
    else:
      self._unavailable.set()

  @property
  def is_available(self) -> bool:
    return self._available.is_set()

  def set_available(self, available: bool) -> None:
    if available:
      self._unavailable.clear()
      self._available.set()
    else:
      self._available.clear()
      self._unavailable.set()

  async def wait_available(self) -> None:
    """Wait until the current backend fleet is safe to use."""
    await self._available.wait()

  async def run(self, completion) -> ModelResponse:
    """Run a completion only during an available epoch, retrying after recovery."""
    while True:
      await self._available.wait()
      task = asyncio.create_task(completion())
      unavailable = asyncio.create_task(self._unavailable.wait())
      try:
        done, _ = await asyncio.wait(
          {task, unavailable}, return_when=asyncio.FIRST_COMPLETED
        )
        if unavailable in done:
          task.cancel()
          with contextlib.suppress(asyncio.CancelledError):
            await task
          continue
        if task in done:
          unavailable.cancel()
          with contextlib.suppress(asyncio.CancelledError):
            await unavailable
          return task.result()
      except asyncio.CancelledError:
        task.cancel()
        unavailable.cancel()
        await asyncio.gather(task, unavailable, return_exceptions=True)
        raise


class AvailabilityModel:
  """Decision-model decorator shared by all configured cognition lanes."""

  def __init__(self, model: VLMModel, gate: ModelAvailabilityGate) -> None:
    self._model = model
    self._gate = gate

  @property
  def reasoning_options(self) -> tuple[str, ...]:
    return self._model.reasoning_options

  @property
  def reasoning_effort(self) -> str | None:
    return self._model.reasoning_effort

  async def complete(self, messages, **kwargs) -> ModelResponse:
    return await self._gate.run(lambda: self._model.complete(messages, **kwargs))


class VLMModel:
  """``DecisionModel`` backed by an OpenAI-compatible chat-completions endpoint."""

  def __init__(self, binding: ModelConfig) -> None:
    self._binding = binding
    self._client = VLMClient.from_config(binding)
    self._log = get_logger("egia.core.decision.model")
    self._log.info(
      "model_bound",
      binding=binding.model,
      model=binding.model,
      endpoint=binding.endpoint,
      timeout_s=binding.timeout_seconds,
      context_window_tokens=binding.context_window_tokens,
      max_tokens=binding.max_tokens,
      reasoning_effort=binding.reasoning_effort,
      reasoning_options=binding.reasoning_options,
    )

  @property
  def reasoning_options(self) -> tuple[str, ...]:
    """Modes the planner may ask this binding to use on a later reply."""
    return tuple(self._binding.reasoning_options)

  @property
  def reasoning_effort(self) -> str | None:
    """The binding default, used only to describe an unoverridden request."""
    return self._binding.reasoning_effort

  async def complete(
    self,
    messages: list[ChatMessage],
    *,
    max_tokens: int | None = None,
    temperature: float | None = None,
    reasoning_effort: Literal["none", "low", "medium", "high"] | None = None,
    on_delta: Callable[[ModelStreamDelta], None] | None = None,
  ) -> ModelResponse:
    started = time.monotonic()
    response = await self._client.complete(
      messages,
      max_tokens=max_tokens,
      temperature=temperature,
      reasoning_effort=reasoning_effort,
      on_delta=on_delta,
    )
    self._log.info(
      "model_completion",
      model=response.model or self._binding.model,
      finish_reason=response.finish_reason,
      prompt_tokens=response.usage.prompt_tokens,
      completion_tokens=response.usage.completion_tokens,
      duration_s=round(time.monotonic() - started, 3),
      reply_chars=len(response.text),
      thinking_chars=len(response.thinking or ""),
    )
    return response
