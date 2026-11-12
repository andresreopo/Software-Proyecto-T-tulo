#!/bin/sh
set -eu

url=http://127.0.0.1:5003/
if ! browser=$(command -v chromium || command -v chromium-browser); then
    printf '%s\n' 'SATTRACK requiere Chromium para abrir la interfaz.' >&2
    exit 1
fi

until curl -fsS --max-time 2 "$url" >/dev/null 2>&1; do
    sleep 1
done

exec "$browser" --kiosk --no-first-run "$url"
