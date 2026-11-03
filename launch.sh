#!/usr/bin/env bash
# Open the EOS CLI; IsOS owns runtime startup and shutdown.
set -euo pipefail
ROOT="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
exec "$ROOT/bin/eos-cli" "$@"
