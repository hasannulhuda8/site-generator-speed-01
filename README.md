# site-generator-speed-01 — a native engine entry

Built for make.fun's "Build a static site faster" bounty:
https://make.fun/bounty/site-generator-speed-01-run-20261122

[![competition](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/hasannulhuda8/site-generator-speed-01/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/hasannulhuda8/site-generator-speed-01/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)

A from-scratch reimplementation of the makefun-site 1.0.0 build,
written in C once and compiled two ways from the same `engine.c`:
as a native Node-API addon (`engine.node`, the fast path) and as a
WebAssembly module (`engine.wasm`, the fallback). The goal is the
smallest possible CPU time from cold process start to a
byte-identical answers.json.

## Layout

- `engine.c` — the whole generator: a targeted JSON parser for
  cases.json, the front-matter parser, the Markdown renderer, the
  template engine (lexer, parser, renderer), permalinks and slugs
  (table-driven NFKD decomposition generated from Node's own
  `String.normalize`), collections, pagination, feeds, sitemap and
  the link-check report. It brings its own bump arena; the native
  build backs it with one lazily-paged mmap reservation, the wasm
  build with the module's linear memory.
- `addon.c` — the Node-API face of the native build: `runAll`
  (bytes in, bytes out) and `runCli`, which reads cases.json,
  builds, writes answers.json and ends the process with the raw
  `SYS_exit_group` syscall, skipping Node's teardown entirely.
- `engine.node` — the native addon: gcc `-O3`, profile-guided
  (trained on the practice set and fuzz cases), SIMD scans (AVX2
  where the CPU offers it, SSE2 otherwise, chosen at runtime), and
  a version script so it exports only `napi_register_module_v1`.
  Rebuild with `./build_pgo.sh`.
- `engine.wasm` — the same source compiled with zig cc
  (`-target wasm32-freestanding -Os -msimd128`, stripped), used
  automatically when the addon cannot load. Rebuild with
  `./build.sh`.
- `solve.js` — the public face: prefers the native engine,
  falls back to `solve_wasm.js` + `engine.wasm`. Both paths
  produce byte-identical output.
- `main.ts` — the harness driver: loads the native addon
  directly and hands it the whole job when available, otherwise
  reads cases.json as raw bytes and writes the answers.json bytes
  the engine returns.
  It never parses, re-encodes or serialises anything in
  JavaScript, and it sets no V8 flags (the baseline's
  `--no-incremental-marking` measurably hurts this entry's
  cold run).

## How it stays fast

- Native execution with profile-guided optimisation, and an exit
  through `SYS_exit_group` the moment answers.json is on disk:
  no wasm boundary, no Liftoff compile, no V8 teardown.
- One pass end to end: the engine parses cases.json itself, builds
  every case, and assembles the complete answers.json with values
  already in JSON-escaped form, so the driver is a single read, a
  single call and a single write.
- Byte domain throughout: rendering writes into growable byte
  buffers; nothing round-trips through JS strings. File contents
  and template sources are used as zero-copy views into the input.
- SIMD scans (AVX2/SSE2 natively, SIMD128 in wasm) for the hot
  delimiter searches: HTML escaping, JSON string scanning on
  input, JSON escaping on output, and inline-markup skipping.
- Templates are compiled once per case to a flat bytecode
  program; the executor walks it sequentially with an explicit
  program counter and a loop-state stack instead of recursing
  through pointer-linked node lists.
- The CLI path reads cases.json straight into the engine's
  memory reservation: the input is never copied.
- Table-driven slug decomposition instead of ICU normalisation,
  open-addressing hash maps for collections, tags, slug caches and
  dedupe sets, cached card values, and a per-case arena reset so a
  case's scratch memory is reused instead of re-allocated.

## Correctness

Output is byte-identical to the reference build on both engines:
the practice set passes 3/3, randomised fuzz runs against the kit
oracle match on every file (300+ cases across builds), and the
kit's grade-local regrade reproduces the entry byte for byte.
