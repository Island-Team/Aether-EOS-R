"""Mission proposer — autonomous mission origination from perception (§3.9).

The autonomy loop on top of the execution loop. Two clocks wake it:

- **Perception events**: a new ``hearing`` or ``detection`` observation since the
  last check starts a proposal pass — EGIA heard or saw something and must
  decide whether it warrants a mission.
- **Idle reflection**: when no mission runs and the table is empty, a slow
  timer reflects unprompted — idle time is for noticing, not just waiting.

One proposal pass is one model call through the §3.8 invocation policy
(triggers ``PROPOSAL_EVENT`` / ``PROPOSAL_IDLE``, the proposer's own token
budget, the shared cooldown). The model sees the latest context snapshot and
the current mission table — running order and pending orders — so it never
duplicates standing work, and answers with

    {"missions": [{"order", "urgency", "value", "reason"}]}

or ``{"missions": []}``. Proposals are scored (``priority.py``; cost
estimated from the most similar recalled plan, §3.3) and admitted with
``origin="self"`` through the same mission table and the same gate — origin
changes the priority class, never the execution path. A bad reply costs one
call and is dropped.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import sqlite3
import time
from collections.abc import Callable
from datetime import UTC, datetime

from egia.api.model_io import ModelExchange, ModelMessage
from egia.core.context import (
  ContextBuilder,
  render_dynamic_message,
  render_lane_sections,
)
from egia.core.decision.planner import _first_json_object
from egia.core.decision.policy import InvocationPolicy, InvocationTrigger
from egia.core.memory import RecallService
from egia.core.missions import MissionQueue
from egia.core.perception.admission import Admission
from egia.internal.model import DecisionModel
from egia.lib.config import AgentPromptConfig, DecisionConfig, MissionTemplateCatalog
from egia.lib.logger import get_logger
from egia.lib.vlm import ChatMessage

PROPOSAL_SOURCE = "self.proposer"
PERCEPTION_KINDS = ("hearing", "detection")

__all__ = ["proposer_entry"]


def _no_active_mission() -> str | None:
  return None


async def proposer_entry(
  missions: MissionQueue,
  model: DecisionModel,
  context: ContextBuilder,
  queue: Admission,
  recall: RecallService | None,
  prompts: AgentPromptConfig,
  templates: MissionTemplateCatalog,
  config: DecisionConfig,
  ready: asyncio.Event,
  stop: asyncio.Event,
  *,
  active_mission: Callable[[], str | None] = _no_active_mission,
  policy: InvocationPolicy | None = None,
  exchange_sink: Callable[[ModelExchange], None] | None = None,
  start: asyncio.Event | None = None,
  ensure_hearing_response: bool = False,
) -> None:
  """Supervisor entry: propose missions from perception until shutdown."""
  log = get_logger("egia.core.decision.proposer")
  proposer_config = config.proposer
  invocation_policy = policy or InvocationPolicy(
    token_budget=proposer_config.proposal_token_budget,
    min_call_interval_s=config.min_call_interval_s,
  )
  seen: dict[str, str] = {}
  _fresh_perception(queue, seen)  # prime: existing observations are history, not events
  admission_revision = queue.revision
  last_pass: float = 0.0
  last_idle_fingerprint: tuple[object, ...] | None = None
  ready.set()
  publisher = _ExchangePublisher(exchange_sink) if exchange_sink is not None else None
  log.info(
    "proposer_ready",
    idle_reflect_interval_s=proposer_config.idle_reflect_interval_s,
    min_proposal_interval_s=proposer_config.min_proposal_interval_s,
  )
  if start is not None:
    # Like the planner, this optional model caller must not build or send a
    # prompt from partial startup state.
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
      log.info("proposer_stopped")
      return
  while not stop.is_set():
    outcome = await _wait_for_trigger(
      queue,
      stop,
      active_mission,
      missions,
      seen,
      admission_revision,
      proposer_config.idle_reflect_interval_s,
    )
    if outcome is None:
      break
    trigger, admission_revision, fresh_kinds = outcome
    now = time.monotonic()
    debounce = proposer_config.min_proposal_interval_s
    if debounce > 0 and now - last_pass < debounce:
      remaining = debounce - (now - last_pass)
      try:
        await asyncio.wait_for(stop.wait(), timeout=remaining)
      except TimeoutError:
        fresh_kinds.update(_fresh_perception(queue, seen))
        admission_revision = queue.revision
      else:
        break
    if trigger is InvocationTrigger.PROPOSAL_IDLE:
      fingerprint = _proposal_fingerprint(missions, queue)
      if fingerprint == last_idle_fingerprint:
        log.debug("proposal_idle_unchanged")
        continue
      last_idle_fingerprint = fingerprint
    last_pass = time.monotonic()
    admitted, handled = await _propose_pass(
      missions,
      model,
      context,
      recall,
      prompts,
      templates,
      config,
      invocation_policy,
      trigger,
      log,
      publisher,
    )
    if (
      ensure_hearing_response
      and "hearing" in fresh_kinds
      and admitted == 0
      and not handled
    ):
      hearing = queue.latest_by_kind("hearing")
      if hearing is not None:
        fallback = await missions.submit(
          f'Handle the latest heard intent exactly as requested: "{hearing.text}"',
          source="self.hearing-response",
          origin="self",
          urgency=0.7,
          value=0.7,
        )
        if fallback is not None:
          log.info(
            "hearing_response_mission_admitted",
            mission_id=fallback.id,
            observation_id=hearing.id,
          )
  log.info("proposer_stopped")


class _ExchangePublisher:
  """Holds the monotonic exchange counter beside the injected sink."""

  def __init__(self, sink: Callable[[ModelExchange], None]) -> None:
    self._sink = sink
    self.counter = 0

  def publish(
    self, messages: list, response, started_at: float, trigger: InvocationTrigger
  ) -> None:
    self._sink(
      ModelExchange(
        exchange_id=self.counter,
        agent_id="proposer",
        mission_id=f"proposer:{trigger.value}",
        step=1,
        model=response.model,
        messages=[
          ModelMessage(
            role=m.role,
            content=m.content if isinstance(m.content, str) else str(m.content),
          )
          for m in messages
        ],
        reply=response.text,
        thinking=response.thinking,
        finish_reason=response.finish_reason,
        prompt_tokens=response.usage.prompt_tokens,
        completion_tokens=response.usage.completion_tokens,
        duration_s=time.monotonic() - started_at,
        timestamp=datetime.now(UTC),
      )
    )
    self.counter += 1

  def start(self, messages: list, trigger: InvocationTrigger) -> None:
    """Publish the in-flight proposal before awaiting the provider."""
    self._sink(
      ModelExchange(
        exchange_id=self.counter,
        agent_id="proposer",
        mission_id=f"proposer:{trigger.value}",
        step=1,
        status="streaming",
        messages=[
          ModelMessage(
            role=message.role,
            content=(
              message.content
              if isinstance(message.content, str)
              else str(message.content)
            ),
          )
          for message in messages
        ],
        reply="",
        timestamp=datetime.now(UTC),
      )
    )


async def _wait_for_trigger(
  queue: Admission,
  stop: asyncio.Event,
  active_mission: Callable[[], str | None],
  missions: MissionQueue,
  seen: dict[str, str],
  admission_revision: int,
  idle_interval_s: float,
) -> tuple[InvocationTrigger, int, set[str]] | None:
  """Block until a perception event or the idle timer fires; ``None`` on stop.

  The idle timer only counts when EGIA is truly idle — no running mission and
  an empty table; a busy EGIA proposes only on perception events. Queue wakes
  from non-perception kinds (such as primitive results) are spurious here and
  simply re-wait.
  """
  while not stop.is_set():
    changed = asyncio.create_task(queue.wait_for_change(admission_revision, stop))
    stop_task = asyncio.create_task(stop.wait())
    timeout = (
      idle_interval_s if active_mission() is None and not missions.pending() else None
    )
    done, _ = await asyncio.wait(
      {changed, stop_task}, timeout=timeout, return_when=asyncio.FIRST_COMPLETED
    )
    for task in (changed, stop_task):
      task.cancel()
    if stop.is_set() or stop_task in done:
      return None
    if changed in done and (changed.cancelled() or changed.result() is None):
      return None
    if timeout is not None and not done:
      return InvocationTrigger.PROPOSAL_IDLE, admission_revision, set()
    if changed in done:
      next_revision = changed.result()
      assert next_revision is not None
      admission_revision = next_revision
    fresh = _fresh_perception(queue, seen)
    if fresh:
      return InvocationTrigger.PROPOSAL_EVENT, admission_revision, fresh
    # Spurious wake (primitive-result observation, or a stop race) — loop and
    # wait again.
  return None


def _fresh_perception(queue: Admission, seen: dict[str, str]) -> set[str]:
  """Record and return the hearing/scene kinds that advanced."""
  fresh: set[str] = set()
  for kind in PERCEPTION_KINDS:
    latest = queue.latest_by_kind(kind)
    if latest is None:
      continue
    if seen.get(kind) != latest.id:
      seen[kind] = latest.id
      fresh.add(kind)
  return fresh


def _proposal_fingerprint(
  missions: MissionQueue, queue: Admission
) -> tuple[object, ...]:
  """Return material idle state, excluding continuously changing body telemetry."""
  mission_state = tuple(
    (record.id, record.state.value, record.preempted_count)
    for record in missions.records()
  )
  perception_state = tuple(
    (record.kind, record.source, record.id)
    for record in queue.records()
    if record.kind != "body"
  )
  return mission_state, perception_state


async def _propose_pass(
  missions: MissionQueue,
  model: DecisionModel,
  context: ContextBuilder,
  recall: RecallService | None,
  prompts: AgentPromptConfig,
  templates: MissionTemplateCatalog,
  config: DecisionConfig,
  policy: InvocationPolicy,
  trigger: InvocationTrigger,
  log,
  publisher: _ExchangePublisher | None = None,
) -> tuple[int, bool]:
  decision = policy.admit(trigger)
  if not decision.call and decision.wait_s > 0:
    await asyncio.sleep(decision.wait_s)
    decision = policy.admit(trigger)
  if not decision.call:
    log.warning("proposal_denied", reason=decision.reason, trigger=trigger.value)
    return 0, False
  messages = _build_messages(missions, context, prompts, templates)
  if publisher is not None:
    publisher.start(messages, trigger)
  started = time.monotonic()
  try:
    response = await model.complete(
      messages,
      temperature=config.temperature,
      max_tokens=config.proposer.proposal_max_tokens,
    )
  except Exception as exc:  # noqa: BLE001 — a failed proposal must never
    # crash the proposer subsystem; it costs one pass and the next trigger
    # retries.
    log.warning("proposal_model_failed", error=str(exc))
    return 0, False
  policy.record(response.usage.total_tokens)
  if publisher is not None:
    publisher.publish(messages, response, started, trigger)
  proposals = _parse_proposals(response.text)
  if proposals is None:
    log.warning(
      "proposal_parse_failed", duration_s=round(time.monotonic() - started, 3)
    )
    return 0, False
  admitted = 0
  handled = False
  for proposal in proposals:
    order = proposal.get("order")
    if not isinstance(order, str) or not order.strip():
      continue
    handled = True
    if missions.self_pending_count() >= config.proposer.max_self_pending:
      log.info("proposal_self_cap", cap=config.proposer.max_self_pending)
      break
    selected_template = None
    template_id = proposal.get("mission_template")
    if template_id is not None:
      if not isinstance(template_id, str) or not template_id.strip():
        log.warning("proposal_template_invalid", value=template_id)
        continue
      selected_template = templates.resolve(template_id.strip())
      if selected_template is None:
        log.warning("proposal_template_unknown", template_id=template_id)
        continue
    admitted_order = (
      selected_template.mission if selected_template is not None else order
    )
    record = await missions.submit(
      order,
      source=PROPOSAL_SOURCE,
      origin="self",
      urgency=_clamp_hint(proposal.get("urgency")),
      value=_clamp_hint(proposal.get("value")),
      est_cost_s=_estimate_cost(recall, admitted_order, log),
      template=selected_template,
    )
    if record is not None:
      admitted += 1
      log.info(
        "mission_proposed",
        mission_id=record.id,
        order=record.order[:80],
        priority=round(record.priority, 3),
        mission_template=record.mission_template_id,
      )
  log.info(
    "proposal_pass", trigger=trigger.value, proposals=len(proposals), admitted=admitted
  )
  return admitted, handled


def _build_messages(
  missions: MissionQueue,
  context: ContextBuilder,
  prompts: AgentPromptConfig,
  templates: MissionTemplateCatalog,
) -> list[ChatMessage]:
  catalog = [
    (
      f"- {template.id}@{template.version}: {template.description}; "
      f"Mission={json.dumps(template.mission, ensure_ascii=False)}"
    )
    for template in templates.templates
  ]
  values = {
    "proposer_templates": "\n".join(catalog) if catalog else "- none",
    "proposer_missions": missions.render_for_prompt(),
  }
  reserved = "\n\n".join(text for _, text in render_lane_sections(prompts, values))
  snapshot = context.build(reserved_text=reserved, exclude_kinds={"missions"})
  frequencies = context.observe_dynamic(values)
  return [
    ChatMessage(role="system", content=prompts.instruction),
    ChatMessage(
      role="user",
      content=render_dynamic_message(
        prompts,
        values,
        context=snapshot,
        token_budget=context.token_budget,
        change_frequencies=frequencies,
      ),
    ),
  ]


def _parse_proposals(text: str) -> list[dict] | None:
  """Parse ``{"missions": [...]}`` from a reply; ``None`` when malformed."""
  candidate = _first_json_object(text)
  if not isinstance(candidate, dict):
    return None
  missions = candidate.get("missions")
  if not isinstance(missions, list):
    return None
  return [item for item in missions if isinstance(item, dict)]


def _clamp_hint(value: object) -> float:
  if isinstance(value, (int, float)):
    return max(0.0, min(1.0, float(value)))
  return 0.5


def _estimate_cost(recall: RecallService | None, order: str, log) -> float | None:
  """Measured duration of the most similar recalled plan (§3.3), if any."""
  if recall is None:
    return None
  try:
    plans = recall.search_plans(order, 1)
  except sqlite3.ProgrammingError as exc:
    log.warning("proposal_recall_failed", error=str(exc))
    return None
  if not plans or not plans[0].steps:
    return None
  return sum(step.duration_s for step in plans[0].steps)
