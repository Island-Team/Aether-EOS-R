"""Command handling and terminal resize sync for libexec/egia_ext/tui.app."""

from __future__ import annotations

import asyncio
import curses
from unittest.mock import MagicMock, patch

import pytest

import egia_ext.tui.app as tui_app
from egia_ext.tui.app import (
  ConsoleApp,
  _refresh_on_resize,
  _sync_terminal_size,
  parse_call_arguments,
  parse_mission_command,
  run_curses,
)
from egia_ext.tui.config import TuiConfig
from egia_ext.tui.feeds import ConsoleState


def _app() -> ConsoleApp:
  return ConsoleApp(ConsoleState(), TuiConfig())


def _mission(mission_id: str, revision: int = 3) -> dict:
  return {
    "mission_id": mission_id,
    "order": "inspect the loading bay",
    "state": "pending",
    "priority_band": "normal",
    "document": {"revision": revision, "completeness": 0.0, "plan": None},
  }


def test_quit_command_sets_quit_event() -> None:
  app = _app()
  app._run_command("quit")
  assert app.quit_event.is_set()


def test_repl_command_queues_only_reachable_backend() -> None:
  app = _app()
  app.on_repl = MagicMock()
  app._state.backend = {
    "backends": [
      {
        "backend": "workcell",
        "connection": "up",
        "endpoint": "127.0.0.1:16003",
      }
    ]
  }

  app._run_command("repl")

  assert app.take_repl_request() == ("workcell", "127.0.0.1:16003")
  assert app.take_repl_request() is None


def test_repl_command_requires_alias_for_fleet() -> None:
  app = _app()
  app.on_repl = MagicMock()
  app._state.backend = {
    "backends": [
      {"backend": "left", "connection": "up", "endpoint": "left:16003"},
      {"backend": "right", "connection": "up", "endpoint": "right:16003"},
    ]
  }

  app._run_command("repl")

  assert app.take_repl_request() is None
  assert any(
    "choose a backend: left, right" in str(item.event) for item in app._state.logs
  )


def test_repl_command_selects_only_reachable_fleet_backend() -> None:
  app = _app()
  app.on_repl = MagicMock()
  app._state.backend = {
    "backends": [
      {"backend": "left", "connection": "down", "endpoint": "left:16003"},
      {"backend": "right", "connection": "up", "endpoint": "right:16003"},
    ]
  }

  app._run_command("repl")

  assert app.take_repl_request() == ("right", "right:16003")


def test_repl_command_rejects_unknown_or_down_backend() -> None:
  app = _app()
  app.on_repl = MagicMock()
  app._state.backend = {
    "backends": [{"backend": "robot", "connection": "down", "endpoint": "robot:16003"}]
  }

  app._run_command("repl missing")
  app._run_command("repl robot")

  assert app.take_repl_request() is None
  messages = [str(item.event.get("event")) for item in app._state.logs]
  assert any("unknown backend: missing" in message for message in messages)
  assert any("backend robot is not up (down)" in message for message in messages)


def test_repl_handoff_releases_and_restores_curses() -> None:
  app = _app()
  app._repl_request = ("robot", "127.0.0.1:16003")
  screens = [MagicMock(), MagicMock()]
  for screen in screens:
    screen.getmaxyx.return_value = (24, 80)
    screen.getch.return_value = -1

  async def launch(alias: str, endpoint: str) -> None:
    assert (alias, endpoint) == ("robot", "127.0.0.1:16003")
    app.quit_event.set()

  app.on_repl = launch
  with (
    patch("curses.initscr", side_effect=screens) as initscr,
    patch("curses.noecho"),
    patch("curses.cbreak"),
    patch("curses.curs_set"),
    patch("curses.nocbreak"),
    patch("curses.echo"),
    patch("curses.endwin") as endwin,
    patch("curses.has_colors", return_value=False),
    patch("curses.update_lines_cols"),
    patch("curses.resizeterm"),
  ):
    asyncio.run(run_curses(app))

  assert initscr.call_count == 2
  assert endwin.call_count == 2
  screens[0].keypad.assert_any_call(False)
  screens[1].keypad.assert_any_call(True)
  screens[1].erase.assert_called_once()


def test_focus_command_switches_pane() -> None:
  app = _app()
  app._run_command("focus logs")
  assert app._focus == "logs"
  app._run_command("focus primitives")
  assert app._focus == "primitives"


def test_pane_headings_have_distinct_module_styles() -> None:
  app = _app()
  styles = {pane: app._pane_title_style(pane) for pane in app._PANES}
  assert styles == {
    "context": "pane_context",
    "missions": "pane_missions",
    "primitives": "pane_primitives",
    "skills": "pane_skills",
    "modelio": "pane_modelio",
    "logs": "pane_logs",
    "status": "pane_status",
  }


def test_status_bar_reports_agent_and_thinking_counts() -> None:
  state = ConsoleState(
    model_requests=6,
    model_agents=[
      {"agent_id": "planner", "thinking": False},
      {"agent_id": "sentinel", "thinking": True},
    ],
  )
  app = ConsoleApp(state, TuiConfig())
  stdscr = MagicMock()

  app._draw_status_bar(stdscr, 23, 160)

  bar = stdscr.addnstr.call_args.args[2]
  assert "model reqs 6 · agents 2 · thinking 1" in bar


def test_tab_completes_a_unique_command() -> None:
  app = _app()
  for char in "foc":
    app.handle_key(ord(char), 24)

  app.handle_key(ord("\t"), 24)

  assert app._input_buf == "focus"
  assert app._input_cursor == len("focus")


def test_tab_completes_an_explicit_live_primitive_target() -> None:
  app = _app()
  app._state.primitives = {
    "primitives": [
      {
        "name": "arm.movej",
        "robot_id": "robot-a",
        "device_id": "arm-0",
        "operator": False,
        "arguments_schema": {
          "type": "object",
          "required": ["joint_positions_rad"],
          "properties": {
            "joint_positions_rad": {
              "type": "array",
              "items": {"type": "number"},
              "minItems": 1,
            },
            "speed_rad_s": {"type": "number"},
          },
        },
      }
    ]
  }
  for char in "call arm.m":
    app.handle_key(ord(char), 24)

  app.handle_key(ord("\t"), 24)

  assert app._input_buf == "call arm.movej@robot-a/arm-0"

  app.handle_key(ord("\t"), 24)

  assert app._input_buf == "call arm.movej@robot-a/arm-0 joint_positions_rad='[0]'"


def test_call_autofill_uses_required_schema_defaults_and_constraints() -> None:
  app = _app()
  app._state.primitives = {
    "primitives": [
      {
        "name": "demo.run",
        "robot_id": "robot-a",
        "device_id": "demo-0",
        "operator": True,
        "arguments_schema": {
          "type": "object",
          "required": ["mode", "count", "nested"],
          "properties": {
            "mode": {"type": "string", "enum": ["safe", "fast"]},
            "count": {"type": "integer", "minimum": 2},
            "nested": {
              "type": "object",
              "required": ["enabled"],
              "properties": {
                "enabled": {"type": "boolean", "default": True},
                "note": {"type": "string"},
              },
            },
            "optional": {"type": "string", "default": "omitted"},
          },
        },
      }
    ]
  }
  app._set_input("call demo.run@robot-a/demo-0 ")

  assert app._input_hint() == "Tab: insert required key=value parameters"
  app.handle_key(ord("\t"), 24)

  assert app._input_buf == (
    "call demo.run@robot-a/demo-0 mode=safe count=2 nested='{\"enabled\":true}'"
  )


def test_call_autofill_inserts_empty_object_without_schema() -> None:
  app = _app()
  app._state.primitives = {
    "primitives": [
      {
        "name": "safety.estop",
        "robot_id": "robot-a",
        "device_id": None,
        "operator": True,
      }
    ]
  }
  app._set_input("call safety.estop@robot-a")

  app.handle_key(ord("\t"), 24)

  assert app._input_buf == "call safety.estop@robot-a {}"


def test_tab_completes_remaining_call_fields_and_enum_values() -> None:
  app = _app()
  app._state.primitives = {
    "primitives": [
      {
        "name": "base.posture",
        "robot_id": "robot-a",
        "device_id": "base-0",
        "operator": False,
        "arguments_schema": {
          "type": "object",
          "properties": {
            "posture": {"type": "string", "enum": ["stand", "sit"]},
            "wait": {"type": "boolean"},
          },
        },
      }
    ]
  }
  app._set_input("call base.posture@robot-a/base-0 po")

  app.handle_key(ord("\t"), 24)
  assert app._input_buf == "call base.posture@robot-a/base-0 posture="
  app.handle_key(ord("\t"), 24)
  assert "Tab: posture=stand, posture=sit" in app._input_hint()


@pytest.mark.parametrize(
  ("text", "expected"),
  [
    (
      "action=open count=2 speed=0.5 enabled=true",
      {"action": "open", "count": 2, "speed": 0.5, "enabled": True},
    ),
    (
      'text="hello world" positions=[0,1,2]',
      {"text": "hello world", "positions": [0, 1, 2]},
    ),
    ('pose=\'{"x":1,"y":2}\' note=a=b', {"pose": {"x": 1, "y": 2}, "note": "a=b"}),
    ('{"action": "open", "count": 2}', {"action": "open", "count": 2}),
  ],
)
def test_parse_call_arguments_accepts_cli_and_json_forms(
  text: str, expected: dict
) -> None:
  assert parse_call_arguments(text) == expected


@pytest.mark.parametrize(
  ("text", "message"),
  [
    ("missing", "expected key=value"),
    ("=value", "expected key=value"),
    ("x=1 x=2", "duplicate argument"),
    ('text="unterminated', "invalid quoting"),
    ("{oops", "not valid JSON"),
    ("[1, 2]", "must be a JSON object"),
  ],
)
def test_parse_call_arguments_rejects_invalid_forms(text: str, message: str) -> None:
  with pytest.raises(ValueError, match=message):
    parse_call_arguments(text)


def test_multiple_tab_matches_remain_in_the_inline_hint() -> None:
  app = _app()
  for char in "focus ":
    app.handle_key(ord(char), 24)

  app.handle_key(ord("\t"), 24)

  assert app._input_buf == "focus "
  assert "Tab: context, missions" in app._input_hint()


def test_tab_from_blank_prompt_lists_commands() -> None:
  app = _app()

  app.handle_key(ord("\t"), 24)

  assert "Tab: backends, call, chat, exit" in app._input_hint()


def test_mission_parser_uses_shlex_without_changing_top_level_parser() -> None:
  from egia_ext.tui.render import parse_command

  assert parse_mission_command('add "inspect the loading bay"') == (
    "add",
    ["inspect the loading bay"],
  )
  assert parse_mission_command("edit abc 'check charger'") == (
    "edit",
    ["abc", "check charger"],
  )
  assert parse_command('chat "hello world"') == ("chat", "hello world")


def test_skill_start_dispatches_exact_identity_and_typed_arguments() -> None:
  app = _app()
  seen: list[dict] = []

  async def send(payload: dict) -> None:
    seen.append(payload)

  app.on_skill = send

  async def drive() -> None:
    app._run_command(
      "skill start scan.area@1.2.0 robot=robot-a timeout=12.5 rows=3 fast=true"
    )
    await asyncio.sleep(0)

  asyncio.run(drive())
  assert seen == [
    {
      "operation": "start",
      "robot_id": "robot-a",
      "name": "scan.area",
      "version": "1.2.0",
      "arguments": {"rows": 3, "fast": True},
      "timeout_s": 12.5,
    }
  ]
  assert app._focus == "skills"


def test_skill_show_watch_and_cancel_dispatch_current_interface() -> None:
  app = _app()
  commands: list[dict] = []
  watches: list[tuple[str, str]] = []

  async def send(payload: dict) -> None:
    commands.append(payload)

  async def watch(robot_id: str, execution_id: str) -> None:
    watches.append((robot_id, execution_id))

  app.on_skill = send
  app.on_skill_watch = watch

  async def drive() -> None:
    app._run_command("skill show exec-1 robot=robot-a")
    app._run_command("skill watch exec-1 robot=robot-a")
    app._run_command("skill cancel exec-1 robot=robot-a")
    await asyncio.sleep(0)

  asyncio.run(drive())
  assert commands == [
    {"operation": "get_execution", "robot_id": "robot-a", "execution_id": "exec-1"},
    {"operation": "cancel", "robot_id": "robot-a", "execution_id": "exec-1"},
  ]
  assert watches == [("robot-a", "exec-1")]


def test_skill_command_validates_robot_identity_and_timeout() -> None:
  app = _app()
  app.on_skill = MagicMock()
  app._run_command("skill start scan@1 timeout=2")
  app._run_command("skill start scan@1 robot=robot-a timeout=0")
  app._run_command("skill start scan robot=robot-a timeout=2")
  messages = [str(entry.event.get("event")) for entry in app._state.logs]
  assert any("robot=<alias> is required" in message for message in messages)
  assert any("timeout must be in (0, 3600]" in message for message in messages)
  assert any("expected exact <name>@<version>" in message for message in messages)


def test_skill_completion_uses_catalog_schema_and_execution_ids() -> None:
  app = _app()
  app._state.skills = {
    "skills": [
      {
        "name": "scan.area",
        "version": "1.2.0",
        "parameters_schema": {
          "type": "object",
          "required": ["mode"],
          "properties": {
            "mode": {"type": "string", "enum": ["safe", "fast"]},
            "rows": {"type": "integer"},
          },
        },
      }
    ]
  }
  app._state.skill_executions["exec-123"] = {"execution_id": "exec-123"}
  app._set_input("skill start scan")
  assert app._completion_candidates() == ["scan.area@1.2.0"]
  app._set_input("skill start scan.area@1.2.0 ")
  assert app._completion_candidates() == ["robot=", "timeout=", "mode=", "rows="]
  app._set_input("skill start scan.area@1.2.0 robot=robot-a timeout=5 mode=")
  assert app._completion_candidates() == ["mode=safe", "mode=fast"]
  app._set_input("skill watch ")
  assert app._completion_candidates() == ["exec-123"]


def test_mission_list_and_show_use_current_stream_state() -> None:
  mission_id = "12345678-1234-1234-1234-123456789abc"
  app = _app()
  app._state.missions = {"missions": [_mission(mission_id)], "history": []}

  app._run_command("mission list")
  assert app._focus == "missions"

  app._run_command("mission show 12345678")
  assert app._mission_detail is not None
  assert app._mission_detail["mission_id"] == mission_id


def test_mission_mutations_dispatch_full_id_and_current_revision() -> None:
  mission_id = "12345678-1234-1234-1234-123456789abc"
  app = _app()
  app._state.missions = {"missions": [_mission(mission_id, 7)], "history": []}
  seen: list[dict] = []

  async def send(payload: dict) -> None:
    seen.append(payload)

  app.on_mission = send

  async def drive() -> None:
    app._run_command('mission edit 12345678 "inspect bay two"')
    app._run_command("mission priority 12345678 urgent")
    app._run_command("mission cancel 12345678")
    app._run_command('mission add "bring the red cup"')
    await asyncio.sleep(0)

  asyncio.run(drive())
  assert seen == [
    {
      "operation": "update",
      "mission_id": mission_id,
      "expected_revision": 7,
      "order": "inspect bay two",
    },
    {
      "operation": "priority",
      "mission_id": mission_id,
      "expected_revision": 7,
      "priority_band": "urgent",
    },
    {
      "operation": "cancel",
      "mission_id": mission_id,
      "expected_revision": 7,
    },
    {"operation": "create", "order": "bring the red cup"},
  ]


def test_mission_prefix_errors_and_usage_are_actionable() -> None:
  app = _app()
  app._state.missions = {
    "missions": [
      _mission("12345678-1111-1111-1111-111111111111"),
      _mission("12345678-2222-2222-2222-222222222222"),
    ],
    "history": [],
  }

  app._run_command("mission cancel 12345678")
  app._run_command("mission show missing")
  app._run_command("mission priority 12345678-1 immediate")
  app._run_command('mission add "unterminated')

  messages = [str(entry.event.get("event")) for entry in app._state.logs]
  assert any("ambiguous ID prefix" in message for message in messages)
  assert any("no mission matches 'missing'" in message for message in messages)
  assert any("low|normal|high|urgent" in message for message in messages)
  assert any("invalid quoting" in message for message in messages)


def test_mission_completion_includes_subcommands_ids_and_priority_bands() -> None:
  app = _app()
  app._state.missions = {
    "missions": [_mission("12345678-1234-1234-1234-123456789abc")],
    "history": [],
  }

  app._set_input("mission sh")
  assert app._completion_candidates() == ["show"]
  app._set_input("mission show ")
  assert app._completion_candidates() == ["12345678"]
  app._set_input("mission priority 12345678 ")
  assert app._completion_candidates() == ["low", "normal", "high", "urgent"]


def test_command_line_colors_known_commands_and_shows_usage_hint() -> None:
  app = _app()
  app._set_input("focus")
  stdscr = MagicMock()
  with patch.dict(
    tui_app._STYLE_PAIRS,
    {"title": (2, 202), "accent": (3, 303), "dim": (4, 404)},
    clear=True,
  ):
    app._draw_input_line(stdscr, 23, 100)

  calls = stdscr.addnstr.call_args_list
  assert any(call.args[2] == "focus" and call.args[4] == 202 for call in calls)
  assert any("focus <pane>" in call.args[2] and call.args[4] == 404 for call in calls)


@pytest.mark.parametrize(
  ("digit", "pane"),
  [
    (1, "context"),
    (2, "missions"),
    (3, "primitives"),
    (4, "skills"),
    (5, "modelio"),
    (6, "logs"),
    (7, "status"),
  ],
)
def test_alt_digit_focuses_its_pane(digit: int, pane: str) -> None:
  app = _app()

  assert app.handle_alt_digit(digit)
  assert app._focus == pane
  assert app.consume_focus_refresh()


def test_alt_digit_closes_an_open_overlay() -> None:
  app = _app()
  app._help_open = True

  assert app.handle_alt_digit(6)
  assert app._focus == "logs"
  assert not app._help_open


def test_plain_digit_stays_in_command_input() -> None:
  app = _app()
  app.handle_key(ord("1"), 24)

  assert app._input_buf == "1"


def test_focus_command_rejects_unknown_pane() -> None:
  app = _app()
  app._run_command("focus nowhere")
  assert app._focus == "context"
  assert any(
    entry.event.get("event") == "unknown pane: nowhere" for entry in app._state.logs
  )


def test_typed_command_buffer_and_enter() -> None:
  app = _app()
  for ch in "quit":
    app.handle_key(ord(ch), 24)
  assert app._input_buf == "quit"
  app.handle_key(ord("\r"), 24)
  assert app._input_buf == ""
  assert app.quit_event.is_set()


def test_command_cursor_edits_without_moving_the_window() -> None:
  app = _app()
  for char in "chat hi":
    app.handle_key(ord(char), 24)
  app.handle_key(curses.KEY_LEFT, 24)
  app.handle_key(curses.KEY_LEFT, 24)
  app.handle_key(ord("e"), 24)
  app.handle_key(curses.KEY_DC, 24)

  assert app._input_buf == "chat ei"
  assert app._scroll["context"] == 0


def test_blank_prompt_arrows_control_window_and_ctrl_p_reads_history() -> None:
  app = _app()
  app._record_history("chat previous")
  app.handle_key(curses.KEY_UP, 10, line_count=8)

  assert app._scroll["context"] == 1
  assert app._input_buf == ""
  app.handle_key(16, 10)  # Ctrl-P
  assert app._input_buf == "chat previous"


def test_typed_prompt_arrows_control_history() -> None:
  app = _app()
  app._record_history("chat previous")
  app.handle_key(ord("c"), 24)
  app.handle_key(curses.KEY_UP, 24)

  assert app._input_buf == "chat previous"


def test_ctrl_r_reverse_searches_session_history() -> None:
  app = _app()
  app._record_history("chat kitchen")
  app._record_history("chat bedroom")
  app._set_input("kitchen")

  app.handle_key(18, 24)  # Ctrl-R

  assert app._input_buf == "chat kitchen"


def test_scroll_stops_at_oldest_line() -> None:
  """Paging above the first line keeps the oldest page visible."""
  app = _app()
  for _ in range(10):
    app.handle_key(curses.KEY_UP, 10, line_count=8)
  assert app._scroll["context"] == 4
  app.handle_key(curses.KEY_PPAGE, 10, line_count=8)
  assert app._scroll["context"] == 4


def test_function_keys_page_when_terminal_intercepts_page_keys() -> None:
  app = _app()

  app.handle_key(curses.KEY_F7, 10, line_count=20)
  assert app._scroll["context"] == 6

  app.handle_key(curses.KEY_F8, 10, line_count=20)
  assert app._scroll["context"] == 0


def test_refresh_on_resize_resizeterm_and_erase() -> None:
  stdscr = MagicMock()
  stdscr.getmaxyx.return_value = (24, 80)
  with (
    patch("egia_ext.tui.app.curses.update_lines_cols"),
    patch("egia_ext.tui.app.curses.resizeterm") as resizeterm,
  ):
    assert _refresh_on_resize(stdscr) == (24, 80)
  resizeterm.assert_called_once_with(24, 80)
  stdscr.erase.assert_called_once()


def test_sync_terminal_size_calls_resizeterm_on_change() -> None:
  stdscr = MagicMock()
  # sync reads size once; refresh_on_resize reads it again
  stdscr.getmaxyx.side_effect = [(24, 80), (30, 100), (30, 100)]
  with (
    patch("egia_ext.tui.app.curses.update_lines_cols"),
    patch("egia_ext.tui.app.curses.resizeterm") as resizeterm,
  ):
    first = _sync_terminal_size(stdscr, None)
    second = _sync_terminal_size(stdscr, first)
  assert first == (24, 80)
  assert second == (30, 100)
  resizeterm.assert_called_once_with(30, 100)
  stdscr.erase.assert_called_once()


def test_sync_terminal_size_force_uses_same_erase_refresh() -> None:
  """Ready refresh must use the same resizeterm+erase path as a size change."""
  stdscr = MagicMock()
  stdscr.getmaxyx.return_value = (24, 80)
  with (
    patch("egia_ext.tui.app.curses.update_lines_cols"),
    patch("egia_ext.tui.app.curses.resizeterm") as resizeterm,
  ):
    size = _sync_terminal_size(stdscr, (24, 80), force=True)
  assert size == (24, 80)
  resizeterm.assert_called_once_with(24, 80)
  stdscr.erase.assert_called_once()


def test_consume_ready_refresh_fires_once_per_ready_edge() -> None:
  app = _app()
  assert not app.consume_ready_refresh()
  app._state.status = {"status": "starting"}
  assert not app.consume_ready_refresh()
  app._state.status = {"status": "ready"}
  assert app.consume_ready_refresh()
  assert not app.consume_ready_refresh()
  app._state.status = {"status": "degraded"}
  assert not app.consume_ready_refresh()
  app._state.status = {"status": "ready"}
  assert app.consume_ready_refresh()


def test_call_command_opens_guide_without_json() -> None:
  app = _app()
  app._run_command("call base.velocity")
  assert app._guide_lines is not None
  joined = "\n".join(text for text, _ in app._guide_lines)
  assert "base.velocity" in joined


def test_call_command_bare_lists_primitives() -> None:
  app = _app()
  app._run_command("call")
  assert app._guide_lines is not None


def test_live_primitive_catalog_keeps_eos_metadata() -> None:
  app = _app()
  app._state.primitives = {
    "primitives": [
      {
        "name": "arm.movej",
        "version": "1.0.0",
        "robot_id": "robot-a",
        "device_id": "arm-0",
        "description": "Move every arm joint",
        "device_description": "six-axis arm",
        "operator": True,
        "limits": {"speed_rad_s": {"minimum": 0.1, "maximum": 1.0}},
        "arguments_schema": {"type": "object", "properties": {}},
      }
    ]
  }

  _, primitives = app._parsed_primitives()

  assert primitives == [
    {
      "name": "arm.movej",
      "version": "1.0.0",
      "robot_id": "robot-a",
      "device_id": "arm-0",
      "description": "Move every arm joint",
      "device_description": "six-axis arm",
      "operator": True,
      "limits": {"speed_rad_s": {"minimum": 0.1, "maximum": 1.0}},
      "schema": {"type": "object", "properties": {}},
    }
  ]


def test_call_command_dispatches_parsed_json() -> None:
  app = _app()
  seen: list[tuple[str, dict, str, str | None]] = []

  async def fake(
    primitive: str, arguments: dict, robot_id: str, device_id: str | None
  ) -> None:
    seen.append((primitive, arguments, robot_id, device_id))

  app.on_call = fake

  async def drive() -> None:
    app._run_command('call base.velocity@sim/base-0 {"x": 2}')
    await asyncio.sleep(0)  # let the scheduled callbacks run

  asyncio.run(drive())
  assert seen == [
    ("base.velocity", {"x": 2}, "sim", "base-0"),
  ]


def test_call_command_dispatches_key_value_arguments() -> None:
  app = _app()
  seen: list[tuple[str, dict, str, str | None]] = []

  async def fake(
    primitive: str, arguments: dict, robot_id: str, device_id: str | None
  ) -> None:
    seen.append((primitive, arguments, robot_id, device_id))

  app.on_call = fake

  async def drive() -> None:
    app._run_command(
      'call speaker.play@sim/speaker-0 text="hello world" volume=0.5 loop=false'
    )
    await asyncio.sleep(0)

  asyncio.run(drive())
  assert seen == [
    (
      "speaker.play",
      {"text": "hello world", "volume": 0.5, "loop": False},
      "sim",
      "speaker-0",
    )
  ]


def test_call_command_rejects_bad_json() -> None:
  app = _app()
  app._run_command("call base.velocity@sim/base-0 {oops")
  assert any(
    "not valid JSON" in str(entry.event.get("event")) for entry in app._state.logs
  )


def test_guide_closes_on_any_key() -> None:
  app = _app()
  app._run_command("call base.velocity")
  assert app._guide_lines is not None
  app.handle_key(ord("q"), 24)
  assert app._guide_lines is None


def test_focus_change_requests_erase_refresh_once() -> None:
  app = _app()
  assert not app.consume_focus_refresh()
  app._run_command("focus logs")
  assert app.consume_focus_refresh() is True
  assert not app.consume_focus_refresh()  # consumed exactly once


def test_blit_paints_at_given_column_not_screen_origin() -> None:
  """Regression: segment painting used the running offset as the column,
  so every pane painted from screen column 0 — the split pane's right
  column erased its own area then drew over the left column."""
  app = _app()
  stdscr = MagicMock()
  calls: list[tuple[int, int, str]] = []

  def record(y, x, text, n, attr=0):
    calls.append((y, x, text))

  stdscr.addnstr.side_effect = record
  app._blit(stdscr, 5, 40, 3, 20, [[("hello ", "plain"), ("world", "accent")]], 0)
  texts = [(y, x) for y, x, t in calls if t.strip()]
  assert (5, 40) in texts and (5, 46) in texts  # painted at x=40, not 0
  assert all(x >= 40 for _, x in texts)


def test_blit_shows_triangles_for_available_scroll() -> None:
  app = _app()
  stdscr = MagicMock()
  calls: list[tuple[int, int, str]] = []

  def record(y, x, text, n, attr=0):
    calls.append((y, x, text))

  stdscr.addnstr.side_effect = record
  lines = [(str(i), "plain") for i in range(5)]
  app._blit(stdscr, 3, 10, 3, 20, lines, 1)

  assert (3, 29, "▲") in calls
  assert (5, 29, "▼") in calls


def test_curses_interprets_escape_prefix_alt_digit(
  monkeypatch: pytest.MonkeyPatch,
) -> None:
  app = _app()
  screen = MagicMock()
  screen.getmaxyx.return_value = (24, 80)
  screen.getch.side_effect = [27, ord("3"), -1]
  original = app.handle_alt_digit

  def focus(digit: int) -> bool:
    focused = original(digit)
    app.quit_event.set()
    return focused

  monkeypatch.setattr(app, "handle_alt_digit", focus)
  monkeypatch.setattr(app, "draw", MagicMock())
  with (
    patch("curses.initscr", return_value=screen),
    patch("curses.noecho"),
    patch("curses.cbreak"),
    patch("curses.curs_set"),
    patch("curses.nocbreak"),
    patch("curses.echo"),
    patch("curses.endwin"),
    patch("curses.has_colors", return_value=False),
    patch("curses.update_lines_cols"),
    patch("curses.resizeterm"),
  ):
    asyncio.run(run_curses(app))

  assert app._focus == "primitives"


def test_curses_interprets_8_bit_meta_digit(
  monkeypatch: pytest.MonkeyPatch,
) -> None:
  app = _app()
  screen = MagicMock()
  screen.getmaxyx.return_value = (24, 80)
  screen.getch.side_effect = [0xB3, -1]
  original = app.handle_alt_digit

  def focus(digit: int) -> bool:
    focused = original(digit)
    app.quit_event.set()
    return focused

  monkeypatch.setattr(app, "handle_alt_digit", focus)
  monkeypatch.setattr(app, "draw", MagicMock())
  with (
    patch("curses.initscr", return_value=screen),
    patch("curses.noecho"),
    patch("curses.cbreak"),
    patch("curses.curs_set"),
    patch("curses.nocbreak"),
    patch("curses.echo"),
    patch("curses.endwin"),
    patch("curses.has_colors", return_value=False),
    patch("curses.update_lines_cols"),
    patch("curses.resizeterm"),
  ):
    asyncio.run(run_curses(app))

  assert app._focus == "primitives"
