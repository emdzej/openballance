#!/usr/bin/env bash
# Download what the gasm build (openballance.wasm) needs into .deps/: wasi-sdk (clang + wasi-libc for wasm32)
# and gasm's C SDK (gasm.h + CMake toolchain). Used by CI; locally a gasm checkout works as well.
#   tools/fetch-gasm-sdk.sh            -> .deps/wasi-sdk, .deps/gasm-c-sdk
# Then:
#   cmake -S . -B build-gasm -DOPENBALLANCE_PLATFORM=gasm -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=.deps/gasm-c-sdk/cmake/gasm-toolchain.cmake -DWASI_SDK_PREFIX="$PWD/.deps/wasi-sdk"
#   tools/fetch-gasm-sdk.sh --version  -> prints GASM_VERSION and exits (tools/fetch-gasm-runner.sh, release jobs)
set -euo pipefail
cd "$(dirname "$0")/.."
WASI_SDK_VERSION=${WASI_SDK_VERSION:-34}
# The one place the gasm version is set (CI, release and Pages builds all call this script; the gasm
# bundles' runners too, via --version).
# Keep it in step with @emdzej/gasm-host in docs/package.json (the browser player's runner).
# 0.5.0 is the minimum: OpenBallance reads the raw keyboard and pointer (input_mode, key_state, pointer),
# renders with gasm:gfx textures, samplers, explicit bind group layouts,
# dynamic offsets and viewports, reads typed text (highscore names) and needs gfx in Worker mode (browser).
GASM_VERSION=${GASM_VERSION:-0.5.0}
if [ "${1:-}" = --version ]; then echo "$GASM_VERSION"; exit 0; fi
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64)  PLAT=arm64-macos ;;
  Darwin-x86_64) PLAT=x86_64-macos ;;
  Linux-x86_64)  PLAT=x86_64-linux ;;
  Linux-aarch64) PLAT=arm64-linux ;;
  *) echo "unsupported host $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac
mkdir -p .deps
if [ ! -x .deps/wasi-sdk/bin/clang ]; then
  url=https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-$WASI_SDK_VERSION/wasi-sdk-$WASI_SDK_VERSION.0-$PLAT.tar.gz
  echo "fetching $url"
  curl -fsSL "$url" | tar xz -C .deps
  rm -rf .deps/wasi-sdk && mv ".deps/wasi-sdk-$WASI_SDK_VERSION.0-$PLAT" .deps/wasi-sdk
  if [ "$(uname -s)" = Darwin ]; then xattr -dr com.apple.quarantine .deps/wasi-sdk 2>/dev/null || true; fi
fi
if [ ! -f .deps/gasm-c-sdk/include/gasm.h ]; then
  url=https://github.com/emdzej/gasm/releases/download/$GASM_VERSION/gasm-c-sdk-$GASM_VERSION.zip
  echo "fetching $url"
  curl -fsSL -o .deps/gasm-c-sdk.zip "$url"
  rm -rf .deps/gasm-c-sdk ".deps/gasm-c-sdk-$GASM_VERSION"
  (cd .deps && unzip -q gasm-c-sdk.zip && mv "gasm-c-sdk-$GASM_VERSION" gasm-c-sdk && rm gasm-c-sdk.zip)
fi
.deps/wasi-sdk/bin/clang --version | head -1
echo "gasm C SDK $GASM_VERSION: .deps/gasm-c-sdk"
