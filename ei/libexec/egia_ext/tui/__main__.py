"""``python -m egia_ext.tui`` — the EGIA operator console.

Two modes:

- **wrap** (default) — spawn :mod:`egia.main` as a child in this terminal and
  drive the console on top; the child's console output is captured into the
  log pane. Exiting the console stops the child.
- **attach** (``--attach``) — connect to an already-running pipeline
  (started via ``--no-tui``); no child is spawned and the console only
  watches.
"""

from __future__ import annotations

import argparse
import asyncio
import os
import sys
from collections.abc import Mapping
from pathlib import Path

from egia.lib.config import LoggingConfig, load_logging_config
from egia.lib.logger import configure_logging, get_logger
from egia_ext.tui.app import ConsoleApp, run_curses
from egia_ext.tui.config import load_tui_config
from egia_ext.tui.feeds import (
  ConsoleState,
  backend_feed,
  child_stderr_feed,
  context_feed,
  feedback,
  health_feed,
  log_tail,
  missions_feed,
  model_io_feed,
  primitives_feed,
  prompts_feed,
  replay_tail_offset,
  send_call,
  send_chat,
  send_mission_command,
  send_skill_command,
  session_replay_path,
  skills_feed,
  watch_skill_execution,
)
from egia_ext.tui.procmon import process_monitor
from egia_ext.tui.repl import launch_repl
from egia_ext.tui.spawn import ChildProcess, spawn_pipeline, stop_pipeline
from island.boot import config_path, factory_config_path
from island.cfg import load as load_product_config
from island.path import Layout


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
  """Parse console arguments; ``--`` and unknown args forward to the child."""
  parser = argparse.ArgumentParser(
    prog="egia_ext.tui",
    description="EGIA operator console (wrap or attach mode)",
  )
  parser.add_argument(
    "--attach",
    action="store_true",
    help="watch an already-running pipeline instead of spawning one",
  )
  parser.add_argument("--etc", default=None, help="config root (default: repo etc/)")
  parser.add_argument(
    "--var",
    default=None,
    help="legacy mutable root split into lib/cache/log/run/tmp",
  )
  parser.add_argument("--state-root", default=None)
  parser.add_argument("--cache-root", default=None)
  parser.add_argument("--log-root", default=None)
  parser.add_argument("--run-root", default=None)
  parser.add_argument("--tmp-root", default=None)
  parser.add_argument(
    "--log-level",
    default=None,
    choices=["DEBUG", "INFO", "WARNING", "ERROR"],
    help="log pane threshold (default: [logging.console] level in egia.toml)",
  )
  parser.add_argument(
    "--no-tui",
    action="store_true",
    help="accepted for pass-through; the launcher consumes it before exec",
  )
  parser.add_argument(
    "--island-config",
    default=None,
    help="product config propagated by the Island launcher",
  )
  args, child_args = parser.parse_known_args(argv)
  args.child_args = child_args
  return args


def _pipeline_argv(etc: str | Path, layout: Layout, forwarded: list[str]) -> list[str]:
  """Build a relocatable EGIA module invocation from injected roots."""
  return [
    "-m",
    "egia.main",
    "--etc",
    str(etc),
    "--state-root",
    str(layout.state),
    "--cache-root",
    str(layout.cache),
    "--log-root",
    str(layout.log),
    "--run-root",
    str(layout.run),
    "--tmp-root",
    str(layout.tmp),
    *forwarded,
  ]


def _selected_product_config(
  layout: Layout,
  explicit: str | None,
  environ: Mapping[str, str] = os.environ,
) -> Path:
  """Resolve the CLI selection before the established environment fallback."""
  return config_path(layout, explicit or environ.get("ISLAND_CONFIG"))


async def _run(args: argparse.Namespace, layout: Layout) -> int:
  config = load_tui_config(
    etc=args.etc or layout.component_etc("egia"),
    defaults=layout.share / "defaults/egia",
    state=layout.component_state("egia"),
    cache=layout.component_cache("egia"),
    log=layout.component_log("egia"),
    run=layout.run,
    tmp=layout.tmp,
    log_level=args.log_level,
  )
  state = ConsoleState()
  app = ConsoleApp(state, config)
  app.on_chat = lambda sentence: send_chat(state, config, sentence)
  app.on_call = lambda primitive, arguments, robot_id, device_id=None: send_call(
    state, config, primitive, arguments, robot_id, device_id
  )
  app.on_mission = lambda payload: send_mission_command(state, config, payload)
  app.on_skill = lambda payload: send_skill_command(state, config, payload)
  app.on_skill_watch = lambda robot_id, execution_id: watch_skill_execution(
    state, config, robot_id, execution_id
  )
  selected_product_config = _selected_product_config(layout, args.island_config)
  product_config = load_product_config(
    selected_product_config,
    defaults=factory_config_path(layout),
  )
  eos_launcher = (layout.prefix / product_config.components.eos.launch).resolve()

  async def open_repl(alias: str, endpoint: str) -> None:
    result = await launch_repl(eos_launcher, endpoint)
    if result.error is not None:
      feedback(state, f"repl {alias}: {result.error}", "warning")
    elif result.exit_code != 0:
      feedback(state, f"repl {alias} exited with status {result.exit_code}", "warning")
    else:
      feedback(state, f"repl {alias} returned to the TUI")

  app.on_repl = open_repl

  # Replay logs append across launches. Capture the precise boundary before
  # feeds and the wrapped pipeline start, so old failures never render as if
  # they belonged to this TUI session.
  replay_offset = replay_tail_offset(config.replay_path)
  session_id = os.environ.get("ISLAND_SESSION_ID")
  session_logging = session_replay_path(config, session_id) is not None
  child: ChildProcess | None = None
  tasks: list[asyncio.Task[None]] = [
    asyncio.create_task(health_feed(state, config), name="tui-health"),
    asyncio.create_task(context_feed(state, config), name="tui-context"),
    asyncio.create_task(backend_feed(state, config), name="tui-backend"),
    asyncio.create_task(model_io_feed(state, config), name="tui-model-io"),
    asyncio.create_task(missions_feed(state, config), name="tui-missions"),
    asyncio.create_task(primitives_feed(state, config), name="tui-primitives"),
    asyncio.create_task(skills_feed(state, config), name="tui-skills"),
    asyncio.create_task(prompts_feed(state, config), name="tui-prompts"),
    asyncio.create_task(
      log_tail(state, config, initial_offset=replay_offset, session_id=session_id),
      name="tui-log-tail",
    ),
  ]
  if not args.attach:
    forwarded = list(args.child_args)
    if args.island_config is not None:
      forwarded = ["--island-config", str(selected_product_config), *forwarded]
    child_argv = _pipeline_argv(config.etc_dir, layout, forwarded)
    child = await spawn_pipeline(child_argv, cwd=layout.prefix)
    tasks.append(
      asyncio.create_task(process_monitor(state, child.process.pid), name="tui-procmon")
    )
    if child.stderr is not None:
      tasks.append(
        asyncio.create_task(
          child_stderr_feed(
            state,
            config,
            child.stderr,
            structured_in_session=session_logging,
          ),
          name="tui-child-stderr",
        )
      )
    tasks.append(
      asyncio.create_task(_watch_child(state, child), name="tui-child-watch")
    )

  try:
    await run_curses(app)
  finally:
    if child is not None:
      code = await stop_pipeline(child)
      state.child_exited = code
    for task in tasks:
      task.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)
  if child is not None:
    return child.process.returncode or 0
  return 0


async def _watch_child(state: ConsoleState, child: ChildProcess) -> None:
  """Record the child exit code in the state once it terminates."""
  state.child_exited = await child.process.wait()
  state.dirty.set()


def main(argv: list[str] | None = None) -> int:
  """Console entry point."""
  args = parse_args(argv)
  layout = Layout.resolve(
    var=args.var,
    state=args.state_root,
    cache=args.cache_root,
    log=args.log_root,
    run=args.run_root,
    tmp=args.tmp_root,
  )
  if not sys.stdout.isatty():
    # No terminal: degrade to attach-free pass-through — just run the child.
    # (The launcher only routes TTY launches here, but be safe when invoked
    # directly from scripts.)
    if not args.attach:
      os.execv(
        sys.executable,
        [
          sys.executable,
          *_pipeline_argv(
            args.etc or layout.component_etc("egia"),
            layout,
            [
              *(
                ["--island-config", args.island_config]
                if args.island_config is not None
                else []
              ),
              *args.child_args,
            ],
          ),
        ],
      )
    return 2
  logging_error: Exception | None = None
  log = None
  try:
    try:
      logging_config = load_logging_config(
        args.etc or layout.component_etc("egia"),
        defaults=layout.share / "defaults/egia",
      )
    except Exception as exc:  # noqa: BLE001 — preserve a file-only crash record
      logging_error = exc
      logging_config = LoggingConfig()
    logging_config = logging_config.model_copy(
      update={
        "console": logging_config.console.model_copy(update={"enabled": False}),
        "replay": logging_config.replay.model_copy(update={"enabled": False}),
      }
    )
    configure_logging(
      log_dir=layout.component_log("egia"),
      config=logging_config,
      replay_dir=layout.component_state("egia"),
    )
    log = get_logger("egia_ext.tui")
    if logging_error is not None:
      log.error("tui_logging_config_failed", error=str(logging_error))
      return 1
    return asyncio.run(_run(args, layout))
  except Exception:
    if log is not None:
      log.exception("tui_unhandled_error")
    return 1


if __name__ == "__main__":
  raise SystemExit(main())
