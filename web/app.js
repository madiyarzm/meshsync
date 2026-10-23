// construct: hands-free controller page (Phase D: face gestures).
// The webcam runs MediaPipe here in the page; only 12 numbers per frame go to the hub, over
// USB serial (Web Serial) or to a simulated hub (the same C++ compiled to WebAssembly).
// The hub keeps all calibration. This page stores none of it.

import { createLandmarker, extractFeatures, DRAW_POINTS, BLENDSHAPE_NAMES, LANDMARK_NAMES } from './features.js';

const GESTURES = ['neutral', 'eyebrows up', 'smile'];
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
      Promise.resolve(this.transport.write(`${id} ${text}`)).catch((error) => console.warn('write failed', error));
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
  features: null,        // latest from the camera
  featuresAt: 0,
  rtts: [],
  mesh: { self: null, peers: {}, slots: [] },
  deletion: null,        // { user, startedAt, removed, done }
  recordDir: null,
  bootTimer: null,
  lastSerialPort: null,
};

function setStatus(text, kind = '') {
  const el = $('hub-status');
  el.textContent = text;
  el.className = 'pill ' + kind;
}

function log(text) {
  const list = $('log');
  const item = document.createElement('li');
  const time = new Date().toLocaleTimeString();
  item.textContent = `${time}  ${text}`;
  list.prepend(item);
  while (list.children.length > 40) list.lastChild.remove();
}

function showPrompt(text, kind = '') {
  const el = $('calib-prompt');
  el.textContent = text;
  el.className = 'prompt ' + kind;
}

function digestColor(digest) {
  const value = parseInt(digest, 16) || 0;
  return `hsl(${value % 360}, 65%, 58%)`;
}

// ======================================================================================
// Connecting
// ======================================================================================

hub.onLog = (line) => log(line);
hub.onClose = () => {
  state.connected = false;
  state.sessions = {};
  setStatus('hub disconnected', 'bad');
  log('hub disconnected');
  renderControls();
};
hub.onEvent = handleEvent;

async function connect(kind) {
  state.connected = false;
  state.sessions = {};
  state.mesh = { self: null, peers: {}, slots: [] };
  setStatus(kind === 'sim' ? 'starting the simulated mesh…' : 'choose the hub\'s USB port…');
  try {
    await hub.connect(kind, null);
    if (kind === 'serial') state.lastSerialPort = hub.transport.port;
  } catch (error) {
    setStatus('not connected', 'bad');
    log(error.message || String(error));
    renderControls();
    return;
  }
  $('sim-panel').hidden = kind !== 'sim';
  setStatus('waiting for the hub to start…', 'warn');
  // Opening the port restarts the board. If no BOOT line comes, it may not have restarted.
  clearTimeout(state.bootTimer);
  state.bootTimer = setTimeout(() => handshake(), BOOT_WAIT_MS);
  renderControls();
  renderSim();
}

async function handshake() {
  clearTimeout(state.bootTimer);
  state.sessions = {};
  const hello = await hub.request('HELLO 1');
  if (!hello.ok) {
    setStatus(`hub did not answer (${hello.code})`, 'bad');
    return;
  }
  state.hubLetter = hello.fields[1];
  state.connected = true;
  setStatus(`connected to hub ${state.hubLetter}`, 'ok');
  log(`connected to hub ${state.hubLetter} (protocol ${hello.fields[2]}, ${hello.fields[3]} slots max)`);
  if (state.user) await sendUser();
  await hub.request(`LAMBDA ${Math.round(state.lambda * 1000)}`);
  refreshSlots();
  renderControls();
}

function handleEvent(text) {
  const f = text.split(' ');
  if (f[0] === 'BOOT') {
    log(`hub ${f[1]} started`);
    state.hubLetter = f[1];
    clearTimeout(state.bootTimer);
    handshake();  // also after an unexpected restart: sessions on the hub are gone
  } else if (f[0] === 'EVENT' && f[1] === 'MESH') {
    state.mesh.self = { digest: f[2], entries: +f[3], slots: +f[4], tombstones: +f[5], syncing: f[6] === '1',
      txPackets: +f[7], txBytes: +f[8], at: performance.now() };
    pruneOldPeers();
    renderMesh();
    checkDeletion();
  } else if (f[0] === 'EVENT' && f[1] === 'PEER') {
    state.mesh.peers[f[2]] = { app: +f[3], online: f[4] === '1', ageMs: +f[5], rssi: +f[6], packets: +f[7],
      bytes: +f[8], hb: +f[9], inv: +f[10], req: +f[11], chunk: +f[12], digest: f[13], entries: +f[14],
      at: performance.now() };
  } else if (f[0] === 'EVENT' && f[1] === 'SLOT') {
    log(`received calibration ${f[2]} from the mesh`);
    refreshSlotsSoon();
  } else if (f[0] === 'EVENT' && f[1] === 'TOMBSTONE') {
    log(`a deletion arrived: slot ${f[2]}`);
    refreshSlotsSoon();
  } else if (f[0] === 'EVENT' && f[1] === 'USER_DELETED') {
    log(`user ${f[2]}'s data was deleted (counter ${f[3]})`);
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
    log(`user ${state.user} at hub ${state.hubLetter}`);
  } else {
    log(`USER failed: ${r.code}`);
  }
  refreshSlotsSoon();
  renderControls();
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
    NO_USER: 'set your user number first.',
  };
  return `Can't start a calibration: ${reasons[code] || code}`;
}

// One frame of calibration: the same frame goes to both feature types' sessions.
async function addSample(label, features) {
  const send = async (type, values) => {
    let r = await hub.request(`ADD ${state.sessions[type]} ${label} ${values.join(' ')}`);
    if (!r.ok && r.code === 'UNKNOWN_SESSION') {
      delete state.sessions[type];  // the hub restarted, or this user's data was just deleted
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
  if (!state.connected) return showPrompt('Connect a hub first.', 'bad');
  if (!state.user) return showPrompt('Enter your user number first.', 'bad');
  if (!landmarker && !freshFeatures()) return showPrompt('Start the camera first.', 'bad');
  state.calibrating = true;
  state.cancel = false;
  renderControls();
  const rows = [];
  try {
    await ensureSessions();
    for (const label of classes) {
      for (let s = 3; s > 0 && !state.cancel; s--) {
        showPrompt(`Get ready: ${GESTURES[label].toUpperCase()} in ${s}…`);
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
          showPrompt(`No face detected. Look at the camera (${GESTURES[label]})`, 'warn');
          continue;
        }
        const r = await addSample(label, features);
        if (!r.ok) throw new Error(`The hub refused a sample: ${r.code}`);
        count++;
        showPrompt(`Hold ${GESTURES[label].toUpperCase()}  ${count}/${SAMPLES_PER_CLASS}`, 'rec');
        $('calib-progress').value = count / SAMPLES_PER_CLASS;
        rows.push([Date.now(), state.user, state.hubLetter, 'gesture', label, '', '', ...features.blend, ...features.landmark]);
      }
    }
    showPrompt(state.cancel ? 'Stopped.' : `Done. Hub ${state.hubLetter} has your calibration and shares it with the other hubs within a few seconds.`, state.cancel ? 'warn' : 'ok');
  } catch (error) {
    showPrompt(error.message, 'bad');
  } finally {
    state.calibrating = false;
    $('calib-progress').value = 0;
    renderControls();
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
    } else if (!features && !state.calibrating) {
      showNoPrediction(landmarker ? 'NO_FACE' : 'NO_CAMERA');
    }
    await sleep(Math.max(0, SEND_INTERVAL_MS - (performance.now() - started)));
  }
}

function showPrediction(fields) {
  // PREDICT <type> <label> <p0> <p1> <p2> <own> <others>
  const label = +fields[2];
  const p = [+fields[3], +fields[4], +fields[5]];
  $('pred-label').textContent = GESTURES[label];
  $('pred-label').className = 'pred-label g' + label;
  $('pred-conf').textContent = `${(p[label] / 10).toFixed(0)}% sure`;
  for (let c = 0; c < 3; c++) {
    $('bar-' + c).style.width = `${p[c] / 10}%`;
    $('barv-' + c).textContent = `${(p[c] / 10).toFixed(0)}%`;
  }
  $('pred-samples').textContent = `model built from ${fields[6]} of your samples + ${fields[7]} from other people (λ = ${state.lambda.toFixed(1)})`;
}

function showNoPrediction(code) {
  const messages = {
    NOT_READY: 'No calibration yet. Calibrate below, or raise λ to use other people\'s calibration.',
    NO_FACE: 'No face detected.',
    NO_CAMERA: 'Camera is off.',
    TIMEOUT: 'The hub did not answer in time.',
    NOT_CONNECTED: 'No hub connected.',
    DISCONNECTED: 'No hub connected.',
  };
  $('pred-label').textContent = '—';
  $('pred-label').className = 'pred-label';
  $('pred-conf').textContent = '';
  $('pred-samples').textContent = messages[code] || `Hub: ${code}`;
  for (let c = 0; c < 3; c++) {
    $('bar-' + c).style.width = '0%';
    $('barv-' + c).textContent = '';
  }
}

function recordRtt(ms) {
  state.rtts.push(ms);
  if (state.rtts.length > 300) state.rtts.shift();
  const sorted = [...state.rtts].sort((a, b) => a - b);
  const pick = (q) => sorted[Math.min(sorted.length - 1, Math.floor(q * sorted.length))];
  $('rtt').textContent = `round trip (last ${sorted.length}): median ${pick(0.5).toFixed(1)} ms, 95% ${pick(0.95).toFixed(1)} ms, max ${sorted[sorted.length - 1].toFixed(1)} ms`;
}

// ======================================================================================
// Delete my data
// ======================================================================================

async function deleteMyData() {
  $('delete-confirm').hidden = true;
  const user = state.user;
  const r = await hub.request(`DELETE_USER ${user}`);
  if (!r.ok) {
    $('delete-status').textContent = `Delete failed: ${r.code}`;
    return;
  }
  state.sessions = {};
  state.epoch = +r.fields[2];
  state.deletion = { user, hub: state.hubLetter, startedAt: performance.now(), removed: +r.fields[1], done: false };
  $('delete-status').textContent = `Deleted ${r.fields[1]} calibration slot(s) on hub ${state.hubLetter}. Waiting for the other hubs…`;
  log(`deleted user ${user}'s data (counter now ${r.fields[2]})`);
  refreshSlotsSoon();
}

// Same digest = identical state, so a peer with our digest has the deletion too.
function checkDeletion() {
  const d = state.deletion;
  if (!d || d.done || !state.mesh.self) return;
  const peers = Object.entries(state.mesh.peers).filter(([, p]) => p.app === 2);
  const behind = peers.filter(([, p]) => !p.online || p.digest !== state.mesh.self.digest).map(([letter, p]) => letter + (p.online ? '' : ' (offline)'));
  const seconds = ((performance.now() - d.startedAt) / 1000).toFixed(1);
  if (peers.length === 0) {
    $('delete-status').textContent = `Deleted ${d.removed} slot(s) on hub ${d.hub}. No other hubs in range right now; they'll get the deletion when they meet any hub that has it.`;
  } else if (behind.length === 0) {
    d.done = true;
    $('delete-status').textContent = `Deleted on hub ${d.hub}; hub ${state.hubLetter} and all ${peers.length} other hub(s) in range now have the same state (${seconds} s). Hubs out of range get it when they reconnect.`;
  } else {
    $('delete-status').textContent = `Deleted on hub ${d.hub}. Not reached yet: ${behind.join(', ')} (${seconds} s)`;
  }
}

// ======================================================================================
// Mesh panel
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
  renderSlots();
}
setInterval(refreshSlots, 3000);

function renderMesh() {
  const self = state.mesh.self;
  if (!self) return;
  $('self-swatch').style.background = digestColor(self.digest);
  $('self-text').textContent = `hub ${state.hubLetter} · ${self.slots} slots · ${self.tombstones} deleted · ${self.syncing ? 'syncing' : 'idle'} · sent ${self.txPackets} packets/s`;
  const rows = Object.entries(state.mesh.peers).sort(([a], [b]) => a.localeCompare(b)).map(([letter, p]) => {
    const same = p.digest === self.digest;
    const other = p.app !== 2;
    const rssi = p.rssi === 0 ? 'n/a' : `${p.rssi} dBm`;
    const bars = p.rssi === 0 ? '' : '▮'.repeat(Math.max(1, Math.min(4, Math.round((p.rssi + 100) / 12))));
    const sync = other ? 'other app' : !p.online ? 'offline' : same ? 'same state' : 'differs';
    return `<tr><td><span class="swatch" style="background:${other ? '#555' : digestColor(p.digest)}"></span>${letter}</td>
      <td class="${p.online ? 'ok' : 'bad'}">${sync}</td><td>${rssi} <span class="bars">${bars}</span></td>
      <td>${p.packets}/s · ${(p.bytes / 1024).toFixed(1)} KB/s</td>
      <td class="mono">${p.hb}/${p.inv}/${p.req}/${p.chunk}</td><td>${(p.ageMs / 1000).toFixed(1)} s</td></tr>`;
  });
  $('peers').innerHTML = rows.length ? rows.join('') : '<tr><td colspan="6" class="muted">No other hubs heard yet.</td></tr>';
}

function renderSlots() {
  const slots = state.mesh.slots;
  const mine = slots.filter((s) => s.user === state.user && state.user);
  const mineSamples = mine.filter((s) => s.type === 'gest_blend').reduce((n, s) => n + s.samples, 0);
  $('mine-text').textContent = state.user
    ? (mine.length ? `Your calibration on this hub: ${mineSamples} gesture samples, from hub(s) ${[...new Set(mine.map((s) => s.owner))].join(', ')}.` : 'This hub has none of your calibration (yet).')
    : 'Set a user number to see your calibration.';
  const types = { gest_blend: 'gesture (blendshapes)', gest_lmk: 'gesture (landmarks)', gaze: 'gaze', digits: 'digits' };
  $('slots').innerHTML = slots.length
    ? slots.map((s) => `<tr class="${s.user === state.user && state.user ? 'mine' : ''}"><td>${s.owner}</td><td>${types[s.type] || s.type}</td><td>${s.user}${s.user === state.user && state.user ? ' (you)' : ''}</td><td>${s.samples}</td><td class="mono">${s.key}</td></tr>`).join('')
    : '<tr><td colspan="5" class="muted">No calibration stored on this hub.</td></tr>';
}

// ======================================================================================
// Simulated mesh controls
// ======================================================================================

function renderSim() {
  const sim = hub.transport && hub.transport.kind === 'sim' ? hub.transport.api : null;
  if (!sim) return;
  const count = sim.count();
  const attached = sim.attached();
  const letters = [...Array(count).keys()].map((i) => String.fromCharCode(65 + i));
  $('sim-attach').innerHTML = letters.map((l, i) => `<button data-hub="${i}" class="${i === attached ? 'on' : ''}">${l}</button>`).join('');
  const pairs = [];
  for (let a = 0; a < count; a++) for (let b = a + 1; b < count; b++) pairs.push([a, b]);
  $('sim-links').innerHTML = pairs.map(([a, b]) => `<button data-a="${a}" data-b="${b}" class="${sim.link(a, b) ? 'on' : ''}">${letters[a]}–${letters[b]}</button>`).join('');
}

$('sim-attach').addEventListener('click', (event) => {
  const button = event.target.closest('button');
  if (!button || state.calibrating) return;
  const target = +button.dataset.hub;
  log(`moving the cable to hub ${String.fromCharCode(65 + target)}`);
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
  $('camera-status').textContent = 'starting the camera…';
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
    $('camera-status').textContent = 'loading the face model (local files, no internet needed)…';
    try {
      landmarker = await createLandmarker();
    } catch (error) {
      $('camera-status').textContent = `Could not load MediaPipe: ${error.message}`;
      return;
    }
  }
  $('camera-status').textContent = 'The video stays in this page. Only 12 numbers per frame go to the hub.';
  $('camera-start').hidden = true;
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
    $('face-status').textContent = features ? (features.clamped ? `face found (${features.clamped} values clamped)` : 'face found') : 'no face detected';
    $('face-status').className = 'pill ' + (features ? 'ok' : 'warn');
    renderFeatureBars(features);
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
  ctx.fillStyle = '#6ea8ff';
  for (const i of DRAW_POINTS) {
    const p = features.points[i];
    ctx.beginPath();
    ctx.arc(p.x * width, p.y * height, 3, 0, 2 * Math.PI);
    ctx.fill();
  }
}

function renderFeatureBars(features) {
  const names = state.featureType === 'gest_blend' ? BLENDSHAPE_NAMES : LANDMARK_NAMES;
  const values = features ? (state.featureType === 'gest_blend' ? features.blend : features.landmark) : null;
  const el = $('features');
  if (el.children.length !== names.length || el.dataset.type !== state.featureType) {
    el.dataset.type = state.featureType;
    el.innerHTML = names.map((n) => `<div class="feat"><span>${n}</span><i><b></b></i><em></em></div>`).join('');
  }
  for (let i = 0; i < names.length; i++) {
    const row = el.children[i];
    const v = values ? values[i] : 0;
    row.querySelector('b').style.width = `${Math.min(100, Math.abs(v) / 10)}%`;
    row.querySelector('em').textContent = values ? v : '';
  }
}

// ======================================================================================
// Controls
// ======================================================================================

function renderControls() {
  const ready = state.connected && state.user > 0 && !state.calibrating;
  for (const id of ['calib-all', 'calib-0', 'calib-1', 'calib-2']) $(id).disabled = !ready;
  $('calib-stop').hidden = !state.calibrating;
  $('delete-start').disabled = !(state.connected && state.user > 0) || state.calibrating;
  $('user-epoch').textContent = state.user ? `deletion counter ${state.epoch}` : '';
  $('reconnect').hidden = !(state.lastSerialPort && !hub.transport);
}

$('connect-serial').onclick = () => connect('serial');
$('connect-sim').onclick = () => connect('sim');
$('reconnect').onclick = async () => {
  try {
    await hub.connect('serial', state.lastSerialPort);
    setStatus('waiting for the hub to start…', 'warn');
    state.bootTimer = setTimeout(() => handshake(), BOOT_WAIT_MS);
  } catch (error) {
    log(error.message);
  }
  renderControls();
};
$('camera-start').onclick = startCamera;
$('user-set').onclick = async () => {
  const value = Number($('user-id').value);
  if (!Number.isInteger(value) || value < 1 || value > 65535) {
    $('user-epoch').textContent = 'Use a whole number from 1 to 65535.';
    return;
  }
  state.user = value;
  state.deletion = null;
  $('delete-status').textContent = '';
  if (state.connected) await sendUser();
  renderControls();
  renderSlots();
};
$('user-random').onclick = () => {
  $('user-id').value = 1 + Math.floor(Math.random() * 65535);
};
$('lambda').oninput = () => {
  state.lambda = Number($('lambda').value);
  $('lambda-value').textContent = state.lambda.toFixed(1);
  clearTimeout(state.lambdaTimer);
  state.lambdaTimer = setTimeout(() => hub.request(`LAMBDA ${Math.round(state.lambda * 1000)}`), 150);
};
$('feature-type').onchange = () => {
  state.featureType = $('feature-type').value;
};
$('calib-all').onclick = () => calibrate([0, 1, 2]);
for (let c = 0; c < 3; c++) $('calib-' + c).onclick = () => calibrate([c]);
$('calib-stop').onclick = () => { state.cancel = true; };
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
    $('record-status').textContent = 'Recording is off.';
  }
};
$('record-folder').onclick = chooseRecordFolder;
$('timing-read').onclick = async () => {
  const r = await hub.request('TIMING');
  $('timing').textContent = r.ok ? describeTiming(r.fields) : `hub: ${r.code}`;
};
$('timing-reset').onclick = async () => {
  state.rtts = [];
  $('rtt').textContent = '';
  const r = await hub.request('TIMING RESET');
  $('timing').textContent = r.ok ? 'reset' : `hub: ${r.code}`;
};

function describeTiming(fields) {
  // TIMING idle=<n>,<avg_us>,<max_us> sync=<n>,<avg_us>,<max_us>
  const part = (text) => {
    const [n, avg, max] = text.split('=')[1].split(',').map(Number);
    return n ? `${n} predictions, avg ${(avg / 1000).toFixed(2)} ms, max ${(max / 1000).toFixed(2)} ms` : 'none yet';
  };
  return `on the hub, while idle: ${part(fields[1])}; while syncing: ${part(fields[2])}`;
}

renderControls();
renderFeatureBars(null);
showNoPrediction('NOT_CONNECTED');
predictLoop();
if (!('serial' in navigator)) {
  $('connect-serial').disabled = true;
  $('connect-serial').title = 'Web Serial needs Chrome or Edge';
}

// For debugging in the console. feed() stands in for the camera (tests without a webcam).
window.construct = {
  state,
  hub,
  feed(features) {
    state.features = features;
    state.featuresAt = performance.now();
  },
};
