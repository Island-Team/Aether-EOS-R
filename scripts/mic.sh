#!/usr/bin/env bash
set -euo pipefail

root="$(readlink -f "$(dirname "${BASH_SOURCE[0]}")/..")"
exec "${root}/src/eos/ext/backends/host/tools/mic.sh" "$@"
