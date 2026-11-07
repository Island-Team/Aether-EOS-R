#!/usr/bin/env bash
# Launch standalone EGIA for the detected CPU architecture, regardless of cwd.
# Pass --no-tui to run EGIA without its terminal console.
set -euo pipefail

SCRIPT="$(readlink -f "${BASH_SOURCE[0]}")"
ROOT="$(cd "$(dirname "$SCRIPT")" && pwd)"

exec "$ROOT/scripts/platform/dispatch.sh" launch "$@"
