#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: mic.sh [-d SECONDS] [-p PATH]

Record the system default microphone to a WAV file. Recording continues until
Ctrl-C unless a duration is specified.

Options:
  -d, --duration SECONDS  Stop after this many seconds
  -p, --path PATH         Output WAV path (default: output.wav)
  -h, --help              Show this help message
EOF
}

duration=""
output_path="output.wav"

while (($#)); do
  case "$1" in
    -d|--duration)
      if (($# < 2)); then
        printf 'mic.sh: %s requires a value\n' "$1" >&2
        exit 2
      fi
      duration="$2"
      shift 2
      ;;
    -p|--path)
      if (($# < 2)); then
        printf 'mic.sh: %s requires a value\n' "$1" >&2
        exit 2
      fi
      output_path="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      printf 'mic.sh: unknown option: %s\n' "$1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -n "$duration" && ! "$duration" =~ ^[1-9][0-9]*$ ]]; then
  printf 'mic.sh: duration must be a positive whole number of seconds\n' >&2
  exit 2
fi

if [[ -z "$output_path" ]]; then
  printf 'mic.sh: output path must not be empty\n' >&2
  exit 2
fi

if ! command -v arecord >/dev/null 2>&1; then
  printf 'mic.sh: arecord is not installed (install alsa-utils)\n' >&2
  exit 127
fi

args=(-q -D default -t wav)
if [[ -n "$duration" ]]; then
  args+=(-d "$duration")
fi

exec arecord "${args[@]}" -- "$output_path"
