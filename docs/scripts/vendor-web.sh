#!/usr/bin/env bash
# Copy the browser runtime of the play page from the pinned npm packages
# (@emdzej/gasm-host, @emdzej/csfs-*) into docs/public/play/vendor/.
# Run after `pnpm install` in docs/; the output is git-ignored.
set -euo pipefail
cd "$(dirname "$0")/.."
nm=node_modules/@emdzej
out=public/play/vendor
rm -rf "$out"
mkdir -p "$out/gasm" "$out/csfs/core" "$out/csfs/fsa" "$out/csfs/opfs"
# gasm-worker.js imports ./gasm-host.js and ./webgpu-gfx.js (gasm:gfx in the worker); gasm-host.js
# imports its modules from ./lib/ (since 0.6.0).
cp "$nm/gasm-host/gasm-host.js" "$nm/gasm-host/gasm-worker.js" "$nm/gasm-host/webgpu-gfx.js" "$nm/gasm-host/LICENSE" "$out/gasm/"
cp -R "$nm/gasm-host/lib" "$out/gasm/lib"
cp "$nm/csfs-core/dist/"*.js "$out/csfs/core/"
cp "$nm/csfs-fsa/dist/index.js" "$out/csfs/fsa/"
cp "$nm/csfs-opfs/dist/index.js" "$out/csfs/opfs/"
cp "$nm/csfs-core/LICENSE" "$out/csfs/LICENSE"
# gasm-host is MIT (LICENSE above); record which version was vendored.
node -e "const p=require('./$nm/gasm-host/package.json');console.log(p.name+'@'+p.version+' ('+p.license+')')" > "$out/gasm/VERSION"
echo "vendored $(cat "$out/gasm/VERSION") and csfs $(node -p "require('./$nm/csfs-core/package.json').version")"
