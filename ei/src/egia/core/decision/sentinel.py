"""Fast side-model sentinel for perception changes during planner inference.

The sentinel never plans work. It reads a bounded safety-focused projection
and emits one tiny verdict. A conservative stop or estop travels through the
monitor latch; a refresh only cancels the stale planner completion. Invalid
or failed sentinel replies never block the primary decision loop.
"""

from __future__ import annotations

import asyncio
import time
from collections.abc import Awaitable, Callable
from dataclasses import dataclass
from datetime import UTC, datetime
from enum import StrEnum
from typing import Literal

from egia.api.model_io import ModelExchange, ModelMessage
from egia.api.observation import ObservationRecord
from egia.core.context import render_dynamic_message
from egia.core.perception.admission import Admission
from egia.internal.model import DecisionModel
from egia.lib.config import AgentPromptConfig, DecisionSentinelConfig
from egia.lib.logger import get_logger
from egia.lib.vlm import ChatMessage, ReasoningEffort

__all__ = ["SafetySentinel", "SentinelSignal", "SentinelSignalKind"]


class SentinelSignalKind(StrEnum):
  REFRESH = "refresh"
  STOP = "stop"
  ESTOP = "estop"


@dataclass(frozen=True)
class SentinelSignal:
  kind: SentinelSignalKind
  detail: str


class SafetySentinel:
  """Continuously judge current safety while one primary completion runs."""

  def __init__(
    self,
    model: DecisionModel,
    queue: Admission,
    config: DecisionSentinelConfig,
    prompts: AgentPromptConfig,
    *,
    token_budget: int = 4096,
    motion_stop: Callable[[], Awaitable[None]],
    engage_estop: Callable[[str], Awaitable[None]],
    exchange_sink: Callable[[ModelExchange], None] | None = None,
  ) -> None:
    self._model = model
    self._queue = queue
    self._config = config
    self._prompts = prompts
    self._token_budget = token_budget
    self._motion_stop = motion_stop
    self._engage_estop = engage_estop
    self._exchange_sink = exchange_sink
    self._exchange_counter = 0
    self._log = get_logger("egia.core.decision.sentinel")
    options = tuple(getattr(model, "reasoning_options", ()))
    self._reasoning_effort: ReasoningEffort | None = (
      "none" if "none" in options else None
    )

  async def watch(self, goal: str, stop: asyncio.Event) -> SentinelSignal | None:
    """Judge new safety-relevant observations until cancelled."""
    revision = self._queue.revision
    seen = {record.id for record in self._queue.records()}
    messages: list[ChatMessage] | None = None
    while not stop.is_set():
      if messages is None:
        changed = await self._queue.wait_for_change(revision, stop)
        if changed is None:
          return None
        revision = changed
        records = self._queue.records()
        new_records = [record for record in records if record.id not in seen]
        seen = {record.id for record in records}
        state = self._render_state(new_records)
        if not state:
          self._log.debug(
            "sentinel_observation_ignored",
            observation_revision=revision,
            kinds=sorted({record.kind for record in new_records}),
          )
          continue
        messages = self._messages(goal, state)
      exchange_id = self._exchange_counter
      self._exchange_counter += 1
      started = time.monotonic()
      self._publish(exchange_id, messages, status="streaming", started=started)
      try:
        response = await self._model.complete(
          messages,
          max_tokens=self._config.max_tokens,
          temperature=0.0,
          reasoning_effort=self._reasoning_effort,
        )
      except asyncio.CancelledError:
        self._publish(exchange_id, messages, status="interrupted", started=started)
        raise
      except Exception as exc:  # noqa: BLE001 — advisory side model never blocks
        self._publish(
          exchange_id,
          messages,
          status="error",
          started=started,
          error=str(exc),
        )
        self._log.warning("sentinel_request_failed", error=str(exc))
        if await self._wait_retry(stop):
          continue
        return None

      self._publish(
        exchange_id,
        messages,
        status="complete",
        started=started,
        response=response,
      )
      messages = None

      signal = _parse_signal(response.text)
      self._log.info(
        "sentinel_verdict",
        verdict=signal.kind.value if signal is not None else "continue",
        detail=signal.detail if signal is not None else "",
        observation_revision=self._queue.revision,
      )
      if signal is not None:
        try:
          if signal.kind is SentinelSignalKind.ESTOP:
            await self._engage_estop(f"model sentinel: {signal.detail}")
          elif signal.kind is SentinelSignalKind.STOP:
            await self._motion_stop()
        except asyncio.CancelledError:
          raise
        except Exception as exc:  # noqa: BLE001 — still refresh the main loop
          self._log.error(
            "sentinel_safety_action_failed",
            verdict=signal.kind.value,
            error=str(exc),
          )
        return signal

    return None

  def _publish(
    self,
    exchange_id: int,
    messages: list[ChatMessage],
    *,
    status: Literal["streaming", "complete", "interrupted", "error"],
    started: float,
    response=None,
    error: str | None = None,
  ) -> None:
    """Project one sentinel completion into the shared model monitor."""
    if self._exchange_sink is None:
      return
    self._exchange_sink(
      ModelExchange(
        exchange_id=exchange_id,
        agent_id="sentinel",
        mission_id="sentinel:watch",
        step=1,
        status=status,
        model=response.model if response is not None else "",
        messages=[
          ModelMessage(role=message.role, content=str(message.content))
          for message in messages
        ],
        reply=response.text if response is not None else "",
        thinking=response.thinking if response is not None else None,
        finish_reason=response.finish_reason if response is not None else "",
        error=error,
        prompt_tokens=response.usage.prompt_tokens if response is not None else 0,
        completion_tokens=response.usage.completion_tokens
        if response is not None
        else 0,
        duration_s=time.monotonic() - started,
        timestamp=datetime.now(UTC),
      )
    )

  async def _wait_retry(self, stop: asyncio.Event) -> bool:
    try:
      await asyncio.wait_for(stop.wait(), timeout=self._config.retry_s)
    except TimeoutError:
      return True
    return False

  @staticmethod
  def _render_state(records: list[ObservationRecord]) -> str:
    state = "\n".join(
      f"- {record.kind}/{record.source}: {record.text[:1000]}"
      for record in records[-16:]
      if record.kind in {"fault", "status", "vision", "scene", "detection", "result"}
    )
    return state

  def _messages(self, goal: str, state: str) -> list[ChatMessage]:
    return [
      ChatMessage(
        role="system",
        content=self._prompts.instruction,
      ),
      ChatMessage(
        role="user",
        content=render_dynamic_message(
          self._prompts,
          {
            "sentinel_goal": goal,
            "sentinel_state": state,
          },
          token_budget=self._token_budget,
        ),
      ),
    ]


def _parse_signal(text: str) -> SentinelSignal | None:
  line = text.strip().splitlines()[0].strip() if text.strip() else ""
  command, _, detail = line.partition(" ")
  command = command.lower()
  if command == "continue":
    return None
  try:
    kind = SentinelSignalKind(command)
  except ValueError:
    return None
  return SentinelSignal(kind=kind, detail=detail.strip() or command)
