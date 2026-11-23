// The harness driver: hand the raw cases.json bytes to the WebAssembly
// engine behind solve.js and write the answers.json bytes it returns.
// The engine parses the JSON and renders every case itself, so the
// driver does no parsing, encoding or serialising of its own. Outside
// a container MAKEFUN_INPUT_DIR and MAKEFUN_OUTPUT_DIR stand in for
// /input and /output.
//
// No V8 flags are set here on purpose: the house baseline's
// --no-incremental-marking measurably hurts this entry (it roughly
// triples the engine's cold-run CPU), so the driver leaves V8 alone.

import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

/** solve.js, imported by its URL (a .js module carries no types). */
const { runAllBytes } = (await import(new URL('./solve.js', import.meta.url).href)) as {
  runAllBytes: (casesBytes: Uint8Array) => Uint8Array;
};

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

const casesBytes = readFileSync(join(INPUT_DIR, 'cases.json'));
writeFileSync(join(OUTPUT_DIR, 'answers.json'), runAllBytes(casesBytes));
