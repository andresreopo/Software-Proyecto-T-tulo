import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

import numpy as np

from rf_metrics import RFPassMetrics, measure_peak


CENTER_HZ = 2_200_000_000
SPAN_HZ = 4_096_000
DOWNLINK_HZ = CENTER_HZ + 100_000


def spectrum(peak_power, index=2148):
    power = np.full(4096, 10, dtype="<f4")
    power[index] = peak_power
    power[80] = 100_000  # interferencia ajena a la bajada
    power[0:2] = [np.nan, 0]
    return power.tobytes()


class RFMetricsTest(unittest.TestCase):
    def test_peak_in_downlink_not_strongest_elsewhere(self):
        snr, frequency = measure_peak(spectrum(1000), CENTER_HZ, SPAN_HZ, DOWNLINK_HZ, DOWNLINK_HZ)
        self.assertAlmostEqual(snr, 20)
        self.assertEqual(frequency, DOWNLINK_HZ)
        self.assertIsNone(measure_peak(spectrum(1000), CENTER_HZ, SPAN_HZ, 145e6, 145e6))
        self.assertIsNone(measure_peak(bytes(4096 * 4), CENTER_HZ, SPAN_HZ, DOWNLINK_HZ, DOWNLINK_HZ))

    def test_persists_best_frame_per_pass_with_its_center_frequency(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "metrics.sqlite3"
            metrics = RFPassMetrics(path)
            session = SimpleNamespace(id="a", transmitter={"frequency_mhz": DOWNLINK_HZ / 1e6},
                                      tracking_status=lambda: None)
            self.assertIsNone(metrics.peak(None))
            metrics.observe(session, spectrum(10_000), CENTER_HZ, SPAN_HZ, 100)
            self.assertFalse(path.exists())
            session.tracking_status = lambda: {"norad_id": 42, "name": "Prueba"}
            metrics.observe(session, spectrum(100), CENTER_HZ, SPAN_HZ, 200)
            # El mejor frame llega después de sintonizar 20 kHz más arriba.
            metrics.observe(session, spectrum(1000, 2128), CENTER_HZ + 20_000, SPAN_HZ, 300)
            metrics.observe(session, spectrum(50), CENTER_HZ, SPAN_HZ, 400)
            session.tracking_status = lambda: None
            metrics.observe(session, spectrum(100_000), CENTER_HZ, SPAN_HZ, 500)
            reopened = RFPassMetrics(path)
            rows = reopened.history()
            self.assertEqual(len(rows), 1)
            peak = reopened.peak("a")
            self.assertAlmostEqual(peak["snr_max_db"], 20)
            self.assertEqual(peak["peak_freq_hz"], DOWNLINK_HZ)
            self.assertEqual(peak["center_freq_hz"], CENTER_HZ + 20_000)
            self.assertEqual(peak["measured_utc"], "1970-01-01T00:05:00+00:00")
            self.assertIsNone(reopened.peak("new-pass"))


if __name__ == "__main__":
    unittest.main()
