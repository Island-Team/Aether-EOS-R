"""Stopped-process, same-UID local authorization for an EGIA latch reset."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from egia.lib.safety_state import SafetyState, SafetyStateError, runtime_lock
from island.path import Layout


def main(argv: list[str] | None = None) -> int:
  parser = argparse.ArgumentParser(prog="egia safety")
  parser.add_argument("command", choices=("reset",))
  parser.add_argument("--state-root", type=Path, default=None)
  parser.add_argument("--episode", required=True)
  parser.add_argument("--graph-revision", required=True)
  args = parser.parse_args(argv)
  if not sys.stdin.isatty() or not sys.stdout.isatty():
    print("EGIA safety reset requires an interactive terminal", file=sys.stderr)
    return 2
  layout = Layout.resolve(state=args.state_root)
  root = layout.component_state("egia") / "safety"
  try:
    with runtime_lock(root):
      state = SafetyState(root)
      aliases = state.aliases()
      if not aliases:
        raise SafetyStateError("no durable EGIA latch is engaged")
      print("Affected EOS aliases: " + ", ".join(sorted(aliases)))
      print(
        "EOS estop must be remediated externally. This only authorizes fresh checks on next EGIA startup."
      )
      answer = input("Type RESET EGIA SAFETY to authorize one startup check: ")
      if answer != "RESET EGIA SAFETY":
        raise SafetyStateError("reset confirmation did not match")
      state.authorize_reset(aliases, args.episode, args.graph_revision)
  except (SafetyStateError, ValueError, OSError) as exc:
    print(f"EGIA safety reset refused: {exc}", file=sys.stderr)
    return 1
  print(
    "One startup reset check authorized; EGIA remains latched until it verifies every alias."
  )
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
