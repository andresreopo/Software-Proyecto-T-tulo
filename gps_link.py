"""GPS (ATGM366H, NMEA por UART) en un hilo de fondo → posición automática del observador.

Solo se usa la sentencia GGA (posición, calidad, satélites, HDOP, altitud); el resto de NMEA se ignora.
No requiere librerías extra: el parseo (con verificación de checksum) está aquí y el puerto lo abre pyserial.

La posición que se informa es "estable": un fix bueno (calidad > 0, satélites y HDOP aceptables) fija un ancla
y esta solo se mueve cuando el receptor se aleja más de GPS_MOVE_M, para que el jitter de unos metros no
dispare recálculos continuos de pases en la interfaz.

IMPORTANTE: un puerto serial solo lo puede abrir un proceso a la vez; con el servidor corriendo no uses
otro programa sobre la misma UART del GPS.
"""
from __future__ import annotations

import math
import random
import threading
import time

import serial

import gps_config as config

SIM_PORT = "sim"


# ══════════════════════════════════════════════════════════════════════════════
# NMEA
# ══════════════════════════════════════════════════════════════════════════════

def nmea_checksum(body: str) -> str:
    cs = 0
    for ch in body:
        cs ^= ord(ch)
    return f"{cs:02X}"


def _coord(value: str, hemi: str, deg_digits: int) -> float | None:
    """'3326.9340','S' → -33.4489 (formato NMEA ddmm.mmmm / dddmm.mmmm)."""
    if not value or not hemi:
        return None
    try:
        deg = float(value[:deg_digits])
        minutes = float(value[deg_digits:])
    except ValueError:
        return None
    if not 0 <= minutes < 60:
        return None
    result = deg + minutes / 60.0
    return -result if hemi in ("S", "W") else result


def _float(value: str):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def parse_gga(line: str) -> dict | None:
    """Devuelve los campos de una sentencia GGA válida (cualquier talker: GP/GN/BD/GL…) o None.

    Una GGA sin fix es válida y trae quality=0 con lat/lon None.
    """
    line = line.strip()
    if not line.startswith("$") or "*" not in line:
        return None
    body, _, checksum = line[1:].partition("*")
    if len(checksum) < 2 or nmea_checksum(body) != checksum[:2].upper():
        return None
    fields = body.split(",")
    if len(fields[0]) != 5 or fields[0][2:] != "GGA" or len(fields) < 10:
        return None
    try:
        quality = int(fields[6] or 0)
        sats = int(fields[7] or 0)
    except ValueError:
        return None
    return {
        "lat": _coord(fields[2], fields[3], 2),
        "lon": _coord(fields[4], fields[5], 3),
        "quality": quality,
        "sats": sats,
        "hdop": _float(fields[8]),
        "alt_m": _float(fields[9]),
    }


def distance_m(lat1, lon1, lat2, lon2) -> float:
    """Distancia aproximada (haversine) en metros."""
    p1, p2 = math.radians(lat1), math.radians(lat2)
    a = (math.sin((p2 - p1) / 2) ** 2
         + math.cos(p1) * math.cos(p2) * math.sin(math.radians(lon2 - lon1) / 2) ** 2)
    return 2 * 6371000.0 * math.asin(math.sqrt(a))


# ══════════════════════════════════════════════════════════════════════════════
# Puerto simulado (sin hardware): imita readline() de serial.Serial
# ══════════════════════════════════════════════════════════════════════════════

class SimPort:
    """Emite una GGA por segundo: primero sin fix y luego con fix en GPS_SIM_POS con ruido de ~2 m."""
    is_open = True

    def __init__(self):
        self._t0 = time.monotonic()
        self._next = 0.0

    @staticmethod
    def _fmt(lat: float, lon: float):
        def ddmm(v, deg_digits):
            v = abs(v)
            d = int(v)
            return f"{d:0{deg_digits}d}{(v - d) * 60:07.4f}"
        return ddmm(lat, 2), "S" if lat < 0 else "N", ddmm(lon, 3), "W" if lon < 0 else "E"

    def readline(self) -> bytes:
        time.sleep(max(0.0, self._next - time.monotonic()))
        self._next = time.monotonic() + 1.0
        clock = time.strftime("%H%M%S", time.gmtime()) + ".00"
        if time.monotonic() - self._t0 < config.GPS_SIM_NOFIX_S:
            body = f"GNGGA,{clock},,,,,0,00,99.99,,,,,,"
        else:
            lat0, lon0, alt = config.GPS_SIM_POS
            lat = lat0 + random.gauss(0, 2.0) / 111320.0
            lon = lon0 + random.gauss(0, 2.0) / (111320.0 * math.cos(math.radians(lat0)))
            la, ns, lo, ew = self._fmt(lat, lon)
            body = f"GNGGA,{clock},{la},{ns},{lo},{ew},1,09,0.90,{alt:.1f},M,28.0,M,,"
        return f"${body}*{nmea_checksum(body)}\r\n".encode()

    def close(self):
        self.is_open = False


# ══════════════════════════════════════════════════════════════════════════════
# Enlace
# ══════════════════════════════════════════════════════════════════════════════

class GpsLink:
    def __init__(self):
        self._lock = threading.Lock()
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._reset()

    def _reset(self):
        self._port = None
        self._baud = None
        self._error = None
        self._quality = 0
        self._sats = None
        self._hdop = None
        self._rx_ts = None        # última sentencia GGA válida (llegue con fix o sin él)
        self._fix_ts = None       # último fix BUENO
        self._anchor = None       # (lat, lon, alt_m) de la posición estable
        self._seq = 0             # sube cada vez que la posición estable cambia

    # ── API pública ───────────────────────────────────────────────────────────
    def start(self, port: str, baud: int = config.GPS_BAUD) -> None:
        self.stop()
        with self._lock:
            self._reset()
            self._port, self._baud = port, baud
            self._stop = threading.Event()
            self._thread = threading.Thread(target=self._run, args=(port, baud, self._stop),
                                            name="gps-reader", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        thread = self._thread
        self._stop.set()
        if thread is not None and thread.is_alive():
            thread.join(timeout=2.0)
        self._thread = None

    def enabled(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    def port(self):
        return self._port if self.enabled() else None

    def fix(self) -> dict | None:
        """Posición estable vigente {lat, lon, alt_m (o None), sats, hdop, age_s} o None."""
        with self._lock:
            return self._fix_locked()

    def state(self) -> dict:
        """Estado serializable a JSON (para /api/gps)."""
        with self._lock:
            now = time.monotonic()
            fix = self._fix_locked()
            rx_age = None if self._rx_ts is None else now - self._rx_ts
            return {
                "enabled": self.enabled(), "port": self._port, "baud": self._baud,
                "sim": self._port == SIM_PORT, "error": self._error,
                "connected": rx_age is not None and rx_age <= config.GPS_RX_STALE_S,
                "has_fix": fix is not None,
                "lat": fix["lat"] if fix else None, "lon": fix["lon"] if fix else None,
                "alt_m": fix["alt_m"] if fix else None,
                "fix_age": fix["age_s"] if fix else None,
                "quality": self._quality, "sats": self._sats, "hdop": self._hdop,
                "rx_age": None if rx_age is None else round(rx_age, 1),
                "seq": self._seq,
                "min_sats": config.GPS_MIN_SATS, "max_hdop": config.GPS_MAX_HDOP,
            }

    # ── internos ──────────────────────────────────────────────────────────────
    def _fix_locked(self):
        if self._anchor is None or self._fix_ts is None:
            return None
        age = time.monotonic() - self._fix_ts
        if age > config.GPS_MAX_AGE_S:
            return None
        lat, lon, alt = self._anchor
        return {"lat": lat, "lon": lon, "alt_m": alt, "sats": self._sats, "hdop": self._hdop,
                "age_s": round(age, 1)}

    def _handle(self, line: str) -> None:
        gga = parse_gga(line)
        if gga is None:
            return
        now = time.monotonic()
        with self._lock:
            self._rx_ts = now
            self._quality, self._sats, self._hdop = gga["quality"], gga["sats"], gga["hdop"]
            good = (gga["quality"] > 0 and gga["lat"] is not None and gga["lon"] is not None
                    and gga["sats"] >= config.GPS_MIN_SATS
                    and (gga["hdop"] is None or gga["hdop"] <= config.GPS_MAX_HDOP))
            if not good:
                return
            self._fix_ts = now
            lat, lon, alt = gga["lat"], gga["lon"], gga["alt_m"]       # alt puede ser None
            if self._anchor is None or distance_m(self._anchor[0], self._anchor[1], lat, lon) > config.GPS_MOVE_M:
                self._anchor = (lat, lon, alt)
                self._seq += 1

    def _open(self, port: str, baud: int):
        if port == SIM_PORT:
            return SimPort()
        return serial.Serial(port, baudrate=baud, timeout=1.0)

    def _run(self, port: str, baud: int, stop: threading.Event) -> None:
        while not stop.is_set():
            ser = None
            try:
                ser = self._open(port, baud)
                with self._lock:
                    self._error = None
                while not stop.is_set():
                    raw = ser.readline()
                    if raw:
                        self._handle(raw.decode("ascii", errors="replace"))
            except (serial.SerialException, OSError) as exc:
                with self._lock:
                    self._error = str(exc)
            finally:
                if ser is not None:
                    try:
                        ser.close()
                    except Exception:
                        pass
            stop.wait(config.GPS_RETRY_S)


gps = GpsLink()
