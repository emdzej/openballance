# gasm host requirements for textured 3D guests (driven by OpenBallance)

> **Status: implemented in gasm 0.4.0** (commit `5cd7788`, "gfx textures, samplers, explicit layouts,
> viewport; text input; Worker gfx"). R1-R5 done, plus read-only storage buffers and asset enumeration.
> OpenBallance's renderer (`src/render/`) uses R1-R3 since 2026-09-30. Kept as the record of what
> OpenBallance needed from gasm and why.

Audience: an agent (or person) implementing changes in the **gasm** repository
(`~/Projects/my/gasm`, https://github.com/emdzej/gasm). Read gasm's `AGENTS.md` and `spec/ABI.md` first
and follow their invariants: ABI changes start in `spec/abi.json` (then `node scripts/gen-abi.mjs`),
both runners change together (`runners/native/src/gfx.rs` + `host.rs`, `runners/web/webgpu-gfx.js` +
`gasm-host.js`), the Rust SDK (`guests/gasm/src/{sys,lib,native}.rs`) and the site docs are updated in the
same change, hash output format is a contract, validation errors trap and never panic, no emojis on the site.

## Context

**OpenBallance** is a faithful reimplementation of Ballance (Cyparade/Atari, 2004), in C11 like OpenRF:
a portable core plus SDL3 and gasm backends. Ballance is a Virtools 2.1 game rendered through DirectX 8
fixed function. Unlike Return Fire it is fully textured 3D, so it cannot use `video_present` (see
"Fallback" below) and needs `gasm:gfx`.

What the renderer has to reproduce (from the game data and `CK2_3D.dll`):

- ~80 textures: 24-bit BMP and 32-bit TGA, 16×16 to 512×512, power of two, with mipmaps
  (`Set MipMap Level`, `TT_SetMipMapping`), linear filtering, wrap and clamp addressing.
  Some textures are colour-keyed (converted to alpha by the guest).
- Materials with up to two texture stages (multi-texture), prelit/vertex colours, up to 8 lights,
  specular, emissive, fog (`Set Fog`), alpha blending with arbitrary blend modes (`Set Blend Modes`),
  alpha test, per-material Z write/test (`Set Material Z Buffer`), two-sided faces.
- Sphere/reflection environment mapping (balls, rails, domes: `TT_ReflectionMapping`), texture scrolling
  (`Texture Scroller`, UV transform in a uniform).
- Sky box (six face textures), particle systems (camera-facing textured quads, additive blending),
  projected "simple shadow" decals on floors (need depth bias), a depth-only occluder pass
  (`DepthTestCubes`: colour `writeMask: 0`, already supported).
- 2D: sprites, a bitmap font (`Font_1.tga`), menu buttons, the HUD.
- One dynamic texture: the `atari.avi` intro (Microsoft Video 1, decoded by the guest) played into a
  texture every frame (`Movie Player`).
- 4:3 presentation (the original ran at 640×480 to 1600×1200) into an arbitrary drawable.

Everything above maps to WGSL shaders in the guest. What `gasm:gfx` v0 lacks is textures and samplers
(`spec/ABI.md`: "Textures, samplers, storage buffers and compute are not in v0"; "gfx v1: textures and
samplers" is already on the roadmap), plus a few pipeline and pass details listed below.

## Goals

1. A guest can upload textures (all mip levels), update them every frame, and sample them.
2. One texture bind group per material can be used with every pipeline variant (not one per pipeline).
3. Hundreds of objects per frame without one bind group per object per pipeline.
4. Decals, letterboxing and a fixed aspect ratio work.

## Non-goals

- No image decoding in the runner (BMP/TGA/AVI are the guest's job). No mipmap generation in the runner:
  WebGPU has none, and doing it in the guest keeps runners small and output identical.
- No render targets / render-to-texture, cube maps, storage textures, compute, stencil: Ballance doesn't
  need them. Leave the door open (descriptor fields), don't implement them now.
- No change to existing imports; existing guests, hashes and tests are unaffected.
- No `gasm:gfx` in Worker mode (unchanged rule).

## Requirements

Priority: **R1 is required.** R2 and R3 are strongly wanted (OpenBallance can work around them at a cost,
noted per item). R4 and R5 are optional.

### R1: Textures and samplers

Additive imports in `gasm:gfx` (objects share the existing handle space; `0` is never valid):

| Import | Signature | Semantics |
|---|---|---|
| `create_texture` | `(ptr, len) -> u32` | JSON `GPUTextureDescriptor` subset: `{"size":[w,h],"format":"rgba8unorm","mipLevelCount":n}`. 2D only. Usage is implied (`TEXTURE_BINDING` \| `COPY_DST`). |
| `write_texture` | `(tex, mip, x, y, w, h, ptr, len)` | Upload a `w×h` RGBA8 region of mip level `mip` at `(x, y)`, tightly packed (`bytes_per_row = w*4`, `len = w*h*4`). Queued like `write_buffer`: every write made before `end_frame` lands before that frame's draws. Valid inside and outside a frame. |
| `create_sampler` | `(ptr, len) -> u32` | JSON `GPUSamplerDescriptor` subset: `addressModeU/V` (`clamp-to-edge`, `repeat`, `mirror-repeat`), `magFilter`, `minFilter`, `mipmapFilter` (`nearest`, `linear`), `lodMinClamp`, `lodMaxClamp`, `maxAnisotropy`. Omitted fields take WebGPU defaults. |

Bind-group entries gain two kinds, next to today's buffer entries:
`{"binding":B,"texture":T}` (a view of the whole texture, all mips) and `{"binding":B,"sampler":S}`.

Rules:

- **Formats:** `rgba8unorm` is required. `rgba8unorm-srgb` is optional (not needed by OpenBallance: DX8
  blends in gamma space). Keep the surface a non-sRGB format on both runners (today `bgra8unorm`/`rgba8unorm`
  natively, `getPreferredCanvasFormat()` on the web); document that colours are not gamma-converted.
- **Limits:** at least 2048×2048 and full mip chains; `maxAnisotropy` up to 16 (clamped to the device).
  Report the actual limits in the docs; don't request above WebGPU defaults.
- **Validation:** out-of-range mip, region outside the level, `len` mismatch, wrong handle kind, unknown
  format → trap with a message (same path as `write_buffer` errors). Never panic.
- **Null GPU (headless):** create calls return handles and validate descriptors; writes validate and
  are otherwise ignored, exactly like buffers today.
- **Hashing:** headless runners fold every `write_texture` payload into the video hash, in call order,
  like `write_buffer` payloads (native, JS and the `gasm::native` stub identical). Consider folding the
  header (`tex, mip, x, y, w, h`) too, so a write to a different region changes the hash; document the choice.
- **SDK:** C header (generated) and Rust wrappers (`Texture`, `Sampler`, `write_texture`) plus the
  `gasm::native` stub.

**Acceptance:** a new guest (e.g. `guests/textured`, Rust like `triangle`) that uploads a procedurally
generated texture with a full mip chain, rewrites a region every frame, and draws a rotating textured,
alpha-blended quad with two samplers (repeat/linear and clamp/nearest). `scripts/determinism-test.sh` gets a
case for it (headless hashes equal on wasmtime JIT, AOT and V8/Node). `--screenshot` on native and the web
smoke test (`scripts/web-smoke.mjs`) produce PNGs that look right (look at them). Existing suites unchanged.

### R2: Explicit bind-group layouts and dynamic offsets

Today every pipeline uses `layout: "auto"`, and auto layouts belong to one pipeline. For OpenBallance that
means one texture bind group per (material × pipeline variant): about 80 textures × 10–20 blend/Z/fog
variants, and one uniform bind group per (object × pipeline). Auto layouts also can't declare dynamic offsets.

Additive:

| Import | Signature | Semantics |
|---|---|---|
| `create_bind_group_layout` | `(ptr, len) -> u32` | JSON `GPUBindGroupLayoutDescriptor` subset: `entries[{binding, visibility, buffer{type:"uniform", hasDynamicOffset, minBindingSize}, texture{sampleType, viewDimension}, sampler{type}}]`. `visibility` = WebGPU `GPUShaderStage` bits. |
| `set_bind_group_offsets` | `(index, bg, ptr, count)` | Like `set_bind_group`, with `count` `u32` dynamic offsets read from guest memory (multiples of 256; validated). |

- `create_pipeline`: `layout` may be an array of bind-group-layout handles (`"layout":[L0,L1]`); `"auto"`/omitted
  keeps today's behaviour.
- `create_bind_group`: accepts `"layout":L` instead of `"pipeline"`/`"group"`. A bind group made from an
  explicit layout works with every pipeline whose layout lists that same layout handle at that index.
- Handle kinds are checked (a layout where a pipeline is expected traps).

Without R2, OpenBallance creates the bind-group matrix up front (a few thousand small objects) and gives
every object its own uniform region per pipeline. That works, but it's wasteful and slows start-up on the web.

**Acceptance:** the R1 test guest draws N objects from one uniform buffer with one bind group and
per-draw dynamic offsets, and shares one texture bind group between two pipelines. Hashes are stable across runners.

### R3: Viewport, scissor and depth bias

| Import | Signature | Semantics |
|---|---|---|
| `set_viewport` | `(x, y, w, h, min_depth, max_depth: f32)` | Inside a frame. Pixels of the current drawable. Default at `begin_frame`: the whole drawable, 0–1. |
| `set_scissor_rect` | `(x, y, w, h)` | Inside a frame. Clamped to the drawable. Default: the whole drawable. |

- Pipeline `depthStencil` gains `depthBias`, `depthBiasSlopeScale`, `depthBiasClamp` (WebGPU fields,
  passed through). Needed for shadow and decal layers drawn over the floor.

Viewport and scissor give a 4:3 image, pillar-boxed into any window. They also clip 2D. Without them the
guest letterboxes through the projection matrix and draws black bars itself. That works, but it's more work
in every shader path, and it can't clip.

**Acceptance:** the test guest renders into a 4:3 viewport inside a 16:9 drawable. The area outside is the
clear colour, on both runners.

### R4 (optional): Text input

Ballance asks for a name after a high score. The 12-button pad can't type. Options:

- **(a)** Additive core import `text_input(dst, cap) -> i32`: the UTF-8 text typed since the previous frame
  (stable within a frame; backspace = `\b`, enter = `\n`), `-1` if the runner has no keyboard. Headless:
  from the `--input` script. Keys already bound to pads still produce text (the guest decides what it wants).
- **(b)** Nothing in gasm. OpenBallance shows an on-screen letter picker (a small, documented UI deviation).

OpenBallance will ship (b) first and switch to (a) if gasm adds it. Related limitation, documented in OpenBallance
and not asked of gasm: the original's key-remapping screen binds raw keyboard keys. Under gasm it maps pad buttons
instead, because gasm's keymap already owns key→button mapping.

### R5 (optional): Lazy assets for `gasm:gfx` guests on the web

gasm's lazy providers (OPFS via `FileSystemSyncAccessHandle`, `FileReaderSync`) exist only in Worker mode,
and `gasm:gfx` guests always run on the main thread. So in the browser, OpenBallance must preload its data
into memory: about 136 MB installed (textures 67 MB, sounds 52 MB, scripts and levels 17 MB), or the 120 MB ISO.
It works, but first start and every later visit pay the read, and the tab holds the memory.

Options, implementer's choice:

- **(a)** `gasm:gfx` in Worker mode through `OffscreenCanvas` (WebGPU is available in dedicated workers in
  Chromium; report Safari/Firefox status). The page transfers the canvas; input/audio bridging as today.
  This lifts the "gfx guests never go to a worker" rule for runners that support it, with main-thread
  mode as the fallback.
- **(b)** Keep gfx on the main thread and serve assets from a helper worker over `SharedArrayBuffer` +
  `Atomics.wait`. Rejected up front: it needs COOP/COEP, which GitHub Pages can't set.

Without R5, OpenBallance preloads (with a progress bar) and caches the files in OPFS, so later visits skip
the network and file picker but still read everything into memory.

## Constraints and compatibility

- All additions are **additive imports**. Runners link missing imports as traps, so older guests are unaffected.
  Whether this bumps `GASM_ABI_VERSION` follows gasm's rules (additive has been treated as compatible so far);
  record the decision in `spec/ABI.md`.
- `@emdzej/gasm-host` stays dependency-free. The native runner stays on wgpu, with no new crates if avoidable.
- Determinism of hashes: texture writes are guest data, so hashes stay bit-identical across runners. Pixels
  remain unhashed (GPU output isn't bit-exact across vendors).
- Update `spec/abi.json` (regenerate `gasm.h`/`sys.rs`), `spec/ABI.md` (the gfx section and the removal of
  "textures, samplers … not in v0" and its roadmap line), `AGENTS.md` (the gfx invariant about "auto" layouts
  now applies only to `"auto"`), the site docs and the SDK examples, in the same change set as the code.

## Fallback considered

Software rendering through `video_present` at 640×480 (perspective-correct, point-sampled) is feasible for
the game's DX7-era geometry. But it gives up filtering, mipmaps and fillrate headroom, and it would be a
second renderer next to the SDL GPU one. It's kept only as a contingency if gfx texture support slips.

## What OpenBallance will do with this

- One uniform ring buffer per frame (camera, lights, fog; per-object world matrix and material via dynamic
  offsets), one texture+sampler bind group per material stage, and a small cache of pipelines keyed by
  (blend mode, Z test/write, cull, alpha test, fog, stage count).
- Textures decoded from the user's BMP/TGA at level load, mips built in the guest. The intro movie is decoded
  per frame into one 256×256 texture via `write_texture`.
- Native: `gasm-run openballance.wasm --asset-dir <installed game or mounted CD>`. Web: the same OPFS import
  flow as OpenRF. Assets are preloaded on the main thread until R5 lands, then read lazily in a Worker.
- Settings and high scores through `gasm:storage`.
