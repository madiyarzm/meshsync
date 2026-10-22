#!/bin/sh
# Rebuilds meshstate_wasm.js: the meshstate library + the simulator as WebAssembly, packed into one
# file so index.html works offline and straight from disk. Needs Emscripten (brew install emscripten).
set -e
cd "$(dirname "$0")"
LIB=../../firmware/src/meshstate
em++ -O2 -std=c++17 -ffp-contract=off -I "$LIB" \
  "$LIB/meshstate.cpp" "$LIB/replica.cpp" "$LIB/models.cpp" "$LIB/sync.cpp" playground.cpp \
  -o meshstate_wasm.js \
  -sMODULARIZE=1 -sEXPORT_NAME=createMeshstate -sSINGLE_FILE=1 -sENVIRONMENT=web \
  -sINITIAL_MEMORY=134217728 -sSTACK_SIZE=1048576 \
  -sEXPORTED_FUNCTIONS=_malloc,_free \
  -sEXPORTED_RUNTIME_METHODS=cwrap,HEAP32
echo "built $(pwd)/meshstate_wasm.js"
