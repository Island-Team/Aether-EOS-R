"""Profile-writer seam — the model composes the job note, the seam polices it.

One completion path for decomposition: the mission order plus the primitive
catalog and the existing profile catalog go in; the model replies with a
profile TOML; the seam validates the skeleton, cross-checks primitive names
against the live registry, retries with the validation error fed back, and
stores the result. The model never sees the registry object or the store —
prompt text in, TOML text out, everything mechanical stays here.

Wired config-gated by the coordination entry (§3.5 work); the serial
planner path never constructs this class, so default behavior is
unchanged.
"""

from __future__ import annotations

import tomllib

from pydantic import ValidationError

from egia.core.context import render_dynamic_message
from egia.core.decision.profiles import (
  Profile,
  ProfileStore,
  render_catalog,
  unknown_primitives,
)
from egia.core.registry import PrimitiveRegistry
from egia.internal.model import DecisionModel
from egia.lib.config import AgentPromptConfig
from egia.lib.logger import get_logger
from egia.lib.vlm import ChatMessage, ReasoningEffort

__all__ = ["ProfileWriter", "ProfileWriterError"]


class ProfileWriterError(Exception):
  """The model could not produce a valid profile within the attempt budget.

  Carries the last validation reason so the caller (mission loop) can fail
  the mission with a diagnosable error instead of a silent gap.
  """


def _extract_toml(text: str) -> str:
  """The TOML body of a reply — fenced block when fenced, else the text."""
  if "```" in text:
    segments = text.split("```")
    for index in range(1, len(segments), 2):
      body = segments[index]
      # Drop an optional language tag on the opening fence.
      first_line_end = body.find("\n")
      head = body if first_line_end < 0 else body[:first_line_end]
      candidate = body if first_line_end < 0 else body[first_line_end + 1 :]
      if head.strip().lower() in ("", "toml") or head.strip().startswith("["):
        return candidate.strip()
    # Fall through: no usable fence content, try the raw text.
  return text.strip()


class ProfileWriter:
  """Decompose one order into a stored profile via the decision model."""

  def __init__(
    self,
    model: DecisionModel,
    registry: PrimitiveRegistry,
    store: ProfileStore,
    *,
    prompts: AgentPromptConfig,
    token_budget: int = 4096,
    max_attempts: int = 3,
    reasoning_effort: ReasoningEffort | None = None,
  ) -> None:
    self._model = model
    self._registry = registry
    self._store = store
    self._prompts = prompts
    self._token_budget = token_budget
    self._max_attempts = max(1, max_attempts)
    options = tuple(getattr(model, "reasoning_options", ()))
    self._reasoning_effort = reasoning_effort if reasoning_effort in options else None
    self._log = get_logger("egia.core.decision.profile_writer")

  async def write(self, order: str) -> Profile:
    """Write (or reuse) the profile for one order.

    Raises:
      ProfileWriterError: When every attempt fails validation; the cause is
        the last validation error, and nothing was stored.
    """
    existing = self._store.load_all()
    last_reason = "no attempt made"
    for attempt in range(1, self._max_attempts + 1):
      messages = [
        ChatMessage(role="system", content=self._prompts.instruction),
        ChatMessage(
          role="user",
          content=self._dynamic_prompt(
            order,
            existing,
            feedback=last_reason if attempt > 1 else None,
          ),
        ),
      ]
      if self._reasoning_effort is None:
        response = await self._model.complete(messages)
      else:
        response = await self._model.complete(
          messages, reasoning_effort=self._reasoning_effort
        )
      reply = response.text
      profile, reason = self._validate(reply)
      if profile is not None:
        self._store.save(profile)
        self._log.info(
          "profile_written",
          order=order,
          profile=profile.name,
          version=profile.version,
          steps=len(profile.steps),
          attempts=attempt,
        )
        return profile
      last_reason = reason
      self._log.warning("profile_rejected", order=order, attempt=attempt, reason=reason)
    raise ProfileWriterError(
      f"profile for order {order!r} rejected {self._max_attempts} time(s): {last_reason}"
    )

  def _dynamic_prompt(
    self,
    order: str,
    existing: list[Profile],
    *,
    feedback: str | None = None,
  ) -> str:
    return render_dynamic_message(
      self._prompts,
      {
        "profile_primitives": self._registry.render_for_prompt(),
        "profile_existing": render_catalog(existing) if existing else None,
        "profile_mission": order,
        "profile_feedback": feedback,
      },
      token_budget=self._token_budget,
    )

  def _validate(self, reply: str) -> tuple[Profile | None, str]:
    """Parse and fully validate one reply; ``(None, reason)`` on any failure."""
    try:
      document = tomllib.loads(_extract_toml(reply))
    except tomllib.TOMLDecodeError as exc:
      return None, f"TOML parse error: {exc}"
    try:
      profile = Profile.model_validate(document)
    except ValidationError as exc:
      return None, str(exc)
    unregistered = unknown_primitives(profile, self._registry)
    if unregistered:
      return None, f"unknown primitive name(s): {', '.join(unregistered)}"
    return profile, ""
