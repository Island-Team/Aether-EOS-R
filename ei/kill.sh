#!/usr/bin/env bash
# Stop scoped Island checkout processes without touching unrelated system services.
set -euo pipefail

SCRIPT="$(readlink -f "${BASH_SOURCE[0]}")"
ROOT="$(cd "$(dirname "$SCRIPT")" && pwd)"
EOS_ROOT="$(readlink -f "${ISLAND_EOS_PREFIX:-$ROOT/..}")"
DRY_RUN=0
TIMEOUT=5
SCOPE="ei"

usage() {
  cat <<EOF
Usage: ./kill.sh [--os | --all] [--dry-run] [--timeout SECONDS]

Stop processes launched from this checkout. The default scope is EI only.

Scopes:
  default             EI and its child processes
  --os                EI, EOS, and their child processes
  --all               --os plus checkout SSH tunnels and all descendants,
                      including ROS backend children started by EOS

Root processes must have command lines below:
  $ROOT
  $EOS_ROOT

Options:
  --os                 Stop EI and EOS
  --all                Stop EI, EOS, checkout SSH tunnels, and their children
  -n, --dry-run         List matching processes without sending signals
  -t, --timeout SECONDS Wait after SIGTERM before SIGKILL (default: 5)
  -h, --help            Show this help
EOF
}

die() { echo "kill.sh: $*" >&2; exit 2; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --os) SCOPE="os"; shift ;;
    --all) SCOPE="all"; shift ;;
    -n|--dry-run) DRY_RUN=1; shift ;;
    -t|--timeout)
      [[ $# -ge 2 && "$2" =~ ^[0-9]+$ ]] || die "--timeout requires whole seconds"
      TIMEOUT="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1 (see --help)" ;;
  esac
done

declare -A PIDS=()
declare -A PARENTS=()
while read -r pid parent command; do
  [[ "$pid" =~ ^[0-9]+$ && "$parent" =~ ^[0-9]+$ && "$pid" != "$$" ]] || continue
  PARENTS["$pid"]="$parent"
  if [[ "$command" == *"$ROOT/"*"-m ei.main"* || "$command" == *"$ROOT/"*"-m egia.main"* || "$command" == *"$ROOT/"*"-m egia_ext.tui"* ]]; then
    PIDS["$pid"]=1
  fi
  if [[ "$SCOPE" != "ei" && "$command" == *"$EOS_ROOT/"*"eosd"* ]]; then
    PIDS["$pid"]=1
  fi
  if [[ "$SCOPE" == "all" && "$command" == *"$ROOT/"*"ssh-tunnel"* ]]; then
    PIDS["$pid"]=1
  fi
done < <(ps -eo pid=,ppid=,args=)

# Selected launchers own their descendant EOS, ROS, and SSH child processes.
changed=1
while [[ "$changed" -eq 1 ]]; do
  changed=0
  for pid in "${!PARENTS[@]}"; do
    if [[ -n "${PIDS[${PARENTS[$pid]}]:-}" && -z "${PIDS[$pid]:-}" ]]; then
      PIDS["$pid"]=1
      changed=1
    fi
  done
done

if [[ ${#PIDS[@]} -eq 0 ]]; then
  echo "kill.sh: no checkout processes found for scope: $SCOPE"
  exit 0
fi

PIDS_KEYS=("${!PIDS[@]}")
if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "kill.sh: dry run ($SCOPE) - would stop PIDs: ${PIDS_KEYS[*]}"
  ps -o pid=,stat=,args= -p "$(IFS=,; echo "${PIDS_KEYS[*]}")"
  exit 0
fi

echo "kill.sh: sending SIGTERM ($SCOPE) to PIDs: ${PIDS_KEYS[*]}"
kill -TERM "${PIDS_KEYS[@]}" 2>/dev/null || true
for ((second = 0; second < TIMEOUT; second++)); do
  alive=0
  for pid in "${PIDS_KEYS[@]}"; do
    kill -0 "$pid" 2>/dev/null && alive=1
  done
  [[ "$alive" -eq 0 ]] && exit 0
  sleep 1
done

for pid in "${PIDS_KEYS[@]}"; do
  if kill -0 "$pid" 2>/dev/null; then
    echo "kill.sh: PID $pid did not exit - sending SIGKILL"
    kill -KILL "$pid" 2>/dev/null || true
  fi
done
