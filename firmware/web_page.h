#pragma once

// The whole web page: HTML, CSS, and JavaScript in one string, served from flash.
// The browser turns the drawing into the 8x8 grid (0-16) the hub expects, the same way the
// scikit-learn digits were made: a 32x32 black-and-white image, ink counted in 4x4 blocks.

static const char WEB_PAGE[] = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>construct hub</title>
<style>
  :root { --ink: #1d1d1f; --muted: #6e6e73; --line: #d2d2d7; --accent: #0a66c2; --bg: #f5f5f7; --card: #fff; }
  * { box-sizing: border-box; }
  body { margin: 0; font-family: -apple-system, system-ui, sans-serif; background: var(--bg); color: var(--ink); }
  main { max-width: 440px; margin: 0 auto; padding: 16px; }
  h1 { font-size: 20px; margin: 4px 0 12px; }
  .card { background: var(--card); border: 1px solid var(--line); border-radius: 12px; padding: 12px; margin-bottom: 12px; }
  .pad { position: relative; }
  canvas#draw { width: 100%; aspect-ratio: 1; border: 1px solid var(--line); border-radius: 8px; touch-action: none; background: #fff; display: block; }
  canvas#preview { position: absolute; right: 8px; bottom: 8px; width: 64px; height: 64px; border: 1px solid var(--line); image-rendering: pixelated; background: #fff; }
  .digits { display: grid; grid-template-columns: repeat(10, 1fr); gap: 4px; margin: 10px 0; }
  button { font: inherit; border: 1px solid var(--line); background: #fff; border-radius: 8px; padding: 10px 0; }
  .digits button.on { background: var(--accent); color: #fff; border-color: var(--accent); }
  .row { display: flex; gap: 8px; }
  .row button { flex: 1; }
  .primary { background: var(--accent); color: #fff; border-color: var(--accent); }
  #result { min-height: 24px; margin-top: 10px; font-size: 18px; }
  .muted { color: var(--muted); font-size: 13px; }
  table { width: 100%; border-collapse: collapse; font-size: 14px; }
  td, th { padding: 4px 6px; text-align: left; border-bottom: 1px solid var(--line); }
  .dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-right: 6px; background: #c7c7cc; }
  .dot.on { background: #34c759; }
</style>
</head>
<body>
<main>
  <h1>Room <span id="board">?</span>: teach me digits</h1>

  <div class="card">
    <div class="pad">
      <canvas id="draw" width="280" height="280"></canvas>
      <canvas id="preview" width="8" height="8"></canvas>
    </div>
    <div class="digits" id="digits"></div>
    <div class="row">
      <button id="clear">Clear</button>
      <button id="teach" class="primary" disabled>Teach: this is a ?</button>
      <button id="guess">Guess</button>
    </div>
    <div id="result"></div>
  </div>

  <div class="card">
    <strong>What this hub knows</strong>
    <div class="muted">Drawings learned per digit, and which room taught them.</div>
    <table id="knowledge"></table>
  </div>

  <div class="card">
    <strong>Mesh</strong>
    <div id="mesh" class="muted">loading...</div>
  </div>
</main>

<script>
const draw = document.getElementById('draw');
const ctx = draw.getContext('2d');
const preview = document.getElementById('preview').getContext('2d');
const result = document.getElementById('result');
const teachButton = document.getElementById('teach');
let label = null;
let drawing = false;
let hasInk = false;

ctx.lineWidth = 18;
ctx.lineCap = 'round';
ctx.lineJoin = 'round';

function point(event) {
  const box = draw.getBoundingClientRect();
  return [(event.clientX - box.left) * draw.width / box.width, (event.clientY - box.top) * draw.height / box.height];
}
draw.addEventListener('pointerdown', e => { drawing = true; hasInk = true; const [x, y] = point(e); ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(x, y); ctx.stroke(); draw.setPointerCapture(e.pointerId); });
draw.addEventListener('pointermove', e => { if (!drawing) return; const [x, y] = point(e); ctx.lineTo(x, y); ctx.stroke(); });
draw.addEventListener('pointerup', () => { drawing = false; showPreview(); });

function clearPad() {
  ctx.clearRect(0, 0, draw.width, draw.height);
  preview.clearRect(0, 0, 8, 8);
  hasInk = false;
  result.textContent = '';
}
document.getElementById('clear').onclick = clearPad;

// Drawing -> 64 numbers 0-16, like the scikit-learn digits.
function toGrid() {
  const ink = ctx.getImageData(0, 0, draw.width, draw.height).data;
  let minX = draw.width, minY = draw.height, maxX = -1, maxY = -1;
  for (let y = 0; y < draw.height; y++) {
    for (let x = 0; x < draw.width; x++) {
      if (ink[(y * draw.width + x) * 4 + 3] > 0) {
        minX = Math.min(minX, x); maxX = Math.max(maxX, x);
        minY = Math.min(minY, y); maxY = Math.max(maxY, y);
      }
    }
  }
  if (maxX < 0) return null;
  // Scale the drawing to fill a 32x32 box (keeping its shape) and center it.
  const w = maxX - minX + 1, h = maxY - minY + 1;
  const scale = 32 / Math.max(w, h);
  const small = document.createElement('canvas');
  small.width = small.height = 32;
  const sctx = small.getContext('2d');
  sctx.drawImage(draw, minX, minY, w, h, (32 - w * scale) / 2, (32 - h * scale) / 2, w * scale, h * scale);
  const bits = sctx.getImageData(0, 0, 32, 32).data;
  const grid = [];
  for (let row = 0; row < 8; row++) {
    for (let col = 0; col < 8; col++) {
      let count = 0;  // black pixels in this 4x4 block: 0-16
      for (let dy = 0; dy < 4; dy++) {
        for (let dx = 0; dx < 4; dx++) {
          if (bits[((row * 4 + dy) * 32 + col * 4 + dx) * 4 + 3] >= 128) count++;
        }
      }
      grid.push(count);
    }
  }
  return grid;
}

function showPreview() {
  const grid = toGrid();
  if (!grid) return;
  const image = preview.createImageData(8, 8);
  grid.forEach((v, i) => { const shade = 255 - Math.round(v * 255 / 16); image.data.set([shade, shade, shade, 255], i * 4); });
  preview.putImageData(image, 0, 0);
}

const digits = document.getElementById('digits');
for (let d = 0; d < 10; d++) {
  const b = document.createElement('button');
  b.textContent = d;
  b.onclick = () => {
    label = d;
    [...digits.children].forEach(c => c.classList.toggle('on', c === b));
    teachButton.textContent = 'Teach: this is a ' + d;
    teachButton.disabled = false;
  };
  digits.appendChild(b);
}

async function post(path, fields) {
  const response = await fetch(path, { method: 'POST', body: new URLSearchParams(fields) });
  return response.json();
}

teachButton.onclick = async () => {
  const grid = toGrid();
  if (!grid || label === null) { result.textContent = 'Draw a digit and pick its number first.'; return; }
  const answer = await post('/teach', { label: label, pixels: grid.join(',') });
  if (answer.ok) { clearPad(); result.textContent = 'Learned it. Thanks!'; refresh(); }
  else result.textContent = 'Error: ' + answer.error;
};

document.getElementById('guess').onclick = async () => {
  const grid = toGrid();
  if (!grid) { result.textContent = 'Draw a digit first.'; return; }
  const answer = await post('/guess', { pixels: grid.join(',') });
  if (!answer.ok) { result.textContent = 'Error: ' + answer.error; return; }
  if (answer.label < 0) { result.textContent = 'I have not learned any digits yet.'; return; }
  result.innerHTML = 'I think it is a <b>' + answer.label + '</b> (' + Math.round(answer.confidence * 100) +
    '% sure)<div class="muted">nearest centroid says ' + answer.centroid + '</div>';
};

const letter = i => String.fromCharCode(65 + i);

async function refresh() {
  let status;
  try { status = await (await fetch('/status')).json(); } catch (e) { return; }
  document.getElementById('board').textContent = status.board;

  let rows = '<tr><th>Digit</th><th>Total</th><th>From rooms</th></tr>';
  status.knowledge.forEach((byRoom, digit) => {
    const total = byRoom.reduce((a, b) => a + b, 0);
    const parts = byRoom.map((n, i) => n ? letter(i) + ' ' + n : '').filter(Boolean).join(', ');
    rows += '<tr><td>' + digit + '</td><td>' + total + '</td><td>' + (parts || '-') + '</td></tr>';
  });
  document.getElementById('knowledge').innerHTML = rows;

  const peers = status.peers.map(p => '<div><span class="dot' + (p.online ? ' on' : '') + '"></span>Room ' + p.board +
    (p.online ? ' online' : ' last seen ' + p.seen_s_ago + ' s ago') + '</div>').join('');
  const versions = status.versions.map((v, i) => v ? letter(i) + ':' + v : '').filter(Boolean).join('  ') || 'none yet';
  document.getElementById('mesh').innerHTML = (peers || '<div>No other rooms heard yet.</div>') +
    '<div style="margin-top:6px">Versions: ' + versions + '</div>' +
    '<div>' + (status.sync.syncing ? 'Syncing a slot now...' : 'Idle') + ' · slots received: ' + status.sync.applied +
    ' · CRC failures: ' + status.sync.crc_failures + '</div>';
}
refresh();
setInterval(refresh, 2000);
</script>
</body>
</html>
)HTML";
