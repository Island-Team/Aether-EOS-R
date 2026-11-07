#!/usr/bin/env bash
set -euo pipefail

root="$(readlink -f "$(dirname "${BASH_SOURCE[0]}")/..")"
exec "${root}/adapters/eos-cxx/ext/backends/host/tools/mic.sh" "$@"
