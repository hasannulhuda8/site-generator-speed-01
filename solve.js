// A fast drop-in replacement for makefun-site 1.0.0, powered by a
// freestanding WebAssembly engine (engine.wasm, built from engine.c).
//
// run(input) returns exactly the output tree makefun-site's build(input)
// returns, path for path and byte for byte. The case is packed into the
// module's linear memory as length-prefixed UTF-8 strings; the engine
// renders everything in the byte domain and serialises the tree back,
// either as a length-prefixed table (for run) or as a JSON object whose
// values are already in JSON-escaped byte form (for the driver, which
// splices the fragments into answers.json without re-encoding anything).

import { readFileSync } from 'node:fs';

const wasmBytes = readFileSync(new URL('./engine.wasm', import.meta.url));
const instance = new WebAssembly.Instance(new WebAssembly.Module(wasmBytes), {});
const memory = instance.exports.memory;
const buildEntry = instance.exports.build_entry;

const PACK_OFF = 16 << 20;
const enc = new TextEncoder();
const dec = new TextDecoder();

const byCodeUnits = (a, b) => (a < b ? -1 : a > b ? 1 : 0);

function ensureCapacity(n) {
  const have = memory.buffer.byteLength;
  if (have < n) memory.grow(Math.ceil((n - have) / 65536));
}

/** One case packed: counts and length-prefixed UTF-8 strings. */
function packCase(input) {
  const chunks = [];
  let len = 0;
  const pushU32 = (v) => {
    const b = new Uint8Array(4);
    new DataView(b.buffer).setUint32(0, v >>> 0, true);
    chunks.push(b);
    len += 4;
  };
  const pushStr = (s) => {
    const b = enc.encode(s);
    pushU32(b.length);
    chunks.push(b);
    len += b.length;
  };
  const tplNames = Object.keys(input.templates).sort(byCodeUnits);
  pushU32(tplNames.length);
  for (const name of tplNames) {
    pushStr(name);
    pushStr(input.templates[name]);
  }
  pushStr(input.site.title);
  pushStr(input.site.base_url);
  pushU32(input.site.per_page);
  pushU32(input.site.recent);
  const files = input.files.slice().sort((x, y) => byCodeUnits(x.path, y.path));
  pushU32(files.length);
  for (const f of files) {
    pushStr(f.path);
    pushStr(f.content);
  }
  const pack = new Uint8Array(len);
  let o = 0;
  for (const c of chunks) {
    pack.set(c, o);
    o += c.length;
  }
  return pack;
}

/** Run the engine over one case; returns a copy of its output bytes. */
function callEngine(input, mode) {
  const pack = packCase(input);
  ensureCapacity(PACK_OFF + pack.length + (4 << 20));
  new Uint8Array(memory.buffer).set(pack, PACK_OFF);
  buildEntry(PACK_OFF, pack.length, mode);
  const view = new DataView(memory.buffer);
  const outPtr = view.getUint32(0, true);
  const outLen = view.getUint32(4, true);
  return new Uint8Array(memory.buffer, outPtr, outLen).slice();
}

export function run(input) {
  const table = callEngine(input, 0);
  const view = new DataView(table.buffer, table.byteOffset, table.byteLength);
  let o = 0;
  const readU32 = () => {
    const v = view.getUint32(o, true);
    o += 4;
    return v;
  };
  const count = readU32();
  const out = {};
  for (let i = 0; i < count; i++) {
    const pl = readU32();
    const path = dec.decode(table.subarray(o, o + pl));
    o += pl;
    const tl = readU32();
    const text = dec.decode(table.subarray(o, o + tl));
    o += tl;
    out[path] = text;
  }
  return out;
}

/** The case's answer as a JSON object string, values already escaped. */
export function runCaseJson(input) {
  return dec.decode(callEngine(input, 1));
}

const buildAllEntry = instance.exports.build_all_entry;

/**
 * The whole answers.json for a cases.json file's bytes: the engine
 * parses the JSON itself and returns the finished array bytes.
 */
export function runAllBytes(casesBytes) {
  ensureCapacity(PACK_OFF + casesBytes.length + (16 << 20));
  new Uint8Array(memory.buffer).set(casesBytes, PACK_OFF);
  buildAllEntry(PACK_OFF, casesBytes.length);
  const view = new DataView(memory.buffer);
  const outPtr = view.getUint32(0, true);
  const outLen = view.getUint32(4, true);
  return new Uint8Array(memory.buffer, outPtr, outLen);
}
