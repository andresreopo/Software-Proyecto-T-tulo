"""
mock_maia.py
============
Simulador de Maia SDR para probar MaiaView sin la Pluto.

Imita los dos servicios que usa la interfaz:
  GET/PATCH  http://<host>:8000/api/ad9361   → configuración del AD9361
  WS         ws://<host>:8000/waterfall      → frames de 4096 puntos float32 (dB)

Las señales simuladas están en frecuencias absolutas (MHz), así que al
cambiar la frecuencia central o el sample rate se mueven/desaparecen igual
que en un receptor real.

Uso:
    pip install flask flask-sock numpy
    python mock_maia.py                 # escucha en 127.0.0.1:8000

Luego, en otra terminal:
    python server/app.py --pluto-ip 127.0.0.1
y abre http://localhost:5001
"""

import argparse
import threading
import time

import numpy as np
from flask import Flask, jsonify, request
from flask_sock import Sock

N = 4096
FRAME_PERIOD_S = 0.05          # ~20 frames/s
NOISE_FLOOR_DB = -95.0

# (frecuencia MHz, potencia pico dB, ancho MHz, deriva Hz/s)
SIGNALS = [
    (137.500,  -55.0, 0.030, 0),       # tipo NOAA APT
    (145.800,  -60.0, 0.010, 0),       # portadora VHF estrecha
    (430.000,  -50.0, 0.050, 0),       # portadora justo en el centro por defecto
    (431.250,  -65.0, 0.200, 0),       # señal ancha
    (437.500,  -58.0, 0.015, 8000),    # tipo satélite con deriva Doppler
    (1090.000, -70.0, 0.500, 0),       # tipo ADS-B
]

app = Flask(__name__)
sock = Sock(app)
lock = threading.Lock()

cfg = {
    "rx_lo_frequency": 430_000_000,
    "sampling_frequency": 61_440_000,
    "rx_rf_bandwidth": 56_000_000,
    "rx_gain": 50.0,
    "rx_gain_mode": "Manual",
}


@app.route("/api/ad9361", methods=["GET", "PATCH"])
def ad9361():
    if request.method == "PATCH":
        data = request.get_json(silent=True) or {}
        with lock:
            for k, v in data.items():
                if k in cfg:
                    cfg[k] = v
    with lock:
        return jsonify(dict(cfg))


def make_frame(t0: float) -> bytes:
    with lock:
        fc = cfg["rx_lo_frequency"]
        sr = cfg["sampling_frequency"]
        gain = cfg["rx_gain"]

    freqs = np.linspace(fc - sr / 2, fc + sr / 2, N)      # Hz
    spec = NOISE_FLOOR_DB + 2.5 * np.random.randn(N)       # piso de ruido
    t = time.time() - t0

    for f_mhz, power, width_mhz, drift in SIGNALS:
        f0 = f_mhz * 1e6 + drift * (t % 60 - 30)           # deriva tipo Doppler
        sigma = max(width_mhz * 1e6, sr / N)               # al menos 1 bin
        spec += (power - NOISE_FLOOR_DB) * np.exp(-0.5 * ((freqs - f0) / sigma) ** 2)

    spec += (gain - 50.0) * 0.5                            # la ganancia sube el nivel
    return spec.astype("<f4").tobytes()


@sock.route("/waterfall")
def waterfall(ws):
    t0 = time.time()
    while True:
        ws.send(make_frame(t0))        # bytes → frame binario
        time.sleep(FRAME_PERIOD_S)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description="Simulador de Maia SDR")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8000)
    args = p.parse_args()
    print(f"[MockMaia] REST: http://{args.host}:{args.port}/api/ad9361")
    print(f"[MockMaia] WS  : ws://{args.host}:{args.port}/waterfall")
    app.run(host=args.host, port=args.port, threaded=True)