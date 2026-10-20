#!/usr/bin/env bash
# Configure and build EOS in the native host architecture's isolated tree.

# Some wrappers invoke build scripts through `sh`, which ignores this file's
# Bash shebang. Re-exec before the POSIX shell reaches Bash-only arrays,
# `[[ ... ]]`, or `case` patterns.
if [ -z "${BASH_VERSION:-}" ]; then
  exec bash "$0" "$@"
fi

set -euo pipefail

SCRIPT="$(readlink -f "${BASH_SOURCE[0]}")"
ROOT="$(cd "$(dirname "$SCRIPT")" && pwd)"
CLEAN_BUILD=0
PRINT_BUILD_DIR=0
SKIP_GRPC_INSTALL=0
CONFIG="$ROOT/etc/eos/eos.toml"

usage() {
  cat <<'EOF'
Usage: ./eos-build.sh [--clean] [--print-build-dir] [--skip-grpc-install]

Build EOS for the native x86_64 or ARM64 host.

  --clean  Remove the selected build tree before rebuilding.
  --print-build-dir  Print the selected build tree and exit.
  --skip-grpc-install  Do not install prebuilt gRPC packages with apt.
  --help   Show this help message.
EOF
}

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --clean)
      CLEAN_BUILD=1
      ;;
    --print-build-dir)
      PRINT_BUILD_DIR=1
      ;;
    --skip-grpc-install)
      SKIP_GRPC_INSTALL=1
      ;;
    --help | -h)
      usage
      exit 0
      ;;
    *)
      printf 'unknown option: %s\n' "$1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

if [[ ! -f "$CONFIG" ]]; then
  printf 'EOS configuration not found: %s\n' "$CONFIG" >&2
  printf 'create a local configuration with: cp %s/etc/eos/eos.example.toml %s\n' \
    "$ROOT" "$CONFIG" >&2
  exit 2
fi

positive_integer() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

minimum() {
  if (( "$1" < "$2" )); then
    printf '%s\n' "$1"
  else
    printf '%s\n' "$2"
  fi
}

CPU_COUNT="$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1\n')"
positive_integer "$CPU_COUNT" || CPU_COUNT=1

# A C++ compiler process can exceed 1 GiB on the gRPC/protobuf sources. Keep
# one 1.5 GiB allowance per job and cap the default to avoid swap-heavy builds.
MEMORY_JOBS="$CPU_COUNT"
if [[ -r /proc/meminfo ]]; then
  AVAILABLE_KIB="$(awk '/^MemAvailable:/ { print $2; exit }' /proc/meminfo)"
  if positive_integer "${AVAILABLE_KIB:-}"; then
    MEMORY_JOBS=$((AVAILABLE_KIB / 1572864))
    (( MEMORY_JOBS > 0 )) || MEMORY_JOBS=1
  fi
fi

DEFAULT_JOBS="$(minimum "$CPU_COUNT" "$MEMORY_JOBS")"
DEFAULT_JOBS="$(minimum "$DEFAULT_JOBS" 8)"
BUILD_JOBS="${AETHER_BUILD_JOBS:-${CMAKE_BUILD_PARALLEL_LEVEL:-$DEFAULT_JOBS}}"
positive_integer "$BUILD_JOBS" || {
  printf 'AETHER_BUILD_JOBS must be a positive integer (got %q)\n' "$BUILD_JOBS" >&2
  exit 2
}

DEFAULT_DEPENDENCY_JOBS="$(minimum "$BUILD_JOBS" 4)"
DEPENDENCY_JOBS="${AETHER_DEPENDENCY_JOBS:-$DEFAULT_DEPENDENCY_JOBS}"
positive_integer "$DEPENDENCY_JOBS" || {
  printf 'AETHER_DEPENDENCY_JOBS must be a positive integer (got %q)\n' "$DEPENDENCY_JOBS" >&2
  exit 2
}

timestamp() {
  date -u +'%Y-%m-%dT%H:%M:%S.%3NZ'
}

log() {
  local level="$1"
  shift
  printf '[aether-eos]%s@build[%s]: %s\n' "$level" "$(timestamp)" "$*" >&2
}

system_grpc_available() {
  command -v pkg-config >/dev/null 2>&1 &&
    pkg-config --exists grpc++ &&
    command -v grpc_cpp_plugin >/dev/null 2>&1 &&
    command -v ldconfig >/dev/null 2>&1 &&
    grep -q 'libgrpc++_reflection' <<<"$(ldconfig -p 2>/dev/null)"
}

install_system_grpc() {
  if [[ "$SKIP_GRPC_INSTALL" -eq 1 ]]; then
    log INFO "system gRPC provisioning disabled; source fallback may be built"
    return
  fi
  if system_grpc_available; then
    log INFO "using prebuilt system gRPC version=$(pkg-config --modversion grpc++)"
    return
  fi
  if ! command -v apt-get >/dev/null 2>&1; then
    log WARN "apt-get is unavailable; source gRPC fallback may be built"
    return
  fi

  local -a elevate=()
  if [[ "$(id -u)" -ne 0 ]]; then
    if ! command -v sudo >/dev/null 2>&1; then
      log WARN "prebuilt gRPC packages need root and sudo is unavailable; source fallback may be built"
      return
    fi
    if sudo -n true >/dev/null 2>&1; then
      elevate=(sudo -n)
    elif [[ -t 0 && -t 1 ]]; then
      elevate=(sudo)
    else
      log WARN "prebuilt gRPC packages need interactive sudo; source fallback may be built"
      return
    fi
  fi

  log INFO "installing prebuilt protobuf/gRPC development packages with apt"
  if [[ "${AETHER_SKIP_APT_UPDATE:-0}" != "1" ]]; then
    if ! "${elevate[@]}" apt-get update; then
      log WARN "apt package index refresh failed; source gRPC fallback may be built"
      return
    fi
  fi
  if ! "${elevate[@]}" apt-get install -y \
      pkg-config \
      libprotobuf-dev \
      protobuf-compiler \
      libgrpc++-dev \
      protobuf-compiler-grpc; then
    log WARN "prebuilt gRPC package installation failed; source fallback may be built"
    return
  fi

  if ! system_grpc_available; then
    log WARN "prebuilt gRPC packages are incomplete; source fallback may be built"
    return
  fi
  log INFO "installed prebuilt system gRPC version=$(pkg-config --modversion grpc++)"
}

toml_string() {
  local file="$1"
  local section="$2"
  local key="$3"
  awk -v wanted_section="$section" -v wanted_key="$key" '
    /^[[:space:]]*\[\[/ {
      current = $0
      sub(/^[[:space:]]*\[\[[[:space:]]*/, "", current)
      sub(/[[:space:]]*\]\][[:space:]]*(#.*)?$/, "", current)
      next
    }
    current == wanted_section && $0 ~ "^[[:space:]]*" wanted_key "[[:space:]]*=" {
      value = $0
      sub("^[[:space:]]*" wanted_key "[[:space:]]*=[[:space:]]*", "", value)
      sub(/[[:space:]]*#.*$/, "", value)
      gsub(/^[[:space:]]*"|"[[:space:]]*$/, "", value)
      print value
      exit
    }
  ' "$file"
}

profile_backend_types() {
  local file="$1"
  local profile="$2"
  awk -v wanted_section="profile.$profile.device.backend" '
    /^[[:space:]]*\[/ {
      current = $0
      sub(/^[[:space:]]*\[[[:space:]]*/, "", current)
      sub(/[[:space:]]*\][[:space:]]*(#.*)?$/, "", current)
      next
    }
    current == wanted_section && /^[[:space:]]*type[[:space:]]*=/ {
      value = $0
      sub(/^[[:space:]]*type[[:space:]]*=[[:space:]]*/, "", value)
      sub(/[[:space:]]*#.*$/, "", value)
      gsub(/^[[:space:]]*"|"[[:space:]]*$/, "", value)
      if (!seen[value]++) print value
    }
  ' "$file"
}

BACKEND_PATH="$(toml_string "$CONFIG" mode.binding backend)"
BACKEND_PROFILE="$(toml_string "$CONFIG" mode.binding profile)"
if [[ -z "$BACKEND_PATH" || -z "$BACKEND_PROFILE" ]]; then
  log ERROR "unable to read the selected backend and profile from $CONFIG"
  exit 2
fi

BACKEND_FILE="$ROOT/etc/eos/$BACKEND_PATH"
if [[ ! -f "$BACKEND_FILE" ]]; then
  log ERROR "selected backend file does not exist: $BACKEND_FILE"
  exit 2
fi

mapfile -t BACKEND_TYPES < <(profile_backend_types "$BACKEND_FILE" "$BACKEND_PROFILE")
if [[ "${#BACKEND_TYPES[@]}" -eq 0 ]]; then
  log ERROR "profile=$BACKEND_PROFILE has no backend types in $BACKEND_FILE"
  exit 2
fi

BUILD_ROS2=0
for backend_type in "${BACKEND_TYPES[@]}"; do
  if [[ "$backend_type" == "ros2" ]]; then
    BUILD_ROS2=1
    break
  fi
done

HOST_MACHINE="$(uname -m)"
case "${HOST_MACHINE,,}" in
  x86_64 | amd64)
    HOST_ARCH="x86_64"
    PRESET="default"
    BUILD_DIR="$ROOT/build/aether"
    ROS2_PRESET_PREFIX="ros2"
    ROS2_BUILD_PREFIX="$ROOT/build/aether-ros2"
    ;;
  aarch64 | arm64)
    HOST_ARCH="arm64"
    PRESET="arm64"
    BUILD_DIR="$ROOT/build/aether-arm64"
    ROS2_PRESET_PREFIX="arm64-ros2"
    ROS2_BUILD_PREFIX="$ROOT/build/aether-arm64-ros2"
    ;;
  *)
    log ERROR "unsupported host architecture: $HOST_MACHINE (expected x86_64 or arm64)"
    exit 2
    ;;
esac

if [[ "$BUILD_ROS2" -eq 1 ]]; then
  case "${ROS_DISTRO:-}" in
    humble | jazzy)
      PRESET="$ROS2_PRESET_PREFIX-$ROS_DISTRO"
      BUILD_DIR="$ROS2_BUILD_PREFIX-$ROS_DISTRO"
      ;;
    "")
      log ERROR "profile=$BACKEND_PROFILE uses ROS 2; source Humble or Jazzy so ROS_DISTRO is set"
      exit 2
      ;;
    *)
      log ERROR "unsupported ROS_DISTRO=$ROS_DISTRO (expected humble or jazzy)"
      exit 2
      ;;
  esac
fi

if [[ "$PRINT_BUILD_DIR" -eq 1 ]]; then
  printf '%s\n' "$BUILD_DIR"
  exit 0
fi

install_system_grpc

if [[ "$CLEAN_BUILD" -eq 1 ]]; then
  log INFO "removing architecture=$HOST_ARCH build=$BUILD_DIR"
  cmake -E remove_directory "$BUILD_DIR"
fi

CONFIGURE_ARGS=(--preset "$PRESET")
if [[ -x "$ROOT/.venv/bin/python" ]]; then
  CONFIGURE_ARGS+=(-DPython3_EXECUTABLE="$ROOT/.venv/bin/python")
fi
GENERATOR="existing/default"
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  while IFS='=' read -r key value; do
    if [[ "$key" == "CMAKE_GENERATOR:INTERNAL" ]]; then
      GENERATOR="$value"
      CONFIGURE_ARGS+=(-G "$GENERATOR")
      break
    fi
  done < "$BUILD_DIR/CMakeCache.txt"
elif command -v ninja >/dev/null 2>&1; then
  CONFIGURE_ARGS+=(-G Ninja)
  GENERATOR="Ninja"
fi

log INFO "configuring architecture=$HOST_ARCH backends=${BACKEND_TYPES[*]} profile=$BACKEND_PROFILE preset=$PRESET generator=$GENERATOR build=$BUILD_DIR"
cmake "${CONFIGURE_ARGS[@]}"
log INFO "prebuilding dependencies jobs=$DEPENDENCY_JOBS"
cmake --build --preset "$PRESET" --target eos_dependencies --parallel "$DEPENDENCY_JOBS"
# Removed or renamed plugins otherwise remain loadable from this shared output
# directory and can make a current build fail admission at launch.
cmake -E remove_directory "$BUILD_DIR/skills"
log INFO "building preset=$PRESET jobs=$BUILD_JOBS"
cmake --build --preset "$PRESET" --parallel "$BUILD_JOBS"
log INFO "build complete: $BUILD_DIR"
