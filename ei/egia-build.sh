#!/usr/bin/env bash
# Build EGIA for the detected CPU architecture, regardless of the cwd.
set -euo pipefail

SCRIPT="$(readlink -f "${BASH_SOURCE[0]}")"
ROOT="$(cd "$(dirname "$SCRIPT")" && pwd)"

exec "$ROOT/scripts/platform/dispatch.sh" build "$@"
