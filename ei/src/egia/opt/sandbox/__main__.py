"""Local EOS-compatible camera/mic/speaker playground process."""

from __future__ import annotations

import argparse
import asyncio
import os
import signal
from pathlib import Path

from egia.lib.config import load_logging_config, load_sandbox_config
from egia.lib.logger import configure_logging, get_logger
from egia.opt.sandbox.server import PlaygroundServer


async def run(
  *,
  etc: str | None = None,
  defaults: str | None = None,
  media_socket: str | None = None,
) -> None:
  """Serve the playground until the owning EGIA process requests shutdown."""
  log = get_logger("egia.opt.sandbox")
  stop = asyncio.Event()
  loop = asyncio.get_running_loop()
  for signum in (signal.SIGINT, signal.SIGTERM):
    loop.add_signal_handler(signum, stop.set)
  runtime = load_sandbox_config(etc, defaults=defaults)
  server = PlaygroundServer(runtime)
  await server.start(media_socket=media_socket)
  log.info(
    "sandbox_ready",
    extra={
      "pid": os.getpid(),
      "grpc": f"{runtime.gateway.host}:{runtime.gateway.grpc_port}",
      "media_socket": media_socket,
    },
  )
  try:
    await stop.wait()
  finally:
    log.info("sandbox_stopping")
    await server.stop()


def main(argv: list[str] | None = None) -> int:
  """Run the sandbox lifecycle."""
  parser = argparse.ArgumentParser(prog="egia.opt.sandbox")
  parser.add_argument("--etc", default=None)
  parser.add_argument("--defaults", default=None)
  parser.add_argument("--log-dir", default="var/log/egia")
  parser.add_argument("--media-socket", default=None)
  args = parser.parse_args(argv)
  logging_config = load_logging_config(args.etc, defaults=args.defaults)
  # The owning EGIA process is the sole replay writer. The playground is a
  # separate process, but it uses the same formatters, levels, routing, and
  # human log layout as every other repository module.
  logging_config = logging_config.model_copy(
    update={"replay": logging_config.replay.model_copy(update={"enabled": False})}
  )
  configure_logging(
    log_dir=Path(args.log_dir),
    config=logging_config,
    replay_dir=Path(args.log_dir),
  )
  log = get_logger("egia.opt.sandbox")
  try:
    asyncio.run(
      run(
        etc=args.etc,
        defaults=args.defaults,
        media_socket=args.media_socket,
      )
    )
  except Exception:
    log.exception("sandbox_failed")
    return 1
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
