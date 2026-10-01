// OpenBallance in the browser: openballance.wasm on gasm. The game draws with gasm:gfx (WebGPU). It runs in
// gasm's Worker mode when the browser has WebGPU in workers: the canvas is transferred to the worker
// (OffscreenCanvas), which renders into it and reads the game data on demand, either from the site's OPFS
// copy (FileSystemSyncAccessHandle) or straight from the picked files (FileReaderSync). Otherwise it runs
// on the main thread with WebGPU there, and the data is loaded into memory first. The page keeps input
// and audio.
//
// Test and debug query parameters:
//   unlockall, mode, level, language   passed to the game (docs/guide/parameters.md)
//   hashframes=N   run N frames with no input on virtual time as fast as possible, then print the
//                  same "frames=... video_fnv32=... audio_fnv32=..." line as gasm-run --headless
//                  (globalThis.__openballanceResult)
//   autoplay       start the imported data right away
//   main           run on the main thread even if the worker could
import { AssetTable, DEFAULT_KEYMAP, GasmHost, IdbStorage, MemoryStorage, ProcExit, Resampler, keyboardPads, parseKeymap, preloadAssets } from './vendor/gasm/gasm-host.js';
import { GasmWorker } from './vendor/gasm/gasm-worker.js';
import { WebGpuGfx } from './vendor/gasm/webgpu-gfx.js';
import * as cd from './cd.js';

const $ = (id) => document.getElementById(id);
const query = new URLSearchParams(location.search);
const GAME_PARAMS = ['unlockall', 'mode', 'level', 'language', 'dump2d'];
const HASH_FRAMES = Number(query.get('hashframes') || 0);
const STORAGE = 'openballance';                // gasm:storage namespace (IndexedDB): highscores, settings
const hasOpfs = !!navigator.storage?.getDirectory;
const hasWebGpu = !!navigator.gpu;
const offscreenOk = typeof HTMLCanvasElement !== 'undefined' && 'transferControlToOffscreen' in HTMLCanvasElement.prototype;

function message(text, kind = 'info') {
  const m = $('message');
  m.hidden = !text;
  m.textContent = text ?? '';
  m.className = `message ${kind}`;
  if (text) console.log(`[play] ${text}`);
}
const show = (id, on) => { $(id).hidden = !on; };

// ---- keyboard layout, typed text and gamepads ---------------------------------------------------
const KEYMAP_KEY = 'openballance.keymap';
// OpenBallance's layout (tools/gasm-bundle/keymap.txt: both Shift keys on L, so Shift + arrows rotate the
// view as in the original); gasm's built-in layout if it can't be fetched.
const BALLANCE_KEYMAP = await fetch(new URL('./keymap.txt', import.meta.url)).then((r) => (r.ok ? r.text() : DEFAULT_KEYMAP)).catch(() => DEFAULT_KEYMAP);
let keymapText = localStorage.getItem(KEYMAP_KEY) ?? BALLANCE_KEYMAP;
let keymap = parseKeymap(keymapText);
if (keymap.errors.length) { keymapText = BALLANCE_KEYMAP; keymap = parseKeymap(BALLANCE_KEYMAP); }

const held = new Set();
let typed = '';                         // text_input: characters typed since the last frame (highscore names)
const typing = (e) => e.target instanceof HTMLInputElement || e.target instanceof HTMLTextAreaElement;
addEventListener('keydown', (e) => {
  if (!running || typing(e) || $('keys-dialog').open) return;
  if (e.key === 'Enter') typed += '\n';
  else if (e.key === 'Backspace') typed += '\b';
  else if (e.key.length === 1 && !e.ctrlKey && !e.metaKey && !e.altKey) typed += e.key;
  if (e.key === 'Backspace' || e.key === ' ') e.preventDefault();   // no "back" navigation or page scroll
  if (!keymap.bindings.has(e.code)) return;
  held.add(e.code);
  e.preventDefault();
});
addEventListener('keyup', (e) => held.delete(e.code));
addEventListener('blur', () => held.clear());
const takeTyped = () => { const t = typed; typed = ''; return t; };

// W3C "standard" gamepad mapping -> gasm button bit (A east, B south, X north, Y west, like gasm's app.js).
const PAD = { 1: 0, 0: 1, 3: 2, 2: 3, 4: 4, 5: 5, 6: 4, 7: 5, 8: 6, 9: 7, 12: 8, 13: 9, 14: 10, 15: 11 };
function readPads() {
  const pads = [0, 0, 0, 0];
  let n = 0;
  for (const gp of navigator.getGamepads?.() ?? []) {
    if (!gp || n > 3) continue;
    let m = 0;
    for (const [btn, bit] of Object.entries(PAD)) if (gp.buttons[btn]?.pressed) m |= 1 << bit;
    const [x = 0, y = 0] = gp.axes;
    if (x < -0.5) m |= 1 << 10; if (x > 0.5) m |= 1 << 11;
    if (y < -0.5) m |= 1 << 8; if (y > 0.5) m |= 1 << 9;
    pads[n++] |= m;
  }
  const kb = keyboardPads(keymap.bindings, held, n);
  return pads.map((p, i) => p | kb[i]);
}

// Controls table from the active layout (src/app.c read_input: pad 1 -> the keys the game reads).
const KEY_NAMES = {
  ArrowUp: 'Up', ArrowDown: 'Down', ArrowLeft: 'Left', ArrowRight: 'Right', Period: 'Period', Comma: 'Comma',
  Slash: '/', Semicolon: ';', Quote: "'", ShiftRight: 'Right Shift', ShiftLeft: 'Left Shift',
  ControlRight: 'Right Ctrl', ControlLeft: 'Left Ctrl', NumpadEnter: 'Keypad Enter', AltRight: 'Right Alt',
  AltLeft: 'Left Alt', Space: 'Space', Enter: 'Enter', Backspace: 'Backspace',
};
const keyName = (code) => KEY_NAMES[code] ?? code.replace(/^Key|^Digit/, '').replace(/^Numpad/, 'Keypad ');
function keysFor(button) {
  const bit = ['a', 'b', 'x', 'y', 'l', 'r', 'select', 'start', 'up', 'down', 'left', 'right'].indexOf(button);
  const codes = [...keymap.bindings].filter(([, bs]) => bs.some((b) => b.pad === 0 && b.bit === bit)).map(([c]) => c);
  return codes.map(keyName).join(' or ') || '-';
}
function renderControls() {
  const arrows = () => {
    const k = ['up', 'left', 'down', 'right'].map(keysFor);
    return k.join(' ') === 'Up Left Down Right' ? 'Arrow keys' : k.join(' / ');
  };
  const rows = [
    ['D-pad', 'Arrow keys', 'Roll the ball; menus: choose', arrows],
    ['A or START', 'Enter', 'Select, continue the tutorial', () => `${keysFor('a')} or ${keysFor('start')}`],
    ['B', 'Esc', 'Pause menu, back', () => keysFor('b')],
    ['L / R + Left / Right', 'Shift + arrows', 'Rotate the camera around the ball', () => `${keysFor('l')} or ${keysFor('r')}, with Left / Right`],
    ['X', 'Space', 'Raise the camera (look ahead)', () => keysFor('x')],
    ['Y', 'Q', 'Quit the tutorial', () => keysFor('y')],
    ['SELECT', 'F1', 'F1 (help)', () => keysFor('select')],
  ];
  const body = $('controls-body');
  body.textContent = '';
  for (const [pad, orig, action, keys] of rows) {
    const tr = document.createElement('tr');
    for (const text of [pad, orig, action, keys()]) {
      const td = document.createElement('td');
      td.textContent = text;
      tr.append(td);
    }
    body.append(tr);
  }
}
renderControls();

$('keys').onclick = () => {
  $('keymap-text').value = keymapText;
  $('keymap-error').textContent = '';
  $('keys-dialog').showModal();
};
$('keymap-save').onclick = (e) => {
  const text = $('keymap-text').value, k = parseKeymap(text);
  if (k.errors.length) { e.preventDefault(); $('keymap-error').textContent = k.errors.join('\n'); return; }
  keymapText = text; keymap = k;
  if (text === BALLANCE_KEYMAP) localStorage.removeItem(KEYMAP_KEY); else localStorage.setItem(KEYMAP_KEY, text);
  renderControls();
};
$('keymap-reset').onclick = (e) => { e.preventDefault(); $('keymap-text').value = BALLANCE_KEYMAP; $('keymap-error').textContent = ''; };

// ---- audio: the gasm web player's AudioWorklet queue ------------------------------------------
const WORKLET = `
class GasmOut extends AudioWorkletProcessor {
  constructor() {
    super();
    this.q = []; this.off = 0; this.len = 0; this.primed = false;
    this.target = Math.round(sampleRate * 0.06) * 2; this.max = Math.round(sampleRate * 0.2) * 2;
    this.port.onmessage = (e) => {
      this.q.push(e.data); this.len += e.data.length;
      while (this.len > this.max && this.q.length > 1) { this.len -= this.q[0].length - this.off; this.q.shift(); this.off = 0; }
    };
  }
  process(_, [out]) {
    const L = out[0], R = out[1] ?? out[0];
    if (!this.primed && this.len >= this.target) this.primed = true;
    for (let i = 0; i < L.length; i++) {
      if (!this.primed || this.len < 2) { this.primed = false; L[i] = R[i] = 0; continue; }
      const b = this.q[0];
      L[i] = b[this.off]; R[i] = b[this.off + 1];
      this.off += 2; this.len -= 2;
      if (this.off >= b.length) { this.q.shift(); this.off = 0; }
    }
    return true;
  }
}
registerProcessor('gasm-out', GasmOut);`;

let audioCtx = null, audioNode = null, gain = null, resampler = null, audioReady = null;
let muted = localStorage.getItem('openballance.muted') === '1';
/** Call from a click handler (browsers only start audio on a user gesture). */
function startAudio() {
  if (!audioCtx) {
    try {
      audioCtx = new AudioContext({ latencyHint: 'interactive' });
    } catch (e) { message(`No sound: ${e.message}`, 'warn'); return; }
    audioReady = audioCtx.audioWorklet.addModule(URL.createObjectURL(new Blob([WORKLET], { type: 'text/javascript' }))).then(() => {
      audioNode = new AudioWorkletNode(audioCtx, 'gasm-out', { outputChannelCount: [2] });
      gain = new GainNode(audioCtx, { gain: muted ? 0 : 1 });
      audioNode.connect(gain).connect(audioCtx.destination);
      resampler = new Resampler(audioCtx.sampleRate);
    }).catch((e) => { message(`No sound: ${e.message}`, 'warn'); audioCtx = null; });
  }
  audioCtx?.resume();
}
function onAudio(samples, rate, channels) {
  if (!audioNode) return;
  const out = resampler.process(samples, rate, channels);
  audioNode.port.postMessage(out.slice());
}
function setMuted(m) {
  muted = m;
  localStorage.setItem('openballance.muted', m ? '1' : '0');
  if (gain) gain.gain.value = m ? 0 : 1;
  $('sound').textContent = m ? 'Sound: off' : 'Sound: on';
}
$('sound').onclick = () => { startAudio(); setMuted(!muted); };
setMuted(muted);

// ---- display: a WebGPU canvas -----------------------------------------------------------------
// A canvas can only ever have one context (and can be transferred to a worker once), so each run
// gets a fresh one. The game letterboxes its 4:3 picture into whatever size the canvas has.
let canvas = $('screen');
function freshCanvas() {
  const c = document.createElement('canvas');
  c.id = 'screen';
  c.setAttribute('aria-label', 'Ballance');
  c.tabIndex = 0;
  c.ondblclick = () => $('fullscreen').onclick();
  canvas.replaceWith(c);
  canvas = c;
  return c;
}
/** The canvas' display size in device pixels (an OffscreenCanvas can't measure itself). */
const canvasSize = () => [Math.max(1, Math.round(canvas.clientWidth * (devicePixelRatio || 1))) || 960,
                          Math.max(1, Math.round(canvas.clientHeight * (devicePixelRatio || 1))) || 720];
$('fullscreen').onclick = () => {
  if (document.fullscreenElement) document.exitFullscreen();
  else ($('stage').requestFullscreen ?? $('stage').webkitRequestFullscreen)?.call($('stage'));
};

// ---- running the game ---------------------------------------------------------------------
// worker: GasmWorker (Worker mode) or null; host + gpu: the main-thread runner.
let worker = null, host = null, gpu = null, running = false, inflight = false, rafId = 0, wasmBytes = null, lastLog = '';
let acc = 0, last = 0, fpsN = 0, fpsT = 0;

async function loadWasm() {
  if (!wasmBytes) {
    const r = await fetch(new URL('openballance.wasm', import.meta.url));
    if (!r.ok) throw new Error(`openballance.wasm: HTTP ${r.status}`);
    wasmBytes = await r.arrayBuffer();
  }
  return wasmBytes.slice(0); // start() transfers its copy to the worker
}

function onLog(msg) {
  console.log(msg);
  if (/^\[guest\]/.test(msg)) lastLog = msg.replace(/^\[guest\]\s*/, '');
}

async function stopGame() {
  cancelAnimationFrame(rafId);
  running = false; inflight = false;
  held.clear(); typed = '';
  const w = worker, h = host;
  worker = null; host = null; gpu = null;
  await w?.exit();          // flushes the saves, releases the OPFS handles
  try { h?.exit(); } catch {}
}

function stopped(e) {
  running = false;
  if (e instanceof ProcExit) message('The game has ended. Press Play to start again.');
  else message(`The game stopped: ${e.message}${lastLog ? ` (${lastLog})` : ''}`, 'error');
  stopGame();
  endGameView();
}

function endGameView() {
  cancelAnimationFrame(rafId);
  show('game', false);
  show('setup', true);
  refreshImported();
}

const params = () => Object.fromEntries(GAME_PARAMS.filter((k) => query.has(k)).map((k) => [k, query.get(k)]));

/** Worker mode: the canvas goes to the worker. Rejects with an error mentioning WebGPU if it has none. */
async function startWorker(source) {
  return GasmWorker.start({
    wasm: await loadWasm(), assets: cd.assetSpecs(source), params: params(), storage: STORAGE, keyboard: true,
    canvas: freshCanvas().transferControlToOffscreen(), size: canvasSize(),
    hashing: HASH_FRAMES > 0, virtualTime: HASH_FRAMES > 0, onLog, onAudio,
  });
}

/** Main thread: WebGPU on the page's canvas, the data preloaded into memory. */
async function startMain(source) {
  const entries = await cd.sourceEntries(source);
  const total = entries.reduce((n, [, f]) => n + f.size, 0);
  $('status').textContent = `Loading the game data (${cd.mb(total)})...`;
  const assets = new AssetTable(onLog).merge(await preloadAssets(entries, {
    log: onLog,
    onProgress: (p) => { $('status').textContent = `Loading the game data: ${cd.mb(p.bytes)} of ${cd.mb(total)}`; },
  }));
  const c = freshCanvas();
  gpu = await WebGpuGfx.create(c, onLog);
  const storage = await IdbStorage.open(STORAGE).catch((e) => {
    message(`Browser storage is unavailable (${e.message}): highscores and settings won't be kept.`, 'warn');
    return new MemoryStorage();
  });
  const h = new GasmHost({ assets, params: params(), gfx: gpu, storage, onAudio, onLog, virtualTime: HASH_FRAMES > 0 });
  h.text = '';             // the page has a keyboard
  if (HASH_FRAMES > 0) h.hashing = true;
  await h.load(await loadWasm());
  return h;
}

/** Start the game from 'opfs' (the imported data) or a checked source (read in place). */
async function play(source) {
  startAudio();                        // still inside the click's user gesture
  message('');
  await stopGame();
  lastLog = '';
  if (!hasWebGpu) {
    message('This browser has no WebGPU, which OpenBallance needs for its 3D graphics. Use a recent Chrome or Edge, Safari 26 or newer, or Firefox 141 or newer (on Windows; on macOS and Linux enable dom.webgpu.enabled), or run it with gasm-run.', 'error');
    return;
  }
  show('game', true);
  show('setup', HASH_FRAMES > 0);      // keep the page visible in test runs
  $('status').textContent = 'Starting...';
  try {
    await audioReady;
    if (offscreenOk && !query.has('main')) {
      try {
        worker = await startWorker(source);
      } catch (e) {
        if (!/WebGPU/i.test(e.message)) throw e;
        console.log(`[play] ${e.message}: running on the main thread`);
      }
    }
    if (!worker) host = await startMain(source);
  } catch (e) {
    worker = null; host = null; gpu = null;
    const busy = /NoModificationAllowed|InvalidState|lock|access handle/i.test(`${e.name} ${e.message}`);
    if (busy) message('The imported data is in use by another OpenBallance tab or window. Close it and try again.', 'error');
    else if (e instanceof ProcExit) message(`The game could not start${lastLog ? `: ${lastLog}` : ''}.`, 'error');
    else if (/WebGPU|adapter|GPU/i.test(e.message)) message(`The game could not start its graphics: ${e.message}. OpenBallance needs WebGPU; check that hardware acceleration is on.`, 'error');
    else message(`The game could not start: ${e.message}`, 'error');
    endGameView();
    return;
  }
  document.title = 'Ballance | OpenBallance';
  $('status').textContent = worker ? '' : 'main thread';
  if (HASH_FRAMES > 0) return hashRun(HASH_FRAMES);
  canvas.focus?.();
  acc = 0; last = performance.now(); running = true;
  rafId = requestAnimationFrame(tick);
}

function tick(now) {
  rafId = requestAnimationFrame(tick);
  if (!running || !(worker || host)) return;
  const period = 1000 / (worker ?? host).frameRate;
  acc += Math.min(now - last, 100);    // clamp after tab switches
  last = now;
  // Fixed timestep; when catching up (at most 4 frames), only the last one is drawn.
  const due = Math.min(4, Math.floor(acc / period));
  if (worker) {
    if (!inflight && due > 0) {        // one batch in flight: the worker sets the pace
      const pads = readPads();
      inflight = true;
      acc -= due * period;
      const texts = Array.from({ length: due }, (_, k) => (k === 0 ? takeTyped() : ''));
      worker.frames(Array.from({ length: due }, () => pads), true, { texts, size: canvasSize() }).then(() => {
        inflight = false;
        fpsN += due;
      }, stopped);
    }
  } else {
    for (let step = 0; step < due; step++) {
      const pads = readPads();
      host.getPad = (p) => pads[p] ?? 0;
      host.text = step === 0 ? takeTyped() : '';
      host.showFrame = step === due - 1;
      gpu.used = false;
      try { host.frame(); } catch (e) { return stopped(e); }
      acc -= period; fpsN++;
    }
  }
  if (acc > period * 4) acc = 0;       // fell far behind: resync
  if (now - fpsT >= 1000) {
    $('status').textContent = `${fpsN} frames/s${worker ? '' : ' (main thread)'}`;
    fpsN = 0; fpsT = now;
  }
}

const hex = (h) => (h >>> 0).toString(16).padStart(8, '0');
async function hashRun(n) {
  let s;
  try {
    if (worker) {
      let left = n;
      while (left > 0) {
        const k = Math.min(250, left);
        await worker.frames(Array.from({ length: k }, () => [0, 0, 0, 0]), left === k, { size: canvasSize() });
        left -= k;
      }
      s = worker.stats;
    } else {
      for (let i = 0; i < n; i++) { host.showFrame = i === n - 1; host.frame(); }
    }
  } catch (e) { if (!(e instanceof ProcExit)) { message(`hash run failed: ${e.message}`, 'error'); throw e; } }
  s ??= { frames: host.frameIndex, presented: host.framesPresented, width: host.width, height: host.height,
          videoHash: host.videoHash, audioHash: host.audioHash, audioFrames: host.audioFrames };
  const out = `frames=${s.frames} presented=${s.presented} size=${s.width}x${s.height} ` +
              `video_fnv32=${hex(s.videoHash)} audio_fnv32=${hex(s.audioHash)} audio_frames=${s.audioFrames}`;
  $('status').textContent = out;
  console.log(out);
  globalThis.__openballanceResult = out;
}

$('stop').onclick = async () => { await stopGame(); endGameView(); };
addEventListener('pagehide', () => { worker?.exit(); try { host?.exit(); } catch {} });

// ---- choosing and importing the data --------------------------------------------------------
let checked = null; // result of cd.folderFrom... / cd.checkImage

const describe = (src) => (src.kind === 'image' ? 'disc image' : src.layout === 'cd' ? 'CD folder' : 'game folder');

function showCheck(result) {
  checked = result;
  const list = $('check-list');
  list.textContent = '';
  const item = (text, ok) => {
    const li = document.createElement('li');
    li.className = ok ? 'ok' : 'bad';
    li.textContent = text;
    list.append(li);
  };
  const src = result.source;
  item(`${src.kind === 'image' ? 'disc image' : 'folder'} "${src.name}": ${src.kind === 'image' ? mbText(result.bytes) : `${result.files} files, ${mbText(result.bytes)}`}`, true);
  if (result.problems.length) {
    for (const p of result.problems) item(p, false);
    item('This does not look like Ballance: choose the installed game folder, the CD, or an image of the CD.', false);
  } else if (src.kind === 'image') {
    item('ISO 9660 file system with Setup/data1.hdr and the cabinets: the Ballance CD', true);
  } else if (src.layout === 'cd') {
    item('Setup/data1.hdr and the data cabinets found: the Ballance CD (the game is read from the installer\'s cabinets)', true);
  } else {
    item('base.cmo, 3D Entities, Textures and Sounds found: an installed Ballance', true);
  }
  show('check', true);
  show('check-actions', !result.problems.length);
  $('import').hidden = !hasOpfs;
  globalThis.__openballanceChecked = { problems: result.problems, files: result.files, bytes: result.bytes, layout: src.layout ?? src.kind };
}
const mbText = cd.mb;

$('pick-folder').onclick = async () => {
  message('');
  if (!window.showDirectoryPicker) return $('folder-input').click(); // Firefox, Safari
  let handle;
  try { handle = await showDirectoryPicker({ id: 'openballance-data', mode: 'read' }); } catch (e) {
    if (e.name !== 'AbortError') message(e.message, 'error');
    return;
  }
  message('Reading the folder...');
  try { showCheck(await cd.folderFromHandle(handle)); message(''); } catch (e) { message(`Could not read the folder: ${e.message}`, 'error'); }
};
$('folder-input').onchange = (e) => {
  if (e.target.files.length) showCheck(cd.folderFromFileList(e.target.files));
  e.target.value = '';
};
$('pick-image').onclick = () => { message(''); $('image-input').click(); };
$('image-input').onchange = async (e) => {
  const f = e.target.files[0];
  e.target.value = '';
  if (f) showCheck(await cd.checkImage(f));
};

$('import').onclick = async () => {
  if (!checked) return;
  startAudio();
  const source = checked.source;
  show('check', false);
  show('progress', true);
  $('pick-folder').disabled = $('pick-image').disabled = true;
  const t0 = performance.now();
  try {
    await stopGame();
    const info = await cd.importSource(source, (p) => {
      $('progress-bar').value = p.total ? p.bytes / p.total : 1;
      $('progress-text').textContent = `Copying ${p.file}: ${mbText(p.bytes)} of ${mbText(p.total)} (${p.done} of ${p.files} files)`;
    });
    $('progress-text').textContent = `Imported ${info.files} files, ${mbText(info.bytes)} in ${((performance.now() - t0) / 1000).toFixed(0)} s.`;
    globalThis.__openballanceImported = info;
    await refreshImported();
    show('progress', false);
    await play('opfs');
  } catch (e) {
    show('progress', false);
    const full = /quota/i.test(`${e.name} ${e.message}`);
    message(full ? `Not enough browser storage for the game data (${mbText(source.kind === 'image' ? source.file.size : checked.bytes)}). Free some space, or play without importing.`
                 : `Import failed: ${e.message}. You can still play without importing.`, 'error');
    show('check', true);
    await cd.removeImport().catch(() => {});
    refreshImported();
  } finally {
    $('pick-folder').disabled = $('pick-image').disabled = false;
  }
};
$('play-direct').onclick = () => { if (checked) play(checked.source); };
$('play-opfs').onclick = () => play('opfs');
$('remove').onclick = async () => {
  await stopGame();
  show('game', false);
  try { await cd.removeImport(); message('The imported game data was removed from this browser.'); } catch (e) { message(`Could not remove it: ${e.message}`, 'error'); }
  refreshImported();
};
$('persist').onclick = async () => {
  const ok = await cd.requestPersist();
  message(ok ? 'The browser will keep the imported data.' : 'The browser did not agree to keep the data; it stays until space runs low.', ok ? 'info' : 'warn');
  refreshImported();
};

async function refreshImported() {
  const info = hasOpfs ? await cd.existingImport() : null;
  show('imported', !!info);
  $('choose-title').textContent = info ? 'Your Ballance' : 'Choose your Ballance';
  if (info) {
    $('imported-desc').textContent = `${describe(info)} "${info.name}", ${info.files} files, ${mbText(info.bytes)}`;
    const s = await cd.storageInfo();
    $('storage').textContent = s.usage !== undefined
      ? `This site uses ${mbText(s.usage)} of the ${mbText(s.quota ?? 0)} the browser allows${s.persisted ? '; kept even when space runs low' : ''}.`
      : '';
    show('persist-row', !s.persisted && !!navigator.storage?.persist);
  }
  return info;
}

if (!hasWebGpu) {
  message('This browser has no WebGPU, which OpenBallance needs for its 3D graphics. Use a recent Chrome or Edge, Safari 26 or newer, or Firefox 141 or newer (Windows; elsewhere enable dom.webgpu.enabled in about:config).', 'error');
} else if (!hasOpfs) {
  message('This browser has no private file storage (OPFS), so the game data can\'t be imported. Play without importing works.', 'warn');
}
if (typeof WebAssembly === 'undefined') message('This browser can\'t run OpenBallance (it needs WebAssembly).', 'error');
$('webgpu').textContent = hasWebGpu ? 'WebGPU is available in this browser.' : 'WebGPU is not available in this browser.';
$('webgpu').className = hasWebGpu ? 'small ok-text' : 'small bad-text';
const ready = refreshImported();
ready.then((info) => {
  globalThis.__openballanceReady = true;
  if (info && query.has('autoplay')) play('opfs');
});
globalThis.openballancePlay = { play, stopGame, refreshImported, cd };
