"""Rotor y receptor sintéticos alimentados por una órbita real propagada."""

from __future__ import annotations

import datetime as dt
import math
import random
import threading
import time
import uuid

from orbit import CHILE, UTC, find_passes, position, range_rate_km_s


C_KM_S = 299_792.458
SAMPLE_POINTS = 256
SPAN_MHZ = 6.0


def _angle_error(target, current):
    return (target - current + 180) % 360 - 180


def _move(current, target, limit):
    difference = _angle_error(target, current)
    return (current + max(-limit, min(limit, difference))) % 360


class SimulationSession:
    def __init__(self, record, observer, transmitter, speed=1, azimuth=None, hours=None, realtime=False):
        self.id = uuid.uuid4().hex
        self.realtime = bool(realtime)             # True: el reloj de la sesión ES la hora real (1×, sin pausa)
        if self.realtime:
            speed = 1
        self.record = record
        self.observer = dict(observer)
        self.transmitter = dict(transmitter)
        self.speed = speed
        self.lock = threading.RLock()
        self.created_wall = time.monotonic()
        self.last_wall = self.created_wall
        self.passes = find_passes(record, observer, days=1 if azimuth else 5, limit=5,
                                  azimuth=azimuth, hours=hours)
        if not self.passes:
            if azimuth:
                raise ValueError("No hay pases compatibles con los filtros en las próximas 24 horas")
            raise ValueError("Este satélite no tiene un pase sobre la elevación mínima en cinco días")
        self.pass_info = self.passes[0]
        now = dt.datetime.now(UTC)
        aos = dt.datetime.fromisoformat(self.pass_info["aos_utc"])
        self.sim_anchor = now if (self.realtime or self.pass_info.get("in_progress")) else aos - dt.timedelta(seconds=20)
        self.wall_anchor = self.created_wall
        self.paused = False
        self.stopped = False
        initial = position(record, observer, self.sim_anchor)
        self.rotor_az = (self.pass_info.get("aos_az_deg") or initial["az_deg"]) % 360
        self.rotor_el = max(0.0, initial["el_deg"]) if self.pass_info.get("in_progress") else 0.0
        self.frames_received = 0
        self.locked_seconds = 0.0

    def _sim_time(self, wall):
        if self.paused:
            return self.sim_anchor
        return self.sim_anchor + dt.timedelta(seconds=(wall - self.wall_anchor) * self.speed)

    def clock(self):
        """(hora simulada, velocidad, en pausa, detenida) de forma atómica, para el seguidor del rotor."""
        with self.lock:
            return self._sim_time(time.monotonic()), self.speed, self.paused, self.stopped

    def control(self, action, speed=None):
        if self.realtime and action in ("pause", "resume", "speed"):
            raise ValueError("En modo tiempo real no se puede pausar ni cambiar la velocidad.")
        with self.lock:
            wall = time.monotonic()
            now_sim = self._sim_time(wall)
            if action == "pause":
                self.sim_anchor = now_sim
                self.wall_anchor = wall
                self.paused = True
            elif action == "resume":
                self.sim_anchor = now_sim
                self.wall_anchor = wall
                self.paused = False
            elif action == "speed":
                if speed not in (1, 5, 10, 20):
                    raise ValueError("Velocidad permitida: 1, 5, 10 o 20")
                self.sim_anchor = now_sim
                self.wall_anchor = wall
                self.speed = speed
            elif action == "stop":
                self.stopped = True
            else:
                raise ValueError("Acción no válida")

    def tracking_status(self):
        when, _, paused, stopped = self.clock()
        aos = dt.datetime.fromisoformat(self.pass_info["aos_utc"])
        los = dt.datetime.fromisoformat(self.pass_info["los_utc"])
        if stopped or paused or not aos <= when < los:
            return None
        norad_id = int(self.record["NORAD_CAT_ID"])
        return {"norad_id": norad_id, "name": self.record.get("OBJECT_NAME") or f"NORAD {norad_id}"}

    def path(self, step_s=4):
        """Trayectoria completa del pase [[az, el], ...] entre AOS y LOS (para dibujar el radar)."""
        aos = dt.datetime.fromisoformat(self.pass_info["aos_utc"])
        los = dt.datetime.fromisoformat(self.pass_info["los_utc"])
        n = max(2, int((los - aos).total_seconds() / step_s))
        out = []
        for i in range(n + 1):
            g = position(self.record, self.observer, aos + (los - aos) * (i / n))
            out.append([round(g["az_deg"], 2), round(g["el_deg"], 2)])
        return out

    def state(self):
        with self.lock:
            if self.stopped:
                return {"id": self.id, "status": "stopped", "simulated": True}
            wall = time.monotonic()
            sim_time = self._sim_time(wall)
            elapsed_wall = max(0.0, min(wall - self.last_wall, 5.0))
            self.last_wall = wall
            geometry = position(self.record, self.observer, sim_time)
            target_az = geometry["az_deg"]
            target_el = max(0.0, min(90.0, geometry["el_deg"]))
            if not self.paused:
                self.rotor_az = _move(self.rotor_az, target_az, 12.0 * elapsed_wall)
                delta_el = target_el - self.rotor_el
                self.rotor_el += max(-6.0 * elapsed_wall, min(6.0 * elapsed_wall, delta_el))
            az_error = abs(_angle_error(target_az, self.rotor_az))
            pointing_error = math.hypot(az_error * max(0.15, math.cos(math.radians(target_el))),
                                        target_el - self.rotor_el)
            visible = geometry["el_deg"] >= self.observer["min_el"]
            aos = dt.datetime.fromisoformat(self.pass_info["aos_utc"])
            los = dt.datetime.fromisoformat(self.pass_info["los_utc"])
            progress = max(0.0, min(1.0, (sim_time - aos).total_seconds() / max(1, (los - aos).total_seconds())))
            phase = "preparación" if sim_time < aos else "en pase" if sim_time < los else "finalizado"
            frequency_hz = self.transmitter["frequency_mhz"] * 1e6
            doppler_hz = -frequency_hz * range_rate_km_s(self.record, self.observer, sim_time) / C_KM_S
            noise_floor = -105.0
            strength = (-70.0 - 20 * math.log10(max(geometry["range_km"], 1) / 700)
                        - 0.38 * pointing_error)
            if not visible:
                strength = noise_floor - 10
            snr = max(0.0, round(strength - noise_floor, 1)) if visible else 0.0
            lock = visible and pointing_error < 20 and snr >= 6
            if lock and not self.paused:
                self.locked_seconds += elapsed_wall
                self.frames_received += int(8 * elapsed_wall * max(0.4, min(1.0, snr / 25)) + 0.5)
            rng = random.Random(int(sim_time.timestamp()) ^ int(self.record["NORAD_CAT_ID"]))
            width_hz = 190_000 if "FM" in self.transmitter["mode"].upper() else 400_000
            center = doppler_hz / (SPAN_MHZ * 1e6)
            spectrum = []
            for i in range(SAMPLE_POINTS):
                x = (i / (SAMPLE_POINTS - 1) - 0.5)
                offset_hz = (x - center) * SPAN_MHZ * 1e6
                envelope = math.exp(-0.5 * (offset_hz / width_hz) ** 2)
                shoulder = 0.18 * math.exp(-0.5 * (offset_hz / (width_hz * 2.3)) ** 2)
                linear = 10 ** (noise_floor / 10)
                if visible:
                    linear += (envelope + shoulder) * 10 ** (strength / 10)
                spectrum.append(round(10 * math.log10(linear) + rng.gauss(0, 1.2), 1))
            if phase == "finalizado" and sim_time > los + dt.timedelta(seconds=45):
                self.sim_anchor = los + dt.timedelta(seconds=45)
                self.wall_anchor = wall
                self.paused = True
            return {
                "id": self.id,
                "status": "paused" if self.paused else "running",
                "simulated": True,
                "hardware_connected": False,
                "norad_id": int(self.record["NORAD_CAT_ID"]),
                "name": self.record.get("OBJECT_NAME"),
                "sim_time_utc": sim_time.isoformat(),
                "sim_time_chile": sim_time.astimezone(CHILE).isoformat(timespec="seconds"),
                "speed": self.speed,
                "realtime": self.realtime,
                "phase": phase,
                "progress": round(progress, 3),
                "pass": self.pass_info,
                "geometry": {**geometry, "visible": visible},
                "rotor": {"az_deg": round(self.rotor_az, 2), "el_deg": round(self.rotor_el, 2),
                          "target_az_deg": target_az, "target_el_deg": target_el,
                          "pointing_error_deg": round(pointing_error, 2), "simulated": True},
                "receiver": {"frequency_mhz": self.transmitter["frequency_mhz"],
                             "mode": self.transmitter["mode"], "source": self.transmitter.get("source"),
                             "span_mhz": SPAN_MHZ, "doppler_hz": round(doppler_hz),
                             "snr_db": snr, "signal_detected": lock,
                             "frames_received": self.frames_received,
                             "spectrum_db": spectrum, "simulated": True},
            }


class SessionStore:
    def __init__(self):
        self.lock = threading.RLock()
        self.sessions = {}

    def add(self, session):
        with self.lock:
            self.sessions[session.id] = session
            # Las sesiones son efímeras; limita acumulación al probar muchos satélites.
            old = [key for key, item in self.sessions.items()
                   if time.monotonic() - item.created_wall > 7200 or item.stopped]
            for key in old:
                if key != session.id:
                    self.sessions.pop(key, None)

    def get(self, session_id):
        with self.lock:
            return self.sessions.get(session_id)

    def current(self):
        with self.lock:
            return self.sessions[next(reversed(self.sessions))] if self.sessions else None


sessions = SessionStore()
