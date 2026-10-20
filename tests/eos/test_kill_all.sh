#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
fixture="$(mktemp -d)"
pid=""
listener=""
stale_pid=""
cleanup() {
  [[ -z "$pid" ]] || kill -TERM "$pid" 2>/dev/null || true
  [[ -z "$listener" ]] || kill -TERM "$listener" 2>/dev/null || true
  [[ -z "$stale_pid" ]] || kill -TERM "$stale_pid" 2>/dev/null || true
  rm -rf -- "$fixture"
}
trap cleanup EXIT

setsid sleep 30 >/dev/null 2>&1 &
pid="$!"
for attempt in {1..50}; do
  line="$(<"/proc/$pid/stat")"
  line="${line##*) }"
  read -r -a fields <<< "$line"
  [[ "${fields[2]}" == "$pid" ]] && break
  sleep 0.01
done
[[ "${fields[2]}" == "$pid" ]]
printf '%s %s\n' "$pid" "${fields[19]}" >"$fixture/eos-prelaunch-$pid.pids"

if ISLAND_RUN="$fixture" \
    "$root/scripts/kill-all.sh" --check >"$fixture/output" 2>&1; then
  printf 'kill-all --check missed a live registered process group\n' >&2
  exit 1
fi
grep -Fq "prelaunch pgid=$pid" "$fixture/output"
[[ -f "$fixture/eos-prelaunch-$pid.pids" ]]

# A reused group ID must not block cleanup or terminate its new owner.
mkdir "$fixture/stale"
setsid sleep 30 >/dev/null 2>&1 &
stale_pid="$!"
printf '%s %s\n' "$stale_pid" 0 >"$fixture/stale/eos-prelaunch-$stale_pid.pids"
if ! ISLAND_KILL_PORTS=1 ISLAND_RUN="$fixture/stale" \
    "$root/scripts/kill-all.sh" >"$fixture/output" 2>&1; then
  cat "$fixture/output" >&2
  exit 1
fi
grep -Fq "ignoring reused prelaunch pgid=$stale_pid" "$fixture/output"
[[ ! -f "$fixture/stale/eos-prelaunch-$stale_pid.pids" ]]
kill -0 "$stale_pid"

# Restart checks ignore retained prelaunch groups.
ISLAND_RUN="$fixture" "$root/scripts/kill-all.sh" --check-runtime >"$fixture/output" 2>&1
grep -Fq 'No EOS runtime processes' "$fixture/output"

# An unrelated listener on a configured deployment port must be reported, not
# killed simply because it owns that port. Use an ephemeral port for isolation.
python3 -u -c 'import socket,time; s=socket.socket(); s.bind(("127.0.0.1",0)); s.listen(); print(s.getsockname()[1],flush=True); time.sleep(30)' >"$fixture/port" &
listener="$!"
for attempt in {1..50}; do
  [[ -s "$fixture/port" ]] && break
  sleep 0.01
done
[[ -s "$fixture/port" ]]
port="$(<"$fixture/port")"
if ISLAND_KILL_PORTS="$port" ISLAND_RUN="$fixture" \
    "$root/scripts/kill-all.sh" --check >"$fixture/output" 2>&1; then
  printf 'kill-all --check missed an unknown listener\n' >&2
  exit 1
fi
grep -Fq "UNKNOWN listener pid=$listener ports= $port" "$fixture/output"
kill -0 "$listener"
ISLAND_KILL_PORTS="$port" ISLAND_RUN="$fixture" \
  "$root/scripts/kill-all.sh" --check-runtime >"$fixture/output" 2>&1
grep -Fq 'No EOS runtime processes' "$fixture/output"
kill -0 "$listener"
mkdir "$fixture/empty"
if ISLAND_KILL_PORTS="$port" ISLAND_RUN="$fixture/empty" \
    "$root/scripts/kill-all.sh" >"$fixture/output" 2>&1; then
  printf 'kill-all silently accepted an unapproved listener\n' >&2
  exit 1
fi
grep -Fq "Leave pid=$listener running" "$fixture/output"
kill -0 "$listener"
