"""Mapa SatTrack: fondo cartográfico estático y capa PNG transparente dinámica."""
from __future__ import annotations
import io
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
try:
    import cartopy.crs as ccrs
    import cartopy.feature as cfeature
    HAS_CARTOPY = True
except ImportError:
    HAS_CARTOPY = False

BG, LAND, GRID = "#091b2b", "#1e293b", "#334155"
TXT, DIM = "#e2e8f0", "#94a3b8"
RED, GREEN, BLUE, AMBER = "#ffc16c", "#a6e65e", "#57c9ed", "#f59e0b"

# Extensión fija relativa al observador: no cambia cuando se mueve el satélite.
def map_extent(observer):
    lon, lat = float(observer["lon"]), float(observer["lat"])
    return (lon - 60, lon + 60, max(-90, lat - 35), min(90, lat + 35))

AX_RECT = [.065, .09, .91, .81]   # posición de los ejes dentro de la figura (fracción)

def observer_fraction(observer):
    """Posición del observador en la imagen del mapa como fracción (x desde la izquierda, y desde arriba).
    Los PNG se estiran sobre todo el recuadro, así que la fracción vale para cualquier tamaño en pantalla."""
    west, east, south, north = map_extent(observer)
    fx = (float(observer["lon"]) - west) / (east - west)
    fy = (float(observer["lat"]) - south) / (north - south)
    x0, y0, w, h = AX_RECT
    return {"x": round(x0 + w * fx, 5), "y": round(1 - (y0 + h * fy), 5)}

def _figure(width, height, transparent=False):
    dpi = 100
    fig = plt.figure(figsize=(width/dpi, height/dpi), dpi=dpi,
                     facecolor=("none" if transparent else BG))
    ax_rect = AX_RECT
    if HAS_CARTOPY:
        proj = ccrs.PlateCarree()
        ax = fig.add_axes(ax_rect, projection=proj)
        ax.set_aspect("auto")
        transform = {"transform": proj}
    else:
        ax = fig.add_axes(ax_rect)
        transform = {}
    ax.set_aspect("auto")
    return fig, ax, transform

def _limits(ax, observer):
    west, east, south, north = map_extent(observer)
    if HAS_CARTOPY:
        ax.set_extent((west, east, south, north), crs=ccrs.PlateCarree())
    else:
        ax.set_xlim(west, east); ax.set_ylim(south, north)
    return west, east, south, north

def _png(fig):
    buf = io.BytesIO()
    fig.savefig(buf, format="png", dpi=100, transparent=fig.get_facecolor() == (0.0, 0.0, 0.0, 0.0),
                facecolor=fig.get_facecolor(), edgecolor="none")
    plt.close(fig)
    return buf.getvalue()

def render_map_base_png(observer, width=1200, height=600):
    """Renderiza una vez la cartografía, cuadrícula y etiquetas de coordenadas."""
    width=max(300,min(int(width),1600)); height=max(200,min(int(height),1000))
    fig, ax, kw = _figure(width, height)
    _limits(ax, observer)
    if HAS_CARTOPY:
        ax.add_feature(cfeature.OCEAN, facecolor=BG, zorder=0)
        ax.add_feature(cfeature.LAND, facecolor=LAND, zorder=1)
        ax.add_feature(cfeature.COASTLINE, linewidth=.7, edgecolor="#64748b", zorder=2)
        ax.add_feature(cfeature.BORDERS, linestyle=":", linewidth=.45, edgecolor="#475569", zorder=2)
        ax.gridlines(draw_labels=False, crs=ccrs.PlateCarree(), color=GRID, linestyle="--", alpha=.55)
    else:
        ax.set_facecolor(BG); ax.grid(True, color=GRID, linestyle="--", alpha=.55)
    ax.set_facecolor(BG)
    ax.tick_params(colors=DIM, labelsize=8)
    ax.set_xlabel("Longitud", color=DIM, fontsize=8); ax.set_ylabel("Latitud", color=DIM, fontsize=8)
    ax.set_title("MAPA DE SEGUIMIENTO · BASE CARTOGRÁFICA", color=TXT, fontsize=10, pad=8)
    for sp in ax.spines.values(): sp.set_edgecolor(GRID)
    return _png(fig)

def render_map_overlay_png(observer, geometry=None, satellite_name="Satélite", visible=False,
                           width=1200, height=600):
    """Renderiza solo marcadores/etiquetas en PNG transparente; no dibuja costas."""
    width=max(300,min(int(width),1600)); height=max(200,min(int(height),1000))
    fig, ax, kw = _figure(width, height, transparent=True)
    _limits(ax, observer)
    ax.set_facecolor("none"); ax.patch.set_alpha(0)
    for sp in ax.spines.values(): sp.set_visible(False)
    ax.set_xticks([]); ax.set_yticks([])
    ox, oy = float(observer["lon"]), float(observer["lat"])
    if geometry:
        sy = float(geometry["subsat_lat_deg"])
        sx = ox + ((float(geometry["subsat_lon_deg"]) - ox + 180) % 360 - 180)
        ax.plot([ox, sx], [oy, sy], color=BLUE, linewidth=1.4, linestyle="--", alpha=.95, zorder=3, **kw)
        ax.text((ox+sx)/2, (oy+sy)/2, f'{geometry.get("range_km",0):,.0f} km'.replace(",", " "),
                color=BLUE, fontsize=8, ha="center", bbox=dict(boxstyle="round,pad=.2",fc=BG,ec="none",alpha=.9), zorder=4, **kw)
    ax.plot(ox, oy, marker="*", color=RED, markersize=13, markeredgecolor="#7c4a13", linestyle="none", zorder=7, **kw)
    ax.text(ox, oy, f'  Observador\n  {oy:.3f}°, {ox:.3f}°', color=TXT, fontsize=8, fontweight="bold",
            ha="left", va="bottom", bbox=dict(boxstyle="round,pad=.25",fc="#1e293b",ec=RED,alpha=.93), zorder=8, **kw)
    if geometry:
        col = GREEN if visible else AMBER
        ax.plot(sx, sy, marker="o", markersize=8, color=col,
                markerfacecolor=col if visible else "none", markeredgewidth=1.6, linestyle="none", zorder=7, **kw)
        label = f'{satellite_name}\nAz {geometry.get("az_deg",0):.1f}°  El {geometry.get("el_deg",0):.1f}°\n{geometry.get("altitude_km",0):.0f} km altitud'
        if not visible: label += '\n(bajo elevación mínima)'
        west, east, _, _ = map_extent(observer)
        ax.text(sx, sy, label, color=TXT, fontsize=8, fontweight="bold",
                ha="left" if (sx-west)/(east-west)<.65 else "right", va="bottom",
                bbox=dict(boxstyle="round,pad=.25",fc="#1e293b",ec=col,alpha=.93), zorder=8, **kw)
    return _png(fig)

# Compatibilidad con versiones anteriores del proyecto.
def render_map_png(observer, geometry=None, satellite_name="Satélite", visible=False, width=900, height=500):
    return render_map_overlay_png(observer, geometry, satellite_name, visible, width, height)
