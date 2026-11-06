"""Async data feeds for the console: WebSocket streams and the log tail.

Everything here is transport, no rendering. The feeds own one shared
:class:`ConsoleState` the curses layer reads; each feed reconnects with a
short backoff so a restarting pipeline never wedges the console.
"""

from __future__ import annotations

import asyncio
import json
import os
import uuid
from collections import deque
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

import websockets
from websockets.exceptions import ConnectionClosed, WebSocketException

from egia_ext.tui.config import TuiConfig
from egia_ext.tui.render import event_at_least, parse_jsonl_event

RECONNECT_S = 2.0
LOG_CAP = 500

HEALTH_PATH = "/api/aether/egia/health"
CONTEXT_PATH = "/api/aether/egia/context"
BACKEND_PATH = "/api/aether/egia/backend"
MODEL_IO_PATH = "/api/aether/egia/model_io"
PRIMITIVES_PATH = "/api/aether/egia/primitives"
SKILLS_PATH = "/api/aether/egia/skills"
PROMPTS_PATH = "/api/aether/egia/prompts"
MISSIONS_STREAM_PATH = "/api/aether/egia/missions/stream"
MISSIONS_COMMAND_PATH = "/api/aether/egia/missions/command"
CHAT_PATH = "/api/aether/egia/chat"
INVOKE_PATH = "/api/aether/egia/primitive/invoke"
SKILLS_COMMAND_PATH = "/api/aether/egia/skills/command"
SKILLS_EXECUTION_PATH = "/api/aether/egia/skills/execution"

# The pane shows the pipeline's own story; transport internals (httpx,
# websockets, grpc, asyncio) are noise even when they log real events.
_LOG_LOGGER_PREFIXES = ("egia.", "egia_ext.")


def logger_allowed(logger: str) -> bool:
  """True when a logger's events belong in the operator pane."""
  return logger.startswith(_LOG_LOGGER_PREFIXES)


def replay_tail_offset(path: Path) -> int:
  """Return the current replay-file size, or zero before its first launch.

  Wrap-mode TUI captures this boundary before it starts EGIA, so the log pane
  follows the new pipeline session rather than replaying every prior launch.
  """
  try:
    return path.stat().st_size
  except FileNotFoundError:
    return 0


def session_replay_path(
  config: TuiConfig, session_id: str | None = None
) -> Path | None:
  """Resolve a protected active Island session, or ``None`` when standalone.

  An inherited session identity is authoritative for an Island child. Attach
  mode may use ``current.json`` only while its collector socket still exists;
  this prevents stale metadata from stealing a standalone EGIA replay.
  """
  sessions = config.run_dir / "island/sessions"
  metadata_path = sessions / (f"{session_id}.json" if session_id else "current.json")
  try:
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
  except (OSError, UnicodeDecodeError, json.JSONDecodeError):
    return None
  if not isinstance(metadata, dict):
    return None
  resolved_id = metadata.get("session_id")
  if not isinstance(resolved_id, str) or (session_id and resolved_id != session_id):
    return None
  expected = config.log_dir.parent / "island/sessions" / f"{resolved_id}.jsonl"
  if metadata.get("log_path") != str(expected):
    return None
  if session_id is None:
    socket_path = metadata.get("socket_path")
    if not isinstance(socket_path, str) or not Path(socket_path).exists():
      return None
  return expected


@dataclass
class LogEntry:
  """One rendered log line; ``count`` reflects consecutive repeats."""

  event: dict[str, Any]
  count: int = 1


@dataclass
class ConsoleState:
  """Latest view of every feed; the curses layer renders this as-is."""

  status: dict[str, Any] | None = None
  snapshot: dict[str, Any] | None = None
  backend: dict[str, Any] | None = None
  model_io_latest: dict[str, Any] | None = None
  model_io: dict[str, Any] | None = None
  model_agents: list[dict[str, Any]] = field(default_factory=list)
  model_requests: int = 0
  primitives: dict[str, Any] | None = None
  skills: dict[str, Any] | None = None
  skill_executions: dict[str, dict[str, Any]] = field(default_factory=dict)
  prompts: dict[str, Any] | None = None
  missions: dict[str, Any] | None = None
  processes: list[dict[str, Any]] = field(default_factory=list)
  logs: deque[LogEntry] = field(default_factory=lambda: deque(maxlen=LOG_CAP))
  child_exited: int | None = None
  health_link: str = "…"
  context_link: str = "…"
  backend_link: str = "…"
  model_io_link: str = "…"
  primitives_link: str = "…"
  skills_link: str = "…"
  prompts_link: str = "…"
  missions_link: str = "…"
  dirty: asyncio.Event = field(default_factory=asyncio.Event)


def _append_log(state: ConsoleState, event: dict[str, Any]) -> None:
  """Append an event, collapsing repeats anywhere in the buffer.

  An identical event (level, logger, message) never produces a second
  line: the existing entry is moved to the tail, its timestamp refreshed
  to the latest occurrence, and its counter bumped. Scanning the capped
  deque (≤ LOG_CAP) at the tail's cadence is cheap.
  """
  signature = (event.get("level"), event.get("logger"), event.get("event"))
  for i, entry in enumerate(reversed(state.logs)):
    e = entry.event
    if (e.get("level"), e.get("logger"), e.get("event")) == signature:
      state.logs.remove(entry)
      entry.event = event
      entry.count += 1
      state.logs.append(entry)
      state.dirty.set()
      return
  state.logs.append(LogEntry(event=event))
  state.dirty.set()


def _seen_recently(state: ConsoleState, event: dict[str, Any]) -> bool:
  """True when an identical (event, timestamp) was logged in the last 50."""
  signature = (event.get("event"), event.get("timestamp"))
  for entry in list(state.logs)[-50:]:
    if (entry.event.get("event"), entry.event.get("timestamp")) == signature:
      return True
  return False


async def _ws_feed(
  state: ConsoleState,
  url: str,
  attr: str,
  link_attr: str,
  *,
  apply: Any = None,
) -> None:
  """Consume a JSON-text frame stream into ``state.<attr>``, reconnecting.

  ``apply`` overrides plain attribute assignment for frames needing more
  than one state field (e.g. model I/O's exchange + total).
  """
  while True:
    try:
      setattr(state, link_attr, "…")
      async with websockets.connect(url, open_timeout=RECONNECT_S) as ws:
        setattr(state, link_attr, "up")
        state.dirty.set()
        async for message in ws:
          if isinstance(message, bytes):
            message = message.decode()
          data = json.loads(message)
          if "error" in data:
            break  # pre-readiness gate: reconnect with backoff
          if apply is not None:
            apply(data)
          else:
            setattr(state, attr, data)
          state.dirty.set()
    except (OSError, ConnectionClosed, WebSocketException, json.JSONDecodeError):
      pass
    setattr(state, link_attr, "down")
    state.dirty.set()
    await asyncio.sleep(RECONNECT_S)


async def health_feed(state: ConsoleState, config: TuiConfig) -> None:
  """Track subsystem status from ``/api/aether/egia/health``."""
  await _ws_feed(
    state,
    f"ws://{config.host}:{config.port}{HEALTH_PATH}",
    "status",
    "health_link",
  )


async def context_feed(state: ConsoleState, config: TuiConfig) -> None:
  """Track the live prompt snapshot from ``/api/aether/egia/context``."""
  await _ws_feed(
    state,
    f"ws://{config.host}:{config.port}{CONTEXT_PATH}",
    "snapshot",
    "context_link",
  )


async def backend_feed(state: ConsoleState, config: TuiConfig) -> None:
  """Track backend link state from ``/api/aether/egia/backend``."""
  await _ws_feed(
    state,
    f"ws://{config.host}:{config.port}{BACKEND_PATH}",
    "backend",
    "backend_link",
  )


async def model_io_feed(state: ConsoleState, config: TuiConfig) -> None:
  """Track all model lanes from ``/api/aether/egia/model_io``."""

  def _apply(data: dict[str, Any]) -> None:
    _apply_model_io_frame(state, data)

  await _ws_feed(
    state,
    f"ws://{config.host}:{config.port}{MODEL_IO_PATH}",
    "__model_io__",  # applied through _apply; never set directly
    "model_io_link",
    apply=_apply,
  )


def _apply_model_io_frame(state: ConsoleState, data: dict[str, Any]) -> None:
  """Apply a current multi-agent frame, retaining legacy-frame compatibility.

  Relative panes (context reply and primitive fallback) follow the planner lane,
  never a sentinel/proposer exchange that happened to publish last. The model
  stream itself receives ``model_io_latest`` plus the complete agent roster.
  """
  latest = data.get("exchange")
  state.model_io_latest = latest if isinstance(latest, dict) else None
  raw_agents = data.get("agents", [])
  state.model_agents = (
    [agent for agent in raw_agents if isinstance(agent, dict)]
    if isinstance(raw_agents, list)
    else []
  )
  planner = next(
    (
      agent.get("exchange")
      for agent in state.model_agents
      if agent.get("agent_id") == "planner" and isinstance(agent.get("exchange"), dict)
    ),
    None,
  )
  state.model_io = planner if isinstance(planner, dict) else state.model_io_latest
  state.model_requests = int(data.get("total", 0))


async def primitives_feed(state: ConsoleState, config: TuiConfig) -> None:
  """Track discovered primitives from ``/api/aether/egia/primitives``."""
  await _ws_feed(
    state,
    f"ws://{config.host}:{config.port}{PRIMITIVES_PATH}",
    "primitives",
    "primitives_link",
  )


async def skills_feed(state: ConsoleState, config: TuiConfig) -> None:
  """Track descriptive native skills from ``/api/aether/egia/skills``."""
  await _ws_feed(
    state,
    f"ws://{config.host}:{config.port}{SKILLS_PATH}",
    "skills",
    "skills_link",
  )


async def prompts_feed(state: ConsoleState, config: TuiConfig) -> None:
  """Track the raw planner system prompt from ``/api/aether/egia/prompts``."""
  await _ws_feed(
    state,
    f"ws://{config.host}:{config.port}{PROMPTS_PATH}",
    "prompts",
    "prompts_link",
  )


async def missions_feed(state: ConsoleState, config: TuiConfig) -> None:
  """Track the live mission table from ``/api/aether/egia/missions/stream``."""
  await _ws_feed(
    state,
    f"ws://{config.host}:{config.port}{MISSIONS_STREAM_PATH}",
    "missions",
    "missions_link",
  )


def feedback(state: ConsoleState, text: str, level: str = "info") -> None:
  """Append a console-originated message to the log pane."""
  _append_log(
    state,
    {
      "level": level,
      "logger": "egia_ext.tui",
      "timestamp": datetime.now(UTC).isoformat(),
      "event": text,
    },
  )


async def send_chat(state: ConsoleState, config: TuiConfig, sentence: str) -> None:
  """Push one sentence through ``/api/aether/egia/chat`` and log the verdict.

  One short-lived connection per command: the endpoint holds the socket open
  for multiple sentences, but the console is an occasional ingress, not a
  stream — connect, send, read the ack, close.
  """
  url = f"ws://{config.host}:{config.port}{CHAT_PATH}"
  try:
    async with websockets.connect(url, open_timeout=RECONNECT_S) as ws:
      await ws.send(json.dumps({"sentence": sentence}))
      reply = json.loads(await ws.recv())
  except (OSError, ConnectionClosed, WebSocketException, json.JSONDecodeError) as exc:
    feedback(state, f'chat "{sentence}" failed: {exc}', "error")
    return
  if reply.get("ok"):
    feedback(state, f'chat "{sentence}" admitted ({reply.get("observation_id", "?")})')
  else:
    feedback(state, f'chat "{sentence}" rejected: {reply.get("error", "?")}', "warning")


async def send_mission_command(
  state: ConsoleState, config: TuiConfig, payload: dict[str, Any]
) -> None:
  """Send one raw mission operation and log its typed acknowledgement."""
  operation = str(payload.get("operation", "mission"))
  url = f"ws://{config.host}:{config.port}{MISSIONS_COMMAND_PATH}"
  try:
    async with websockets.connect(url, open_timeout=RECONNECT_S) as ws:
      await ws.send(json.dumps(payload))
      reply = json.loads(await ws.recv())
  except (OSError, ConnectionClosed, WebSocketException, json.JSONDecodeError) as exc:
    feedback(state, f"mission {operation} transport failed: {exc}", "error")
    return
  if not reply.get("ok"):
    error = reply.get("error", "unknown_error")
    reason = reply.get("reason")
    feedback(
      state,
      f"mission {operation} rejected: {error}" + (f" ({reason})" if reason else ""),
      "warning",
    )
    return
  mission = reply.get("mission") if isinstance(reply.get("mission"), dict) else {}
  mission_id = str(mission.get("mission_id", "?"))
  state_name = str(mission.get("state", "?"))
  revision = mission.get("revision", "?")
  feedback(
    state,
    f"mission {operation} acknowledged: {mission_id} {state_name} r{revision}",
  )


async def send_call(
  state: ConsoleState,
  config: TuiConfig,
  primitive: str,
  arguments: dict[str, Any],
  robot_id: str,
  device_id: str | None,
) -> None:
  """Push one operator primitive call through ``/primitive/invoke``.

  Same one-connection-per-command shape as :func:`send_chat`: connect,
  send, read the gate's verdict, close. The safety gate on the pipeline
  side is the only validator — the console never pre-filters arguments.
  The target is the configured robot alias plus the EOS device id. Only the
  process-wide ``safety.estop`` primitive omits ``device_id``.
  """
  target = f"{primitive}@{robot_id}" + (f"/{device_id}" if device_id else "")
  payload: dict[str, Any] = {
    "primitive": primitive,
    "robot_id": robot_id,
    "arguments": arguments,
  }
  if device_id:
    payload["device_id"] = device_id
  url = f"ws://{config.host}:{config.port}{INVOKE_PATH}"
  try:
    async with websockets.connect(url, open_timeout=RECONNECT_S) as ws:
      await ws.send(json.dumps(payload))
      reply = json.loads(await ws.recv())
  except (OSError, ConnectionClosed, WebSocketException, json.JSONDecodeError) as exc:
    feedback(state, f"call {target} failed: {exc}", "error")
    return
  if not reply.get("ok"):
    reason = reply.get("reason") or ""
    feedback(
      state,
      f"call {target} rejected: {reply.get('error', '?')}"
      + (f" ({reason})" if reason else ""),
      "warning",
    )
    return
  if reply.get("accepted"):
    result = reply.get("result")
    feedback(
      state,
      f"call {target} accepted" + (f" → {json.dumps(result)}" if result else ""),
    )
  else:
    feedback(
      state,
      f"call {target} rejected by backend: {reply.get('reason', '?')}",
      "warning",
    )


async def send_skill_command(
  state: ConsoleState, config: TuiConfig, payload: dict[str, Any]
) -> dict[str, Any] | None:
  """Send one skill operation and retain any returned execution snapshot."""
  operation = str(payload.get("operation", "skill"))
  request = dict(payload)
  if operation == "start":
    request.setdefault("request_id", str(uuid.uuid4()))
  url = f"ws://{config.host}:{config.port}{SKILLS_COMMAND_PATH}"
  try:
    async with websockets.connect(url, open_timeout=RECONNECT_S) as ws:
      await ws.send(json.dumps(request))
      reply = json.loads(await ws.recv())
  except (OSError, ConnectionClosed, WebSocketException, json.JSONDecodeError) as exc:
    feedback(state, f"skill {operation} transport failed: {exc}", "error")
    return None
  if not reply.get("ok"):
    reason = reply.get("reason")
    feedback(
      state,
      f"skill {operation} rejected: {reply.get('error', '?')}"
      + (f" ({reason})" if reason else ""),
      "warning",
    )
    return None
  execution = reply.get("execution")
  if isinstance(execution, dict) and execution.get("execution_id"):
    state.skill_executions[str(execution["execution_id"])] = execution
    state.dirty.set()
    feedback(state, _skill_execution_summary(execution, operation))
    return execution
  descriptor = reply.get("descriptor")
  if isinstance(descriptor, dict):
    feedback(
      state,
      f"skill {descriptor.get('name', '?')}@{descriptor.get('version', '?')} found"
      f" on {descriptor.get('backend', '?')}",
    )
  return None


def _skill_execution_summary(execution: dict[str, Any], operation: str) -> str:
  """Return a compact operator message for one complete skill snapshot."""
  text = (
    f"skill {operation}: {execution.get('name', '?')}@{execution.get('version', '?')}"
    f" execution={execution.get('execution_id', '?')}"
    f" request={execution.get('request_id', '?')}"
    f" status={execution.get('status', '?')} seq={execution.get('sequence', '?')}"
  )
  reason = execution.get("reason_code") or execution.get("message")
  return text + (f" ({reason})" if reason else "")


async def watch_skill_execution(
  state: ConsoleState,
  config: TuiConfig,
  robot_id: str,
  execution_id: str,
) -> None:
  """Follow one execution, resuming reconnects after its last seen sequence."""
  current = state.skill_executions.get(execution_id, {})
  after_sequence = int(current.get("sequence", 0))
  terminal = {"succeeded", "rejected", "cancelled", "timed_out", "failed"}
  url = f"ws://{config.host}:{config.port}{SKILLS_EXECUTION_PATH}"
  while True:
    try:
      async with websockets.connect(url, open_timeout=RECONNECT_S) as ws:
        await ws.send(
          json.dumps(
            {
              "robot_id": robot_id,
              "execution_id": execution_id,
              "after_sequence": after_sequence,
            }
          )
        )
        async for message in ws:
          if isinstance(message, bytes):
            message = message.decode()
          snapshot = json.loads(message)
          if "error" in snapshot:
            reason = snapshot.get("reason")
            feedback(
              state,
              f"skill watch rejected: {snapshot.get('error', '?')}"
              + (f" ({reason})" if reason else ""),
              "warning",
            )
            return
          sequence = int(snapshot.get("sequence", 0))
          if sequence <= after_sequence:
            continue
          after_sequence = sequence
          state.skill_executions[execution_id] = snapshot
          state.dirty.set()
          feedback(state, _skill_execution_summary(snapshot, "watch"))
          if snapshot.get("status") in terminal:
            return
    except (OSError, ConnectionClosed, WebSocketException, json.JSONDecodeError):
      await asyncio.sleep(RECONNECT_S)


async def log_tail(
  state: ConsoleState,
  config: TuiConfig,
  *,
  initial_offset: int = 0,
  session_id: str | None = None,
) -> None:
  """Tail the active Island session, falling back to EGIA's durable replay.

  ``initial_offset`` applies only to the standalone replay. Island sessions
  begin at offset zero and attach mode follows guarded ``current.json``
  replacement. Inode replacement and shrinkage are handled for both sources.
  """
  explicit_session = session_id or os.environ.get("ISLAND_SESSION_ID")
  session_path = session_replay_path(config, explicit_session)
  path = session_path or config.replay_path
  offset = 0 if session_path is not None else initial_offset
  file_identity: tuple[int, int] | None = None
  partial = ""
  while True:
    discovered_session_path = session_replay_path(config, explicit_session)
    # Once attached, retain the session through collector shutdown so its
    # final drained records remain visible. A new guarded current session may
    # still replace it in attach mode.
    next_session_path = discovered_session_path or session_path
    next_path = next_session_path or config.replay_path
    if next_path != path:
      session_path = next_session_path
      path = next_path
      offset = 0 if next_session_path is not None else replay_tail_offset(path)
      file_identity = None
      partial = ""
      state.logs.clear()
      state.dirty.set()
    if path.is_file():
      stat = path.stat()
      identity = (stat.st_dev, stat.st_ino)
      if file_identity is not None and identity != file_identity:
        offset = 0  # rotation can replace the inode before size drops below offset
        partial = ""
        if next_session_path is None:
          state.logs.clear()
      elif stat.st_size < offset:
        offset = 0  # replay stream was rotated or replaced
        partial = ""
        if next_session_path is None:
          state.logs.clear()
      file_identity = identity
      with path.open(encoding="utf-8") as fh:
        fh.seek(offset)
        chunk = fh.read()
        offset = fh.tell()
        lines = (partial + chunk).splitlines(keepends=True)
        partial = ""
        if lines and not lines[-1].endswith(("\n", "\r")):
          partial = lines.pop()
        for line in lines:
          event = parse_jsonl_event(line)
          if event is not None and next_session_path is not None:
            component = str(event.get("component", ""))
            if component not in {"island", "eos", "egia", "tui"}:
              continue
            event = dict(event)
            event.setdefault("logger", component)
            event.setdefault(
              "timestamp", event.get("source_timestamp", event.get("collected_at", "-"))
            )
          if (
            event is not None
            and (
              next_session_path is not None
              or logger_allowed(str(event.get("logger", "")))
            )
            and event_at_least(event, config.log_level)
          ):
            _append_log(state, event)
    else:
      await asyncio.sleep(1.0)
      continue
    await asyncio.sleep(0.25)


async def child_stderr_feed(
  state: ConsoleState,
  config: TuiConfig,
  stream,
  *,
  structured_in_session: bool = False,
) -> None:
  """Mirror the child's console output into the log pane.

  The child's console sink is colorized; ANSI codes are stripped here. Lines
  that look like structured console output are deduped against the jsonl tail
  (both sinks see the same events); anything else (tracebacks) passes through
  as a warning-tagged line so it is never invisible.
  """
  import re

  ansi = re.compile(r"\x1b\[[0-9;]*m")
  from egia_ext.tui.render import parse_console_line

  in_traceback = False
  while True:
    raw = await stream.readline()
    if not raw:
      return
    line = ansi.sub("", raw.decode("utf-8", errors="replace")).rstrip("\n")
    if not line:
      in_traceback = False
      continue
    event = parse_console_line(line)
    if event is not None:
      in_traceback = False
      if structured_in_session:
        continue
    if event is None:
      # A traceback is the child dying — say so in red with an honest logger,
      # instead of tagging every frame line WARNING egia.main. The block runs
      # from "Traceback" to the next blank line (frame lines, the exception
      # line, and "During handling..." chains are all part of it).
      if line.startswith("Traceback") or in_traceback:
        in_traceback = True
        level, logger = "error", "child.stderr"
      else:
        in_traceback = False
        level, logger = "warning", "egia.main"
      _append_log(
        state,
        {"level": level, "logger": logger, "timestamp": "-", "event": line},
      )
      continue
    if not logger_allowed(str(event.get("logger", ""))):
      continue
    if not event_at_least(event, config.log_level):
      continue
    if not _seen_recently(state, event):
      _append_log(state, event)
