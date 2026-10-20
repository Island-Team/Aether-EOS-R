#!/usr/bin/env bash
set -euo pipefail

script="$(readlink -f "${BASH_SOURCE[0]}")"
root="$(dirname "$(dirname "$script")")"
timeout=15
check_only=0
runtime_check=0
exclude_pid=""

usage() {
  cat <<'EOF'
Usage: kill-all.sh [--check] [--timeout SECONDS]

Stop EOS processes launched from this source tree, including simulator, north
HTTP, REPL, and the process groups created for [[launch.prelaunch]] entries.
Inspect EOS listener ports and ask before stopping an unrecognized process
(leave it running without a terminal).

Options:
  --check            Report whether related processes exist without stopping them
  --check-runtime    Report only EOS launcher/core/simulator/HTTP/REPL processes;
                     ignore prelaunch groups and model services (used by eos-launch.sh)
  --timeout SECONDS  Wait this long before sending SIGKILL (default: 15)
  -h, --help         Show this help message
EOF
}

while (($#)); do
  case "$1" in
    --check)
      check_only=1
      shift
      ;;
    --check-runtime)
      check_only=1
      runtime_check=1
      shift
      ;;
    --exclude-pid)
      (($# >= 2)) || { printf 'kill-all.sh: --exclude-pid requires a value\n' >&2; exit 2; }
      exclude_pid="$2"
      shift 2
      ;;
    --timeout)
      (($# >= 2)) || { printf 'kill-all.sh: --timeout requires a value\n' >&2; exit 2; }
      timeout="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      printf 'kill-all.sh: unknown option: %s\n' "$1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

[[ "$timeout" =~ ^[0-9]+$ ]] || {
  printf 'kill-all.sh: timeout must be a non-negative whole number of seconds\n' >&2
  exit 2
}
[[ -z "$exclude_pid" || "$exclude_pid" =~ ^[0-9]+$ ]] || {
  printf 'kill-all.sh: --exclude-pid must be a process ID\n' >&2
  exit 2
}

declare -A parent=() command=() cwd_path=() start_time=() selected=() eosd=() groups=() group_start=()
declare -A port_owners=() unknown_ports=() hidden_ports=()
declare -a unresolved_registries=()
run_dir="${ISLAND_RUN:-$root/var/run}"
# Only EOS ports are candidates. A port never authorizes a kill by itself.
expected_ports="${ISLAND_KILL_PORTS:-16003 16004 16005}"

read_process() {
  local pid="$1" line key value
  local -a argv=()
  [[ -r "/proc/$pid/status" && -r "/proc/$pid/stat" ]] || return 1
  while IFS=: read -r key value; do
    if [[ "$key" == "PPid" ]]; then
      value="${value//[[:space:]]/}"
      parent["$pid"]="$value"
      break
    fi
  done < "/proc/$pid/status"
  line="$(<"/proc/$pid/stat")" || return 1
  line="${line##*) }"
  read -r -a fields <<< "$line"
  start_time["$pid"]="${fields[19]}"
  mapfile -d '' -t argv < "/proc/$pid/cmdline" || return 1
  command["$pid"]="${argv[*]:-}"
  cwd_path["$pid"]="$(readlink -f "/proc/$pid/cwd" 2>/dev/null || true)"
}

is_repo_process() {
  local pid="$1" exe base
  exe="$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)"
  base="${exe##*/}"
  if [[ "$exe" == "$root/"* ]]; then
    case "$base" in
      eosd|eos-simulator|eos-north-http|eos-repl) return 0 ;;
    esac
  fi
  [[ " ${command[$pid]} " == *" $root/eos-launch.sh "* ]] && return 0
  # An out-of-tree build still identifies this checkout through eosd's --etc path.
  [[ "$base" == "eosd" && " ${command[$pid]} " == *" $root/etc/eos "* ]]
}

discover_port_owners() {
  local line rest port pid allowed found_pid
  if ! command -v ss >/dev/null 2>&1; then
    printf '  WARN ss is unavailable; cannot inspect listener ownership\n' >&2
    return
  fi
  while IFS= read -r line; do
    [[ "$line" =~ :([0-9]+)[[:space:]] ]] || continue
    port="${BASH_REMATCH[1]}"
    allowed=0
    for candidate in $expected_ports; do
      [[ "$candidate" == "$port" ]] && { allowed=1; break; }
    done
    ((allowed)) || continue
    rest="$line"
    found_pid=0
    while [[ "$rest" =~ pid=([0-9]+) ]]; do
      pid="${BASH_REMATCH[1]}"
      port_owners["$pid"]="${port_owners[$pid]:-} $port"
      found_pid=1
      rest="${rest#*pid=$pid}"
    done
    ((found_pid)) || hidden_ports["$port"]="$line"
  done < <(ss -H -ltnp 2>/dev/null || true)
}

pid_is_same_running() {
  local pid="$1" expected="$2" line
  [[ -r "/proc/$pid/stat" ]] || return 1
  line="$(<"/proc/$pid/stat")"
  line="${line##*) }"
  read -r -a fields <<< "$line"
  [[ "${fields[19]:-}" == "$expected" && "${fields[0]:-}" != "Z" ]]
}

group_has_live_members() {
  # `ps -g` selects sessions on procps, not the process-group ID. Match the
  # actual PGID and ignore zombies, including after children are reparented.
  ps -eo pgid=,stat= | awk -v wanted="$1" \
    '$1 == wanted && $2 !~ /^Z/ {live=1} END {exit !live}'
}

for path in /proc/[0-9]*; do
  pid="${path##*/}"
  [[ "$pid" != "$$" && "$pid" != "$exclude_pid" ]] || continue
  read_process "$pid" || continue
done

for pid in "${!parent[@]}"; do
  if is_repo_process "$pid"; then
    selected["$pid"]=1
    [[ "${command[$pid]}" == *"eosd"* ]] && eosd["$pid"]=1
  fi
done

# Include the complete process trees so prelaunch services do not need to expose
# pidfiles or retain the shell command from eos.toml in their final argv.
changed=$((!runtime_check))
while ((changed)); do
  changed=0
  for pid in "${!parent[@]}"; do
    ppid="${parent[$pid]}"
    if [[ -n "${selected[$ppid]:-}" && -z "${selected[$pid]:-}" ]]; then
      selected["$pid"]=1
      changed=1
    fi
  done
done

if (( !runtime_check )); then
  discover_port_owners
fi
for port in "${!hidden_ports[@]}"; do
  printf '  UNKNOWN listener port=%s owner PID is hidden; leave running: %s\n' \
    "$port" "${hidden_ports[$port]}" >&2
done
for pid in "${!port_owners[@]}"; do
  [[ -n "${selected[$pid]:-}" ]] || unknown_ports["$pid"]="${port_owners[$pid]}"
done

for pid in "${!unknown_ports[@]}"; do
  printf '  UNKNOWN listener pid=%s ports=%s command=%s\n' \
    "$pid" "${unknown_ports[$pid]}" "${command[$pid]:-unreadable}" >&2
  if ((check_only)); then continue; fi
  if [[ ! -t 0 ]]; then
    printf '  Leave pid=%s running; use an interactive terminal to approve its removal.\n' \
      "$pid" >&2
    continue
  fi
  printf 'Type "kill %s" to stop this unknown listener, or press Enter to leave it: ' "$pid" >&2
  answer=""
  IFS= read -r answer || true
  if [[ "$answer" == "kill $pid" && -n "${start_time[$pid]:-}" ]] &&
     pid_is_same_running "$pid" "${start_time[$pid]}" &&
     ss -H -ltnp 2>/dev/null | grep -Fq "pid=$pid,"; then
    selected["$pid"]=1
    unset 'unknown_ports[$pid]'
  else
    printf '  Leave pid=%s running.\n' "$pid" >&2
  fi
done

# Remember prelaunch process groups before eosd starts reaping their leaders.
for pid in "${!eosd[@]}"; do
  (( !runtime_check )) || break
  for child in "${!selected[@]}"; do
    ancestor="$child"
    while [[ -n "${parent[$ancestor]:-}" && "${parent[$ancestor]}" != "0" ]]; do
      ancestor="${parent[$ancestor]}"
      if [[ "$ancestor" == "$pid" ]]; then
        pgid="$(ps -o pgid= -p "$child" 2>/dev/null || true)"
        pgid="${pgid//[[:space:]]/}"
        if [[ -n "$pgid" && "$pgid" == "$child" && -n "${start_time[$pgid]:-}" ]]; then
          groups["$pgid"]=1
          group_start["$pgid"]="${start_time[$pgid]}"
        fi
        break
      fi
    done
  done
done

# Registries survive an abruptly killed launcher/eosd, when prelaunch groups
# have already been reparented and can no longer be found through ancestry.
shopt -s nullglob
for registry in "$run_dir"/eos-prelaunch-*.pids; do
  (( !runtime_check )) || break
  registry_valid=1
  while read -r pgid recorded_start extra; do
    if [[ ! "$pgid" =~ ^[0-9]+$ || ! "$recorded_start" =~ ^[0-9]+$ || -n "$extra" ]]; then
      printf '  WARN ignoring unsafe legacy/malformed registry entry in %s: %s %s\n' \
        "$registry" "$pgid" "$recorded_start" >&2
      registry_valid=0
      continue
    fi
    if [[ "${start_time[$pgid]:-}" == "$recorded_start" ]] && group_has_live_members "$pgid"; then
      groups["$pgid"]=1
      group_start["$pgid"]="$recorded_start"
    elif group_has_live_members "$pgid"; then
      if [[ -n "${start_time[$pgid]:-}" ]]; then
        # A different leader now owns this ID; the recorded group is gone.
        printf '  WARN ignoring reused prelaunch pgid=%s from %s\n' "$pgid" "$registry" >&2
      else
        # A leaderless group may still contain the original children. Its
        # identity cannot be verified, so preserve the registry for review.
        printf '  WARN retaining leaderless prelaunch pgid=%s from %s\n' "$pgid" "$registry" >&2
        registry_valid=0
      fi
    fi
  done < "$registry"
  ((registry_valid)) || unresolved_registries+=("$registry")
done
shopt -u nullglob

if ((${#selected[@]} == 0 && ${#groups[@]} == 0)); then
  if ((runtime_check)); then
    printf 'No EOS runtime processes from %s are running.\n' "$root"
  else
    printf 'No known EOS processes from %s are running.\n' "$root"
  fi
  if ((${#unknown_ports[@]} > 0 || ${#hidden_ports[@]} > 0)); then
    printf '%d unknown port owner(s) still require review.\n' \
      "$((${#unknown_ports[@]} + ${#hidden_ports[@]}))" >&2
    exit 1
  fi
  if ((${#unresolved_registries[@]} > 0)); then
    printf 'Retained %d unsafe registry file(s) for manual inspection.\n' \
      "${#unresolved_registries[@]}" >&2
    exit 1
  fi
  if (( !check_only )); then
    for registry in "$run_dir"/eos-prelaunch-*.pids; do
      [[ -f "$registry" ]] && rm -f -- "$registry"
    done
  fi
  exit 0
fi

if ((check_only)); then
  if ((runtime_check)); then
    printf 'Found %d EOS runtime process(es) from %s.\n' "${#selected[@]}" "$root" >&2
  else
    printf 'Found %d known EOS process(es), %d prelaunch group(s), and %d unknown port owner(s) from %s.\n' \
      "${#selected[@]}" "${#groups[@]}" "$((${#unknown_ports[@]} + ${#hidden_ports[@]}))" "$root" >&2
  fi
  for pgid in "${!groups[@]}"; do
    printf '  prelaunch pgid=%s\n' "$pgid" >&2
  done
  for pid in "${!selected[@]}"; do
    printf '  pid=%s ports=%s %s\n' "$pid" "${port_owners[$pid]:-none}" "${command[$pid]}" >&2
  done
  exit 1
fi

printf 'Stopping %d EOS process(es) from %s\n' "${#selected[@]}" "$root"
for pid in "${!selected[@]}"; do
  printf '  TERM pid=%s %s\n' "$pid" "${command[$pid]}"
  pid_is_same_running "$pid" "${start_time[$pid]}" && kill -TERM "$pid" 2>/dev/null || true
done
for pgid in "${!groups[@]}"; do
  printf '  TERM prelaunch pgid=%s\n' "$pgid"
  pid_is_same_running "$pgid" "${group_start[$pgid]}" &&
    kill -TERM -- "-$pgid" 2>/dev/null || true
done

deadline=$((SECONDS + timeout))
while ((SECONDS < deadline)); do
  running=0
  for pid in "${!selected[@]}"; do
    [[ -r "/proc/$pid/stat" ]] || continue
    line="$(<"/proc/$pid/stat")"
    line="${line##*) }"
    read -r -a fields <<< "$line"
    [[ "${fields[19]:-}" == "${start_time[$pid]}" && "${fields[0]:-}" != "Z" ]] && running=1
  done
  for pgid in "${!groups[@]}"; do
    pid_is_same_running "$pgid" "${group_start[$pgid]}" &&
      group_has_live_members "$pgid" && running=1
  done
  if ((running == 0)); then
    break
  fi
  sleep 0.1
done

for pgid in "${!groups[@]}"; do
  if group_has_live_members "$pgid" &&
     pid_is_same_running "$pgid" "${group_start[$pgid]}"; then
    kill -KILL -- "-$pgid" 2>/dev/null || true
  fi
done
for pid in "${!selected[@]}"; do
  if [[ -r "/proc/$pid/stat" ]]; then
    line="$(<"/proc/$pid/stat")"
    line="${line##*) }"
    read -r -a fields <<< "$line"
    if [[ "${fields[19]:-}" == "${start_time[$pid]}" && "${fields[0]:-}" != "Z" ]]; then
      printf '  KILL pid=%s %s\n' "$pid" "${command[$pid]}"
      kill -KILL "$pid" 2>/dev/null || true
    fi
  fi
done

survivors=0
for pgid in "${!groups[@]}"; do
  if pid_is_same_running "$pgid" "${group_start[$pgid]}" &&
     group_has_live_members "$pgid"; then
    printf '  ERROR prelaunch pgid=%s survived SIGKILL\n' "$pgid" >&2
    survivors=1
  fi
done
for pid in "${!selected[@]}"; do
  if pid_is_same_running "$pid" "${start_time[$pid]}"; then
    printf '  ERROR pid=%s survived SIGKILL: %s\n' "$pid" "${command[$pid]}" >&2
    survivors=1
  fi
done
if ((survivors == 0 && ${#unresolved_registries[@]} == 0)); then
  for registry in "$run_dir"/eos-prelaunch-*.pids; do
    [[ -f "$registry" ]] && rm -f -- "$registry"
  done
else
  printf 'Retaining prelaunch registries because cleanup is incomplete.\n' >&2
  exit 1
fi
if ((${#unknown_ports[@]} > 0 || ${#hidden_ports[@]} > 0)); then
  printf '%d unknown port owner(s) were left running.\n' \
    "$((${#unknown_ports[@]} + ${#hidden_ports[@]}))" >&2
  exit 1
fi
