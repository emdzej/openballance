# IVP contact response: contact points, friction system, friction solver, impact solver

This is a port-ready spec of the part of the Ipion IVP engine in `physics_RT.dll` that turns collision-detection
results into velocity changes:

- the persistent **contact points** (IVP's "friction mindists") and their per-PSI geometric update;
- the **friction system**, which groups the contact points of touching cores. It runs the tangential friction
  "spring", the normal force, the energy bookkeeping, and the merging and splitting of systems;
- the **friction linear solver**: an LCP over the normal impulses of all contacts in a system;
- the **impact solver**: the elastic collision response, and the impact system that chains it through touching
  bodies;
- **materials**: how friction and elasticity are combined.

It complements [physics.md](physics.md), sections 3–6, which give the address map and the glue.

- **Addresses.** Function entry points in `physics_RT.dll`, image base `0x10000000`.
- **Names.** The DLL has no symbols. Class and field names follow Ipion/Havok IVP wherever the layout and
  behaviour match, and are **inferred**.
- **Evidence.** Everything was read from the disassembly. The Ghidra output in `re/physics_RT.dll.c` was used only
  as a map, because it gets x87 code and merged stack slots wrong.

## 0. How to read this document

### 0.1 Section prefixes

The four analysis parts keep their own numbering, with a prefix:

| Prefix | Part | Address range |
|---|---|---|
| **CP** | contact points, materials, collision-detection interface | `0x1001e000-0x10020000`, `0x1001a8b0`, `0x1000bd00-0x1000bf90` |
| **FS** | friction system | `0x1001a000-0x1001da00`, `0x1000b000-0x1000b4d0` |
| **LS** | friction linear solver | `0x10033500-0x10037000` |
| **IM** | impact solver | `0x10022000-0x10024e80` |

A reference such as "§CP4.2" or "[FS]" points into that part. Section 2 is shared: it merges the struct layouts
all four parts rely on, and it wins wherever a part's own table disagrees.

### 0.2 Precision notation

- `f32`/`float` is a value stored as float. `f64`/`double` is a value stored as double.
- "ext" or "x87" means the value is kept on the x87 stack at 64-bit mantissa and never rounded. The PSI loop sets
  the control word to extended precision, so every intermediate is extended until it is stored.
- A port that uses `double` for every intermediate and rounds exactly at the documented `f32` stores gets the
  same behaviour. It will not be bit-exact.
- Dot products of two `f32x3` are summed as `(a.z*b.z + a.y*b.y) + a.x*b.x` (`0x1001b060`) unless stated
  otherwise.

### 0.3 Corrections made when merging

The part texts below have been corrected where they disagreed. These are the corrections:

| Fact | Correct value | Wrong in | Evidence |
|---|---|---|---|
| `core+0x94` / `core+0xa4` | `+0x94` = **rot_speed ω (core space)**; `+0xa4` = **speed v (world)** | CP's core table had them swapped | `0x1000bf90(core, p_cs, &core+0xa4, &core+0x94, out)` forms the cross product from arg 3 (`+0x94`) and adds arg 2 (`+0xa4`) |
| `tmp+0x70` / `tmp+0x74` | `+0x70` = virt_mass, `+0x74` = inv_virt_mass | IM's tmp table had them swapped | `0x1001f860` accumulates the sum into `+0x74` and stores `1.0f/sum` into `+0x70` |
| mindist `do_impact` | vtable `0x100637b4` slot 7 (`+0x1c`), the entry at `0x100637d0` = `0x100240a0` | CP said "vtable +0x10", counting from `0x100637c0` | ctor `0x1001629f` stores `0x100637b4` |
| `tmp+0x60..+0x6c` | **union**: friction solver: `+0x60/+0x64` friction-info pointers, `+0x68` key copy, `+0x6c` gap copy. Impact path: `+0x60` rescue addon, `+0x64` predicted gap, `+0x68` elasticity | each part listed only its own use | `0x100366a0` writes `+0x60..+0x6c`, and `0x10024040`/`0x100249b0` write the same offsets. tmp is rebuilt on every update, so the two uses never meet |
| friction pair `+0x20` initial value | `-1000.0` (dwords `0`, `0xc08f4000`) | IM listed it as open | `0x1001d340` |

## 1. The whole thing in one page

### 1.1 Objects

- **Mindist (collision detection).** One per close pair of collision features, for example ball against
  triangle ledge. It holds the two objects, an ordered pair of synapses with feature types, and the current gap
  (`md+0x54`).
- **Contact point (cp, 0x78 bytes).** Created from a mindist. It holds only feature references:
  `(obj, type, compact-edge pointer)` per side.
  - It recomputes normal, contact point, gap, arms and virtual mass from the current poses on every update
    (`0x1001f860`). The results go into a fresh 0xe0-byte **tmp_contact_info** in short-term memory.
  - It keeps state across PSIs:
    - the friction "spring" displacement `s0, s1` along two tangents;
    - the normal pressure `N`;
    - a warm-start key;
    - the combined friction μ = f0·f1.
- **Friction system (fs, 0x50 bytes).** It contains:
  - every contact point of a connected group of touching cores, grouped into **core pairs** (0x38 bytes);
  - a per-core **friction info** (0xc bytes): the list of the core's contacts in this fs.

  Fixed cores are shared between systems and never connect components.

### 1.2 Per PSI, in order

`0x10013cb0` runs the environment step. The core-integration and collision parts own what is not detailed here.

1. **PSI listeners.**
2. **Ball triangle transfer** (`0x10017790` → `0x10018040`, §CP3.2). For every **car-wheel** ball mindist whose
   closest triangle changed while the gap is below 0.045, create the new triangle's contact point. Hand the old
   contact's pressure and spring displacement over to it. Car wheels do not exist in Ballance, so this never
   runs there (§8 item 7).
3. **Controllers, per simulation unit, highest priority first** (§FS1.2):
   1. **2000, fs update** (`0x1001d610`):
      - recompute every cp's tmp (`0x1001f860`);
      - accumulate the "anti-energy" injected by gap changes;
      - ease that energy out of the pair's relative motion (`0x1001ccd0`), at most 10% of the relative kinetic
        energy per PSI, through the pending velocity-change fields.
   2. **1500, SetPhysicsForce** (glue). An async push into the pending `Δv`/`Δω` fields (`core+0x84`/`+0x74`).
   3. **600, friction spring** (`0x1001d750`):
      - clamp each contact's displacement `|s|` to `μ·N·inv_vm·dt²`;
      - apply a 2D tangential impulse (`0x1001b080`) that drives the tangential relative velocity to `s/dt`,
        clamped to `μ·N·dt`. It writes `core.speed`/`rot_speed` **directly**;
      - with ≥2 contacts, every 5 PSIs, equalize the springs of coplanar contacts of a pair (`0x1001c070`).
   4. **0, normal force** (`0x1001d6c0`):
      - **1 contact**: one impulse that drives the approach speed to `−k·(0.02 − gap)`, with k = 1, or 20 when
        gap > 0.02, and **no 1/dt**. It is never negative, and it writes speed directly (`0x1001ae40`);
      - **≥2 contacts**: build the n×n normal-impulse matrix, then warm-start Gauss, else LCP pivoting with at
        most 250 steps (§LS2–4). Apply the result as async pushes, and commit or abort them by an energy test.
      - Remove contacts whose gap is ≥ 0.045 or that left their feature. Delete an empty fs, or split it by
        union-find.
4. **Integration** (core part): consumes the pending `Δv`/`Δω`, damping, gravity, and computes the next PSI's
   pose.
5. **Mindist passes** (collision part). Between PSIs the time manager fires mindist events. When a mindist's
   gap falls below **0.011 m**, `do_impact` runs **at that exact time**:
   1. move the cores to the event time (`0x1000cfa0`);
   2. find or create the contact point;
   3. run the impact solver (§IM6): friction-cone compression in 10% steps, restitution at √e, and a minimum
      separation speed of 1.2·(0.02 + rescue);
   4. chain impacts to the other contacts of the pushed cores, up to 5000 rounds (§IM8);
   5. fire the post-collision event used by PhysicsCollDetection;
   6. recompute the next-PSI poses of the pushed cores.

### 1.3 Distances

All of these come from the settings block in §2.1:

| Gap | Meaning |
|---|---|
| < 0.011 | impact fires |
| 0.02 | **rest gap held by the normal force**: resting bodies float 2 cm apart, which is faithful |
| ≥ 0.045 | contact removed |
| up to 0.22 | impact system considers chained impacts |

### 1.4 Sign conventions, used throughout

- `n = tmp+0x00` is unit, world, and points **from obj0 toward obj1**.
- The contact point `tmp+0x20` lies on obj0's surface.
- The approach speed is `vr = (v0(p) − v1(p))·n`. It is positive when the bodies close.
- A positive normal impulse x pushes core0 by `−n·x` and core1 by `+n·x`.
- In the impact solver, `rel = v1 − v0`, so approaching means `n·rel < 0`.
- For a ball on the floor, obj0 is the ball (BALL feature) and obj1 is the floor (TRIANGLE feature).

## 2. Shared data (authoritative layouts)

### 2.1 Mindist settings (`IVP_Mindist_Settings`, static object at `0x10075db0`)

The block is built at DLL init: static ctor `0x10015fc0`/`0x10015fd0` → `0x100160c0` → `0x10015fe0(this, base)`.

- `base` is pushed as the double `0x3f847ae140000000`, which is `(double)0.01f` = 0.009999999776.
- The formulas are evaluated on x87, and every field is **stored as f32**.
- The fields are runtime globals: the file holds zeros, so a port must compute them, or hard-code the f32
  values below.

| Address | Off | Formula | f32 value | Name | Used by |
|---|---|---|---|---|---|
| `0x10075db0` | +0x000 | 0.1·b | 0.001 | real_coll_dist | |
| `0x10075db4` | +0x004 | 0.1b + 0.9b | 0.01 | min_coll_dist | rescue shell (IM4) |
| `0x10075db8..eb4` | +0x008 | min + (max − min)·k/64, k = 0..63 | 64 × 0.01 | coll_dists[64] | impact trigger (IM1) |
| `0x10075eb8` | +0x108 | b | 0.01 | max_coll_dist | next_impact threshold (IM8) |
| **`0x10075ebc`** | +0x10c | max + b | **0.02** (`0x3ca3d70a`) | **friction_dist** | cp initial gap; normal-force target (FS5.4, LS2.5) |
| `0x10075ec0` | +0x110 | 0.3b + friction_dist | 0.023 | keeper_dist | edge–edge parallel gap (CP4.5) |
| `0x10075ec4` | +0x114 | sqrt((keeper − min)·19.62) | ≈0.505 | speed_after_keeper_dist | collision part only |
| **`0x10075ec8`** | +0x118 | 0.01b | **0.0001** | distance_keepers_safety | contact reorder (LS2.2) |
| **`0x10075ecc`** | +0x11c | 2.5b + friction_dist | **0.045** (`0x3d3851eb`) | **max_distance_for_friction** | contact creation and removal |
| `0x10075ed0` | +0x120 | 20b + friction_dist | 0.22 | max_distance_for_impact_system | impact prediction cut-off (IM8) |
| `0x10075ed4` | +0x124 | 0.1·min | 0.001 | minimum_friction_dist | impact trigger epsilon (IM1) |
| `0x10075ed8` | +0x128 | 2b | 0.02 | mindist_change_force_dist | base rescue speed (IM6) |

The parts disagree in the third decimal of `0x10075ec4` (0.50498 against 0.50503). Only the collision part reads
it, so recompute it from the formula.

### 2.2 Environment and event-sim fields

**Event sim `es`.** It is built in `0x100124c0` and passed to every controller:

| Off | Type | Meaning |
|---|---|---|
| +0x00 | f64 | `delta_time` = env+0xc0 = 1/66 |
| +0x08 | f64 | `i_delta_time` = env+0xc8 = 66 |
| +0x10 | env* | |
| +0x14 | sim_unit* | its first dword holds the flags (FS1.2) |

**Environment fields used:**

| Off | Type | Meaning |
|---|---|---|
| +0x20 | ptr | anomaly manager (vtable `0x10063a58`) |
| +0x24 | ptr | anomaly limits: `+8` max_velocity 2000.0f, `+0xc` max_collisions_per_psi 70000, `+0x10` max_angular_velocity_per_psi π/2 |
| +0x54, +0x58, +0x64, +0x68, +0x6c | int | statistics counters (the impact path increments them) |
| +0x70 | f64 | total destroyed energy (statistics) |
| +0x8c | int | count of cp updates (statistics) |
| +0xac | ptr | material manager (§2.9) |
| +0xb0 | ptr | short-term memory used by the solver and the easing |
| +0xb4 | ptr | short-term memory used by tmp_contact_info and the impact path |
| +0xc0 | f64 | PSI length = 1/66 |
| +0xc8 | f64 | 66 |
| +0xf0 | f32 | \|gravity\|, set by `0x10013680`; 20 in Ballance |
| +0xf6 / +0xf8 | u16 / ptr | global collision listeners (count, array) |
| +0x120 | f64 | current time |
| +0x128 | f64 | time of the next PSI |
| +0x138 | int | PSI counter (cache stamps) |
| +0x13c | int | impact stamp |
| +0x148 | f64 | 0.9^(1/66) = 0.998404902, the per-PSI decay of the anti-energy |

**Short-term memory.**

- It is a bump allocator: `+8` cursor, `+0xc` limit, 32-byte aligned. `0x1000d110` is the fast path and
  `0x10020210` the slow path.
- It is transactional: `+0x10` is a u16 depth, and `0x100201b0` frees everything once the depth returns to 0.
- **Every tmp_contact_info lives only until the end of the PSI step, or of the enclosing impact/revive/transfer
  scope.** A port can use one per-PSI arena, plus nested marks for those scopes.

### 2.3 `IVP_Core` (0x238 bytes): fields used by this subsystem

| Off | Type | Meaning |
|---|---|---|
| +0x000 | u32 | flags: |
| | | - bits 2..3 = movement type: `&0xc ≠ 0` means **fixed / infinite mass**, `(flags & 0xc) == 4` means unmovable (its friction info is a hash keyed by fs); |
| | | - bits 0..1 = a calm/moving state, tested by `0x10036b80` and set to `1` (bit 1 cleared) by `0x10036d10` |
| +0x004 | f32 | radius/extent, used only by the rotation part of the rescue speed (IM4) |
| +0x00c | env* | |
| +0x010 | ptr | `car_wheel` / "no friction in impact". Zeroed by the core ctor and never set in Ballance |
| +0x014 | f32×3 | rot_inertia I (principal, core space) |
| +0x020 | f32 | mass |
| +0x034 | f32×3 | inv_rot_inertia I⁻¹ |
| +0x040 | f32 | inv_mass |
| +0x04c | f32x3* | spin clipping. NULL in Ballance |
| +0x052 / +0x054 | u16 / obj** | the core's objects |
| +0x05c | ptr | friction info (§2.8): one record for a movable core, a hash fs → record for an unmovable one |
| +0x060 | u32 | byte 0 = movement state (< 8 means simulated); byte 1 = temporarily_unmovable (anomaly) |
| +0x064 | i16 | impacts this PSI |
| +0x068 | f64 | time of the last PSI |
| +0x070 | f32 | rotation-sync scale (used by `0x1000cfa0`) |
| +0x074 | f32×3 | **rot_speed_change** Δω: pending, core space |
| +0x084 | f32×3 | **speed_change** Δv: pending, world |
| **+0x094** | f32×3 | **rot_speed ω, core space** |
| **+0x0a4** | f32×3 | **speed v, world** |
| +0x0b8 | f64×3 | position at the last PSI (sync) |
| +0x0d8 | f32×3 | velocity used by the sync |
| +0x108 | f64×4 | quaternion |
| +0x128 | f64[3][4] | **m_world_f_core** rotation, core to world. Element (r, c) is at `0x128 + 0x20·r + 8·c`. world = R·core, so core = Rᵀ·world |
| +0x188 | f64×3 | **position** of the mass centre, world |
| +0x1c8 | vector | controllers attached to the core |
| +0x1d4 | ptr | simulation unit |
| +0x1d8, +0x1e0 | f64 | freeze-check reference times. `0x1000cec0` sets both to env.time ("reset freeze check") |
| +0x228 | core* | union-find parent (FS7.2) |
| +0x22c | ptr | sync backup (0x38 bytes, `+0x30` = "pushed" flag) |
| +0x230 | int | impact stamp |

**Core helpers used everywhere:**

- **`0x1000bf90(core, p_cs, &v, &ω, out)`** gives the point velocity in world: `out = R·(ω × p_cs) + v`. The cross
  product is f32, the rotation is computed in doubles, and the result is rounded to f32.
- **`0x1000c830`** is the async push. It adds `Δω` and `Δv` of an impulse at a core-space point into the pending
  `+0x74`/`+0x84`.
- **`0x1000ca80` test_push(p_cs, imp_cs, imp_ws, out_dv, out_dw)** computes `dw = (p_cs × imp_cs) ⊙ I⁻¹` and
  `dv = imp_ws·inv_mass`. It does not touch the core.
- **`0x1000cbd0` commit** sets `ω += Δω; v += Δv; Δ = 0`.
- **`0x1000c5f0` abort** sets `Δ = 0`.
- **`0x1000c480`** is the kinetic energy: `0.5·(m·v² + Σ I_i ω_i²)`.
- **`0x1000c200` worst_vm(core, p)** is `1/(max(I⁻¹x(p.y²+p.z²), I⁻¹y(p.x²+p.z²), I⁻¹z(p.x²+p.y²)) + inv_mass)`.
  The max is taken in the order x, y, z, each strictly greater. The squares are f32.

### 2.4 `IVP_Real_Object` fields

| Off | Type | Meaning |
|---|---|---|
| +0x18 | env* | |
| +0x20 | ptr | head of the mindist-synapse list |
| +0x28 | ptr | head of the friction-synapse list (LIFO, §2.6) |
| +0x40 | ptr | object cache (§CP2.8), created on demand by `0x1001a190` |
| +0x80 | u32 | byte 0 = movement state; bit `0x2000` = the object has object listeners (friction create/delete and collision events) |
| +0x8c | ptr | surface manager. Vtable `+0x20` releases a ledge reference when a cp is destroyed |
| +0x98 | material* | the object's material |
| **+0xa0** | f32 | **extra radius**: the ball radius, 0 for polyhedra |
| **+0xa4** | core* | **physical core**, used for all dynamics |
| +0xa8 | core* | "friction core", used for fs membership in `0x10022180` and the flag test in `0x10036b80`. It equals +0xa4 in Ballance, which has no merged cores |

### 2.5 Mindist fields read by this subsystem

These are owned by collision detection.

| Off | Type | Meaning |
|---|---|---|
| vtable | `0x100637b4` | slot 0 `0x100181b0` simulate_time_event; slot 4 `0x10016190` deleting dtor; slot 7 `0x100240a0` **do_impact** |
| +0x14 | u32 | status: |
| | | - bits 8..9 = the index of the synapse that becomes cp.syn0. `syn0 = md.syn[(f>>8)&3]`, `syn1 = md.syn[((f^0x100)>>8)&3]`; |
| | | - bits 0..3 and `0xc000` = "not ok" (no impact, no creation on revive); |
| | | - bits `0x3000` = skip on revive; |
| | | - bits 22..29 = index into coll_dists |
| +0x18, +0x34 | 0x1c bytes each | synapse 0 and 1: `+0x08` next in the object's mindist-synapse list, `+0x10` obj, `+0x14` compact-edge pointer g, `+0x18` s16 offset back to the mindist, `+0x1a` s16 feature type |
| +0x28 / +0x44 | obj* | the objects of synapse 0 and 1 |
| +0x54 | f32 | **gap** = distance − both radii |
| +0x84 | ptr | the last triangle seen by the ball triangle transfer |

### 2.6 `IVP_Contact_Point` (cp, 0x78 bytes; `new(0x78)`, ctor `0x1001a8b0(cp, md)`)

| Off | Type | Init | Meaning |
|---|---|---|---|
| +0x00 / +0x04 | cp* | | next / prev in the fs contact list (head at fs+0x20) |
| +0x08 | synapse (0x14) | | **syn0**: `+0 next`, `+4 prev` in obj+0x28 (head insert); `+8` obj*; `+0xc` s16 = cp − synapse; `+0xe` u8 **feature type** (0 POINT, 1 EDGE, 2 TRIANGLE, 3 BALL); `+0x10` compact-edge pointer g |
| +0x1c | synapse | | **syn1**, same layout. Flat offsets: obj0 +0x10, type0 +0x16, g0 +0x18, obj1 +0x24, type1 +0x2a, g1 +0x2c |
| +0x30 | f32 | set at creation | **inv_virt_mass_mindist_no_dir**: the worst-case inverse mass of the pair (FS5.2 / CP4.6) |
| +0x34 | u8 | 0 | two_friction_values / anisotropic flag. Always 0 in Ballance |
| +0x38 | f32 | 0 | **span_friction_s[0]**: spring displacement along tmp+0x80 |
| +0x3c | f32 | 0 | **span_friction_s[1]**: along tmp+0x90 |
| +0x40 | tmp* | | tmp_contact_info, reallocated on every update |
| +0x44 | f32 | | **μ = f0·f1**, written only by `0x10024040` (creation, impact passes) |
| +0x48 | f32 | 0 | integrated_destroyed_energy (statistics only) |
| +0x4c | f32 | (TRIANGLE only) | `1/|(P1−P0)×(P2−P0)|` of syn1's triangle, set once in the ctor |
| +0x50 | f32 | 0 | old_energy_dynamic_fr: spring energy at the previous PSI |
| +0x54 | f32 | 0 | **now_friction_pressure N** = normal impulse × 66 |
| +0x58 | f32 | 0.02 | **gap**: surface distance minus both radii, clamped to ≥ 0, rewritten on every update |
| +0x5c | u16 | 20 | `slowly_turn_on_keeper`-like counter. No reader found |
| +0x5e | u16 | — | not touched |
| +0x60 | u8 (+3 bytes kept) | 1 | **"friction broken / recheck feature" flag**: |
| | | | - set by the ctor, by every update with syn0 = BALL, and whenever the spring is clamped (`0x1001bd04`, `0x1001d7c9`); |
| | | | - consumed (cleared) by the feature-validity tests in CP4.3–4.5. Validity is therefore re-tested only after a slip, or always for balls |
| +0x64 | i32 | 0 | **has_negative_pull_since**: the warm-start key (LS2.9) |
| +0x68 | f64 | env.time | time of the last update (dt for the spring integration). Rebased by fs reset_time |
| +0x70 | fs* | 0 | owning friction system |

### 2.7 `tmp_contact_info` (0xe0 bytes; `IVP_Contact_Situation` + `IVP_Impact_Solver_Long_Term`)

It is allocated from env+0xb4 short-term memory on **every** `0x1001f860`.

| Off | Type | Writer | Meaning |
|---|---|---|---|
| +0x00 | f32×3 (+pad) | update | **surf_normal n**: unit, world, obj0 → obj1. The CollDetection listener may negate it in place (IM3) |
| +0x10 | f32×3 | impact | **rel speed v1 − v0** at the contact, before the impact. This is the event speed |
| +0x20 | f64×3 | update | **contact_point_ws**, on obj0's surface |
| +0x40 / +0x44 | obj* | read_materials | obj0, obj1 |
| +0x48 / +0x4c | ptr | read_materials | g0, g1 |
| +0x50 / +0x54 | material* | read_materials | materials of syn0, syn1 |
| +0x58 | i16 | solver | index in the friction solver, −1 if excluded |
| +0x5a | i16 | update = 0; impact system ++ | number of pushes in the current impact system |
| +0x5c | u32 | update clears bits 0..9 | byte 0 = "prediction evaluated" (impact system). **Bits 8..9: `(x & 0x300) == 0x100` means the contact left its feature, so remove it** |
| +0x60 | union | solver / impact | solver: friction info of core0 in this fs. Impact: f32 rescue addon |
| +0x64 | union | solver / impact | solver: friction info of core1. Impact: f32 predicted gap at the next PSI (1e20 = far) |
| +0x68 | union | solver / read_materials | solver: copy of cp+0x64. read_materials: f32 **elasticity e0·e1** |
| +0x6c | f32 | solver | copy of cp+0x58 |
| +0x70 | f32 | update | **virt_mass** = 1.0f / inv_virt_mass (inf if both cores are fixed) |
| +0x74 | f32 | update | **inv_virt_mass** along n = Σ over non-fixed cores of `(c·(I⁻¹⊙c)) + inv_mass`, with c = +0xc0/+0xd0 |
| +0x78 / +0x7c | core* | update | core0 / core1, **NULL if fixed** |
| +0x80 | f32×3 (+pad) | update | **span_friction_v[0]**: unit tangent |
| +0x90 | f32×3 (+pad) | update | **span_friction_v[1]** = n × v0 |
| +0xa0 / +0xb0 | f32×3 (+pad) | update | contact point in core 0 / core 1 space: `Rᵀ(cp_ws − pos)`; 0 if fixed |
| +0xc0 / +0xd0 | f32×3 (+pad) | update | `p_cs × n_cs` per core, so that `ω·cross = n·(ω × r)`; 0 if fixed |

### 2.8 Friction system, core pair, friction info

**`IVP_Friction_System`** (0x50 bytes, ctor `0x1000b000(env)`, dtor `0x1000b100`):

| Off | Type | Meaning |
|---|---|---|
| +0x00 | vtbl `0x100633d0` | controller, **priority 600** (do_simulation `0x1001d750`) |
| +0x04 | env* | |
| +0x08 | vtbl `0x10063408` | controller, **priority 0** (do_simulation `0x1001d6c0`; slot 0 core_deleted `0x1001d9a0`) |
| +0x0c | fs* | back pointer |
| +0x10 | vtbl `0x100633ec` | controller, **priority 2000** (do_simulation `0x1001d610`) |
| +0x14 | fs* | back pointer |
| +0x20 | cp* | head of the contact list |
| +0x24 | `IVP_U_Vector<core>` | all cores, fixed ones included. `{u16 cap, u16 n @+0x26, core** @+0x28}` |
| +0x2c | vector | movable cores (n @+0x2e, elements @+0x30) |
| +0x34 | vector | core pairs (n @+0x36, elements @+0x38) |
| +0x3c | s16 | number of cores |
| +0x3e | s16 | **number of contact points** |
| +0x40 | s16 | solver's excluded count. Always set to 0 by `0x10036b80`, so it is dead |
| +0x44 | u8 | union_find_needed |

`IVP_U_Vector` grows as `cap = 2·cap + 1` (`0x1000b600`). Elements are removed by shifting the tail down, and
searches run from the end.

**`IVP_Friction_Core_Pair`** (0x38 bytes, init `0x1001d340`):

| Off | Type | Init | Meaning |
|---|---|---|---|
| +0x00 | vector of cp* | empty | `{u16 cap, u16 n @+2, cp** @+4}` |
| +0x18 | s32 | 1 | PSIs until the next spring easing (reset to 5) |
| +0x20 | f64 | −1000.0 | time of the last impact of the pair |
| +0x28 | f32 | 0 | integrated_anti_energy |
| +0x2c / +0x30 | core* | | the two cores, in either order |

**Friction info for a core** (0xc bytes, at core+0x5c): `{u16 cap, u16 n, cp** elems, fs* owner}`.

- `0x1000d960(core, fs)` gets the info.
- `0x1000d9b0` inserts it, `0x1000da50` removes it, `0x1000da80` removes and frees it.

### 2.9 Materials

**Material** (`IVP_Material_Simple`, 0x30 bytes, ctor `0x1000bf00(double friction, double elasticity)`, vtable
`0x10063464`):

| Off | Type | Value | Accessor |
|---|---|---|---|
| +0x08 | int | 0 | second_friction_x_enabled (anisotropy). Always 0 |
| +0x10 | f64 | friction | slot 0 `0x1000bed0` |
| +0x18 | f64 | **uninitialised** | slot 1 `0x1000bf40`, second friction. Read only in dead code |
| +0x20 | f64 | elasticity | slot 2 `0x1000bee0` |
| +0x28 | f64 | 0.0 | slot 3 `0x1000bef0`, adhesion |

Slot 4 `0x1000bf80` returns the name, a constant string. Slot 5 is the deleting dtor.

**Material manager** (env+0xac, vtable `0x10063434`, ctor `0x1000bda0`):

| Slot | Function | Result |
|---|---|---|
| 0 | `0x1000be30` | ignores its arguments and returns a lazily created static default material (0.5, 0.5) at `0x10075d7c` |
| 1 | `0x1000bd10` | friction(cs) = `cs.mat[1]→friction() * cs.mat[0]→friction()`. The mats are at cs+0x54 and cs+0x50; the product is f64 |
| 2 | `0x1000bd40` | elasticity(cs) = `mat[0].e * mat[1].e` |
| 3 | `0x1000bd70` | adhesion(cs) = the sum. Always 0 and unused |

The per-synapse material lookup and `read_materials` are in §CP5.

**No rolling-resistance term and no static/dynamic friction threshold** is reachable in Ballance:

- The spring behaves statically until `|s| > μ·N·inv_vm·dt²`, then it slips.
- Rolling losses come only from the energy easing (§FS6), the gap spring and the damping (core part).
- The static ≠ dynamic friction path needs material +8 ≠ 0, which never happens.

## 3. Contact points [CP]

Layouts of cp, tmp, synapses, objects and cores are in §2. This part covers the geometry encoding, creation, the per-PSI update, materials, removal and the collision-detection interface.

### CP2 Geometry encoding, constructor and object cache

#### CP2.1 Compact-ledge geometry referenced by synapses (read-only)

These are IVP compact ledges built by the surface builders. A synapse stores a pointer `g` to a **compact edge** (a
4-byte dword inside a 16-byte compact triangle). This holds for every feature type:

- **Triangle.** `tri = g & ~0xf`. The triangle dword `*tri` holds the triangle index in bits 0..11 and the material
  index in bits 24..30 (`(*tri >> 24) & 0x7f`, read by `0x1001d830`).
- **Ledge.** `ledge = tri − ((*tri & 0xfff) + 1)·16`. The ledge header comes before triangle 0.
- **Points.** `points = ledge + *(int*)ledge`. Each point is 16 bytes: three floats, then a 4th dword.
- **Edge.** The edge dword holds the start-point index in bits 0..15, and in bits 16..30 a signed offset, counted in
  dwords, to the **opposite edge** of the neighbouring triangle: `opp = g + sext15(*g >> 16)·4`, coded as
  `(int)(*g << 1) >> 17`.
- **Next and previous edge in the triangle.**
  - `next(g) = g + T_next[g & 0xc]`, with `T_next` at `0x100685b8` = {0, +4, +4, −8}.
  - `prev(g) = g + T_prev[g & 0xc]`, with `T_prev` at `0x100685c8` = {0, +8, −4, −4}.

  Edge slots sit at triangle offsets +4, +8 and +0xc.
- **What `g` means per type.** For a POINT the point is `start(g)`. For an EDGE the edge runs from `start(g)` to
  `start(next(g))`. For a TRIANGLE `g` is any edge of the triangle and P0, P1, P2 = start(g), start(next(g)),
  start(prev(g)).
- **Balls.** Ball synapses also carry a valid `g`. The update code always forms `ledge`/`points` from both synapses,
  and `0x1001d830` dereferences `g` to read a material index. A port must give balls a dummy one-triangle ledge
  with material index 0, or special-case them.

Triangle normal `0x10021280(g, ledge, out double3)`: `N = (P1 − P0) × (P2 − P0)`. It is computed in doubles from the
float points and is **not normalized**. IVP orders the edges so that N points out of the solid.

#### CP2.2 Synapse ordering invariant

**Ordering invariant.** syn0's type is always POINT, EDGE or BALL. The update dispatcher writes to address 0 (a
deliberate crash) for type0 = TRIANGLE. A TRIANGLE is always syn1. EDGE as syn0 implies EDGE–EDGE.

#### CP2.4 Contact-point constructor `0x1001a8b0(cp, md)`

The ctor `0x1001a8b0(cp, md)` does the following:

1. For k = 0, 1 (with s = md.syn[(md.flags>>8 ^ k·1)&3]): copy obj and g; set the low byte of syn.+0xe to `s.type`;
   set +0xc to the back-offset; set prev = NULL; push the synapse onto the head of `obj->friction_synapses`
   (obj+0x28), fixing the old head's prev.
2. `cp+0x68 = env(md.obj0).time`.
3. If `type1 == TRIANGLE`: `cp+0x4c = (float)(1.0 / |N|)`, where N comes from `0x10021280` in double and the length
   from `0x1000dd20` (fsqrt).
4. Zero +0x50, +0x54, +0x48, +0x38, +0x3c, +0x64, +0x70 and the low byte of +0x34. Set the low byte of +0x60 to 1.
   Set +0x58 = friction_dist (0.02) and +0x5c = 20.

#### CP2.8 Object cache (`IVP_Cache_Object`, from `0x1001a190(obj)`)

| Off | Type | Meaning |
|---|---|---|
| +0x00 | int | PSI stamp, compared with env+0x138 |
| +0x04 | int | reference count. Incremented on get; the update decrements it at the end. |
| +0x30..+0x80 | double 3×3 | m_world_f_object rotation (rows at +0x30, +0x50, +0x70) |
| +0x90 | double3 | m_world_f_object translation = **object origin in world** (the ball centre) |

On get, the cache is refreshed (`0x10018a40`) if obj.state < 8 and stamp < env.psi_counter.

- `0x10018d10` / `0x100217d0(g, ctx)`: world position of `start(g)` as `M·p + t`, in double.
- `0x10018ca0`: world point to object space, `Rᵀ(p − t)`.
- `0x10018ea0`: double vector, object to world, `R·v`.
- `0x10018f10`: float vector, object to world, `R·v`, stored as float.

`ctx` is a 5-dword "cache ledge point" record: {points, ledge, cache, obj, synapse}.

### CP3 Creation: how a mindist becomes a contact point

#### CP3.1 Triggers (callers of `0x10022180`)

1. **Impact** ([IM]). Mindist vtable slot 4 `do_impact` `0x100240a0` → `0x10023cd0`. **Every impact first creates or
   finds the cp** with update = 1, and the impact is then solved on that cp's tmp_contact_info. So every collision
   that is resolved leaves a contact point behind.
2. **Core revive** `0x1000aea0` → `0x1001d4d0(core)`. For each object of the core and each of its mindists
   (obj+0x20 list), all of these must hold:
   - `(md.flags & 0x3000) == 0`;
   - the other object's core is not fixed;
   - the other core has no friction info yet;
   - after the exact recalc (`0x10019950`), `(md.flags & 0xc000) == 0`;
   - `md.gap (+0x54) < 0.045`.

   Then `try_to_generate_managed_friction(md, …, sim_unit = core+0x1d4, update = 1)` runs. If a cp was created, the
   other core is revived (`0x1000cec0`). Otherwise, if this core is fixed (`&0xc`), it calls `md->vtable[+0x10](1)`
   (a mindist delete or reset; see the collision detection docs).
3. **Ball rolling onto a new triangle** `0x10017790`, once per PSI, **before the controllers**, called from env step
   `0x10013cb0` after the PSI listeners `0x10013c40`. For each mindist in the mindist manager's wheel vector
   (mgr+0x10, count u16 at mgr+0xe). Only car-wheel mindists are in it (§8 item 7):
   1. recalc it (`0x100187a0`, collision detection);
   2. let B be the synapse with type BALL and T the other;
   3. if `T.type == TRIANGLE`, `T.g != md+0x84` (triangle changed) and `md.gap < 0.045`, call
      `0x10018040(md, ball_obj, md.gap)` and set `md+0x84 = T.g`.

#### CP3.2 `0x10018040` ball triangle transfer (keeps friction state across seams)

```
transfer(md, ball_obj, gap_md):
  if ball_obj.friction_synapses == NULL: return       // the ball has no contact: nothing to inherit
  shortterm.nest++
  new = try_to_generate_managed_friction(md, &fs, &created, NULL, update=1)
  if created:
    old = cp_of(ball_obj.friction_synapses)      // head = most recently created cp of the ball
    t_saved = old.time; old.time = env.time
    update(old)                                   // dt = 0, so the span is not integrated; refreshes old.tmp
    new.pressure(+0x54) = old.pressure; old.pressure = 0
    new.time = t_saved; new.gap = gap_md          // overrides the gap the update just computed
    old.time = t_saved
    sv  = old.s0*old.tmp.v0 + old.s1*old.tmp.v1   // float3; .y and .z rounded to float, .x kept x87
    new.s0 = dot(sv, new.tmp.v0); new.s1 = dot(sv, new.tmp.v1)
  shortterm.nest--; shortterm_reset_if_unnested()
```

The old cp is not deleted here. It is removed later by the distance and feature tests (§CP6).

#### CP3.3 `0x10022180` try_to_generate_managed_friction(md, out_fs*, out_created*, sim_unit_hint, update)

The friction-system bookkeeping belongs to [FS]. Here it is summarised only at the interface.

```
  oA = md.syn[sort].obj, oB = md.syn[sort^1].obj
  cM = oA.friction_core(+0xa8), cO = oB.friction_core
  if cM.flags & 0xc: swap(cM, cO)                 // cM = a movable core
  fiM = cM.friction_info(+0x5c); env = cM+0xc
  cp, created = find_or_create(md)                // 0x10020030
  if !created:
      *out_fs = fiM.fs; *out_created = 0
      if update: update(cp); read_materials(cp, cp.tmp)      // 0x1001f860, 0x10024040
      return cp
  if update: update(cp); read_materials(cp, cp.tmp)
  fire "friction created" (env listeners 0x10013bc0; object listeners via 0x1000a8a0 for objects with flag 0x2000)
  *out_created = 1
  [friction-system join/merge/create, then fs.add_dist(cp) (0x1000b360: push at head of fs+0x20, cp.fs = fs,
   fs.n_dists(+0x3e)++) and 0x1000b2c0(fs,cp) (core-pair bookkeeping); append cp to both cores' per-fs info
   vectors (0x1003aee0)]
  *out_fs = fs
  init_cp_constants(cp)                           // 0x1001d3d0, §CP4.6
  if both cores are movable and in different simulation units: merge the units (0x10011770/0x10011890)
  return cp
```

`find_or_create` `0x10020030` → `0x1001ffe0`: walk `md.obj0.friction_synapses`. For each node,
`cp = node + node.off`. If `cp.obj0 == md.obj1` or `cp.obj1 == md.obj1`, and `0x1001d910(cp, md)` returns true,
reuse that cp. Otherwise `new(0x78)` and run the ctor.

`0x1001d910(cp, md)` (the pair matches) is true if either holds:

- `cp.obj0 == md.syn[0].obj && cp.obj1 == md.syn[1].obj && match(cp.syn0, md.syn[0]) && match(cp.syn1, md.syn[1])`;
- the same with the pairs crossed.

`0x1001d840 match(cs, ms)`: the types must be equal (`cs.type == ms.type`), then:

| Type | Match rule |
|---|---|
| POINT | same ledge, and `(*cs.g ^ *ms.g) & 0xffff == 0` (same point index) |
| EDGE | `ms.g == cs.g` or `ms.g == opp(cs.g)` (either half of the shared edge) |
| TRIANGLE | `(cs.g ^ ms.g) & ~0xf == 0` (same triangle) |
| BALL | always |
| other | write to address 0 |

### CP4 Per-PSI geometric update `0x1001f860` (`IVP_Contact_Point::recalc_tmp_contact_info`, inferred)

Callers:

- [FS]'s friction system per PSI: `0x1001d610`, which updates the single cp of a 1-cp system, or calls `0x1000b190`,
  which updates every cp of a multi-cp system and accumulates `Σ (gap_old − gap_new)·pressure` into the pair
  energy at pair+0x28 when positive;
- creation (§CP3.3);
- the transfer (§CP3.2);
- `0x1001cc20` (impact pre-pass: update every cp of a core pair, read materials, remove the cp if
  `(tmp.flags & 0x300) == 0x100`);
- `0x100242c0` ([IM]).

The update reads the poses and velocities of the cores. **It writes nothing to the cores.**

#### CP4.1 Driver

```
update(cp):
  env = cp.obj0.env
  t = shortterm_alloc(env+0xb4, 0xe0); t.s5a = 0; t.flags(+0x5c) &= ~0x3ff
  cp.tmp = t; env.n_cp_updates(+0x8c)++
  ctx1 = {points1, ledge1, cache1 = get_cache(obj1), obj1, &cp.syn1}
  ctx0 = {points0, ledge0, cache0 = get_cache(obj0), obj0, &cp.syn0}
  vtmp = (0,0,0)                                  // float3 scratch passed to the case fns, later reused for core1's velocity
  switch type0:
    POINT: p0 = world(start(g0), ctx0)            // double3
    BALL : p0 = cache0.translation(+0x90); cp.flags60.lowbyte = 1
    EDGE : edge_edge(cp, g0, g1, ctx0, ctx1, t); goto tail       // assumes type1 == EDGE
    TRIANGLE/other: *(int*)0 = 0                  // assert
  switch type1:
    POINT   : p1 = world(start(g1), ctx1); point_point(cp, p0, p1, t)
    EDGE    : point_edge(cp, p0, g1, ctx1, t)
    TRIANGLE: point_triangle(cp, p0, g1, ctx1, t)
    BALL    : point_point(cp, p0, cache1.translation, t)
tail:
  r0 = obj0.radius(+0xa0)
  t.cp_ws.y = t.n.y*r0 + t.cp_ws.y; t.cp_ws.z = t.n.z*r0 + t.cp_ws.z; t.cp_ws.x = t.n.x*r0 + t.cp_ws.x   // x87, stored double
  cp.gap = cp.gap - (r0 + obj1.radius)            // float
  if cp.gap < 0.0f: cp.gap = 0                    // constant 0x10063370; penetration depth is discarded
  cache0.ref--; cache1.ref--
  normalize_f(t.v0)                               // 0x1000e030: if |v|^2 < 1e-19 leave unchanged, else v *= 1/sqrt(|v|^2)
  t.v1 = (n.y*v0.z - v0.y*n.z, v0.x*n.z - n.x*v0.z, n.x*v0.y - n.y*v0.x)    // n × v0
  for k in 0,1:                                   // core k = obj_k.physical_core(+0xa4)
    if core.flags & 0xc:  cs_k = 0; cross_k = 0; vel_k := 0 (k = 0 only); t.core[k] = NULL; inv_k = 0
    else:
      d = t.cp_ws - core.pos                      // double
      cs_k  = (float) Rᵀ·d                        // x = (R10*dy + R20*dz) + R00*dx, likewise y and z
      ncs   = (float) Rᵀ·n                        // n_cs
      cross_k = cs_k × ncs                        // float: (cs.y*n.z - cs.z*n.y, cs.z*n.x - cs.x*n.z, n.y*cs.x - n.x*cs.y)
      vel_k = point_velocity(core, cs_k)          // world float3 (0x1000bf90)
      inv_k = ((cross.z*I⁻¹z)*cross.z + (cross.y*I⁻¹y)*cross.y) + (cross.x*I⁻¹x)*cross.x + inv_mass
      t.core[k] = core
  t.inv_virt_mass(+0x74) = inv_0 (+ inv_1 if core1 is present, added as (sum1 + invm1) + previous)
  if core1 is present: vrel = vel_0 - vel_1  else vrel = vel_0                  // 0x100102d0
  t.virt_mass(+0x70) = 1.0f / t.inv_virt_mass                                 // float division; inf if both are fixed
  dt = env.time(+0x120) - cp.time; cp.time = env.time                          // double
  cp.s0 = cp.s0 - ((vrel.z*v0.z + vrel.y*v0.y) + vrel.x*v0.x) * dt             // float result
  cp.s1 = cp.s1 - dot(vrel, v1) * dt
```

Notes:

- **The order is significant.** `cp_ws` is moved onto the ball surface before the core-space arms are computed, so
  every arm, cross and inverse mass refers to the surface point, not the ball centre.
- **Span semantics.** `s_i` integrates −(v0_point − v1_point)·span_i, the tangential displacement of obj1 relative to
  obj0 since contact began. It also contains a small normal component, because vrel is not projected. This is the
  static-friction "spring" state that the friction solver ([FS]/[LS]) reads. The basis (v0, v1) is recomputed on every
  update but the scalars are kept, so a basis that rotates between PSIs reinterprets the stored slip. For TRIANGLE
  syn1 the basis is tied to the triangle's first edge, so it is stable. For POINT/BALL against POINT/BALL it is tied
  to the normal, with the axis switch described in §CP4.2.
- The update never reads `cp.pressure` or the material values.

#### CP4.2 `0x1001f050` point_point(cp, p0, p1, t): POINT/BALL against POINT/BALL

```
d = p1 - p0 (double3); len = normalize_d(d)       // 0x1000de30: if |d|^2 < 1e-19 return 0.0 and leave d; else d *= invsqrt, return |d|
cp.gap = (float)len
t.n = (float)d
if n.x*n.x < 0.9f (0x10063878): a = (1,0,0) else a = (0,0,1)     // only a.x and a.z are used; a.y is implicitly 0
t.v0 = (n.y*a.z, n.z*a.x - n.x*a.z, 0 - n.y*a.x)  // n × a
normalize_f(t.v0)
t.cp_ws = p0
```

The normal points from p0 (obj0) to p1 (obj1). A ball–ball pair gives a normal along the centre line, and the tail
puts the contact point on ball 0's surface. The function has no validity test.

#### CP4.3 `0x1001f160` point_edge(cp, p0, g1, ctx1, t): POINT/BALL against EDGE

```
A = world(start(g1)), B = world(start(next(g1)))     // double3
e = (float)(B - A); r = (float)(p0 - A)
inv_len = invsqrt_d( (e.z*e.z + e.y*e.y) + e.x*e.x ) // 0x1000db80, double, no zero guard
c = (r.z*e.y - e.z*r.y, e.z*r.x - r.z*e.x, r.y*e.x - e.y*r.x)   // c = e × r, float
dist = |c|_f * inv_len                               // 0x1000e480 (float sqrt)
cp.gap = (float)dist
if dist*dist <= 1e-19: c = (1,0,0)
else: k = inv_len / dist; c *= k                     // unit; c.z is held in x87, the others are stored float
t.n = (c.z*e.y - c.y*e.z, e.z*c.x - c.z*e.x, c.y*e.x - e.y*c.x)   // n = e × c: from p0 toward the edge line
normalize_f(t.n)
t.cp_ws = p0
t.v0 = c                                             // perpendicular to both the edge and n; normalized again in the tail
if cp.flags60.lowbyte == 1:
   cp.flags60.lowbyte = 0
   u = ((e.z*r.z + r.y*e.y) + r.x*e.x) * inv_len*inv_len   // projection parameter along the edge
   if u < 0.0 || !(u <= 1.0): t.flags = (t.flags & ~0x200) | 0x100      // left the edge
```

#### CP4.4 `0x1001ee70` point_triangle(cp, p0, g1, ctx1, t): POINT/BALL against TRIANGLE

```
t.cp_ws = p0
p_os = cache1.inv_transform(p0)                      // double3
if cp.flags60.lowbyte == 1:
   cp.flags60.lowbyte = 0
   bary = 0x10020ad0(ledge1, g1, p_os)               // unnormalized barycentric coords (float), see below
   if signbit(b0)|signbit(b1)|signbit(b2): t.flags = (t.flags & ~0x200) | 0x100   // projection outside (−0.0 counts)
N  = triangle_normal(g1)                             // double, (P1−P0)×(P2−P0)
n_os = N * cp.inv_triangle_det                       // x87 products stored double: unit outward normal in object space
n_ws = cache1.R · n_os                               // double3
P0 = points1[start(g1)]                              // float
cp.gap = ((p_os.z*n.z + p_os.y*n.y) + p_os.x*n.x) - ((P0.y*n.y + P0.x*n.x) + P0.z*n.z)   // signed plane distance, float
t.n = (float)(n_ws * -1.0)                           // into the triangle: from obj0 toward obj1
e = P1 - P0 (float, object space; P1 = start(next(g1)))
t.v0 = (float) cache1.R · e                          // world; normalized in the tail
```

`0x10020ad0` works in float in object space. With a = P1−P0, b = P2−P0 (P0 = start of the triangle's first edge
slot), q = p − P0:

- aa = a·a, bb = b·b, ab = a·b, det = bb·aa − ab²;
- qa = q·a, qb = q·b, λ1 = qa·bb − qb·ab, λ2 = qb·aa − qa·ab, λ0 = det − λ1 − λ2.

It stores det and the three λ, permuted by the edge slot. Only the sign bits of the three λ are used.

The gap is the distance from the plane, not from the triangle. If the projection falls outside the triangle, the
contact is flagged and later removed (§CP6). It is not clamped to the triangle's edges.

#### CP4.5 `0x1001f3b0` edge_edge(cp, g0, g1, ctx0, ctx1, t): EDGE against EDGE (convex–convex)

```
A0,A1 = world ends of g0; B0,B1 = world ends of g1                 // double
eb = normalize_d(B1 - B0); ea = normalize_d(A1 - A0)              // 0x1000dd50
c = ea × eb (double, 0x1000e280 = a×b); cc = |c|^2
if cc <= 1e-24 (0x100634e8):                                      // parallel
   t.flags = (t.flags & ~0x200) | 0x100
   cp.gap = keeper_dist (0.023); t.cp_ws = A0; t.n = (1,0,0); t.v0 = (0,1,0)
   return
na = ea × c; nb = eb × c                                          // double
tB = (na·B0 - na·A0) * (1.0 / (na·B0 - na·B1))
tA = (nb·A0 - nb·B0) * (1.0 / (nb·A0 - nb·A1))
PA = (1-tA)*A0 + tA*A1;  PB = (1-tB)*B0 + tB*B1                   // 0x1000dc40
d = PB - PA; len = |d| (fsqrt)
if len <= 1e-19: cp.gap = 0; k = 1/sqrt(cc); t.n = (float)(c*k)  // the sign of c is arbitrary
else:            cp.gap = len; k = 1/len; t.n = (float)(d*k)     // from edge0 toward edge1
t.v0 = (float)ea; t.cp_ws = PA
if cp.flags60.lowbyte == 1:
   cp.flags60.lowbyte = 0
   if tB < 0 || tB > 1 || tA < 0 || tA > 1: t.flags = (t.flags & ~0x200) | 0x100
```

#### CP4.6 `0x1001d3d0` init_cp_constants (creation only, after the update)

```
mats = get_materials(cp)                                 // 0x10023fa0 (§CP5)
if mats[0].aniso(+8) != 0 || mats[1].aniso(+8) != 0: cp.aniso(+0x34 low byte) = 1
c0 = obj0.phys_core, c1 = obj1.phys_core
if c0 fixed: cp.inv_vm_no_dir(+0x30) = 1.0 / worst_vm(c1, t.cs1)
elif c1 fixed: cp+0x30 = 1.0 / worst_vm(c0, t.cs0)
else: m0 = worst_vm(c0, t.cs0); m1 = worst_vm(c1, t.cs1); cp+0x30 = (float)(1.0 / ((m1*m0)/(m1+m0)))   // = 1/m0 + 1/m1
worst_vm(core, p) (0x1000c200) = 1.0 / ( max( I⁻¹x*(p.y²+p.z²), I⁻¹y*(p.x²+p.z²), I⁻¹z*(p.x²+p.y²) ) + inv_mass )
```

The squares are float. The max is taken in the order x, then y (taken if strictly greater), then z (taken if strictly
greater).

### CP5 Materials

**Material object** (`IVP_Material_Simple`, 0x30 bytes, ctor `0x1000bf00(friction:double, elasticity:double)`,
vtable `0x10063464`).

| Off | Type | Value | Accessor |
|---|---|---|---|
| +0x00 | vtable | | |
| +0x04 | | not initialised | |
| +0x08 | int | 0 | **second_friction_x_enabled**: the anisotropy flag tested by `0x1001d3d0` |
| +0x10 | double | friction | slot 0 `0x1000bed0` get_friction_factor |
| +0x18 | double | **uninitialised** | slot 1 `0x1000bf40` get_second_friction_factor. Only meaningful when +0x08 ≠ 0, which is never the case in Ballance. |
| +0x20 | double | elasticity | slot 2 `0x1000bee0` get_elasticity |
| +0x28 | double | 0.0 | slot 3 `0x1000bef0` get_adhesion |

Slot 4 is the name (`0x1000bf80` returns a constant string) and slot 5 is the deleting dtor.

**Material manager** (env+0xac, vtable `0x10063434`, ctor `0x1000bda0`):

| Slot | Function | Does |
|---|---|---|
| 0 | `0x1000be30` | `get_material_by_index(?, idx)`: ignores both arguments and returns a lazily created static default material (0.5, 0.5) at `0x10075d7c` |
| 1 | `0x1000bd10` | `get_friction_factor(cs)` = `cs.mat[1].friction() * cs.mat[0].friction()`. The mats are at cs+0x54 and cs+0x50. Double, x87. |
| 2 | `0x1000bd40` | `get_elasticity(cs)` = `cs.mat[0].elasticity() * cs.mat[1].elasticity()` |
| 3 | `0x1000bd70` | `get_adhesion(cs)` = sum (always 0) |

**Per-synapse material**, `0x10023fa0(cp, out[2])`. For k = 0, 1:

- `idx = (*(cp.syn_k.g & ~0xf) >> 24) & 0x7f` (`0x1001d830`);
- if idx == 0, `out[k] = cp.syn_k.obj.material(+0x98)`;
- otherwise `out[k] = env(obj0).matmgr->slot0(0, idx)`, which is the default 0.5/0.5 material. Triangles with a non-zero
  material index therefore get 0.5/0.5. Ballance's runtime-built ledges presumably have index 0 everywhere; confirm
  this in the surface builder.

**`0x10024040` read_materials(cp, t)**:

```
get_materials(cp, &t.mat[0])          // t+0x50, t+0x54
t.obj[0..1] = cp.obj0, cp.obj1        // t+0x40, +0x44
t.g[0..1]   = cp.g0, cp.g1            // t+0x48, +0x4c
t.elasticity(+0x68) = (float) matmgr.get_elasticity(t)
cp.friction(+0x44)  = (float) matmgr.get_friction_factor(t)
```

It is called at creation (`0x10022180`, whether or not the cp is new), in the impact pre-pass (`0x1001cc20`), and in
`0x100242c0`. It is **not** called on the per-PSI update. `cp+0x44` is therefore fixed from creation onwards, and
`t+0x68` holds valid data only in a tmp that the impact path filled.

**Thresholds.** No rolling-resistance or static/dynamic friction threshold exists in the contact-point code. The
only distance thresholds are those of §2.1, and the friction-force thresholds belong to [FS]. Materials never alter the
geometry.

### CP6 Removal

`0x1001c460(fs, cp)` ([FS]) unlinks the cp from the friction system and its pair bookkeeping, then calls the dtor.

Dtor `0x1001c230`:

1. For each synapse, `obj.surface_manager(+0x8c)->vtable[+0x20](ledge(g))` (release the ledge reference).
2. Fire "friction deleted" (`0x10013c00` env listeners; `0x1000a900` object listeners for objects with flag 0x2000).
3. Unlink both synapses from their `obj+0x28` lists.

The memory is freed by the caller.

| Trigger | Where | Condition |
|---|---|---|
| Distance or feature (multi-cp fs) | `0x10036b80`, [LS] solver setup | `cp.gap >= 0.045` **or** `(cp.tmp.flags & 0x300) == 0x100` → remove |
| Reorder (not a removal) | `0x10036b80` | otherwise, if `cp.gap > 0.02 + 0.0001` and `(cO.flags & cM.flags & 3) != 0` (friction cores), move the cp to the head of fs+0x20 (remove + add) |
| Distance or feature (single-cp fs) | `0x1001d660`, after the one-contact solve `0x1001ae40(cp, ev, 0.02, 1.0)` | same two conditions → remove |
| Impact pre-pass | `0x1001cc20` | after update and read_materials, `(t.flags & 0x300) == 0x100` → remove |
| Object leaves collision or is deleted | `0x10009a40(obj, keep_asleep)` from `0x10009350` / `0x10009ab0` | every cp on obj+0x28. If the argument is 0, both friction cores are first revived (`0x1000dab0`). An emptied fs is deleted through its vtable +0x18. |
| Core deleted | `0x1001d9a0` (fs secondary vtable slot 0) | every cp of the pairs that contain that core |
| [IM] | `0x100242c0` | see [IM] |

The feature-status bit is set only by the validity tests in §CP4.3–4.5 and by the parallel-edge case. A cp whose
projection leaves its edge or triangle therefore survives at most until the next removal pass of that PSI. A ball
rolling across a seam keeps its friction state only through the §CP3.2 transfer, which creates the next triangle's cp
before the old one is dropped.

### CP7 Inputs from collision detection (for a custom discrete collider)

The contact-point code takes no normal, point or distance from collision detection. **It recomputes them every PSI
from feature references and the current poses.** A replacement collider has to supply, per close pair:

1. **Two objects** obj0 and obj1. Each must expose a physical core with:
   - flags: is it fixed;
   - inv inertia (3 floats, principal, core space) and inv mass;
   - v (world float3) and ω (core-space float3);
   - R_core (double 3×3, core to world) and pos_core (double3, mass centre);
   - an object world transform (R_obj, t_obj), double;
   - a radius r (ball radius, or 0);
   - a material (friction, elasticity; anisotropy off).
2. **Two features with types, ordered** so that feature 0 is POINT, BALL or EDGE and feature 1 is POINT, EDGE,
   TRIANGLE or BALL. EDGE–EDGE is the only pairing with an EDGE as feature 0. Valid combinations: P–P, P–E, P–T, P–B,
   B–P, B–E, B–T, B–B, E–E. **Ball against triangle soup, the core case in Ballance, is (BALL, TRIANGLE)**: the ball
   is obj0 and the floor is obj1.
3. **Feature geometry** in object space: a point; an edge as two endpoints; a triangle as P0, P1, P2 in a consistent
   order with outward normal (P1−P0)×(P2−P0). The contact uses P1−P0 as its tangent basis, so the order must not
   change between PSIs. Edges need an "opposite twin" only for cp matching.
4. **When to create:**
   - at every impact the collider resolves;
   - on core revive, for pairs with gap < 0.045;
   - when a ball that already has a contact comes within 0.045 of a new triangle, by transfer (§CP3.2).

   Before creating, look for an existing cp with the same objects and matching features (§CP3.3).
5. **Gap semantics** expected downstream:
   - surface distance minus both radii, clamped to ≥ 0;
   - against triangles, the signed distance from the plane, computed from the ball centre;
   - the normal is unit and points from obj0 to obj1;
   - the contact point lies on obj0's surface (centre + n·r0);
   - the friction cutoff is 0.045 and the solver's "touching" distance is 0.02.
6. **Per-feature validity each PSI.** If the projection of the obj0 point leaves the edge or triangle (or two edges
   become parallel), flag the contact so that it is deleted. Do not clamp it.

Anything that bypasses the case functions and feeds (n, cp_ws, gap, v0) directly must reproduce §CP4.2–4.5 exactly to
keep the span basis and the gap semantics. The tail of §CP4.1 (arms, cross products, virtual mass, span integration) is
independent of the collider and should be ported as written.

### CP8 Other functions in `0x1001e000-0x10020000` (not contact logic)

| Address | What |
|---|---|
| `0x1001e070/0e0c0/1e1b0/1e200/1e260`, `0x1001ee10` | pointer-keyed open-addressing hash (CRC32 of the 4 key bytes, \| 0x80000000): remove, find, set |
| `0x1001ed10/1ed90` | dtors of hash containers (vtables `0x100633c0/0x100633c8`, compare `0x1001ee50`) used for per-core friction info (built at `0x1000a530/0x1000a56a` and `0x1001ed2e/0x1001edae`) |
| `0x1001ecd0` | vector push |
| `0x1001e300` and `0x1001e700..0x1001eb10` | core integration (out of scope) |
| `0x1001f850` | `cp.time -= dt`: friction-system time reset (fs vtable slot 3 `0x1001cbc0`) |
| `0x100200c0..0x10020290` | malloc wrappers and short-term memory pool |

`0x1001fa40` and `0x1001f6a0` are not functions: they are a jump-table target inside `0x1001f860` and an address
inside `0x1001f3b0`. The jump table for type1 is at `0x1001ffcc`: {POINT `0x1001fa54`, EDGE `0x1001fa8d`,
TRIANGLE `0x1001faae`, BALL `0x1001fa43`}.

## 4. Friction system [FS]

Struct layouts are in §2.8. Contact-point and tmp fields are in §2.6–2.7.

### Notation

Notation:
- `f32` is a float stored in memory. `f64` is a double. "ext" means the value stays on the x87 stack at 64-bit
  mantissa and is never rounded. A port should use `double` for ext values.
- `rsqrt(x)` is `1000dae0`. It takes an f32 argument and seeds from the exponent bits (`0x1ff00000 + (0x7ff00000 - hi)>>1`). It then does 5 Newton steps `y *= 1.5 - 0.5·x·y²` in ext. The result is 1/sqrt(x) to double precision, so port it as `1.0/sqrt((double)(float)x)`.
- `fast_normize(v)` is `1000df30`. If `|v|² < 1e-19` (f64 const `0x10063480`) it returns 0 and leaves `v` unchanged. Otherwise it scales `v` by rsqrt and returns the length `|v|²·rsqrt`.
- `normize(v)` is `1000e030`. It behaves the same way but returns 1 or 0.
- `len(v)` is `1000e480`: sqrt of the f32 components summed in ext.
- `cross(a,b)` is `1000e2c0` and computes the ordinary `a×b`. `addmul(o,a,b,s)` is `10010290` and computes `o = a + b·s` with `s` an f64. The sum is done in ext and stored as f32.
- `Mv(core,v)` is `1000f690`: `core.R · v`, where R is the f64 3×3 at core+0x128 with rows 0x20 apart. It maps core space to world space. `MTv` is `1000f7d0`: `Rᵀ·v`, mapping world space to core space. Both give f32 results.

### FS1 Where the friction system runs in a PSI

#### FS1.1 Three controllers

A friction system is one C++ object of 0x50 bytes, allocated as `new(0x50)` and built by the ctor `1000b000(env)`.
It carries **three controller interfaces**:

| Subobject | Vtable | Back pointer | Priority (slot 5) | `do_simulation_controller` (slot 4) | Purpose |
|---|---|---|---|---|---|
| fs+0x10 | `0x100633ec` | fs+0x14 = fs | **2000** (`1000b080` returns 0x7d0) | `1001d610` | Recompute contact info for every contact. Accumulate penetration energy. Ease energy. |
| fs+0x00 | `0x100633d0` | | **600** (`1000b0c0` returns 0x258) | `1001d750` | Tangential friction impulses (the "friction spring"). |
| fs+0x08 | `0x10063408` | fs+0x0c = fs | **0** (`1000b0b0` returns 0) | `1001d6c0` | Normal-force solve: single contact here, ≥2 contacts in [LS]'s `10036d70`. Delete or split the system. |

The other slots are:

- `0x100633d0`:
  - [0] `core_is_going_to_be_deleted`: no-op (`1000a490`).
  - [1] `get_minimum_simulation_frequency`: 1.0.
  - [2] `get_associated_controlled_cores` = `1001d600`: returns `&fs.moveable_cores` (fs+0x2c).
  - [3] `reset_time(t)` = `1001cbc0`: for every pair and every cp in it, `cp+0x68 -= t` (`1001f850`).
  - [6] dtor `1000b0d0` → `1000b100`, which frees the three vectors.
- `0x100633ec`: [0] no-op, [1] 1.0, [2] returns the static empty vector `0x10075d90`, [3] reset_time no-op, [6] dtor.
- `0x10063408`: [0] `core_is_going_to_be_deleted(core)` = `1001d9a0`, [1] 1.0, [2] the empty vector, [3] no-op, [6] dtor.

**Add and remove core.**

- `1000b390(fs, core)` adds a core:
  1. Append it to `fs.cores` (+0x24).
  2. If it is movable (`(core.flags & 0xc) == 0`), append it to `fs.moveable_cores` (+0x2c) and register **all three** controllers on the core, in this order: fs+8, fs, fs+0x10 (`10011c70`, which appends to core+0x1c8 and inserts into the core's sim unit).
  3. In every case, `fs.n_cores` (+0x3c) is incremented.
- `1000b410(fs, core)` reverses this. It uses `10011c00` in the order fs+0x10, fs, fs+8.

#### FS1.2 Controller order inside a sim unit

`100121b0` runs a sim unit:

- The per-unit controller list (+0x20, count u16 +0x1e) is kept **sorted ascending by priority**. `10011db0` does an insertion sort that swaps when `prio[i+1] < prio[i]`.
- The list is iterated **from the last entry to the first**, so the highest priority runs first. Equal priorities keep insertion order.
- For each entry `{controller*, IVP_U_Vector<core>}` it calls `controller->do_simulation(es, &cores)`.

Per PSI, a sim unit that holds a friction system therefore runs:

1. **2000**: `1001d610` updates contact geometry and energy bookkeeping.
2. 1500: SetPhysicsForce controllers, and anything else between 2000 and 600 (see the core and controller part).
3. **600**: `1001d750` applies the friction impulses.
4. **0**: `1001d6c0` applies the normal impulses, then deletes or splits the system.

Gravity and damping are not controllers here. They are handled by the core integration part.

**Event sim `es`.** It is built in `100124c0` and handed to every controller:

| Offset | Type | Meaning |
|---|---|---|
| +0x00 | f64 | `delta_time` = env+0xc0 (the PSI, 1/66) |
| +0x08 | f64 | `i_delta_time` = env+0xc8 (66) |
| +0x10 | env* | environment |
| +0x14 | sim_unit* | its first dword is the sim-unit flags |

**Sim-unit flag bits used here.** They are maintained in `100121b0`:

- `0x400` is set, and `0x800` cleared, when any core of the unit has \|v\|² > 1 this PSI.
- In the slow case, bits 0x3000 take the old 0xc00 bits and then 0xc00 is cleared.
- `0x100` is set and `0x200` cleared to mark "unit changed, recheck".

### FS2 Constants used by the friction code

The mindist settings are in §2.1.

Other constants used by friction:

| Constant | Value |
|---|---|
| `0x10063838` | f32 1e-6 |
| `0x100637e0` | f64 9.999999974752427e-07 = (double)1e-6f |
| `0x10063830` | f64 1e-38 |
| `0x10063690` | f64 0.0010000000474974513 = (double)0.001f |
| `0x10063500` | f64 0.10000000149011612 |
| `0x10063388` | f64 20.0 |
| `0x10063840` | f64 10000.0 |
| `0x10063280` | f64 1e-4 |
| `0x10063230` | f64 -1.0 |
| `0x100631d0` | f64 0.5 |
| `0x10063288` | f64 1.0 |
| `0x10063370` | f32 0.0 |
| `0x10063488` | f64 0.0 |
| `0x10063480` | f64 1e-19 |
| env+0x148 | f64 `0.9^(1/66)` = 0.998404902 (energy decay per PSI) |

There are **no static/rolling friction thresholds** in the code Ballance reaches.

- The friction "spring" (§FS4) behaves statically until its displacement exceeds `μ·N·m⁻¹·dt²`, then it slips.
- Rolling resistance comes only from the energy easing (§FS6) and the normal-gap spring.
- The two-friction-value path (static ≠ dynamic friction, §FS8) is unreachable, because every material has
  `+8 == 0`.

### FS4 Tangential friction ("friction spring")

#### FS4.1 `1001d750`: `IVP_Friction_System::do_simulation_controller(es, cores)`, priority 600

```
if fs.n_contacts > 1: do_friction_multi(fs, es)     // 1001c550 = 1001bc20 then 1001c1f0
                       return
cp = fs.first
t  = (f32)(cp.inv_vm30 * cp.N54 * cp.mu44 * es.dt * es.dt)   // multiply order exactly this, ext, then rounded to f32
s2 = cp.s0*cp.s0 + cp.s1*cp.s1                              // ext, kept as f64
if s2 > t*t + 1e-6f:                                        // rhs in ext (t f32, 1e-6f)
    r   = rsqrt((f32)s2)
    len = r * s2
    cp.destroyed48 = (f32)((len - t) * cp.N54 * cp.mu44 + cp.destroyed48)
    cp.byte60 = 1
    k = r * t                                               // ext
    cp.s0 = (f32)(cp.s0 * k); cp.s1 = (f32)(cp.s1 * k)
if cp.two_friction_values(+0x34) == 1: 1001b8b0(cp, es)     // dead in Ballance (§FS8)
else:                                  1001b080(cp, es)     // the returned energy is discarded
```

The slip limit `t` is a length: the largest displacement the friction impulse `μ·N·dt` can undo in one PSI with
inverse virtual mass `inv_vm30`.

#### FS4.2 `1001bc20`: friction for all pairs (≥2 contacts)

Pairs are visited from the last to the first. Within a pair, cps are visited from the last to the first.

```
for each pair P in fs.pairs (reverse):
    sum = 0 (ext)
    for each cp in P (reverse): sum += cp.N54 * cp.mu44 * cp.inv_vm30
    t  = (f32)(sum * dt * dt)            // the limit is SHARED by every contact of the pair
    if P.n == 0: continue
    th = (f32)(t*t + 1e-6f)              // f32 here (single-contact path: ext)
    e  = 0.0f
    for each cp in P (reverse):
        s2 = cp.s0² + cp.s1²  (ext → kept f64)
        if s2 > th:                       // same clamp as 4.1, using the pair's t
            r = rsqrt((f32)s2)
            cp.destroyed48 = (f32)((r*s2 - t)*cp.N54*cp.mu44 + cp.destroyed48)
            cp.byte60 = 1
            cp.s0 *= r*t; cp.s1 *= r*t    (f32 stores)
        if cp.byte34 == 1: 1001b8b0(cp, es)
        else: e = (f32)(e + 1001b080(cp, es))
    if e > 0: P.anti_energy28 = (f32)(e + P.anti_energy28)
```

#### FS4.3 `1001b080`: `IVP_Contact_Point::friction_force_local_constraint_2d(es)` → f32 energy delta

```
tmp = cp.tmp
maxI = cp.mu44 * cp.N54 * es.dt              // ext, kept as f64 (stack); f32*f32 then *f64
if maxI < 9.999999974752427e-07: return 0.0f   // nothing touched (cp.old_energy50 kept)
c0 = tmp.core0(+0x78); c1 = tmp.core1(+0x7c)
if c0 && c0.car_wheel && !c1 && (cp.next == NULL || cp.next.N54 == 0.0f):
    → car-wheel variant (§FS8; unreachable)
tcb.init(c0, c1, &tmp.cp_ws(+0x20), &tmp.span_v0(+0x80), &tmp.span_v1(+0x90), NULL)   // 10033a30
a = cp.s0 * es.i_dt - tcb.dv[0]      // ext; dv = relative velocity along the spans (below)
b = cp.s1 * es.i_dt - tcb.dv[1]
det = tcb.m00 * tcb.m11 - tcb.m01 * tcb.m01                   // f64 matrix, ext math
if det*det < 1e-38: return 0.0f                               // no impulse, old_energy50 kept
inv = 1/det
c00 = tcb.m11*inv (f64)   c01 = -tcb.m01*inv (f64)   c11 = inv*tcb.m00 (ext)
i0 = (f32)(c00*a + c01*b)
i1 = (f32)(c01*a + c11*b)
q  = i0*i0 + i1*i1                 // f32 operands, ext sum, stored f64  ← UNCLAMPED, reused for energy
if q > maxI*maxI:
    k  = rsqrt((f32)q) * maxI
    i0 = (f32)(i0*k); i1 = (f32)(i1*k)
tcb.exert_impulse_dim2(c0, c1, {i0, i1})                      // 10033ad0
E = sqrt(es.dt*es.dt * q * (cp.s0² + cp.s1²)) * 0.5           // ext; q is the pre-clamp value (quirk)
d = (f32)(E - cp.old_energy50)
cp.old_energy50 = (f32)E
return d
```

Notes:

- The impulse drives the relative tangential velocity to `s/dt`. `s` holds the negated accumulated slip, which
  [CP]'s `1001f860` keeps up to date: `s_k -= (v_rel·span_v_k)·(t_now − cp.t68)`. One PSI therefore undoes the
  displacement.
- `cp.s` is **not** reduced by the clamp here. The clamp on s (§FS4.1 and §FS4.2) happens before the call.
- The matrix is treated as symmetric. Only m00, m01 and m11 are read.

**`10033a30` + `10033560`: `IVP_Solver_Core_Reaction::init_reaction_solver_translation_ws` ([LS]'s file, as used here).**

The tcb is 0x144 bytes on the stack. Its fields:

| Offset | Meaning |
|---|---|
| +0, +4, +8 | dir pointers |
| +0x0c/+0x1c/+0x2c | core0 cross_cs per dir (f32×4) |
| +0x3c/+0x4c/+0x5c | core1 cross_cs per dir |
| +0x74/+0x84/+0x94 | core0 Δω per unit impulse (f32×4, 4th = inv_mass) |
| +0xa4/+0xb4/+0xc4 | core1 Δω per unit impulse |
| +0xd8 | f64 matrix, row stride 0x20: m00 +0xd8, m01 +0xe0, m11 +0x100, m02 +0xe8, m12 +0x108, m22 +0x128 |
| +0x138 | f32×3 dv |

Initialization:

1. If dir1 is set, zero the whole matrix and dv. Otherwise zero only m00 and dv0.
2. For core0 (sign σ = +1), if it is not NULL, and then for core1 (σ = −1), if it is not NULL, run `10033560`:

```
r      = (f32)(pos_ws - core.pos188)
for k in dirs:   c_k = MTv(core, cross(r, d_k))     (f32);  w_k = invI ⊙ c_k (f32), w_k.4th = inv_mass
m00 += c0·w0 + inv_mass      (accumulated in f64)
dv0 += σ * (d0·core.speed + core.rot_speed·c0)      (f32 store)
m11 += c1·w1 + inv_mass ;  m01 += w1·c0  (no d0·d1 mass term: dirs assumed orthonormal)
dv1 += σ * (d1·core.speed + core.rot_speed·c1)
```

**`10033ad0`: `exert_impulse_dim2(c0, c1, imp[2])`.**

```
c0 (if not NULL):  speed += d0*(imp0*invm);  speed += d1*(imp1*invm);  rot += w0_c0*imp0;  rot += w1_c0*imp1
c1 (if not NULL):  speed += d0*(-imp0*invm); speed += d1*(-imp1*invm); rot += w0_c1*(-imp0); rot += w1_c1*(-imp1)
```

- Each line is a separate `addmul` in ext, stored f32.
- invm is used as an f64 copy of the f32.
- The write goes **directly to core.speed (+0xa4) and core.rot_speed (+0x94)**.

### FS5 Normal force and the contact list (priorities 2000 and 0)

#### FS5.1 `1001d610`: priority-2000 controller (fs+0x10)

```
fs = this.fs
if fs.n_contacts <= 1: recalc_contact(fs.first)          // [CP]'s 1001f860 (≥1 contact always exists)
                       return
1000b190(fs):                                            // per pair, reverse; per cp, reverse
    e = 0.0f
    for cp: g_old = cp.gap58; 1001f860(cp); e = (f32)((g_old - cp.gap58) * cp.N54 + e)
    if e > 0: P.anti_energy28 = (f32)(e + P.anti_energy28)
if sim_unit.flags & 0x3000: for every pair P: P.anti_energy28 = 0       // 1001ccb0
if (sim_unit.flags & 0xc00) == 0: for every pair P (reverse): ease_energy(P)   // 1001cc90 → 1001ccd0
```

With one contact there is no energy bookkeeping and no easing.

#### FS5.2 Contact setup at creation: `1001d3d0` (called by the creation path `10022180`)

```
mats = 10023fa0(cp)          // per synapse: ledge material index (1001d830: (*(geom&~0xf)>>24)&0x7f);
                             // 0 → obj.material(+0x98) else env.mapper(+0xac)->vfunc0(0, idx)
if mats[0]+8 != 0 or mats[1]+8 != 0: cp.byte34 = 1           // never in Ballance
c0 = obj0.core, c1 = obj1.core
if c0 fixed: cp.inv_vm30 = (f32)(1 / vmw(c1, tmp.p_cs1))
elif c1 fixed: cp.inv_vm30 = (f32)(1 / vmw(c0, tmp.p_cs0))
else: a = vmw(c0, tmp.p_cs0), b = vmw(c1, tmp.p_cs1) (f64); cp.inv_vm30 = (f32)(1 / ((b*a)/(b+a)))
```

`vmw(core, p)` is `1000c200`, the worst-case virtual mass. All products are f32 stores:

```
A = (p.y² + p.z²) * invI.x
B = (p.x² + p.z²) * invI.y
C = (p.x² + p.y²) * invI.z
return 1.0 / (max(A, B, C) + invm)
```

The max is taken in the order A, then B, then C.

#### FS5.3 `1001d6c0`: priority-0 controller (fs+8)

```
fs = this.fs
if fs.n_contacts <= 1: 1001d660(this, es) else 10036d70(fs, es)    // [LS]: multi-contact normal-force LCP
if fs.n_contacts == 0:
    this.fs = NULL; delete fs (vtbl+0x18, 1)
    sim_unit.flags = (sim_unit.flags & ~0x200) | 0x100
    return
if fs.uf_needed44:
    fs.uf_needed44 = 0
    c = 1001c7f0(fs)                                     // a root core of a disconnected component, or 0
    if c: 1001c8a0(fs, c); c.sim_unit.flags = (… & ~0x200) | 0x100
```

#### FS5.4 `1001d660` + `1001ae40`: single-contact normal impulse ("keep the gap at friction_dist")

```
cp = fs.first
1001ae40(cp, es, desired = friction_dist (0.02f), k = 1.0f)
if cp.gap58 >= 0.045f (max_distance_for_friction) or (tmp.flags5c & 0x300) == 0x100:
    remove_contact(fs, cp)                               // 1001c460
```

`1001ae40(cp, es, desired, k)`:

```
tmp = cp.tmp; vr = 0.0 (f64)
if c0 = tmp.core0: vr = (tmp.cr0·c0.rot_speed) + (c0.speed·n)          // ext; dot order: z,y,x terms then speed z,y,x
if c1 = tmp.core1: vr = vr - ((c1.rot_speed·tmp.cr1) + (c1.speed·n))
x = desired - cp.gap58                          // ext
if x < 0: k = k * 20.0                           // too far apart: pull 20× harder
imp = (k*x + vr) * tmp.virt_mass70              // ext → f64.  NO dt factor: x is used directly as m/s
if imp <= 0: cp.N54 = 0; return                  // no pull: contact only pushes
cp.N54 = (f32)(imp * es.i_dt)                    // pressure (force)
if c0: c0.rot_speed += (tmp.cr0 ⊙ c0.invI) * (-imp);  c0.speed += n * (-(c0.invm*imp))
if c1: c1.rot_speed += (tmp.cr1 ⊙ c1.invI) * (+imp);  c1.speed += n * (c1.invm*imp)
```

- Each component is stored f32. The product `cr ⊙ invI` is formed as f32 first.
- `vr > 0` means the bodies approach along n.
- The normal is taken to point from obj0 toward obj1. [CP] must agree.

#### FS5.5 Contact removal: `1001c460(fs, cp)` (`delete_friction_distance`)

1. Reset the freeze-check times of both cores (`1000cec0`).
2. Unlink cp from the fs list (`n_contacts--`).
3. Remove cp from its pair. If the pair dies, set `fs.uf_needed44 = 1`.
4. For each core k = 0, 1:
   1. Remove cp from `info = core_k.info(fs)`.
   2. If the info is now empty: delete it (`1000da80`), remove the core from fs (`1000b410`, which also detaches
      the 3 controllers), and set `core.sim_unit.flags = (& ~0x200) | 0x100`.
5. Run `1001c230(cp)` and free cp:
   1. Release the ledge references through `obj.surface_mgr(+0x8c)->vfunc+0x20(ledge)`.
   2. Fire **"friction contact deleted"**:
      - to the env global listeners that have bit 4 (`10013c00`, vfunc +0xc);
      - for each object with `obj+0x80 & 0x2000`, to its object listeners (`1000a900`).

      The event is `{env, …, cp, obj0, obj1, geom0, geom1}`. This feeds the glue's continuous-contact counters.
   3. Unlink both synapses from `obj+0x28`.

Other callers of `1001c460`:
- `1001cc20` ([IM]'s per-PSI impact pass): after `1001f860` and `10024040`, remove the cp if `tmp.flags & 0x300 == 0x100`.
- `1001d9a0`.
- `10009a40`.
- `100242c0`.
- `10036b80` ([LS]).

`1001d9a0`: `core_is_going_to_be_deleted(core)`, slot 0 of fs+8:
1. For every pair (reverse) that contains the core, remove every cp (reverse).
2. Then, if `n_contacts == 0`, delete fs.

### FS6 Energy bookkeeping and "easing"

#### FS6.1 Sources of `P.anti_energy28`

`P.anti_energy28` grows by:
- the positive per-PSI sum of `(gap_old − gap_new)·N` (§FS5.1), and
- the positive sum of friction-spring energy increases (§FS4.2).

#### FS6.2 `1001ccd0`: `ease_energy(P)`, every PSI while the unit is slow

```
e = core0(P+0x2c).env(+0xc).decay148 (f64) * P.anti_energy28     // ext
P.anti_energy28 = (f32)e
if e > 0:
    d = 1001d2a0(P, e)                       // ext e passed as f64 (not the f32-rounded value)
    env.destroyed70 += d                     // f64
    P.anti_energy28 = (f32)(P.anti_energy28 - d)
```

`1001d2a0(P, e)` → f64:

```
R = rel_energy_setup(P.c0, P.c1)    // 1001cdb0 into a 0xa0 stack struct, then 1001d060
if e > R.Etot98*0.10000000149011612: e = R.Etot98*0.10000000149011612   // remove at most 10%/PSI
if R.Etot98 < 1e-19: return 0.0
1001d0e0(R, e / R.Etot98); return e
```

**`1001cdb0`: setup of the relative-motion struct R.** All f64 unless noted.

1. **Choose the cores.** If `b` is fixed: `R.c0(+0x90) = b`, `R.c1(+0x94) = a`. Otherwise `c0 = a`, `c1 = b`. If
   either core is fixed, it ends up as c0.
2. **Linear part.** `R.dv(+0, f32×3) = c1.speed − c0.speed`, then `R.vlen30 = fast_normize(R.dv)`.
3. **Angular part.**
   - `w = Mv(c1, c1.rot_speed) − Mv(c0, c0.rot_speed)` (f32×3).
   - `R.wlen58 = fast_normize(w)`.
   - `R.ax0(+0x10) = MTv(c0, w)` and `R.ax1(+0x20) = MTv(c1, −w)`. The −w is formed in f32 with the −1.0 multiplies.
4. **Rotational masses.**
   - `R.Ir0_60 = len(c0.I ⊙ R.ax0)` (f32 products). If it is < 1e-19, `R.Ir0_60 = 1.0` and `R.iIr0_70 = 1.0`;
     otherwise `R.iIr0_70 = 1/R.Ir0_60`.
   - Likewise `R.Ir1_68 = len(R.ax1 ⊙ c1.I)` with `R.iIr1_78`.
5. **Masses.** `R.m1_40 = c1.mass` and `R.im1_50 = c1.inv_mass`.
   - If c0 is fixed: `R.m0_38 = R.m1_40·1e4`, `R.im0_48 = R.im1_50·1e-4`, `R.Ir0_60 = R.Ir1_68·1e4`,
     `R.iIr0_70 = R.iIr1_78·1e-4`.
   - Otherwise: `R.m0_38 = c0.mass`, `R.im0_48 = c0.inv_mass`.

**`1001d060`.**

- `R.Er80 = E(R.wlen58, R.Ir0_60, R.Ir1_68, R.iIr0_70, R.iIr1_78)`
- `R.El88 = E(R.vlen30, R.m0_38, R.m1_40, R.im0_48, R.im1_50)`
- `R.Etot98 = R.El88 + R.Er80`

**`E(v, m0, m1, im0, im1)` (`1001cd60`).** It is an ext expression. It is not the textbook ½v²/(im0+im1);
port it literally:

```
a = v/(im0+im1);  u = v - a*im1;  U = (f64)(u*u*m1);  w = a*im0
return ((v*v*m1 - w*w*m0) - U) * 0.5
```

**`1001d0e0(R, f)`: removing the energy.**

```
iR = J(R.wlen58, R.iIr0_70, R.iIr1_78, f*R.Er80)  → rounded f64
iL = J(R.vlen30, R.im0_48,  R.im1_50,  f*R.El88)  (ext)
where J(v, i0, i1, E) = (v - sqrt(|v*v - 2E*(i0+i1)|)) / (i0+i1)        // 1001cd20
if c0 movable:
    c0.speed_change(+0x84) += R.dv * (iL*R.im0_48)
    c0.rot_speed_change(+0x74) += R.ax0 * (iR*R.iIr0_70)
R.dv = -R.dv (f32 stores)
c1.speed_change += R.dv * (iL*R.im1_50)
c1.rot_speed_change += R.ax1 * (iR*R.iIr1_78)
```

The energy is removed through the **delayed velocity-change fields**, not through speed or rot_speed directly.

#### FS6.3 `1001c1f0` + `1001c070` + `1001bdf0` + `1001bd90`: easing the friction springs

`1001c1f0` runs at priority 600, after `1001bc20`, and only when there are ≥2 contacts:

```
for each pair P (reverse): if --P.next_ease == 0: ease_pair(P, env.mem_b0); P.next_ease = 5
```

`ease_pair` (`1001c070`): with n = P.n, it copies the cps into scratch memory (`10020210` on env+0xb0) and zeroes
the deltas `d[n]` (f32×3):

```
f = 1.0 / (n + 1e-19)            (f64)
for i in 0..n-2: for j in i+1..n-1:
    if | |n_i · n_j| - 1 | < 0.0010000000474974513:      // nearly parallel normals (tmp+0), ext
        1001bdf0(cp_j, cp_i, &d[j], &d[i], f)
for k: cp_k.s0 += span_v0_k · d[k];  cp_k.s1 += span_v1_k · d[k]       // 1001bd90, f32
```

`1001bdf0(A, B, dA, dB, f)` takes A = cp_j and B = cp_i. All values are f32 stores, and each scale by f is a
`(float)f` multiply:

```
sg = +1 if A.obj0.core == B.obj0.core else -1
SA = A.s0*A.span0 + A.s1*A.span1;  SB = B.s0*B.span0 + B.s1*B.span1      (world)
u  = normize((f32)(A.tmp.cp_ws - B.tmp.cp_ws))
pA = u*(u·SA);  pB = u*((u·SB)*sg);  avg = (pA+pB)*0.5
dA += (avg - pA)*f;   dB += (avg - pB)*(sg*f)
```

Contacts of the same pair that share a plane get the components of their springs along the line joining them
equalized. This removes the tension between contacts (seams, for example).

### FS7 Merging and splitting systems

#### FS7.1 `1001c570(dst, src)`: merge src into dst (called by the creation path `10022180`)

1. **Move the contacts.** For each cp in src's list (head first): unlink it from src, remove it from its src pair,
   link it into dst, and add it to the dst pair (creating the pair if needed).
2. **Move the cores.** For each core in `src.cores` (reverse), let `fi_s = info(core, src)` and
   `fi_d = info(core, dst)`:
   - If `fi_d` is missing: move `fi_s` to dst (re-key it in the core, `fi_s.fs = dst`), remove the core from src,
     and add it to dst (the 3 controllers move with it).
   - Otherwise: append every cp of `fi_s` to `fi_d`, remove them from `fi_s`, and delete `fi_s`.
3. Delete src.

#### FS7.2 `1001c7f0`: union-find over pairs (no path compression, no rank)

```
for every core in fs.cores: core.uf228 = 0
for every pair (reverse) with both cores movable: r0=root(c0), r1=root(c1); if r0!=r1: r1.uf = r0
ref = root of the movable core with the LOWEST index in fs.cores
ret = 0; for movable cores (reverse): if root(core) != ref: ret = root(core)     // last hit = lowest index
return ret
```

`root` is `1001c7d0`, which follows +0x228 until it reaches NULL. **Fixed cores never connect components.**

#### FS7.3 `1001c8a0(fs, root)`: split off a component

This loops until no component is left over:

1. **Create the new system.** `new = new IVP_Friction_System(fs.env)`.
2. **Move the cores.** For each core in `fs.cores` (reverse):
   - Movable with `root(core) == root`: remove it from fs, add it to new, and set `info.fs = new`.
   - Fixed: give it a fresh empty info (0xc bytes) for new, and add it to new.
3. **Move the pairs.** For each pair (reverse), use its movable core for the test, and if a core is fixed, look up
   that core's `fi_old` (fs) and `fi_new` (new). If the root matches:
   1. Move the pair to new.
   2. Move every cp from the fs list to the new list. If a cp is not in fs's list, the code writes `*0 = 0`
      (deliberate crash).
   3. Move cp from `fi_old` to `fi_new`.
4. **Drop empty infos.** For each fixed core of fs: delete an empty `fi_new` and remove the core from new; delete an
   empty `fi_old` and remove the core from fs.
5. **Tidy up and repeat.**
   - If `new.n_cores (+0x3c) < 2`: drop the info of `new.cores[0]`, delete new, and return.
   - If `fs.n_cores < 2`: drop the info of `fs.cores[0]`, delete fs, and return.
   - Otherwise `root = 1001c7f0(fs)`. If it is 0, return; else loop.

#### FS7.4 `1001d4d0(core)`: on core revive (from `1000aea0`), create contacts with sleeping neighbours

For each object of the core, walk its mindist synapse list (obj+0x20, mindist = synapse + synapse.s16@+0x18):

- Skip a mindist with `md+0x14 & 0x3000` set.
- Let `o` be the other core.
- If `o` is movable, and has no friction info, and after recalc (`10019950`) `md+0x14 & 0xc000 == 0`, and
  `md.len(+0x54) < 0.045`:
  1. Within a short-term memory scope (env+0xb4 counter +0x10), call `10022180(md, &x, &flag, core.sim_unit, 1)`.
  2. If `flag == 1`, reset `o`'s freeze check and return 1.
- Otherwise, if `o` is fixed and this core is fixed, delete the mindist (vfunc +0x10).

### FS8 Paths that are unreachable in Ballance (documented so a port can drop them)

- **Two friction values** (`cp.byte34 == 1`, only if a material has +8≠0; `1000bf00` always sets 0):
  - `1001b8b0`: a 1D impulse along the spring direction from `1001ad90`, limited to `|imp| ≤ μN·dt` (the sign is
    lost when clamping).
  - `1001ad90`: `S = s0·span0 + s1·span1`, fast_normize. If byte34, calls `1001aa20`.
  - `1001aa20`: a static/dynamic friction cone that uses material slot 1 (`+0x18`, the uninitialized "second
    friction") against the other material's friction.

  All three are dead.
- **Car wheel branch** of `1001b080`, taken when `core0.car_wheel (+0x10)` ≠ NULL and core1 == NULL:
  - It uses the wheel's axis (`wheel+4`, `+0x80`, the doubles at +8/+0x28/+0x48 indexed by the axis), checks a
    0.001 cross threshold and a 0.3/0.9 longitudinal factor.
  - It writes wheel+0x88..0x90 (contact point), +0xa0 (time) and +0x98 (slip).

  Unreachable: no car constraint exists.

### FS9 Other functions in `0x1001a000-0x1001a8b0` (not friction; for the collision and core parts)

| Address | Function |
|---|---|
| `1001a190` | get or create the object's compact-ledge cache entry (obj+0x40, refcount +4) |
| `1001a220`, `1001a370` | mindist recalc cases (ball vs ledge, ball vs ball: normal at md+0x68, len = \|d\| − md+0x50 at md+0x54, md+0x58 relative speed) |
| `1001a5c0` | triangle walk |
| `1001a6b0` | fills the mindist dispatch table `0x10075ee0` |
| `1001a740`, `1001a7a0`, `1001a7e0` | hull-manager ctor, dtor and clear |
| `1001a820` | hull-manager time rebase |

`1001a3c0` and `1001be40` are false function starts inside `1001a370` and `1001bdf0`.

## 5. Friction linear solver [LS]

The multi-contact normal-force solve of one friction system, and the core-reaction helper used by the tangential friction. Core, cp and tmp fields are in §2.

Scope: the per-PSI normal-force solve of one friction system ("IVP_Friction_Solver"), the dense
matrix helpers ("IVP_Great_Matrix_Many_Zero"), the LCP pivoting solver ("IVP_Linear_Constraint_Solver",
250-step limit), its incremental LU ("IVP_Incr_L_U_Matrix"), and the small 1..3-direction
"core reaction" helper used by the tangential friction code at 0x1001b080 ("IVP_Solver_Core_Reaction").
Names in quotes are the matching Ipion/Valve-IVP class names, inferred from structure; the binary has no symbols.

Conventions:
- `f32` = float, `f64` = double. All arithmetic runs on x87 with a 64-bit mantissa (the env loop
  sets the control word). Intermediates are therefore 80-bit until stored; "stored as f32/f64" marks
  every rounding point that matters. A port using SSE f64 for all intermediates is fine (not bit exact anyway).
- `dot(a,b)` of two f32x3 is computed as `(a.z*b.z + a.y*b.y) + a.x*b.x` (0x1001b060), unrounded.
- `TEST_EPS = 0x3e7ad7f2a0000000` = 1.0000000116860974e-7 (the float 1e-7 widened to double).
- Constants: `0x10063288` f64 1.0, `0x10063388` f64 20.0, `0x10063488` f64 0.0, `0x10063480` f64 1e-19,
  `0x10063230` f64 -1.0, `0x10063af8` f64 9.999999747378752e-6 (float 1e-5), `0x10063b00` f64 1000.0,
  `0x10063b08` f32 0.01, `0x10063500` f64 0.10000000149011612 (float 0.1), `0x10063b1c` f32 9.81,
  `0x10063b18` f32 -1.0, `0x10063870` f64 10.0, `0x100631d0` f64 0.5, `1e101` = 0x54e6dc186ef9f45c.

### LS1 Solver objects

#### LS1.5 IVP_Great_Matrix_Many_Zero (dense square system, 0x20 bytes; ctor 0x10034100)

| Off | Type | Meaning |
|---|---|---|
| +0x00 | f64 | MATRIX_EPS, ctor sets 1e-9 (0x3e112e0be826d695) |
| +0x08 | i32 | columns n |
| +0x0c | i32 | aligned_row_len (row stride, = n everywhere here) |
| +0x10 | f64* | matrix_values, row-major `A[row*stride + col]` |
| +0x14 | f64* | desired_vector b (overwritten by the solve) |
| +0x18 | f64* | result_vector x |

0x10033e70 masks +0x10 with ~7 (a no-op alignment). Memory always comes from the environment's
short-term pool `env+0xb0` (bump allocator: `+8` cursor, `+0xc` limit, 32-byte aligned, `0x10020210`
is the slow path, `+0x10` u16 transaction depth, `0x100201b0` ends a transaction and frees).

#### LS1.6 IVP_Friction_Solver (stack object in 0x10036d70; ctor 0x1001c350)

| Off | Type | Meaning |
|---|---|---|
| +0x00 | Great_Matrix (0x20) | the n×n system: values = A, desired = b, result = x |
| +0x20 | f64 | unscale factor (set by 0x10036190) |
| +0x28 | env* | `fs+4` |
| +0x2c | IVP_Event_Sim* | es: `+0` f64 delta_time (= env+0xc0 = 1/66), `+8` f64 i_delta_time (= env+0xc8 = 66), `+0x10` env |
| +0x30 | u16 | vector capacity (init 0x200) |
| +0x32 | u16 | vector count = n contacts |
| +0x34 | `tmp_contact_info**` | elements (points at +0x38 unless grown; grown storage freed by `free`) |
| +0x38 | 0x800 bytes | inline storage for 512 pointers |

Ctor: `n = fs.n_dists(+0x3e) − fs.(+0x40)` (in practice +0x40 is always 0 here, see §LS2.2); cols = stride = n;
allocates A (n·n·8), b (n·8), x (n·8) from `env+0xb0` in that order.

#### LS1.7 IVP_Linear_Constraint_Solver (stack object, 0x130 bytes, in 0x100362e0)

| Off | Type | Meaning |
|---|---|---|
| +0x00 | f64 | TEST_EPS = 1e-7 (float-widened) |
| +0x08 | f64 | GAUSS_EPS = 1e-7 |
| +0x10 | f64 | MAX_ERROR = TEST_EPS·1000 ≈ 1e-4 (final consistency test) |
| +0x18 | f64 | MAX_STEP = 10000.0 (0x40c38800_00000000) |
| +0x20 | f64* | full_A (n×n, stride +0x50) |
| +0x24 | f64* | full_b |
| +0x28 | f64* | test vector (= +0xa0), Ax−b scratch |
| +0x2c | f64* | full_x (output; points to caller's x) |
| +0x30 | f64* | delta_f |
| +0x34 | f64* | accel (= A·x − b, maintained incrementally) |
| +0x38 | f64* | delta_accel |
| +0x3c | f64* | reset_x (scratch) |
| +0x40 | f64* | reset_accel (scratch) |
| +0x44 | i32* | actives_inactives_ignored: permutation of variable indices. Positions `[0, r_actives)` = active, `[r_actives, ignored_pos)` = inactive (clamped), `[ignored_pos, n)` = not yet treated |
| +0x48 | i32* | variable_is_found_at (inverse permutation) |
| +0x4c | i32 | n_variables |
| +0x50 | i32 | aligned_size (= n) |
| +0x54 | i32 | r_actives |
| +0x58 | i32 | initial n_first (copy, unused afterwards) |
| +0x5c | i32 | ignored_pos |
| +0x60 | i32 | 0 (unused) |
| +0x64 | i32 | counter of Gauss-fallback solves (statistics only) |
| +0x68,+0x6c | i32 | first/second permute index (actives) |
| +0x70,+0x74 | i32 | first/second permute index (ignored) |
| +0x78 | i32 | sub_solver_status: 0 = incremental LU valid, 1 = LU invalid (use Gauss), 2 = LU failed |
| +0x80 | Incr_L_U_Matrix (§LS1.8) | inv_mat |
| +0xb0 | Great_Matrix | Gauss fallback matrix (eps = GAUSS_EPS; values +0xc0 n·n, desired +0xc4, result +0xc8) |
| +0xd0 | Great_Matrix | "full" matrix wrapper: eps = TEST_EPS, cols = stride = n, values +0xe0 = full_A, desired +0xe4 = delta_f, result +0xe8 = delta_accel |
| +0x110 | Great_Matrix | allocated (+0x120/+0x124/+0x128), eps = TEST_EPS, never used (debug) |

All vectors are allocated from the short-term pool by 0x10034420 (`+0x44,+0x48` ints, then
`+0x34,+0x38,+0x30,+0x3c,+0x40` doubles, LU `+0x88,+0x8c` n·n, `+0x98,+0x9c,+0xa0` n, then
`+0xa4` n, `+0xc0` n·n, `+0xc4,+0xc8` n, `+0x120` n·n, `+0x124,+0x128` n via 0x1000d110). Pointers
in +0xc0..+0xc8, +0xe0..+0xe8 and +0x120..+0x128 are cleared on exit.

#### LS1.8 IVP_Incr_L_U_Matrix (at lcs+0x80)

Maintains `L·A_act = U`, where A_act is the active sub-matrix in active-order, L is a full n_sub×n_sub
matrix (it is really L⁻¹·P), U is upper triangular with **unit diagonal**.

| Off (lcs) | Off (lu) | Type | Meaning |
|---|---|---|---|
| +0x80 | +0x00 | f64 | eps = TEST_EPS·10 ≈ 1e-6 |
| +0x88 | +0x08 | f64* | L (stride +0x28) |
| +0x8c | +0x0c | f64* | U (stride +0x28) |
| +0x98 | +0x18 | f64* | input_vec |
| +0x9c | +0x1c | f64* | out_vec |
| +0xa0 | +0x20 | f64* | tmp_vec |
| +0xa4 | +0x24 | f64* | mult_vec |
| +0xa8 | +0x28 | i32 | aligned_row_len (= n) |
| +0xac | +0x2c | i32 | n_sub (current size) |

#### LS1.9 IVP_Solver_Core_Reaction (used by 0x1001b080; size ≥ 0x144)

| Off | Type | Meaning |
|---|---|---|
| +0x00,+0x04,+0x08 | f32x3* | dir0, dir1 (or NULL), dir2 (or NULL) — world directions |
| +0x0c,+0x1c,+0x2c | f32x4 | cr_out core0 for dir0..2: (c_cs.x, c_cs.y, c_cs.z, 1.0), c = r × dir in core space |
| +0x3c,+0x4c,+0x5c | f32x4 | cr_out core1 |
| +0x6c | ptr | `&core.m_world_f_core` of the last core processed |
| +0x74,+0x84,+0x94 | f32x4 | cr_mult core0: (c_cs ⊙ inv_I, inv_mass) |
| +0xa4,+0xb4,+0xc4 | f32x4 | cr_mult core1 |
| +0xd8 | f64[3][4] | response matrix m (rows +0xd8, +0xf8, +0x118); only m00 +0xd8, m01 +0xe0, m02 +0xe8, m11 +0x100, m12 +0x108, m22 +0x128 are filled |

### LS2 Entry: IVP_Friction_System::do_friction_system — 0x10036d70

Called by the friction system's PSI handler 0x1001d6c0 when `fs.n_dists (+0x3e) >= 2` ([FS]; with < 2
it uses the pairwise path 0x1001d660). Args: `this = fs`, `es`.

```
fs_solve(fs, es):
  mem = fs.env(+4)->short_term(+0xb0); mem.depth(+0x10)++      // begin transaction
  g_solver_calls(0x10075f20)++                                  // stat
  bubble_sort_dists_importance(fs)          // 0x10036c60
  clear_far_dists(fs)                        // 0x10036b80  (also sets fs+0x40 = 0)
  if fs.n_dists - fs.(+0x40) > 150:          // 0x96
      too_many_contacts(fs)                  // 0x10036d10
  else:
      FrictionSolver S(fs, es)               // 0x1001c350 (allocates A, b, x)
      S.setup_coords(fs)                     // 0x100366a0
      idx = mem.alloc(4 * fs.n_dists)        // i32 list of warm-start indices
      k = S.build_matrix(fs, idx)            // 0x10036760
      S.solve_and_push(fs, idx, k, mem)      // 0x100362e0
      free S's grown vector storage if any
  mem.depth--; mem.end_transaction()         // 0x100201b0
```

#### LS2.1 bubble_sort_dists_importance — 0x10036c60 (+ swap 0x10036c20)

Sorts the fs list (head `fs+0x20`, links `cp+0/+4`) ascending by `cp.has_negative_pull_since (+0x64)`,
signed i32. Algorithm (insertion sort on a linked list): walk `a = head`; while `b = a.next`:
if `a.key > b.key`, swap a and b (0x10036c20 swaps two **adjacent** nodes, fixing head, prev/next),
then keep swapping b backwards while `b.prev && b.prev.key > b.key`; continue from the node that is now
after the original position. Result: contacts that pushed last PSI (key < 0, most negative = longest
pushing) come first, then 0, then positive.

#### LS2.2 clear_far_dists — 0x10036b80

```
for cp in fs list (next fetched before processing):
    if cp.gap(+0x58) >= max_distance_for_friction (0.045)          // f32 compare
       or (cp.info(+0x40).flags(+0x5c) & 0x300) == 0x100:
        fs.remove_contact_point(cp)                               // 0x1001c460 ([FS])
    elif cp.gap > distance_keepers_safety + friction_dist (0.0001 + 0.02, f32 sum)
         and (obj0.original_core(+0xa8).flags & obj1.original_core.flags & 3) != 0:
        unlink(fs, cp)       // 0x1000b290: list remove, fs.n_dists--
        push_front(fs, cp)   // 0x1000b360: cp.fs = fs, insert at head, fs.n_dists++
fs.(+0x40) = 0  (i16)
```
Quirk: the move-to-front happens **after** sorting, so far contacts of resting pairs precede the
negative-key ones. `fs+0x40` is zeroed here, so every later "first fs+0x40 contacts are excluded"
branch (0x100366a0, 0x100364c0) is dead in practice; keep it as a no-op.

#### LS2.3 too_many_contacts — 0x10036d10 (+ 0x10036cb0)

```
for core in fs.moveable_cores (+0x2c vector: u16 count +0x2e, ptr +0x30), from last to first:
    core.flags = (core.flags & ~2) | 1
    // 0x10036cb0: count pairs in fs.pairs (+0x34 vector: count +0x36, ptr +0x38) where
    // (pair.core0(+0x2c) == core or pair.core1(+0x30) == core) and
    // ((pair.core0.flags | pair.core1.flags) & 0xc) == 0
    if that count > 1:
        core.speed(+0xa4..+0xac) = 0; core.rot_speed(+0x94..+0x9c) = 0
```
No forces are applied that PSI. Not reached in Ballance (≤ a few dozen contacts).

#### LS2.4 setup_coords — 0x100366a0

```
skipped = 0; idx = 0
for cp in fs list:
    info = cp.info(+0x40)
    if skipped < fs.(+0x40):            // dead (fs+0x40 == 0); note: 'skipped' never increments here
        info.index(+0x58) = -1
        continue
    skipped++
    info.fi0(+0x60) = get_friction_info(obj0.core, fs)   // 0x1000d960
    info.fi1(+0x64) = get_friction_info(obj1.core, fs)
    info.gap(+0x6c) = cp.gap(+0x58)
    info.key(+0x68) = cp.has_negative_pull_since(+0x64)
    S.infos.push(info)          // grows via 0x1000b600
    info.index = idx++          // i16
```

#### LS2.5 build_matrix — 0x10036760 (returns k = number of warm-start indices written to idx)

Rows/columns: **one variable per contact point** = the normal impulse magnitude (friction is not in this
matrix; tangential friction is handled separately by 0x1001b080 with the core-reaction helper).

```
zero A[0 .. n*stride-1]
k = 0
for i in 0..n-1:
    t = S.infos[i]
    // relative normal velocity (f64 accumulator; products in x87)
    vrel = 0.0
    if t.core0: vrel  = dot(core0.rot_speed(+0x94), t.cross_cs[0](+0xc0)) + dot(core0.speed(+0xa4), t.n)
    if t.core1: vrel -= dot(core1.rot_speed, t.cross_cs[1](+0xd0)) + dot(core1.speed, t.n)
    d = friction_dist(0.02 f32) - t.gap(+0x6c)          // x87 extended
    factor = (d < 0.0) ? 20.0 : 1.0                     // 0x10063388 / 0x10063288
    b[i] = (f64)(factor * d + vrel)
    if t.key(+0x68) != 0: idx[k++] = i                  // warm-start guess: pushed or "negative" last PSI
    // column i of A: velocity change of every contact j caused by unit impulse i
    if t.core0 (c = core0):
        a = -(t.cross_cs[0] ⊙ c.inv_I(+0x34..+0x3c))    // 0x1001d020 then negate 0x10036b00 (f32x3)
        bb = t.n * (f32)(c.inv_mass(+0x40) * -1.0f)    // 0x10036ac0, f32x3
        for cpj in t.fi0.elements[0 .. t.fi0.n-1]:
            j = cpj.info.index; if j < 0: continue
            tj = S.infos[j]
            if tj.core0 == c: s = -1.0, kk = 0  else: s = +1.0, kk = 1
            if tj.core[kk] == NULL: continue          // (cannot happen for the matching core)
            A[j*stride + i] += (dot(a, tj.cross_cs[kk]) + dot(bb, tj.n)) * s    // f64 store
    if t.core1 (c = core1):
        a = t.cross_cs[1] ⊙ c.inv_I            // not negated
        bb = t.n * c.inv_mass                  // f32 multiply (0x10036ac0 with f64 arg narrowed)
        for cpj in t.fi1.elements:
            j = cpj.info.index; if j < 0: continue
            tj = S.infos[j]
            if tj.core0 == c: s = -1.0, kk = 0  else: s = +1.0, kk = 1
            if tj.core[kk] == NULL: continue
            A[j*stride + i] += (dot(tj.n, bb) + dot(tj.cross_cs[kk], a)) * s   // 0x10036b30
// debug check: walking fs list, every info.index >= 0 must equal its position; else write to [0] (crash)
return k
```
Resulting meaning: `A[j][i]` = change of contact j's approach speed `(v0−v1)·n_j` per unit impulse i
(positive diagonal). `b[i]` = current approach speed of contact i plus a gap-restoring term. Sign:
contact i separates when `(v0 + ω0×r0 − v1 − ω1×r1)·n < 0`. Impulse x pushes core0 by −n·x,
core1 by +n·x, reducing the approach speed by `A x`. The LCP solved is

    find x ≥ 0 with w = A·x − b ≥ 0 and x_i·w_i = 0.

Gap term: `factor·(0.02 − gap)` [m, used as m/s]. Penetrating below 0.02 m adds a small push
(factor 1); farther than 0.02 m reduces the needed push 20× harder (factor 20), so a contact up to the
removal limit 0.045 m only pushes if it is approaching faster than 20·(gap−0.02).

#### LS2.6 solve_and_push — 0x100362e0

```
solve_and_push(S, fs, idx, k, mem):
  M2 = Great_Matrix(); M2.n = M2.stride = k
  S.scale()                                       // 0x10036190
  M2.desired = alloc(k*8); M2.result = alloc(k*8); M2.values = alloc(k*k*8)   // in this order
  zero S.x[0..n-1]
  M2.gather(S, idx, k)                            // 0x10034120: M2.A = A[idx][idx], M2.b = b[idx]
  ok = M2.gauss_solve()                           // 0x10033e50, k==0 → returns 1
  if not ok or not S.check_guess(M2.result, idx, k, mem):     // 0x10036080
      nfirst = count of leading list cps with has_negative_pull_since < 0   // 0x100362c0
      LCS lcs   // 0x130-byte stack object
      if lcs.init_and_solve(S.A, S.b, S.x, n, nfirst, mem) != 1:            // 0x100346e0
          return          // FAILURE: no impulses, cp keys/pressure untouched this PSI
  S.x *= S.scale_factor                          // 0x10036170, all n entries
  E0 = Σ_{core in fs.cores(+0x24 vector: count +0x26, ptr +0x28)} KE(core, v+Δv, ω+Δω)   // 0x10036ec0
  S.do_resulting_pushes(fs)                      // 0x100364c0
  E1 = same sum                                  // 0x10036ec0
  tol = Σ_{core in fs.cores, (flags & 0xc)==0} core.mass * 9.81f * 0.1    // 0x10036fc0 (f32 9.81, f64 0.1)
  if E1 > E0 + tol:  for core in fs.moveable_cores: abort_async_push(core)  // 0x10036fa0 → 0x1000c5f0
  else:              for core in fs.moveable_cores: commit_async_push(core) // 0x10036f80 → 0x1000cbd0
```
Quirks:
- The energy test compares `(tol + E0) < E1` (x87). Energies include any Δv/Δω already pending on the
  cores before the solve (from earlier controllers this PSI), and abort/commit affect those too:
  **abort discards all pending async pushes of the system's movable cores, not only the friction ones.**
  (Port: check what is pending at this point; force controllers 0x1000c830 push into the same Δ fields.)
- `tol` uses `|g| = 9.81` hard-coded, not the game's 20, so ≈ 0.981·m J of slack per PSI.

#### LS2.7 scale — 0x10036190

```
maxd = 0.0; for i = n-1..0: if A[i][i] > maxd: maxd = A[i][i]      // signed, strict >
sA = (maxd > 1e-19) ? 1.0/maxd : 1.0
maxb = 0.0; for i = n-1..0: if |b[i]| > maxb: maxb = |b[i]|
if maxb > 1e-19: sb = 1.0/maxb;  S.scale_factor(+0x20) = sA * maxb
else:            sb = 1.0;       S.scale_factor = sA * 1.0
A *= sA (all n rows × stride cols); b *= sb
```
(Then x_true = x_scaled · sA·maxb, applied by 0x10036170.)

#### LS2.8 check_guess — 0x10036080

```
flag = mem.alloc(4*n), zeroed
bad = false
for m in 0..k-1:
    i = idx[m]; xm = M2.result[m]
    if t_i.inv_virt_mass(+0x74, f32) * S.scale_factor * xm  <  env.gravity_len(+0xf0, f32) * 0.01f:
        bad = true
    flag[i] = 1; S.x[i] = xm                       // still scaled
for i in 0..n-1:
    if flag[i] == 0 and not S.row_ok(i): return false     // 0x10033d80
return not bad
```
`env+0xf0` = |gravity| (set by set_gravity 0x10013680; 20 in Ballance → threshold 0.2 m/s).
`row_ok(i)` (0x10033d80): `s = Σ_c A[i][c]·x[c]` (c ascending, f64); return `|b_i · 1e-5| + s >= b_i`.

So the warm start accepts the guessed active set only if every guessed contact gets a velocity change of at
least 0.01·|g| from its own impulse and every non-guessed contact is not penetrating (w ≥ −1e-5·|b|).

#### LS2.9 do_resulting_pushes — 0x100364c0 (returns 0)

```
i = 0
for cp in fs list:
    if i < fs.(+0x40):  p = 0.0                     // dead branch
    else:
        x = S.x[i] (f64)
        if x > 0:
            cp.key = (cp.key >= 0) ? -1 : cp.key - 1
        elif x == 0:
            cp.key = 0; p = 0.0; goto store
        else:  // x < 0 (numerical)
            if cp.key < 0: cp.key = 0
            cp.key += 1; if cp.key > 9: cp.key = 0
        t = cp.info
        if t.core0 (c):
            c.Δω(+0x74) = c.Δω + (t.cross_cs[0] ⊙ c.inv_I)·(−x)        // f32 products, f64 factor
            c.Δv(+0x84) = c.Δv + t.n · (−(c.inv_mass · x))
        if t.core1 (c):
            c.Δω += (t.cross_cs[1] ⊙ c.inv_I) · x
            c.Δv += t.n · (c.inv_mass · x)
        p = (x < 0) ? 0.0 : x
    store:
    cp.now_friction_pressure(+0x54) = (f32)(p * es.i_delta_time(+8))   // impulse/PSI → force
    i++
```
Quirk: a negative x (only possible from round-off in the direct path) **is** applied as a pull,
but reported as pressure 0. Keys: negative = consecutive PSIs with push (−1, −2, …); positive 1..9 cycles
while x < 0; 0 when x == 0.

### LS3 Dense matrix helpers (IVP_Great_Matrix_Many_Zero)

#### LS3.1 gather — 0x10034120 (this = small matrix, args: big, idx, k)
If k == 0: nothing. If `idx[k−1] == k−1` (indices are the prefix 0..k−1; idx is ascending): copy rows
`idx[r]` of big, first `this.stride` columns, and `b[idx[r]`. Otherwise: `this.A[r][c] = big.A[idx[r][idx[c]`,
`this.b[r] = big.b[idx[r]`.

#### LS3.2 gauss_solve — 0x10033e50 = eliminate 0x10033f80 + back_substitute 0x10034030 (returns 1 ok / 0 fail)

```
eliminate():
  for p in 0..n-1:
     pivot_search(p)                        // 0x10033e80
     d = A[p][p]
     if |d| < eps: continue                 // eps = this+0 (1e-9 for friction matrices, 1e-7 for LCS fallback)
     inv = -1.0 / d
     for r in p+1..n-1:
        f = A[r][p]
        if |f| > eps: add_row(p, r, f*inv)  // 0x10033de0: A[r][c] += s·A[p][c] for c = p..n-1; b[r] += s·b[p]
pivot_search(p):                            // 0x10033e80
  best = -1; m = |A[p][p]|
  for r = n-1 down to p+1: if |A[r][p]| > m: m = |A[r][p]|, best = r
  if best >= 0: swap_rows(p, best)          // 0x10033f00: whole row (n entries) and b
back_substitute():                          // 0x10034030
  for i = n-1 down to 0:
     s = b[i]; for c = n-1 down to i+1: s -= A[i][c] * b[c]     // b holds solved values
     d = A[i][i]
     if |d| < eps:
        if |s| < eps*1000: b[i] = 0; continue
        x[0..n-1] = 0; return 0
     b[i] = s / d
  x[i] = b[i] for all i; return 1
```

#### LS3.3 Others
- 0x10034270 `mult()`: `result[r] = Σ_c A[r][c]·desired[c]` (c ascending).
- 0x10033d80 `row_ok(i)`: see §LS2.8.
- 0x10033de0 `add_row(p, r, s)`: see §LS3.2.
- 0x10036030 (on an LU object) `L[r][c] -= f·L[p][c]` for c in 0..n_sub−1 (args p, r, f).

### LS4 LCP solver (IVP_Linear_Constraint_Solver)

#### LS4.1 init_and_solve — 0x100346e0 (A, b, x, n, nfirst, mem) → 1 ok / 0 fail

```
n_variables = aligned = n; allocate vectors (0x10034420)
r_actives = ignored_pos = 0
TEST_EPS = GAUSS_EPS = 1e-7; MAX_ERROR = TEST_EPS*1000; MAX_STEP = 10000.0
status = 0; lu.aligned = n; lu.n_sub = 0; lu.eps = TEST_EPS*10
gauss(+0xb0).eps = GAUSS_EPS; full(+0xd0).eps = TEST_EPS; (+0x110).eps = TEST_EPS
full.values = A; full.desired = delta_f; full.result = delta_accel; full.n = full.stride = n
for i in 0..n-1: aii[i] = i; found[i] = i; x[i] = 0; accel[i] = -b[i]
full_A = A; full_b = b; full_x = x; permute counters (+0x60..+0x74) = 0
start_initialize(nfirst)        // 0x10035380
r = solve_lc()                  // 0x10034ab0
clear scratch pointers; return r
```
Variable meaning in this solver: `accel_i = (A·x − b)_i` = post-impulse separating speed of contact i.
Active: x_i > 0 and accel_i = 0. Inactive: x_i = 0, accel_i ≥ 0.

#### LS4.2 start_initialize — 0x10035380 (warm start with the first nfirst variables active)

```
r_actives = (+0x58) = ignored_pos = lu.n_sub = nfirst
reset_x[0..aligned-1] = 0
if setup_l_u_solver() == 1:                 // 0x100351c0 (full LU of A_act, lu.input = b_act)
    status = 0
    loop:
        lu.solve()                           // 0x10035ff0: lu.out = U⁻¹·L·lu.input
        for m < r_actives: reset_x[aii[m] = lu.out[m]
        recompute_x_accel()                  // 0x100352c0
        find the highest position m < r_actives with x[aii[m] < 0.0; if none: return
        v = aii[m]; x[v] = 0; reset_x[v] = 0
        r_actives--; ignored_pos--
        swap_pos(r_actives, m)               // 0x10034f10; v becomes the first not-yet-treated variable
        lu_remove(m)                         // 0x10034870
        if status > 0: break
        for m < r_actives: lu.input[m] = b[aii[m]
    accel[0..n-1] = 0; x[0..n-1] = 0
r_actives = ignored_pos = lu.n_sub = 0        // reached on LU failure or the break
```
`recompute_x_accel` (0x100352c0): `reset_accel = A·reset_x` (via the full matrix wrapper, 0x10034270),
`reset_accel[i] -= b[i]` for all i, `reset_accel[aii[m] = 0` for active m, then for i = n−1..0:
`accel[i] = reset_accel[i]; x[i] = reset_x[i]`.

Note: in this warm start nfirst counts the leading **list** contacts with key < 0. Because the solver
variable index equals list position, these are variables 0..nfirst−1, exactly the initial
`aii` prefix.

#### LS4.3 solve_lc — 0x10034ab0 (main pivoting loop; returns 1 ok / 0 fail)

Locals: `steps = 0`, `zero_steps = 0`, `did_real_step = 0`, `full_test_countdown = 7`, `eps = TEST_EPS`.

```
loop:                                            // L_TOP 0x10034ae2
  if did_real_step: steps++
  if steps > 250: return 0
  if full_test_countdown == 0:
      if not full_test(): goto L_RESTART         // 0x100343a0
      full_test_countdown = 7
  else:
      full_test_countdown--
L_MAIN:                                          // 0x10034b20
  if ignored_pos >= n: return 1
  ig = aii[ignored_pos]
  if status > 0:
      if status == 1 and did_real_step:
          little_random_permutation()            // 0x10034990
          if setup_l_u_solver() == 1: status = 0 // 0x100351c0
      else:
          status = 1
  if |accel[ig]| < eps:
      if |x[ig]| < eps: goto L_TO_INACTIVE
      jpos = ignored_pos; goto L_IG_TO_ACTIVE
  if accel[ig] >= 0: goto L_TO_INACTIVE
  // accel[ig] < 0: contact ig still approaching → increase x[ig]
  if get_fx_dx() == 0: goto L_RESTART            // 0x10034ff0: delta_f for actives, delta_f[ig]=1
  delta_f[ig] = 1.0
  compute_delta_accel()                          // 0x100342c0, for positions r_actives..n-1
  for m < r_actives: delta_accel[aii[m] = 0
  // ratio test
  if MAX_STEP * delta_accel[ig] <= -accel[ig]:   jpos = -1; step = 1e101
  else:                                          jpos = ignored_pos; step = -accel[ig] / delta_accel[ig]
  for m in 0..r_actives-1:                       // actives that would drop below 0
      v = aii[m]; df = delta_f[v]
      if df < -eps:
          t = -(x[v] / df)
          if |t| < eps and x[v] < eps: jpos = m; step = t; goto L_STEP_CHECK   // degenerate: take it now
          if t < step + eps: step = t; jpos = m
  for m in r_actives..ignored_pos-1:             // inactives whose accel would hit 0
      v = aii[m]; da = delta_accel[v]
      if da < -TEST_EPS:
          t = -(accel[v] / da)
          if t < step - eps: step = t; jpos = m
  if step < 0: step = 0
  if jpos < 0: return 0                           // unbounded
L_STEP_CHECK:                                     // 0x10034d36
  if step > MAX_STEP: goto L_RESTART
  if step < TEST_EPS:
      zero_steps++
      if zero_steps > (n >> 1) + 2: goto L_RESTART
  else:
      zero_steps = 0
  did_real_step = 1
  update_step(step)                              // 0x10034f40
  if jpos < r_actives:                           // an active hits x = 0 → make it inactive
      x[aii[jpos] = 0
      r_actives--; swap_pos(r_actives, jpos); lu_remove(jpos)        // 0x10034870
      goto loop
  if jpos < ignored_pos:                         // an inactive hits accel = 0 → make it active
      if step > TEST_EPS: prune_actives()        // 0x10034a40
      accel[aii[jpos] = 0
      swap_pos(r_actives, jpos); r_actives++; lu_add()               // 0x100348b0
      goto loop
  // jpos == ignored_pos: ig reached accel = 0
L_IG_TO_ACTIVE:                                  // 0x10034e47 (also entered with step unchanged)
  prune_actives()
  v = aii[jpos]; accel[v] = 0; if x[v] < 0: x[v] = 0
  swap_pos(r_actives, jpos)
  ignored_pos++; r_actives++
  if ignored_pos < n: lu_add(); goto loop
  full_test_countdown = 0; goto loop
L_TO_INACTIVE:                                   // 0x10034ea8
  did_real_step = 0
  x[ig] = 0; if accel[ig] < 0: accel[ig] = 0
  ignored_pos++
  if ignored_pos >= n: full_test_countdown = 0
  goto loop
L_RESTART:                                       // 0x10034d68
  steps++                                        // unconditional
  if steps > 250: return 0
  zero_steps = 0
  if full_setup() == 0: return 0                 // 0x100354e0
  full_test_countdown = 7
  goto L_MAIN
```
Notes:
- The code path entering L_IG_TO_ACTIVE from the `|accel| < eps, |x| ≥ eps` test does not clear
  `did_real_step` and does not run the ratio test.
- "250 iterations" = `steps > 0xfa` (so at most 250 counted steps; non-real steps (moves to inactive)
  are not counted, restarts are).
- On a 0 return the caller (§LS2.6) skips all pushes for the PSI.

#### LS4.4 Helpers

- **swap_pos(p, q)** 0x10034f10: swap `aii[p]`, `aii[q]` and update `found[]`.
- **update_step(s)** 0x10034f40: for all i < n: `accel[i] += s·delta_accel[i]; x[i] += s·delta_f[i]`;
  then for inactive positions `[r_actives, ignored_pos)`: `if accel < 0: accel = 0`; for actives
  `[0, r_actives)`: `if x < 0: x = 0`.
- **compute_delta_accel** 0x100342c0: for positions p in `[r_actives, n)`: `v = aii[p]`;
  `delta_accel[v] = Σ_{m<r_actives} delta_f[aii[m]·A[v][aii[m] + A[v][aii[ignored_pos]`.
- **get_fx_dx** 0x10034ff0: `delta_f[0..aligned-1] = 0`. If `r_actives == 0` return 1 (caller sets
  `delta_f[ig] = 1`). If `status == 0`: `lu.input[m] = −A[aii[m][ig]`, `lu.solve()`, ret = 1. Else
  (`+0x64++`) build Gauss matrix (+0xb0) with `n = r_actives`: `values[m][c] = A[aii[m][aii[c]`,
  `desired[m] = −A[aii[m][ig]`, `ret = gauss_solve()` (eps = GAUSS_EPS), `lu.out[m] = result[m]`.
  Then `delta_f[aii[m] = lu.out[m]` for m < r_actives, `delta_f[ig] = 1.0`, return ret.
  (The Gauss branch is also taken after a failure, so results come from `lu.out` either way.)
- **full_test** 0x100343a0: `test = A·x − b` (0x10034340, inner sum over c = n−1..0); fail if any active has
  `|test[v]| > MAX_ERROR`, or any position in `[r_actives, n)` has `|test[v] − accel[v]| > MAX_ERROR`.
- **prune_actives** 0x10034a40: for m = 0 while m < r_actives: `v = aii[m]`; if `x[v] < TEST_EPS`:
  `x[v] = 0; swap_pos(m, r_actives−1); r_actives--; lu_remove(m); m--`. m++.
- **lu_remove(m)** 0x10034870: if `status == 0`: `if lu.delete_row_col(m) != 1: status = 2` (0x10035d00);
  else `lu.n_sub--`.
- **lu_add()** 0x100348b0: `v = aii[r_actives−1]`. If `status == 0`: `U[lu.n_sub][m] = A[v][aii[m]` for
  m < r_actives; `lu.input[m] = A[aii[m][v]` for m < r_actives−1; `if lu.add_row_col() != 1: status = 2`
  (0x10035be0). Else `lu.n_sub++`.
- **setup_l_u_solver** 0x100351c0: for m < r_actives: `lu.input[m] = b[aii[m]`,
  `U[m][c] = A[aii[m][aii[c]` (c < r_actives); return `lu.decompose()` (0x10035ae0). Caller must have
  set `lu.n_sub = r_actives`.
- **little_random_permutation** 0x10034990 (deterministic): if `r_actives >= 2`:
  `p1 = (p1+1) mod r_actives`, `p2 = (p2+2) mod r_actives` (mod by repeated subtraction of the stored
  counter), `swap_pos(p1, p2)`; then `q2 += 2; q1 += 1; m = n − ignored_pos − 1`; if `m >= 2`:
  `q1 = q1 mod m, q2 = q2 mod m`, `swap_pos(q1 + 1 + ignored_pos, q2 + 1 + ignored_pos)`.
  Nothing at all happens when r_actives < 2. It does not touch the LU, which is why callers refactorize.
- **sort_actives_by_x** 0x10035250: insertion sort of positions `[0, r_actives)` by `x` descending.
- **rotate_to_end(p)** 0x10035680: `for q = p+1..n−1: swap_pos(q−1, q)` (moves the variable to position
  n−1, shifts the rest down).
- **full_setup** 0x100354e0 (restart from current active set), returns 1 ok / 0 fail:
  ```
  repeat:
     lu.n_sub = r_actives; reset_x[0..aligned-1] = 0
     little_random_permutation()
     if setup_l_u_solver() == 1:
         status = 0; lu.solve(); reset_x[aii[m] = lu.out[m]
     else:
         status = 2; sort_actives_by_x(); little_random_permutation()
         gauss(+0xb0): n = stride = r_actives, values = A_act, desired = b_act
         r = gauss_solve(); if r != 1: return r          // 0
         reset_x[aii[m] = gauss.result[m]
     recompute_x_accel()                                  // 0x100352c0
     k = remove_negatives()                               // 0x100356b0
  until k < 1
  return 1
  ```
- **remove_negatives** 0x100356b0: for actives m (with m-- on removal): `v = aii[m]`; if `x[v] < TEST_EPS`:
  if `x[v] <= −TEST_EPS`: `rotate_to_end(m); count++; ignored_pos--` else `x[v] = 0; swap_pos(m, r_actives−1)`;
  in both cases `r_actives--; lu.n_sub--; status = 1`. Then for inactive positions p in
  `[r_actives, ignored_pos)`: if `accel[aii[p] < 0`: `rotate_to_end(p); p--; ignored_pos--`. Returns count
  (only strongly negative actives count).

#### LS4.5 Incremental LU (lcs+0x80)

All loops are over the current `n = lu.n_sub` unless stated.

- **decompose** 0x10035ae0 (U pre-filled with A_act): if n == 0 return true. `L = I`.
  For p = 0..n−2: `pivot(p)` (0x10035950: m = |U[p][p]|; for r = n−1 down to p+1, strict `|U[r][p]| > m`
  picks r; if r ≠ p swap U rows r,p from column p (0x100358a0) and full L rows); `if !normalize(p): return false`;
  for r = n−1 down to p+1: `f = U[r][p]; if f != 0: eliminate(p, r, f)`.
  Finally `return normalize(n−1)`.
- **normalize(p)** 0x100357f0: `d = U[p][p]`; if `|d| < eps` return 0; `inv = 1/d`; `L[p][0..n−1] *= inv`;
  `U[p][p+1..n−1] *= inv`; `U[p][p] = 1.0`; return 1.
- **eliminate(p, r, f)** 0x100359d0: `U[r][c] −= f·U[p][c]` for c = p+1..n−1; `L[r][c] −= f·L[p][c]` for
  c = 0..n−1; `U[r][p] = 0`.
- **solve** 0x10035ff0: `mult = input`; `tmp = L·mult` (0x10035f40, sums c = n−1..0);
  for i = n−1..0: `tmp[i] −= Σ_{c=n−1..i+1} tmp[c]·U[i][c]; out[i] = tmp[i]` (0x10035f90).
- **add_row_col** 0x10035be0 (new last row of U already holds `A[v][act]`, input holds the new column
  `A[act][v]`, n = old size): `mult = input; tmp = L·mult; U[i][n] = tmp[i]` (i < n); `L[i][n] = 0` (i < n);
  `L[n][0..n] = 0, L[n][n] = 1`; `n_sub = n+1`; for i = 0..n−1: `eliminate(i, n, U[n][i])` (no zero test;
  U[n][i] re-read each step); return `normalize(n)`.
- **delete_row_col(k)** 0x10035d00 (n = current size, last = n−1):
  swap columns k and last in L (0x10035e80) and in U (0x10035ee0);
  for r = n−2 down to k+1: `f = U[r][k]; if f != 0: L[r][*] −= f·L[last][*]` (0x10036030); `U[r][k] = 0`;
  if `!normalize(k)`: `L[k][*] += L[last][*]` (0x10036030 with f = −1.0); `U[k][k] = 1.0`;
  for p = k..n−2: `f = U[last][p]; if f != 0: eliminate(p, last, f)`;
  if `normalize_L(last)` (0x10035780: `d = L[last][last]`, fail if `|d| < eps`, `L[last][*] *= 1/d`,
  `L[last][last] = 1`): for r = n−2 down to 0: `f = L[r][last]; if f != 0: L[r][*] −= f·L[last][*], L[r][last] = 0`
  (0x10035a80); `n_sub--`; return 1. Else `n_sub--`; return 0.

Port note: the incremental LU only changes rounding and which singularity tests fire (status → 2 →
Gauss with eps 1e-7). A port may replace `get_fx_dx`'s LU solve by a fresh partial-pivot Gauss solve of
the active sub-system (the existing status ≠ 0 branch), keeping all pivot decisions of solve_lc. Keep the
deterministic permutations: they reorder `aii` and therefore which variable is treated next.

### LS5 Core reaction helper (tangential friction, called from 0x1001b080)

#### LS5.1 init — 0x10033a30 (this, core0|NULL, core1|NULL, f64x3* p_ws, f32x3* dir0, f32x3* dir1|NULL, f32x3* dir2|NULL)
```
this.dir = (dir0, dir1, dir2)
if dir1 == NULL: m00 = 0 (only +0xd8); vel[0] = 0
else:            m[0..2][0..3] = 0 (0x60 bytes); vel[0..2] = 0
if core0: add_core(p_ws, core0, cr_out0(+0x0c), cr_mult0(+0x74), +1.0f)   // 0x10033560
if core1: add_core(p_ws, core1, cr_out1(+0x3c), cr_mult1(+0xa4), -1.0f)
```
#### LS5.2 add_core — 0x10033560 (p_ws, core, out[3] f32x4, mult[3] f32x4, f32 sign)
```
this.(+0x6c) = &core.m_world_f_core
r = (f32x3)(p_ws − core.pos(+0x188))                 // f64 subtract, stored f32
c = r × dir0  (f32, x87);  out[0].xyz = Mᵀ·c (f32), out[0].w = 1.0
mult[0] = (out[0].x·invIx, out[0].y·invIy, out[0].z·invIz, inv_mass)
m00 += dot4(out[0], mult[0])                           // f64 store
vel[0] += sign · (dot(core.rot_speed, out[0].xyz) + dot(dir0, core.speed))   // f32 store
if dir1:
   c1 = r × dir1; out[1] = (Mᵀ·c1, 1.0); mult[1] = out[1] ⊙ (invI, inv_mass)
   m11 += dot4(out[1], mult[1])
   m01 += dot3(mult[1].xyz, out[0].xyz)                // no inv_mass·(dir0·dir1) term: dirs assumed ⊥
   vel[1] += sign · (dot(core.rot_speed, out[1].xyz) + dot(core.speed, dir1))
   if dir2:
      c2 = r × dir2 (0x10021fd0); out[2].xyz = Mᵀ·c2 (0x10033d20); out[2].w = 1.0
      mult[2] = out[2] ⊙ (invI, inv_mass)              // 0x100339e0 (f32x4 product with core+0x34)
      m22 += dot4(mult[2], out[2]); m02 += dot3(mult[2], out[0]); m12 += dot3(mult[2], out[1])
      vel[2] += sign · (dot(core.speed, dir2) + dot(core.rot_speed, out[2].xyz))
```
`dot4` (0x100339b0) = x·x' + y·y' + z·z' + w·w'. The response matrix is the sum over both cores
(with the same sign for both cores, since the sign enters only the velocity).

#### LS5.3 apply 2D impulse — 0x10033ad0 (this, core0|NULL, core1|NULL, f32 imp[2]) — writes velocities directly (not async)
```
if core0:
   core0.speed += dir0 · (f64)(imp[0]·inv_mass0); core0.speed += dir1 · (f64)(imp[1]·inv_mass0)   // 0x10010290
   core0.rot_speed += cr_mult0[0].xyz · imp[0]; core0.rot_speed += cr_mult0[1].xyz · imp[1]
if core1:
   core1.speed += dir0 · (−imp[0]·inv_mass1); core1.speed += dir1 · (−imp[1]·inv_mass1)
   core1.rot_speed += cr_mult1[0].xyz · (−imp[0]); core1.rot_speed += cr_mult1[1].xyz · (−imp[1])
```
(Only two directions are ever applied; dir2 data is setup-only.)

### LS6 Function index (0x10033500-0x10037000)

| Addr | Name / purpose |
|---|---|
| 0x10033510, 0x10033520, 0x10033550 | small zero-init helpers used by triangulation (0x10048b60/0x10048ed0); not solver |
| 0x10033560 | core_reaction add_core (§LS5.2) |
| 0x100339b0 | dot4 |
| 0x100339e0 | f32x4 component product |
| 0x10033a30 | core_reaction init (§LS5.1) |
| 0x10033ad0 | core_reaction apply 2D impulse (§LS5.3) |
| 0x10033d20 | Mᵀ·v (world → core) with f64 matrix, f32 vector |
| 0x10033d80 | row_ok (§LS2.8) |
| 0x10033de0 | add_row (§LS3.2) |
| 0x10033e50 | gauss_solve (§LS3.2) |
| 0x10033e70 | align values pointer (no-op) |
| 0x10033e80 / 0x10033f00 | pivot search / swap rows |
| 0x10033f80 / 0x10034030 | eliminate / back-substitute |
| 0x10034100 | Great_Matrix ctor (eps 1e-9) |
| 0x10034120 | gather sub-matrix (§LS3.1) |
| 0x10034270 | mult |
| 0x100342c0 | compute_delta_accel |
| 0x10034340 / 0x100343a0 | A·x−b / full_test |
| 0x10034420 | LCS alloc |
| 0x100346e0 | LCS init_and_solve (§LS4.1) |
| 0x10034870 / 0x100348b0 | lu_remove / lu_add |
| 0x10034990 | little_random_permutation |
| 0x10034a40 | prune_actives |
| 0x10034aa0 | `(*p)++` (step counter) |
| 0x10034ab0 | solve_lc (§LS4.3) |
| 0x10034f10 / 0x10034f40 | swap_pos / update_step |
| 0x10034ff0 | get_fx_dx |
| 0x100351c0 | setup_l_u_solver |
| 0x10035250 | sort_actives_by_x |
| 0x100352c0 | recompute_x_accel |
| 0x10035380 | start_initialize (§LS4.2) |
| 0x100354e0 | full_setup |
| 0x10035680 | rotate_to_end |
| 0x100356b0 | remove_negatives |
| 0x10035780 | LU normalize_L |
| 0x100357f0 | LU normalize (U row) |
| 0x100358a0 / 0x10035950 | LU swap rows / pivot |
| 0x100359d0 / 0x10035a80 | LU eliminate (U+L) / eliminate (L only) |
| 0x10035ae0 | LU decompose |
| 0x10035be0 | LU add_row_col |
| 0x10035d00 | LU delete_row_col |
| 0x10035e80 / 0x10035ee0 | swap columns in L / U |
| 0x10035f40 / 0x10035f90 / 0x10035ff0 | L·v / U back-subst / LU solve |
| 0x10036030 | L row op |
| 0x10036080 | check_guess (§LS2.8) |
| 0x10036170 | x *= scale |
| 0x10036190 | scale (§LS2.7) |
| 0x100362c0 | count leading cps with key < 0 |
| 0x100362e0 | solve_and_push (§LS2.6) |
| 0x100364c0 | do_resulting_pushes (§LS2.9) |
| 0x100366a0 | setup_coords (§LS2.4) |
| 0x10036760 | build_matrix (§LS2.5) |
| 0x10036ac0 / 0x10036b00 / 0x10036b30 | v·s / −v / dot(n,b)+dot(cross[k],a) |
| 0x10036b80 | clear_far_dists (§LS2.2) |
| 0x10036c20 / 0x10036c60 | swap adjacent list nodes / sort (§LS2.1) |
| 0x10036cb0 / 0x10036d10 | pair count / too_many_contacts (§LS2.3) |
| 0x10036d70 | do_friction_system (2) |
| 0x10036e90 | inline-vector dtor (exception unwind) |
| 0x10036ec0 | Σ kinetic energy incl. pending pushes |
| 0x10036f80 / 0x10036fa0 | commit / abort async pushes of movable cores |
| 0x10036fc0 | energy tolerance Σ m·9.81·0.1 |

## 6. Impact solver [IM]

Scope: the elastic collision response IVP runs when a mindist reaches collision distance. That covers the
per-pair impact solver (`IVP_Impact_Solver`, a 0x124-byte stack object), the impact system that chains impacts
through the friction system's pairs (`IVP_Impact_System`, a 0x24-byte stack object), and the post-collision
event. Names follow the IVP source where the structure matches.

Function map of the range:

| Addr | IVP name (inferred) | Role |
|---|---|---|
| `0x100240a0` | `IVP_Mindist::do_impact` | Entry point, mindist vtable `0x100637b4` slot 7 (`+0x1c`) |
| `0x10023cd0` | `IVP_Impact_Solver::do_impact_of_two_objects` | Builds the contact, runs the first impact and the impact system, fires events |
| `0x10022180` | `IVP_Mindist::try_to_generate_managed_friction` | Finds or creates the contact point and its friction system ([CP]/[FS] overlap, summarised in §IM11) |
| `0x10024930` | `IVP_Contact_Point::get_rescue_speed_impact` | Extra separation speed for penetration and rotation |
| `0x100248a0` | (helper) | Rotation part of the rescue speed |
| `0x10024140` | `IVP_Impact_Solver_Long_Term::do_impact_long_term` | Sets up an `IVP_Impact_Solver` from `tmp_contact_info` and calls `do_impact` |
| `0x10023ff0` | `get_cos_sin_for_impact` | Friction cone angle |
| `0x10024740` | (anisotropic friction setup) | **Dead in Ballance** (material `+8` is always 0) |
| `0x10022ed0` | `IVP_Impact_Solver::do_impact` | The response algorithm |
| `0x10022500` | `calc_relative_speed` | `rel = v1(p1) - v0(p0)` |
| `0x10022580` | `calc_push_direction` | Clamps the push direction to the friction cone |
| `0x10022720` | (elliptic cone clamp, uses `asin`) | Only reached with anisotropic friction, so **dead** |
| `0x10023940` | `push(f)` | Applies impulse `f·dir` (+ to core0, − to core1) to the solver's copies of the velocities |
| `0x10022950` | `undo_push` | Subtracts the last push |
| `0x10022a20` | `ensure_min_separation(dir, clamp)` | "Rescue" push up to a minimum separating speed |
| `0x10023b90` / `0x10023c80` | `calc_virtual_masses` / `get_full_elastic_impulse` | Virtual masses along n, and the impulse `2·m_red·vn` (m_red = m0·m1/(m0+m1)) |
| `0x10023680` | `apply_result` (with delaying) | Writes velocities back to the cores, optionally deferring the heavy one |
| `0x100228c0` / `0x10023870` / `0x100238f0` | write direct / write as pending change / clear pending changes | Core write-back |
| `0x10024040` | `IVP_Contact_Point::read_materials` | Fills `tmp+0x40..0x54`, `tmp+0x68` elasticity and `cp+0x44` friction |
| `0x10023fa0` | `get_materials` | Material for each synapse |
| `0x10024320` | `IVP_Impact_System::init_and_solve_impact_system` | Chained impacts, up to 5000 rounds |
| `0x10024560` | `IVP_Impact_System::next_impact` | Picks the most critical contact point and impacts it |
| `0x100249b0` | `IVP_Contact_Point::calc_impact_prediction` | Predicted gap at the next PSI |
| `0x100244a0` / `0x10024450` / `0x10024530` / `0x10024480` / `0x10024700` | impact-system set helpers | |
| `0x100242c0` | `recalc_other_contacts_of_pair` | |
| `0x10024aa0` | `IVP_Impact_System::finish` | Undo unused syncs, recompute the next-PSI matrices of pushed cores |
| `0x10024c20` | `IVP_U_Vector` ctor with capacity | |
| `0x10023f30` | impact-system dtor (unwind funclet target) | |
| `0x10022010`, `0x10022030` | object position-cache helpers (vtable `0x100631f8` via `0x1000bb00`) | **Not impact related** |
| `0x10022160` | sets vtable `0x10063890` (a listener base ctor) | not impact |
| `0x10022170` | `ret 4` no-op used in many listener vtables | |
| `0x10024c60`, `0x10024db0` | buoyancy helpers (callers `0x10027bc0`, `0x1000f940`) | **Unused** |

Throughout:

- `0x10063230` is a **double −1.0**, always used as `fmul qword`. The decompiler's `(float)` casts are wrong.
- `0x10063488` is double 0.0, `0x10063238` float 1.0, `0x100634a0` float 0.5, and `0x10063288` double 1.0.
- All arithmetic is x87, at 64-bit precision inside the event loop. Floats are float32 storage, and
  intermediates are extended.

### IM1 Trigger: from collision detection to `do_impact`

- **Time-manager event.** Mindist vtable slot 0 (`0x100181b0`, `simulate_time_event`) handles the event at a
  mindist's predicted time:
  1. profiler phase 8;
  2. `recalc_mindist` (`0x10019950`);
  3. let `st = mindist+0x14`. If `(st & 0xc000) == 0` and `(st & 0xf) == 0` (status ok), and
     `len(mindist+0x54) < coll_dists[(st >> 22) & 0xff] + 0.001f` (`0x10075ed4`): call the virtual
     `do_impact` (slot `+0x1c`, `0x100240a0`).
  4. Otherwise reschedule with `0x10017870(this, 0, 2)`, or `(this, 0, 1)` when `st & 0xf` is set.
  5. Profiler phase 0xe.
- **Threshold.** `coll_dists[]` (64 entries at `0x10075db8`) are all 0.01 in this build (§IM10), so an
  **impact fires when the surface distance is below 0.011 m**.
- **Second caller.** `0x10030668` (inside `0x10030540`, vtable `0x10063ae0` slot 3) also calls `0x100240a0`
  directly. It looks like a ledge-tree / recursive mindist variant: a switch on the synapse types at `+0x32`
  and `+0x4e`, then edge flag bit 31. **Not traced.**

`IVP_Mindist` fields used here:

| Off | Type | Meaning |
|---|---|---|
| `+0x14` | u32 | Status bits. Bits 8-9 are the synapse order: `objA = syn[(f >> 8) & 3]`, `objB = syn[((f ^ 0x100) >> 8) & 3]`. Bits 22-29 index `coll_dists`. |
| `+0x18`, `+0x34` | synapse 0 and 1 (stride 0x1c) | `+0x10` in each is the object, so `mindist+0x28` and `mindist+0x44` are obj0 and obj1 |
| `+0x54` | float | Current distance |

### IM2 `0x100240a0` `IVP_Mindist::do_impact()` (fastcall, `this` = mindist)

```
env = obj0(mindist+0x28)->env(+0x18)
for i in 0,1: obj[i] = synapse[i].obj; revive_object_for_simulation(obj[i])   // 0x10009670
env->short_term_mem(+0xb4)->transaction_depth(+0x10, i16) += 1               // start transaction
for i in 0,1:
    core = obj[i]->core(+0xa4)
    if (int8)core->+0x60 < 8: core->synchronize_with_rot_z()                  // 0x1000cfa0 (§IM12)
env->+0x13c += 1                     // impact stamp, later copied to core+0x230
do_impact_of_two_objects(mindist, obj[0], obj[1])                             // 0x10023cd0
env->short_term_mem->transaction_depth -= 1; end_transaction(mem)             // 0x100201b0
```

All `tmp_contact_info` blocks (0xe0 bytes) and core sync backups (0x38 bytes) live in this short-term memory
and are freed here.

### IM3 `0x10023cd0` `do_impact_of_two_objects(mindist, obj0, obj1)` (cdecl)

```
core0 = obj0->core; core1 = obj1->core
sim_unit_keep = ((int8)obj0->+0x80 >= 8) ? core0->sim_unit(+0x1d4) : core1->sim_unit
cp = mindist->try_to_generate_managed_friction(&fs, &having_new, sim_unit_keep, /*recalc*/1)   // §IM11
tmp = cp->tmp_contact_info (cp+0x40)
rescue_addon = cp->get_rescue_speed_impact(env = obj0->+0x18)                                // §IM4
tmp->do_impact_long_term(pushed_cores[2] /*out*/, rescue_addon, cp)                         // §IM5
saved_rel = tmp->rel_speed (tmp+0x10)                                     // v1 - v0 at the contact, pre-impact
if core1->flags & 0xc: saved_rel *= -1.0      // undoes the core swap made in §IM5 when core1 is fixed
pair = fs->find_pair(core0, core1)            // 0x1001d380; pair+0x2c/+0x30 are its cores, either order
t_prev = pair->last_impact_time (+0x20, double); pair->+0x20 = env->current_time (+0x120)
IVP_Impact_System sys; sys.init_and_solve_impact_system(mindist, fs, pair, cp)              // §IM8
tmp->rel_speed = saved_rel                    // restored, because the impact system may overwrite it
event = { float d_time_since_last = (float)(now - t_prev), env, contact_situation = tmp }
env->fire_event_post_collision(&event)        // 0x10013b80: env listeners (+0xf6 count, +0xf8 array), flag bit0, slot 0
if obj0->+0x80 & 0x2000: env->+0xc listener manager: fire per-object (obj0, &event)          // 0x1000a770
if obj1->+0x80 & 0x2000: same for obj1
```

**Post-collision event.** This is what PhysicsCollDetection, `FUN_100042b0`, consumes.

- `event+0`: float, the time since the last impact of this core pair.
- `event+4`: env.
- `event+8`: `tmp`, which is an `IVP_Contact_Situation`.

The listener reads these `tmp` fields:

| Field | Meaning |
|---|---|
| `tmp+0x40`, `tmp+0x44` | the two objects |
| `tmp+0x10` | relative speed vector. **Speed = \|tmp+0x10\|**: the full relative velocity at the contact point (normal and tangential), **before** the impact, computed from speed + pending change at collision time |
| `tmp+0x00` | surf_normal. It points from obj0 to obj1, since approach means `n·(v1 − v0) < 0` |
| `tmp+0x20` | contact point (world, double) |

- **The listener negates `tmp+0` in place** when its own object is not `tmp+0x40`.
  - The reported normal therefore points from the listener's object toward the other object.
  - Because the negation mutates shared data, any later listener for the same event sees the flipped vector.
    That includes a second CollDetection BB on obj1, which flips it back.
  - Order of listeners: environment first, then obj0's, then obj1's.
- Only the mindist-triggered impact fires this event. Chained impacts inside the impact system fire nothing.

### IM4 Rescue speed

**`0x10024930` `cp->get_rescue_speed_impact(env)`**, which returns a float:

```
if cp->gap(+0x58, float) < min_coll_dist (0x10075db4 = 0.01):
    v = (0.01 - gap) * env->inv_psi(+0xc8, double)            // speed needed to leave the 0.01 shell in one PSI
else:
    tmp->+0x60 = 0.0f;  v = 0                                  // side effect: clears the stored rescue value
return (v + rot_part(cp)) * 2          // fadd st0,st0
```

**`0x100248a0` `rot_part(cp)`**:

```
s = 0.0f
for i in 0,1:                          // core_i = tmp->core[i] (tmp+0x78 / +0x7c); synapse type byte = cp+0x16 + 0x14*i
    if core_i == NULL or synapse_type_i == 3 (ball): continue
    w2 = |core_i->rot_speed(+0x94)|^2 * 2.5e-5        // double 0x100638e0; equals (0.005·|ω|)^2
    if w2 > 0.25: w2 = 0.25
    s += (1 - fcos(fsqrt(w2))) * core_i->+0x04 (float, object radius/extent) * env->inv_psi
return s
```

`cp+0x58` is the gap. `0x1001f860` builds it as `cp+0x58 − (r0 + r1)`, using the objects' `+0xa0` extra
radii, and clamps it to ≥ 0.

### IM5 `0x10024140` `tmp->do_impact_long_term(pushed_cores[2], rescue_addon, cp)` (thiscall, `this` = tmp)

This builds the solver `S` on the stack (0x124 bytes, layout in §IM13).

```
if tmp->core[1] (+0x7c) == NULL:          // obj1's core is fixed: swap so that S.core1 is the movable one
    S.core[0] = obj1(tmp+0x44)->core;  S.core[1] = tmp->core[0] (+0x78)
    S.p_cs[0] = &tmp->cp_cs[1] (+0xb0);  S.p_cs[1] = &tmp->cp_cs[0] (+0xa0)
    S.normal  = &(local copy of -tmp->surf_normal)
else:
    S.core[0] = tmp->core[0] ? tmp->core[0] : obj0(tmp+0x40)->core;  S.core[1] = tmp->core[1]
    S.p_cs[0] = &tmp->+0xa0;  S.p_cs[1] = &tmp->+0xb0;  S.normal = &tmp->surf_normal (+0)
S.elasticity(+0x110) = tmp->+0x68           // e = e0·e1 (§IM9)
S.rel_out(+0x120)    = &tmp->+0x10
S.aniso(+0xe8)       = 0
if S.core[0]->+0x10 == 0 && S.core[1]->+0x10 == 0:
    get_cos_sin_for_impact(mu = cp->+0x44, e, &S.cos(+0x114), &S.sin(+0x118))
    if cp->+0x34 (byte) != 0: setup_anisotropic(S, cp)       // 0x10024740: dead in Ballance (§IM7)
else:
    get_cos_sin_for_impact(0.0, e, ...)                       // no friction during the impact
S.do_impact(pushed_cores, allow_delaying = 1, pushes = (int16)tmp->+0x5a, rescue_addon)       // 0x10022ed0
```

- `core+0x10` is an int that disables friction in impacts when it is set. Where it is set was not traced. It
  is presumably 0 for every Ballance core.
- `cp+0x34` is a byte that enables anisotropic friction.

**`0x10023ff0` `get_cos_sin_for_impact(float mu, float e, float *c, float *s)`** (stdcall, `ret 0x10`;
ECX is ignored):

```
x  = (fsqrt(e) + 1.0) * mu            // tan(theta) = mu·(1 + sqrt(e))
th = fpatan(x, 1.0)                   // atan(x)
t2 = th*th
c  = t2*t2*0.041666668f + (1.0f - t2*0.5f)       // Taylor cos(theta), float consts 0x100638bc, 0x100634a0
*c = c;  *s = c * x                    // = cos·tan, approximately sin
```

Keep the truncated Taylor series: at large μ it differs from the real cosine. At θ = π/2 it gives
c = 0.0200, where the exact value is 0.

### IM6 `0x10022ed0` `S.do_impact(int *pushed_cores, int allow_delaying, int pushes, float rescue_addon)`

`n = *S.normal` points from core0 to core1. `rel = v1 − v0` is the velocity of point p1 on core1 minus that of
p0 on core0. They are approaching when `n·rel < 0`.

```
e_eff = 1.0f - (1.0f - S.elasticity) * (1.0f / ((float)pushes * 0.5f + 1.0f))   // kept as double; e_eff = e when pushes == 0
S.allow_delaying(+0x18) = allow_delaying
S.rescue(+0x00) = (mindist_change_force_dist(0x10075ed8 = 0.02f) + rescue_addon) * 1.2f   // float 0x100638d0
pushed_cores[0] = S.core[0]; pushed_cores[1] = S.core[1]
S.core[0]->env(+0xc)->+0x54 += 1                      // statistics
S.mat[i](+0x30/+0x34) = &S.core[i]->m_world_f_core(+0x128)
for i: S.rot[i]   = core_i->rot_speed(+0x94) + core_i->rot_speed_change(+0x74)   // core space
       S.speed[i] = core_i->speed(+0xa4)     + core_i->speed_change(+0x84)       // world
S.+0x28 = S.+0x2c = 0
calc_relative_speed(S)                               // S.rel(+0xb8) = v1 - v0   (0x10022500)
*S.rel_out = S.rel                                   // into tmp+0x10, used by the event
iter = 0
f = (double)get_full_elastic_impulse(S) * 0.1        // 0x10023c80; double const 0x10063500 = 0.1f-as-double
if n·rel <= -1e-4 (double 0x100638c8 = -9.999999747e-5):        // approaching
    calc_push_direction(S)                           // 0x10022580: S.dir from rel and the friction cone
    S.dir2(+0xd8) = 0
    vn0 = -(n·rel)                                   // double; dot order is (z*nz + y*ny) + x*nx
    target = fsqrt((double)pushes) * 0.01 + sqrt(e_eff) * vn0      // double 0x10063758 = 0.0099999998
    vn = vn0
    while vn > 0.0 and iter < 100:                    // compression, friction-limited
        iter++; push(S, f); calc_relative_speed(S); vn = -(n·rel); calc_push_direction(S)
    target = vn + target                               // vn is now <= 0, the overshoot
    S.dir = n * -1.0
    if target > 0.0 and iter != 100:                  // restitution along -n (core0 gets -n, core1 gets +n)
        push(S, 1.0); calc_relative_speed(S)
        d = (n·rel) + vn                               // change of n·rel per unit impulse
        k = (|d| <= 1e-4 (double 0x100638c0)) ? 0.0 : target / d
        undo_push(S)                                   // 0x10022950
        push(S, k)
    ensure_min_separation(S, &S.dir /* = -n */, clamp=0)       // 0x10022a20
else:                                                  // not approaching (resting or penetration)
    ensure_min_separation(S, -n, clamp=1)
    if pushes > 10: S.allow_delaying = 0
// velocity limits, per core i (i = 0 then 1)
lim = env->anomaly_limits(+0x24); am = env->anomaly_manager(+0x20)
if core_i->spin_clip(+0x4c, float3*) != NULL:
    clamp each S.rot[i] component to [-clip, +clip]
    QUIRK: for the y component, a value below -clip.y is set to +clip.y (sign bug; x and z are correct)
elif |S.rot[i]|^2 > (env->inv_psi(+0xc8) * lim->max_ang_per_psi(+0x10 = pi/2))^2:
    am->max_angular_velocity_exceeded(lim, core_i, &S.rot[i])      // slot 1, 0x1002f720: rot *= 0.9·66·pi/2 / |rot|
if |S.speed[i]|^2 > lim->max_velocity(+0x08 = 2000.0f)^2:
    am->max_velocity_exceeded(lim, core_i, &S.speed[i])            // slot 0, 0x1002f6d0: v *= 0.99·2000 / |v|
apply_result(S, pushed_cores)                          // 0x10023680
if (core0->+0x61 byte) | (core1->+0x61 byte):          // temporarily_unmovable (max-collisions anomaly)
    core0->env->+0x6c += 1
    for each core c:
        c->+0x61 = (1 - s(c)) & 0xff, where s(c) = 2-bit signed field of bits 2..3 of c->flags (0 for movable cores, which then get 1)
        c->speed_change += c->speed;  c->rot_speed_change += c->rot_speed;  c->speed = c->rot_speed = 0
        pushed_cores[i] = c
```

**Final separating speed** along n:

```
max( sqrt(e_eff)·vn0 + 0.01·sqrt(pushes),  1.2·(0.02 + rescue_addon) )
```

So **even with e = 0 every impact leaves the pair separating at least 0.024 m/s** (relative, at the contact
point). The non-approaching branch always adds a separating impulse of `S.rescue` (§IM6.4).

#### IM6.1 `0x10022500` calc_relative_speed

```
v0 = core0.get_surface_speed(p0, S.speed[0], S.rot[0])
v1 = core1.get_surface_speed(p1, S.speed[1], S.rot[1])
S.rel = v1 - v0
```

`get_surface_speed` is `0x1000bf90(core, p_cs, speed_ws, rot_cs, out)`:

```
w   = rot_cs × p_cs
out = M·w + speed_ws
```

Here `M = core+0x128`: three rows of 4 doubles at a stride of 0x20, mapping core space to world space.

#### IM6.2 `0x10022580` calc_push_direction (`S.dir` at `+0xc8`)

```
dir = normalize(S.rel)                // 0x1000e030: fast inverse sqrt plus 4 Newton steps; unchanged if |v|^2 < 1e-19
c = n·dir                             // stored as double
if c > 0.0:   dir = normalize(S.dir2)               // effectively dead: called only while n·rel < 0, and dir2 is zeroed
elif S.aniso == 0:
    if -S.cos < c:                                  // outside the cone around -n: sliding
        t = normalize(dir - c*n)
        dir = (-S.cos)*n + S.sin*t
    // else inside the cone: keep dir = rel direction (sticking: removes all relative motion)
else: elliptic_cone(S, c)                           // 0x10022720
```

**`0x10022720` elliptic_cone**. This is the only `asin` use in this range, and it is dead in Ballance:

```
t = dir - c*n; L = |t|; t = t/L                      // 0x1000df30 normalize, returns the length (0 if tiny)
a = |t·S.axis(+0xf0)|
th = asin(a); ca = taylor_cos(th)
R2 = (S.sin·ca)^2 + (S.sin2(+0xec)·a)^2
if L*L > R2:  r = sqrt(R2); ps = asin(r); dir = -taylor_cos(ps)*n + r*t
```

#### IM6.3 `0x10023940` push(double f) and `0x10022950` undo

```
imp = S.dir * (float)f                                                  // world
if core0 movable (flags & 0xc == 0):
    imp_cs = M0^T·imp (entries cast to float)
    core0.test_push(p0, imp_cs, imp, &S.dv0(+0x98), &S.dw0(+0x78))     // 0x1000ca80
    S.rot[0] += S.dw0;  S.speed[0] += S.dv0
if core1 movable:
    same with imp * -1.0 into S.dv1(+0xa8), S.dw1(+0x88), applied to S.rot[1] and S.speed[1]
```

`0x1000ca80` `test_push(p_cs, imp_cs, imp_ws, out_dv, out_dw)`:

```
out_dw = (p_cs × imp_cs) ⊙ inv_I(+0x34, +0x38, +0x3c)
out_dv = imp_ws * inv_mass(+0x40)
```

The core is not modified.

`undo_push` subtracts S.dw and S.dv from S.rot and S.speed, for each movable core.

#### IM6.4 `0x10022a20` ensure_min_separation(float3 *dir_in, int clamp)

```
d = dir_in * -1.0                      // with dir_in = -n this is d = n
s = (v1 - v0)·d                        // surface speeds from S.speed and S.rot
if clamp == 1 and s > 0: s = 0
diff = (double)(S.rescue - s)
if diff >= 0:
    inv = 0.0
    if core1 movable: inv += (velocity change at p1 from a unit impulse +d)·d            // test_push + get_surface_speed
    if core0 movable: inv += (velocity change at p0 from a unit impulse -d)·(-d)
    k = diff / inv
    if k >= 0:
        core0: impulse -k·d   (S.dw0/S.dv0 stored and added)
        core1: impulse +k·d   (S.dw1/S.dv1)
```

It also computes the world positions of p0 and p1 (`0x1000f5f0`), but the results are unused.

#### IM6.5 Virtual masses: `0x10023b90` and `0x10023c80`

**`calc_virtual_masses(n)`**:

```
S.m1(+0x10) = 1 / |Δv_p1(unit impulse +n on core1)|       // 0x1000c2b0: LENGTH of the point-velocity change, not its n component
S.m0(+0x08) = 1 / |Δv_p0(unit impulse -n on core0)|
if core0 fixed: S.m0 = S.m1 * 100000.0      // double 0x100638d8
if core1 fixed: S.m1 = S.m0 * 100000.0      // uses the possibly overwritten m0
```

**`get_full_elastic_impulse`**:

```
calc_virtual_masses(n)
return (1/(m0 + m1)) * 2*(-(n·rel))*m1 * m0      // = 2·m0·m1/(m0 + m1) · vn
```

#### IM6.6 `0x10023680` apply_result(pushed_cores)

```
for each core c: c->+0x64 (i16, impacts this PSI) += 1 - s(c)        // s as in §IM6: +1 for movable cores
if S.allow_delaying:
    H = (S.m0 <= S.m1) ? 1 : 0;  L = 1 - H                           // H = heavier (larger virtual mass)
    if pushed_cores[H] is movable:
        r = v_H(pH; core_H's ACTUAL pre-impact speed/rot_speed, without pending change) - v_L(pL; S.speed[L], S.rot[L])   // 0x1000c150
        q = n·r;  if H == 1: q *= -1.0
        if q < S.rescue * -0.8333333f (0x100638d4):     // light body now separates from the heavy body's old motion by more than (0.02 + addon)
            env->+0x64 += 1
            pushed_cores[H] = NULL
            clear_pending(both)                         // 0x100238f0: speed_change = rot_speed_change = 0 on both cores
            write_direct(L)
            write_as_pending(H)                         // 0x10023870: core_H.speed_change = S.speed[H] - core_H.speed;
                                                        //             core_H.rot_speed_change = S.rot[H] - core_H.rot_speed
            core_H->+0x64 -= 1
            return
clear_pending(both); write_direct(0); write_direct(1)
```

**`0x100228c0` write_direct(i)**:

```
core->speed(+0xa4) = S.speed[i]; core->rot_speed(+0x94) = S.rot[i]
if (int16)core->+0x64 > lim->max_collisions_per_psi(+0x0c, int 70000):
    core->+0x61 = am->max_collisions_exceeded_check(lim, core)       // slot 3, 0x1002fcc0, always returns 1
```

It does not check the fixed flag. For fixed cores it writes their own (zero) speeds back.

**Interpretation.**

- An impact writes **final velocities directly** into `core->speed` and `core->rot_speed`. Pending
  `*_change` vectors are folded in at the start and then cleared.
- When delaying is allowed and the light body bounces clear, the heavy body's response is stored as a
  **pending change** instead. Core integration applies it at the next PSI.
- In both cases the impact sets the velocity of a body mid-PSI, at the synchronized collision time (§IM12).

### IM7 Anisotropic friction (`0x10024740`): dead

```
mats = get_materials(cp)     // §IM9
for each side i with mats[i]->+0x08 != 0:
    axis = project_out(M_core_i column 0, n)            // 0x1000e220
    if |axis| >= 1e-19:
        S.axis = normalize(axis); S.aniso = 1
        get_cos_sin(mu_i', e, &dummy, &S.sin2)
```

Here `mu_i' = mu - (mu - mats[1-i].friction·mats[i].vfunc1()) * |axis|`.

The material constructor `0x1000bf00` sets `+8 = 0`, and so does the default 0.5/0.5 material. **Ballance
never enables this.** `S.aniso` stays 0, and `0x10022720` and its `asin` never run. A port may omit them.

Note: the `asin` in the core sync `0x1000cfa0` (§IM12) is live.

### IM8 Impact system (chained impacts)

**`0x10024320` `init_and_solve_impact_system(mindist, fs, pair, cp)`**, with `this` = `sys`:

```
sys.env = pair->core0->env;  sys.iter(+4) = 0;  sys.fs(+0x20) = fs
cA = cp->obj0(+0x10)->core; cB = cp->obj1(+0x24)->core
if cA movable: add_pushed_core(cA, pair)                      // 0x100244a0
if pair->core0(+0x2c) movable: sys.synced(+0x10).add(pair->core0)
if cB movable: add_pushed_core(cB, pair)
if pair->core1(+0x30) movable: sys.synced.add(pair->core1)
sys.pairs(+0x18).add(pair)                                    // 0x10024450
recalc_other_contacts_of_pair(pair, cp)                       // 0x100242c0
n = 0
while next_impact() == 1:                                     // 0x10024560
    n++; if ++sys.iter > 5000: { if mindist: mindist->vfunc[4](1) /* deleting dtor: mindist destroyed */; break }
env->+0x58 += n + 1
finish()                                                      // 0x10024aa0
```

**`add_pushed_core(core, exclude_pair)`**:

```
sys.pushed(+0x08).add(core)
core->sync_backup(+0x22c)->+0x30 = 1          // keep the synchronized state
for each pair q in fs->pairs (+0x36 count, +0x38 array), from last to first:
    if (q->core0 == core or q->core1 == core) and q != exclude_pair and q not in sys.pairs
       and fs_recalc_pair(q, fs) > 0:          // 0x1001cc20 ([FS]): recompute q's contacts, delete invalid ones, return count
        sys.pairs.add(q)
```

**`recalc_other_contacts_of_pair(pair, skip_cp)`**:

```
for every cp' of pair (+2 count, +4 array) except skip_cp:
    0x1001f860(cp')                           // recompute tmp_contact_info at the current (synced) time; [CP]
    read_materials(cp', cp'->tmp)
    if (tmp'->+0x5c & 0x300) == 0x100: fs->delete_contact_point(cp')    // 0x1001c460 ([FS])
```

**`0x10024560` `next_impact()`**, which returns 1 if an impact was done:

```
best = (double)min_coll_dist (0x10075eb8 = 0.01); best_cp = NULL
for each pair q in sys.pairs, from last to first:
    X(c) = ((c->+0x60 & 0xffffff00) << 16) | (int32)((c->flags & ~3) << 28) >> 6
    skip q if (X(q->core0) & X(q->core1)) != 0          // both fixed and/or temporarily unmovable, bitwise
    for each cp in q, from last to first:
        if (byte)cp->tmp->+0x5c != 1: env->+0x68 += 1; calc_impact_prediction(cp)     // 0x100249b0
        if cp->tmp->+0x64 < best: best = that; best_cp = cp; best_pair = q
if !best_cp: return 0
for c in (tmp->core[1], tmp->core[0]):
    if c and c->+0x22c == NULL: sys.synced.add(c); if (int8)c->+0x60 < 8: c->synchronize_with_rot_z()
tmp->+0x5a (i16, pushes) += 1
tmp->do_impact_long_term(pc[2], tmp->+0x60 /*rescue*/, best_cp)       // §IM5; pc[] reuses the 'best' double slot
for c in (pc[1], pc[0]):
    if c and movable:
        if c->+0x22c->+0x30 == 0: add_pushed_core(c, best_pair)
        else: reset_processed(c)              // 0x10024700: clear (byte)tmp->+0x5c of every cp in c's friction info for fs
return 1
```

`0x1000d960(core, fs)` returns the core's friction info for fs. Its `+2` and `+4` are a vector of contact
points.

**`0x100249b0` `calc_impact_prediction(cp)`**:

```
(byte)tmp->+0x5c = 1                                         // "evaluated"
if cp->gap(+0x58) > max_distance_for_impact_system (0x10075ed0 = 0.22): tmp->+0x64 = 1e20 (0x60ad78ec); return
tmp->+0x60 = get_rescue_speed_impact(cp, env)               // §IM4
vc = 0.0
if tmp->core0: vc = core0->rot_speed·tmp->+0xc0 + core0->speed·n      // +0xc0 = (p0 × n) in core0 space
if tmp->core1: vc = vc - (core1->rot_speed·tmp->+0xd0 + core1->speed·n)
tmp->+0x64 = gap - (vc + tmp->+0x60 * 0.5f) * env->delta_psi(+0xc0)    // predicted gap after one PSI (vc > 0 means closing)
```

This uses the cores' current velocities **without** the pending changes, and the contact geometry from the last
`0x1001f860`. So each round impacts the contact point that would be deepest below 0.01 m at the next PSI, given
the current velocities, until none is.

**`0x10024aa0` `finish()`**:

```
for c in sys.synced: if c->+0x22c->+0x30 == 0: c->restore_sync()       // 0x1000d140: put back rot_speed (+0x94) and quaternion (+0x108); +0x22c = 0
                     c->+0x22c = 0
dt = env->time_of_next_psi(+0x128) - env->current_time(+0x120)          // double
es = { dt, (dt <= 1e-10 (0x100633a8)) ? 9999999866.485682 : 1/dt, env }
vector touched (inline capacity 0x100)
for c in sys.pushed, if movable:
    IVP_Calc_Next_PSI_Solver{c}.calc_next_PSI_matrix(&es, &touched)     // 0x1001e300 (core integration)
    c->+0x22c = 0; reset_processed(c)
commit_all_calc_next_PSI_matrix(env, &touched)                          // 0x1001eb10
for c in sys.pushed, if movable: c->+0x230 = env->+0x13c; for each object o of c (+0x52 count, +0x54 array): 0x10009610(o)
```

### IM9 Materials and per-contact material data

**`0x10023fa0` `get_materials(cp, out[2])`**. For synapse i (base `cp+8+0x14*i`):

```
idx = (*(u32*)(synapse.geom(+0x10) & ~0xf) >> 24) & 0x7f        // 0x1001d830: material index stored in the ledge triangle
out[i] = idx == 0 ? synapse.obj->material(+0x98) : env->material_manager(+0xac)->vfunc0(0, idx)
```

Material manager slot 0 (`0x1000be30`) ignores its arguments and returns the static default material
(friction 0.5, elasticity 0.5). So a nonzero triangle material index silently means 0.5/0.5.

**`0x10024040` `read_materials(cp, tmp)`**:

```
get_materials(cp, &tmp->+0x50)
tmp->+0x40 = cp->obj0 (+0x10); tmp->+0x44 = cp->obj1 (+0x24)
tmp->+0x48 = cp->+0x18; tmp->+0x4c = cp->+0x2c                  // synapse geometry pointers
tmp->+0x68 = (float) mm->vfunc2(tmp)    // 0x1000bd40: e = mat[0x54]->elasticity(+0x20) * mat[0x50]->elasticity
cp->+0x44  = (float) mm->vfunc1(tmp)    // 0x1000bd10: mu = mat[0x54]->friction(+0x10) * mat[0x50]->friction
```

- Material fields: `+0x10` friction, `+0x18` second friction (left uninitialised by the ctor; read only in the
  dead anisotropic path), `+0x20` elasticity and `+0x28` adhesion, all doubles. `+0x08` is the anisotropy flag.
- Slot 3 (`0x1000bd70`) is adhesion, combined as a sum. It is not used by the impact solver.
- **The elasticity e is applied as sqrt(e)** to the normal speed. The friction cone uses
  `tan θ = μ(1 + sqrt(e))`.

### IM10 Mindist settings (static object at `0x10075db0`, set up by `0x10015fe0(base = 0.01f as double)`)

The values are float32. They are computed on x87 and stored.

| Addr | Offset | Value | Name (IVP) | Used here |
|---|---|---|---|---|
| `0x10075db0` | `+0x000` | 0.001 | real_coll_dist | |
| `0x10075db4` | `+0x004` | 0.01 | min_coll_dists | rescue: penetration shell |
| `0x10075db8` | `+0x008..+0x104` | 64 × 0.01 | coll_dists[] | impact trigger |
| `0x10075eb8` | `+0x108` | 0.01 | (max coll dist) | next_impact threshold |
| `0x10075ebc` | `+0x10c` | 0.02 | friction_dist | ([FS]) |
| `0x10075ec0` | `+0x110` | 0.023 | keeper_dist | ([CP]) |
| `0x10075ec4` | `+0x114` | sqrt(0.013·19.62) ≈ 0.505 | speed_after_keeper_dist | |
| `0x10075ec8` | `+0x118` | 0.0001 | distance_keepers_safety | |
| `0x10075ecc` | `+0x11c` | 0.045 | max_distance_for_friction | ([FS], `0x1001d660`) |
| `0x10075ed0` | `+0x120` | 0.22 | max_distance_for_impact_system | prediction cut-off |
| `0x10075ed4` | `+0x124` | 0.001 | minimum_friction_dist | impact trigger epsilon |
| `0x10075ed8` | `+0x128` | 0.02 | mindist_change_force_dist | base rescue speed (×1.2) |

The coll_dists loop computes `min + (max − min)·k/64`, which is constant because max = min.

### IM11 `0x10022180` `try_to_generate_managed_friction(&fs, &having_new, sim_unit_keep, recalc)` (summary; [FS]/[CP] own the details)

1. `cp = 0x10020030(mindist, &status)` finds or creates the contact point.
2. **Existing cp** (`status != 1`):
   - `fs = coreA->friction_info(+0x5c)->fs(+8)` and `having_new = 0`;
   - if `recalc`: `0x1001f860(cp)` and then `read_materials`;
   - return.
3. **New cp:**
   1. if `recalc`: `0x1001f860(cp)` and `read_materials`;
   2. fire "friction created": env listeners flag bit 2, slot 2, via `0x10013bc0`, then per object with flag
      `0x2000` via `0x1000a8a0`. The event is `{env, tmp, cp}`;
   3. `having_new = 1`.
4. **Attach to a friction system.**
   - Core A is the movable one (swapped if needed) and core B the other.
   - If A has no friction info:
     - if B is fixed or has none, create a new `IVP_Friction_System` (0x50 bytes, `0x1000b000`) and 0xc-byte
       infos for both cores;
     - otherwise join B's system.
   - If A has one:
     - if B has no info for that system but has one for another system, merge them (`0x1001c570`);
     - if B has none at all, create one.
   - Then call `fs.add_dist(cp)` (`0x1000b360`), `fs.add_pair(cp)` (`0x1000b2c0`) and `fs.add_core` as
     needed. Add cp to both infos (`0x1003aee0`), then call `0x1001d3d0(cp)`.
5. **Merge simulation units** if both cores are movable with different units: `0x10011770` and `0x10011890`,
   then delete the absorbed unit. `sim_unit_keep` is the one kept.

### IM12 Interfaces outside this range (behaviour relied on)

- **`0x1000cfa0` core synchronize_with_rot_z.**
  1. Allocate a 0x38-byte backup from short-term memory into `core+0x22c` (its `+0x30` "pushed" flag is set
     to 0), saving `rot_speed` and the quaternion at `+0x108`.
  2. Move the core to `env->current_time`. The quaternion and matrix `+0x128` come from the last PSI rotated
     by `rot_speed·(t − t_psi(+0x68))`, and the position is `+0x188 = pos_psi(+0xb8) + v(+0xd8)·(t − t_psi)`.
  3. Recompute `rot_speed` from the quaternion delta using `asin(...)·2·core+0x70`.

  **The impact therefore runs at the exact collision time, not at a PSI.** `0x1000d140` restores the backup.
- **`0x1001e300`/`0x1001eb10`** recompute the core's next-PSI matrix after a push (core integration).
- **`0x1001f860`** ([CP]) fills `tmp_contact_info`. **`0x1001cc20`** and **`0x1001c460`** ([FS]) recompute or
  delete a pair's contacts.

### IM13 Struct layouts

**`IVP_Impact_Solver` S** (stack, 0x124 bytes; at `esp+0x18` in `0x10024140`)

| Off | Type | Meaning |
|---|---|---|
| `+0x00` | float | rescue = (0.02 + addon)·1.2, the minimum separating speed |
| `+0x08` | double | virtual mass along n, core0 |
| `+0x10` | double | virtual mass along n, core1 |
| `+0x18` | int | allow_delaying |
| `+0x28` | 8 bytes | zeroed, unused |
| `+0x30`, `+0x34` | double[3][4]* | `&core_i->m_world_f_core` (core+0x128) |
| `+0x38`, `+0x48` | float3 (stride 16) | rot_speed[i] (core space), working copy |
| `+0x58`, `+0x68` | float3 | speed[i] (world), working copy |
| `+0x78`, `+0x88` | float3 | last push Δω[i] |
| `+0x98`, `+0xa8` | float3 | last push Δv[i] (array-constructed by `0x10001bd0` with `0x1000c2a0`) |
| `+0xb8` | float3 | rel = v1 − v0 at the contact |
| `+0xc8` | float3 | push direction (world) |
| `+0xd8` | float3 | secondary direction (zeroed) |
| `+0xe8` | int | anisotropic friction on (always 0) |
| `+0xec` | float | sin of the second cone |
| `+0xf0` | float3 | anisotropy axis |
| `+0x100`, `+0x104` | IVP_Core* | cores (the fixed one first if swapped) |
| `+0x108`, `+0x10c` | float3* | contact point in core space (into tmp) |
| `+0x110` | float | e (combined elasticity) |
| `+0x114` | float | cos θ |
| `+0x118` | float | sin θ (friction cone) |
| `+0x11c` | float3* | normal (tmp+0, or a negated local copy) |
| `+0x120` | float3* | rel output → tmp+0x10 |

**`IVP_Impact_System` sys** (stack, 0x24 bytes). `IVP_U_Vector` is `{u16 cap, u16 n, T **elems}`, grown by
`0x1000b600` and freed by `0x10039c00`.

| Off | Meaning |
|---|---|
| `+0x00` | env |
| `+0x04` | int iteration count |
| `+0x08` | vector pushed cores |
| `+0x10` | vector synchronized cores |
| `+0x18` | vector friction pairs |
| `+0x20` | friction system |

### IM14 Port notes for the impact path

- **Hook point.** In a per-PSI discrete collider (option (c)), call `do_impact` when a pair's surface distance
  falls below 0.011 m and the pair is approaching or penetrating.
  - First synchronize each core to the collision time. In a discrete step this is the PSI time itself, so the
    sync and restore become no-ops.
  - Build cp and tmp with `0x1001f860`, run §IM5 and §IM6, then run the impact system (§IM8) over the friction
    system's other contacts of the pushed cores.
- **Write-back** is direct velocity assignment (`speed`, `rot_speed`), plus pending `*_change` for delayed
  heavy cores. Those changes must be consumed by the integrator at the next PSI.
- **What may be dropped:**
  - the anisotropic friction branch (`0x10024740`, `0x10022720`);
  - the spin-clip path (`core+0x4c` is presumably NULL);
  - the max-collisions anomaly (70000 per PSI).
- **What must be kept:**
  - the y-clamp sign bug, if spin clipping is ever used;
  - the in-place normal negation in the CollDetection listener.
## 7. Port mapping

### 7.1 What collision detection must deliver

Full detail is in §CP7 and §IM1. Per close pair, the collider must provide a persistent **pair record**, the
mindist equivalent:

1. **Objects and order.** Two objects with an order (syn0, syn1). Each needs a physical core with the §2.3 fields,
   an object transform, a radius (`obj+0xa0`) and a material.
2. **Feature references, not results.**
   - Feature 0 is POINT, BALL or EDGE. Feature 1 is POINT, EDGE, TRIANGLE or BALL. EDGE–EDGE is the only pairing
     with an EDGE as feature 0.
   - Ball on a triangle soup is `(BALL, TRIANGLE)`, `(BALL, EDGE)` or `(BALL, POINT)`, depending on the closest
     feature. The ball is obj0.
   - A triangle feature must keep its vertex order (P1−P0 is the friction tangent basis). Its normal
     (P1−P0)×(P2−P0) must point out of the solid.
3. **The gap** `md+0x54`: surface distance minus radii. It is used for:
   - the impact trigger: `gap < 0.011`, status ok;
   - creation on revive and on transfer: `gap < 0.045`.
4. **Status flags.** "OK" (no impact or creation otherwise), and "closest triangle changed" for balls (§CP3.1).

**The contact code recomputes normal, point and gap itself from the feature references** (§CP4). A collider that
only yields (point, normal, distance) has two choices. It can synthesize features, which is easy for spheres
against triangles. Or it can bypass the case functions, as long as it reproduces their exact semantics:

- the gap against a triangle is the **plane** distance of the ball centre;
- the tangent basis is the first edge;
- removal happens when the projection leaves the triangle.

### 7.2 New C modules

The names below are suggestions. Every struct keeps only the fields listed in section 2.

| Module | Contents | Source sections |
|---|---|---|
| `src/phys/ivp_const.h` | §2.1 settings as f32 constants; PSI 1/66 and 66; the other constants | 2.1, FS2, LS0 |
| `src/phys/ivp_core.h` | `IvpCore`: flags (fixed), mass, inv_mass, I, I⁻¹, `speed` (world f32), `rot_speed` (**core-space** f32), `speed_change`, `rot_speed_change`, `R` (double 3×3, core→world), `pos` (double), extent (+4), impacts_this_psi, temporarily_unmovable, fric_info, uf_parent, sync backup. Helpers: point velocity, async push, test_push, commit, abort, KE, worst_vm | 2.3 |
| `src/phys/ivp_arena.c` | short-term bump arena with nested marks (tmp infos, solver matrices, impact scratch) | 2.2 |
| `src/phys/ivp_contact.c` | `IvpContactPoint`, `IvpContactInfo` (tmp); `cp_find_or_create`, ctor, `cp_update` (`0x1001f860`) with point_point / point_edge / point_triangle / edge_edge, `cp_read_materials`, `cp_init_constants`, `cp_destroy` and its events, ball triangle transfer, materials | CP2–6 |
| `src/phys/ivp_friction.c` | `IvpFrictionSystem`, `IvpFrictionPair`, `IvpFrictionInfo`; controllers `fs_update` (2000), `fs_spring` (600), `fs_normal` (0); tangential impulse with the core-reaction helper; single-contact normal; energy easing; spring easing; add/remove contact; merge; union-find split; revive creation; creation (`0x10022180`) | FS1–7, CP3.3, IM11 |
| `src/phys/ivp_fsolver.c` | multi-contact normal solve: sort, far-clear, matrix build, scaling, warm-start Gauss and check, LCP (`solve_lc` with its helpers), pushes, energy commit/abort | LS1.5–4 |
| `src/phys/ivp_impact.c` | `do_impact` (the mindist entry), impact solver, rescue speed, friction cone, apply/delay, impact system with prediction and finish, post-collision event | IM1–12 |

Drop the following, since they are unreachable in Ballance:

- anisotropic / two-friction-value code (`0x1001b8b0`, `0x1001ad90`, `0x1001aa20`, `0x10024740`, `0x10022720`
  with its asin);
- the car-wheel branch;
- spin clipping;
- the max-collisions anomaly;
- `too_many_contacts` (more than 150 contacts).

Leave an assert in each place.

**LCP solver.** The incremental LU (`IVP_Incr_L_U_Matrix`) may be replaced by a fresh partial-pivot Gauss solve of
the active subsystem, which is the code's own `status ≠ 0` branch. Keep every pivoting decision, the
deterministic permutations, the epsilons, and the 250-step limit (§LS4.5 port note).

### 7.3 Changes to existing code (`src/phys/phys.c`, `phys_internal.h`, `phys_collide.c`)

1. **Body representation.**
   - `PhysBody` must carry an `IvpCore`: core-space ω, a double rotation matrix and position, and the pending Δ
     fields. Today it keeps a world ω and a float quaternion.
   - The point velocity, the arms (`Rᵀ(p − pos)`) and the inverse inertia must use the core-space principal
     form. All solver formulas depend on it.
2. **Force controller.** `push()` must become the async push (`0x1000c830`) into `speed_change`/`rot_speed_change`.
   It must not write `v`/`w` directly.
   - This matters: the friction spring and the single-contact normal read `speed` **without** the pending part.
   - The multi-contact energy test includes it, and its abort **discards it**.
3. **Remove** `prepare_contact`, `solve_contact`, the 12-iteration loop and `impact_events`. Joints keep their own
   solver until the constraint port, but must run at their controller priority (open question 3).
4. **Collision.** `phys_collide()` must produce persistent pair records (§7.1) instead of per-PSI `PhysContact`s:
   - pairs must be reported up to at least 0.045 for friction and 0.22 for the impact system. Today the margin
     is 0.05;
   - concave meshes are a soup of single-triangle ledges, so one record per (body, triangle ledge) within range
     matches IVP;
   - convex hulls give one record per (body, hull) with the closest features.

### 7.4 The PSI loop after the change

```
psi_step(w):
  save prev poses                                       (write-back interpolation, unchanged)
  arena_begin(w)
  collide_update_pairs(w)                                // gaps, closest features, status
  // impacts. IVP: events at the exact time the gap crosses 0.011, between PSIs (§IM1).
  for each pair with gap < 0.011 and status ok, ordered by predicted time:
      ivp_do_impact(pair)                               // §IM2: sync cores, cp, solver, impact system, event
  // ball rolling onto a new triangle (0x10017790), before the controllers
  for each BALL–TRIANGLE pair whose triangle changed and gap < 0.045: ivp_ball_transfer(pair)   // §CP3.2
  // controllers, per sim unit, priority descending, stable for ties (§FS1.2)
  for fs: fs_update(fs, es)          // 2000: cp_update all, anti-energy, easing
  forces: async pushes               // 1500
  for fs: fs_spring(fs, es)          // 600
  joints                              // priority: open question 3
  for fs: fs_normal(fs, es)          // 0: 1 contact → 0x1001ae40, ≥2 → §LS2; removals, delete or split
  integrate(w)                        // core part: consume pending Δ, damping, gravity, next pose, sleep
  arena_end(w)
```

### 7.5 Making the discrete loop match IVP's event-driven impacts

IVP moves the cores to the **time of impact** (`0x1000cfa0`), solves there, and then recomputes the next-PSI pose
from that time (`0x1001e300`, §IM8 finish).

- A discrete port that impacts at PSI time with deep penetration still separates, through the rescue speed
  `(0.01 − gap)·66·2·1.2`. But fast balls will bounce from a deeper position than in the original.
- **Recommended:** for an approaching pair, estimate `t* = t_psi + (gap − 0.011)/(closing speed)` within the PSI.
  Then:
  1. extrapolate the core poses to `t*` (position `+= v·Δt`, rotation by ω·Δt);
  2. run the impact;
  3. set the next-PSI pose to `pose(t*) + v_new·(t_next − t*)`.

  This reproduces IVP's sync, solve and next-PSI recompute, and needs the core-integration part.

### 7.6 Events for the glue

These events drive PhysicsCollDetection and PhysicsContinuousContact.

| Event | Fired by | Carries |
|---|---|---|
| post-collision (`0x10013b80`, then the per-object listeners) | the **mindist-triggered impact only**, after the impact system (§IM3) | time since the pair's last impact; `tmp` = obj0/1 (+0x40/+0x44), normal (+0), contact point (+0x20), **speed = \|tmp+0x10\|**: the full pre-impact relative velocity, normal and tangential |
| friction created (`0x10013bc0`, `0x1000a8a0`) | a new cp (§CP3.3) | `{env, tmp, cp}` |
| friction deleted (`0x10013c00`, `0x1000a900`) | cp destructor `0x1001c230` | `{env, …, cp, obj0, obj1, g0, g1}` |

Two changes follow for the interim callbacks:

- `PhysImpactFn` must pass `|rel|`, not `−vn`.
- The touching or continuous-contact state should come from cp create and delete (the registry's per-object
  contact count), not from distance tests.

The CollDetection listener negates the shared normal in place when its object is not obj0. Keep that order effect:
the environment listeners run first, then obj0's, then obj1's.

## 8. Open questions and their answers

The port (`src/phys/phys_contact.c`, `phys_friction.c`, `phys_fsolver.c`, `phys_impact.c`) resolved these from
the disassembly:

1. **Pending Δ consumption.** The integrator (`0x1001e300` / `0x1001ea50`) never reads `speed_change` /
   `rot_speed_change`. They are committed in two places only (docs/ivp_core.md 6.3, 7.1):
   - at the start of each unit's PSI (`0x100121b0`): pushes made between PSIs, including the delayed heavy
     core of an impact (`0x10023870`);
   - by the gravity controller (`0x10012010`, priority 1000) **after** damping: damp, commit, add `g·dt`.

   So the energy easing (2000) and SetPhysicsForce (1500) are committed undamped at 1000. The friction spring
   (600) and the single-contact normal (0) write `speed` directly. When the multi-contact solve (0) runs its
   energy test, the only pending changes are its own: the constraints (405) push directly (`0x1000c8b0`),
   so **the abort never discards force pushes**.
2. **Second do_impact caller.** `0x10030540` is slot 7 of the recursive mindist (vtable `0x10063ad0`). For
   real features (bit 31 of the edge and triangle words clear, which holds for every Ballance ledge) every
   branch calls `0x100240a0` directly. Recursive mindists are created only for ledge-tree hull nodes, which
   Ballance's surface builder never makes (docs/ivp_collision.md 3.6). The only impact entry is `0x100240a0`.
3. **Constraint priority.** Slot 5 of the constraint vtable `0x10063930` is `0x10028230`, which returns
   `0x195` = 405. Per PSI and unit the order is:
   - 2000 fs update;
   - 1500 forces and springs;
   - 1000 gravity;
   - 600 friction spring;
   - 405 constraints;
   - 0 normal force.

   Hinges therefore see the friction impulses of the same PSI, and the normal force sees the constraint
   corrections. The friction system's controllers are attached in the order fs+8, fs, fs+0x10 (`0x1000b390`).
   Only the 600 controller associates cores (slot 2 `0x1001d600`; the other two return the empty vector
   `0x10075d90`). It is the one that keeps a system's movable cores in one sim unit through the unit split.
4. **Fields and bits.**
   - Core flag bits 0..1 are `fast_piling_allowed` (docs/ivp_core.md 1.2), 0 in Ballance. The reorder
     branch of `0x10036b80` is therefore dead.
   - Sim-unit flags (`0x100121b0`): bits 10–11 (`0xc00`) mean "some core is faster than 1 m/s this PSI";
     bits 12–13 (`0x3000`) hold the previous slow PSI's value. The easing runs only in slow PSIs. The
     anti-energy is reset in the PSIs after a fast one.
   - `cp+0x5c` still has no reader.
5. **Triangle material index.** Always 0. The pancake and hull builders write material 0
   (docs/ivp_collision.md 1.2, 1.6.2). Every contact uses the objects' materials.
6. **Ball synapse geometry.** A ball synapse carries the first edge of the ball surface manager's 32-byte
   all-zero dummy ledge (global `0x10076380`, docs/ivp_collision.md 1.3). Its triangle word is 0, so the
   material index is 0 and the object's material is used. The update never dereferences its points.
7. **Seam behaviour (and a correction to §1.2 step 2 and §CP3.1 item 3).** The ball triangle transfer
   (`0x10017790` → `0x10018040`) walks the mindist manager's **wheel** vector (`mgr+0xc`). Mindists enter it
   only when one object's core has `car_wheel` (`core+0x10`) set: `0x10016ea5` in `0x10016e30` and
   `0x1001700a` in `0x10016f90`. `0x100182cb` removes them again. No writer of `core+0x10` exists outside the
   car system, so **the transfer never runs in Ballance**.
   - A ball rolling across a seam loses the old contact by the feature test (the projection leaves its
     triangle, `0x10020ad0`, re-tested on every update for BALL syn0) in the normal-force controller of that
     PSI.
   - It then falls under gravity until the next triangle's mindist reaches 0.011 m. That mindist is already
     exact at about the same gap. The impact creates the next contact.
   - On a level floor the ball therefore rolls at a gap of about 0.011 m. Below 0.02 the gap spring
     recovers at only `(0.02 − gap)` m/s.
   - The port keeps the wheel vector and the transfer, both reachable only for car wheels.
8. **`core+0x10`.** It is never set in Ballance (see 7). The impact solver always uses the friction cone.
9. **Statistics fields.** They remain irrelevant to behaviour, and the port drops them.

**The glue's consumers of the events (§7.6), as traced for the port:**

- **PhysicsCollDetection** (`0x100042b0`, listener `{+8 last time, +0x10 sleep, +0x14 min, +0x18 max, +0x1c
  obj, +0x20 mgr, +0x24 behavior, +0x28 id}`). The checks run in this order:
  1. the other object's "Coll Detection ID" (−1 when it has none) must equal the listener's ID when the
     setting is on;
  2. the listener's **own** object must be in the registry;
  3. `now − last ≥ sleep`;
  4. output 0 must not still be active;
  5. `|tmp+0x10| > min`.

  The outputs are then set:
  - Speed = `1e-4` when `|rel| ≤ 1e-4`; else 1 when `max < 1e-4`; else `min(|rel|/max, 1)`.
  - The listener negates `tmp+0` in place (×−1.0) when its object is not `tmp+0x40`, then writes it out.
  - The position is `(x, y, 0)`: y is written twice.

  The object listeners run per object from the most recently added (`0x1000a770`): the mindist's synapse-0
  object first, then synapse 1.
- **PhysicsContinuousContact** (object listener vtable `0x100631b8`, flags 5). The install `0x10001470` keeps
  per registry record (`+0x2c`) the fields `{t_on, t_off, timers, behavior, groups[n] = {on, count},
  listener}`. The group count `n` is local 0 and is stored globally in `mgr+0x54+0`.
  - The group of a contact is the other object's "Continuous Contact ID" **minus 1**. An object without the
    attribute is ignored.
  - **created** (`0x100018f0`): `count++`. On the first contact of an off group, an old timer for the group
    (older than t_on) turns it on at once. Otherwise a timer starts. In an on group the pending off-timers
    are dropped.
  - **deleted** (`0x10001af0`): `count--` (clamped at 0). When the count reaches 0 in an on group, a timer
    starts.
  - **The manager's per-frame pass** (`0x100017a0`, after the step) handles each timer:
    - an on group with no contact turns off after `t_off`;
    - an on group with a contact drops the timer;
    - an off group with a contact turns on after `t_on`;
    - an off group without a contact drops the timer once `dt/2 > t_on`.

    The outputs are "on" `2g` and "off" `2g + 1`. Timers are one per (record, group) (`0x10001720`).
  - Off (`0x100015d0`) removes the record's timers and fires "off" for every group that is on. The BB itself
    returns "activate next frame" until Off.

## 9. Coverage and confidence

| Area | Functions | Confidence |
|---|---|---|
| Contact point layout, creation, update (4 geometry cases), removal, materials | all of `0x1001e000-0x10020000` that relates to contacts, plus `0x1001a8b0`, `0x1001ffe0`/`0x10020030`, `0x1001d840`/`0x1001d910`, `0x1000bd00-0x1000bf90` | high, except the fs bookkeeping inside `0x10022180` (medium) |
| Friction system: controllers, spring, single-contact normal, energy, easing, merge/split | `0x1001a000-0x1001da00`, `0x1000b000-0x1000b4d0` | high on the live path; dead paths summarized only |
| Friction linear solver | all 78 functions in `0x10033500-0x10037000` | high on algorithm and constants, medium on names |
| Impact solver and impact system | all 41 functions in `0x10022000-0x10024e80` | high, except `0x10030540` (not traced) |

Every algorithm and constant was checked against the disassembly. The remaining uncertainty is in the names and
the flag-bit meanings listed above, not in the arithmetic.
