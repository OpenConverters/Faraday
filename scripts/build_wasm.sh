#!/usr/bin/env bash
# Build the Faraday WASM engine into web/public/ (faraday.js + faraday.wasm).
#   env: EMSDK_ENV (default /home/alf/emsdk/emsdk_env.sh)
# -sDYNAMIC_EXECUTION=0: embind falls back to eval() without it, which the
# OpenConverters SPA CSP (script-src 'self' 'wasm-unsafe-eval') blocks —
# the engine would die at load on prod while vite preview passes.
set -euo pipefail
cd "$(dirname "$0")/.."

EMSDK_ENV="${EMSDK_ENV:-/home/alf/emsdk/emsdk_env.sh}"
[ -f "$EMSDK_ENV" ] || { echo "emsdk env not found: $EMSDK_ENV (set EMSDK_ENV)" >&2; exit 1; }
source "$EMSDK_ENV" >/dev/null

# nlohmann/json single-include: reuse the native build's FetchContent checkout
JSON_INC="build/_deps/json-src/include"
[ -d "$JSON_INC" ] || { echo "nlohmann json not found at $JSON_INC — run the native cmake configure first" >&2; exit 1; }

mkdir -p web/public
em++ -O2 -std=c++20 -fwasm-exceptions --bind \
  -I cpp/include -I "$JSON_INC" \
  -sDYNAMIC_EXECUTION=0 \
  -sMODULARIZE=1 -sEXPORT_NAME=createFaraday -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=2097152 \
  cpp/bindings/wasm.cpp \
  -o web/public/faraday.js
echo "built web/public/faraday.js ($(du -h web/public/faraday.wasm | cut -f1) wasm)"
