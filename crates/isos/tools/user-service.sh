#!/usr/bin/env bash
set -euo pipefail

action="${1:-}"
if [[ "$action" != install && "$action" != remove ]]; then
  echo "usage: tools/user-service.sh install|remove" >&2
  exit 2
fi
command -v systemctl >/dev/null || { echo "systemd user manager is unavailable" >&2; exit 1; }
unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
unit="$unit_dir/isos.service"
if [[ "$action" == remove ]]; then
  systemctl --user disable --now isos.service 2>/dev/null || true
  rm -f "$unit"
  systemctl --user daemon-reload
  exit 0
fi
if [[ -n "${ISOS_EXECUTABLE:-}" ]]; then
  [[ -x "$ISOS_EXECUTABLE" ]] || { echo "IsOS executable is unavailable: $ISOS_EXECUTABLE" >&2; exit 1; }
  isos_executable="$(readlink -f "$ISOS_EXECUTABLE")"
else
  command -v isos >/dev/null || { echo "isos executable is unavailable" >&2; exit 1; }
  isos_executable="$(readlink -f "$(command -v isos)")"
fi
if [[ "$isos_executable" == *' '* ]]; then
  echo "IsOS executable path cannot contain spaces for this systemd unit" >&2
  exit 1
fi
mode_file="${ISOS_HOME:-${XDG_CONFIG_HOME:-$HOME/.config}/isos}/config/mode"
if [[ -z "${ISOS_HOME:-}" ]]; then
  mode_file="${XDG_CONFIG_HOME:-$HOME/.config}/isos/mode"
fi
[[ "$(cat "$mode_file" 2>/dev/null)" == product ]] || { echo "run isos init --mode product first" >&2; exit 1; }
mkdir -p "$unit_dir"
environment_line=""
if [[ -n "${ISOS_HOME:-}" ]]; then
  environment_line="Environment=ISOS_HOME=$ISOS_HOME"
fi
cat > "$unit" <<EOF
[Unit]
Description=IsOS product manager
After=network.target

[Service]
Type=simple
$environment_line
ExecStart=$isos_executable serve
ExecStop=$isos_executable stop
Restart=on-failure

[Install]
WantedBy=default.target
EOF
systemctl --user daemon-reload
systemctl --user enable --now isos.service
echo "Installed $unit"
