#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
if command -v cargo >/dev/null 2>&1; then
  CARGO_BIN="$(command -v cargo)"
elif [[ -x "${HOME:-}/.cargo/bin/cargo" ]]; then
  CARGO_BIN="${HOME}/.cargo/bin/cargo"
else
  echo "IsOS requires Rust Cargo; install Rust from https://rustup.rs or add ~/.cargo/bin to PATH" >&2
  exit 127
fi
"$CARGO_BIN" build --release
