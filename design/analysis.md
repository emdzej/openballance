# OpenBallance: initial analysis

Source: `~/Downloads/Ballance.iso` (Atari/Cyparade, CD dated 2004-02-23, multilingual, German installer).
Extracted locally (git-ignored): `cd/` (ISO), `game/` (InstallShield payload, via `unshield`), `re/` (dumps).

## 1. What Ballance actually is

Ballance is a **Virtools Dev 2.1** title (CK version `0x13022002`, file version 8). Unlike Return Fire,
there is almost no game-specific native code:

| Layer | Files | Role |
|---|---|---|
| Player | `Bin/Player.exe` (471 fn) | Generic Virtools player: window, render context, loads `base.cmo` |
| Engine | `Bin/CK2.dll` (2,794 fn), `Bin/VxMath.dll` (726 fn), `CKZlib.dll` | CK2 object model, file I/O, behavior scheduler, messages, parameters, arrays; math |
| Render | `RenderEngines/CK2_3D.dll` (3,907 fn) + `CKDX8Rasterizer.dll` | Scene graph, meshes, materials, textures, sprites, DX8 fixed-function |
| Managers | `Dx5InputManager`, `DX7SoundManager`, `ParameterOperations`, `CK2UI` | Input, sound, parameter ops |
| Building Blocks | 20 DLLs in `BuildingBlocks/` | The "instruction set" the scripts call |
| **Game logic** | `base.cmo` + `3D Entities/*.nmo` + `3D Entities/PH/*.nmo` | **Behavior graphs (visual scripts)**: menus, gameplay, ball types, modules |
| **Levels** | `3D Entities/Level/Level_01..12.NMO` | Pure data: ~130 meshes/objects, materials, textures, 29 named groups |

Levels carry no scripts. Semantics come from group names the gameplay scripts iterate:
`Phys_Floors`, `Phys_FloorRails`, `PS_Levelstart`, `PE_Levelende`, `PC_Checkpoints`, `PR_Resetpoints`,
`Sector_NN`, `P_Modul_NN`, `P_Ball_*`, `P_Trafo_*`, `P_Extra_*`, `Sound_HitID_NN`, `Sound_RollID_NN`,
`Shadow`, `DepthTestCubes`. Each `P_*` group is instantiated from the matching `PH/P_*.nmo` prefab.

### Script inventory (object table of every .cmo/.nmo, `tools/nmo.py`)

- **4,469 behavior nodes** in total. Largest: `Menu.nmo` 1,394, `Gameplay.nmo` 1,038, `base.cmo` 611,
  `Levelinit.nmo` 375, `Sound.nmo` 197, `Balls.nmo` 171.
- Exact, by prototype GUID (`re/bb_inventory.txt`): **184 distinct Building Blocks** in **3,885 BB nodes**
  (530 of them `Op`, the parameter-operation BB), plus **584 graph nodes** (the game's own subgraphs:
  `reset Level`, `physicalize new Ball`, `Activate Sector`, ...). No prototype is used under two names.

| BB DLL | Distinct BBs | Uses | Notes |
|---|---:|---:|---|
| Logics | 62 | 1,800 | flow control, messages, arrays (`Get/Set Cell` 350), interpolators, iterators |
| TT_Toolbox_RT | 28 | 264 | Terratools: push buttons, proximity, restore IC, sprites, fonts |
| Visuals | 6 | 268 | show/hide, 2D materials |
| Narratives | 12 | 246 | activate/deactivate scripts, object create/load/delete |
| **physics_RT** | **14** | **196** | Physicalize 65, SetPhysicsForce 37, Hinge 34, CollDetection 17, WakeUp, Globals, Slider, Spring, Ball Joint, Impulse |
| Controllers | 5 | 180 | Key Event 170 |
| Interface | 5 | 148 | 2D Text 110 |
| Sounds | 7 | 92 | Wave Player, pitch/volume/pan |
| 3DTransfo | 9 | 65 | |
| Materials | 13 | 61 | incl. Texture Scroller, Movie Player |
| TT_ParticleSystems_RT | 4 | 29 | point/spherical/planar particles |
| TT_Gravity_RT | 9 | 14 | TT Sky, SpeedOMeter, Simple Shadow, Extra, ProximityVolumeControl |
| others | 10 | 30 | DatabaseManager (highscores/settings), InterfaceManager (screen modes), Fog, Cameras, Lights, Box-Box intersection |

### Physics

`physics_RT.dll` statically links **Ipion ("ipion 99/07/10")** and **qhull**. 1,946 functions, decompiles
cleanly. Uses doubles, a handful of `sin/cos/asin/acos/atan` calls. `Physicalize` parameters: Fixed,
Friction, Elasticity, Mass, Collision Group, Start Frozen, Enable Collision, Auto Mass Center,
Linear/Rot Speed Dampening, Collision Surface, Convex/Ball/Concave counts, Shift Mass Center.
Needed features: ball + convex + concave (floor meshes) collision, friction/elasticity, damping, hinge,
slider, spring, ball joint, forces, impulses, collision callbacks, continuous contact, freeze/wake.

This is the single hardest and most fidelity-critical subsystem: ball feel **is** the game.
Clean-room only: port from our own decompilation. Do not consult the leaked Ipion source.

### Data formats (all simple)

- `.cmo/.nmo`: `Nemo Fi` header, zlib'd header1 (object table + plugin GUIDs) and data (manager and object
  `CKStateChunk`s: `dataVer|classId|chunkVer=7|options`, dword payload, optional ID/sub-chunk/manager lists).
- Textures: 24-bit BMP (32–512 px, power of two) and 32-bit uncompressed TGA; cube-face sky BMPs.
- Sounds: PCM16 WAV, 11–48 kHz, mono/stereo (52 MB).
- `atari.avi`: Microsoft Video 1 (`CRAM`), trivial codec.
- Fonts: `Font_1.tga` bitmap font, plus Windows system fonts **Arial / Arial Black** (need a metric-compatible
  substitute such as Arimo/Liberation Sans; don't ship MS fonts).
- `Database.tdb`: TT_DatabaseManager arrays (settings, highscores) → `gasm:storage`.
- Distribution: data sits in **InstallShield cabinets** (`Setup/data1.hdr/.cab`, `data2.cab`, zlib),
  not as loose files on the CD. OpenRF-style "point at the mounted CD" needs an IS-cab reader in the port
  (unshield documents the format) in addition to an ISO 9660 reader; an installed folder also works.

## 2. Approach: implement a Virtools subset, run the original scripts

**Recommended: Option A, a CK2-compatible runtime that loads the user's `.cmo/.nmo` and executes the
original behavior graphs**, with native reimplementations of the ~190 BBs, the parameter operations,
the Ipion subset and a renderer.

Why:
- The scripts are the game's source code. Executing them keeps every quirk by construction, the way
  OpenRF keeps the original's arithmetic.
- It naturally satisfies OpenRF's rule 2: nothing from the original (graphs, geometry, strings) enters the
  source tree; everything loads at runtime from the user's data.
- Work is bounded by the BB inventory (~190 + ops), not by 4,469 hand-translated nodes.
- Each BB is a small, independently testable unit ported from its DLL (`Logics.dll` fn → `bb_logics.c`).

**Option B (rejected): hand-port the graphs to C.** 4,469 nodes, Virtools scheduling semantics (link delays
in frames, priorities, parameter propagation, message timing) would be emulated implicitly and
inconsistently. Fidelity loss would be silent. Keep it only as a fallback for isolated graphs.

What the runtime must reproduce exactly (from `CK2.dll`): behavior activation and link-delay scheduling,
priorities, input/output parameter pull and push, parameter operations, local/shared params, messages
(`Send/Wait/Broadcast Message`), attributes, groups, data arrays, scene/level activation, initial-condition
save/restore (`Restore IC` is used 54 times), object create/copy/delete/load (`Object Load` of `PH/*.nmo`).

### Proposed layout (mirrors OpenRF)

| Path | What |
|---|---|
| `src/ck/` | object model, `CKStateChunk` reader, file loader, per-class readers (mesh, material, texture, 3D entity, group, array, behavior, parameter), scheduler, messages, IC |
| `src/vx/` | VxMath: vectors, matrices, quaternions, the exact order of operations |
| `src/bb/` | Building Blocks, one file per original DLL, each citing `/* BB name, DLL 0xADDR */` |
| `src/phys/` | Ipion subset + convex hull |
| `src/render/` | fixed-function DX8 pipeline on top of the platform GPU layer |
| `src/` | app, `platform.h`, vfs (ISO 9660 + InstallShield cab + host dirs), audio mixer, WAV/BMP/TGA/CRAM decoders |
| `tools/` | `ck.py` (file reader + graph dumper), `nmo.py` (object table), graph dumper (behavior graphs to readable text for porting/debugging), `ghidra/` scripts |

## 3. gasm: changes needed

Full spec for the gasm side: [`gasm-gfx-requirements.md`](gasm-gfx-requirements.md) (R1 textures/samplers,
R2 explicit layouts + dynamic offsets, R3 viewport/scissor/depth bias, R4 text input, R5 lazy assets for gfx
guests on the web). Summary:

`gasm:gfx` v0 has **no textures and no samplers**. Ballance is fully textured DX8 fixed-function
(multi-texture materials, alpha blend and alpha test, fog, lights, sphere/reflection mapping, texture
scrolling, 2D sprites/text, sky box, projected shadow decals, `DepthTestCubes` depth-only pass). All of
that maps to WGSL shaders except the missing resource types. This is already on gasm's roadmap as "gfx v1".

Required (R1):
- `create_texture(desc_json) -> u32` (2D, `rgba8unorm`, mip levels), `write_texture(tex, mip, x, y, w, h, ptr, len)`
  (also per-frame, for the AVI movie and any dynamic texture).
- `create_sampler(desc_json) -> u32` (filter, mipmap filter, address modes).
- Bind-group entries of kind `texture` and `sampler`.
- `depthStencil.depthBias*` in pipeline descriptors (shadow decals); cheap to allow.

Strongly wanted (R2):
- Dynamic uniform offsets on `set_bind_group` (one buffer + one bind group for hundreds of objects instead of
  one region and bind group per object).
- `begin_frame` variant that doesn't clear color (not required: the sky box covers the frame).

Input (R3, small): the game needs arrows, Shift (rotate camera), Space (overview), Enter, Esc (pause). The
12-button pad covers it (Shift → L, Space → R, Esc → START, since gasm reserves Escape for quitting).
**Highscore name entry** needs text input: either an optional `gasm` text/key-event import or an on-screen
letter picker (a UI deviation). Menus are keyboard-navigable; a mouse is not needed.

No change needed: audio (guest mixer, `audio_push`), assets (folder or mounted CD, `asset_read_at`),
storage (settings/highscores), determinism (wasm floats and a guest-compiled libm are deterministic;
bit-exactness with the x87 original is not achievable anyway).

Fallback if gfx v1 slips: a software rasterizer presenting via `video_present` (640×480, perspective-correct,
point sampling). Feasible for DX7-era geometry but a detour; the SDL backend would use the GPU regardless.

## 4. Risks

1. **Physics fidelity.** Ipion is a substantial engine and the original ran on x87 extended precision.
   Target: same behavior and feel, verified against trajectories recorded from the original (run under a
   Windows VM or Wine; a small recorder in the original process logging ball position per frame).
2. **Virtools scheduling semantics.** Subtle ordering bugs will look like gameplay bugs. Mitigation: port the
   scheduler from `CK2.dll` first, then a graph tracer that logs activations per frame for comparison.
3. **CK2 chunk formats per class.** Mechanical but wide; `CK2.dll`/`CK2_3D.dll` load paths are the reference.
4. **Scale of engine RE.** CK2 + CK2_3D + VxMath ≈ 7,400 functions, but only the subset the game uses is needed.

## 5. Suggested order

1. Tooling: full `CKStateChunk` reader in Python, per-class decoders, behavior-graph dumper; mesh viewer
   that renders a level (proves formats + render pipeline).
2. Write the gasm gfx v1 requirements doc (like OpenRF's `design/gasm-host-requirements.md`), SDL3 GPU backend
   meanwhile.
3. CK2 core: object model, loader, scheduler, parameters, messages; run `base.cmo` headless with stubbed BBs
   and log which BBs fire.
4. BBs in usage order (Logics → Narratives/Visuals/Interface → Controllers/Sounds → TT_*).
5. Physics: static concave floors + one ball, then joints and forces, then collision callbacks/sounds.
6. Menus and levels 1–12, highscores, gasm bundle and browser player.

## Ghidra

Project `ballance` (ghidra-cli), programs: `Player.exe`, `CK2.dll`, `CK2_3D.dll`, `VxMath.dll`,
`physics_RT.dll`, `Logics.dll`, `TT_Toolbox_RT.dll`, `TT_Gravity_RT.dll`, `TT_ParticleSystems_RT.dll`.

- Pass `--java-home /opt/homebrew/Cellar/openjdk@21/21.0.12.1/libexec/openjdk.jdk/Contents/Home`
  (auto-detection hangs in `LaunchSupport`).
- Import-time analysis finds almost nothing in these DLLs (VxMath: 0 functions). Run
  `ghidra script run $PWD/tools/ghidra/Reanalyze.java --project ballance --program X.dll` after importing:
  it disassembles all exports and reruns auto-analysis (VxMath 0 → 726, CK2 54 → 2,794).
- Full dumps: `tools/ghidra/DumpAllNamed.java` → `re/<program>.c` (done for physics_RT, TT_Gravity_RT, Player).
- Remaining BB DLLs and managers still to import: Narratives, Visuals, Interface, Controllers, Sounds,
  3DTransfo, Materials, TT_DatabaseManager_RT, TT_InterfaceManager_RT, ParameterOperations, CK2UI,
  Dx5InputManager, DX7SoundManager, CKDX8Rasterizer.
