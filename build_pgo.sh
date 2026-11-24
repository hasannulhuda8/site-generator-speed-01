#!/bin/sh
# Build both engines from the one engine.c, with a PGO pass on the
# native addon: instrument, train on the practice set and a fuzz
# batch, then rebuild with the profile. The addon exports only the
# Node-API registration symbol.
set -e
cd "$(dirname "$0")"
rm -rf pgo && mkdir -p pgo
gcc -O3 -msse2 -fPIC -shared -DNATIVE_BUILD -fprofile-generate=$PWD/pgo \
    -I/usr/include/node engine.c addon.c -o engine.node
(cd ~/workspace/makefun/site-gen && \
  node cmp6.mjs >/dev/null 2>&1; \
  node fuzz6.mjs 10 >/dev/null 2>&1; true)
gcc -O3 -msse2 -fPIC -shared -DNATIVE_BUILD -fprofile-use=$PWD/pgo \
    -fprofile-correction -I/usr/include/node engine.c addon.c \
    -Wl,--version-script=exports.map -s -o engine.node
ZIG="${ZIG:-$HOME/workspace/tools/zig-linux-x86_64-0.13.0/zig}"
"$ZIG" cc -target wasm32-freestanding -nostdlib -Os -msimd128 \
    -Wl,--no-entry -Wl,--export-memory -Wl,--strip-all \
    engine.c -o engine.wasm
ls -la engine.node engine.wasm
