'use strict';
// Loaded before editor.js, whose state these use once the page is running

// ---- Preview ---------------------------------------------------------------

let previewMs = 0, lastFrame = performance.now(), nextSample = 0;
let beatMilli = 0;   // the preview's beat clock, at the strip's tempo; the speed slider doesn't touch it

// The tempo the board hears, or 120 BPM while it hears no beat
function previewBpm() {
  return device && device.beat_found && device.bpm > 0 ? device.bpm : 120;
}
let historyFill = 0;
const scopeData = [];   // [first, middle, last] brightness per history sample

function resetPreview() {
  previewMs = 0;
  nextSample = 0;
  scopeData.length = 0;
  const h = $('history');
  h.getContext('2d').clearRect(0, 0, h.width, h.height);
}

function css([r, g, b]) {
  // Levels are perceptual, close enough to how a screen shows colour
  return `rgb(${r / 257 | 0},${g / 257 | 0},${b / 257 | 0})`;
}

function fitCanvas(canvas) {
  const scale = window.devicePixelRatio || 1;
  const w = Math.round(canvas.clientWidth * scale), h = Math.round(canvas.clientHeight * scale);
  if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; return true; }
  return false;
}

function drawStrip(canvas, frame, glow) {
  fitCanvas(canvas);
  const ctx = canvas.getContext('2d');
  const w = canvas.width, h = canvas.height, n = frame.length;
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, w, h);
  const cell = w / n;
  for (let i = 0; i < n; i++) {
    ctx.fillStyle = css(frame[i]);
    if (glow && cell > 10) {
      ctx.shadowColor = ctx.fillStyle;
      ctx.shadowBlur = cell * 0.8;
      ctx.beginPath();
      ctx.arc((i + 0.5) * cell, h / 2, Math.min(cell * 0.32, h * 0.32), 0, Math.PI * 2);
      ctx.fill();
    } else {
      ctx.fillRect(i * cell + (cell > 4 ? 1 : 0), 0, Math.max(1, cell - (cell > 4 ? 2 : 0)), h);
    }
  }
  ctx.shadowBlur = 0;
}

function addHistoryRow(frame) {
  const canvas = $('history');
  if (fitCanvas(canvas)) scopeData.length = 0;
  const ctx = canvas.getContext('2d');
  const w = canvas.width, rowH = canvas.height / HISTORY_ROWS, n = frame.length;
  ctx.drawImage(canvas, 0, rowH);
  for (let i = 0; i < n; i++) {
    ctx.fillStyle = css(frame[i]);
    ctx.fillRect(Math.floor(i * w / n), 0, Math.ceil(w / n), Math.ceil(rowH));
  }
  const level = c => Math.max(c[0], c[1], c[2]) / 65535;
  scopeData.unshift([level(frame[0]), level(frame[n >> 1]), level(frame[n - 1])]);
  if (scopeData.length > HISTORY_ROWS) scopeData.pop();
}

function drawScope() {
  const canvas = $('scope');
  fitCanvas(canvas);
  const ctx = canvas.getContext('2d');
  const w = canvas.width, h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  ctx.strokeStyle = '#333';
  ctx.lineWidth = 1;
  for (const f of [0.25, 0.5, 0.75]) { ctx.beginPath(); ctx.moveTo(0, h * f); ctx.lineTo(w, h * f); ctx.stroke(); }
  const colours = ['#fbbf24', '#60a5fa', '#f472b6'];
  ctx.lineWidth = 1.5 * (window.devicePixelRatio || 1);
  for (let k = 2; k >= 0; k--) {
    ctx.strokeStyle = colours[k];
    ctx.beginPath();
    scopeData.forEach((d, i) => {
      const x = w - (i / (HISTORY_ROWS - 1)) * w, y = h - 3 - d[k] * (h - 6);
      if (i) ctx.lineTo(x, y); else ctx.moveTo(x, y);
    });
    ctx.stroke();
  }
}

function drawTimeline(p, ms) {
  const canvas = $('timeline');
  fitCanvas(canvas);
  const ctx = canvas.getContext('2d');
  const w = canvas.width, h = canvas.height;
  const total = p.playlist.reduce((t, s) => t + s.ms, 0);
  const shades = ['#3f3f46', '#52525b'];
  ctx.font = `${11 * (window.devicePixelRatio || 1)}px system-ui`;
  ctx.textBaseline = 'middle';
  let x = 0;
  p.playlist.forEach((s, i) => {
    const sw = s.ms / total * w;
    ctx.fillStyle = shades[i % 2];
    ctx.fillRect(x, 0, sw, h);
    ctx.fillStyle = '#e4e4e7';
    ctx.fillText(set.patches[s.patch].name, x + 6, h / 2, sw - 10);
    x += sw;
  });
  const at = (ms % total) / total * w;
  ctx.fillStyle = '#f59e0b';
  ctx.fillRect(at - 1, 0, 3, h);
  const step = Patch.playlist_step(set, selected, ms);
  $('timeline-now').textContent = set.patches[p.playlist[step].patch].name;
}

function drawPaletteBar(p) {
  const canvas = $('palette-bar');
  if (!canvas || !p || p.playlist_count) return;
  fitCanvas(canvas);
  const ctx = canvas.getContext('2d');
  for (let x = 0; x < canvas.width; x++) {
    ctx.fillStyle = css(Patch.palette_colour(p, x / (canvas.width - 1)));
    ctx.fillRect(x, 0, 1, canvas.height);
  }
}

const frame = [];
function tick(now) {
  const dt = Math.min(now - lastFrame, 100);
  lastFrame = now;
  if (set && selected < set.count) {
    const sameShape = set.patches.length === lib.patches.length;
    previewMs += dt * speedMultiplier();
    beatMilli += dt * previewBpm() / 60;
    const ms = Math.floor(previewMs);
    const beat = Math.floor(beatMilli);
    Patch.render(set, selected, ms, beat, 1, frame, ledCount);   // full energy, so beat patches pulse while editing
    frame.length = ledCount;
    drawStrip($('strip'), frame, true);
    while (nextSample <= previewMs) {
      const sample = [];
      // History rows are sampled in effect time; their beat clock is worked back from it
      const behind = (previewMs - nextSample) / speedMultiplier();
      Patch.render(set, selected, Math.floor(nextSample), Math.floor(beatMilli - behind * previewBpm() / 60), 1, sample, ledCount);
      addHistoryRow(sample);
      nextSample += HISTORY_SAMPLE_MS * speedMultiplier();
    }
    drawScope();
    const p = set.patches[selected];
    if (p.playlist_count) drawTimeline(p, ms);
    drawPaletteBar(p);

    // Library cards play at normal speed, all at the same time
    if (sameShape) {
      document.querySelectorAll('#cards canvas, .chip canvas').forEach(c => {
        const i = Number(c.dataset.index);
        const cardFrame = [];
        Patch.render(set, i, Math.floor(now), beat, 1, cardFrame, ledCount);
        drawStrip(c, cardFrame, false);
      });
    }
  }
  requestAnimationFrame(tick);
}
