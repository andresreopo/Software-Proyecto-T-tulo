/* Monitor RF — portado de MaiaView (server/templates/index.html).
 * Recibe frames float32 (dB) por /ws/rf, dibuja espectro y waterfall a resolución completa
 * y controla el AD9361 por /api/rf/config. Depende de log() y toast() de app.js. */
(() => {
"use strict";
const q = id => document.getElementById(id);
const css = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
const specC = q("spectrum-canvas"), wfC = q("waterfall-canvas");
const sctx = specC.getContext("2d"), wctx = wfC.getContext("2d");

// El waterfall se acumula en un lienzo fuera de pantalla (una fila por frame).
const wfBuf = document.createElement("canvas"), bctx = wfBuf.getContext("2d");
wfBuf.width = 4096; wfBuf.height = 256;
bctx.fillStyle = css("--bg"); bctx.fillRect(0, 0, wfBuf.width, wfBuf.height);

let cfg = {freq: 0, rate: 0, bw: 0};
let N = 0;
let smooth = null, peaks = null;
let lo = -120, hi = -20;
let hoverX = null, lastFrameAt = 0, wsState = "disconnected";
const PAD = {l: 60, r: 24, t: 10, b: 28};

// ── Mapa de color del waterfall ─────────────────────────────────────────────
const LUT = (() => {
  const stops = [[0,[240,246,252]],[.25,[150,195,232]],[.5,[30,115,190]],[.75,[232,120,20]],[1,[110,10,20]]];
  const out = new Uint8ClampedArray(256 * 3);
  for (let i = 0; i < 256; i++) {
    const t = i / 255; let k = 0;
    while (k < stops.length - 2 && t > stops[k + 1][0]) k++;
    const [t0, c0] = stops[k], [t1, c1] = stops[k + 1], u = (t - t0) / (t1 - t0);
    for (let j = 0; j < 3; j++) out[i * 3 + j] = c0[j] + (c1[j] - c0[j]) * u;
  }
  return out;
})();

// ── Utilidades de dibujo ────────────────────────────────────────────────────
function fit(c) {
  const r = c.getBoundingClientRect(), d = devicePixelRatio || 1;
  const w = Math.round(r.width * d), h = Math.round(r.height * d);
  if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
}
function niceStep(range, target) {
  const raw = range / target, p = Math.pow(10, Math.floor(Math.log10(raw))), f = raw / p;
  return (f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10) * p;
}
const fmtMHz = v => v.toFixed(4) + " MHz";
const fmtHz = v => v >= 1e6 ? (v / 1e6).toFixed(3) + " MHz" : v >= 1e3 ? (v / 1e3).toFixed(2) + " kHz" : v.toFixed(0) + " Hz";

function prep(c, ctx, bottom = PAD.b) {
  fit(c);
  const d = devicePixelRatio || 1;
  ctx.setTransform(1, 0, 0, 1, 0, 0); ctx.clearRect(0, 0, c.width, c.height); ctx.scale(d, d);
  const w = c.width / d, h = c.height / d;
  return {w, h, x0: PAD.l, x1: w - PAD.r, y0: PAD.t, y1: h - bottom};
}
function freqAxis(ctx, g, gridColor, labels) {
  const f0 = (cfg.freq - cfg.rate / 2) / 1e6, f1 = (cfg.freq + cfg.rate / 2) / 1e6;
  const sx = niceStep(f1 - f0, 5);
  ctx.font = "16px " + css("--mono"); ctx.fillStyle = css("--muted");
  ctx.strokeStyle = gridColor; ctx.lineWidth = 1;
  ctx.textAlign = "center"; ctx.textBaseline = "top";
  for (let f = Math.ceil(f0 / sx) * sx; f <= f1; f += sx) {
    const x = Math.round(g.x0 + (f - f0) / (f1 - f0) * (g.x1 - g.x0)) + .5;
    ctx.beginPath(); ctx.moveTo(x, g.y0); ctx.lineTo(x, g.y1); ctx.stroke();
    if (labels) ctx.fillText(f.toFixed(sx < 1 ? 2 : 1), x, g.y1 + 6);
  }
}
function hoverLine(ctx, g) {
  if (hoverX === null || hoverX < g.x0 || hoverX > g.x1) return;
  ctx.strokeStyle = "rgba(15,30,45,.45)"; ctx.lineWidth = 1; ctx.beginPath();
  ctx.moveTo(hoverX + .5, g.y0); ctx.lineTo(hoverX + .5, g.y1); ctx.stroke();
}
function peakIndex(a) { let k = 0; for (let i = 1; i < a.length; i++) if (a[i] > a[k]) k = i; return k; }

// ── Espectro ────────────────────────────────────────────────────────────────
function drawSpectrum() {
  const g = prep(specC, sctx, 4);
  if (!cfg.rate) return;
  const border = css("--line");
  const Y = v => g.y1 - (v - lo) / (hi - lo) * (g.y1 - g.y0);
  const sy = niceStep(hi - lo, 4);
  sctx.font = "16px " + css("--mono"); sctx.fillStyle = css("--muted");
  sctx.strokeStyle = border; sctx.lineWidth = 1;
  sctx.textAlign = "right"; sctx.textBaseline = "middle";
  for (let v = Math.ceil(lo / sy) * sy; v <= hi; v += sy) {
    const y = Math.round(Y(v)) + .5;
    sctx.beginPath(); sctx.moveTo(g.x0, y); sctx.lineTo(g.x1, y); sctx.stroke();
    sctx.fillText(v.toFixed(0), g.x0 - 6, y);
  }
  freqAxis(sctx, g, border, false);
  sctx.strokeStyle = border; sctx.strokeRect(g.x0 + .5, g.y0 + .5, g.x1 - g.x0, g.y1 - g.y0);
  if (!smooth) return;

  sctx.save(); sctx.beginPath(); sctx.rect(g.x0, g.y0, g.x1 - g.x0, g.y1 - g.y0); sctx.clip();
  const line = (arr, color, lw) => {
    sctx.strokeStyle = color; sctx.lineWidth = lw; sctx.beginPath();
    const n = arr.length, step = Math.max(1, Math.floor(n / (g.x1 - g.x0)));
    for (let i = 0; i < n; i += step) {
      const x = g.x0 + i / (n - 1) * (g.x1 - g.x0), y = Y(arr[i]);
      i ? sctx.lineTo(x, y) : sctx.moveTo(x, y);
    }
    sctx.stroke();
  };
    line(smooth, css("--cyan"), 1.4);

  {
    const k = peakIndex(smooth), x = g.x0 + k / (smooth.length - 1) * (g.x1 - g.x0), y = Y(smooth[k]);
    sctx.fillStyle = css("--amber"); sctx.beginPath();
    sctx.moveTo(x, y - 3); sctx.lineTo(x - 5, y - 12); sctx.lineTo(x + 5, y - 12); sctx.closePath(); sctx.fill();
  }
  sctx.restore();
  hoverLine(sctx, g);
}

// ── Waterfall ───────────────────────────────────────────────────────────────
function pushWaterfallRow(a) {
  if (wfBuf.width !== a.length) {
    wfBuf.width = a.length; wfBuf.height = 256;
    bctx.fillStyle = css("--bg"); bctx.fillRect(0, 0, wfBuf.width, wfBuf.height);
  }
  bctx.drawImage(wfBuf, 0, 1);                       // desplaza todo una fila hacia abajo
  const row = bctx.createImageData(a.length, 1), span = hi - lo || 1;
  for (let i = 0; i < a.length; i++) {
    let t = (a[i] - lo) / span; t = t < 0 ? 0 : t > 1 ? 1 : t;
    const k = (t * 255 | 0) * 3, o = i * 4;
    row.data[o] = LUT[k]; row.data[o + 1] = LUT[k + 1]; row.data[o + 2] = LUT[k + 2]; row.data[o + 3] = 255;
  }
  bctx.putImageData(row, 0, 0);                      // fila nueva arriba
}
function drawWaterfall() {
  const g = prep(wfC, wctx);
  if (!cfg.rate) return;
  wctx.imageSmoothingEnabled = true;
  wctx.drawImage(wfBuf, 0, 0, wfBuf.width, wfBuf.height, g.x0, g.y0, g.x1 - g.x0, g.y1 - g.y0);
  freqAxis(wctx, g, "rgba(148,163,184,.22)", true);
  wctx.strokeStyle = css("--line"); wctx.strokeRect(g.x0 + .5, g.y0 + .5, g.x1 - g.x0, g.y1 - g.y0);
  wctx.fillStyle = css("--muted"); wctx.font = "16px " + css("--mono"); wctx.textAlign = "right";
  wctx.textBaseline = "top";    wctx.fillText("ahora", g.x0 - 5, g.y0);
  wctx.textBaseline = "bottom"; wctx.fillText("antes", g.x0 - 5, g.y1);
  hoverLine(wctx, g);
}

// ── Pestañas espectro / waterfall (mismo lugar en pantalla) ─────────────────
let drawPending = false;
function draw() {
  if (drawPending) return;
  drawPending = true;
  requestAnimationFrame(() => { drawPending = false; if (q("panel-rf").hidden) return; drawSpectrum(); drawWaterfall(); });
}

// ── Escala vertical automática (rango fijo en múltiplos de 10 dB; se contrae tras ~3 s) ──
let shrinkFrames = 0, rangeInit = false;
function autoScale() {
  let mn = Infinity, mx = -Infinity;
  for (let i = 0; i < N; i++) { const v = smooth[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
  const tlo = Math.floor((mn - 5) / 10) * 10, thi = Math.ceil((mx + 5) / 10) * 10;
  if (!rangeInit) { lo = tlo; hi = thi; rangeInit = true; return; }
  if (tlo < lo) lo = tlo;
  if (thi > hi) hi = thi;
  shrinkFrames = (tlo > lo + 10 || thi < hi - 10) ? shrinkFrames + 1 : 0;
  if (shrinkFrames > 60) { lo = tlo; hi = thi; shrinkFrames = 0; }
}

// ── Estado del enlace (píldora) ─────────────────────────────────────────────
function updatePill() {
  const pill = q("signal-pill");
  const streaming = wsState === "connected" && performance.now() - lastFrameAt < 2000;
  q("tab-rf-dot").className = "dot " + (streaming ? "ok" : wsState !== "connected" ? "bad" : "");
  if (wsState !== "connected") {
    pill.textContent = "MAIA DESCONECTADA"; pill.className = "pill warn";
  } else if (!streaming) {
    pill.textContent = "ESPERANDO RF"; pill.className = "pill muted";
  } else {
    pill.textContent = "RF EN VIVO"; pill.className = "pill";
  }
}

// ── Frames de espectro ──────────────────────────────────────────────────────
let lastPeakTxt = 0;
function onSpectrum(buf) {
  if (buf.byteLength % 4) return;
  const a = new Float32Array(buf); N = a.length; lastFrameAt = performance.now();
  q("rx-empty").style.display = "none";
  if (!smooth || smooth.length !== N) { smooth = Float32Array.from(a); peaks = Float32Array.from(a); }
  const k = 0.5;
  for (let i = 0; i < N; i++) {
    smooth[i] = k * smooth[i] + (1 - k) * a[i];
    if (a[i] > peaks[i]) peaks[i] = a[i];
  }
  autoScale();
  pushWaterfallRow(a);
  draw();
  const now = performance.now();
  if (now - lastPeakTxt > 250) {
    lastPeakTxt = now;
    const pk = peakIndex(smooth), fpk = (cfg.freq - cfg.rate / 2 + pk / (N - 1) * cfg.rate) / 1e6;
    q("rx-peak-big").textContent = fpk.toFixed(4);
    updatePill();
  }
}

setInterval(updatePill, 1000);

// ── WebSocket hacia el servidor Flask (/ws/rf) ──────────────────────────────
let lastWs = null;
function setWs(state) {
  wsState = state; updatePill();
  if (state !== lastWs) {
    log(state === "connected" ? "WebSocket de Maia conectado." : "WebSocket de Maia desconectado.");
    lastWs = state;
  }
  if (state === "connected" && !cfg.rate) loadConfig();   // Maia apareció después de abrir la página
}
function connect() {
  const ws = new WebSocket((location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws/rf");
  ws.binaryType = "arraybuffer";
  ws.onmessage = e => {
    if (typeof e.data === "string") {
      try { const m = JSON.parse(e.data); if (m.type === "status") setWs(m.ws); } catch (_) {}
    } else onSpectrum(e.data);
  };
  ws.onclose = () => { setWs("disconnected"); setTimeout(connect, 1500); };
}

// ── Configuración del receptor (REST → AD9361) ──────────────────────────────
function fillForm(c) {
  q("rx-gain").value = Number(c.rx_gain).toFixed(1);
  q("rx-gmode").value = c.rx_gain_mode;
  q("rx-gain").disabled = q("rx-gslider").disabled = c.rx_gain_mode !== "Manual";
  q("rx-gslider").value = c.rx_gain;
  cfg.freq = c.rx_lo_frequency; cfg.rate = c.sampling_frequency; cfg.bw = c.rx_rf_bandwidth;
  draw();
}
async function request(method, body) {
  const r = await fetch("/api/rf/config", {
    method, headers: {"Content-Type": "application/json"}, body: body ? JSON.stringify(body) : undefined
  });
  const j = await r.json();
  if (!r.ok) throw new Error(j.error || "Error desconocido");
  return j;
}
async function loadConfig() {
  try { fillForm(await request("GET")); log("Configuración leída desde Maia."); }
  catch (e) { log(e.message); }
}
async function applyConfig(body) {
  try {
    fillForm(await request("PATCH", body));
    if (smooth) peaks = Float32Array.from(smooth);
    rangeInit = false;
    log("Configuración aplicada: Fc " + fmtHz(cfg.freq) + ", SR " + fmtHz(cfg.rate));
  } catch (e) { log(e.message); if (typeof toast === "function") toast(e.message); loadConfig(); }
}
function applyGain() {
  const mode = q("rx-gmode").value, g = parseFloat(q("rx-gain").value);
  if (mode === "Manual" && isNaN(g)) { log("Ingresa una ganancia válida."); return; }
  const body = {rx_gain_mode: mode};
  if (mode === "Manual") body.rx_gain = g;      // la ganancia solo se envía en modo manual
  applyConfig(body);
}
q("rx-gslider").addEventListener("input", () => { q("rx-gain").value = Number(q("rx-gslider").value).toFixed(1); });
q("rx-gslider").addEventListener("change", applyGain);
q("rx-gain").addEventListener("input", () => { const v = parseFloat(q("rx-gain").value); if (!isNaN(v)) q("rx-gslider").value = v; });
q("rx-gain").addEventListener("keydown", e => { if (e.key === "Enter") e.target.blur(); });
q("rx-gain").addEventListener("change", applyGain);
q("rx-gmode").onchange = () => { q("rx-gain").disabled = q("rx-gslider").disabled = q("rx-gmode").value !== "Manual"; applyGain(); };

// ── Interacción con el espectro y el waterfall ──────────────────────────────
function onMove(e) {
  hoverX = e.clientX - e.currentTarget.getBoundingClientRect().left;
  draw();
}
function onDblClick(e) {
  const r = e.currentTarget.getBoundingClientRect(), x0 = PAD.l, x1 = r.width - PAD.r;
  const t = (e.clientX - r.left - x0) / (x1 - x0);
  if (t < 0 || t > 1 || !cfg.rate) return;
  applyConfig({rx_lo_frequency: Math.round(cfg.freq - cfg.rate / 2 + t * cfg.rate)});
}
[specC, wfC].forEach(c => {
  c.addEventListener("mousemove", onMove);
  c.addEventListener("mouseleave", () => { hoverX = null; draw(); });
  c.addEventListener("dblclick", onDblClick);
});
addEventListener("resize", draw);

// Diagnóstico visible: a qué dirección intenta conectarse el servidor y por qué falla.
async function refreshLinkNote() {
  try {
    const s = await (await fetch("/api/rf/status")).json(), pill = q("signal-pill");
    pill.title = !s.connected ? `Sin conexión con Maia en ${s.server_url}${s.error ? " — " + s.error : ""}. Verifica la IP de la Pluto (--pluto-ip) y el puerto 8000.`
      : !s.streaming ? `Conectado a ${s.server_url}, pero no llegan frames de espectro${s.error ? " — " + s.error : ""}.` : `Maia en ${s.server_url}`;
  } catch (_) {}
}
setInterval(refreshLinkNote, 3000);

loadConfig(); connect(); draw(); refreshLinkNote();
})();
