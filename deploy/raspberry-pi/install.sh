#!/usr/bin/env bash
set -euo pipefail

fail() {
    printf '%s\n' "$1" >&2
    exit 1
}

if (( EUID == 0 )); then
    fail 'Ejecuta el instalador con el usuario del escritorio. El script llama a sudo cuando lo necesita.'
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
project_dir=$(cd -- "$script_dir/../.." && pwd -P)
desktop_user=$(id -un)

if [[ -n ${SATTRACK_PYTHON:-} ]]; then
    python_bin=$SATTRACK_PYTHON
elif [[ -n ${VIRTUAL_ENV:-} ]]; then
    python_bin=$VIRTUAL_ENV/bin/python
elif [[ -x $project_dir/.venv/bin/python ]]; then
    python_bin=$project_dir/.venv/bin/python
else
    python_bin=$(command -v python3 || true)
fi

[[ $python_bin == /* && -x $python_bin ]] ||
    fail 'Indica la ruta absoluta al Python de SATTRACK con SATTRACK_PYTHON.'
command -v systemctl >/dev/null || fail 'El instalador requiere systemd.'
command -v curl >/dev/null || fail 'Instala curl con sudo apt install curl.'
command -v chromium >/dev/null || command -v chromium-browser >/dev/null ||
    fail 'Instala Chromium con sudo apt install chromium.'

if ! (cd -- "$project_dir" && "$python_bin" -B -c 'import app'); then
    fail "No se pudo importar SATTRACK con $python_bin. Activa el entorno que ya usas o indica SATTRACK_PYTHON."
fi

if [[ $project_dir == *[$'\r\n']* || $python_bin == *[$'\r\n']* ]]; then
    fail 'Usa rutas sin saltos de línea para el repositorio y el intérprete de Python.'
fi

# WorkingDirectory toma la ruta completa. ExecStart necesita un argumento entre comillas.
unit_dir=${project_dir//%/%%}
unit_python=${python_bin//%/%%}
unit_python=${unit_python//\\/\\\\}
unit_python=${unit_python//\"/\\\"}

unit_file=$(mktemp)
trap 'rm -f -- "$unit_file"' EXIT
cat > "$unit_file" <<EOF
[Unit]
Description=SATTRACK

[Service]
Type=exec
User=$desktop_user
WorkingDirectory=$unit_dir
EnvironmentFile=-/etc/default/sattrack
ExecStart="$unit_python" -u app.py \$SATTRACK_ARGS
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

sudo install -m 0644 -- "$unit_file" /etc/systemd/system/sattrack.service

labwc_dir=${XDG_CONFIG_HOME:-$HOME/.config}/labwc
mkdir -p -- "$labwc_dir"
install -m 0755 -- "$script_dir/start-kiosk.sh" "$labwc_dir/sattrack-kiosk.sh"
autostart_entry='"${XDG_CONFIG_HOME:-$HOME/.config}/labwc/sattrack-kiosk.sh" &'
if ! grep -Fxq -- "$autostart_entry" "$labwc_dir/autostart" 2>/dev/null; then
    printf '\n%s\n' "$autostart_entry" >> "$labwc_dir/autostart"
fi

sudo systemctl daemon-reload
sudo systemctl enable sattrack.service
sudo systemctl restart sattrack.service

if ! curl -fsS --retry 8 --retry-connrefused --retry-delay 1 --max-time 3 \
    http://127.0.0.1:5003/ -o /dev/null; then
    fail 'SATTRACK no respondió. Revisa sudo journalctl -u sattrack.service -b -n 50 --no-pager.'
fi

printf '%s\n' 'SATTRACK está activo. Chromium se abrirá en la próxima sesión del escritorio.'
