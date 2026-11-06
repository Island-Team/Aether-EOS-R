#!/usr/bin/env bash
set -euo pipefail
exec "${ISOS_EXECUTABLE:-isos}" physical-stop stop "$@"
