"""Context compression — prompt snapshot assembly from the state queue."""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import Awaitable, Callable, Mapping

from egia.api.context import ContextSnapshot
from egia.api.prompts import PromptsSnapshot
from egia.core.context.builder import ContextBuilder
from egia.core.context.detection_digest import build_detection_digest
from egia.core.context.dynamic import render_dynamic_message, render_lane_sections
from egia.core.context.hardware import render_hardware
from egia.core.context.mode import ModeState
from egia.core.context.object_digest import build_object_digest
from egia.core.perception.admission import Admission
from egia.lib.config import PromptConfig
from egia.lib.logger import get_logger


async def context_entry(
  queue: Admission,
  prompts: PromptConfig,
  ready: asyncio.Event,
  stop: asyncio.Event,
  *,
  publish: Callable[[ContextSnapshot], Awaitable[None]] | None = None,
  backend_render: Callable[[], str] | None = None,
  mode_state: ModeState | None = None,
  dynamic_renderers: Mapping[str, Callable[[], str]] | None = None,
  mission_changed: asyncio.Event | None = None,
  system_prompt: PromptsSnapshot | None = None,
  start: asyncio.Event | None = None,
) -> None:
  """Supervisor entry: rebuild context whenever admitted state changes.

  Args:
    publish: Optional northbound exchange (``egia.io``'s ``ContextStore``);
      each rebuilt snapshot is forwarded to it so the ``/context`` stream and
      operator consoles see the live prompt. Unwired in tests and standalone
      use.
    backend_render: Optional zero-arg callable rendering the backend link
      line appended to every snapshot (see :mod:`egia.core.link.status`).
    dynamic_renderers: Live renderers for discovered primitives and skills.
  """
  log = get_logger("egia.core.context")
  builder = ContextBuilder(
    queue,
    prompts,
    backend_render=backend_render,
    mode_state=mode_state,
    dynamic_renderers=dynamic_renderers,
    system_prompt=system_prompt,
  )
  ready.set()
  log.info("context_ready")
  if start is not None:
    # Context is initialized now, but the first prompt is deliberately held
    # until the supervisor confirms that every module (including the model
    # caller) is ready.  This keeps startup-only partial state out of both the
    # operator log and the first model request.
    start_task = asyncio.create_task(start.wait())
    stop_task = asyncio.create_task(stop.wait())
    done, pending = await asyncio.wait(
      {start_task, stop_task}, return_when=asyncio.FIRST_COMPLETED
    )
    for task in pending:
      task.cancel()
    for task in pending:
      with contextlib.suppress(asyncio.CancelledError):
        await task
    if stop_task in done:
      log.info("context_stopped")
      return
  snapshot = builder.build()
  if publish is not None:
    await publish(snapshot)
  admission_revision = queue.revision

  async def _rebuild() -> None:
    snapshot = builder.build()
    if publish is not None:
      await publish(snapshot)

  while not stop.is_set():
    admission_task = asyncio.create_task(
      queue.wait_for_change(admission_revision, stop)
    )
    mission_task = (
      asyncio.create_task(mission_changed.wait())
      if mission_changed is not None
      else None
    )
    wait_set = {admission_task}
    if mission_task is not None:
      wait_set.add(mission_task)
    try:
      await asyncio.wait(wait_set, return_when=asyncio.FIRST_COMPLETED)
    finally:
      for task in wait_set:
        task.cancel()
      for task in wait_set:
        with contextlib.suppress(asyncio.CancelledError):
          await task
    if stop.is_set():
      break
    if not admission_task.cancelled():
      next_revision = admission_task.result()
      if next_revision is not None:
        admission_revision = next_revision
    if mission_changed is not None and mission_changed.is_set():
      mission_changed.clear()
    await _rebuild()
  log.info("context_stopped")


__all__ = [
  "ContextBuilder",
  "ContextSnapshot",
  "ModeState",
  "build_detection_digest",
  "build_object_digest",
  "context_entry",
  "render_dynamic_message",
  "render_hardware",
  "render_lane_sections",
]
