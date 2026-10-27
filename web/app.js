// meshsync: hands-free controller page.
// The webcam runs MediaPipe here in the page; only 12 numbers per frame go to the hub, over
// USB serial (Web Serial) or to a simulated hub (the same C++ compiled to WebAssembly).
// The hub keeps all calibration. This page stores none of it.

import { createLandmarker, extractFeatures, DRAW_POINTS, BLENDSHAPE_NAMES, LANDMARK_NAMES } from './features.js';

const GESTURES = ['neutral', 'eyebrows up', 'smile'];
const INSTRUCTIONS = ['Relax your face', 'Raise your eyebrows', 'Smile'];
const SAMPLES_PER_CLASS = 45;     // 3 s at 15 Hz
const SEND_INTERVAL_MS = 1000 / 15;
const CLASS_TIMEOUT_MS = 15000;   // give up on a gesture if no face for this long
const REPLY_TIMEOUT_MS = 1500;
const BOOT_WAIT_MS = 3000;        // opening the port resets the board; it says BOOT when ready
const FRESH_FEATURES_MS = 250;    // older features count as "no face"
const SIM_HUBS = 3;

const $ = (id) => document.getElementById(id);
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// ======================================================================================
// Transports: both deliver whole lines to onLine and accept whole lines in write().
// ======================================================================================

class SerialTransport {
  constructor(onLine, onClose) {
    this.onLine = onLine;
    this.onClose = onClose;
    this.kind = 'serial';
  }

  async open(port) {
    if (!('serial' in navigator)) {
      throw new Error('Web Serial is not available. Use Chrome or Edge, and open the page from localhost.');
    }
    this.port = port || await navigator.serial.requestPort();
    await this.port.open({ baudRate: 115200, bufferSize: 8192 });
    try {
      // Neither line asserted = the board runs normally (not held in reset or in boot mode).
      await this.port.setSignals({ dataTerminalReady: false, requestToSend: false });
    } catch (error) {
      console.warn('setSignals failed', error);
    }
    this.writer = this.port.writable.getWriter();
    this.readLoop();
  }

  async readLoop() {
    const decoder = new TextDecoder();
    let buffer = '';
    this.reader = this.port.readable.getReader();
    try {
      while (true) {
        const { value, done } = await this.reader.read();
        if (done) break;
        buffer += decoder.decode(value, { stream: true });
        let end;
        while ((end = buffer.indexOf('\n')) >= 0) {
          const line = buffer.slice(0, end).replace(/\r$/, '');
          buffer = buffer.slice(end + 1);
          if (line) this.onLine(line);
        }
        if (buffer.length > 4096) buffer = '';  // noise without newlines (boot messages at another speed)
      }
    } catch (error) {
      console.warn('serial read ended', error);  // unplugged, or the port was closed
    } finally {
      try { this.reader.releaseLock(); } catch (error) { /* already released */ }
      this.onClose();
    }
  }

  write(line) {
    return this.writer.write(new TextEncoder().encode(line + '\n'));
  }

  async close() {
    try { await this.reader?.cancel(); } catch (error) { /* gone */ }
    try { this.writer?.releaseLock(); } catch (error) { /* gone */ }
    try { await this.port?.close(); } catch (error) { /* gone */ }
  }
}

class SimTransport {
  constructor(onLine, onClose) {
    this.onLine = onLine;
    this.onClose = onClose;
    this.kind = 'sim';
  }

  async open() {
    if (typeof createMeshstate !== 'function') {
      throw new Error('playground/meshstate_wasm.js did not load. Rebuild it with web/playground/build.sh.');
    }
    const m = await createMeshstate();
    this.api = {
      reset: m.cwrap('hub_reset', null, ['number', 'number']),
      attach: m.cwrap('hub_attach', null, ['number']),
      line: m.cwrap('hub_line', null, ['string']),
      step: m.cwrap('hub_step', null, ['number']),
      output: m.cwrap('hub_output', 'string', []),
      count: m.cwrap('hub_count', 'number', []),
      attached: m.cwrap('hub_attached', 'number', []),
      setLink: m.cwrap('hub_set_link', null, ['number', 'number', 'number']),
      link: m.cwrap('hub_link', 'number', ['number', 'number']),
    };
    this.api.reset(SIM_HUBS, Math.floor(Math.random() * 100000));
    this.last = performance.now();
    // Simulated time follows real time (in 5 ms steps), so sync takes as long as on boards.
    this.timer = setInterval(() => {
      const now = performance.now();
      let ms = Math.min(200, now - this.last);
      ms -= ms % 5;
      if (ms >= 5) {
        this.api.step(ms);
        this.last += ms;
      }
      if (now - this.last > 1000) this.last = now;  // tab was in the background
      this.flush();
    }, 20);
    setTimeout(() => this.flush(), 0);
  }

  flush() {
    const text = this.api.output();
    if (!text) return;
    for (const line of text.split('\n')) {
      if (line) this.onLine(line);
    }
  }

  write(line) {
    this.api.line(line);
    setTimeout(() => this.flush(), 0);  // replies arrive asynchronously, like over serial
  }

  async close() {
    clearInterval(this.timer);
    this.onClose();
  }
}

// ======================================================================================
// Hub client: request ids, replies, events.
// ======================================================================================

class Hub {
  constructor() {
    this.transport = null;
    this.nextId = 1;
    this.pending = new Map();
    this.timeouts = 0;
    this.onEvent = () => {};
    this.onLog = () => {};
    this.onClose = () => {};
    this.onTraffic = () => {};  // (direction 'out' | 'in', line): every line, for the packet panel
  }

  async connect(kind, port) {
    await this.disconnect();
    const transport = kind === 'sim'
      ? new SimTransport((line) => this.handleLine(line), () => this.closed(transport))
      : new SerialTransport((line) => this.handleLine(line), () => this.closed(transport));
    this.transport = transport;
    try {
      await transport.open(port);
    } catch (error) {
      this.transport = null;
      throw error;
    }
  }

  closed(transport) {
    if (this.transport !== transport) return;  // an older connection
    this.transport = null;
    for (const [, request] of this.pending) {
      clearTimeout(request.timer);
      request.resolve({ ok: false, code: 'DISCONNECTED', lines: [] });
    }
    this.pending.clear();
    this.onClose();
  }

  async disconnect() {
    const transport = this.transport;
    if (transport) {
      this.transport = null;
      await transport.close();
    }
  }

  handleLine(line) {
    this.onTraffic('in', line);
    if (line.startsWith('#')) {
      this.onLog(line);
      return;
    }
    const match = /^(\d+) (.*)$/.exec(line);
    if (!match) return;  // boot messages from the chip, or noise
    const id = Number(match[1]);
    const rest = match[2];
    if (id === 0) {
      this.onEvent(rest);
      return;
    }
    const request = this.pending.get(id);
    if (!request) return;
    if (rest.startsWith('SLOT ')) {
      request.lines.push(rest.split(' ').slice(1));
      return;
    }
    this.pending.delete(id);
    clearTimeout(request.timer);
    this.timeouts = 0;
    if (rest.startsWith('OK ')) {
      request.resolve({ ok: true, fields: rest.slice(3).split(' '), lines: request.lines });
    } else {
      request.resolve({ ok: false, code: rest.replace(/^ERR /, ''), lines: request.lines });
    }
  }

  request(text) {
    if (!this.transport) return Promise.resolve({ ok: false, code: 'NOT_CONNECTED', lines: [] });
    const id = this.nextId;
    this.nextId = this.nextId >= 65535 ? 1 : this.nextId + 1;
    return new Promise((resolve) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        this.timeouts++;
        resolve({ ok: false, code: 'TIMEOUT', lines: [] });
      }, REPLY_TIMEOUT_MS);
      this.pending.set(id, { resolve, timer, lines: [] });
      const line = `${id} ${text}`;
      this.onTraffic('out', line);
      Promise.resolve(this.transport.write(line)).catch((error) => console.warn('write failed', error));
    });
  }
}

// ======================================================================================
// Page state
// ======================================================================================

const hub = new Hub();
const state = {
  connected: false,      // HELLO answered
  hubLetter: '',
  user: 0,
  epoch: 0,
  lambda: 1.0,
  featureType: 'gest_blend',
  sessions: {},          // payload type -> session id on the current hub
  calibrating: false,
  cancel: false,
  skippedCalibration: false,
  setupOpen: false,      // the person asked to see the setup steps again
  features: null,        // latest from the camera
  featuresAt: 0,
  rtts: [],
  mesh: { self: null, peers: {}, slots: [] },
  deletion: null,        // { user, hub, startedAt, removed, cleared: {letter: seconds} }
  wire: { out: null, reply: null, outAt: 0, window: [] },
  ledUntil: 0,           // mirrors the board's LED flash after a detected gesture
  lastLabel: -1,
  recordDir: null,
  bootTimer: null,
  lastSerialPort: null,
};

const GESTURE_COLORS = ['#7d8c98', '#c98a12', '#0f9a80'];

// The chip in the top bar is short; the line under step 1 can say more.
function setStatus(chip, kind = '', detail = '') {
  $('hub-status').textContent = chip;
  $('hub-status').className = 'chip ' + kind;
  $('connect-status').textContent = detail;
  $('connect-status').className = 'status-line ' + (kind === 'ok' ? '' : kind);
}

function log(text) {
  const list = $('log');
  const item = document.createElement('li');
  item.textContent = `${new Date().toLocaleTimeString()}  ${text}`;
  list.prepend(item);
  while (list.children.length > 60) list.lastChild.remove();
}

function showPrompt(text, kind = '') {
  $('calib-prompt').textContent = text;
  $('calib-prompt').className = 'status-line ' + kind;
}

// Big instruction on top of the camera image during calibration. fraction = progress 0-1.
function showCue(text, sub, fraction) {
  $('cue').hidden = false;
  $('cue-text').textContent = text;
  $('cue-sub').textContent = sub;
  $('cue-fill').style.width = `${Math.round(fraction * 100)}%`;
}

function hideCue() {
  $('cue').hidden = true;
}

function plural(n, word) {
  return `${n} ${word}${n === 1 ? '' : 's'}`;
}

// ======================================================================================
// Connecting
// ======================================================================================

hub.onLog = (line) => log(line);
hub.onClose = () => {
  state.connected = false;
  state.sessions = {};
  setStatus('Hub disconnected', 'bad', 'The hub was unplugged or stopped answering. Plug it back in and press Reconnect.');
  log('hub disconnected');
  render();
};
hub.onEvent = handleEvent;
hub.onTraffic = recordTraffic;

async function connect(kind) {
  state.connected = false;
  state.sessions = {};
  state.mesh = { self: null, peers: {}, slots: [] };
  setStatus('Connecting', 'warn', kind === 'sim' ? 'Starting three simulated hubs…' : 'Choose the hub in the list Chrome shows (usually "CP2102" or "usbserial").');
  try {
    await hub.connect(kind, null);
    if (kind === 'serial') state.lastSerialPort = hub.transport.port;
  } catch (error) {
    const cancelled = error && error.name === 'NotFoundError';  // closed the port chooser
    setStatus('No hub connected', cancelled ? '' : 'bad', cancelled ? '' : (error.message || String(error)));
    log(error.message || String(error));
    render();
    return;
  }
  $('sim-panel').hidden = kind !== 'sim';
  setStatus('Starting', 'warn', 'Waiting for the hub to start…');
  // Opening the port restarts the board. If no BOOT line comes, it may not have restarted.
  clearTimeout(state.bootTimer);
  state.bootTimer = setTimeout(() => handshake(), BOOT_WAIT_MS);
  render();
  renderSim();
}

async function handshake() {
  clearTimeout(state.bootTimer);
  state.sessions = {};
  const hello = await hub.request('HELLO 1');
  if (!hello.ok) {
    setStatus('Hub not answering', 'bad', `The hub did not answer (${hello.code}). Check that it runs the face firmware and that the Arduino serial monitor is closed.`);
    return;
  }
  if (hello.fields[1] !== state.hubLetter) {
    showPrompt('');  // messages about the previous hub
    state.mesh = { self: null, peers: {}, slots: [] };
  }
  state.hubLetter = hello.fields[1];
  state.connected = true;
  setStatus(`Hub ${state.hubLetter} connected`, 'ok', `Connected to hub ${state.hubLetter}.`);
  log(`connected to hub ${state.hubLetter} (protocol ${hello.fields[2]}, ${hello.fields[3]} slots max)`);
  if (state.user) await sendUser();
  await hub.request(`LAMBDA ${Math.round(state.lambda * 1000)}`);
  refreshSlots();
  render();
}

function handleEvent(text) {
  const f = text.split(' ');
  if (f[0] === 'BOOT') {
    log(`hub ${f[1]} started`);
    clearTimeout(state.bootTimer);
    handshake();  // also after an unexpected restart: sessions on the hub are gone
  } else if (f[0] === 'EVENT' && f[1] === 'MESH') {
    state.mesh.self = { digest: f[2], entries: +f[3], slots: +f[4], tombstones: +f[5], syncing: f[6] === '1',
      txPackets: +f[7], txBytes: +f[8], heard: parseInt(f[9] || '0', 16), at: performance.now() };
    pruneOldPeers();
    checkDeletion();
    renderMesh(true);
    renderData();
  } else if (f[0] === 'EVENT' && f[1] === 'PEER') {
    state.mesh.peers[f[2]] = { app: +f[3], online: f[4] === '1', ageMs: +f[5], rssi: +f[6], packets: +f[7],
      bytes: +f[8], hb: +f[9], inv: +f[10], req: +f[11], chunk: +f[12], digest: f[13], entries: +f[14],
      heard: parseInt(f[15] || '0', 16), at: performance.now() };
  } else if (f[0] === 'EVENT' && f[1] === 'SLOT') {
    log(`received calibration ${f[2]} from the mesh`);
    refreshSlotsSoon();
  } else if (f[0] === 'EVENT' && f[1] === 'TOMBSTONE') {
    log(`a deletion arrived: slot ${f[2]}`);
    refreshSlotsSoon();
  } else if (f[0] === 'EVENT' && f[1] === 'USER_DELETED') {
    log(`person ${f[2]}'s data was deleted (counter ${f[3]})`);
    if (+f[2] === state.user) {
      state.sessions = {};
      state.epoch = +f[3];
    }
    refreshSlotsSoon();
  } else if (f[0] === 'ERR') {
    log(`hub: ${text}`);
  }
}

function pruneOldPeers() {
  const now = performance.now();
  for (const [letter, peer] of Object.entries(state.mesh.peers)) {
    if (now - peer.at > 3000) delete state.mesh.peers[letter];  // the hub stopped reporting it
  }
}

// ======================================================================================
// User, sessions, calibration
// ======================================================================================

async function sendUser() {
  const r = await hub.request(`USER ${state.user}`);
  if (r.ok) {
    state.epoch = +r.fields[2];
    state.sessions = {};
    log(`person ${state.user} at hub ${state.hubLetter}`);
  } else {
    log(`USER failed: ${r.code}`);
  }
  refreshSlotsSoon();
  render();
}

async function ensureSessions() {
  for (const type of ['gest_blend', 'gest_lmk']) {
    if (state.sessions[type]) continue;
    const r = await hub.request(`SESSION ${type}`);
    if (!r.ok) throw new Error(sessionError(r.code));
    state.sessions[type] = +r.fields[1];
  }
}

function sessionError(code) {
  const reasons = {
    STORE_FULL: 'the mesh is full (64 calibration slots). Delete someone\'s data first.',
    TOO_MANY_OPEN: 'the hub already has two calibrations open. Reconnect the hub.',
    NO_USER: 'choose your number first.',
  };
  return `Can't start a calibration: ${reasons[code] || code}`;
}

// One frame of calibration: the same frame goes to both feature types' sessions.
async function addSample(label, features) {
  const send = async (type, values) => {
    let r = await hub.request(`ADD ${state.sessions[type]} ${label} ${values.join(' ')}`);
    if (!r.ok && r.code === 'UNKNOWN_SESSION') {
      delete state.sessions[type];  // the hub restarted, or this person's data was just deleted
      await ensureSessions();
      r = await hub.request(`ADD ${state.sessions[type]} ${label} ${values.join(' ')}`);
    }
    return r;
  };
  const [blend, landmark] = await Promise.all([send('gest_blend', features.blend), send('gest_lmk', features.landmark)]);
  return blend.ok ? landmark : blend;
}

function freshFeatures() {
  return state.features && performance.now() - state.featuresAt < FRESH_FEATURES_MS ? state.features : null;
}

async function calibrate(classes) {
  if (!state.connected) return showPrompt('Connect a hub first (step 1).', 'bad');
  if (!state.user) return showPrompt('Choose your number first (step 2).', 'bad');
  if (!landmarker && !freshFeatures()) return showPrompt('Start the camera first.', 'bad');
  state.calibrating = true;
  state.cancel = false;
  state.setupOpen = true;  // keep step 3 visible while it runs
  render();
  const rows = [];
  try {
    await ensureSessions();
    for (const label of classes) {
      for (let s = 3; s > 0 && !state.cancel; s--) {
        showCue(INSTRUCTIONS[label], `Starts in ${s}`, 0);
        showPrompt(`Next: ${INSTRUCTIONS[label].toLowerCase()}.`);
        await sleep(1000);
      }
      let count = 0;
      const started = performance.now();
      let next = performance.now();
      while (count < SAMPLES_PER_CLASS && !state.cancel) {
        next += SEND_INTERVAL_MS;
        await sleep(Math.max(0, next - performance.now()));
        if (performance.now() - started > CLASS_TIMEOUT_MS) {
          throw new Error(`No face for too long while recording "${GESTURES[label]}".`);
        }
        const features = freshFeatures();
        if (!features) {
          showCue(INSTRUCTIONS[label], 'Can\'t see your face. Look at the camera.', count / SAMPLES_PER_CLASS);
          continue;
        }
        const r = await addSample(label, features);
        if (!r.ok) throw new Error(`The hub refused a sample: ${r.code}`);
        count++;
        showCue(INSTRUCTIONS[label], 'Hold it', count / SAMPLES_PER_CLASS);
        showPrompt(`Recording: ${INSTRUCTIONS[label].toLowerCase()}.`);
        rows.push([Date.now(), state.user, state.hubLetter, 'gesture', label, '', '', ...features.blend, ...features.landmark]);
      }
    }
    if (state.cancel) {
      showPrompt('Stopped. The samples recorded so far are kept.', 'warn');
    } else {
      showPrompt(`Done. Hub ${state.hubLetter} has your calibration and passes it to the other hubs within a few seconds.`, 'ok');
      state.setupOpen = false;
    }
  } catch (error) {
    showPrompt(error.message, 'bad');
  } finally {
    state.calibrating = false;
    hideCue();
    render();
    refreshSlotsSoon();
    if (rows.length && state.recordDir) await saveRecording(rows);
  }
}

// ======================================================================================
// Live prediction (15 Hz, one request at a time)
// ======================================================================================

let predicting = false;
async function predictLoop() {
  while (true) {
    const started = performance.now();
    const features = freshFeatures();
    if (state.connected && !state.calibrating && features && !predicting) {
      predicting = true;
      const values = state.featureType === 'gest_blend' ? features.blend : features.landmark;
      const r = await hub.request(`PREDICT ${state.featureType} ${values.join(' ')}`);
      predicting = false;
      if (r.ok) {
        recordRtt(performance.now() - started);
        showPrediction(r.fields);
      } else {
        showNoPrediction(r.code);
      }
    } else if (state.calibrating) {
      showNoPrediction('CALIBRATING');
    } else if (!features) {
      showNoPrediction(landmarker ? 'NO_FACE' : 'NO_CAMERA');
    }
    renderValues(features);
    await sleep(Math.max(0, SEND_INTERVAL_MS - (performance.now() - started)));
  }
}

function showPrediction(fields) {
  // PREDICT <type> <label> <p0> <p1> <p2> <own> <others>
  const label = +fields[2];
  const p = [+fields[3], +fields[4], +fields[5]];
  $('pred-label').textContent = GESTURES[label];
  $('pred-label').className = 'word g' + label;
  $('pred-conf').textContent = `${Math.round(p[label] / 10)}% sure`;
  for (let c = 0; c < 3; c++) {
    $('bar-' + c).style.width = `${p[c] / 10}%`;
    $('barv-' + c).textContent = `${Math.round(p[c] / 10)}%`;
    $('name-' + c).className = c === label ? 'top' : '';
  }
  // Same rule as the board's LED: flash when a confident non-neutral gesture starts.
  if (label !== state.lastLabel && label !== 0 && p[label] >= 800) state.ledUntil = performance.now() + 150;
  state.lastLabel = label;
  const own = +fields[6];
  const others = +fields[7];
  $('pred-samples').textContent = others === 0 ? `Built from your ${own} samples.`
    : own === 0 ? `Built from ${others} samples of other people. None of yours yet.`
      : `Built from your ${own} samples and ${others} from other people.`;
}

function showNoPrediction(code) {
  const messages = {
    CALIBRATING: 'Paused while you calibrate.',
    NOT_READY: 'Nothing to go on yet. Calibrate, or let other people\'s calibration count.',
    NO_FACE: 'Can\'t see a face.',
    NO_CAMERA: 'Start the camera to see your gesture here.',
    TIMEOUT: 'The hub did not answer in time.',
    NOT_CONNECTED: 'Connect a hub to start.',
    DISCONNECTED: 'Connect a hub to start.',
  };
  $('pred-label').textContent = code === 'CALIBRATING' ? 'Paused' : 'No gesture yet';
  $('pred-label').className = 'word idle';
  $('pred-conf').textContent = '';
  $('pred-samples').textContent = messages[code] || `The hub answered ${code}.`;
  state.lastLabel = -1;
  for (let c = 0; c < 3; c++) {
    $('bar-' + c).style.width = '0%';
    $('barv-' + c).textContent = '';
    $('name-' + c).className = '';
  }
}

function recordRtt(ms) {
  state.rtts.push(ms);
  if (state.rtts.length > 300) state.rtts.shift();
  const sorted = [...state.rtts].sort((a, b) => a - b);
  const pick = (q) => sorted[Math.min(sorted.length - 1, Math.floor(q * sorted.length))];
  $('rtt').textContent = `Page to hub and back, last ${sorted.length} predictions: typical ${pick(0.5).toFixed(1)} ms, 95% under ${pick(0.95).toFixed(1)} ms, slowest ${sorted[sorted.length - 1].toFixed(1)} ms.`;
}

// ======================================================================================
// What the hub receives: the exact lines on the wire
// ======================================================================================

const encoder = new TextEncoder();
const bytesOf = (line) => encoder.encode(line + '\n').length;

function recordTraffic(direction, line) {
  const now = performance.now();
  if (direction === 'out') {
    state.wire.window.push({ at: now, bytes: bytesOf(line) });
    // Show the data-carrying requests; HELLO, STATUS and the like are bookkeeping.
    if (/^\d+ (PREDICT|ADD) /.test(line)) {
      state.wire.out = line;
      state.wire.outAt = now;
      state.wire.reply = null;
      state.wire.outId = line.split(' ')[0];
    }
  } else if (state.wire.outId && line.startsWith(state.wire.outId + ' ')) {
    state.wire.reply = line;
  }
}

function escapeHtml(text) {
  return text.replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
}

let lastWireRender = 0;
function renderWire() {
  const now = performance.now();
  if (now - lastWireRender < 150) return;  // readable, not flickering
  lastWireRender = now;
  state.wire.window = state.wire.window.filter((w) => now - w.at < 1000);
  const perSecond = state.wire.window.reduce((n, w) => n + w.bytes, 0);
  $('wire-rate').textContent = state.wire.window.length ? `${state.wire.window.length} messages, ${perSecond} bytes in the last second` : '';
  const out = state.wire.out;
  if (!out) return;
  const reply = state.wire.reply;
  $('wire').innerHTML = `<span class="dir">to hub ${escapeHtml(state.hubLetter)} →</span> ${escapeHtml(out)}`
    + (reply ? `\n<span class="dir">← reply</span> <span class="ok">${escapeHtml(reply)}</span>` : '');
  $('wire-meta').innerHTML = `<span>This message: <b>${bytesOf(out)} bytes</b></span>`
    + `<span>One camera frame: <b>${(640 * 480 * 3).toLocaleString('en-US')} bytes</b>, never sent</span>`;
}

let lastValuesRender = 0;
function renderValues(features) {
  const now = performance.now();
  if (now - lastValuesRender < 150) return;
  lastValuesRender = now;
  const names = state.featureType === 'gest_blend' ? BLENDSHAPE_NAMES : LANDMARK_NAMES;
  const values = features ? (state.featureType === 'gest_blend' ? features.blend : features.landmark) : null;
  const el = $('values');
  if (el.children.length !== names.length || el.dataset.type !== state.featureType) {
    el.dataset.type = state.featureType;
    el.innerHTML = names.map((n) => `<div><span title="${n}">${n}</span><b></b></div>`).join('');
  }
  for (let i = 0; i < names.length; i++) {
    el.children[i].querySelector('b').textContent = values ? values[i] : '–';
  }
}

// ======================================================================================
// Mesh: every hub this hub hears, plus the hubs they hear (from their heartbeats).
// Pulses show the packets counted in the last second, travelling along each sender's links.
// ======================================================================================

const SVG_NS = 'http://www.w3.org/2000/svg';
const reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
const pulses = [];   // { from, to, start, kind }
let layout = {};     // letter -> { x, y }
let linkCurves = {}; // "AB" -> control point of the arc, or null for a straight link

function svg(tag, attributes, text) {
  const el = document.createElementNS(SVG_NS, tag);
  for (const [key, value] of Object.entries(attributes)) el.setAttribute(key, value);
  if (text !== undefined) el.textContent = text;
  return el;
}

const letterOf = (bit) => String.fromCharCode(65 + bit);
const bitsOf = (mask) => [...Array(8).keys()].filter((b) => mask & (1 << b)).map(letterOf);

// Links as "AB" strings (sorted letters), and every hub that appears anywhere.
function meshGraph() {
  const self = state.mesh.self;
  const links = new Set();
  const hubs = new Set([state.hubLetter]);
  const add = (a, b) => { if (a !== b) { links.add([a, b].sort().join('')); hubs.add(a); hubs.add(b); } };
  for (const letter of bitsOf(self.heard)) add(state.hubLetter, letter);
  for (const [letter, p] of Object.entries(state.mesh.peers)) {
    if (p.app !== 2) continue;
    hubs.add(letter);
    for (const other of bitsOf(p.heard)) add(letter, other);
  }
  return { links, hubs: [...hubs].sort() };
}

function hubState(letter) {
  if (letter === state.hubLetter) return 'self';
  const p = state.mesh.peers[letter];
  if (!p) return 'far';         // known only from another hub's heartbeat
  if (p.app !== 2) return 'other';
  if (!p.online) return 'far';
  return p.digest === state.mesh.self.digest ? 'same' : 'behind';
}

function mySamplesHere() {
  return state.mesh.slots.filter((s) => state.user && s.user === state.user && s.type === 'gest_blend')
    .reduce((n, s) => n + s.samples, 0);
}

function renderMesh(spawnPulses) {
  const self = state.mesh.self;
  if (!self || !state.hubLetter) return;
  $('mesh-key').hidden = false;
  const { links, hubs } = meshGraph();
  const others = hubs.filter((h) => h !== state.hubLetter);
  const same = others.filter((h) => hubState(h) === 'same');
  $('self-text').textContent = others.length === 0
    ? `Hub ${state.hubLetter} doesn't hear any other hub yet.`
    : `Hub ${state.hubLetter} holds ${plural(self.slots, 'calibration slot')}. ${same.length ? `${same.join(' and ')} ${same.length === 1 ? 'has' : 'have'} exactly the same data.` : ''}`;
  $('mesh-note').textContent = self.syncing ? 'syncing now' : '';

  // Up to 4 hubs sit in one row in letter order; links that skip a hub arc over it.
  // More hubs go on a ring.
  const W = 520, row = hubs.length <= 4;
  // Room above the row only for arcs, sized by the longest skip.
  let longestSkip = 1;
  if (row) for (const pair of links) longestSkip = Math.max(longestSkip, Math.abs(hubs.indexOf(pair[0]) - hubs.indexOf(pair[1])));
  const rowY = longestSkip > 1 ? 30 + 25 * longestSkip + 20 : 40;
  $('mesh-svg').setAttribute('viewBox', `0 0 ${W} ${row ? rowY + 82 : 260}`);
  layout = {};
  hubs.forEach((letter, i) => {
    if (row) {
      const gap = hubs.length === 1 ? 0 : Math.min(190, (W - 140) / (hubs.length - 1));
      layout[letter] = { x: W / 2 + (i - (hubs.length - 1) / 2) * gap, y: rowY };
    } else {
      const angle = Math.PI + (2 * Math.PI * i) / hubs.length;
      layout[letter] = { x: W / 2 + 190 * Math.cos(angle), y: 120 + 95 * Math.sin(angle) };
    }
  });

  const linkLayer = $('mesh-links');
  linkLayer.replaceChildren();
  linkCurves = {};
  for (const pair of links) {
    const a = layout[pair[0]], b = layout[pair[1]];
    const skip = row ? Math.abs(hubs.indexOf(pair[0]) - hubs.indexOf(pair[1])) : 1;
    // Control point of a curve above the row (straight line for neighbours).
    const c = skip > 1 ? { x: (a.x + b.x) / 2, y: a.y - 2 * (25 * skip) } : null;  // arc peaks 25*skip above
    linkCurves[pair] = c;
    const touchesSelf = pair.includes(state.hubLetter);
    const peer = state.mesh.peers[touchesSelf ? pair.replace(state.hubLetter, '') : ''];
    const width = peer && peer.rssi ? Math.max(1.5, Math.min(6, (peer.rssi + 100) / 10)) : 2.5;
    const d = c ? `M${a.x},${a.y} Q${c.x},${c.y} ${b.x},${b.y}` : `M${a.x},${a.y} L${b.x},${b.y}`;
    linkLayer.append(svg('path', { d, fill: 'none', stroke: '#b9c4cc', 'stroke-width': width, 'stroke-linecap': 'round' }));
    if (peer && peer.rssi) {
      const mid = c ? { x: c.x, y: (a.y + 2 * c.y + b.y) / 4 } : { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 };
      linkLayer.append(svg('text', { x: mid.x, y: mid.y - 9, 'text-anchor': 'middle', fill: '#5d6c78', style: 'font-size:11px' }, `${peer.rssi} dBm`));
    }
  }

  const nodeLayer = $('mesh-nodes');
  nodeLayer.replaceChildren();
  const mine = mySamplesHere();
  for (const letter of hubs) {
    const { x, y } = layout[letter];
    const kind = hubState(letter);
    const look = {
      self: { fill: '#15222c', stroke: '#15222c', text: '#fff', dash: '', sub: 'this laptop' },
      same: { fill: '#2f5bea', stroke: '#2f5bea', text: '#fff', dash: '', sub: 'same data' },
      behind: { fill: '#fff', stroke: '#c98a12', text: '#15222c', dash: '', sub: 'catching up' },
      far: { fill: '#fff', stroke: '#94a1ab', text: '#5d6c78', dash: '4 3', sub: 'out of range' },
      other: { fill: '#fff', stroke: '#94a1ab', text: '#5d6c78', dash: '4 3', sub: 'other app' },
    }[kind];
    nodeLayer.append(svg('circle', { cx: x, cy: y, r: 26, fill: look.fill, stroke: look.stroke, 'stroke-width': kind === 'behind' ? 3 : 2, 'stroke-dasharray': look.dash }));
    nodeLayer.append(svg('text', { x, y: y + 6, 'text-anchor': 'middle', fill: look.text, style: 'font-size:17px;font-weight:700' }, letter));
    nodeLayer.append(svg('text', { x, y: y + 46, 'text-anchor': 'middle', fill: '#5d6c78', style: 'font-size:12px' }, look.sub));
    // What this hub holds of the current person (only knowable for us, or hubs with our exact data).
    let badge = '';
    const d = state.deletion;
    if (d && performance.now() - d.startedAt < 20000) {
      badge = d.cleared[letter] !== undefined ? 'deleted ✓' : kind === 'far' ? 'can\'t confirm from here' : 'deleting…';
    } else if (state.user && (kind === 'self' || kind === 'same')) {
      badge = mine ? `your ${mine} samples` : '';
    }
    if (badge) {
      const color = badge.startsWith('deleted') ? '#1d8a52' : badge.startsWith('your') ? '#2f5bea' : '#8a5d00';
      nodeLayer.append(svg('text', { x, y: y + 62, 'text-anchor': 'middle', fill: color, style: 'font-size:12px;font-weight:600' }, badge));
    }
    if (kind === 'self') {
      // The board's blue LED: on while syncing, and a short flash after a detected gesture.
      nodeLayer.append(svg('circle', { id: 'led', cx: x + 20, cy: y - 20, r: 6, fill: '#c7d0d6', stroke: '#fff', 'stroke-width': 2 }));
    }
  }

  if (spawnPulses && !reduceMotion) spawnTraffic(links);
}

function spawnTraffic(links) {
  const now = performance.now();
  const neighbours = (letter) => [...links].filter((pair) => pair.includes(letter)).map((pair) => pair.replace(letter, ''));
  const send = (from, count, kind) => {
    for (const to of neighbours(from)) {
      for (let i = 0; i < Math.min(count, 6); i++) pulses.push({ from, to, start: now + (i * 1000) / Math.min(count, 6) + Math.random() * 80, kind });
    }
  };
  for (const [letter, p] of Object.entries(state.mesh.peers)) {
    if (!p.online || p.app !== 2) continue;
    send(letter, p.hb, 'beat');
    send(letter, p.inv + p.req + p.chunk, 'data');
  }
  const self = state.mesh.self;
  send(state.hubLetter, Math.min(1, self.txPackets), 'beat');
  send(state.hubLetter, Math.max(0, self.txPackets - 1), 'data');
  if (pulses.length > 400) pulses.splice(0, pulses.length - 400);
}

const PULSE_MS = 650;
function animate() {
  const now = performance.now();
  const layer = $('mesh-pulses');
  layer.replaceChildren();
  for (let i = pulses.length - 1; i >= 0; i--) {
    const p = pulses[i];
    const t = (now - p.start) / PULSE_MS;
    if (t > 1) { pulses.splice(i, 1); continue; }
    if (t < 0 || !layout[p.from] || !layout[p.to]) continue;
    const a = layout[p.from], b = layout[p.to];
    const c = linkCurves[[p.from, p.to].sort().join('')];
    // Point on the straight link, or on the arc (quadratic curve) for links that skip a hub.
    const x = c ? (1 - t) * (1 - t) * a.x + 2 * (1 - t) * t * c.x + t * t * b.x : a.x + (b.x - a.x) * t;
    const y = c ? (1 - t) * (1 - t) * a.y + 2 * (1 - t) * t * c.y + t * t * b.y : a.y + (b.y - a.y) * t;
    layer.append(svg('circle', { cx: x, cy: y, r: p.kind === 'data' ? 4.5 : 3, fill: p.kind === 'data' ? '#2f5bea' : '#94a1ab' }));
  }
  const led = document.getElementById('led');
  if (led) {
    const on = (state.mesh.self && state.mesh.self.syncing) || now < state.ledUntil;
    led.setAttribute('fill', on ? '#3d7bff' : '#c7d0d6');
  }
  renderWire();
  requestAnimationFrame(animate);
}

// ======================================================================================
// Your data: where it is, and deletion you can watch
// ======================================================================================

let slotsTimer = null;
function refreshSlotsSoon() {
  clearTimeout(slotsTimer);
  slotsTimer = setTimeout(refreshSlots, 400);
}

async function refreshSlots() {
  if (!state.connected) return;
  const r = await hub.request('SLOTS');
  if (!r.ok) return;
  // SLOT <key> <owner> <type> <session> <user> <samples>
  state.mesh.slots = r.lines.map((f) => ({ key: f[0], owner: f[1], type: f[2], session: +f[3], user: +f[4], samples: +f[5] }));
  render();
}
setInterval(refreshSlots, 3000);

async function deleteMyData() {
  $('delete-confirm').hidden = true;
  const user = state.user;
  const r = await hub.request(`DELETE_USER ${user}`);
  if (!r.ok) {
    $('delete-status').textContent = `Delete failed: ${r.code}`;
    $('delete-status').className = 'status-line bad';
    return;
  }
  state.sessions = {};
  state.epoch = +r.fields[2];
  state.skippedCalibration = true;  // keep the setup folded: there's simply nothing of theirs left
  state.deletion = { user, hub: state.hubLetter, startedAt: performance.now(), removed: +r.fields[1], cleared: { [state.hubLetter]: 0 } };
  log(`deleted person ${user}'s data (counter now ${r.fields[2]})`);
  checkDeletion();
  refreshSlotsSoon();
  renderData();
}

// A hub with exactly our state (same digest) has the deletion too.
function checkDeletion() {
  const d = state.deletion;
  if (!d || !state.mesh.self) return;
  const seconds = (performance.now() - d.startedAt) / 1000;
  for (const [letter, p] of Object.entries(state.mesh.peers)) {
    if (p.app === 2 && p.online && p.digest === state.mesh.self.digest && d.cleared[letter] === undefined) {
      d.cleared[letter] = seconds;
      d.justCleared = letter;
    }
  }
  const { hubs } = meshGraph();
  const inRange = hubs.filter((h) => hubState(h) !== 'far' && hubState(h) !== 'other');
  const waiting = inRange.filter((h) => d.cleared[h] === undefined);
  const hidden = hubs.length - inRange.length;
  $('delete-status').className = 'status-line ' + (waiting.length ? 'warn' : 'ok');
  $('delete-status').textContent = waiting.length
    ? `Deleted on hub ${d.hub}. Waiting for ${waiting.join(', ')}.`
    : `Person ${d.user}'s data is gone from all ${inRange.length} hubs in range${hidden ? `; ${hidden} more can't be checked from here` : ''}.`;
}

function renderData() {
  const d = state.deletion;
  const mine = mySamplesHere();
  $('data-title').textContent = state.user ? `Person ${state.user} on the mesh` : 'Your data on the mesh';
  $('delete-start').disabled = !(state.connected && state.user > 0) || state.calibrating;
  $('delete-start').textContent = state.user ? `Delete person ${state.user} from every hub` : 'Delete my data from every hub';
  if (!state.connected || !state.mesh.self) {
    $('data-lead').textContent = state.user ? 'Connect a hub to see where your calibration is.' : 'Choose your number to see where your calibration is.';
    $('hubs').innerHTML = '';
    return;
  }
  const { hubs } = meshGraph();
  $('data-lead').textContent = !state.user ? 'Choose your number to see where your calibration is.'
    : d && d.user === state.user ? 'Each hub confirms the deletion once it has the same state as this one.'
      : mine ? 'Every hub with the same data as this one holds a full copy.' : 'No hub in sight holds any of your calibration.';
  if (!state.user) {
    $('hubs').innerHTML = '';
    return;
  }
  $('hubs').innerHTML = hubs.map((letter) => {
    const kind = hubState(letter);
    let what = '', status = '', cls = '';
    if (d && d.user === state.user) {
      const t = d.cleared[letter];
      what = t === undefined
        ? (kind === 'far' ? `Out of hub ${state.hubLetter}'s range, so it can't be checked from here. It gets the deletion through the hubs between, or when it comes back.` : 'Receiving the deletion…')
        : 'Your calibration is removed.';
      status = t === undefined ? (kind === 'far' ? 'not visible' : 'deleting') : `deleted ✓ ${t ? `${t.toFixed(1)} s` : ''}`;
      cls = t === undefined ? (kind === 'far' ? '' : 'warn') : 'ok';
    } else if (kind === 'self') {
      what = mine ? `Holds your ${mine} samples.` : 'Holds none of your samples.';
      status = 'plugged in';
    } else if (kind === 'same') {
      what = mine ? `Holds the same ${mine} samples.` : 'Same data as this hub.';
      status = mine ? 'has a copy' : 'in sync';
      cls = 'ok';
    } else if (kind === 'behind') {
      what = 'Still receiving the latest changes.';
      status = 'catching up';
      cls = 'warn';
    } else {
      what = 'Out of this hub\'s radio range. Gets your data through the hubs between.';
      status = 'out of range';
    }
    const flash = d && d.justCleared === letter ? ' class="flash"' : '';
    return `<li${flash}><span class="tag ${kind === 'self' ? 'self' : ''}">${letter}</span><span class="what">${what}</span><span class="state ${cls}">${status}</span></li>`;
  }).join('');
  if (d) d.justCleared = null;
}

function renderSlots() {
  // One row per person and hub (the two feature kinds hold the same frames, so count one).
  const rows = state.mesh.slots.filter((s) => s.type === 'gest_blend' || s.type === 'gaze');
  const isMine = (s) => state.user && s.user === state.user;
  const people = new Set(rows.map((s) => s.user)).size;
  $('everything-title').textContent = `Everyone's calibration on hub ${state.hubLetter || ''} (${people === 0 ? 'nobody yet' : people === 1 ? '1 person' : `${people} people`})`;
  $('slots').innerHTML = rows.length
    ? rows.map((s) => `<tr class="${isMine(s) ? 'mine' : ''}"><td>${s.user}${isMine(s) ? ' (you)' : ''}</td><td>hub ${s.owner}</td><td class="num">${s.samples}</td></tr>`).join('')
    : '<tr class="empty-row"><td colspan="3">Nothing yet.</td></tr>';
}

// ======================================================================================
// Simulated mesh controls
// ======================================================================================

function renderSim() {
  const sim = hub.transport && hub.transport.kind === 'sim' ? hub.transport.api : null;
  if (!sim) return;
  const count = sim.count();
  const attached = sim.attached();
  const letters = [...Array(count).keys()].map(letterOf);
  $('sim-attach').innerHTML = letters.map((l, i) => `<button data-hub="${i}" class="btn small ${i === attached ? 'on' : ''}">${l}</button>`).join('');
  const pairs = [];
  for (let a = 0; a < count; a++) for (let b = a + 1; b < count; b++) pairs.push([a, b]);
  $('sim-links').innerHTML = pairs.map(([a, b]) => `<button data-a="${a}" data-b="${b}" class="btn small" aria-pressed="${sim.link(a, b) ? 'true' : 'false'}">${letters[a]}–${letters[b]}</button>`).join('');
}

$('sim-attach').addEventListener('click', (event) => {
  const button = event.target.closest('button');
  if (!button || state.calibrating) return;
  const target = +button.dataset.hub;
  log(`moving the cable to hub ${letterOf(target)}`);
  state.connected = false;
  hub.transport.api.attach(target);  // answers with BOOT, which runs the handshake
  renderSim();
});
$('sim-links').addEventListener('click', (event) => {
  const button = event.target.closest('button');
  if (!button) return;
  const api = hub.transport.api;
  const a = +button.dataset.a;
  const b = +button.dataset.b;
  api.setLink(a, b, api.link(a, b) ? 0 : 1);
  renderSim();
});

// ======================================================================================
// Recorder: CSV of calibration features (numbers only), after explicit consent.
// ======================================================================================

async function chooseRecordFolder() {
  if (!('showDirectoryPicker' in window)) {
    $('record-status').textContent = 'This browser cannot write to a folder. Use Chrome or Edge.';
    return;
  }
  try {
    state.recordDir = await window.showDirectoryPicker({ mode: 'readwrite' });
    $('record-status').textContent = `Saving each calibration to folder "${state.recordDir.name}".`;
  } catch (error) {
    $('record-status').textContent = 'No folder chosen; nothing will be saved.';
  }
}

async function saveRecording(rows) {
  const header = ['time_ms', 'user_id', 'hub', 'kind', 'label', 'target_x', 'target_y',
    ...BLENDSHAPE_NAMES.map((n) => 'b_' + n), ...LANDMARK_NAMES.map((n) => 'l_' + n)];
  const stamp = new Date().toISOString().replace(/[-:]/g, '').replace(/\..*/, '').replace('T', '-');
  const name = `calib-${stamp}-hub${state.hubLetter}-user${state.user}.csv`;
  try {
    const file = await state.recordDir.getFileHandle(name, { create: true });
    const writable = await file.createWritable();
    await writable.write([header, ...rows].map((r) => r.join(',')).join('\n') + '\n');
    await writable.close();
    $('record-status').textContent = `Saved ${rows.length} rows to ${state.recordDir.name}/${name}`;
  } catch (error) {
    $('record-status').textContent = `Could not save the recording: ${error.message}`;
  }
}

// ======================================================================================
// Camera + MediaPipe
// ======================================================================================

let landmarker = null;
let lastVideoTime = -1;

async function startCamera() {
  if (!landmarker && !document.createElement('canvas').getContext('webgl2')) {
    // MediaPipe converts every frame with WebGL, even on its CPU path.
    $('camera-status').textContent = 'This browser has WebGL turned off, and MediaPipe needs it. In Chrome: Settings → System → "Use graphics acceleration when available", then restart Chrome. chrome://gpu shows the status.';
    return;
  }
  const video = $('video');
  $('camera-status').textContent = 'Starting the camera…';
  try {
    const stream = await navigator.mediaDevices.getUserMedia({ video: { width: 640, height: 480, facingMode: 'user' }, audio: false });
    video.srcObject = stream;
    await video.play();
  } catch (error) {
    const help = {
      NotAllowedError: 'Camera permission was denied. Click the camera icon in the address bar, allow the camera for this page, then press "Start camera" again.',
      NotFoundError: 'No camera found. Plug one in and press "Start camera" again.',
      NotReadableError: 'The camera is in use by another app. Close it and try again.',
    };
    $('camera-status').textContent = help[error.name] || `Camera error: ${error.message}`;
    return;
  }
  if (!landmarker) {
    $('camera-status').textContent = 'Loading the face model from this laptop…';
    try {
      landmarker = await createLandmarker();
    } catch (error) {
      $('camera-status').textContent = `Could not load MediaPipe: ${error.message}`;
      return;
    }
  }
  $('camera-empty').hidden = true;
  $('face-status').hidden = false;
  requestAnimationFrame(frameLoop);
}

function frameLoop() {
  const video = $('video');
  if (video.readyState >= 2 && video.currentTime !== lastVideoTime) {
    lastVideoTime = video.currentTime;
    const result = landmarker.detectForVideo(video, performance.now());
    const features = extractFeatures(result, video.videoWidth, video.videoHeight);
    if (features) {
      state.features = features;
      state.featuresAt = performance.now();
    }
    drawOverlay(features, video.videoWidth, video.videoHeight);
    $('face-status').textContent = features ? 'Face found' : 'No face';
    $('face-status').className = 'chip face ' + (features ? 'ok' : 'warn');
  }
  requestAnimationFrame(frameLoop);
}

function drawOverlay(features, width, height) {
  const canvas = $('overlay');
  if (canvas.width !== width) {
    canvas.width = width;
    canvas.height = height;
  }
  const ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, width, height);
  if (!features) return;
  ctx.fillStyle = '#8fb0ff';
  for (const i of DRAW_POINTS) {
    const p = features.points[i];
    ctx.beginPath();
    ctx.arc(p.x * width, p.y * height, 2.5, 0, 2 * Math.PI);
    ctx.fill();
  }
}

// ======================================================================================
// Setup steps and controls
// ======================================================================================

// Steps 1-3 light up in order. Once all are done, they fold into one line of checkmarks.
function render() {
  const calibrated = mySamplesHere() > 0;
  const done = [state.connected, state.connected && state.user > 0, calibrated || state.skippedCalibration];
  const next = done.indexOf(false);
  ['step-connect', 'step-you', 'step-calib'].forEach((id, i) => {
    $(id).className = 'step' + (done[i] ? ' done' : i === next ? ' current' : '');
  });
  if (state.calibrating) $('step-calib').className = 'step current';
  const folded = next === -1 && !state.setupOpen && !state.calibrating;
  $('setup').hidden = folded;
  $('setup-summary').hidden = !folded;
  $('sum-hub').textContent = `Hub ${state.hubLetter}`;
  $('sum-person').textContent = `Person ${state.user}`;
  $('sum-calib').textContent = calibrated ? `Calibrated, ${mySamplesHere()} samples`
    : state.deletion && state.deletion.user === state.user ? 'Data deleted' : 'Using other people\'s calibration';

  const ready = state.connected && state.user > 0 && !state.calibrating;
  for (const id of ['calib-all', 'calib-0', 'calib-1', 'calib-2', 'calib-skip']) $(id).disabled = !ready;
  $('calib-stop').hidden = !state.calibrating;
  $('calib-skip').hidden = calibrated;
  $('user-epoch').textContent = state.user ? `You are person ${state.user}.` : '';
  $('user-epoch').className = 'status-line';
  $('reconnect').hidden = !(state.lastSerialPort && !hub.transport);
  renderData();
  renderSlots();
}

$('connect-serial').onclick = () => connect('serial');
$('connect-sim').onclick = () => connect('sim');
$('reconnect').onclick = async () => {
  try {
    await hub.connect('serial', state.lastSerialPort);
    setStatus('Starting', 'warn', 'Waiting for the hub to start…');
    state.bootTimer = setTimeout(() => handshake(), BOOT_WAIT_MS);
  } catch (error) {
    setStatus('No hub connected', 'bad', error.message);
    log(error.message);
  }
  render();
};
$('camera-start').onclick = startCamera;
$('setup-open').onclick = () => { state.setupOpen = true; render(); };
$('user-set').onclick = async () => {
  const value = Number($('user-id').value);
  if (!Number.isInteger(value) || value < 1 || value > 65535) {
    $('user-epoch').textContent = 'Use a whole number from 1 to 65535.';
    $('user-epoch').className = 'status-line bad';
    return;
  }
  state.user = value;
  state.deletion = null;
  state.skippedCalibration = false;
  $('delete-status').textContent = '';
  if (state.connected) await sendUser();
  render();
};
$('user-random').onclick = () => { $('user-id').value = 1 + Math.floor(Math.random() * 65535); };
$('lambda').oninput = () => {
  state.lambda = Number($('lambda').value);
  $('lambda-value').textContent = state.lambda === 0 ? 'none' : state.lambda === 1 ? 'as much as yours' : `${state.lambda.toFixed(1)}× yours`;
  clearTimeout(state.lambdaTimer);
  state.lambdaTimer = setTimeout(() => hub.request(`LAMBDA ${Math.round(state.lambda * 1000)}`), 150);
};
$('feature-type').onchange = () => { state.featureType = $('feature-type').value; };
$('calib-all').onclick = () => calibrate([0, 1, 2]);
for (let c = 0; c < 3; c++) $('calib-' + c).onclick = () => calibrate([c]);
$('calib-stop').onclick = () => { state.cancel = true; };
$('calib-skip').onclick = () => { state.skippedCalibration = true; state.setupOpen = false; render(); };
$('delete-start').onclick = () => {
  $('delete-who').textContent = state.user;
  $('delete-confirm').hidden = false;
};
$('delete-cancel').onclick = () => { $('delete-confirm').hidden = true; };
$('delete-yes').onclick = deleteMyData;
$('record-consent').onchange = () => {
  $('record-folder').disabled = !$('record-consent').checked;
  if (!$('record-consent').checked) {
    state.recordDir = null;
    $('record-status').textContent = 'Not recording.';
  }
};
$('record-folder').onclick = chooseRecordFolder;
$('timing-read').onclick = async () => {
  const r = await hub.request('TIMING');
  $('timing').textContent = r.ok ? describeTiming(r.fields) : `The hub answered ${r.code}.`;
};
$('timing-reset').onclick = async () => {
  state.rtts = [];
  $('rtt').textContent = 'No predictions yet.';
  const r = await hub.request('TIMING RESET');
  $('timing').textContent = r.ok ? 'Started over.' : `The hub answered ${r.code}.`;
};

function describeTiming(fields) {
  // TIMING idle=<n>,<avg_us>,<max_us> sync=<n>,<avg_us>,<max_us>
  const part = (text) => {
    const [n, avg, max] = text.split('=')[1].split(',').map(Number);
    return n ? `${n} predictions, average ${(avg / 1000).toFixed(2)} ms, slowest ${(max / 1000).toFixed(2)} ms` : 'none yet';
  };
  return `Time the hub spends per prediction. While idle: ${part(fields[1])}. While syncing: ${part(fields[2])}.`;
}

// Developer tools stay out of the product view unless asked for (remembered per browser).
function setDeveloper(on) {
  $('dev').hidden = !on;
  $('dev-toggle').setAttribute('aria-pressed', on ? 'true' : 'false');
  try { localStorage.setItem('meshsync-dev', on ? '1' : '0'); } catch (error) { /* private mode */ }
}
$('dev-toggle').onclick = () => setDeveloper($('dev').hidden);
try { setDeveloper(localStorage.getItem('meshsync-dev') === '1'); } catch (error) { setDeveloper(false); }

render();
showNoPrediction('NOT_CONNECTED');
renderValues(null);
predictLoop();
requestAnimationFrame(animate);
if (!('serial' in navigator)) {
  $('connect-serial').disabled = true;
  $('connect-serial').title = 'Web Serial needs Chrome or Edge';
}

// For debugging in the console. feed() stands in for the camera (tests without a webcam).
window.meshsync = {
  state,
  hub,
  feed(features) {
    state.features = features;
    state.featuresAt = performance.now();
  },
};
