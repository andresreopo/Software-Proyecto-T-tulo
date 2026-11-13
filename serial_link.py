"""
serial_link.py
==============
Enlace serial bidireccional con la ESP32 que mueve el rotor.

Protocolo (texto, una línea por mensaje terminada en \\n, 115200 baud por defecto):
  PC    → ESP32 :  SET,<az>,<el>     referencia en grados, p. ej.  SET,123.40,45.00
  PC    → ESP32 :  TRK,<az>,<el>,<vaz>,<vel>
                   seguimiento: posición del satélite en grados y su velocidad angular
                   en °/s (vaz > 0 = azimut creciente). Se envía a ~5 Hz; la ESP32 debe
                   extrapolar con la velocidad entre mensajes y detenerse si dejan de llegar.
  ESP32 → PC    :  POS,<az>,<el>     posición actual, ~5 Hz,       POS,121.80,44.10
  ESP32 → PC    :  BAT,<volts>       voltaje de la batería en V, ~1 Hz,   BAT,12.63
  ESP32 → PC    :  cualquier otra línea se muestra en el registro de la interfaz
                   ("ACK" se ignora; úsala para confirmar un SET).

El módulo no sabe nada de Flask: avisa los cambios con el callback on_event(dict).

Para probar sin hardware, usa el puerto "sim": una ESP32 simulada que se mueve
hacia la referencia a velocidad finita.
"""

import collections
import logging
import math
import queue
import threading
import time
from typing import Callable, Optional

import serial
from serial.tools import list_ports as _list_ports

import rotor_config as config
from capturar_bno import BNO_PREFIX

SIM_PORT = "sim"


# ══════════════════════════════════════════════════════════════════════════════
# UTILIDADES
# ══════════════════════════════════════════════════════════════════════════════

def list_ports() -> list[dict]:
    """Puertos seriales del equipo, más el simulador."""
    ports = [{"device": p.device, "description": p.description or p.device}
             for p in sorted(_list_ports.comports(), key=lambda p: p.device)]
    ports.append({"device": SIM_PORT, "description": "Simulador de ESP32 (sin hardware)"})
    return ports


def parse_line(text: str):
    """'POS,12.3,45.6' → ("pos", (az, el)); 'BAT,12.6' → ("bat", volts); otro texto → ("msg", texto); vacío → None."""
    text = text.strip()
    if not text:
        return None
    parts = [p.strip() for p in text.split(",")]
    if parts[0].upper() == "BAT" and len(parts) == 2:
        try:
            volts = float(parts[1])
        except ValueError:
            return ("msg", text)
        return ("bat", volts) if math.isfinite(volts) and volts >= 0 else ("msg", text)
    if parts[0].upper() == "POS" and len(parts) == 3:
        try:
            az, el = float(parts[1]), float(parts[2])
        except ValueError:
            return ("msg", text)
        if math.isfinite(az) and math.isfinite(el):
            return ("pos", (az % 360.0, el))
        return ("msg", text)
    return ("msg", text)


def validate_target(az: float, el: float) -> tuple[float, float]:
    if not (math.isfinite(az) and math.isfinite(el)):
        raise ValueError("Azimut y elevación deben ser números.")
    if not 0.0 <= az <= config.ROTOR_AZ_MAX:
        raise ValueError(f"El azimut del rotor debe estar entre 0° y {config.ROTOR_AZ_MAX:.0f}°.")
    if not config.ROTOR_EL_MIN <= el <= config.ROTOR_EL_MAX:
        raise ValueError(f"La elevación del rotor debe estar entre {config.ROTOR_EL_MIN:.0f}° "
                         f"y {config.ROTOR_EL_MAX:.0f}°.")
    return az % 360.0, el


def validate_track(az: float, el: float, vaz: float, vel: float):
    az, el = validate_target(az, el)
    if not (math.isfinite(vaz) and math.isfinite(vel)) or abs(vaz) > 360.0 or abs(vel) > 360.0:
        raise ValueError("Las velocidades angulares deben ser números entre -360 y 360 °/s.")
    return az, el, vaz, vel


# ══════════════════════════════════════════════════════════════════════════════
# ESP32 SIMULADA
# ══════════════════════════════════════════════════════════════════════════════

class SimPort:
    """Imita lo mínimo de serial.Serial (readline / write / close) con un rotor ideal.
    SET: gira a velocidad máxima hasta la referencia. TRK: sigue una referencia móvil con
    feedforward de velocidad (misma ley de control que el sketch de la ESP32)."""
    AZ_RATE, EL_RATE, PERIOD = None, None, 0.2        # se leen de config al crear (°/s)
    KP, TRK_TIMEOUT = 2.0, 1.5

    def __init__(self):
        self.AZ_RATE, self.EL_RATE = config.ROTOR_MAX_RATE_AZ, config.ROTOR_MAX_RATE_EL
        self.az, self.el = 0.0, 0.0
        self.tgt = (0.0, 0.0)
        self.trk = False
        self.ff = (0.0, 0.0)
        self.trk_t = 0.0
        self._t = time.monotonic()
        self._q: queue.Queue = queue.Queue()
        self.is_open = True
        self._bat_t = 0.0
        self._q.put(b"# ESP32 simulada lista\n")

    def write(self, data: bytes) -> int:
        for ln in data.decode(errors="ignore").splitlines():
            p = [x.strip() for x in ln.split(",")]
            try:
                if len(p) == 3 and p[0].upper() == "SET":
                    self.tgt = (float(p[1]) % 360.0, float(p[2]))
                    self.trk, self.ff = False, (0.0, 0.0)
                    self._q.put(b"ACK\n")
                elif len(p) == 5 and p[0].upper() == "TRK":
                    self.tgt = (float(p[1]) % 360.0, float(p[2]))
                    self.ff = (float(p[3]), float(p[4]))
                    self.trk, self.trk_t = True, time.monotonic()
                elif p[0].upper() in ("SET", "TRK"):
                    self._q.put(b"ERR,formato\n")
                elif ln.strip():
                    self._q.put(b"ERR,comando desconocido\n")
            except ValueError:
                self._q.put(b"ERR,formato\n")
        return len(data)

    def _step(self):
        now = time.monotonic(); dt = now - self._t; self._t = now
        if self.trk and now - self.trk_t > self.TRK_TIMEOUT:      # dejaron de llegar TRK: mantener
            self.trk, self.ff = False, (0.0, 0.0)
            self.tgt = (self.az, self.el)
        if self.trk:
            age = now - self.trk_t
            t_az = self.tgt[0] + self.ff[0] * age                  # extrapola la referencia
            t_el = self.tgt[1] + self.ff[1] * age
            r_az = self.ff[0] + self.KP * ((t_az - self.az + 540.0) % 360.0 - 180.0)
            r_el = self.ff[1] + self.KP * (t_el - self.el)
            r_az = max(-self.AZ_RATE, min(self.AZ_RATE, r_az))
            r_el = max(-self.EL_RATE, min(self.EL_RATE, r_el))
            self.az = (self.az + r_az * dt) % 360.0
            self.el = max(config.ROTOR_EL_MIN, min(config.ROTOR_EL_MAX, self.el + r_el * dt))
            return
        d = (self.tgt[0] - self.az + 180.0) % 360.0 - 180.0
        self.az = (self.az + max(-self.AZ_RATE * dt, min(self.AZ_RATE * dt, d))) % 360.0
        d = self.tgt[1] - self.el
        self.el += max(-self.EL_RATE * dt, min(self.EL_RATE * dt, d))

    def readline(self) -> bytes:
        try:
            return self._q.get(timeout=self.PERIOD)
        except queue.Empty:
            self._step()
            now = time.monotonic()
            if now - self._bat_t >= 1.0:                   # batería simulada ~12.6 V con una ligera oscilación
                self._bat_t = now
                self._q.put(f"BAT,{12.6 + 0.05 * math.sin(now / 5.0):.2f}\n".encode())
            return f"POS,{self.az:.2f},{self.el:.2f}\n".encode()

    def close(self):
        self.is_open = False


# ══════════════════════════════════════════════════════════════════════════════
# ENLACE
# ══════════════════════════════════════════════════════════════════════════════

class SerialLink:
    def __init__(self, on_event: Optional[Callable[[dict], None]] = None):
        self._on_event = on_event or (lambda e: None)
        self.on_bno_sample: Optional[Callable[[bytes], bool]] = None
        self._lock  = threading.Lock()      # estado
        self._wlock = threading.Lock()      # escritura al puerto
        self._gen = 0                       # cada connect()/disconnect() invalida el hilo anterior
        self._thread: Optional[threading.Thread] = None
        self._ser = None
        self._first = threading.Event()
        self._msg_times: collections.deque = collections.deque(maxlen=4)
        self._reset_state()

    # ── estado ────────────────────────────────────────────────────────────────
    def _reset_state(self):
        self._connected = False
        self._port = None
        self._baud = None
        self._error = None
        self._az = self._el = None
        self._pos_ts = None
        self._bat = None                    # último voltaje de batería (V)
        self._bat_ts = None
        self._tgt = (None, None)

    def state(self) -> dict:
        with self._lock:
            age = None if self._pos_ts is None else time.monotonic() - self._pos_ts
            bat_age = None if self._bat_ts is None else time.monotonic() - self._bat_ts
            return {
                "connected": self._connected, "port": self._port, "baud": self._baud,
                "sim": self._port == SIM_PORT, "error": self._error,
                "az": self._az, "el": self._el, "age": age,
                "tgt_az": self._tgt[0], "tgt_el": self._tgt[1],
                "el_min": config.ROTOR_EL_MIN, "el_max": config.ROTOR_EL_MAX,
                "stale_s": config.SERIAL_STALE_S,
                "bat_v": self._bat, "bat_age": bat_age, "bat_stale_s": config.BAT_STALE_S,
            }

    def _emit(self, extra: Optional[dict] = None):
        ev = {"type": "rotor", **self.state()}
        if extra:
            ev.update(extra)
        try:
            self._on_event(ev)
        except Exception as e:                          # un cliente roto no debe matar el hilo
            print("[SerialLink] error en callback:", e)

    # ── API pública ───────────────────────────────────────────────────────────
    def connect(self, port: str, baud: int = config.SERIAL_BAUD, wait_s: float = 4.0) -> dict:
        """Abre el puerto. Si no se puede abrir, lanza RuntimeError con el motivo."""
        self.disconnect()
        with self._lock:
            self._gen += 1
            gen = self._gen
            self._reset_state()
            self._port, self._baud = port, baud
            self._first.clear()
        self._thread = threading.Thread(target=self._run, args=(gen, port, baud), daemon=True)
        self._thread.start()
        self._first.wait(wait_s)
        st = self.state()
        if not st["connected"]:
            raise RuntimeError(st["error"] or "No se pudo abrir el puerto serial.")
        return st

    def disconnect(self) -> None:
        with self._lock:
            self._gen += 1
            th = self._thread
        if th and th.is_alive():
            th.join(2.0)
        with self._lock:
            self._reset_state()
        self._emit()

    def send_target(self, az: float, el: float) -> dict:
        az, el = validate_target(az, el)
        with self._lock:
            ser = self._ser if self._connected else None
        if ser is None:
            raise RuntimeError("El rotor no está conectado.")
        try:
            with self._wlock:
                ser.write(f"SET,{az:.2f},{el:.2f}\n".encode()); self._io("tx", f"SET,{az:.2f},{el:.2f}")
                if hasattr(ser, "flush"):
                    ser.flush()
        except (serial.SerialException, OSError) as e:
            raise RuntimeError(f"No se pudo escribir en el puerto: {e}")
        with self._lock:
            self._tgt = (az, el)
        self._emit()
        return self.state()

    def send_raw(self, text: str) -> None:
        """Comando manual de la terminal: una línea de texto imprimible, tal cual, terminada en \\n."""
        text = str(text or "").strip()
        if not text:
            raise ValueError("Escribe un comando.")
        if len(text) > 120 or not all(32 <= ord(c) < 127 for c in text):
            raise ValueError("El comando debe ser texto ASCII imprimible de hasta 120 caracteres.")
        with self._lock:
            ser = self._ser if self._connected else None
        if ser is None:
            raise RuntimeError("El rotor no está conectado.")
        try:
            with self._wlock:
                ser.write((text + "\n").encode("ascii")); self._io("tx", text)
                if hasattr(ser, "flush"):
                    ser.flush()
        except (serial.SerialException, OSError) as e:
            raise RuntimeError(f"No se pudo escribir en el puerto: {e}")

    def send_track(self, az: float, el: float, vaz: float, vel: float) -> dict:
        """Referencia móvil: posición del satélite y su velocidad angular (°/s)."""
        az, el, vaz, vel = validate_track(az, el, vaz, vel)
        with self._lock:
            ser = self._ser if self._connected else None
        if ser is None:
            raise RuntimeError("El rotor no está conectado.")
        try:
            with self._wlock:
                ser.write(f"TRK,{az:.3f},{el:.3f},{vaz:.3f},{vel:.3f}\n".encode()); self._io("tx", f"TRK,{az:.3f},{el:.3f},{vaz:.3f},{vel:.3f}")
                if hasattr(ser, "flush"):
                    ser.flush()
        except (serial.SerialException, OSError) as e:
            raise RuntimeError(f"No se pudo escribir en el puerto: {e}")
        with self._lock:
            self._tgt = (az, el)
        self._emit()
        return self.state()

    # ── hilo lector ───────────────────────────────────────────────────────────
    def _open(self, port: str, baud: int):
        if port == SIM_PORT:
            return SimPort()
        ser = serial.Serial()
        ser.port, ser.baudrate = port, baud
        ser.timeout, ser.write_timeout = 0.5, 1.0
        ser.dtr = False          # evita que abrir el puerto reinicie la mayoría de las ESP32
        ser.rts = False
        ser.open()
        return ser

    def _alive(self, gen: int) -> bool:
        return self._gen == gen

    def _run(self, gen: int, port: str, baud: int):
        ever = False
        while self._alive(gen):
            ser = None
            try:
                ser = self._open(port, baud)
                with self._lock:
                    if not self._alive(gen):
                        ser.close(); return
                    self._ser, self._connected, self._error = ser, True, None
                ever = True
                self._first.set()
                self._emit()
                buf = b""
                while self._alive(gen):
                    raw = ser.readline()
                    if not raw:
                        continue
                    buf += raw
                    if len(buf) > 2048:
                        buf = b""
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        self._handle(line)
                continue
            except (serial.SerialException, OSError, ValueError) as e:
                err = str(e) or e.__class__.__name__
            finally:
                if ser is not None:
                    try: ser.close()
                    except Exception: pass
            if not self._alive(gen):
                return
            with self._lock:
                self._connected, self._ser, self._error = False, None, err
            self._emit()
            if not ever:                     # el primer intento falló: avisar y no insistir
                self._first.set()
                return
            for _ in range(int(config.SERIAL_RETRY_S * 10)):    # puerto perdido: reintentar
                if not self._alive(gen):
                    return
                time.sleep(0.1)

    def _io(self, direction: str, text: str):
        try:
            self._on_event({"type": "rotor_io", "dir": direction, "text": text[:200]})
        except Exception:
            pass

    def _handle(self, raw: bytes):
        if raw.startswith(BNO_PREFIX):
            capture = self.on_bno_sample
            if capture is not None:
                try:
                    capture(raw)
                except OSError:
                    self.on_bno_sample = None
                    logging.exception("Se detuvo la captura BNO por un error al escribir el CSV")
            return
        self._io("rx", raw.decode("utf-8", "replace").strip())
        parsed = parse_line(raw.decode("utf-8", "replace"))
        if parsed is None:
            return
        kind, val = parsed
        if kind == "pos":
            with self._lock:
                self._az, self._el = val
                self._pos_ts = time.monotonic()
            self._emit()
        elif kind == "bat":
            with self._lock:
                self._bat = val
                self._bat_ts = time.monotonic()
            self._emit()
        elif val.upper() != "ACK":
            now = time.monotonic()
            if len(self._msg_times) == self._msg_times.maxlen and now - self._msg_times[0] < 1.0:
                return                                  # máx. 4 mensajes por segundo al registro
            self._msg_times.append(now)
            try:
                self._on_event({"type": "rotor_msg", "text": val[:200]})
            except Exception:
                pass
