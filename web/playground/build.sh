#!/bin/sh
# Rebuilds meshstate_wasm.js: the meshstate library + the simulator + the face-mode protocol as
# WebAssembly, packed into one file so the pages work offline. Used by this playground and by the
# face page's simulated hub (web/index.html). Needs Emscripten (brew install emscripten).
set -e
cd "$(dirname "$0")"
LIB=../../firmware/src/meshstate
em++ -O2 -std=c++17 -ffp-contract=off -I "$LIB" -I ../../firmware \
  "$LIB/meshstate.cpp" "$LIB/replica.cpp" "$LIB/models.cpp" "$LIB/sync.cpp" ../../firmware/face_app.cpp playground.cpp \
  -o meshstate_wasm.js \
  -sMODULARIZE=1 -sEXPORT_NAME=createMeshstate -sSINGLE_FILE=1 -sENVIRONMENT=web \
  -sINITIAL_MEMORY=134217728 -sSTACK_SIZE=1048576 \
  -sEXPORTED_FUNCTIONS=_malloc,_free \
  -sEXPORTED_RUNTIME_METHODS=cwrap,HEAP32,UTF8ToString
echo "built $(pwd)/meshstate_wasm.js"
