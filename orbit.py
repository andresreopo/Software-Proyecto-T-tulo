"""Geometría SGP4 para el selector y la simulación del seguimiento."""

from __future__ import annotations

import datetime as dt
import math
from zoneinfo import ZoneInfo

import numpy as np
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


def _in_hour_window(rise, setting, start, end, hours):
    if hours is None:
        return True
    first = max(rise, start).astimezone(CHILE)
    last = min(setting, end).astimezone(CHILE)
    midnight = first.replace(hour=0, minute=0, second=0, microsecond=0)
    while midnight.date() <= last.date():
        opening = midnight + dt.timedelta(minutes=hours[0])
        closing = midnight + dt.timedelta(minutes=hours[1])
        if max(first, opening) < min(last, closing):
            return True
        midnight += dt.timedelta(days=1)
    return False


def _within_azimuth(topo, rise, setting, azimuth):
    if azimuth is None:
        return True
    az_from, az_to = azimuth
    span = az_to - az_from if az_to >= az_from else az_to + 360 - az_from
    if span == 360:
        return True
    count = min(4096, max(2, math.ceil((setting - rise) * 86400) + 1))
    times = rise + np.linspace(0.0, setting - rise, count)
    # Igual que en EarthSatellite.find_events: las rotaciones de precesión y
    # nutación se cancelan al calcular alt/az de un satélite SGP4 desde la Tierra.
    times.gast = np.zeros(count)
    times.M = times.MT = np.eye(3)
    offsets = (topo.at(times).altaz()[1].degrees - az_from) % 360.0
    # En el arco permitido los desplazamientos son continuos. Un salto cerca
    # del extremo inicial del arco delata un cruce entre dos muestras.
    return bool(np.all(offsets <= span) and np.all(np.abs(np.diff(offsets)) <= 180.0))


def find_passes(record, observer, start=None, days=5, limit=12, azimuth=None, hours=None):
    start = start or dt.datetime.now(UTC)
    end = start + dt.timedelta(days=days)
    sat = spacecraft(record)
    obs = station(observer)
    topo = sat - obs
    threshold = observer["min_el"]
    times, events = sat.find_events(
        obs,
        TS.from_datetime(start - dt.timedelta(hours=2)),
        TS.from_datetime(end + dt.timedelta(hours=2)),
        altitude_degrees=threshold,
    )
    pending = None
    passes = []
    found_pass = False
    for moment, event in zip(times, events):
        if event == 0:
            pending = {"aos": moment, "peak": None, "max_el": -90.0}
        elif event == 1 and pending is not None:
            el, _, _ = topo.at(moment).altaz()
            if el.degrees > pending["max_el"]:
                pending["peak"] = moment
                pending["max_el"] = float(el.degrees)
        elif event == 2 and pending is not None:
            rise, peak = pending["aos"], pending["peak"]
            peak_el = pending["max_el"]
            pending = None
            rise_at, set_at = _utc(rise), _utc(moment)
            if peak is None or set_at <= start or rise_at >= end:
                continue
            found_pass = True
            if (not _in_hour_window(rise_at, set_at, start, end, hours)
                    or not _within_azimuth(topo, rise, moment, azimuth)):
                continue
            _, rise_az, _ = topo.at(rise).altaz()
            _, set_az, _ = topo.at(moment).altaz()
            passes.append({
                "aos_utc": rise_at.isoformat(),
                "peak_utc": _utc(peak).isoformat(),
                "los_utc": set_at.isoformat(),
                "aos_chile": _local(rise),
                "peak_chile": _local(peak),
                "los_chile": _local(moment),
                "max_el_deg": round(peak_el, 1),
                "aos_az_deg": round(float(rise_az.degrees), 1),
                "los_az_deg": round(float(set_az.degrees), 1),
                "in_progress": rise_at < start,
            })
            if limit and len(passes) >= limit:
                break
    if not found_pass:
        instant = TS.from_datetime(start)
        current_el = topo.at(instant).altaz()[0].degrees
        if (current_el >= threshold and _in_hour_window(start, end, start, end, hours)
                and _within_azimuth(topo, instant, TS.from_datetime(end), azimuth)):
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


def az_el_rates(record, observer, when, h=0.5):
    """(az °, el °, vaz °/s, vel °/s) por segundo de tiempo simulado, con diferencia centrada ±h s."""
    sat = spacecraft(record)
    topo = sat - station(observer)
    times = TS.from_datetimes([when - dt.timedelta(seconds=h), when, when + dt.timedelta(seconds=h)])
    el, az, _ = topo.at(times).altaz()
    a, e = az.degrees, el.degrees
    vaz = (((a[2] - a[0] + 540.0) % 360.0) - 180.0) / (2 * h)       # sin salto en 0°/360°
    vel = (e[2] - e[0]) / (2 * h)
    return float(a[1]) % 360.0, float(e[1]), float(vaz), float(vel)


def pass_rate_peaks(record, observer, aos, los):
    """Velocidades angulares máximas (°/s de tiempo real de la órbita) durante un pase, muestreadas a 1 s."""
    n = max(3, int((los - aos).total_seconds()) + 1)
    n = min(n, 3600)
    times = TS.from_datetimes([aos + dt.timedelta(seconds=i) for i in range(n)])
    el, az, _ = (spacecraft(record) - station(observer)).at(times).altaz()
    azu = np.degrees(np.unwrap(np.radians(az.degrees)))
    return float(np.abs(np.gradient(azu)).max()), float(np.abs(np.gradient(el.degrees)).max())
