# Credits and licences

**OpenBallance** is released under the [GNU General Public License v3.0](https://github.com/emdzej/openballance/blob/main/LICENSE).
It contains no code, art, sound or data from the original game: everything is read from your own copy at run
time.

**Ballance** is © 2004 Cyparade / Atari. OpenBallance is not affiliated with them.

## Published and community sources

- **[Virtools SDK 2.1 headers](https://github.com/doyaGu/Virtools-SDK-2.1)** (published by doyaGu) — vtable slot
  order, method names and the `CKPGUID_*` parameter types.
- **The public Ipion (IVP) sources** — class, field and "IVP name" labels in the physics notes, marked as
  inferred. The leaked Ipion source was deliberately not consulted.
- **[unshield](https://github.com/twogood/unshield)** (MIT) — its documentation and sources explained the
  InstallShield cabinet layouts; no code was taken from it.
- **RFC 1950 / 1951 and RFC 1321** — the project's own inflate and MD5.

## Reference implementations

- **qhull**, linked into `physics_RT.dll` — its behaviour was documented and replaced by a small hull builder.

## Tools

[Ghidra](https://ghidra-sre.org), [ghidra-cli](https://github.com/akiselev/ghidra-cli), [Capstone](https://www.capstone-engine.org), objdump, numpy, Pillow, [wasi-sdk](https://github.com/WebAssembly/wasi-sdk), [VitePress](https://vitepress.dev).

## Runtime

[gasm](https://gasm.emdzej.pl) (MIT): `gasm-run`, the C SDK, and on this site `@emdzej/gasm-host`. OpenBallance is part of
[ENCORE](https://encore.emdzej.pl), with its sibling reimplementations.

If we have missed someone whose work this project relied on, please
[open an issue](https://github.com/emdzej/openballance/issues) — gaps in these credits are bugs.
