"""Validate EOS schemas loaded from configured Island shared roots."""

from __future__ import annotations

import argparse
from pathlib import Path

from egia.contracts.source import configure, schema_documents, schema_registry
from island.share import repository_resources, require_eos_contracts

ROOT = Path(__file__).resolve().parents[2]
RESOURCES = repository_resources(ROOT)


def check() -> None:
  require_eos_contracts(RESOURCES)
  configure(RESOURCES)
  documents = schema_documents()
  if not documents:
    raise ValueError("configured shared roots contain no JSON Schemas")
  schema_registry()


def main(argv: list[str] | None = None) -> int:
  parser = argparse.ArgumentParser()
  parser.add_argument("--check", action="store_true")
  args = parser.parse_args(argv)
  try:
    del args
    check()
  except (OSError, TypeError, ValueError) as exc:
    print(exc)
    return 1
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
