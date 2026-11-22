import csv
import io
from pathlib import Path
from tempfile import TemporaryDirectory
import time
import unittest
from unittest.mock import patch

import app as sattrack
from capturar_bno import BnoCapture, CSV_HEADER, parse_bno
from serial_link import SerialLink


class BnoCaptureTest(unittest.TestCase):
    def test_validates_vectors_and_optional_reference(self):
        sample = b"BNO,200,1,0,0,9.81,0,0,0,35,0,40,3,nan,11,22,nan,nan"
        row = parse_bno(sample)
        self.assertEqual(row[:5], [200, 1, 0.0, 0.0, 9.81])
        self.assertEqual(row[-2:], ["", ""])
        self.assertIsNone(parse_bno(sample.replace(b"9.81", b"nan")))
        self.assertIsNone(parse_bno(sample[:-3] + b"10"))
        self.assertIsNone(parse_bno(b"BNO,200,1,1,0,0,0,3,nan,11,22"))

    def test_download_contains_set_and_trk_references(self):
        with TemporaryDirectory() as directory:
            link = SerialLink()
            link.bno = BnoCapture(Path(directory))
            try:
                link.connect("sim")
                with patch.object(sattrack, "rotor", link):
                    client = sattrack.app.test_client()
                    started = client.post("/api/bno/capture/start")
                    self.assertEqual(started.status_code, 200)
                    self.wait_samples(link, 1)
                    self.assertEqual(client.post("/api/serial/send", json={"text": "SET,123.25,18.5"}).status_code, 200)
                    self.wait_samples(link, 3)
                    link.send_track(11.125, 31.75, 1.2, -0.25)
                    self.wait_samples(link, 5)
                    self.assertEqual(client.post("/api/serial/send", json={"text": "STOP"}).status_code, 200)
                    self.wait_samples(link, 7)
                    stopped = client.post("/api/bno/capture/stop").get_json()
                    response = client.get("/api/bno/captures/" + stopped["filename"])
                    try:
                        self.assertEqual(response.status_code, 200)
                        reader = csv.DictReader(io.StringIO(response.get_data(as_text=True)))
                        self.assertEqual(tuple(reader.fieldnames), CSV_HEADER)
                        rows = list(reader)
                    finally:
                        response.close()
                targets = [(row["az_objetivo_deg"], row["el_objetivo_deg"]) for row in rows]
                self.assertEqual(targets[0], ("", ""))
                self.assertIn(("123.25", "18.5"), targets)
                self.assertIn(("11.125", "31.75"), targets)
                self.assertEqual(targets[-1], ("", ""))
                self.assertTrue(all(row["acel_z_m_s2"] and row["giro_z_rad_s"] and row["mag_z_ut"] for row in rows))
            finally:
                link.disconnect()

    def wait_samples(self, link, count):
        deadline = time.monotonic() + 3
        while link.bno.state()["samples"] < count and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertGreaterEqual(link.bno.state()["samples"], count)


if __name__ == "__main__":
    unittest.main()
