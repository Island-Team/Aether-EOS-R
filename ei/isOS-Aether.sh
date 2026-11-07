#!/usr/bin/env bash
# Compatibility wrapper for the selected IsOS deployment.
set -euo pipefail

SCRIPT="$(readlink -f "${BASH_SOURCE[0]}")"
ROOT="$(cd "$(dirname "$SCRIPT")" && pwd)"

exec "$ROOT/bin/island" "$@"
