# Make a release

Push a tag `vX.Y.Z`. The release workflow builds `openballance.wasm`, packages it with the gasm runner
for macOS (universal), Linux (x86_64, arm64) and Windows (x86_64) with `tools/package-gasm.sh`, smoke-tests
the macOS and Linux bundles (`tools/smoke-gasm-bundle.sh`) and publishes them with checksums on the
GitHub release.

The gasm version is pinned in one place, `GASM_VERSION` in `tools/fetch-gasm-sdk.sh`; keep
`@emdzej/gasm-host` in `docs/package.json` in step with it.
