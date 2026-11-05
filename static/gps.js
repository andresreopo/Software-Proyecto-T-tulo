/* GPS → posición de la estación de observación.
 * Modo "Automática" (por defecto): consulta /api/gps y, cuando hay fix, copia lat/lon/alt a los campos
 * de la estación (solo lectura) y refresca el catálogo, igual que «Actualizar posición».
 * Sin fix se mantienen los valores de los campos, que siguen editables como respaldo.
 * Modo "Manual": los campos son libres y el GPS no los toca.
 * Depende de $, api() y selected/selectSatellite/loadCatalog de app.js. */
(() => {
"use strict";
const MODE_KEY = "sattrack.obsmode";
const POLL_MS = 3000;
const FIELDS = ["lat", "lon", "alt_m"];
const sel = $("obs-mode"), pill = $("gps-pill"), hint = $("gps-hint");

let mode = "auto";
try { if (localStorage.getItem(MODE_KEY) === "manual") mode = "manual"; } catch (_) {}
sel.value = mode;

let lastSeq = null, locked = false;

function setLocked(lock) {
  locked = lock;
  FIELDS.forEach(id => { const el = $(id); el.readOnly = lock; el.classList.toggle("auto-fed", lock); });
}

function show(text, cls, title, hintText) {
  pill.textContent = text;
  pill.className = "pill " + cls;
  pill.title = title || "";
  hint.hidden = !hintText;
  hint.textContent = hintText || "";
}

function render(g) {
  const auto = mode === "auto";
  if (!g || !g.enabled) {
    show("SIN GPS", "muted", g && g.error ? g.error : "El servidor no tiene GPS configurado",
         auto ? "Sin GPS: se usan los campos." : "");
  } else if (g.has_fix) {
    show(`FIX · ${g.sats} SAT`, "", `HDOP ${g.hdop ?? "—"} · ${g.port}${g.sim ? " (simulado)" : ""}`);
  } else if (!g.connected) {
    show("SIN DATOS", "muted", g.error || `El módulo no envía datos por ${g.port}`,
         auto ? "GPS sin datos: se usan los campos." : "");
  } else {
    const n = g.sats ?? 0;
    show(`BUSCANDO · ${n} SAT`, "warn", `Se necesitan ≥ ${g.min_sats} satélites y HDOP ≤ ${g.max_hdop}`,
         auto ? "Sin fix aún: se usan los campos." : "");
  }
}

function applyFix(g) {
  $("lat").value = g.lat.toFixed(5);
  $("lon").value = g.lon.toFixed(5);
  if (g.alt_m !== null) $("alt_m").value = Math.round(g.alt_m);
  $("apply-observer").click();                       // recarga catálogo y satélite seleccionado
  log(`Posición tomada del GPS: ${g.lat.toFixed(5)}°, ${g.lon.toFixed(5)}°.`);
}

async function poll() {
  let g = null;
  try { g = await api("/api/gps"); } catch (_) {}
  render(g);
  const auto = mode === "auto";
  setLocked(auto && !!g && g.has_fix);
  if (auto && g && g.has_fix && g.seq !== lastSeq) { lastSeq = g.seq; applyFix(g); }
}

sel.addEventListener("change", () => {
  mode = sel.value;
  try { localStorage.setItem(MODE_KEY, mode); } catch (_) {}
  lastSeq = null;                                    // al volver a Automática se reaplica el fix actual
  poll();
});

setLocked(false);
poll();
setInterval(poll, POLL_MS);
})();
