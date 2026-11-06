"""Pure formatters for the console panes — no curses, fully testable.

Each function returns ``Styled`` lines: a text string plus a style tag the
curses layer maps to a color pair. Style vocabulary (kept small on purpose):

- ``title``    pane titles and section headers
- ``good``     ready/healthy/with-data — green
- ``warn``     starting/stopping/empty sections — yellow
- ``bad``      degraded/failed/restarts — red
- ``accent``   subsystem names, log logger names — blue
- ``dim``      timestamps, idle text, hints — dimmed
- ``plain``    body text
"""

from __future__ import annotations

import json
import math
import re
import textwrap
from typing import Any

from egia_ext.tui.config import level_value

Styled = tuple[str, str]
# A rendered line is either one (text, style) pair or a list of such
# segments — the curses layer paints segments left-to-right, each with its
# own color, so a field bullet can color the name, type, and flags apart.
Line = Styled | list[Styled]

_INLINE_TOKEN = re.compile(r"(\[[^\]\n]+\]|`[^`\n]+`)")
_INLINE_LABEL = re.compile(r"^(\s*)([A-Za-z][A-Za-z0-9 _/-]{0,30}:)(.*)$")


def _describe_type(spec: dict[str, Any]) -> str:
  """One-phrase human type for a JSON Schema fragment."""
  enum = spec.get("enum")
  if isinstance(enum, list) and enum:
    return "one of " + " | ".join(str(v) for v in enum)
  kind = spec.get("type", "any")
  if kind == "array":
    items = spec.get("items")
    inner = _describe_type(items) if isinstance(items, dict) else "any"
    return f"list of {inner}"
  if kind == "object":
    return "object"
  return str(kind)


def wrap_segments(segments: list[Styled], width: int, indent: str = "") -> list[Line]:
  """Word-wrap segmented text so each painted line fits in ``width``.

  Words keep the style of the segment they start in (segments align with
  phrase boundaries, so a word never straddles a style change); continuation
  lines are prefixed with ``indent``. Long words hard-break like
  :func:`wrap_lines`.
  """
  words: list[tuple[str, str]] = []
  for text, style in segments:
    for word in text.split(" "):
      if word:
        words.append((word, style))
  body = max(width - len(indent), 1)
  lines: list[list[Styled]] = []
  current: list[tuple[str, str]] = []
  used = 0
  for word, style in words:
    need = len(word) + (1 if current else 0)
    if current and used + need > body:
      lines.append(current)
      current = []
      used = 0
      need = len(word)
    while len(word) > body:  # hard-break tokens longer than a line
      if current:
        lines.append(current)
        current, used = [], 0
      lines.append([(word[:body], style)])
      word = word[body:]
    current.append((word, style))
    used += need
  if current:
    lines.append(current)
  out: list[Line] = []
  for i, words_ in enumerate(lines):
    segs: list[Styled] = []
    if i > 0 and indent:
      segs.append((indent, "dim"))
    for word, style in words_:
      if segs and segs[-1][1] == style:
        segs[-1] = (segs[-1][0] + " " + word, style)
      else:
        segs.append((" " + word if len(segs) > 1 else word, style))
    out.append(segs if len(segs) > 1 else segs[0])
  return out or [("", "plain")]


def _describe_fields(
  schema: dict[str, Any],
  required: set[str],
  indent: str,
  out: list[Line],
  width: int,
) -> None:
  """Render an object schema's fields as ``• name — type, required. desc``.

  Each bullet is a segmented line: name in ``accent``, the type plain, the
  required/optional flag green/dim, and the description dim — the fields an
  operator scans for stand out from the prose.
  """
  for name, spec in schema.get("properties", {}).items():
    if not isinstance(spec, dict):
      continue
    need, need_style = ("required", "good") if name in required else ("optional", "dim")
    bullet: list[Styled] = [
      ("•", "plain"),
      (name, "accent"),
      ("—", "dim"),
      (_describe_type(spec), "plain"),
      (need, need_style),
    ]
    desc = str(spec.get("description", "")).strip()
    if desc:
      bullet.append((desc, "dim"))
    body = max(width - len(indent), 8)
    out.extend(wrap_segments(bullet, body, indent=indent + "  "))
    if spec.get("type") == "object" and isinstance(spec.get("properties"), dict):
      _describe_fields(spec, set(spec.get("required", [])), indent + "  ", out, width)


def format_primitive_overview(
  instruction: str, primitives: list[dict[str, Any]], width: int
) -> list[Line]:
  """Page 0 of the primitives pane: instruction gist and catalog index."""
  body = max(width, 16)
  lines: list[Line] = [("primitives · overview (← ▶ browse)", "title")]
  if instruction:
    lines.append(("", "plain"))
    for wrapped in wrap_lines(instruction, body)[:12]:
      lines.append((wrapped, "dim"))
  lines.append(("", "plain"))
  lines.append((f"{len(primitives)} primitives — ◀ ▶ for one page each:", "title"))
  for i, primitive in enumerate(primitives):
    label = f"{primitive['name']} v{primitive['version']}"
    if primitive.get("robot_id"):
      label += f"@{primitive['robot_id']}/{primitive['device_id']}"
    elif primitive.get("backend"):
      label += f"@{primitive['backend']}"
    desc = _clip(str(primitive["description"]), max(body - len(label) - 6, 8))
    lines.append(
      [
        (f"  {i + 1:>2}. ", "dim"),
        (label, "accent"),
        (" — ", "dim"),
        (desc, "plain"),
      ]
    )
  return lines


def format_primitive_page(
  primitive: dict[str, Any],
  width: int,
  *,
  index: int,
  count: int,
  sub: int = 1,
  subs: int = 1,
) -> list[Line]:
  """One primitive per page: identity, description, semantic argument list.

  When a primitive overflows the pane it is split into continuation
  pages (``sub`` is the one being shown); the header carries both counters.
  """
  body = max(width, 16)
  header = f"primitives · {index}/{count}"
  if subs > 1:
    header += f" · page {sub}/{subs}"
  header += " ◀ ▶"
  lines: list[Line] = [(header, "title"), ("", "plain")]
  lines.extend(format_primitive_semantic(primitive, body))
  return lines


def call_target(primitive: dict[str, Any]) -> str:
  """Return the explicit ``name@robot/device`` primitive target."""
  robot_id = primitive.get("robot_id")
  if robot_id:
    return f"{primitive['name']}@{robot_id}/{primitive.get('device_id') or ''}".rstrip(
      "/"
    )
  backend = primitive.get("backend")
  return f"{primitive['name']}@{backend}" if backend else str(primitive["name"])


def format_primitive_semantic(primitive: dict[str, Any], width: int) -> list[Line]:
  """One primitive as text — identity, description, semantic arguments.

  Never JSON: fields render as colored ``• name — type, required. desc``
  bullets (markdown-ish prose), the same block the paginated primitives pane
  and the context side column show.
  """
  body = max(width, 16)
  header = f"{primitive['name']}  v{primitive['version']}"
  if primitive.get("robot_id"):
    header += f"  @{primitive['robot_id']}/{primitive['device_id']}"
  elif primitive.get("backend"):
    header += f"  @{primitive['backend']}"
  lines: list[Line] = [(header, "accent")]
  for wrapped in wrap_indented(str(primitive["description"]), body):
    lines.append((wrapped, "plain"))
  device_description = str(primitive.get("device_description", "")).strip()
  if device_description:
    for wrapped in wrap_indented(f"device: {device_description}", body):
      lines.append((wrapped, "dim"))
  if primitive.get("operator"):
    lines.append(("  operator-only primitive", "warn"))
  limits = primitive.get("limits")
  if isinstance(limits, dict) and limits:
    lines.append(("  discovered limits:", "title"))
    for name, interval in limits.items():
      if not isinstance(interval, dict):
        continue
      lower = interval.get("minimum")
      upper = interval.get("maximum")
      lower_mark = "(" if interval.get("minimum_exclusive") else "["
      upper_mark = ")" if interval.get("maximum_exclusive") else "]"
      lines.append((f"    {name}: {lower_mark}{lower}, {upper}{upper_mark}", "dim"))
  lines.append(("", "plain"))
  schema = primitive.get("schema")
  if not isinstance(schema, dict) or "properties" not in schema:
    lines.append(("(no argument schema)", "dim"))
    return lines
  lines.append(("arguments:", "title"))
  required = set(schema.get("required", []))
  _describe_fields(schema, required, "  ", lines, body)
  return lines


def schema_placeholder(spec: dict[str, Any], *, required_only: bool = False) -> Any:
  """Build an editable value for a JSON Schema fragment."""
  if "default" in spec:
    return spec["default"]
  if "const" in spec:
    return spec["const"]
  enum = spec.get("enum")
  if isinstance(enum, list) and enum:
    return enum[0]
  variants = spec.get("oneOf") or spec.get("anyOf")
  if isinstance(variants, list):
    branch = next(
      (
        item
        for item in variants
        if isinstance(item, dict) and item.get("type") != "null"
      ),
      None,
    )
    if branch is not None:
      return schema_placeholder(branch, required_only=required_only)
  kind = spec.get("type")
  if kind == "array":
    items = spec.get("items")
    count = max(int(spec.get("minItems", 0)), 0)
    if not isinstance(items, dict):
      return []
    return [
      schema_placeholder(items, required_only=required_only) for _ in range(count)
    ]
  if kind == "object":
    props = spec.get("properties", {})
    required = set(spec.get("required", []))
    return {
      name: schema_placeholder(s, required_only=required_only)
      for name, s in props.items()
      if isinstance(s, dict) and (not required_only or name in required)
    } or {}
  if kind == "integer":
    if "minimum" in spec:
      return math.ceil(float(spec["minimum"]))
    if "exclusiveMinimum" in spec:
      return math.floor(float(spec["exclusiveMinimum"])) + 1
    return 0
  if kind == "number":
    if "minimum" in spec:
      return spec["minimum"]
    if "exclusiveMinimum" in spec:
      return float(spec["exclusiveMinimum"]) + 1
    return 0
  if kind == "boolean":
    return False
  return ""


def call_arguments_skeleton(schema: Any) -> dict[str, Any]:
  """Return the minimum editable argument object required by a primitive schema."""
  if not isinstance(schema, dict) or schema.get("type") != "object":
    return {}
  value = schema_placeholder(schema, required_only=True)
  return value if isinstance(value, dict) else {}


def format_call_guide(
  primitive: dict[str, Any] | None,
  primitives: list[dict[str, Any]] | None = None,
  width: int = 72,
) -> list[Styled]:
  """The ``call`` field guide for a primitive and its argument skeleton.

  ``primitive=None`` lists the catalog instead — the entry point when the
  operator typed bare ``call``. Single-style lines: the guide renders in
  the plain overlay box, not the segment-aware pane.
  """
  body = max(width - 4, 16)
  if primitive is None:
    lines: list[Styled] = [("call guide — primitives", "title"), ("", "plain")]
    for item in primitives or []:
      lines.append((f"  call {call_target(item)} key=value...", "accent"))
    lines.append(("", "plain"))
    lines.append(("call <primitive> with no arguments opens this guide", "dim"))
    return lines
  header = f"call guide · {primitive['name']}  v{primitive['version']}"
  if primitive.get("robot_id"):
    header += f"  @{primitive['robot_id']}/{primitive['device_id']}"
  elif primitive.get("backend"):
    header += f"  @{primitive['backend']}"
  lines = [(header, "title"), ("", "plain")]
  for wrapped in wrap_lines(str(primitive["description"]), body):
    lines.append((wrapped, "dim"))
  lines.append(("", "plain"))
  schema = primitive.get("schema")
  if not isinstance(schema, dict) or "properties" not in schema:
    lines.append(("no arguments — call base-style: call name {}", "dim"))
    return lines
  lines.append(("arguments:", "title"))
  required = set(schema.get("required", []))
  for name, spec in schema.get("properties", {}).items():
    if not isinstance(spec, dict):
      continue
    need = "required" if name in required else "optional"
    desc = str(spec.get("description", "")).strip()
    line = f"  {name}: {_describe_type(spec)}  # {need}"
    if desc:
      line += f" · {desc}"
    lines.append((_clip(line, body), "plain"))
  skeleton = {
    name: schema_placeholder(s)
    for name, s in schema.get("properties", {}).items()
    if isinstance(s, dict)
  }
  lines.append(("", "plain"))
  lines.append(("JSON form:", "title"))
  for wrapped in wrap_lines(
    f"call {call_target(primitive)} {json.dumps(skeleton)}", body
  ):
    lines.append((wrapped, "accent"))
  lines.append(("", "plain"))
  lines.append(("Tab after the call target inserts required key=value fields", "dim"))
  lines.append(("Tab also completes remaining field names and enum values", "dim"))
  lines.append(("edit the values, then Enter · any key closes", "dim"))
  return lines


_PLACEHOLDERS = ("no data received",)

# Where an operator can feed an empty section from — shown as a dim hint
# under the empty fallback so "no data" never reads as broken.
_EMPTY_HINTS = {
  "hearing": "→ send a sentence through the /chat WebSocket",
}


def _clip(text: str, width: int) -> str:
  """Ellipsize ``text`` to ``width`` cells."""
  if len(text) <= width:
    return text
  return text[: max(width - 1, 0)] + "…"


def wrap_lines(text: str, width: int) -> list[str]:
  """Word-wrap ``text`` preserving explicit newlines; empty lines survive.

  Hard-breaks tokens longer than ``width`` so the curses blit never clips a
  line mid-string (JSON schemas and primitive catalogs often have no spaces).
  """
  width = max(int(width), 1)
  out: list[str] = []
  for raw in text.splitlines():
    if not raw:
      out.append("")
      continue
    chunks = textwrap.wrap(
      raw,
      width=width,
      replace_whitespace=False,
      break_long_words=True,
      break_on_hyphens=True,
    ) or [""]
    for chunk in chunks:
      if len(chunk) <= width:
        out.append(chunk)
      else:
        for i in range(0, len(chunk), width):
          out.append(chunk[i : i + width])
  return out or [""]


def wrap_indented(text: str, width: int, indent: str = "  ") -> list[str]:
  """Wrap ``text`` so each painted line (indent + body) fits in ``width``."""
  body = max(width - len(indent), 1)
  return [indent + line for line in wrap_lines(text, body)]


def _fmt_uptime(seconds: float) -> str:
  s = int(seconds)
  if s < 60:
    return f"{s}s"
  if s < 3600:
    return f"{s // 60}m{s % 60:02d}s"
  return f"{s // 3600}h{s % 3600 // 60:02d}m"


def format_status_lines(status: dict[str, Any] | None, width: int) -> list[Styled]:
  """Render the process-status pane from the ``/health`` wire payload."""
  if status is None:
    return [
      ("pipeline", "title"),
      ("(disconnected — waiting for health stream)", "dim"),
    ]
  lines: list[Styled] = []
  overall = str(status.get("status", "?"))
  overall_style = {
    "ready": "good",
    "starting": "warn",
    "degraded": "bad",
    "stopping": "warn",
  }.get(overall, "dim")
  uptime = _fmt_uptime(float(status.get("uptime_seconds", 0.0)))
  version = status.get("version", "?")
  lines.append((f"pipeline {overall}  v{version}  up {uptime}", overall_style))
  safety = status.get("safety")
  if isinstance(safety, dict):
    counters = (
      f"gate {int(safety.get('admitted', 0))} admitted"
      f" · {int(safety.get('rejected', 0))} rejected"
    )
    if safety.get("estop_engaged"):
      lines.append(("⚠ estop engaged — all workload egress rejected", "bad"))
      lines.append((counters, "dim"))
    else:
      lines.append((f"{counters} · estop off", "dim"))
  lines.append(("", "plain"))
  lines.append(("subsystem        state      up       restarts", "title"))
  for sub in status.get("subsystems", []):
    name = _clip(str(sub.get("name", "?")), 16).ljust(16)
    state = str(sub.get("state", "?"))
    state_style = {
      "ready": "good",
      "starting": "warn",
      "degraded": "bad",
      "stopping": "warn",
    }.get(state, "dim")
    restarts = int(sub.get("restarts", 0))
    row = f"{name} {state.ljust(10)} {_fmt_uptime(float(sub.get('uptime_seconds', 0.0))).ljust(8)} {restarts}"
    lines.append((row, state_style if restarts == 0 else "bad"))
    error = sub.get("last_error")
    if error:
      lines.append((f"  {_clip(str(error), max(width - 2, 8))}", "bad"))
  return lines


def format_missions_lines(payload: dict[str, Any] | None, width: int) -> list[Line]:
  """Render the mission table as mission → step → primitive layers.

  One row per mission in table order (running first, then pending by rank,
  then finished): marker, origin, state, §3.9 score, and order text. The
  running row expands its plan into ordered barrier steps, each with its
  parallel primitive calls and explicit robot binding.
  """
  if payload is None:
    return [
      ("mission list", "title"),
      ("(disconnected — waiting for missions stream)", "dim"),
    ]
  missions = list(payload.get("missions") or [])
  history = list(payload.get("history") or [])
  if not missions and not history:
    return [
      ("mission list", "title"),
      (
        "(no missions — use `mission add`, or wait for the proposer)",
        "dim",
      ),
    ]
  counts = {
    state: sum(1 for m in missions if m.get("state") == state)
    for state in ("running", "pending", "succeeded", "failed", "cancelled")
  }
  lines: list[Line] = [
    ("mission list", "title"),
    (
      (
        f"{counts['running']} running · {counts['pending']} pending"
        f" · {counts['succeeded']} succeeded · {counts['failed']} failed"
        f" · {counts['cancelled']} cancelled"
      ),
      "dim",
    ),
    ("", "plain"),
    ("  id       st         band    cls  score  u/v       order", "title"),
  ]
  plan = payload.get("plan")
  mission_ids = [str(m.get("mission_id", "?")) for m in missions]
  for m in missions:
    state = str(m.get("state", "?"))
    origin = str(m.get("origin", "?"))
    cls = "op" if origin == "operator" else "self"
    score = float(m.get("priority", 0.0))
    u = float(m.get("urgency", 0.0))
    v = float(m.get("value", 0.0))
    pre = int(m.get("preempted_count", 0))
    mission_id = str(m.get("mission_id", "?"))
    prefix_length = min(8, len(mission_id))
    while prefix_length < len(mission_id) and any(
      other != mission_id and other.startswith(mission_id[:prefix_length])
      for other in mission_ids
    ):
      prefix_length += 1
    short_id = mission_id[:prefix_length]
    band = str(m.get("priority_band", "normal"))
    order = str(m.get("order", "?")).strip()
    marker, style = {
      "running": ("▶", "good"),
      "pending": (" ", "plain"),
      "succeeded": ("✓", "dim"),
      "failed": ("✗", "bad"),
      "cancelled": ("×", "warn"),
    }.get(state, (" ", "dim"))
    head = (
      f"{marker} {short_id:<8} {state:<10} {band:<7} {cls:<4} "
      f"{score:<5.2f} {u:.1f}/{v:.1f}"
    )
    if pre:
      head += f" ×{pre}"
    room = max(width - len(head) - 2, 8)
    lines.append([(head, style), (f"  {_clip(order, room)}", style)])
    if (
      state == "running"
      and plan
      and plan.get("mission_id") in (None, m.get("mission_id"))
    ):
      lines.extend(_render_mission_layers(plan, width))
    error = m.get("error")
    if error:
      lines.append((f"    error: {_clip(str(error), max(width - 8, 8))}", "bad"))
    result = m.get("result")
    if state == "succeeded" and result:
      lines.append((f"    → {_clip(str(result), max(width - 6, 8))}", "dim"))
  history = list(payload.get("history") or [])
  live_plan_mission_id = str(plan.get("mission_id")) if plan else None
  if history:
    lines.extend([("", "plain"), ("history", "title")])
    for event in history:
      state = str(event.get("state", "?"))
      timestamp = str(event.get("timestamp", ""))[11:19] or "?"
      order = str(event.get("order", "?")).strip()
      detail = event.get("detail")
      marker, style = {
        "running": ("▶", "good"),
        "pending": ("·", "plain"),
        "succeeded": ("✓", "dim"),
        "failed": ("✗", "bad"),
        "cancelled": ("×", "warn"),
      }.get(state, ("·", "dim"))
      status = f"{marker} {state}"
      description = f"{order} ({detail})" if detail else order
      room = max(width - len(timestamp) - len(status) - 4, 8)
      lines.append(
        [
          (f"{timestamp} ", "dim"),
          (f"{_clip(description, room)}  ", style),
          (status, style),
        ]
      )
      steps = list(event.get("steps") or [])
      if steps and str(event.get("mission_id")) != live_plan_mission_id:
        lines.extend(
          _render_mission_layers(
            {
              "revision": event.get("revision"),
              "completeness": event.get("completeness"),
              "steps": steps,
            },
            width,
          )
        )
  return lines


def format_mission_detail(mission: dict[str, Any], width: int) -> list[Styled]:
  """Render one stream mission with mutation-relevant identity and revision."""
  mission_id = str(mission.get("mission_id", "?"))
  state = str(mission.get("state", "?"))
  style = {
    "running": "good",
    "pending": "plain",
    "succeeded": "good",
    "failed": "bad",
    "cancelled": "warn",
  }.get(state, "dim")
  document = mission.get("document")
  revision = document.get("revision") if isinstance(document, dict) else None
  lines: list[Styled] = [
    (f"id        {mission_id}", "accent"),
    (f"state     {state}", style),
    (f"priority  {mission.get('priority_band', 'normal')}", "plain"),
    (f"revision  {revision if revision is not None else 'unavailable'}", "plain"),
    ("", "plain"),
  ]
  lines.extend(
    (line, "plain")
    for line in wrap_indented(
      str(mission.get("order", "?")), width, indent="order     "
    )
  )
  for key in ("origin", "submitted_at", "started_at", "finished_at"):
    value = mission.get(key)
    if value is not None:
      lines.append((f"{key:<10}{_clip(str(value), max(width - 10, 8))}", "dim"))
  for key, field_style in (("result", "good"), ("error", "bad")):
    value = mission.get(key)
    if value:
      lines.append((f"{key:<10}{_clip(str(value), max(width - 10, 8))}", field_style))
  if isinstance(document, dict):
    completeness = document.get("completeness")
    if completeness is not None:
      lines.append((f"complete  {float(completeness):.0%}", "dim"))
    plan = document.get("plan")
    if isinstance(plan, dict):
      title = str(plan.get("title") or plan.get("plan_id") or "current plan")
      plan_state = str(plan.get("state") or "pending")
      lines.append(
        (f"plan      {_clip(title, max(width - 16, 8))} ({plan_state})", "dim")
      )
  lines.extend([("", "plain"), ("any key closes", "dim")])
  return lines


def _mission_layer_state(value: Any) -> tuple[str, str, str]:
  """Return marker, canonical label, and style for plan execution state."""
  state = str(value or "pending").lower()
  return {
    "done": ("✓", "done", "good"),
    "succeeded": ("✓", "done", "good"),
    "failed": ("✗", "failed", "bad"),
    "executing": ("▶", "executing", "warn"),
    "running": ("▶", "executing", "warn"),
    "verifying": ("?", "verifying", "warn"),
    "aborted": ("◌", "aborted", "warn"),
    "pending": ("·", "pending", "dim"),
  }.get(state, ("·", state, "dim"))


def _render_mission_layers(plan: dict[str, Any], width: int) -> list[Line]:
  """Expand a running mission into its step barriers and primitive calls."""
  revision = plan.get("revision")
  completeness = plan.get("completeness")
  meta = "  plan"
  if revision is not None:
    meta += f" r{revision}"
  if completeness is not None:
    meta += f" · {float(completeness):.0%} complete"
  mode = str(plan.get("decision_mode") or "plan")
  meta += f" · {mode.upper()}"
  lines: list[Line] = [("", "plain"), (meta, "title")]
  semantic_steps = list(plan.get("plan") or [])
  for semantic in semantic_steps:
    marker, state, style = _mission_layer_state(semantic.get("status"))
    step_id = str(semantic.get("id") or "?")
    sentence = str(semantic.get("sentence") or "")
    attributes: list[str] = []
    if semantic.get("required"):
      attributes.append("required")
    if semantic.get("after"):
      attributes.append("after=" + ",".join(map(str, semantic["after"])))
    if semantic.get("terminal"):
      attributes.append("terminal")
    suffix = f" ({'; '.join(attributes)})" if attributes else ""
    line = f"  {marker} {step_id} · {state}{suffix} — {sentence}"
    lines.append((_clip(line, width), style))
    if semantic.get("evidence"):
      lines.append((_clip(f"      evidence: {semantic['evidence']}", width), "dim"))
    for command in semantic.get("commands") or []:
      lines.append((_clip(f"      {command}", width), "dim"))
  graph = plan.get("decision_graph")
  if isinstance(graph, dict):
    goal = str(graph.get("goal") or "")
    if goal:
      lines.append((f"  goal: {_clip(goal, max(width - 8, 8))}", "accent"))
    nodes = list(graph.get("nodes") or [])
    for node in nodes:
      node_id = str(node.get("id") or "?")
      kind = str(node.get("kind") or "?")
      title = str(node.get("title") or "")
      after = list(node.get("after") or [])
      dependency = f" after {','.join(str(item) for item in after)}" if after else ""
      line = f"  ◇ {node_id} [{kind}]{dependency} — {title}"
      lines.append((_clip(line, width), "plain"))
  steps = list(plan.get("steps") or [])
  if not steps:
    return lines + [("  └─ (execution has not started)", "dim")]
  for step in steps:
    marker, state, style = _mission_layer_state(step.get("state", step.get("outcome")))
    index = step.get("index", "?")
    line = f"  ├─ step {index}  {marker} {state}"
    final = step.get("final")
    reason = step.get("reason")
    detail = final or reason
    if detail:
      line += f" — {_clip(str(detail), max(width - len(line) - 2, 8))}"
    lines.append((line, style))
    primitives = list(step.get("primitives") or step.get("actions") or [])
    if not primitives:
      lines.append(("  │  └─ (no primitive calls)", "dim"))
      continue
    for primitive_index, primitive in enumerate(primitives):
      marker, state, style = _mission_layer_state(
        primitive.get("state", primitive.get("outcome"))
      )
      branch = "└─" if primitive_index == len(primitives) - 1 else "├─"
      name = str(primitive.get("primitive") or primitive.get("action") or "?").strip()
      robot = str(primitive.get("robot_id") or "unassigned")
      index = primitive.get("index", "?")
      reason = primitive.get("reason")
      line = f"  │  {branch} primitive {index}  {marker} {robot} · {name}"
      if reason:
        line += f" — {_clip(str(reason), max(width - len(line) - 2, 8))}"
      lines.append((line, style))
      arguments = primitive.get("arguments")
      if isinstance(arguments, dict):
        rendered = json.dumps(
          arguments, ensure_ascii=False, sort_keys=True, default=str
        )
        indent = "   " if branch == "└─" else "│  "
        lines.append(
          (
            f"  │  {indent}args: {_clip(rendered, max(width - 17, 8))}",
            "dim",
          )
        )
  return lines


def _inline_segments(text: str, base_style: str) -> list[Styled]:
  """Style context labels, ``[modalities]``, and code terms inline."""
  segments: list[Styled] = []
  match = _INLINE_LABEL.match(text)
  if match:
    leading, label, text = match.groups()
    if leading:
      segments.append((leading, base_style))
    segments.append((label, "title"))
  elif text.lstrip().startswith("- "):
    leading = text[: len(text) - len(text.lstrip())]
    if leading:
      segments.append((leading, base_style))
    segments.append(("-", "accent"))
    text = text.lstrip()[1:]
  offset = 0
  for token in _INLINE_TOKEN.finditer(text):
    if token.start() > offset:
      segments.append((text[offset : token.start()], base_style))
    segments.append((token.group(), "accent"))
    offset = token.end()
  if offset < len(text) or not segments:
    segments.append((text[offset:], base_style))
  return segments


def _inline_wrapped(text: str, width: int, base_style: str) -> list[Line]:
  """Wrap one context/prompt line while preserving its inline styles."""
  leading = text[: len(text) - len(text.lstrip())]
  lines = wrap_segments(
    _inline_segments(text.lstrip(), base_style), max(width - 1, 16), indent=leading
  )
  if leading:
    first = lines[0]
    first_segments = first if isinstance(first, list) else [first]
    lines[0] = [(leading, base_style), *first_segments]
  return lines


def _hardware_lines(rendered: str) -> list[str]:
  """Split a backend's semicolon-delimited inventory into one device per row."""
  inventory = rendered.removeprefix("Hardware:").strip()
  backend, separator, devices = inventory.partition(": ")
  if not separator or ";" not in devices:
    return [inventory]
  return [
    f"{backend} · {device.strip()}" for device in devices.split(";") if device.strip()
  ]


def format_context_lines(snapshot: dict[str, Any] | None, width: int) -> list[Line]:
  """Render the prompt pane from the ``/context`` wire payload.

  Sections with real data get a green header; sections rendering the
  configured ``empty`` fallback are marked yellow — that is the uncommon
  state an operator wants to notice.
  """
  body = max(width, 16)
  if snapshot is None:
    return [
      ("context prompt", "title"),
      ("(no snapshot yet — the context subsystem publishes one at readiness)", "dim"),
    ]
  prompt = str(snapshot.get("prompt", ""))
  sections = snapshot.get("sections", [])
  lines: list[Line] = []
  lines.append(
    (
      f"context prompt  v{snapshot.get('version', '?')}  built {str(snapshot.get('built_at', '?'))[11:19]}Z",
      "title",
    )
  )
  lines.append(("", "plain"))
  for section in sections:
    kind = str(section.get("kind", "?"))
    has_data = bool(section.get("has_data"))
    # ◌ marks a budget-dropped section: data existed, the token budget
    # replaced it with the omitted fallback — feeding the section cannot
    # help, so it never carries an empty-source hint.
    omitted = bool(section.get("omitted"))
    style = "good" if has_data else "warn"
    marker = "●" if has_data else "◌" if omitted else "○"
    lines.append((f"{marker} [{kind}]", style))
    rendered = str(section.get("rendered", ""))
    text_style = "plain" if has_data else "dim"
    rendered_lines = (
      _hardware_lines(rendered) if kind == "hardware" else rendered.splitlines()
    )
    for source_line in rendered_lines:
      lines.extend(_inline_wrapped(f"  {source_line}", body, text_style))
    if not has_data and not omitted and kind in _EMPTY_HINTS:
      lines.extend(_inline_wrapped(f"  {_EMPTY_HINTS[kind]}", body, "dim"))
    image = section.get("image_path")
    if image:
      lines.extend(_inline_wrapped(f"  image: {image}", body, "dim"))
    # One blank line between sections: sections are blocks, not a run-on
    # paragraph — the eye needs the seam.
    lines.append(("", "plain"))
  if not sections and prompt:
    for source_line in prompt.splitlines():
      lines.extend(_inline_wrapped(source_line, body, "plain"))
  return lines


def append_model_reply(
  lines: list[Line], exchange: dict[str, Any] | None, width: int
) -> list[Line]:
  """Append the model's latest reply as a context-prompt section.

  The reply is rendered semantically, not as raw JSON: a primitive step becomes
  ``primitive calls: base.velocity({...})``, a conclusion becomes ``final: <text>``,
  a proposer pass lists its proposals. Raw text (prose, parse garbage)
  falls through unchanged.
  """
  body = max(width, 16)
  lines = lines + [("", "plain"), ("◆ model reply (latest)", "good")]
  if exchange is None:
    lines.append(("  (no model reply yet)", "dim"))
    return lines
  header = _model_reply_header(exchange)
  if header:
    lines.append((header, "dim"))
  reply = str(exchange.get("reply", ""))
  rendered = _render_reply_semantic(reply)
  if rendered:
    for block in rendered:
      for wrapped in wrap_indented(block, body):
        lines.append((wrapped, "plain"))
  elif not reply.strip():
    lines.append(("  (no model reply yet)", "dim"))
  else:
    for wrapped in wrap_indented(reply, body):
      lines.append((wrapped, "plain"))
  return lines


def _model_reply_header(exchange: dict[str, Any]) -> str:
  """One dim context line: whose call this was, which model, how long."""
  agent = str(exchange.get("agent_id") or "planner")
  mission = str(exchange.get("mission_id", ""))
  step = exchange.get("step", "?")
  who = f"mission {mission[:8]} step {step}" if mission else "unknown caller"
  model = str(exchange.get("model") or "?")
  duration = float(exchange.get("duration_s", 0.0))
  return f"  {agent}  ·  {who}  ·  {model}  ·  {duration:.1f}s"


def _render_reply_semantic(reply: str) -> list[str]:
  """Render prose plus a semantic summary of the first JSON reply payload."""
  span = _first_json_span(reply)
  if span is None:
    return []
  start, end, payload = span
  if not isinstance(payload, dict):
    return []
  prose = _reply_prose(reply, start, end)
  summary = ""
  final = payload.get("final")
  if isinstance(final, str) and final.strip():
    summary = f"final: {final.strip()}"
  else:
    calls: list[str] = []
    primitives = payload.get("primitives") or payload.get("actions")
    if isinstance(primitives, list) and primitives:
      calls = [_render_call(item) for item in primitives if isinstance(item, dict)]
    else:
      single = _render_call(payload)
      if single:
        calls = [single]
    if calls:
      summary = "primitive calls: " + "; ".join(c for c in calls if c)
    else:
      missions = payload.get("missions")
      if isinstance(missions, list):
        if not missions:
          summary = "proposer: no missions proposed"
        else:
          proposals = []
          for item in missions:
            if isinstance(item, dict) and isinstance(item.get("order"), str):
              urgency = item.get("urgency", "?")
              proposals.append(f'"{item["order"]}" (urgency {urgency})')
          summary = "proposer: " + (
            "; ".join(proposals) if proposals else f"{len(missions)} proposals"
          )
  return [*prose, *([summary] if summary else [])]


def _render_call(payload: dict[str, Any]) -> str:
  """Render one primitive invocation as ``name(args)``."""
  primitive = payload.get("primitive") or payload.get("action")
  if not isinstance(primitive, str) or not primitive.strip():
    return ""
  args = payload.get("arguments") or payload.get("args") or {}
  rendered = json.dumps(args, ensure_ascii=False) if args else ""
  if len(rendered) > 48:
    rendered = rendered[:45] + "..."
  name = primitive.strip()
  return f"{name}({rendered})" if rendered else f"{name}()"


def _first_json_object(text: str) -> object | None:
  """Parse the first balanced ``{...}`` span in ``text`` as JSON."""
  span = _first_json_span(text)
  return span[2] if span is not None else None


def _first_json_span(text: str) -> tuple[int, int, object] | None:
  """Return the first parseable balanced JSON object and its text span."""
  start = text.find("{")
  while start != -1:
    depth = 0
    in_string = False
    escaped = False
    for index in range(start, len(text)):
      char = text[index]
      if in_string:
        if escaped:
          escaped = False
        elif char == "\\":
          escaped = True
        elif char == '"':
          in_string = False
        continue
      if char == '"':
        in_string = True
      elif char == "{":
        depth += 1
      elif char == "}":
        depth -= 1
        if depth == 0:
          try:
            return start, index + 1, json.loads(text[start : index + 1])
          except json.JSONDecodeError:
            break
    start = text.find("{", start + 1)
  return None


def _reply_prose(reply: str, start: int, end: int) -> list[str]:
  """Keep non-JSON reply text while discarding optional JSON fence labels."""
  text = reply[:start] + reply[end:]
  lines = [line.strip() for line in text.splitlines()]
  return [
    line
    for line in lines
    if line and line.casefold() not in {"<json>", "</json>", "```json", "```"}
  ]


def brief_context_lines(snapshot: dict[str, Any] | None) -> Styled:
  """One-line summary of the context snapshot for a collapsed pane."""
  if snapshot is None:
    return ("no snapshot yet", "dim")
  sections = snapshot.get("sections", [])
  with_data = sum(1 for s in sections if s.get("has_data"))
  built = str(snapshot.get("built_at", "?"))[11:19]
  kinds = " ".join(
    ("●" if s.get("has_data") else "◌" if s.get("omitted") else "○")
    + str(s.get("kind", "?"))[:6]
    for s in sections
  )
  return (
    f"v{snapshot.get('version', '?')} {with_data}/{len(sections)} data · {built}Z · {kinds}",
    "plain",
  )


def format_model_io_lines(
  exchange: dict[str, Any] | None,
  width: int,
  *,
  agents: list[dict[str, Any]] | None = None,
) -> list[Styled]:
  """Render the all-agent roster, then the globally latest exchange."""
  body = max(width, 16)
  lines = _format_model_agent_lines(agents or [], body)
  if exchange is None:
    if not lines:
      lines.append(("model stream", "title"))
    lines.append(("(no model requests yet)", "dim"))
    return lines
  status = str(exchange.get("status", "complete"))
  status_style = {
    "streaming": "warn",
    "complete": "good",
    "interrupted": "bad",
    "error": "bad",
  }.get(status, "dim")
  if lines:
    lines.extend([("", "plain"), ("latest exchange", "title")])
  lines.extend(
    [
      (
        (
          f"exchange #{exchange.get('exchange_id', '?')}  "
          f"step {exchange.get('step', '?')}  "
          f"model {exchange.get('model') or '?'}  "
          f"{exchange.get('prompt_tokens', 0)}+"
          f"{exchange.get('completion_tokens', 0)} tok  "
          f"{float(exchange.get('duration_s', 0.0)):.1f}s"
        ),
        "title",
      ),
      (f"● {status}", status_style),
      ("", "plain"),
    ]
  )
  for message in exchange.get("messages", []):
    role = str(message.get("role", "?"))
    lines.append((f"▶ {role}", "accent"))
    content = str(message.get("content", ""))
    for wrapped in wrap_indented(content, body):
      lines.append((wrapped, "plain"))
    lines.append(("", "plain"))
  thinking = str(exchange.get("thinking") or "")
  if thinking:
    lines.append(("◇ provider reasoning", "warn"))
    for wrapped in wrap_indented(thinking, body):
      lines.append((wrapped, "dim"))
    lines.append(("", "plain"))
  lines.append(("◆ answer", "good"))
  reply = str(exchange.get("reply", ""))
  if reply:
    for wrapped in wrap_indented(reply, body):
      lines.append((wrapped, "good"))
  elif status == "streaming":
    lines.append(("  waiting for answer tokens…", "dim"))
  else:
    lines.append(("  (empty answer)", "dim"))
  return lines


def _format_model_agent_lines(agents: list[dict[str, Any]], width: int) -> list[Styled]:
  """Compact coding-agent-style roster with the latest call per lane."""
  if not agents:
    return []
  lines: list[Styled] = [(f"model agents ({len(agents)})", "title")]
  for agent in agents:
    agent_id = str(agent.get("agent_id") or "?")
    status = str(agent.get("status") or "idle")
    style = {
      "thinking": "warn",
      "working": "accent",
      "error": "bad",
      "idle": "dim",
    }.get(status, "dim")
    marker = "●" if agent.get("thinking") else "◆" if status == "working" else "○"
    current = agent.get("exchange")
    details = ""
    if isinstance(current, dict):
      details = (
        f" · exchange #{current.get('exchange_id', '?')}"
        f" · model {current.get('model') or '?'}"
      )
    lines.append((f"{marker} {agent_id} · {status}{details}", style))
    activity = agent.get("activity")
    if not isinstance(activity, dict):
      continue
    arguments = json.dumps(
      activity.get("arguments", {}), ensure_ascii=False, sort_keys=True
    )
    call = (
      f"  ↳ {activity.get('name', '?')} {arguments} · {activity.get('status', '?')}"
    )
    for wrapped in wrap_indented(call, width):
      lines.append((wrapped, style))
    outcome = activity.get("error") or activity.get("result")
    if outcome not in (None, ""):
      for wrapped in wrap_indented(f"    {_clip(str(outcome), 240)}", width):
        lines.append((wrapped, "bad" if activity.get("error") else "dim"))
  return lines


def brief_model_io_line(exchange: dict[str, Any] | None, total: int) -> Styled:
  """One-line summary of model I/O for a collapsed pane or status bar."""
  if exchange is None:
    return (f"reqs {total} · no reply yet", "dim")
  status = str(exchange.get("status", "complete"))
  reply = str(exchange.get("reply", "")).strip().replace("\n", " ")
  if not reply and status == "streaming" and exchange.get("thinking"):
    reply = "reasoning…"
  return (
    (
      f"reqs {total} · {status} · step {exchange.get('step', '?')} · "
      f"{_clip(reply, 60) or '∅'}"
    ),
    "warn" if status == "streaming" else "plain",
  )


# Worst-state ranking for a /backend frame: the summary line carries the
# style of the unhealthiest endpoint so one down backend cannot hide.
_BACKEND_RANK = {"up": 0, "unknown": 1, "degraded": 2, "down": 3}
_BACKEND_RANK_STYLE = {0: "good", 1: "dim", 2: "warn", 3: "bad"}


def backend_states(frame: dict[str, Any] | None) -> list[tuple[str, str]]:
  """``(backend_id, connection)`` pairs carried by one ``/backend`` frame.

  Frames hold one ``BackendStatus`` per wired endpoint under ``backends``;
  a top-level ``connection`` is the legacy single-backend shape.
  """
  if not isinstance(frame, dict):
    return []
  states: list[tuple[str, str]] = []
  for entry in frame.get("backends") or []:
    if isinstance(entry, dict):
      states.append(
        (
          str(entry.get("backend", "default")),
          str(entry.get("connection", "unknown")),
        )
      )
  if not states and isinstance(frame.get("connection"), str):
    states.append(("default", frame["connection"]))
  return states


def backend_profiles(frame: dict[str, Any] | None) -> list[dict[str, str]]:
  """Robot profile rows from one ``/backend`` frame, in frame order.

  Each row carries ``backend`` (endpoint id), ``name`` / ``description``
  (the endpoint config's robot profile, blank when unprofiled),
  ``connection``, and ``endpoint``. The legacy single-backend frame shape
  (top-level ``connection``) renders one anonymous row.
  """
  if not isinstance(frame, dict):
    return []
  rows: list[dict[str, str]] = []
  for entry in frame.get("backends") or []:
    if isinstance(entry, dict):
      rows.append(
        {
          "backend": str(entry.get("backend", "default")),
          "name": str(entry.get("name") or ""),
          "description": str(entry.get("description") or ""),
          "connection": str(entry.get("connection", "unknown")),
          "endpoint": str(entry.get("endpoint") or ""),
        }
      )
  if not rows and isinstance(frame.get("connection"), str):
    rows.append(
      {
        "backend": "default",
        "name": "",
        "description": "",
        "connection": str(frame["connection"]),
        "endpoint": "",
      }
    )
  return rows


def format_backend_hardware_lines(
  frame: dict[str, Any] | None, width: int
) -> list[Styled]:
  """Render EOS hardware and device health, including absent sandbox media."""
  if not frame:
    return []
  lines: list[Styled] = [("backend hardware", "title")]
  for backend in frame.get("backends") or []:
    if not isinstance(backend, dict):
      continue
    backend_id = str(backend.get("backend") or "default")
    identity = " / ".join(
      str(value)
      for value in (
        backend.get("hardware_vendor"),
        backend.get("hardware_model"),
        backend.get("hardware_profile"),
      )
      if value
    )
    lines.append((f"{backend_id} · {identity or 'hardware unknown'}", "plain"))
    devices = {
      str(item.get("device_class")): item
      for item in backend.get("devices", [])
      if isinstance(item, dict)
    }
    classes = (
      ("camera", "mic", "speaker") if backend_id == "sandbox" else tuple(devices)
    )
    for device_class in classes:
      device = devices.get(device_class)
      if device is None:
        lines.append((f"  {device_class}: unavailable", "bad"))
        continue
      usable = bool(device.get("alive")) and bool(device.get("admitted"))
      state = "ready" if usable else "unavailable"
      info = " ".join(
        str(value)
        for value in (device.get("backend_name"), device.get("backend_version"))
        if value
      )
      text = f"  {device_class}: {device.get('device_id', '—')} · {state}"
      if info:
        text += f" · {info}"
      lines.append((_clip(text, width), "good" if usable else "bad"))
  return lines if len(lines) > 1 else []


def backend_summary(frame: dict[str, Any] | None, link: str = "…") -> Styled:
  """One-line southbound link summary for the status bar.

  A single wired backend keeps the classic one-word state; a fleet renders
  one ``id:state`` pair per endpoint so each backend stays visible. No
  frame yet falls back to the feed's connect marker.
  """
  states = backend_states(frame)
  if not states:
    return (f"backend {link}", "dim")
  style = _BACKEND_RANK_STYLE[max(_BACKEND_RANK.get(s, 1) for _, s in states)]
  if len(states) == 1:
    return (f"backend {states[0][1]}", style)
  return ("backends " + " · ".join(f"{bid}:{state}" for bid, state in states), style)


def brief_status_line(
  status: dict[str, Any] | None, backend: dict[str, Any] | None
) -> Styled:
  """One-line summary of pipeline + backend state for a collapsed pane."""
  overall = str(status.get("status", "?")) if status else "disconnected"
  style = {
    "ready": "good",
    "starting": "warn",
    "degraded": "bad",
    "stopping": "warn",
  }.get(overall, "dim")
  conn, conn_style = backend_summary(backend)
  # Two segments, two styles: pipeline and backend are independent states.
  return (
    f"pipeline {overall} · {conn}",
    style if conn_style == style else "plain",
  )


def format_process_lines(samples: list[dict[str, Any]]) -> list[Styled]:
  """Render live process samples (pid, cpu, rss) as a status sub-table."""
  if not samples:
    return []
  lines = [("", "plain"), ("process        cpu%   rss", "title")]
  for sample in samples:
    name = _clip(str(sample.get("name", "?")), 20).ljust(20)
    cpu = f"{float(sample.get('cpu_pct', 0.0)):4.1f}".ljust(5)
    rss = f"{float(sample.get('rss_mb', 0.0)):6.1f}m"
    lines.append((f"{name} {cpu} {rss}", "plain"))
  return lines


def format_primitives_lines(catalog: dict[str, Any] | None, width: int) -> list[Styled]:
  """Render the discovered primitive catalog for the status pane."""
  if catalog is None:
    return [("", "plain"), ("primitives (disconnected)", "dim")]
  primitives = catalog.get("primitives", [])
  count = int(catalog.get("count", len(primitives)))
  lines: list[Styled] = [("", "plain"), (f"primitives ({count})", "title")]
  if not primitives:
    lines.append(("  (no primitives discovered)", "dim"))
    return lines
  for primitive in primitives:
    name = str(primitive.get("name", "?"))
    target = str(primitive.get("robot_id", "?"))
    device = primitive.get("device_id")
    if device:
      target += f"/{device}"
    scope = " [operator]" if primitive.get("operator") else ""
    lines.append((f"  {name}@{target}{scope}", "accent"))
    desc = str(primitive.get("description", "")).strip()
    if desc:
      for wrapped in wrap_indented(desc, max(width, 16), indent="    "):
        lines.append((wrapped, "dim"))
    device_desc = str(primitive.get("device_description", "")).strip()
    if device_desc:
      for wrapped in wrap_indented(
        f"device: {device_desc}", max(width, 16), indent="    "
      ):
        lines.append((wrapped, "dim"))
  return lines


def format_skills_lines(catalog: dict[str, Any] | None, width: int) -> list[Styled]:
  """Render a compact native-skill catalog summary for the status pane."""
  if catalog is None:
    return [("", "plain"), ("skills (disconnected)", "dim")]
  skills = catalog.get("skills", [])
  count = int(catalog.get("count", len(skills)))
  available = bool(catalog.get("available"))
  style = "title" if available else "warn"
  lines: list[Styled] = [("", "plain"), (f"skills ({count})", style)]
  for item in skills:
    name = str(item.get("name", "?"))
    version = str(item.get("version", "?"))
    backend = str(item.get("backend", "?"))
    lines.append((f"  {name}@{version} [{backend}]", "accent"))
    primitives = item.get("primitives") or []
    if primitives:
      detail = "composes: " + ", ".join(str(value) for value in primitives)
      for wrapped in wrap_indented(detail, max(width, 16), indent="    "):
        lines.append((wrapped, "dim"))
  unavailable = catalog.get("unavailable_backends") or []
  if unavailable:
    lines.append(("  unavailable: " + ", ".join(map(str, unavailable)), "warn"))
  if not skills and not unavailable:
    lines.append(("  (no skills reported)", "dim"))
  return lines


def format_skill_pane_lines(
  catalog: dict[str, Any] | None,
  executions: dict[str, dict[str, Any]],
  width: int,
) -> list[Line]:
  """Render executable skill descriptors, schemas, and observed executions."""
  body = max(width, 16)
  if catalog is None:
    return [("skills (disconnected — waiting for catalog)", "dim")]
  skills = catalog.get("skills")
  entries = skills if isinstance(skills, list) else []
  available = bool(catalog.get("available"))
  lines: list[Line] = [
    (
      f"skills ({len(entries)}) · service {'available' if available else 'degraded'}",
      "title" if available else "warn",
    )
  ]
  for item in entries:
    if not isinstance(item, dict):
      continue
    identity = (
      f"{item.get('name', '?')}@{item.get('version', '?')}"
      f"  robot={item.get('backend', '?')}"
    )
    lines.append((identity, "accent"))
    for wrapped in wrap_indented(str(item.get("description", "")), body):
      lines.append((wrapped, "plain"))
    schema = item.get("parameters_schema")
    if isinstance(schema, dict) and isinstance(schema.get("properties"), dict):
      lines.append(("  parameters:", "title"))
      _describe_fields(schema, set(schema.get("required", [])), "    ", lines, body)
    result_schema = item.get("returns_schema")
    if isinstance(result_schema, dict) and result_schema:
      lines.append((f"  result: {_describe_type(result_schema)}", "dim"))
    lines.append(("", "plain"))
  unavailable = catalog.get("unavailable_backends") or []
  if unavailable:
    lines.append(("unavailable: " + ", ".join(map(str, unavailable)), "warn"))
  if not entries and not unavailable:
    lines.append(("(no skills reported)", "dim"))
  if executions:
    lines.extend([("", "plain"), ("executions", "title")])
  for execution in executions.values():
    status = str(execution.get("status", "?"))
    style = {
      "succeeded": "good",
      "queued": "warn",
      "running": "warn",
      "rejected": "bad",
      "cancelled": "warn",
      "timed_out": "bad",
      "failed": "bad",
    }.get(status, "dim")
    lines.append(
      (
        (
          f"  {execution.get('execution_id', '?')} · {execution.get('name', '?')}"
          f"@{execution.get('version', '?')} · {status}"
          f" · seq {execution.get('sequence', '?')}"
        ),
        style,
      )
    )
    lines.append(
      (
        (
          f"    robot={execution.get('robot_id', '?')}"
          f" request={execution.get('request_id', '?')}"
          f" cancel={str(bool(execution.get('cancellation_requested'))).lower()}"
        ),
        "dim",
      )
    )
    reason = execution.get("reason_code") or execution.get("message")
    if reason:
      lines.append((f"    reason: {reason}", "warn" if status != "failed" else "bad"))
    if execution.get("result") is not None:
      for wrapped in wrap_indented(
        "result: " + json.dumps(execution["result"], sort_keys=True),
        body,
        indent="    ",
      ):
        lines.append((wrapped, "plain"))
  return lines


def extract_system_prompt(
  exchange: dict[str, Any] | None = None,
  prompts: dict[str, Any] | None = None,
) -> str | None:
  """Return the raw planner system prompt for the primitives pane.

  Prefers the ``/prompts`` snapshot (available at readiness); falls back to
  the system message on the latest ``/model_io`` exchange.
  """
  if prompts is not None:
    content = str(prompts.get("system", "")).strip()
    if content:
      return content
  if exchange is None:
    return None
  for message in exchange.get("messages", []):
    if str(message.get("role", "")) == "system":
      content = str(message.get("content", "")).strip()
      return content or None
  return None


def brief_primitives_prompt_line(
  exchange: dict[str, Any] | None = None,
  prompts: dict[str, Any] | None = None,
) -> Styled:
  """One-line summary of the planner prompt for a collapsed pane."""
  content = extract_system_prompt(exchange, prompts)
  if not content:
    return ("no planner prompt yet", "dim")
  preview = content.strip().replace("\n", " ")
  return (_clip(preview, 72) or "∅", "plain")


def format_primitives_prompt_lines(
  exchange: dict[str, Any] | None = None,
  width: int = 80,
  prompts: dict[str, Any] | None = None,
) -> list[Line]:
  """Render planner instructions; primitives come from the live catalog."""
  body = max(width, 16)
  lines: list[Line] = [("planner prompt", "title"), ("", "plain")]
  content = extract_system_prompt(exchange, prompts)
  if not content:
    for wrapped in wrap_lines(
      "(no system prompt yet — waiting for /prompts or the first planner step)",
      body,
    ):
      lines.append((wrapped, "dim"))
    return lines
  for source_line in content.splitlines():
    lines.extend(_inline_wrapped(source_line, body, "dim"))
  return lines


def append_primitives_prompt(
  lines: list[Line],
  exchange: dict[str, Any] | None = None,
  width: int = 80,
  prompts: dict[str, Any] | None = None,
) -> list[Line]:
  """Append planner instructions when the pane is too narrow to split."""
  body = max(width, 16)
  lines = lines + [("", "plain"), ("◆ planner prompt", "accent")]
  content = extract_system_prompt(exchange, prompts)
  if not content:
    lines.append(("  (no system prompt yet)", "dim"))
    return lines
  for source_line in content.splitlines():
    lines.extend(_inline_wrapped(f"  {source_line}", body, "dim"))
  return lines


def parse_jsonl_event(line: str) -> dict[str, Any] | None:
  """Parse one ``var/lib/egia/egia.jsonl`` line; ``None`` when not an event."""
  try:
    data = json.loads(line)
  except json.JSONDecodeError:
    return None
  return data if isinstance(data, dict) and "level" in data else None


def event_at_least(event: dict[str, Any], threshold: str) -> bool:
  """True when the event's level meets the pane threshold."""
  return level_value(str(event.get("level", "INFO"))) >= level_value(threshold)


def log_alert_style(entries: Any) -> str | None:
  """Title alert style for the logs pane: ``bad`` on errors, ``warn`` on
  warnings, ``None`` when the buffer is clean."""
  worst: str | None = None
  for entry in entries:
    level = str(entry.event.get("level", "INFO")).upper()
    if level in ("ERROR", "CRITICAL"):
      return "bad"
    if level == "WARNING":
      worst = "warn"
  return worst


def parse_command(text: str) -> tuple[str, str]:
  """Split a command line into ``(name, argument)``.

  The argument is stripped of one layer of matching quotes, so both
  ``chat hello`` and ``chat "hello world"`` yield the same sentence.
  """
  name, _, arg = text.strip().partition(" ")
  arg = arg.strip()
  if len(arg) >= 2 and arg[0] in "\"'" and arg[-1] == arg[0]:
    arg = arg[1:-1]
  return name.lower(), arg


def format_log_event(event: dict[str, Any], width: int, count: int = 1) -> Styled:
  """Render one structured event as a single clipped plain-text line.

  ``count > 1`` appends a ``(xN)`` repeat marker; the event's timestamp is
  expected to be the latest occurrence's.
  """
  level = str(event.get("level", "info")).upper()
  ts = str(event.get("timestamp", "-"))
  clock = ts[11:19] if len(ts) >= 19 else ts
  logger = str(event.get("logger", "-"))
  name = event.get("event", "")
  extras = " ".join(
    f"{k}={v}"
    for k, v in sorted(event.items())
    if k not in {"level", "logger", "timestamp", "event", "correlation_id"}
  )
  marker = f"(x{count}) " if count > 1 else ""
  head = _clip(f"{clock} {level:<7} {logger}: {marker}{name}", max(width, 8))
  room = width - len(head) - 1
  if extras and room > 0:
    line = f"{head} {_clip(extras, room)}"
  else:
    line = head
  return (_clip(line, max(width, 8)), "plain")


def format_log_event_lines(
  event: dict[str, Any], width: int, count: int = 1
) -> list[Line]:
  """Wrap one event, coloring only its severity label.

  The timestamp, logger, message, and extra fields retain the plain style.
  Only ``WARNING``, ``ERROR``, and ``CRITICAL`` receive a color style, so a
  severe event does not paint its whole row.
  """
  level = str(event.get("level", "info")).upper()
  ts = str(event.get("timestamp", "-"))
  clock = ts[11:19] if len(ts) >= 19 else ts
  logger = str(event.get("logger", "-"))
  name = str(event.get("event", ""))
  extras = " ".join(
    f"{key}={value}"
    for key, value in sorted(event.items())
    if key not in {"level", "logger", "timestamp", "event", "correlation_id"}
  )
  level_style = {"WARNING": "warn", "ERROR": "bad", "CRITICAL": "bad"}.get(
    level, "plain"
  )
  marker = f"(x{count}) " if count > 1 else ""
  segments: list[Styled] = [
    (f"{clock} ", "plain"),
    (level, level_style),
    (f" {logger}: {marker}{name}", "plain"),
  ]
  if extras:
    segments.append((f" {extras}", "plain"))
  return wrap_segments(segments, max(width, 8))


def parse_console_line(line: str) -> dict[str, Any] | None:
  """Parse one colorless console line into a synthetic event.

  Matches the stderr format from ``egia.lib.logger`` (ANSI stripped by the
  reader): ``[aether-egia] LEVEL @ name [timestamp]: event k=v``. Returns
  ``None`` for anything else (tracebacks, banners), which the app shows raw.
  """
  # Cheap shape check before splitting: [prefix] LEVEL @ name [ts]: rest
  if not line.startswith("[aether-egia]") or "]:" not in line:
    return None
  try:
    head, event = line.split("]:", 1)
    _, rest = head.split("]", 1)
    level, _, rest = rest.strip().partition(" @ ")
    logger, _, ts = rest.partition(" [")
    return {
      "level": level.lower(),
      "logger": logger.strip(),
      "timestamp": ts.strip("]"),
      "event": event.strip(),
    }
  except ValueError:
    return None
