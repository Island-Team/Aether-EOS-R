#!/usr/bin/env bash
set -euo pipefail

root="$(readlink -f "$(dirname "${BASH_SOURCE[0]}")/..")"
exec python3 "${root}/scripts/vad_test.py" "$@"
