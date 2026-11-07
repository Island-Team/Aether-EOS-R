"""The line-oriented language between the model and primitive invocations.

The model writes CLI-like commands. This adapter turns them into the same
``ActionInvocation`` and plan edits the planner, safety gate, and backend
validate. The accepted forms are plan edits, an optional reasoning request,
one named step followed by its primitive lines, or one final line. A fast-model
reply may delegate complex planning while also returning one safe ready step.
"""

from __future__ import annotations

import re
import shlex
from dataclasses import dataclass, field

from egia.api.plan import (
  PlanEdit,
  PlanEditKind,
)
from egia.internal.decision import ActionInvocation

__all__ = ["CommandReply", "parse_command_reply", "parse_primitive_command"]

_INT = re.compile(r"^-?\d+$")
_FLOAT = re.compile(r"^-?(?:\d+\.\d*|\d*\.\d+)(?:[eE][+-]?\d+)?$")


@dataclass
class CommandReply:
  """Normalized planner reply produced by the command parser."""

  plan_edits: list[PlanEdit]
  actions: list[ActionInvocation]
  final: str | None
  next_reasoning_effort: str | None
  plan_step_id: str | None = None
  commands: list[str] = field(default_factory=list)
  reasoning_request: str | None = None


def parse_command_reply(text: str) -> CommandReply | None:
  """Parse one command reply; return ``None`` when it is not this language.

  Blank lines and a surrounding code fence are tolerated. Primitive lines
  following one ``step`` header form one concurrent barrier. The adapter is
  all-or-nothing so a malformed line never causes partial work.
  """
  lines = _lines(text)
  if not lines or any(line.lstrip().startswith("{") for line in lines):
    return None

  effort: str | None = None
  reasoning_request: str | None = None
  plan_edits: list[PlanEdit] = []
  commands: list[str] = []
  for line in lines:
    lowered = line.lower()
    if lowered.startswith("effort "):
      tokens = _tokens(line)
      if (
        tokens is None
        or len(tokens) != 2
        or effort is not None
        or plan_edits
        or reasoning_request is not None
        or commands
      ):
        return None
      effort = tokens[1]
      continue
    if lowered.startswith("plan ") and _is_plan_edit(line):
      if reasoning_request is not None or commands:
        return None
      operation = _plan_edit(line)
      if operation is None:
        return None
      plan_edits.append(operation)
      continue
    if lowered.startswith("reason "):
      if reasoning_request is not None or commands:
        return None
      reasoning_request = line.split(maxsplit=1)[1].strip()
      if not reasoning_request:
        return None
      continue
    commands.append(line)

  if not commands:
    if not plan_edits and reasoning_request is None:
      return None
    return CommandReply(
      plan_edits, [], None, effort, reasoning_request=reasoning_request
    )

  if commands[0].lower().startswith("plan "):
    return None

  if len(commands) == 1 and commands[0].lower().startswith("final "):
    if reasoning_request is not None:
      return None
    final = commands[0].split(maxsplit=1)[1].strip()
    if not final:
      return None
    return CommandReply(plan_edits, [], final, effort)

  header = _tokens(commands[0])
  if header is not None and len(header) == 2 and header[0].lower() == "step":
    raw_commands = commands[1:]
    if not raw_commands or any("&" in (_tokens(line) or []) for line in raw_commands):
      return None
    actions = [_command(line) for line in raw_commands]
    if not all(action is not None for action in actions):
      return None
    invocations = [action for action in actions if action is not None]
    if not _independent(invocations):
      return None
    return CommandReply(
      plan_edits,
      invocations,
      None,
      effort,
      plan_step_id=header[1],
      commands=raw_commands,
      reasoning_request=reasoning_request,
    )
  return None


def parse_primitive_command(text: str) -> ActionInvocation | None:
  """Parse text only when it is exactly one targeted primitive command."""
  lines = _lines(text)
  if len(lines) != 1:
    return None
  return _action(lines[0], require_full_target=True)


def _command(line: str) -> ActionInvocation | None:
  """Parse either one primitive command or the explicit skill form."""
  tokens = _tokens(line)
  if tokens and tokens[0].lower() == "skill":
    return _skill(tokens)
  return _action(line)


def _skill(tokens: list[str]) -> ActionInvocation | None:
  if len(tokens) < 3:
    return None
  name, separator, version = tokens[1].partition("@")
  values = _assignments(tokens[2:])
  if not separator or not name or not version or values is None:
    return None
  robot = values.pop("robot", None)
  if not isinstance(robot, str) or not robot or "device" in values:
    return None
  intent = values.pop("intent", "")
  preconditions = values.pop("preconditions", [])
  return ActionInvocation(
    action_kind="skill",
    action=name,
    version=version,
    robot_id=robot,
    intent=intent if isinstance(intent, str) else "",
    preconditions=_strings(preconditions, separator="|"),
    arguments=values,
  )


def _lines(text: str) -> list[str]:
  lines = [line.strip() for line in text.strip().splitlines()]
  if lines and lines[0].startswith("```"):
    lines = lines[1:]
  if lines and lines[-1] == "```":
    lines = lines[:-1]
  return [line for line in lines if line]


def _tokens(line: str) -> list[str] | None:
  try:
    return shlex.split(line, comments=False, posix=True)
  except ValueError:
    return None


def _is_plan_edit(line: str) -> bool:
  tokens = _tokens(line)
  return bool(
    tokens
    and len(tokens) >= 3
    and tokens[1].lower() in {"add", "edit", "remove", "complete", "retry", "fail"}
  )


def _plan_edit(line: str) -> PlanEdit | None:
  tokens = _tokens(line)
  if tokens is None or len(tokens) < 3:
    return None
  operation, step_id = tokens[1].lower(), tokens[2]
  if operation in {"remove", "retry"} and len(tokens) == 3:
    return PlanEdit(op=PlanEditKind(operation), step_id=step_id)
  if operation in {"add", "edit", "complete", "fail"} and len(tokens) >= 4:
    sentence = " ".join(tokens[3:]).strip()
    if not sentence:
      return None
    return PlanEdit(op=PlanEditKind(operation), step_id=step_id, sentence=sentence)
  return None


def _action(line: str, *, require_full_target: bool = False) -> ActionInvocation | None:
  tokens = _tokens(line)
  if tokens is None or not tokens:
    return None
  name, separator, named_target = tokens[0].partition("@")
  if "." not in name or not separator:
    return None
  values = _assignments(tokens[1:])
  if values is None:
    return None
  if "robot" in values or "device" in values:
    return None
  robot, target_separator, device = named_target.partition("/")
  if not robot or (require_full_target and (not target_separator or not device)):
    return None
  if not target_separator:
    device = None
  intent = values.pop("intent", "")
  preconditions = values.pop("preconditions", [])
  return ActionInvocation(
    action=name,
    robot_id=robot,
    device_id=device if isinstance(device, str) and device else None,
    intent=intent if isinstance(intent, str) else "",
    preconditions=_strings(preconditions, separator="|"),
    arguments=values,
  )


def _assignments(tokens: list[str]) -> dict[str, object] | None:
  values: dict[str, object] = {}
  for token in tokens:
    key, separator, raw = token.partition("=")
    if not separator or not key or key in values:
      return None
    try:
      values[key] = _value(key, raw)
    except ValueError:
      return None
  return values


def _value(key: str, raw: str) -> object:
  lowered = raw.lower()
  if lowered == "true":
    return True
  if lowered == "false":
    return False
  if lowered in {"null", "none"}:
    return None
  if key in {"fields", "after", "success", "failure"}:
    return [] if raw in {"", "-"} else raw.split(",")
  if key in {"requires", "preconditions"}:
    return [] if raw in {"", "-"} else raw.split("|")
  if _INT.fullmatch(raw):
    return int(raw)
  if _FLOAT.fullmatch(raw):
    return float(raw)
  if "," in raw:
    return [_value("", item) for item in raw.split(",")]
  return raw


def _strings(value: object, *, separator: str = ",") -> list[str]:
  if value is None:
    return []
  if isinstance(value, list):
    return [str(item) for item in value if str(item)]
  if isinstance(value, str):
    return [item for item in value.split(separator) if item and item != "-"]
  return []


def _independent(actions: list[ActionInvocation]) -> bool:
  """Reject concurrency groups with a provable shared resource.

  General causal independence remains model judgment, but explicit robot,
  device, object, and destination identities are hard resource claims. Two
  commands claiming one of them cannot safely start in the same barrier.
  """
  claimed: set[tuple[str, str]] = set()
  for action in actions:
    resources = {("resource", action.robot_id), *_resource_claims(action.arguments)}
    if action.device_id:
      resources.add(("resource", f"{action.robot_id}/{action.device_id}"))
    if claimed.intersection(resources):
      return False
    claimed.update(resources)
  return True


def _resource_claims(arguments: dict) -> set[tuple[str, str]]:
  claims: set[tuple[str, str]] = set()
  for key, value in arguments.items():
    if isinstance(value, str) and (key.endswith("_id") or key == "to_station"):
      claims.add(("resource", value))
  return claims
