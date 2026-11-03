#!/usr/bin/env bash
# Build the Rust-owned EOS v2 process set. The C++ side chain is opt-in.
if [ -z "${BASH_VERSION:-}" ]; then exec bash "$0" "$@"; fi
set -euo pipefail

ROOT="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
TARGET_DIR="${CARGO_TARGET_DIR:-$ROOT/target}"
case "$TARGET_DIR" in /*) ;; *) TARGET_DIR="$ROOT/$TARGET_DIR" ;; esac
CPP_SIDE_CHAIN=0
CLEAN=0
PRINT=0
for arg in "$@"; do
  case "$arg" in
    --cpp-side-chain) CPP_SIDE_CHAIN=1 ;;
    --clean) CLEAN=1 ;;
    --print-build-dir) PRINT=1 ;;
    --help|-h)
      cat <<'EOF'
Usage: ./build.sh [--clean] [--print-build-dir] [--cpp-side-chain]

Build the Rust v2 EOS workspace in release mode. --cpp-side-chain also builds
the unlaunched C++ implementation and hardware integration targets with CMake.
EOF
      exit 0 ;;
    *) printf 'unknown option: %s\n' "$arg" >&2; exit 2 ;;
  esac
done
if [[ "$PRINT" -eq 1 ]]; then printf '%s\n' "$TARGET_DIR/release"; exit 0; fi
export CARGO_TARGET_DIR="$TARGET_DIR"
CARGO_BIN="$(command -v cargo || true)"
if [[ -z "$CARGO_BIN" && -x "$HOME/.cargo/bin/cargo" ]]; then CARGO_BIN="$HOME/.cargo/bin/cargo"; fi
if [[ -z "$CARGO_BIN" ]]; then echo 'build.sh: cargo is required' >&2; exit 127; fi
if [[ "$CLEAN" -eq 1 ]]; then "$CARGO_BIN" clean --manifest-path "$ROOT/Cargo.toml"; fi
"$CARGO_BIN" build --manifest-path "$ROOT/Cargo.toml" --release --workspace
if [[ "$CPP_SIDE_CHAIN" -eq 1 ]]; then
  cmake -S "$ROOT" -B "$ROOT/build/cpp-side-chain" \
    -DISLAND_BUILD_EOS=OFF -DISLAND_BUILD_EOS_CPP=ON
  cmake --build "$ROOT/build/cpp-side-chain" --parallel "${AETHER_BUILD_JOBS:-2}"
fi
