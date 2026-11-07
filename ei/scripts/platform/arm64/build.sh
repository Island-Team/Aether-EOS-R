#!/usr/bin/env bash
# Bootstrap dependencies and build EGIA on ARM64 Linux systems.
set -euo pipefail

if ! command -v uv >/dev/null 2>&1; then
  echo "egia-build: uv is required; install it from https://docs.astral.sh/uv/" >&2
  exit 127
fi

cd "$EGIA_ROOT"
# Seed build requirements from the lockfile before disabling isolated builds.
PYTHON_REQUEST="${EGIA_PYTHON:-${EGIA_PYTHON_VERSION:-3.12}}"
uv sync --python "$PYTHON_REQUEST" --all-extras --no-install-project
uv sync --python "$PYTHON_REQUEST" --all-extras --no-build-isolation
exec uv build --no-build-isolation "$@"
