/* Monitor RF — portado de MaiaView (server/templates/index.html).
 * Recibe potencia lineal float32 por /ws/rf y dibuja espectro y waterfall en esa escala
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

let cfg = {freq: 0, rate: 0};
let N = 0;
let smooth = null;
// Integrador: promedio móvil exponencial de potencia lineal, con arranque
// en promedio acumulado. Con M frames la desviación del ruido baja ≈ √M, así que las señales débiles emergen del piso.
let integM = 4, integSeen = 0, fps = 0, prevFrameT = 0, noisePower = null;
function resetIntegration() { smooth = null; integSeen = 0; noisePower = null; }
let lo = 0, hi = 1;
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
const powerDb = power => 10 * Math.log10(power);
const validPower = power => Number.isFinite(power) && power > 0;
const trimZeros = (v, d) => v.toFixed(d).replace(/\.?0+$/, "");
function fmtPower(value, step) {
  if (value === 0) return "0";
  const scale = value >= 1e6 ? 1e6 : value >= 1e4 ? 1e3 : 1;
  const unit = scale === 1e6 ? "M" : scale === 1e3 ? "k" : "";
  const precision = Math.max(1, Math.ceil(-Math.log10(step / scale)));
  return precision > 6 ? value.toExponential(2) : trimZeros(value / scale, precision) + unit;
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
// Piso de ruido: mediana del espectro integrado sin el 10 % de cada borde (donde cae el filtro del AD9361),
// robusta frente a señales de banda angosta. Si la señal ocupa más de la mitad de la banda, subestima el SNR.
function estimateNoise(a) {
  const s = Float32Array.from(a.subarray(Math.floor(a.length * .1), Math.ceil(a.length * .9))).sort();
  return s[s.length >> 1];
}
function peakIndex(a) { let k = 0; for (let i = 1; i < a.length; i++) if (a[i] > a[k]) k = i; return k; }

// ── Espectro ────────────────────────────────────────────────────────────────
function drawSpectrum() {
  // El margen izquierdo se ajusta al ancho de las etiquetas para que el número siempre quepa.
  if (cfg.rate && hi > lo) {
    const sy0 = niceStep(hi - lo, 4); sctx.font = "16px " + css("--mono");
    const wmax = Math.max(sctx.measureText(fmtPower(lo, sy0)).width, sctx.measureText(fmtPower(hi, sy0)).width);
    PAD.l = Math.max(60, Math.ceil(wmax) + 14);
  }
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
    sctx.fillText(fmtPower(v, sy), g.x0 - 6, y);
  }
  freqAxis(sctx, g, border, false);
  sctx.strokeStyle = border; sctx.strokeRect(g.x0 + .5, g.y0 + .5, g.x1 - g.x0, g.y1 - g.y0);
  if (!smooth) return;

  sctx.save(); sctx.beginPath(); sctx.rect(g.x0, g.y0, g.x1 - g.x0, g.y1 - g.y0); sctx.clip();
  const line = (arr, color, lw) => {
    sctx.strokeStyle = color; sctx.lineWidth = lw; sctx.beginPath();
    const n = arr.length, step = Math.max(1, Math.floor(n / (g.x1 - g.x0)));
    for (let i = 0; i < n; i += step) {
      const x = g.x0 + i / n * (g.x1 - g.x0), y = Y(arr[i]);
      i ? sctx.lineTo(x, y) : sctx.moveTo(x, y);
    }
    sctx.stroke();
  };
    line(smooth, css("--cyan"), 1.4);
  if (noisePower !== null && q("rx-noise-line").checked) {
    const y = Y(noisePower);
    sctx.save(); sctx.setLineDash([6, 4]); sctx.strokeStyle = css("--amber"); sctx.lineWidth = 1.2;
    sctx.beginPath(); sctx.moveTo(g.x0, y); sctx.lineTo(g.x1, y); sctx.stroke(); sctx.restore();
    sctx.fillStyle = css("--amber"); sctx.font = "14px " + css("--mono"); sctx.textAlign = "right"; sctx.textBaseline = "bottom";
    sctx.fillText("ruido " + fmtPower(noisePower, noisePower * .01), g.x1 - 4, y - 2);
  }

  {
    const k = peakIndex(smooth), x = g.x0 + k / smooth.length * (g.x1 - g.x0), y = Y(smooth[k]);
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

// ── Escala vertical automática en potencia lineal ────────────────────────────
let shrinkFrames = 0, rangeInit = false;
function autoScale() {
  let mn = Infinity, mx = -Infinity;
  for (let i = 0; i < N; i++) { const v = smooth[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
  const padding = Math.max((mx - mn) * .05, mx * .01);
  const step = niceStep(mx - mn + 2 * padding, 4);
  const tlo = Math.max(0, Math.floor((mn - padding) / step) * step);
  const thi = Math.ceil((mx + padding) / step) * step;
  if (!rangeInit) { lo = tlo; hi = thi; rangeInit = true; return; }
  if (tlo < lo) lo = tlo;
  if (thi > hi) hi = thi;
  const slack = (hi - lo) * .15;
  shrinkFrames = (tlo > lo + slack || thi < hi - slack) ? shrinkFrames + 1 : 0;
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
  if (buf.byteLength < 8 || buf.byteLength % 4) return;
  const a = new Float32Array(buf);
  // Maia manda potencia lineal. Los ceros, valores negativos y no finitos no son niveles en dB.
  let mn = Infinity, mx = -Infinity, bad = 0;
  for (const v of a) { if (validPower(v)) { if (v < mn) mn = v; if (v > mx) mx = v; } else bad++; }
  if (!Number.isFinite(mn)) return;
  N = a.length;
  lastFrameAt = performance.now();
  q("rx-empty").style.display = "none";
  const nowT = performance.now();
  if (prevFrameT && nowT - prevFrameT < 2000) fps = fps ? 0.9 * fps + 0.1 * (1000 / (nowT - prevFrameT)) : 1000 / (nowT - prevFrameT);
  prevFrameT = nowT;
  if (!smooth || smooth.length !== N) {
    smooth = new Float64Array(N); integSeen = 0; noisePower = null;
  }
  const fill = integSeen === 0 && bad ? a.filter(validPower).sort()[Math.floor((N - bad) / 2)] : 0;
  integSeen++;
  const alpha = 1 / Math.min(integM, integSeen);
  for (let i = 0; i < N; i++) {
    const v = a[i];
    if (integSeen === 1) smooth[i] = validPower(v) ? v : fill;
    else if (validPower(v)) smooth[i] += alpha * (v - smooth[i]);
  }
  autoScale();
  pushWaterfallRow(smooth);          // la cascada también muestra la señal integrada
  draw();
  const now = performance.now();
  if (now - lastPeakTxt > 250) {
    lastPeakTxt = now;
    const pk = peakIndex(smooth), fpk = cfg.freq + (pk - Math.floor(N / 2)) * cfg.rate / N;
    q("rx-peak-big").textContent = cfg.rate ? trimZeros(fpk / 1e6, 4) : "—";
    noisePower = estimateNoise(smooth);
    const peakDb = powerDb(smooth[pk]), noiseDb = powerDb(noisePower);
    q("rx-power").textContent = peakDb.toFixed(1);
    q("rx-noise").textContent = noiseDb.toFixed(1);
    q("rx-snr").textContent = (peakDb - noiseDb).toFixed(1);
    q("rx-integ-note").textContent = integM === 1 ? "Sin promedio" :
      `Promedio de ${integM} frames` + (fps > 0.5 ? ` ≈ ${(integM / fps).toFixed(integM / fps < 10 ? 1 : 0)} s` : "") + (integSeen < integM ? ` · acumulando ${integSeen}/${integM}` : "");
    q("rx-integ-in").textContent = `Entrada: ${powerDb(mn).toFixed(1)} … ${powerDb(mx).toFixed(1)} dB`
      + (bad ? ` · ${bad} bins no válidos` : "");
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
function setSpectrumGeometry(freq, rate) {
  if (!Number.isFinite(freq) || !Number.isFinite(rate) || rate <= 0) return;
  if (cfg.freq === freq && cfg.rate === rate) return;
  cfg = {freq, rate};
  resetIntegration();
  rangeInit = false;
  bctx.fillStyle = css("--bg");
  bctx.fillRect(0, 0, wfBuf.width, wfBuf.height);
  draw();
}

function fillForm(c) {
  q("rx-gain").value = Number(c.rx_gain).toFixed(1);
  q("rx-gmode").value = c.rx_gain_mode;
  q("rx-gain").disabled = q("rx-gslider").disabled = c.rx_gain_mode !== "Manual";
  q("rx-gslider").value = c.rx_gain;
  q("rx-fc").value = trimZeros(c.rx_lo_frequency / 1e6, 6);
  q("rx-fslider").value = c.rx_lo_frequency / 1e6;
  q("rx-sr").value = trimZeros(c.sampling_frequency / 1e6, 3);
  q("rx-sslider").value = c.sampling_frequency / 1e6;
  q("rx-bw").value = trimZeros(c.rx_rf_bandwidth / 1e6, 3);
  q("rx-bslider").value = c.rx_rf_bandwidth / 1e6;
  setSpectrumGeometry(c.center_freq_hz ?? c.rx_lo_frequency, c.span_hz ?? c.sampling_frequency);
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
async function applyConfig(body, quiet = false) {
  try {
    fillForm(await request("PATCH", body));
    resetIntegration();                 // otra Fc/muestreo: el promedio anterior ya no corresponde
    rangeInit = false;
    if (!quiet) log("Configuración aplicada: Fc " + fmtHz(cfg.freq) + ", SR " + fmtHz(cfg.rate));
  } catch (e) { log(e.message); if (!quiet && typeof toast === "function") toast(e.message); loadConfig(); }
}
// ── Barrido de frecuencia central ±ancho mientras la casilla esté marcada ───────────────────────────
// Triángulo continuo base−ancho … base+ancho a la velocidad indicada (MHz/s); sirve para comprobar que hay señal.
// Cada paso es un PATCH a Maia, así que el paso es de 1 MHz y el intervalo sale de step/velocidad; si eso bajara
// de SWEEP_MIN_MS, se fija el intervalo y se agranda el paso. Al desmarcar la casilla la Fc vuelve a la base.
// Si cambias la Fc a mano durante el barrido, esa pasa a ser la base. No depende de que haya seguimiento activo.
const SWEEP_STEP_HZ = 1e6, SWEEP_MIN_MS = 300;
let sweepBase = null, sweepLast = null, sweepOff = 0, sweepDir = 1, sweepBusy = false;
function sweepParams() {
  const w = parseFloat(q("rx-sweep-width").value), v = parseFloat(q("rx-sweep-speed").value);
  const width = Number.isFinite(w) ? Math.min(100, Math.max(0.1, w)) * 1e6 : 10e6;     // Hz, semiancho
  const speed = Number.isFinite(v) ? Math.min(100, Math.max(0.05, v)) * 1e6 : 1e6;      // Hz/s
  let ms = SWEEP_STEP_HZ / speed * 1000, step = SWEEP_STEP_HZ;
  if (ms < SWEEP_MIN_MS) { ms = SWEEP_MIN_MS; step = speed * ms / 1000; }
  return {width, speed, step, ms: Math.min(ms, 10000)};
}
function sweepNote(t) { q("rx-sweep-note").textContent = t; }
let sweepTimer = null;
async function sweepTick() {
  clearTimeout(sweepTimer);
  const prm = sweepParams();
  sweepTimer = setTimeout(sweepTick, prm.ms);          // se reprograma con los parámetros vigentes en cada paso
  const on = q("rx-sweep").checked;
  if (sweepBusy) return;
  if (!on) {
    if (sweepBase !== null) {
      sweepBusy = true;
      const base = sweepBase; sweepBase = null; sweepLast = null;
      if (cfg.freq !== base) await applyConfig({rx_lo_frequency: base}, true);
      sweepBusy = false; log("Barrido detenido: Fc restituida a " + fmtHz(base) + ".");
    }
    sweepNote("Inactivo");
    return;
  }
  if (!cfg.rate) { sweepNote("Esperando a Maia…"); return; }
  if (sweepBase === null || cfg.freq !== sweepLast) {          // inicio, o Fc cambiada desde fuera: nueva base
    sweepBase = cfg.freq; sweepOff = 0; sweepDir = 1;
    if (sweepLast === null) log("Barrido de Fc iniciado: " + fmtHz(sweepBase) + " ± " + fmtHz(prm.width) + " a " + trimZeros(prm.speed / 1e6, 2) + " MHz/s.");
  }
  sweepOff += sweepDir * prm.step;
  if (sweepOff >= prm.width) { sweepOff = prm.width; sweepDir = -1; } else if (sweepOff <= -prm.width) { sweepOff = -prm.width; sweepDir = 1; }
  if (Math.abs(sweepOff) > prm.width) sweepOff = Math.sign(sweepOff) * prm.width;   // el ancho se redujo con el barrido en curso
  const hz = Math.min(6000e6, Math.max(70e6, sweepBase + sweepOff));
  sweepBusy = true;
  await applyConfig({rx_lo_frequency: Math.round(hz)}, true);
  sweepLast = cfg.freq; sweepBusy = false;
  sweepNote(`Barriendo: Fc ${fmtHz(cfg.freq)} (base ${fmtHz(sweepBase)}, ${sweepOff >= 0 ? "+" : "−"}${trimZeros(Math.abs(sweepOff) / 1e6, 2)} MHz)`);
}
q("rx-sweep").addEventListener("change", sweepTick);
sweepTimer = setTimeout(sweepTick, 700);

// Llamado desde app.js al elegir satélite/transmisor: centra el receptor en esa frecuencia (rango AD9361: 70–6000 MHz).
window.rfTune = async (mhz, label) => {
  if (!Number.isFinite(mhz) || mhz < 70 || mhz > 6000) { log("Frecuencia fuera del rango del AD9361 (70–6000 MHz): " + mhz); return false; }
  const hz = Math.round(mhz * 1e6);
  if (cfg.freq === hz) return true;
  await applyConfig({rx_lo_frequency: hz}, true);
  if (label) log("Fc ajustada a " + trimZeros(mhz, 6) + " MHz (" + label + ").");
  return true;
};
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

// Muestreo y ancho de banda (MHz en la interfaz, Hz hacia el AD9361)
function bindMHz(key, num, sld) {
  const send = () => { const v = parseFloat(q(num).value); if (isNaN(v) || v <= 0) { loadConfig(); return; } applyConfig({[key]: Math.round(v * 1e6)}); };
  q(sld).addEventListener("input", () => { q(num).value = trimZeros(Number(q(sld).value), 3); });
  q(sld).addEventListener("change", send);
  q(num).addEventListener("input", () => { const v = parseFloat(q(num).value); if (!isNaN(v)) q(sld).value = v; });
  q(num).addEventListener("keydown", e => { if (e.key === "Enter") e.target.blur(); });
  q(num).addEventListener("change", send);
}
bindMHz("rx_lo_frequency", "rx-fc", "rx-fslider");
bindMHz("sampling_frequency", "rx-sr", "rx-sslider");
bindMHz("rx_rf_bandwidth", "rx-bw", "rx-bslider");

// Integrador
q("rx-integ").addEventListener("change", () => { integM = parseInt(q("rx-integ").value) || 1; resetIntegration(); rangeInit = false; draw(); });
q("rx-integ-reset").addEventListener("click", () => { resetIntegration(); rangeInit = false; log("Integración reiniciada."); });
q("rx-noise-line").addEventListener("change", draw);

// ── Interacción con el espectro y el waterfall ──────────────────────────────
function cursorFreqMHz(x, width) {
  const x0 = PAD.l, x1 = width - PAD.r;
  if (!cfg.rate || x < x0 || x > x1) return null;
  return (cfg.freq - cfg.rate / 2 + (x - x0) / (x1 - x0) * cfg.rate) / 1e6;
}
function showCursorFreq(f) { q("rx-cursor-freq").textContent = f === null ? "—" : trimZeros(f, 4); }
function onMove(e) {
  const r = e.currentTarget.getBoundingClientRect();
  hoverX = e.clientX - r.left;
  showCursorFreq(cursorFreqMHz(hoverX, r.width));
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
  c.addEventListener("mouseleave", () => { hoverX = null; showCursorFreq(null); draw(); });
  c.addEventListener("dblclick", onDblClick);
});
addEventListener("resize", draw);

// Diagnóstico visible: a qué dirección intenta conectarse el servidor y por qué falla.
async function refreshLinkNote() {
  try {
    const response = await fetch("/api/rf/status", {cache: "no-store"});
    if (!response.ok) throw Error("Estado RF no disponible");
    const s = await response.json(), pill = q("signal-pill");
    setSpectrumGeometry(s.center_freq_hz, s.span_hz);
    const label = q("rf-tracking");
    const trackingText = s.tracking ? "En seguimiento: " + s.tracking.name : "Sin seguimiento activo";
    if (label.textContent !== trackingText) label.textContent = trackingText;
    const peak = s.pass_peak;
    q("rf-pass-snr").textContent = peak ? `${peak.snr_max_db.toFixed(1)} dB` : "—";
    q("rf-pass-peak").textContent = peak ? fmtMHz(peak.peak_freq_hz / 1e6) : "—";
    q("rf-pass-center").textContent = peak ? fmtMHz(peak.center_freq_hz / 1e6) : "—";
    q("rf-metrics-status").textContent = s.metrics_error ||
      (peak && !s.tracking ? `Guardado · ${peak.satellite}` : s.tracking && !peak ? "Esperando mediciones en la bajada" : "");
    pill.title = !s.connected ? `Sin conexión con Maia en ${s.server_url}${s.error ? " — " + s.error : ""}. Verifica la IP de la Pluto (--pluto-ip) y el puerto 8000.`
      : !s.streaming ? `Conectado a ${s.server_url}, pero no llegan frames de espectro${s.error ? " — " + s.error : ""}.` : `Maia en ${s.server_url}`;
  } catch (_) { q("rf-tracking").textContent = "Estado de seguimiento no disponible"; }
}
setInterval(refreshLinkNote, 1000);

loadConfig(); connect(); draw(); refreshLinkNote();
})();
