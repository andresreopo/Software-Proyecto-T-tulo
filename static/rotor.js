/* Rotor (ESP32 por puerto serial) — portado de MaiaView.
 * Conexión serial, referencia manual (SET), seguimiento del pase de la sesión (TRK), posición real
 * devuelta por la ESP32 (POS), vector de azimut sobre el mapa y barra de elevación.
 * Depende de log(), toast(), latest y sessionId de app.js. */
(() => {
"use strict";
const stub = document.createElement("div");           // elementos que ya no existen en la interfaz
const q = id => document.getElementById(id) || stub;
const css = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
const getLatest = () => (typeof latest !== "undefined" ? latest : null);      // `let` globales de app.js
const getSession = () => (typeof sessionId !== "undefined" ? sessionId : null);

let rotor = {connected: false, port: null, baud: null, az: null, el: null, tgtAz: null, tgtEl: null,
             error: null, sim: false, elMin: 0, elMax: 90, staleMs: 2000, bat: null, batStaleMs: 5000};
let follow = {status: "idle"};
let bno = null, bnoBusy = false, bnoPolling = false;
let rotorAt = null, batAt = null, lastErr = null, drawPending = false, tickKey = "";
const isStale = () => rotorAt === null || performance.now() - rotorAt > rotor.staleMs;
const wrap180 = d => ((d + 540) % 360) - 180;
const fmtDur = s => {
  s = Math.max(0, Math.round(s)); const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60), x = s % 60, p = n => String(n).padStart(2, "0");
  return h ? h + ":" + p(m) + ":" + p(x) : m + ":" + p(x);
};

// ── Barra de elevación ──────────────────────────────────────────────────────
function buildTicks() {
  const key = rotor.elMin + "/" + rotor.elMax; if (key === tickKey) return; tickKey = key;
  const sc = q("el-scale"); sc.querySelectorAll(".el-tick").forEach(t => t.remove());
  const step = 30;
  for (let v = rotor.elMin; v <= rotor.elMax + 1e-6; v += step) {
    const t = document.createElement("div"); t.className = "el-tick";
    t.style.bottom = ((v - rotor.elMin) / (rotor.elMax - rotor.elMin) * 100) + "%"; t.textContent = v + "°";
    sc.appendChild(t);
  }
}
const elPct = el => (Math.max(rotor.elMin, Math.min(rotor.elMax, el)) - rotor.elMin) / (rotor.elMax - rotor.elMin) * 100;

// ── Interfaz ────────────────────────────────────────────────────────────────
function updateUI() {
  buildTicks();
  const live = rotor.connected && rotor.az !== null, stale = isStale();
  const lat = getLatest(), sess = lat && lat.rotor && getSession() ? lat.rotor : null;     // sin rotor real: valores de la sesión
  const curEl = live ? rotor.el : sess ? sess.el_deg : null;
  const tgtEl = rotor.tgtEl !== null && rotor.connected ? rotor.tgtEl : sess ? sess.target_el_deg : null;
  const c = q("el-cur"), t = q("el-tgt");
  c.hidden = curEl === null;
  if (curEl !== null) {
    c.style.bottom = elPct(curEl) + "%"; c.classList.toggle("stale", live && stale);
    q("el-cur-v").textContent = curEl.toFixed(1) + "°"; q("el-fill").style.height = elPct(curEl) + "%";
  } else q("el-fill").style.height = "0";
  t.hidden = tgtEl === null;
  if (!t.hidden) { t.style.bottom = elPct(tgtEl) + "%"; q("el-tgt-v").textContent = tgtEl.toFixed(1) + "°"; }
  q("el-leg-c").textContent = curEl !== null ? curEl.toFixed(1) + "°" : "—";
  q("el-leg-t").textContent = tgtEl !== null ? tgtEl.toFixed(1) + "°" : "—";

  q("rm-act").textContent = live ? "az " + rotor.az.toFixed(1) + "° · el " + rotor.el.toFixed(1) + "°" : "—";
  q("rm-tgt").textContent = rotor.tgtAz !== null ? "az " + rotor.tgtAz.toFixed(1) + "° · el " + rotor.tgtEl.toFixed(1) + "°" : "—";
  q("rm-err").textContent = live && rotor.tgtAz !== null
    ? "Δaz " + wrap180(rotor.tgtAz - rotor.az).toFixed(1) + "° · Δel " + (rotor.tgtEl - rotor.el).toFixed(1) + "°" : "—";

  const batOn = rotor.connected && rotor.bat !== null, batStale = batAt === null || performance.now() - batAt > rotor.batStaleMs;
  q("bat-top-v").textContent = batOn ? rotor.bat.toFixed(2) + " V" : "—";   // solo voltaje, sin porcentaje
  q("bat-top").classList.toggle("off", !batOn);                              // sin rotor/dato: atenuado
  q("bat-top").classList.toggle("stale", batOn && batStale);                // dato viejo: más atenuado

  const pill = q("rot-pill");
  pill.textContent = !rotor.connected ? "DESCONECTADO" : rotor.sim ? "SIMULADOR" : "CONECTADO";
  pill.className = "pill " + (!rotor.connected ? "muted" : stale && rotor.az !== null ? "warn" : "");
  q("rot-conn").textContent = rotor.connected ? "Desconectar" : "Conectar";
  q("rot-status").textContent = rotor.connected
    ? (rotor.sim ? "Simulador activo." : "Conectado a " + rotor.port + " · " + rotor.baud + " baud.")
      + (rotor.az === null ? " Esperando POS de la ESP32…" : stale ? " Sin datos de la ESP32 (posición obsoleta)." : "")
    : (rotor.error ? "Error: " + rotor.error : "Desconectado.");
  renderFollow();
  renderBno();
}

function renderBno() {
  const active = Boolean(bno?.active);
  const record = q("bno-record"), download = q("bno-download");
  record.textContent = active ? "Detener captura" : "Grabar BNO";
  record.disabled = bnoBusy || !bno || (!active && !rotor.connected);
  download.hidden = active || !bno?.samples;
  if (!download.hidden) download.href = "/api/bno/captures/" + encodeURIComponent(bno.filename);
  else download.removeAttribute("href");
  const count = (bno?.samples ?? 0).toLocaleString("es-CL");
  const samples = count + (bno?.simulated ? " muestras (sim)" : " muestras");
  if (active) {
    q("bno-status").textContent = bno.samples ? "Grabando · " + samples : "Esperando muestras BNO…";
  } else if (bno?.error) {
    q("bno-status").textContent = bno.samples ? bno.error + " · " + samples + " guardadas" : bno.error;
  } else if (bno?.filename) {
    q("bno-status").textContent = bno.samples ? samples : "Captura sin muestras.";
  } else {
    q("bno-status").textContent = !rotor.connected ? "Conecta la ESP32 para grabar."
      : bno ? "Captura disponible" : "Consultando estado de captura…";
  }
}

function renderFollow() {
  const st = follow.status, active = st === "waiting" || st === "tracking" || st === "paused";
  const mine = !!getSession() && follow.session_id === getSession();
  const on = active && mine;
  let txt = "—";
  if (mine || active) {
    if (st === "waiting") txt = "Esperando AOS · en " + fmtDur(follow.t_to_aos);
    else if (st === "tracking") txt = "Siguiendo · LOS en " + fmtDur(follow.t_to_los);
    else if (st === "paused") txt = "En pausa (rotor fijo)";
    else if (st === "finished") txt = "Pase terminado";
    else if (st === "stopped") txt = "Detenido";
  }
  if (txt !== "—") q("rot-status").textContent += " · " + txt;
  q("rf-vel").textContent = st === "tracking" && follow.vaz !== null ? follow.vaz.toFixed(2) + " / " + follow.vel.toFixed(2) + " °/s" : "—";
  const f = q("rot-follow");
  f.textContent = on ? "■ Detener seguimiento" : "▶ Seguir con el rotor";
  f.className = "button " + (on ? "ghost" : "primary") + " full";
  f.disabled = !rotor.connected || !getSession();
  const warn = q("rot-warn"), msgs = (follow.warnings || []).concat(follow.error ? ["Rotor: " + follow.error] : []);
  const key = JSON.stringify(msgs);
  if (warn.dataset.key !== key) {
    warn.dataset.key = key; warn.textContent = "";
    msgs.forEach(t => conLine("sys", "⚠ " + t));
  }
}

function schedule() {
  if (drawPending) return; drawPending = true;
  requestAnimationFrame(() => { drawPending = false; updateUI(); });
}

// ── Consola serial (TX/RX) ──────────────────────────────────────────────────
const con = q("serial-console"); let conPaused = false, conBuf = [];
function conLine(kind, text) {
  if (kind !== "sys" && !q("con-poll").checked && /^(POS|TRK|BAT)\b/.test(text)) return;
  if (conPaused) { conBuf.push([kind, text]); if (conBuf.length > 1000) conBuf.shift(); return; }
  const row = document.createElement("div"); row.className = "con-" + kind;
  const tm = new Date(), p = n => String(n).padStart(2, "0");
  row.textContent = `${p(tm.getHours())}:${p(tm.getMinutes())}:${p(tm.getSeconds())} ${kind === "tx" ? "→" : kind === "rx" ? "←" : "·"} ${text}`;
  const near = con.scrollHeight - con.scrollTop - con.clientHeight < 24;
  con.appendChild(row);
  while (con.children.length > 1000) con.firstChild.remove();
  if (near) con.scrollTop = con.scrollHeight;
}
q("con-clear").onclick = () => { con.replaceChildren(); conBuf = []; };
q("con-pause").onclick = () => {
  conPaused = !conPaused; q("con-pause").textContent = conPaused ? "Reanudar" : "Pausar";
  if (!conPaused) { const b = conBuf; conBuf = []; b.forEach(([k, t]) => conLine(k, t)); }
};

// Entrada de la terminal: Enter envía; ↑/↓ recorren el historial de comandos enviados.
const history_ = []; let histPos = 0;
q("con-form").addEventListener("submit", async e => {
  e.preventDefault();
  const input = q("con-input"), text = input.value.trim();
  if (!text) return;
  if (history_[history_.length - 1] !== text) history_.push(text);
  if (history_.length > 50) history_.shift();
  histPos = history_.length; input.value = "";
  try { await call("/api/serial/send", {text}); }
  catch (err) { conLine("sys", "Error: " + err.message); }
  input.focus();
});
q("con-input").addEventListener("keydown", e => {
  if (e.key !== "ArrowUp" && e.key !== "ArrowDown") return;
  if (!history_.length) return;
  histPos = Math.max(0, Math.min(history_.length, histPos + (e.key === "ArrowUp" ? -1 : 1)));
  q("con-input").value = history_[histPos] ?? ""; e.preventDefault();
});

// ── Eventos del servidor (/ws/rotor) ────────────────────────────────────────
function applyRotor(m) {
  const was = rotor.connected;
  rotor = {connected: m.connected, port: m.port, baud: m.baud, az: m.az, el: m.el, tgtAz: m.tgt_az, tgtEl: m.tgt_el,
           error: m.error, sim: m.sim, elMin: m.el_min ?? 0, elMax: m.el_max ?? 90, staleMs: (m.stale_s ?? 2) * 1000,
           bat: m.bat_v ?? null, batStaleMs: (m.bat_stale_s ?? 5) * 1000};
  batAt = m.bat_age === null || m.bat_age === undefined ? null : performance.now() - m.bat_age * 1000;
  rotorAt = m.age === null || m.age === undefined ? null : performance.now() - m.age * 1000;
  if (rotor.connected && rotor.port && [...q("rot-port").options].some(o => o.value === rotor.port)) q("rot-port").value = rotor.port;   // refleja el puerto realmente abierto
  if (rotor.connected !== was) { log(rotor.connected ? "Rotor conectado en " + rotor.port : "Rotor desconectado"); conLine("sys", rotor.connected ? "Conectado a " + rotor.port + " @ " + rotor.baud : "Desconectado"); if (rotor.connected) setTimeout(enableFollow, 0); }
  if (rotor.error && rotor.error !== lastErr) { log("Rotor: " + rotor.error); conLine("sys", "Error: " + rotor.error); }
  lastErr = rotor.error;
  schedule();
}
function applyFollow(m) {
  const was = follow.status; follow = m;
  if (m.status !== was) {
    if (m.status === "tracking") log("AOS: el rotor sigue al satélite.");
    else if (m.status === "finished") log("LOS: pase terminado, el rotor se detiene.");
    else if (m.status === "stopped" && was !== "idle") log("Seguimiento del rotor detenido.");
  }
  schedule();
}
function connectWs() {
  const ws = new WebSocket((location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws/rotor");
  ws.onmessage = e => {
    let m; try { m = JSON.parse(e.data); } catch (_) { return; }
    if (m.type === "rotor") applyRotor(m);
    else if (m.type === "rotor_io") conLine(m.dir, m.text);
    else if (m.type === "rotor_msg") log("ESP32: " + m.text);
    else if (m.type === "follow") applyFollow(m);
  };
  ws.onclose = () => setTimeout(connectWs, 1500);
}
setInterval(schedule, 500);            // refresca el aviso de "posición obsoleta" y el vector al llegar el estado de la sesión
addEventListener("resize", schedule);

// ── Acciones ────────────────────────────────────────────────────────────────
async function call(url, body, method = "POST") {
  const r = await fetch(url, {method, cache: "no-store", headers: {"Content-Type": "application/json"}, body: method === "GET" ? undefined : JSON.stringify(body || {})});
  const j = await r.json();
  if (!r.ok) throw new Error(j.error || "Error desconocido");
  return j;
}
async function refreshBno() {
  if (bnoBusy || bnoPolling || q("panel-track").hidden) return;
  const before = bno;
  bnoPolling = true;
  try {
    const state = await call("/api/bno/capture", null, "GET");
    if (!bnoBusy && bno === before) bno = state;
  } catch (_) {
    if (!bnoBusy && bno === before) bno = null;
  } finally {
    bnoPolling = false;
    schedule();
  }
}
q("bno-record").onclick = async () => {
  if (bnoBusy || !bno) return;
  bnoBusy = true;
  schedule();
  try {
    const action = bno.active ? "stop" : "start";
    bno = await call("/api/bno/capture/" + action);
  } catch (error) {
    toast(error.message);
  } finally {
    bnoBusy = false;
    schedule();
  }
};
setInterval(refreshBno, 1000);

async function loadPorts() {
  try {
    const j = await call("/api/serial/ports", null, "GET"), sel = q("rot-port"), keep = sel.value || rotor.port;
    sel.replaceChildren();
    j.ports.forEach(p => { const o = document.createElement("option"); o.value = p.device; o.textContent = p.device + " — " + p.description; sel.appendChild(o); });
    if (keep && [...sel.options].some(o => o.value === keep)) sel.value = keep;
  } catch (e) { log("No se pudo listar los puertos: " + e.message); }
}
q("rot-refresh").onclick = loadPorts;
q("rot-conn").onclick = async () => {
  q("rot-conn").disabled = true;
  try {
    if (rotor.connected) applyRotor(await call("/api/serial/disconnect"));
    else {
      const baud = parseInt(q("rot-baud").value);
      if (!q("rot-port").value || isNaN(baud)) { toast("Elige un puerto y unos baudios válidos."); return; }
      applyRotor(await call("/api/serial/connect", {port: q("rot-port").value, baud}));
    }
  } catch (e) { toast(e.message); log(e.message); }
  finally { q("rot-conn").disabled = false; }
};
async function enableFollow() {
  const sid = getSession();
  if (!sid || !rotor.connected) return;
  const active = follow.status === "waiting" || follow.status === "tracking" || follow.status === "paused";
  if (active && follow.session_id === sid) return;
  try { applyFollow(await call(`/api/session/${encodeURIComponent(sid)}/rotor-follow`, {enabled: true})); }
  catch (e) { toast(e.message); conLine("sys", "Error: " + e.message); }
}
window.rotorAutoFollow = () => rotor.connected;

loadPorts(); connectWs(); schedule();
})();
