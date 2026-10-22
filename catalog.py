"""Catálogo local CelesTrak/SatNOGS con actualización no bloqueante."""

from __future__ import annotations

import datetime as dt
import json
import threading
from pathlib import Path

import requests
from skyfield.api import EarthSatellite, load, wgs84


DATA = Path(__file__).resolve().parent / "data"
CELESTRAK = "https://celestrak.org/NORAD/elements/gp.php"
SATNOGS = "https://db.satnogs.org/api/transmitters/"
HEADERS = {"User-Agent": "SatTrackMaiaSim/1.0 (academic prototype)"}


def _read(name, default):
    try:
        return json.loads((DATA / name).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return default


def _write(name, data):
    DATA.mkdir(parents=True, exist_ok=True)
    path = DATA / name
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
    temporary.replace(path)


def _age_hours(meta):
    try:
        stamp = meta.get("fetched_at") or meta.get("saved_at")
        moment = dt.datetime.fromisoformat(stamp.replace("Z", "+00:00"))
        return (dt.datetime.now(dt.timezone.utc) - moment).total_seconds() / 3600
    except (AttributeError, TypeError, ValueError):
        return None


class CatalogStore:
    def __init__(self):
        self.lock = threading.RLock()
        self.records = _read("active.json", [])
        self.orbit_meta = _read("active.meta.json", {})
        self.transmitters = _read("transmitters.json", [])
        self.tx_meta = _read("transmitters.meta.json", {})
        self.by_id = {}
        self.tx_by_id = {}
        self.updating = False
        self.last_error = None
        self._reindex()

    def _reindex(self):
        self.by_id = {
            int(r["NORAD_CAT_ID"]): r for r in self.records
            if isinstance(r, dict) and r.get("NORAD_CAT_ID") and r.get("EPOCH")
        }
        grouped = {}
        for tx in self.transmitters:
            try:
                norad = int(tx.get("norad_cat_id") or 0)
                frequency = tx.get("downlink_low") or tx.get("downlink_high")
                if tx.get("status") != "active" or not norad or frequency is None:
                    continue
                grouped.setdefault(norad, []).append(tx)
            except (TypeError, ValueError, AttributeError):
                continue
        self.tx_by_id = grouped

    def status(self):
        with self.lock:
            orbital_age = _age_hours(self.orbit_meta)
            tx_age = _age_hours(self.tx_meta)
            return {
                "catalog_count": len(self.by_id),
                "transmitter_count": len(self.transmitters),
                "celestrak": {**self.orbit_meta, "age_hours": round(orbital_age, 1) if orbital_age is not None else None,
                              "stale": orbital_age is None or orbital_age > 24},
                "satnogs": {**self.tx_meta, "age_hours": round(tx_age, 1) if tx_age is not None else None,
                            "stale": tx_age is None or tx_age > 24},
                "updating": self.updating,
                "last_error": self.last_error,
            }

    def record(self, norad):
        with self.lock:
            return self.by_id.get(int(norad))

    def transmitters_for(self, norad, band_only=False):
        with self.lock:
            records = list(self.tx_by_id.get(int(norad), []))
        result = []
        for tx in records:
            low = tx.get("downlink_low")
            high = tx.get("downlink_high")
            try:
                freq = float(low if low is not None else high) / 1e6
                upper = float(high if high is not None else low) / 1e6
            except (TypeError, ValueError):
                continue
            if band_only and not (2000 <= freq <= 4000):
                continue
            result.append({
                "uuid": tx.get("uuid"),
                "frequency_mhz": round(freq, 6),
                "high_mhz": round(upper, 6),
                "mode": tx.get("mode") or "Desconocido",
                "description": tx.get("description") or "Bajada catalogada",
                "service": tx.get("service"),
                "unconfirmed": bool(tx.get("unconfirmed")),
                "updated": tx.get("updated"),
            })
        return sorted(result, key=lambda item: item["frequency_mhz"])

    def search(self, query, band, visible_only, observer, page, page_size):
        query = str(query or "").strip().casefold()[:80]
        with self.lock:
            records = list(self.by_id.values())
            tx_ids = set(self.tx_by_id)
        if band == "s":
            records = [r for r in records if int(r["NORAD_CAT_ID"]) in tx_ids
                       and self.transmitters_for(int(r["NORAD_CAT_ID"]), band_only=True)]
        if query:
            records = [r for r in records if query in str(r.get("OBJECT_NAME", "")).casefold()
                       or query in str(r["NORAD_CAT_ID"])]
        ts = load.timescale()
        moment = ts.now()
        station = wgs84.latlon(observer["lat"], observer["lon"], elevation_m=observer["alt_m"])
        items = []
        for record in records:
            try:
                sat = EarthSatellite.from_omm(ts, record)
                elevation, azimuth, _ = (sat - station).at(moment).altaz()
                el = float(elevation.degrees)
                if visible_only and el < observer["min_el"]:
                    continue
                tx = self.transmitters_for(int(record["NORAD_CAT_ID"]), band_only=True)
                items.append({
                    "norad_id": int(record["NORAD_CAT_ID"]),
                    "name": record.get("OBJECT_NAME") or f"NORAD {record['NORAD_CAT_ID']}",
                    "elevation_deg": round(el, 1),
                    "azimuth_deg": round(float(azimuth.degrees), 1),
                    "visible_now": el >= observer["min_el"],
                    "frequencies_mhz": sorted({x["frequency_mhz"] for x in tx}),
                    "epoch_utc": record.get("EPOCH"),
                })
            except (ValueError, TypeError, KeyError, ArithmeticError):
                continue
        items.sort(key=lambda x: (-x["elevation_deg"], x["name"]) if visible_only
                   else (x["name"].casefold(), x["norad_id"]))
        begin = (page - 1) * page_size
        return {"items": items[begin:begin + page_size], "total": len(items),
                "page": page, "page_size": page_size, "computed_at": moment.utc_datetime().isoformat()}

    def refresh_async(self):
        with self.lock:
            if self.updating:
                return False
            self.updating = True
            self.last_error = None
        threading.Thread(target=self._refresh, daemon=True, name="catalog-refresh").start()
        return True

    def _refresh(self):
        errors = []
        try:
            response = requests.get(CELESTRAK, params={"GROUP": "ACTIVE", "FORMAT": "JSON"},
                                    headers=HEADERS, timeout=(6, 40))
            response.raise_for_status()
            records = response.json()
            if not isinstance(records, list) or len(records) < 1000:
                raise ValueError("Respuesta orbital incompleta")
            valid = [r for r in records if isinstance(r, dict) and r.get("NORAD_CAT_ID")
                     and r.get("EPOCH") and r.get("MEAN_MOTION")]
            meta = {"fetched_at": dt.datetime.now(dt.timezone.utc).isoformat(),
                    "source": CELESTRAK + "?GROUP=ACTIVE&FORMAT=JSON", "count": len(valid)}
            _write("active.json", valid)
            _write("active.meta.json", meta)
            with self.lock:
                self.records = valid
                self.orbit_meta = meta
                self._reindex()
        except (requests.RequestException, ValueError, OSError) as exc:
            errors.append(f"CelesTrak: {exc}")
        try:
            response = requests.get(SATNOGS, params={"status": "active"},
                                    headers=HEADERS, timeout=(6, 35))
            response.raise_for_status()
            records = response.json()
            if not isinstance(records, list) or not records:
                raise ValueError("Respuesta de transmisores vacía")
            meta = {"fetched_at": dt.datetime.now(dt.timezone.utc).isoformat(),
                    "source": SATNOGS, "count": len(records)}
            _write("transmitters.json", records)
            _write("transmitters.meta.json", meta)
            with self.lock:
                self.transmitters = records
                self.tx_meta = meta
                self._reindex()
        except (requests.RequestException, ValueError, OSError) as exc:
            errors.append(f"SatNOGS: {exc}")
        with self.lock:
            self.updating = False
            self.last_error = "; ".join(errors) if errors else None


store = CatalogStore()
