"""Geometría SGP4 para el selector y la simulación del seguimiento."""

from __future__ import annotations

import datetime as dt
import math
from zoneinfo import ZoneInfo

from skyfield.api import EarthSatellite, load, wgs84


UTC = dt.timezone.utc
CHILE = ZoneInfo("America/Santiago")
TS = load.timescale()


def station(observer):
    return wgs84.latlon(observer["lat"], observer["lon"], elevation_m=observer["alt_m"])


def spacecraft(record):
    return EarthSatellite.from_omm(TS, record)


def _utc(moment):
    return moment.utc_datetime().astimezone(UTC)


def _local(moment):
    return _utc(moment).astimezone(CHILE).isoformat(timespec="seconds")


def position(record, observer, when):
    sat = spacecraft(record)
    instant = TS.from_datetime(when)
    topocentric = (sat - station(observer)).at(instant)
    elevation, azimuth, distance = topocentric.altaz()
    subpoint = wgs84.subpoint(sat.at(instant))
    result = {
        "az_deg": round(float(azimuth.degrees), 2),
        "el_deg": round(float(elevation.degrees), 2),
        "range_km": round(float(distance.km), 1),
        "subsat_lat_deg": round(float(subpoint.latitude.degrees), 4),
        "subsat_lon_deg": round(float(subpoint.longitude.degrees), 4),
        "altitude_km": round(float(subpoint.elevation.km), 1),
    }
    if not all(math.isfinite(v) for v in result.values()):
        raise ValueError("Geometría orbital no finita")
    return result


def range_rate_km_s(record, observer, when):
    sat = spacecraft(record)
    obs = station(observer)
    before = TS.from_datetime(when - dt.timedelta(seconds=1))
    after = TS.from_datetime(when + dt.timedelta(seconds=1))
    distance_before = (sat - obs).at(before).distance().km
    distance_after = (sat - obs).at(after).distance().km
    return float(distance_after - distance_before) / 2


def find_passes(record, observer, start=None, days=5, limit=12):
    start = start or dt.datetime.now(UTC)
    end = start + dt.timedelta(days=days)
    sat = spacecraft(record)
    obs = station(observer)
    threshold = observer["min_el"]
    times, events = sat.find_events(
        obs,
        TS.from_datetime(start - dt.timedelta(hours=2)),
        TS.from_datetime(end + dt.timedelta(hours=2)),
        altitude_degrees=threshold,
    )
    pending = None
    passes = []
    for moment, event in zip(times, events):
        if event == 0:
            pending = {"aos": moment, "peak": None, "max_el": -90.0}
        elif event == 1 and pending is not None:
            el, _, _ = (sat - obs).at(moment).altaz()
            if el.degrees > pending["max_el"]:
                pending["peak"] = moment
                pending["max_el"] = float(el.degrees)
        elif event == 2 and pending is not None:
            rise, peak = pending["aos"], pending["peak"]
            peak_el = pending["max_el"]
            pending = None
            if peak is None or _utc(moment) <= start or _utc(rise) >= end:
                continue
            _, rise_az, _ = (sat - obs).at(rise).altaz()
            _, set_az, _ = (sat - obs).at(moment).altaz()
            passes.append({
                "aos_utc": _utc(rise).isoformat(),
                "peak_utc": _utc(peak).isoformat(),
                "los_utc": _utc(moment).isoformat(),
                "aos_chile": _local(rise),
                "peak_chile": _local(peak),
                "los_chile": _local(moment),
                "max_el_deg": round(peak_el, 1),
                "aos_az_deg": round(float(rise_az.degrees), 1),
                "los_az_deg": round(float(set_az.degrees), 1),
                "in_progress": _utc(rise) < start,
            })
            if limit and len(passes) >= limit:
                break
    if not passes:
        current_el = (sat - obs).at(TS.from_datetime(start)).altaz()[0].degrees
        if current_el >= threshold:
            passes.append({
                "aos_utc": start.isoformat(), "peak_utc": start.isoformat(), "los_utc": end.isoformat(),
                "aos_chile": start.astimezone(CHILE).isoformat(timespec="seconds"),
                "peak_chile": start.astimezone(CHILE).isoformat(timespec="seconds"),
                "los_chile": end.astimezone(CHILE).isoformat(timespec="seconds"),
                "max_el_deg": round(float(current_el), 1),
                "aos_az_deg": None, "los_az_deg": None,
                "in_progress": True, "continuous": True,
            })
    return passes
