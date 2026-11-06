#!/usr/bin/env bash
# Dispatch source-checkout build and launch commands by host CPU architecture.
set -euo pipefail

SCRIPT="$(readlink -f "${BASH_SOURCE[0]}")"
ROOT="$(cd "$(dirname "$SCRIPT")/../.." && pwd)"
ACTION="${1:-}"

case "$ACTION" in
  build|launch)
    shift
    ;;
  *)
    echo "egia-platform: expected action 'build' or 'launch'" >&2
    exit 2
    ;;
esac

case "$(uname -m)" in
  aarch64|arm64)
    ARCH="arm64"
    ;;
  x86_64|amd64)
    ARCH="x86_64"
    ;;
  *)
    echo "egia-platform: unsupported architecture '$(uname -m)'; expected arm64 or x86_64" >&2
    exit 2
    ;;
esac

export EGIA_ROOT="$ROOT"
exec "$ROOT/scripts/platform/$ARCH/$ACTION.sh" "$@"
