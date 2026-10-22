"""Cliente directo de Maia SDR (firmware en la Pluto), sin pasar por MaiaView.

Servicios usados (puerto 8000 de Maia):
  GET/PATCH /api/ad9361  -> rx_lo_frequency, sampling_frequency, rx_rf_bandwidth, rx_gain, rx_gain_mode
  WS        /waterfall   -> frames binarios de 4096 float32 little-endian (dB)
"""
from __future__ import annotations

import json
import os
import queue
import threading
import time
from urllib.parse import urlparse

import requests
import websocket

STALE_AFTER_S = 2.0          # sin bloques por este tiempo => ya no se considera "streaming"
CONFIG_POLL_S = 2.0


class MaiaRFClient:
    def __init__(self, base_url: str | None = None):
        self.base_url = (base_url or os.getenv("MAIA_SERVER_URL", "http://127.0.0.1:8000")).rstrip("/")
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
        last = state["last_frame_unix"]
        if state["streaming"] and last is not None and time.time() - last > STALE_AFTER_S:
            state["streaming"] = False
            state["maia_status"] = "stalled"
        cfg = state["config"] or {}
        fc, fs = cfg.get("rx_lo_frequency"), cfg.get("sampling_frequency")
        state["center_freq_hz"] = float(fc) if fc is not None else None
        state["span_hz"] = float(fs) if fs is not None else None
        return state

    def set_config(self, cfg):
        """Actualiza la copia de la configuración (p. ej. tras un PATCH exitoso)."""
        with self._lock:
            self._state["config"] = cfg

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
                r = requests.get(f"{self.base_url}/api/ad9361", timeout=2)
                r.raise_for_status()
                with self._lock:
                    self._state["config"] = r.json()
            except (requests.RequestException, ValueError):
                pass  # el estado de conexión lo informa el WS; la config queda con el último valor
            time.sleep(CONFIG_POLL_S)

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
                ws.run_forever(ping_interval=25, ping_timeout=10)
            except Exception as exc:
                with self._lock:
                    self._state["error"] = str(exc)
                    self._state["connected"] = False
                    self._state["streaming"] = False
            if self._is_running():
                time.sleep(2)

    def _on_open(self, ws):
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
        with self._lock:
            self._state["streaming"] = True
            self._state["frames_received"] += 1
            self._state["sample_count"] = len(message) // 4
            self._state["last_frame_unix"] = time.time()
            self._state["error"] = None
        # float32 little-endian tal como llega; el navegador lo interpreta con Float32Array.
        self._broadcast(bytes(message))


maia_rf = MaiaRFClient()
