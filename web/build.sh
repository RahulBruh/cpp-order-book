#!/usr/bin/env bash
# Builds web/orderbook.wasm from wasm_api.cpp.
#
# Needs clang, wasm-ld and a WASI sysroot with libc++. On Ubuntu 24.04:
#   sudo apt-get install clang lld wasi-libc libc++-18-dev-wasm32 libclang-rt-18-dev-wasm32
# With wasi-sdk instead, set CXX=$WASI_SDK/bin/clang++ and WASI_SYSROOT=$WASI_SDK/share/wasi-sysroot.
set -euo pipefail
cd "$(dirname "$0")"
CXX="${CXX:-clang++}"
SYSROOT_FLAG=()
[[ -n "${WASI_SYSROOT:-}" ]] && SYSROOT_FLAG=(--sysroot="$WASI_SYSROOT")

"$CXX" --target=wasm32-wasi "${SYSROOT_FLAG[@]}" \
  -std=c++20 -O3 -fignore-exceptions -fno-rtti -DNDEBUG -I ../include \
  -mexec-model=reactor -Wl,--strip-all -Wl,-z,stack-size=1048576 \
  -o orderbook.wasm wasm_api.cpp

echo "built web/orderbook.wasm ($(wc -c < orderbook.wasm) bytes)"
