# IVP core and per-PSI dynamics (port spec)

This is the implementation spec for the dynamics half of the Ipion "IVP" engine inside `physics_RT.dll`: real
objects and their cores, mass and inertia setup, pushes and impulses, damping, gravity, the controller
manager and the PSI step order, integration, anomaly clamps, pose interpolation and write-back, sleep and
wake-up of simulation units, the time manager, and the spring and constraint controllers. Collision
detection (mindist, hull manager internals, OV tree), the friction system and the impact solver are out of
scope; where the dynamics hand data to them, the interface is given.

It complements `docs/physics.md` (BB values, manager, address map). Addresses are entry points in
`physics_RT.dll`, image base `0x10000000`. Everything below was read from the disassembly; the Ghidra output
in `re/physics_RT.dll.c` was used as a guide only (its float/double casts are frequently wrong, see 0.2).

The original names in quotes (`IVP_Core`, `calc_next_PSI_matrix`, ...) are our identifications. They match
the layout and behaviour of the IVP SDK that Valve later shipped, but the DLL carries no symbols.

## 0. Conventions

### 0.1 Types and storage

- `f32` = IEEE single, `f64` = IEEE double.
- `IVP_U_Point`: 3 × f64, **padded to 0x20 bytes** (a 4th f64 slot is never read).
- `IVP_U_Float_Point`: 3 × f32 (0xc bytes). Some members are 0x10 with a pad/hesse slot; offsets below are exact.
- `IVP_U_Quat`: 4 × f64 `(x, y, z, w)`, 0x20 bytes.
- `IVP_U_Matrix`: 3 rows of 4 × f64 (row stride 0x20, entry `[r][c]` at `r*0x20 + c*8`, column 3 unused) =
  0x60 bytes, then `vv` (translation, IVP_U_Point) at `+0x60`. Total 0x80.
  - Column-vector convention: `world = R · local + vv` with `R[r][c]` as stored.
  - `m_world_f_core` maps core coordinates to world. "core" = the mass-centre frame.
- Times are f64 seconds of simulated time.

### 0.2 Precision

The code is x87 with the control word at 64-bit mantissa during simulation (`docs/physics.md` 4). All
intermediate arithmetic is extended precision; values are rounded only when stored. In the pseudocode:

- a variable or field declared `f32` is rounded to single when assigned;
- `(f32)x` marks a store/rounding that is actually in the code;
- unmarked expressions are evaluated in high precision (use f64 in the port).

A port in f64 for intermediates and f32 for f32 fields reproduces the stored values to within an ulp most of
the time; bit-exactness is not achievable.

### 0.3 Notation for the 2-bit flag fields

IVP packs booleans into 2-bit bitfields (`IVP_BOOL:2`). "field at bits 2-3" means `(flags >> 2) & 3`. A value of
1 is TRUE.

### 0.4 Movement states (`IVP_Movement_Type`, one byte, compared as *signed char*)

| Value | Name | Meaning |
|---|---|---|
| 1 | `IVP_MT_MOVING` | simulated, moving (set on revive and by the sleep test) |
| 2 | `IVP_MT_SLOW` | simulated, inside the short calm window but not long enough (extra damping +0.1) |
| 3 | `IVP_MT_CALM` | simulated, calm long enough to freeze (extra damping +0.1) |
| 8 | `IVP_MT_NOT_SIM` | frozen / not simulated (also the state of a new object until revived) |
| 0x10 | `IVP_MT_STATIC` | unmovable object (real-object state only) |
| 0x21 | | temporary core state set by `0x100099f0` around a mindist-manager call |

Tests in the code are `(signed char)state < 8` ("simulated") and `(signed char)state >= 2` ("slow or calm",
damping). The state byte of a core is rewritten only by the sleep test (10.2), revive (1) and freeze (8).

## 1. Data structures

Only fields the dynamics read or write are listed. Unknown fields are left out or marked `?`.

### 1.1 Real object (`IVP_Real_Object`, base ctor `0x10009690`; ball `0x10030010`, polygon `0x1002fd00`)

| Offset | Type | Meaning |
|---|---|---|
| `+0x00` | vtable | base `0x10063390`, polygon `0x10063a84`, ball `0x10063abc`. Slot 2 (`+8`) = `set_new_m_object_f_core` (`0x100093d0`) |
| `+0x04` | i32 | object type: 2 = polygon, 3 = ball |
| `+0x14` | char* | name (copied) |
| `+0x18` | env* | environment |
| `+0x1c..+0x28` | | zeroed in the ctor (lists) |
| `+0x24` | ptr | head of the object's synapse (mindist) list; walked after controllers (`0x100099a0`) |
| `+0x2c` | IVP_U_Quat* | `q_core_f_object`, NULL when the object→core rotation is identity (always, in Ballance) |
| `+0x30` | f32[3] | `shift_core_f_object`: object origin in core coordinates (= −mass centre) |
| `+0x40` | ptr | per-PSI pose cache object (`0x10018930`), see 4.4 |
| `+0x48` | struct | hull manager (collision; 8.4) |
| `+0x80` | u32 | flags. Low byte = object movement state (0.4): 1 simulated, 8 not simulated, 0x10 unmovable. Bits 8-9: collision enabled (`0x100`, `0x10009350`). Bits 10-11 = 1: `shift_core_f_object` is zero |
| `+0x88` | ptr | anchor list (constraint/spring anchors; each re-expressed in core space when the mass centre changes) |
| `+0x8c` | ptr | surface manager (polygon) or the static ball manager `0x10076380` (ball) |
| `+0x90` | char[8] | no-collision group (`strncpy` 8 from template `+4`) |
| `+0x98` | ptr | material (template `+0x14`) |
| `+0x9c` | ptr | collision-system handle (`0x10009350`) |
| `+0xa0` | f32 | `extra_radius`; for balls = template extra radius + ball radius |
| `+0xa4` | IVP_Core* | physical core |
| `+0xa8` | IVP_Core* | original core (same as `+0xa4`; used by revive of frozen objects) |
| `+0xac` | IVP_Core* | third core pointer (same) |
| `+0xb0` | void* | client data (template `+0x68`); the glue stores the CK entity here |

### 1.2 Core (`IVP_Core`, 0x238 bytes, ctor `0x1000d400`)

| Offset | Type | Name | Meaning |
|---|---|---|---|
| `+0x00` | u32 | flags | bits 0-1 `fast_piling_allowed` (template `+0x10`, 0 in Ballance); bits 2-3 `physical_unmoveable` (template `+0xc`; test `flags & 0xc`); bits 4-5 `is_in_wakeup_vec`; bits 6-7 `rot_inertias_are_equal` (set by `0x1000d1a0`) |
| `+0x04` | f32 | `upper_limit_radius` | max distance of surface from the mass centre (+ extra radius) |
| `+0x08` | f32 | `max_surface_deviation` | |
| `+0x0c` | env* | `environment` | |
| `+0x14` | f32[3] | `rot_inertia` | principal inertia (core axes) |
| `+0x20` | f32 | `mass` | |
| `+0x24` | f32[3] | `rot_speed_damp_factor` | per-axis rotational damping (1/s) |
| `+0x34` | f32[3] | `inv_rot_inertia` | 1/`rot_inertia` |
| `+0x40` | f32 | `inv_mass` | 1/`mass` |
| `+0x44` | f32 | `speed_damp_factor` | linear damping (1/s) |
| `+0x48` | f32 | `inv_object_diameter` | 0.5 / `upper_limit_radius` |
| `+0x4c` | f32* | `max_rot_speed` | optional per-axis clamp of `rot_speed` (NULL in Ballance) |
| `+0x50` | u16, u16, ptr | `objects` | vector of real objects (cap, count, data), inline storage at `+0x58` (1 entry) |
| `+0x5c` | ptr | ? | deleted in the dtor only for unmovable cores |
| `+0x60` | u8 | `movement_state` | 0.4. Byte `+0x61` is cleared every PSI |
| `+0x64` | u16 | ? | cleared every PSI (per-PSI impact/collision counter) |
| `+0x68` | f64 | `time_of_last_psi` | |
| `+0x70` | f32 | `i_delta_time` | 1 / PSI length |
| `+0x74` | f32[3] | `rot_speed_change` | accumulated async angular pushes (core axes) |
| `+0x84` | f32[3] | `speed_change` | accumulated async linear pushes (world) |
| `+0x94` | f32[3] | `rot_speed` | angular velocity, **core axes**, rad/s |
| `+0xa4` | f32[3] | `speed` | linear velocity of the mass centre, **world**, m/s |
| `+0xb8` | IVP_U_Point | `pos_world_f_core_last_psi` | mass-centre position at `time_of_last_psi` |
| `+0xd8` | f32[3] | `delta_world_f_core_psis` | the velocity used to extrapolate the position from `time_of_last_psi` (= `speed` committed at the last integration) |
| `+0xe8` | IVP_U_Quat | `q_world_f_core_last_psi` | rotation at `time_of_last_psi` |
| `+0x108` | IVP_U_Quat | `q_world_f_core_next_psi` | rotation at the next PSI |
| `+0x128` | IVP_U_Matrix | `m_world_f_core_last_psi` | pose at the current PSI (rotation from `q_next`, see 6.3) |
| `+0x1a8` | f32[3] | `rotation_axis_world` | unit axis of this PSI's rotation |
| `+0x1b8` | f32 | `current_speed` | \|speed\| |
| `+0x1bc` | f32 | `abs_omega` | rotation speed of this PSI (rad/s) |
| `+0x1c0` | f32 | `max_surface_rot_speed` | `abs_omega · max_surface_deviation` |
| `+0x1c8` | u16, u16, ptr | `controllers_of_core` | vector, inline storage `+0x1d0` |
| `+0x1d4` | IVP_Sim_Unit* | `sim_unit_of_core` | |

Section 10 lists the sleep-test fields (`+0x1d8..+0x238`).

### 1.3 Template (`IVP_Template_Real_Object`, 0x70 bytes, default ctor `0x10015ef0`)

| Offset | Type | Default | Glue (`0x10007460`) writes |
|---|---|---|---|
| `+0x00` | char* | NULL | entity name |
| `+0x04` | char[8] | "" | collision group |
| `+0x0c` | i32 `physical_unmoveable` | 0 | Fixed |
| `+0x10` | i32 `enable_piling_optimization` | 0 | |
| `+0x14` | material* | NULL | the per-body material |
| `+0x18` | f64 `mass` | 1.0 | (f64)Mass |
| `+0x20` | i32 `rot_inertia_is_factor` | 1 | 1 |
| `+0x24` | f32[3] `rot_inertia` | (1, 1, 1) | |
| `+0x34` | f32 `auto_check_rot_inertia` | 0.03 (`0x3cf5c28f`) | |
| `+0x38` | f64 `speed_damp_factor` | `0x3f847ae140000000` (= (f64)0.01f) | (f64)Linear damp |
| `+0x40/48/50` | f64[3] `rot_speed_damp_factor` | same | (f64)Rot damp on each axis |
| `+0x60` | f32 `extra_radius` | 0 | |
| `+0x64` | IVP_U_Matrix* `mass_center_override` | NULL | identity rotation, `vv` = (f64)Shift, when Auto Mass Center is FALSE |
| `+0x68` | void* `client_data` | NULL | |

Ball template: radius = the Physicalize radius (2 for every game ball).

Pose passed to the object ctor: position = entity world matrix row 3 (f64), rotation =
`VxQuaternion::FromMatrix(transposeof3x3(M), MatIsUnit=TRUE, RestoreMat=TRUE)` widened to f64. The entity
matrix contains the scale, so for scaled entities the quaternion is not unit; the core ctor normalizes it
(`0x100194e0`). For uniformly scaled matrices the result is the right rotation; for non-uniform scale it
differs slightly from a proper polar decomposition (keep: it is what the game sees).

### 1.4 Simulation unit (`IVP_Sim_Unit`, 0x24 bytes, ctor `0x10011920`)

| Offset | Type | Meaning |
|---|---|---|
| `+0x00` | u32 | flags. Low byte = movement state (1 = simulated, 8 = frozen). Bits 8-9 (`0x300`): "structure changed, split/merge" (`0x100120d0`). Bits 10-11 (`0x400`/`0x800`): "some core faster than 1 m/s this PSI" (`0x800` cleared, never set). Bits 12-13: the same field one PSI later |
| `+0x04` | ptr | previous sim unit in the manager's list |
| `+0x08` | ptr | next sim unit in the manager's list |
| `+0x0c` | u16, u16, ptr | cores (cap, count, data), inline storage `+0x14` (2 entries) |
| `+0x1c` | u16, u16, ptr | controller entries (cap, count, data), sorted by ascending priority |

Controller entry (0xc bytes): `{ IVP_Controller *c; u16 cap; u16 n; IVP_Core **cores; }`, the cores of this
unit that the controller acts on.

### 1.5 Simulation event (`IVP_Event_Sim`, built on the stack)

| Offset | Type | Meaning |
|---|---|---|
| `+0x00` | f64 | `delta_time` (the PSI length, env `+0xc0`) |
| `+0x08` | f64 | `i_delta_time` |
| `+0x10` | env* | |
| `+0x14` | IVP_Sim_Unit* | set by `0x100121b0` |

### 1.6 Environment fields used here

| Offset | Type | Meaning |
|---|---|---|
| `+0x00` | ptr | gravity controller (1.7) |
| `+0x04` | ptr | time manager (11.1) |
| `+0x08` | ptr | sim-unit manager (10.0); `+0x18` = head of the active list, `+0x1a8` = head of the frozen list |
| `+0x0c` | ptr | cluster / per-object listener manager |
| `+0x10` | ptr | mindist manager |
| `+0x20` | ptr | anomaly manager (9) |
| `+0x24` | ptr | anomaly limits (9) |
| `+0x28` | ptr | performance counter (vtable `+0` start, `+4` = `pcount(phase)`, `+8` end) |
| `+0x98` | f32 | `freeze_check_time` = 0.3 (`0x3e99999a`, set by `0x10012a50`) |
| `+0xb4` | ptr | temporary-memory pool (u16 nesting counter at `+0x10`; `0x100201b0` releases) |
| `+0xc0` | f64 | `delta_PSI_time` = 1/66 (`0x3f8f07c1f07c1f08`, `0x10013240`) |
| `+0xc8` | f64 | `inv_delta_PSI_time` = 1.0 / `delta_PSI_time` (66) |
| `+0xfe/+0x100` | u16 count, ptr | PSI listeners |
| `+0x106/+0x108` | u16 count, ptr | cores queued for revive (`core_revive_list`) |
| `+0x120` | f64 | `current_time` (0 initially) |
| `+0x128` | f64 | `time_of_next_psi` (`delta_PSI_time` initially) |
| `+0x130` | f64 | `time_of_last_psi` (0 initially) |
| `+0x138` | u32 | time-change counter: starts at 1, +1 on every `set_current_time` (time code for the pose caches) |
| `+0x140` | u16 | global sleep-test countdown, initially 10 (10.1) |
| `+0x144` | i32 | state: 0 PSI start, 2 hull pass, 3 short mindists, 4 critical mindists, 5 between PSIs (initial) |
| `+0x150/+0x152/+0x154` | u16 cap, u16 n, ptr | global object listeners (vtable `+0` deleted, `+8` revived, `+0xc` frozen) |

Section 11 has the time manager.

### 1.7 Gravity controller (vtable `0x10063598`, 0x14 bytes, env `+0`)

| Offset | Type | Meaning |
|---|---|---|
| `+0x04` | f32[3] | gravity (m/s²); set by `0x10011fe0` from an f64 vector |

Priority 1000. Every core is attached to it at creation (`0x1000d2f0`).

## 2. Object and core creation

### 2.1 `0x10009690` real object ctor `(env, surface, tmpl, quat*, pos*)`

```
base ctor 0x10009b00(env, tmpl)          // links into env object list, copies name
obj.flags80 = 0; zero +0x1c,+0x20..+0x40, +0x88, +0x9c
obj.surface(+0x8c) = surface
strncpy(obj.nocoll(+0x90), tmpl+4, 8)
if quat == NULL: quat = (0,0,0,1)          // local identity, position unchanged
core = new IVP_Core(obj, quat, pos, tmpl.physical_unmoveable(+0xc), tmpl.enable_piling(+0x10))   // 0x1000d400
obj+0xa4 = obj+0xa8 = obj+0xac = core
if tmpl.physical_unmoveable: obj.flags80 low byte = 0x10 (IVP_MT_STATIC)
else                         obj.flags80 low byte = 8    (IVP_MT_NOT_SIM)
obj.material(+0x98) = tmpl+0x14
env->(+0xc)->(+0xc) add object (0x1000adf0)       // cluster manager, collision side
obj.client_data(+0xb0) = tmpl+0x68
```

Subclass ctors then set the vtable and type, and:

- ball (`0x10030010`): `obj.extra_radius = tmpl.extra_radius + ball.radius`; surface = the static ball manager.
- polygon (`0x1002fd00`): `obj.extra_radius = tmpl.extra_radius`.

Both then call `init_object_core` `0x10009f00(env, tmpl)` (2.4) and register the object with the environment
(`0x10013ac0`).

### 2.2 `0x1000d400` core ctor `(obj, q*, pos*, unmovable, piling)`

```
0x1000d2f0(obj):                     // init
    zero all 0x238 bytes
    objects = {cap 1, data = inline +0x58}; objects.add(obj)
    environment = obj.env
    sim_unit_of_core = new IVP_Sim_Unit (0x10011920: flags low byte 8, cores cap 2 inline, no controllers)
    sim_unit.add_core(core)          // 0x100119d0
    sim_unit.flags low byte = 8
    add_core_controller(env.gravity_controller)   // 0x10011c70, see 6.4
    movement_state = 8               // IVP_MT_NOT_SIM
controllers_of_core = {0, 0, NULL}
flags bits 2-3 = unmovable & 3
env.sim_unit_manager.add_sim_unit(sim_unit)      // 0x10011ef0
q_world_f_core_next_psi = *q ; normalize (0x100194e0)
q_world_f_core_last_psi = q_world_f_core_next_psi
pos_world_f_core_last_psi = *pos
delta_world_f_core_psis = (0,0,0)
m_world_f_core_last_psi.R = matrix(q_last)        // 0x100190c0
m_world_f_core_last_psi.vv = *pos
upper_limit_radius = 1000.0f; max_surface_deviation = 1000.0f   // 0x447a0000, replaced in 2.4
flags bits 0-1 = piling & 3
```

At this point the core sits at the **object** origin; 2.4 moves it to the mass centre.

### 2.3 `0x100194e0` quaternion normalize (cheap)

```
n2 = x² + y² + z² + w²   (f64)
if n2 > 1e-19 (0x3bfd83c94fb6d2ac): s = 1/sqrt(n2); x,y,z,w *= s
```

### 2.4 `0x10009f00` `init_object_core(env, tmpl)` — mass centre, radius, mass, inertia, damping

```
// mass centre in object coordinates
if tmpl.mass_center_override (+0x64) != NULL:
    mc = (f32) override.vv          // only the translation; the override's rotation is ignored
else:
    mc = surface->get_mass_center()  // vtable +4
m_object_f_core = { R = identity (from quat (0,0,0,1)), vv = (f64)mc }
this->set_new_m_object_f_core(&m_object_f_core)   // vtable +8 = 0x100093d0 (2.5)

core = obj.core
switch obj.type:
  2 polygon: surface->get_radius_and_radius_dev_to_given_center(&mc, &radius, &radius_dev)  // vtable +8
  3 ball:    radius = 0; radius_dev = 0
  other:     crash (write to address 0)
radius = (f32)(obj.extra_radius + radius)      // for a ball: the ball radius
core.upper_limit_radius = radius; core.max_surface_deviation = radius_dev     // 0x1000c4f0

mass = tmpl.mass (f64)
if mass < 1e-8 (0x3e45798ee2308c3a): mass = 1.0

if tmpl.rot_inertia_is_factor (+0x20):
    switch obj.type:
      2: I = surface->get_rotation_inertia()       // vtable +0xc, unit-mass inertia
      3: I = (f32)(radius*radius*0.4f) on all three axes      // 0.4 = 0x3ecccccd (f32)
    core.rot_inertia[i] = (f32)(I[i] * tmpl.rot_inertia[i] * mass)
else:
    core.rot_inertia[i] = tmpl.rot_inertia[i]

if tmpl.auto_check_rot_inertia (+0x34) != 0.0f:
    m = |core.rot_inertia| * tmpl.auto_check_rot_inertia     // Euclidean length of the 3-vector (0x1000e480)
    for i in 0..2: if core.rot_inertia[i] < m: core.rot_inertia[i] = (f32)m
    // test is "store when m > I_i"; equal values are left

core.mass = (f32)mass
core.speed_damp_factor = (f32)tmpl.speed_damp_factor
core.rot_speed_damp_factor[i] = (f32)tmpl.rot_speed_damp_factor[i]
(m_core_f_object = inverse(m_object_f_core), computed and unused)
core.rot_speed = core.speed = (0,0,0)
core.calc_calc()                 // 0x1000d1a0 (2.6)
core.transform_to_mass_center(&m_object_f_core)    // 0x1000d270 (2.7)

ev.delta_time = env.time_of_next_psi(+0x128) - env.current_time(+0x120)
ev.i_delta_time = ev.delta_time > 1e-10 (0x3ddb7cdfe0000000) ? 1.0/ev.delta_time : 9999999866.485682 (0x4202a05f1bd3e2ad)
ev.env = env
core.init_core_for_simulation(&ev)  // 0x1000cc70 (2.8)
```

Ballance values (template defaults, `docs/physics.md` 2.1): balls have radius 2 and mass m, so inertia is
1.6·m on each axis; auto-check (0.03·|I|) never binds for balls. For polygons the inertia comes from the
compact surface (unit mass, about the surface's mass centre, principal axes assumed aligned with object axes,
since the code takes three numbers) times the mass, then auto-checked against 0.03·|I|.

**Mass centre in Ballance.** The glue always passes an override (Auto Mass Center is never TRUE on a creating
instance), so `mc = Shift` (default 0). The surface's own mass centre is therefore **not** used: the core is at
the object origin plus Shift, and the surface inertia (computed about the surface's mass centre) is applied
about that point without a parallel-axis correction. Port exactly that.

### 2.5 `0x100093d0` `set_new_m_object_f_core(const IVP_U_Matrix *m_object_f_core)`

```
m_core_f_object = inverse(*m_object_f_core)    // 0x1000f2c0: R^T, vv = -(R^T vv)
for each anchor a in obj.anchor_list (+0x88, next at +0):
    a.core_pos(+0x1c, f32[3]) = m_core_f_object * a.object_pos(+0xc, f32[3])   // 0x1000f4c0
v = m_core_f_object.vv
if |v|² < 1e-16 (0x3c9cd2b297d889bd):
    obj.flags80 bits 10-11 = 1 ; obj.shift_core_f_object = (0,0,0)
else:
    obj.shift_core_f_object = (f32)v ; obj.flags80 bits 10-11 = 0
if m_object_f_core.R[0][0] == 1.0 && R[1][1] == 1.0 && R[2][2] == 1.0:
    delete obj.q_core_f_object; obj.q_core_f_object = NULL
else:
    if NULL: obj.q_core_f_object = new IVP_U_Quat
    *obj.q_core_f_object = quat(m_core_f_object.R)   // 0x100191b0, then normalize 0x100194e0
if obj.cache_object (+0x40): invalidate (0x10018910)
```

### 2.6 `0x1000d1a0` `calc_calc()`

```
inv_rot_inertia[i] = (f32)(1.0f / rot_inertia[i])  for i = 0,1,2
inv_mass = 1.0f / mass
a = inv_rot_inertia
d = (a.y - a.z)² + (a.x - a.y)² + (a.z - a.x)²
if d < (I.z² + I.y² + I.x²) * 0.01  (f64 0x3f847ae151eb8520 = (f64)0.01f):
    flags bits 6-7 = 1        // rot_inertias_are_equal
else
    flags bits 6-7 = 0
inv_object_diameter = 0.5f / upper_limit_radius
```

Quirk: it compares the spread of the **inverse** inertias with the squared length of the inertias, which is
dimensionally inconsistent. Keep it. For a ball the three values are identical and the flag is TRUE; for
most boxes it is FALSE unless the inertia is large (> ~10, where the inverse spread becomes tiny): the flag
selects the integrator path in 8.2.

### 2.7 `0x1000d270` `transform_to_mass_center(const IVP_U_Matrix *m_object_f_core)`

```
m_world_f_core_last_psi = m_world_f_core_last_psi * m_object_f_core     // 0x1000ec90: R = R·R', vv = R·vv' + vv
pos_world_f_core_last_psi = m_world_f_core_last_psi.vv
q_world_f_core_last_psi = quat(m_world_f_core_last_psi.R)   // 0x100191b0
q_world_f_core_next_psi = q_world_f_core_last_psi
```

So the core position = object position + R_object · mc.

### 2.8 `0x1000cc70` `init_core_for_simulation(const IVP_Event_Sim *ev)`

```
i_delta_time = (f32)ev.i_delta_time
q_next = q_last                         // copies +0xe8 -> +0x108
abs_omega(+0x1bc) = 0
speed = (0,0,0)
delta_world_f_core_psis = (0,0,0)
current_speed(+0x1b8) = 0 ; max_surface_rot_speed(+0x1c0) = 0
rotation_axis_world = (1, 0, 0)
```

`rot_speed` is not cleared here (it was cleared in 2.4). `time_of_last_psi` is not set here; the revive path
(section 10) sets it.

## 3. Math helpers

| Address | Function | Definition |
|---|---|---|
| `0x100190c0` | quat → matrix | q = (x,y,z,w) f64. `R = [[1-2(y²+z²), 2xy-2zw, 2yw+2zx], [2zw+2xy, 1-2(z²+x²), 2zy-2xw], [2zx-2yw, 2xw+2zy, 1-2(y²+x²)]]`. Computed as `x2=2x` etc.; e.g. `R[0][0] = 1 - (z2*z + y2*y)`. Does not touch `vv` |
| `0x100191b0` | matrix → quat | `t = R00+R11+R22`. If `t > 0`: `s = sqrt(t+1); w = s*0.5; s = 0.5/s; x = (R21-R12)s; y = (R02-R20)s; z = (R10-R01)s`. Else pick i = argmax diagonal (`i = R00<R11 ? 1 : 0; if R[i][i] < R22: i = 2`), j = (i+1)%3, k = (j+1)%3: `s = sqrt(R[i][i] - (R[k][k] + R[j][j]) + 1); q[i] = s*0.5; if s != 0: s = 0.5/s; w = (R[k][j] - R[j][k])s; q[j] = (R[j][i] + R[i][j])s; q[k] = (R[k][i] + R[i][k])s`. Then normalize (2.3) |
| `0x100194e0` | normalize | 2.3 |
| `0x10019540` | normalize (iterative) | `n = |q|²`; if `|1 - n| > 1e-12`: `y = 1.5 - 0.5n`; repeat `y = (1 - y²n)·0.5 + y` while `|1 - y²n| > 1e-12`; `q *= y` |
| `0x1001e7c0` | Hamilton product `r = a ⊗ b` | `r.x = a.w b.x + a.x b.w + a.y b.z − a.z b.y`; `r.y = a.w b.y − a.x b.z + a.y b.w + a.z b.x`; `r.z = a.w b.z + a.x b.y − a.y b.x + a.z b.w`; `r.w = a.w b.w − a.x b.x − a.y b.y − a.z b.z` (output may alias inputs; all products read before writing) |
| `0x10019320` | interpolate `r = slerp(a, b, t)` | see below |
| `0x10018f80` | rotation quaternion, polynomial (8.2) | `h = dt*0.5; for i: s_i = ω_i h; s_i = s_i − s_i³·(f32)(1/6)` (`0x3e2aaaab`); `w = sqrt(1 − |s|²)` |
| `0x10019010` | rotation quaternion, sine (8.2) | `h = dt*0.5; s_i = sin(ω_i h)` (x87 `fsin`, stored f64); `n = |s|²`; if `n > 1.0`: `s *= 0.99999999 (0x3feffffffaa19c47) / sqrt(n)`; `w = sqrt(1 − |s|²)` |
| `0x1000ec90` | matrix product `C = A·B` | `C.R = A.R·B.R; C.vv = A.R·B.vv + A.vv` |
| `0x1000f2c0` | inverse (orthonormal) | `R' = Rᵀ; vv' = −Rᵀ·vv` |
| `0x1000f6f0` | `R·v` (f64) | |
| `0x1000f760` | `Rᵀ·v` (f64) | |
| `0x1000f7d0` | `Rᵀ·v` (f32 in, f32 out) | |
| `0x1000f370` | `Rᵀ·(p − vv)` (f64 in, f32 out) | |
| `0x1000f3e0` | `Rᵀ·(p − vv)` (f64) | |
| `0x1000f5f0` | `R·v + vv` (f32 v, f64 out) | |
| `0x1000f4c0` | `R·v + vv` (f32 in/out) | |
| `0x1000e2c0` | f32 cross product | |
| `0x1000e480` | f32 length | |
| `0x1001e7a0` | f32 squared length | |
| `0x1000e120` | f64 normalize | `n = |v|²`; if `n < 1e-19` return 0 (unchanged). Else `y` = bit-trick initial guess (`hi32 = (0x7ff00000 − hi32(n))/2 + 0x1ff00000`), 5 Newton steps `y = (0.5 − 0.5n·y² + 1)·y`, `v *= y`, return 1 |

**`0x10019320` slerp(a, b, t)** (all f64):

```
d = a·b (4-component dot)
if d <= 0: d = -d; sign = -1.0f else sign = +1.0f
if d < 0.9989999999525025 (0x3feff7ced9100000 = (f64)0.999f):
    θ = acos(d)
    k = 1/sqrt(1 - d²)
    ca = sin((1 - t)·θ)·k
    cb = sin(t·θ)·sign·k
    r = cb·b + ca·a                    // all four components
else:
    r = (sign·b − a)·t + a             // componentwise lerp
    n = |r|²·0.5
    y = 1.5 − n
    y = (0.5 − y²·n) + y               // two Newton steps of 1/sqrt(2n) around 1
    y = (0.5 − y²·n) + y
    r *= y
```

## 4. Pose queries and write-back

### 4.1 `0x1000c510` `core.get_m_world_f_core_PSI(t, out)` — core pose at time t

```
dt = t - time_of_last_psi
if (signed char)movement_state < 8 and dt != 0.0:
    q = slerp(q_last(+0xe8), q_next(+0x108), (f64)i_delta_time · dt)
    out.R = matrix(q)
    out.vv = pos_world_f_core_last_psi + delta_world_f_core_psis · dt    // f64
else:
    out = m_world_f_core_last_psi       // 0x80-byte copy
```

Used by the force controller, Physics Impulse, world-space pushes.

### 4.2 `0x10009d70` `obj.get_m_world_f_object_AT(t, out)` — object pose at time t (write-back)

```
core = obj.core
dt = t - core.time_of_last_psi
q = slerp(core.q_last, core.q_next, dt · (f64)core.i_delta_time)       // no movement-state test
out.vv = core.pos_world_f_core_last_psi + (f64)core.delta_world_f_core_psis · dt
out.R = matrix(q)
if (obj.flags80 & 0xc00) == 0:            // shift not zero
    out.vv = out.R · obj.shift_core_f_object + out.vv        // 0x1000f5f0
if obj.q_core_f_object:
    q = q ⊗ *obj.q_core_f_object ; out.R = matrix(q)
```

For a frozen core `q_last == q_next` and `delta_world_f_core_psis == 0` (section 10), so the result is static.

### 4.3 `0x10006ec0` glue write-back (per awake object, from PostProcess)

```
M = obj.get_m_world_f_object_AT(env.current_time)        // via 0x10009c40
Vx[c][r] = (f32)M.R[r][c]  for r,c < 3 ; Vx[0..2][3] = 0
Vx[3] = ((f32)M.vv.x, (f32)M.vv.y, (f32)M.vv.z, 1.0f)
entity->SetWorldMatrix(Vx, FALSE)       // vtable +0x170
```

`env.current_time` is the frame's target time after `simulate` (section 11), so poses are extrapolated
forward from the last PSI by up to one PSI.

### 4.4 Pose cache (`0x10018930` alloc, `0x10018a40` update), collision-facing

A 0xd0-byte cache per object (ring of entries in env `+0xa4`), valid while `cache+0 == env.psi_counter(+0x138)`.
`0x10018a40` stores, at `env.current_time`: `+0x10` quaternion (`q_last` copied when `dt == 0`, else the slerp of
4.1), `+0x30` `m_world_f_object` (as 4.2), `+0x90` and `+0xb0` the core position. Rebuilt lazily by collision
code when stale. The dynamics never read it; a port can compute poses on demand.

## 5. Pushes and impulses

All pushes are f32.

| Address | Name | Effect |
|---|---|---|
| `0x1000ca80` | `calc_push_core(p_cs, imp_cs, imp_ws, out_dv, out_dw)` | `out_dw = inv_rot_inertia ⊙ (p_cs × imp_cs)`; `out_dv = imp_ws · inv_mass` |
| `0x1000c830` | `async_push_core(p_cs, imp_cs, imp_ws)` | `rot_speed_change += out_dw; speed_change += out_dv` (deferred to the next commit) |
| `0x1000c8b0` | `push_core(p_cs, imp_cs, imp_ws)` | `rot_speed += out_dw; speed += out_dv` (immediate; used by the constraint solver) |
| `0x1000c940` | `async_push_core_ws(p_ws f64*, imp_ws f32*)` | `r = (f32)(p_ws − m_world_f_core_last_psi.vv)`; `t = r × imp_ws` (f32); `rot_speed_change += inv_rot_inertia ⊙ (Rᵀ t)` with R = `m_world_f_core_last_psi.R`; `speed_change += imp_ws · inv_mass`. Used by the spring actuator |
| `0x1000cb20` | `calc_rot_speed_change(dw_cs, k, out)` | `out = inv_rot_inertia ⊙ dw_cs · (f32)k` |
| `0x1000cb70` | `rot_push_core_cs(t_cs)` | `rot_speed += inv_rot_inertia ⊙ t_cs` (immediate) |
| `0x1000c810` | `get_surface_speed_on_test(p_cs, out)` | `out = speed + R·(rot_speed × p_cs)` with R = `m_world_f_core_last_psi.R` (`0x1000bf90`) |
| `0x1000cbd0` | `commit_all_async_pushs()` | `rot_speed += rot_speed_change; speed += speed_change;` both changes = 0 |
| `0x1000a3e0` | object `async_push_object_ws(p_ws f64*, imp_ws f32*)` | wake the object (10.4); `M = core.get_m_world_f_core_PSI(env.current_time)`; `p_cs = (f32)(Mᵀ(p_ws − M.vv))`; `imp_cs = Rᵀ·imp_ws` (f32); `core.async_push_core(p_cs, imp_cs, imp_ws)` |
| `0x1000a350` | object `async_add_speed_object_ws(dv)` | wake; `core.speed_change += dv` |
| `0x1000a3a0` | object `async_add_rot_speed_object_cs(dw)` | wake; `core.rot_speed_change += dw` |

Note `0x1000c940` uses `m_world_f_core_last_psi` (the pose at the current PSI), whereas `0x1000a3e0` uses the
pose interpolated at `env.current_time`; inside a PSI the two agree because `current_time == time_of_last_psi`
after integration, and `m_world_f_core_last_psi` is the pose at the PSI start (6.3).

## 6. The PSI step

### 6.1 `0x10013cb0` `env.simulate_psi()`

```
pcount(1); state(+0x144) = 0
if revive_list.count (+0x106): revive_cores_PSI()  0x1000b590   // revive every queued core (0x1000dab0), clear its is_in_wakeup_vec bits 4-5, empty the list
if env+0x2c (range manager): cluster_manager(env+0xc).check_for_unused_objects(range_mgr)   0x1000a9f0   // not used by Ballance (no range manager is installed? see Q)
env+0xa8->vtbl[0x28](env)               // collision delegator root: environment begins PSI
fire PSI listeners: for i = n-1..0: listeners[i]->vtbl[0](&{env})        0x10013c40
mindist_manager.pre-PSI pass            0x10017790 (collision)
pcount(2)
cores_this_psi = vector(cap 0x80, inline)
controller_manager(env+8).simulate_sim_units(env, &cores_this_psi)        0x100124c0  (6.2)
pcount(3)
hulls = vector(cap 0x80)
integrate: 0x1001ea50(env, &cores_this_psi, &hulls)                        (8.1)
pcount(4); state = 2
fire hull events: 0x1001eb10(env, &hulls)                                  (8.4, collision)
pcount(5); state = 3
mindist_manager.recheck short mindists   0x10017850  (collision)
pcount(6); state = 4
mindist_manager.recheck critical mindists 0x10017770 (collision)
pcount(7); state = 5
```

The time manager calls this once per PSI after setting `env.current_time` to the PSI time (section 11).

### 6.2 `0x100124c0` iterate the active sim units

For each sim unit in the list at `manager+0x18` (linked through `+8`), call `0x100121b0(su, &ev, cores_out)` with
`ev = { env.delta_PSI_time, env.inv_delta_PSI_time, env, NULL }`.

**Order quirk.** The loop is written with a 3-deep look-ahead. Units are processed in list order **except
the last three, which are processed in reverse**. For a list `A B C D E` the order is `A B E D C`; for `A B` it
is `B A`; for `A B C` it is `C B A`. Sim units are independent of each other, so the order affects only the
shared sleep countdown (10.1) and the order of `cores_out`.

### 6.3 `0x100121b0` `sim_unit.do_sim_unit_PSI(ev, cores_out)` (controllers and sleep, per unit)

```
ev.sim_unit = su
t = env.current_time
fast = false
for each core c of su, index n-1 down to 0:
    // core state sync: the pose at this PSI
    dt = t - c.time_of_last_psi
    c.m_world_f_core_last_psi.R  = matrix(c.q_next)
    c.m_world_f_core_last_psi.vv = c.pos_world_f_core_last_psi + (f64)c.delta_world_f_core_psis · dt    // 0x10012470
    c.commit_all_async_pushs()          // pushes made between PSIs (impulses, BB pushes) land here
    byte c+0x61 = 0 ; u16 c+0x64 = 0
    if (f32)(1.0 - |c.speed|²) < 0: fast = true        // sign-bit test: |speed| > 1 m/s
if fast:
    su.flags bits 10-11 = 1
    test_sleep = false
else:
    su.flags bits 12-13 = su.flags bits 10-11            // previous-PSI fast flag
    if su.flags bits 12-13 != 0: for each core (n-1..0): c.reset_freeze_check_values()   0x100120b0 -> 0x1000cec0 (10.3)
    su.flags bits 10-11 = 0
    test_sleep = env.sleep_countdown()                    // 0x10013610 (10.1)
for each controller entry e of su, index n-1 down to 0:     // descending priority (6.4)
    e.c->do_simulation_controller(&ev, &e.cores)          // vtable +0x10
for each core c, n-1..0: cores_out.add(c)
if test_sleep: su.try_to_freeze(env)                       // 0x100120f0 (10.2)
for each core c, n-1..0: for each object o of c, n-1..0:
    o.update_synapses_after_controllers()                  // 0x100099a0 (collision: mindist recalculation)
if su.flags & 0x300: su.split_or_merge()                   // 0x100120d0
```

So the order within a PSI is: commit pushes from outside the PSI → controllers by priority (forces, springs,
gravity+damping+commit, constraints, friction) → integration of every core of every unit.

Notes:

- The "fast" test uses only the linear `speed`, after the commit and **before** the controllers.
- The sleep test runs after the controllers and before integration. A unit frozen here has already put its
  cores into `cores_out`, so they still go through 8.2 this PSI, with all velocities and
  `delta_world_f_core_psis` zeroed by the freeze. Hence `pos_world_f_core_last_psi` is not advanced by the
  last PSI's velocity (a sub-millimetre jump between the extrapolated `m_world_f_core_last_psi.vv` and the
  frozen pose; keep it).
- A frozen unit is not in the active list: none of its controllers run, so **SetPhysicsForce does nothing to a
  sleeping ball** until something wakes it (Ball Navigation calls Physics WakeUp for that reason).

### 6.4 Controllers, attachment, priorities

`core.add_core_controller(c)` (`0x10011c70`): append `c` to `controllers_of_core`, then in the core's sim unit
(`0x10011cc0`) find the entry for `c` (`0x10011370`) or append a new one (`0x10011410`), add the core to the
entry (`0x100113b0`), and re-sort (`0x10011db0`).

The sort is an insertion sort to **ascending** priority (`vtable+0x14`), swapping only when strictly greater, so
equal priorities keep insertion order. `0x100121b0` walks the entries from the end, so controllers run in
**descending priority, and among equal priorities the most recently added first**.

`0x10011c00` removes a controller from a core (and the core from the entry; an empty entry is deleted,
`0x10011d00`).

| Priority | Controller | Notes |
|---|---|---|
| 2000 | friction-system controller (vtable fn `0x1001d610`) | friction spec |
| 1600 | buoyancy (`0x1000fe50`) | unused |
| 1500 | glue force controller (`0x100046b0`); spring/actuators (`0x100146d0`, others) | 7.2, 12 |
| 1000 | gravity controller (`0x10012010`) | 7.1 |
| 600 | friction-related controller (`0x1001d750`) | friction spec |
| 405 | constraint solver (`0x10028960`) | 12 |

Consequence for one PSI of a free body: force pushes accumulate in `*_change`; the gravity controller then
damps the velocities from the previous PSI, commits the pushes (undamped) and adds `g·dt`; constraints then
correct the velocities; integration moves the body.

## 7. Controllers in the dynamics

### 7.1 `0x10012010` gravity controller `do_simulation_controller(ev, cores)`

```
for each core c in cores, index n-1 down to 0:
    c.damp_object(ev.delta_time)      // 0x1000c610 (7.3)
    c.commit_all_async_pushs()        // 0x1000cbd0
    c.speed += gravity · ev.delta_time    // f32 gravity × f64 dt, stored f32
```

### 7.2 Glue force controller (vtable `0x10063240`, 0x40 bytes, priority 1500)

| Offset | Type | Meaning |
|---|---|---|
| `+0x04` | mgr* | physics manager (env = mgr `+0xc0`) |
| `+0x08` | IVP_Core* | the target's core |
| `+0x10` | f64[3] | application point in **core** coordinates |
| `+0x30` | f32[3] | force (impulse per PSI) in world coordinates |

Vtable: `+0` core-deleted event (removes itself, `0x10011b20`); `+4` minimum frequency = 1.0; `+0x10`
`do_simulation_controller` `0x100046b0`; `+0x14` priority 1500; `+0x18` dtor (detaches from the core).

```
do_simulation_controller(ev, cores):          // 0x100046b0
    if cores.n < 1: return
    M = core.get_m_world_f_core_PSI(env.current_time)     // 4.1; at a PSI this is m_world_f_core_last_psi
    f_cs = (f32)(M.Rᵀ · (f64)force)
    p_cs = (f32)point
    core.async_push_core(p_cs, f_cs, force)              // 5
```

So every PSI: `speed_change += F/m`, `rot_speed_change += I⁻¹ ⊙ (p × Rᵀ F)` with no `dt`. They are committed
in the same PSI by the gravity controller, after damping.

Creation (SetPhysicsForce `Execute` `0x10004930`, after the registry lookup):

```
ctrl = new (0x40); vtable = 0x10063240; ctrl.core = obj.core
dir = DirRef ? DirRef->TransformVector(Direction) : Direction           // CK3dEntity vtable +0x184 (rotation only)
d = (f64)dir
if |d|² > 1e-4 (0x3f1a36e2eb1c432d): normalize d (0x1000e120) else d = (1, 0, 0)
ctrl.force = (f32)(d · ForceValue)
if PosRef == target:
    ctrl.point = (f64)Position          // object-local coordinates used AS core coordinates (no Shift correction)
else:
    pw = PosRef ? PosRef->Transform(Position) : Position                 // vtable +0x17c
    M = core.get_m_world_f_core_PSI(env.current_time)
    ctrl.point = M.Rᵀ · ((f64)pw − M.vv)                                  // 0x1000f3e0
core.add_core_controller(ctrl)   (0x10011b10)
store ctrl in the BB's local 0
```

Quirk: when the position referential is the target itself, the local position is not shifted by the mass
centre. For bodies with a Shift (Ballance never combines the two) the lever arm is off by Shift.

### 7.3 Damping: `0x1000c610` `damp_object(dt)` and `0x1000c6a0` `apply_damping(dt, rot_factor*, lin_factor)`

```
damp_object(dt):
    if (signed char)movement_state >= 2:        // calm
        rf[i] = (f32)(rot_speed_damp_factor[i] + 0.1f)    // 0x3dcccccd
        lf    = speed_damp_factor + 0.1f
        apply_damping(dt, rf, lf)
    else:
        apply_damping(dt, rot_speed_damp_factor, speed_damp_factor)

apply_damping(dt, rf, lf):                      // dt f64, rf f32[3], lf f64
    a = (rf.x·dt, rf.y·dt, rf.z·dt)            // a.y, a.z rounded to f32 on the way
    if a.x² + a.y² + a.z² < 0.5:               // f64 0x3fe0000000000000
        k[i] = (f32)(1.0f − a[i])
    else:
        k[i] = (f32)exp(−a[i])                  // x87 f2xm1/fscale
    b = dt · lf
    if b < 0.25 (f64):  kl = 1.0 − b
    else:               kl = exp(−b)
    rot_speed[i] *= k[i]
    speed[i] *= kl
```

With Ballance's damping (≤ 6/s) and dt = 1/66 the linear branch (1 − x) is always taken. Note the rotational
choice is made **once for all three axes** from the vector length.

## 8. Integration

### 8.1 `0x1001ea50` integrate all cores of the PSI

```
ev.delta_time = env.delta_PSI_time
ev.i_delta_time = ev.delta_time > 1e-10 ? 1/ev.delta_time : 9999999866.485682
ev.env = env
for i = n-1 down to 0: calc_next_PSI_matrix(cores_this_psi[i], &ev, hulls)   // 0x1001e300
```

`cores_this_psi` is the concatenation produced by 6.3 (each unit's cores in reverse index order), so
integration runs over it in reverse.

### 8.2 `0x1001e300` `core.calc_next_PSI_matrix(ev, hulls)` — clamps, rotation, position

```
lim = env.anomaly_limits (+0x24)
rs = &rot_speed
// --- angular clamp
if max_rot_speed (+0x4c) != NULL:          // per-axis box clamp; never set in Ballance
    x: if rs.x > m.x: rs.x = m.x  elif rs.x < -m.x: rs.x = -m.x
    y: if rs.y > m.y: rs.y = m.y  elif rs.y < -m.y: rs.y = +m.y      // original bug: sets +m.y
    z: if rs.z > m.z: rs.z = m.z  elif rs.z < -m.z: rs.z = -m.z
else:
    a = env.inv_delta_PSI_time · lim.max_angular_velocity_per_psi    // 66 · π/2 ≈ 103.67 rad/s
    if |rs|² > a²: anomaly_manager->max_angular_velocity_exceeded(lim, core, rs)   // vtable +4 (9)
// --- linear clamp
if |speed|² > lim.max_velocity²: anomaly_manager->max_velocity_exceeded(lim, core, speed)   // vtable +0
current_speed (+0x1b8) = (f32)|speed|
dt = ev.delta_time
i_delta_time = (f32)ev.i_delta_time
// --- rotation of this PSI, in core axes
if (flags bits 6-7) == 0 and env.state != 5:          // inertias not equal: gyroscopic sub-steps
    n = 1
    x = |rs|²·dt·dt
    if x > 1/36 (0x3f9c71c71c71c71c):
        n = trunc(sqrt(x·144.0)) + 1                    // _ftol truncates; 144 = 0x4062000000000000
        dt = dt / n
    Q = quat_poly(rs, dt)                              // 0x10018f80
    e = ((I.y − I.z)·invI.x, (I.z − I.x)·invI.y, (I.x − I.y)·invI.z)    // f64
    i = 1
    loop:
        g = ((f32)(rs.z·rs.y·e.x), (f32)(rs.z·rs.x·e.y), (f32)(rs.x·rs.y·e.z))   // Euler: ω̇ = (I_j − I_k)/I_i ω_j ω_k
        rs = rs + g·(f32)dt                             // 0x10010290, f32
        if i >= n: break
        Q = quat_poly(rs, dt) ⊗ Q                        // new sub-step on the LEFT
        i++
else:                                                   // balls, equal inertia
    Q = quat_sin(rs, dt)                               // 0x10019010
// --- advance the PSI
dtp = env.current_time − time_of_last_psi
time_of_last_psi = env.current_time
pos_world_f_core_last_psi += (f64)delta_world_f_core_psis · dtp
delta_world_f_core_psis = speed
q_last = q_next
q_next = q_next ⊗ Q ; normalize iteratively (0x10019540)
calc_rotation_info(Q)                                  // 0x1001e870 (8.3)
// --- hull manager update (collision interface)
for each object o of the core, n-1..0:
    o.hull.update(env.current_time, (f32)ev.delta_time, max_surface_rot_speed + current_speed, current_speed)   // 0x1001e750
    if o.hull.exceeded(): hulls.add(&o.hull)            // 0x1001e730, 0x1001ecd0
```

Notes:

- The scheme is semi-implicit Euler: the PSI's velocities (after controllers) become
  `delta_world_f_core_psis` and the rotation `q_next`, which are the motion **from this PSI to the next**. The
  position stored at this PSI is the extrapolation with the previous velocity, the same value 6.3 put in
  `m_world_f_core_last_psi.vv`.
- In the gyroscopic loop the angular velocity is updated with an explicit Euler step of Euler's equations
  without torques; the original `dt` is used in the 1/36 test, the divided `dt` afterwards. The sub-step
  quaternion uses `rs` **before** its update for the first step and after each update for later steps.
- `quat_poly` is a third-order sine approximation of the half angle **per component** (not of the axis-angle
  vector); for small `|ω|dt` it equals `quat_sin` to O((ω dt)^5).
- Ballance: balls take the sine path; boxes and PH modules usually the gyroscopic path.

### 8.3 `0x1001e870` `calc_rotation_info(Q)`

```
n = Q.x² + Q.y² + Q.z²
if n <= 1e-19:
    abs_omega = 0 ; rotation_axis_world = (1, 0, 0)
else:
    y = 1/sqrt(n) (bit-trick guess, 5 Newton steps as 0x1000e120)
    s = y·n                                   // = |Q.xyz| = sin(half angle)
    asin_s = s + s³·(1/6)f + s⁵·0.40413999557495117f (0x3eceeb70)   // asin approximation
    abs_omega = (f32)(2 · asin_s · (f64)i_delta_time)
    rotation_axis_world = (f32)(R · (Q.xyz · y))    // R = m_world_f_core_last_psi.R
max_surface_rot_speed = abs_omega · max_surface_deviation
```

### 8.4 Hull manager interface (`obj+0x48`), collision side

| Offset | Type | Meaning |
|---|---|---|
| `+0x00` | f64 | time of the last update |
| `+0x08` | f32 | gradient (surface speed bound, × 1.00001) |
| `+0x0c` | f32 | centre gradient |
| `+0x10` | f32 | hull value at the last update |
| `+0x14` | f32 | centre hull value |
| `+0x18` | f32 | hull value at the next PSI |
| `+0x28` | f32 | the smallest hull value at which a synapse must be re-checked (maintained by the collision code) |

```
update(t, dt, g, gc):                 // 0x1001e750
    d = t − hull.time ; hull.time = t
    hull.value += d·hull.gradient
    hull.center_value += d·hull.center_gradient ; hull.center_gradient = gc
    hull.gradient = g · 1.00001f       // 0x10063868
    hull.value_next_psi = hull.gradient·dt + hull.value
exceeded(): return hull.limit(+0x28) − hull.value_next_psi < 0.0f
```

`0x1001eb10` then, for every exceeded hull (vector walked from the end), fires the hull listeners while
`limit − value_next_psi < 0` (at most 101 calls) and updates a time code; this is mindist event scheduling,
out of scope. A per-PSI discrete collider can ignore the hull manager entirely but should keep
`current_speed`, `abs_omega` and `max_surface_rot_speed`, which the friction and impact code read.

## 9. Anomaly limits and manager

Limits (`0x1002f5e0`, 0x14 bytes, env `+0x24`):

| Offset | Type | Value | Meaning |
|---|---|---|---|
| `+0x08` | f32 | 2000.0 (`0x44fa0000`) | `max_velocity` (m/s) |
| `+0x0c` | i32 | 70000 | `max_collisions_per_psi` (an integer count, read by the collision code) |
| `+0x10` | f32 | π/2 (`0x3fc90fdb`) | `max_angular_velocity_per_psi` (rad per PSI) |

Default manager (`0x1002f660`, vtable `0x10063a58`):

```
max_velocity_exceeded(lim, core, v):          // 0x1002f6d0
    v *= (lim.max_velocity · 0.99000000953674 (0x3fefae1480000000)) / |v|
max_angular_velocity_exceeded(lim, core, w):  // 0x1002f720
    w *= (env.inv_delta_PSI_time · lim.max_angular_velocity_per_psi · 0.8999999761581 (0x3fecccccc0000000)) / |w|
```

Vtable slot 2 (`0x1002f8f0`) handles inter-penetration of a pair (collision side, not covered).

Ballance never reaches these limits (2000 m/s; 104 rad/s ≈ a radius-2 ball rolling at 207 m/s).

## 10. Simulation units, sleep, freeze and revive

### 10.0 Sim-unit manager (env `+0x08`, 0x1b0 bytes, ctor `0x10011e70`)

| Offset | Type | Meaning |
|---|---|---|
| `+0x00` | env* | |
| `+0x08` | f64 | 9.73 (`0x402375c280000000`), use unknown |
| `+0x10` | f64 | 0.3 (`0x3fd3333340000000`), use unknown |
| `+0x18` | ptr | head of the **active** sim-unit list (state byte < 8) |
| `+0x1a8` | ptr | head of the **frozen** sim-unit list (state byte ≥ 8) |

`add_sim_unit` (`0x10011ef0`) pushes at the head of the list selected by the unit's state byte;
`remove_sim_unit` is `0x10011f50`. Because insertion is push-front, "list order" in 6.2 is newest unit first.

Units merge when a controller spans several cores: `0x10011b30(ctrl_mgr, ctrl)` (constraints, springs) merges
the sim units of all movable cores of the controller into one (`0x10011720`), attaches the controller to each
core (`0x10011c70`), ANDs the cores' state bytes starting from 8, and revives the unit (`0x10011f80`) if the
result is < 8. **Bodies joined by a constraint or spring share one unit and sleep and wake together.** The
glue force controller is attached with `0x10011b10` (no merge, one core). Contacts merge units through the
friction system (`0x10022180`, friction spec).

Sleep-test fields of the core (all set only by 10.2 and 10.3):

| Offset | Type | Meaning |
|---|---|---|
| `+0x1d8` | f64 | `time_calm_ref[0]` (short window start) |
| `+0x1e0` | f64 | `time_calm_ref[1]` (long window start) |
| `+0x1e8` | f32[4] | `q_calm_ref[0]`, compared with `q_next` (`+0x108`) |
| `+0x1f8` | f32[4] | `q_calm_ref[1]`, compared with `q_last` (`+0xe8`) |
| `+0x208` | f32[3] | `pos_calm_ref[0]` (`+0x214` unused) |
| `+0x218` | f32[3] | `pos_calm_ref[1]` |

A new core is zero-filled, so all references start at 0 (position origin, quaternion (0,0,0,0), time 0).

### 10.1 `0x10013610` global sleep countdown and the RNG `0x1002fcd0`

```
rng():                                   // DLL global seed 0x100685b4, initial 1, never reseeded, shared by all worlds
    seed = seed * 75                     // u32 wrap (written as ×3, ×5, ×5)
    return (seed & 0xffff) * 2^-16       // f32 constant 0x37800000 (1.52587890625e-05)

env.sleep_countdown():                   // returns 1 when the calling unit must be sleep-tested
    env.countdown(+0x140, u16) -= 1
    if env.countdown != 0: return 0
    r = rng()
    env.countdown = 15 - trunc(r * -5.0f)      // -5.0f at 0x100635b4; reload 15..19
    return 1
```

The countdown is **per environment**: every slow active unit decrements it once per PSI (6.3), and only the
unit that reaches 0 is tested. The first test happens after 10 decrements. With one ball awake, it is tested
every 15-19 PSIs (≈ 0.25 s of sim time). `docs/physics.md` says 10-15; this supersedes it.

The same LCG may have other callers in the engine (not enumerated here); any call advances the shared seed.
Keep it a process-global in the port.

### 10.2 `0x100120f0` `sim_unit.try_to_freeze(env)` and `0x1000d680` `core.calc_movement_state(t)`

```
try_to_freeze(su, env):
    t = env.current_time ; acc = 3
    for each core c, n-1..0:
        s = c.calc_movement_state(t)
        c.movement_state = s              // kept even if the unit stays awake (damping reads it)
        acc &= s
    if acc != 3: return 0
    for each core c, n-1..0: freeze_core(c)     // 0x1000afe0 (10.3)
    manager.remove(su); su.state byte = 8; manager.add(su)   // -> frozen list
    return 1
```

`calc_movement_state(c, t)`; `R` = `upper_limit_radius` (`+0x04`); positions are `pos_world_f_core_last_psi`
(f64) minus the f32 references; all comparisons as written:

```
d0 = pos - pos_calm_ref[0]
if |d0|² <= 9.999999552965169e-05 (0x3f1a36e2d7731900):
    dot = q_calm_ref[0] · q_next                // 4D
    if (1.0 - dot²)·2·R·R <= 2.4999998882412923e-05 (0x3efa36e2d7731900):
        return (t - time_calm_ref[0] > (f64)env.freeze_check_time /*0.3f*/) ? 3 : 2
// short window moved: restart it
q_calm_ref[0] = (f32)q_next ; pos_calm_ref[0] = (f32)pos ; time_calm_ref[0] = t
d1 = pos - pos_calm_ref[1]
if |d1|² > 0.010000000298023226 (0x3f847ae151eb8520)
   or (1.0 - (q_calm_ref[1] · q_last)²)·2·R·R > 0.040000001192092904 (0x3fa47ae151eb8520):
    q_calm_ref[1] = (f32)q_last ; pos_calm_ref[1] = (f32)pos ; time_calm_ref[1] = t
    return 1
return (t - time_calm_ref[1] > 4.0) ? 3 : 1
```

Notes:

- The rotation measure `(1 − cos²(θ/2))·2R² = 2R² sin²(θ/2)` is about half the squared surface displacement,
  so for a radius-2 ball the short-window limit allows ≈ 0.0025 rad of rotation.
- The short window uses `q_next`, the long window `q_last` (keep the asymmetry; at test time `q_next` is the
  rotation at this PSI and `q_last` the one at the previous PSI).
- A ball rolling at 0.01 m/s moves 0.0025 m per test interval and stays awake; resting balls freeze ≈ 0.3 s
  after they stop, slow creep freezes after 4 s.

### 10.3 Freeze: `0x1000afe0` = `0x1000ce20` + `0x1000af90`

```
freeze_core(c):
    stop_physical_movement(c):                  // 0x1000cd80
        c.movement_state = 8
        speed = rot_speed = speed_change = rot_speed_change = delta_world_f_core_psis = 0
        c.init_core_for_simulation({env.delta_PSI_time, env.inv_delta_PSI_time})    // 2.8
    tnow = env.current_time
    for each object o of c, n-1..0:
        o.state byte = 8
        mindist_manager.remove_object(o)        // 0x10017140 (collision)
        dt = tnow - o.hull.time
        o.hull.value += dt·o.hull.gradient ; o.hull.gradient = 0
        o.hull.center_value += dt·o.hull.center_gradient ; o.hull.center_gradient = 0
        hull reset 0x1001a820(o.hull)
        if o.cache_object: invalidate (0x10018910)
    fire listeners (0x1000af90), objects n-1..0:
        per-object listeners (env+0xc, hash on o): vtable +0xc (frozen)({env, o})
        global listeners (env+0x150, n-1..0):      vtable +0xc (frozen)({env, o})
```

### 10.4 Wake-up and revive

```
obj.wake()  (0x1000a460; Physics WakeUp, Physicalize when not Start Frozen, Physics Impulse, world pushes):
    if obj.state byte != 8: c.reset_freeze_check_values()   // 0x1000cec0, obj.core
    else:                   env.enqueue_revive(obj+0xa8 core)  // 0x1000b540

reset_freeze_check_values(c):  time_calm_ref[0] = time_calm_ref[1] = env.current_time   // references kept

enqueue_revive(env, c):
    if (c.flags & 0x30) != 0x10:  env.revive_list.add(c); c.flags = (c.flags & ~0x20) | 0x10

revive_cores_PSI(env)  (0x1000b590, at the start of the next PSI, 6.1):
    for i = n-1..0: c = list[i]; ensure_in_simulation(c) (0x1000dab0); c.flags &= ~0x30
    clear the list

ensure_in_simulation(c):
    if c.flags & 0xc: return                                // unmovable
    if c.movement_state != 8: reset_freeze_check_values for every core of c's unit (0x100120b0)
    else: revive_sim_unit(c.sim_unit)                       // 0x10011f80

revive_sim_unit(su):
  restart:
    for each core c, n-1..0:
        if (signed char)c.movement_state < 8: continue
        if revive_core(c) == 1: goto restart                // the unit was merged/changed
    if su.state byte == 8: manager.remove(su); su.state byte = 1; manager.add(su)   // -> active list

revive_core(c)  (0x1000aea0) -> int:
    for each object o: o.state byte = 1
    0x1000cf20: c.movement_state = 1; reset_freeze_check_values(c); c.time_of_last_psi = env.current_time
                for each object o: o.state byte = 1; re-register with the mindist manager (0x100099f0)
    dt = env.time_of_next_psi - env.current_time
    i_dt = dt <= 1e-10 ? 9999999866.485682 : 1/dt
    c.init_core_for_simulation({dt, i_dt})                  // speed, delta zeroed; q_next = q_last
    r = 0x1001d4d0(c)        // friction system: pull resting partners into this unit (friction spec)
    for each object o: per-object and global listeners vtable +8 (revived)({env, o})
    return r
```

Consequences:

- Waking an awake body only restarts its 0.3 s and 4 s timers (it does not reset the references, so a body
  that is still calm freezes as soon as the timer elapses again).
- Waking a frozen body takes effect at the **start of the next PSI**, before the controllers. Pushes made in
  between (`async_push_*`) are not cleared by the revive and are committed in that PSI (6.3).
- Revive zeroes `speed` but **not `rot_speed`** (freeze already zeroed it).
- `0x10009670` is an immediate revive (if `obj.state == 8`: `revive_sim_unit`), used by collision code.
- Attaching/detaching controllers also wakes: `0x10011ad0(ctrl)` revives (or resets the timers of) the unit of
  the controller's first core; `0x10011a60(ctrl, keep)` marks the unit `0x100` (changed) on detach.

### 10.5 Glue object listener (vtable `0x10063350`, `this+4` = manager)

The event is `{env, obj}`.

- `+0x08` revived (`0x10008030`): append obj to the write-back vector `mgr+0x28` (no duplicate check).
- `+0x0c` frozen (`0x10007fd0`): search the vector from the end and remove the entry. Bug: if not found it
  still removes element 0 (unreachable: only revived objects freeze).
- `+0x00` deleted (`0x10007f20`): if the core is movable, remove from the vector (safe search); then destroy the
  registry record's continuous-contact handler, if any.

Lifecycle: Physicalize creates the object frozen (state 8, unit in the frozen list); unless Start Frozen the
glue wakes it (queue); at the next PSI it is revived and added to the write-back vector; when it freezes it is
removed. Fixed bodies are never revived, never integrated and never written back.

## 11. Time manager and PSI driving

### 11.1 Time manager (0x20 bytes, ctor `0x1002f0b0`, env `+0x04`)

| Offset | Type | Meaning |
|---|---|---|
| `+0x04` | ptr | strategy (vtable `0x100635b8`): slot 0 `0x1002ef70` simulate-until, slot 1 `0x1002f010` variable step (unused). Its `+4` stop flag is never set |
| `+0x08` | ptr | event queue |
| `+0x0c` | ptr | the PSI event (vtable `0x10063a2c`; `+4` u16 queue index, `0xffff` = not queued) |
| `+0x10` | f64 | relative time of the event being processed |
| `+0x18` | f64 | `base_time`; queued times are **f32 offsets** from it |

The ctor queues the PSI event at time 0.0, so **the first PSI runs at t = 0** on the first `simulate` with
dt > 0.

Event queue (`0x100300f0`): a sorted linked list of 16-byte nodes `{u16 skip_next, u16 skip_prev, u16 next,
u16 prev, f32 value, event*}` with a skip list; header `{u16 cap, u16 free_head, nodes*, f32 min_value,
u16 skip_head, u16 head, u16 count}`. Empty: `min_value` = 1e10f (`0x501502f9`). Insert (`0x10030180`) puts a new
event before the first node with `v <= node.value` (ties: newest first). Remove `0x10030470`. A port can use a
binary heap on `(f32 value, newest first)`. `0x1002f1d0(tm, ev, t)` inserts at `(f32)(t − tm.base)`.

### 11.2 `env.simulate(dt)`: `0x100138c0` → `0x1002f250` → `0x1002ef70`

```
simulate(env, dt):                         // 0x100138c0
    target = env.current_time + dt
    run(tm, env, target)                    // 0x1002f250

run(tm, env, target):
    save the x87 control word; CW |= 0x300 (64-bit mantissa), rounding unchanged
    perf.start(); tm.strategy.simulate_until(tm, env, target); perf.end()
    restore the control word

simulate_until(tm, env, target):           // 0x1002ef70
    loop:
        m = queue.min_value                         // f32
        if !((target - tm.base) > m): break          // strict: an event exactly at target waits
        ev = queue.head.event ; queue.remove(ev) ; ev.index = 0xffff
        tm.cur_rel = m
        set_current_time(env, m + tm.base)           // 0x100138f0: env.+0x138 += 1; env.current_time = t
        ev->simulate_time_event(env)                 // vtable +0
    set_current_time(env, target)
```

There is no cap on PSIs per call. Mindist events share the queue but are only queued before
`time_of_next_psi`, so none crosses a PSI.

### 11.3 The PSI event `0x1002f2c0`

```
psi_event(ev, env):
    T = env.current_time
    env.time_of_last_psi(+0x130) = T
    env.time_of_next_psi(+0x128) = env.delta_PSI_time + T
    for each queued node: node.value = (f32)(node.value - T) ; queue.min_value = (f32)(min_value - T)
    tm.base = T ; tm.cur_rel = 0
    env.simulate_psi()                               // 0x10013cb0 (6.1)
    insert(ev, (f32)(env.time_of_next_psi - T))      // re-queue
```

- The rebase subtracts the absolute `T` (not `T − old_base`); harmless because the queue is empty apart from
  this event at that moment. A port keeps event times relative to the last PSI.
- **The effective PSI period is `(f32)(1/66)` = 0.015151515603065491**, while integration uses the f64
  `delta_PSI_time`. PSI times are `T₀ = 0`, `T_{k+1} = T_k + (f64)(f32)(1/66)`.

So for a frame with target time `X`: run every PSI with `T_k < X` (strictly; `T_k` relative to the base as f32),
then set `current_time = X`. The write-back pose (4.2) is extrapolated from the last PSI to `X`.

## 12. Spring actuator and constraint solver

Covered: everything Ballance reaches. Outlined only (unreachable from the content): the constraint branch with
exactly one locked rotation axis, the `#rotFixed == 1` acos scaling, the max-force/break block, the
active-float spring variant (`0x100144d0`) and the spring break listeners.

Extra matrix helpers used here: `0x1000f690` R·v (f32); `0x1000ede0` M⁻¹·P (M orthonormal); `0x1000f0e0`
M·P⁻¹ (P orthonormal); `0x1000f140` rotation M·Pᵀ; `0x1000ef10` rotation M·P; `0x1000e8f0(M, eps=1e-19)` general
in-place inverse by cofactors (returns 0 if |det| < eps); `0x1000eb00` identity incl. vv; `0x1000eb30` identity
rotation.

Orthonormal basis `0x1000eb60(M, axis, k)`: `a = normalize(axis)`; `v = normalize(a.y, a.z − a.x, −a.y)`, or if
that fails `normalize(a.z, −a.z, a.y − a.x)`; columns `col[k] = a`, `col[(k+1)%3] = v × a`, `col[(k+2)%3] = v`.

`0x1000c0b0(c1, c0, p1_cs, p0_cs, out)` (cdecl): `out = v_surf(c1, p1) − v_surf(c0, p0)` (5, `0x1000c810`
formula); a NULL or unmovable core contributes 0.

Controller vtable layout (both): slot 0 core deleted (deletes self), 1 minimum frequency = 1.0, 2 controlled
cores, 3 no-op, **4 `do_simulation_controller(ev, cores)`**, 5 priority, 6 deleting dtor.

### 12.1 Anchors (`0x10013e80` template, `0x10013ea0` init)

Template anchor (0x28): `+0` object, `+8` f64[3] world position.

Anchor (0x30):

| Offset | Meaning |
|---|---|
| `+0x00/+0x04` | next / prev in the object's anchor list (`obj+0x88`, `0x100095b0`) |
| `+0x08` | object |
| `+0x0c` | f32[3] position, object space |
| `+0x1c` | f32[3] position, core space |
| `+0x2c` | owning actuator |

```
anchor_init(a, actuator, ta):
    o = ta.object ; a.object = o ; a.actuator = actuator
    M_cf = 0x10009be0(o)    // m_core_f_object: rotation q_core_f_object (identity if NULL), vv = shift_core_f_object
    refresh o's pose cache (4.4) if simulated and stale
    a.p_object = (f32)(inverse(cache.m_world_f_object) · ta.pos_ws)      // 0x10018ca0
    a.p_core = (f32)(M_cf · a.p_object)
    link a into o's anchor list
```

### 12.2 Set Physics Spring (`Execute` `0x10006490`) and the spring template

1. No target: done (`0xa004`). Object2 NULL or either body not physicalized: return 0 (retried each frame).
2. Pins: Object2 (0), pos1 (1), ref1 (2), pos2 (3), ref2 (4), **Length** (5, 1.0), **Constant** (6, 1.0),
   **Linear Dampening** (7, 0.1), **Global Dampening** (8, 0.1).
3. Template `0x10014200`: 14 dwords zeroed, `[9] = 1e20f` (`0x60ad78ec`).
4. `w1 = ref1 ? ref1->Transform(pos1) : pos1`; anchor A1 on the **target**. `w2` likewise; anchor A2 on Object2.
5. `tmpl[1] = &A1, [2] = &A2, [3] = Length, [4] = 0, [5] = Constant, [6] = LinDamp, [7] = GlobalDamp`.
6. `h = 0x10013700(env, tmpl)`; store in local 0.

Spring template (dwords): `[0]` copied to actuator `+0x6c` (0); `[1],[2]` anchors; `[3]` rest length; `[4]` values
relative (scale by reduced mass; 0 here); `[5]` k; `[6]` damping along the spring; `[7]` damping of the relative
velocity; `[8]` break enable; `[9]` break length; `[10..13]` active floats (NULL → plain spring).

Create `0x10013700` → 0x98 bytes, ctor `0x10014250` (base `0x10014020`, `0x10013f60`):

| Offset | Meaning |
|---|---|
| `+0x00` | vtable `0x10063604`, priority 1500 |
| `+0x04/06/08` | controlled cores (u16 cap, u16 n, ptr): anchor cores that are movable (second skipped if equal) |
| `+0x0c` | anchor 0 (target, pos1@ref1) |
| `+0x3c` | anchor 1 (Object2, pos2@ref2) |
| `+0x70` | env |
| `+0x74` | L = rest length |
| `+0x78` | f = 1.0 (or `m0·m1/(m0+m1)` with `0x1000c390` effective masses if `tmpl[4]`) |
| `+0x7c` | k = `tmpl[5]·f` |
| `+0x80` | c_lin = `tmpl[6]·f` |
| `+0x84` | c_rel = `tmpl[7]·f` |
| `+0x88` | break length; `+0x8c` break flag |
| `+0x90..` | listener vector |

Registration: `0x10011b30` with the controller manager of anchor 0's core (10.0: merges the units).

### 12.3 Spring `do_simulation_controller` `0x100146d0`

```
c0 = core(anchor0) ; c1 = core(anchor1)
p0 = c0.m_world_f_core_last_psi · anchor0.p_core      // f64 (0x1000f5f0)
p1 = c1.m_world_f_core_last_psi · anchor1.p_core
d = (f32)(p0 - p1) ; len = normalize(d)              // 0x1000df30: 5 Newton rsqrt steps; returns 0 if |d|² < 1e-19
if len < 1e-10f (0x2edbe6ff): return
if len > break_length and break_flag == 1: fire listeners, delete self, return     // never in Ballance
F = (len - L)·k                                      // f64
u = v_surf(c1, anchor1.p_core) - v_surf(c0, anchor0.p_core)    // 0x1000c0b0, world f32
s = (F - c_lin·(d·u)) · ev.delta_time
I = d·s - (c_rel·ev.delta_time)·u                     // f32[3]
if (signed char)c1.movement_state < 8: c1.async_push_core_ws(p1, +I)    // 0x1000c940
if (signed char)c0.movement_state < 8: c0.async_push_core_ws(p0, -I)    // × -1.0 (0xbff0000000000000)
```

**Mapping (resolves `docs/physics.md` 2.4 / 7):** Length = rest length; Constant = k (force = k·stretch);
Linear Dampening damps the relative velocity **along** the spring (−c_lin·(d·Δv)); Global Dampening damps the
**full** relative anchor velocity Δv = v(Object2) − v(target) in all directions (−c_rel·Δv). Forces become
impulses × dt per PSI, applied as async pushes (+ on Object2, − on the target), committed by the gravity
controller of each core in the same PSI (springs run first at 1500). It pushes and pulls (no slack mode).

### 12.4 Constraint template (`0x10012570` init, ≥ 0x200 bytes)

| Offset | Type | Meaning (init value) |
|---|---|---|
| `+0x004` | obj* | objR, reference = the BB target (0) |
| `+0x008` | Matrix | `m_Rfs_f_Rcs`: constraint frame in R's object space (identity) |
| `+0x088` | Matrix* | → `+8` or NULL (0) |
| `+0x0f0` | ptr | optional extra rotation (0) |
| `+0x0f4` | obj* | objA = Object2 (0) |
| `+0x0f8` | Matrix | `m_Afs_f_Acs` (identity) |
| `+0x178` | Matrix* | → `+0xf8` or NULL (0): when NULL, A's frame is derived from R's at creation |
| `+0x17c` | f32 | force_factor (1.0) |
| `+0x180` | f32 | damp_factor (1.0) |
| `+0x184` | f32 | limit_factor (0.3, `0x3e99999a`) |
| `+0x188..+0x19c` | i32[6] | axis types trans x,y,z, rot x,y,z: 0 free, 1 fixed, 2 limited (init 1,1,1,0,0,0) |
| `+0x1a0..+0x1b4` | f32[6] | lower borders |
| `+0x1b8..+0x1cc` | f32[6] | upper borders |
| `+0x1d0..+0x1e4` | f32[6] | max force/torque (0 = none) |
| `+0x1e8..+0x1fc` | i32[6] | over-max mode: 1 scale, 2 break |

`0x10012870(tmpl, objR, anchor_ws*, axis_ws*, nTrans, nRot, objA, mA*)` (`set_constraint_ws`):

```
M = objR ? objR.get_m_world_f_object_AT(env.current_time) : identity      // 0x10009c40
anchor_Rfs = anchor ? M⁻¹·anchor : 0 ; axis_Rfs = axis ? Mᵀ_R·axis : 0     // f64
0x10012690(tmpl, objR, &anchor_Rfs, &axis_Rfs, nTrans, nRot, objA, mA):
    if anchor and axis pointers are both NULL: tmpl+0x88 = NULL
    else:
        tmpl+0x88 = &tmpl+8
        R(+8) = no axis ? identity
              : (nRot == 2 or nTrans == 2) ? basis(axis, 2)
              : (nRot == 1 or nTrans == 1) ? basis(axis, 0) : identity
        vv(+8) = anchor_Rfs
    tmpl+0xf0 = 0
    if mA: tmpl+0x178 = &tmpl+0xf8 ; +0xf8 = mA·(m_Afs_f_Rfs·m_Rfs_f_Rcs) else tmpl+0x178 = NULL   // glue: NULL
    tmpl+4 = objR ; tmpl+0xf4 = objA
    trans types = (nTrans > 0, nTrans > 1, nTrans > 2) ; rot types = (nRot > 0, nRot > 1, nRot > 2)
```

The constraint axis is always constraint-space **z** (index 2): the free (or limited) axis. Limits:
`0x10012960(tmpl, i, lo, hi)` trans type[i] = 2 with borders; `0x10012990` the same for rotation axis i.
Create `0x100129c0(env, tmpl)`: NULL if both objects are NULL, else 0x190 bytes, ctor `0x10028270`.

Glue:

| BB | Execute | anchor | axis | nTrans | nRot | Limits |
|---|---|---|---|---|---|---|
| Hinge | `0x100059d0` | referential world position (vtable `+0x128`) | referential world Z (Dir, vtable `+0x130`); (0,0,1) without referential | 3 | 2 | if set: rot axis 2, lo·π/180, hi·π/180 (`0x3f91df46a2529d39`); never set by the game |
| Slider | `0x10005f10` | world position of *Axis first Point* | pos(second) − pos(first), unnormalized | 2 | 3 | if set: trans axis 2, lo, hi (m) |
| Ball joint | `0x100052f0` | pos1 by ref1 (`+0x17c`) or raw | NULL | 3 | 0 | |

objR = target, objA = Object2. Hinge/Slider: NULL target or Object2 → done, nothing created; unphysicalized
body → retried. The slider calls `0x10012870` twice, (3,2) then (2,3); only the second counts.

**Slider limit sign.** The error is `p_A − p_R` along the axis (12.6). With R the moving target and A the
fixed anchor body, a displacement s of the target along the axis gives err = −s, so PE_Box_slide's −30000..0
allows s ∈ [0, 30000]: it can only move toward the second point.

### 12.5 Constraint object (ctor `0x10028270`, init `0x100284d0`, 0x190 bytes, vtable `0x10063930`, priority 405)

| Offset | Type | Meaning |
|---|---|---|
| `+0x04` | u32 | flags (bit 0 set when registered, `0x10037620` → `0x10011b30`) |
| `+0x08/0a/0c` | u16, u16, ptr | controlled cores (inline 2 at `+0x10`) |
| `+0x18` | f32 | force_factor |
| `+0x1c` | f32 | damp_factor / force_factor |
| `+0x20..+0x34` | u32[6] | axis types; bit 0 = active this PSI, bit 1 = limited (2 inactive limit, 3 active limit) |
| `+0x38..+0x4c` | f32[6] | lower borders |
| `+0x50..+0x64` | f32[6] | upper borders |
| `+0x68` | f32 | limit_factor (0.3) |
| `+0x6c` | f32* | max-force block (NULL in Ballance) |
| `+0x70` | Matrix | `m_Rcs_f_Rcore` |
| `+0xf0` | obj* | objR |
| `+0xf4` | ptr | optional rotation (NULL) |
| `+0xf8` | Matrix | `m_Acs_f_Acore` |
| `+0x178` | obj* | objA |
| `+0x17c` | ptr | optional rotation (NULL) |
| `+0x180..+0x182` | i8[3] | translation axis order (written with a slot bug, never read) |
| `+0x183..+0x185` | i8[3] | rotation axis order: fixed, then limited, then free |
| `+0x186/+0x187` | u8 | number of fixed trans / rot axes |
| `+0x188/+0x189` | u8 | number of limited trans / rot axes |
| `+0x18a` | u8 | number of active axes this PSI |

Init:

```
push the movable cores of objR, objA
Wl_R = world_f_R (identity if NULL) ; Wl_A = world_f_A
Rfs_f_Afs = Wl_R⁻¹·Wl_A
Rcore_f_Rfs = m_core_f_object(objR) ; Acore_f_Afs = m_core_f_object(objA)      // 0x10009be0, identity if NULL
Rcore_f_Acore = (Rcore_f_Rfs·Rfs_f_Afs)·(Acore_f_Afs)⁻¹
+0x70 = inverse(Rcore_f_Rfs·tmpl.m_Rfs_f_Rcs)  (or inverse(Rcore_f_Rfs) if tmpl+0x88 is NULL)   // 0x1000e8f0
+0xf8 = tmpl+0x178 ? inverse(Acore_f_Afs·tmpl.m_Afs_f_Acs) : +0x70·Rcore_f_Acore
        // glue path: A's constraint frame coincides with R's at creation
copy types, borders, factors ; build order bytes (0x10028310 trans, 0x100283f0 rot)
register (0x10037620 → 0x10011b30: units merged, revived if either body is awake)
```

### 12.6 Constraint solver `0x10028960` (`do_simulation_controller`)

Notation: `dt = (f32)ev.delta_time`, `idt = ev.i_delta_time`; `cR`, `cA` the cores (may be NULL; a fixed core
contributes geometry only); `WR`, `WA` their `m_world_f_core_last_psi` (identity if NULL); rotations are 3×3.

```
CsW = R(+0x70)·R(WR)ᵀ                 // Rcs_f_world
CsA = CsW·R(WA)                       // Rcs_f_Acore
Rr = R(+0x70) ; Ra = R(+0xf8)
CsA_rot = Rr·R(WR)ᵀ·R(WA)              // = CsA here

// angular error e_rot (f64[3]), n = #rotFixed + #rotLimited
n == 3 (slider): Q = CsA_rot·Raᵀ ; q = quat(Q) (0x100191b0)
                 e_i = 2q_i + 0.24f·q_i³ + 0.58f·q_i⁵   (0x3e75c28f, 0x3f147ae1; e.x uses (f32)q.x)
                 if q.w < 0: e = -e
n == 2 (hinge):  b = column order[2] of Q (A's free axis in Rcs); with (o0,o1,o2) = order bytes (0,1,2):
                 e[o0] = -atan2(b[o1], b[o2]) ; e[o1] = atan2(b[o0], b[o2]) ; e[o2] = 0
n == 0 (ball):   e = 0
(n == 1: not used; per-PSI frame around the single axis, not detailed)

// linear part
aR = (f32)(Rᵀ70·(−vv70)) ; aA = (f32)(Rᵀf8·(−vvf8))        // anchors in core coordinates (0x1000f370)
pR = WR·aR ; pA = WA·aA                                    // world f64
vR = cR ? v_surf(cR, aR) : 0 ; vA = cA ? v_surf(cA, aA) : 0
err  = CsW·(pA − pR)                                       // Rcs
relv = CsW·vA − CsW·vR
k = dt·(+0x1c)
for i in 0..2:
    b_i = err_i + k·relv_i
    if type_i & 2:                                         // limited
        if   b_i < lower_i: type_i |= 1 ; b_i = dt·relv_i + (err_i − lower_i)·limit_factor
        elif b_i > upper_i: type_i |= 1 ; b_i = dt·relv_i + (err_i − upper_i)·limit_factor
        else: type_i &= ~1
g = −(+0x18)·(f32)idt ; b_i = (f32)b_i·g                    // desired change of relative velocity
// angular part: the same with relω = CsA_rot·ωA − Rr·ωR (ω = rot_speed, core axes), e_rot, rot borders → c_i
+0x18a = Σ (type & 1) over the 6 axes

// response matrix, for each active axis a (unit e_a in Rcs):
trans a: dR_core = Rᵀ70·e_a ; dR_ws = CsWᵀ·e_a ; dA_core = CsAᵀ·(−e_a) ; dA_ws = CsWᵀ·(−e_a)
         per movable core: (dv, dω) = calc_push_core(anchor, d_core, d_ws) (0x1000ca80);
         Δv_surf = v_surf formula with (dv, dω) (0x1000bf90)
         T[a] = CsW·Δv_surf_A − CsW·Δv_surf_R ; Rm[a] = CsA_rot·dω_A − Rr·dω_R
rot a:   tR = Rrᵀ·e_a ; tA = CsA_rotᵀ·(−e_a) ; dω = inv_rot_inertia ⊙ t (0x1000cb20, k = 1) ; Δv = dω × anchor (core)
         T[3+a] = R70·Δv_A − R70·Δv_R        // quirk: A side rotated by CsA (local_760), R side by +0x70
         Rm[3+a] = CsA_rot·dω_A − Rr·dω_R
M[row k][col a] = response of measured axis k (T for k < 3, Rm for k ≥ 3) to a unit impulse on axis a,
                  active axes only; rhs = (b, c) in active order

// solve (0x10033e50; eps = (f64)1e-9f = 0x3e112e0be0000000)
forward elimination (0x10033f80), for each column i:
    pivot row = largest |A[r][i]| for r > i scanning from the bottom (strictly larger wins); swap rows and rhs
    if |pivot| >= eps: for each row j > i with |A[j][i]| > eps: row_j += (A[j][i]·(−1/pivot))·row_i (and rhs)
back substitution (0x10034030), i = n−1..0:
    s = rhs_i − Σ_{k>i} A[i][k]·x_k
    if |A[i][i]| >= eps: x_i = s / A[i][i]
    elif |s| < eps·1000: x_i = 0
    else: fail → zero x, return 0
on failure the constraint applies nothing this PSI
λ_t[3], λ_r[3] = solution (0 for inactive axes)

// apply, immediately (not async)
if cR movable: push_core(cR, aR, Rᵀ70·λ_t, R(WR)·(Rᵀ70·λ_t)) ; rot_push_core_cs(cR, Rrᵀ·λ_r)
if cA movable: push_core(cA, aA, CsAᵀ·(−λ_t), R(WA)·(CsAᵀ·(−λ_t))) ; rot_push_core_cs(cA, CsA_rotᵀ·(−λ_r))
```

With force and damp factors 1 this is a **one-PSI exact velocity solve**: the relative anchor velocity along
each locked axis becomes −err/dt (full positional correction every PSI, no iterations), and −0.3·overshoot/dt
on an active limit. It runs at priority 405, last in the unit: after the friction controllers (2000, 600),
the forces and springs (1500) and gravity (1000, which damps, commits the async pushes and adds `g·dt`).
Spring pushes are therefore already in the velocities the constraint corrects.

## 13. Glue paths that use the core

### 13.1 Physics Impulse (`0x10002f90`)

```
read pins: Position (0), Referential (1), Direction (2, default (0,0,1)), Direction Ref (3), Impulse (4, 10.0)
settings: twoPos (local 0), constantForce (local 1)
if constantForce: Impulse *= mgr.sim_dt (+0xcc)
rec = registry(target); if none: skip
obj = rec.object ; if obj.core.flags & 0xc: skip (fixed)
obj.wake()                                                 // 10.4
pw = Referential ? Referential->Transform(Position) : Position          // f32, then f64
if !twoPos: d = DirRef ? DirRef->TransformVector(Direction) : Direction
else:       d = (DirRef ? DirRef->Transform(Direction) : Direction) − pw
d = normalize(d) (0x1000e120: unchanged if |d|² < 1e-19) ; I = d·Impulse     // f64, then f32
if Referential == target:
    M = core.get_m_world_f_core_PSI(env.current_time)
    core.async_push_core((f32)Position /* local, used as core coords */, (f32)(Mᵀ·I), (f32)I)
else:
    obj.async_push_object_ws(pw, (f32)I)                     // 5 (wakes again)
activate input 0 off, output 0
```

The impulse lands in `*_change` and is committed at the start of the next PSI (6.3), before the controllers.
If the body was frozen, the queued revive (6.1) runs first and does not clear the pending change, so the
impulse still applies in full.

### 13.2 Physicalize, wake and write-back

See `docs/physics.md` 2.1 and 3.1 for the glue; the IVP side is 1.3 (template), 2 (creation), 10.4 (the wake
after creation: a queued revive, effective at the next PSI), 10.5 and 4.3 (write-back of revived objects only).

## 14. Port mapping (onto `src/phys`)

The API in `src/phys/phys.h` used by `src/bb/bb_physics.c` can stay. What changes is the inside of
`phys.c`. The collision/contact code in `phys_collide.c` stays interim until the collision and friction specs
exist; 14.4 says where it plugs in.

### 14.1 Structs

```c
typedef struct { double x, y, z, w; } IvpQuat;
typedef struct { double r[3][3]; double vv[3]; } IvpMatrix;     /* column-vector, world = r·local + vv */

typedef struct IvpCore {
    uint32_t unmovable : 1, rot_inertias_equal : 1, in_revive_list : 1;
    float upper_limit_radius, max_surface_deviation;
    float rot_inertia[3], mass, inv_rot_inertia[3], inv_mass;
    float rot_speed_damp[3], speed_damp, inv_object_diameter;
    int8_t movement_state;                 /* 1, 2, 3, 8 (0.4) */
    double time_of_last_psi; float i_delta_time;
    float rot_speed_change[3], speed_change[3];
    float rot_speed[3];                    /* core axes */
    float speed[3];                        /* world */
    double pos_last[3];                    /* pos_world_f_core_last_psi */
    float delta_pos[3];                    /* delta_world_f_core_psis */
    IvpQuat q_last, q_next;
    IvpMatrix m_world_f_core;              /* m_world_f_core_last_psi */
    float rotation_axis_world[3], current_speed, abs_omega, max_surface_rot_speed;
    /* sleep (10.2) */
    double time_calm_ref[2]; float q_calm_ref[2][4], pos_calm_ref[2][3];
    struct IvpSimUnit *unit;
    struct IvpController **controllers; uint32_t ncontrollers;
    PhysBody *body;                        /* one object per core in Ballance */
} IvpCore;

typedef struct IvpController {
    const struct IvpControllerVt *vt;      /* priority(), simulate(ctrl, ev, cores, n), core_deleted() */
} IvpController;

typedef struct IvpSimUnit {
    int8_t state;                          /* 1 active, 8 frozen */
    uint8_t fast, fast_prev, changed;
    IvpCore **cores; uint32_t ncores;
    struct { IvpController *c; IvpCore **cores; uint32_t n; } *entries; uint32_t nentries;   /* ascending priority */
    struct IvpSimUnit *prev, *next;
} IvpSimUnit;
```

`PhysBody` keeps the glue-facing data (entity, shape, material, group, collision flag) plus
`IvpCore core; float shift_core_f_object[3]; bool shift_is_zero; int8_t object_state (1/8/0x10)`. The interim
fields `pos, q, v, w, prev_*, sleep_counter, calm, short_*, long_*, awake` go away; `awake` becomes
`object_state == 1`.

`PhysWorld` gains: `double current_time, time_of_last_psi, time_of_next_psi, delta_psi, inv_delta_psi;
float psi_offset_f32; uint16_t sleep_countdown (10); IvpController gravity (with float g[3]);
IvpSimUnit *active, *frozen; IvpCore **revive_list;` and a pointer to the write-back set (the glue can keep
iterating bodies with `object_state == 1 && !fixed`, which is the same set). The RNG seed is a file-static
`uint32_t` starting at 1, shared by all worlds.

### 14.2 Functions

| New C function | Original | Replaces in `phys.c` |
|---|---|---|
| `ivp_core_init(core, q, pos, unmovable)` | `0x1000d400`/`0x1000d2f0` | part of `phys_body_create` |
| `ivp_object_init_core(body, desc)` | `0x10009f00` + `0x100093d0`, `0x1000d1a0`, `0x1000d270`, `0x1000cc70` | the mass/inertia block of `phys_body_create` (ball: 0.4·r², polygon: surface unit inertia × mass, auto-check 0.03; mass centre = Shift) |
| `ivp_quat_to_matrix`, `ivp_matrix_to_quat`, `ivp_quat_normalize`, `ivp_quat_normalize_iter`, `ivp_quat_mul`, `ivp_quat_slerp`, `ivp_quat_from_rot_poly`, `ivp_quat_from_rot_sin` | 3 | `phys_quat_to_mat`, `phys_mat_to_quat`, `quat_normalize` |
| `ivp_core_m_world_f_core_at(core, t, out)` | `0x1000c510` | |
| `ivp_body_m_world_f_object_at(body, t, out)` | `0x10009d70` | `body_matrix` |
| `ivp_async_push_core`, `ivp_push_core`, `ivp_async_push_core_ws`, `ivp_rot_push_core_cs`, `ivp_surface_speed`, `ivp_commit_pushes` | 5 | `push`, `apply_impulse` |
| `ivp_body_async_push_ws(body, p, imp)` | `0x1000a3e0` | `phys_impulse` (world case) |
| `ivp_damp(core, dt)` | `0x1000c610`/`0x1000c6a0` | `damp_factor` and the damping loop |
| `ivp_gravity_simulate` | `0x10012010` | the damping+gravity loop |
| `ivp_force_simulate` | `0x100046b0` | the force loop in `psi_step`; `PhysForce` stores `double point_cs[3]; float force_ws[3]` and becomes an `IvpController` (priority 1500) |
| `ivp_spring_simulate` | `0x100146d0` | the `PHYS_SPRING` branch of `solve_joint` |
| `ivp_constraint_create`, `ivp_constraint_simulate`, `ivp_lin_solve` | 12.4-12.6 | the rest of `solve_joint` and the 12-iteration loop |
| `ivp_sim_unit_psi(unit, ev, cores_out)` | `0x100121b0` | the body loops of `psi_step` |
| `ivp_calc_next_psi_matrix(core, ev)` | `0x1001e300` (+ `0x1001e870`, anomaly 9) | the integrate loop |
| `ivp_calc_movement_state`, `ivp_try_to_freeze`, `ivp_freeze_core` | 10.2-10.3 | `sleep_test` |
| `ivp_body_wake`, `ivp_revive_cores_psi`, `ivp_revive_sim_unit`, `ivp_revive_core` | 10.4 | `phys_body_wake` |
| `ivp_simulate_psi(world)` | `0x10013cb0` | `psi_step` |
| PSI loop in `phys_frame` | 11.2-11.3 | the `while` loop |

`ivp_simulate_psi`:

```
revive queued cores
cores_out = []
for unit in active list (6.2 order): ivp_sim_unit_psi(unit, ev, cores_out)
for i = len(cores_out)-1..0: ivp_calc_next_psi_matrix(cores_out[i], ev)
(collision passes: interim, 14.4)
```

`phys_frame`:

```
smoothed = (delta_ms + 3·smoothed)·0.25 ; target = current_time + smoothed·time_factor
// base = time of the last PSI (0 initially); next = f32 offset of the next PSI (0.0f initially, then (f32)(1/66))
while (target − base) > (double)next:
    current_time = base + (double)next
    time_of_last_psi = current_time ; time_of_next_psi = current_time + delta_psi
    base = current_time ; next = (float)(time_of_next_psi − current_time)
    ivp_simulate_psi(world)
current_time = target
write back every body with object_state == 1 using ivp_body_m_world_f_object_at(body, current_time)
```

(The comparison is `(target − base) > (f64)next_offset` in extended precision; doing it in f64 is fine.)

### 14.3 Behavioural differences from the interim code (what the port fixes)

- **PSI phase.** IVP runs a PSI when time reaches `T_n` (first at 0) and that PSI computes the motion over
  `[T_n, T_{n+1}]`; the pose between PSIs is extrapolated forward from `T_n`. The interim code steps at
  `T_{n+1}`.
- **Force timing.** Interim pushes go straight into `v` and are then damped; IVP commits them after damping
  (factor `1 − d·dt` more effect).
- **Impulses and wake-ups are deferred** to the next PSI (async push; queued revive for frozen bodies).
- **Rotation.** `rot_speed` is in core axes; the step quaternion is composed on the right
  (`q_next = q_next ⊗ Q`) and built with `sin` (balls) or the gyroscopic sub-stepping (unequal inertia).
- **Rotational damping is per axis**, chosen 1−x or exp by the vector length.
- **Sleep:** one global countdown (15-19 PSIs), skip test `|v| > 1 m/s` linear, short window 1e-4 / 2.5e-5·(R
  scaled) for 0.3 s → freeze, long window 0.01 / 0.04 for 4 s; extra damping +0.1 when the state is 2 or 3.
  Units (bodies plus their joints/springs) freeze and wake together.
- **Inertia** of polygons comes from the surface (needs the compact-surface inertia; the interim bounding-box
  inertia is a stand-in) and is auto-checked to ≥ 0.03·|I|.
- **Constraints** are one exact solve per PSI after gravity, not 12 sequential-impulse iterations with a 0.2
  bias; **springs** are async pushes with the 12.3 mapping.

### 14.4 Where the interim collision plugs in

Until the friction system is ported, run the interim contact generation and solve as a controller of the
unit at priority 600 (after gravity has committed pushes and added `g·dt`, before constraints), using
`m_world_f_core_last_psi` (the pose at this PSI) and pushing with `ivp_push_core` (immediate). Contacts should
merge the touching movable bodies' units (as the friction system does) or at least revive the partner, so a
resting stack sleeps as one. Impact events can stay where they are.

## 15. Open questions

1. Compact-surface manager virtuals used at creation: `get_mass_center` (vtable `+4`),
   `get_radius_and_radius_dev_to_given_center` (`+8`), `get_rotation_inertia` (`+0xc`). Their algorithms
   belong to the surface-builder spec; the port needs them for polygon bodies (inertia, radius → sleep scale).
2. The friction-system controllers at priorities 2000 (`0x1001d610`) and 600 (`0x1001d750`), and the revive
   cascade `0x1001d4d0`/`0x10022180` (does reviving a body wake/merge everything resting on it?).
3. Unknown core fields: `+0x10`, `+0x30`, `+0x5c` (deleted for unmovable cores), byte `+0x61` and u16 `+0x64`
   (cleared every PSI); sim-unit manager `+0x08` (9.73) and `+0x10` (0.3); sim-unit flag `0x800` (never set?).
4. `env+0x2c` (range manager → `0x1000a9f0`) and `env+0xa8` vtable `+0x28` at PSI start: presumably absent / a
   collision-side hook in Ballance; confirm the manager never installs a range manager.
5. `0x100099a0` (after controllers) and `0x1001eb10`/`0x10017850`/`0x10017770` (after integration): the
   mindist-side recomputation. Needed by the collision spec, not by the dynamics.
6. Spring and force controllers share priority 1500; with equal priority the most recently attached runs first.
   Both only accumulate async pushes, so the order does not change results (addition order aside).
7. The constraint solver branches not covered (12, intro) are unreachable from the shipped content; verify
   that no level enables hinge limits (`docs/physics.md` says none does).
8. Corrections to `docs/physics.md` found here: sleep tests every 15-19 PSIs on a world-wide countdown, the
   skip test is linear speed (`|v|² > 1`), the extra damping applies to states 2 and 3, `70000` is
   `max_collisions_per_psi`, the spring pin mapping is resolved (12.3), and SetPhysicsForce pushes are
   committed after damping.
