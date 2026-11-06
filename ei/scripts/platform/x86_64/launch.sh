#!/usr/bin/env bash
# Launch standalone EGIA from a source checkout on x86-64 Linux systems.
set -euo pipefail

if [[ -z "${EGIA_PYTHON:-}${ISLAND_PYTHON:-}" && ! -x "$EGIA_ROOT/.venv/bin/python" ]]; then
  if ! command -v uv >/dev/null 2>&1; then
    echo "egi-launch: uv is required for first launch" >&2
    exit 127
  fi
  cd "$EGIA_ROOT"
  PYTHON_REQUEST="${EGIA_PYTHON_VERSION:-3.12}"
  uv sync --python "$PYTHON_REQUEST" --no-install-project
  uv sync --python "$PYTHON_REQUEST" --no-build-isolation
fi

for arg in "$@"; do
  if [[ "$arg" == "-h" || "$arg" == "--help" ]]; then
    exec "$EGIA_ROOT/bin/egia" "$@"
  fi
done

exec "$EGIA_ROOT/bin/egia" "$@"
