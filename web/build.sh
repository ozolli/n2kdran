#!/bin/sh
# Compile libn2k + n2k_web.c en WebAssembly (web/n2k.js + web/n2k.wasm).
# Prérequis : emscripten (emsdk) ; EMSDK_ENV peut pointer vers emsdk_env.sh.
set -e
cd "$(dirname "$0")"
# emcc du PATH, sinon celui d'emsdk (~/emsdk, ou EMSDK=…) avec sa configuration.
EMCC=emcc
if ! command -v emcc >/dev/null 2>&1; then
    EMSDK="${EMSDK:-$HOME/emsdk}"
    export EM_CONFIG="$EMSDK/.emscripten"
    EMCC="$EMSDK/upstream/emscripten/emcc"
fi
"$EMCC" -O2 -std=c11 -Wall -Wextra \
    -I ../libn2k/include \
    ../libn2k/src/ydraw.c ../libn2k/src/n2k.c ../libn2k/src/decode.c \
    ../libn2k/src/polar.c ../libn2k/src/derive.c ../libn2k/src/ais.c \
    n2k_web.c \
    -s MODULARIZE=1 -s EXPORT_NAME=N2kModule -s ENVIRONMENT=web \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s EXPORTED_RUNTIME_METHODS='["UTF8ToString","HEAPU8","HEAPF64","HEAPU32"]' \
    -o n2k.js
echo "web/n2k.js + web/n2k.wasm"
