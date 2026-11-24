// solve.js - the entry's public face.
//
// The fast path is the native engine (engine.node, a Node-API addon
// built from the same engine.c): runAll hands it the raw cases.json
// bytes and gets the answers.json bytes back, and runCli lets it do
// the whole driver job (read, build, write, exit) without Node's
// teardown ever running. If the addon cannot load on this machine,
// everything transparently falls back to the WebAssembly build of
// the same source (solve_wasm.js + engine.wasm), which produces
// byte-identical output.

import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);

let native = null;
try {
  native = require('./engine.node');
} catch {
  native = null;
}

let impl;
if (native) {
  const encoder = new TextEncoder();
  const decoder = new TextDecoder();
  impl = {
    runAllBytes: (casesBytes) => native.runAll(casesBytes),
    run: (input) => {
      const bytes = encoder.encode(
        JSON.stringify([{ id: 'case', input }]),
      );
      return JSON.parse(decoder.decode(native.runAll(bytes)))[0];
    },
  };
} else {
  impl = await import('./solve_wasm.js');
}

export const run = impl.run;
export const runAllBytes = impl.runAllBytes;
// Present only when the native engine loaded: runCli() returns -1 if
// the CLI path could not run (the caller then does the ordinary
// flow); on success it never returns.
export const runCli = native ? () => native.runCli() : null;
