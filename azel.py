"""Azimut y elevación de un objetivo visto desde un observador, a partir de lat/lon/altura.

Uso como módulo:
    from azel import az_el
    az, el, rango_km = az_el(obs=(-33.45, -70.66, 520), tgt=(-30.0, -72.0, 550_000))

Uso por consola (lat, lon en grados; altura en metros sobre el elipsoide WGS84):
    python azel.py -33.45 -70.66 520  -30.0 -72.0 550000
"""
from __future__ import annotations

import math
import sys

# Elipsoide WGS84
_A = 6378137.0
_F = 1 / 298.257223563
_E2 = _F * (2 - _F)


def _ecef(lat_deg: float, lon_deg: float, h_m: float) -> tuple[float, float, float]:
    lat, lon = math.radians(lat_deg), math.radians(lon_deg)
    n = _A / math.sqrt(1 - _E2 * math.sin(lat) ** 2)
    return ((n + h_m) * math.cos(lat) * math.cos(lon),
            (n + h_m) * math.cos(lat) * math.sin(lon),
            (n * (1 - _E2) + h_m) * math.sin(lat))


def az_el(obs: tuple[float, float, float],
          tgt: tuple[float, float, float]) -> tuple[float, float, float]:
    """obs/tgt = (lat°, lon°, altura m). Devuelve (azimut° desde el norte 0..360,
    elevación° sobre el horizonte, distancia km). La elevación es negativa si el
    objetivo está bajo el horizonte."""
    ox, oy, oz = _ecef(*obs)
    tx, ty, tz = _ecef(*tgt)
    dx, dy, dz = tx - ox, ty - oy, tz - oz
    lat, lon = math.radians(obs[0]), math.radians(obs[1])
    # Vector observador->objetivo en coordenadas locales Este-Norte-Arriba
    e = -math.sin(lon) * dx + math.cos(lon) * dy
    n = (-math.sin(lat) * math.cos(lon) * dx - math.sin(lat) * math.sin(lon) * dy
         + math.cos(lat) * dz)
    u = (math.cos(lat) * math.cos(lon) * dx + math.cos(lat) * math.sin(lon) * dy
         + math.sin(lat) * dz)
    rng = math.sqrt(e * e + n * n + u * u)
    az = math.degrees(math.atan2(e, n)) % 360.0
    el = math.degrees(math.asin(u / rng)) if rng else 90.0
    return az, el, rng / 1000.0


if __name__ == "__main__":
    if len(sys.argv) != 7:
        sys.exit(__doc__)
    v = list(map(float, sys.argv[1:]))
    az, el, rng = az_el(tuple(v[:3]), tuple(v[3:]))
    print(f"Azimut {az:.2f}°  Elevación {el:.2f}°  Distancia {rng:.1f} km")
