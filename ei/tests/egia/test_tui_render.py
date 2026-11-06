"""Pure rendering and parsing logic for libexec/egia_ext/tui — no curses."""

from __future__ import annotations

import json
from datetime import UTC, datetime
from itertools import pairwise

from egia_ext.tui.config import level_value, load_tui_config
from egia_ext.tui.render import (
  append_model_reply,
  backend_summary,
  brief_context_lines,
  brief_model_io_line,
  brief_status_line,
  format_backend_hardware_lines,
  format_context_lines,
  format_log_event,
  format_log_event_lines,
  format_mission_detail,
  format_missions_lines,
  format_model_io_lines,
  format_primitives_prompt_lines,
  format_process_lines,
  format_status_lines,
  parse_console_line,
  parse_jsonl_event,
  wrap_lines,
)
from island.path import Layout


def flatten(lines) -> list[str]:
  """Plain text of mixed lines — single-style tuples or segment lists."""
  out = []
  for line in lines:
    segs = line if isinstance(line, list) else [line]
    out.append("".join(text for text, _ in segs))
  return out


def styles_for(lines, text: str) -> list[str]:
  """Styles used by an exact text fragment across segmented render lines."""
  return [
    style
    for line in lines
    for chunk, style in (line if isinstance(line, list) else [line])
    if chunk.strip() == text
  ]


HEALTH_PAYLOAD = {
  "status": "ready",
  "version": "1.0.0",
  "started_at": "2026-08-14T12:00:00Z",
  "uptime_seconds": 3661.0,
  "correlation_id": None,
  "subsystems": [
    {
      "name": "perception",
      "state": "ready",
      "uptime_seconds": 3650.0,
      "restarts": 0,
      "last_error": None,
    },
    {
      "name": "context",
      "state": "degraded",
      "uptime_seconds": 12.0,
      "restarts": 2,
      "last_error": "boom",
    },
  ],
}

SNAPSHOT_PAYLOAD = {
  "version": "1",
  "prompt": "You are a robot assistant observing the world.\nYou hear: hi",
  "sections": [
    {
      "kind": "hearing",
      "rendered": "You hear: hi",
      "has_data": True,
      "image_path": None,
    },
    {
      "kind": "environment",
      "rendered": "Environment: unavailable",
      "has_data": False,
      "image_path": None,
    },
  ],
  "built_at": datetime(2026, 8, 14, 12, 34, 56, tzinfo=UTC).isoformat(),
  "observation_ids": ["obs-1"],
}


def test_status_lines_show_overall_and_subsystems() -> None:
  lines = format_status_lines(HEALTH_PAYLOAD, 40)
  text = [t for t, _ in lines]
  assert any("pipeline ready" in t for t in text)
  assert any("perception" in t and "ready" in t for t in text)
  degraded = [style for t, style in lines if "context" in t]
  assert degraded == ["bad"]
  assert any("boom" in t for t in text)


def test_status_lines_disconnected_placeholder() -> None:
  lines = format_status_lines(None, 40)
  assert any(style == "dim" for _, style in lines)


def test_status_lines_render_gate_safety() -> None:
  # Counters render dim while the estop latch is open…
  calm = dict(
    HEALTH_PAYLOAD, safety={"admitted": 12, "rejected": 3, "estop_engaged": False}
  )
  lines = format_status_lines(calm, 40)
  assert any(
    "gate 12 admitted · 3 rejected · estop off" == t and s == "dim" for t, s in lines
  )
  # …and an engaged estop leads with its own red line.
  halted = dict(
    HEALTH_PAYLOAD, safety={"admitted": 12, "rejected": 4, "estop_engaged": True}
  )
  lines = format_status_lines(halted, 40)
  assert any("estop engaged" in t and s == "bad" for t, s in lines)
  assert any("gate 12 admitted · 4 rejected" == t and s == "dim" for t, s in lines)
  # No gate wired (safety null) — no safety lines at all.
  lines = format_status_lines(HEALTH_PAYLOAD, 40)
  assert not any("gate" in t or "estop" in t for t, _ in lines)


def test_context_lines_mark_empty_sections() -> None:
  lines = format_context_lines(SNAPSHOT_PAYLOAD, 60)
  styles = {
    t: s for line in lines for t, s in (line if isinstance(line, list) else [line])
  }
  assert styles["● [hearing]"] == "good"
  assert styles["○ [environment]"] == "warn"
  assert any(
    "You hear:" in t and s == "title"
    for line in lines
    for t, s in (line if isinstance(line, list) else [line])
  )
  assert styles_for(lines, "Environment:") == ["title"]
  assert styles_for(lines, "unavailable") == ["dim"]


def test_context_hardware_is_one_line_per_device_with_inline_color() -> None:
  snapshot = dict(
    SNAPSHOT_PAYLOAD,
    sections=[
      {
        "kind": "hardware",
        "rendered": "Hardware: rover-a: arm: manipulator; camera: front vision",
        "has_data": True,
      },
      {
        "kind": "perception",
        "rendered": "14:32:07 [detection] crate pickable 1.2m",
        "has_data": True,
      },
    ],
  )
  lines = format_context_lines(snapshot, 80)
  text = flatten(lines)
  assert "  rover-a · arm: manipulator" in text
  assert "  rover-a · camera: front vision" in text
  assert all(";" not in line for line in text)
  assert styles_for(lines, "[detection]") == ["accent"]


def test_context_lines_placeholder_without_snapshot() -> None:
  lines = format_context_lines(None, 60)
  assert any(style == "dim" for _, style in lines)


def test_wrap_lines_preserves_empty_lines() -> None:
  assert wrap_lines("a b c", 3) == ["a b", "c"]
  assert wrap_lines("a\n\nb", 10) == ["a", "", "b"]


def test_wrap_lines_hard_breaks_long_tokens() -> None:
  from egia_ext.tui.render import wrap_indented

  assert wrap_lines("abcdefghij", 4) == ["abcd", "efgh", "ij"]
  indented = wrap_indented("abcdefghij", 6, indent="  ")
  assert indented == ["  abcd", "  efgh", "  ij"]
  assert all(len(line) <= 6 for line in indented)


def test_parse_jsonl_event_and_threshold() -> None:
  event = parse_jsonl_event(
    json.dumps(
      {
        "level": "warning",
        "logger": "egia.core",
        "timestamp": "2026-08-14T12:00:00Z",
        "event": "queue_full",
      }
    )
  )
  assert event is not None and event["level"] == "warning"
  assert parse_jsonl_event("not json") is None
  assert parse_jsonl_event(json.dumps({"x": 1})) is None


def test_format_log_event_colors_and_clips() -> None:
  event = {
    "level": "error",
    "logger": "egia.core.context",
    "timestamp": "2026-08-14T12:34:56.1Z",
    "event": "boom",
    "detail": "x" * 80,
  }
  text, style = format_log_event(event, 40)
  assert style == "plain"
  assert text.startswith("12:34:56 ERROR")
  assert len(text) <= 40
  assert "boom" in text


def test_log_event_lines_color_only_the_severity_label() -> None:
  event = {
    "level": "error",
    "logger": "egia.core.context",
    "timestamp": "2026-08-14T12:34:56Z",
    "event": "boom",
  }

  lines = format_log_event_lines(event, 80)
  segments = [
    segment
    for line in lines
    for segment in (line if isinstance(line, list) else [line])
  ]

  assert ("ERROR", "bad") in segments
  assert all(style == "plain" for text, style in segments if text.strip() != "ERROR")


def test_format_log_event_lines_wraps_instead_of_clipping() -> None:
  event = {
    "level": "info",
    "logger": "egia.core.context",
    "timestamp": "2026-08-14T12:34:56Z",
    "event": "context_built",
    "detail": "y" * 80,
  }
  lines = format_log_event_lines(event, 40)
  text = flatten(lines)
  assert len(lines) > 1
  assert all(len(line) <= 40 for line in text)
  assert "".join(text).count("y") == 80


def test_format_primitives_prompt_lines_renders_prompt_without_parsing_catalog() -> (
  None
):
  prompts = {
    "system": (
      "You have primitives.\n"
      "- move (v1): move the robot\n"
      '  arguments schema: {"type": "object", "required": ["x"], '
      '"properties": {"x": {"type": "number", "description": "meters east"}}}'
    )
  }
  lines = format_primitives_prompt_lines(prompts=prompts, width=80)
  text = "\n".join(flatten(lines))
  assert "move (v1)" in text
  assert "move the robot" in text
  assert "meters east" in text
  assert "{" in text


def test_format_log_event_repeat_marker() -> None:
  event = {
    "level": "warning",
    "logger": "egia.core",
    "timestamp": "2026-08-14T12:34:56Z",
    "event": "queue_full",
  }
  text, _ = format_log_event(event, 60, count=3)
  assert "(x3)" in text
  assert "queue_full" in text
  assert len(format_log_event(event, 40, count=12)[0]) <= 40


def test_level_value_ordering() -> None:
  assert level_value("WARNING") > level_value("INFO")
  assert level_value("nonsense") == level_value("WARNING")


def test_load_tui_config_defaults_and_level_override(tmp_path) -> None:
  cfg = load_tui_config(etc=tmp_path, var=tmp_path / "var")
  assert cfg.host == "127.0.0.1" and cfg.port == 16060
  assert cfg.log_level == "INFO"  # no toml at all → operator default
  assert cfg.state_dir == tmp_path / "var/lib/egia"
  assert cfg.replay_path == tmp_path / "var/lib/egia/egia.jsonl"
  assert cfg.cache_dir == tmp_path / "var/cache/egia"
  assert cfg.log_dir == tmp_path / "var/log/egia"
  assert cfg.run_dir == tmp_path / "var/run"
  assert cfg.tmp_dir == tmp_path / "var/tmp"
  assert cfg.sandbox_enabled is False
  assert cfg.sandbox.websocket.port is None

  cfg = load_tui_config(etc=tmp_path, var=tmp_path / "var", log_level="ERROR")
  assert cfg.log_level == "ERROR"

  # The pane threshold mirrors [logging.console] level from egia.toml.
  etc = tmp_path / "etc"
  etc.mkdir()
  (etc / "egia.toml").write_text(
    '[server]\nport = 18030\n[logging.console]\nlevel = "DEBUG"\n'
  )
  cfg = load_tui_config(etc=etc, var=tmp_path / "var")
  assert cfg.log_level == "DEBUG"
  cfg = load_tui_config(etc=etc, var=tmp_path / "var", log_level="ERROR")
  assert cfg.log_level == "ERROR"  # --log-level overrides the console sink

  (tmp_path / "egia.toml").write_text(
    '[server]\nhost = "0.0.0.0"\nport = 9999\n'
    '[logging]\nlevel = "INFO"\n[logging.console]\nlevel = "ERROR"\n'
  )
  cfg = load_tui_config(etc=tmp_path, var=tmp_path / "var")
  assert (cfg.host, cfg.port) == ("0.0.0.0", 9999)


def test_tui_config_layers_partial_overlay_over_factory_defaults(tmp_path) -> None:
  (tmp_path / "egia.toml").write_text(
    "[server]\nport = 19020\n[logging.console]\nenabled = false\n"
  )
  defaults = Layout.resolve(environ={}).share / "defaults/egia"

  cfg = load_tui_config(etc=tmp_path, defaults=defaults)

  assert cfg.host == "127.0.0.1"
  assert cfg.port == 19020
  assert cfg.log_level == "WARNING"
  assert cfg.sandbox_enabled is False
  assert cfg.sandbox.gateway.grpc_port == 16022


def test_tui_config_reads_sandbox_media_endpoint(tmp_path) -> None:
  (tmp_path / "egia.toml").write_text("[launch]\nsandbox = true\n")
  (tmp_path / "opt").mkdir()
  (tmp_path / "opt/sandbox.toml").write_text(
    '[websocket]\nhost = "localhost"\nport = 19031\n'
  )

  cfg = load_tui_config(etc=tmp_path)

  assert cfg.sandbox_enabled is True
  assert cfg.sandbox.websocket.host == "localhost"
  assert cfg.sandbox.websocket.port == 19031


def test_parse_console_line_structured_and_raw() -> None:
  event = parse_console_line(
    "[aether-egia] WARNING @ egia.core [2026-08-14T12:34:56Z]: queue_full kind=hearing"
  )
  assert event is not None
  assert event["level"] == "warning"
  assert event["logger"] == "egia.core"
  assert event["event"].startswith("queue_full")
  assert parse_console_line("Traceback (most recent call last):") is None


def test_context_sections_separated_by_blank_lines() -> None:
  lines = format_context_lines(SNAPSHOT_PAYLOAD, 60)
  text = flatten(lines)
  kinds = [i for i, line in enumerate(text) if line.startswith(("●", "○"))]
  # A blank line sits between each section block boundary.
  for a, b in pairwise(kinds):
    between = text[a:b]
    assert "" in between


def test_context_empty_sections_do_not_offer_removed_runtime_routes() -> None:
  lines = format_context_lines(SNAPSHOT_PAYLOAD, 60)
  assert not any("map" in text.lower() for text in flatten(lines))
  assert not any("context.read" in text for text in flatten(lines))


def test_context_lines_mark_omitted_sections() -> None:
  snapshot = {
    "version": "1",
    "prompt": "",
    "built_at": "2026-08-22T10:00:00Z",
    "sections": [
      {
        "kind": "perception",
        "rendered": "[perception omitted: token budget]",
        "has_data": False,
        "omitted": True,
      },
    ],
  }
  lines = format_context_lines(snapshot, 60)
  styles = {
    t: s for line in lines for t, s in (line if isinstance(line, list) else [line])
  }
  assert styles["◌ [perception]"] == "warn"
  assert styles_for(lines, "[perception omitted: token budget]") == ["accent"]
  # A budget drop is not an empty source — no feed hint is offered.
  assert not any("→" in t for t in flatten(lines))


def test_append_model_reply_section() -> None:
  base = format_context_lines(SNAPSHOT_PAYLOAD, 60)
  none = append_model_reply(base, None, 60)
  assert any("no model reply yet" in t for t in flatten(none))
  exchange = {"reply": '{"action": "base.velocity", "arguments": {"distance": 0.5}}'}
  with_reply = append_model_reply(base, exchange, 60)
  assert any(
    "model reply" in t and s == "good"
    for line in with_reply
    for t, s in (line if isinstance(line, list) else [line])
  )
  assert any("primitive calls: base.velocity(" in t for t in flatten(with_reply))


def test_append_model_reply_semantic_variants() -> None:
  base = format_context_lines(SNAPSHOT_PAYLOAD, 60)

  def rendered(reply: str) -> list[str]:
    exchange = {
      "reply": reply,
      "mission_id": "90b9ce24-aaaa",
      "step": 3,
      "model": "kimi-k2.6",
      "duration_s": 1.25,
    }
    return flatten(append_model_reply(base, exchange, 60))

  lines = rendered('{"final": "stood up"}')
  assert any("final: stood up" in t for t in lines)
  assert any("mission 90b9ce24 step 3" in t and "kimi-k2.6" in t for t in lines)

  lines = rendered(
    '{"actions": [{"action": "gimbal.motion", "arguments": {"angle": 90}},'
    ' {"action": "base.velocity", "arguments": {"distance": 0.5}}]}'
  )
  assert any("primitive calls:" in t for t in lines)
  assert any("gimbal.motion(" in t for t in lines)
  assert any("base.velocity(" in t for t in lines)

  # The v2 reply vocabulary (action/arguments) renders identically.
  lines = rendered(
    '{"actions": [{"action": "gimbal.motion", "arguments": {"angle": 90}},'
    ' {"action": "base.velocity", "arguments": {"distance": 0.5}}]}'
  )
  assert any("primitive calls: gimbal.motion(" in t for t in lines)
  assert any("base.velocity(" in t for t in lines)
  assert not any('"action"' in t for t in lines)

  lines = rendered('{"action": "gripper", "arguments": {"state": "open"}}')
  assert any("primitive calls: gripper(" in t for t in lines)

  lines = rendered('{"missions": []}')
  assert any("proposer: no missions proposed" in t for t in lines)

  lines = rendered('{"missions": [{"order": "check the noise", "urgency": 0.9}]}')
  assert any('"check the noise"' in t and "urgency 0.9" in t for t in lines)

  lines = rendered("just prose, no json")
  assert any("just prose" in t for t in lines)

  lines = rendered(
    'Reply: path is clear\n<JSON>\n{"action": "base.velocity", '
    '"arguments": {"distance": 0.5}}'
  )
  reply_index = next(
    i for i, line in enumerate(lines) if "Reply: path is clear" in line
  )
  call_index = next(
    i for i, line in enumerate(lines) if "primitive calls: base.velocity(" in line
  )
  assert reply_index < call_index
  assert not any('"action"' in line for line in lines)


def test_brief_lines() -> None:
  text, _ = brief_context_lines(SNAPSHOT_PAYLOAD)
  assert "1/2" in text  # hearing has data, environment does not
  text, _ = brief_context_lines(None)
  assert text == "no snapshot yet"
  text, _ = brief_model_io_line(None, 0)
  assert "reqs 0" in text
  exchange = {"step": 2, "reply": '{"intent": "drive"}'}
  text, _ = brief_model_io_line(exchange, 7)
  assert "reqs 7" in text and "drive" in text
  text, style = brief_status_line(HEALTH_PAYLOAD, {"connection": "up"})
  assert "ready" in text and "up" in text and style == "good"


def test_backend_summary_frame_shapes() -> None:
  # One wired backend collapses to the classic single word.
  single = {"backends": [{"backend": "default", "connection": "up"}]}
  assert backend_summary(single) == ("backend up", "good")
  # A fleet renders one id:state per endpoint; style is the worst state.
  fleet = {
    "backends": [
      {"backend": "b1", "connection": "up"},
      {"backend": "b2", "connection": "down"},
    ]
  }
  text, style = backend_summary(fleet)
  assert "b1:up" in text and "b2:down" in text and style == "bad"
  # unknown (no probe yet) ranks below up but above degraded.
  assert backend_summary(
    {"backends": [{"backend": "b1", "connection": "unknown"}]}
  ) == ("backend unknown", "dim")
  # Legacy single-backend frame (top-level connection) still parses.
  assert backend_summary({"connection": "degraded"}) == ("backend degraded", "warn")
  # No frame yet — the feed's connect marker stands in.
  assert backend_summary(None, link="down") == ("backend down", "dim")
  assert backend_summary({}) == ("backend …", "dim")


def test_sandbox_hardware_status_keeps_unavailable_media_visible() -> None:
  lines = format_backend_hardware_lines(
    {
      "backends": [
        {
          "backend": "sandbox",
          "hardware_vendor": "Island",
          "hardware_model": "local-system-devices",
          "devices": [
            {
              "device_id": "mic-0",
              "device_class": "mic",
              "backend_name": "system",
              "backend_version": "1.0",
              "alive": True,
              "admitted": True,
            }
          ],
        }
      ]
    },
    100,
  )
  rendered = [text for text, _ in lines]
  assert "sandbox · Island / local-system-devices" in rendered
  assert "  camera: unavailable" in rendered
  assert "  mic: mic-0 · ready · system 1.0" in rendered
  assert "  speaker: unavailable" in rendered


def test_format_model_io_lines() -> None:
  lines = format_model_io_lines(None, 60)
  assert any("no model requests" in t for t, _ in lines)
  exchange = {
    "exchange_id": 1,
    "step": 1,
    "model": "kimi",
    "messages": [{"role": "system", "content": "You are EGIA."}],
    "reply": '{"intent": "drive"}',
    "prompt_tokens": 100,
    "completion_tokens": 8,
    "duration_s": 1.5,
  }
  lines = format_model_io_lines(exchange, 60)
  texts = [t for t, _ in lines]
  assert any("exchange #1" in t for t in texts)
  assert any("▶ system" in t for t in texts)
  assert any("◆ answer" in t for t in texts)


def test_format_model_io_lines_shows_live_reasoning_and_status() -> None:
  exchange = {
    "exchange_id": 2,
    "step": 1,
    "status": "streaming",
    "messages": [],
    "thinking": "Checking the available route",
    "reply": "",
  }
  lines = format_model_io_lines(exchange, 60)
  texts = [text for text, _style in lines]
  assert "● streaming" in texts
  assert "◇ provider reasoning" in texts
  assert any("Checking the available route" in text for text in texts)
  assert any("waiting for answer tokens" in text for text in texts)


def test_format_model_io_lines_shows_all_agents_and_primitive_activity() -> None:
  sentinel = {
    "exchange_id": 8,
    "agent_id": "sentinel",
    "status": "streaming",
    "messages": [],
    "reply": "",
  }
  agents = [
    {
      "agent_id": "planner",
      "thinking": False,
      "status": "working",
      "exchange": {"exchange_id": 3, "model": "kimi"},
      "activity": {
        "name": "base.velocity",
        "arguments": {"linear_x_m_s": 0.1},
        "status": "running",
      },
    },
    {
      "agent_id": "sentinel",
      "thinking": True,
      "status": "thinking",
      "exchange": sentinel,
      "activity": None,
    },
  ]

  texts = [text for text, _style in format_model_io_lines(sentinel, 80, agents=agents)]

  assert "model agents (2)" in texts
  assert any("planner · working" in text for text in texts)
  assert any("sentinel · thinking" in text for text in texts)
  assert any("base.velocity" in text and "linear_x_m_s" in text for text in texts)
  assert "latest exchange" in texts


def test_format_process_lines() -> None:
  lines = format_process_lines([])
  assert lines == []
  lines = format_process_lines(
    [{"name": "python (pipeline)", "cpu_pct": 12.5, "rss_mb": 180.2}]
  )
  assert any("python (pipeline)" in t for t, _ in lines)


def test_format_primitives_and_skills_lines() -> None:
  from egia_ext.tui.render import format_primitives_lines, format_skills_lines

  lines = format_primitives_lines(None, 60)
  assert any("disconnected" in t for t, _ in lines)
  catalog = {
    "count": 2,
    "primitives": [
      {
        "name": "base.velocity",
        "description": "Move base",
        "device_description": "warehouse tug",
        "operator": True,
        "limits": {
          "linear_x_m_s": {"minimum": -1.0, "maximum": 1.0},
        },
        "robot_id": "robot-a",
        "device_id": "base-0",
      },
      {
        "name": "gimbal.motion",
        "description": "Point camera",
        "robot_id": "robot-a",
        "device_id": "gimbal-0",
      },
    ],
  }
  lines = format_primitives_lines(catalog, 60)
  assert any("primitives (2)" in t for t, _ in lines)
  assert any("base.velocity@robot-a/base-0 [operator]" in t for t, _ in lines)
  assert any("device: warehouse tug" in t for t, _ in lines)
  skills = format_skills_lines(
    {
      "available": True,
      "count": 1,
      "skills": [
        {
          "name": "scan",
          "version": "1.0.0",
          "backend": "robot-a",
          "primitives": ["gimbal.motion"],
        }
      ],
    },
    60,
  )
  assert any("skills (1)" in t for t, _ in skills)
  assert any("composes: gimbal.motion" in t for t, _ in skills)


def test_format_skill_pane_shows_schema_and_execution_details() -> None:
  from egia_ext.tui.render import format_skill_pane_lines

  lines = format_skill_pane_lines(
    {
      "available": True,
      "skills": [
        {
          "name": "scan",
          "version": "1.0.0",
          "backend": "robot-a",
          "description": "Scan the work area",
          "parameters_schema": {
            "type": "object",
            "required": ["rows"],
            "properties": {"rows": {"type": "integer", "description": "Rows"}},
          },
          "returns_schema": {"type": "object"},
        }
      ],
    },
    {
      "exec-1": {
        "execution_id": "exec-1",
        "request_id": "request-1",
        "robot_id": "robot-a",
        "name": "scan",
        "version": "1.0.0",
        "status": "succeeded",
        "sequence": 3,
        "cancellation_requested": False,
        "result": {"found": 2},
      }
    },
    80,
  )

  text = "\n".join(flatten(lines))
  assert "scan@1.0.0  robot=robot-a" in text
  assert "rows" in text and "required" in text
  assert "exec-1" in text and "request=request-1" in text
  assert 'result: {"found": 2}' in text


def test_format_primitive_semantic_keeps_eos_device_metadata() -> None:
  from egia_ext.tui.render import format_primitive_semantic

  lines = format_primitive_semantic(
    {
      "name": "arm.movej",
      "version": "1.0",
      "robot_id": "robot-a",
      "device_id": "arm-0",
      "description": "Move every arm joint",
      "device_description": "six-axis arm beside the conveyor",
      "operator": True,
      "limits": {
        "speed_rad_s": {"minimum": 0.1, "maximum": 1.0},
      },
      "schema": {"type": "object", "properties": {}},
    },
    80,
  )

  text = "\n".join(flatten(lines))
  assert "device: six-axis arm beside the conveyor" in text
  assert "operator-only primitive" in text
  assert "speed_rad_s: [0.1, 1.0]" in text


def test_format_primitives_prompt_lines() -> None:
  from egia_ext.tui.render import (
    append_primitives_prompt,
    format_primitives_prompt_lines,
  )

  lines = format_primitives_prompt_lines(None, 60)
  assert any("no system prompt yet" in t for t, _ in lines)
  long = "x" * 80
  exchange = {
    "messages": [
      {
        "role": "system",
        "content": f"You are EGIA.\n{long}\n- base.velocity (v1.0): move",
      }
    ]
  }
  lines = format_primitives_prompt_lines(exchange, 40)
  text = flatten(lines)
  assert any("planner prompt" in t for t in text)
  assert any("base.velocity" in t for t in text)
  assert all(len(t) <= 40 for t in text if t)
  # /prompts snapshot wins over model_io
  prompts = {"system": "RAW SYSTEM\n- gripper.open (v1.0): open"}
  lines = format_primitives_prompt_lines(exchange, 40, prompts=prompts)
  assert any("RAW SYSTEM" in t for t, _ in lines)
  assert any("gripper.open" in t for t in flatten(lines))
  base = format_context_lines(SNAPSHOT_PAYLOAD, 40)
  stacked = append_primitives_prompt(base, exchange, 40)
  stacked_text = flatten(stacked)
  assert any("◆ planner prompt" in t for t in stacked_text)
  assert all(len(t) <= 40 for t in stacked_text if t)


def test_log_alert_style() -> None:
  from egia_ext.tui.feeds import LogEntry
  from egia_ext.tui.render import log_alert_style

  assert log_alert_style([]) is None
  info = LogEntry(event={"level": "info"})
  warn = LogEntry(event={"level": "warning"})
  error = LogEntry(event={"level": "error"})
  assert log_alert_style([info]) is None
  assert log_alert_style([info, warn]) == "warn"
  assert log_alert_style([warn, error]) == "bad"


def test_parse_command() -> None:
  from egia_ext.tui.render import parse_command

  assert parse_command("  quit ") == ("quit", "")
  assert parse_command('chat "where is the cup?"') == ("chat", "where is the cup?")
  assert parse_command("chat hello") == ("chat", "hello")
  assert parse_command("HELP") == ("help", "")


def test_mission_detail_and_cancelled_list_are_operator_actionable() -> None:
  mission = {
    "mission_id": "12345678-1234-1234-1234-123456789abc",
    "order": "inspect the loading bay",
    "origin": "operator",
    "state": "cancelled",
    "priority_band": "urgent",
    "priority": 0.8,
    "urgency": 0.5,
    "value": 0.6,
    "document": {"revision": 4, "completeness": 0.0, "plan": None},
  }
  detail = format_mission_detail(mission, 80)
  detail_text = "\n".join(flatten(detail))
  assert mission["mission_id"] in detail_text
  assert "state     cancelled" in detail_text
  assert "priority  urgent" in detail_text
  assert "revision  4" in detail_text

  listing = format_missions_lines({"missions": [mission], "history": []}, 100)
  list_text = "\n".join(flatten(listing))
  assert "12345678" in list_text
  assert "cancelled" in list_text
  assert "urgent" in list_text


def test_empty_mission_state_points_to_tui_command_not_stale_route() -> None:
  text = "\n".join(flatten(format_missions_lines({"missions": [], "history": []}, 80)))
  assert "mission add" in text
  assert "/missions" not in text


def test_format_call_guide_skeleton_and_listing() -> None:
  from egia_ext.tui.render import format_call_guide

  primitive = {
    "name": "move",
    "version": "2.0.0",
    "description": "move the base",
    "robot_id": "robot-a",
    "device_id": "base-0",
    "schema": {
      "type": "object",
      "required": ["x"],
      "properties": {
        "x": {"type": "number", "description": "meters east"},
        "mode": {"type": "string", "enum": ["walk", "run"]},
      },
    },
  }
  lines = format_call_guide(primitive)
  text = "\n".join(t for t, _ in lines)
  assert "x: number  # required" in text
  assert 'call move@robot-a/base-0 {"x": 0, "mode": "walk"}' in text
  assert "Tab after the call target inserts required key=value fields" in text

  listing = format_call_guide(None, [primitive])
  assert any("call move@robot-a/base-0 key=value..." in t for t, _ in listing)


def test_call_arguments_skeleton_includes_only_required_fields() -> None:
  from egia_ext.tui.render import call_arguments_skeleton

  schema = {
    "type": "object",
    "required": ["positions", "lease_ms"],
    "properties": {
      "positions": {
        "type": "array",
        "minItems": 2,
        "items": {"type": "number"},
      },
      "lease_ms": {"type": "integer", "exclusiveMinimum": 9},
      "speed": {"type": "number", "default": 0.5},
    },
  }

  assert call_arguments_skeleton(schema) == {
    "positions": [0, 0],
    "lease_ms": 10,
  }


MISSIONS_PAYLOAD = {
  "missions": [
    {
      "mission_id": "m-1",
      "order": "patrol the loading bay",
      "origin": "operator",
      "class_rank": 0,
      "state": "running",
      "priority": 0.62,
      "urgency": 0.8,
      "value": 0.7,
      "est_cost_s": 45.0,
      "preempted_count": 0,
      "submitted_at": "2026-08-18T06:30:00Z",
      "started_at": "2026-08-18T06:30:02Z",
      "finished_at": None,
      "result": None,
      "error": None,
    },
    {
      "mission_id": "m-2",
      "order": "check the charger",
      "origin": "self",
      "class_rank": 1,
      "state": "pending",
      "priority": 0.31,
      "urgency": 0.4,
      "value": 0.5,
      "est_cost_s": None,
      "preempted_count": 1,
      "submitted_at": "2026-08-18T06:29:00Z",
      "started_at": None,
      "finished_at": None,
      "result": None,
      "error": None,
    },
    {
      "mission_id": "m-0",
      "order": "find the cup",
      "origin": "operator",
      "class_rank": 0,
      "state": "succeeded",
      "priority": 0.5,
      "urgency": 0.5,
      "value": 0.5,
      "est_cost_s": None,
      "preempted_count": 0,
      "submitted_at": "2026-08-18T06:25:00Z",
      "started_at": "2026-08-18T06:25:01Z",
      "finished_at": "2026-08-18T06:26:00Z",
      "result": "cup found on the table",
      "error": None,
    },
  ],
  "plan": {
    "plan_id": "plan-live",
    "mission_id": "m-1",
    "steps": [
      {
        "index": 1,
        "outcome": "succeeded",
        "reason": None,
        "final": None,
        "actions": [
          {
            "index": 1,
            "robot_id": "robot-a",
            "intent": "drive to the bay entrance",
            "action": "base.velocity",
            "arguments": {"target": "bay entrance"},
            "outcome": "succeeded",
            "reason": None,
          },
          {
            "index": 2,
            "robot_id": "robot-a",
            "intent": "announce the patrol",
            "action": "speaker.say",
            "outcome": "succeeded",
            "reason": None,
          },
        ],
      },
      {
        "index": 2,
        "outcome": "failed",
        "reason": "rejected by gate (rate_limited)",
        "final": None,
        "actions": [
          {
            "index": 1,
            "robot_id": "robot-b",
            "intent": "scan the bay",
            "action": "vision.detect",
            "outcome": "failed",
            "reason": "rejected by gate (rate_limited)",
          }
        ],
      },
      {
        "index": 3,
        "outcome": "succeeded",
        "reason": None,
        "final": "patrol complete",
        "actions": [],
      },
    ],
    "plan": [
      {"id": "t1", "sentence": "Patrol the loading bay.", "status": "done"},
      {"id": "t2", "sentence": "Scan the dock entrance.", "status": "pending"},
    ],
    "annotations": 0,
  },
}


def test_missions_lines_render_table() -> None:
  lines = format_missions_lines(MISSIONS_PAYLOAD, 80)
  text = flatten(lines)
  assert any("mission list" in t for t in text)
  assert any("1 running · 1 pending · 1 succeeded" in t for t in text)
  running_row = next(t for t in text if "patrol the loading bay" in t)
  assert "▶" in running_row
  assert "running" in running_row
  assert "op" in running_row
  # The running mission expands through all three layers: mission, step, action.
  assert any("step 1  ✓ done" in t for t in text)
  assert any("step 2  ✗ failed" in t for t in text)
  assert any("primitive 1  ✓ robot-a · base.velocity" in t for t in text)
  assert any('args: {"target": "bay entrance"}' in t for t in text)
  assert any("primitive 1  ✗ robot-b · vision.detect" in t for t in text)
  assert any("patrol complete" in t for t in text)
  pending_row = next(t for t in text if "check the charger" in t)
  assert "self" in pending_row and "×1" in pending_row
  assert any("cup found on the table" in t for t in text)


def test_missions_lines_render_plan_phase_and_decision_graph() -> None:
  payload = {
    "missions": [
      {
        "mission_id": "m-1",
        "order": "prepare the package",
        "origin": "operator",
        "state": "running",
        "priority": 1,
        "urgency": 1,
        "value": 1,
      }
    ],
    "history": [],
    "plan": {
      "mission_id": "m-1",
      "revision": 2,
      "decision_mode": "plan",
      "decision_graph": {
        "goal": "package is ready",
        "nodes": [
          {
            "id": "inspect",
            "kind": "observe",
            "title": "inspect the package",
            "after": [],
          },
          {
            "id": "finish",
            "kind": "action",
            "title": "finish the package",
            "after": ["inspect"],
          },
        ],
      },
      "steps": [],
    },
  }
  text = flatten(format_missions_lines(payload, 80))
  assert any("plan r2" in line and "PLAN" in line for line in text)
  assert any("goal: package is ready" in line for line in text)
  assert any("inspect [observe]" in line for line in text)
  assert any("finish [action] after inspect" in line for line in text)


def test_missions_lines_do_not_repeat_live_plan_in_history() -> None:
  payload = {
    "missions": [{"mission_id": "m-1", "state": "running", "order": "move"}],
    "plan": {
      "mission_id": "m-1",
      "revision": 2,
      "steps": [{"index": 1, "outcome": "pending", "actions": []}],
    },
    "history": [
      {
        "mission_id": "m-1",
        "state": "running",
        "order": "move",
        "timestamp": "2026-09-19T03:37:21Z",
        "revision": 2,
        "steps": [{"index": 1, "outcome": "pending", "actions": []}],
      }
    ],
  }

  text = "\n".join(flatten(format_missions_lines(payload, 100)))
  assert text.count("plan r2") == 1


def test_missions_lines_render_history() -> None:
  payload = {
    "missions": [],
    "history": [
      {
        "mission_id": "mission-1",
        "order": "inspect the dock",
        "state": "pending",
        "timestamp": "2026-08-18T06:25:00Z",
        "detail": None,
      },
      {
        "mission_id": "mission-1",
        "order": "inspect the dock",
        "state": "failed",
        "timestamp": "2026-08-18T06:26:00Z",
        "detail": "primitive rejected",
        "revision": 2,
        "completeness": 1.0,
        "steps": [
          {
            "index": 1,
            "state": "failed",
            "reason": "primitive rejected",
            "actions": [
              {
                "index": 1,
                "robot_id": "robot-a",
                "action": "base.velocity",
                "arguments": {"distance": 0.5},
                "state": "failed",
                "reason": "primitive rejected",
              }
            ],
          }
        ],
      },
    ],
  }
  text = flatten(format_missions_lines(payload, 80))
  assert any(line == "history" for line in text)
  assert "06:25:00 inspect the dock  · pending" in text
  assert "06:26:00 inspect the dock (primitive rejected)  ✗ failed" in text
  assert any("step 1  ✗ failed" in line for line in text)
  assert any("primitive 1  ✗ robot-a · base.velocity" in line for line in text)
  assert any('args: {"distance": 0.5}' in line for line in text)


def test_missions_lines_empty_and_disconnected() -> None:
  empty = format_missions_lines({"missions": [], "history": []}, 80)
  assert any("no missions" in t for t in flatten(empty))
  disconnected = format_missions_lines(None, 80)
  assert any("disconnected" in t for t in flatten(disconnected))


def test_backend_profiles_rows_carry_robot_intro() -> None:
  from egia_ext.tui.render import backend_profiles

  fleet = {
    "backends": [
      {
        "backend": "station-1",
        "name": "station-1 robot",
        "description": "assembly arm serving station-1, reaches into station-2",
        "connection": "up",
        "endpoint": "127.0.0.1:18041",
      },
      {"backend": "station-2", "connection": "down"},
    ]
  }
  rows = backend_profiles(fleet)
  assert rows[0]["name"] == "station-1 robot"
  assert "reaches into station-2" in rows[0]["description"]
  assert rows[1]["name"] == "" and rows[1]["connection"] == "down"
  # Legacy single-backend shape renders one anonymous row.
  assert backend_profiles({"connection": "up"})[0]["backend"] == "default"
  assert backend_profiles(None) == []
