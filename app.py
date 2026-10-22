"""Selector + seguimiento + receptor Maia/Pluto simulados en una sola web."""

from __future__ import annotations

import argparse
import math

from flask import Flask, jsonify, render_template, request

from catalog import store
from orbit import UTC, find_passes, position
from simulation import SimulationSession, sessions


app = Flask(__name__)


def observer_from(values):
    try:
        observer = {
            "lat": float(values.get("lat", -33.4489)),
            "lon": float(values.get("lon", -70.6693)),
            "alt_m": float(values.get("alt_m", 570)),
            "min_el": float(values.get("min_el", 10)),
        }
    except (TypeError, ValueError) as exc:
        raise ValueError("La ubicación debe contener números válidos") from exc
    if not all(math.isfinite(x) for x in observer.values()):
        raise ValueError("La ubicación contiene un valor no finito")
    if not (-90 <= observer["lat"] <= 90 and -180 <= observer["lon"] <= 180
            and -500 <= observer["alt_m"] <= 10000 and 0 <= observer["min_el"] <= 90):
        raise ValueError("Ubicación fuera de rango")
    return observer


def error(message, code=400):
    return jsonify({"ok": False, "error": str(message)}), code


@app.get("/")
def index():
    return render_template("index.html")


@app.get("/api/status")
def status():
    return jsonify({"ok": True, "mode": "simulation", "hardware_connected": False,
                    "catalog": store.status()})


@app.post("/api/catalog/refresh")
def refresh():
    started = store.refresh_async()
    return jsonify({"ok": True, "started": started, "catalog": store.status()})


@app.get("/api/catalog")
def catalog():
    try:
        observer = observer_from(request.args)
        page = int(request.args.get("page", 1))
        page_size = int(request.args.get("page_size", 40))
        band = request.args.get("band", "s")
        if page < 1 or page_size < 1 or page_size > 100 or band not in ("s", "all"):
            raise ValueError("Filtro o paginación inválidos")
        data = store.search(request.args.get("q", ""), band,
                            request.args.get("visible") == "1", observer, page, page_size)
        return jsonify({"ok": True, **data, "observer": observer, "catalog": store.status()})
    except ValueError as exc:
        return error(exc)


@app.get("/api/catalog/<int:norad_id>")
def satellite_detail(norad_id):
    try:
        observer = observer_from(request.args)
        record = store.record(norad_id)
        if record is None:
            return error("Satélite no encontrado en el catálogo orbital", 404)
        from datetime import datetime
        now = datetime.now(UTC)
        return jsonify({"ok": True, "satellite": {
            "norad_id": norad_id,
            "name": record.get("OBJECT_NAME"),
            "object_id": record.get("OBJECT_ID"),
            "epoch_utc": record.get("EPOCH"),
            "orbit": {"inclination_deg": record.get("INCLINATION"),
                      "period_min": round(1440 / float(record["MEAN_MOTION"]), 2)},
            "current": position(record, observer, now),
            "passes": find_passes(record, observer, start=now, days=5, limit=8),
            "transmitters": store.transmitters_for(norad_id),
            "catalog": store.status(),
        }})
    except (ValueError, KeyError, ArithmeticError) as exc:
        return error(exc)


@app.post("/api/session")
def start_session():
    body = request.get_json(silent=True) or {}
    try:
        norad_id = int(body.get("norad_id"))
        observer = observer_from(body)
        speed = int(body.get("speed", 10))
        if speed not in (1, 5, 10, 20):
            raise ValueError("Velocidad permitida: 1, 5, 10 o 20")
        record = store.record(norad_id)
        if record is None:
            return error("Satélite no encontrado", 404)
        transmitters = store.transmitters_for(norad_id, band_only=True)
        selected_uuid = body.get("transmitter_uuid")
        transmitter = next((tx for tx in transmitters if tx["uuid"] == selected_uuid), None)
        if selected_uuid and transmitter is None:
            raise ValueError("El transmisor seleccionado no pertenece al satélite")
        if transmitter is None and transmitters and not body.get("manual_frequency_mhz"):
            transmitter = transmitters[0]
        if transmitter is None:
            try:
                frequency = float(body.get("manual_frequency_mhz"))
            except (TypeError, ValueError) as exc:
                raise ValueError("Elige una bajada S o indica una frecuencia simulada") from exc
            if not math.isfinite(frequency) or not 2000 <= frequency <= 4000:
                raise ValueError("La frecuencia manual debe estar entre 2000 y 4000 MHz")
            transmitter = {"frequency_mhz": frequency, "mode": "Sin confirmar", "source": "manual"}
        else:
            transmitter = {**transmitter, "source": "SatNOGS DB"}
        session = SimulationSession(record, observer, transmitter, speed)
        sessions.add(session)
        return jsonify({"ok": True, "session_id": session.id, "state": session.state()})
    except (TypeError, ValueError, KeyError, ArithmeticError) as exc:
        return error(exc)


@app.get("/api/session/<session_id>")
def get_session(session_id):
    session = sessions.get(session_id)
    if session is None:
        return error("Sesión no encontrada o expirada", 404)
    try:
        return jsonify({"ok": True, "state": session.state()})
    except (ValueError, ArithmeticError) as exc:
        return error(exc, 500)


@app.post("/api/session/<session_id>/control")
def control_session(session_id):
    session = sessions.get(session_id)
    if session is None:
        return error("Sesión no encontrada o expirada", 404)
    body = request.get_json(silent=True) or {}
    try:
        speed = int(body["speed"]) if "speed" in body else None
        session.control(body.get("action"), speed)
        return jsonify({"ok": True, "state": session.state()})
    except (TypeError, ValueError, ArithmeticError) as exc:
        return error(exc)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="SatTrack + MaiaView (simulación)")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5003)
    parser.add_argument("--refresh-on-start", action="store_true")
    args = parser.parse_args()
    if args.refresh_on_start:
        store.refresh_async()
    app.run(host=args.host, port=args.port, threaded=True, use_reloader=False)
