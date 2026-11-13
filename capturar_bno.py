"""Guarda la telemetría BNO del rotor en un CSV local."""

from __future__ import annotations

import argparse
import csv
import math
from contextlib import closing
from datetime import datetime, timezone
from pathlib import Path
import sys
import threading

import serial


BNO_PREFIX = b"BNO,"
CSV_HEADER = (
    "pc_utc", "esp_ms", "muestra", "qw", "qx", "qy", "qz",
    "estado_precision", "error_estimado_deg", "az_sin_offset_deg",
    "el_sin_offset_deg",
)


def parse_bno(line: bytes) -> list[int | float | str] | None:
    if not line.startswith(BNO_PREFIX):
        return None
    fields = line[len(BNO_PREFIX):].strip().split(b",")
    if len(fields) != len(CSV_HEADER) - 1:
        return None
    try:
        esp_ms, sample_id = map(int, fields[:2])
        accuracy_status = int(fields[6])
        quaternion = [float(field) for field in fields[2:6]]
        angles = [float(field) for field in fields[7:]]
    except ValueError:
        return None
    if not (0 <= esp_ms <= 0xFFFFFFFF and 0 <= sample_id <= 0xFFFFFFFF
            and 0 <= accuracy_status <= 3):
        return None
    if any(math.isinf(value) for value in quaternion + angles):
        return None
    return [esp_ms, sample_id,
            *("" if math.isnan(value) else value for value in quaternion),
            accuracy_status, *("" if math.isnan(value) else value for value in angles)]


class BnoCsv:
    def __init__(self, path: Path):
        self._lock = threading.Lock()
        self._file = path.open("x", newline="", encoding="utf-8")
        self._writer = csv.writer(self._file)
        try:
            self._writer.writerow(CSV_HEADER)
            self._file.flush()
        except OSError:
            self._file.close()
            raise

    def write(self, line: bytes) -> bool:
        sample = parse_bno(line)
        if sample is None:
            return False
        timestamp = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
        with self._lock:
            if self._file.closed:
                return False
            self._writer.writerow([timestamp, *sample])
            self._file.flush()
        return True

    def close(self) -> None:
        with self._lock:
            self._file.close()


def capture_serial(port: serial.Serial, destination: BnoCsv) -> None:
    # readline puede devolver parte de una línea si vence el timeout.
    line = bytearray()
    discard = False
    while True:
        chunk = port.readline(256)
        if not discard:
            line.extend(chunk)
            if len(line) > 256:
                line.clear()
                discard = True
        if chunk.endswith(b"\n"):
            if not discard:
                destination.write(bytes(line))
            line.clear()
            discard = False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Puerto serial del ESP32, por ejemplo COM6")
    parser.add_argument("--output", required=True, type=Path, help="Ruta de un CSV nuevo")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    port = serial.Serial(baudrate=args.baud, timeout=0.5)
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        with port, closing(BnoCsv(args.output)) as destination:
            print(f"Capturando BNO en {args.output.resolve()}. Ctrl+C para terminar.", flush=True)
            capture_serial(port, destination)
    except KeyboardInterrupt:
        return 0
    except (OSError, ValueError) as exc:
        print(f"No se pudo completar la captura BNO: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
