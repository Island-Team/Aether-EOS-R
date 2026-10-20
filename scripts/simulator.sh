#!/usr/bin/env bash
set -euo pipefail
SCRIPT="$(readlink -f "${BASH_SOURCE[0]}")"
ROOT="$(cd "$(dirname "$SCRIPT")/.." && pwd)"
case "$(uname -m)" in
  x86_64|amd64) BUILD_DIR="${ISLAND_BUILD_DIR:-$ROOT/build/aether}"; PRESET=default ;;
  aarch64|arm64) BUILD_DIR="${ISLAND_BUILD_DIR:-$ROOT/build/aether-arm64}"; PRESET=arm64 ;;
  *) printf 'eos-simulator: unsupported host architecture\n' >&2; exit 2 ;;
esac
[[ "$BUILD_DIR" = /* ]] || { printf 'eos-simulator: ISLAND_BUILD_DIR must be absolute\n' >&2; exit 2; }
PROGRAM="$BUILD_DIR/src/eos/bin/eos-simulator"
if [[ ! -x "$PROGRAM" ]]; then
  [[ -f "$BUILD_DIR/CMakeCache.txt" ]] || cmake --preset "$PRESET" -S "$ROOT"
  cmake --build "$BUILD_DIR" --target eos-simulator
fi
export ISLAND_PREFIX="$ROOT" ISLAND_MODE="${ISLAND_MODE:-source}"
export ISLAND_ETC="${ISLAND_ETC:-$ROOT/etc}" ISLAND_SHARE="${ISLAND_SHARE:-$ROOT/share}"
export ISLAND_STATE="${ISLAND_STATE:-$ROOT/var/lib}" ISLAND_CACHE="${ISLAND_CACHE:-$ROOT/var/cache}"
export ISLAND_LOG="${ISLAND_LOG:-$ROOT/var/log}" ISLAND_RUN="${ISLAND_RUN:-$ROOT/var/run}" ISLAND_TMP="${ISLAND_TMP:-$ROOT/var/tmp}"
exec "$PROGRAM" --etc "$ISLAND_ETC/eos" --contracts "$ISLAND_SHARE" "$@"
