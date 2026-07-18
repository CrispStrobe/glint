#!/usr/bin/env bash
# Build a WebAssembly module exposing glint's one-shot codec API for use in
# browsers / webviews (e.g. the CrispAudio Tauri+web app). Produces
# glint.mjs (ES6 loader) + glint.wasm in this directory.
set -euo pipefail
cd "$(dirname "$0")/../.."   # repo root
SRC=$(sed -n '/set(GLINT_SOURCES/,/)/p' CMakeLists.txt | grep -oE 'src/[a-z0-9_]+\.cpp')
emcc \
  -std=c++17 -O3 -flto \
  -I include -I src \
  $SRC \
  -o bindings/wasm/glint.mjs \
  -sMODULARIZE=1 -sEXPORT_ES6=1 \
  -sALLOW_MEMORY_GROWTH=1 \
  -sINITIAL_MEMORY=33554432 \
  -sSTACK_SIZE=2097152 \
  -sENVIRONMENT='web,worker,node' \
  -sEXPORTED_FUNCTIONS='["_glint_encode_audio","_glint_decode_audio","_glint_vorbis_decode","_glint_wav_read","_glint_wav_write","_glint_free","_malloc","_free"]' \
  -sEXPORTED_RUNTIME_METHODS='["cwrap","getValue","setValue","HEAPU8","HEAPF32","HEAP32"]' \
  -sEXPORT_NAME=createGlint
echo "built bindings/wasm/glint.mjs + glint.wasm"; ls -la bindings/wasm/glint.wasm
