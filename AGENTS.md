# AGENTS.md

Guidance for coding agents working in this repository. Humans: see [README.md](README.md) and
[openballance.emdzej.pl](https://openballance.emdzej.pl).

## What this is

OpenBallance is a faithful reimplementation of **Ballance** (Cyparade / Atari, 2004), a Virtools 2.1 game,
in C11, running on [gasm](https://gasm.emdzej.pl) (`openballance.wasm`: native runners and the browser).
Ballance keeps its game logic in Virtools behaviour graphs inside its `.cmo` / `.nmo` files, so the port
does not rewrite the game: it loads the user's files into a CK2-compatible runtime (`src/ck`) and executes
the original graphs. The Building Blocks they call are ported from the original DLLs (`src/bb`), the physics
is a port of the Ipion (IVP) engine in `physics_RT.dll` (`src/phys`), and the renderer reproduces the
fixed-function Direct3D look on gasm's graphics API (`src/render`). The approach and its reasons are in
`design/analysis.md`.

**Fidelity is the product.** Reproduce the original's behaviour, quirks included. If something looks wrong,
check the graph dump and the disassembly; if the original does it, keep it and add a comment.

## Hard rules

1. **Never commit original data or anything derived from it.** `cd/`, `game/`, `data`, `re/` (decompiler
   and graph dumps) and `out/` are git-ignored: keep it that way. No disc images, extracted textures,
   meshes, sounds or graphs. The site's screenshots are renders of the port, never the game's own files.
2. **The graphs are data, not source.** Nothing from the `.cmo` / `.nmo` files (graphs, geometry, strings)
   enters the source tree; everything is loaded at run time from the user's copy.
3. **Cite the original.** Every ported function names its DLL and address in a comment.
4. **Don't paste decompiler output** into source or docs. Write the port and the docs in your own words.
   The physics is ported from our own disassembly of `physics_RT.dll`; do not consult leaked Ipion source.
5. **Explain the game, not only port it.** Keep the specification pages (`docs/*.md`: the CK2 runtime, the
   Building Blocks, the physics) and `docs/internals/` accurate as each subsystem is ported or corrected.
   Keep `docs/guide/theory.md`, the theory of operation, up to date: how the game was designed and how it
   works, in prose, for someone who wants to build such a game. No decompiler text, no game data, no
   addresses in the main text.

## Docs map

| Page | What |
|---|---|
| `docs/guide/` | The user guide; `theory.md` is the theory of operation |
| `docs/howto/` | Building, tests, headless runs, releases, the reverse-engineering workflow |
| `docs/internals/` | Architecture and the design records (`design/*.md` included) |
| `docs/ck-runtime.md` | The Virtools runtime semantics from `CK2.dll` |
| `docs/gameplay_bbs.md`, `fonts.md`, `sky.md`, `animation.md`, `particles.md` | Building Block specifications |
| `docs/physics.md`, `ivp_core.md`, `ivp_collision.md`, `ivp_contact.md` | The physics glue and the IVP engine |

The site is VitePress (`docs/.vitepress/config.ts`); `pnpm build` in `docs/` must pass with no dead links.

## Build and test

See `docs/howto/build-from-source.md` and `docs/howto/tests.md`. The tests need the original game data
(`OPENBALLANCE_DATA`), so CI only compiles them.

## Commits

Imperative or descriptive subject, body explaining why when non-obvious. Release tags are plain semver
without a `v` prefix.
