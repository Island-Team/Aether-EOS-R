#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="${HOME}/.cargo/bin:${PATH}"
cd "$repo_root"

run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
log_dir="$repo_root/logs/test/$run_id"
mkdir -p "$log_dir"
{
  printf 'run_id=%s\n' "$run_id"
  printf 'revision=%s\n' "$(git rev-parse --short HEAD)"
  printf 'architecture=%s\n' "$(uname -m)"
  printf 'rustc=%s\n' "$(rustc --version)"
  if [[ -r /proc/cpuinfo ]]; then
    awk -F: '/^model name[[:space:]]*:/ { gsub(/^[[:space:]]+/, "", $2); print "cpu=" $2; exit }' /proc/cpuinfo
  fi
} > "$log_dir/metadata.txt"

run_logged() {
  local name="$1"
  shift
  printf 'Running %s; log: %s/%s.log\n' "$name" "$log_dir" "$name"
  "$@" 2>&1 | tee "$log_dir/$name.log"
}

run_logged fmt cargo fmt --all -- --check
run_logged clippy cargo clippy --workspace --all-targets -- -D warnings
run_logged test cargo test --workspace --all-targets

if [[ "${1:-}" == "--bench" ]]; then
  run_logged bench cargo run -p aether-dispatcher --example bench --release
fi

printf 'EOS v2 checks passed; logs: %s\n' "$log_dir"
