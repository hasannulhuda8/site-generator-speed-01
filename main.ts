// The harness driver. Fast path first: when the native engine
// (engine.node) is loaded, it reads cases.json, builds the whole
// site set, writes answers.json and ends the process itself with
// SYS_exit_group, so Node's teardown never runs. It returns only
// when that path could not run; then the ordinary flow below does
// the same job through runAllBytes (native bytes path, or the
// WebAssembly fallback behind solve.js). Outside a container
// MAKEFUN_INPUT_DIR and MAKEFUN_OUTPUT_DIR stand in for /input
// and /output.
//
// No V8 flags are set here on purpose: the house baseline's
// --no-incremental-marking measurably hurts this entry (it roughly
// triples the engine's cold-run CPU), so the driver leaves V8 alone.

import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

/** solve.js, imported by its URL (a .js module carries no types). */
const solve = (await import(new URL('./solve.js', import.meta.url).href)) as {
  runAllBytes: (casesBytes: Uint8Array) => Uint8Array;
  runCli: (() => number) | null;
};

if (solve.runCli) {
  // Returns only on failure; on success the process has already
  // exited from inside the addon with answers.json written.
  solve.runCli();
}

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

const casesBytes = readFileSync(join(INPUT_DIR, 'cases.json'));
writeFileSync(join(OUTPUT_DIR, 'answers.json'), solve.runAllBytes(casesBytes));
