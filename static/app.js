"use strict";

const $ = id => document.getElementById(id);
const fmt = (n, digits = 1) => Number.isFinite(Number(n)) ? Number(n).toLocaleString("es-CL", {minimumFractionDigits:digits, maximumFractionDigits:digits}) : "—";
const localTime = value => value ? new Date(value).toLocaleString("es-CL", {timeZone:"America/Santiago", day:"2-digit", month:"2-digit", hour:"2-digit", minute:"2-digit", hour12:false}) : "—";
const longTime = value => value ? new Date(value).toLocaleString("es-CL", {timeZone:"America/Santiago", day:"2-digit", month:"2-digit", year:"numeric", hour:"2-digit", minute:"2-digit", second:"2-digit", hour12:false}) : "—";
let selected = null, sessionId = null, latest = null, page = 1, pages = 1;
let querySequence = 0, detailSequence = 0, polling = false, lastPhase = null, lastLock = null;
let trail = [], passPath = null, pathSession = null, stateAt = 0;

function toast(message) {
  const box = $("toast"); box.textContent = message; box.hidden = false;
  clearTimeout(toast.timer); toast.timer = setTimeout(() => box.hidden = true, 6000);
}
function log(message) {
  const box = $("event-log");
  if (!box) { console.log("[SATTRACK]", message); return; }
  const row = document.createElement("div"), time = document.createElement("time");
  if (box.children.length === 1 && box.firstElementChild.textContent === "Selecciona un satélite para comenzar.") box.replaceChildren();
  time.textContent = new Date().toLocaleTimeString("es-CL", {timeZone:"America/Santiago", hour12:false}) + "  ";
  row.append(time, document.createTextNode(message)); box.prepend(row);
  while (box.children.length > 16) box.lastChild.remove();
}
async function api(path, options = {}) {
  const response = await fetch(path, {cache:"no-store", ...options});
  let data;
  try { data = await response.json(); } catch { throw Error("El servidor devolvió una respuesta no válida"); }
  if (!response.ok || !data.ok) throw Error(data.error || `HTTP ${response.status}`);
  return data;
}
function observer() {
  return {lat:Number($("lat").value), lon:Number($("lon").value), alt_m:Number($("alt_m").value), min_el:Number($("min_el").value)};
}
function queryForObserver() { return new URLSearchParams(observer()).toString(); }

function showSource(catalog) {
  if (!catalog) return;
  const c = catalog.celestrak, s = catalog.satnogs;
  const stale = c.stale || s.stale;
  const indicator = $("source-indicator");
  indicator.textContent = catalog.updating ? "Actualizando fuentes…" : stale ? "FUENTES CON CACHÉ ANTIGUO" : "FUENTES ACTUALIZADAS";
  indicator.classList.toggle("warn", stale);
  $("source-note").textContent = `CelesTrak: ${c.fetched_at ? localTime(c.fetched_at) : "sin fecha"} · SatNOGS: ${s.fetched_at || s.saved_at ? localTime(s.fetched_at || s.saved_at) : "sin fecha"}. Las frecuencias catalogadas no garantizan emisión durante un pase.`;
  $("source-note").classList.toggle("warn", stale);
  if (catalog.last_error) $("source-note").textContent += ` Última actualización: ${catalog.last_error}`;
}
async function loadStatus() {
  try {
    const data = await api("/api/status");
    showSource(data.catalog);
    window.RF_SERVER_URL = (data.rf || {}).server_url || null;   // lo usa rf.js para el aviso de conexión
  }
  catch (error) { toast(error.message); }
}
function satButton(item) {
  const button = document.createElement("button");
  button.className = "sat-item" + (selected && selected.norad_id === item.norad_id ? " active" : "");
  const left = document.createElement("span"), name = document.createElement("strong"), sub = document.createElement("small");
  name.textContent = item.name;
  sub.textContent = `#${item.norad_id} · ${item.frequencies_mhz.length ? item.frequencies_mhz.slice(0,2).join(" / ") + " MHz" : "sin bajada S"}`;
  left.append(name, sub);
  const right = document.createElement("span"), el = document.createElement("span"), hint = document.createElement("small");
  right.className = "elevation" + (item.visible_now ? " visible" : "");
  el.textContent = fmt(item.elevation_deg, 1) + "°"; hint.textContent = item.visible_now ? "visible" : "bajo mínimo";
  right.append(el, hint); button.append(left, right);
  button.addEventListener("click", () => selectSatellite(item.norad_id));
  return button;
}
async function loadCatalog(resetPage = false) {
  if (resetPage) page = 1;
  const sequence = ++querySequence;
  $("catalog-summary").textContent = "Calculando catálogo…";
  const params = new URLSearchParams({...observer(), q:$("search").value.trim(), band:$("sband-only").checked ? "s" : "all", visible:$("visible-only").checked ? "1" : "0", page:String(page), page_size:"40"});
  try {
    const data = await api("/api/catalog?" + params);
    if (sequence !== querySequence) return;
    pages = Math.max(1, Math.ceil(data.total / data.page_size));
    $("catalog-summary").textContent = `${data.total.toLocaleString("es-CL")} objetos · ${data.catalog.catalog_count.toLocaleString("es-CL")} órbitas`;
    $("page-label").textContent = `${page} / ${pages}`;
    $("prev-page").disabled = page <= 1;
    $("next-page").disabled = page >= pages;
    const list = $("satellite-list"); list.replaceChildren(...data.items.map(satButton));
    if (!data.items.length) { const note = document.createElement("div"); note.className = "empty-note"; note.textContent = "Sin satélites para estos filtros."; list.append(note); }
    showSource(data.catalog);
  } catch (error) {
    if (sequence !== querySequence) return;
    $("catalog-summary").textContent = "No se pudo cargar el catálogo";
    toast(error.message);
  }
}

function renderPasses(passes) {
  const root = $("passes"); root.replaceChildren();
  if (!passes.length) { const note = document.createElement("div"); note.className = "empty-note"; note.textContent = "No hay pases sobre la elevación mínima en los próximos cinco días."; root.append(note); return; }
  for (const pass of passes.slice(0, 4)) {
    const card = document.createElement("div"); card.className = "pass-item";
    const title = document.createElement("strong"), when = document.createElement("small"), elevation = document.createElement("div");
    title.textContent = localTime(pass.aos_chile);
    when.textContent = `${localTime(pass.aos_chile).split(", ").pop()} → ${localTime(pass.los_chile).split(", ").pop()}${pass.in_progress ? " · en curso" : ""}`;
    elevation.className = "pass-el"; elevation.textContent = `▲ ${fmt(pass.max_el_deg, 1)}°`;
    card.append(title, when, elevation); root.append(card);
  }
}
function renderSelected(sat) {
  selected = sat;
  $("selected-name").textContent = sat.name || `NORAD ${sat.norad_id}`;
  $("selected-meta").textContent = `${sat.object_id || "Objeto en órbita"} · época orbital ${sat.epoch_utc ? localTime(sat.epoch_utc) : "desconocida"}`;
  const visible = sat.current.el_deg >= observer().min_el;
  const pill = $("selected-visibility"); pill.textContent = visible ? "VISIBLE AHORA" : "BAJO ELEVACIÓN MÍNIMA"; pill.className = "pill " + (visible ? "" : "muted");
  $("selected-id").textContent = sat.norad_id;
  $("selected-el").textContent = fmt(sat.current.el_deg, 1) + "°";
  $("selected-az").textContent = fmt(sat.current.az_deg, 1) + "°";
  $("selected-period").textContent = fmt(sat.orbit.period_min, 1) + " min";
  const tx = sat.transmitters.filter(x => x.frequency_mhz >= 2000 && x.frequency_mhz <= 4000);
  const select = $("transmitter"); select.replaceChildren();
  for (const item of tx) {
    const option = document.createElement("option"); option.value = item.uuid || "";
    option.textContent = `${fmt(item.frequency_mhz, 3)} MHz · ${item.mode}${item.unconfirmed ? " · sin confirmar" : ""}`;
    select.append(option);
  }
  const manual = document.createElement("option"); manual.value = "manual"; manual.textContent = "Frecuencia manual (solo simulación)"; select.append(manual);
  if (!tx.length) select.value = "manual";
  select.disabled = false;
  $("manual-wrap").hidden = select.value !== "manual";
  $("start-session").disabled = sat.passes.length === 0;
  renderPasses(sat.passes);
  document.querySelectorAll(".sat-item").forEach(button => button.classList.toggle("active", button.textContent.includes(`#${sat.norad_id}`)));
  showSource(sat.catalog);
}
async function selectSatellite(norad) {
  const sequence = ++detailSequence;
  $("selected-name").textContent = "Cargando satélite…";
  try {
    const data = await api(`/api/catalog/${norad}?${queryForObserver()}`);
    if (sequence !== detailSequence) return;
    renderSelected(data.satellite);
    log(`${data.satellite.name} (#${norad}) seleccionado del catálogo.`);
  } catch (error) { if (sequence === detailSequence) { $("selected-name").textContent = "Selecciona un satélite"; toast(error.message); } }
}

async function startSession() {
  if (!selected) return;
  const button = $("start-session"); button.disabled = true;
  try {
    const txChoice = $("transmitter").value;
    const body = {norad_id:selected.norad_id, ...observer(), speed:Number($("speed").value), rotor_follow:!!(window.rotorAutoFollow && window.rotorAutoFollow())};
    if (txChoice === "manual") body.manual_frequency_mhz = Number($("manual-frequency").value);
    else body.transmitter_uuid = txChoice;
    if (sessionId) await api(`/api/session/${sessionId}/control`, {method:"POST", headers:{"Content-Type":"application/json"}, body:JSON.stringify({action:"stop"})}).catch(() => {});
    const data = await api("/api/session", {method:"POST", headers:{"Content-Type":"application/json"}, body:JSON.stringify(body)});
    sessionId = data.session_id; trail = []; lastPhase = null; lastLock = null; passPath = null; pathSession = null;
    $("pause-session").disabled = false; $("stop-session").disabled = false;
    if (data.follow_error) toast(data.follow_error);
    showTab("track");   // el seguimiento ocupa la pantalla; la selección queda en la pestaña 1
    renderState(data.state);
    log(`Simulación iniciada para ${selected.name}; próximo pase acelerado ${body.speed}×.`);
  } catch (error) { toast(error.message); }
  finally { button.disabled = !selected || !selected.passes.length; }
}
async function control(action, extra = {}) {
  if (!sessionId) return;
  try {
    const data = await api(`/api/session/${sessionId}/control`, {method:"POST", headers:{"Content-Type":"application/json"}, body:JSON.stringify({action,...extra})});
    if (action === "stop") {
      sessionId = null; latest = null; passPath = null; pathSession = null; $("pause-session").disabled = true; $("stop-session").disabled = true;
      $("tracking-title").textContent = "Sesión detenida"; 
      $("phase").textContent = "DETENIDA"; $("phase").className = "pill muted";
      requestAnimationFrame(drawAll);
      log("Sesión de seguimiento detenida.");
    } else { renderState(data.state); log(action === "speed" ? `Velocidad ajustada a ${extra.speed}×.` : action === "pause" ? "Simulación pausada." : "Simulación reanudada."); }
  } catch (error) { toast(error.message); }
}
async function pollSession() {
  if (!sessionId || polling) return;
  polling = true;
  const polledId = sessionId;
  try { const data = await api(`/api/session/${polledId}`); if (sessionId === polledId) renderState(data.state); }
  catch (error) { if (sessionId === polledId) { toast(error.message); sessionId = null; $("pause-session").disabled = true; $("stop-session").disabled = true; } }
  finally { polling = false; }
}
function renderState(state) {
  if (state.status === "stopped") return;
  latest = state;
  stateAt = performance.now();
  if (sessionId && pathSession !== sessionId) loadPath(sessionId);
  const geo = state.geometry, rotor = state.rotor, rf = state.receiver;
  $("tracking-title").textContent = state.name;
  $("sim-time").textContent = longTime(state.sim_time_chile) + ` · ${state.speed}×`;
  const phase = $("phase"); phase.textContent = state.phase.toUpperCase(); phase.className = "pill " + (state.phase === "en pase" ? "" : state.phase === "finalizado" ? "warn" : "muted");
  $("pass-progress").style.width = `${Math.round(state.progress * 100)}%`;
  $("pause-session").textContent = state.status === "paused" ? "Reanudar" : "Pausar";
  if (lastPhase !== state.phase) { log(`Fase del pase: ${state.phase}.`); lastPhase = state.phase; }
  const previous = trail.at(-1);
  if (!previous || previous.time !== state.sim_time_utc) {
    trail.push({time:state.sim_time_utc, az:geo.az_deg, el:geo.el_deg, lat:geo.subsat_lat_deg, lon:geo.subsat_lon_deg});
    if (trail.length > 120) trail.shift();
  }
  requestAnimationFrame(drawAll);
}

function canvasContext(id) {
  const canvas = $(id), rect = canvas.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
  const width = Math.max(10, Math.round(rect.width)), height = Math.max(10, Math.round(rect.height));
  if (canvas.width !== Math.round(width * dpr) || canvas.height !== Math.round(height * dpr)) { canvas.width = Math.round(width * dpr); canvas.height = Math.round(height * dpr); }
  const ctx = canvas.getContext("2d"); ctx.setTransform(dpr, 0, 0, dpr, 0, 0); ctx.clearRect(0, 0, width, height);
  return {ctx, width, height};
}
function skyPoint(az, el, cx, cy, radius) {
  const r = radius * (1 - Math.max(0, Math.min(90, el)) / 90), radians = az * Math.PI / 180;
  return [cx + r * Math.sin(radians), cy - r * Math.cos(radians)];
}
function dot(ctx, x, y, radius, color) { ctx.beginPath(); ctx.arc(x, y, radius, 0, Math.PI * 2); ctx.fillStyle = color; ctx.fill(); }
async function loadPath(id) {
  pathSession = id; passPath = null;
  try { const d = await api(`/api/session/${encodeURIComponent(id)}/path`); if (sessionId === id) { passPath = d.path; requestAnimationFrame(drawAll); } }
  catch (_) { if (sessionId === id) pathSession = null; }
}
// Hora simulada interpolada entre consultas al servidor (avanza a `speed`× salvo en pausa).
const utcMs = v => Date.parse(/(Z|[+-]\d\d:?\d\d)$/.test(v) ? v : v + "Z");
function simNow() {
  if (!latest) return null;
  const base = utcMs(latest.sim_time_utc);
  return base + (latest.status === "running" ? (performance.now() - stateAt) * latest.speed : 0);
}
const clock = s => { s = Math.max(0, Math.round(s)); const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60), x = s % 60, p = n => String(n).padStart(2, "0"); return (h ? h + ":" : "") + p(m) + ":" + p(x); };
function drawSky() {
  const {ctx,width:w,height:h} = canvasContext("sky-canvas"), mono = "Consolas, 'JetBrains Mono', monospace";
  const margin = 17, radius = Math.max(20, Math.min(w, h) / 2 - margin), cx = w - radius - margin - 2, cy = h / 2;
  // Anillos de elevación (0°, 30°, 60°) y ejes de azimut cada 45°
  ctx.strokeStyle = "#9fc0d5"; ctx.lineWidth = 1;
  for (const k of [1, 2/3, 1/3]) { ctx.beginPath(); ctx.arc(cx, cy, radius * k, 0, Math.PI * 2); ctx.stroke(); }
  ctx.beginPath();
  for (let a = 0; a < 180; a += 45) { const r = a * Math.PI / 180; ctx.moveTo(cx - radius * Math.sin(r), cy + radius * Math.cos(r)); ctx.lineTo(cx + radius * Math.sin(r), cy - radius * Math.cos(r)); }
  ctx.stroke();
  ctx.fillStyle = "#799bb0"; ctx.font = `16px ${mono}`; ctx.textAlign = "left";
  ctx.fillText("30°", cx + 3, cy - radius * 2/3 + 10); ctx.fillText("60°", cx + 3, cy - radius / 3 + 10);
  ctx.fillStyle = "#2b4e60"; ctx.font = `700 16px ${mono}`; ctx.textAlign = "center"; ctx.textBaseline = "middle";
  ctx.fillText("N", cx, cy - radius - 9); ctx.fillText("S", cx, cy + radius + 9); ctx.fillText("E", cx + radius + 9, cy); ctx.fillText("O", cx - radius - 9, cy);
  ctx.textBaseline = "alphabetic";
  if (!latest) {
    ctx.fillStyle = "#5e7b8f"; ctx.font = `16px ${mono}`; ctx.textAlign = "center"; ctx.fillText("Inicia una sesión", cx, cy + 4);
    return;
  }
  // Trayectoria completa del pase (tenue) y tramo ya recorrido (brillante)
  const line = (pts, color, width, dash) => {
    ctx.strokeStyle = color; ctx.lineWidth = width; ctx.setLineDash(dash || []); ctx.beginPath(); let on = false;
    for (const p of pts) { if (p.el < 0) { on = false; continue; } const [x, y] = skyPoint(p.az, p.el, cx, cy, radius); on ? ctx.lineTo(x, y) : ctx.moveTo(x, y); on = true; }
    ctx.stroke(); ctx.setLineDash([]);
  };
  if (passPath && passPath.length) {
    const pts = passPath.map(([az, el]) => ({az, el}));
    line(pts, "#1284a866", 1.6, [5, 4]);
    for (const [i, name] of [[0, "AOS"], [pts.length - 1, "LOS"]]) {
      const [x, y] = skyPoint(pts[i].az, Math.max(0, pts[i].el), cx, cy, radius);
      dot(ctx, x, y, 3, "#395d70"); ctx.fillStyle = "#395d70"; ctx.font = `600 16px ${mono}`; ctx.textAlign = "center";
      ctx.fillText(name, Math.max(14, Math.min(w - 14, x)), y + (y < cy ? -7 : 14));
    }
  }
  line(trail.map(p => ({az: p.az, el: p.el})), "#1284a8", 2.2);
  const g = latest.geometry, up = g.el_deg >= 0;
  const [rx, ry] = skyPoint(latest.rotor.az_deg, latest.rotor.el_deg, cx, cy, radius);
  dot(ctx, rx, ry, 4, "#935500");
  if (up) { const [sx, sy] = skyPoint(g.az_deg, g.el_deg, cx, cy, radius); dot(ctx, sx, sy, 7, "#61a119"); ctx.strokeStyle = "#f8fafc"; ctx.lineWidth = 1.5; ctx.stroke(); }

  // Lectura en tiempo real (esquina superior izquierda) y cuenta atrás
  ctx.textAlign = "left";
  ctx.fillStyle = "#5e7e90"; ctx.font = `700 16px ${mono}`; ctx.fillText("AZIMUT", 4, 16); ctx.fillText("ELEVACIÓN", 4, 76);
  ctx.fillStyle = "#061319"; ctx.font = `700 28px ${mono}`; ctx.fillText(g.az_deg.toFixed(1) + "°", 4, 46);
  ctx.fillStyle = up ? "#61a119" : "#061319"; ctx.fillText(g.el_deg.toFixed(1) + "°", 4, 106);
  const t = simNow(), aos = utcMs(latest.pass.aos_utc), los = utcMs(latest.pass.los_utc);
  let label, secs, color;
  if (t < aos) { label = "AOS en"; secs = (aos - t) / 1000; color = "#935500"; }
  else if (t < los) { label = "LOS en"; secs = (los - t) / 1000; color = "#61a119"; }
  else { label = "PASE TERMINADO"; secs = null; color = "#395d70"; }
  ctx.fillStyle = "#5e7e90"; ctx.font = `700 16px ${mono}`; ctx.fillText(label.toUpperCase(), 4, 136);
  if (secs !== null) { ctx.fillStyle = color; ctx.font = `700 28px ${mono}`; ctx.fillText(clock(secs), 4, 164); }
  ctx.fillStyle = "#5e7e90"; ctx.font = `16px ${mono}`;
  ctx.fillStyle = "#61a119"; ctx.fillText("● satélite", 4, h - 20); ctx.fillStyle = "#935500"; ctx.fillText("● rotor", 4, h - 7);
}
function drawAll(){ if ($("panel-track").hidden) return; drawSky(); }
setInterval(() => { if (latest && !$("panel-track").hidden) drawSky(); }, 250);

let searchTimer;
$("search").addEventListener("input",()=>{clearTimeout(searchTimer);searchTimer=setTimeout(()=>loadCatalog(true),250);});
$("sband-only").addEventListener("change",()=>loadCatalog(true));
$("visible-only").addEventListener("change",()=>loadCatalog(true));
$("apply-observer").addEventListener("click",()=>{loadCatalog(true);if(selected)selectSatellite(selected.norad_id);log("Posición del observador actualizada.");});
$("prev-page").addEventListener("click",()=>{if(page>1){page--;loadCatalog();}});
$("next-page").addEventListener("click",()=>{if(page<pages){page++;loadCatalog();}});
$("quick-list").addEventListener("click",event=>{const button=event.target.closest("button[data-norad]");if(button)selectSatellite(Number(button.dataset.norad));});
$("transmitter").addEventListener("change",()=>$("manual-wrap").hidden=$("transmitter").value!=="manual");
$("start-session").addEventListener("click",startSession);
$("pause-session").addEventListener("click",()=>control(latest?.status==="paused"?"resume":"pause"));
$("stop-session").addEventListener("click",()=>control("stop"));
$("speed").addEventListener("change",()=>{if(sessionId)control("speed",{speed:Number($("speed").value)});});
$("refresh-sources").addEventListener("click",async()=>{try{const data=await api("/api/catalog/refresh",{method:"POST"});showSource(data.catalog);log(data.started?"Actualización de CelesTrak y SatNOGS iniciada.":"Las fuentes ya se están actualizando.");}catch(error){toast(error.message);}});
window.addEventListener("resize",()=>requestAnimationFrame(drawAll));
setInterval(()=>{$("clock").textContent=new Date().toLocaleString("es-CL",{timeZone:"America/Santiago",hour12:false})+" CL";},1000);
setInterval(pollSession,900);
setInterval(loadStatus,30000);
// ── Pestañas (pantalla de 7"): 1 Selección · 2 Seguimiento · 3 Monitor RF ──
const TABS = ["select", "track", "rf"];
function showTab(name) {
  if (!TABS.includes(name)) name = "select";
  TABS.forEach(t => {
    $("panel-" + t).hidden = t !== name;
    const button = $("tab-" + t); button.setAttribute("aria-selected", String(t === name)); button.tabIndex = t === name ? 0 : -1;
  });
  try { localStorage.setItem("sattrack.tab", name); } catch (_) {}
  try { history.replaceState(null, "", "#" + name); } catch (_) {}
  requestAnimationFrame(() => window.dispatchEvent(new Event("resize")));   // los canvas se redimensionan al mostrarse
}
document.querySelector(".tabbar").addEventListener("click", e => { const b = e.target.closest("[data-tab]"); if (b) showTab(b.dataset.tab); });
document.querySelector(".tabbar").addEventListener("keydown", e => {
  const i = TABS.findIndex(t => $("tab-" + t).getAttribute("aria-selected") === "true");
  if (e.key === "ArrowRight" || e.key === "ArrowLeft") { const n = TABS[(i + (e.key === "ArrowRight" ? 1 : TABS.length - 1)) % TABS.length]; showTab(n); $("tab-" + n).focus(); e.preventDefault(); }
});
(() => { let saved = null; try { saved = localStorage.getItem("sattrack.tab"); } catch (_) {} showTab(location.hash.slice(1) || saved || "select"); })();
loadStatus();loadCatalog();requestAnimationFrame(drawAll);
