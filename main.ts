// The harness driver. Fastest path first: load the native engine
// (engine.node) directly and let it read cases.json, build the
// whole site set, write answers.json and end the process itself
// with SYS_exit_group, so neither solve.js's module graph nor
// Node's teardown costs anything on the clock. The addon returns
// only when that path could not run; then solve.js is imported and
// the same job goes through runAllBytes (the native bytes path, or
// the WebAssembly fallback behind solve.js). Outside a container
// MAKEFUN_INPUT_DIR and MAKEFUN_OUTPUT_DIR stand in for /input
// and /output.
//
// No V8 flags are set here on purpose: the house baseline's
// --no-incremental-marking measurably hurts this entry (it roughly
// triples the engine's cold-run CPU), so the driver leaves V8 alone.

import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { createRequire } from 'node:module';

try {
  const require = createRequire(import.meta.url);
  const addon = require('./engine.node') as {
    runCli?: () => number;
  };
  if (addon && typeof addon.runCli === 'function') {
    // Returns only on failure; on success the process has already
    // exited from inside the addon with answers.json written.
    addon.runCli();
  }
} catch {
  // No usable native addon: fall through to solve.js.
}

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
