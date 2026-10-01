# Architecture

```
src/
  app.c             application: boot base.cmo, read input, run a frame, render
  platform_gasm.c   the gasm platform layer (pads, text input, storage, audio, data sources)
  vfs*.c iscab.c    files: host folders, ISO 9660 images, InstallShield cabinets
  inflate.c         DEFLATE (Virtools files and cabinets)
  image.c movie.c   BMP / TGA decoders, movie textures
  audio.c           the sound mixer
  ck/               the Virtools runtime (CK2): objects, file loading, behaviours, parameters,
                    messages, data arrays, attributes, 3D classes, curves, fonts, particles, sound
  bb/               the Building Blocks, one file per original DLL
  phys/             the Ipion (IVP) physics engine port
  render/           the renderer on the gasm graphics API
```

## The Virtools runtime

Ballance's `.cmo` / `.nmo` files are Virtools composition files: lists of objects saved as state chunks.
`src/ck` loads them into a CK2-like object model (behaviours, parameters, data arrays, groups, meshes,
materials, textures, 3D and 2D entities, cameras, lights, sounds) and runs the behaviour engine the way
`CK2.dll` does: active behaviours and their links each frame, parameter operations, messages, scene
activation and initial conditions. See [Virtools runtime](/ck-runtime).

## Building Blocks

Each Building Block the game uses is a C function registered by its GUID, ported from the DLL that
implements it (`Logics.dll`, `Narratives.dll`, `3DTransfo.dll`, `TT_Toolbox_RT.dll`, `physics_RT.dll`
…). The game uses about 180 different BBs; all are implemented.

## Rendering

`src/render` draws with gasm's graphics API (WebGPU-style pipelines, buffers and textures), reproducing
the Direct3D 7 fixed-function pipeline Ballance used: per-vertex lighting, fog, blend and alpha-test
states, colour-keyed textures, render-first sky geometry, material channels (the ball shadow), particles,
3D sprites and the 2D foreground with text.

## Physics

Ballance's physics is the Ipion (IVP) engine compiled into `physics_RT.dll`. `src/phys` ports it: the
core dynamics, the event-driven collision detection on compact ledges, and the friction and impact
systems. See [Physics](/physics) and the IVP pages.

## Platform

One module, `openballance.wasm`, runs on gasm: natively with `gasm-run` and in the browser with
`@emdzej/gasm-host`. The game data comes from the player's own copy, read on demand.
