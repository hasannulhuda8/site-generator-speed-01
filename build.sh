#!/bin/sh
# Build engine.wasm from engine.c (freestanding, SIMD128 enabled,
# stripped: the name section alone costs real cold-compile CPU).
exec ~/workspace/tools/zig-linux-x86_64-0.13.0/zig cc \
  -target wasm32-freestanding -nostdlib -Os -msimd128 \
  -Wl,--no-entry -Wl,--export-memory -Wl,--strip-all \
  engine.c -o engine.wasm
