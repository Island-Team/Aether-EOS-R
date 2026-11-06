"""Curses UI: layout, colors, input, and the render loop.

Layout is focus-driven: the focused pane takes the large area (full width,
~2/3 height) with full detail; the other panes collapse to a separator line
plus one-line brief stacked underneath. ``Alt+1`` through ``Alt+7`` select a
pane directly. The command line has shell-style editing, history, colored
commands, hints, and Tab completion for live console targets.

```┌──────────────────────────────────────────────────────────────┐
│ FOCUSED PANE (full info: sections, exchanges, events, ...)   │
│                                                              │
├ context prompt ──────────────────────────────────────────────┤
│ v1 1/2 data · ●hearing ○perception                            │
├ model stream ────────────────────────────────────────────────┤
│ reqs 3 · step 1 · {"intent": …}                                │
├ logs ────────────────────────────────────────────────────────┤
│ 12 events · 06:41:02 INFO egia.io: connected                   │
│ ↑↓ scroll · model reqs N · backend up                          │
│ > quit                                                         │
└──────────────────────────────────────────────────────────────┘
```

Colors: each pane has its own heading/border color; section markers are green
(with data) or yellow (empty fallback); overall state is green/yellow/red;
log lines are colored by level and timestamps are dim.
"""

from __future__ import annotations

import asyncio
import curses
import json
import shlex
from typing import TYPE_CHECKING, Any, ClassVar

from egia_ext.tui.config import TuiConfig
from egia_ext.tui.feeds import feedback
from egia_ext.tui.render import (
  Line,
  append_model_reply,
  append_primitives_prompt,
  backend_profiles,
  backend_summary,
  call_arguments_skeleton,
  call_target,
  extract_system_prompt,
  format_backend_hardware_lines,
  format_call_guide,
  format_context_lines,
  format_log_event_lines,
  format_mission_detail,
  format_missions_lines,
  format_model_io_lines,
  format_primitive_overview,
  format_primitive_page,
  format_primitives_lines,
  format_primitives_prompt_lines,
  format_process_lines,
  format_skill_pane_lines,
  format_status_lines,
  log_alert_style,
  parse_command,
)

if TYPE_CHECKING:
  from egia_ext.tui.feeds import ConsoleState

_STYLE_PAIRS: dict[str, tuple[int, int]] = {}
_STYLE_IDS = {
  "plain": 1,
  "title": 2,
  "good": 3,
  "warn": 4,
  "bad": 5,
  "accent": 6,
  "dim": 7,
  "border": 8,
  "pane_context": 9,
  "pane_missions": 10,
  "pane_primitives": 11,
  "pane_skills": 12,
  "pane_modelio": 13,
  "pane_logs": 14,
  "pane_status": 15,
}


def _init_colors() -> None:
  """Define the color pairs; falls back to monochrome when unavailable."""
  if not curses.has_colors():
    return
  curses.start_color()
  curses.use_default_colors()
  pairs = {
    _STYLE_IDS["plain"]: curses.COLOR_WHITE,
    _STYLE_IDS["title"]: curses.COLOR_CYAN,
    _STYLE_IDS["good"]: curses.COLOR_GREEN,
    _STYLE_IDS["warn"]: curses.COLOR_YELLOW,
    _STYLE_IDS["bad"]: curses.COLOR_RED,
    _STYLE_IDS["accent"]: curses.COLOR_BLUE,
    _STYLE_IDS["dim"]: curses.COLOR_WHITE,
    _STYLE_IDS["border"]: curses.COLOR_CYAN,
    _STYLE_IDS["pane_context"]: curses.COLOR_BLUE,
    _STYLE_IDS["pane_missions"]: curses.COLOR_MAGENTA,
    _STYLE_IDS["pane_primitives"]: curses.COLOR_GREEN,
    _STYLE_IDS["pane_skills"]: curses.COLOR_CYAN,
    _STYLE_IDS["pane_modelio"]: curses.COLOR_YELLOW,
    _STYLE_IDS["pane_logs"]: curses.COLOR_CYAN,
    _STYLE_IDS["pane_status"]: curses.COLOR_WHITE,
  }
  for pair_id, fg in pairs.items():
    curses.init_pair(pair_id, fg, -1)
  _STYLE_PAIRS.clear()
  for style, pair_id in _STYLE_IDS.items():
    attr = curses.color_pair(pair_id)
    if style == "title" or style.startswith("pane_"):
      attr |= curses.A_BOLD
    elif style == "dim" or style == "border":
      attr |= curses.A_DIM
    _STYLE_PAIRS[style] = (pair_id, attr)


def _attr(style: str) -> int:
  return _STYLE_PAIRS.get(style, (0, curses.A_NORMAL))[1]


def _common_prefix(values: list[str]) -> str:
  """Return the longest shared prefix of non-empty completion candidates."""
  if not values:
    return ""
  prefix = values[0]
  for value in values[1:]:
    while not value.startswith(prefix):
      prefix = prefix[:-1]
  return prefix


def parse_mission_command(argument: str) -> tuple[str, list[str]]:
  """Parse a mission subcommand with shell quoting, unlike top-level commands."""
  words = shlex.split(argument)
  if not words:
    return "", []
  return words[0].lower(), words[1:]


def parse_call_arguments(argument: str) -> dict[str, Any]:
  """Parse a JSON object or shell-style ``key=value`` primitive arguments.

  Values that are valid JSON scalars or containers retain their JSON types;
  other values are strings. Schema validation remains the runtime's job.

  Raises:
      ValueError: If the argument syntax does not describe one object.
  """
  text = argument.strip()
  if text.startswith("{"):
    try:
      value = json.loads(text)
    except json.JSONDecodeError as exc:
      raise ValueError("argument is not valid JSON") from exc
    if not isinstance(value, dict):
      raise ValueError("arguments must be a JSON object")
    return value
  try:
    value = json.loads(text)
  except json.JSONDecodeError:
    pass
  else:
    if not isinstance(value, dict):
      raise ValueError("arguments must be a JSON object")  # noqa: TRY004
    return value

  try:
    words = shlex.split(text)
  except ValueError as exc:
    raise ValueError(f"invalid quoting: {exc}") from exc
  arguments: dict[str, Any] = {}
  for word in words:
    key, separator, raw_value = word.partition("=")
    if not separator or not key:
      raise ValueError(f"expected key=value, got {word!r}")
    if key in arguments:
      raise ValueError(f"duplicate argument: {key}")
    try:
      arguments[key] = json.loads(raw_value)
    except json.JSONDecodeError:
      arguments[key] = raw_value
  return arguments


def _format_call_value(value: Any) -> str:
  """Format one placeholder as a shell-safe ``key=value`` value."""
  if isinstance(value, str):
    return shlex.quote(value)
  return shlex.quote(json.dumps(value, ensure_ascii=False, separators=(",", ":")))


def _format_call_arguments(arguments: dict[str, Any]) -> str:
  """Format primitive arguments as editable shell-style assignments."""
  return " ".join(
    f"{name}={_format_call_value(value)}" for name, value in arguments.items()
  )


def _rebind_terminal(stdscr) -> tuple[int, int]:
  """Rebind curses to the current tty size — shared by all refresh methods."""
  curses.update_lines_cols()
  size = stdscr.getmaxyx()
  curses.resizeterm(*size)
  return size


def _refresh_on_resize(stdscr) -> tuple[int, int]:
  """Refresh 1/3 — terminal size changed.

  The old buffer no longer matches the new geometry, so ``erase`` blanks the
  virtual window and the next ``refresh`` repaints every cell from scratch;
  stale words beyond the new frame cannot survive.
  """
  size = _rebind_terminal(stdscr)
  stdscr.erase()
  return size


def _refresh_on_init(stdscr) -> tuple[int, int]:
  """Refresh 2/3 — one-shot init/ready settle.

  Same erase path as a size change, fired exactly once when the pipeline
  first reports ready (see ``consume_ready_refresh``): the settled frame is
  rebuilt from scratch. Because it fires once and the draw follows in the
  same loop iteration, no blank intermediate frame is ever shown.
  """
  size = _rebind_terminal(stdscr)
  stdscr.erase()
  return size


def _refresh_common(stdscr) -> tuple[int, int]:
  """Refresh 3/3 — common refresh, no erase.

  ``touchwin`` marks the old buffer all-changed and the next repaint
  overwrites it cell-for-cell with no blank frame — the default for any
  forced refresh. (``clear`` is never used anywhere: its clear-screen
  escape blanks the physical terminal, which reads as blinking.)
  """
  size = _rebind_terminal(stdscr)
  stdscr.touchwin()
  return size


def _sync_terminal_size(
  stdscr, last: tuple[int, int] | None, *, force: bool = False
) -> tuple[int, int]:
  """Return the current terminal size, picking the right refresh method.

  Size change → ``_refresh_on_resize``; pipeline-ready settle →
  ``_refresh_on_init`` (once per ready transition); otherwise no refresh,
  just the current size.
  """
  curses.update_lines_cols()
  size = stdscr.getmaxyx()
  if last is not None and size != last:
    return _refresh_on_resize(stdscr)
  if force:
    return _refresh_on_init(stdscr)
  return size


class ConsoleApp:
  """One full-screen console over a :class:`ConsoleState`."""

  _PANES = (
    "context",
    "missions",
    "primitives",
    "skills",
    "modelio",
    "logs",
    "status",
  )
  _PANE_SHORTCUTS: ClassVar[dict[int, str]] = dict(enumerate(_PANES, start=1))
  _COMMANDS = (
    "backends",
    "call",
    "chat",
    "exit",
    "focus",
    "help",
    "mission",
    "quit",
    "repl",
    "skill",
  )
  _MISSION_SUBCOMMANDS = ("add", "list", "show", "edit", "priority", "cancel")
  _PRIORITY_BANDS = ("low", "normal", "high", "urgent")
  _SKILL_SUBCOMMANDS = ("start", "show", "watch", "cancel")
  _COMMAND_HINTS: ClassVar[dict[str, str]] = {
    "backends": "show the live backend roster",
    "call": "call <primitive>@<robot>/<device> key=value... | {json}",
    "chat": 'chat "sentence"',
    "focus": "focus <pane>",
    "help": "show commands and keys",
    "mission": "mission add|list|show|edit|priority|cancel",
    "repl": "repl [backend]",
    "skill": "skill start|show|watch|cancel",
  }
  _MAX_HISTORY = 1000
  _PANE_TITLES: ClassVar[dict[str, str]] = {
    "context": "context prompt",
    "missions": "mission list",
    "primitives": "primitives",
    "skills": "skills",
    "modelio": "model stream",
    "logs": "logs",
    "status": "egia status",
  }
  _FOCUS_ALIASES: ClassVar[dict[str, str]] = {
    "context": "context",
    "prompt": "context",
    "missions": "missions",
    "mission": "missions",
    "missiontable": "missions",
    "mission table": "missions",
    "queue": "missions",
    "primitives": "primitives",
    "primitive": "primitives",
    "primitivecatalog": "primitives",
    "primitive catalog": "primitives",
    "skill": "skills",
    "skillcatalog": "skills",
    "skill catalog": "skills",
    "modelio": "modelio",
    "model": "modelio",
    "stream": "modelio",
    "io": "modelio",
    "logs": "logs",
    "log": "logs",
    "status": "status",
    "health": "status",
  }
  _BRIEF_ROWS = 1  # unfocused panes: one rule line with the title only
  # Split once there is room for a half-width primitive column.
  _CONTEXT_SPLIT_MIN = 80

  def __init__(self, state: ConsoleState, config: TuiConfig) -> None:
    self._state = state
    self._config = config
    self._focus = "context"
    self._scroll: dict[str, int] = {
      "context": 0,
      "context_primitives": 0,
      "missions": 0,
      "primitives": 0,
      "skills": 0,
      "modelio": 0,
      "logs": 0,
      "status": 0,
    }
    self._input_buf = ""
    self._input_cursor = 0
    self._input_history: list[str] = []
    self._history_index: int | None = None
    self._history_query: str | None = None
    self._completion_matches: list[str] = []
    self._primitives_page = 0
    self._primitives_visible = 0
    self._help_open = False
    self._backends_open = False  # fleet roster overlay; opened by `backends`
    self._guide_lines: list[tuple[str, str]] | None = None  # call JSON guide
    self._mission_detail: dict[str, Any] | None = None
    self._saw_ready = False
    self._focus_changed = False  # focus moved → the frame needs an erase repaint
    self.on_chat: Any = None  # async callback(sentence), wired by __main__
    # async callback(primitive, arguments, robot_id, device_id), wired by __main__
    self.on_call: Any = None
    self.on_mission: Any = None  # async callback(raw payload), wired by __main__
    self.on_skill: Any = None  # async callback(raw payload), wired by __main__
    self.on_skill_watch: Any = None  # async callback(robot, execution), wired by main
    self.on_repl: Any = None  # async callback(alias, endpoint), wired by __main__
    self._repl_request: tuple[str, str] | None = None
    self._quit = asyncio.Event()

  @property
  def quit_event(self) -> asyncio.Event:
    """Set by input handling; the main loop exits on it."""
    return self._quit

  def take_repl_request(self) -> tuple[str, str] | None:
    """Consume the pending ``(backend alias, endpoint)`` terminal handoff."""
    request = self._repl_request
    self._repl_request = None
    return request

  def consume_ready_refresh(self) -> bool:
    """True once when ``/health`` first reports ``ready`` (or again after a drop).

    Callers should refresh via ``_refresh_on_init`` (the one-shot erase
    path): IDE/web terminals often finish resizing right as the pipeline
    becomes ready, leaving a torn frame.
    """
    ready = (
      self._state.status is not None
      and str(self._state.status.get("status", "")) == "ready"
    )
    if ready and not self._saw_ready:
      self._saw_ready = True
      return True
    if not ready:
      self._saw_ready = False
    return False

  def consume_focus_refresh(self) -> bool:
    """True once when the focused pane changed (shortcut or ``focus``).

    Different panes paint different layouts over the same cells; without an
    erase the previous pane's text survives wherever the new frame writes
    shorter lines. Callers should refresh via ``_refresh_on_resize``.
    """
    changed = self._focus_changed
    self._focus_changed = False
    return changed

  # --- full-pane renderers -------------------------------------------------

  def _context_split_width(self, width: int) -> tuple[int, int] | None:
    """Return ``(main_inner_w, side_inner_w)`` when the pane should split.

    The primitives column uses half the width so schemas stay readable;
    use ``focus primitives`` for the full terminal width.
    """
    if width < self._CONTEXT_SPLIT_MIN:
      return None
    side_w = max(width // 2, 36)
    if width - side_w < 36:
      return None
    return width - side_w - 5, side_w - 2

  def _context_main_lines(self, width: int, *, split: bool) -> list[Line]:
    lines: list[Line] = list(format_context_lines(self._state.snapshot, width))
    lines = append_model_reply(lines, self._state.model_io, width)
    if not split:
      lines = append_primitives_prompt(
        lines, self._state.model_io, width, prompts=self._state.prompts
      )
    return lines

  def _context_lines(self, width: int) -> list[Line]:
    split = self._context_split_width(width) is not None
    main_w = self._context_split_width(width)
    inner_w = main_w[0] if main_w else width
    return self._context_main_lines(inner_w, split=split)

  def _context_primitives_lines(self, width: int) -> list[Line]:
    return format_primitives_prompt_lines(
      self._state.model_io, width, prompts=self._state.prompts
    )

  def _primitives_lines(self, width: int) -> list[Line]:
    """Paginated primitive browser: overview plus one page per primitive.

    A long descriptor is split into continuation pages.
    """
    body = max(width, 16)
    instruction, primitives, pages = self._primitives_layout(body)
    if not primitives:
      return format_primitives_prompt_lines(
        self._state.model_io, width, prompts=self._state.prompts
      )
    page = min(self._primitives_page, len(pages) - 1)
    kind, primitive_index, sub, subs = pages[page]
    if kind == "overview":
      return format_primitive_overview(instruction, primitives, body)
    full = format_primitive_page(
      primitives[primitive_index],
      body,
      index=primitive_index + 1,
      count=len(primitives),
      sub=sub,
      subs=subs,
    )
    if subs > 1:
      start = (sub - 1) * self._primitives_subpage_rows()
      return full[start : start + self._primitives_subpage_rows()]
    return full

  def _primitives_subpage_rows(self) -> int:
    """Rows per continuation page — the pane's visible height when known."""
    return max(self._primitives_visible, 8)

  def _primitives_layout(
    self, width: int
  ) -> tuple[str, list[dict[str, Any]], list[tuple[str, int, int, int]]]:
    """Flat page list: overview, then primitive continuation pages."""
    instruction, primitives = self._parsed_primitives()
    if not primitives:
      return instruction, primitives, []
    rows = self._primitives_subpage_rows()
    pages: list[tuple[str, int, int, int]] = [("overview", 0, 1, 1)]
    for i, primitive in enumerate(primitives):
      full = format_primitive_page(primitive, width, index=i + 1, count=len(primitives))
      subs = max((len(full) + rows - 1) // rows, 1)
      for sub in range(1, subs + 1):
        pages.append(("primitive", i, sub, subs))
    return instruction, primitives, pages

  def _parsed_primitives(self) -> tuple[str, list[dict[str, Any]]]:
    """Return planner instructions plus the live primitive catalog."""
    content = extract_system_prompt(self._state.model_io, self._state.prompts)
    instruction = content or ""
    snapshot = self._state.primitives or {}
    entries = snapshot.get("primitives")
    if not isinstance(entries, list):
      return instruction, []
    primitives = []
    for entry in entries:
      if not isinstance(entry, dict):
        continue
      primitives.append(
        {
          "name": str(entry.get("name", "")),
          "version": str(entry.get("version", "?")),
          "robot_id": entry.get("robot_id"),
          "device_id": entry.get("device_id"),
          "description": str(entry.get("description", "")),
          "device_description": str(entry.get("device_description", "")),
          "operator": bool(entry.get("operator", False)),
          "limits": entry.get("limits"),
          "schema": entry.get("arguments_schema"),
        }
      )
    return instruction, primitives

  def _primitives_page_count(self, width: int = 80) -> int:
    """Overview plus all primitive continuation pages."""
    return len(self._primitives_layout(max(width, 16))[2])

  def _missions_lines(self, width: int) -> list[Line]:
    lines: list[Line] = list(format_missions_lines(self._state.missions, width))
    return lines

  def _modelio_lines(self, width: int) -> list[Line]:
    lines: list[Line] = list(
      format_model_io_lines(
        self._state.model_io_latest,
        width,
        agents=self._state.model_agents,
      )
    )
    return lines

  def _skills_lines(self, width: int) -> list[Line]:
    """Render the live skill catalog and all executions seen this session."""
    return format_skill_pane_lines(
      self._state.skills, self._state.skill_executions, width
    )

  def _log_lines(self, width: int) -> list[Line]:
    lines: list[Line] = []
    for entry in self._state.logs:
      lines.extend(format_log_event_lines(entry.event, width, entry.count))
    return lines

  def _status_lines(self, width: int) -> list[Line]:
    lines: list[Line] = list(format_status_lines(self._state.status, width))
    lines.extend(format_backend_hardware_lines(self._state.backend, width))
    lines.extend(format_primitives_lines(self._state.primitives, width))
    lines.extend(format_process_lines(self._state.processes))
    return lines

  # --- brief renderers -----------------------------------------------------
  # Unfocused panes show only their title rule; the brief one-liners the
  # collapsed rows used to paint live on in render.py for the status bar
  # and tests.

  def _focused_line_count(self, width: int) -> int:
    inner_w = max(width - 4, 16)
    lines = {
      "context": self._context_lines,
      "missions": self._missions_lines,
      "primitives": self._primitives_lines,
      "skills": self._skills_lines,
      "modelio": self._modelio_lines,
      "logs": self._log_lines,
      "status": self._status_lines,
    }[self._focus](inner_w)
    return len(lines)

  # --- layout ---------------------------------------------------------------

  def draw(self, stdscr, *, terminal_size: tuple[int, int]) -> None:
    """Repaint the whole frame from the current state.

    No ``erase()`` up front: the layout paints every cell (pane blanks,
    rules, status bar, input line), and a full-screen clear before the
    repaint is exactly the flash terminals show as flicker.
    """
    height, width = terminal_size
    if height < 12 or width < 40:
      stdscr.addnstr(0, 0, "terminal too small", max(width - 1, 0), _attr("warn"))
      stdscr.refresh()
      return
    body_h = height - 2  # status bar + command line
    others = [p for p in self._PANES if p != self._focus]
    large_h = max(body_h - self._BRIEF_ROWS * len(others), 3)

    y = 0
    self._draw_large_pane(stdscr, y, large_h, width)
    y += large_h
    for pane in others:
      self._draw_hrule(
        stdscr, y, width, self._PANE_TITLES[pane], self._pane_title_style(pane)
      )
      y += self._BRIEF_ROWS
    self._draw_status_bar(stdscr, height - 2, width)
    self._draw_input_line(stdscr, height - 1, width)
    if self._help_open:
      self._draw_help(stdscr, height, width)
    elif self._mission_detail is not None:
      self._draw_mission_detail(stdscr, height, width)
    elif self._guide_lines is not None:
      self._draw_guide(stdscr, height, width)
    elif self._backends_open:
      self._draw_backends(stdscr, height, width)
    stdscr.refresh()

  def _pane_title_style(self, pane: str) -> str:
    """Title style for a pane; the logs pane alerts on WARNING/ERROR."""
    if pane == "logs":
      return log_alert_style(self._state.logs) or "pane_logs"
    return f"pane_{pane}"

  def _draw_large_pane(self, stdscr, y: int, h: int, width: int) -> None:
    if h < 3 or width < 4:
      return
    pane = self._focus
    title_style = self._pane_title_style(pane)
    if pane == "context" and self._context_split_width(width) is not None:
      self._draw_context_split_pane(stdscr, y, h, width, title_style)
      return
    self._draw_box(stdscr, y, 0, h, width, self._PANE_TITLES[pane], True, title_style)
    inner_w = width - 4
    if pane == "primitives":
      # Pagination depends on how many rows the pane actually shows.
      self._primitives_visible = max(h - 2, 8)
    lines = {
      "context": self._context_lines,
      "missions": self._missions_lines,
      "primitives": self._primitives_lines,
      "skills": self._skills_lines,
      "modelio": self._modelio_lines,
      "logs": self._log_lines,
      "status": self._status_lines,
    }[pane](inner_w)
    visible = h - 2
    # All panes are bottom-anchored: scroll 0 shows the newest content and
    # ↑ walks toward older lines. (A former logs-only remap here inverted
    # the arrows in that pane.)
    scroll = self._scroll[pane]
    self._blit(stdscr, y + 1, 1, visible, width - 2, lines, scroll)

  def _draw_context_split_pane(
    self, stdscr, y: int, h: int, width: int, title_style: str
  ) -> None:
    """Context pane with a side column for planner primitive guidance."""
    if h < 3 or width < 4:
      return
    split = self._context_split_width(width)
    assert split is not None
    main_inner_w, side_inner_w = split
    side_panel_w = side_inner_w + 2
    div_x = width - side_panel_w - 1
    self._draw_box(
      stdscr, y, 0, h, width, self._PANE_TITLES["context"], True, title_style
    )
    if 0 < div_x < width - 1:
      for row in range(y + 1, y + h - 1):
        try:
          stdscr.addch(row, div_x, curses.ACS_VLINE, _attr("border"))
        except curses.error:
          break
      try:
        stdscr.addnstr(
          y, div_x + 2, " primitives ", max(side_panel_w - 4, 0), _attr("accent")
        )
      except curses.error:
        pass
    visible = h - 2
    main_lines = self._context_main_lines(main_inner_w, split=True)
    side_lines = self._context_primitives_lines(side_inner_w)
    main_scroll = self._scroll["context"]
    side_scroll = self._scroll["context_primitives"]
    self._blit(stdscr, y + 1, 1, visible, main_inner_w, main_lines, main_scroll)
    self._blit(stdscr, y + 1, div_x + 1, visible, side_inner_w, side_lines, side_scroll)

  def _draw_hrule(
    self, stdscr, y: int, width: int, title: str, title_style: str
  ) -> None:
    """Draw ``├ title ───…───┤`` across the terminal width."""
    label = f" {title} "
    stdscr.addnstr(y, 0, " " * max(width - 1, 0), max(width - 1, 0), _attr("border"))
    if width < 4:
      return
    stdscr.addch(y, 0, curses.ACS_LTEE, _attr("border"))
    label_w = min(len(label), max(width - 2, 0))
    stdscr.addnstr(y, 1, label, label_w, _attr(title_style))
    for col in range(1 + label_w, width - 1):
      stdscr.addch(y, col, curses.ACS_HLINE, _attr("border"))
    stdscr.addch(y, width - 1, curses.ACS_RTEE, _attr("border"))

  def _draw_input_line(self, stdscr, y: int, width: int) -> None:
    """Draw the shell-style command line with syntax color and a live hint."""
    stdscr.addnstr(y, 0, " " * max(width - 1, 0), max(width - 1, 0), _attr("plain"))
    stdscr.addnstr(y, 0, "> ", 2, _attr("accent"))
    command, _, remainder = self._input_buf.partition(" ")
    command_style = "title" if command in self._COMMANDS else "bad"
    if not command:
      command_style = "plain"
    text_room = max(width - 3, 0)
    stdscr.addnstr(y, 2, command, text_room, _attr(command_style))
    if remainder:
      stdscr.addnstr(
        y, 2 + len(command), f" {remainder}", text_room - len(command), _attr("plain")
      )
    hint = self._input_hint()
    hint_x = min(2 + len(self._input_buf), max(width - 1, 0))
    if hint and hint_x < width - 1:
      stdscr.addnstr(y, hint_x, f"  {hint}", width - hint_x - 1, _attr("dim"))
    stdscr.move(y, min(2 + self._input_cursor, max(width - 2, 0)))

  _HELP_LINES = (
    ("commands", "title"),
    ("  quit / exit        stop the console (and the child pipeline)", "plain"),
    ("  help               this panel", "plain"),
    ("  backends           live fleet roster", "plain"),
    ("  repl [backend]     open eos-repl; quit/exit returns here", "plain"),
    ("  skill start <name>@<version> robot=<alias> timeout=<s> key=value...", "plain"),
    ("  skill show|watch|cancel <execution-id> robot=<alias>", "plain"),
    ('  chat "sentence"    enqueue one heard sentence via /chat', "plain"),
    ("  call <primitive>@<robot>/<device> key=value...", "plain"),
    ("                      or pass one {json object}", "plain"),
    ("                      exact target via /primitive/invoke", "plain"),
    ("                      bare `call <primitive>` shows its field guide", "plain"),
    ('  mission add "order"                  create a mission', "plain"),
    ("  mission list                         focus mission pane", "plain"),
    ("  mission show <id-prefix>             show current details", "plain"),
    ('  mission edit <id-prefix> "order"     replace pending order', "plain"),
    ("  mission priority <id-prefix> <band>  low|normal|high|urgent", "plain"),
    ("  mission cancel <id-prefix>           cancel pending mission", "plain"),
    (
      "  focus <pane>       context, missions, primitives, skills, model, logs, status",
      "plain",
    ),
    ("window", "title"),
    (
      "  Alt+1..Alt+7      focus context, missions, primitives, skills, model, logs, status",
      "plain",
    ),
    ("  PgUp/PgDn or F7/F8 scroll focused pane by page", "plain"),
    ("  empty prompt: ↑↓ Home End ←→", "plain"),
    ("                     line/top/bottom; primitive page or context side", "plain"),
    ("command", "title"),
    ("  type / Tab         edit command / complete commands and live targets", "plain"),
    ("  ←→ Home End        move cursor · Ctrl-A/E and Ctrl-B/F also work", "plain"),
    ("  ↑↓ after typing    history · Ctrl-P/N history from an empty prompt", "plain"),
    ("  Ctrl-R             reverse history search · Ctrl-U/W clear line/word", "plain"),
    ("", "plain"),
    ("  type on the bottom row · Enter run · any key closes help", "dim"),
  )

  def _draw_help(self, stdscr, height: int, width: int) -> None:
    """Overlay the command reference; opened by the ``help`` command."""
    rows = list(self._HELP_LINES)
    max_rows = max(height - 2, 1)
    if len(rows) > max_rows:
      rows = [*rows[: max_rows - 1], ("  terminal too short for complete help", "dim")]
    menu_h = len(rows) + 2
    menu_w = max(len(text) for text, _ in rows) + 4
    y = max((height - menu_h) // 2, 0)
    x = max((width - menu_w) // 2, 0)
    self._draw_box(stdscr, y, x, menu_h, menu_w, "help", True)
    for i, (text, style) in enumerate(rows):
      stdscr.addnstr(y + 1 + i, x + 2, text, menu_w - 4, _attr(style))

  def _draw_backends(self, stdscr, height: int, width: int) -> None:
    """Overlay the fleet roster: one robot per row — id, state, profile.

    Opened by the ``backends`` command; reads the latest ``/backend``
    frame, so the roster shows the endpoint config's name/intro (or the
    blank defaults) with the live connection state.
    """
    rows = backend_profiles(self._state.backend)
    if not rows:
      rows = [
        {
          "backend": "default",
          "name": "",
          "connection": "unknown",
          "description": "no /backend frame yet — the stream has not answered",
          "endpoint": "",
        }
      ]
    lines: list[tuple[str, str]] = [("robot            state     profile", "title")]
    entries = {
      str(item.get("backend", "default")): item
      for item in (self._state.backend or {}).get("backends", [])
      if isinstance(item, dict)
    }
    for row in rows:
      backend = row["backend"][:16].ljust(17)
      conn = row["connection"][:8].ljust(10)
      lines.append((f"{backend}{conn}{row['name'] or '—'}", "plain"))
      if row["description"]:
        indent = " " * 27
        lines.append((f"{indent}{row['description'][:60]}", "dim"))
      entry = entries.get(row["backend"], {})
      hardware = " / ".join(
        str(value)
        for value in (
          entry.get("hardware_vendor"),
          entry.get("hardware_model"),
          entry.get("hardware_profile"),
        )
        if value
      )
      if hardware:
        lines.append((f"    hardware  {hardware}", "dim"))
      devices = {
        str(item.get("device_class")): item
        for item in entry.get("devices", [])
        if isinstance(item, dict)
      }
      classes = (
        ("camera", "mic", "speaker") if row["backend"] == "sandbox" else tuple(devices)
      )
      for device_class in classes:
        device = devices.get(device_class)
        if device is None:
          lines.append((f"    {device_class:<8} unavailable", "bad"))
          continue
        usable = bool(device.get("alive")) and bool(device.get("admitted"))
        state = "ready" if usable else "unavailable"
        backend_info = " ".join(
          value
          for value in (
            str(device.get("backend_name") or ""),
            str(device.get("backend_version") or ""),
          )
          if value
        )
        detail = f"{device.get('device_id', '—')} · {state}"
        if backend_info:
          detail += f" · {backend_info}"
        lines.append((f"    {device_class:<8} {detail}", "good" if usable else "bad"))
    lines.append(("", "plain"))
    lines.append(("  any key closes", "dim"))
    menu_h = len(lines) + 2
    menu_w = max(max(len(text) for text, _ in lines) + 4, 36)
    y = max((height - menu_h) // 2, 0)
    x = max((width - menu_w) // 2, 0)
    self._draw_box(stdscr, y, x, menu_h, menu_w, "backends", True)
    for i, (text, style) in enumerate(lines):
      stdscr.addnstr(y + 1 + i, x + 2, text, menu_w - 4, _attr(style))

  # --- input ----------------------------------------------------------------

  def _input_hint(self) -> str:
    """Return the contextual gray hint shown after the command cursor."""
    if self._completion_matches:
      shown = ", ".join(self._completion_matches[:4])
      suffix = " ..." if len(self._completion_matches) > 4 else ""
      return f"Tab: {shown}{suffix}"
    before_cursor = self._input_buf[: self._input_cursor]
    words = before_cursor.split()
    if not words:
      return "Alt+1..7 panes · Tab completes · help"
    command = words[0].lower()
    if command not in self._COMMANDS:
      return "unknown command"
    if len(words) == 1 and before_cursor == command:
      return self._COMMAND_HINTS.get(command, "")
    if self._call_autofill_primitive() is not None:
      return "Tab: insert required key=value parameters"
    candidates = self._completion_candidates()
    if candidates:
      shown = ", ".join(candidates[:4])
      suffix = " ..." if len(candidates) > 4 else ""
      return f"Tab: {shown}{suffix}"
    return self._COMMAND_HINTS.get(command, "")

  def _completion_candidates(self) -> list[str]:
    """Return live completion candidates for the token under the cursor."""
    cursor = self._input_cursor
    start = cursor
    while start > 0 and not self._input_buf[start - 1].isspace():
      start -= 1
    prefix = self._input_buf[start:cursor]
    try:
      earlier = shlex.split(self._input_buf[:start])
    except ValueError:
      earlier = self._input_buf[:start].split()
    if not earlier:
      candidates = list(self._COMMANDS)
    else:
      command = earlier[0].lower()
      if command in {"focus", "help"} and len(earlier) == 1:
        candidates = list(self._PANES if command == "focus" else self._COMMANDS)
      elif command == "repl" and len(earlier) == 1:
        candidates = [row["backend"] for row in backend_profiles(self._state.backend)]
      elif command == "call" and len(earlier) == 1:
        _, primitives = self._parsed_primitives()
        candidates = sorted(
          {
            f"{item['name']}@{item['robot_id']}"
            + (f"/{item['device_id']}" if item.get("device_id") else "")
            for item in primitives
            if item.get("name") and item.get("robot_id")
          }
        )
      elif command == "call" and len(earlier) >= 2:
        primitive = self._call_primitive(earlier[1])
        schema = primitive.get("schema") if primitive is not None else None
        properties = schema.get("properties") if isinstance(schema, dict) else None
        if not isinstance(properties, dict):
          candidates = []
        elif "=" in prefix:
          name = prefix.partition("=")[0]
          spec = properties.get(name)
          candidates = self._call_value_candidates(name, spec)
        else:
          used = {word.partition("=")[0] for word in earlier[2:] if "=" in word}
          required = (
            set(schema.get("required", [])) if isinstance(schema, dict) else set()
          )
          names = [name for name in properties if name not in used]
          names.sort(key=lambda name: (name not in required, name))
          candidates = [f"{name}=" for name in names]
      elif command == "skill":
        candidates = self._skill_completion_candidates(earlier, prefix)
      elif command == "mission":
        if len(earlier) == 1:
          candidates = list(self._MISSION_SUBCOMMANDS)
        elif len(earlier) == 2 and earlier[1] in {
          "show",
          "edit",
          "priority",
          "cancel",
        }:
          candidates = self._mission_completion_ids()
        elif len(earlier) == 3 and earlier[1] == "priority":
          candidates = list(self._PRIORITY_BANDS)
        else:
          candidates = []
      else:
        candidates = []
    return [candidate for candidate in candidates if candidate.startswith(prefix)]

  def _skill_completion_candidates(self, earlier: list[str], prefix: str) -> list[str]:
    """Return schema-aware candidates for the skill command at the cursor."""
    if len(earlier) == 1:
      return list(self._SKILL_SUBCOMMANDS)
    operation = earlier[1]
    if operation == "start" and len(earlier) == 2:
      return sorted(
        {
          f"{item.get('name')}@{item.get('version')}"
          for item in (self._state.skills or {}).get("skills", [])
          if isinstance(item, dict) and item.get("name") and item.get("version")
        }
      )
    if operation == "start" and len(earlier) >= 3:
      descriptor = self._skill_descriptor(earlier[2])
      used = {word.partition("=")[0] for word in earlier[3:] if "=" in word}
      schema = descriptor.get("parameters_schema") if descriptor else None
      raw_properties = schema.get("properties") if isinstance(schema, dict) else None
      properties: dict[str, Any] = (
        raw_properties if isinstance(raw_properties, dict) else {}
      )
      if "=" in prefix and descriptor is not None:
        name = prefix.partition("=")[0]
        return self._call_value_candidates(name, properties.get(name))
      fixed = [name for name in ("robot", "timeout") if name not in used]
      required = set(schema.get("required", [])) if isinstance(schema, dict) else set()
      fields = [name for name in properties if name not in used]
      fields.sort(key=lambda name: (name not in required, name))
      return [*(f"{name}=" for name in fixed), *(f"{name}=" for name in fields)]
    if operation in {"show", "watch", "cancel"}:
      if len(earlier) == 2:
        return sorted(self._state.skill_executions)
      if len(earlier) >= 3 and not any(w.startswith("robot=") for w in earlier[3:]):
        return ["robot="]
    return []

  def _skill_descriptor(self, identity: str) -> dict[str, Any] | None:
    """Resolve an exact ``name@version`` from the current catalog."""
    return next(
      (
        item
        for item in (self._state.skills or {}).get("skills", [])
        if isinstance(item, dict)
        and f"{item.get('name')}@{item.get('version')}" == identity
      ),
      None,
    )

  def _complete_input(self) -> None:
    """Expand the current token or leave the available matches in the hint."""
    primitive = self._call_autofill_primitive()
    if primitive is not None:
      arguments = call_arguments_skeleton(primitive.get("schema"))
      command = self._input_buf.rstrip()
      formatted = _format_call_arguments(arguments) if arguments else "{}"
      self._set_input(f"{command} {formatted}")
      self._reset_history_search()
      return
    candidates = self._completion_candidates()
    if not candidates:
      self._completion_matches = []
      return
    cursor = self._input_cursor
    start = cursor
    while start > 0 and not self._input_buf[start - 1].isspace():
      start -= 1
    token = self._input_buf[start:cursor]
    replacement = candidates[0] if len(candidates) == 1 else _common_prefix(candidates)
    if len(replacement) <= len(token):
      self._completion_matches = candidates
      return
    self._input_buf = self._input_buf[:start] + replacement + self._input_buf[cursor:]
    self._input_cursor = start + len(replacement)
    self._completion_matches = []
    self._reset_history_search()

  def _call_autofill_primitive(self) -> dict[str, Any] | None:
    """Resolve an exact call target when required arguments are still absent."""
    if self._input_cursor != len(self._input_buf):
      return None
    words = self._input_buf.split()
    if len(words) != 2 or words[0].lower() != "call":
      return None
    target = words[1]
    return self._call_primitive(target)

  def _call_primitive(self, target: str) -> dict[str, Any] | None:
    """Return the live primitive descriptor for one exact call target."""
    _, primitives = self._parsed_primitives()
    return next(
      (primitive for primitive in primitives if call_target(primitive) == target),
      None,
    )

  @staticmethod
  def _call_value_candidates(name: str, spec: Any) -> list[str]:
    """Return assignment completions for a schema enum, constant, or boolean."""
    if not isinstance(spec, dict):
      return []
    if "const" in spec:
      values = [spec["const"]]
    elif isinstance(spec.get("enum"), list):
      values = spec["enum"]
    elif spec.get("type") == "boolean":
      values = [True, False]
    else:
      return []
    return [f"{name}={_format_call_value(value)}" for value in values]

  def _record_history(self, line: str) -> None:
    """Keep a bounded unique session history without persisting operator input."""
    if not line:
      return
    if line in self._input_history:
      self._input_history.remove(line)
    self._input_history.append(line)
    del self._input_history[: -self._MAX_HISTORY]

  def _set_input(self, value: str) -> None:
    """Replace the editable input buffer and place the cursor at its end."""
    self._input_buf = value
    self._input_cursor = len(value)
    self._completion_matches = []

  def _reset_history_search(self) -> None:
    """Leave history navigation whenever the user edits the current command."""
    self._history_index = None
    self._history_query = None
    self._completion_matches = []

  def _history_previous(self) -> None:
    """Load the previous command from the session history."""
    if not self._input_history:
      return
    if self._history_index is None:
      self._history_index = len(self._input_history) - 1
    else:
      self._history_index = max(self._history_index - 1, 0)
    self._set_input(self._input_history[self._history_index])

  def _history_next(self) -> None:
    """Load the next command from the session history, ending with a blank line."""
    if self._history_index is None:
      return
    if self._history_index >= len(self._input_history) - 1:
      self._reset_history_search()
      self._set_input("")
      return
    self._history_index += 1
    self._set_input(self._input_history[self._history_index])

  def _history_search(self) -> None:
    """Reverse-search session history for the command present at first Ctrl-R."""
    if not self._input_history:
      return
    if self._history_query is None:
      self._history_query = self._input_buf
    start = (
      self._history_index
      if self._history_index is not None
      else len(self._input_history)
    )
    for index in range(start - 1, -1, -1):
      if self._history_query in self._input_history[index]:
        self._history_index = index
        self._set_input(self._input_history[index])
        return

  def handle_key(
    self, key: int, height: int, *, line_count: int = 0, width: int = 80
  ) -> None:
    """Process one keypress."""
    if (
      self._help_open
      or self._mission_detail is not None
      or self._guide_lines is not None
      or self._backends_open
    ):
      self._help_open = False
      self._mission_detail = None
      self._guide_lines = None
      self._backends_open = False
      return
    if key in (9, ord("\t")):  # Tab — complete the token at the cursor.
      self._complete_input()
      return
    if key in (ord("\n"), ord("\r"), curses.KEY_ENTER):
      self._record_history(self._input_buf)
      self._run_command(self._input_buf)
      self._set_input("")
      self._reset_history_search()
      return
    if key in (curses.KEY_BACKSPACE, 127, 8):
      if self._input_cursor:
        cursor = self._input_cursor
        self._input_buf = self._input_buf[: cursor - 1] + self._input_buf[cursor:]
        self._input_cursor -= 1
        self._reset_history_search()
      return
    if key == curses.KEY_DC:
      cursor = self._input_cursor
      self._input_buf = self._input_buf[:cursor] + self._input_buf[cursor + 1 :]
      self._reset_history_search()
      return
    command_active = bool(self._input_buf)
    if key == 2 or (command_active and key == curses.KEY_LEFT):  # Ctrl-B or Left
      self._input_cursor = max(self._input_cursor - 1, 0)
      return
    if key == 6 or (command_active and key == curses.KEY_RIGHT):  # Ctrl-F or Right
      self._input_cursor = min(self._input_cursor + 1, len(self._input_buf))
      return
    if key == 1 or (command_active and key == curses.KEY_HOME):  # Ctrl-A or Home
      self._input_cursor = 0
      return
    if key == 5 or (command_active and key == curses.KEY_END):  # Ctrl-E or End
      self._input_cursor = len(self._input_buf)
      return
    if key == 21:  # Ctrl-U
      self._set_input("")
      self._reset_history_search()
      return
    if key == 23:  # Ctrl-W
      cursor = self._input_cursor
      start = cursor
      while start and self._input_buf[start - 1].isspace():
        start -= 1
      while start and not self._input_buf[start - 1].isspace():
        start -= 1
      self._input_buf = self._input_buf[:start] + self._input_buf[cursor:]
      self._input_cursor = start
      self._reset_history_search()
      return
    if key == 18:  # Ctrl-R
      self._history_search()
      return
    if key == 16:  # Ctrl-P
      self._history_previous()
      return
    if key == 14:  # Ctrl-N
      self._history_next()
      return
    if command_active and key == curses.KEY_UP:
      self._history_previous()
      return
    if command_active and key == curses.KEY_DOWN:
      self._history_next()
      return
    if 32 <= key <= 126:  # printable ASCII → command line
      cursor = self._input_cursor
      self._input_buf = self._input_buf[:cursor] + chr(key) + self._input_buf[cursor:]
      self._input_cursor += 1
      self._reset_history_search()
      return
    pane = self._focus
    page = max(height - 4, 1)
    visible = max(height - 6, 1)
    max_scroll = max(line_count - visible, 0)
    if key == curses.KEY_UP:
      self._scroll[pane] = min(self._scroll[pane] + 1, max_scroll)
    elif key == curses.KEY_DOWN:
      self._scroll[pane] = max(self._scroll[pane] - 1, 0)
    elif key in (curses.KEY_PPAGE, curses.KEY_F7):
      self._scroll[pane] = min(self._scroll[pane] + page, max_scroll)
    elif key in (curses.KEY_NPAGE, curses.KEY_F8):
      self._scroll[pane] = max(self._scroll[pane] - page, 0)
    elif key == curses.KEY_HOME:
      self._scroll[pane] = max_scroll
    elif key == curses.KEY_END:
      self._scroll[pane] = 0
    if pane == "primitives":
      # One page per primitive (0 = overview); a fresh page restarts at its top.
      if key == curses.KEY_LEFT:
        self._primitives_page = max(self._primitives_page - 1, 0)
        self._scroll["primitives"] = 0
      elif key == curses.KEY_RIGHT:
        self._primitives_page = min(
          self._primitives_page + 1,
          max(self._primitives_page_count(width) - 1, 0),
        )
        self._scroll["primitives"] = 0
    if pane == "context":
      split = self._context_split_width(width)
      if split is not None:
        _, side_inner_w = split
        side_visible = max(height - 6, 1)
        side_max = max(
          len(self._context_primitives_lines(side_inner_w)) - side_visible, 0
        )
        if key == curses.KEY_LEFT:
          self._scroll["context_primitives"] = min(
            self._scroll["context_primitives"] + 1, side_max
          )
        elif key == curses.KEY_RIGHT:
          self._scroll["context_primitives"] = max(
            self._scroll["context_primitives"] - 1, 0
          )

  def handle_alt_digit(self, digit: int) -> bool:
    """Focus the pane bound to an Alt digit and return whether it was recognized.

    Terminals normally encode Alt as Escape followed by the base character;
    the curses loop separates that transport detail before calling this method.
    """
    pane = self._PANE_SHORTCUTS.get(digit)
    if pane is None:
      return False
    self._set_focus(pane)
    self._help_open = False
    self._mission_detail = None
    self._guide_lines = None
    self._backends_open = False
    return True

  def _set_focus(self, pane: str) -> None:
    """Focus a pane and request the erase repaint needed by its layout."""
    self._focus = pane
    self._focus_changed = True

  def _resolve_focus(self, name: str) -> str | None:
    key = name.lower().replace("_", " ").strip()
    if key in self._FOCUS_ALIASES:
      return self._FOCUS_ALIASES[key]
    for pane in self._PANES:
      if key in {
        pane,
        self._PANE_TITLES[pane],
        self._PANE_TITLES[pane].replace(" ", ""),
      }:
        return pane
    return None

  def _draw_guide(self, stdscr, height: int, width: int) -> None:
    """Overlay the ``call`` JSON guide — fields plus a fill-in skeleton."""
    lines = self._guide_lines or []
    menu_h = min(len(lines) + 2, max(height - 2, 3))
    menu_w = min(
      max(len(text) for text, _ in lines) + 4 if lines else 20,
      max(width - 2, 10),
    )
    y = max((height - menu_h) // 2, 0)
    x = max((width - menu_w) // 2, 0)
    self._draw_box(stdscr, y, x, menu_h, menu_w, "guide", True)
    for i, (text, style) in enumerate(lines[: menu_h - 2]):
      stdscr.addnstr(y + 1 + i, x + 2, text, menu_w - 4, _attr(style))

  def _draw_mission_detail(self, stdscr, height: int, width: int) -> None:
    """Overlay the selected mission from the current stream snapshot."""
    lines = format_mission_detail(self._mission_detail or {}, max(width - 8, 16))
    menu_h = min(len(lines) + 2, max(height - 2, 3))
    menu_w = min(
      max(max(len(text) for text, _ in lines) + 4, 44),
      max(width - 2, 10),
    )
    y = max((height - menu_h) // 2, 0)
    x = max((width - menu_w) // 2, 0)
    self._draw_box(stdscr, y, x, menu_h, menu_w, "mission detail", True)
    for i, (text, style) in enumerate(lines[: menu_h - 2]):
      stdscr.addnstr(y + 1 + i, x + 2, text, menu_w - 4, _attr(style))

  def _open_call_guide(
    self,
    primitive_name: str | None,
    robot_id: str | None = None,
    device_id: str | None = None,
  ) -> None:
    """Show the catalog list or one primitive's argument skeleton."""
    _, primitives = self._parsed_primitives()
    if primitive_name is None:
      self._guide_lines = format_call_guide(None, primitives)
      return

    def _matches(primitive: dict[str, Any]) -> bool:
      if str(primitive.get("name", "")) != primitive_name:
        return False
      return robot_id is None or (
        primitive.get("robot_id") == robot_id
        and primitive.get("device_id") == device_id
      )

    primitive = next((item for item in primitives if _matches(item)), None)
    if primitive is None and self._state.primitives:
      for entry in self._state.primitives.get("primitives", []):
        if str(entry.get("name", "")) == primitive_name:
          primitive = {
            "name": primitive_name,
            "version": str(entry.get("version", "?")),
            "description": str(entry.get("description", "")),
            "device_description": str(entry.get("device_description", "")),
            "operator": bool(entry.get("operator", False)),
            "limits": entry.get("limits"),
            "robot_id": entry.get("robot_id") or robot_id,
            "device_id": entry.get("device_id") or device_id,
            "schema": entry.get("arguments_schema"),
          }
          break
    if primitive is None:
      primitive = {
        "name": primitive_name,
        "version": "?",
        "description": "",
        "device_description": "",
        "operator": False,
        "limits": None,
        "robot_id": robot_id,
        "device_id": device_id,
        "schema": None,
      }
    self._guide_lines = format_call_guide(primitive)

  def _fire(self, coro: Any) -> None:
    """Schedule a command callback; drop it when no loop is running (tests)."""
    try:
      asyncio.create_task(coro)
    except RuntimeError:
      coro.close()

  def _run_command(self, text: str) -> None:
    """Execute one operator command or queue a foreground REPL handoff."""
    name, arg = parse_command(text)
    if not name:
      return
    if name in ("quit", "exit"):
      self._quit.set()
    elif name == "help":
      self._help_open = True
    elif name == "backends":
      self._backends_open = True
    elif name == "repl":
      self._queue_repl(arg)
    elif name == "focus":
      if not arg:
        feedback(
          self._state,
          "usage: focus context|missions|primitives|modelio|logs|status",
          "warning",
        )
      else:
        pane = self._resolve_focus(arg)
        if pane is None:
          feedback(self._state, f"unknown pane: {arg}", "warning")
        else:
          self._set_focus(pane)
    elif name == "mission":
      self._run_mission_command(arg)
    elif name == "skill":
      self._run_skill_command(arg)
    elif name == "chat":
      if not arg:
        feedback(self._state, 'usage: chat "sentence"', "warning")
      elif self.on_chat is None:
        feedback(self._state, "chat is not wired", "warning")
      else:
        self._fire(self.on_chat(arg))
    elif name == "call":
      head, _, rest = arg.partition(" ")
      head = head.strip()
      rest = rest.strip()
      # Remote primitives use ``primitive@robot/device``. A bare primitive
      # opens its catalog guide so the operator can choose an exact target.
      primitive, separator, target = head.partition("@")
      primitive = primitive.strip()
      robot_id, slash, device_id = target.partition("/")
      robot_id = robot_id.strip() if separator else ""
      device_id = device_id.strip() if slash else ""
      if not primitive or not rest or not robot_id:
        self._open_call_guide(primitive or None, robot_id or None, device_id or None)
        return
      try:
        arguments = parse_call_arguments(rest)
      except ValueError as exc:
        feedback(self._state, f"call: {exc}", "warning")
        self._open_call_guide(primitive, robot_id, device_id or None)
        return
      if self.on_call is None:
        feedback(self._state, "call is not wired", "warning")
      else:
        self._fire(self.on_call(primitive, arguments, robot_id, device_id or None))
    else:
      feedback(self._state, f"unknown command: {name} (try help)", "warning")

  def _run_skill_command(self, text: str) -> None:
    """Validate and dispatch one exact northbound skill operation."""
    try:
      words = shlex.split(text)
    except ValueError as exc:
      feedback(self._state, f"skill: invalid quoting ({exc})", "warning")
      return
    if len(words) < 2 or words[0] not in self._SKILL_SUBCOMMANDS:
      feedback(
        self._state,
        "usage: skill start <name>@<version> robot=<alias> timeout=<s> key=value..."
        " | skill show|watch|cancel <execution-id> robot=<alias>",
        "warning",
      )
      return
    operation, target, *assignments = words
    try:
      values = parse_call_arguments(
        " ".join(shlex.quote(value) for value in assignments)
      )
    except ValueError as exc:
      feedback(self._state, f"skill {operation}: {exc}", "warning")
      return
    robot_id = values.pop("robot", None)
    if not isinstance(robot_id, str) or not robot_id:
      feedback(self._state, f"skill {operation}: robot=<alias> is required", "warning")
      return
    if operation == "start":
      name, separator, version = target.rpartition("@")
      if not separator or not name or not version:
        feedback(self._state, "skill start: expected exact <name>@<version>", "warning")
        return
      timeout = values.pop("timeout", None)
      if not isinstance(timeout, int | float) or isinstance(timeout, bool):
        feedback(self._state, "skill start: timeout=<seconds> is required", "warning")
        return
      timeout_s = float(timeout)
      if not 0 < timeout_s <= 3600:
        feedback(self._state, "skill start: timeout must be in (0, 3600]", "warning")
        return
      payload = {
        "operation": "start",
        "robot_id": robot_id,
        "name": name,
        "version": version,
        "arguments": values,
        "timeout_s": timeout_s,
      }
      self._dispatch_skill(payload)
      self._set_focus("skills")
      return
    if values:
      feedback(self._state, f"skill {operation}: unexpected arguments", "warning")
      return
    if operation == "watch":
      if self.on_skill_watch is None:
        feedback(self._state, "skill watch is not wired", "warning")
      else:
        self._set_focus("skills")
        self._fire(self.on_skill_watch(robot_id, target))
      return
    self._dispatch_skill(
      {
        "operation": "get_execution" if operation == "show" else "cancel",
        "robot_id": robot_id,
        "execution_id": target,
      }
    )
    self._set_focus("skills")

  def _dispatch_skill(self, payload: dict[str, Any]) -> None:
    """Schedule one skill command through the callback installed by main."""
    if self.on_skill is None:
      feedback(self._state, "skill command is not wired", "warning")
      return
    self._fire(self.on_skill(payload))

  def _mission_entries(self) -> list[dict[str, Any]]:
    """Return current stream missions and history, deduplicated by full ID."""
    payload = self._state.missions or {}
    entries: list[dict[str, Any]] = []
    seen: set[str] = set()
    for key in ("missions", "history"):
      values = payload.get(key)
      if not isinstance(values, list):
        continue
      for value in values:
        if not isinstance(value, dict):
          continue
        mission_id = value.get("mission_id")
        if isinstance(mission_id, str) and mission_id not in seen:
          seen.add(mission_id)
          entries.append(value)
    return entries

  def _mission_completion_ids(self) -> list[str]:
    """Return compact prefixes that uniquely identify visible missions."""
    ids = sorted(str(entry["mission_id"]) for entry in self._mission_entries())
    prefixes: list[str] = []
    for mission_id in ids:
      length = min(8, len(mission_id))
      while length < len(mission_id) and any(
        other != mission_id and other.startswith(mission_id[:length]) for other in ids
      ):
        length += 1
      prefixes.append(mission_id[:length])
    return prefixes

  def _resolve_mission(self, prefix: str) -> dict[str, Any] | None:
    """Resolve an exact mission ID or unique prefix from the current snapshot."""
    entries = self._mission_entries()
    exact = [entry for entry in entries if entry.get("mission_id") == prefix]
    matches = exact or [
      entry for entry in entries if str(entry.get("mission_id", "")).startswith(prefix)
    ]
    if not matches:
      feedback(self._state, f"mission: no mission matches '{prefix}'", "warning")
      return None
    if len(matches) > 1:
      choices = ", ".join(str(entry["mission_id"]) for entry in matches)
      feedback(
        self._state,
        f"mission: ambiguous ID prefix '{prefix}': {choices}",
        "warning",
      )
      return None
    return matches[0]

  def _mission_mutation_payload(
    self, operation: str, mission: dict[str, Any]
  ) -> dict[str, Any] | None:
    """Build compare-and-swap fields from the stream's current document."""
    document = mission.get("document")
    revision = document.get("revision") if isinstance(document, dict) else None
    if not isinstance(revision, int):
      feedback(
        self._state,
        f"mission {operation}: current document revision is unavailable",
        "warning",
      )
      return None
    return {
      "operation": operation,
      "mission_id": str(mission["mission_id"]),
      "expected_revision": revision,
    }

  def _run_mission_command(self, argument: str) -> None:
    """Validate and dispatch one mission command against current console state."""
    try:
      subcommand, args = parse_mission_command(argument)
    except ValueError as exc:
      feedback(self._state, f"mission: invalid quoting: {exc}", "warning")
      return
    usage = {
      "": "mission add|list|show|edit|priority|cancel",
      "add": 'mission add "order"',
      "list": "mission list",
      "show": "mission show <unique-id-prefix>",
      "edit": 'mission edit <unique-id-prefix> "order"',
      "priority": "mission priority <unique-id-prefix> low|normal|high|urgent",
      "cancel": "mission cancel <unique-id-prefix>",
    }
    if subcommand not in self._MISSION_SUBCOMMANDS:
      feedback(self._state, f"usage: {usage['']}", "warning")
      return
    expected = {"add": 1, "list": 0, "show": 1, "edit": 2, "priority": 2, "cancel": 1}
    if len(args) != expected[subcommand] or (args and not all(args)):
      feedback(self._state, f"usage: {usage[subcommand]}", "warning")
      return
    if subcommand == "list":
      self._set_focus("missions")
      return
    if subcommand == "add":
      payload: dict[str, Any] = {"operation": "create", "order": args[0]}
    else:
      mission = self._resolve_mission(args[0])
      if mission is None:
        return
      if subcommand == "show":
        self._set_focus("missions")
        self._mission_detail = mission
        return
      operation = "update" if subcommand == "edit" else subcommand
      mutation = self._mission_mutation_payload(operation, mission)
      if mutation is None:
        return
      payload = mutation
      if subcommand == "edit":
        payload["order"] = args[1]
      elif subcommand == "priority":
        if args[1] not in self._PRIORITY_BANDS:
          feedback(self._state, f"usage: {usage[subcommand]}", "warning")
          return
        payload["priority_band"] = args[1]
    if self.on_mission is None:
      feedback(self._state, "mission command is not wired", "warning")
    else:
      self._fire(self.on_mission(payload))

  def _queue_repl(self, alias: str) -> None:
    """Validate a live backend selection and queue its terminal handoff."""
    rows = backend_profiles(self._state.backend)
    reachable = [row for row in rows if row["connection"] == "up"]
    selected = [row for row in rows if row["backend"] == alias] if alias else reachable
    if not alias and len(reachable) > 1:
      choices = ", ".join(row["backend"] for row in reachable)
      feedback(self._state, f"repl: choose a backend: {choices}", "warning")
      return
    if not selected:
      detail = f"unknown backend: {alias}" if alias else "no backend is up"
      feedback(self._state, f"repl: {detail}", "warning")
      return
    row = selected[0]
    if row["connection"] != "up":
      feedback(
        self._state,
        f"repl: backend {row['backend']} is not up ({row['connection']})",
        "warning",
      )
      return
    if not row["endpoint"]:
      feedback(
        self._state, f"repl: backend {row['backend']} has no endpoint", "warning"
      )
      return
    if self.on_repl is None:
      feedback(self._state, "repl is not wired", "warning")
      return
    self._repl_request = (row["backend"], row["endpoint"])

  # --- primitives ------------------------------------------------------------

  def _clear_block(self, stdscr, y: int, x: int, h: int, w: int) -> None:
    """Blank a rectangular overlay region before painting its current frame."""
    if h <= 0 or w <= 0:
      return
    blank = " " * w
    for row in range(h):
      try:
        stdscr.addnstr(y + row, x, blank, w, _attr("plain"))
      except curses.error:
        return

  def _blit(
    self, stdscr, y: int, x: int, h: int, w: int, lines, scroll_from_bottom: int
  ) -> None:
    """Draw a bottom-anchored viewport and its available-scroll indicators."""
    if h <= 0 or w <= 0:
      return
    total = len(lines)
    max_scroll = max(total - h, 0)
    scroll = min(max(scroll_from_bottom, 0), max_scroll)
    end = max(total - scroll, 0)
    start = max(end - h, 0)
    segment = lines[start:end]
    blank = " " * w
    for row in range(h):
      try:
        stdscr.addnstr(y + row, x, blank, w, _attr("plain"))
      except curses.error:
        return
    for row, line in enumerate(segment):
      segs = line if isinstance(line, list) else [line]
      col = 0  # running offset within the blit; paint at x + col, never x
      for text, style in segs:
        if col >= w:
          break
        try:
          stdscr.addnstr(y + row, x + col, text, w - col, _attr(style))
        except curses.error:
          return
        col += len(text)
    # The pane is bottom-anchored: ▲ means older lines are available above,
    # while ▼ means the viewport can return toward the newest lines below.
    if h > 1:
      try:
        if scroll < max_scroll:
          stdscr.addnstr(y, x + w - 1, "▲", 1, _attr("accent"))
        if scroll > 0:
          stdscr.addnstr(y + h - 1, x + w - 1, "▼", 1, _attr("accent"))
      except curses.error:
        return

  def _draw_box(
    self,
    stdscr,
    y: int,
    x: int,
    h: int,
    w: int,
    title: str,
    focused: bool,
    title_style: str = "title",
  ) -> None:
    if h < 2 or w < 2:
      return
    border = _attr(title_style) if focused else _attr("border")
    # Leave the terminal's bottom-right cell alone — writing it scrolls/errors.
    max_x = x + w - 1
    try:
      stdscr.attron(border)
      for col in range(x, max_x):
        stdscr.addch(y, col, curses.ACS_HLINE)
        stdscr.addch(y + h - 1, col, curses.ACS_HLINE)
      for row in range(y, y + h):
        stdscr.addch(row, x, curses.ACS_VLINE)
        if max_x > x:
          stdscr.addch(row, max_x, curses.ACS_VLINE)
      stdscr.addch(y, x, curses.ACS_ULCORNER)
      stdscr.addch(y, max_x, curses.ACS_URCORNER)
      stdscr.addch(y + h - 1, x, curses.ACS_LLCORNER)
      stdscr.addch(y + h - 1, max_x, curses.ACS_LRCORNER)
      stdscr.attroff(border)
      stdscr.addnstr(y, x + 2, f" {title} ", max(w - 4, 0), _attr(title_style))
    except curses.error:
      try:
        stdscr.attroff(border)
      except curses.error:
        pass

  def _draw_status_bar(self, stdscr, y: int, width: int) -> None:
    state = self._state
    backend, _ = backend_summary(state.backend, state.backend_link)
    child = (
      "attached"
      if state.child_exited is None
      else f"child exited ({state.child_exited})"
    )
    hints = "Alt+1..7 panes · Tab complete · PgUp/Dn or F7/F8 scroll · help"
    thinking = sum(bool(agent.get("thinking")) for agent in state.model_agents)
    agent_summary = f" · agents {len(state.model_agents)}"
    if thinking:
      agent_summary += f" · thinking {thinking}"
    left = f" {hints}  │  model reqs {state.model_requests}{agent_summary}  │  "
    bar = f"{left}{backend}  │  {child}  │  state:{self._config.state_dir} "
    stdscr.addnstr(y, 0, bar.ljust(width - 1), width - 1, curses.A_REVERSE)


async def run_curses(app: ConsoleApp) -> None:
  """Drive the app on the current terminal until the user quits.

  Input is polled at a fixed cadence; frames are painted when the state
  changed (``state.dirty``), a key was handled, or the 1s uptime tick
  rolled over — but feed-driven frames are throttled to ``_MIN_FRAME_S``:
  the parts/context feeds push at up to 10Hz and one full repaint per
  push reads as the whole page blinking.
  """
  import curses as _curses
  from time import monotonic

  _MIN_FRAME_S = 0.25  # feed-driven repaint ceiling (4 fps)

  def configure_screen():
    """Enter curses mode and configure the screen used by the render loop."""
    screen = _curses.initscr()
    _curses.noecho()
    _curses.cbreak()
    screen.keypad(True)
    screen.nodelay(True)
    screen.scrollok(False)
    try:
      _curses.curs_set(0)
    except _curses.error:
      pass
    _init_colors()
    return screen

  def release_screen(screen) -> None:
    """Restore normal tty modes before a foreground child takes ownership."""
    try:
      _curses.curs_set(1)
    except _curses.error:
      pass
    screen.keypad(False)
    _curses.nocbreak()
    _curses.echo()
    _curses.endwin()

  stdscr = configure_screen()
  screen_active = True
  try:
    terminal_size: tuple[int, int] | None = None
    last_draw = -1e9  # force the first frame
    while not app.quit_event.is_set():
      force = app.consume_ready_refresh()
      terminal_size = _sync_terminal_size(stdscr, terminal_size, force=force)
      key = stdscr.getch()
      resized = False
      handled = False
      while key != -1:
        if key == _curses.KEY_RESIZE:
          resized = True
        elif key == 27:  # Most terminals encode Alt+key as Escape then the key.
          next_key = stdscr.getch()
          if ord("1") <= next_key <= ord("6") and app.handle_alt_digit(
            next_key - ord("0")
          ):
            handled = True
            key = stdscr.getch()
            continue
          app.handle_key(
            key,
            terminal_size[0],
            line_count=app._focused_line_count(terminal_size[1]),
            width=terminal_size[1],
          )
          handled = True
          key = next_key
          continue
        elif 0xB1 <= key <= 0xB6 and app.handle_alt_digit(key - 0xB0):
          # Some terminals send Meta as an 8-bit character instead of Escape.
          handled = True
        else:
          app.handle_key(
            key,
            terminal_size[0],
            line_count=app._focused_line_count(terminal_size[1]),
            width=terminal_size[1],
          )
          handled = True
        key = stdscr.getch()
      repl_request = app.take_repl_request()
      if repl_request is not None:
        alias, endpoint = repl_request
        release_screen(stdscr)
        screen_active = False
        try:
          await app.on_repl(alias, endpoint)
        finally:
          stdscr = configure_screen()
          screen_active = True
          terminal_size = _refresh_on_resize(stdscr)
          last_draw = -1e9
          app._state.dirty.set()
        continue
      if resized:
        # Erase-refresh ONLY on a real size change. resizeterm synthesizes a
        # fresh KEY_RESIZE on every call, so treating any drained KEY_RESIZE
        # as a change self-perpetuates: erase → resizeterm → KEY_RESIZE →
        # erase…, which reads as constant blinking. A synthesized key shows
        # no size delta and is skipped.
        curses.update_lines_cols()
        size = stdscr.getmaxyx()
        if terminal_size is None or size != terminal_size:
          terminal_size = _refresh_on_resize(stdscr)
      if app.consume_focus_refresh():
        # Focus moved (shortcut or `focus`): the new pane paints a
        # different layout over the same cells — erase or the old pane's
        # shorter-line leftovers survive beside the new content.
        terminal_size = _refresh_on_resize(stdscr)
        handled = True
      now = monotonic()
      # User input repaints immediately; feed churn coalesces to 4 fps.
      due = now - last_draw >= (_MIN_FRAME_S if not (handled or force) else 0.0)
      if due and (
        handled
        or resized
        or force
        or app._state.dirty.is_set()
        or now - last_draw >= 1.0
      ):
        app._state.dirty.clear()
        try:
          app.draw(stdscr, terminal_size=terminal_size)
        except curses.error:
          terminal_size = None
          continue
        last_draw = now
      await asyncio.sleep(0.05)
  finally:
    if screen_active:
      release_screen(stdscr)
