#!/usr/bin/env bash
set -euo pipefail
install_user_service=false
active_env_source=""
usage() {
  echo "usage: ./install.sh [--activate-env /absolute/setup.env] [--install-user-service]"
}
while (( $# )); do
  case "$1" in
    --install-user-service) install_user_service=true; shift ;;
    --activate-env)
      if (( $# < 2 )); then usage >&2; exit 2; fi
      active_env_source="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT/../.." && pwd)"
if [[ -n "$active_env_source" ]]; then
  [[ "$active_env_source" == /* && -f "$active_env_source" && ! -L "$active_env_source" ]] || {
    echo "--activate-env requires an absolute regular file" >&2; exit 2;
  }
  declare -A selected=()
  line_number=0
  while IFS= read -r line || [[ -n "$line" ]]; do
    ((line_number+=1))
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line%"${line##*[![:space:]]}"}"
    [[ -z "$line" || "$line" == \#* ]] && continue
    line="${line#export }"
    [[ "$line" == *=* ]] || { echo "invalid active env line $line_number" >&2; exit 2; }
    key="${line%%=*}"
    value="${line#*=}"
    case "$key" in
      ISOS_HOME|ISOS_CONFIG|ISOS_HTTP_PORT|ISOSUI_PORT|ISOS_AETHER_BIN_DIR|ISOSUI_DIST_DIR|ISOS_EGIA_BIN_DIR|ISOS_EGIA_DEFAULTS_DIR|ISOS_EGIA_FACTORY_DEFAULTS_DIR|EGIA_PYTHON|ISLAND_PREFIX|ISLAND_MODE|ISLAND_ETC|ISLAND_SHARE|ISLAND_CONFIG|EGIA_ETC|ISLAND_CACHE|ISLAND_LOG|ISLAND_RUN|ISLAND_TMP) ;;
      *) echo "unsupported active env key $key on line $line_number; see $WORKSPACE_ROOT/docs/isos/usage/configuration.md" >&2; exit 2 ;;
    esac
    [[ -n "$value" && ! -v selected[$key] ]] || { echo "empty or duplicate active env key on line $line_number" >&2; exit 2; }
    case "$key" in
      ISOS_HTTP_PORT|ISOSUI_PORT)
        [[ "$value" =~ ^[0-9]+$ ]] && (( 10#$value >= 1024 && 10#$value <= 65535 )) || { echo "invalid active env port on line $line_number" >&2; exit 2; } ;;
      ISLAND_MODE) [[ "$value" == system || "$value" == user || "$value" == dev ]] || { echo "invalid ISLAND_MODE" >&2; exit 2; } ;;
      *) [[ "$value" == /* ]] || { echo "active env path must be absolute on line $line_number" >&2; exit 2; } ;;
    esac
    selected[$key]="$value"
  done < "$active_env_source"
  [[ -n "${selected[ISOS_HOME]:-}" ]] || { echo "active env requires ISOS_HOME" >&2; exit 2; }
  [[ -n "${selected[ISOS_HTTP_PORT]:-}" && -n "${selected[ISOSUI_PORT]:-}" ]] || { echo "active env requires API and UI ports" >&2; exit 2; }
  [[ "${selected[ISOS_HTTP_PORT]:-}" != "${selected[ISOSUI_PORT]:-}" ]] || { echo "API and UI ports must differ" >&2; exit 2; }
fi
if command -v cargo >/dev/null 2>&1; then
  CARGO_BIN="$(command -v cargo)"
elif [[ -x "${HOME:-}/.cargo/bin/cargo" ]]; then
  CARGO_BIN="${HOME}/.cargo/bin/cargo"
else
  echo "IsOS requires Rust Cargo; install Rust from https://rustup.rs or add ~/.cargo/bin to PATH" >&2
  exit 127
fi
INSTALL_ROOT="${ISOS_INSTALL_ROOT:-${CARGO_HOME:-${HOME}/.cargo}}"
# Replacing the on-disk CLI does not require stopping a running owner. Its
# next explicit restart uses the new binary; installation never changes a
# deployment's stop evidence or reaches another home by accident.
if command -v systemctl >/dev/null 2>&1 && systemctl --user is-active --quiet isos-server.service; then
  echo "Obsolete isos-server.service is active; stop it with: systemctl --user stop isos-server.service" >&2
  exit 1
fi
"$ROOT/build.sh"
"$CARGO_BIN" install --force --locked --path "$ROOT" --root "$INSTALL_ROOT"
BUILD_TARGET="${CARGO_TARGET_DIR:-$WORKSPACE_ROOT/target}"
[[ "$BUILD_TARGET" == /* ]] || BUILD_TARGET="$WORKSPACE_ROOT/$BUILD_TARGET"
EXPECTED_VERSION="$("$BUILD_TARGET/release/isos" --version)"
INSTALLED_VERSION="$("$INSTALL_ROOT/bin/isos" --version)"
if [[ "$EXPECTED_VERSION" != "$INSTALLED_VERSION" ]]; then
  echo "Installed IsOS version does not match this checkout: $INSTALLED_VERSION != $EXPECTED_VERSION" >&2
  exit 1
fi
COMPLETION_ROOT="${XDG_DATA_HOME:-${HOME}/.local/share}/isos/completions"
mkdir -p "$COMPLETION_ROOT" "${XDG_CONFIG_HOME:-${HOME}/.config}/fish/completions"
"$INSTALL_ROOT/bin/isos" --generate-completion bash > "$COMPLETION_ROOT/isos.bash"
"$INSTALL_ROOT/bin/isos" --generate-completion zsh > "$COMPLETION_ROOT/isos.zsh"
"$INSTALL_ROOT/bin/isos" --generate-completion fish > "${XDG_CONFIG_HOME:-${HOME}/.config}/fish/completions/isos.fish"
if [[ -f "$HOME/.bashrc" || "${SHELL:-}" == */bash ]]; then
  touch "$HOME/.bashrc"
  if ! grep -Fq '# IsOS completion' "$HOME/.bashrc"; then
    printf '\n# IsOS completion\n[[ -f "%s/isos.bash" ]] && source "%s/isos.bash"\n' "$COMPLETION_ROOT" "$COMPLETION_ROOT" >> "$HOME/.bashrc"
  fi
fi
if [[ -f "$HOME/.zshrc" || "${SHELL:-}" == */zsh ]]; then
  touch "$HOME/.zshrc"
  if ! grep -Fq '# IsOS completion' "$HOME/.zshrc"; then
    printf '\n# IsOS completion\nif [[ -f "%s/isos.zsh" ]]; then\n  (( $+functions[compdef] )) || { autoload -Uz compinit && compinit; }\n  source "%s/isos.zsh"\nfi\n' "$COMPLETION_ROOT" "$COMPLETION_ROOT" >> "$HOME/.zshrc"
  fi
fi
echo "Installed IsOS shell completions; open a new shell to enable Tab completion."
if ! command -v isos >/dev/null 2>&1; then
  case ":$PATH:" in
    *":${HOME}/.local/bin:"*)
      mkdir -p "$HOME/.local/bin"
      if [[ ! -e "$HOME/.local/bin/isos" && ! -L "$HOME/.local/bin/isos" ]]; then
        ln -s "$INSTALL_ROOT/bin/isos" "$HOME/.local/bin/isos"
        echo "Linked IsOS at $HOME/.local/bin/isos"
      fi
      ;;
  esac
fi
if ! command -v isos >/dev/null 2>&1; then
  echo "Add $INSTALL_ROOT/bin to PATH, then open a new shell to run isos." >&2
fi
if [[ -n "$active_env_source" ]]; then
  activation_dir="${XDG_CONFIG_HOME:-$HOME/.config}/isos"
  [[ "$activation_dir" == /* ]] || { echo "activation directory must be absolute" >&2; exit 2; }
  selected_home="${selected[ISOS_HOME]}"
  selected_config="${selected[ISOS_CONFIG]:-$selected_home/config/isos.toml}"
  mkdir -p -m 700 "$selected_home/config" "$activation_dir"
  chmod 700 "$activation_dir"
  if [[ ! -e "$selected_config" ]]; then
    [[ "$selected_config" == "$selected_home/config/isos.toml" ]] || { echo "selected ISOS_CONFIG does not exist" >&2; exit 2; }
    # Activation's owner paths come from absolute entries in active.env.
    # The example supplies all other defaults; comments have no runtime effect.
    config_tmp="$(mktemp "$selected_home/config/.isos.toml.XXXXXX")"
    chmod 600 "$config_tmp"
    printf '# Local overrides for config/isos.example.toml; defaults load automatically.\n' > "$config_tmp"
    mv -- "$config_tmp" "$selected_config"
  fi
  [[ -f "$selected_config" && ! -L "$selected_config" ]] || { echo "selected ISOS_CONFIG must be a regular file" >&2; exit 2; }
  activation_tmp="$(mktemp "$activation_dir/.active.env.XXXXXX")"
  chmod 600 "$activation_tmp"
  cp -- "$active_env_source" "$activation_tmp"
  if [[ -z "${selected[ISOS_CONFIG]:-}" ]]; then
    printf 'export ISOS_CONFIG=%s\n' "$selected_config" >> "$activation_tmp"
  fi
  mv -f -- "$activation_tmp" "$activation_dir/active.env"
  export ISOS_HOME="$selected_home" ISOS_CONFIG="$selected_config"
  echo "Activated IsOS home $ISOS_HOME via $activation_dir/active.env"
fi
if [[ "$install_user_service" == true ]]; then
  ISOS_EXECUTABLE="$INSTALL_ROOT/bin/isos" "$ROOT/tools/user-service.sh" install
fi
