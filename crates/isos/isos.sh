#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$ROOT/.env" ]]; then
  set -a
  # shellcheck disable=SC1091
  source "$ROOT/.env"
  set +a
fi
set -a
# shellcheck disable=SC1091
source "$ROOT/.env.example"
set +a
if command -v isos >/dev/null 2>&1; then
  exec isos "$@"
fi
exec "$ROOT/bin/isos" "$@"
