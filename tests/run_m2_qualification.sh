#!/usr/bin/env bash
# Run the exact merged M2 fixture in bounded, independently logged batches.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
projects="$(cd "$root/.." && pwd)"
: "${MUJOCO_LIBRARY_ROOT:?set MUJOCO_LIBRARY_ROOT to the MuJoCo 3.16.0 runtime root}"
run_id="${M2_LOG_ID:-$(date -u +%Y%m%d-%H%M%S)-m2-qualification}"
artifacts="$root/logs/test/$run_id"
if [[ -e "$artifacts" ]]; then
  echo "artifact directory already exists: $artifacts" >&2
  exit 2
fi
mkdir -p "$artifacts"
exec > >(tee "$artifacts/replay.log") 2>&1

mapfile -t heads < <(python3 - "$root/tests/fixtures/m2_qualification/cases.json" <<'PY'
import json
import sys
baseline = json.load(open(sys.argv[1], encoding="utf-8"))["baseline"]
for name in ("eos", "isos", "egia", "mujoco", "isosui"):
    print(baseline[name])
PY
)
[[ "${#heads[@]}" -eq 5 ]] || { echo "incomplete M2 baseline" >&2; exit 2; }

common=(
  --mujoco-library-root "$MUJOCO_LIBRARY_ROOT"
  --eos-root "${EOS_ROOT:-$projects/Island-eos}"
  --isos-root "${ISOS_ROOT:-$projects/isos}"
  --egia-root "${EGIA_ROOT:-$projects/Island-egia}"
  --mujoco-root "${MUJOCO_ROOT:-$projects/isos-mujoco-sim}"
  --isosui-root "${ISOSUI_ROOT:-$projects/IsosUI}"
  --expect-eos "${heads[0]}"
  --expect-isos "${heads[1]}"
  --expect-egia "${heads[2]}"
  --expect-mujoco "${heads[3]}"
  --expect-isosui "${heads[4]}"
)
names=(a b1 b2 c d e)
cases=(
  M2-01,M2-02,M2-03,M2-04,M2-05,M2-06,M2-07
  M2-08,M2-09,M2-10,M2-11
  M2-12,M2-13,M2-14
  M2-15,M2-16,M2-17
  M2-18,M2-19,M2-20,M2-21
  M2-22,M2-23,M2-24,M2-25,M2-26,M2-27,M2-28
)
batch_dirs=()
for index in "${!names[@]}"; do
  directory="$artifacts/${names[index]}"
  batch_dirs+=("$directory")
  python3 "$root/tests/m2_cross_repo_replay.py" --run --only "${cases[index]}" \
    "${common[@]}" --artifacts "$directory"
done
python3 "$root/tests/m2_cross_repo_replay.py" --aggregate-batches "${batch_dirs[@]}" \
  "${common[@]}" --artifacts "$artifacts/gate"
