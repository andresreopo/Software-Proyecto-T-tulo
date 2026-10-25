"""Seguidor del rotor: envía a la ESP32 la posición y la velocidad angular del satélite de la sesión.

Por qué se envía también la velocidad: una referencia de posición suelta obliga al rotor a "perseguir"
un blanco que se mueve y siempre llega con retraso. Con la velocidad (TRK), la ESP32 extrapola entre
mensajes y mueve el rotor al mismo ritmo que el satélite.

La sesión de SATTRACK reproduce el próximo pase (empieza 20 s antes del AOS) a la velocidad elegida.
Las velocidades enviadas ya están multiplicadas por esa velocidad: a 1× es seguimiento en tiempo real;
a 5×, 10× o 20× casi ningún rotor puede seguir al satélite (se avisa).
"""
from __future__ import annotations

import datetime as dt
import threading
import time

import rotor_config as cfg
from orbit import UTC, az_el_rates, pass_rate_peaks


class RotorFollower:
    def __init__(self, link, on_event=None):
        self._link = link
        self._on_event = on_event or (lambda e: None)
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = None
        self._state = {"status": "idle"}

    def state(self):
        with self._lock:
            return dict(self._state)

    def active(self):
        return self.state().get("status") in ("waiting", "tracking", "paused")

    def session_id(self):
        return self.state().get("session_id")

    def _set(self, **kw):
        with self._lock:
            self._state.update(kw)
            ev = {"type": "follow", **self._state}
        try:
            self._on_event(ev)
        except Exception as exc:                       # un cliente roto no debe matar el hilo
            print("[RotorFollower] error en callback:", exc)

    # ── control ──────────────────────────────────────────────────────────────
    def start(self, session):
        """Arranca el seguimiento del pase de `session`. Lanza RuntimeError si el rotor no está conectado."""
        if not self._link.state()["connected"]:
            raise RuntimeError("Conecta primero el rotor (puerto serial de la ESP32).")
        self.stop(hold=False)
        info = session.pass_info
        aos = dt.datetime.fromisoformat(info["aos_utc"])
        los = dt.datetime.fromisoformat(info["los_utc"])
        warnings = []
        speed = session.clock()[1]
        try:
            max_az, max_el = pass_rate_peaks(session.record, session.observer, aos, los)
        except Exception:                              # pase "continuo" u órbita sin datos: sin aviso de velocidades
            max_az = max_el = None
        if max_az is not None:
            for label, peak, limit in (("azimut", max_az, cfg.ROTOR_MAX_RATE_AZ),
                                       ("elevación", max_el, cfg.ROTOR_MAX_RATE_EL)):
                need = peak * speed
                if need > limit:
                    warnings.append(f"A {speed}× el {label} debe girar hasta {need:.1f} °/s, más que los "
                                    f"{limit:.0f} °/s configurados del rotor: se quedará atrás.")
        if speed != 1:
            warnings.append(f"Velocidad de simulación {speed}×: para seguir al satélite en tiempo real usa 1×.")
        stop = threading.Event()
        self._stop = stop
        with self._lock:
            self._state = {"status": "waiting", "session_id": session.id, "speed": speed, "error": None,
                           "warnings": warnings, "az": None, "el": None, "vaz": None, "vel": None,
                           "t_to_aos": None, "t_to_los": None,
                           "max_az_rate": None if max_az is None else round(max_az, 2),
                           "max_el_rate": None if max_el is None else round(max_el, 2)}
        self._thread = threading.Thread(target=self._run, args=(stop, session, aos, los), daemon=True)
        self._thread.start()
        self._set()
        return self.state()

    def stop(self, hold=True):
        """Detiene el seguimiento. Con hold=True fija la referencia en el último punto para que el
        rotor se quede donde está y deje de extrapolar la velocidad."""
        self._stop.set()
        th = self._thread
        if th and th.is_alive() and th is not threading.current_thread():
            th.join(2.0)
        st = self.state()
        if st.get("status") in ("waiting", "tracking", "paused"):
            if hold and st.get("az") is not None:
                try:
                    self._link.send_target(st["az"], st["el"])
                except (RuntimeError, ValueError):
                    pass
            self._set(status="stopped")
        return self.state()

    # ── hilo ─────────────────────────────────────────────────────────────────
    def _run(self, stop, session, aos, los):
        period = 1.0 / cfg.TRACK_RATE_HZ
        next_t = time.monotonic()
        pre_t = hold_sent = None
        last_err = None
        while not stop.is_set():
            sim_now, speed, paused, stopped = session.clock()
            if stopped:
                self.stop(hold=True)
                return
            if sim_now >= los:
                st = self.state()
                if st.get("az") is not None:
                    try:
                        self._link.send_target(st["az"], st["el"])      # LOS: el rotor se queda donde está
                    except (RuntimeError, ValueError):
                        pass
                self._set(status="finished", t_to_aos=0.0, t_to_los=0.0)
                return
            try:
                if paused:
                    st = self.state()
                    if hold_sent is None and st.get("az") is not None:
                        self._link.send_target(st["az"], st["el"])
                        hold_sent = True
                    self._set(status="paused", speed=speed, error=None)
                elif sim_now < aos:
                    hold_sent = None
                    # Antes del AOS: llevar el rotor al punto de aparición (se reenvía cada 5 s por si el puerto se reconectó).
                    if pre_t is None or time.monotonic() - pre_t > 5.0:
                        aos_az = session.pass_info.get("aos_az_deg")
                        self._link.send_target(0.0 if aos_az is None else aos_az % 360.0, cfg.ROTOR_EL_MIN)
                        pre_t = time.monotonic()
                    self._set(status="waiting", speed=speed, error=None,
                              t_to_aos=(aos - sim_now).total_seconds() / speed,
                              t_to_los=(los - sim_now).total_seconds() / speed)
                else:
                    hold_sent = None
                    az, el, vaz, vel = az_el_rates(session.record, session.observer, sim_now)
                    vaz, vel = vaz * speed, vel * speed                  # °/s de tiempo real
                    el_cmd = min(max(el, cfg.ROTOR_EL_MIN), cfg.ROTOR_EL_MAX)
                    if el_cmd != el:                                     # el rotor no puede ir más allá del límite
                        vel = 0.0
                    self._link.send_track(az, el_cmd, vaz, vel)
                    self._set(status="tracking", speed=speed, error=None, az=az, el=el_cmd, vaz=vaz, vel=vel,
                              t_to_aos=0.0, t_to_los=(los - sim_now).total_seconds() / speed)
            except (RuntimeError, ValueError) as exc:                    # p. ej. el puerto se cayó: sigue intentando
                if str(exc) != last_err:
                    last_err = str(exc)
                    self._set(error=last_err)
            next_t += period
            stop.wait(max(0.0, next_t - time.monotonic()))
