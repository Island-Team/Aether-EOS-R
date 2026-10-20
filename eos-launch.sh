#!/usr/bin/env bash
# Supervise the complete development stack: simulator -> EOS -> north HTTP -> REPL.
set -euo pipefail

SCRIPT="$(readlink -f "${BASH_SOURCE[0]}")"
ROOT="$(cd "$(dirname "$SCRIPT")" && pwd)"
NO_REPL=0
PRESERVE_ON_EXIT=0
NO_SIMULATOR=0
SIMULATOR_SELECTION_EXPLICIT=0
SIMULATOR_PORT="${EOS_SIMULATOR_PORT:-16004}"
SIMULATOR_PORT_EXPLICIT=0
[[ -n "${EOS_SIMULATOR_PORT:-}" ]] && SIMULATOR_PORT_EXPLICIT=1
EOS_HOST="127.0.0.1"
EOS_GRPC_PORT="16003"
NORTH_HTTP_HOST="127.0.0.1"
NORTH_HTTP_PORT="16005"
NORTH_HTTP_UPSTREAM="127.0.0.1:16003"
EOS_CONFIG=""
EOS_CONFIG_EXPLICIT=0
EOS_HOST_OVERRIDDEN=0
EOS_PORT_OVERRIDDEN=0
NORTH_HTTP_CONFIG="${EOS_NORTH_HTTP_CONFIG:-$ROOT/etc/eos/north-http.toml}"
NORTH_HTTP_CONFIG_EXPLICIT=0
[[ -n "${EOS_NORTH_HTTP_CONFIG:-}" ]] && NORTH_HTTP_CONFIG_EXPLICIT=1
NORTH_HTTP_HOST_OVERRIDDEN=0
NORTH_HTTP_PORT_OVERRIDDEN=0
EOS_ARGS=()
REPL_ARGS=()
LAUNCH_LOG_FILE=""
EOS_LAUNCH_LOG_DIR=""
PRELAUNCH_PID_FILE=""
CORE_DUMP_LIMIT="${EOS_CORE_DUMP_LIMIT:-0}"
SEGFAULT_HANDLER=""
SEGFAULT_LOG_FILE=""

timestamp() { date -u +'%Y-%m-%dT%H:%M:%S.%3NZ'; }
log() {
  local level="$1"
  shift
  local line
  line="[aether-eos]${level}@launch[$(timestamp)]: $*"
  printf '%s\n' "$line" >&2
  if [[ -n "$LAUNCH_LOG_FILE" ]]; then
    printf '%s\n' "$line" >>"$LAUNCH_LOG_FILE"
  fi
}

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --no-repl) NO_REPL=1; shift ;;
    --preserve-on-exit) PRESERVE_ON_EXIT=1; shift ;;
    --simulator) NO_SIMULATOR=0; SIMULATOR_SELECTION_EXPLICIT=1; shift ;;
    --no-simulator) NO_SIMULATOR=1; SIMULATOR_SELECTION_EXPLICIT=1; shift ;;
    --simulator-port)
      [[ "$#" -ge 2 ]] || { log ERROR "--simulator-port requires a value"; exit 2; }
      SIMULATOR_PORT="$2"; SIMULATOR_PORT_EXPLICIT=1
      SIMULATOR_SELECTION_EXPLICIT=1; shift 2 ;;
    --host)
      [[ "$#" -ge 2 ]] || { log ERROR "--host requires a value"; exit 2; }
      EOS_HOST="$2"; EOS_HOST_OVERRIDDEN=1; EOS_ARGS+=("$1" "$2"); shift 2 ;;
    --grpc-port)
      [[ "$#" -ge 2 ]] || { log ERROR "--grpc-port requires a value"; exit 2; }
      EOS_GRPC_PORT="$2"; EOS_PORT_OVERRIDDEN=1; EOS_ARGS+=("$1" "$2"); shift 2 ;;
    --north-http-host)
      [[ "$#" -ge 2 ]] || { log ERROR "--north-http-host requires a value"; exit 2; }
      NORTH_HTTP_HOST="$2"; NORTH_HTTP_HOST_OVERRIDDEN=1; shift 2 ;;
    --north-http-port)
      [[ "$#" -ge 2 ]] || { log ERROR "--north-http-port requires a value"; exit 2; }
      NORTH_HTTP_PORT="$2"; NORTH_HTTP_PORT_OVERRIDDEN=1; shift 2 ;;
    --north-http-config)
      [[ "$#" -ge 2 ]] || { log ERROR "--north-http-config requires a value"; exit 2; }
      NORTH_HTTP_CONFIG="$2"; NORTH_HTTP_CONFIG_EXPLICIT=1; shift 2 ;;
    --config)
      [[ "$#" -ge 2 ]] || { log ERROR "--config requires a value"; exit 2; }
      EOS_CONFIG="$2"; EOS_CONFIG_EXPLICIT=1; shift 2 ;;
    --repl-arg)
      [[ "$#" -ge 2 ]] || { log ERROR "--repl-arg requires a value"; exit 2; }
      REPL_ARGS+=("$2"); shift 2 ;;
    --help|-h)
      cat <<'EOF'
usage: eos-launch.sh [options] [eosd-options]
  --simulator             start the development simulator explicitly
  --no-simulator          do not start the development simulator
  --no-repl               run the services without an interactive REPL
  --preserve-on-exit      keep EOS prelaunch services when the REPL exits
  --simulator-port N      simulator HTTP/WebSocket port (default 16004)
  --host ADDR             EOS gRPC bind address
  --grpc-port N           EOS gRPC port and north HTTP upstream port
  --north-http-host ADDR  north HTTP bind address (default 127.0.0.1)
  --north-http-port N     north HTTP/SSE port (default 16005)
  --north-http-config PATH
                          north HTTP TOML file
  --config PATH           EOS configuration file
  --repl-arg VALUE        pass one argument to eos-repl (repeatable)

Set EOS_CORE_DUMP_LIMIT=unlimited to request a core dump. When libSegFault is
available, eosd writes a native backtrace to the run's eosd/segfault.log.
EOF
      exit 0 ;;
    *) EOS_ARGS+=("$1"); shift ;;
  esac
done

if [[ "$CORE_DUMP_LIMIT" != "unlimited" && ! "$CORE_DUMP_LIMIT" =~ ^[0-9]+$ ]]; then
  log ERROR "EOS_CORE_DUMP_LIMIT must be 'unlimited' or a non-negative byte limit"
  exit 2
fi

if ! "$ROOT/scripts/kill-all.sh" --check-runtime --exclude-pid "$$" >/dev/null; then
  log ERROR "refusing to start while an EOS launcher, core, simulator, north HTTP, or REPL from this checkout is still running"
  log ERROR "stop the remaining EOS runtime process; $ROOT/scripts/kill-all.sh is for full stack cleanup"
  exit 2
fi

toml_value() {
  local path="$1" section="$2" key="$3"
  awk -v wanted_section="$section" -v wanted_key="$key" '
    /^[[:space:]]*\[\[?[^]]+\]\]?[[:space:]]*([#].*)?$/ {
      current = $0
      sub(/^[[:space:]]*\[\[?/, "", current)
      sub(/\]\]?[[:space:]]*([#].*)?$/, "", current)
      active = current == wanted_section
      next
    }
    active {
      line = $0
      if (line !~ "^[[:space:]]*" wanted_key "[[:space:]]*=") next
      sub("^[[:space:]]*" wanted_key "[[:space:]]*=[[:space:]]*", "", line)
      sub(/[[:space:]]*#[[:space:]].*$/, "", line)
      sub(/[[:space:]]+$/, "", line)
      if (line ~ /^".*"$/ || line ~ /^\047.*\047$/) {
        line = substr(line, 2, length(line) - 2)
      }
      print line
      exit
    }
  ' "$path"
}

absolute_file() {
  local path="$1" label="$2"
  [[ "$path" = /* ]] || path="$PWD/$path"
  path="$(readlink -m "$path")"
  [[ -f "$path" ]] || { log ERROR "$label not found: $path"; return 1; }
  printf '%s\n' "$path"
}

endpoint() {
  local host="$1" port="$2"
  if [[ "$host" == *:* && "$host" != \[*\] ]]; then
    printf '[%s]:%s' "$host" "$port"
  else
    printf '%s:%s' "$host" "$port"
  fi
}

grpc_bind_host() {
  if [[ "$1" == *:* && "$1" != \[*\] ]]; then
    printf '[%s]' "$1"
  else
    printf '%s' "$1"
  fi
}

probe_host() {
  case "$1" in
    0.0.0.0) printf '127.0.0.1' ;;
    ::) printf '::1' ;;
    *) printf '%s' "$1" ;;
  esac
}

http_host() {
  if [[ "$1" == *:* && "$1" != \[*\] ]]; then
    printf '[%s]' "$1"
  else
    printf '%s' "$1"
  fi
}

port_available() {
  python3 - "$1" "$2" <<'PY'
import socket
import sys

host = sys.argv[1].strip("[]")
candidate = int(sys.argv[2])
family = socket.AF_INET6 if ":" in host else socket.AF_INET
with socket.socket(family, socket.SOCK_STREAM) as listener:
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        listener.bind((host, candidate))
    except OSError:
        raise SystemExit(1)
PY
}

port_owner() {
  local port="$1"
  command -v ss >/dev/null 2>&1 || return 0
  ss -ltnp "sport = :$port" 2>/dev/null | awk 'NR > 1 {sub(/^[[:space:]]+/, ""); print; exit}'
}

log_occupied_port() {
  local label="$1" port="$2" owner
  owner="$(port_owner "$port")"
  [[ -z "$owner" ]] || log WARN "$label port $port owner: $owner"
  log WARN "run $ROOT/scripts/kill-all.sh if this is a stale EOS stack"
}

valid_port() {
  [[ "$1" =~ ^[0-9]+$ ]] && ((10#$1 >= 1 && 10#$1 <= 65535))
}

if [[ "$EOS_CONFIG_EXPLICIT" -eq 1 ]]; then
  EOS_CONFIG="$(absolute_file "$EOS_CONFIG" "EOS configuration")"
  EOS_ARGS+=(--config "$EOS_CONFIG")
elif [[ -n "${EOS_ETC:-}" ]]; then
  EOS_CONFIG="$EOS_ETC/eos.toml"
elif [[ -n "${ISLAND_ETC:-}" ]]; then
  EOS_CONFIG="$ISLAND_ETC/eos/eos.toml"
else
  EOS_CONFIG="$ROOT/etc/eos/eos.toml"
fi
if [[ -f "$EOS_CONFIG" ]]; then
  CONFIGURED_EOS_HOST="$(toml_value "$EOS_CONFIG" gateway host)"
  CONFIGURED_EOS_PORT="$(toml_value "$EOS_CONFIG" gateway grpc_port)"
  CONFIGURED_BINDING_BACKEND="$(toml_value "$EOS_CONFIG" mode.binding backend)"
  [[ "$EOS_HOST_OVERRIDDEN" -eq 1 || -z "$CONFIGURED_EOS_HOST" ]] || EOS_HOST="$CONFIGURED_EOS_HOST"
  [[ "$EOS_PORT_OVERRIDDEN" -eq 1 || -z "$CONFIGURED_EOS_PORT" ]] || EOS_GRPC_PORT="$CONFIGURED_EOS_PORT"
  if [[ "$SIMULATOR_SELECTION_EXPLICIT" -eq 0 && -n "$CONFIGURED_BINDING_BACKEND" &&
        "${CONFIGURED_BINDING_BACKEND##*/}" != "demo.toml" ]]; then
    NO_SIMULATOR=1
    log INFO "simulator disabled for hardware binding $CONFIGURED_BINDING_BACKEND"
  fi
fi

# Give every supervised process one shared diagnostic run. Retained launches use
# a sortable unique ID; explicit overwrite mode reuses a fixed, clean `current`
# directory without touching durable state outside the log and attachment trees.
CONFIGURED_LOG_OVERWRITE=""
[[ ! -f "$EOS_CONFIG" ]] || CONFIGURED_LOG_OVERWRITE="$(toml_value "$EOS_CONFIG" log overwrite)"
if [[ -n "${ISLAND_LOG:-}" ]]; then
  [[ "$ISLAND_LOG" = /* ]] || { log ERROR "ISLAND_LOG must be absolute"; exit 2; }
  EOS_LOG_ROOT="$(readlink -m "$ISLAND_LOG/eos")"
else
  EOS_LOG_ROOT="$ROOT/var/log/eos"
fi
EOS_LOG_RUNS="$EOS_LOG_ROOT/runs"
mkdir -p "$EOS_LOG_RUNS"
if [[ "$CONFIGURED_LOG_OVERWRITE" == "true" ]]; then
  EOS_LAUNCH_ID="current"
  CURRENT_LOG_DIR="$EOS_LOG_RUNS/current"
  if [[ -L "$CURRENT_LOG_DIR" ]]; then
    log ERROR "refusing overwrite through symbolic link: $CURRENT_LOG_DIR"
    exit 2
  fi
  mkdir -p "$CURRENT_LOG_DIR"
  find "$CURRENT_LOG_DIR" -mindepth 1 -delete
else
  RANDOM_ID="$(cat /proc/sys/kernel/random/uuid 2>/dev/null || python3 -c 'import uuid; print(uuid.uuid4())')"
  EOS_LAUNCH_ID="$(date -u +'%Y%m%dT%H%M%SZ')-$RANDOM_ID"
fi
export EOS_LAUNCH_ID
EOS_LAUNCH_LOG_DIR="$EOS_LOG_RUNS/$EOS_LAUNCH_ID"
mkdir -p "$EOS_LAUNCH_LOG_DIR"
LAUNCH_LOG_FILE="$EOS_LAUNCH_LOG_DIR/launcher.log"
LATEST_TEMP="$EOS_LOG_ROOT/.latest.$$"
ln -s "runs/$EOS_LAUNCH_ID" "$LATEST_TEMP"
mv -Tf "$LATEST_TEMP" "$EOS_LOG_ROOT/latest"
log INFO "diagnostics run=$EOS_LAUNCH_ID path=$EOS_LAUNCH_LOG_DIR"

if ! ulimit -c "$CORE_DUMP_LIMIT"; then
  log ERROR "could not set core dump limit to $CORE_DUMP_LIMIT"
  exit 2
fi
SEGFAULT_HANDLER="$(ldconfig -p 2>/dev/null | awk '$1 == "libSegFault.so" {print $NF; exit}')"
SEGFAULT_LOG_FILE="$EOS_LAUNCH_LOG_DIR/eosd/segfault.log"
if [[ -n "$SEGFAULT_HANDLER" ]]; then
  mkdir -p "${SEGFAULT_LOG_FILE%/*}"
  log INFO "crash tracing enabled handler=$SEGFAULT_HANDLER output=$SEGFAULT_LOG_FILE core_limit=$CORE_DUMP_LIMIT"
else
  log WARN "native crash tracing unavailable; libSegFault.so was not found core_limit=$CORE_DUMP_LIMIT"
fi

NORTH_HTTP_CONFIG="$(absolute_file "$NORTH_HTTP_CONFIG" "north HTTP configuration")"
CONFIGURED_NORTH_HTTP_HOST="$(toml_value "$NORTH_HTTP_CONFIG" north_http host)"
CONFIGURED_NORTH_HTTP_PORT="$(toml_value "$NORTH_HTTP_CONFIG" north_http port)"
CONFIGURED_NORTH_HTTP_UPSTREAM="$(toml_value "$NORTH_HTTP_CONFIG" north_http upstream)"
[[ "$NORTH_HTTP_HOST_OVERRIDDEN" -eq 1 || -z "$CONFIGURED_NORTH_HTTP_HOST" ]] || \
  NORTH_HTTP_HOST="$CONFIGURED_NORTH_HTTP_HOST"
[[ "$NORTH_HTTP_PORT_OVERRIDDEN" -eq 1 || -z "$CONFIGURED_NORTH_HTTP_PORT" ]] || \
  NORTH_HTTP_PORT="$CONFIGURED_NORTH_HTTP_PORT"
[[ -z "$CONFIGURED_NORTH_HTTP_UPSTREAM" ]] || NORTH_HTTP_UPSTREAM="$CONFIGURED_NORTH_HTTP_UPSTREAM"

valid_port "$EOS_GRPC_PORT" || {
  log ERROR "EOS gRPC port must be an integer from 1 to 65535: $EOS_GRPC_PORT"
  exit 2
}
if ! port_available "$EOS_HOST" "$EOS_GRPC_PORT"; then
  log_occupied_port "EOS gRPC" "$EOS_GRPC_PORT"
  log ERROR "EOS gRPC port is already in use: $(endpoint "$EOS_HOST" "$EOS_GRPC_PORT")"
  exit 2
fi

valid_port "$NORTH_HTTP_PORT" || {
  log ERROR "north HTTP port must be an integer from 1 to 65535: $NORTH_HTTP_PORT"
  exit 2
}
if ! port_available "$NORTH_HTTP_HOST" "$NORTH_HTTP_PORT"; then
  log_occupied_port "north HTTP" "$NORTH_HTTP_PORT"
  log ERROR "north HTTP port is already in use: $(endpoint "$NORTH_HTTP_HOST" "$NORTH_HTTP_PORT")"
  exit 2
fi

EOS_CONNECT_HOST="$(probe_host "$EOS_HOST")"
EOS_ARGS+=(--host "$(grpc_bind_host "$EOS_HOST")" --grpc-port "$EOS_GRPC_PORT")
if [[ "$EOS_HOST_OVERRIDDEN" -eq 1 || "$EOS_PORT_OVERRIDDEN" -eq 1 || \
      ( "$EOS_CONFIG_EXPLICIT" -eq 1 && "$NORTH_HTTP_CONFIG_EXPLICIT" -eq 0 ) ]]; then
  NORTH_HTTP_UPSTREAM="$(endpoint "$EOS_CONNECT_HOST" "$EOS_GRPC_PORT")"
fi
NORTH_HTTP_CONNECT_HOST="$(probe_host "$NORTH_HTTP_HOST")"
NORTH_HTTP_HTTP_HOST="$(http_host "$NORTH_HTTP_CONNECT_HOST")"

# gRPC C-core reads the lowercase proxy bypass variables. Keep local EOS
# traffic out of an HTTP proxy when the environment only provides NO_PROXY.
if [[ -n "${NO_PROXY:-}" ]]; then
  export no_proxy="${no_proxy:-$NO_PROXY}"
  export no_grpc_proxy="${no_grpc_proxy:-$NO_PROXY}"
fi

valid_port "$SIMULATOR_PORT" || {
  log ERROR "simulator port must be an integer from 1 to 65535: $SIMULATOR_PORT"
  exit 2
}
if [[ "$NO_SIMULATOR" -eq 0 ]] && ! port_available "127.0.0.1" "$SIMULATOR_PORT"; then
  log_occupied_port "simulator" "$SIMULATOR_PORT"
  log ERROR "simulator port is already in use: 127.0.0.1:$SIMULATOR_PORT"
  exit 2
fi

if [[ "$EOS_GRPC_PORT" == "$NORTH_HTTP_PORT" ]]; then
  log ERROR "EOS gRPC and north HTTP must use different ports: $EOS_GRPC_PORT"
  exit 2
fi
if [[ "$NO_SIMULATOR" -eq 0 ]] &&
   { [[ "$SIMULATOR_PORT" == "$EOS_GRPC_PORT" ]] ||
     [[ "$SIMULATOR_PORT" == "$NORTH_HTTP_PORT" ]]; }; then
  log ERROR "simulator, EOS gRPC, and north HTTP ports must be distinct"
  exit 2
fi

if [[ -n "${ISLAND_BUILD_DIR:-}" ]]; then
  [[ "$ISLAND_BUILD_DIR" = /* ]] || { log ERROR "ISLAND_BUILD_DIR must be absolute"; exit 2; }
  BUILD_DIR="$ISLAND_BUILD_DIR"
else
  BUILD_DIR="$("$ROOT/eos-build.sh" --print-build-dir)"
fi
for program in "$BUILD_DIR/src/eos/bin/eosd" \
               "$BUILD_DIR/src/eos/ext/north_http/eos-north-http"; do
  [[ -x "$program" ]] || { log ERROR "missing build product: $program"; exit 2; }
done
if [[ "$NO_SIMULATOR" -eq 0 && ! -x "$BUILD_DIR/src/eos/bin/eos-simulator" ]]; then
  log ERROR "missing simulator build product; run $ROOT/eos-build.sh"
  exit 2
fi
if [[ "$NO_REPL" -eq 0 && ! -x "$BUILD_DIR/src/eos/ext/repl/eos-repl" ]]; then
  log ERROR "missing REPL build product; run $ROOT/eos-build.sh"
  exit 2
fi

SIMULATOR_PID=""
EOSD_PID=""
NORTH_HTTP_PID=""
REPL_PID=""
group_live() {
  ps -eo pgid=,stat= | awk -v wanted="$1" \
    '$1 == wanted && $2 !~ /^Z/ {live=1} END {exit !live}'
}

owned_prelaunch_group_live() {
  local group="$1" expected_start="${prelaunch_start[$1]:-}" leader_line
  local -a leader_fields=()
  [[ -n "$expected_start" && -r "/proc/$group/stat" ]] || return 1
  leader_line="$(<"/proc/$group/stat")"
  leader_line="${leader_line##*) }"
  read -r -a leader_fields <<< "$leader_line"
  [[ "${leader_fields[19]:-}" == "$expected_start" ]] && group_live "$group"
}

prelaunch_group_ambiguous() {
  local group="$1"
  [[ ! -r "/proc/$group/stat" ]] && group_live "$group"
}

cleanup() {
  local status=$?
  trap - EXIT INT TERM
  local -a items=("repl:$REPL_PID" "north-http:$NORTH_HTTP_PID" "eosd:$EOSD_PID"
                  "simulator:$SIMULATOR_PID")
  local -a prelaunch_groups=()
  local -A prelaunch_start=()
  local item name pid group state any_running deadline
  if [[ -n "$PRELAUNCH_PID_FILE" && -f "$PRELAUNCH_PID_FILE" ]]; then
    while read -r group group_start; do
      [[ "$group" =~ ^[0-9]+$ && "$group_start" =~ ^[0-9]+$ ]] &&
        { prelaunch_groups+=("$group"); prelaunch_start["$group"]="$group_start"; }
    done < "$PRELAUNCH_PID_FILE"
  fi
  if [[ -n "$EOSD_PID" ]] && kill -0 "$EOSD_PID" 2>/dev/null; then
    while read -r pid group; do
      [[ -n "$pid" && "$pid" == "$group" ]] && prelaunch_groups+=("$group")
    done < <(ps -o pid=,pgid= --ppid "$EOSD_PID" 2>/dev/null || true)
  fi
  for item in "${items[@]}"; do
    name="${item%%:*}"; pid="${item#*:}"
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      log INFO "stopping $name pid=$pid"
      kill -TERM "$pid" 2>/dev/null || true
    fi
  done
  # eosd owns prelaunch process groups and needs time to drain their children.
  deadline=$((SECONDS + 15))
  while ((SECONDS < deadline)); do
    any_running=0
    for item in "${items[@]}"; do
      pid="${item#*:}"
      [[ -n "$pid" ]] || continue
      if kill -0 "$pid" 2>/dev/null; then
        state="$(ps -o stat= -p "$pid" 2>/dev/null || true)"
        [[ "$state" == Z* ]] || any_running=1
      fi
    done
    [[ "$any_running" -eq 1 ]] || break
    sleep 0.1
  done
  for group in "${prelaunch_groups[@]}"; do
    if owned_prelaunch_group_live "$group"; then
      log WARN "killing unresponsive eosd prelaunch group pgid=$group"
      kill -KILL -- "-${group}" 2>/dev/null || true
    fi
  done
  for item in "${items[@]}"; do
    name="${item%%:*}"; pid="${item#*:}"
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      state="$(ps -o stat= -p "$pid" 2>/dev/null || true)"
      if [[ "$state" != Z* ]]; then
        log WARN "killing unresponsive $name pid=$pid"
        kill -KILL "$pid" 2>/dev/null || true
      fi
    fi
  done
  for item in "${items[@]}"; do
    pid="${item#*:}"
    [[ -z "$pid" ]] || wait "$pid" 2>/dev/null || true
  done
  if [[ -n "$PRELAUNCH_PID_FILE" && -f "$PRELAUNCH_PID_FILE" ]]; then
    local retained=0
    for group in "${prelaunch_groups[@]}"; do
      if owned_prelaunch_group_live "$group" || prelaunch_group_ambiguous "$group"; then
        retained=1
        break
      fi
    done
    if ((retained)); then
      log WARN "retaining prelaunch registry for still-running group(s): $PRELAUNCH_PID_FILE"
    else
      rm -f -- "$PRELAUNCH_PID_FILE"
    fi
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

report_service_failure() {
  local service="$1" directory="$EOS_LAUNCH_LOG_DIR/$1" line file
  log ERROR "$service diagnostics: $directory"
  if [[ -f "$directory/main.log" ]]; then
    while IFS= read -r line; do
      log ERROR "$service root-cause: $line"
    done < <(grep -E 'ERROR|FATAL|runtime_binding_failed|daemon_start_failed' \
      "$directory/main.log" | tail -n 20)
    while IFS= read -r line; do
      log ERROR "$service: $line"
    done < <(tail -n 50 "$directory/main.log")
  fi
  if [[ -f "$directory/adapter.log" ]]; then
    log ERROR "$service adapter diagnostics: $directory/adapter.log"
    while IFS= read -r line; do
      log ERROR "$service adapter: $line"
    done < <(tail -n 40 "$directory/adapter.log")
  fi
  for file in "$directory"/prelaunch-*.log; do
    [[ -f "$file" ]] || continue
    log ERROR "$service prelaunch diagnostics: $file"
    while IFS= read -r line; do
      log ERROR "$service prelaunch: $line"
    done < <(tail -n 20 "$file")
  done
}

report_recent_events() {
  local service="$1" file="$EOS_LAUNCH_LOG_DIR/$1/events.jsonl"
  [[ -r "$file" ]] || return 0
  while IFS= read -r line; do
    log ERROR "$service recent event: $line"
  done < <(python3 - "$file" <<'PY'
import json
import sys

path = sys.argv[1]
try:
    with open(path, encoding="utf-8") as events:
        relevant = []
        for raw in events:
            try:
                event = json.loads(raw)
            except (json.JSONDecodeError, UnicodeDecodeError):
                continue
            if event.get("level") in {"warning", "error", "critical"}:
                relevant.append({
                    "ts": event.get("ts"),
                    "level": event.get("level"),
                    "event": event.get("event"),
                    "msg": event.get("msg"),
                })
        for event in relevant[-20:]:
            print(json.dumps(event, separators=(",", ":"), ensure_ascii=False))
except OSError as error:
    print(f"could not read recent events from {path}: {error}")
PY
  )
}

start_eosd() {
  if [[ -n "$SEGFAULT_HANDLER" ]]; then
    SEGFAULT_OUTPUT_NAME="$SEGFAULT_LOG_FILE" \
      LD_PRELOAD="$SEGFAULT_HANDLER${LD_PRELOAD:+:$LD_PRELOAD}" \
      "$ROOT/bin/eosd" "$@" </dev/null &
  else
    "$ROOT/bin/eosd" "$@" </dev/null &
  fi
  EOSD_PID=$!
}

process_running() {
  local pid="$1" state
  kill -0 "$pid" 2>/dev/null || return 1
  state="$(ps -o stat= -p "$pid" 2>/dev/null || true)"
  [[ -n "$state" && "$state" != Z* ]]
}

wait_tcp() {
  local host="$1" port="$2" pid="$3" name="$4"
  for _ in $(seq 1 300); do
    if ! process_running "$pid"; then
      log ERROR "$name exited before readiness"
      report_service_failure "$name"
      return 1
    fi
    if (exec 9<>"/dev/tcp/$host/$port") 2>/dev/null; then exec 9>&-; return 0; fi
    sleep 0.1
  done
  log ERROR "$name readiness timed out at $host:$port"
  report_service_failure "$name"
  return 1
}

wait_http() {
  local url="$1" pid="$2" name="$3" dependency_pid="${4:-}"
  for _ in $(seq 1 600); do
    if ! process_running "$pid"; then
      log ERROR "$name exited before readiness"
      report_service_failure "$name"
      return 1
    fi
    if [[ -n "$dependency_pid" ]] && ! process_running "$dependency_pid"; then
      log ERROR "EOS exited before $name became ready"
      report_service_failure eosd
      return 1
    fi
    curl --noproxy '*' --fail --silent --show-error --max-time 1 "$url" >/dev/null 2>&1 && return 0
    sleep 0.1
  done
  log ERROR "$name readiness timed out at $url"
  report_service_failure "$name"
  return 1
}

wait_unix() {
  local path="$1" pid="$2" name="$3"
  for _ in $(seq 1 300); do
    if ! process_running "$pid"; then
      log ERROR "$name exited before Unix socket readiness"
      report_service_failure "$name"
      return 1
    fi
    if [[ -S "$path" ]] && python3 - "$path" <<'PY'
import socket
import sys

client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.settimeout(0.2)
try:
    client.connect(sys.argv[1])
except OSError:
    raise SystemExit(1)
finally:
    client.close()
PY
    then
      return 0
    fi
    sleep 0.1
  done
  log ERROR "$name readiness timed out at $path"
  report_service_failure "$name"
  return 1
}

export ISLAND_BUILD_DIR="$BUILD_DIR"
ISLAND_RUN="${ISLAND_RUN:-$ROOT/var/run}"
mkdir -p "$ISLAND_RUN"
PRELAUNCH_PID_FILE="$ISLAND_RUN/eos-prelaunch-$$.pids"
: > "$PRELAUNCH_PID_FILE"
export EOS_PRELAUNCH_PID_FILE="$PRELAUNCH_PID_FILE"
REPL_STDIN_FD=""
if [[ "$NO_REPL" -eq 0 ]]; then
  exec {REPL_STDIN_FD}<&0
fi
if [[ "$NO_SIMULATOR" -eq 0 ]]; then
  log INFO "starting simulator HTTP=127.0.0.1:$SIMULATOR_PORT"
  "$ROOT/bin/eos-simulator" --port "$SIMULATOR_PORT" </dev/null & SIMULATOR_PID=$!
  wait_tcp 127.0.0.1 "$SIMULATOR_PORT" "$SIMULATOR_PID" simulator
  wait_unix "$ISLAND_RUN/simulator.sock" "$SIMULATOR_PID" simulator
  log INFO "simulator ready pid=$SIMULATOR_PID"
fi

# head_find_trash ships pre-synthesized announcements; mirror them into the
# EOS asset root so speaker.play can resolve their asset:// URIs at runtime.
# bin/eosd derives its state root from ISLAND_STATE (default: var/lib), so the
# same precedence is used here.
SKILL_ASSETS_SRC="$ROOT/share/skills/native/head_find_trash/assets"
EOS_STATE_ROOT="${ISLAND_STATE:-$ROOT/var/lib}/eos"
if [[ -d "$SKILL_ASSETS_SRC" ]]; then
  mkdir -p "$EOS_STATE_ROOT/assets/head_find_trash"
  for wav in "$SKILL_ASSETS_SRC"/*.wav; do
    [[ -f "$wav" ]] || continue
    target="$EOS_STATE_ROOT/assets/head_find_trash/$(basename "$wav")"
    cmp -s "$wav" "$target" || cp "$wav" "$target"
  done
fi

log INFO "starting eosd gRPC=$EOS_HOST:$EOS_GRPC_PORT"
start_eosd "${EOS_ARGS[@]}"
wait_tcp "$EOS_CONNECT_HOST" "$EOS_GRPC_PORT" "$EOSD_PID" eosd
log INFO "eosd ready pid=$EOSD_PID"

log INFO "starting North HTTP listen=$(endpoint "$NORTH_HTTP_HOST" "$NORTH_HTTP_PORT") upstream=$NORTH_HTTP_UPSTREAM"
"$ROOT/bin/eos-north-http" --config "$NORTH_HTTP_CONFIG" --host "$NORTH_HTTP_HOST" \
  --port "$NORTH_HTTP_PORT" --upstream "$NORTH_HTTP_UPSTREAM" </dev/null &
NORTH_HTTP_PID=$!
wait_http "http://$NORTH_HTTP_HTTP_HOST:$NORTH_HTTP_PORT/health/live" "$NORTH_HTTP_PID" north-http "$EOSD_PID"
if curl --noproxy '*' --fail --silent --show-error --max-time 1 \
    "http://$NORTH_HTTP_HTTP_HOST:$NORTH_HTTP_PORT/health/ready" >/dev/null 2>&1; then
  log INFO "EOS and north HTTP ready"
else
  log WARN "north HTTP is live but deployment readiness is pending; starting REPL"
fi

start_repl() {
  log INFO "starting eos-repl"
  "$ROOT/bin/eos-repl" --host "$NORTH_HTTP_CONNECT_HOST" --port "$NORTH_HTTP_PORT" \
    --config "$NORTH_HTTP_CONFIG" --eosd-pid "$EOSD_PID" \
    "${REPL_ARGS[@]}" <&"$REPL_STDIN_FD" &
  REPL_PID=$!
}

supervised_pids() {
  PIDS=("$EOSD_PID" "$NORTH_HTTP_PID")
  [[ -n "$SIMULATOR_PID" ]] && PIDS+=("$SIMULATOR_PID")
  [[ -n "$REPL_PID" ]] && PIDS+=("$REPL_PID")
  # The final [[ ]] && list returns 1 when the REPL/simulator is disabled,
  # which would trip set -e at the call site.
  return 0
}

if [[ "$NO_REPL" -eq 0 ]]; then
  start_repl
fi
supervised_pids

while true; do
  set +e
  FINISHED_PID=""
  wait -n -p FINISHED_PID "${PIDS[@]}"
  STATUS=$?
  set -e
  if [[ "$FINISHED_PID" == "$REPL_PID" && "$STATUS" -eq 77 ]]; then
    log INFO "REPL exited; leaving EOS running and returning to the shell"
    REPL_PID=""
    # Relinquish supervision without running cleanup. The service processes
    # remain reachable through scripts/kill-all.sh.
    trap - EXIT INT TERM
    exit 0
  fi
  if [[ "$FINISHED_PID" == "$REPL_PID" && "$STATUS" -eq 76 ]]; then
    if [[ "$PRESERVE_ON_EXIT" -eq 0 ]]; then
      log INFO "REPL requested exit; stopping EOS and its prelaunch services"
      exit 0
    fi
    log INFO "REPL requested exit; preserving EOS prelaunch services as requested"
    REPL_PID=""
    # eosd owns prelaunch handles, so bypass its destructor. The cleanup trap
    # still stops North HTTP and the simulator, but must not read or remove the
    # retained prelaunch registry.
    kill -KILL "$EOSD_PID" 2>/dev/null || true
    wait "$EOSD_PID" 2>/dev/null || true
    EOSD_PID=""
    PRELAUNCH_PID_FILE=""
    exit 0
  fi
  if [[ "$FINISHED_PID" == "$REPL_PID" && "$STATUS" -eq 75 ]]; then
    log INFO "REPL requested rebuild/restart of eosd and eos-repl"
    REPL_PID=""
    if ! cmake --build "$BUILD_DIR" --target eosd eos_repl --parallel; then
      log ERROR "rebuild failed; leaving eosd and prelaunch running"
      start_repl
      supervised_pids
      continue
    fi
    log INFO "rebuild complete; replacing eosd without launching prelaunch"
    kill -KILL "$EOSD_PID"
    wait "$EOSD_PID" 2>/dev/null || true
    start_eosd "${EOS_ARGS[@]}" --no-prelaunch
    wait_tcp "$EOS_CONNECT_HOST" "$EOS_GRPC_PORT" "$EOSD_PID" eosd
    log INFO "eosd restarted pid=$EOSD_PID prelaunch=disabled"
    start_repl
    supervised_pids
    continue
  fi
  if ((STATUS >= 128 && STATUS <= 192)); then
    SIGNAL_NUMBER=$((STATUS - 128))
    SIGNAL_NAME="$(kill -l "$SIGNAL_NUMBER" 2>/dev/null || printf unknown)"
    log ERROR "stack process terminated by signal pid=$FINISHED_PID signal=SIG$SIGNAL_NAME number=$SIGNAL_NUMBER core_limit=$CORE_DUMP_LIMIT; stopping remaining processes"
    if [[ "$FINISHED_PID" == "$EOSD_PID" ]]; then
      report_service_failure eosd
      report_recent_events eosd
      if [[ -r "$SEGFAULT_LOG_FILE" ]]; then
        while IFS= read -r line; do
          log ERROR "eosd native crash trace: $line"
        done < <(tail -n 80 "$SEGFAULT_LOG_FILE")
      fi
    fi
  else
    log "$([[ "$STATUS" -eq 0 ]] && printf INFO || printf ERROR)" \
      "stack process exited pid=$FINISHED_PID status=$STATUS; stopping remaining processes"
  fi
  exit "$STATUS"
done
