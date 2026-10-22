"use strict";

const $ = id => document.getElementById(id);
const fmt = (n, digits = 1) => Number.isFinite(Number(n)) ? Number(n).toLocaleString("es-CL", {minimumFractionDigits:digits, maximumFractionDigits:digits}) : "—";
const localTime = value => value ? new Date(value).toLocaleString("es-CL", {timeZone:"America/Santiago", day:"2-digit", month:"2-digit", hour:"2-digit", minute:"2-digit", hour12:false}) : "—";
const longTime = value => value ? new Date(value).toLocaleString("es-CL", {timeZone:"America/Santiago", day:"2-digit", month:"2-digit", year:"numeric", hour:"2-digit", minute:"2-digit", second:"2-digit", hour12:false}) : "—";
let selected = null, sessionId = null, latest = null, page = 1, pages = 1;
let querySequence = 0, detailSequence = 0, polling = false, lastPhase = null, lastLock = null;
let trail = [], waterfall = [];

function toast(message) {
  const box = $("toast"); box.textContent = message; box.hidden = false;
  clearTimeout(toast.timer); toast.timer = setTimeout(() => box.hidden = true, 6000);
}
function log(message) {
  const box = $("event-log"), row = document.createElement("div"), time = document.createElement("time");
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
  try { const data = await api("/api/status"); showSource(data.catalog); }
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
    const body = {norad_id:selected.norad_id, ...observer(), speed:Number($("speed").value)};
    if (txChoice === "manual") body.manual_frequency_mhz = Number($("manual-frequency").value);
    else body.transmitter_uuid = txChoice;
    if (sessionId) await api(`/api/session/${sessionId}/control`, {method:"POST", headers:{"Content-Type":"application/json"}, body:JSON.stringify({action:"stop"})}).catch(() => {});
    const data = await api("/api/session", {method:"POST", headers:{"Content-Type":"application/json"}, body:JSON.stringify(body)});
    sessionId = data.session_id; trail = []; waterfall = []; lastPhase = null; lastLock = null;
    $("pause-session").disabled = false; $("stop-session").disabled = false;
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
      sessionId = null; latest = null; $("pause-session").disabled = true; $("stop-session").disabled = true;
      $("tracking-title").textContent = "Sesión detenida"; $("signal-pill").textContent = "SIN SEÑAL"; $("signal-pill").className = "pill muted";
      $("phase").textContent = "DETENIDA"; $("phase").className = "pill muted";
      $("snr").textContent = "—"; $("doppler").textContent = "—";
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
  const geo = state.geometry, rotor = state.rotor, rf = state.receiver;
  $("tracking-title").textContent = `${state.name} · #${state.norad_id}`;
  $("sim-time").textContent = longTime(state.sim_time_chile) + ` · ${state.speed}×`;
  const phase = $("phase"); phase.textContent = state.phase.toUpperCase(); phase.className = "pill " + (state.phase === "en pase" ? "" : state.phase === "finalizado" ? "warn" : "muted");
  $("pass-progress").style.width = `${Math.round(state.progress * 100)}%`;
  $("target-pointing").textContent = `${fmt(rotor.target_az_deg, 1)}° / ${fmt(rotor.target_el_deg, 1)}°`;
  $("rotor-pointing").textContent = `${fmt(rotor.az_deg, 1)}° / ${fmt(rotor.el_deg, 1)}°`;
  $("pointing-error").textContent = `${fmt(rotor.pointing_error_deg, 1)}°`;
  $("range").textContent = `${fmt(geo.range_km, 0)} km`;
  $("rf-frequency").replaceChildren(document.createTextNode(fmt(rf.frequency_mhz, 3) + " "));
  const mhz = document.createElement("span"); mhz.textContent = "MHz"; $("rf-frequency").append(mhz);
  $("rf-mode").textContent = `${rf.mode} · ${rf.source === "manual" ? "frecuencia manual" : "SatNOGS DB"} · receptor virtual`;
  $("snr").textContent = `${fmt(rf.snr_db, 1)} dB`;
  $("doppler").textContent = `${rf.doppler_hz >= 0 ? "+" : ""}${fmt(rf.doppler_hz, 0)} Hz`;
  $("frames").textContent = rf.frames_received.toLocaleString("es-CL");
  const signal = $("signal-pill"); signal.textContent = rf.signal_detected ? "SEÑAL SIMULADA" : "SIN SEÑAL"; signal.className = "pill " + (rf.signal_detected ? "" : "muted");
  $("pause-session").textContent = state.status === "paused" ? "Reanudar" : "Pausar";
  if (lastPhase !== state.phase) { log(`Fase del pase: ${state.phase}.`); lastPhase = state.phase; }
  if (lastLock !== rf.signal_detected) { if (lastLock !== null) log(rf.signal_detected ? "Receptor virtual: señal detectada." : "Receptor virtual: señal perdida."); lastLock = rf.signal_detected; }
  const previous = trail.at(-1);
  if (!previous || previous.time !== state.sim_time_utc) {
    trail.push({time:state.sim_time_utc, az:geo.az_deg, el:geo.el_deg, lat:geo.subsat_lat_deg, lon:geo.subsat_lon_deg});
    if (trail.length > 120) trail.shift();
    waterfall.unshift(rf.spectrum_db);
    if (waterfall.length > 100) waterfall.pop();
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
function drawSky() {
  const {ctx,width:w,height:h} = canvasContext("sky-canvas"), cx = w / 2, cy = h / 2 + 6, radius = Math.min(w * .42, h * .43);
  ctx.strokeStyle = "#2a4b60"; ctx.lineWidth = 1;
  for (const k of [1,.67,.33]) { ctx.beginPath(); ctx.arc(cx,cy,radius*k,0,Math.PI*2); ctx.stroke(); }
  ctx.beginPath(); ctx.moveTo(cx-radius,cy); ctx.lineTo(cx+radius,cy); ctx.moveTo(cx,cy-radius); ctx.lineTo(cx,cy+radius); ctx.stroke();
  ctx.fillStyle = "#65869a"; ctx.font = "11px Consolas"; ctx.textAlign = "center";
  ctx.fillText("N",cx,cy-radius-10); ctx.fillText("S",cx,cy+radius+17); ctx.fillText("E",cx+radius+12,cy+4); ctx.fillText("O",cx-radius-12,cy+4);
  if (!latest) { ctx.fillStyle = "#708da1"; ctx.fillText("Inicia una sesión",cx,cy+4); return; }
  ctx.strokeStyle = "#57c9ed88"; ctx.lineWidth = 2; ctx.beginPath(); let started = false;
  for (const p of trail) { if (p.el < 0) continue; const [x,y] = skyPoint(p.az,p.el,cx,cy,radius); if (!started) {ctx.moveTo(x,y);started=true;} else ctx.lineTo(x,y); }
  ctx.stroke();
  const [sx,sy] = skyPoint(latest.geometry.az_deg,latest.geometry.el_deg,cx,cy,radius);
  const [rx,ry] = skyPoint(latest.rotor.az_deg,latest.rotor.el_deg,cx,cy,radius);
  dot(ctx,sx,sy,6,"#a6e65e"); dot(ctx,rx,ry,4,"#ffc16c");
  ctx.fillStyle = "#b9d5e2"; ctx.font = "10px Consolas"; ctx.textAlign = "left";
  ctx.fillText("● satélite",11,h-21); ctx.fillStyle="#ffc16c"; ctx.fillText("● rotor",11,h-8);
}
function geoPoint(lat,lon,w,h) { return [(lon+180)/360*w, (90-lat)/180*h]; }
function drawMap() {
  const {ctx,width:w,height:h} = canvasContext("map-canvas");
  ctx.fillStyle="#091c2b"; ctx.fillRect(0,0,w,h); ctx.strokeStyle="#1f3f51";ctx.lineWidth=1;
  for (let lon=-120;lon<=120;lon+=60){const x=(lon+180)/360*w;ctx.beginPath();ctx.moveTo(x,0);ctx.lineTo(x,h);ctx.stroke();}
  for (let lat=-60;lat<=60;lat+=30){const y=(90-lat)/180*h;ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(w,y);ctx.stroke();}
  const obs = observer(), [ox,oy]=geoPoint(obs.lat,obs.lon,w,h);
  if (latest) {
    ctx.strokeStyle="#57c9edaa";ctx.lineWidth=1.6;ctx.beginPath();let prev=null;
    for (const p of trail){const xy=geoPoint(p.lat,p.lon,w,h);if(!prev||Math.abs(xy[0]-prev[0])>w/2)ctx.moveTo(...xy);else ctx.lineTo(...xy);prev=xy;}
    ctx.stroke();
    const [sx,sy]=geoPoint(latest.geometry.subsat_lat_deg,latest.geometry.subsat_lon_deg,w,h);
    ctx.strokeStyle="#79c4c777";ctx.setLineDash([3,4]);ctx.beginPath();ctx.moveTo(ox,oy);ctx.lineTo(sx,sy);ctx.stroke();ctx.setLineDash([]);
    dot(ctx,sx,sy,5,"#a6e65e");
  }
  dot(ctx,ox,oy,5,"#ffc16c"); ctx.fillStyle="#c5dce6";ctx.font="10px Consolas";ctx.fillText("Santiago",ox+8,oy-7);
  ctx.fillStyle="#67879b";ctx.fillText("180° O",8,h-8);ctx.fillText("180° E",w-50,h-8);
}
function drawSpectrum() {
  const {ctx,width:w,height:h}=canvasContext("spectrum-canvas"), l=41,r=w-9,t=12,b=h-28;
  ctx.strokeStyle="#244459";ctx.lineWidth=1;ctx.font="10px Consolas";ctx.fillStyle="#65869a";
  for(let db=-110;db<=-60;db+=10){const y=b-(db+115)/60*(b-t);ctx.beginPath();ctx.moveTo(l,y);ctx.lineTo(r,y);ctx.stroke();ctx.fillText(String(db),3,y+3);}
  for(let i=0;i<=6;i++){const x=l+(r-l)*i/6;ctx.beginPath();ctx.moveTo(x,t);ctx.lineTo(x,b);ctx.stroke();ctx.textAlign="center";ctx.fillText(String(i-3),x,b+15);}ctx.textAlign="left";
  if(!latest)return;
  const values=latest.receiver.spectrum_db;ctx.strokeStyle="#57c9ed";ctx.lineWidth=1.7;ctx.beginPath();
  for(let i=0;i<values.length;i++){const x=l+(r-l)*i/(values.length-1),y=b-(values[i]+115)/60*(b-t);i?ctx.lineTo(x,y):ctx.moveTo(x,y);}ctx.stroke();
  const peak=values.indexOf(Math.max(...values)),px=l+(r-l)*peak/(values.length-1);
  ctx.strokeStyle="#ffc16c";ctx.beginPath();ctx.moveTo(px,t);ctx.lineTo(px,b);ctx.stroke();
  ctx.fillStyle="#88adbf";ctx.fillText("MHz desde portadora",l+5,h-6);
}
function drawWaterfall() {
  const {ctx,width:w,height:h}=canvasContext("waterfall-canvas");ctx.fillStyle="#091b29";ctx.fillRect(0,0,w,h);
  if(!waterfall.length)return;
  const source=document.createElement("canvas");source.width=256;source.height=waterfall.length;
  const sctx=source.getContext("2d"),image=sctx.createImageData(source.width,source.height);
  for(let y=0;y<waterfall.length;y++)for(let x=0;x<256;x++){
    const v=Math.max(0,Math.min(1,(waterfall[y][x]+112)/50)),i=(y*256+x)*4;
    image.data[i]=Math.round(8+220*v*v);image.data[i+1]=Math.round(27+180*v);image.data[i+2]=Math.round(48+55*(1-v));image.data[i+3]=255;
  }
  sctx.putImageData(image,0,0);ctx.imageSmoothingEnabled=true;ctx.drawImage(source,0,0,w,h);
}
function drawAll(){drawSky();drawMap();drawSpectrum();drawWaterfall();}

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
loadStatus();loadCatalog();requestAnimationFrame(drawAll);
