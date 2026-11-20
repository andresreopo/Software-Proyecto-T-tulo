"""Máximo de SNR medido por pase a partir de los espectros lineales de Maia."""

import math
import sqlite3
from contextlib import closing
from datetime import datetime, timezone

import numpy as np


FIELDS = ("session_id", "norad_id", "satellite", "measured_utc",
          "snr_max_db", "peak_freq_hz", "center_freq_hz")
DOPPLER_MARGIN_HZ = 250_000


def measure_peak(frame, center_hz, span_hz, downlink_hz, downlink_high_hz):
    """Devuelve el pico de la bajada y su relación con la mediana de ruido de la banda."""
    if len(frame) % 4:
        return None
    power = np.frombuffer(frame, dtype="<f4")
    n = power.size
    if n < 2 or not center_hz or not span_hz or span_hz <= 0:
        return None
    bin_hz = span_hz / n
    frequencies = center_hz + (np.arange(n) - n // 2) * bin_hz
    valid = (power > 0) & np.isfinite(power)
    # La mediana excluye los bordes atenuados del espectro.
    trim = n // 10
    interior = power[trim:n - trim][valid[trim:n - trim]]
    if interior.size == 0:
        return None
    margin = max(DOPPLER_MARGIN_HZ, bin_hz / 2)
    band = (frequencies >= downlink_hz - margin) & (frequencies <= downlink_high_hz + margin)
    candidates = np.flatnonzero(valid & band)
    if candidates.size == 0:
        return None
    peak = candidates[np.argmax(power[candidates])]
    noise = float(np.median(interior.astype(np.float64)))
    snr_db = 10 * (math.log10(float(power[peak])) - math.log10(noise))
    return snr_db, float(frequencies[peak])


class RFPassMetrics:
    def __init__(self, path):
        self.path = path
        self.error = None
        # Solo el lector del WebSocket escribe. SQLite protege las lecturas concurrentes de Flask.
        self._last_session = None
        self._best = -math.inf

    def observe(self, session, frame, center_hz, span_hz, received_at):
        tracking = session.tracking_status() if session else None
        if not tracking:
            return
        downlink_hz = session.transmitter["frequency_mhz"] * 1e6
        high_mhz = session.transmitter.get("high_mhz")
        high_hz = max(downlink_hz, high_mhz * 1e6) if high_mhz else downlink_hz
        peak = measure_peak(frame, center_hz, span_hz, downlink_hz, high_hz)
        if peak is None:
            return
        if session.id != self._last_session:
            self._last_session = session.id
            self._best = -math.inf
        snr_db, peak_hz = peak
        if snr_db <= self._best:
            return
        try:
            with closing(sqlite3.connect(self.path, timeout=1)) as db, db:
                db.execute("""
                    CREATE TABLE IF NOT EXISTS rf_pass_peaks (
                        session_id TEXT PRIMARY KEY, norad_id INTEGER NOT NULL, satellite TEXT NOT NULL,
                        measured_utc TEXT NOT NULL, snr_max_db REAL NOT NULL, peak_freq_hz REAL NOT NULL,
                        center_freq_hz REAL NOT NULL
                    )
                """)
                db.execute("""
                    INSERT INTO rf_pass_peaks VALUES (?, ?, ?, ?, ?, ?, ?)
                    ON CONFLICT(session_id) DO UPDATE SET
                        measured_utc = excluded.measured_utc, snr_max_db = excluded.snr_max_db,
                        peak_freq_hz = excluded.peak_freq_hz, center_freq_hz = excluded.center_freq_hz
                    WHERE excluded.snr_max_db > rf_pass_peaks.snr_max_db
                """, (session.id, tracking["norad_id"], tracking["name"],
                      datetime.fromtimestamp(received_at, timezone.utc).isoformat(),
                      snr_db, peak_hz, center_hz))
        except sqlite3.Error as exc:
            self.error = f"No se pudieron guardar las métricas RF: {exc}"
        else:
            self.error = None
            self._best = snr_db

    def _read(self, query, params=()):
        if not self.path.is_file():
            return []
        with closing(sqlite3.connect(self.path, timeout=1)) as db, db:
            db.row_factory = sqlite3.Row
            return [dict(row) for row in db.execute(query, params)]

    def peak(self, session_id):
        rows = self._read("""
            SELECT * FROM rf_pass_peaks WHERE (? IS NULL OR session_id = ?)
            ORDER BY measured_utc DESC LIMIT 1
        """, (session_id, session_id))
        return rows[0] if rows else None

    def history(self):
        return self._read("SELECT * FROM rf_pass_peaks ORDER BY measured_utc DESC")
