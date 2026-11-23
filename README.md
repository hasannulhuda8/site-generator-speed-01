# site-generator-speed-01 — a WebAssembly engine entry

A from-scratch reimplementation of the makefun-site 1.0.0 build,
written in C and compiled to WebAssembly, with a thin JavaScript
shell. The goal is the smallest possible CPU time from cold process
start to a byte-identical answers.json.

## Layout

- `engine.c` — the whole generator: a targeted JSON parser for
  cases.json, the front-matter parser, the Markdown renderer, the
  template engine (lexer, parser, renderer), permalinks and slugs
  (table-driven NFKD decomposition generated from Node's own
  `String.normalize`), collections, pagination, feeds, sitemap and
  the link-check report. Freestanding: it brings its own memory
  functions and a bump arena over the module's linear memory.
- `engine.wasm` — `engine.c` compiled with zig cc
  (`-target wasm32-freestanding -O3 -msimd128`, stripped). Rebuild
  with `./build.sh`.
- `solve.js` — loads the module once, packs a case into linear
  memory for `run(input)`, and exposes `runAllBytes(casesBytes)`
  for the driver path.
- `main.ts` — the harness driver: reads cases.json as raw bytes,
  hands them straight to the engine, and writes the answers.json
  bytes the engine returns. It never parses, re-encodes or
  serialises anything in JavaScript, and it sets no V8 flags (the
  baseline's `--no-incremental-marking` measurably hurts this
  entry's cold run).

## How it stays fast

- One pass end to end: the engine parses cases.json itself, builds
  every case, and assembles the complete answers.json in linear
  memory with values already in JSON-escaped form, so the driver is
  a single read, a single call and a single write.
- Byte domain throughout: rendering writes into growable byte
  buffers; nothing round-trips through JS strings.
- SIMD128 scans (16 bytes per step) for the hot delimiter searches:
  HTML escaping, JSON string scanning on input, and JSON escaping
  on output.
- Table-driven slug decomposition instead of ICU normalisation,
  open-addressing hash maps for collections, tags, slug caches and
  dedupe sets, and a per-case arena reset so a case's scratch
  memory is reused instead of re-allocated.

## Correctness

Output is byte-identical to the reference build: the practice set
passes 3/3, a 54-case randomised fuzz run against the kit oracle
matches on every file, and the kit's grade-local regrade reproduces
the entry byte for byte.
