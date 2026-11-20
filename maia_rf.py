"""Cliente directo de Maia SDR (firmware en la Pluto), sin pasar por MaiaView.

Servicios usados (puerto 8000 de Maia):
  GET/PATCH /api/ad9361  -> rx_lo_frequency, sampling_frequency, rx_rf_bandwidth, rx_gain, rx_gain_mode
  WS        /waterfall   -> frames binarios float32 little-endian de potencia lineal
"""
from __future__ import annotations

import json
import math
import os
import queue
import threading
import time
from urllib.parse import urlparse

import requests
import websocket

DEFAULT_MAIA_URL = "http://192.168.2.1:8000"   # IP de fábrica de la Pluto (igual que MaiaView)
STALE_AFTER_S = 2.0          # sin bloques por este tiempo => ya no se considera "streaming"
CONFIG_POLL_S = 2.0
RETUNE_SETTLE_S = 0.25


class MaiaRFClient:
    def __init__(self, base_url: str | None = None):
        self.base_url = (base_url or os.getenv("MAIA_SERVER_URL", DEFAULT_MAIA_URL)).rstrip("/")
        parsed = urlparse(self.base_url)
        ws_scheme = "wss" if parsed.scheme == "https" else "ws"
        self.ws_url = f"{ws_scheme}://{parsed.netloc}/waterfall"
        self._lock = threading.RLock()
        self._running = False
        self._thread = None
        self._ws = None
        self._state = {
            "connected": False,
            "streaming": False,
            "frames_received": 0,
            "sample_count": 0,
            "last_frame_unix": None,
            "error": None,
            "maia_status": "connecting",
            "config": None,          # última respuesta de GET /api/ad9361 de Maia
        }
        self._cfg_thread = None
        self._subs: set[queue.Queue] = set()
        self._spectrum_config = (0, None)  # ajuste DDC y ancho del espectro, si Maia usa DDC
        self._settle_until = 0.0
        self.on_frame = None

    def set_url(self, base_url: str):
        """Cambia la dirección de Maia (antes de start())."""
        base_url = base_url.strip().rstrip("/")
        if "://" not in base_url:
            base_url = "http://" + base_url
        parsed = urlparse(base_url)
        self.base_url = base_url
        self.ws_url = f"{'wss' if parsed.scheme == 'https' else 'ws'}://{parsed.netloc}/waterfall"

    def start(self):
        with self._lock:
            if self._running:
                return
            self._running = True
            self._thread = threading.Thread(target=self._worker, daemon=True, name="maia-rf")
            self._thread.start()
            self._cfg_thread = threading.Thread(target=self._config_worker, daemon=True, name="maia-cfg")
            self._cfg_thread.start()

    def stop(self):
        with self._lock:
            self._running = False
            ws = self._ws
        if ws:
            try:
                ws.close()
            except Exception:
                pass

    def snapshot(self):
        with self._lock:
            state = dict(self._state)
            state["center_freq_hz"], state["span_hz"] = self._spectrum_coordinates()
        last = state["last_frame_unix"]
        if state["streaming"] and last is not None and time.time() - last > STALE_AFTER_S:
            state["streaming"] = False
            state["maia_status"] = "stalled"
        return state

    def _spectrum_coordinates(self):
        cfg = self._state["config"] or {}
        offset, ddc_span = self._spectrum_config
        try:
            center = float(cfg["rx_lo_frequency"]) + offset
            span = float(cfg["sampling_frequency"] if ddc_span is None else ddc_span)
        except (TypeError, ValueError, KeyError):
            return None, None
        if not math.isfinite(center) or not math.isfinite(span) or center <= 0 or span <= 0:
            return None, None
        return center, span

    def set_config(self, cfg, spectrum_config=None):
        """Actualiza la copia de la configuración (p. ej. tras un PATCH exitoso)."""
        with self._lock:
            previous = self._spectrum_coordinates()
            self._state["config"] = cfg
            if spectrum_config is not None:
                self._spectrum_config = spectrum_config
            if self._spectrum_coordinates() != previous:
                self._settle_until = time.monotonic() + RETUNE_SETTLE_S

    # ── Relay hacia los navegadores (un Queue por cliente; si va lento, descarta el más viejo) ──
    def subscribe(self):
        q = queue.Queue(maxsize=8)
        with self._lock:
            self._subs.add(q)
        return q

    def unsubscribe(self, q):
        with self._lock:
            self._subs.discard(q)

    def _broadcast(self, item):
        with self._lock:
            subs = list(self._subs)
        for q in subs:
            try:
                q.put_nowait(item)
            except queue.Full:
                try:
                    q.get_nowait()
                    q.put_nowait(item)
                except (queue.Empty, queue.Full):
                    pass

    def ws_state(self):
        return "connected" if self.snapshot()["connected"] else "disconnected"

    def _config_worker(self):
        while self._is_running():
            try:
                self._refresh_config()
            except (requests.RequestException, ValueError, TypeError, KeyError):
                pass  # el estado de conexión lo informa el WS; la config queda con el último valor
            time.sleep(CONFIG_POLL_S)

    def _refresh_config(self):
        response = requests.get(f"{self.base_url}/api", timeout=2)
        try:
            state = response.json() if response.ok else None
        except ValueError:
            state = None
        if isinstance(state, dict) and isinstance(state.get("ad9361"), dict):
            spectrometer = state.get("spectrometer", {})
            geometry = (0, None)
            if str(spectrometer.get("input", "")).upper() == "DDC":
                geometry = (float(state["ddc"]["frequency"]), spectrometer["input_sampling_frequency"])
            self.set_config(state["ad9361"], geometry)
            return
        response = requests.get(f"{self.base_url}/api/ad9361", timeout=2)
        response.raise_for_status()
        self.set_config(response.json(), (0, None))

    def _is_running(self):
        with self._lock:
            return self._running

    def _worker(self):
        while self._is_running():
            try:
                ws = websocket.WebSocketApp(
                    self.ws_url,
                    on_open=self._on_open,
                    on_message=self._on_message,
                    on_error=self._on_error,
                    on_close=self._on_close,
                )
                with self._lock:
                    self._ws = ws
                ws.run_forever()   # sin pings: como MaiaView, que conectaba bien con Maia
            except Exception as exc:
                with self._lock:
                    self._state["error"] = str(exc)
                    self._state["connected"] = False
                    self._state["streaming"] = False
            if self._is_running():
                time.sleep(2)

    def _on_open(self, ws):
        print(f"[Maia] WebSocket conectado: {self.ws_url}")
        with self._lock:
            self._state["connected"] = True
            self._state["error"] = None
            self._state["maia_status"] = "connected"
        self._broadcast(json.dumps({"type": "status", "ws": "connected"}))

    def _on_close(self, ws, code, reason):
        with self._lock:
            self._state["connected"] = False
            self._state["streaming"] = False
            self._state["maia_status"] = "disconnected"
        self._broadcast(json.dumps({"type": "status", "ws": "disconnected"}))

    def _on_error(self, ws, error):
        print(f"[Maia] error de WebSocket ({self.ws_url}): {error}")
        with self._lock:
            self._state["error"] = str(error)
            self._state["connected"] = False
            self._state["streaming"] = False
            self._state["maia_status"] = "error"

    def _on_message(self, ws, message):
        if isinstance(message, str):
            return  # Maia solo envía frames binarios por /waterfall
        if not isinstance(message, (bytes, bytearray, memoryview)) or len(message) < 256 or len(message) % 4:
            return
        received_at = time.time()
        with self._lock:
            self._state["streaming"] = True
            self._state["frames_received"] += 1
            self._state["sample_count"] = len(message) // 4
            self._state["last_frame_unix"] = received_at
            self._state["error"] = None
            center, span = self._spectrum_coordinates()
            settled = time.monotonic() >= self._settle_until
        # float32 little-endian tal como llega; el navegador lo interpreta con Float32Array.
        frame = bytes(message)
        self._broadcast(frame)
        if self.on_frame is not None and settled and center is not None:
            self.on_frame(frame, center, span, received_at)


maia_rf = MaiaRFClient()
