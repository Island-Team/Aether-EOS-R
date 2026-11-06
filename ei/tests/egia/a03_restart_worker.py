"""Separate EGIA owner process for the A03 stable-run stub replay.

This test-only entry point keeps one stub EOS server and grant in the pytest
parent while each invocation creates a fresh interpreter and EGIA client.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
from pathlib import Path

from egia.core import launch
from egia.core.link import EosClient
from egia.lib import shared_events
from egia.lib.config import LaunchConfig
from egia.lib.operation_ledger import OperationLedger, PendingOperation
from egia.lib.safety_state import SafetyState, runtime_lock
from egia.stages import StageRuntime, compose_stages

VELOCITY = {
  "linear_x_m_s": 0.1,
  "linear_y_m_s": 0.0,
  "angular_z_rad_s": 0.0,
  "valid_for_ms": 100,
}


async def _reserve_then_exit(
  config: LaunchConfig, state_root: Path, operation_id: str, *, submit: bool
) -> None:
  """Persist the pre-egress checkpoint, optionally Submit, then crash before settlement."""
  client = EosClient(
    config.gateway, backend="robot-a", expected_revision=os.environ["A03_REVISION"]
  )
  discovery = await client.initialize()
  ledger = OperationLedger(state_root)
  ledger.reserve(
    PendingOperation(
      operation_id,
      "robot-a",
      "base.velocity",
      "base-0",
      None,
      discovery.robot_id,
      os.environ["A03_REVISION"],
    )
  )
  if submit:
    receipt = await client.call_primitive(
      "base.velocity", "base-0", VELOCITY, command_id=operation_id
    )
    if receipt.status.value != "succeeded":
      raise AssertionError(receipt)
  print(
    json.dumps(
      {
        "phase": "reserved",
        "operation_id": operation_id,
        "submitted": submit,
        "robot_id": discovery.robot_id,
      }
    ),
    flush=True,
  )
  # Deliberate crash boundary: the EOS stub has a terminal receipt but EGIA's
  # durable checkpoint remains pending. The Python process must really exit.
  os._exit(71 if submit else 72)


async def _launch_from_checkpoint(
  config: LaunchConfig, graph: dict, state_root: Path, etc_root: str
) -> None:
  """Run the production pending-operation startup loop in a fresh process."""
  run_id = shared_events.select_run_id(True, os.environ.get("ISOS_RUN_ID"))
  shared_events.configure(run_id)
  shared_events.emit("egia_starting")
  with runtime_lock(state_root / "safety"):
    reset_authorization = SafetyState(state_root / "safety").consume_reset()
    runtime = StageRuntime(compose_stages(graph), run_id)
    await launch(
      config,
      stage_runtime=runtime,
      managed=True,
      state_dir=state_root,
      etc=etc_root,
      reset_authorization=reset_authorization,
      reset_checked=True,
    )


def main() -> None:
  """Execute one bounded worker phase from the parent test fixture."""
  parser = argparse.ArgumentParser()
  parser.add_argument("phase", choices=("submit", "reserve_only", "launch"))
  parser.add_argument("--config", type=Path, required=True)
  parser.add_argument("--graph", type=Path, required=True)
  parser.add_argument("--state-root", type=Path, required=True)
  parser.add_argument("--etc", required=True)
  parser.add_argument("--operation-id", required=True)
  args = parser.parse_args()
  os.umask(0o077)
  config = LaunchConfig.model_validate_json(args.config.read_text())
  if args.phase == "launch":
    asyncio.run(
      _launch_from_checkpoint(
        config, json.loads(args.graph.read_text()), args.state_root, args.etc
      )
    )
  else:
    asyncio.run(
      _reserve_then_exit(
        config, args.state_root, args.operation_id, submit=args.phase == "submit"
      )
    )


if __name__ == "__main__":
  main()
