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


class BnoCapture:
    """Captura compartida por los clientes de SATTRACK y el lector serial."""

    def __init__(self, directory: Path):
        self.directory = directory
        self._lock = threading.Lock()
        self._csv: BnoCsv | None = None
        self._state = dict(active=False, filename=None, samples=0, simulated=False, error=None)

    def state(self) -> dict:
        with self._lock:
            return dict(self._state)

    def start(self, simulated: bool) -> dict:
        with self._lock:
            if self._csv is None:
                self.directory.mkdir(parents=True, exist_ok=True)
                stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
                prefix = "bno_sim" if simulated else "bno"
                filename = f"{prefix}_{stamp}.csv"
                self._csv = BnoCsv(self.directory / filename)
                self._state.update(active=True, filename=filename, samples=0, simulated=simulated, error=None)
            return dict(self._state)

    def _finish(self, error: str | None) -> None:
        try:
            self._csv.close()
        except OSError as exc:
            error = f"No se pudo cerrar el CSV: {exc}"
        finally:
            self._csv = None
            self._state.update(active=False, error=error)

    def stop(self, error: str | None = None) -> dict:
        with self._lock:
            if self._csv is not None:
                self._finish(error)
            return dict(self._state)

    def write(self, line: bytes) -> None:
        with self._lock:
            if self._csv is None:
                return
            try:
                if self._csv.write(line):
                    self._state["samples"] += 1
            except OSError as exc:
                self._finish(f"No se pudo escribir el CSV: {exc}")


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
