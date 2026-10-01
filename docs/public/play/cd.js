// The player's Ballance data: picking it, checking it, and importing it into the site's origin private
// file system (OPFS). Nothing here is game data; everything comes from the user's own copy of the game
// and stays on their device.
//
// A source is either
//   { kind: 'folder', name, layout: 'installed' | 'cd', entries: [[path, File], ...] }
//       the installed game folder (base.cmo, 3D Entities, Sounds, Textures, ...) or the CD's files
//       (Setup/data1.hdr + the dataN.cab cabinets): the game reads the installer's cabinets itself
//   { kind: 'image', name, file }   the CD image: an .iso, or a raw MODE1/2352 .bin
// The game (openballance.wasm) reads a folder as separate assets named by their paths and an image as
// the single asset "rom" (see src/platform_gasm.c: mount_game).
import { directoryHandleEntries, fileListEntries } from './vendor/gasm/gasm-host.js';
import { clearNamespace, opfsFileSystem, persist } from '@emdzej/csfs-opfs';

/** OPFS directory the data is imported into. */
export const NAMESPACE = 'openballance-data';
/** Written last: an import without it is incomplete. Hidden, so the game never sees it. */
const MARKER = '.openballance-import.json';
/** Folders of the installed game that hold program code (the original's DLLs), not data: skipped. */
const CODE_DIRS = ['BIN', 'BUILDINGBLOCKS', 'MANAGERS', 'PLUGINS', 'RENDERENGINES', 'HELP'];
/** What an installed folder must have (src/iscab.c: vfs_find_game looks for base.cmo). */
const INSTALLED_REQUIRED = ['base.cmo', '3D Entities/Menu.nmo', '3D Entities/Level/Level_01.NMO'];
const INSTALLED_DIRS = ['3D Entities', 'Textures', 'Sounds'];
/** The InstallShield cabinets on the CD (src/iscab.c). */
const CABINET = /^data\d*\.(hdr|cab)$/i;

const upper = (s) => s.replace(/[a-z]/g, (c) => c.toUpperCase());
export const mb = (n) => (n >= 1 << 30 ? `${(n / 2 ** 30).toFixed(1)} GB` : `${(n / 1048576).toFixed(n < 10485760 ? 1 : 0)} MB`);

// ---- picking --------------------------------------------------------------------------

/** Folder from showDirectoryPicker() (Chromium). */
export async function folderFromHandle(handle) {
  return checkFolder(handle.name, await directoryHandleEntries(handle));
}

/** Folder from <input type="file" webkitdirectory> (all browsers). */
export function folderFromFileList(files) {
  const name = files[0]?.webkitRelativePath.split('/')[0] || 'folder';
  return checkFolder(name, fileListEntries(files));
}

/** The shortest path ending in `file` (case-insensitive) at most `depth` folders down, or undefined. */
function locate(entries, test, depth) {
  return entries
    .map(([p]) => p)
    .filter((p) => test(p.split('/').pop()) && p.split('/').length <= depth + 1)
    .sort((a, b) => a.length - b.length)[0];
}
const dirOf = (p) => (p.includes('/') ? p.slice(0, p.lastIndexOf('/') + 1) : '');

/**
 * Keep what the game needs and check it. Accepts the installed game folder (base.cmo) or the CD
 * (Setup/data1.hdr), the folder itself or one level above it. Returns { source, problems, files, bytes }.
 */
export function checkFolder(name, entries) {
  const problems = [];
  let kept = [], root = '', layout = null;
  const base = locate(entries, (f) => upper(f) === 'BASE.CMO', 1);
  const hdr = base ? undefined : locate(entries, (f) => upper(f) === 'DATA1.HDR', 2);
  if (base) {
    layout = 'installed';
    root = dirOf(base);
    for (const [path, file] of entries) {
      if (!path.startsWith(root)) continue;
      const rel = path.slice(root.length), segs = rel.split('/');
      if (segs.length > 1 && CODE_DIRS.includes(upper(segs[0]))) continue;
      if (segs.length === 1 && /\.(exe|dll|ico|inf)$/i.test(rel)) continue;
      kept.push([rel, file]);
    }
    const byName = new Set(kept.map(([p]) => upper(p)));
    for (const req of INSTALLED_REQUIRED) if (!byName.has(upper(req))) problems.push(`${req} is missing`);
    for (const d of INSTALLED_DIRS) {
      if (![...byName].some((p) => p.startsWith(`${upper(d)}/`))) problems.push(`the ${d} folder is missing or empty`);
    }
  } else if (hdr) {
    layout = 'cd';
    // data1.hdr sits in Setup/ on the CD; keep that folder name so the game finds Setup/data1.hdr.
    const setup = dirOf(hdr);                          // "Setup/", "Ballance/Setup/" or "" (Setup picked)
    root = upper(setup).endsWith('SETUP/') ? setup.slice(0, setup.length - 'Setup/'.length) : setup;
    for (const [path, file] of entries) {
      if (!path.startsWith(setup)) continue;
      const rel = path.slice(setup.length);
      if (rel.includes('/') || !CABINET.test(rel)) continue;
      kept.push([`${setup.slice(root.length)}${rel}`, file]);
    }
    const names = new Set(kept.map(([p]) => upper(p.split('/').pop())));
    for (const f of ['DATA1.HDR', 'DATA1.CAB', 'DATA2.CAB']) if (!names.has(f)) problems.push(`Setup/${f.toLowerCase()} is missing`);
  } else {
    problems.push('neither base.cmo (the installed game) nor Setup/data1.hdr (the Ballance CD) is in this folder');
  }
  const bytes = kept.reduce((n, [, f]) => n + f.size, 0);
  const shown = root ? `${name}/${root.slice(0, -1)}` : name;
  return { source: { kind: 'folder', layout, name: shown, entries: kept }, problems, files: kept.length, bytes };
}

/** An .iso or raw .bin: an ISO 9660 file system with the Setup folder in its root directory. */
export async function checkImage(file) {
  const problems = [];
  const source = { kind: 'image', name: file.name, file };
  const result = { source, problems, files: 1, bytes: file.size };
  if (/\.cue$/i.test(file.name)) {
    problems.push('this is the cue sheet: choose the .bin file it names');
    return result;
  }
  if (file.size >= 2 ** 32) {
    problems.push('the image is larger than 4 GB; the Ballance CD is about 115 MB');
    return result;
  }
  const read = async (off, len) => new Uint8Array(await file.slice(off, off + len).arrayBuffer());
  // ISO 9660: the primary volume descriptor is sector 16. Cooked .iso: 2048-byte sectors; raw .bin:
  // 2352-byte sectors with 16 (MODE1) or 24 (MODE2 form 1) bytes before the 2048 data bytes (src/vfs.c).
  let layout = null;
  for (const [size, head] of [[2048, 0], [2352, 16], [2352, 24]]) {
    const pvd = await read(16 * size + head, 2048);
    if (pvd[0] === 1 && String.fromCharCode(...pvd.subarray(1, 6)) === 'CD001') { layout = { size, head, pvd }; break; }
  }
  if (!layout) {
    problems.push('not an ISO 9660 disc image (.iso, or a raw MODE1/2352 .bin)');
    return result;
  }
  const u32 = (b, o) => b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] * 16777216);
  const sectors = async (lba, n) => {
    const out = new Uint8Array(n * 2048);
    for (let i = 0; i < n; i++) out.set(await read((lba + i) * layout.size + layout.head, 2048), i * 2048);
    return out;
  };
  const listDir = async (rec) => {
    const dir = await sectors(u32(rec, 2), Math.min(8, Math.ceil(u32(rec, 10) / 2048)));
    const names = new Map();
    for (let p = 0; p < dir.length;) {
      const len = dir[p];
      if (!len) { p = (Math.floor(p / 2048) + 1) * 2048; continue; }
      const name = String.fromCharCode(...dir.subarray(p + 33, p + 33 + dir[p + 32])).replace(/;1$/, '');
      names.set(upper(name), { rec: dir.subarray(p, p + len), size: u32(dir, p + 10), isDir: (dir[p + 25] & 2) !== 0 });
      p += len;
    }
    return names;
  };
  const root = await listDir(layout.pvd.subarray(156));
  const setup = root.get('SETUP');
  if (!setup?.isDir) {
    problems.push('the Setup folder is not on this disc (is it the Ballance CD?)');
    return result;
  }
  const files = await listDir(setup.rec);
  for (const f of ['DATA1.HDR', 'DATA1.CAB', 'DATA2.CAB']) if (!files.has(f)) problems.push(`Setup/${f.toLowerCase()} is not on this disc`);
  return result;
}

// ---- OPFS import ----------------------------------------------------------------------

/** The completed import's description ({ kind, layout, name, files, bytes, date }), or null. */
export async function existingImport() {
  try {
    const root = await navigator.storage.getDirectory();
    const dir = await root.getDirectoryHandle(NAMESPACE);
    const f = await (await dir.getFileHandle(MARKER)).getFile();
    return JSON.parse(await f.text());
  } catch {
    return null;
  }
}

export async function removeImport() {
  await clearNamespace(NAMESPACE);
  try { await (await navigator.storage.getDirectory()).removeEntry(NAMESPACE, { recursive: true }); } catch {}
}

/** Ask the browser to keep the data under storage pressure. Firefox may never answer a prompt. */
export async function requestPersist() {
  return persist({ signal: AbortSignal.timeout(10000) }).catch(() => false);
}

export async function storageInfo() {
  const est = await navigator.storage?.estimate?.().catch(() => null);
  const persisted = await navigator.storage?.persisted?.().catch(() => false);
  return { usage: est?.usage, quota: est?.quota, persisted: !!persisted };
}

/**
 * Copy a checked source into OPFS (csfs writes each file through a stream, so a large file never sits
 * in memory). onProgress({ bytes, total, file, files, done }).
 */
export async function importSource(source, onProgress = () => {}) {
  const items = source.kind === 'image' ? [['rom', source.file]] : source.entries;
  const total = items.reduce((n, [, f]) => n + f.size, 0);
  await persist({ signal: AbortSignal.timeout(3000) }).catch(() => false);
  await removeImport();
  const fs = await opfsFileSystem({ namespace: NAMESPACE });
  let bytes = 0, last = 0;
  for (let i = 0; i < items.length; i++) {
    const [path, file] = items[i];
    const counted = file.stream().pipeThrough(new TransformStream({
      transform(chunk, ctrl) {
        bytes += chunk.byteLength;
        const now = performance.now();
        if (now - last > 100) { last = now; onProgress({ bytes, total, file: path, files: items.length, done: i }); }
        ctrl.enqueue(chunk);
      },
    }));
    await fs.write(`/${path}`, counted);
    onProgress({ bytes, total, file: path, files: items.length, done: i + 1 });
  }
  const info = { kind: source.kind, layout: source.layout ?? null, name: source.name, files: items.length, bytes: total, date: new Date().toISOString() };
  await fs.write(`/${MARKER}`, new TextEncoder().encode(JSON.stringify(info)));
  return info;
}

// ---- running --------------------------------------------------------------------------

/** gasm Worker-mode asset specs for a source ('opfs' = the imported data). */
export function assetSpecs(source) {
  if (source === 'opfs') return [{ kind: 'opfs', dir: NAMESPACE }];
  if (source.kind === 'image') return [{ kind: 'files', entries: [['rom', source.file]] }];
  return [{ kind: 'files', entries: source.entries }];
}

/** [name, File] entries of a source, for the main-thread runner (which preloads them into memory). */
export async function sourceEntries(source) {
  if (source === 'opfs') {
    const dir = await (await navigator.storage.getDirectory()).getDirectoryHandle(NAMESPACE);
    return directoryHandleEntries(dir);           // hidden files (the marker) are skipped
  }
  if (source.kind === 'image') return [['rom', source.file]];
  return source.entries;
}
