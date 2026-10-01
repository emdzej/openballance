# Particle systems (TT_ParticleSystems_RT.dll)

`TT_ParticleSystems_RT.dll`, image base `0x25080000`, Ghidra project `ballance`, program
`TT_ParticleSystems_RT.dll`. The plugin registers 11 Building Blocks (category `TT Particles` /
`TT Particles Terratools`) and one manager, the `Particle Manager` (`1dd91197:01f703f3`). All 11 BBs share
one prototype function, one execute function and one callback. What differs between them is the emitter
object that the callback creates for each GUID (§3): its vtable says where a particle is born and in which
direction it starts.

| Function | Address |
|---|---|
| prototype creation (shared by all 11 BBs) | `FUN_25084c50` |
| execute | `FUN_250853a0` |
| behavior callback (all messages, mask `0xffffffff`) | `FUN_25085770` |
| emitter factory (GUID -> emitter) | `FUN_250875f0` |
| emitter base constructor / particle pool allocation | `FUN_250817d0` / `FUN_25081a60` |
| read settings (locals) | `FUN_25082a00` |
| read input pins every frame (generic / time-dependent) | `FUN_25082770` / `FUN_25083290` |
| emit one "emission event" | `FUN_250820e0` |
| per-frame update (integrate, age, kill) | `FUN_25081af0` |
| time-dependent trail emission (called from the renderers) | `FUN_25083540` |
| register / unregister the render callback | `FUN_25085290` |
| shared sprite render states | `FUN_25082ed0` |
| renderers (post-render callbacks), one per render mode | `0x2508ddc0` .. `0x25090b00` (§7) |
| parameter types and manager registration | `FUN_25091bb0` |

The renderers and the emitter vtable functions (`0x250826a0`, `0x250830a0`, `0x25083020`, `0x250830d0`,
`0x250812b0`, ...) were only labels in the imported program. I created functions there for this analysis
(`tools/ghidra/MakeFunctions.java`), so they decompile now, but `re/TT_ParticleSystems_RT.dll.c` does not
contain them yet. The decompiles of the renderers and of `FUN_25083540` mix up stack slots; the details
below that matter (vertex order, which vector gets normalised, rand() order) come from the disassembly.

In short: an emitter owned by a 3D frame spawns particles at the frame's position (or on a unit square or
unit sphere around it) with a speed along a direction jittered by yaw/pitch variances. Particles live in a
fixed-size pool, move in a straight line in world space, and interpolate colour and size linearly from their
initial to their ending values over their lifespan. Every frame the BB integrates them. The frame's
post-render callback then draws them as camera-facing quads, triangles or lines, with the blend factors
from the settings, no z-write and no lighting. Ballance uses none of the "physics" features (deflectors,
interactors, bouncing, gravity), and none of the texture animation or rotation.

## 1. Which BBs the game uses

`re/bb_map.txt` lists four of the 11 BBs, 29 instances in all. I found all of them with a probe that loads
every `.nmo` (§2):

| BB | GUID | Uses | Emitter shape (§4.1) |
|---|---|---|---|
| `Point Particle System` | `506b40f7:30852e46` | 18 | at the frame's position |
| `TT_TimedependentPointParticlesystem` | `569d2cc2:3bcb01b9` | 6 | point, plus a trail between frames (§4.4) |
| `SphericalParticleSystem` | `67c88f47:8880721e` | 3 | on the unit sphere, radial direction |
| `PlanarParticleSystem` | `49957bfe:0ff27ffc` | 2 | on the unit square in the frame's XY plane |

Unused BBs, with the same pins: `LinearParticleSystem` `1f1e7188:0ec26d1f`, `CubicParticleSystem`
`3d7453f9:1dba691f`, `DiscParticleSystem` `3fb65480:6ca450e2`, `ObjectParticleSystem` `13172f30:3a7876ed`
(compatible class 0x29, emits from the vertices of the frame's mesh), `CurveParticleSystem`
`4a6524c4:13b55824` (class 0x2b), `CylindricalParticleSystem` `67c90f47:1480721e`, and
`TT_WaveParticleSystem` `02da5a18:52227285` (its own emission `FUN_25083e70` and update `FUN_250844a0`).
There are no separate "deflector" BBs. Deflectors and interactors are object attributes (§8). Every
Ballance instance has `Manage Deflectors = 0` and `Manage Interactors = 0`, so the manager never looks at
them.

All four used BBs have compatible class 0x21 (3D entity). The BB's **owner** is the emitter: a mesh-less 3D
frame (`Ball_Particle_Frame`, `FourFlames_Flame_A`, `P_Extra_Point_Frame4`, ...), not a target parameter.

## 2. Pins, locals, settings

Inputs: `0 On`, `1 Off`, `2 Freeze`. Outputs: `0 Exit On`, `1 Exit Off`, `2 Exit Freeze`. When the
deflector setting has bit `Outputs Impacts` (0x40), the settings callback adds input `3 Impacts Loop In`,
output `3 Impacts Loop Out` and four output parameters (`Impact Position`, `Impact Direction`,
`Impact Object`, `Impact Texture Coordinates`). This never happens in Ballance.

Input parameters. Emitter offsets are into the C++ emitter object (§3). "Var bit" is the bit of the
`Variances` setting that enables the variance pin. "When read": "frame" means `FUN_25082770` copies it into
the emitter at every execute, before emission (so runtime changes affect new particles).

| # | Name (exact) | Type | Default | Emitter | When read | Meaning |
|---|---|---|---|---|---|---|
| 0 | `Emission Delay` | Time (float ms) | `0m 0s 200ms` | - | execute, On | ms between emission events. Time-dependent BB: also the trail **rate** in particles/s (§4.4), `+0x124` (truncated to int) |
| 1 | `Emission Delay Variance` | Time | `0m 0s 0ms` | - | execute | delay jitter; one rand() every active frame even if 0 |
| 2 | `Yaw Variance` | Angle (float rad) | `0:20` (20 deg) | `+0x18` | frame | direction jitter, see §4.2 |
| 3 | `Pitch Variance` | Angle | `0:20` | `+0x1c` | frame | direction jitter |
| 4 | `Speed` | Float | `0.005` | `+0x20` | frame | world units per ms |
| 5 | `Speed Variance` | Float | `0.001` | `+0x24` | frame, var bit 0x1 | |
| 6 | `Angular Speed/Spreading` | Float | `0.0` | `+0x28` | frame | per render mode, §4.3 |
| 7 | `Angular Speed Variance/Spreading Variation` | Float | `0.0` | `+0x2c` | frame, var bit 0x2 | per render mode, §4.3 |
| 8 | `Lifespan` | Time | `0m 1s 0ms` | `+0x78` | frame | ms |
| 9 | `Lifespan Variance` | Time | `0m 0s 250ms` | `+0x7c` | frame, var bit 0x4 | |
| 10 | `Maximum Number` | Integer | `100` | `+0x68` | frame | cap on live particles (the pool size is setting 3) |
| 11 | `Emission` | Integer | `10` | `+0x70` | frame | particles per emission event |
| 12 | `Emission Variance` | Integer | `5` | `+0x74` | frame, var bit 0x8 | |
| 13 | `Initial Size` | Float | `1.0` | `+0x30` | frame | world units (§7) |
| 14 | `Initial Size Variance` | Float | `0.0` | `+0x34` | frame, var bit 0x10 | |
| 15 | `Ending Size` | Float | `0.1` | `+0x38` | frame, only if Evolutions has Size | |
| 16 | `Ending Size Variance` | Float | `0.0` | `+0x3c` | frame, Size and var bit 0x20 | |
| 17 | `Bounce` | Float | `0.8` | `+0x50` | frame, if any deflector | deflector response |
| 18 | `Bounce Variance` | Float | `0` | `+0x54` | var bit 0x40 | |
| 19 | `Weight` | Float | `1.0` | `+0x40` | frame, if interactor Gravity | |
| 20 | `Weight Variance` | Float | `0.0` | `+0x44` | var bit 0x80 | |
| 21 | `Surface` | Float | `1.0` | `+0x48` | frame, if interactor Global Wind, Local Wind or Atmosphere (0x46) | |
| 22 | `Surface Variance` | Float | `0.0` | `+0x4c` | var bit 0x100 | |
| 23 | `Initial Color and Alpha` | Color (4 floats RGBA 0..1) | `255,255,255,255` | `+0x88` | frame | |
| 24 | `Variance` | Color | `0,0,0,0` | `+0x98` | frame, var bit 0x200 | per channel |
| 25 | `Ending Color and Alpha` | Color | `0,0,0,0` | `+0xa8` | frame, only if Evolutions has Color | |
| 26 | `Variance` | Color | `0,0,0,0` | `+0xb8` | Color and var bit 0x400 | |
| 27 | `Texture` | Texture | NULL | `+0x08` (CK_ID) | frame | NULL: untextured |
| 28 | `Initial Texture Frame` | Integer | `0` | `+0xc8` | frame | |
| 29 | `Initial Texture Frame Variance` | Integer | `0` | `+0xcc` | frame, var bit 0x800 | |
| 30 | `Texture Speed` | Integer | `100` | `+0xd0` | frame, only if Evolutions has Texture | ms per frame; the sign sets the direction |
| 31 | `Texture Speed Variance` | Integer | `20` | `+0xd4` | Texture and var bit 0x1000 | |
| 32 | `Texture Frame Count` | Integer | `1` | `+0xd8` | frame | frames in the atlas (§7.5) |
| 33 | `Texture Loop` | `Loop Mode` enum `63942d15:05ac51a7` | `No Loop` | `+0xdc` | frame | `No Loop=0, Loop=1, To and Fro=2` |
| 34 | `Real-Time Mode` | Boolean | `true` | - | execute | true: dt = the behavior context's DeltaTime |
| 35 | `DeltaTime` | Float | `20.0` | - | execute | fixed dt in ms if not real-time |

A pin that is disabled or has no source is not read, so its emitter field keeps its constructor value
(0, except `+0x88..` = 0.6, 0.6, 1.0, 0). Every read above is behind the same flag test as the pin's
enable test, so this never matters.

Locals and settings (local parameter index = setting index):

| # | Name | Type | Default | Emitter | Meaning |
|---|---|---|---|---|---|
| 0 | `Emitter` | `4d082c90:0c8339a2` (4-byte buffer) | - | | pointer to the C++ emitter. The value saved in files is garbage; recreate the emitter on load |
| 1 | `Activity` | Integer | 0 | | bit 0 on (emitting), bit 1 frozen (§5) |
| 2 | `EmissionTime` | Float | `0` | | emission accumulator in ms (§4) |
| 3 | `Maximum Number` (setting) | Integer | `100` | `+0x64` | **pool size**. On change and > 0, reallocates the pool, which kills all particles |
| 4 | `Particle Rendering` | enum `089e77d4:2ef077d2` | `3` | `+0xf0` | `Point=1, Line=2, Sprite=3, Object=4, Orientable Sprite=5, Radial Sprite=6, Fast Sprite=7, Comet=8` (9 also has a renderer) |
| 5 | `Source Blend` | VXBLEND_MODE `089e28d4:2ef031d2` | `Source Alpha` | `+0x80` | |
| 6 | `Destination Blend` | VXBLEND_MODE | `One` | `+0x84` | |
| 7 | `Objects` | Group | NULL | `+0x0c` | render mode Object only |
| 8 | `Evolutions` | flags `270f7b39:6e0b184c` | `Color,Size,Texture` | `+0xe0` | `Size=1, Color=2, Texture=4` |
| 9 | `Variances` | flags `083e73d4:2e3073d2` | all 13 | `+0xe4` | `Speed=1, Angular Speed=2, Lifespan=4, Emission=8, Initial Size=16, Ending Size=32, Bounce=64, Weight=128, Surface=256, Initial Color=512, Ending Color=1024, Initial Texture=2048, Texture Speed=4096` |
| 10 | `Manage Deflectors` | flags `083348d4:234873d2` | Plane..Object | `+0xec` | `Plane=1, Infinite Plane=2, Cylinder=4, Sphere=8, Box=16, Object=32, Outputs Impacts=64, Die On Impact=128` |
| 11 | `Message To Deflectors` | Message `03881e12:5ba34e2b` | NULL | `+0x10` | also a version check: if the local isn't of this type, "Wrong Particle System Version..." and the settings are not read |
| 12 | `Manage Interactors` | flags `083231d4:223173d2` | all 10 | `+0xe8` | `Gravity=1, Global Wind=2, Local Wind=4, Magnet=8, Vortex=16, Disruption Box=32, Atmosphere=64, Mutation Box=128, Tunnel=256, Projector=512` |
| 13 | `Interactors/Deflectors Display` | Boolean | `TRUE` | manager `+0xb4` | editor gizmos only |

Note the flag strings in the prototype's defaults list the names in a different order than the registered
flag values (e.g. `Color,Size,Texture` vs `Size=1,Color=2`). The values are what count.

`SetFlags(1)`, behavior flags `0xb000000`.

### 2.1 Game values (probe over `base.cmo` + every `3D Entities/**/*.nmo`)

Almost every pin comes from a script local of the same name. The only exception is the `Texture` of the
four `PS_FourFlames` flames: it comes from the local `Particle_Flames Texture` of `PS_FourFlames_MF Script`.
That local is NULL in the file, and an `Op` in that script writes it at runtime (a texture lookup). Pins 34
and 35 are missing on both `P_Extra_Life` systems (an older 34-pin version of the BB, see Uncertain). "RT"
is real-time mode. Colours are RGBA floats.

| Instances | BB, render, blend (src/dst) | Evol / Var | Delay | Emission | Max / pool | Speed | Yaw/pitch var | Life | Size | Colour start -> end | Texture |
|---|---|---|---|---|---|---|---|---|---|---|---|
| MenuLevel `FourFlames_Flame_A..D`, PH `PS_FourFlames_Flame_A..D`, `PC_TwoFlames_Flame_SmallA/B` (10) | Point, 7 Fast Sprite, 5/2 | 3 / 29 | 20 | 1 +- 1 | 50 (Flame_C pin 90) / 50 | 0.008 +- 0.003 | 3 deg | 1000 +- 250 | 3 +- 0.3 -> 0.1 | (.922,.165,.275,1) -> 0 | `I_Particle_Flames` / runtime / `Particle_Flames` |
| `PC_TwoFlames_Flame_Big` | Point, 7, 5/2 | 3 / 29 | 20 | 1 +- 1 | 50 / 50 | 0.012 +- 0.004 | 3 deg | 1000 +- 500 | 5 +- 0.3 -> 0.1 | (.922,.165,.275,.98) -> 0 | `Particle_Flames` |
| `Ball_Particle_Frame` (Balls.nmo) | Spherical, 3 Sprite, 5/2 | 3 / 5 | 0 | 20 | 60 / 60 | 0.001 +- 0.0005 | (20 deg, unused) | 1600 +- 1000 | 2 -> 3 | (.784 x4) -> 0 | `Ball_Particle_Smoke` |
| `P_Extra_Life_Particle_Blob` | Spherical, 5 Orientable, 5/2 | 3 / 53 | 200 | 10 | 20 / 20 | 0.01 +- 0.004 | unused | 150 +- 100 | 1 +- 0.6 -> 0.5 +- 0.1 | (1,1,1,1) -> (1,1,1,.392) | `P_Extra_Life_Particle` |
| `P_Extra_Life_Particle_Fizz` | Spherical, 2 Line, 5/2 | 2 / 5 | 200 | 50 | 100 / 100 | 0.01 +- 0.005 | unused | 200 +- 200 | (4, unused) | (.816,.690,.835,1) constant | none |
| `P_Extra_Point_Frame0` | Point, 7, 2/3 | 3 / 4 | 0 | 40 | 100 / 100 | 0.005 | 360 deg | 400 | 1 -> 0 | 1 -> (.392 x4) | `ExtraBall` |
| `P_Extra_Point_Ball_HitFrame01..06` (6) | Point, 3 Sprite, 2/3 | 3 / 4 | 0 | 40 | 100 / 100 | 0.003 | 360 deg | 400 | 0.5 -> 0 | white constant | `ExtraBall` |
| `P_Extra_Point_Frame1..6` (6) | TimeDependent, 7, 5/2 | 3 / 0 | 90 (also 90/s trail) | 1 | 100 / 100 | 0 | 0 | 1000 | 0.5 -> 0.2 | 1 -> (.157 x4) | `ExtraParticle` |
| `P_Modul_18_Particle` #1 | Planar, 2 Line, 5/2 | 2 / 4 | 0 | 3 | 100 / 100 | 0.04 | 0 | 400 +- 10 | (4, unused) | (1,1,1,.235) -> 0 | none |
| `P_Modul_18_Particle` #2 (same frame) | Planar, 3 Sprite, 5/2 | 3 / 12 | 20 | 1 +- 1 | 40 / 40 | 0.036 | 0 | 800 +- 10 | 2.3 -> 3 | (1,1,1,.118) -> 0 | `Particle_Smoke` |

"+-" is only listed when the variance bit is set. For example `Frame0` has a speed variance pin of 0.001,
but Variances = 4 (Lifespan only), so the speed has no variance. Blend 5/2 is SRCALPHA/ONE (additive with
alpha); 2/3 is ONE/SRCCOLOR. Emission Delay Variance is 0 everywhere. Frame count is 1 everywhere, and no
instance has the Texture evolution or the Angular Speed variance. So in Ballance texture animation,
rotation and streak stretching (§4.3) are all inactive, except that the Orientable/Line renderers still
use the "previous position" tail. All owner frames are saved visible, except `P_Modul_18_Particle`, which
is saved hidden.

## 3. The emitter object

Created in the callback on ATTACH and LOAD (`FUN_250875f0`, which also counts emitters in manager
`+0xb8`). It is stored as a raw pointer in local 0. Common part, 0x118 bytes (constructor `FUN_250817d0`):

| Offset | Content |
|---|---|
| `+0x00` | vtable: slot 0 `InitPosition(particle)`, slot 1 `InitDirection(particle)` |
| `+0x04` | owner entity CK_ID |
| `+0x08` / `+0x0c` / `+0x10` | texture CK_ID / Objects group CK_ID / message type (-1) |
| `+0x14` | the current render callback (by render mode) |
| `+0x18 .. +0x54` | pin values (table in §2) |
| `+0x58` / `+0x60` | head of the live list / head of the free list |
| `+0x5c` | raw pool allocation (`pool size * 0x70 + 0x10`, aligned up to 16) |
| `+0x64` / `+0x68` / `+0x6c` | pool size (setting 3, constructor 100) / max live (pin 10) / live count |
| `+0x70 .. +0xdc` | pin values |
| `+0xe0` / `+0xe4` / `+0xe8` / `+0xec` | Evolutions / Variances / Interactors / Deflectors |
| `+0xf0` | render mode (constructor 3) |
| `+0xf4` | editor gizmo mesh (manager "Point Mesh", `FUN_2508cf50`). Added to the owner while not playing, removed on RESUME. Ignore it |
| `+0xf8` | CKContext* |
| `+0xfc..+0x104`, `+0x108` | impacts XArray (0x24-byte records: pos, dir, uv, object), output iterator |
| `+0x10c` | "first trail call" byte, 1 |
| `+0x10d` / `+0x10e` | time-dependent BB / wave BB |
| `+0x110` | the last execute's dt |
| `+0x114` | "emitting" byte (On/Off/Freeze); only the time-dependent trail reads it |

The time-dependent emitter (`FUN_250831f0`, 300 bytes) adds `+0x118` the previous trail position (vec3,
starts at `VxVector::axis0()` = 0,0,0), `+0x124` the trail rate (int), and `+0x128` the trail's
accumulated ms. It sets `+0x10c = +0x10d = 1`.

Per-GUID vtables (`FUN_250875f0`):

| BB | vtable | InitPosition | InitDirection |
|---|---|---|---|
| Point | `0x250932c0` | `0x250830a0` | `0x250826a0` |
| TimeDependent | `0x250931ec` | `0x250830a0` | `0x250826a0` |
| Planar | `0x250932b0` | `0x25083020` | `0x250826a0` |
| Spherical | `0x25093280` | `0x250830d0` | `0x250812b0` (empty; the position function sets the direction) |

### 3.1 Particle record (0x70 bytes, pool entries, 16-byte aligned)

| Offset | Field |
|---|---|
| `+0x00` | colour RGBA (floats) |
| `+0x10` | colour delta per ms |
| `+0x20` | position (world) |
| `+0x2c` | "angle" (sprite rotation; streak length factor for Line/Orientable, §4.3) |
| `+0x30` | velocity (world units per ms) |
| `+0x3c` | angular speed (per ms) |
| `+0x40` | remaining life (ms) |
| `+0x44` / `+0x48` | prev / next in the live list (next also links the free list) |
| `+0x4c` | last dt (the step applied in the last update; 0.01 at birth) |
| `+0x50` / `+0x54` | size / size delta per ms |
| `+0x58` / `+0x5c` / `+0x60` | bounce / weight / surface |
| `+0x64` | texture frame (int) |
| `+0x68` / `+0x6c` | texture time accumulator (ms) / texture speed (signed ms per frame) |

Pool reset (`FUN_25081a60`, also on the RESET message): all entries go into the free list in address
order, live list empty, count 0.

## 4. Emission

### 4.1 Random numbers

All randomness comes from MSVCRT `rand()` (IAT `0x25093174`). That is the process-wide msvcrt state shared
with every other rand() user; in our runtime it is `ck_rand(ctx)`. There are two forms:

- `R(x) = (float)(rand() - 0x3fff) * 6.1038903e-05f * x` (`0x250931c8` = 1/16383), in [-x, 1.00006 x];
- `U(n) = trunc(rand() * 3.0518498e-05f * n)` (`0x250931cc` = 1/32767), in [0, n] (Object frame choice
  and initial texture frame variance).

"Base +- variance" always means `base + R(variance)`. Truncation (`_ftol`) is toward zero.

### 4.2 Execute-side emission (`FUN_250853a0`, then `FUN_250820e0`)

Once per active execute, after the input handling of §5, when `Activity != 0` and not frozen:

```
t = EmissionTime(local 2) + dt
d = pin0 + R(pin1)                 // one rand() every active frame, even when pin1 == 0
if d <= 0:  emit_event()           // exactly one event per frame
else while d < t: t -= d; emit_event()
... update (§6) ...
EmissionTime = t                   // also stored while Activity == 0
```

On sets `EmissionTime = pin0`. So an On frame always emits at once (`pin0 + dt > pin0`). `emit_event`
(`FUN_250820e0`):

```
n = pin11 (+ trunc(R(pin12)) if var & Emission)      // rand() before the particle loop
repeat n times:
  stop if the free list is empty or live >= pin10
  p = pop free; push p at the HEAD of the live list; live++
  InitPosition(p); InitDirection(p)                  // vtable, rand() per shape below
  s = pin4 (+ R(pin5) if var & Speed);  p.vel = dir * s
  angle / angular speed by render mode (§4.3)
  life = pin8 (+ R(pin9) if var & Lifespan);  inv = 1 / life
  colour = pin23 (+ R(pin24[c]) for r,g,b,a in that order if var & Initial Color)
  dcolour = Evol&Color ? ((pin25 (+ R(pin26[c]) if var & Ending Color)) - colour) * inv : 0   // r,g,b,a
  size = pin13 (+ R(pin14) if var & Initial Size)
  dsize = Evol&Size ? ((pin15 (+ R(pin16) if var & Ending Size)) - size) * inv : 0
  if mode == 3 or 5..9:
      frame = pin28 (+ U(pin29) if var & Initial Texture); tex_acc = 0
      texspeed = (Evol&Texture && pin32 >= 2) ? (float)pin30 (+ R((float)pin31) if var & Texture Speed) : 0
  else texspeed = 0                                   // frame left as it was, except in mode 4
  if any deflector:          bounce  = pin17 (+ R(pin18) if var & Bounce)
  if interactor Gravity:     weight  = pin19 (+ R(pin20) if var & Weight)
  last_dt = 0.01
  if interactors & 0x46:     surface = pin21 (+ R(pin22) if var & Surface)
```

Shapes (the entity is the owner frame; `Transform` = entity `+0x17c` local point to world,
`TransformVector` = `+0x184` local vector to world, which includes the frame's rotation **and scale**):

- **Point position** `0x250830a0`: `pos = entity world position` (`GetPosition(&pos, NULL)`, `+0x128`).
  No rand().
- **Point / Planar / TimeDependent direction** `0x250826a0` (4 rand(), in this order):
  ```
  cp = cos(R(pitch))          // pitch = +0x1c
  x  = sin(R(yaw)) * cp       // yaw   = +0x18
  z  = cos(R(yaw)) * cp       // a NEW rand() for this yaw angle
  y  = sin(R(pitch))          // a NEW rand() for this pitch angle
  dir = TransformVector((x, y, z))
  ```
  With zero variance this is local +Z. The two yaw and two pitch terms use independent random angles, so
  `dir` is not unit length and not a proper cone. It is also not normalised after the transform, so a
  scaled frame scales the speed.
- **Planar position** `0x25083020`: `a = R(1)`, `b = R(1)`, `pos = Transform((b, a, 0))`: the square
  [-1,1]^2 in the frame's local XY plane (2 rand(); the first one is local y).
- **Spherical position + direction** `0x250830d0`: `a = R(1)`, `b = R(1)`, `c = R(1)`,
  `v = normalize((c, b, a))`, `pos = Transform(v)`, `dir = normalize(TransformVector(v))`: the unit sphere
  around the frame, moving radially outward (3 rand(), before the speed rand()). Yaw/pitch are unused.

Per particle, a Ballance flame (Point, var 29, mode 7) consumes 4 (dir) + 1 (speed) + 1 (life) + 1 (size)
= 7 rand(). Each event also takes one rand() for its count, and each active frame one for the delay.

### 4.3 Angle, angular speed and the "Spreading" meaning by render mode

Jump table at `0x25082684`:

- modes 2 Line, 5 Orientable Sprite, 8 Comet: `angle = pin6`, `angular speed = pin7` (no rand()). For
  these modes `angle` is the streak length factor ("Spreading"): the head of the line/streak is
  `pos + vel * angle` (§7).
- modes 3 Sprite, 7 Fast Sprite: `angle = 0`; `angular speed = var & Angular Speed ? pin6 + R(pin7) : 0`.
- mode 4 Object: `frame = U(group count)` (rand() even without a group), `angle = 0`, angular speed as for
  3/7.
- modes 1, 6 and others: both 0.

### 4.4 Time-dependent trail (`FUN_25083540`, called at the top of every renderer when `+0x10d`)

This runs in the **render** callback, after the execute's normal emission and update. So a
`TT_TimedependentPointParticlesystem` emits both ways: normal events every `pin0` ms, and this trail:

```
if not emitting (+0x114 == 0): return          // nothing, not even the previous position
k = (acc + last_execute_dt) * 0.001             // seconds
r = (var & Emission) ? rate + R(pin12) : rate   // rate = trunc(pin0), read each execute into +0x124
if r * k > 0.9 and not first_call:
    n = trunc(r * k); acc = 0                   // the fraction is dropped
    cur = entity world position
    step = (prev - cur) / n
    for i in 0..n-1 (at least one; stops early if the pool is exhausted or live >= pin10):
        p = pop free, push at head; p.pos = cur + i * step
        InitDirection(p) and everything after it in §4.2 (not InitPosition)
    prev = cur
else:
    acc += last_execute_dt
    if first_call: first_call = 0; prev = entity world position
```

Differences from `FUN_250820e0`: mode 8 falls into the "0, 0" branch of §4.3, the texture branch covers
modes 3,5,6,7,9 and does not require `pin32 >= 2`. At 60 fps and rate 90, `r*k` is about 1.5, so one
particle per frame at the current position (60/s, not 90/s). The trail only interpolates when a frame
takes longer than 2/rate seconds. If the owner isn't rendered, the trail doesn't run.

## 5. Execute (`FUN_250853a0`): inputs, activity, outputs

```
E = local 0; if NULL return 0xa008
act = local 1
if pin34: dt = behcontext.DeltaTime else dt = pin35          // both pins read every call
if input 3 active: clear it; goto impacts                     // only exists with Outputs Impacts
elif input 2 (Freeze): clear; activate out 2
     if act & 2: act &= ~2; E.emitting = 1  else: act |= 2; E.emitting = 0
elif input 0 (On): clear; activate out 0
     EmissionTime = pin0; E.emitting = 1; act |= 1; register render callback (§7)
elif input 1 (Off): clear; act = 0; E.emitting = 0
local 1 = act
if act & 2: return 0                         // frozen: no emission, no update, BB goes inactive
read pins (FUN_25082770, or FUN_25083290 for the time-dependent BB)
E.last_dt = dt
emission (§4.2) if act != 0; update (§6)
impacts: if an impact is pending, output it (params 0..3) and activate out 3
if act == 0 and live list empty: unregister render callback; activate out 1 (Exit Off); return 0
return 1 (CKBR_ACTIVATENEXTFRAME)
```

Consequences:

- **Off** only stops emitting. Live particles keep moving and ageing, and are drawn, until the last one
  dies. Only then does `Exit Off` fire and the render callback go away. Off also clears the freeze bit.
- **Freeze** toggles. While frozen the BB returns 0, so it stops running; particles stay in place, still
  drawn (the render callback stays). The next Freeze pulse resumes. On while frozen sets bit 0 but the BB
  stays frozen.
- `Exit On` fires on the On frame, `Exit Freeze` on each Freeze pulse.
- Pins read in the frozen frame are not refreshed.

## 6. Per-frame update (`FUN_25081af0`)

Runs only when the live list is non-empty:

```
bbox = {min = +1e5, max = -1e5}
interactors (bits of +0xe8, FUN_2508a0c0 ..) - never in Ballance
for p in live list, head (newest) to tail:
    if p.life <= 0: unlink p, push it on the free list, live--; continue
    h = min(dt, p.life)
    p.angle += h * p.angular_speed
    grow bbox by p.pos;  p.pos += h * p.vel;  grow bbox by p.pos
    if Evol & Color: p.colour += h * p.dcolour; clamp each channel to [0, 1]
    if Evol & Size:  p.size += h * p.dsize            // no clamp
    if p.texspeed != 0 and Evol & Texture and mode != 4: texture step (below)
    p.life -= h;  p.last_dt = h
entity->SetBoundingBox(&{max, min}, FALSE)   // +0x1c4, see §9
deflectors (bits of +0xec, FUN_250880f0 ..) - never in Ballance
```

- Motion is straight-line: no gravity and no drag unless interactors are on.
- Particles born this frame are at the head and get a full `h = dt` step before their first draw.
- A particle whose life reaches 0 is drawn one more time in its exact end state (because `h` is clamped
  to the remaining life). It is freed at the start of the next update. A particle born with life <= 0 is
  freed before it is ever drawn (except trail particles, which are drawn once).
- The bounding box covers particle centres only, not their sizes.

Texture step (loop modes from pin 33, `count = pin32`):

```
acc += h; step = sign(texspeed); s = |texspeed|
while s < acc:
    frame += step
    if frame >= count: No Loop: frame = count-1;  Loop: frame = 0;  To and Fro: frame = count-2, texspeed = -texspeed
    elif frame < 0:    No Loop: frame = 0;  Loop: frame = count-1;  To and Fro: frame = 1, texspeed = -texspeed
    acc -= s
```

`step` and `s` stay as computed before the loop, even when To and Fro flips the stored speed.

## 7. Rendering

### 7.1 When it draws

`FUN_25085290(on)` first removes all nine possible render functions with
`owner->RemovePostRenderCallBack(fn, E)` (`+0x80`). With `on` it then calls
`owner->AddPostRenderCallBack(E+0x14, E, FALSE)` (`+0x7c`). So particles are drawn from the **owner
frame's post-render callback**, once per frame in which the render context renders that entity. That
requires the frame to be visible and its bounding box (set in §6) to pass the frustum test. The callback is
registered by On, SETTINGSEDITED/RESUME/ACTIVATESCRIPT (when the script is active in the scene) and LOAD
paths (§8). It is removed by Off-with-no-particles, PAUSE, DEACTIVATESCRIPT and DETACH. Deactivating the
owning script therefore makes the particles vanish at once and reactivating it brings back whatever is
still in the pool. The flames' `TT Scaleable Proximity` logic uses exactly this.

Every renderer returns early if the live count is 0. It saves the render context's world matrix (`+0x16c`)
and sets `world = saved * entity->GetInverseWorldMatrix()` (`+0x178`). During the entity's own
post-render, the saved world matrix is the entity's world matrix, so this is the identity. Particles are in
world space. The renderer restores the saved matrix at the end and also sets ZWRITEENABLE back to TRUE.

Particles are not sorted. They are drawn in live-list order, newest first.

### 7.2 Render states

Sprite-type renderers call `FUN_25082ed0(rc)` (render context `+0xe8` SetState, `+0xf0`
SetTexture(tex, clamp=0, stage 0), `+0xf4` SetTextureStageState(state, value, stage)):

- SetTexture(pin 27 texture or NULL);
- SPECULARENABLE = 0; FILLMODE = SOLID (3); SHADEMODE = FLAT (1) without a texture, GOURAUD (2) with one;
- TSS TEXTUREMAPBLEND = MODULATEALPHA (4); MAGFILTER = LINEAR (2); MINFILTER = LINEARMIPLINEAR (6);
- WRAP0 = 0; CULLMODE = NONE (1); SRCBLEND = setting 5; DESTBLEND = setting 6;
- if `dst == ZERO(1)` and `src` is ONE(2), SRCCOLOR(3) or SRCALPHA(5): ALPHABLENDENABLE = 0 and
  ZWRITEENABLE = 1. Otherwise ALPHABLENDENABLE = 1 and **ZWRITEENABLE = 0** (all Ballance instances);
- stage 1 STAGEBLEND(0), i.e. single texture stage.

ZENABLE, ZFUNC, ALPHATEST, FOG and LIGHTING are not touched; they keep the render context's current
values. The draw-primitive flags are `0x10000215` (sprites: transform, clip, diffuse, stage-0 UVs,
vertex-buffer hint) or `0x215`, and never include CKRST_DP_LIGHT. So vertices are pre-lit: the colour is
the particle colour (`RGBAFTOCOLOR`), times the texture.

Line (and Point) renderers don't call `FUN_25082ed0`. They set SetTexture(NULL), ZWRITEENABLE = 0,
SRCBLEND, DESTBLEND, ALPHABLENDENABLE = (dst != ZERO), STAGEBLEND(0) on stage 1, and use flags `0x15` (no
UVs). Shade and cull modes are inherited.

### 7.3 Geometry per mode (R, U, F = camera world matrix rows 0, 1, 2 via `rc->GetViewpoint()`
`+0x198` -> `GetWorldMatrix()` `+0x174`, rotated by the identity world above; `s` = `p.size`)

- **3 Sprite** `0x2508e240`: a camera-facing quad of width and height `s`. `r = R*s/2`, `u = U*s/2`. If
  `angle != 0`: `r' = r cos a - u sin a`, `u' = u cos a + r sin a`. Vertices (colour = particle colour for
  all 4):
  `v0 = pos - r + u` (u0, v0), `v1 = pos + r + u` (u0+du, v0), `v2 = pos + r - u` (u0+du, v0+du),
  `v3 = pos - r - u` (u0, v0+du). Indexed triangle list, 16-bit indices `0,1,2, 0,2,3` per quad (a static
  index buffer that only grows, `0x25096274`). Batches of at most 1000 quads / 4000 vertices.
- **7 Fast Sprite** `0x2508ea60`: one triangle per particle, same `r`, `u` and rotation:
  `v0 = pos + u` (u0 + du/2, v0), `v1 = pos + r - u` (u0+du, v0+du), `v2 = pos - r - u` (u0, v0+du).
  Non-indexed triangle list, 3n vertices, one draw with no batching. The flame texture sits inside that
  triangle.
- **5 Orientable Sprite** `0x2508f150`: a quad stretched along the motion, facing the camera.
  ```
  T = pos - vel * last_dt            // where the particle was one step ago (the tail)
  H = pos + vel * angle              // head; angle = pin6 = 0 in Ballance, so H = pos
  D = normalize(H - T)
  S = normalize(F x D)               // F = camera Z axis
  s0 = size - dsize * last_dt;  c0 = colour - dcolour * last_dt    // the tail uses the previous size/colour
  v0 = H + D*s + S*s   (c, u0, v0)        v1 = H + D*s - S*s   (c, u0+du, v0)
  v2 = T - D*s0 - S*s0 (c0, u0+du, v0+du) v3 = T - D*s0 + S*s0 (c0, u0, v0+du)
  ```
  Note the full size here, not half. Indices `0,1,2, 0,2,3`.
- **2 Line** `0x2508df80`: one segment per particle, `VX_LINELIST`, 2n vertices:
  `a = pos - vel*last_dt` with colour `colour - dcolour*last_dt`, and `b = pos + vel*angle` with the
  current colour. With `angle = 0` it is the path covered in the last step (a motion streak one frame
  long). Size is unused. Lines are one pixel wide.
- 1 Point `0x2508ddc0`: `VX_POINTLIST` at `pos`, colour only. 4 Object `0x250908b0` draws group members.
  6 Radial `0x25090130`, 8 Comet `0x2508fa60` and 9 `0x25090b00` are also sprite variants. None of these
  are used by Ballance.

### 7.4 Sizes

Sizes are world units: the sprite quad side is `size`, the fast-sprite triangle spans `size` in both
directions, and the orientable quad is `2*size` wide. Sizes are independent of the frame's scale. Speeds
are not (§4.2).

### 7.5 Texture atlas

If `pin32 > 1`: `n = trunc(sqrt(pin32 - 1)) + 1`, `du = 1/n`. Frame `f` maps to `u0 = (f mod n)*du`,
`v0 = (f div n)*du` (computed by repeated subtraction, so a negative `f` gives `v0 = 0`,
`u0 = f*du`). Frames run left to right, then top to bottom, V down. Otherwise `u0 = v0 = 0` and `du = 1`.

## 8. Behavior callback (`FUN_25085770`) and the manager

Messages (CKM_BEHAVIOR*):

- **3 ATTACH**: the owner must be a 3D entity (`+0xac` class flags & 2). The Object and Curve BBs skip
  this check; otherwise the callback logs "You can only attach this particul system to a Frame" and fails.
  Local 13 = manager display flag. Then the same as LOAD.
- **11 LOAD**: create the emitter (§3), store it in local 0. If not playing, add the gizmo mesh. Read the
  settings (`FUN_25082a00`: pool size, render function, blends, flag words; enable/disable pins; add or
  remove the impacts IO). Read the pins. `EmissionTime = 0`.
- **4 DETACH**: remove the gizmo, `RemovePostRenderCallBack`, destroy the emitter, local 0 = NULL.
- **5 PAUSE**: add editor gizmos, then unregister the render callback. **17 DEACTIVATESCRIPT**: unregister.
- **6 RESUME**: remove the gizmo, then like **16 ACTIVATESCRIPT**: if the parent script is active in the
  scene, register the render callback.
- **9 RESET**: `FUN_25081a60`, all particles die; add the gizmo.
- **13 SETTINGSEDITED**: re-read the settings; if playing and the script is active, register.
- **1 PRESAVE / 10 POSTSAVE / 15 NEWSCENE**: gizmo and display bookkeeping only.

The manager (`FUN_25086c20`, 0xbc bytes) registers the 10 interactor and 6 deflector attribute types
(`FUN_25086e70`): `Particle Gravity` (float, `-0.0001`), `Particle Global Wind : Force`,
`Particle Local Wind : Force/Decay`, `Particle Magnet`, `Particle Vortex`, `Particle Disruption Box`,
`Particle Mutation Box`, `Particle Atmosphere`, `Particle Tunnel`, `Particle Projector`,
`Particle Plane/Infinite Plane/Cylinder/Sphere/Box Deflector` (`Deflectors` struct
`Response;Friction;Density`, `1.0;1.0;100`), and `Particle Object Deflector` (plus `Smoothed Normal`).
Their callbacks only handle editor display meshes. The update functions (`FUN_2508a0c0` ..
`FUN_2508b120`, `FUN_25087b50` .. `FUN_250899e0`) run only for flag bits that Ballance never sets. They are
not described here.

## 9. Vtable and import identification

| Class | Offset | Method | Confidence |
|---|---|---|---|
| CKRenderObject | +0x7c / +0x80 | AddPostRenderCallBack(fn, arg, temp) / RemovePostRenderCallBack(fn, arg) | high (docs/sky.md, docs/fonts.md) |
| CK3dEntity | +0x128 | GetPosition(VxVector*, ref) | high |
| CK3dEntity | +0x174 / +0x178 | GetWorldMatrix / GetInverseWorldMatrix | high / high (makes the world identity) |
| CK3dEntity | +0x17c / +0x184 | Transform(dst, src, ref) / TransformVector(dst, src, ref) | high (argument shapes, usage) |
| CK3dEntity | +0x1c4 | SetBoundingBox(const VxBbox*, local) | medium |
| CKRenderContext | +0x98 | GetDrawPrimitiveStructure(flags, vertexCount) | high |
| CKRenderContext | +0x15c | DrawPrimitive(type, indices, count, data) | high |
| CKRenderContext | +0x160 / +0x16c | SetWorldTransformationMatrix / GetWorldTransformationMatrix | high |
| CKRenderContext | +0x198 | GetViewpoint | high |
| CKRenderContext | +0xe8 / +0xf0 / +0xf4 | SetState / SetTexture(tex, clamp, stage) / SetTextureStageState(state, value, stage) | high |

VxDrawPrimitiveData: `+0x08/+0x0c` positions/stride, `+0x18/+0x1c` colours/stride, `+0x28/+0x2c`
UVs/stride. Imports: `rand` and `_CIacos` (MSVCRT); `Vx3DRotateVector`, `Vx3DMultiplyMatrix`,
`VxVector::Normalize`, `Vx3DMatrixFromEulerAngles`, `RGBAFTOCOLOR` (VxMath). Constants: `0x250931c8`
1/16383, `0x250931cc` 1/32767, `0x250931d0` 1.0, `0x250931d4` 0.5, `0x250931e8` 0.0, `0x250931f4` 0.9,
`0x250931f8` 0.001.

## 10. Implementation notes for OpenBallance

- One BB implementation (`src/bb/`) for the four GUIDs, with a shape switch for position and direction.
  Keep the state (emitter fields, pool as an array plus live/free lists or an ordered array with the same
  newest-first order) in `bb_state`, created on LOAD/ATTACH. Local 0 in the file is garbage.
- Use `ck_rand(ctx)` in exactly the order of §4. The per-frame delay rand() and the per-event emission
  rand() are easy to miss. Other BBs share the stream, so the BB execution order matters too.
- dt: `ctx->delta_ms` when pin 34 is true or missing, otherwise pin 35.
- The renderer needs a list of "particle draws" per frame: for each registered emitter whose owner entity
  is (hierarchically) visible, build world-space vertices as in §7.3 into a dynamic vertex buffer. Draw
  them in the blended pass after opaque geometry with depth test on (the context default, less-equal),
  depth write off, cull none, the setting's blend factors, vertex colour times the texture
  (modulate, alpha included), trilinear filtering, no lighting. Untextured: vertex colour only. Lines:
  line-list topology.
- Frustum culling of the owner can be skipped (it only hides particles that are off-screen anyway, apart
  from the "centres only" bbox edge case).
- Run the time-dependent trail in the render path, or equivalently once per frame after the update when the
  owner is visible. Either way it must use the last execute's dt and the entity's world position at that
  time.

## Uncertain

- **P_Extra_Life systems without pins 34/35.** The execute reads `Real-Time Mode` and `DeltaTime` into
  uninitialised stack slots (`[esp+0x24]`, `[esp+0x1c]`). With the pins missing, those keep whatever was
  there. The original's behaviour is undefined; real-time with the frame dt is the only sensible choice.
- **Draw order relative to other objects.** The particles are drawn when the render context gets to their
  owner frame. I did not check where CK2_3D puts a mesh-less frame in its opaque/transparent ordering.
  Since the particles don't write depth, drawing them before farther opaque geometry would let that
  geometry overdraw them. Drawing them last is recommended.
- **Fog.** The renderers leave FOGENABLE alone, so in the original particles probably get the scene fog
  (tinting additive particles). Not verified against CK2_3D.
- `+0x1c4` is identified as SetBoundingBox from its use only. Culling by it is also unverified.
- Whether render callbacks of a hidden entity are skipped (`P_Modul_18_Particle` is saved hidden) is
  assumed from Virtools behaviour, not read from CK2_3D.
- Degenerate cases that occur with low probability in the game data: a lifespan of exactly 0 (Fizz:
  `200 + R(200)` with `rand() == 0`) gives `inv = inf` and a NaN colour delta. It is harmless for
  execute-emitted particles, which are freed by the same frame's update before any draw; only a trail
  particle would be drawn once like that. A zero velocity with the Orientable renderer normalises a zero
  vector.
- Unused renderers (1, 4, 6, 8, 9), deflectors and interactors are only summarised.
