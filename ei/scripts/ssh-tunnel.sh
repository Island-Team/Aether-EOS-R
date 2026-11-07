#!/usr/bin/env bash
# Load local credentials and maintain the development SSH tunnels.
set -euo pipefail

SCRIPT="$(readlink -f "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(cd "$(dirname "$SCRIPT")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if [[ ! -f "$ROOT/.env" ]]; then
  echo "ssh-tunnel: missing $ROOT/.env" >&2
  exit 1
fi

set -a
# shellcheck source=/dev/null
source "$ROOT/.env"
set +a

exec python3 "$SCRIPT_DIR/ssh-tunnel.py" "$@"
