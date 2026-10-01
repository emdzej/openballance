# Build from source

You need CMake 3.20+, a C11 compiler and zlib (native build), plus wasi-sdk and the gasm C SDK for
`openballance.wasm`.

## The gasm module

```sh
tools/fetch-gasm-sdk.sh        # wasi-sdk and the gasm 0.5.0 C SDK into .deps/
cmake -S . -B build-gasm -DOPENBALLANCE_PLATFORM=gasm -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=.deps/gasm-c-sdk/cmake/gasm-toolchain.cmake -DWASI_SDK_PREFIX="$PWD/.deps/wasi-sdk"
cmake --build build-gasm -j
```

The result is `build-gasm/openballance.wasm`. Run it with `gasm-run` ([Running on gasm](/guide/running));
`tools/fetch-gasm-runner.sh <platform>` downloads a released runner.

## The native tools and tests

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This builds the engine as a library with a null GPU backend, plus the test programs (`ck_test`,
`run_test`, `cab_test`). See [Run the tests](/howto/tests).

## The site

```sh
cd docs
pnpm install
scripts/vendor-web.sh                              # gasm's web host into public/play/vendor
scripts/copy-wasm.sh ../build-gasm/openballance.wasm
pnpm dev
```
