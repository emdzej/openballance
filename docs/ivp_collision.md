# IVP collision detection in physics_RT.dll (port spec)

This is the port-ready spec of the Ipion (IVP) collision detection embedded in Ballance's `physics_RT.dll`, as
the game exercises it. It covers:

- the compact collision geometry and the ledge tree;
- the OV-tree broadphase, the collision delegators and pair creation;
- the time manager, the event list and the per-object hull manager;
- mindist objects, the mindist manager, and the closest-feature ("minimize") and time-of-impact ("event")
  solvers;
- the hand-over to the friction and impact systems.

It extends `docs/physics.md` (sections 3-6) and corrects its address map in places (section 0.2).

Everything was read from the disassembly of `game/Programmdateien_der_Anwendung/BuildingBlocks/physics_RT.dll`
(image base `0x10000000`). The Ghidra decompile `re/physics_RT.dll.c` was used only as a guide, and no
decompiler output is reproduced here. IVP class and field names in *italics* or in parentheses come from the
public Ipion naming. They fit the code but are hypotheses, because the DLL has no symbols.

## 0. Conventions, corrections, what Ballance exercises

### 0.1 Conventions

- **Addresses** are function entry points, or globals as `0x1007xxxx`. Offsets such as `md+0x54` are byte
  offsets into the original objects. A C port may use its own layout, except where the spec says the byte layout
  matters: the compact ledge format relies on pointer arithmetic.
- **Precision.**
  - `f32` and `f64` are storage types.
  - "ext" means the value is computed on the x87 in 80-bit registers. The event loop sets the precision control
    to a 64-bit mantissa (`0x1002f250`), so intermediates are extended precision and are rounded only when
    stored.
  - A port using SSE `double` for intermediates and rounding to `float` exactly where the spec says f32 is
    deterministic. It is not bit-exact, and nothing can be.
- **Inverse square roots** (`0x1000dae0` f32 argument, 4 Newton steps; `0x1000db80` f64, 5 steps;
  `0x1000dd50` normalize, 4 steps; `0x1000e120` normalize, 5 steps; `0x1000de30` normalize and return length,
  5 steps).
  - The seed is a bit trick: high word `((0x7ff00000 - hi) >> 1) + 0x1ff00000` (arithmetic shift), low word 0.
  - Each step is `y = ((0.5 - (0.5*x)*y*y) + 1)*y`.
  - Measured relative error: 8e-15 after 4 steps, 3e-16 after 5. **A port can use `1/sqrt(x)`**.
  - All normalize helpers return 0 and leave the vector unchanged when `|v|^2 < 1e-19` (`0x10063480`).
- **Asserts.** `mov [0], 0`, a write to address 0, is IVP's assert. Port it as `assert()`/`abort()`.
- **Confidence tags** used in the chapters:
  - **[V]** verified against the disassembly;
  - **[D]** read from the decompile and cross-checked;
  - **[H]** hypothesis, for a name or a role.

### 0.2 Corrections to the address map in docs/physics.md section 4

| Range / address | physics.md | Actually |
|---|---|---|
| `0x10015fe0` | contact distances | *IVP_Mindist_Settings::set_collision_tolerance*, applied to the global settings block at `0x10075db0` (section 4.1) |
| `0x10018000-0x1001a740` | core state sync | mindist-manager list code (`0x10018040-0x10018930`), the object cache (`0x10018930-0x100195f0`), the recalc ("minimize") driver with its 4×4 table at **`0x10075ee0`** (filled by `0x1001a6b0`), ball-ball minimize, the pierce walk |
| `0x1002a540-0x1002a9b4` | case solvers | constraint methods (unrelated) |
| table `0x1007632c` (`0x1002a9c0-0x1002d8e0`) | mindist case solvers | the **event solver**: predicts, from the closest features found by the minimizer, the earliest time within the current PSI at which the pair collides or the closest features change. It never computes distances |
| `0x1002d8e0` | "penetration recovery" constants | **range manager** ctor (env `+0x1c`); its constants are look-ahead ranges (section 2.4) |
| `0x10030860-0x10030c50` | narrowphase | recursive mindist class (not instantiated by Ballance content, 0.3) |
| `0x10030c60-0x10033510` | recursive mindist over ledge trees | the **minimize solver** cases: the closest-feature walk PP, PK, PF, KK, BP, BK, BF |
| `0x10016650` | mindist | the actual ledge-tree "recursion": the **child exchange** used by the OO watcher |
| `0x10037e30-0x10038290` | surface builders | the **OO watcher** (object-object watcher for multi-ledge surfaces) |
| `0x10037790-0x10037d80` | constraint and mindist helpers | the event solver's root finder |

### 0.3 What Ballance actually exercises

These facts shrink the port considerably:

1. **Objects.**
   - Balls are type 3. They have no geometry: a dummy all-zero ledge, with the radius in `obj+0xa0`.
   - Polygons are type 2, with a compact surface.
   - Concave floors are compact surfaces with one 2-triangle "pancake" ledge per face, front and back
     (section 1.6.2).
   - Each convex mesh is one hull ledge.
   - The builder never creates hull ledges for internal tree nodes, because the root-hull option is off.
2. **Pair classes** (`0x1002f430`):
   - Both surfaces are a single ledge (ball, single-convex body): a plain **mindist**. That covers ball-ball,
     ball vs single convex hull, and hull vs hull.
   - Either surface has several ledges (every concave floor, multi-convex PH modules): an **OO watcher**. It
     keeps one plain mindist per ledge pair within range, through the child exchange `0x10016650`.
   - The **recursive mindist is never created**. It needs a ledge with `has_children != 0`, which only the
     disabled root-hull path produces. Port it as an `assert`, or skip it (chapter 3 documents it anyway).
3. **The ball is always synapse 0** of a ball-polygon mindist, with sort flag `k = 0`. The tables assert
   otherwise. For ball-ball, the ball with the larger `obj+0xb0` (entity pointer) is synapse 0. A port needs a
   deterministic replacement, for example the CK id; only the sign of the normal depends on it.
4. **Contact distances are constant.** Every `coll_dist[i]` is 0.01, `real_coll_dist` is 0.001,
   `friction_dist` 0.02, `keeper_dist` 0.023, `max_dist_for_friction` 0.045 and `max_dist_for_impact` 0.22
   (section 4.1).
5. **The hot path** is ball (B) vs floor triangle ledges (P/K/F) under an OO watcher:
   - minimize: BF, BK, BP, and the backside pierce walk to the back face of a pancake;
   - event: BF (0x20), BK (0x30/0x31), BP (0x10/0x11).

   Next come ball-ball (BB) and the convex cases for boxes and PH modules.
6. **Unused paths:** phantoms (`obj+0x1c`), car wheels (`core+0x10`), the universe manager (`env+0x2c` NULL),
   ray casting, the root hull and virtual triangles, and the variable-PSI time mode.

### 0.4 Chapter map

| Ch. | Content | Original source section |
|---|---|---|
| 1 | Compact geometry: layouts, ledge helpers, builders, mass properties | geometry |
| 2 | Broadphase and scheduling: environment, objects, cores, time manager, event list, hull manager, range manager, OV tree, filters, pair creation | broadphase/time |
| 3 | Narrowphase: ledge-tree query, child exchange, OO watcher, recursive mindist, and the **minimize solver** (all cases) | narrowphase |
| 4 | Mindist objects and manager: settings, layouts, lists, recalc driver, event scheduling, the per-PSI passes, friction and impact hand-over | mindist |
| 5 | Event solver, dispatch and root finder; ball cases (and the ball minimize paths in detail) | ball cases |
| 6 | Event solver, convex cases | convex cases |
| 7 | End-to-end scheduling model (consolidated) | |
| 8 | Port mapping | |
| 9 | Open questions | |
| 10 | Coverage and confidence | |

Where two chapters describe the same function, the more specific chapter is authoritative:

- ball minimize: chapter 5;
- convex minimize: chapter 3;
- mindist list, recalc and scheduling: chapter 4;
- OV tree, hull and time: chapter 2;
- geometry: chapter 1.

Known disagreements were reconciled and are noted in place. The struct tables in different chapters agree;
each chapter repeats only the fields it needs.

## 1. Compact collision geometry (compact surface, ledges, ledge tree) and how it is built

Scope: the runtime binary format of IVP's compact geometry as `physics_RT.dll` builds and reads it, the ledge
geometry helpers at `0x10020000-0x10022000`, and the surface builders at `0x10038000-0x1003c990` to the depth
needed to reproduce their output. All offsets are in bytes. "f32" is IEEE float and "f64" is double. Unless a
row says otherwise, everything below was checked against the disassembly. Where it was not, the row says
**unverified**.

### 1.1 Overview

A polygon body references an **IVP_SurfaceManager_Polygon** (`obj+0x8c`). That is an 8-byte object
`{vtable 0x100631e0, IVP_Compact_Surface *cs}`, and the glue caches it by *Collision Surface* name. The compact
surface is one contiguous, 16-byte-aligned block:

```
+0x00  IVP_Compact_Surface header (0x30 bytes)
+0x30  ledge 0: header (0x10) + n_tri*0x10 triangles   } all ledges packed back-to-back
       ledge 1: ...                                     }
       [shared point pool, 0x10 per point]   (only when the surface has >= 2 ledges, see 1.6.5)
+offset_ledgetree_root:  ledge-tree nodes, 0x1c each, preorder (2*n_ledges-1 nodes)
```

A ball body has no compact surface. `obj+0x8c` points at the global ball surface manager at `0x10076380`, and
the radius is the float at `obj+0xa0` (1.7).

### 1.2 Binary layouts

#### 1.2.1 IVP_Compact_Poly_Point (16 bytes, 16-aligned)

| Off | Type | Meaning |
|---|---|---|
| 0x0 | f32 x | object-space (= surface-space) coordinates, entity scale baked in |
| 0x4 | f32 y | |
| 0x8 | f32 z | |
| 0xc | f32 | "hesse"/padding. Always **0.0** in this DLL: the ledge generator writes 0, and the 3-point template copy and the point-pool merge both copy the 0 |

Points are addressed as `point(ledge, i) = (char*)ledge + ledge->c_point_offset + 16*i`.

#### 1.2.2 IVP_Compact_Edge (4 bytes, one u32)

| Bits | Field | Meaning |
|---|---|---|
| 0..15 | `start_point_index` | index into the ledge's point array (`& 0xffff`) |
| 16..30 | `opposite_index` | signed 15-bit offset, **in edges (4-byte units)**, from this edge to the opposite (twin) edge in the neighbouring triangle. Decoded as `(int32)(w << 1) >> 17` |
| 31 | `is_virtual` | always 0 in Ballance (only the unused root-hull path sets it, 1.6.6) |

Navigation is by pointer arithmetic on the edge address. Every edge sits at `tri+4`, `tri+8` or `tri+0xc`
inside a 16-byte-aligned triangle, so `(addr & 0xc) >> 2` is the edge's index 1..3:

- `triangle(e) = (IVP_Compact_Triangle*)(addr & ~0xf)`.
- `next(e) = addr + T_next[addr & 0xc]`, with `T_next` at `0x100685b8` = `{0, +4, +4, -8}`, i.e. 1→2→3→1.
- `prev(e) = addr + T_prev[addr & 0xc]`, with `T_prev` at `0x100685c8` = `{0, +8, -4, -4}`.
- `opposite(e) = (u32*)addr + opposite_index`.
- `ledge(e) = (char*)triangle(e) - 16*(tri_index + 1)`, as used for example in `FUN_10030a30` and `FUN_1001a5c0`.

#### 1.2.3 IVP_Compact_Triangle (16 bytes, 16-aligned)

| Off | Bits | Field | Meaning |
|---|---|---|---|
| 0x0 | 0..11 | `tri_index` | index of the triangle in its ledge (`& 0xfff`) |
| 0x0 | 12..23 | `pierce_index` | index of the "pierce" (opposite-facing) triangle of the same ledge (`>> 12 & 0xfff`) |
| 0x0 | 24..30 | `material_index` | always 0 |
| 0x0 | 31 | `is_virtual` | always 0 in Ballance |
| 0x4 | | edge 0 (`c_three_edges[0]`) | starts at the triangle's first vertex |
| 0x8 | | edge 1 | |
| 0xc | | edge 2 | |

The edges run p0→p1, p1→p2 and p2→p0, with each edge's start point being p0, p1 and p2 respectively. The
outward normal is `(p1-p0) × (p2-p0)`, the right-hand rule over the edge order. `FUN_10021280` computes it as
`(next.start - start) × (prev.start - start)`.

#### 1.2.4 IVP_Compact_Ledge (16-byte header followed by n_triangles triangles)

| Off | Type / bits | Field | Meaning |
|---|---|---|---|
| 0x0 | i32 | `c_point_offset` | byte offset from the ledge header to its point array (may point past the ledge into the surface's shared pool) |
| 0x4 | i32 | `ledgetree_node_offset` / client data | `node - ledge`. **Set only for a ledge attached to an internal node (root hull). It stays 0 for every leaf ledge**, which is every ledge Ballance has |
| 0x8 | bits 0..1 | `has_children_flag` | 1 = hull ledge of an internal node, 0 = terminal (the tree writer clears it for leaves) |
| 0x8 | bits 2..3 | `is_compact_flag` | 1 (the generator ORs in 4) |
| 0x8 | bits 4..7 | dummy | 0 |
| 0x8 | bits 8..31 | `size_div_16` | ledge byte size / 16 *as generated*: `1 + n_tri + n_points`. **Stale after the merge copy** (1.6.5): the copy keeps the original value, although the stored ledge is only `1 + n_tri` blocks long |
| 0xc | i16 | `n_triangles` | |
| 0xe | i16 | `for_future_use` | 0 |
| 0x10 | | triangles[n_triangles] | triangle i at `ledge + 0x10 + 16*i` |

#### 1.2.5 IVP_Compact_Ledgetree_Node (0x1c = 28 bytes, packed, 4-aligned)

| Off | Type | Field | Meaning |
|---|---|---|---|
| 0x00 | i32 | `offset_right_node` | `right - this`. **0 means a leaf.** The left child is always `this + 0x1c` |
| 0x04 | i32 | `offset_compact_ledge` | leaf: `ledge - this`. Internal: `hull - this`, or 0 when there is no hull (always 0 in Ballance) |
| 0x08 | f32[3] | `center` | sphere centre, surface space |
| 0x14 | f32 | `radius` | sphere radius |
| 0x18 | u8[3] | `box_sizes` | AABB half-extent quantised in units of `radius*0.004`: `half_extent_k ≈ box_sizes[k] * radius * 0.004`. Always ≥ the true extent (values 1..251) |
| 0x1b | u8 | `free_0` | 0 |

Terminal-ledge enumeration (`FUN_10020430` and `FUN_10020480`, the latter starting at `cs + cs->offset_ledgetree_root`):

```
collect(node): while node.offset_right_node != 0: collect(node + 0x1c); node += node.offset_right_node
               out.append(node + node.offset_compact_ledge)        # left-to-right DFS order
```

#### 1.2.6 IVP_Compact_Surface (header, 0x30 bytes)

| Off | Type | Field | Meaning |
|---|---|---|---|
| 0x00 | f32[3] | `mass_center` | volume centroid (1.6.7) |
| 0x0c | f32[3] | `rotation_inertia` | **unit** inertia (per kg) about the mass centre, axis-aligned. Note the non-physical formula in 1.6.7 |
| 0x18 | f32 | `upper_limit_radius` | max distance of any terminal-ledge point from `mass_center` |
| 0x1c | bits 0..7 | `max_factor_surface_deviation` | `(u8) trunc(dev / (R*0.004) + 1)`, see 1.6.7 |
| 0x1c | bits 8..31 | `byte_size` | total surface size in bytes |
| 0x20 | i32 | `offset_ledgetree_root` | byte offset from the surface to the root node |
| 0x24 | i32[3] | dummy | 0 |

### 1.3 Access paths at run time

- **Object type**: `obj+4` is **2** for a polygon (vtable `0x10063a84`, ctor `0x1002fd00`) and **3** for a ball
  (vtable `0x10063abc`, ctor `0x10030010`). Both objects are 0xb8 bytes and share the base ctor `0x10009690`.
- **Surface manager**: `obj+0x8c`. The base ctor stores it there.
  - Polygon: the manager the glue passes in, `{0x100631e0, cs}`.
  - Ball: the global at `0x10076380`, `{vtable 0x10063a90, dummy_ledge}`.
- **Extra radius**: `obj+0xa0` is f32.
  - Polygon: `template+0x60`. The glue zeroes the 0x70-byte template (`FUN_10015ef0`), so this is **0**.
  - Ball: `template+0x60 + radius` = **radius** (2.0 for every game ball).
- **Root node**: `cs + *(i32*)(cs+0x20)`.

#### 1.3.1 Polygon surface manager vtable `0x100631e0`

| Slot | Addr | Method | Behaviour |
|---|---|---|---|
| 0 | `0x1000bcd0` | get_single_convex | root is a leaf → its ledge; else root hull if `offset_compact_ledge != 0`; else NULL |
| 1 | `0x1000bc00` | get_mass_center(f32[3] out) | copies `cs+0x00` |
| 2 | `0x1000bc40` | get_radius_and_radius_dev_to_given_center(const f32 c[3], f32 *radius, f32 *radius_dev) | `d = |cs.mass_center - c|` in f32 (computed on x87 and stored to f32). `*radius = cs.upper_limit_radius + d`; `*radius_dev = (f32)(cs.max_factor & 0xff) * cs.upper_limit_radius * 0.004f + d` (0.004f is `0x10063430`) |
| 3 | `0x1000bc20` | get_rotation_inertia(f32[3] out) | copies `cs+0x0c` |
| 4 | `0x1000bb30` | get_all_ledges_within_radius(const f64 c_os[3], f64 radius, const Ledge *root_ledge, …, vector *out) | `root_ledge == NULL`: `query(root)`. Otherwise `node = root_ledge + root_ledge->ledgetree_node_offset`, then `query(node+0x1c)` and `query(node + node.offset_right_node)`. That is the descent into a hull's children and is never used in Ballance. It also computes an AABB `c±r` that nothing uses |
| 5 | `0x1000bbe0` | get_all_terminal_ledges(vector*) | `FUN_10020480` |
| 6 | `0x1000bb00` | insert_all_ledges_hitting_ray | ray solver, 1.5.4 |
| 7, 8 | `0x1000a490` | add/remove reference | no-ops (`ret 4`) |
| 9 | `0x10002da0` | scalar deleting destructor | |
| 10 | `0x1000bcf0` | get_type | returns **0** (polygon) |
| 11 | `0x10003940` | (unidentified) | |

**`query` = `FUN_1000b950(node, c, r, …, out)`** is the sphere-tree descent used by the narrowphase. Its
arithmetic runs on the x87 in extended precision. Node fields are f32; `c` and `r` are f64.

```
loop:
  d = node.center - c                              # per component, extended precision
  R = node.radius + r
  if R*R < |d|^2: return                           # sphere test
  s = (f32)(node.radius * 0.004)                   # 0.004 = f64 0x10063428, result stored to f32
  for k in 0..2:
      if |d_k| >= box_sizes[k]*s + r: return       # box test, strict < keeps
  if node.offset_compact_ledge != 0:               # leaf, or internal node with a hull
      out.append(node + node.offset_compact_ledge); return
  if node.offset_right_node != 0:
      query(node + 0x1c, …); node += node.offset_right_node; goto loop
  # unreachable in practice: both offsets are 0. The original then tests a 2-triangle ledge:
  # point-triangle dist^2 (FUN_10021550) of c against triangle 0, keeping it if <= r^2.
```

Quirk: the 2-triangle exact test at `0x1000ba97` is **dead code**. Leaves always have
`offset_compact_ledge != 0`, so concave triangle ledges are accepted on the sphere and box tests alone.

#### 1.3.2 Ball surface manager vtable `0x10063a90` (global at `0x10076380`, constructed by `0x1002fdf0`)

The object is `{vtable, dummy_ledge}`. `dummy_ledge` is a 32-byte, 16-aligned **all-zero** block allocated with
`FUN_10020100(0x20, 0x10)`: a ledge header with 0 triangles plus one zero triangle.

| Slot | Addr | Behaviour |
|---|---|---|
| 0 | `0x1002fe50` | get_single_convex → dummy ledge |
| 1 | `0x1002fe60` | mass centre → (0,0,0) |
| 2 | `0x1002fe80` | radius: deliberate crash stub. The ball core path (type 3) never calls it; `FUN_10009f00` uses `obj+0xa0` and 0.4·r² instead |
| 3 | `0x10016180` | rotation inertia: crash stub (same reason) |
| 4 | `0x1002fe90` | within-radius: if `!(r*r < |c_os|^2)`, append the dummy ledge |
| 5 | `0x1002ff00` | terminal ledges → dummy ledge |
| 6 | `0x1002ff40` | ray: crash stub |
| 10 | `0x1002ff50` | get_type → **1** (ball) |

### 1.4 Object cache and "cache ledge point" as the helpers see them

Several helpers take `clp` arguments, which correspond to IVP_Cache_Ledge_Point. Only these fields are read:

| Off | Meaning |
|---|---|
| clp+0x0 | `const Poly_Point *points`, the ledge's point array base. These helpers index `points + 16*idx` directly, **not** ledge-relative |
| clp+0x4 | `const IVP_Compact_Ledge *ledge` |
| clp+0x8 | `IVP_Cache_Object *cache` |

IVP_Cache_Object, as read here:

- `cache+0x30` holds `m_world_f_object`, a 3×3 f64 matrix stored row-major as three IVP_U_Points of 4 f64
  each: row 0 at `+0x30/+0x38/+0x40`, row 1 at `+0x50/+0x58/+0x60`, row 2 at `+0x70/+0x78/+0x80`.
- `cache+0x90/+0x98/+0xa0` is the translation.
- The transform is `world = R·p + t`; the inverse is `p = Rᵀ(world - t)`.

### 1.5 Ledge geometry helpers (`0x10020000-0x10022000`)

Notation: `P(e)` is the start point of edge `e` (f32[3]); `N(e)` is `P(next(e))`.

#### 1.5.1 Memory and container utilities

| Addr | Purpose |
|---|---|
| `0x100200c0` | `malloc` wrapper |
| `0x100200d0` | `calloc(n, size)` (malloc + zero) |
| `0x10020100(size, align)` | aligned malloc. Allocates `size+8+align`, writes magic `0x65981234` at the block start, returns `p = (block+align+7) & -align`, and stores the raw pointer at `p-4` |
| `0x10020130(p)` | aligned free: `free(*(p-4))` |
| `0x10020150 / 201b0 / 20210 / 20270 / 20290` (`0x10020140` is a thunk to `0x10020290`) | IVP_U_Memory, a chunked arena (32-aligned chunks of `max(0x7fd8, n)` bytes plus a 0x28 header) used by the environment. Not geometry |
| `0x10020030` | pooled get-or-new of a 0x78-byte friction object (`FUN_1001ffe0`/`FUN_1001a8b0`). Belongs to the friction system |
| `0x10021a20 / 0x10021a50` | growable pointer vector: free, and grow (`cap = 2*cap+1`) |

#### 1.5.2 Single-ledge queries (ledge-relative points: `points = ledge + ledge->c_point_offset`)

**`FUN_100202e0(ledge, f64 min[3], f64 max[3])`**: AABB over the start point of every edge of every triangle.

- min and max start at the first point, triangle 0 edge 0. Each point updates min if smaller, otherwise max if
  larger. The comparisons are f32.
- The results are written as f64. The decompiler output appears to drop `max.y`; it is written.

**`FUN_10021280(edge, ledge, f64 n[3])`**: the unnormalised triangle normal of edge's triangle. The f32 points are
subtracted on the x87 (exact) and stored f64; the cross product is f64 (`0x100212e7..0x10021343`). *(Correction:
earlier revisions said f32.)*

```
a = P(prev e) - P(e), b = N(e) - P(e)
n = ( b.y*a.z - b.z*a.y,  b.z*a.x - b.x*a.z,  b.x*a.y - b.y*a.x )
```

This is `(p1-p0)×(p2-p0)` for `e` = edge 0. `FUN_10021350` is identical but writes f32.

**`FUN_10020a10(ledge, edge, f64 p[3], f32 out[2])`**: unscaled edge parameters. The differences and the dot
products are x87 extended from the f32 points and the f64 `p` (`0x10020a4e..0x10020abc`); only the two results
are stored f32. *(Correction: earlier revisions said all f32.)*

```
d = N(e) - P(e)
out[0] = (p - P(e))·d          # = u·|d|^2,      u = param along e from P(e)
out[1] = (N(e) - p)·d          # = (1-u)·|d|^2
```

`p` is not rounded to f32 (`fsub qword`). Both values ≥ 0 means the projection lies inside the segment.

**`FUN_10021420(ledge, edge, f64 p[3]) -> long double`**: squared distance from `p` to the *infinite line*
through the edge:

```
|(p - P)×(N - P)|^2 * (1.0 / |P - N|^2)
```

It is computed in mixed f32/f64/extended precision; `1.0` is `0x10063288`.

**`FUN_100216f0(ledge, edge, f64 p[3])`**: `|p - P(e)|^2` in extended precision.

**`FUN_10021740(f64 p[3], ledge, edge)`**: squared point-segment distance.

```
(s0, s1) = FUN_10020a10(ledge, edge, p)
if s0 >= 0 and s1 >= 0: return line_dist2(edge, p)            # FUN_10021420
if s0 < 0:  return |p - P(e)|^2
else:       return |p - N(e)|^2                                # via FUN_100216f0(next(e))
```

The decompiler loses the return value of the last two branches.

**`FUN_10020ad0(ledge, edge, f64 p[3], f32 out[4])`**: unscaled barycentric "QR" values of `p` projected onto
edge's triangle, expressed relative to `edge`. The edge vectors and `p - q1` are f64 (qword temporaries), the dot
products and the weights are formed in extended precision (`0x10020b38..0x10020c22`); only the four outputs are
stored f32 (`0x10020c28..0x10020c52`). *(Correction: earlier revisions said everything is f32. In f32 the weight
of a point lying on an edge is rounding noise of about `det * 2^-24` instead of 0; a ball centred over the shared
edge of two large pancake triangles then lands on a backside and fails its recalc.)*

```
t = edge & ~0xf;  q0 = P(t.e0), q1 = P(t.e1), q2 = P(t.e2)      # always the triangle's own order
a = q0 - q1;  b = q2 - q1;  d = p - q1
aa=a·a; bb=b·b; ab=a·b; det = aa*bb - ab*ab
da = d·a;  db = d·b
w0 = da*bb - db*ab           # weight of q0 (unnormalised)
w2 = db*aa - da*ab           # weight of q2
w1 = det - w0 - w2           # weight of q1
out[3] = det
# Permute so that out[k] is the check for the k-th edge counting from `edge`
# (out[k] < 0 means p lies outside edge_k):
#   out[0] = weight of the vertex opposite `edge`
#   out[1] = weight of the vertex opposite next(edge)
#   out[2] = weight of the vertex opposite prev(edge)
```

Implementation: `u = (edge>>2)&3`. The table at `0x100684ff` = `{00,00,08,04,00,08,00}` gives byte offsets: w2
goes to `[T[u]]`, w1 to `[T[u+1]]` and w0 to `[T[u+2]]`. The values are divided by `det` only by the callers
that need true barycentrics.

**`FUN_10021550(ledge, triangle, f64 p[3]) -> long double`**: squared point-triangle distance.

```
c = FUN_10020ad0(ledge, tri+4, p)
if c[0],c[1],c[2] all have the sign bit clear:                  # inside the prism (-0.0 counts as outside)
    n = FUN_10021280(tri+4); h = n·p - n·P(tri.e0)
    return h*h / |n|^2
best = 1e101
for e in (tri.e2, tri.e1, tri.e0):                              # note: reverse order
    (s0,s1) = FUN_10020a10(e, p)
    if s0 < 0:            best = min(best, |p - P(e)|^2)
    elif s1 >= 0:         best = min(best, line_dist2(e, p))
    # s0 >= 0, s1 < 0: nothing. The next edge's start point covers N(e)
return best
```

**`FUN_1001a5c0(edge, f64 p[3])`** (in the friction range, listed here because it is a pure ledge walk) walks
from the **pierce triangle** to the triangle "facing" `p`:

1. Start at `pierce_triangle.e0` and mark visited triangles by `tri_index`.
2. Repeatedly compute `FUN_10020ad0` and look for the first of the 3 edges whose check is `<= 0` and whose
   opposite triangle is not visited. Cross that edge to its opposite.
3. When no such edge exists, return the current edge.

#### 1.5.3 Two-object helpers (they use `clp` points, not ledge-relative)

**`FUN_10020710(edge, clpA, clpB, f64 out[3])`**: point `P(edge)` of A (f32 from `clpA->points`) transformed
into B's object space: `out = R_Bᵀ((R_A·p + t_A) - t_B)`.

- Precision: the products `(f32)R_A[i][j]*p_j` are computed with each f64 matrix entry **rounded to f32**, and
  the row sum is formed in f32 before being widened.
- The inverse half is f64.

**`FUN_100208f0(f32 p[3], clpA, clpB, f64 out[3])`**: the same transform but entirely f64.

**`FUN_10020810(f64 v[3], clpA, clpB, f64 out[3])`**: a direction transform, `R_Bᵀ R_A v`.

**`FUN_100217d0(edge, clp, f64 out[3])`**: `P(edge)` to world via `FUN_10018d10(cache, …)`.

**KK input, `FUN_10020c60(KK *this, edgeK, edgeL, clpK, clpL)`**: K lives in object A and L in object B. Every
vector is expressed in **L's object space**.

| Off | Content |
|---|---|
| +0x00 | `const f32 *L0 = P(L)` |
| +0x04 | `const f32 *L1 = N(L)` |
| +0x08 | f64[3] `K0`, i.e. `P(K)` in L space (`FUN_10020710`) |
| +0x28 | f64[3] `K1`, i.e. `N(K)` in L space |
| +0x48 | f64[3] `Kdir`, i.e. `(N(K)-P(K))` from f32 rotated by `FUN_10020810` |
| +0x68 | f64[3] `Ldir = L1 - L0` (f32 subtraction, widened) |
| +0x88 | edgeK |
| +0x8c | edgeL |
| +0x90 | clpK |
| +0x94 | clpL |
| +0x98 | f64[3] `cross = Kdir × Ldir` (`FUN_1000e280`) |

**`FUN_10020e60(KK*, f32 out[4]) -> int`**: unscaled closest-point parameters of two edges. `out[0..1]` are
`(s, 1-s)·scale` along K from K0; `out[2..3]` are `(u, 1-u)·scale` along L from L0.

```
if |cross|^2 <= 1e-18 (0x10063880):                             # parallel
    tbl = [-1, .5, 2, 0, 1, -.001, .001, .999, 1.001, -1e-6, 1e-6, .999999, 1.000001]   (f32)
    best = 1e101
    for t in tbl[0..10]:                                       # 11 samples on K
        x = lerp(K0, K1, t)   (f64, FUN_1000dc40: (1-t)K0 + tK1)
        d = FUN_10021420(clpL->ledge, L, x)
        if d < best: best = d; out[0]=t; out[1]=1.0f-t; out[2..3] = FUN_10020a10(L, x)
    A0 = FUN_100208f0(L0 → K space); A1 = FUN_100208f0(L1 → K space)
    for t in tbl[0..8]:                                        # 9 samples on L
        x = lerp(A0, A1, t)
        d = FUN_10021420(clpK->ledge, K, x)
        if d < best: best = d; out[2]=t; out[3]=1-t; out[0..1] = FUN_10020a10(K, x)
    return 0
nK = Kdir × cross;  nL = Ldir × cross                          # f64
a = (f32)(L0·nK);  b = (f32)(L1·nK);  c = (f32)(K0·nK)          # L0 and L1 are f32 points
out[2] = (a - c)*(a - b);   out[3] = (c - b)*(a - b)
f8 = nL·K0 (extended);  f9 = nL·K1 (extended);  f3 = (f32)(L0·nL)
den = (f32)(f8 - f9)
out[0] = (f32)((f8 - f3)*den);  out[1] = (f3 - (f32)f9)*den
return 1
```

**`FUN_10020da0(KK*) -> long double`**: squared distance between the infinite lines.

- `|cross|^2 > 1e-24` (`0x100634e8`): `((K0·cross) - (L0·cross))^2 / |cross|^2`.
- Otherwise: `FUN_10021740(K0, ledge(L), L)`.

**`FUN_10021800(edgeK, edgeL, clpK, clpL) -> long double`**: squared distance between two edge **segments**.
Sign tests are on the f32 sign bit, so `-0.0` counts as negative.

```
kk = FUN_10020c60 + FUN_10020e60    →  (k0,k1,l0,l1)
if l0<0 or l1<0:
    if k0>=0 and k1>=0:
        if l0 <= 0: return seg_dist2(L0→Kspace, K)      # FUN_10020710(L, clpL, clpK) + FUN_10021740
        if l1 <= 0: return seg_dist2(L1→Kspace, K)
        crash (unreachable)
    return min(seg_dist2(L0→K), seg_dist2(K0, L), seg_dist2(L1→K), seg_dist2(K1, L))
if k0 <= 0: return seg_dist2(K0, L)
if k1 >  0: return FUN_10020da0
return seg_dist2(K1, L)
```

#### 1.5.4 Ray casting (`FUN_10021ee0` tree walk, `FUN_10021ab0` ledge)

These are used only by `insert_all_ledges_hitting_ray` (vtable slot 6, via the ray solver `0x10022010/30`). The
Ballance gameplay path does not appear to use them (**unverified**: no range or ray BB is in use).

The ray solver object holds:

| Off | Field |
|---|---|
| +0x00 | f64 start[3] |
| +0x20 | f32 centre[3] |
| +0x30 | f64 end[3] |
| +0x50 | f32 dir[3] |
| +0x60 / +0x64 | hit listener and its argument |
| +0x68 | f32 length |

The tree walk prunes nodes whose sphere misses the ray capsule (`radius + 0.5·length`, using the 0.5 at
`0x100634a0`, then the perpendicular distance via `FUN_10021fd0`, a cross product).

The ledge test:

- **2-triangle ledges** test only triangle 0's plane. The normal is flipped by -1 when the start lies behind it.
- **Other ledges** find the first front-facing triangle (dir·n ≤ -1e-12) that the segment crosses, then walk
  across edges using `FUN_10020ad0` checks.

On a hit, the listener's slot 0 is called as `(arg, ledge, triangle, (f64)(length·t), normal)`.

### 1.6 Surface builders (`0x10038000-0x1003c990`, plus the ledge generator at `0x10048b10-0x10048fa0`)

Not in scope: `0x10038000/380e0/38100` is a collision-watcher class with two 0x10-byte synapses (vtable
`0x10063bb8`), not a builder.

#### 1.6.1 Glue entry (in `FUN_10002380`; recap of the order)

1. `FUN_10038290` initialises an IVP_SurfaceBuilder_Ledge_Soup on the stack. It zeroes everything and sets
   `+0x38..0x40 = 1e6f` and `+0x48..0x50 = -1e6f`.
2. **Convex meshes first**, in pin order. `FUN_10007260(builder, mesh, scale)`:
   1. Take the vertex positions (f32).
   2. Remove *exact* duplicates with an O(n²) bitwise `==` on x, y, z, keeping first occurrences in vertex
      order.
   3. Multiply by the entity scale component-wise in f32 and widen to f64.
   4. `FUN_1003ae20(point_list)`. A non-NULL ledge is added with `FUN_10038430` (append to `+0x20/22/24`).
3. **Then concave meshes**, in pin order. `FUN_10006fb0(builder, mesh, scale)` handles each face `f` in index
   order:
   1. `GetFaceVertexIndex(f, &a, &b, &c)`, then the positions of a, b and c in that order.
   2. Multiply by the scale in f32 and widen to f64.
   3. Call `FUN_1003ae20` with exactly three points (p0=a, p1=b, p2=c). This takes the 3-point path in 1.6.2.
   4. Degenerate faces return NULL and are skipped silently. The mesh still counts as "usable".
4. `FUN_100384b0(builder, NULL)` compiles (1.6.4). The result is wrapped as `{0x100631e0, cs}` and cached
   under the surface name.

**Ledge order = insertion order** (convex ledges, then concave faces). This order drives the tree build.

#### 1.6.2 Point soup to ledge: `FUN_1003ae20(list of f64[3]*)`

- Fewer than 3 points: NULL.
- **Exactly 3 points**: `FUN_1003ac00(p0, p1, p2)`, the triangle ledge.
- More than 3 points: `FUN_1003a750`, the convex hull ledge (1.6.3).

**Triangle ledge (`FUN_1003ac00`)**:

1. Reject when `|(p1-p0)×(p2-p0)|^2 < 1e-12` (f64, `0x100634f0`) by returning NULL.
2. Otherwise copy a cached template ledge (built once into `0x100763a8` by `FUN_1003c760` → `FUN_1003c150` from
   dummy points) into a fresh `FUN_10020100(96, 16)` block.
3. Overwrite points 0, 1 and 2 with `(f32)p0`, `(f32)p1` and `(f32)p2`.

The result is fully determined, 96 bytes long:

```
+0x00 header: c_point_offset=0x30, node_offset=0, flags=0x00000604 (size_div_16=6, is_compact=1), n_triangles=2
+0x10 tri 0: word = 0x00001000 (tri_index 0, pierce 1)
       e0: start 0, opposite +4   (→ tri1.e0)
       e1: start 1, opposite +5   (→ tri1.e2)
       e2: start 2, opposite +3   (→ tri1.e1)
+0x20 tri 1: word = 0x00000001 (tri_index 1, pierce 0)
       e0: start 1, opposite -4   (→ tri0.e0)
       e1: start 0, opposite -3   (→ tri0.e2)
       e2: start 2, opposite -5   (→ tri0.e1)
+0x30 points: p0, p1, p2 (f32 x,y,z, 0)
```

Triangle 0 = (p0,p1,p2) has normal `(b-a)×(c-a)`, the mesh face winding. Triangle 1 = (p1,p0,p2) is the back
face. As encoded words: `edge = start | (opp & 0x7fff) << 16`, giving e.g. tri0.e0 = `0x00040000` and
tri1.e0 = `0x7ffc0001`.

The template derivation comes from `FUN_10048960`, which builds a two-sided triangle A=(p0,p1,p2), B=(p1,p0,p2)
with A↔B as pierce partners, followed by the generator rules in 1.6.3. It was derived by hand, not
dump-verified; recheck by dumping `0x100763a8` at run time if you need bit-exactness.

#### 1.6.3 Convex hull ledge (`FUN_1003a750`, `FUN_1003a330`, `FUN_10039cf0`, then polygon → ledge)

**Step 1: deduplicate.** The input points are deduplicated again by a hash of their 24 f64 bytes, keeping
first-occurrence order.

**Step 2: run qhull.** Call qhull `FUN_10046c30(dim 3, n, pts, …)` with the options
`"qhull Qs Pp C-0 W1e-14 E1.0e-18"`. `C-0` merges coplanar facets, so facets are convex **polygons**.

**Step 3: filter degenerate facets** (`FUN_1003a330`). For each qhull facet, in qhull's facet-list order:

1. Collect its vertices and the facet normal.
2. Compute `A = Σ_k n·((v_{k+1}-v_k)×(v_k-v_0))` (`FUN_1003a1e0`). If A < 0, flip n and A.
3. Compute `L2 = Σ |v_{k+1}-v_k|^2` (`FUN_1003a2d0`).
4. If `A < sqrt(L2)*0.005` (`0x10063828`), the facet is a sliver.
   - Unless one of its vertices is already marked, find the vertex farthest from vertex 0, then the vertex
     farthest from that one. Mark the first remaining vertex that is neither of the two for removal.
   - Set the "retry" flag.
5. Retry when a vertex was removed:
   1. Rebuild the point list without the marked points.
   2. If exactly 3 remain, take the triangle-ledge path.
   3. If fewer than 3 remain, give up and return NULL.
   4. Otherwise run qhull again. If no point was removed, switch to joggle mode:
      `"qhull Qs QJ%G C-0 Pp W1e-14 E1.0e-18"` with `QJ` starting at 1e-12 (`0x10063be0`), growing as
      `(QJ + 1e-12) * 1.2` per round (`0x100639a0`), until `QJ >= 0.02` (`0x10063bd8`).

**Step 4: build the polygon template.** When no facet needed fixing, `FUN_10039cf0` builds an IVP polygon
template:

- Points are unique in hull-vertex order.
- Lines are unique `(min,max)` index pairs.
- Each surface is the facet's vertex loop in qhull order, plus a normal.

**Step 5: triangulate.** `FUN_1003c720` → `FUN_1003c080` → `FUN_1003c580`:

1. `FUN_10048320` triangulates every surface polygon in surface order, using the 2-D triangulation at
   `FUN_10047890`/`FUN_10047c00`. That algorithm was **not reproduced**.
2. Each triangle is oriented so that its normal points along the surface normal.
3. Each triangle is **prepended** to the polygon's triangle list.
4. A hidden twin is created per triangle (flag byte `+0x19 = 1`) and excluded later.
5. `FUN_10047170` computes the unit normals.
6. `FUN_10048a60` assigns pierce partners (below).
7. The visible triangles (`+0x19 == 0`) are collected in list order (`FUN_1003c6d0`). That is **reverse
   creation order**: the last facet's last triangle comes first.

**Pierce assignment (`FUN_10048a60`)**:

1. Clear all pierce pointers.
2. For each visible triangle T in list order whose pierce is still NULL:
   1. Find the visible triangle U with the most negative `n_T·n_U` that is also below **-1e-6**. Ties keep the
      first such U in list order.
   2. Set `T.pierce = U` and `U.pierce = T`. This **overwrites** U's previous partner, if any, so pairs are not
      necessarily symmetric.

**Ledge generator (`FUN_10048b60` sizes, `FUN_10048ed0` writes)** takes an ordered list of triangles, each with
3 edges in ring order and opposite pointers.

- **Points**: walk the triangles in order and their 3 edges in ring order. A point gets the next index the first
  time it is seen, so points are in first-appearance order.
- **Triangle i**:
  - `tri_index = i`, `pierce_index = index(pierce)`. A missing pierce prints `"no valid pierce index…"`.
  - `material = 0`, `is_virtual = 0`.
  - Edge k: `start_point_index = index(start point)`.
  - The edge's global slot is `4i+k+1` (its u32 offset from the start of the triangle array).
- **Opposite**: `opposite_index = (slot(opposite edge) - (4i+k+1)) & 0x7fff`.
- **Layout**: header, then triangles, then points (`x,y,z,0`).
  - `c_point_offset = 0x10 + 16·n_tri`.
  - `flags = ((1+n_tri+n_pts) << 8) | 4`.
  - The size is `(n_pts + 1 + n_tri) * 16`.

**Reimplementation**: any correct convex hull whose coplanar facets are merged, triangulated and wound outward,
with edges twinned and pierce assigned as above, produces an equivalent ledge. Triangle and point order then
matter only for tie-breaking, for example which triangle a mindist starts from. qhull itself is not needed.

#### 1.6.4 Compile: `FUN_100384b0(builder, template)`

`template == NULL` selects the defaults built by `FUN_10038490`, the only case Ballance uses:

| Field | Default | Meaning |
|---|---|---|
| `t[0]` | 0 | build root convex hull. Off |
| `t[1]` | 1 | free input ledges after copying |
| `t[2]` | 0 | link to input ledges instead of copying. Off |
| `t[3]` | 1 | merge points into one pool |

```
if n_ledges == 0: return NULL
ledges_to_spheres()           # FUN_10038750
build_tree()                  # FUN_10038a70 → FUN_10038c90 (recursive)
if t[0] && n>1: root hull     # FUN_100385e0. Skipped
allocate_and_copy()           # FUN_10039600 + FUN_100399b0 + FUN_10039820
write_tree()                  # FUN_10039bd0 → FUN_10039a70
mass_props()                  # FUN_10039b30
cleanup()                     # FUN_100386b0
if n_spheres>1 && t[2]==0 && t[3]==1:
    re-copy into an exact FUN_10020100(byte_size, 16) block and free the original   # same bytes
return cs
```

#### 1.6.5 Spheres, tree, layout (exactly reproducible)

**`ledges_to_spheres` (`FUN_10038750`)**: for each ledge `i = 1..n` in insertion order, create a 0x40-byte
sphere record:

| Off | Field |
|---|---|
| +0x00 | index i |
| +0x08 | f64 centre[3] |
| +0x28 | f64 radius |
| +0x30 | u8 box[3] |
| +0x34 | ledge |
| +0x38 | left |
| +0x3c | right |

```
(min,max) = FUN_100202e0(ledge)                      # f32 values widened to f64
center = 0.5*min + 0.5*max                           # FUN_1000dc40(max, min, 0.5)
radius = sqrt(|max - center|^2)                      # f64
box[k] = (u8)( (i32)((max_k - center_k) / (radius*0.004)) + 1 )   # 0.004 = f64 0x10063428, truncation
```

It also keeps a global f32 AABB `center±radius` (unused afterwards), the smallest radius (unused), and the
longest-axis index at `+0x58` (unused). The spheres go into a doubly linked list in index order.

**`build(list)` (`FUN_10038c90`)**, recursive top-down:

```
if |list| == 1: return list[0]                       # leaf = the ledge sphere itself
(bmin,bmax) = union of member boxes                  # FUN_10038b40, f32, init ±1e6f.
    member box_k = [ (f32)(c_k - box_k*(0.004*r)), (f32)(c_k + box_k*(0.004*r)) ]   # 0.004 = 0x10068978
node = new internal sphere
node.center = 0.5*bmin + 0.5*bmax                    # f64 from f32 (FUN_1000dcb0)
node.radius = |(f32)(bmax - (f32)center)|            # per-component f32 difference, length in extended precision
node.box[k] = (u8)((i32)((bmax_k - center_k) / (0.004*radius)) + 1)
if |list| == 2: node.left = build([list[0]]); node.right = build([list[1]]); return node
ext = bmax - bmin (f64)
for axis k in 0,1,2:
    split = bmin_k + ext_k*0.5;  eps = (f64)1e-6f  (0x100637e0)
    low = [], high = [], toggle = 1
    for j, s in enumerate(list):                     # list order
        c = s.center[k]
        if   c < split-eps: low.append(s)
        elif c > split+eps: high.append(s)
        else:
            nb = list[j+1].center[k] if j < |list|-1 else list[j-1].center[k]
            if   nb < split-eps: high.append(s)       # go opposite to the neighbour
            elif nb > split+eps: low.append(s)
            elif toggle == 1:    low.append(s);  toggle = 0
            else:                high.append(s); toggle = 1
    cost[k] = vol(AABB_boxes(low)) + vol(AABB_boxes(high))    # f32 |dx|·|dy|·|dz|. Empty list → (2e6)^3
k* = (cost1 <= cost0) ? (cost1 < cost2 ? 1 : 2) : (cost0 < cost2 ? 0 : 2)
node.left = build(low[k*]); node.right = build(high[k*]); return node
```

Quirk: if the chosen split leaves one side empty (for example all centres on one side, with a huge member box
pulling the midpoint away), the original recurses forever. A port must guard this case, for example by
splitting by median order. It does not occur with Ballance's content in practice (**unverified**).

**`allocate_and_copy` (`FUN_10039600`)** with the defaults:

- `n_nodes = 2n-1`.
- **n == 1** (a single convex mesh): no merging. The ledge is copied whole (header + triangles + its own points,
  `size_div_16*16` bytes) to `cs+0x30`. Its `c_point_offset` is unchanged.
- **n ≥ 2** (several convex meshes, or any concave mesh): **merged points**.
  1. The ledges are copied in sphere-list order (= insertion order) to `cs+0x30`, each taking only
     `(n_tri+1)*16` bytes.
  2. The point pool starts at `pool = cs + 0x30 + 16*(n_ledges + Σn_tri)`.
  3. Every edge's point is looked up by exact 12-byte xyz equality (hash `FUN_1003b4b0` = CRC32 over 12 bytes
     with table `0x10067ad8`, OR `0x80000000`). It is appended to the pool when new, and `start_point_index` is
     rewritten to its pool index.
  4. Each ledge's `c_point_offset = pool - ledge`. Its stale `size_div_16` is kept.
  5. Original ledges are freed (`t[1] == 1`).
- `offset_ledgetree_root = 0x30 + 16*(n_pool_points + n_ledges + Σn_tri)` when merged, else
  `0x30 + ledge_size`.
- `byte_size = offset_ledgetree_root + 0x1c*n_nodes`. The `0x24..0x2c` dummies are 0.

**`write_tree` (`FUN_10039a70`)** writes nodes contiguously from the root offset in **preorder**: node, then its
whole left subtree (which therefore starts at `node+0x1c`), then the right subtree.

```
write(s):
    n = next_slot++
    n.center = (f32)s.center; n.radius = (f32)s.radius; n.box = s.box; n.free_0 = 0
    if s.left:                                      # internal
        n.offset_compact_ledge = s.ledge ? s.ledge - n : 0
        if s.ledge: s.ledge.node_offset = n - s.ledge; s.ledge.flags = (flags & ~3) | 1     # hull only
        write(s.left); r = write(s.right); n.offset_right_node = r - n
    else:                                           # leaf
        s.ledge.flags &= ~3;  n.offset_right_node = 0;  n.offset_compact_ledge = s.ledge - n
    return n
```

#### 1.6.6 Root hull (not used by Ballance)

`FUN_100385e0`, `FUN_1003af10`, `FUN_1003b110` and `FUN_1003b480` build a convex hull over the points of all
ledges and attach it to the root node.

- Triangles that are not present in any input ledge get `is_virtual = 1`.
- Edges that are not present in any input ledge get `is_virtual = 1`.

The default template disables this, so no node has a hull and no virtual bits exist.

#### 1.6.7 Mass centre, inertia, radius (`FUN_10039b30`)

**Step 1: mass centre and inertia.** `FUN_1003bbf0(cs, &mc, &I)` works over all terminal ledges (DFS order).

1. **Accumulate** over all triangles, using `n = (p1-p0)×(p2-p0)` in f32 (`FUN_1003b8e0`):
   - `S2 += |n|^2` (f64)
   - `S6 += n·p0` (f64; 6 × the signed tetra volume to the origin)
   - `C += (n·p0)*0.25f * (p0+p1+p2)` (f32 accumulator; 0.25 is `0x10063498`)
2. **Flat test**: if `S6 <= sqrt(S2)^3 * 1e-9` (`0x10063bf0`), the surface has no volume. This is always true for
   concave triangle soups, because each face's front and back triangles cancel.
   - `mc` = centre of the union AABB of all ledges.
   - `I[0..2] = (0.5*|max-min|)^2 * 0.5`.
3. **Otherwise**: `mc = (f64)((f32)C * (f32)(1/S6))` (`FUN_1003bf40`). For each axis triple
   (A,B,C) = (0,1,2), (1,2,0), (2,0,1), `FUN_1003bb20` integrates over the closed surface with coordinates
   relative to `mc` (`FUN_1000f450`, f32). It uses Green's-theorem edge integrals (`FUN_1003b550`):
   1. Per triangle, take the unit normal n (`FUN_1000e120`).
   2. If `|n_B| <= |n_C|`, use `g = 0` and `a0 = -0.5*n_B/n_C` (or 0 when `|n_C| <= 1e-12`); the integrand is
      `a0·B²`. Otherwise use `a0 = 0` and `g = 0.5*n_C/n_B`; the integrand is `B·C + g·C²`.
   3. Skip any edge with `|d_A| < |d|*1e-12`.
   4. Integrate the polynomial in A along each edge with `[A]`, `[A²/2]`, `[A³/3]`, `[A⁴/4]` and `[A⁵·0.2]`.
   5. The results are `V = ∫dV`, `∫A dV` and `∫A² dV`, and the function returns `m_A = ∫A² dV / V`. When V < 1e-19
      it returns 0, 0 and 1.0 instead.

   This was numerically confirmed on a unit cube: V = 1 and m = 1/12 per axis.

   **Stored unit inertia (quirk):**
   ```
   I[0] = sqrt(m_y^2 + m_z^2),  I[1] = sqrt(m_x^2 + m_z^2),  I[2] = sqrt(m_x^2 + m_y^2)
   ```
   This is not the physical `m_y + m_z`. For a cube it is `1/√2` of the true value (unit cube: 0.1179 instead of
   0.1667). `FUN_10009f00` multiplies it by mass to get the core's inertia, so **ported boxes must use this
   formula to rotate like the original**. Also apply the template `+0x34` minimum-ratio clamp (0.03) that
   follows in `FUN_10009f00`.
4. Store `cs.mass_center = (f32)mc` and `cs.rotation_inertia = (f32)I`.

**Step 2: radius and deviation.** `FUN_10020650(cs, mc, &R, &dev)` works over all terminal ledges and all
triangle vertices p:

- `R = max |p - mc|`
- `dev = max sqrt(|n×(p-mc)|^2 / |n|^2)`, with n the triangle's normal (distance from p to the axis through mc
  along n)
- `cs.upper_limit_radius = (f32)R`
- `cs.max_factor_surface_deviation = (u8)(i32)(dev / (R*0.004) + 1.0)`

### 1.7 Ball geometry

- No compact surface. The radius is f32 at `obj+0xa0`; it is the template's extra radius (0) plus the
  Physicalize radius.
- Unit inertia comes from `FUN_10009f00`: `0.4·r²` per axis (`0x100633b0`).
- The ledge-tree machinery sees the dummy all-zero ledge from the ball surface manager. Ball cases in the
  narrowphase and mindist (synapse status BALL) use the object's centre and `obj+0xa0` rather than geometry.

### 1.8 Port notes

- **Concave surfaces**: one 2-triangle ledge per non-degenerate face, laid out exactly as in 1.6.2. All ledges
  share a merged point pool. The tree is built by 1.6.5. Every step is deterministic and cheap, so reproduce it
  bit-exactly. The mass and inertia fallback in 1.6.7 applies; floors are fixed, so this matters little.
- **Convex surfaces**: a hull with coplanar facets merged, triangulated and wound outward, twins linked, pierce
  assigned by most-anti-parallel normal. One ledge per convex mesh; tree as in 1.6.5. Use the inertia quirk.
- A C port can keep this exact byte format (offsets and bitfields) so that the ported mindist and narrowphase
  code can use the same pointer arithmetic (`& ~0xf`, `& 0xc`, ±16·index). Alternatively, use an index-based
  equivalent that keeps the 16-byte triangle and 4-byte edge encoding.

### 1.9 Open questions

- The 2-D triangulation inside `FUN_10047890` and `FUN_10047c00` (the fan/ear choice, and therefore the triangle
  and point order of convex ledges) was not reproduced. It matters only for tie-breaking.
- Slot 11 of the polygon manager (`0x10003940`) and slot 9 of the ball manager are unidentified (destructor or
  release).
- Whether any Ballance mesh triggers the sliver-facet retry or the joggle path in qhull.
- Whether the tree builder's empty-side recursion can trigger on any level mesh. Run a check over all
  `Phys_Floors*` meshes when porting.
- The template ledge at `0x100763a8` was derived by hand. Dump it once from a running original to confirm the
  edge words in 1.6.2.

## 2. Broadphase, time manager, hull manager and range manager

Scope: OV-tree broadphase and collision delegator (`0x1002d8e0-0x1002f0b0`), time manager / event list and the
classes defined next to it (`0x1002f0b0-0x10030860`), the per-object hull manager (in the real object, updated
from `0x1001e300`/`0x1001eb10`, helpers `0x1001a820`, `0x10018390-0x100185a0`, `0x10017d70`), the range manager
(env `+0x1c`, `0x1002d8e0` ctor - the "penetration recovery" constants of `docs/physics.md` section 5 are in fact
range-manager look-ahead constants), and the end-to-end event model.

All addresses are function entry points in `physics_RT.dll`. "ext" means the x87 computes in 64-bit-mantissa
extended precision (the event loop sets PC=11, section 2.2.5); results are rounded only where they are stored to a
`float`/`double` field. Where the decompiler printed `(float)` casts inside expressions the disassembly was
checked; unless stated otherwise those casts are bogus and the expression is evaluated in extended precision.

Confidence tags: **[V]** verified against disassembly, **[D]** read from the decompile and plausible, **[H]**
hypothesis (IVP naming / role inferred).

---------------------------------------------------------------------------------------------------------------

### 2.1 Struct layouts used by this section

#### 2.1.1 Environment fields (`IVP_Environment`, 0x178 bytes, ctor `0x10012a60`)

| Off | Type | Meaning | |
|---|---|---|---|
| `+0x04` | ptr | time manager (0x20 bytes, `0x1002f0b0`) | V |
| `+0x10` | ptr | mindist manager (0x18 bytes, `0x100186e0`) | V |
| `+0x14` | ptr | OV-tree manager (0x2c0 bytes, `0x1002dff0`) | V |
| `+0x18` | ptr | collision filter (Ballance: the glue's filter chain `0x10014d40`) | V |
| `+0x1c` | ptr | range manager (0x60 bytes, `0x1002d8e0`; the glue passes none so the default is created) | V |
| `+0x20` | ptr | anomaly manager (8 bytes, `0x1002f660`, vtable `0x10063a58`) | V |
| `+0x24` | ptr | anomaly limits (0x14 bytes, `0x1002f5e0`) | V |
| `+0x28` | ptr | performance counter (profiler; slot0 start, slot1 `pcount(id)`, slot2 stop) | V |
| `+0x2c` | ptr | universe manager. **NULL in Ballance** (app-env slot 5 is never set by `0x10006bb0`) | V |
| `+0xc0` | double | `delta_PSI_time` = 1/66 (`0x3f8f07c1f07c1f08`), set by `0x10013240` | V |
| `+0xc8` | double | `inv_delta_PSI_time` = 1/`+0xc0` | V |
| `+0x120` | double | `current_time` (written only by `set_current_time` `0x100138f0`, which also increments `+0x138`) | V |
| `+0x128` | double | `time_of_next_PSI` (ctor: = `+0xc0`) | V |
| `+0x130` | double | `time_of_last_PSI` (ctor: 0) | V |
| `+0x138` | int | time-change counter (ctor 1, ++ per `set_current_time`) | V |
| `+0x144` | int | PSI phase (0 start, 2 after integration, 3 hull done, 4 short done, 5 = idle; ctor 5) | V |
| `+0x158/+0x15a/+0x15c` | u16 cap, u16 n, ptr | vector of collision-delegator roots (Ballance: one, `0x1002f5a0`) | V |

#### 2.1.2 Real object fields used here (`IVP_Real_Object`; ball `0x10030010`, polygon `0x1002fd00`)

| Off | Type | Meaning | |
|---|---|---|---|
| `+0x04` | int | object type: **2 polygon, 3 ball** | V |
| `+0x18` | ptr | environment | V |
| `+0x1c` | ptr | phantom/controller hook; non-NULL turns new mindists into "phantom" mindists (flag `0x1000`). NULL in Ballance | D |
| `+0x20` | ptr | head of the object's EXACT synapse list (synapse `+8` next, `+0xc` prev) | V |
| `+0x24` | ptr | head of the object's INVALID synapse list | V |
| `+0x40` | ptr | cached object (world transform cache, `0x10018930`) | D |
| `+0x48 .. +0x7b` | struct | **hull manager**, section 2.1.6 | V |
| `+0x80` | u32 | object movement state; `& 7 != 0` = moving (simulated); `8` = frozen | V |
| `+0x8c` | ptr | surface manager (vtable slot0 = `get_single_convex()`, slot 0x1c/0x20 = add/remove ledge reference) | V |
| `+0x90` | char[8] | no-collision group string (group filter) | V |
| `+0x9c` | ptr | OV element (NULL = not in collision detection) | V |
| `+0xa0` | float | extra radius (`template+0x60` for polygons; `template+0x60 + radius` for balls) | V |
| `+0xa4` | ptr | physical core | V |
| `+0xa8` | ptr | second core pointer; two objects with equal `+0xa8` never get a pair (same rigid unit) | D/H |
| `+0xb0` | ptr/int | Virtools entity (glue); also used to order ball-ball synapses | V |

The ball is a **point**: its surface manager is the global `0x10076380` (vtable `0x10063a90`, built by
`0x1002fdf0`), which owns one zero-filled 0x20-byte compact ledge (16-byte aligned) and returns it from
`get_single_convex`; all sphere radius lives in `obj+0xa0`. [V]

#### 2.1.3 Core fields used here (`IVP_Core`, 0x238 bytes)

| Off | Type | Meaning | |
|---|---|---|---|
| `+0x00` | byte | flags; `& 0xc` = unmovable/fixed core | V |
| `+0x04` | float | `upper_limit_radius` = `obj+0xa0` + surface radius about the mass centre (`0x10009f00` via `0x1000c4f0`) | V |
| `+0x08` | float | `max_surface_deviation` (radius deviation from the surface manager; ball: 0) | V |
| `+0x0c` | ptr | environment | V |
| `+0x10` | ptr | non-NULL puts new exact mindists into mindist-manager vector `+0xc` (wheel/ball list) | D |
| `+0x20` | float | mass | V |
| `+0x52/+0x54` | u16 n, ptr | objects of this core | V |
| `+0x60` | byte | movement state of the core (`< 8` moving/not static; `0x21` used as a temporary marker by `0x100099f0`) | D |
| `+0x68` | double | `time_of_last_psi` | V |
| `+0x70` | float | `1/delta_PSI` (set every PSI in `0x1001e300`) | V |
| `+0xa4` | float3 | linear velocity (world) | V |
| `+0xb8` | double3 | position at last PSI (`pos += delta*dt` in `0x1001e300`) | V |
| `+0xd8` | float3 | `delta_world_f_core_psis` (velocity used for extrapolation between PSIs) | V |
| `+0x128..+0x180` | double 3x4 | `m_world_f_core` rotation rows at `+0x128`,`+0x148`,`+0x168` (stride 0x20) | V |
| `+0x188` | double3 | translation of that matrix: **mass-centre world position** used for OV centre | V |
| `+0x1a8` | float3 | rotation axis of the last PSI, **world space**, unit: `R_world_f_core · q.xyz/|q.xyz|` ((1,0,0) when no rotation) | V |
| `+0x1b8` | float | `current_speed` = \|v\| (`0x1000e480` of `+0xa4`) | V |
| `+0x1bc` | float | angular speed of the last PSI (rad/s), 2·asin(\|q.xyz\|)·(1/dt) | V |
| `+0x1c0` | float | `max_surface_rot_speed` = `+0x1bc` × `+0x08` | V |

#### 2.1.4 Time manager (0x20 bytes, ctor `0x1002f0b0`)

| Off | Type | Meaning |
|---|---|---|
| `+0x00` | u32 | 0 |
| `+0x04` | ptr | mode object (8 bytes): vtable `0x100635b8` (slot0 `0x1002ef70` event loop, slot1 `0x1002f010` variable step), `+4` stop flag (never set to 1 in this binary) |
| `+0x08` | ptr | event list: `IVP_U_Min_List` (2.1.5), initial capacity 16 |
| `+0x0c` | ptr | the PSI event (8 bytes: vtable `0x10063a2c`, `+4` list index) |
| `+0x10` | double | relative time (to `+0x18`) of the last fired event |
| `+0x18` | double | `base_time`: event keys are `float(t - base_time)` |

The ctor inserts the PSI event at absolute time **0.0**. [V]

An *event* is any object with `vtable[0] = simulate_time_event(env)` and a u32 list index at `+4` (`0xffff` = not
queued). Event classes in Ballance: the PSI event and every mindist (`mindist+4`, vtable slot0 `0x100181b0`). [V]

#### 2.1.5 `IVP_U_Min_List` (sorted list with skip links; `0x100300f0` ctor, `0x10030180` add, `0x10030470` remove)

Header (0x14 bytes):

| Off | Type | Meaning |
|---|---|---|
| `+0x00` | u16 | allocated element count |
| `+0x02` | u16 | free-list head (`0xffff` = full; grow to `2n+1`) |
| `+0x04` | ptr | element array (16 bytes each) |
| `+0x08` | float | `min_value` (value of the first element; **1e10** = `0x501502f9` when empty) |
| `+0x0c` | u16 | first "long" (skip-list) element |
| `+0x0e` | u16 | first element (smallest) |
| `+0x10` | u16 | element count |

Element: `+0` u16 next_long (`0xfffe` = not in skip list), `+2` u16 prev_long, `+4` u16 next, `+6` u16 prev,
`+8` float value, `+0xc` void* payload.

Semantics a port must reproduce [V]:
- `add(payload, value)`: if `value <= min_value` the element becomes the **first** (ties: newest first) and
  `min_value = value`. Otherwise it is inserted **before the first element whose value is `>= value`** (again:
  among equal values the new element goes first). The skip list only accelerates the search (a new skip node is
  added when the linear walk exceeded `3 + skip hops`); it does not affect order.
- `remove(index)`: unlink; if it was first, `min_value` = the next element's value or 1e10.
- Returns/uses the u16 index as handle; the owners store it (`event+4`, `synapse+4`, `ov_element+4`).

A binary heap is **not** equivalent (tie order). Use a sorted doubly linked list or a balanced tree keyed by
`(value, insertion_serial descending)`.

#### 2.1.6 Hull manager (`IVP_Hull_Manager`, embedded in the real object at `obj+0x48`)

Offsets relative to the hull manager (object offset in brackets):

| Off | Obj | Type | Meaning |
|---|---|---|---|
| `+0x00` | `+0x48` | double | `last_time`: time the values below refer to |
| `+0x08` | `+0x50` | float | `gradient`: hull growth per second = (\|v\| + rot_surface_speed) × **1.00001** (`0x10063868`) |
| `+0x0c` | `+0x54` | float | `center_gradient`: centre growth per second = \|v\| |
| `+0x10` | `+0x58` | float | `hull_value` at `last_time` |
| `+0x14` | `+0x5c` | float | `center_hull_value` at `last_time` |
| `+0x18` | `+0x60` | float | `hull_value_next_psi` = `hull_value + gradient·dPSI` |
| `+0x1c` | `+0x64` | int | time of next rebase (`ftol(last_time + 10.0)`) |
| `+0x20` | `+0x68` | Min_List | listeners keyed by the hull value at which they want to be told (`min_value` at `+0x28` / `obj+0x70`) |

The hull value is a monotone "distance budget consumed": an upper bound of how far any surface point of the
object may have moved since `hull_value` was 0. A listener registered with key `hull_value(now) + d` asks to be
woken once the object may have moved `d` more. [V]

Hull listener interface (vtable slots): `[0]` type, `[1]` `hull_limit_exceeded_event(hull_mgr, float intrusion)`,
`[2]` `hull_manager_is_going_to_be_deleted_event`, `[3]` `hull_manager_is_reset(float dhull, float dcenter)`,
`[4]` deleting dtor. Listener `+4` = its Min_List index. Two listener kinds exist:

| Listener | vtable | `[1]` exceeded | `[3]` reset |
|---|---|---|---|
| synapse (mindist+0x18 / +0x34) | `0x100637a0` | `0x10017d10` → `mindist_hull_limit_exceeded` `0x10017d70` | `0x10017cf0` → `0x10017d50` |
| OV element | `0x10063a00` | `0x1002ddc0` → `recheck_ov_element(obj)` `0x10017140` | `0x1001a810` (no-op) |

#### 2.1.7 Synapse (0x1c bytes; two per mindist at `+0x18` and `+0x34`)

| Off | Type | Meaning |
|---|---|---|
| `+0x00` | vtable | `0x100637a0` |
| `+0x04` | u32 | hull Min_List index |
| `+0x08/+0x0c` | ptr | next/prev in the object's EXACT or INVALID synapse list |
| `+0x10` | ptr | real object |
| `+0x14` | ptr | compact edge (`& ~0xf` = triangle; for the ball: ledge+0x14) |
| `+0x18` | s16 | offset synapse → mindist (negative) |
| `+0x1a` | s16 | status: 0 point, 1 edge, 2 face, 3 ball (index into the 4×4 case table) |

#### 2.1.8 Mindist fields consumed by the scheduler (layout owned by chapter 4)

| Off | Type | Meaning |
|---|---|---|
| `+0x04` | u32 | time-manager list index (`0xffff` = no event) |
| `+0x08` | ptr | collision delegator (root) |
| `+0x0c/+0x10` | s32 | index in OV element 0 / 1 collision vector (-1 = none) |
| `+0x14` | u32 | flags: bits 0-7 event hint returned by the case solver; bit 8 synapse order; `0x400/0x800` phantom-listener; `0x1000/0x2000` phantom/"coll listener" (`0x1000` = phantom); `0xc000` invalid/penetration; bits 18-21 status **2 INVALID, 3 EXACT, 4 HULL (recursive), 5 HULL**; bits 22-29 coll-dist step counter |
| `+0x50` | float | sum of both objects' extra radii |
| `+0x54` | float | `len_numerator`: last exact distance (surface-to-surface, minus extra radii) |
| `+0x58` | float | projection of (centre A − centre B) on the contact normal at the last exact/hull check |
| `+0x60` | double | sum of both objects' *angular* hull values (`hull - center_hull`) at the last check |
| `+0x68` | float3 | contact normal of the last exact computation |
| `+0x7c/+0x80` | ptr | next/prev in the mindist manager's EXACT (or INVALID) list |

Mindist manager (0x18 bytes, `0x100186e0`): `+4` env, `+8` EXACT list head, `+0xc/+0xe/+0x10` vector of exact
mindists whose core has `core+0x10` set, `+0x14` INVALID list head. [V]

#### 2.1.9 OV element (0x30 bytes, ctor `0x1002dc00`, vtable `0x10063a00`)

| Off | Type | Meaning |
|---|---|---|
| `+0x00` | vtable | hull-listener interface (2.1.6); `[0]` returns 3 (type OV element) |
| `+0x04` | u32 | hull Min_List index |
| `+0x08` | ptr | OV node containing it (NULL = not in tree) |
| `+0x0c` | ptr | hull manager it is registered in (NULL until first insert) |
| `+0x10` | float3 | centre (world) |
| `+0x20` | float | radius actually used in the tree (ctor: -1.0) |
| `+0x24` | ptr | real object |
| `+0x28/+0x2a/+0x2c` | u16 cap (16), u16 n, ptr | vector of collisions (mindists) of this object |

#### 2.1.10 OV node (0x28 bytes, `0x1002dea0`) and OV-tree manager (0x2c0 bytes, `0x1002dff0`)

Node:

| Off | Type | Meaning |
|---|---|---|
| `+0x00/+0x04/+0x08` | int32 | x, y, z: box lower corner in units of `2^L` |
| `+0x0c` | int32 | `L` ("sizelevel"): cell size `2^L`. The box is `[x, x+2]·2^L` per axis (edge `2^(L+1)`) |
| `+0x10` | int32 | `rasterlevel` = `L+1` |
| `+0x14` | ptr | parent |
| `+0x18/+0x1a/+0x1c` | u16 cap, u16 n, ptr | children (a child has `L-1`; its x is `2px`, `2px+1` or `2px+2`, so ≤ 27 children) |
| `+0x20/+0x22/+0x24` | u16 cap, u16 n, ptr | elements |

Manager:

| Off | Type | Meaning |
|---|---|---|
| `+0x000..+0x280` | double[81] | `powerlist[j] = 2^(j-40)` (so `+0x140` = 1.0) |
| `+0x288..+0x2af` | node | search node (only x,y,z,L,raster used as key) |
| `+0x2b0` | ptr | output vector for collision candidates (NULL = don't collect) |
| `+0x2b4` | ptr | node hash (256 buckets; key = 20 bytes x,y,z,L,raster through a CRC32 table at `0x10067ad8`, OR `0x80000000`; equality compares L,x,y,z) |
| `+0x2bc` | ptr | root node |

The hash is only an identity lookup "node with these coordinates"; any map keyed by `(x,y,z,L)` is equivalent.

---------------------------------------------------------------------------------------------------------------

### 2.2 Time manager and event loop

#### 2.2.1 `0x100138c0` `env_simulate_dt(env, double dt)` [V]
`target = env.current_time + dt` (ext → double); calls `0x1002f250(tm, env, target)`.

#### 2.2.2 `0x1002f250` `tm_simulate_until(tm, env, double target)` [V]
```
cw = fnstcw(); fldcw(cw | 0x300)          # precision control = 64-bit mantissa, rounding unchanged
env.perf->start()
tm.mode->vtable[0](tm, env, target)        # = 0x1002ef70
env.perf->stop()
fldcw(cw)
```

#### 2.2.3 `0x1002ef70` event loop [V]
```
loop:
    m = tm.list.min_value                                   # float
    if !((target - tm.base) > m): break                     # ext compare; equal does NOT fire
    ev = payload of tm.list.first
    tm.list.remove(ev.index); ev.index = 0xffff
    tm.last_rel = (double)m
    env.set_current_time((double)m + tm.base)               # 0x100138f0, ++env[+0x138]
    ev.vtable[0](env)                                       # simulate_time_event
    if tm.mode.stop == 1: break                             # never true in Ballance
env.set_current_time(target)
```
So `env.current_time` jumps to each event's time, then to the frame target. Events fire strictly before
`target`; an event exactly at `target` waits for the next frame. There is no cap on the number of events.

#### 2.2.4 Event list helpers
- `0x1002f1d0 insert_event(tm, ev, double t)`: `ev.index = list.add(ev, float(t - tm.base))` (ext subtract,
  rounded to float). [V]
- `0x1002f200 remove_event(tm, ev)`: `list.remove(ev.index)` (caller resets index). [V]
- `0x1002f220 get_next_event(tm)`: pop first (used only by the dtor `0x1002f170`). [V]
- `0x1002f010` mode slot1 "simulate one variable PSI": fires events until the PSI event is met the second time,
  calling `0x10013240(new_dPSI)` at the first meeting. **Unused by the glue.** [V]

#### 2.2.5 PSI event, `0x1002f2c0` `simulate_time_event(env)` (vtable `0x10063a2c`) [V]
```
env.time_of_last_psi = env.current_time
env.time_of_next_psi = env.time_of_last_psi + env.delta_PSI          # ext, stored double
# rebase the event list
T = env.time_of_last_psi
for e in tm.list (in order): e.value = float(e.value - T)
tm.list.min_value = float(min_value - T)
tm.base = T ; tm.last_rel = 0.0
env.simulate_psi(T)                                                  # 0x10013cb0 (2.5)
tm.insert_event(this, env.time_of_next_psi)                          # key = float(dPSI)
```
**Quirk:** the rebase subtracts the absolute `T`, not `T - old_base`. It is harmless in Ballance because, when the
PSI event fires, nothing else is queued: every mindist event is scheduled strictly before `time_of_next_psi`
(2.2.6), so it has fired (or been removed) before the PSI event, and the empty list's 1e10 stays ~1e10. A port
should store event times as `base + float_offset` with `base = time_of_last_psi` (keep the float quantisation)
and may implement the rebase as `value -= (T - old_base)`; behaviour is identical as long as the list is empty.

The very first PSI fires at time 0.0, as soon as the first frame's target is > 0.

#### 2.2.6 How mindist events are timed (summary; `0x10017870`, details in chapter 4)
`recalc_next_event(m, allow_hull, mode)`:
```
S_rot = A.rot_surface_speed + B.rot_surface_speed                    # core+0x1c0, float sum
S     = A.|v| + B.|v| + S_rot                                       # ext
if m.event_index != 0xffff: tm.remove_event(m)
cd = coll_dist[m.flags>>22 & 0xff]                                   # float table at 0x10075db8
if dPSI*S*2.1 (0x100637f0) + cd < m.len:                             # far: leave the exact list
    if allow_hull: exact_list.remove(m); mindist_to_hull(m, m.len - cd)    # 0x10018480 (2.3.4)
    return
approach = (n·vB - n·vA)                                             # n = m+0x68; v = core+0xa4
         + sqrt(1.001 - (axisA·n)^2)*A.rot_surface_speed + same for B   # axis = core+0x1a8
if approach < settings+0x114 (0x10075ec4) and approach < 1e-19: return   # separating; settings = 0x10075db0 (0x10015fe0)
if (next_psi - now)*approach + cd <= m.len: return                   # cannot touch before next PSI
if phantom (flags & 0x3000 == 0x1000): return
step the coll-dist counter down every 3rd call (global counter 0x10075edc)
res = CASE_TABLE[status(syn0)*4 + status(syn1)](ctx)                 # 0x1007632c; ctx = {S_rot,S,m,env,now,next_psi}
if res.hint == 0: return
t = res.time
if t - now < 1e-6 (0x100637e0):                                       # impact "now"
    if mode == 0: t = now
    else:
        d = m.len - real_coll_dist (0x10075db0)
        if mode == 2 or res.hint & 0xf:
            t = d < 1e-12 ? now + dPSI*0.001 : float(dPSI)*1e-4 + d/S + now     (float arithmetic)
        else:
            t = d < 1e-12 ? now + dPSI*1e-5  : float(dPSI)*1e-7 + d*0.1/S + now
        if t - next_psi >= 0: return
tm.insert_event(m, t);   m.flags.low8 = res.hint
```
The mindist's own event (`0x100181b0`) recomputes the exact distance (`0x10019950`) at the event time, then: if
flags & `0xc000` nothing; else if hint & `0xf` → `recalc_next_event(m,0,1)`; else if `len > cd + 0x10075ed4`
→ `recalc_next_event(m,0,2)`; else **impact** via vtable slot `0x1c` (`0x100240a0`, impact system).

---------------------------------------------------------------------------------------------------------------

### 2.3 Hull manager

#### 2.3.1 Per-PSI update, inside `0x1001e300` (core integration) [V]
After the core is integrated and `core+0x1b8/+0x1bc/+0x1c0` are refreshed, for each object of the core (index
`n-1 .. 0`):
```
h = obj.hull ; now = env.current_time (== this PSI's time) ; dt = (float)dPSI
hull_update(h, now, dt, speed = core.rot_surface_speed + core.|v|, center_speed = core.|v|)   # 0x1001e750
if h.list.min_value - h.hull_value_next_psi < 0.0: check_vector.append(h)                  # 0x1001ecd0
```
`0x1001e750 hull_update` (ext, stores float) [V]:
```
d = now - h.last_time ; h.last_time = now
h.hull_value        += d * h.gradient
h.center_hull_value += d * h.center_gradient          # old center gradient
h.center_gradient    = center_speed
h.gradient           = speed * 1.00001f
h.hull_value_next_psi = h.gradient * dt + h.hull_value
```
Only cores in the controller manager's "moving" vector are integrated, so fixed and sleeping objects' hulls do
not grow. When a core freezes (`0x1000ce20`) every object gets state 8, a `recheck_ov_element`, and its hull
values are advanced to `now` with `gradient = center_gradient = 0`.

Angular speed (`0x1001e870`) [V]: `s2 = |q.xyz|^2` of the PSI rotation quaternion (double). If `s2 <= 1e-19`:
`ang = 0`, axis = (1,0,0). Else `inv = rsqrt(s2)` (bit-trick seed `((0x7ff00000 - hi) >> 1) + 0x1ff00000` in the
high word, then 5 Newton steps `inv = (0.5 - 0.5*s2*inv*inv + 1)*inv`), `x = inv*s2` (= sin θ/2), `a = x + x^3/6
+ 0.40414*x^5` (`0x10063800`, `0x1006386c`; an asin series), `ang = 2*a*core.inv_dt` → `core+0x1bc`; axis =
`R_world_f_core * q.xyz * inv` (rows of `core+0x128` dotted with `q.xyz`; world space) → `core+0x1a8..+0x1b0` (float); `core+0x1c0 = ang * core+0x08`.

#### 2.3.2 Hull phase `0x1001eb10(env, check_vector)` [V]
For each hull manager `h` in the vector, from the last to the first:
```
k = 100
while (d = h.list.min_value - h.hull_value_next_psi) < 0.0:        # float subtract
    h.list.first.payload.vtable[1](h, d)                            # hull_limit_exceeded_event; d < 0 = intrusion
    if k-- < 0: break                                               # at most 102 calls
if (double)h.reset_time < h.last_time:
    hull_reset(h)                                                   # 0x1001a820
    h.reset_time = ftol(h.last_time + 10.0)
```
Each handler must remove or re-key the first listener, otherwise the loop spins until `k` runs out.

`0x1001a820 hull_reset(h)` (every ~10 s per object, only when that object had an event this PSI) [V]:
```
dh = -h.hull_value ; dc = -h.center_hull_value
for e in h.list (in order): e.value += dh; e.payload.vtable[3](dh, dc)
h.list.min_value += dh ; h.hull_value = 0 ; h.center_hull_value = 0 ; h.hull_value_next_psi += dh
```
Synapse reset handler `0x10017d50`: `m.ang_hull_sum(+0x60) = double(float(dh - dc) + float(m+0x60))`.

#### 2.3.3 Mindist hull handler `0x10017d70 mindist_hull_limit_exceeded(m, float intrusion)` [D, key lines V]
```
if m.status == 4: recursive_mindist_hull_exceeded(m); return         # 0x10030b20 (chapter 3)
synA = m.syn[(flags>>8)&1], synB = the other ; A,B = their objects ; cA,cB = cores ; now = env.time
pA = cA.pos_last_psi(+0xb8) + cA.delta(+0xd8) * (now - cA.time_last_psi)   # 0x10012470, double
pB likewise
sA = cA.rot_surface_speed + cA.|v| + 1e-19 ; sB likewise ; S = sA + sB      # ext
if (flags & 0x30000) == 0:
    proj = n·(pA - pB)                       # n = m+0x68 (float3), ext
    ang  = Σ_{X∈{B,A}} (hX.hull_value - hX.center_hull_value) + (now - hX.last_time)*(hX.gradient - hX.center_gradient)
    budget = (m.len - (ang - m.ang_hull_sum)) - (m.proj - proj) + intrusion
    if dPSI * S * 6.0 (0x100637f8) < budget:
        m.ang_hull_sum = ang ; m.proj = float(proj) ; m.len = float(budget) - intrusion
        t = budget / S
        rekey synA in A.hull: key = (now - hA.last_time)*hA.gradient + hA.hull_value + t*sA
        rekey synB in B.hull: key = ... + t*sB
        return
# not enough budget: back to exact checking
remove both synapses from their hulls                                   # 0x10018390
if (flags & 0x3000) == 0x1000: mm.add_phantom(m)  (0x100170a0)
else:                          mm.add_exact(m)    (0x10016f90: EXACT list, exact distance 0x10019950,
                                                   then recalc_next_event(m, allow_hull = (cA|cB).state < 0x21, 0))
```
Meaning: the remaining gap is re-estimated cheaply: the translational approach along the old normal is known
exactly (`proj` change), the rotational part is bounded by the angular hull growth. If the gap still exceeds six
PSIs of worst-case approach, the synapses are re-armed for the time the gap could be closed at speeds `sA`,`sB`.

#### 2.3.4 Moving a mindist into the hull state
- `0x10018480 mindist_to_hull(m, float d)` (from `recalc_next_event`, critic phase) [V]:
  if A not moving (`A+0x80 & 7 == 0`): `hull_insert(m, 0, d)`; elif B not moving: `hull_insert(m, d, 0)`;
  else `s0 = sA + sB*0.1`, `s1 = sB + sA*0.1` (s = rot_surface_speed + |v| + 1e-10f, float),
  `f = d/(s0+s1)`, `hull_insert(m, f*s0, f*s1)`.
- `0x100183c0 hull_insert(m, dA, dB)` [V]: status := 5 (flags `& 0xffd7ffff | 0x140000`); insert synA with key
  `(now - hA.last_time)*hA.gradient + hA.hull_value + dA` (ext → float), synB likewise;
  `m.ang_hull_sum = double( (hB.hv - hB.chv) + (hB.g - hB.cg)*dtB + (hA.hv - hA.chv) + (hA.g - hA.cg)*dtA )`
  in float arithmetic.
- `0x100185a0` / `0x10018540` (phantom / invalid path): same split but static side gets `1e-10`, and the keys are
  `h.hull_value_next_psi + d` (not the "now" value).
- `0x10018390`: remove both synapses from their hull lists.

---------------------------------------------------------------------------------------------------------------

### 2.4 Range manager (env `+0x1c`, 0x60 bytes, ctor `0x1002d8e0(this, env, delete_flag=1)`, vtable `0x100639e0`)

| Off | double | Use |
|---|---|---|
| `+0x10` | 0.5 | intra: look-ahead time |
| `+0x18` | 0.9 | intra: max = factor × min radius |
| `+0x20` | 0.8 | intra: min distance |
| `+0x28` | 10.0 | intra: max distance |
| `+0x30` | 0.1 | intra: min seconds |
| `+0x38` | 1.0 | world: look-ahead time |
| `+0x40` | 5.0 | world: max = factor × radius |
| `+0x48` | 0.5 | world: min distance |
| `+0x50` | 15.0 | world: max distance |
| `+0x58` | 0.1 | world: min seconds |

(`+0x08` delete flag, `+0x0c` env.) These are the "penetration recovery" values of `docs/physics.md` §5.

#### 2.4.1 `0x1002dab0 get_coll_range_in_world(obj)` (slot 1) [V]
```
c = obj.core
s = (double)(c.rot_surface_speed + c.|v|) + 1e-19     # float add, then ext
x = min(s*1.0, c.upper_limit_radius*5.0)
x = max(x, 0.5) ; x = min(x, 15.0)
x = x - dPSI*s
return max(x, s*0.1 + c.upper_limit_radius)           # quirk: the floor includes the radius
```
The final floor `0.1·s + R` (not `0.1·s`) is what the code does: the returned range is never below the core
radius, so every OV sphere has radius ≥ 2R. For a static floor (s≈0) the range is `max(0.5, R)`.

#### 2.4.2 `0x1002d990 get_coll_range_intra_objects(A, B, double *rA, double *rB)` (slot 0) [V]
```
sA = (cA.rot + cA.|v|) + 1e-19 ; sB likewise ; S = sA + sB
x = min(S*0.5, min(cA.R, cB.R)*0.9) ; x = max(x, 0.8) ; x = min(x, 10.0)
x = max(x - dPSI*S, S*0.1)
sA' = sB*0.2 + sA ; sB' = sA'*0.18 + sB          # note: sB' uses the UPDATED sA'; constants are (double)0.2f, 0.18f
f = x / (sA' + sB')     (computed as (1/(sA'+sB'))*x)
*rA = f*sA' ; *rB = f*sB'
```
Only caller: the recursive mindist (`0x10030710`) when it parks itself in the hulls with budget `rA + rB`.

---------------------------------------------------------------------------------------------------------------

### 2.5 Where collision runs inside a PSI (`0x10013cb0 simulate_psi`) [V]

```
perf.pcount ; env.phase = 0 ; step listeners (if any) ; universe manager (NULL) ; env[+0xa8]->vfunc28()
0x10013c40(env)
0x10017790(mm)                 # ball/wheel exact mindists vector: recalc + friction-like hook (chapter 4)
controllers                    # 0x100124c0 -> fills the vector of moving cores
integrate cores                # 0x1001ea50 -> 0x1001e300 per core: integrate, speeds, HULL UPDATE (2.3.1)
env.phase = 2
HULL phase                     # 0x1001eb10 (2.3.2): fires synapse and OV-element hull events
env.phase = 3
SHORT phase                    # 0x10017850: for each EXACT mindist: 0x100187a0 (exact distance, invalid handling)
env.phase = 4
CRITIC phase                   # 0x10017770: for each EXACT mindist: recalc_next_event(m, 1, 1) (2.2.6)
env.phase = 5
```
Between PSIs, the event loop fires mindist events (impacts) and the next PSI event.

---------------------------------------------------------------------------------------------------------------

### 2.6 OV-tree broadphase

#### 2.6.1 `0x100176e0 enable_collision_detection(mm, obj)` [V]
Called from the glue path `0x10009350` (Physicalize with Enable Collision). Deletes an old element, creates a new
OV element (`0x1002dc00`), stores it in `obj+0x9c`, then `recheck_ov_element(mm, obj)`.

#### 2.6.2 `0x10017140 recheck_ov_element(mm, obj)` [V]
`mm+0` is a re-entrancy flag. Callers: 6.1, the OV element's hull event, core freeze `0x1000ce20`,
`0x100099f0`, phantom code.
```
e = obj.ov_element ; if !e: return
ov.remove(e)                                   # 6.6
env[+0x88]++
e.center = float3(core.m_world_f_core.translation)       # core+0x188.. (double -> float)
if mm.in_recursion:                            # only via a universe manager: not in Ballance
    ov.insert(e, R, R, NULL); hull_add(e, obj.hull, 1e-19); return
r = core.upper_limit_radius + range_mgr.get_coll_range_in_world(obj)      # float + ext
(universe-manager callback when env+0x2c != NULL and obj moving: skipped in Ballance)
cand = []
r_used = ov.insert(e, r, r, &cand)             # 6.3 (min == max, so r_used == r)
hull_add(e, obj.hull, r_used - R)              # 0x1002dd00: key = hull_value(now) + (r_used - R)
# reconcile existing collisions with the candidates
build a hash of e.collisions keyed by the partner object (partner = o0 ^ o1 ^ obj)
keep = 0 ; new = []
for c in cand, from last to first:
    other = c.object
    if !(obj.moving or other.moving): continue                 # moving = (+0x80 & 7) != 0
    if obj[+0xa8] == other[+0xa8]: continue                    # also excludes obj itself
    if !env.filter.check(obj, other): continue                 # 6.8
    if a collision with `other` exists at index i:
        if keep < i: swap e.collisions[keep] and [i] (0x100188c0, fixes their +0xc/+0x10 indices)
        keep++
    else: new.append(other)
for i = len(e.collisions)-1 down to keep: delete e.collisions[i]      # mindist dtor (removes itself everywhere)
for other in new, from last to first:
    for root in env.delegator_roots, from last to first:
        if root.object_pairs_may_collide(obj, other): break          # 0x1002f430 (2.6.7)
```
Consequences: a pair exists while the two OV spheres overlap at the last recheck of either object; it is
destroyed when a recheck no longer finds it, when both objects are non-moving (freezing), or when the filter
rejects it. New pairs are created in candidate-collection order.

#### 2.6.3 `0x1002ec10 insert_ov_element(ov, e, double rmin, double rmax, vector *cand)` [V]
```
if !e: return 0.0
r = calc_optimal_box(ov, e, rmin, rmax)        # fills ov.search = {x,y,z,L,raster}; 6.4
e.radius = float(r)
n = hash.find(ov.search)
if !n:
    n = new node(ov.search) ; n.elements.append(e) ; e.node = n
    if !ov.root: ov.root = n ; hash.add(n) ; return r          # first element: no candidates collected
    while !(n.L <= root.L and contains(root, n, root.L - n.L)): expand_root(ov, n)     # 6.5
    if root.L == n.L:                                          # same box as the root
        root.elements.append(e) ; e.node = root ; delete n ; n = root
    else:
        hash.add(n) ; p = find_node(root, n) ; connect_boxes(ov, p, n)
else:
    n.elements.append(e) ; e.node = n
ov.cand = cand
if cand: collect(ov, e, ov.root, n)            # 6.5
return r
```

#### 2.6.4 `0x1002e210 calc_optimal_box(ov, e, double r, double R)` [V]
All arithmetic in ext/double; `floor`/`ceil` from the CRT, then `ftol`.
```
lvl = exponent(2r) + 1             # exponent(x) = ((hi_word >> 20) & 0x7ff) - 0x3ff  (0x1002e150)
if lvl < -40: lvl = -40
loop:
    s = 2^(1-lvl)                  # powerlist[41-lvl]
    for each axis a: lo_a = floor((c_a - r)*s) ; hi_a = ceil((c_a + r)*s)
    if all hi_a <= lo_a + 2:
        search = {lo, L = lvl-1, raster = lvl}
        if R <= r: return r
        s' = 2^(-lvl)              # one level up, with the larger radius
        if all axes ceil((c+R)s') <= floor((c-R)s') + 2:
            search = {lo', L = lvl, raster = lvl+1} ; return R
        return r
    lvl++
```
(Each axis is tested and the loop moves on at the first failing axis.) Ballance always passes `r == R`.

#### 2.6.5 Tree maintenance and queries [V]
- `0x1002e180 contains(A, B, s)` (A coarser by `s` levels): per axis
  `A.x<<s <= B.x <= (A.x<<s) + (2<<s) - 2`.
- `0x1002e4b0 overlaps(A, B, s)` (A coarser): per axis `B.x + 2 > A.x<<s` and `(A.x+2)<<s > B.x` (touching boxes
  do not overlap).
- `0x1002e560 find_node(root, n)`: from the root, repeatedly descend into the **first** child (index order) with
  `contains(child, n, child.L - n.L)`; return the deepest node found.
- `0x1002e5b0 connect_boxes(ov, p, n)`: while `p.raster - n.raster != 1`, create the child `c` of `p` towards `n`:
  with `d = p.raster - n.raster`, per axis `c.x = 2p.x` if `n.x < (2p.x+1)<<(d-1)`, else `2p.x+1` if
  `n.x < (p.x+1)<<d`, else `2p.x+2`; `c.L = p.L-1`, `c.raster = p.raster-1`, `c.parent = p`, append to
  `p.children`, `hash.add(c)`, print "Excessive amount of children" if `p` now has > 27; `p = c`. Finally
  `n.parent = p`, append `n` to `p.children` (same warning).
- `0x1002e7b0 expand_root(ov, n)`: prints "Excessive sizelevel" if `root.raster > 40`. New parent `P`:
  `P.x = root.x / 2` (C division, truncates toward zero), same for y,z; `P.L = root.L+1`, `P.raster =
  root.raster+1`. Per axis with `rem = root.x % 2` (C remainder, sign of the dividend): if `rem == -1`: `P.x -= 1`;
  if `rem == 0` and `n.x * 2^n.L < P.x * 2^P.L` (doubles): `P.x -= 1`. Append root to `P.children`
  ("Mehr als 27 Kinder" check is on the new, empty P), `root.parent = P`, `hash.add(P)`, `root = P`.
- `0x1002ea70 collect(ov, e, cur, target)` (target = e's node):
  ```
  for x in cur.elements, last to first: if |x.c - e.c|^2 <= (x.r + e.r)^2: cand.append(x)     # float; includes e itself
  if target.raster < cur.raster - 1:   # target is deeper than cur's children
      for ch in cur.children (index order):
          if ch == target: collect_all(e, ch)
          elif overlaps(ch, target, ch.L - target.L): collect(e, ch, target)
  else:
      for ch in cur.children (index order):
          if ch == target: collect_all(e, ch)
          elif overlaps(target, ch, target.L - ch.L): collect(e, ch, target)
  ```
- `0x1002e9b0 collect_all(e, node)`: sphere test on `node.elements` (last to first), then recurse into
  `node.children` (last to first).
- `0x1002eec0 remove_ov_element(ov, e)`: `n = e.node`; `e.node = NULL`; remove `e` from `n.elements`
  (order-preserving); then repeatedly `0x1002ee50`: if the node has no elements and no children, unhash it, set
  `root = NULL` if it has no parent, delete it (its dtor `0x1002def0` removes it from the parent's children,
  order-preserving) and continue with the parent.
- `0x1002dd00 hull_add(e, hm, double delta)`: if `e.hull` is set: remove and reinsert in `e.hull` with key
  `float((now - h.last_time)*h.gradient + h.hull_value + delta)`; else `e.hull = hm` and insert likewise.
- `0x1002dde0 / 0x1002de30`: append a collision to / swap-remove it from `e.collisions`, maintaining the
  collision's slot index (`+0xc` for the first element it was added to, `+0x10` for the second; -1 = none).
- `0x1002dc70` element dtor: remove from its hull, `0x10013650(env,obj)`, `remove_ov_element`, free vector.

**Port note.** The OV tree only decides *which pairs get a mindist*; once a pair exists its fate is decided by the
mindist. A port may replace the tree by a brute-force or grid sphere-overlap query **provided** it returns the
same candidate set (all elements whose stored sphere overlaps `e`'s, `<=`) and preserves the reconcile order of
6.2. Exact candidate order only affects mindist creation order, hence tie order of simultaneous events; to be
bit-faithful keep the tree.

#### 2.6.6 OV element sphere and hull semantics (summary)
At each recheck the element sphere = `(core position, R + range_world)` and the element listens on the object's
hull with budget `range_world` (since `r_used - R = range`). So an object's pairs are re-evaluated whenever it may
have moved (`(|v| + |ω|·dev)·1.00001` integrated) by `range_world`, checked one PSI ahead (2.3.2). Static objects
never recheck on their own; their spheres are set when they enter collision detection.

#### 2.6.7 Collision delegator root (`0x1002f5a0`, vtable `0x10063a3c`, 4 bytes) [V]
| Slot | Fn | Purpose |
|---|---|---|
| 0 | `0x1002f3f0` | `collision_is_going_to_be_deleted(c)`: get its two objects, swap-remove `c` from both OV elements |
| 1 | `0x1002f5c0` | dtor |
| 2 | `0x1002f3b0` | `object_is_removed(obj)`: for each collision of obj's element (last to first): `c.vtable[3](root)` → the collision deletes itself |
| 3 | `0x1002f430` | `object_pairs_may_collide(A, B)` |

`0x1002f430 object_pairs_may_collide(root, A, B)`:
```
la = A.surface_manager.get_single_convex() ; lb = B...      # compact ledge or NULL
if !la or !lb:  c = new 0x40-byte compound collision 0x10038000(root, A, B)   # concave/multi-ledge surfaces
else:
    if (la.flags(+8) & 3) == 0 and (lb.flags & 3) == 0:     # no ledge children
         c = new mindist (0x88, 0x10016290(env.mm, root))
    else c = new recursive mindist (0x98, 0x10030860(env.mm, root))
    mindist_init(c, A, B, la+0x14, lb+0x14)                  # 0x10016490 (first edge of each ledge)
A.ov_element.collisions.append(c) ; B.ov_element.collisions.append(c)
return c
```
`mindist_init` [D]: puts a ball always in synapse 0 when the other object is a polygon; for ball-ball the
assignment depends on `obj+0xb0` ordering. Polygons get status from the edge (`0x10016630(..,0)` = point) and add
a ledge reference; balls get status 3. `m.sum_extra_radius = A.extra + B.extra`. Then, if neither object has the
phantom hook, `mm.add_exact(m)` (`0x10016f90`: EXACT list, exact distance, `recalc_next_event(m, allow_hull =
core states < 0x21, 0)`); else phantom path `0x100170a0`.

Which objects get which collision class in Ballance: ball vs concave floor (soup with many ledges) → compound
collision `0x10038000` (chapter 3) which creates per-ledge mindists; ball vs single convex hull →
plain mindist; ball vs ball → plain mindist (both point ledges, no children).

#### 2.6.8 Collision filters (env `+0x18`) [V]
- Chain `0x10014d40` (vtable `0x10063678`, slot0 `0x10014ca0`): calls **every** filter (last to first) and ANDs
  the results; 1 = may collide.
- Group filter `0x10014900`: 1 if either `obj+0x90` string is empty, else `strncmp(a, b, 8) != 0`.
- Exclusive-pair filter `0x10014b30` (glue, `mgr+0x64`): 1 unless the ordered pair is registered in its hash
  (`0x10014b10` builds the key, `0x10014a20` looks it up).

---------------------------------------------------------------------------------------------------------------

### 2.7 Other classes in `0x1002f0b0-0x10030860`

- `0x1002fd00` polygon real object ctor (vtable `0x10063a84`, type 2, `+0xa0 = template+0x60`), dtor
  `0x1002fdc0` (thunk to `0x10009830`). [V]
- `0x10030010` ball real object ctor (vtable `0x10063abc`, type 3, surface manager `0x10076380`,
  `+0xa0 = template+0x60 + radius`). [V]
- `0x1002fde0/0x1002fdf0` static init of the ball surface manager (0x20-byte zero ledge from pool
  `0x10020100(0x20,0x10)`); `0x1002ff00` its `get_all_ledges` (pushes the single ledge). [V]
- `0x1002f5e0` anomaly limits: `+8` max velocity **2000.0f**, `+0xc` **70000** = max collisions per PSI per core
  (checked against `core+0x64` in the impact solver `0x100228c0`; exceeding calls anomaly slot 3 which returns 1
  and sets `core+0x61`), `+0x10` max angular velocity per PSI **π/2** (`0x3fc90fdb`). [V]
- Anomaly manager `0x1002f660` (vtable `0x10063a58`) [V]:
  - `[0] 0x1002f6d0 max_velocity_exceeded(limits, core, float3 *v)`: `v *= 2000*0.99 / |v|`.
  - `[1] 0x1002f720 max_angular_velocity_exceeded(limits, core, float3 *w)`: `w *= inv_dPSI*(π/2)*0.9 / |w|`.
    (Both triggered from `0x1001e300` when `|v|^2 > 2000^2` resp. `|w|^2 > (inv_dPSI·π/2)^2`.)
  - `[2] 0x1002f8f0 inter_penetration(m, A, B)`: if neither core is fixed (`core[0] & 0xc`) → `0x1002f780`.
    If one is fixed and its synapse status < 3 (a polygon): take the moving core's position in the fixed
    object's space, find over the fixed synapse's ledge triangles the plane with the largest signed distance
    (`-1e101` start, default normal (1,0,0)), rotate that normal to world and add `normal * push_speed` to the
    moving core's async velocity change (`0x1000a350`, `core+0x84`, wakes the object) if it is not static.
    Otherwise `0x1002f780`. Caller not located in this range (invalid-mindist handling).
  - `0x1002f780` push apart: `dir = normalize(float3(posB - posA))` (`core+0x188`); for each non-static core
    (`core+0x60` byte < 8) apply impulse `∓mass*push_speed*dir` at `posA` (`0x1000a3e0`) and `0x1000a3a0(obj,
    {(float)dPSI,0,0})`.
  - `[3] 0x1002fcc0` returns 1; `[5] 0x1002fc80 get_push_speed_penetration = 2*|gravity(env+0xd0)|*dPSI`.
- `0x1002fcd0` IVP random: `seed *= 75` (u32, initial 1 at `0x100685b4`), returns `(seed & 0xffff) * 2^-16`
  (float). Used by the sleep counter `0x10013610`. [V]

---------------------------------------------------------------------------------------------------------------

### 2.8 End-to-end scheduling model

1. **Entering collision detection.** Physicalize with Enable Collision → OV element → recheck: sphere
   `(pos, R + range_world)` inserted; all overlapping elements become candidates; each candidate pair passing
   "one is moving, different unit, filter" gets a collision object (mindist / compound) through the delegator.
2. **New mindist.** Exact distance immediately; `recalc_next_event` decides: far (gap > `cd + 2.1·dPSI·S`)
   → HULL state with budget `gap - cd` split between the objects by speed (0 for a static side); near → a case
   solver predicts the time of contact within the current PSI; if one exists before `time_of_next_psi`, a time
   event is queued, otherwise the mindist stays EXACT and is re-predicted every PSI in the CRITIC phase.
3. **Every PSI** (2.2.5, 5): integrate, update each moving object's hull (`gradient = (|v|+|ω|·dev)·1.00001`,
   predicted one PSI ahead), fire hull listeners whose key is below `hull_value_next_psi`:
   - OV element listener → full recheck (new/destroyed pairs);
   - synapse listener → cheap gap re-estimate (2.3.3); re-armed if the gap still exceeds `6·dPSI·S`, else the
     mindist returns to EXACT.
   Then SHORT (recompute exact distances, handle invalid ones) and CRITIC (re-predict, park far ones in the hull).
4. **Between PSIs** the event loop fires mindist events in time order; each recomputes the exact distance at its
   time; at or inside the collision distance it calls the impact system (`vtable[0x1c]`), otherwise it re-predicts
   (`mode` 1 or 2 give the small forward nudges of 2.2.6). Resting contact is not an event: once the impact system
   or friction system takes over, the pair is a friction mindist (`flags 0x3000` paths) handled by the friction
   section.
5. **Sleeping**: freezing a core rechecks its objects as non-moving; pairs with only non-moving partners are
   destroyed; hull gradients are zeroed. Waking resumes hull growth; the next OV recheck recreates pairs.

---------------------------------------------------------------------------------------------------------------

### 2.9 Open questions (this section)

- `obj+0xa8`: exact identity (second core pointer?) — only its equality matters for pair rejection.
- Caller of anomaly `inter_penetration` (slot 2) and the meaning of `0x1000a3a0(obj, {dPSI,0,0})`.
- `0x10075ec4` (= settings `+0x114`), `0x10075ed4` (`+0x124`) and the coll-dist table at `0x10075db8` (`+0x8`) are
  runtime-initialised mindist settings (`0x10015fe0` on the global at `0x10075db0`); values in chapter 4.
- Writer of `m+0x58` (centre projection) and `m+0x68` (normal) at exact-distance time: case solvers / `0x10019950`.
- Whether any event other than PSI and mindist events can be queued across a PSI (would expose the rebase quirk
  of 2.2.5). None found.
- `env+0x9c` (0x30-byte object `0x1002db50`, dtor `0x1002dbb0`) and `0x10013c40` were not identified.

## 3. Narrowphase: ledge-tree recursion, OO watcher and the mindist minimize solver

Chapter 3. Sources: disassembly of physics_RT.dll (image base 0x10000000), `re/physics_RT.dll.c`.
"Verified" means read from the disassembly or a clean decompile. "IVP name" columns are hypotheses from the
public Ipion sources and are only used as labels.

### 3.0 Corrections to the docs/physics.md address map (important for the other sections)

| Range / address | physics.md says | It actually is |
|---|---|---|
| `0x10030860-0x10030b20` + `0x10030540`, `0x10030710`, `0x100309c0`, `0x10030c50` | narrowphase | **recursive mindist** class (IVP_Mindist_Recursive), vtable `0x10063ad0` |
| `0x10030b20` | narrowphase | recursive mindist hull-event re-check |
| `0x10030c60-0x10033510` | narrowphase on compact ledges | **mindist minimize solver case functions** (closest-feature walk: PP, PK, PF, KK, BP, BK, BF) |
| `0x100194e0-0x1001a740` | core state sync | minimize solver **driver**, loop hash, dispatch table `0x10075ee0` (filled by `0x1001a6b0`), BB case, pierce walk |
| `0x1002a540-0x1002d8e0`, table `0x1007632c` | mindist case solvers | the 4x4 table is filled with PP/PK/PF/KK -> `0x1002d6c0`, B×{P,K,F} -> `0x1002d4d0`, BB -> `0x1002d5f0`. It is consulted after the minimize step, so it is most likely the **event solver** (time of next event / impact, IVP_Mindist_Event_Solver). Distances are NOT computed there. |
| `0x1002d8e0` | "penetration recovery constants" | **range manager** constructor (vtable `0x100639e0`); its constants are the range parameters (3.8) |
| `0x10038000-0x10038290`, `0x10037e30-0x10037ff0` | surface builders | **OO watcher** (object-object watcher used when an object has no single convex hull) |
| `0x1000b950`, `0x1000bb30` | (unlisted) | compact surface manager: ledge-tree sphere query |
| `0x10016650` | mindist | **child exchange** for recursive mindists and OO watchers (the actual ledge-tree "recursion") |

### 3.1 Runtime structures as the narrowphase reads them

All offsets are bytes. `f32` float, `f64` double, `i32` int, `p` pointer (32-bit).

#### 3.1.1 Compact ledge, triangle, edge, point (verified from the accessors)

Compact ledge (16-byte header, 16-byte aligned):

| Off | Type | Meaning |
|---|---|---|
| +0x00 | i32 | `c_point_offset`: points array = `ledge + c_point_offset` |
| +0x04 | i32 | `ledgetree_node_offset`: the ledgetree node owning this ledge = `ledge + off` (used for node radius) |
| +0x08 | u8 bits 0-1 | `has_children_flag` (`&3 != 0`: this ledge is a **hull** of a ledge-tree node with children) |
| +0x0c | i16 | `n_triangles` (pierce walk sizes its visited array with it) |
| +0x10 | | triangle[0], then triangle[1] ... (16 bytes each) |

Triangle (16 bytes, 16-aligned): dword 0 = `tri_index:12 | pierce_index:12 | material:7 | is_virtual:1`
(bit 31). Dwords 1..3 = the three edges.

- `tri_index` is the triangle's own index: `ledge = tri - (tri_index+1)*16`.
- `pierce_index`: index of the triangle "on the other side" of the ledge, the start of the pierce walk (3.6.9).
- `is_virtual` (bit 31 of the triangle dword): triangle exists only in a hull, not in real geometry.

Edge (dword): `start_point_index:16 | opposite:15 (signed) | is_virtual:1` (bit 31).

- Edge position in its triangle: `pos = (addr >> 2) & 3`, 1..3. Triangle = `addr & ~0xf`.
- `next(e) = e + T1[e & 0xc]` with `T1 = {0, +4, +4, -8}` (table `0x100685b8`).
- `prev(e) = e + T2[e & 0xc]` with `T2 = {0, +8, -4, -4}` (table `0x100685c8`).
- `opposite(e) = e + 4*sext15(bits 16..30)` (the same edge, reversed, in the neighbouring triangle).
- `point(e) = points + start_point_index*16` (float x,y,z; 4th float unused here).
- "Points around vertex P" walk used by the P cases: start with `e0 = prev(edgeP)` (an edge ending at P),
  then `e = prev(opposite(e))` until `e == e0`; each `e` starts at a neighbour Q of P and ends at P.
- Status code of a synapse with edge `e`: P = `point(e)`, K = segment `point(e) -> point(next(e))`, F = the
  triangle of `e`.

Triangle virtual flag is tested on the triangle dword (`*(e & ~0xf)`), edge virtual flag on the edge dword.

#### 3.1.2 Ledgetree node (28 bytes, verified in `0x1000b950`)

| Off | Type | Meaning |
|---|---|---|
| +0x00 | i32 | `offset_right_node` (0 = terminal) ; right child = `node + off` |
| +0x04 | i32 | `offset_compact_ledge` (0 = none); ledge = `node + off` (hull for inner nodes, the ledge for leaves) |
| +0x08 | f32[3] | sphere centre (object space) |
| +0x14 | f32 | sphere radius |
| +0x18 | u8[3] | box_sizes: half extents of an AABB around the centre in units of `radius * 0.004` |
| +0x1b | u8 | pad |
| +0x1c | | left child (always directly follows) |

#### 3.1.3 Compact surface (verified in the surface manager methods `0x1000bb00-0x1000bcd0`)

| Off | Type | Meaning |
|---|---|---|
| +0x00 | f32[3] | mass centre |
| +0x0c | f32[3] | rotation inertia |
| +0x18 | f32 | upper_limit_radius |
| +0x1c | u32 | low 8 bits `max_factor_surface_deviation`, high 24 bits byte size |
| +0x20 | i32 | `offset_ledgetree_root` (root node = `surface + off`) |

#### 3.1.4 Surface managers (object `+0x8c`)

Polygon surface manager, 8 bytes `{vtable 0x100631e0, compact_surface*}`, built by the glue in `FUN_10002380`.

| Slot | Addr | Purpose |
|---|---|---|
| 0 | `0x1000bcd0` | get_single_convex: root node; terminal -> its ledge; else its ledge if `offset_compact_ledge != 0`; else NULL |
| 1 | `0x1000bc00` | mass centre |
| 2 | `0x1000bc40` | radius and radius deviation to a given centre: `r = upper_limit_radius + |mc - c|`, `dev = (max_factor & 0xff) * upper_limit_radius * 0.004f + |mc - c|` |
| 3 | `0x1000bc20` | rotation inertia |
| 4 | `0x1000bb30` | **get_all_ledges_within_radius** (3.2) |
| 5 | `0x1000bbe0` | all terminal ledges (`0x10020480`) |
| 6 | `0x1000bb00` | (calls `0x10022030`/`0x10022010`, not in scope) |
| 7, 8 | `0x1000a490` | add/remove reference to ledge: no-ops |

Ball surface manager: one static instance at `0x10076380` (vtable `0x10063a90`), constructed by
`0x1002fdf0`. It owns one 32-byte, 16-aligned, all-zero pseudo-ledge (`+4`). A ball synapse's "edge" is that
ledge's `+0x14` (first edge of its first triangle); nothing ever dereferences its geometry.

| Slot | Addr | Purpose |
|---|---|---|
| 0 | `0x1002fe50` | get_single_convex -> the pseudo-ledge |
| 1 | `0x1002fe60` | mass centre (0,0,0) |
| 4 | `0x1002fe90` | within-radius query: adds the pseudo-ledge iff `|observer|² <= radius²` (ball centre is the object origin) |
| 7, 8 | `0x10022170` | no-ops |

#### 3.1.5 Cache object (IVP_Cache_Object, from object `+0x40`)

Obtained by `0x1001a190(obj)`: create via `0x10018930` if `obj+0x40 == 0`; `refcount(+4)++`; if the object is
not static (`(obj+0x80 & 0xff) < 8`) and `cache+0 < env+0x138` (time code) refresh it (`0x10018a40`). The
solver drops the refcount (`+4`) after use.

| Off | Type | Meaning |
|---|---|---|
| +0x00 | i32 | valid-until time code |
| +0x04 | i32 | reference count |
| +0x30 | f64 matrix | `m_world_f_object` rotation, row i at `+0x30 + 0x20*i` (3 doubles + pad each) |
| +0x90 | f64[3] | `m_world_f_object` translation (object origin in world; for balls = ball centre) |
| +0xb0 | f64[3] | core position (mass centre) in world |

Transforms (all double, verified): `0x10018d10` object point (float) to world `M p + t`;
`0x10018ca0` world point to object `Mᵀ(w - t)`; `0x10018ea0` object vector to world `M v`;
`0x10018dc0` world vector to object `Mᵀ v`; `0x1000f3e0(m, w)` = `Mᵀ(w - t)` on a matrix pointer.

#### 3.1.6 Mindist fields used here (full layout is chapter 4's; offsets verified)

| Off | Type | Meaning |
|---|---|---|
| +0x00 | p | vtable (`0x100637b4` plain, `0x10063ad0` recursive) |
| +0x08 | p | collision delegator (owner) |
| +0x0c, +0x10 | i32 | two "fvector index" slots (-1 = none); the owner's child array index is in whichever slot is set |
| +0x14 | u32 | status bitfield: bits 8-9 `sel` (which synapse is "A" for the solver; flipped by `^0x100`); `0xc000` minimize failure bits (`0x4000` = last minimize returned 2 or 3); `0x3000` (`0x1000` set when an object has `+0x1c` listeners: suppresses the failure callback); bits 18-21 list id (`(status<<10)>>28`: 2, 3 = exact, 4 = **recursive parent**, 5); bits 22-29 coll-dist index (init 0x3f) |
| +0x18 | synapse[0] | 0x1c bytes |
| +0x34 | synapse[1] | 0x1c bytes |
| +0x50 | f32 | sum_extra_radius = `obj0+0xa0 + obj1+0xa0` |
| +0x54 | f32 | len_numerator = distance − sum_extra_radius |
| +0x58 | f32 | contact_dot_diff_center = `(coreA_pos − coreB_pos) · n` |
| +0x68 | f32[3] | contact plane `n` (unit, world), pointing from object B toward object A (see 3.6.1) |
| +0x78 | i32 | time code of the last minimize (`== env+0x138` -> skip, return 4) |

Synapse (0x1c): `+4` hull-manager slot index, `+8/+0xc` next/prev in the object's synapse list,
`+0x10` object, `+0x14` edge, `+0x18` i16 `mindist − synapse`, `+0x1a` i16 status
(0 P, 1 K, 2 F, 3 B, 5 = "F, and solver.pos holds a point inside this ledge" marker, transient).

Object fields used: `+4` type (2 polygon, 3 ball), `+0x18` environment, `+0x1c` listener (non-zero ->
`0x1000`), `+0x20` exact-synapse list head, `+0x40` cache, `+0x48` f64 hull t0, `+0x50` f32 hull slope,
`+0x58` f32 hull offset, `+0x68` hull manager list, `+0x80` movement state byte (`&7 == 0` static),
`+0x8c` surface manager, `+0x9c` per-object collision list (`0x1002dde0`), `+0xa0` f32 extra radius
(ball: radius + template extra radius), `+0xa4` core, `+0xb0` id used to order ball-ball synapses.

Core fields used: `+4` f32 upper_limit_radius, `+0x10` "is moving" (non-zero: mindist goes into the
manager's per-PSI array), `+0x68` f64 time of the last PSI, `+0xb8` f64[3] position at that PSI,
`+0xd8` f32[3] linear velocity, `+0x1b8`, `+0x1c0` f32 speed terms (sum = max surface speed).

Environment: `+0x10` mindist manager, `+0x1c` range manager, `+0x20` mindist failure handler,
`+0x84` watcher-check counter, `+0xc0` f64 PSI length, `+0x120` f64 current time, `+0x138` i32 time code.

Mindist settings global at `0x10075db0` (built by `0x10015fe0(0.01)`): `+0x08 + 4*i` coll_dist[i]
(all 0.01), `+0x10c` = **0.02** (used below as the recursive collapse distance), `+0x124` = 0.001.

#### 3.1.7 Recursive mindist (0x98 bytes, ctor `0x10030860`)

| Off | Type | Meaning |
|---|---|---|
| +0x00..0x87 | | plain mindist (ctor `0x10016290`) |
| +0x88 | p | second vtable `0x10063ac8` = collision-delegator interface: [0] `0x100309c0` child_deleted(child), [1] `0x10030c50` delete (thunk, this−0x88) |
| +0x8c | i32 | side to expand (0 or 1; −1 initially) |
| +0x90 | u16 | children capacity |
| +0x92 | u16 | children count |
| +0x94 | p | children array (IVP_Collision*); inline buffer is empty (address `this+0x98`) |

Primary vtable `0x10063ad0`: [0] `0x100181b0` hull event (shared), [1] `0x10016410` get_objects,
[2] `0x10016430` get_ledges, [3] `0x10016190` delete, [4] `0x100308f0` dtor, [5] `0x10028220`
failure callback = **no-op**, [6] `0x10030710` minimize-failed handler, [7] `0x10030540` "do impact"
replacement.

`get_ledges` returns `{ledge(syn0.edge), ledge(syn1.edge)}`; `get_objects` returns `{syn0.obj, syn1.obj}`.

#### 3.1.8 OO watcher (0x40 bytes, ctor `0x10038000`)

| Off | Type | Meaning |
|---|---|---|
| +0x00 | p | vtable `0x10063bb8`: [1] `0x10038240` get_objects, [2] `0x10038260` get_ledges = {0,0}, [3] delete, [4] dtor `0x100380e0` |
| +0x08 | p | delegator (the root delegator that created it) |
| +0x0c, +0x10 | i32 | fvector indices (−1) |
| +0x14 | p | delegator interface vtable `0x10063bb0`: [0] `0x100381d0` child_deleted, [1] `0x10038280` delete |
| +0x18 | hull synapse 0 | 16 bytes: `+0` vtable `0x10063b90`, `+4` hull slot index, `+8` object, `+0xc` watcher |
| +0x28 | hull synapse 1 | same, for object 1 |
| +0x38 | u16 cap (8), u16 count | children (mindists) |
| +0x3c | p | children array (malloc 0x20) |

Hull synapse vtable `0x10063b90`: [0] `0x10037f80` returns 2, [1] `0x10037e20` hull limit exceeded ->
`watcher_check` (`0x10037e30`), [2] `0x10037f50` hull manager going away -> delete watcher, [4] dtor
`0x10037f90` (removes itself from the hull list).

### 3.2 Ledge-tree sphere query

#### `0x1000bb30` polygon get_all_ledges_within_radius(observer_os f64[3], radius f64, root_ledge, other_object (unused), other_ref_ledge (unused), out_list)

```
lo = observer - radius; hi = observer + radius     // computed, unused by the walk
if root_ledge == NULL:
    walk(surface + surface.offset_ledgetree_root, observer, radius, out)
else:
    node = root_ledge + root_ledge.ledgetree_node_offset
    walk(node + 0x1c, ...)          // left child
    walk(node + node.offset_right, ...)
```

#### `0x1000b950` walk(node, observer, radius, out)

Tail-recursive on the right child. All arithmetic in x87 extended precision.

```
loop:
    d = node.center - observer                 // f32 centre minus f64 observer
    R = node.radius + radius
    if |d|² > R²: return                       // accept when |d|² <= R²
    s = (float)(node.radius * 0.004)           // f64 constant 0x10063428, stored as f32
    for axis k in 0..2:
        if !(|d_k| < box_sizes[k] * s + radius): return     // strict
    ledge = node.offset_compact_ledge ? node + offset : NULL
    if ledge: out.add(ledge); return           // hull of an inner node, or a leaf's ledge
    if node.offset_right != 0:
        walk(node + 0x1c, observer, radius, out)
        node = node + node.offset_right; continue
    // terminal node without ledge offset (cannot happen with valid data):
    ledge = node (offset 0); if ledge.n_triangles == 2:
        q = squared distance from observer to its first triangle (0x10021550)
        if radius² < q: return
    out.add(ledge); return
```

Quirk: an inner node that has a hull is reported as the hull and **not descended**. Descent happens later, lazily,
through a recursive mindist on that hull (3.4). The `n_triangles == 2` exact test is dead code in practice.

### 3.3 Pair creation `0x1002f430` (root delegator slot 3, `this` = root delegator)

```
l0 = obj0.surfman.get_single_convex(); l1 = obj1.surfman.get_single_convex()
if l0 == NULL or l1 == NULL:
    c = new OO_Watcher(this, obj0, obj1)                     // 3.5
else if !has_children(l0) and !has_children(l1):
    c = new Mindist(env = obj0+0x18, delegator = this)       // 0x10016290, 0x88 bytes
    mindist_init(c, obj0, obj1, l0+0x14, l1+0x14)            // 0x10016490
else:
    c = new Recursive_Mindist(env, this)                     // 0x10030860, 0x98 bytes
    mindist_init(c, obj0, obj1, l0+0x14, l1+0x14)
obj0+0x9c.add(c); obj1+0x9c.add(c)                           // 0x1002dde0
```

`has_children(l) = (l+8 & 3) != 0`. The ball always has a single convex (its pseudo-ledge). A concave
floor goes to the OO watcher, because the builder never gives the root a hull (chapter 1).

`mindist_init` (`0x10016490`, owned by chapter 4) puts the ball on synapse 0 whenever exactly one
object is a ball (ball-ball: ordered by `obj+0xb0`), sets poly synapses to status P (0) with the given edge
and ball synapses to status B (3), sets `+0x50 = obj0.extra + obj1.extra`, then inserts the mindist into the
manager (`0x10016f90` exact list + immediate minimize, or `0x100170a0` with `0x1000` if an object has a
listener).

### 3.4 Child exchange `0x10016650` (the actual recursion)

Signature (cdecl, verified from the call sites `0x10030ad6` and `0x10037e90`):
`exchange(obj0, obj1, f64 dist, children_vec*, fixed0, fixed1, expand0, expand1, delegator)`.

- `fixedK != NULL`: object K contributes exactly that ledge.
- Otherwise object K is queried below `expandK` (NULL = from the root).

```
env = obj0.env
// candidate ledges of obj0
if fixed0 == NULL:
    c1 = obj1.core; p = c1.pos + c1.vel * (env.time - c1.t_psi)      // 0x10012470, f64
    r  = obj0.extra_radius + c1.upper_limit_radius + dist
    p_os = obj0.cache.m_world_f_object⁻¹ (p)                          // 0x1000f3e0
    A = obj0.surfman.get_all_ledges_within_radius(p_os, r, expand0, NULL, fixed1)
else A = [fixed0]
// candidate ledges of obj1, symmetric (observer = obj0's core, r = obj1.extra + c0.upper_limit_radius + dist)
B = ... [fixed1] or query(obj1, root = expand1, other_ref = fixed0)

// open-addressing hash of the existing children, key = child.get_ledges()
need = (|A|*|B| + n_children)*2 + 2
size = 0x400; if need < 0x400: while size > need: size >>= 1          // largest pow2 <= need
if size > n_children: table on the stack (0x400 shorts), mask = size-1
else: size' = smallest pow2 > 2*n_children starting from size; heap table of size' shorts; mask = size'-1
fill table with -1 (int16)
for i = n_children-1 down to 0: insert i at hash(child[i]) with linear probing (+1 & mask)
hash(l0, l1): h = (l1*0x4b) ^ l0;  return ((int)h >> 8) * 0x3ff + h        // 32-bit wrap, arithmetic shift

keep = 0; new_list = []
for a in A from last to first:
    for b in B from last to first:
        probe from hash(a, b): for each slot s with value i != -1:
            if child[i].get_ledges() == (a, b):
                if i > keep: swap children keep and i (0x100188c0, updates the
                             children's fvector index slots) and fix both hash slots
                keep++
                found = child[i] (non-NULL) -> skip creation
                break
        if not found:
            if !has_children(a) and !has_children(b): m = new Mindist(env, delegator)
            else:                                     m = new Recursive_Mindist(env, delegator)
            mindist_init(m, obj0, obj1, a + 0x14, b + 0x14)
            new_list.push(m)
for i = n_children-1 down to keep: if child[i]: child[i].delete()    // child dtor calls delegator.child_deleted -> array shrinks
for m in new_list from last to first:
    children.push(m); m.set_fvector_index(-1 -> index)                // 0x10016d60
```

Consequences:

- **Children persist** across re-checks as long as their ledge pair is still within range. Their synapses,
  and so the friction contacts hanging off them, survive. Only pairs that leave the range are destroyed.
- New children start at the ledges' first edge with status P (or B). The first minimize walks them to the
  closest features.

### 3.5 OO watcher

#### `0x10038000` constructor(root_delegator, obj0, obj1)

```
init hull synapse 0 (0x10037fb0): obj = obj0; slot = obj0.hull.insert(syn, hull_value(obj0, env.time) + 1.0000000200408773e20)
init hull synapse 1 likewise for obj1        // effectively "never", overwritten at once
children = vector(cap 8)
watcher_check(this)                           // 0x10037e30
```

`hull_value(obj, t) = (t − obj.f64[+0x48]) * obj.f32[+0x50] + obj.f32[+0x58]` (the hull manager's linear
growth function; see chapter 2). The f32 result is the list key.

#### `0x10037e30` watcher_check

```
env.counter_84++
range(obj0, obj1, &r0, &r1)                    // env+0x1c vtable[0], 3.8
exchange(obj0, obj1, r0 + r1, &children, 0, 0, 0, 0, &this->deleg_iface)
t = env.time
obj0.hull.remove(syn0.slot); syn0.slot = obj0.hull.insert(&syn0, (float)(hull_value(obj0, t) + r0))
obj1.hull.remove(syn1.slot); syn1.slot = obj1.hull.insert(&syn1, (float)(hull_value(obj1, t) + r1))
```

It runs again when either object's hull grows past its key, i.e. after that object has moved by up to `rK`.
It never computes a distance itself. It only keeps the set of ledge-pair mindists, with the whole surface as
root, matching the current sphere overlap.

Destructor (`0x10038100`): deletes all children, notifies its own delegator (vtable[0]), frees the array and
removes both hull synapses (`0x10037f60`).

`0x100381d0` child_deleted(child): idx = child.fvec[0] if valid and `arr[idx] == child`, else child.fvec[1];
`count--`; move the last element into idx and update the moved child's matching index slot; set the removed
child's slot to −1. (`0x100309c0` is the same for the recursive mindist array.)

### 3.6 Recursive mindist methods

#### `0x10030860` constructor(env, delegator)

Plain mindist ctor (`0x10016290`: env counters `+0x78++`, `+0x7c++`), then vtables, `+0x8c = −1`, empty
children.

#### `0x10030910` destructor

`delete_children` (`0x100307f0`), free the array, plain mindist dtor (`0x100162f0`).

#### `0x100307f0` delete_children

`for i = count-1 down to 0: if child[i]: child[i].delete(1)`; free the heap buffer (if not inline),
cap = count = 0.

#### `0x10030540` vtable[7]: impact reached on the hull pair

Called by the shared hull event `0x100181b0` when, after a minimize, `+0x54 <= coll_dist[idx] + 0.001`
and the mindist is in no friction state (`status & 0xf == 0`). The plain mindist would do an impact
(`0x100240a0`). The recursive one first checks whether the closest features are real:

```
s0 = syn0.status; s1 = syn1.status; E0 = syn0.edge; E1 = syn1.edge
virtE(e) = bit31(*e); virtT(e) = bit31(*(e & ~0xf))
switch s0:
 case P, B: switch s1:
     P, B: return do_impact()                      // 0x100240a0, real contact
     K:    if virtE(E1) side = 1 else return do_impact()
     F:    if virtT(E1) side = 1 else return do_impact()
 case K: switch s1:
     P, B: if virtT(E0) side = 0 else return do_impact()     // note: tests the triangle flag
     K:    v1 = virtE(E1); v0 = virtE(E0)
           if !v1: if v0 side = 0 else return do_impact()
           elif !v0: side = 1
           else side = (radius(node(L0)) > radius(node(L1))) ? 0 : 1    // expand the bigger
     F:    assert (write to address 0)
 case F: if virtT(E0) side = 0 else return do_impact()       // s1 not examined
this.side = side
expand(this)
```

`expand` (shared tail, also in `0x10030710`):

```
mindist_manager.remove_from_exact(this)                // 0x10018230
range(obj0, obj1, &r0, &r1)
arm_hull_split(this, (float)(r0 + r1))                 // 0x100185a0: split by speed, 1e-10 for static objects
status = (status & 0xffd3ffff) | 0x100000              // list id 4 = recursive parent
exchange_children(this, r0 + r1)                       // 0x10030a30
```

#### `0x10030710` vtable[6]: minimize failed (arg = mindist manager)

```
L0 = ledge(E0); L1 = ledge(E1)
if has_children(L0):
    side = (!has_children(L1) or radius(L0) > radius(L1)) ? 0 : 1
else side = 1
expand(this)            // with manager = arg
```

`radius(L) = f32 at (L + L.ledgetree_node_offset + 0x14)`.

#### `0x10030a30` exchange_children(this, f64 dist)

```
s = this.side
expand_s = ledge(syn[s].edge);  fixed_other = ledge(syn[1-s].edge)
exchange(obj0, obj1, dist, &this.children,
         fixed0 = (s == 1 ? ledge(syn0) : NULL), fixed1 = (s == 0 ? ledge(syn1) : NULL),
         expand0 = (s == 0 ? ledge(syn0) : NULL), expand1 = (s == 1 ? ledge(syn1) : NULL),
         delegator = this + 0x88)
```

The expanded side's hull is replaced by the hulls or leaves of its two children that lie within reach. Only one
side is split per step. A child that is still a hull pair becomes another recursive mindist and splits again
when it reaches collision distance. So the tree is descended lazily, one level per "hull contact".

#### `0x10030b20` re-check on a hull event (list id 4)

Called from the mindist hull event `0x10017d70` when `(status & 0x3c0000) == 0x100000`.

```
minimize(this)                                    // 0x100197f0
if (status & 0xc000) == 0 and this.len_numerator > 0.02 (settings+0x10c):
    collapse(this); return                         // 0x10030af0
range(obj0, obj1, &r0, &r1)
env.counter_84++
exchange_children(this, r0 + r1)
t = env.time
obj0.hull.remove(syn0.slot); syn0.slot = obj0.hull.insert(&syn0, (float)(hull_value(obj0, t) + r0))
obj1.hull.remove(syn1.slot); syn1.slot = obj1.hull.insert(&syn1, (float)(hull_value(obj1, t) + r1))
```

#### `0x10030af0` collapse

```
delete_children(this)
mgr = obj0.env.mindist_manager
mgr.remove_from_recursive_list(this)              // 0x10018390
mgr.insert_exact(this)                            // 0x10016e30: list id 3, link synapses into obj+0x20 lists,
                                                  // add to the per-PSI array if either core is moving
status &= ~0x3000
```

The collapsed parent is again an ordinary hull mindist, scheduled by the mindist manager.

#### Vtable[5] `0x10028220`: no-op

A recursive mindist never reports minimize failures (penetration) to the environment. It splits instead.

### 3.7 Scheduling summary for the narrowphase

1. The OV-tree reports an object pair, and the root delegator creates a watcher, a hull mindist or a plain mindist
   (3.3).
2. A hull or plain mindist is minimized immediately and handed to the mindist manager. The manager decides from
   distance and speeds when it is next checked (hull synapses or the per-PSI exact list; chapter 4).
3. When a hull mindist reaches collision distance (impact path, 3.6 slot 7) or fails (slot 6), it becomes a
   recursive parent: hull synapses are armed with `range = r0 + r1`, split by speed, and children are created
   for the near sub-ledges.
4. While a parent's hull-pair distance stays ≤ 0.02 m, each hull event re-runs the exchange with the current
   range and re-arms each object's synapse with its own `rK`. Beyond 0.02 m all children are deleted.
5. A watcher never measures distance. It re-runs the exchange whenever either object has moved by its share of
   the range.

### 3.8 Range manager (`0x1002d8e0`, vtable `0x100639e0`, at env+0x1c) — owned by chapter 2

Only vtable[0] `0x1002d990` get_coll_range_intra_objects(o0, o1, &r0, &r1) is used here.

```
s0 = core0.f32[0x1c0] + core0.f32[0x1b8] + 1e-19      // f64
s1 = core1.f32[0x1c0] + core1.f32[0x1b8] + 1e-19
sum = s0 + s1
r = min(sum * K10, min(core0.upper_limit_radius, core1.upper_limit_radius) * K18)
r = max(r, K20); r = min(r, K28)
r = r − env.psi_length(+0xc0) * sum
r = max(r, sum * K30)
a0 = s1 * 0.2000000029802322 + s0        // f64 0x100639f8
a1 = a0 * 0.1800000071525574 + s1        // f64 0x100639f0 (uses the new a0: quirk)
f = r / (a0 + a1)
r0 = f * a0; r1 = f * a1
```

Constants (doubles in the object): K10 = 0.5, K18 = 0x3feccccc_c0000000 (≈0.9), K20 = 0x3fe99999_a0000000
(≈0.8), K28 = 10.0, K30 = 0x3fb99999_a0000000 (≈0.1). `+0x38..+0x5c` (1, 5, 0.5, 15, 0.1) belong to
vtable[1] `0x1002dab0` (not used by this section).

### 3.9 Minimize solver

#### 3.9.1 Solver object (on the stack, 0x83c bytes)

| Off | Type | Meaning |
|---|---|---|
| +0x000 | p | mindist |
| +0x004 | i32 | loop countdown: decremented before every case step. While ≥ 0 no loop check; once negative every step goes through the loop hash. `0x100197f0` starts at 0 (always hashed), `0x10019950` at 20 |
| +0x008 | f64[3] | `pos`: point in the coordinates of the synapse marked status 5 (result code 3) |
| +0x028 + 8k | u32[2] | loop hash entries, k < 256 |
| +0x828 | i32 | entry count |

Info block per polygon side (built by the driver): `{+0 points base, +4 ledge, +8 cache, +0xc object,
+0x10 synapse}`. Ball side: `{+0 cache, +4 object, +8 synapse}`.

Return codes: 1 = converged, 2 = failure (loop detected, more than 256 steps, or coincident points),
3 = backside (a point is inside the other convex ledge; `pos` and status 5 mark it), 4 = already done this
time code, 5 = illegal-case stub.

#### 3.9.2 `0x100196f0` check_loop_hash(solver, statusA, edgeA, statusB, edgeB)

```
a = statusA | edgeA; b = statusB | edgeB          // edges are 4-aligned, statuses 0..3
hi = max_signed(a, b); lo = the other
if any stored entry == (hi, lo): return 1          // searched from newest to oldest
if count > 0xff: return 1
store (hi, lo); count++; return 0
```

Call sites (`(sA, eA, sB, eB)`): PF `(0,P,2,F)`; KK core `(1,KA,1,KB)`; BP `(3,0,0,P)`; BK `(3,0,1,K)`;
PP `(0,PA,0,PB)`; PK `(0,P,1,K)`. Each case step does `if (--solver.count_04 < 0 && check_loop_hash(...))
return 2` first.

#### 3.9.3 `0x100197f0` minimize (recalc_mindist), and `0x10019950` (variant)

```
minimize(m):
    if m.time_code_78 == env.time_code: return 4
    m.time_code_78 = env.time_code
    solver = {m, count = 0, hash empty}
    loop:
        A = syn[sel], B = syn[sel ^ 1]
        rc = TABLE[A.status*4 + B.status](&solver)            // 0x10075ee0
        if rc == 1: m.status &= ~0xc000; return 1
        m.status = (m.status & ~0x8000) | 0x4000
        if rc == 2: break
        if rc == 3: pierce_fix(&solver); break                // 0x10019790
        assert
    if (m.status & 0x3000) != 0x1000 and (m.status & 0x3c0000) != 0x100000:
        m.vtable[5]()                                         // plain: env+0x20 handler; recursive: no-op
    return rc
```

`0x10019950` is identical except `count = 20`, and on rc 3 it calls `pierce_fix` and retries (at most 2
pierce fixes). The table index there is `syn[sel].status*4 + syn[sel^1].status` (via `0x10019a80`). It is
used by the hull event and by the manager's immediate insert.

`0x10019790` pierce_fix: choose `syn[sel]` if its status is 5, else `syn[sel^1]`. Set its
`edge = pierce_walk(edge, solver.pos)` and its status to 2 (F).

#### 3.9.4 Dispatch table `0x10075ee0` (`0x1001a6b0`), index `status(syn[sel])*4 + status(syn[sel^1])`

| A \ B | P | K | F | B |
|---|---|---|---|---|
| P | `0x10019ff0` | `0x10019ff0` | `0x10019ff0` | `0x1001a1f0` (assert, then swap + BALL) |
| K | `0x1001a350` (swap sel, `0x10019ff0`) | `0x10019ff0` | `0x1001a6a0` (assert, returns 5) | `0x1001a1f0` |
| F | `0x1001a350` | `0x1001a6a0` | `0x10019ff0` | `0x1001a1f0` |
| B | `0x1001a220` | `0x1001a220` | `0x1001a220` | `0x1001a370` |

`mov [0], 0` is IVP's assert (crash). Port as `assert()`.

#### 3.9.5 `0x10019ff0` poly-poly entry

```
A = syn[sel], B = syn[sel^1]
infoA = {points(ledge(A.edge)), ledge(A.edge), cache(A.obj), A.obj, &A}; infoB likewise
switch A.status*4 + B.status:
  0: PP(A.edge, B.edge, infoA, infoB)        // 0x10032350
  1: PK(A.edge, B.edge, ...)                 // 0x10032da0
  2: PF(A.edge, B.edge, ...)                 // 0x10030c60
  5: KK(A.edge, B.edge, ...)                 // 0x10031440
  default: FF_init(A.edge, B.edge, ...)      // 0x10019aa0, reached by F-F
release both caches
```

#### 3.9.6 `0x1001a220` ball (syn0) vs P/K/F (syn1)

Ball info = `{cache(obj0), obj0, &syn0}`; poly info from syn1. Then by `syn1.status`: 0 -> BP
`0x10032050`, 1 -> BK `0x10032860`, 2 -> BF `0x10030d50`, else assert. The ball is always syn0 here (mindist
init guarantees it), and `sel` is ignored.

#### 3.9.7 `0x1001a370` BB

```
c0 = cache(obj0), c1 = cache(obj1)
v = (f32)(c0.pos(+0x90) − c1.pos)                      // f64 difference stored into m+0x68 as f32
q = |v|² (f32)
inv = (|q| <= (f32)1e-19) ? 1.0 : invsqrt5(q)          // 5 Newton steps, see 3.10
m.len_numerator = inv*q − m.sum_extra_radius            // = |v| − r0 − r1
m.n = v * inv
m.dot_diff_center = (c0.core − c1.core) · m.n
return 1
```

The normal points from object 1 toward object 0.

#### 3.9.8 `0x10019aa0` FF_init (brute-force restart for F-F)

```
best = 1e101
for each vertex a of triA, each vertex b of triB:            // world positions (0x100217d0)
    if |a − b|² < best: best = it; A := P(a), B := P(b)
for (face, pts) in [(triB, triA), (triA, triB)]:
    plane = normalized hesse plane of face in its object space (0x10021210: n, d)
    for each vertex p of pts:
        X = p in face space; if F-values(X) all ≥ 0 (sign bits):
            q = (n·X + d)²; if q * 1.000000000001 < best: best = q; P(p), F(face)
for each edge ea of triA, each edge eb of triB:
    kk = kk_setup(ea, eb, A, B); r = kk_params(kk)
    if r[0], r[1], r[2], r[3] all ≥ 0 and kk_qlen(kk) * 1.000000000001 < best: best = it; K(ea), K(eb)
first = A-info, second = B-info; if second.status == P and first.status != P: swap
make sel point at first's synapse
dispatch: (P,P) PP, (P,K) PK, (P,F) PF, (K,K) KK, else assert
```

The factor `1.000000000001` (f64 `0x10063820`) prefers earlier, lower-dimensional candidates on ties.

#### 3.9.9 `0x1001a5c0` pierce_walk(edge, pos) -> edge

```
L = ledge(edge); visited[L.n_triangles] = 0 (stack, alloca)
t = triangle at index pierce_index(tri(edge)); e = first edge of t
loop:
    visited[tri_index(e)] = 1
    w = F-values(pos, e)                                   // 3.10, w[k] for edge e+k
    for k in 0..2 (ek = e, next(e), next(next(e))):
        if w[k] <= 0 and !visited[tri(opposite(ek))]:
            e = opposite(ek); continue loop
    return e                                               // pos projects inside, or nowhere new to go
```

#### 3.9.10 Case functions (all `thiscall(solver, ...)`; X_inB = X transformed from A's object space to B's)

Notation: `info.syn.edge = e; info.syn.status = s` is written `set(info, e, s)`. "flip sel to make I first"
means `if I.synapse != syn[sel]: m.status ^= 0x100`. `invsqrt4`/`invsqrt5` and `qlen*` are in 3.10.
All dot products use f32 point data promoted to extended precision. Results are stored as f32.

Normal convention for every case: `n` points from the B-side object toward the A-side object, where A is the
first info argument. Every converged case also sets `m.dot_diff_center = (A.core − B.core) · n` (cache
`+0xb0`).

**PF entry `0x10030c60`(P, F, A, B)**

```
X = point(P) in B space (0x10020710)
w = F-values(X, F)
if any w < 0:
    pick the edge e of tri(F) (F, next, next²) with minimal qlen_PK(X, e) (first minimum, start 1e101)
    return PK(P, e, A, B)
return PF_core(P, X, F, A, B)
```

**PF core `0x10030f80`(P, X, F, A, B)** (X = P in B space)

```
loop check (0, P, 2, F)
h = hesse(F) (0x10021280, f64, unnormalized), normalize4(h)      // 0x1000dd50
nW = B.M h; nA = A.Mᵀ nW
dist = h·X − h·point(F)
m.len_numerator = dist; m.n = nW
if dist < 0: nA = −nA                                            // f64 −1.0
m.len_numerator −= m.sum_extra_radius
m.dot_diff_center = (A.core − B.core)·n
best = 0; bestE = NULL
for each edge e around P (starts at neighbour Q, ends at P):
    proj = (Q − P)·nA
    if proj < 0: val = invsqrt4(|Q−P|²) * proj; if val < best: best = val; bestE = e
if bestE == NULL:
    set(A, P, 0); set(B, F, 2)
    if len_numerator + sum_extra_radius >= 0: return 1
    solver.pos = X; set(B, F, 5); return 3                       // P is behind F
XQ = point(bestE) in B space; w = F-values(XQ, F)
if all w ≥ 0: return PF_core(bestE, XQ, F, A, B)
if exactly one w[k] < 0: return KK(bestE, F_k, A, B)            // F_k = edge k of F, in walk order
among k with w[k] <= 0 pick minimal qlen_KK(bestE, F_k) (0x10021800, start 1e101): return KK(bestE, F_k, A, B)
```

**PK `0x10032da0`(P, K, A, B)**

```
X = P in B space; (u, v) = K-values(X, K)        // u = (X−K0)·d, v = (K1−X)·d
if u < 0: return PP(P, K, A, B)                  // K's start point
if v < 0: return PP(P, next(K), A, B)
return PK_core(P, K, A, B)
```

**PK core `0x10032e60`(P, K, A, B)**

```
loop check (0, P, 1, K)
X = P in B space; K0 = point(K); d = point(next K) − K0
t1 = point(prev K) − K0;  t2 = point(prev(opposite K)) − K0
n1 = d × t1; n2 = t2 × d                                  // planes of K's triangle and its neighbour
s1 = (X−K0)·n1; s2 = (X−K0)·n2
w1 = F-values(X, K); w2 = F-values(X, opposite K)          // w[0]: "inside across K"
if w1[0] > 0:
    if w2[0] <= 0 or s2 <= 0: return PF(P, K, A, B)       // PF entry with K's triangle
    return PF(P, opposite K, A, B)
if w2[0] > 0: return PF(P, opposite K, A, B)
// edge region
if s1 < 0 and s2 < 0:
    set(A, P, 0); set(B, K, 5); solver.pos = X; return 3
c = d × (X − K0); id = 1/|d|²; q = |c|² * id              // squared distance to the line
if q <= 1e-19:
    m.len_numerator = −sum_extra_radius
    m.n = normalize5(any_perpendicular(d))                // 0x1000f870; QUIRK: left in B object space, no world transform
else:
    s = invsqrt5(q); m.len_numerator = s*q − sum_extra_radius
    nW = B.M (d × c);  m.n = nW * (−s*id)                 // unit, from the edge toward P
m.dot_diff_center = ...
// is a neighbour of P closer? (nW unscaled points from P toward the edge)
nA = A.Mᵀ nW (unscaled); best = q * 1e-12; bestE = NULL
for each edge e around P: proj = (Q−P)·nA
    if proj > 0: val = invsqrt4(|Q−P|²) * proj; if val > best: best = val; bestE = e
if bestE == NULL: set(A, P, 0); set(B, K, 1); return 1
if best < 1e-8:                                            // nearly parallel: avoid flip-flopping
    kk = kk_setup(K, bestE, B, A); kk_params(kk, r)
    if r[2] < 0: set(A,P,0); set(B,K,1); return 1          // r[2..3] = params along bestE; strict, -0.0 is not < 0
return KK(bestE, K, A, B)
```

(When `q <= 1e-19` the neighbour loop still runs. It uses `c = d × (X − K0)`, which is in B object space,
tiny but initialised, and rotates it with `A.Mᵀ` as if it were a world vector, against the threshold
`q·1e-12`. The walk is deterministic but meaningless. The stored normal in this branch stays in B object
space. Recommended port: treat the branch as converged, `set(A,P,0); set(B,K,1); return 1`, and rotate the
normal to world. This is a deliberate, harmless deviation; it only occurs with exact contact on the edge
line. Reviewed in 3.13.)

**PP `0x10032350`(PA, PB, A, B)**

```
loop check (0, PA, 0, PB)
WA, WB = world points; XA_inB = B-space of WA; XB_inA = A-space of WB
q = |WA − WB|²; if q <= 1e-12: return 2
s = invsqrt5(q); m.len_numerator = s*q − sum_r; m.n = (WA − WB)*s; dot_diff_center
best = 0
for side in [ (PA, A, other = XB_inA), (PB, B, other = XA_inB) ]:
    v = other − P; base = P·v
    for each edge e around P: proj = Q·v − base
        if proj > 0: val = invsqrt4(|Q−P|²) * proj; if val > best: remember (e, side)
if found and K-values(other, e).v > 0:
    if K-values.u >= 0: flip sel to make the other side first; return PK_core(otherP, e, other_info, side_info)
    flip sel to make this side first; return PP(e /*point Q*/, otherP, side_info, other_info)
set(A, PA, 0); set(B, PB, 0); return 1
```

**KK `0x10031440`(KA, KB, A, B)**

```
kk = kk_setup(KA, KB, A, B); kk_params(kk, r)     // r[0],r[1]: along KA; r[2],r[3]: along KB
if r[2] >= 0 and r[3] >= 0:
    if r[0] < 0: return PK(KA, KB, A, B)          // KA's start
    if r[1] >= 0: return KK_core(KA, KB, kk, r, A, B)
    return PK(next KA, KB, A, B)
if r[0] >= 0 and r[1] >= 0:
    flip sel to make B first
    return PK(r[2] >= 0 ? next KB : KB, KA, B, A)
// both out of range
(pa, qa) = (r[1] <= r[0]) ? (next KA, KA) : (KA, next KA)     // pa: candidate end, qa: the other
(pb, qb) = (r[3] <= r[2]) ? (next KB, KB) : (KB, next KB)
(u, v) = K-values(pa in B space, KB); if u ≥ 0 and v ≥ 0: return PK(pa, KB, A, B)
(u2, v2) = K-values(pb in A space, KA)
if u2 ≥ 0 and v2 ≥ 0: flip sel (B first); return PK(pb, KA, B, A)
if u*r[2] < 0: return PP(pa, qb, A, B)                         // r[2], not r[3] (3.13)
if u2*r[0] < 0: flip sel (B first); return PP(pb, qa, B, A)
return PP(pa, pb, A, B)
// note: the entry ignores kk_params' return value; in the parallel case the sampled r[] are used as if real
```

**KK core `0x100317a0`(KA, KB, kk, r, A, B)**

```
loop check (1, KA, 1, KB)
c = kk.cross (dirA_inB × dirB, f64); s = (KB0 − KA0_inB)·c (f32 math); neg = signbit(s)
inv = invsqrt5(|c|²)
nW = B.M c; nA = A.Mᵀ nW
m.len_numerator = |s * inv| − sum_r
m.n = nW * ((neg − 0.5f)*2 * inv)               // sign so that n points from B toward A
m.dot_diff_center = ...
KB0_inA, KB1_inA = KB's ends in A space
// four adjacent faces: the two triangles of KA (owner A) and of KB (owner B)
E0 = {tri opposite(KA), other edge KB,          point KB0_inA, owner A, vec nA}
E1 = {tri KA,          other opposite(KB), point KB1_inA, owner A, vec nA}
E2 = {tri opposite(KB), other KA,          point KA0_inB, owner B, vec c}
E3 = {tri KB,          other opposite(KA), point KA1_inB, owner B, vec c}
for i in 0..3: flag[i] = signbit(hesse(Ei.tri)·Ei.vec) ^ (i>>1) ^ neg
best = −4e-12; pick = none
for i in 0..3:
    j = flag[i] ^ i ^ neg; k = i ^ neg
    v = E[k].point − E[k^1].point
    dot = v·hesse(Ei.tri)
    if dot < 0:
        c_ = invsqrt4(|v|²) * dot * invsqrt4(|h|²)
        if c_ < best and F-values(E[j].point, Ei.tri)[0] > 0:
            best = c_; pick = (tri = Ei.tri, owner = Ei.owner, other = E[j].other_edge, other_info = E[j].owner_of_other, w = F-values)
if none:
    set(A, KA, 1); set(B, KB, 1)
    if flag[0] + flag[1] == 2: set(A, KA, 5); set(B, KB, 2); solver.pos = lerp(KB0_inA, KB1_inA, r[2]/(r[2]+r[3])); return 3
    if flag[2] + flag[3] == 2: set(A, KA, 2); set(B, KB, 5); solver.pos = lerp(KA0_inB, KA1_inB, r[0]/(r[0]+r[1])); return 3
    return 1
flip sel to make other_info first
if all w ≥ 0: X = start(other) in owner space; return PF_core(other, X, tri, other_info, owner)
if w[2] >= 0: return KK(other, next(tri), other_info, owner)
if w[1] >= 0: return KK(other, prev(tri), other_info, owner)
q1 = qlen_KK(other, next(tri)); q2 = qlen_KK(other, opposite(prev(tri)))
    // original bug (medium-high confidence): the second call's first edge is a stack address (&flag[4]),
    // not `other`. Port it with `other`, the evident intent (3.13)
return KK(other, q2 < q1 ? prev(tri) : next(tri), other_info, owner)
```

`lerp(a, b, t) = a*(1−t) + b*t` (`0x1000dc40`). The E0..E3 field assignment was rebuilt from the
stack layout at `0x10031a15-0x10031b28`. An independent rebuild confirmed it (3.13), so confidence is high.

The ball cases below (BP, BK, BK core, BF) are also covered in detail by `chapter 5`
(chapter 5). If the two disagree, check the disassembly. The summaries here are kept for cross-reference.

**BP `0x10032050`(ball, P, B)** (ball = A)

```
loop check (3, 0, 0, P)
W = world(P); C = ball centre (cache +0x90); X = C in B space
v = C − W; len = normalize(v) (0x1000de30, 5 steps; 0 if |v|² < 1e-19)
m.n = v; m.len_numerator = len − sum_r; dot_diff_center
u = X − P; base = P·u; best = 0
for each edge e around P: proj = Q·u − base
    if proj > 0: val = invsqrt4(|Q−P|²)*proj; if val > best: best = val; bestE = e
if bestE and K-values(X, bestE).v > 0:
    if K-values.u >= 0: return BK(ball, bestE, B)
    return BP(ball, bestE /*point Q*/, B)
set(B, P, 0); return 1
```

**BK `0x10032860`(ball, K, B)**: `X = C in B space; (u, v) = K-values(X, K)`; `u < 0` -> BP(K);
`v < 0` -> BP(next K); else BK_core.

**BK core `0x10032910`**: as the PK core without the neighbour walk:

```
loop check (3, 0, 1, K)
compute s1, s2, w1[0], w2[0] as in PK core with X = ball centre
if w1[0] > 0: return BF(ball, (w2[0] <= 0 or s2 <= 0) ? K : opposite K, B)
if w2[0] > 0: return BF(ball, opposite K, B)
if s1 < −1e-19 and s2 < −1e-19: solver.pos = X; set(B, K, 5); return 3
c = d × (X − K0); id = 1/|d|²; q = |c|²*id; s = invsqrt5(q)
m.len_numerator = s*q − sum_r; m.n = B.M(d × c) * (−s*id); dot_diff_center
set(B, K, 1); return 1
```

**BF `0x10030d50`(ball, F, B)**

```
X = C in B space; w = F-values(X, F)
if any w < 0: pick edge e of F with minimal qlen_PK(X, e); return BK(ball, e, B)
h = normalize5(hesse(F)); nW = B.M h; m.n = nW
dist = h·X − h·point(F)
set(B, F, 2)
if dist >= 0: m.len_numerator = dist − sum_r; dot_diff_center (ball core − B core); return 1
set(B, F, 5); solver.pos = X; return 3                    // ball centre behind the face
```

BF has no loop check and does not modify the ball synapse.

### 3.10 Compact-ledge solver helpers used (semantics; precise code is chapter 1's)

| Addr | Name here | Result |
|---|---|---|
| `0x10020710` | pos_other_space(P, from, to) | `to.Mᵀ(from.M p + from.t − to.t)`, f64 |
| `0x10020810` | vec_other_space | same, rotation only |
| `0x100208f0` | float point other space | same for an f32[3] |
| `0x10020a10` | K-values(X, e) -> f32[2] | `d = K1−K0`; `[0] = (X−K0)·d`, `[1] = (K1−X)·d` (extended precision, results stored f32) |
| `0x10020ad0` | F-values(X, e) -> f32[4] | `P0,P1,P2` = starts of the triangle's edges 0,1,2; `u=P0−P1`, `v=P2−P1`; `a=(X−P1)·u`, `b=(X−P1)·v`; `det=|u|²|v|²−(u·v)²`; `s=a|v|²−b(u·v)`, `t=b|u|²−a(u·v)`. `out[3]=det`; weights `t` (P2), `det−s−t` (P1), `s` (P0) are stored so that `out[k]` belongs to edge `e+k` (byte tables at `0x100684ff`/`0x10068500`/`0x10068501`: pos 1 -> slots (0,8,4), pos 2 -> (8,4,0), pos 3 -> (4,0,8) for (t, det−s−t, s)). `out[k] < 0` = outside across edge `e+k` |
| `0x10021280` | hesse(e) | `(B−A)×(C−A)`, A = point(e), B = point(next e), C = point(prev e), f64 math (exact differences of the f32 points), stored f64 |
| `0x10021210` | hesse plane | normal and offset of the triangle, normalized (`0x1000e680`, `0x1000e740`) |
| `0x10021740` | qlen_PK(X, e) | inside the segment: `|(X−K0)×d|²/|d|²`; `u<0`: `|X−K0|²`; else `|X−K1|²` |
| `0x10021420` | point-line qlen | `|(X−K0)×d|² / |d|²` |
| `0x10020c60` | kk_setup(KA, KB, A, B) | `+0x08/+0x28` KA ends in B space, `+0x48` dirA in B, `+0/+4` pointers to KB's points, `+0x68` dirB, `+0x88/+0x8c` edges, `+0x90/+0x94` infos, `+0x98` `dirA × dirB` |
| `0x10020e60` | kk_params(kk, r) -> bool | if `|cross|² <= 1e-18`: parallel fallback by sampling t ∈ {−1, .5, 2, 0, 1, −.001, .001, .999, 1.001, −1e-6, 1e-6} on KA (11) and the first 9 on KB, minimizing point-line qlen; returns 0. Else r[0..1] along KA and r[2..3] along KB, as signed unscaled products. Returns 1 |
| `0x10020da0` | kk_qlen | `|c|² > 1e-24`: `((KA0−KB0)·c)²/|c|²`; else qlen_PK |
| `0x10021800` | qlen_KK(KA, KB) | segment-segment squared distance via the above |
| `0x1000dae0` | invsqrt4(f32) | `y0 = bits((0x7ff00000 − hi(x)) >> 1 + 0x1ff00000, 0)`; 4× `y = ((0.5 − 0.5x·y·y) + 1)·y` |
| `0x1000db80` | invsqrt5(f64) | same, 5 iterations |
| `0x1000dd50` | normalize4 | false if `|v|² < 1e-19`, 4 iterations |
| `0x1000e120` | normalize5 | same, 5 iterations |
| `0x1000de30` | normalize-and-length | 5 iterations, returns `|v|` (0 if `|v|² < 1e-19`) |
| `0x1000e280` | cross | `a × b` |

The iteration is `y·(1.5 − 0.5·x·y²)` written as `((0.5 − (0.5x)y²) + 1)·y`. A port can use
`1/sqrt` (it differs in the last bits).

### 3.11 Port notes

- Create `phys_ledge.h` with the packed edge/triangle/ledge/node structs and the `next`/`prev`/`opposite`
  helpers. Keep the IVP memory layout (offset-based, 16-aligned) so that pointer tricks such as
  `edge & ~0xf` and the `status | edge` loop-hash keys work unchanged.
- `phys_minimize.c`: the solver object, loop hash, table and the 12 case functions above. They are pure
  functions of (two ledges, two cache poses) plus the synapse state. Test them by feeding pose pairs and
  checking `len_numerator`, `n` and the final features against brute force.
- `phys_recursive.c`: the ledge-tree query, child exchange (keep the persistence semantics: keyed by ledge pair),
  recursive mindist slot 6/7/hull-event methods, and the OO watcher.
- For Ballance the hot path is: ball (syn0, B) against floor triangle ledges. That means BF/BK/BP and the BF
  backside (rc 3) pierce walk. A single-triangle ledge is a 2-triangle "pancake" (front and back), so
  `pierce_index` just points at the other face.

### 3.12 Open questions

1. Which mindist list ids 2 and 5 are (chapter 4).
2. Resolved by chapter 1: the builder never gives internal nodes a hull (the root-hull option is off). Floors
   and multi-convex bodies always use the OO watcher, and recursive mindists never occur.
3. Meaning of core `+0x1b8`/`+0x1c0` (assumed speed and rotation speed × radius) and of `+0x10`.
4. The KK-core E0..E3 table was confirmed independently (3.13). Still compare the port against a brute-force
   segment distance on random boxes.
5. PK core with `q <= 1e-19`: the walk uses a B-space vector as a world vector. Recommended deterministic
   replacement: return 1.
6. The event solver (`0x1007632c` table) consumes `len_numerator`, `n`, `dot_diff_center` and the final
   synapse statuses. Their exact use is listed in 3.13.R3.

### 3.13 Review of the convex minimize solvers (independent check against the disassembly)

Every convex minimize case of 3.9 was rechecked against the disassembly: FF_init `0x10019aa0`, PF `0x10030c60`,
PF core `0x10030f80`, PK `0x10032da0`, PK core `0x10032e60`, PP `0x10032350`, KK `0x10031440` and KK core
`0x100317a0`. The 3.9 pseudocode is right except for the points below; R2.1-R2.4 and R2.8 have been applied inline in 3.9. The event-side (time of impact) cases are in
chapter 6.

#### 3.13.R1 Table `0x10075ee0` (filled by `0x1001a6b0`): exact contents

The index is `4*status(syn[k]) + status(syn[k^1])`, with `k = (m.flags >> 8) & 3`.

| idx | pair | target |
|---|---|---|
| 0, 1, 2, 5, 10 | PP, PK, PF, KK, **FF** | `0x10019ff0` poly driver |
| 4, 8 | KP, FP | `0x1001a350`: `m.flags ^= 0x100`, then `0x10019ff0` |
| 3, 7, 11 | PB, KB, FB | `0x1001a1f0`: **writes to address 0 first** (debug trap), then flips and calls `0x1001a220` |
| 12, 13, 14 | BP, BK, BF | `0x1001a220` (ball, chapter 5) |
| 15 | BB | `0x1001a370` (chapter 5) |
| 6, 9 | KF, FK | `0x1001a6a0`: writes to address 0, returns 5 |

`0x10019ff0` builds the two 0x14-byte infos `{points, ledge, cache (refcounted, 0x1001a190), obj, synapse*}`.
`info+0x10` is the synapse pointer that every `set(info, e, s)` writes through. It then switches on
`4*sA + sB` through the jump table at `0x1001a174`:

| index | target |
|---|---|
| 0 | PP `0x10032350` |
| 1 | PK `0x10032da0` |
| 2 | PF `0x10030c60` |
| 5 | KK `0x10031440` |
| 3, 4 and anything > 5 (FF = 10) | `0x10019aa0` |

So **FF reaches FF_init through the default case**. A face-face pair is never a stored final state.

#### 3.13.R2 Corrections

1. **KK entry `0x10031440`, last branch.** The products use **r[2]**, not r[3]. Exact order:
   ```
   if (u * r[2] < 0)       return PP(pa, qb, A, B)                       // no sel flip
   if (u2 * r[0] < 0)      { flip sel so B first; return PP(pb, qa, B, A) }
   return PP(pa, pb, A, B)                                               // no sel flip
   ```
   - `pa = (r0 < r1) ? KA : next(KA)` and `qa` is the other end. `pb`/`qb` are the same with r2/r3 and KB.
   - `(u, v)` are the K-values of `pa` (in B space) on KB. `(u2, v2)` are those of `pb` (in A space) on KA.
   - Also, the entry ignores `kk_params`' return value. In the parallel case it uses the sampled r[] as if they
     were real.
2. **PK core, `q <= 1e-19` branch.** The neighbour walk does not read stale stack.
   - The vector at local `+0x30` is `c = d x (X - K0)`, computed just before the test. It is tiny but
     initialized, and it is in **B object space**.
   - The walk rotates it with `A.R^T` as if it were world space, against a threshold of `q*1e-12`.
   - Result: the walk is deterministic but meaningless. Port recommendation stands: treat it as converged,
     `set(A,P,0); set(B,K,1); return 1`. That matches the original whenever no neighbour passes
     `proj > q*1e-12`.
   - The stored normal in this branch (`any_perpendicular(d)` then normalize5) stays in B object space, not
     world. This is the "near-zero-distance point-edge quirk". It only happens with exact contact on the edge
     line, so the friction code then gets a wrong-frame normal. Port it as-is or rotate it (deviation, harmless).
3. **PK core, "nearly parallel" test.** It reads **r[2]**. With `kk_setup(K, bestE, B, A)`, r[2..3] are the
   parameters along `bestE`. The test is `r[2] < 0` → converged. Strict: `-0.0` is not `< 0`.
4. **KK core: the second qlen in the "both w[1], w[2] < 0" branch (`0x10031fa9`).**
   - The first call is `q1 = qlen_KK(other, next(tri), other_info, owner)`.
   - The second call passes `[esp+0x10]` as its first edge. At that point that slot holds the flag-loop pointer
     (`&flag[4]`, a stack address), not `other`.
   - Its second edge is `opposite(prev(tri))`. Then `q2 < q1 ? KK(other, prev(tri)) : KK(other, next(tri))`.
   - This looks like an original bug: the edge is a garbage stack address, masked to an index into the other
     ledge's points. the 3.9  reading `qlen_KK(other, opposite(prev(tri)))` is the evident intent.
   - Port it with `other`. Confidence that the original reads the stack pointer: medium-high (stack offset
     arithmetic checked twice). The branch is reachable only when the picked face's two other edge checks are
     both negative, which is rare.
5. **Newton step counts.** These are exact, and D gets them right:
   - **5 steps**: PP's inline isqrt (`0x1003244a..0x100324e6`) and KK core's inline isqrt of `|c|^2`
     (`0x10031882..0x100318fe`).
   - **4 steps**: `0x1000dae0` (f32 argument) and `0x1000dd50`.
   - Every per-edge neighbour criterion `isqrt(|Q-P|^2)` goes through `0x1000dae0`, so its argument is
     **rounded to f32 first**.
6. **PF core.** All comparisons use stored f32 values:
   - The sign test that flips `nA` uses the **stored f32** `m.len` before the extra radius is subtracted.
   - The backside test is `(f32)m.len + (f32)m.extra < 0`, strict.
   - `|Q-P|^2` in the neighbour loop round-trips `dz` through f64. This is harmless.
7. **F-values tests.** "All inside" tests OR the three f32 words and check bit 31, so `-0.0` counts as outside.
   The follow-ups count with strict `< 0` (PF core: `n_neg == 1`) or `<= 0` (the qlen candidates).
   - So a `-0.0` weight on an otherwise inside point takes the "min qlen" branch, where `-0.0 <= 0` qualifies.
   - It never ends with no candidate, so the NULL-edge KK call cannot happen.
8. **KK core E0..E3 table at `0x10031a15-0x10031b28`.** Rebuilt independently from the stores; it **confirms the 3.9
   assignment** (high confidence). The 0x38-byte entry layout is:

   | off | meaning |
   |---|---|
   | +0x00 | triangle edge |
   | +0x04 | other edge |
   | +0x08 | `f64 *point` (other edge endpoint in the triangle owner's space) |
   | +0x0c | info of the other edge's object |
   | +0x10 | info of the triangle's owner |
   | +0x14 | `f64 *vec` (nA for A's faces, c in B space for B's faces) |
   | +0x18 | `f64[3]` hesse of the triangle |

   - `flag[i] = signbit((f32)(hesse_i . vec_i)) ^ (i >> 1) ^ neg`.
   - `best` starts at `-4e-12` (f64 `0xbd919799812dea11`).
   - Comparisons: `dot < 0` (strict), `c_ < best` (strict), `F-values(...)[0] > 0` (strict).
   - `c_ = isqrt4(|v|^2) * dot * isqrt4(|h|^2)`. The `|h|` factor is computed first and kept in f64.
   - The backside position uses `t = r2/(r2+r3)` (or `r0/(r0+r1)`), computed on the x87 stack from the f32 r
     values and stored as f64. `lerp` is `0x1000dc40(this = solver+8, a, b, t)`.
9. **PP.** The criterion is `c = ((Q-P).(X-P)) * isqrt4(|Q-P|^2)`, with `v = X - P` **unnormalized**.
   - `best` starts at 0, compared strictly. The A side is scanned first, so on equal values A's edge wins.
   - The edges scanned are the incoming edges of P: start Q, end P. They begin at `prev(opp(prev(e0)))` and
     end with `prev(e0)`.
10. **FF_init `0x10019aa0`.**
    - Vertex pairs use plain `<` against 1e101 (f64 `0x54e6dc186ef9f45c`), without the `1.000000000001`
      factor.
    - Point-face and edge-edge candidates use the factor.
    - The edge-edge candidates require all four r values sign-clear and also ignore `kk_params`' return value.
11. **Loop-check keys** (`0x100196f0(a, b, c, d)`):

    | case | key |
    |---|---|
    | PP | `(0\|PA, 0\|PB)` |
    | PK core | `(0\|P, 1\|K)` |
    | PF core | `(0\|P, 2\|F)` |
    | KK core | `(1\|KA, 1\|KB)` |

    PK, PF and KK *entries* do no check. With `0x100197f0` (counter 0) every check runs. With `0x10019950`
    (counter 20) the first 20 core steps skip it. A loop gives return code 2 and the mindist keeps whatever
    `len`/`n` the last executed core step wrote.

#### 3.13.R3 What the event solver (`0x1007632c`, chapter 6) takes from the minimizer

| minimizer output | used by event side as |
|---|---|
| `syn[k].edge`, `syn[k^1].edge` (+0x14) | features: P = `start(edge)`; K = edge; F = `tri(edge)` |
| statuses (+0x1a) | table index `4*sA + sB`. **Must be 0..3 and ordered** (PP/PK/PF/KK with the point/lower status first, ball first), otherwise `0x1002d850` crashes. The minimizer keeps this through its `flip sel` before every swapped-argument call and in FF_init |
| `m.len` (+0x54, f32, minus extra radius) | PP/PF Voronoi limits (`min(len, cd)`), PK `dmin = len - dt*speed`, the caller's "event possible" gate `(t_max - t_now)*s.0x10 + cd > len`, and the PF hit start value `xr + len` |
| `m.normal` (+0x68, f32 world, from the second (B-side) object toward the first) | `s.0x10`, the projected approach speed. PP: `u = -n`. KK: the sign of `(dA x dB)`. **PK's degenerate branch leaves n in B object space**, so `s.0x10` is then computed with a wrong-frame vector |
| `m.extra` (+0x50) | hit targets `cd + xr`, `0.5xr + rcd`, `0.9xr + rcd` |
| `m+0x58` dot_diff_center | not read by the event side |

**Backside (rc 3).**
- Status 5 never reaches the event table. `fix_backside` (`0x10019790`) turns it into status 2 on the walked
  face.
- With the single-pass driver `0x100197f0`, `len`/`n` then still hold the values the backside-producing core
  wrote:
  - PF core: a negative `len` with `n` = the face normal;
  - KK core: the edge-edge values;
  - PK core: nothing new; it returns before writing.
- They are not recomputed for the new face until the next recalc. `0x10019950` re-runs the minimizer, at most
  2 fixes.

#### 3.13.R4 Confidence

- **High** for R1, R2.1, R2.3, R2.5-R2.11 and R3. All were read directly from the disassembly.
- **Medium-high** for R2.4 (garbage first edge in the second qlen).
- **Medium** for R2.2: the meaning is clear, but whether the walk ever finds an edge in practice is not.

## 4. Mindist objects, mindist manager, scheduling, friction and impact hand-over

Scope: `0x10015fc0-0x10018a40` and the mindist code that continues into `0x10018a40-0x1001a740`
(recalc drivers, minimize dispatch table, ball-ball solver, cache objects), plus the hand-over into the friction
system (`0x10020030`, `0x1001a8b0`, `0x1001f860`, `0x10022180`, `0x1001d4d0`) and the interface of the impact system
(`0x100240a0` → `0x10023cd0`).

Names in *italics* are IVP names that fit the code (hypothesis; the DLL has no symbols). Everything else is read from
the disassembly. "ext" = x87 80-bit intermediate (control word is set to 64-bit mantissa in the event loop), "f32"
and "f64" are storage types.

### 4.0 Corrections to docs/physics.md section 4

- `0x10018000-0x1001a740` is **not** "core state sync": it is mindist-manager list code (`0x10018040-0x10018930`),
  the *IVP_Cache_Object* manager (`0x10018930-0x100195f0`), and the mindist recalc driver with its own 4×4 table at
  **`0x10075ee0`** (filled by `0x1001a6b0`).
- There are **two** 4×4 dispatch tables, both indexed `table[status(sorted syn0)*4 + status(sorted syn1)]`:
  - `0x10075ee0` = *minimize* (distance recalc) drivers, `0x10019ff0` / `0x1001a220` / `0x1001a370`. They call the
    actual feature-walk solvers in `0x10030c60-0x10032da0` (the "narrowphase" range).
  - `0x1007632c` = *event solvers* (time of next event inside the PSI), `0x1002d4d0` / `0x1002d5f0` / `0x1002d6c0`, which
    call `0x1002aed0-0x1002d000`. It is called only from `update_exact_mindist_events` (`0x10017870`).
- `0x10038000` is the *IVP_OO_Watcher* constructor (pair delegator for multi-ledge objects), not a surface builder.
- `env+0x1c` (`0x1002d8e0`, 0x60 bytes) is the *range manager*. The "penetration recovery" constants in physics.md
  section 5 are probably its fields.
- The constants at `0x10075db0` are *IVP_Mindist_Settings* (4.1), not contact-point constants.

### 4.1 Global mindist settings `0x10075db0` (*ivp_mindist_settings*)

Static init `0x10015fc0` → `0x10015fd0` (`ecx = 0x10075db0`) → `0x100160c0`. That calls `set_collision_tolerance(t)`
(`0x10015fe0`, thiscall, one f64 argument) with `t = 0x3f847ae140000000` = `(double)0.01f` = 0.009999999776482582.

All fields are f32. Each product is computed ext and rounded on store. The constants are f64 copies of f32 literals.

| Off | Global | Formula (in this order) | Value (f32 hex) | Use |
|---|---|---|---|---|
| +0x000 | 10075db0 | `real_coll = t*0.1f` | 0.001 (3a83126f) | event-time offset in `0x10017870`; event solvers |
| +0x004 | 10075db4 | `min_coll = real_coll + t*0.9f` | 0.01 (3c23d70a) | impact `0x10024930` |
| +0x008..+0x104 | 10075db8 | `coll_dists[i] = (f108 - f4)*(i*(1/64)) + f4`, i=63..0 | all 0.01, because f108 == f4 | coll_dist per mindist, selected by flag bits 22-29 |
| +0x108 | 10075eb8 | `= min_coll` (written together with +4) | 0.01 | `0x10024560` |
| +0x10c | 10075ebc | `friction_dist = f108 + t` | 0.02 (3ca3d70a) | new contact point distance; friction system (`0x1001d660`, `0x10036760`, `0x10036b80`); `0x10030b20` |
| +0x110 | 10075ec0 | `keeper_dist = f10c + 0.3f*t` | 0.023 (3cbc6a7f) | contact point point-edge case `0x1001f160` |
| +0x114 | 10075ec4 | `speed_after_keeper = sqrt((f110 - f4) * 19.62f)` | 0.50503469 (3f0149f4) | only a redundant guard in `0x10017870` |
| +0x118 | 10075ec8 | `t*0.01f` | 1e-4 (38d1b717) | friction solver `0x10036b80` |
| +0x11c | 10075ecc | `max_dist_for_friction = f10c + 2.5f*t` | 0.045 (3d3851eb) | friction creation and keep (`0x1001d4d0`, `0x1001d660`, wheel pass) |
| +0x120 | 10075ed0 | `max_dist_for_impact = f10c + 20f*t` | 0.22 (3e6147ae) | impact `0x100249b0` |
| +0x124 | 10075ed4 | `f4 * 0.1f` | 0.001 | impact tolerance in simulate (`0x100181b0`) and event solvers |
| +0x128 | 10075ed8 | `t + t` | 0.02 | impact solver `0x10022ed0` |
| +0x12c | 10075edc | (int) global counter, initially 0 | | coll_dist index decay in `0x10017870` |

Nothing else writes `+0x8..+0x104`, so **every mindist's coll_dist is 0.01** whatever its 8-bit index. A port can keep
a single constant. The index decay in `0x10017870` (below) then has no numeric effect, though it still advances the
global counter.

### 4.2 Structures

#### 4.2.1 Mindist object (*IVP_Mindist*, 0x88 bytes; *IVP_Mindist_Recursive* 0x98)

The hierarchy is *IVP_Time_Event* → *IVP_Collision* → *IVP_Mindist_Base* (`0x100160e0`, vtable `0x10063778`) →
*IVP_Mindist* (`0x10016290`, vtable `0x100637b4`) → *IVP_Mindist_Recursive* (`0x10030860`, vtable `0x10063ad0`, with a
second vtable `0x10063ac8` at +0x88).

| Off | Type | Meaning | Init |
|---|---|---|---|
| +0x00 | vtbl | see 4.2.2 | |
| +0x04 | int | time-event heap index in the time manager, `0xffff` = not scheduled | 0xffff |
| +0x08 | ptr | delegator (owner *IVP_Collision_Delegator*: OO watcher, recursive parent, or the root) | ctor arg |
| +0x0c, +0x10 | int[2] | *fvector_index*: index of this collision in each OV element's collision vector (`0x10016d60` updates whichever slot equals the old index) | -1, -1 |
| +0x14 | u32 | flags (4.2.3) | `& 0xcfc00000 | 0x0fc00000` |
| +0x18 | synapse[0] | 0x1c bytes (4.2.4) | |
| +0x34 | synapse[1] | 0x1c bytes | |
| +0x50 | f32 | *sum_extra_radius* = `obj0+0xa0 + obj1+0xa0` (ball radius for balls, 0 for polygons) | init |
| +0x54 | f32 | *len_numerator*: signed distance surface-to-surface, extra radii already subtracted | |
| +0x58 | f32 | *contact_dot_diff_center* = `dot(core0_pos - core1_pos, n)` at recalc (halfspace optimisation) | |
| +0x5c | | padding | |
| +0x60 | f64 | *sum_angular_hull_time* (see hull manager) | 0 |
| +0x68 | f32[3] | *contact_plane* normal n, unit. Points from synapse 1's surface toward synapse 0 (ball-ball: `c0 - c1`) | |
| +0x74 | f32 | 4th component of the f32 hesse (not seen used here) | |
| +0x78 | int | *recalc_time_stamp* = `env+0x138` at the last recalc | 0 |
| +0x7c | ptr | next mindist in the manager list (exact or invalid list) | |
| +0x80 | ptr | prev | |
| +0x84 | ptr | wheel: last friction-handled edge (only for car wheels; unused in Ballance) | 0 |
| +0x88.. | | recursive only: 2nd vtable, +0x8c = -1, +0x90/0x92/0x94 = child vector (u16 cap, u16 n, ptr) | |

The constructor `0x10016290(this, env, delegator)` increments `env+0x78` (live mindists) and `env+0x7c` (created). The
destructor decrements `env+0x78` and increments `env+0x80`.

#### 4.2.2 Vtables

- `0x10063778`, base: `[0]` simulate_time_event = `0x10016180` (crash stub); `[1]` get_objects `0x10016410`
  (`out[0]=syn0.l_obj, out[1]=syn1.l_obj`); `[2]` get_ledges `0x10016430` (ledge of each synapse edge, see 4.2.4);
  `[3]` delegator_is_going_to_be_deleted `0x10016190` (= delete this); `[4]` scalar deleting dtor.
- `0x100637b4`, mindist: `[0]` **simulate_time_event `0x100181b0`**; `[1..3]` as base; `[4]` dtor `0x100162d0`;
  `[5]` **inter_penetration hook `0x10019770`**: `env+0x20 (anomaly manager)->vtbl[2](this, syn0.l_obj, syn1.l_obj)`,
  which is `0x1002f8f0`; `[6]` **exact_mindist_went_invalid `0x10016f70`**: `mgr.remove_exact(this);
  mgr.insert_invalid(this)`; `[7]` **do_impact `0x100240a0`**.
- `0x10063ad0`, recursive: `[5]` `0x10028220`, `[6]` `0x10030710`, `[7]` `0x10030540` (chapter 3).
- Synapse vtable `0x100637a0` (an *IVP_Listener_Hull*): `[0]` `0x1000b0b0` returns 0 (listener type); `[1]`
  **hull_limit_exceeded(hull_mgr, f32 intrusion)** `0x10017d10` → `mindist(syn).0x10017d70(intrusion)`; `[2]`
  hull_manager_is_going_to_be_deleted `0x10017d30` → `delete mindist`; `[3]` hull_manager_is_reset(f32 a, f32 b)
  `0x10017cf0` → `0x10017d50`: `md+0x60 = (f64)((a - b) + (f32)md+0x60)` (computed in f32; see note); `[4]` dtor
  `0x100161e0`. A synapse finds its mindist through `mindist = syn + (short)syn+0x18`.

  Note on `0x10017d50`: the decompile shows an f32 add. That is not verified in the disassembly (not dumped);
  treat it as f32 math stored to f64.

#### 4.2.3 Mindist flags `+0x14`

| Bits | Mask | Name | Values |
|---|---|---|---|
| 0-7 | 0xff | *coll_type* of the scheduled event (event-solver output) | low nibble 0 = real collision event, nonzero = topology-change event (IVP enum hypothesis: 0x10 PP_COLL, 0x11 PP_PK, 0x20 PF_COLL, 0x21 PF_NPF, 0x30 PK_COLL, 0x31 PK_PF, 0x32 PK_KK, 0x40 KK_COLL, 0x41 KK_PARALLEL, 0x42 KK_PF, plus ball types; chapters 5-6) |
| 8-9 | 0x300 | *synapse_sort_flag*: sorted synapse k = `synapse[((flags>>8)&3) ^ k]` (only bit 8 ever toggles) | toggled by `0x1001a350` / `0x1001a1f0` |
| 10-11 | 0xc00 | *is_in_phantom_set* | phantom only |
| 12-13 | 0x3000 | *mindist_function*: 0 = collision, 1 = phantom (set when either object has `obj+0x1c`) | 0 in Ballance |
| 14-15 | 0xc000 | *recalc_result*: 0 OK, 1 (0x4000) intrusion / recalc failed | |
| 16-17 | 0x30000 | set with the phantom set (`0x10018660` sets bit 16). Nonzero disables the halfspace shortcut in `0x10017d70` | 0 in Ballance |
| 18-21 | 0x3c0000 | *mindist_status* (signed 4 bits): 0 uninit, 2 INVALID, 3 EXACT, 4 HULL_RECURSIVE, 5 HULL | |
| 22-29 | 0x3fc00000 | coll_dist index into `coll_dists[]` (8 bits, init 63) | |
| 30-31 | | unused (kept by the ctor mask) | |

#### 4.2.4 Synapse (*IVP_Synapse_Real*, 0x1c bytes, two per mindist at +0x18 and +0x34)

| Off | Type | Meaning |
|---|---|---|
| +0x00 | vtbl | `0x100637a0` |
| +0x04 | int | index in the object's hull-manager heap (`obj+0x68`) while the mindist is HULL |
| +0x08 | ptr | next synapse in the object's list (exact: `obj+0x20`, invalid: `obj+0x24`) |
| +0x0c | ptr | prev |
| +0x10 | ptr | `l_obj` (real object) |
| +0x14 | ptr | `edge`: *IVP_Compact_Edge**. Point/edge/face are given by an edge; for a ball, its pseudo ledge's first edge |
| +0x18 | i16 | `mindist - synapse` (byte offset) |
| +0x1a | i16 | status: 0 POINT, 1 EDGE, 2 TRIANGLE (face), 3 BALL, 5 BACKSIDE (transient) |

Ledge from edge (`0x10016470` and others): `tri = edge & ~0xf; ledge = tri - ((tri->dword0 & 0xfff) + 1)*16`. The 12-bit
triangle index is at `tri+0`, the ledge header is 16 bytes, and triangles are 16 bytes each.

#### 4.2.5 Mindist manager (`env+0x10`, 0x18 bytes, ctor `0x100186e0(env)`)

| Off | Meaning |
|---|---|
| +0x00 | int reentrancy flag for `recheck_ov_element` (1 while the env+0x2c pre-listener runs) |
| +0x04 | env |
| +0x08 | head of the **exact** list (links +0x7c/+0x80). Insert at head |
| +0x0c/+0x0e/+0x10 | u16 cap, u16 n, ptr: vector of exact mindists where either core has `core+0x10 != 0` (*car wheel*; never set in Ballance) |
| +0x14 | head of the **invalid** list |

HULL mindists are in no manager list, only in the two objects' hull heaps. The synapses of EXACT mindists are linked in
`obj+0x20`, those of INVALID mindists in `obj+0x24`.

#### 4.2.6 Fields of other objects that this code relies on

Real object (*IVP_Real_Object*):

| Off | Meaning |
|---|---|
| +0x04 | type: 2 polygon, 3 ball |
| +0x18 | env |
| +0x1c | phantom controller (NULL in Ballance) |
| +0x20 / +0x24 / +0x28 | exact / invalid / friction synapse list heads |
| +0x2c | ptr: *q_core_f_object*, NULL if not rotated |
| +0x30 | f32[3] *shift_core_f_object* |
| +0x40 | cache object ptr (4.2.7) |
| +0x48 | **hull manager** (4.2.8) |
| +0x80 | flags: low byte = *movement state* (<8 simulated; 1 after revive; 8 = not simulated); `&7 == 0` means not moving (static or frozen); bit 0x2000 = has collision listeners; bits 0xc00 = q/shift present (skip in cache) |
| +0x8c | surface manager: vtbl `[0]` get_single_convex_ledge(), `[4]` get_all_ledges_within_radius(center_os, f64 r, root_ledge, other_obj, other_ledge, out_list), `[7]` add_ledge_reference(ledge), `[8]` remove_ledge_reference(ledge) |
| +0x9c | OV element (0x30 bytes, `0x1002dc00`) |
| +0xa0 | f32 *extra_radius* |
| +0xa4 | core used for speeds and positions. +0xa8 = *physical core*, used by friction creation and do_impact. They are the same in Ballance unless units merge |
| +0xb0 | client data (entity). Also the ball-ball synapse order key |

Core (fields read here):

| Off | Meaning |
|---|---|
| +0x00 | flags: bits 2-3 = unmovable/fixed (`&0xc`) |
| +0x04 | f32 *upper_limit_radius* (bounding radius about the mass centre) |
| +0x0c | env |
| +0x10 | car-wheel flag (0) |
| +0x52 / +0x54 | object count, object array |
| +0x60 | low byte = movement state (8 not simulated, <8 simulated, 0x21 forced "get mindist" used by `0x100099f0`) |
| +0x68 | f64 time of last PSI |
| +0x70 | f32 1/psi? (scales dt for the quaternion interpolation in the cache) |
| +0xa4 | f32[3] linear speed |
| +0xb8 | f64[3] mass-centre position at the last PSI (rows padded to 32 bytes) |
| +0xd8 | f32[3] velocity used for extrapolation: `pos(t) = b8 + d8*(t - t68)`, f64 = f64 + f32×f64 (`0x10012470`; the decompiler's f32 casts are wrong) |
| +0xe8 / +0x108 | f64 quaternions at this and the next PSI |
| +0x128 | f64 rotation rows, 3 × 32 bytes; translation +0x188 = *m_world_f_core_last_psi* |
| +0x1a8 | f32[3] unit rotation axis |
| +0x1b8 | f32 *current_speed* \|v\| |
| +0x1c0 | f32 *max_surface_rot_speed* (\|ω\| × radius) |
| +0x1d4 | sim unit |
| +0x230 | int impact stamp |

Environment:

| Off | Meaning |
|---|---|
| +0x04 | time manager: `+8` event heap, `+0x18` f64 heap base time |
| +0x10 | mindist manager |
| +0x14 | OV-tree manager |
| +0x18 | collision filter |
| +0x1c | range manager |
| +0x20 | anomaly manager (vtbl `0x10063a58`) |
| +0x28 | perf counter |
| +0x2c | optional collision pre-listener |
| +0x78/+0x7c/+0x80 | mindist counters |
| +0x88 | OV recheck counter |
| +0x8c | contact-point recalc counter |
| +0xa4 | cache-object manager |
| +0xb4 | short-term arena (+0x10 nesting count) |
| +0xc0 | f64 **psi** (1/66) |
| +0x120 | f64 **current time** |
| +0x128 | f64 **time of next PSI** |
| +0x138 | int **time code**, incremented by every `set_current_time` (`0x100138f0`) |
| +0x13c | int impact counter |
| +0x15a/+0x15c | collision-delegator roots vector |

#### 4.2.7 Cache object (*IVP_Cache_Object*, 0xd0 bytes, manager `env+0xa4`)

`0x10018930` gets a slot from a ring of 0xd0-byte entries (`mgr+0` = size (power of 2), `+4` = cursor, `+8` = array). It
skips slots whose refcount (`+4`) is nonzero and evicts the previous owner (`0x10018910` clears `owner+0x40`).
`0x1001a190(obj)` returns the object's cache, creating it, refcount++. It updates the cache (`0x10018a40`) when
`obj.movement_state < 8` and `cache.time_code < env.time_code`. Callers decrement `+4` when done.

| Off | Meaning |
|---|---|
| +0x00 | time code |
| +0x04 | refcount |
| +0x08 | object |
| +0x10 | f64 q_world_f_core at time t: copy of `core+0xe8` when `t == core.t68`, else `0x10019320(q_this, q_next, (t - t68) * core.f70)` |
| +0x30 | **m_world_f_object**: rows of 4 f64 at +0x30, +0x50, +0x70; translation at **+0x90** |
| +0xb0 | f64[3] **core mass centre at t** |

The translation `+0x90` starts as `core pos(t)` and is then shifted by `obj+0x30` (`0x1000f5f0`) unless `obj+0x80 & 0xc00`.
If `obj+0x2c` is set, the quaternion is post-multiplied and the matrix rebuilt. `0x1000f3e0(m, in, out)` transforms a
world point into object space.

#### 4.2.8 Hull manager (inside the real object, base `obj+0x48`)

| Off (obj) | Type | Meaning |
|---|---|---|
| +0x48 | f64 | *last_vpsi_time* t_h |
| +0x50 | f32 | *gradient* g: hull growth per second (max surface speed) |
| +0x54 | f32 | *center_gradient* cg (linear part) |
| +0x58 | f32 | *hull_value_last_vpsi* h |
| +0x5c | f32 | *hull_center_value_last_vpsi* ch |
| +0x60 | f32 | *hull_value_next_psi* |
| +0x68 | | min-heap of synapses keyed by f32 hull value: insert `0x10030180(heap, elem, f32 key) → index`, remove `0x10030470(heap, index)`. +0x70 = current min key, +0x6c elems (16 bytes each, element ptr at +0xc), +0x76 = index of the minimum (chapter 2) |

- Current hull value: `H(t) = h + g*(t - t_h)`.
- Angular hull value: `A(t) = (h - ch) + (g - cg)*(t - t_h)`.
- The hull pass (`0x1001eb10`, after integration) loops while `heap.min_key - hull_value_next_psi < 0.0f`, at most 102
  times. Each time it calls the min synapse's `hull_limit_exceeded(hm, intrusion = min_key - hull_value_next_psi)`,
  which is ≤ 0.

#### 4.2.9 Event-solver input/output block (stack struct passed to `0x1007632c[i](blk)`)

| Off | Type | Meaning |
|---|---|---|
| +0x00 | f64 | `sum_rot = core0.f1c0 + core1.f1c0` (f32 add) |
| +0x08 | f64 | `proj_center_speed = dot(n, core1.v) - dot(n, core0.v)` (approach speed of the mass centres along n) |
| +0x10 | f64 | `worst_case_speed = proj + sqrt(1.001 - dot(axis0,n)²)*core0.f1c0 + sqrt(1.001 - dot(axis1,n)²)*core1.f1c0` |
| +0x18 | f64 | `sum_max_speed = core1.f1b8 + core0.f1b8 + sum_rot` |
| +0x20 | ptr | mindist |
| +0x24 | ptr | env |
| +0x28 | f64 | t_now (`env+0x120`) |
| +0x30 | f64 | t_max (`env+0x128`, next PSI) |
| +0x38 | int | out: event type (0 = none) |
| +0x40 | f64 | out: event time |

core0/core1 are the cores of the *sorted* synapses 0/1.

#### 4.2.10 Minimize-solver block (stack, about 0x840 bytes)

`+0x00` = mindist, `+0x04` = 0x14 in `recalc_mindist` (`0x10019950`) and 0 in `0x100197f0`
(an iteration budget?), `+0x828` = 0. Results:
- 1 OK
- 2 failed / intrusion
- 3 BACKSIDE: the point is on the back of the face; the caller re-roots it with `0x10019790`
- 4 (caller) already recalculated at this time code

The solver internals belong to chapter 3.

### 4.3 Lifecycle

#### `0x100160e0` *IVP_Mindist_Base* ctor `(this, delegator)`
`+8 = delegator; +0xc = +0x10 = -1; construct 2 synapses (vtbl 0x100637a0); +0x60 = 0 (f64); flags = (flags &
0xcfc00000) | 0x0fc00000; +4 = 0xffff`.

#### `0x10016290` *IVP_Mindist* ctor `(this, env, delegator)`
Base ctor, vtbl `0x100637b4`, `+0x78 = 0`, `+0x84 = 0`, env counters.

#### `0x10016490` init `(md, objA, objB, edgeA, edgeB)`
Callers: `create_exact_mindists` and the delegator root `0x1002f430`. The edges are `&ledge->tri[0].edge[0]` (`ledge+0x14`).

```
s0 = &md.syn[0]; s1 = &md.syn[1]
if objB.type == 2 (polygon):
    s1.l_obj = objB; s1.edge = edgeB; s1.status = 0 (POINT)
    objB.surface_mgr->add_ledge_reference(ledge_of(edgeB))
    first = s0
elif objB.type == 3 (ball):
    if objA.type != 3 or objA.b0 < objB.b0:      // unsigned compare of client data
        ballsyn = s0; first = s1
    else:
        ballsyn = s1; first = s0
    ballsyn.l_obj = objB; ballsyn.status = 3; ballsyn.edge = edgeB
else: crash
first.l_obj = objA; first.edge = edgeA
first.status = (objA.type == 2) ? 0 : 3; if polygon, add_ledge_reference(ledge_of(edgeA))
set both mindist_offsets
md.sum_extra_radius(+0x50) = objA.a0 + objB.a0          // f32
mgr = objA.env.mindist_manager
if objA.phantom == 0 and objB.phantom == 0: mgr.insert_and_recalc_exact(md)   // 0x10016f90
else: flags = flags & ~0x2000 | 0x1000; mgr.insert_phantom(md)                // 0x100170a0
```

Consequences:
- **For ball vs polygon the ball is always synapse 0.** The minimize table entries P/K/F-vs-B (`0x1001a1f0`) write to
  address 0 (assert) before flipping.
- For two balls, the one with the larger `+0xb0` value is in synapse 0. Only the sign of n depends on this.
- Polygon synapses start as POINT at the ledge's first point. The first recalc walks from there.

#### `0x100162f0` dtor
1. `env.+0x78--`, `env.+0x80++`.
2. If in the phantom set, leave it (`0x100186a0`).
3. By status: 2 → `remove_invalid`, 3 → `remove_exact`, 4 or 5 → `remove_hull`.
4. `remove_ledge_reference` for both synapse ledges (surface manager vtbl `[8]`).
5. `delegator->vtbl[0](this)` (*collision_is_going_to_be_deleted_event*).
6. Destroy the synapses.

### 4.4 Manager list operations

All of these are thiscall on the manager unless stated otherwise.

- **`0x10016e30` insert_exact(md)**:
  1. Status = 3.
  2. Push at the head of `mgr+8`: `md.next = head; md.prev = 0; head.prev = md`.
  3. Push syn0 onto `syn0.l_obj+0x20` and syn1 onto `syn1.l_obj+0x20` (`syn.next = head; syn.prev = 0;
     head.prev = syn`).
  4. If either core has `+0x10`, append to the wheel vector.
- **`0x10016ef0` insert_invalid(md)**: status = 2, list `mgr+0x14`, synapse lists `obj+0x24`.
- **`0x10016f90` insert_and_recalc_exact(md)**:
  1. Same linking as insert_exact.
  2. `recalc_mindist(md)` (`0x10019950`), then the wheel vector check.
  3. If `recalc_result == OK`:
     `update_exact_mindist_events(md, allow_hull = ((core0.f60 | core1.f60) & 0xff) < 0x21 (signed byte), hint = 0)`.
  4. Else `md->vtbl[6](mgr)` (went_invalid: exact → invalid list).
- **`0x10018230` remove_exact(md)**: if `md+4 != 0xffff`, remove the event from the time manager (`0x1002f200`) and set
  0xffff. Unlink from `mgr+8` and from both `obj+0x20` lists. Remove from the wheel vector (swap with last).
- **`0x10018310` remove_invalid(md)**: unlink from `mgr+0x14` and `obj+0x24`.
- **`0x10018390` remove_hull(md)**: `0x10030470(syn0.l_obj+0x68, syn0.heapidx)`, then the same for syn1.
- **`0x10018480` insert_hull(md, f32 dist)** (cdecl), with `dist = len - coll_dist`:
  ```
  if (syn0.l_obj.flags80 & 7) == 0: hull_insert(md, 0, dist)        // only the moving side grows its hull
  elif (syn1.l_obj.flags80 & 7) == 0: hull_insert(md, dist, 0)
  else:
      rA = core0.f1c0 + core0.f1b8 + 1e-10f      // ext
      rB = core1.f1c0 + core1.f1b8 + 1e-10f
      wA = (f32)(rB*0.1f + rA); wB = rA*0.1f + rB
      f  = (f32)(dist / (wB + wA))
      hull_insert(md, f*wA, f*wB)
  ```
  Unsorted synapse order (syn0 = +0x18).
- **`0x100183c0` hull_insert(md, f32 sA, f32 sB)** (cdecl):
  ```
  status = 5 (HULL)
  for k, s in (0, sA), (1, sB):
      o = syn[k].l_obj
      dt_k = (f32)(env.now - o.t48)                      // stored f32, the product uses ext
      syn[k].heapidx = heap_insert(o+0x68, &syn[k], (f32)(dt_ext*o.g + o.h + s))
  md.sum_angular_hull_time(+0x60) = ((gB - cgB)*dtB_f32 + (hB - chB)) + ((gA - cgA)*dtA_f32 + (hA - chA))   // f64
  ```
  So each synapse fires when its own object's hull has grown by `s` beyond its current value.
- **`0x10018540` / `0x100185a0`**: phantom variants. They use `o.f60 + s` as the key and 1e-10 instead of 0. Unused.
- **`0x10018660` / `0x100186a0`**: enter or leave the phantom set (`0x10400` set / `0x30c00` cleared). They notify
  `obj+0x1c` (buoyancy phantom `0x100109e0` / `0x10010b70`). Unused in Ballance.
- **`0x10018710`** manager dtor: deletes all exact, then all invalid mindists, frees the vector.

### 4.5 Recalc (exact distance)

#### `0x10019950` recalc_mindist(md) → int
```
if md.+0x78 == env.time_code: return 4
md.+0x78 = env.time_code
blk = {md, 0x14, ..., 0}
for tries in 0..1:
    r = table_0x10075ee0[status(sorted s0)*4 + status(sorted s1)](&blk)
    if r == 1: flags &= ~0xc000; return 1
    flags = flags & ~0x8000 | 0x4000              // recalc_result = 1
    if r == 2: break
    if r == 3: fix_backside(&blk) (0x10019790); continue
    else: crash
if mindist_function != phantom and status != 4: md->vtbl[5]()     // inter_penetration (anomaly manager)
return r
```
`0x100197f0` is the same without the retry: it returns after one backside fix and passes 0 instead of 0x14 in `blk+4`.
It is used by invalid-retry (`0x100099a0`), the phantom path and `0x10030b2f`.

#### `0x10019790` fix_backside(blk)
Pick the sorted synapse with status 5 (else the other one). Set its `edge = 0x1001a5c0(edge, blk+8 (point))` and its
status to 2.

`0x1001a5c0` walks the ledge's triangles from the edge's triangle, marking visited triangles in a stack bitmap sized by
the ledge triangle count. For each triangle it computes the three edge-plane distances of the point (`0x10020ad0`, into
f32[3]). It moves to the opposite triangle across the first edge whose distance is not `> 0` and whose neighbour is
unvisited. It returns the triangle where all three are `> 0` or all neighbours are visited (the triangle the point
projects into).

#### Minimize table `0x10075ee0` (filled by `0x1001a6b0`)

| s0 \ s1 | P | K | F | B |
|---|---|---|---|---|
| P | 19ff0 | 19ff0 | 19ff0 | 1a1f0 |
| K | 1a350 | 19ff0 | 1a6a0 (crash) | 1a1f0 |
| F | 1a350 | 1a6a0 | 19ff0 | 1a1f0 |
| B | 1a220 | 1a220 | 1a220 | 1a370 |

- `0x1001a350` (KP, FP): `flags ^= 0x100` (swap sort), then `0x10019ff0`.
- `0x1001a1f0` (PB, KB, FB): writes to address 0 (assert), flips, then `0x1001a220`.
- **`0x10019ff0` poly-poly driver**: get both cache objects (`0x1001a190`) and `point_base = ledge + ledge.c_point_offset`
  (`*ledge`) for each sorted synapse. Dispatch by `s0.status*4 + s1.status`:
  - 0 → `0x10032350` (PP)
  - 1 → `0x10032da0` (PK)
  - 2 → `0x10030c60` (PF)
  - 5 → `0x10031440` (KK)
  - default (FF, 10) → `0x10019aa0`

  The arguments are `(blk, edge0, edge1, &{pointbase0, ledge0, cache0, syn0, obj1?...}, &{...})`. Release both caches.
- **`0x1001a220` ball-poly driver**: ball = `md.syn[0]` (fixed, not sorted). Arguments `(blk, &{cache_ball, ...}, edge_poly,
  &{pointbase, ledge, cache_poly, obj, syn1})`. By `syn1.status`: 0 → `0x10032050` (B-P), 1 → `0x10032860` (B-K),
  2 → `0x10030d50` (B-F). Release the caches.
- **`0x1001a370` ball-ball** (complete; it always returns 1):
  ```
  cA = cache(syn0.l_obj); cB = cache(syn1.l_obj)        // refcount++, updated to the current time code
  n = (f32)(cA.pos90 - cB.pos90)                        // f64 subtraction (ext), rounded per component
  s = n.x*n.x + n.y*n.y + n.z*n.z                       // ext, from the f32 n
  if |s| <= 1e-19: inv = 1.0
  else: inv = rsqrt(s)                                  // bit-trick seed + 5 Newton steps y = y*(1.5 - 0.5*s*y*y), ext
  md.len(+0x54) = (f32)(inv*s - md.sum_extra_radius)    // |cA - cB| - (rA + rB)
  md.n *= inv (each f32 store)
  md.+0x58 = (f32)( (cA.c_b8 - cB.c_b8)*n.y + (cA.c_c0 - cB.c_c0)*n.z + (cA.c_b0 - cB.c_b0)*n.x )   // core centres
  release caches
  ```
  Port: `inv = 1/sqrt((double)s)` matches to within the last ext bit.

### 4.6 Event scheduling for an exact mindist

#### `0x10017870` update_exact_mindist_events(md, allow_hull, hint)

thiscall, ret 8. Sorted synapses; c0 and c1 are their objects' `+0xa4` cores. `cd = coll_dists[idx]` (always 0.01).
```
sum_rot  = (f64)(c0.f1c0 + c1.f1c0)
sum_max  = (f64)((c1.f1b8 + c0.f1b8) + sum_rot)
if md.+4 != 0xffff: time_mgr.remove(md); md.+4 = 0xffff

// 1. far away: back to hull
if psi * sum_max * 2.1 + cd < len:                   // 2.1 = 0x100637f0 (f64 of 2.1f), psi = env.+0xc0
    if allow_hull: mgr.remove_exact(md); insert_hull(md, (f32)(len - cd))
    return

// 2. predict
proj  = (n.z*v1.z + n.y*v1.y + n.x*v1.x) - (n.z*v0.z + n.y*v0.y + n.x*v0.x)          // ext → f64
d0 = dot(c0.axis1a8, n); d1 = dot(c1.axis1a8, n)
worst = proj + sqrt(1.001 - d0*d0)*c0.f1c0 + sqrt(1.001 - d1*d1)*c1.f1c0             // 1.001 = 0x100637e8
if worst < 0.50503 and worst < 1e-19: return          // separating: no event (the first test is redundant)
if (t_max - t_now)*worst + cd <= len: return          // cannot reach coll dist in this PSI
if mindist_function == phantom: return

// 3. coll_dist index decay (no numeric effect, see 4.1)
if idx != 0: if (g_counter++ > 2): idx--, g_counter = 0     // global counter at 0x10075edc

// 4. event solver
blk = {sum_rot, proj, worst, sum_max, md, env, t_now, t_max, type=0, time}
table_0x1007632c[status(s0)*4 + status(s1)](&blk)
if blk.type == 0: return
T = blk.time
if T - t_now < 1e-6:                                // 0x100637e0 = (f64)1e-6f
    if hint == 0: T = t_now
    else:
        f = len - 0.001f                            // real_coll_dist, ext
        if hint == 2 or (blk.type & 0xf) != 0:
            T = (f < 1e-12) ? t_now + psi*0.001f : (f/sum_max + t_now) + psi*1e-4f
        else:                                       // hint 1 and a real collision event
            T = (f < 1e-12) ? t_now + psi*1e-5f : (f*0.1/sum_max + t_now) + psi*1e-7f
        if T - t_max >= 0: return                   // leave it to the next PSI's critical pass
md.+4 = time_mgr.event_heap(tm+8).insert(md, (f32)(T - tm.base_time(tm+0x18)))
flags.coll_type(low byte) = blk.type
```

Hint values:

| Hint | Callers | Meaning |
|---|---|---|
| 0 | `insert_and_recalc_exact` | a fresh exact mindist; an event "now" is kept at now |
| 1 | the critical pass, and simulate after a topology event | |
| 2 | simulate after a near miss, phantom insert, `0x10009610` after an impact | |

In the hint 1/2 cases a too-early event is pushed forward a little: by the time to close the remaining distance at
`sum_max` (or 10% of it), plus a tiny psi fraction. That guarantees progress.

#### `0x100181b0` simulate_time_event(md, env) (vtbl[0]: the time manager pops the event at T)

The env time has been set to T and the time code bumped, so the recalc runs at T.
```
perf(8)
recalc_mindist(md)
if recalc_result != OK: perf(0xe); return             // the inter_penetration hook already ran
if (coll_type & 0xf) != 0: update_events(md, 0, 1)    // topology change: re-predict from the new features
elif len < cd + 0.001f: md->vtbl[7]()                 // DO_IMPACT (0x100240a0)
else: update_events(md, 0, 2)                         // near miss: re-predict
perf(0xe)
```
After the impact nothing re-arms this mindist directly. The impact system changes speeds and calls `0x1000d930` per
touched core, which runs `0x10009610` (below), and the next critical pass also covers it.

#### `0x10017d70` hull_limit_exceeded(md, f32 intrusion) (from either synapse)
```
if status == 4: recursive_hull_exceeded(md) (0x10030b20); return
s0, s1 = sorted synapses; oA = s0.l_obj, oB = s1.l_obj; cA, cB their cores
pA = cA.pos(now); pB = cB.pos(now)                     // 0x10012470, f64
d  = pA - pB (f64)
rA = cA.f1c0 + cA.f1b8 + 1e-19;  rB = cB.f1c0 + cB.f1b8 + 1e-19   // f64
sum = rA + rB
if (flags & 0x30000) == 0:                             // halfspace optimisation
    proj = n.y*d.y + n.z*d.z + n.x*d.x                 // ext
    ang  = A_oA(now) + A_oB(now)                       // angular hull values, ext
    new  = len - (ang - md.f60) - (md.f58 - proj) + intrusion
    if new > psi * sum * 6.0:                          // 0x100637f8
        md.f60 = ang (f64); md.f58 = (f32)proj; md.len = (f32)(new - intrusion)
        ratio = new / sum
        re-key s0 in oA's heap: remove; insert(H_oA(now) + ratio*rA)      // f32 key
        re-key s1 in oB's heap: remove; insert(H_oB(now) + ratio*rB)
        return
remove_hull(md)
if phantom: insert_phantom(md) else insert_and_recalc_exact(md)
```
This is the cheap "still on the far side of the separating plane" extension. It treats the distance as
`len - (rotational hull growth) + (centre motion along n)` and avoids an exact recalc while the gap is larger than 6
PSIs of worst-case closing.

#### The per-PSI passes (from env step `0x10013cb0`)

The perf indices are 1..7: START, UNIVERSE, CONTROLLERS, INTEGRATORS, HULL, SHORT, CRITICAL.

1. Step listeners, `0x10013c40`, then **`0x10017790` wheel pass** (no-op in Ballance: the vector is empty).
2. Controllers (`0x100124c0`), integration (`0x1001ea50`), **hull pass** (`0x1001eb10`, see 4.2.8).
3. **SHORT `0x10017850` recalc_all_exact_mindists**. For each md in the exact list from the head (next read first):
   `0x100187a0(md)`:
   1. `recalc_mindist(md)`.
   2. For a collision mindist, if the recalc failed, `md->vtbl[6]()` (to invalid).
   3. The phantom branch is unused.
4. **CRITICAL `0x10017770` recalc_all_exact_mindist_events**. For each md in the exact list from the head:
   `update_exact_mindist_events(md, allow_hull=1, hint=1)`.

   At this point `env.now` is the PSI time, `t_max` is the next PSI and the recalc stamp is current, so this
   (re)predicts every exact pair for the coming PSI and may demote far pairs to hull.

#### Wheel pass `0x10017790` / `0x10018040` (car wheels only, documented for completeness)

For wheel mindists with a ball synapse and a face synapse whose edge changed (`!= md+0x84`) and `len < 0.045`:
1. `try_to_generate_managed_friction`.
2. If the contact point is not new, re-run `recalc_friction_s_vals` at the current time, keeping the old time stamp.
3. Transport the old friction span values onto the new face basis.
4. Store the edge in `+0x84`.

#### Object-level helpers

- **`0x10009610`** (obj): for each exact synapse of obj whose two cores have different `+0x230` stamps:
  `recalc_mindist`; if OK, `update_events(md, 0, 2)`.
- **`0x1000d930`** (core) sets `core+0x230 = env+0x13c` (impact counter) and runs `0x10009610` on its objects. It is
  called by the impact system after changing speeds (`0x10024c60` region).
- **`0x100099a0`** (obj): for each invalid synapse, run `0x100197f0`. If the recalc succeeds, move the mindist invalid →
  exact **without** scheduling (the next critical pass schedules it). Called from the controller/sim-unit step
  `0x100121b0`.
- **`0x100099f0`** (obj): `recheck_ov_element` with `core.f60` low byte forced to 0x21, then restored (this blocks hull
  demotion of the new mindists).

### 4.7 Pair creation

- **`0x100176e0` enable_collision_detection(mgr, obj)** (from glue `0x10009350`): delete the old `obj+0x9c` OV element,
  create a new one (`0x1002dc00`), then `recheck_ov_element`.
- **`0x10017140` recheck_ov_element(mgr, obj)** (broadphase; chapter 2 owns the OV tree):
  ```
  e = obj.ov_elem; if !e return
  ovtree.remove(e) (0x1002eec0); env.+0x88++
  e.center(+0x10..+0x18) = (f32)core.pos188..198        // current PSI centre
  if mgr.+0 == 0:
      range = core.upper_limit_radius + env.range_mgr->vtbl[1](obj)          // ext → f64
      if env.+0x2c and (obj.f80 & 7): mgr.+0=1; env.+0x2c->vtbl[0](obj, &e.center, range); mgr.+0=0
      r = ovtree.insert_and_collect(e, range, &list) (0x1002ec10)            // returns the radius actually used
      e.set_hull(obj+0x48, r - core.upper_limit_radius) (0x1002dd00)        // the OV element is a hull listener
      hash the existing collisions in e.+0x28 vector by objects pair (h = o0 ^ o1 ^ obj; h = (h>>8)*0x3ff + h)
      for other in list (reverse):
          if ((obj.f80 | other.f80) & 7) and obj.a8 != other.a8 and env.coll_filter->vtbl[0](obj, other):
              if a collision with `other` exists: keep it (move into the kept prefix) else new.append(other)
      delete the collisions not kept (vtbl[4](1))
      for other in new: for root in env.delegator_roots (from last): if root->vtbl[3](obj, other): break
  else:
      ovtree.insert_and_collect(e, (f64)core.upper_limit_radius, NULL); e.set_hull(obj+0x48, 1e-19)
  ```
- **Delegator root `0x1002f430` (create_collision(obj0, obj1))**:
  1. `l0 = obj0.surface_mgr->vtbl[0]()`, `l1 = ...` (single convex ledge, or NULL if the surface has several).
  2. If both exist: create the mindist directly. It is *IVP_Mindist* when both ledges have `has_children == 0` (bits 0-1
     of `ledge+8`), else *IVP_Mindist_Recursive*. Init with `(obj0, obj1, l0+0x14, l1+0x14)`.
  3. Otherwise create the OO watcher (`0x10038000`, 0x40 bytes). It later calls `create_exact_mindists`.
  4. Add the collision to both OV elements (`0x1002dde0`).

  The ball and the convex hulls are single ledges, so they get a direct mindist. The concave floor (one ledge per
  face) gets an OO watcher.
- **`0x10016650` create_exact_mindists(obj0, obj1, f64 scan_radius, FVector *mindists, single0, single1, root0, root1,
  delegator)** (cdecl; called by the OO watcher `0x10037e90` and the recursive mindist `0x10030ad6`):
  ```
  if single0: L0 = [single0]
  else:
      c = core1.pos(now) (f64)
      r = obj0.extra_radius + core1.upper_limit_radius + scan_radius   // f32+f32 ext, + f64
      c_os = cache(obj0).m_world_f_object^-1 * c
      obj0.surface_mgr->get_all_ledges_within_radius(&c_os, r, root0, NULL, single1, &L0)
  L1 likewise (obj1, core0, root1, single0)
  open-address hash (size: power of two ≤ 0x400 fitted to 2*(|L0|*|L1| + n) + 2, heap-allocated if larger)
      of existing mindists by ledge pair: h = (l1*0x4b) ^ l0; h = (h>>8)*0x3ff + h (signed >>)
  for a in L0 (reverse): for b in L1 (reverse):
      if a mindist with ledges (a, b) exists: move it into the kept prefix
      else: md = (terminal(a) && terminal(b)) ? new Mindist(env, delegator) : new Mindist_Recursive(env, delegator)
            md.init(obj0, obj1, a+0x14, b+0x14); new.append(md)
  delete mindists[kept..n-1] (vtbl[4]); append the new ones, setting their fvector index
  ```
  `init` immediately recalcs and schedules each new mindist (`insert_and_recalc_exact`).

### 4.8 Hand-over to the friction system

**Entry: `0x10022180` try_to_generate_managed_friction(md, IVP_Friction_System **fs_out, int *new_out,
IVP_Simulation_Unit *keep_su, bool call_recalc)** (thiscall).

1. Sorted objects o0 and o1, physical cores (`+0xa8`). If core0 is fixed (`&0xc`) swap, so that `c_mov` is movable.
2. `cp = 0x10020030(md, &is_new)`:
   - `0x1001ffe0` searches `o0+0x28` (friction synapse list) for a contact point with the other object whose features
     match the mindist (`0x1001d910`).
   - Otherwise it allocates 0x78 bytes and runs `0x1001a8b0(cp, md)`.
3. If not new: `*fs_out = core's friction system; *new_out = 0`. If call_recalc: `recalc_friction_s_vals(cp)`
   (`0x1001f860`) and `0x10024040(cp, cp.info)`. Return cp.
4. If new: optionally recalc. Fire the friction-created event: `env 0x10013bc0(env, &{env, cp.info, cp})`, and for each
   object with `f80 & 0x2000`, `0x1000a8a0` (this drives PhysicsContinuousContact). `*new_out = 1`.
5. Then attach cp to a friction system: create one (0x50 bytes, `0x1000b000`) or join or merge the cores' systems
   (`0x1000d9a0`/`0x1000d9b0`/`0x1001c570`). Add cp (`0x1000b360`, `0x1000b2c0`) and the cores (`0x1000b390`).
   `0x1001d3d0(cp)`.
6. Merge the two sim units if both are movable and differ, keeping `keep_su`.

**`0x1001a8b0` contact point ctor (cp, md)**. These are the fields filled from the mindist:

| cp off | Meaning |
|---|---|
| +0x08 | friction synapse 0 (0x14 bytes: next +0, prev +4, `l_obj` +8 (= sorted syn0.l_obj), i16 offset to cp +0xc, status byte +0xe (= syn0.status low byte), edge +0x10). Linked at the head of `l_obj+0x28` |
| +0x1c | friction synapse 1 from sorted syn1 (`l_obj` +0x24, status +0x2a, edge +0x2c) |
| +0x68 | f64 = env.now |
| +0x4c | if syn1 is a face: f32 `1/|face normal|` (`0x10021280` normal, `0x1000dd20` length) |
| +0x58 | f32 distance = `friction_dist` (0.02) placeholder until recalc |
| +0x5c | i16 = 20 |
| +0x60 | low byte = 1 |
| +0x38, +0x3c, +0x48, +0x50, +0x54, +0x64, +0x70 | 0 |
| +0x34 | low byte = 0 |

**`0x1001f860` recalc_friction_s_vals(cp)** (*IVP_Contact_Point*). It is also called every PSI by the friction system
(`0x1001d610`, `0x1001cc20`, `0x1000b190`, `0x100242c0`).

1. Allocate a 0xe0-byte *IVP_Impact_Solver_Long_Term* / contact situation `info` from the arena `env+0xb4`, store it at
   `cp+0x40`, clear `info+0x5a` and the low 10 bits of `info+0x5c`; `env+0x8c++`.
2. Geometry by `(cp.status0, cp.status1)`. Points are world coordinates via the caches (`0x100217d0` = ledge point to
   world):
   - s0 = POINT: p0 = world(point). By s1: POINT → `0x1001f050(cp, p0, p1, info)`; EDGE → `0x1001f160` (uses
     keeper_dist); FACE → `0x1001ee70`; BALL → `0x1001f050` with p1 = ball centre (`cache+0x90`).
   - s0 = EDGE: `0x1001f3b0` (edge-edge).
   - s0 = BALL: p0 = ball centre (`cache+0x90`), `cp.+0x60` low byte = 1, then the s1 switch as for POINT.
   - These write `info.normal` (+0x00, f32[3]), the world contact point `info+0x20` (f64[3]) and the raw distance
     `cp+0x58`.
3. Ball correction:
   - `info.p += normal * o0.extra_radius`, so the point is on synapse 0's surface (the ball).
   - `cp.dist(+0x58) -= (o0.extra_radius + o1.extra_radius)`, clamped to ≥ 0.
4. Tangent `info+0x80` = some vector orthogonal to n (`0x1000e030`), `info+0x90 = n × tangent`.
5. For each movable core k: contact point in core space (`m_world_f_core_last_psi` transposed, from `core+0x188`),
   `r×n` in core space, surface speed (`0x1000bf90`). Inverse effective mass:
   `info+0x74 = Σ_k (Σ_i (r×n)_i² / I_i (core+0x34..0x3c) + 1/m (core+0x40))` and `info+0x70 = 1.0f/that`
   (`_DAT_10063238`). `info+0x78/+0x7c` = the cores, or 0 if fixed.
6. Advance the static-friction springs:
   - `dt = now - cp.+0x68`; `cp.+0x68 = now`.
   - `cp.+0x38 -= dot(v_rel, info.tangent) * dt`.
   - `cp.+0x3c -= 0x1001b060(v_rel, info.+0x90) * dt`.

**Distance conditions for creating or keeping friction**:
- **Impact**: always (`do_impact` → `0x10023cd0` calls try_to_generate with call_recalc = 1). The first contact of a
  falling ball comes from its impact.
- **Revive** (`0x1000aea0` → `0x1001d4d0` per revived core): for each exact collision mindist of the core's objects whose
  other core is movable and not yet in a friction system:
  1. `recalc_mindist`.
  2. If OK and `len < max_dist_for_friction (0.045)`: try_to_generate (call_recalc 1).
  3. If it is new, revive the other core.

  If both cores are fixed, the mindist is deleted. A fixed partner is not handled here; it comes in through impacts.
- **Keep**: the friction system drops a contact point when `cp.dist ≥ 0.045` or `info.+0x5c & 0x300 == 0x100`
  (`0x1001d660`, after `0x1001ae40(cp, friction_dist 0.02, 1.0f)`).

### 4.9 Impact system interface (`0x10022000-0x10024e80`, interface only)

**`0x100240a0` do_impact(md)** (mindist vtbl[7]):
1. For both synapse objects: `0x10009670` (if `obj.movement_state == 8`, wake the sim unit `0x10011f80`).
2. `arena(env+0xb4).+0x10++`. For each core with movement state < 8: `0x1000cfa0(core)`.
3. `env.+0x13c++` (impact counter).
4. **`0x10023cd0(md, o0, o1)`** (cdecl; o0 and o1 are the unsorted syn0 and syn1 objects).
5. `arena.+0x10--`; reset the arena (`0x100201b0`).

**`0x10023cd0` impact entry** reads the mindist only through try_to_generate and the contact point:
1. `cp = try_to_generate_managed_friction(md, &fs, &is_new, su (sim unit of the movable core +0x1d4), 1)`. This builds
   `cp.info` with the normal, the world point, r×n and the inverse masses, as above.
2. `speed = 0x10024930(cp, env)`. It uses min_coll_dist +0x4.
3. `0x10024140(info, &out, speed, cp)` runs the impact solver `0x10022ed0(..., (short)info+0x5a, speed)`. Material
   values come from `cp+0x44`.
4. `info.normal` is saved, and scaled by `_DAT_10063230` while the solver runs if `core1.flags & 0xc` (fixed partner).
   `0x1001d380` returns the pair's friction-system record. Its `+0x20` time is set to now; the old value gives
   `d_time_since_last = (f32)(now - old)`.
5. `0x10024320` post-processes the friction points of the system. This includes `recalc_friction_s_vals` and
   `0x1000d930` per core, which reschedules that core's mindists.
6. Collision event: `0x10013b80(env, &{f32 d_time_since_last, env, info})` to global listeners, and `0x1000a770` per
   object with `f80 & 0x2000`. This is what PhysicsCollDetection's callback receives. The normal, the point and the
   speed are in `info`.
7. Writes: core velocities (inside `0x10022ed0`), the contact point and friction-system membership, and the
   `core+0x230` stamps.

### 4.10 End-to-end model (mindist view)

1. **Pair birth.** `recheck_ov_element` (on enable, and whenever the OV element's hull listener fires) finds overlapping
   OV spheres. `range` = the range manager's radius plus the core bounding radius. The collision filter applies. A
   delegator root creates a direct mindist (single-ledge objects) or an OO watcher. The OO watcher calls
   `create_exact_mindists` with the ledge tree. A new mindist is recalculated at once (`insert_and_recalc_exact`, hint 0).
2. **HULL** (status 5): far pairs sit in the two objects' hull heaps with keys `H(now) + share`. The share is
   `len - 0.01` split by speed (`0x10018480`). After every PSI's integration the hull pass compares each object's
   `hull_value_next_psi` against the heap minimum. Exceeded synapses call `hull_limit_exceeded`, which either extends the
   key cheaply (halfspace test, needs > 6 PSIs of slack) or converts back to EXACT with a recalc and a prediction.
3. **EXACT** (status 3): every PSI, SHORT recalcs every exact mindist and CRITICAL re-predicts it:
   - far (`len > 2.1·psi·Σspeed + 0.01`) → back to HULL;
   - separating or unreachable in this PSI → no event;
   - otherwise the event solver gives the time of the next feature change or of reaching coll_dist, as a time-manager
     event (f32 offset from the heap base).
4. **Event** at T (inside the PSI, before the next PSI, time code bumped): recalc at T, then:
   - topology event → re-predict (hint 1);
   - `len < 0.011` → **do_impact** (impact solver plus friction contact);
   - else re-predict (hint 2).

   After the impact, `0x1000d930` re-predicts the cores' other mindists, and the friction system holds the pair while
   `dist < 0.045`.
5. **INVALID** (status 2): the recalc failed (penetration or a degenerate case). The anomaly manager's inter_penetration
   pushes the objects apart. The sim-unit step retries the recalc (`0x100099a0`) and returns the mindist to the exact
   list on success.

### 4.11 Open questions

- Event-type codes (low byte) per event solver: chapters 5-6. Here only "low nibble 0 = collision" is relied on.
- `0x10017d50` (hull reset callback) f32 vs f64 math: only seen in the decompile.
- The meaning of `blk+4` (0x14 vs 0) in the minimize block: an iteration budget?
- `env+0x2c` pre-listener: is it NULL in Ballance? It is `param_2[5]` of the env template. Check `0x10006bb0`.
- The range-manager radius (`env+0x1c` vtbl[1], `0x1002d8e0`), which sets the OV/hull scan: chapter 2.
- Ball-ball synapse order depends on `obj+0xb0` (entity pointer) values. A port should pick a deterministic rule (for
  example the CK id). The physics is symmetric except for the sign of n.
- `0x10019aa0` (the FF / default minimize case) and the minimize solvers in `0x10030c60-0x10032da0`: chapter 3.
- Whether `core+0xa4` and `core+0xa8` ever differ in Ballance (unit merging by constraints?). This code uses `+0xa4` for
  prediction and `+0xa8` for friction and impact.

## 5. Event solver dispatch, root finder and the ball cases

Scope: `0x1002a540-0x1002d8e0` (ball side), the generic root finder it uses (`0x10037790-0x10037d80`), and,
because the Voronoi walk and the mindist outputs belong with them, the **ball minimize solvers** that the
table at `0x1007632c` does *not* contain (they sit in a second table at `0x10075ee0`, functions at
`0x1001a1f0-0x1001a370` and `0x10030d50`, `0x10032050`, `0x10032860`, `0x10032910`). Chapter 3 covers the same
range (`0x10030860-0x10033500`); this chapter is authoritative for the *ball* paths.

Confidence legend: **[V]** verified from disassembly, **[D]** read from decompile and cross-checked, **[H]**
hypothesis (naming from public IVP, or inferred role).

### 5.0 Two dispatch tables

There are **two** 4x4 tables, both indexed `status[k]*4 + status[k^1]` (row = synapse `k`, see 5.1.2):

| Table | Filled by | Called from | Role |
|---|---|---|---|
| `0x1007632c` | `0x1002d860` (static init) | `FUN_10017870` at `0x10017b3b` (only xref) | **event solver**: predicts the time of the next event (collision or Voronoi-region change) of an exact mindist within `[t_now, t_next_psi]` [V] |
| `0x10075ee0` | `0x1001a6b0` (static init) | `FUN_100197f0` (`0x100198a2`), `FUN_10019950` (`0x100199e3`) | **minimize solver** ("recalc mindist"): walks the closest features over the compact ledge (Voronoi walk), writes distance, normal and synapse status [V] |

IVP names (unverified [H]): `IVP_Mindist_Event_Solver::mim_function_table` and
`IVP_Mindist_Minimize_Solver::mms_function_table`.

#### 5.0.1 Event table `0x1007632c` (filled by `0x1002d860`) [V]

Status codes: `0 = POINT`, `1 = EDGE` (K), `2 = FACE/triangle` (F), `3 = BALL`. Index = row*4 + col.

| row\col | P | K | F | B |
|---|---|---|---|---|
| **P** | `0x1002d6c0` | `0x1002d6c0` | `0x1002d6c0` | illegal |
| **K** | illegal | `0x1002d6c0` | illegal | illegal |
| **F** | illegal | illegal | illegal | illegal |
| **B** | `0x1002d4d0` | `0x1002d4d0` | `0x1002d4d0` | `0x1002d5f0` |

"illegal" = `0x1002d850`: `*(int*)0 = 0; ret` — a deliberate null write (IVP's CORE assert). A port should
`abort()`/assert. The convex generic `0x1002d6c0` re-reads `k` and dispatches PP `0x1002bcf0`, PK `0x1002c820`,
PF `0x1002aed0`, KK `0x1002b690` (chapter 6).

#### 5.0.2 Minimize table `0x10075ee0` (filled by `0x1001a6b0`) [V]

| row\col | P | K | F | B |
|---|---|---|---|---|
| **P** | `0x10019ff0` | `0x10019ff0` | `0x10019ff0` | `0x1001a1f0` |
| **K** | `0x1001a350` | `0x10019ff0` | illegal `0x1001a6a0` | `0x1001a1f0` |
| **F** | `0x1001a350` | illegal | `0x10019ff0` | `0x1001a1f0` |
| **B** | `0x1001a220` | `0x1001a220` | `0x1001a220` | `0x1001a370` |

- `0x1001a350`: `mindist.flags ^= 0x100` (swap `k`), then poly-poly generic `0x10019ff0` (chapter 3).
- `0x1001a1f0` (poly row, ball column): **first writes `*(int*)0 = 0` (crash)**, then swaps `k` and calls
  `0x1001a220`. So "poly in synapse k, ball in the other" is illegal at runtime.
- `0x1001a220` and the event function `0x1002d4d0` both **hard-code synapse 0 as the ball and synapse 1 as the
  polygon** (they do not look at `k`). Invariant for a port: a ball-polygon mindist has the ball in synapse 0
  and `k = 0`. A ball-ball mindist uses synapse 0 = A, synapse 1 = B.

### 5.1 Structures relied on

#### 5.1.1 Mindist (partial; owner: chapter 4) [V unless marked]

| Off | Type | Meaning |
|---|---|---|
| `+0x04` | u32 | time-manager event handle, `0xffff` = none |
| `+0x14` | u32 | flags: bits 0-7 last event type (5.2.3); bits 8-9 `k` (index of the "first" synapse); bits 12-13 state (`0x1000` = no events); bits 14-15 set by the minimize driver (`0x4000` = last recalc did not end with code 1); bits 16-17 (`0x30000`); bits 18-21 mindist kind (`0x100000` = recursive/ledge-tree mindist); bits 22-29 coll-dist index |
| `+0x18` | Synapse[2] | synapse `i` at `+0x18 + 0x1c*i` |
| `+0x50` | f32 | `sum_extra_radius` [H name]: sum of ball radii of the pair (ball-poly: 2; ball-ball: 4) |
| `+0x54` | f32 | `len_numerator` [H name]: **surface distance** = feature distance − `sum_extra_radius` (negative = penetration) |
| `+0x58` | f32 | `(cacheA.core_pos − cacheB.core_pos) · normal` (A = synapse 0) — projection of the mass-centre separation; consumer not traced [H] |
| `+0x68` | f32[3] | contact-plane normal, world, unit, **points from synapse 1 (polygon / ball B) toward synapse 0 (ball / A)** |
| `+0x78` | u32 | env "time code" (`env+0x138`) of the last minimize; equal → recalc returns 4 without work |

Synapse (0x1c bytes): `+0x10` real object, `+0x14` compact-edge pointer (polygon side), `+0x1a` i16 status
(0 P, 1 K, 2 F, 3 B, **5 = backside/penetrated, transient**).

#### 5.1.2 Real object / core / environment fields used [V]

| Where | Off | Type | Meaning |
|---|---|---|---|
| object | `+0x18` | ptr | environment |
| object | `+0x40` | ptr | cache object (lazy, see 5.1.3) |
| object | `+0x80` | i8 | movement state; `(int8)state < 8` = simulated (cache must be refreshed each time code), `>= 8` = static |
| object | `+0xa4` | ptr | core |
| core | `+0x04` | f32 | object radius (upper limit) [H] |
| core | `+0x48` | f32 | `0.5 / core[+4]` = inverse object diameter (`FUN_1000d1a0`) |
| core | `+0x68` | f64 | time of the core's last PSI |
| core | `+0xa4` | f32[3] | linear velocity (world) |
| core | `+0x1a8` | f32[3] | unit rotation axis (world) [H] |
| core | `+0x1b8` | f32 | \|v\| |
| core | `+0x1bc` | f32 | \|ω\| |
| core | `+0x1c0` | f32 | max surface speed due to rotation (≈ \|ω\|·radius) [H] |
| env | `+0x04` | ptr | time manager (`+0x08` event heap, `+0x18` f64 base time) |
| env | `+0x10` | ptr | mindist manager |
| env | `+0xc0` | f64 | PSI length (1/66) |
| env | `+0x120` | f64 | current time `t_now` |
| env | `+0x128` | f64 | time of the next PSI `t_next` |
| env | `+0x138` | i32 | time code (incremented per PSI; cache validity) |

#### 5.1.3 Cache object (0xd0 bytes, ring-allocated by `FUN_10018930`) [V layout, H origin]

| Off | Type | Meaning |
|---|---|---|
| `+0x00` | i32 | time code it was computed for |
| `+0x04` | i32 | reference count (solvers ++ on entry, −− on exit) |
| `+0x08` | ptr | owner object |
| `+0x10` | f64[4] | core quaternion copy |
| `+0x30` | Matrix (0x80) | `m_world_f_object` at `t_now`: rows at `+0x30,+0x50,+0x70` (3 f64 each, 8 bytes pad), translation `+0x90` |
| `+0xb0` | f64[3] | core (mass-centre) world position |

Getter (`0x1001a190`, inlined in `0x1002d4d0`/`0x1002d5f0`): if `obj.cache == NULL` allocate; `cache.ref++`; if
the object is simulated and `cache.time_code < env.time_code`, refresh (`FUN_10018a40`: pose at `env.t_now`,
copied from the core when `t_now == core.t_psi`, else interpolated). Cache transforms:
`FUN_10018d10` object point (f32) → world (f64); `FUN_10018ca0` world point → object (`R^T (p − t)`);
`FUN_10018ea0` `R v`; `FUN_10018dc0` `R^T v`.

#### 5.1.4 Matrix (0x80 bytes, `IVP_U_Matrix` [H]) [V]

Rows `r0 +0x00`, `r1 +0x20`, `r2 +0x40` (each 3 f64 + pad), translation `+0x60`. `M*p = (r0·p, r1·p, r2·p) + t`
(`FUN_1000f550` f64 point, `FUN_1000f5f0` f32 point), `R v` = `FUN_1000f6f0`.

#### 5.1.5 Ledge context passed to the poly-side cases [V]

`PolyCtx { +0 points (ledge + ledge.c_point_offset), +4 ledge, +8 cache, +0xc object, +0x10 synapse* }`
(the event path uses only `+0..+0xc`). Points are 16 bytes, `float x,y,z,pad`. Compact edge (u32): bits 0-15
start point index, bits 16-30 signed opposite offset in **edges** (4-byte units, relative to this edge),
bit 31 virtual. Triangle = 16 bytes: header u32 (bits 0-11 tri index, 12-23 pierce index, …) + 3 edges at
`+4,+8,+0xc`; `edge & ~0xf` is its triangle, `(edge & 0xc)>>2 ∈ {1,2,3}`, ledge = `tri − (tri_index+1)*16`.
Edge navigation tables: `next = edge + T_next[edge & 0xc]` with `T_next @0x100685b8 = {0,+4,+4,−8}`,
`prev = edge + T_prev[edge & 0xc]` with `T_prev @0x100685c8 = {0,+8,−4,−4}`, `opp(e) = e + 4*sext15(e>>16)`.
`end(e) = start(next(e))`. The edges leaving point `start(e)` are visited by `e ← opp(prev(e))`.

`BallCtx { +0 cache, +4 object, +8 synapse* }` (minimize path; event path passes the ball cache directly).

#### 5.1.6 Contact-distance settings (global at `0x10075db0`, init `FUN_10015fe0(0.01)`) [V]

Only the members the ball cases read:

| Addr | Off | Value | Use |
|---|---|---|---|
| `0x10075db0` | `+0x000` | `0.1·t = 0.001` | `real_coll_dist` (f32) |
| `0x10075db4` | `+0x004` | `0.9·t + 0.001 = 0.01` | min coll dist |
| `0x10075db8` | `+0x008` | f32[64] `coll_dist[i] = d4 + (d108 − d4)·i/64`; `+0x108` = `+0x004` at init, so **all 0.01** | indexed by mindist flags bits 22-29 |
| `0x10075ec4` | `+0x114` | `sqrt(19.62·(0.023 − 0.01)) ≈ 0.50504` | speed threshold in `FUN_10017870` |
| `0x10075ed4` | `+0x124` | `0.1·(+0x004) = 0.001` | BK face-entry margin |

Constants: `0.9` = f64 `0x3feccccce0000000` (`0x10063590`); `0.1` = f64 `0x3fb99999a0000000` (`0x10063500`).

### 5.2 Event solver driver: `FUN_10017870(mindist, int allow_hull, int mode)` (thiscall, ret 8) [V]

Called for exact mindists by the mindist manager (owner chapter 4). It builds the solver state `S` on its stack,
calls one event function, and schedules the result.

#### 5.2.1 Event solver state `S` (stack struct, 0x48 bytes) [V]

| Off | Type | Meaning |
|---|---|---|
| `+0x00` | f64 | `rot_sum = A.c1c0 + B.c1c0` (f32 add, then widened) |
| `+0x08` | f64 | `proj_v = n·vB − n·vA` (positive = approaching) |
| `+0x10` | f64 | `worst_approach` = projected worst-case closing speed (below) |
| `+0x18` | f64 | `worst_total = B.c1b8 + A.c1b8 + rot_sum` (unprojected) |
| `+0x20` | ptr | mindist |
| `+0x24` | ptr | environment |
| `+0x28` | f64 | `t_now` (env `+0x120`) |
| `+0x30` | f64 | `t_max` = `t_next` (env `+0x128`) |
| `+0x38` | i32 | out: event type (0 = none), set to 0 by every case function on entry |
| `+0x40` | f64 | out: event time (case functions initialise it to `t_max`) |

A = core of synapse `k`, B = core of synapse `k^1`, `n` = mindist `+0x68`.

#### 5.2.2 Algorithm

```
FUN_10017870(md, allow_hull, mode):
  A = md.syn[k].obj.core; B = md.syn[k^1].obj.core; env = md.syn[0].obj.env
  S.rot_sum     = (f64)(A.rotspeed1c0 + B.rotspeed1c0)
  S.worst_total = (f64)(B.speed1b8 + A.speed1b8) + S.rot_sum
  if md.event != 0xffff: time_manager_remove(env.tm, md) (FUN_1002f200); md.event = 0xffff
  cd  = coll_dist[md.flags>>22 & 0xff]
  len = md.len_numerator
  if env.psi_len * S.worst_total * 2.1 /*f64 0x100637f0 = 2.0999999046325684*/ + cd < len:
      if !allow_hull: return
      mindist_manager_to_hull(env.mm, md)            // FUN_10018230
      hull_insert(md, len - cd)                       // FUN_10018480 (chapter 4/C)
      return
  S.proj_v = n·B.v - n·A.v                            // f32 products, f64 sums
  dA = A.axis1a8·n;  dB = B.axis1a8·n
  S.worst_approach = sqrt(1.001 - dA*dA)*A.c1c0 + sqrt(1.001 - dB*dB)*B.c1c0 + S.proj_v
                     // 1.001 = f64 0x100637e8 (1.0010000467300415)
  S.t_now = env.t_now; S.t_max = env.t_next
  if S.worst_approach < 1e-19 /*0x10063480*/: return          // (the 0x10075ec4 test is redundant)
  if (S.t_max - S.t_now)*S.worst_approach + cd <= len: return  // cannot close the gap this PSI
  if (md.flags & 0x3000) == 0x1000: return
  if (md.flags>>22 & 0xff) != 0 and g_counter_10075edc++ > 2:    // global counter, all mindists
      md.coll_idx -= 1; g_counter = 0                              // no effect while all coll_dist equal
  S.mindist = md; S.env = env
  EVENT_TABLE[md.syn[k].status*4 + md.syn[k^1].status](&S)
  if S.type == 0: return
  t = S.time
  if t - S.t_now < 1e-6 /*f64 0x100637e0*/:
      if mode == 0: t = S.t_now
      else:
          slack = len - real_coll_dist              // f32 global 0x10075db0
          if mode == 2 or (S.type & 0xf) != 0:      // Voronoi event, or mode 2
              t = slack < 1e-12 ? S.t_now + psi*0.001f
                                : slack/S.worst_total + S.t_now + psi*1e-4f
          else:                                     // mode 1, distance event
              t = slack < 1e-12 ? S.t_now + psi*1e-5f
                                : slack*0.1/S.worst_total + S.t_now + psi*1e-7f
          if t - env.t_next >= 0: return
  md.event = event_heap_insert(env.tm.heap, md, (f32)(t - env.tm.base))   // FUN_10030180
  md.flags = (md.flags & ~0xff) | (S.type & 0xff)
```

(`0.001f = 0x100632a8`, `1e-4f = 0x1006323c`, `1e-5f = 0x100637d4`, `1e-7f = 0x100637d8`, `1e-12 = 0x100634f0`.)

#### 5.2.3 Event type codes (`S+0x38`) [V]

High nibble = case, low nibble 0 = **distance event** (pair reaches `coll_dist`), non-zero = **Voronoi event**
(closest feature may change; the minimize must re-run at that time).

| Code | Set by | Meaning |
|---|---|---|
| `0x10` | BB `0x1002c670`, BP `0x1002c260`, PP | point/ball distance |
| `0x11` | BP, PP | leaves point region |
| `0x20` | BF `0x1002b370`, PF | face-plane distance |
| `0x21` | PF | (convex) |
| `0x30` | BK `0x1002d000`, PK | edge-line distance |
| `0x31` | BK, PK | enters a face region |
| `0x32` | PK | (convex) |
| `0x40-0x42` | KK | (convex) |

Why only these Voronoi checks for the ball [H, consistent with the geometry]: the predictor of the current
feature is a **lower bound** of the true distance only if the true closest feature's distance is ≥ it. Moving
point→edge or edge→face makes the true distance *smaller* than the point/line predictor, so BP checks leaving
the point region and BK checks entering a face; face→edge and edge→point only increase it, so BF has no
Voronoi check and BK does not check its endpoints.

### 5.3 Root finder and distance functors (`0x10037790-0x10037d80`, vtables `0x100639a8-0x100639dc`) [V]

#### 5.3.1 Event simulation per object ("Event_Sim", 0xb18 bytes on stack; init `FUN_1002b300(sim, cache)`)

| Off | Type | Meaning |
|---|---|---|
| `+0x00` | ptr | real object (`cache+8`) |
| `+0x04` | ptr | core |
| `+0x10` | i32 | cache time code |
| `+0x14` | Matrix*[21] | slot `i` = pose at `t_now + i·0.005f`, filled lazily |
| `+0x68` | Matrix[21] | storage, `0x80` each |

Init: simulated object → slot 0 = `&cache.m_world_f_object`, slots 1..20 = NULL. Static object → all 21
slots = `&cache.m_world_f_object`. A slot is filled by `FUN_10009d70(obj, t, &storage[i])` (object pose at
time `t`: position `+ v·(t − t_psi)`, quaternion interpolation; owner: the core/integration port).

Port note: slots only exist for `i ≤ 20` (0.1 s). The PSI is 1/66 s, so a PSI window needs ≤ 4 slots; the
outer search returns "no event" at `i == 20` [V].

#### 5.3.2 Functor objects (single-method vtables) [V]

Common header: `+0 vtable`, `+0x08 f64 speed`, `+0x10 f64 1/speed`. `f(MA, MB)` returns a signed distance-like
value; MA/MB are the slot matrices of the two sims (A = ball sim, B = other).

| vtable | method | Fields | `f(MA, MB)` |
|---|---|---|---|
| `0x100639c8` | `0x1002ab30` "point-point" | `+0x28` f64[3] pA (obj A), `+0x48` f64[3] pB (obj B), `+0x68` f64[3] dir (world, unit) | `d = MB*pB − MA*pA; p = 1.2·(d·dir)` (1.2 = f64 `0x100639a0`); `return (p·|p| < d·d) ? p : sqrt(d·d)` i.e. `min(1.2·d·dir, |d|)` with sign |
| `0x100639ac` | `0x1002a9c0` "point-plane" | `+0x28` pA, `+0x48` plane normal (obj B), `+0x68` plane point (obj B) | `(MA*pA − MB*q)·(RB*n)` |
| `0x100639bc` | `0x1002abc0` "plane through B-point" | `+0x28` pA, `+0x48` pB, `+0x68` direction (obj B) | `(MB*pB − MA*pA)·(RB*dir)` |
| `0x100639dc` | `0x1002ad90` "point-line" | `+0x28` f64 offset, `+0x30` f32[3] pA, `+0x40` f32[3] line point, `+0x50` f64[3] line dir (unit, obj B), `+0x70` f64[3] ref normal (obj B, unit) | `v = MB*P − MA*pA; c = v × (RB*e); return min(|c|, c·(RB*nref) + off)` |
| `0x100639a8`, `0x100639b0`, `0x100639b4`, `0x100639b8`, `0x100639d8` | `0x1002ae80`, `0x1002aae0`, `0x1002b620`, `0x1002b530`, `0x1002ac40` | | convex cases (chapter 6) |

#### 5.3.3 `find_event(F, a, b, t0, t1, simA, simB, f0_ptr|NULL, &out)` = `0x10037b30` (thiscall on F, ret 0x30)

Finds the first time in `[t0, t1]` at which `f ≤ a`. `b` (< `a`) is only used when the pair is already within
`a` at `t0`.

```
f0 = f0_ptr ? *f0_ptr : F(simA.slot[0], simB.slot[0])
if f0 > a:
    return search_below(F, a, t0, t1, 0, simA, simB, &f0, out)          // 0x10037910
// already within a at t0
inv = 1.0 / F.speed;  t = t0;  i = 0;  f = f0
while t - t1 < 0:
    dt = (f - b) * inv
    if t - t1 + dt > 0:  step = t1 - t + 1e-8          // 1e-8 = 0x100633b8
    elif dt < 0:         step = 0
    else:                step = dt
    n = max(1, trunc(step * 200.0))                    // 200 = 0x10063b88, _ftol truncates
    i += n;  t_new = t + n * 0.005f                     // 0.005f = 0x10063a30 (0x3ba3d70a)
    fill slot i of both sims at t_new;  f = F(simA.slot[i], simB.slot[i])
    if f <= a:
        if f <= f0: *out = t; return 1                 // still close and not separating: event at t (first step: t0)
        t = t_new                                       // separating while close: keep walking (f0 NOT updated)
        continue
    // left the a-band
    if t_new - t1 > 0: return 0
    return search_below(F, a, t_new, t1, i, simA, simB, &f, out)
return 0
```

#### 5.3.4 `search_below(F, a, t0, t1, i, simA, simB, &f0, &out)` = `0x10037910` (ret 0x2c)

```
f = f0_ptr ? *f0_ptr : F(simA.slot[0], simB.slot[0])   // note: slot 0, not slot i
if f <= a: *out = t0; return 1
t = t0
loop:
    need = (f - a) * F.inv_speed
    x = (f32)(t - t1)
    if (t - t1) + need > 0: return 0                    // cannot reach a before t1 at worst-case speed
    step = 2*need;  if x + step > 0: step = (t1 - t) + 1e-8
    n = max(1, trunc(step*200));  i += n;  t_new = t + n*0.005f
    fill slot i (both sims, lazily);  f_new = F(slot_i A, slot_i B)
    if f_new <= a:
        r = regula_falsi(F, t, t_new, a, f, f_new, simA.obj, simB.obj)   // 0x10037790
        if r - t1 > 0: return 0
        *out = r; return 1
    if i == 20: return 0
    t = t_new; f = f_new
```

#### 5.3.5 `regula_falsi(F, &res, t_lo, t_hi, a, f_lo, f_hi, objA, objB)` = `0x10037790` (ret 0x34)

Invariant `f_lo > a ≥ f_hi`. Matrices are computed fresh with `FUN_10009d70` (not cached).

```
for it = 0;; :
    t = (t_hi - t_lo)*(a - f_lo)/(f_hi - f_lo) + t_lo
    if (it & 3) == 3:
        if it > 64: res = t_lo; return               // conservative exit
        t = ((t_lo - t) + (t_hi - t))*0.375 + t       // 0.375 = 0x10063b80: pull toward the midpoint
    f = F(pose(objA,t), pose(objB,t))
    if |f - a| < 1e-8: res = t; return
    it++
    if f < a: t_hi = t, f_hi = f  else: t_lo = t, f_lo = f
```

### 5.4 Ball event functions

All are thiscall on `S` (the case function's `this` is `S`). They build sims for the ball (from the ball cache)
and for the other object, set `S.type = 0` and `S.time = S.t_max`, then call the root finder with `t1 = t_max`
or, for second-phase checks, `t1 = S.time` (the best so far, so only earlier events replace it). The code is
f64 throughout except where f32 fields are read. `r = md.sum_extra_radius`, `cd = coll_dist[idx]`,
`rc = real_coll_dist`, `len = md.len_numerator`.

#### 5.4.1 Ball-polygon dispatch `0x1002d4d0(S)` [V]

```
ball = md.syn[0].obj; bc = cache(ball)               // getter of 5.1.3, ref++
e = md.syn[1].edge; poly = md.syn[1].obj
ctx = { points(ledge_of(e)), ledge_of(e), cache(poly) /*0x1001a190*/, poly }
S.type = 0
switch md.syn[1].status:
    0: BP(S, ball, e, bc, &ctx)   // 0x1002c260
    1: BK(S, ball, e, bc, &ctx)   // 0x1002d000
    2: BF(S, e, bc, &ctx)         // 0x1002b370
    else: crash
bc.ref--; ctx.cache.ref--
```

#### 5.4.2 Ball-ball `0x1002d5f0(S)` → `0x1002c670(S, cacheA, cacheB)` [V]

```
simA = sim(cacheA /*syn0*/); simB = sim(cacheB /*syn1*/)
S.time = S.t_max
F = PointPoint{ speed = S.worst_approach, inv = 1/speed, pA = 0, pB = 0,
                dir = normalize(cacheB.pos - cacheA.pos) /*FUN_1000e120*/ }
a = r + cd;  b = 0.5f*r + rc                          // 0.5 = f32 0x100634a0
if find_event(F, a, b, S.t_now, S.t_max, simA, simB, NULL, &S.time): S.type = 0x10
```

#### 5.4.3 Ball-face (BF) `0x1002b370(S, e, bc, ctx)` [V]

```
simB = sim(bc); simP = sim(ctx.cache)
P = point[start(e)]  (as f64)
n = triangle_normal(e, ledge) /*FUN_10021280: (Q-P)x(R-P), Q=end(e), R=start(prev(e))*/; normalize (FUN_1000dd50)
F = PointPlane{ speed = S.worst_approach, inv = 1/speed, pA = 0, n = n, q = P }
a  = r + cd;  b = 0.5f*r + rc
f0 = r + len                                          // exact distance of the centre from the plane
if find_event(F, a, b, S.t_now, S.t_max, simB, simP, &f0, &S.time): S.type = 0x20
```

`f = (C − P)·n_world`: positive on the outward side (triangle winding is outward; `n` from the polygon toward
the ball, consistent with `+0x68`).

#### 5.4.4 Ball-point (BP) `0x1002c260(S, ball, e, bc, ctx)` [V]

Phase 1, distance:
```
simB = sim(bc); simP = sim(ctx.cache); S.time = S.t_max
P = point[start(e)]
F1 = PointPoint{ speed = S.worst_approach, inv, pA = 0, pB = P, dir = -md.normal (f32 → f64, ×-1.0) }
if find_event(F1, r + cd, 0.5f*r + rc, S.t_now, S.t_max, simB, simP, NULL, &S.time): S.type = 0x10
```
Phase 2, leaving the point's Voronoi region along any incident edge:
```
dmax  = (S.time - S.t_now)*S.worst_total + len
speed = |P|*poly.core.w1bc + poly.core.v1b8 + poly.core.w1bc*dmax + ball.core.v1b8
k     = -min(dmax*dmax, cd*cd) * poly.core.inv_diam48     // computed once, before the loop
F2    = Plane{ speed, inv = 1/speed, pA = 0, pB = P }
g = opp(prev(e))
loop:
    Q = point[end(g)];  d = Q - P (f32 diffs → f64)
    F2.dir = d * invsqrt(|d|²)                         // FUN_1000dae0: exponent-halving guess + 4 Newton steps
    a = |d| * k                                        // |d| = invsqrt(|d|²)*|d|²; NOTE: length × length² / length
    if search_below(F2, a, S.t_now, S.time, 0, simB, simP, NULL, &S.time): S.type = 0x11
    if g == e: break
    g = opp(prev(g))
```
`F2 = (P − C)·e_world`; the event fires when the ball centre has moved past `P` along an incident edge by
`|PQ|·min(dmax², cd²)·inv_diam` (a tiny margin; dimensionally odd but literal) [V, meaning H].

#### 5.4.5 Ball-edge (BK) `0x1002d000(S, ball, e, bc, ctx)` [V]

Phase 1, distance to the edge line:
```
simB = sim(bc); simP = sim(ctx.cache); S.time = S.t_max
P = point[start(e)] (f32[3]);  Q = point[end(e)]
ed = normalize((f64)(Q - P))                          // FUN_1000dd50
Pw = ctx.cache.obj_to_world(P); ew = ctx.cache.R * ed
nref = normalize(ctx.cache.R^T * ((Pw - bc.pos) x ew))
F1 = PointLine{ speed = S.worst_approach, inv, off = 0.5*(r + cd) /*0.5 = f64 0x100631d0*/,
                pA = 0 (f32), P = P, e = ed, nref = nref }
a = r + cd;  b = 0.9f*r + rc                          // 0.9f = 0x10063878
if find_event(F1, a, b, S.t_now, S.t_max, simB, simP, NULL, &S.time): S.type = 0x30
```
Phase 2, entering either adjacent face (2 iterations: `ee = e`, then `ee = opp(e)`):
```
F2 = PointPlane{ speed = S.worst_total, inv = 1/S.worst_total, pA = 0 }
for ee in (e, opp(e)):
    P = start(ee); Q = end(ee); R = start(prev(ee))   // third vertex of ee's triangle
    nt = (Q-P) x (R-P)                                 // triangle normal (unnormalised)
    F2.n = normalize((Q-P) x nt)                       // in-plane, perpendicular to the edge, pointing AWAY from R
    F2.q = P
    a = -settings[+0x124]  (= -0.001)
    if search_below(F2, a, S.t_now, S.time, 0, simB, simP, NULL, &S.time): S.type = 0x31
```
`F2 = (C − P)·m_world` with `m` the outward edge normal of that triangle; the event fires once the centre is
0.001 inside the triangle's side of the edge.

### 5.5 Minimize (recalc) solver, ball paths

#### 5.5.1 Drivers `FUN_100197f0` / `FUN_10019950` (fastcall mindist) [D]

Solver state on stack (0x840 bytes): `+0x00 mindist`, `+0x04 i32 steps_until_loop_check`, `+0x08 f64[3]`
backside point (object space of the polygon), `+0x28` pair list `(u32,u32)[256]`, `+0x828` count.

```
recalc(md):                                           // 0x100197f0: counter = 0, one pass
    if md.time_code78 == env.time_code: return 4
    md.time_code78 = env.time_code
    solver = { md, counter = 0 (0x100197f0) | 20 (0x10019950) }
    rc = MIN_TABLE[status[k]*4 + status[k^1]](&solver)
    if rc == 1: md.flags &= ~0xc000; return 1
    md.flags = (md.flags & ~0x8000) | 0x4000
    if rc == 3: fix_backside(&solver)                 // 0x10019790
        // 0x10019950 then re-runs the table (at most 2 fixes in total)
    elif rc != 2: crash
    if (md.flags & 0x3000) != 0x1000 and (md.flags & 0x3c0000) != 0x100000: md.vtbl[5]()
    return rc
```
Return codes: **1** converged, **2** loop detected (or degenerate), **3** centre behind a face (synapse status 5),
**4** already up to date. `fix_backside` (`0x10019790`): pick the synapse whose status is 5 (synapse `k` if
so, else `k^1`), `syn.edge = surface_walk(syn.edge, solver.backside_point)` (`FUN_1001a5c0`), `syn.status = 2`.

`surface_walk(edge, X)` (`0x1001a5c0`) [D]: visited[] = bytes per triangle (`ledge+0xc` = n_triangles). Start at
edge 0 of the triangle `pierce_index` of the given triangle (header bits 12-23; the triangle "behind" it). Loop:
mark the triangle; `λ = bary(ledge, e0, X)` (5.5.3); for j = 0..2 over `e0, next, next(next)`: if `λ[j] > 0` or the
triangle across `opp(edge_j)` is visited, continue; else move to `opp(edge_j)` and restart. If all three pass,
return the current entry edge.

Loop check `0x100196f0(solver, a, b, c, d)` [V]: `x = a|b, y = c|d`, ordered so `x ≥ y` (signed); return 1 if
`(x,y)` is already in the list or the list holds 256 entries, else append and return 0. Each BP/BK step
decrements `solver[+4]` and runs the check only when it goes negative. Keys: BP `(3, edge)`, BK `(3, edge|1)`.

#### 5.5.2 Ball-ball minimize `0x1001a370` [V]

```
ca = cache(syn0.obj); cb = cache(syn1.obj)
v  = (f32)(ca.pos - cb.pos)                            // world, object origins (+0x90)
L2 = |v|² (f32)
inv = (|L2| <= 1e-19) ? 1.0 : invsqrt(L2)             // inlined, 5 Newton steps
md.len_numerator = inv*L2 - md.sum_extra_radius       // |v| - (r0+r1)
md.normal = v*inv                                      // from B (syn1) to A (syn0)
md.f58 = (ca.core_pos - cb.core_pos)·md.normal
return 1
```

#### 5.5.3 Ledge geometry helpers used [V]

- `bary(ledge, e, X)` `FUN_10020ad0` → f32[4]: with the triangle's vertices `A = start(e0)`, `B = start(e1)`,
  `C = start(e2)`, `u = A−B`, `v = C−B`, `D = |u|²|v|² − (u·v)²` (out[3]); out[0..2] are the barycentric weights
  × D, **rotated so that out[j] belongs to the j-th edge counted from `e`** and is the weight of the vertex
  opposite that edge (out[0] > 0 ⇔ X projects on the triangle's side of `e`). Inside test: none of the three has
  its sign bit set (so −0.0 counts as outside). Arithmetic: exact point differences, f64/extended accumulation,
  f32 results.
- `edge_params(ledge, e, X)` `FUN_10020a10` → f32[2]: `d = Q−P`; `out[0] = (X−P)·d`, `out[1] = (Q−X)·d`.
- `dist2_point_segment(X, ledge, e)` `FUN_10021740`: both params ≥ 0 → squared line distance
  `|(X−P)×(Q−P)|²/|Q−P|²` (`FUN_10021420`), else squared distance to the nearer endpoint (`FUN_100216f0`).
- `triangle_normal(e, ledge)` `FUN_10021280`: `(Q−P)×(R−P)` with `Q = end(e)`, `R = start(prev(e))`, f64.

All comparisons against zero in this section use the f32 constant `0x10063370` = 0.0f.

#### 5.5.4 Ball-polygon minimize driver `0x1001a220` [V]

`bc = cache(syn0.obj)`; `ctx = PolyCtx{points, ledge, cache(syn1.obj), syn1.obj, &syn1}`;
`bctx = {bc, syn0.obj, &syn0}`; switch `syn1.status`: 0 → `BPmin`, 1 → `BKmin`, 2 → `BFmin`; returns the
callee's code; caches `ref--`.

#### 5.5.5 `BFmin(solver, bctx, e, ctx)` `0x10030d50` [D]

```
X = ctx.cache.world_to_obj(bc.pos)                    // f64, polygon object space
λ = bary(ledge, e, X)
if any sign bit of λ[0..2]:
    best = argmin over g in (e, next(e), next(next(e))) of dist2_point_segment(X, ledge, g)  // strict <, init 1e101
    return BKmin(solver, bctx, best, ctx)
n = normalize(triangle_normal(e, ledge))              // FUN_1000e120
md.normal = (f32)(ctx.cache.R * n)
syn1.edge = e; syn1.status = 2
h = n·X - n·P                                         // P = start(e) (f32 → extended)
if h >= 0.0:
    md.len_numerator = (f32)(h - r); md.f58 = ...; return 1
syn1.edge = e; syn1.status = 5; solver.backside = X; return 3
```
No loop-check decrement here.

#### 5.5.6 `BKmin(solver, bctx, e, ctx)` `0x10032860` → `BKcore` `0x10032910` [D]

```
BKmin:
    X = world_to_obj(bc.pos)
    (a, b) = edge_params(ledge, e, X)
    if a < 0: return BPmin(solver, bctx, e, ctx)          // nearer P
    if b < 0: return BPmin(solver, bctx, next(e), ctx)    // nearer Q (start of next(e))
    return BKcore(solver, bctx, e, ctx)

BKcore:
    if --solver.counter < 0 and loop_check(solver, 3, 0, 1, e): return 2
    X = world_to_obj(bc.pos)
    P = start(e); Q = end(e); R = start(prev(e)); S = start(prev(opp(e)))  // S: third vertex of the twin triangle
    d = Q-P; x = X-P; r = R-P; s = S-P                    // f32 diffs widened to f64
    n1 = d x r;  n2 = s x d                               // the two face normals (unnormalised, outward)
    h1 = x·n1;   h2 = x·n2
    λ1 = bary(ledge, e, X)[0];  λ2 = bary(ledge, opp(e), X)[0]
    if λ1 <= 0:
        if λ2 <= 0:                                       // in the edge's region
            if h1 < -1e-19 and h2 < -1e-19:               // behind both faces (f64 0x10063af0 = -1e-19)
                solver.backside = X; syn1.edge = e; syn1.status = 5; return 3
            c   = d x x
            id2 = 1/|d|²
            q   = |c|²*id2                                // squared distance to the line
            is  = invsqrt_f64(q)                          // FUN_1000db80
            md.len_numerator = (f32)(is*q - r)
            w   = R * (d x c)                             // world
            md.normal = (f32)(-(is*id2) * w)              // unit, from the edge line toward the centre
            md.f58 = ...; syn1.edge = e; syn1.status = 1; return 1
        return BFmin(solver, bctx, opp(e), ctx)
    if λ2 <= 0 or h2 <= 0.0: return BFmin(solver, bctx, e, ctx)
    return BFmin(solver, bctx, opp(e), ctx)
```

#### 5.5.7 `BPmin(solver, bctx, e, ctx)` `0x10032050` [D]

```
if --solver.counter < 0 and loop_check(solver, 3, 0, 0, e): return 2
Pw = ctx.cache.obj_to_world(point[start(e)])          // FUN_100217d0
X  = world_to_obj(bc.pos)
v  = bc.pos - Pw;  L = normalize_ret_len(v)           // FUN_1000de30: returns 0 and leaves v if |v|² < 1e-19
md.normal = (f32)v;  md.len_numerator = (f32)(L - r);  md.f58 = ...
P = point[start(e)];  x = X - P;  c0 = P·x             (f64: 0x100321a1..0x10032203, no f32 rounding)
best = NULL; bestv = 0
g = prev(opp(prev(e)))                                // an edge N -> P
loop:
    N = point[start(g)]
    f = N·x - c0                                      // = (N-P)·(X-P), stored f64 (0x10032235)
    if f > 0:
        w = invsqrt4((f32)|N-P|²) * f                 // N-P and its square on the x87; only |N-P|² is f32
        if w > bestv: bestv = w; best = g
    if g == prev(e): break
    g = prev(opp(g))
if best:
    (a, b) = edge_params(ledge, best, X)              // along N -> P
    if b > 0:                                          // not beyond P
        return a >= 0 ? BKmin(solver, bctx, best, ctx)    // inside segment N-P
                      : BPmin(solver, bctx, best, ctx)    // beyond N: move to point N
syn1.edge = e; syn1.status = 0; return 1
```

#### 5.5.8 Outputs summary

After a successful ball minimize (`rc == 1`): `syn1.edge/status` name the closest feature (P/K/F),
`len_numerator = feature distance − r` (f32), `normal` (f32, world, unit, polygon → ball), `f58`. These are the
inputs of the event solver (5.2) and of contact creation (4.8). For ball-ball, synapses do not
change.

### 5.6 What the convex cases share (for chapter 6)

- Same `S` struct, same `find_event`/`search_below`/`regula_falsi`, same Event_Sim and the slot grid.
- `0x1002d6c0`: picks `X = syn[k]`, `Y = syn[k^1]`, builds two PolyCtx (`points, ledge, cache, obj`), sets
  `S.type = 0`, dispatches on `(X.status, Y.status)`: PP `0x1002bcf0`, PK `0x1002c820`, PF `0x1002aed0`,
  KK `0x1002b690` (args `(edgeX, edgeY, &ctxX, &ctxY)`, thiscall on `S`); anything else crashes.
- Event codes per 5.2.3; functor vtables `0x100639a8/b0/b4/b8/d8`.

### 5.7 Port notes

- Keep the root finder bit-for-bit in structure: the 5 ms slot grid anchored at `t_now`, `trunc(step·200)` with
  minimum 1, the `+1e-8` overshoot clamp, regula falsi with the every-4th-iteration midpoint pull and the
  `it > 64` exit returning `t_lo`. These decide when contacts are created and thus the feel at impacts.
- Distances are f32 in the mindist; the solvers compute in f64 (x87 extended in the original).
- `invsqrt` helpers (`FUN_1000dae0` f32 input, `FUN_1000db80` f64 input, inlined copies) converge to f64
  precision; use `1.0/sqrt(x)`.
- Illegal table entries should assert. Keep the ball in synapse 0 with `k = 0`.
- The global coll-dist index decay counter (`0x10075edc`) is shared across mindists; with all `coll_dist[i]`
  equal (0.01) it has no effect unless something rewrites the table (check chapter 4).

### 5.8 Open questions

1. `md.f58` consumer and exact meaning (mass-centre separation along the normal) — check friction/impact code.
2. BP phase-2 threshold `|PQ|·min(dmax², cd²)·inv_diam` is dimensionally length²; literal port, meaning unclear.
3. Who calls `FUN_10017870` with `mode` 0/1/2 and `allow_hull`, and which recalc driver (`0x100197f0` vs
   `0x10019950`) runs when (chapter 4).
4. `core+0x1a8` (unit rotation axis) and `core+0x1c0` (rotational surface speed) are inferred from use.
5. Exact output order of `bary()` slots 1 and 2 is not needed by the ball code (only "any negative" and slot 0
   are), but the surface walk and convex code index them; confirm with chapter 1.
6. Whether `coll_dist[]` is ever changed at runtime (the 64-entry ramp collapses to a constant at init).

## 6. Mindist event solver: convex (polygon-polygon) cases

Scope: the 4x4 table at `0x1007632c` and the polygon entry `0x1002d6c0` with its four cases
(PP `0x1002bcf0`, PK `0x1002c820`, PF `0x1002aed0`, KK `0x1002b690`), the distance functors they use
(`0x1002a9c0..0x1002b681`), the shared per-object pose cache `0x1002b300` and the time search
`0x10037790 / 0x10037910 / 0x10037b30`. The ball cases (`0x1002d4d0`, `0x1002d5f0` and callees) are
in chapter 5; shared pieces are flagged.

### 6.0 What this table is (correction to the address map)

The table at `0x1007632c` is **not** the closest-feature minimizer. It is IVP's *mindist event
solver*: given the closest-feature pair the minimizer already found (synapse statuses and edges) and the
current distance, it predicts **the earliest time inside the current PSI** at which either

- the distance drops to the collision distance ("hit", code `0xN0`), or
- the closest-feature pair stops being valid, i.e. a Voronoi-region boundary is crossed (codes `0xN1`,
  `0xN2`), so the minimizer has to run again.

The only caller is the mindist event recalc `FUN_10017870` (`call [edx*4+0x1007632c]` at `0x10017b3b`).
Nothing in `0x1002a540-0x1002d8e0` walks features, iterates to convergence or handles penetration.
The minimizer lives elsewhere: the functions that use the edge next/prev tables (`0x100685b8/c8`) are
`0x10030d50`, `0x10031440`, `0x10032050`, `0x10032350` and `0x10032860..0x10032e60` (narrowphase range)
plus `0x10019aa0`. Feature walking, loop detection, iteration limits and penetration fallback belong to
that section, not this one. The convex minimizer itself is specified in chapter 3 4.9 (chapter 3), with corrections in 65_minimize_review.md. The closest thing to "already touching" handling here is the
close branch of `0x10037b30` (6.4.3).

`0x1002a540-0x1002a9b4` are constraint methods (write `this+0x2c+4i` etc., call `0x100283f0`). They are
not collision code; they only share the address window.

### 6.1 Dispatch

#### `0x1002d860` table init (static initializer)

The 16 entries are indexed `4*status(first) + status(second)`. Synapse status: 0 = POINT, 1 = EDGE,
2 = FACE, 3 = BALL.

| idx | pair | target |
|---|---|---|
| 0, 1, 2, 5 | PP, PK, PF, KK | `0x1002d6c0` polygon entry |
| 12, 13, 14 | BP, BK, BF | `0x1002d4d0` (chapter 5) |
| 15 | BB | `0x1002d5f0` (chapter 5) |
| all others (KP, FP, FK, FF, PB, KB, FB, KF) | | `0x1002d850`: writes to address 0 (deliberate crash) |

So the mindist must always order its synapses so that status(first) <= status(second) for polygons
(and ball first). "first" is selected by mindist flag bit 8 (6.2).

#### Caller-side index computation (in `FUN_10017870`, for reference)

```
f      = m->flags (m+0x14)
first  = (f >> 8) & 3            // 0 or 1 in practice
second = ((f ^ 0x100) >> 8) & 3
idx    = 4 * m->syn[first].status + m->syn[second].status   // status: short at syn+0x1a
table[idx](&solver)
```

#### `0x1002d6c0` polygon entry (cdecl, one arg: `EventSolver *s`)

```
m  = s->mindist                                   // s+0x20
A  = &m->syn[(m->flags >> 8) & 3]                 // m+0x18 + i*0x1c
B  = &m->syn[((m->flags ^ 0x100) >> 8) & 3]
for X in (A, B):
    edge   = X->edge                              // syn+0x14 (IVP_Compact_Edge*)
    tri    = edge & ~0xF
    ledge  = tri - ((tri->dword0 & 0xFFF) + 1) * 16
    info.points = ledge + ledge->c_point_offset   // *(int*)ledge, float4 array
    info.ledge  = ledge
    info.cache  = cache_of(X->obj)                // 0x1001a190: refcount++ and refresh (below)
    info.obj    = X->obj                          // syn+0x10
s->result = 0                                     // s+0x38
switch (A->status, B->status):
    (0,0): PP(edgeA, edgeB, &infoA, &infoB)       // 0x1002bcf0
    (0,1): PK(...)                                // 0x1002c820
    (0,2): PF(...)                                // 0x1002aed0
    (1,1): KK(...)                                // 0x1002b690
    else : write to address 0
infoA.cache->refcount--; infoB.cache->refcount--
```

All four cases are `__thiscall` with `ecx = s`, args `(edgeA, edgeB, CaseInfo *infoA, CaseInfo *infoB)`,
`ret 0x10`.

`cache_of(obj)` (`0x1001a190`, identical inline code in the ball entries):

```
if (!obj->cache) obj->cache = cache_mgr_alloc(obj->core->cache_mgr?, obj)   // 0x10018930 on obj->env(+0x18)->+0xa4
obj->cache->refcount++
if ((int8)obj->movement_state < 8 && env->time_code > obj->cache->time_code)   // obj+0x80 low byte; env+0x138
    cache_refresh(obj->cache)                     // 0x10018a40: pose at env->current_time
return obj->cache
```

Movement state (obj+0x80 low byte): `< 8` simulated (moving/slow/calm), `>= 8` not simulated
(IVP_MT_NOT_SIM = 8, STATIC = 0x10; hypothesis from public IVP, consistent with the code).

### 6.2 Structures relied on

#### Event solver context (`EventSolver`, on `FUN_10017870`'s stack, 0x48 bytes)

| off | type | meaning (as filled by `FUN_10017870`) |
|---|---|---|
| +0x00 | f64 | `coreA.rot_surface_speed + coreB.rot_surface_speed` (core+0x1c0) |
| +0x08 | f64 | `n . (v_second - v_first)` (n = mindist normal m+0x68, v = core+0xa4) |
| +0x10 | f64 | **approach-speed bound along n**: `+0x08 + sqrt(1.001 - (n.axis_first)^2)*first.rot_surface_speed + sqrt(1.001 - (n.axis_second)^2)*second.rot_surface_speed` (axis = core+0x1a8, 1.001 = `0x100637e8` f64 1.0010000467300415) |
| +0x18 | f64 | **general speed bound**: `coreA.speed + coreB.speed + (+0x00)` (speed = core+0x1b8, abs linear speed) |
| +0x20 | ptr | mindist |
| +0x24 | ptr | environment |
| +0x28 | f64 | `t_now` = env+0x120 |
| +0x30 | f64 | `t_max` = env+0x128 (time of the next PSI) |
| +0x38 | i32 | result code, 0 = no event (set to 0 by the entry) |
| +0x40 | f64 | event time; each case initialises it to `+0x30` and only ever lowers it |

The caller only reaches the table when an event inside this PSI is possible:
`(t_max - t_now) * s.0x10 + coll_dist[idx] > m.len` (`len` = m+0x54), and copies `result & 0xff`
into the low byte of m+0x14 afterwards.

#### `CaseInfo` (16 bytes, case argument)

| off | meaning |
|---|---|
| +0 | `float4 *points` (ledge + ledge->c_point_offset) |
| +4 | `IVP_Compact_Ledge *ledge` |
| +8 | `Cache *cache` (per-object pose cache, below) |
| +0xc | `Real_Object *obj` |

#### Mindist fields used

| off | type | meaning |
|---|---|---|
| +0x14 | u32 | flags: bits 0-7 last event code, bit 8 "first synapse index", bits 22-29 `coll_dist` index (`(f >> 22) & 0xff`) |
| +0x18 / +0x34 | Synapse[2] (0x1c each) | +0x10 obj, +0x14 compact edge, +0x1a short status |
| +0x50 | f32 | `sum_extra_radius` (0 for polygon pairs; ball radius otherwise) |
| +0x54 | f32 | `len`: current distance minus `sum_extra_radius` (geometric distance = len + extra) |
| +0x68 | f32[3] | contact normal, world (direction from second toward first; inferred from sign use) |

#### Mindist settings (global object at `0x10075db0`, built by `0x10015fe0(0.01)`)

| addr | settings off | value | used here as |
|---|---|---|---|
| `0x10075db0` | +0x000 | 0.1 x 0.01 = **0.001** | `real_coll_dist` (lower search distance `v2`) |
| `0x10075db8 + 4i` | +0x008.. | **0.01** for every i in 0..63 | `coll_dist[idx]` (hit distance) |
| `0x10075ed4` | +0x124 | 0.1 x 0.01 = **0.001** | small negative Voronoi thresholds |

(Floats; the file image holds zeros, so the disassembler annotates them as 0.)

#### Core fields used

| off | type | meaning |
|---|---|---|
| +0x04 | f32 | radius (`upper_limit_radius`, hypothesis) |
| +0x08 | f32 | radius-like value multiplied into +0x1c0 (`max_surface_deviation`, hypothesis) |
| +0x48 | f32 | `0.5 / core+0x04` (`inv_object_diameter`; set in `0x1000d1a0`) |
| +0x70 | f32 | 1 / PSI length (set in `0x1001e300`) |
| +0xa4 | f32[3] | linear velocity (world) |
| +0x1a8 | f32[3] | unit rotation axis (world) |
| +0x1b8 | f32 | \|v\| (`0x1001e300`) |
| +0x1bc | f32 | rotation speed bound, rad/s: `2*asin_approx(\|q_vec\|) * core+0x70` (`0x1001e870`) |
| +0x1c0 | f32 | `core+0x1bc * core+0x08` |

#### Pose cache (`Cache`, 0xd0 bytes, ring of entries in the cache manager; `0x10018930/0x10018a40`)

| off | type | meaning |
|---|---|---|
| +0x00 | i32 | time code (env+0x138) at which it was computed |
| +0x04 | i32 | refcount |
| +0x08 | ptr | real object |
| +0x10 | f64[4] | quaternion at that time |
| +0x30 | Matrix (0x80) | object -> world at `t_now`; rows at +0x30/+0x50/+0x70, translation at +0x90 |
| +0xb0 | f64[3] | core position |

Helpers on it: `0x10018d10` float3 object point -> f64 world point; `0x10018ea0` rotate f64 vector
object -> world; `0x10018dc0` rotate f64 vector world -> object.

#### Matrix (`IVP_U_Matrix`, 0x80 bytes)

Three rows of 4 doubles (`+0x00`, `+0x20`, `+0x40`; only 3 used) and translation `vv` at `+0x60`.
`world = R * p + vv`.

| fn | op |
|---|---|
| `0x1000f550` | `out = R*p + vv` (f64 in) |
| `0x1000f5f0` | same, float3 in, f64 out |
| `0x1000f6f0` | `out = R*v` (f64) |
| `0x1000f760` | `out = R^T * v` (f64) |
| `0x1000e280` | `this = a x b` (f64, args `a, b`) |
| `0x1000e480` | `\|v\|` of a float3 (fsqrt) |
| `0x1000dd20` | `\|v\|` of a double3 (fsqrt) |
| `0x1000dd50`, `0x1000e120` | normalize double3 in place; return 0 and leave it unchanged if `\|v\|^2 < 1e-19` |
| `0x1000dae0` | `1/sqrt(x)` of a float arg |

**Fast inverse square root** (`0x1000dae0` and `0x1000dd50`: 4 Newton steps; `0x1000db80`, `0x1000e120`,
`0x1000de30`: 5 steps). The initial guess is built from the high word of the double `x`:
`hi' = ((0x7ff00000 - hi) >> 1) + 0x1ff00000` (arithmetic shift), low word 0. Each step is
`y = y * ((0.5 - (0.5*x)*y*y) + 1.0)`. The measured relative error is about 8e-15 after 4 steps and 3e-16
after 5 (see 0.1), so `1/sqrt` is an adequate replacement. `0x1000dae0` takes an f32 argument, so its input
is rounded to f32 first.

#### Event-sim object (built on each case's stack by `0x1002b300`, 0xae8 bytes)

| off | meaning |
|---|---|
| +0x00 | real object (`cache+0x08`) |
| +0x04 | core (`obj+0xa4`) |
| +0x10 | `cache+0x00` (time code; unused afterwards) |
| +0x14 + 4i, i = 0..20 | `Matrix *slot[i]`: object -> world at `t_now + i * 0.005` |
| +0x68 + 0x80i | `Matrix store[21]` (constructor `0x1000c2a0` is a no-op) |

`0x1002b300(sim, cache)`:
```
sim.obj  = cache.obj; sim.core = sim.obj->core
if ((int8)sim.obj->movement_state >= 8)        // not simulated: one pose for all times
    slot[0..20] = &cache.matrix
else
    slot[1..20] = NULL; slot[0] = &cache.matrix
sim.+0x10 = cache.time_code
```
A slot `i > 0` is filled on demand with `obj->pose_at(t, &store[i])` (`0x10009d70`, thiscall
`(double t, Matrix *out)`: the object's interpolated world matrix at time t).

### 6.3 Edge navigation used by the cases

The compact edge dword holds `start_point_index` (bits 0-15, `& 0xffff`) and `opposite_index` (bits
16-30, signed 15-bit, read as `(e << 1) >> 17`, in units of 4-byte edges). The triangle is
`edge & ~0xf`. Its three edges sit at triangle +4, +8 and +0xc.

| table | indexed by `edge & 0xc` | meaning |
|---|---|---|
| `0x100685b8` | {0, +4, +4, -8} bytes | `next(e)`: +4 -> +8 -> +0xc -> +4 |
| `0x100685c8` | {0, +8, -4, -4} bytes | `prev(e)` |

```
start(e) = points[e & 0xffff]                     // float4, xyz used
end(e)   = start(next(e))
opp(e)   = e + 4 * opposite_index(e)
// all edges leaving the start point P of e0 (one ring), e0 itself last:
ce = opp(prev(e0)); loop { visit(ce); if ce == e0 break; ce = opp(prev(ce)) }
```

Triangle normal (`0x10021280(edge, ledge, f64 out[3])`, cdecl; also inlined in PK). Not normalized,
ledge space:
```
p0 = start(e); pn = start(next(e)); pp = start(prev(e))
n  = (pn - p0) x (pp - p0)           // floats subtracted on the x87 stack, result f64
```
The case code always normalizes it afterwards (`0x1000dd50`).

### 6.4 Time search primitives

Each test is a **functor**: an object whose vtable slot 0 is `double eval(Matrix *mA, Matrix *mB)`.
At `+0x08` it holds a speed bound `speed` (f64), at `+0x10` `inv_speed = 1/speed` (f64), and its
geometry from `+0x28` on. A functor returns a scalar that decreases toward an event. An event happens
when the value drops to `target` or below.

#### 6.4.1 `0x10037790` regula falsi refinement

`thiscall(F)`, args `(double *out, double t0, double t1, double target, double f0, double f1,
Object *objA, Object *objB)`, `ret 0x34`. Requires `f0 > target >= f1`.

```
count = 0
loop:
    t = t0 + (t1 - t0) * (target - f0) / (f1 - f0)
    if ((count & 3) == 3):
        if (count > 0x40) { *out = t0; return }           // gives up after 67 evaluations, keeps the safe side
        t = t + ((t0 - t) + (t1 - t)) * 0.375             // 0x10063b80: pull 75% toward the midpoint
    objA->pose_at(t, &mA); objB->pose_at(t, &mB)          // fresh, not cached
    f = F.eval(&mA, &mB)
    if (|f - target| < 1e-8) { *out = t; return }         // 0x100633b8
    count++
    if (f < target) { t1 = t; f1 = f } else { t0 = t; f0 = f }
```
There is no guard for `f1 == f0`.

#### 6.4.2 `0x10037910` conservative advancement, "find first t with f(t) <= target"

`thiscall(F)`, args `(double target, double t_start, double t_max, int i_start, Sim *simA, Sim *simB,
double *f_start /*nullable*/, double *out)`, `ret 0x2c`. Returns 1 when it found an event (and writes
`*out`), otherwise 0.

```
f = f_start ? *f_start : F.eval(simA.slot[0], simB.slot[0])
if (f <= target) { *out = t_start; return 1 }
t = t_start; i = i_start
loop:
    dt_safe = (f - target) * F.inv_speed
    d       = t - t_max                                   // extended precision
    if (d + dt_safe > 0) return 0                         // cannot reach target before t_max
    step = 2 * dt_safe
    if ((float)d + step > 0) step = (t_max - t) + 1e-8    // quirk: d rounded to f32 for this test only
    n = (int)(step * 200.0)  /* _ftol, truncation */ ; if (n < 1) n = 1
    i += n
    t_new = t + n * (double)0.005f                        // 0x10063a30 = 0.004999999888241291
    mA = simA.slot[i] ?: (simA.slot[i] = &simA.store[i], simA.obj->pose_at(t_new, simA.slot[i]))
    mB = same for simB
    f_new = F.eval(mA, mB)
    if (f_new > target) {
        if (i == 20) return 0                             // exact compare: i > 20 would overrun slot[];
                                                          // unreachable because t_max - t_start <= 1 PSI (~3 steps)
        t = t_new; f = f_new; goto loop
    }
    rootfind(&t_hit, t, t_new, target, f, f_new, simA.obj, simB.obj)   // 0x10037790
    if (t_hit - t_max > 0) return 0
    *out = t_hit; return 1
```
Slot `i` stands for time `t_start + i*0.005f`. That holds because every call within one case starts
at `(t_now, i = 0)`, or continues `(t_new, i)` from `0x10037b30`. Static objects return the same
matrix for every slot.

#### 6.4.3 `0x10037b30` hit search with the "already within distance" branch

`thiscall(F)`, args `(double target, double v2, double t_start, double t_max, Sim *simA, Sim *simB,
double *f_start /*nullable*/, double *out)`, `ret 0x30`.

```
f0 = f_start ? *f_start : F.eval(simA.slot[0], simB.slot[0])
if (f0 > target)
    return FUN_10037910(F, target, t_start, t_max, 0, simA, simB, &f0, out)
// already at or below the hit distance
inv = 1.0 / F.speed
if (!(t_start - t_max < 0)) return 0
t = t_start; i = 0; f = f0
loop:
    step = (f - v2) * inv                                 // time to close down to v2 at max speed
    if ((t - t_max) + step > 0) step = (t_max - t) + 1e-8
    else if (step < 0) step = 0
    n = max(1, (int)(step * 200.0)); i += n; t_new = t + n * (double)0.005f
    f_new = F.eval(slotA[i] (computed at t_new if missing), slotB[i] ...)
    if (f_new > target) {                                 // moved out of the hit zone: look for re-entry
        if (t_new - t_max <= 0)
            return FUN_10037910(F, target, t_new, t_max, i, simA, simB, &f_new, out)
        return 0
    }
    if (f_new <= f0) { *out = t; return 1 }               // not separating versus the start: event at the previous sample (t_start on the first pass)
    t = t_new; f = f_new                                  // still inside but separating: keep sampling
    if (t_new - t_max < 0) goto loop
    return 0
```
Quirk: it compares against the initial value `f0`, never against the previous sample.

### 6.5 Distance functors (vtables at `0x100639a8..0x100639dc`)

Each vtable has one slot. All functor arithmetic is double. "A-space" means object A's ledge
coordinates. `wX = MX * p` is the world point and `RX * d` the world direction.

| vtable | eval | fields | value |
|---|---|---|---|
| `0x100639ac` | `0x1002a9c0` | +0x28 p (A), +0x48 n (B), +0x68 q (B) | `(MA p - MB q) . (RB n)`: signed distance of p from the plane (q, n) |
| `0x100639a8` | `0x1002ae80` | +0x28 d (A), +0x48 n (B) | `(RA^T (RB n)) . d` = `(RA d).(RB n)` |
| `0x100639b0` | `0x1002aae0` | +0x28 d (space of arg1), +0x48 n (space of arg2) | `(R1 d) . (R2 n)` |
| `0x100639b8` | `0x1002b530` | +0x28 pA, +0x48 dA (A); +0x68 pB, +0x88 dB (B); +0xa8 sign | `sign * ((MA pA - MB pB) . c) * isqrt_f32(\|c\|^2)`, with `c = (RA dA) x (RB dB)` (isqrt `0x1000dae0` of the float-rounded squared length) |
| `0x100639b4` | `0x1002b620` | +0x28 dA (A), +0x48 dB (B) | `\|(RA dA) x (RB dB)\|^2` |
| `0x100639c8` | `0x1002ab30` | +0x28 pA (A), +0x48 pB (B), +0x68 u (world, constant) | `d = MB pB - MA pA; s = 1.2*(d.u)` (1.2 = `0x100639a0`, f64 1.2000000476837158); returns `s` if `s*\|s\| < \|d\|^2`, else `sqrt(\|d\|^2)` |
| `0x100639bc` | `0x1002abc0` | +0x28 p1 (arg1 space), +0x48 p2, +0x68 v (arg2 space) | `(M2 p2 - M1 p1) . (R2 v)` |
| `0x100639dc` | `0x1002ad90` | +0x28 h (f64), +0x30 p (f32, A), +0x40 q (f32, B), +0x50 e (B, unit), +0x70 c (B, unit) | `w = (MB q - MA p) x (RB e)`; `len = \|w\|` (`0x1000dd20`); returns `min(len, w.(RB c) + h)` |
| `0x100639d8` | `0x1002ac40` | +0x28 p (A), +0x48 v (A), +0x68 q (B), +0x88 e (B) | `d = MA p - MB q; f = ((RB e) x d) x (RB e)`, normalized (`0x1000e120`); returns `(RA v) . f` |

(`0x100639c0` = f64 -0.5 and `0x100639d0` = f64 -0.30000001192092896 sit between the vtables.)
The ball cases reuse `0x100639ac`, `0x100639bc`, `0x100639c8` and `0x100639dc`.

### 6.6 Common case prologue

```
simA = Sim(infoA.cache); simB = Sim(infoB.cache)           // 0x1002b300
coreA = infoA.obj->core; coreB = infoB.obj->core
rotsum = coreA.rot_speed + coreB.rot_speed                  // core+0x1bc, summed on the x87 stack, stored f64
s.event_time = s.t_max                                      // s+0x40 = s+0x30
cd   = coll_dist[(m.flags >> 22) & 0xff]                    // 0.01
rcd  = real_coll_dist                                       // 0.001
eps  = settings+0x124                                       // 0.001
xr   = m.sum_extra_radius (m+0x50); len = m.len (m+0x54)
```
Every later search uses `t_start = s.t_now`, `i_start = 0`, `t_max = s.event_time` (current best) and
`out = &s.event_time`. A success overwrites `s.result`. The final event is therefore the earliest one,
and on equal times the test that ran later wins.

Point coordinates are float in the ledge and widened to double. Differences of two ledge floats are
taken on the x87 stack (`fld f32; fsub f32`), so they are exact in extended precision before the
double store.

### 6.7 PF: point (A) vs face (B), `0x1002aed0`

```
P  = start(edgeA)                    (A)
Q  = start(edgeB)                    (B)
nB = normalize(tri_normal(edgeB))    (B)

// 1. hit: point-plane distance
F = {vt 0x100639ac, speed = s.0x10, p = P, n = nB, q = Q}
f_now = xr + len                     // x87 sum of two f32, stored f64; passed as f_start (no evaluation at t_now)
if FUN_10037b30(F, target = xr + cd, v2 = 0.5*xr + rcd, t_now, s.event_time, simA, simB, &f_now, &s.event_time):
    s.result = 0x20

// 2. Voronoi: an edge leaving P dips below the face plane
G = {vt 0x100639a8, speed = rotsum + 1e-19, n = nB}
lim = (xr*0.1f + min(len, cd)) * -(eps * coreB.inv_diam) / cd      // order: (-(eps*coreB+0x48)) * (0.1*xr + min) / cd
nA  = RA_now^T (RB_now nB)           // 0x1000f6f0 on cacheB matrix, then 0x1000f760 on cacheA matrix
maxdev = (s.event_time - t_now) * (rotsum + 1e-19)   // event_time already includes step 1
for ce in ring(edgeA):               // edges leaving P, edgeA last
    v = end(ce) - P                  (A, f64)
    k = isqrt_f32((float)|v|^2)      // 0x1000dae0
    c0 = (nA . v) * k                // current cosine
    if (c0 < maxdev):                // only edges that could reach 0 in time
        G.d = v * k
        if FUN_10037910(G, target = lim, t_now, s.event_time, 0, simA, simB, &c0, &s.event_time):
            s.result = 0x21
```
`maxdev` is computed once, before the loop, from the event time after step 1.

### 6.8 PP: point (A) vs point (B), `0x1002bcf0`

```
PA = start(edgeA) (A);  PB = start(edgeB) (B)
// 1. hit: projected distance
Pf = {vt 0x100639c8, speed = s.0x10, pA = PA, pB = PB, u = -(double)m.normal}
if FUN_10037b30(Pf, target = cd + xr, v2 = 0.5*xr + rcd, t_now, s.t_max, simA, simB, NULL, &s.event_time):
    s.result = 0x10

// 2. Voronoi cones of both points
maxd = (s.event_time - t_now) * s.0x18 + len                   // distance bound at the event time
sA = |PA|_f32 * coreA.rot_speed + coreA.speed                   // |p| from 0x1000e480 (object origin, not mass centre)
sB = |PB|_f32 * coreB.rot_speed + coreB.speed
spdB = coreB.rot_speed * maxd + sB + sA                         // for the ring around PB
spdA = coreA.rot_speed * maxd + sB + sA                         // for the ring around PA
R  = max(coreA.radius, coreB.radius)                            // core+0x04
k  = -0.5 * min(maxd^2, cd^2) / R

for (e0, P0, Pother, sim_e0, sim_other, spd) in
        [(edgeB, PB, PA, simB, simA, spdB), (edgeA, PA, PB, simA, simB, spdA)]:
    Q = {vt 0x100639bc, speed = spd, p1 = Pother, p2 = P0}
    for ce in ring(e0):
        v  = end(ce) - P0                       (f64, e0's space)
        l2 = |v|^2;  ki = isqrt_f32((float)l2)
        Q.v = v * ki
        target = (l2 * ki) * k                  // = |v| * k
        // value: (P0_w - Pother_w) . v_w, which drops when Pother leaves P0's cone across edge ce
        if FUN_10037910(Q, target, t_now, s.event_time, 0, sim_other /*M1*/, sim_e0 /*M2*/, NULL, &s.event_time):
            s.result = 0x11
```

### 6.9 PK: point (A) vs edge (B), `0x1002c820`

```
P  = start(edgeA) (A, f32 kept);  E0 = start(edgeB), E1 = end(edgeB) (B)
e  = normalize(E1 - E0) (B, f64)

// 1. hit: distance from the line, with a projected variant
wP = cacheA.to_world(P); wE0 = cacheB.to_world(E0); we = cacheB.rot(e)       // 0x10018d10, 0x10018ea0
c  = normalize(cacheB.rot_inv((wE0 - wP) x we))                               // 0x10018dc0, 0x1000e120 (B space)
R  = {vt 0x100639dc, speed = s.0x10, h = 0.5*(cd + xr), p = P, q = E0, e = e, c = c}
if FUN_10037b30(R, target = cd + xr, v2 = 0.9f*xr + rcd, t_now, s.t_max, simA, simB, NULL, &s.event_time):
    s.result = 0x30                                   // 0.9 = 0x10063878 (f32)

// 2. P enters one of the two faces adjacent to the edge
F = {vt 0x100639ac, speed = s.0x18, p = (double)P}
for ce in [edgeB, opp(edgeB)]:
    p0 = start(ce); pn = end(ce); pp = start(prev(ce))       (B)
    nrm = (pn - p0) x (pp - p0)
    x   = normalize((pn - p0) x nrm)          // in-plane, perpendicular to the edge, pointing out of that triangle
    F.n = x; F.q = p0
    if FUN_10037910(F, target = -eps, t_now, s.event_time, 0, simA, simB, NULL, &s.event_time):
        s.result = 0x31

// 3. an edge leaving P turns toward the edge
dmin = len - (s.event_time - t_now) * s.0x18; if (dmin < 1e-8) dmin = 1e-8
sA = |P| * coreA.rot_speed + coreA.speed
sB = coreB.rot_surface_speed * coreB.rot_speed + coreB.speed    // QUIRK: core+0x1c0 * core+0x1bc (rot speed applied twice)
S  = {vt 0x100639d8, speed = (sB + sA) / dmin + rotsum, p = P, q = E0, e = e}
target = 2*coreB.inv_diam * min(len, cd) * -0.30000001192092896
for ce in ring(edgeA):
    S.v = normalize(end(ce) - P)                                  (A)
    if FUN_10037910(S, target, t_now, s.event_time, 0, simA, simB, NULL, &s.event_time):
        s.result = 0x32
```

### 6.10 KK: edge (A) vs edge (B), `0x1002b690`

```
A0 = start(edgeA), dA = normalize(end(edgeA) - A0)  (A)
B0 = start(edgeB), dB = normalize(end(edgeB) - B0)  (B)
c  = (RA_now dA) x (RB_now dB)                       // cache matrices
if (-(m.normal . c) > 0) { flag = 0; sign = -1.0 } else { flag = 1; sign = +1.0 }   // sign*c points along the normal

// 1. hit: signed distance between the lines
H = {vt 0x100639b8, speed = s.0x10, pA = A0, dA, pB = B0, dB, sign}
if FUN_10037b30(H, target = cd, v2 = rcd, t_now, s.event_time, simA, simB, NULL, &s.event_time) != 0:
    s.result = 0x40                                   // note: no extra radius term in KK

// 2. edges become parallel
J = {vt 0x100639b4, speed = 2*rotsum + 1e-19, dA, dB}
if FUN_10037910(J, target = 1e-19, t_now, s.event_time, 0, simA, simB, NULL, &s.event_time) != 0:
    s.result = 0x41

// 3. the other edge's direction enters an adjacent face's half-space
K = {vt 0x100639b0, speed = rotsum + 1e-19}
for (face_edge, dir, sim_dir, sim_face, ledge) in
      [(opp(edgeA), flag ? +dB : -dB, simB, simA, ledgeA),
       (edgeA,      flag ? -dB : +dB, simB, simA, ledgeA),
       (opp(edgeB), flag ? +dA : -dA, simA, simB, ledgeB),
       (edgeB,      flag ? -dA : +dA, simA, simB, ledgeB)]:
    K.d = dir                                         // negation by multiplying with f64 -1.0 (0x10063230)
    K.n = normalize(tri_normal(face_edge, ledge))     // 0x10021280 + 0x1000dd50
    target = -(eps * sim_dir.core->inv_diam)
    if FUN_10037910(K, target, t_now, s.event_time, 0, sim_dir /*M1*/, sim_face /*M2*/, NULL, &s.event_time):
        s.result = 0x42
```

### 6.11 Result codes (written to s+0x38, then to the low byte of m+0x14)

| code | case | meaning |
|---|---|---|
| 0x10 | PP | distance reaches `cd + xr` |
| 0x11 | PP | the other point leaves a point's Voronoi cone |
| 0x20 | PF | point-plane distance reaches `cd + xr` |
| 0x21 | PF | an edge at P crosses the face plane direction |
| 0x30 | PK | point-line distance reaches `cd + xr` |
| 0x31 | PK | P moves over one of the two faces at the edge |
| 0x32 | PK | an edge at P turns toward the edge |
| 0x40 | KK | line-line distance reaches `cd` |
| 0x41 | KK | edges become parallel (`\|dA x dB\|^2 <= 1e-19`) |
| 0x42 | KK | an edge direction enters an adjacent face's region |

Codes ending in 0 are collisions (impact or friction hand-over). Higher codes are feature changes
(re-minimize). What the recalc does with each is in the mindist-manager section.

### 6.12 Port notes

- **Module.** `ivp_event_poly.c`:
  - `EventSolver`, `CaseInfo` and `EventSim` with the 21 lazily filled slots;
  - the three search primitives, ported literally including the 0.005f quantization, the f32-rounded
    `d`, the 0.375 pull every 4th iteration, the 67-evaluation cap and the `i == 20` check;
  - nine functor structs, each with an eval function pointer, sharing a header `{speed, inv_speed}`.
- **Pose at time t.** `pose_at` must be the same interpolation the write-back uses (`0x10009d70`).
  Slot 0 must be the cache matrix at `t_now`, which is the same value.
- **Math.** Keep the double math and the float-to-double widening points as listed. `1/sqrt` may replace
  the bit-trick inverse square roots (`0x1000dd50`, `0x1000e120`, `0x1000dae0`), but keep the f32 rounding of
  `0x1000dae0`'s argument. Use real `sqrt` where it calls `fsqrt` (`0x1000e480`, `0x1000dd20`,
  `0x1002ab30`).
- **Mindist order.** Respect the invariant `status(first) <= status(second)` when the minimizer sets
  flag bit 8; otherwise the original would crash at `0x1002d850`.
- **Feel.** Ballance's polygon-polygon pairs are boxes and PH modules against floors and each other,
  so these cases decide only when convex contacts begin. The ball-floor path is in chapter 5.

### 6.13 Confidence and open questions

- **High** for control flow, argument mapping, constants, functor formulas and result codes. All were
  read from the disassembly; the decompiler output for these functions is unusable.
- **Medium** for field names of core +0x04/+0x08/+0x1bc and for "direction from second toward first"
  of m+0x68. The arithmetic is exact; the names are inferred.
- **Open.**
  - Whether the PK `sB` term (`core+0x1c0 * core+0x1bc`) is an original bug or intended. Port it
    as-is.
  - Why the PF/PK Voronoi limits scale with the **face/edge object's** `inv_object_diameter` while the
    rotating edge belongs to the other object. Port it as-is.
  - `s.0x10` can be 0 or negative when `len <= cd`. Then `inv_speed` is ±inf and the searches return 0
    or take minimum steps. This is unverified behavior; the x87 masks the exceptions.
  - The minimizer (feature walk, penetration) is not in this range; see 6.0.

## 7. End-to-end scheduling model (consolidated)

This chapter states the whole collision timeline once, using the functions of chapters 2-6. Times are
simulation seconds. `dPSI = 1/66`. "Speed" of an object means `s = |v| + |ω|·dev` (`core+0x1b8 + core+0x1c0`;
balls have `dev = 0`, so their rotation never grows a hull).

### 7.1 Frame and PSI

```
phys_frame(dt_frame):                                      # glue PostProcess, docs/physics.md 3.1
    target = env.time + dt_frame                            # 0x100138c0
    with x87 PC = 64-bit:                                   # 0x1002f250
        loop:                                               # 0x1002ef70
            m = event_list.min                              # float offset from base
            if !((target - base) > m): break                # an event exactly at target waits a frame
            ev = pop first; env.time = base + m; env.time_code++
            ev.simulate()                                   # PSI event or mindist event
        env.time = target; env.time_code++
```

The PSI event (`0x1002f2c0`), at `T = k·dPSI` (the first at 0):

```
env.time_of_last_psi = T; env.time_of_next_psi = T + dPSI
rebase event list to base = T (it is empty here, 2.2.5)
simulate_psi(T):                                            # 0x10013cb0
    step listeners; 0x10013c40; wheel pass (no-op)
    controllers (forces, constraints, friction system)      # 0x100124c0
    integrate moving cores                                  # 0x1001ea50 -> 0x1001e300 per core
        -> refresh |v| (+0x1b8), |ω| (+0x1bc), axis (+0x1a8), |ω|·dev (+0x1c0)
        -> hull_update per object: gradient = (|v|+|ω|dev)·1.00001, hull_next_psi = hull + gradient·dPSI
        -> objects whose hull min-key < hull_next_psi go into the check vector
    HULL phase (0x1001eb10): fire hull listeners whose key < hull_next_psi
        OV element  -> recheck_ov_element (pairs born / destroyed)
        OO watcher  -> child exchange (ledge-pair mindists born / destroyed), re-arm with its range share
        synapse     -> mindist_hull_limit_exceeded: halfspace re-estimate, re-arm if > 6·dPSI·S of slack,
                       else back to EXACT (exact recalc + event prediction, hint 0)
    SHORT phase (0x10017850): recalc_mindist for every EXACT mindist; a failed recalc -> INVALID list
    CRITIC phase (0x10017770): update_exact_mindist_events(md, allow_hull=1, hint=1) for every EXACT mindist
insert PSI event at T + dPSI
```

Between two PSI events the loop fires the mindist events that CRITIC (or impacts) queued, in time order. Ties
go newest first: the event list is a sorted list, not a heap (2.1.5).

### 7.2 Life of a pair

1. **Birth.** An object enters collision detection (Physicalize with Enable Collision, `0x10009350` →
   `0x100176e0`).
   - Its OV element sphere is set to `(mass centre, R + range_world)`. `R` is `core+0x04` and `range_world`
     comes from `0x1002dab0`: about `clamp(s·1, 0.5, min(5R, 15)) − dPSI·s`, floored at `0.1·s + R`.
   - Every element whose sphere overlaps is a candidate. A candidate becomes a pair when at least one of the
     two is moving, they are different units (`obj+0xa8`), and the filter chain passes (group strings,
     exclusive pairs).
   - The delegator root creates a plain mindist (both surfaces single-ledge) or an OO watcher.
   - The OV element then listens on the object's hull with budget `range_world`. The object is rechecked once
     it may have moved that far, so static objects never recheck on their own.
2. **OO watcher.** It never measures distance.
   - Its range is `get_coll_range_intra_objects(A,B)`: `r = clamp(0.5·S, 0.8, min(0.9·min(RA,RB), 10))`, minus
     `dPSI·S` and floored at `0.1·S`, then split by speed between the two objects.
   - It runs the child exchange with that range: a sphere query of each object's ledge tree around the other
     object's mass centre, padded by the other's radius, the extra radius and the range.
   - It keeps existing ledge-pair mindists (contacts persist), creates the missing ones and deletes the ones
     that left.
   - It re-arms each object's hull listener with that object's share.
3. **New mindist.** `mindist_init` (`0x10016490`) puts the ball in synapse 0, sets polygon synapses to POINT at
   the ledge's first point, and calls `insert_and_recalc_exact` (`0x10016f90`):
   - EXACT list;
   - `recalc_mindist` (`0x10019950`): the minimize walk to the closest features, writing `len` (+0x54), `n`
     (+0x68), `+0x58` and the synapse statuses;
   - `update_exact_mindist_events(md, allow_hull = core states < 0x21, hint 0)`.
4. **Prediction** (`0x10017870`), with `cd = 0.01`, `S = Σ(|v| + |ω|dev)` and `n` from B to A:
   - **Far:** `len > 2.1·dPSI·S + cd` → HULL state (status 5). The budget `len − cd` is split between the two
     objects' hulls: all of it to the moving side when one side is static, otherwise `s0 = sA + 0.1·sB`,
     `s1 = sB + 0.1·sA`, proportional.
   - **Separating:** `worst = n·(vB − vA) + Σ sqrt(1.001 − (axis·n)²)·|ω|dev < 1e-19` → no event this PSI.
     CRITIC re-predicts next PSI.
   - **Unreachable:** `(t_next − t_now)·worst + cd <= len` → no event.
   - **Otherwise** the event solver for the current feature pair returns the earliest `t` in
     `[t_now, t_next_psi]` at which:
     - the distance functor drops to `cd + extra radii` (a distance event, low nibble 0); or
     - a Voronoi boundary of the current features is crossed (a topology event, low nibble ≠ 0).

     Search: conservative advancement on a 5 ms grid anchored at `t_now`, with `step = trunc(200·2·(f−a)/speed)`
     grid cells, at least one, then regula falsi. Poses come from `pose_at(t)` = position `+ v·(t − t_psi)` with
     quaternion interpolation.
   - An event earlier than `t_now + 1e-6` is nudged forward according to `hint`: by the time to close the slack
     at speed `S` (or 10% of it), plus a tiny fraction of the PSI. It is dropped if that passes `t_next_psi`.
   - The event is queued at `float(t − base)` and the code stored in `flags & 0xff`.
5. **Event** (`0x100181b0`), at time `t`:
   1. `recalc_mindist` at `t`; the poses come from the cache at the new time code.
   2. A failed recalc does nothing more: `inter_penetration` has already pushed the objects apart and SHORT will
      move the mindist to INVALID.
   3. A topology event re-predicts with `hint 1`.
   4. Else, if `len < cd + 0.001`, **do_impact** (`0x100240a0`).
   5. Else (a near miss) re-predict with `hint 2`.
6. **Impact.** `do_impact` wakes both objects and bumps the impact counter. `0x10023cd0` then:
   - creates or finds the friction contact point for this mindist (`0x10022180`; geometry by `0x1001f860`);
   - runs the impact solver on it (elastic response, materials from the contact point);
   - post-processes the friction system;
   - re-predicts every other mindist of the touched cores (`0x1000d930` → `0x10009610`, `hint 2`);
   - fires the collision listeners (PhysicsCollDetection): normal, point and speed come from the contact
     situation `info`.
7. **Resting and rolling contact.** The friction system owns the contact point.
   - It recomputes the contact geometry every PSI (`recalc_friction_s_vals` `0x1001f860`) from the mindist's
     current closest features.
   - It drops the contact once `dist >= 0.045` (`max_dist_for_friction`).
   - The mindist itself stays EXACT and keeps being recalculated (SHORT) and predicted (CRITIC). A resting
     ball has `worst ≈ 0`, so it usually gets no events.
8. **Hull wake-up.** A HULL mindist's synapse fires when either object's hull has grown by its share.
   - The handler (`0x10017d70`) first tries the cheap halfspace update. The new budget is
     `len − Δ(angular hull) + Δ(centre separation along n) + intrusion`, using `+0x58` and `+0x60`.
   - If the budget still exceeds `6·dPSI·S`, both synapses are re-armed and no exact recalc happens.
   - Otherwise the mindist goes back to EXACT (step 3).
9. **Death.** A pair dies when:
   - an OV recheck no longer finds it (spheres apart, both objects non-moving, or the filter rejects it);
   - the watcher's exchange drops the ledge pair;
   - either object leaves collision detection or is destroyed.

   When a core freezes, its objects are rechecked as non-moving and their hull gradients are zeroed, so
   static-static pairs die. On revive (`0x1000aea0` → `0x1001d4d0`), exact mindists closer than 0.045 to another
   *movable* core get friction contacts directly. Contacts against fixed partners come back only through
   impacts.

### 7.3 What determines "when a pair is checked next"

| State | Next check |
|---|---|
| Not a pair yet | when either object's hull reaches its OV-element key (moved `range_world`), via `recheck_ov_element` |
| Watcher child set | when either object moved by its share of the intra range |
| HULL mindist | when either object's hull reaches the synapse key (moved its share of `len − cd`); then the halfspace test |
| EXACT, no event | every PSI: SHORT recalc + CRITIC prediction |
| EXACT, event queued | at the predicted time inside the PSI, and again in the next PSI's SHORT/CRITIC |
| Friction contact | every PSI, by the friction system (geometry only); the mindist continues as EXACT |

## 8. Port mapping

### 8.1 Modules

Suggested names; all under `src/phys/`. They replace `phys_collide.c` and the contact part of `phys.c`.

| Module | Contents | Spec |
|---|---|---|
| `ivp_math.h` | f64 3×3 + translation matrix (`IvpMat`: `R[3][3]`, `t[3]`), f64/f32 vectors, quaternion slerp used by `pose_at`, `ivp_isqrt` (= `1/sqrt`), the 1e-19 normalize guards | 0.1, 2.1.3, 5.1.3-5.1.4 |
| `ivp_geom.h/.c` | **Compact surface byte format** (keep it: 16-byte triangles, u32 edges, `& ~0xf`, `next/prev` tables, opposite offsets, ledge-relative points); the pancake triangle ledge (1.6.2, exact words); convex hull ledge (own hull builder: merge coplanar facets, triangulate, wind outward, twin edges, pierce = most anti-parallel); compile (spheres, top-down tree with the ±1e-6 band rule and the min-volume axis choice, preorder layout, point-pool merge); mass properties with the **`sqrt(m_y²+m_z²)` inertia quirk** and the flat fallback; surface radius and deviation; the ball dummy ledge; `get_single_convex`; `ledges_within_radius` (sphere + box test) | 1 |
| `ivp_cache.c` | per-object pose cache at `env.time` keyed by `env.time_code` (`m_world_f_object`, core position); `pose_at(obj, t)`; object↔world point/vector transforms | 4.2.7, 5.1.3 |
| `ivp_time.c` | `IvpMinList` with the exact tie order (sorted doubly linked list; skip links optional); time manager (`base`, float keys); PSI event; `simulate_until(target)` | 2.1.4-2.1.5, 2.2 |
| `ivp_hull.c` | hull manager embedded in the body (`last_time`, `gradient`, `center_gradient`, `hull`, `center_hull`, `hull_next_psi`, listener `IvpMinList`); `hull_update`, hull phase (≤ 102 calls per object), the 10 s rebase | 2.3 |
| `ivp_ov.c` | OV element and tree (or an equivalent sphere-overlap query, see the note in 2.6.5), range manager (two functions, constants verbatim), filter chain (group, exclusive pairs), delegator root `object_pairs_may_collide`, `recheck_ov_element` with the keep/new reconcile order | 2.4, 2.6 |
| `ivp_watcher.c` | OO watcher, child exchange `0x10016650` (ledge-pair keyed persistence, creation order), recursive mindist (optional: assert-only in Ballance) | 3.2-3.6 |
| `ivp_mindist.c` | mindist object (synapses, flags), manager lists (EXACT / INVALID, synapse lists per object), `insert_and_recalc_exact`, `recalc_mindist` (minimize driver with retries and backside fix), `update_exact_mindist_events`, `simulate_time_event`, `hull_limit_exceeded`, hull insert/remove, SHORT and CRITIC passes, `0x10009610`, `0x100099a0` | 4 |
| `ivp_minimize.c` | minimize solver: loop hash, table `0x10075ee0`, PP/PK/PF/KK/FF-init, BP/BK/BF/BB, pierce walk, ledge helpers (`F-values`, `K-values`, segment/line distances, KK setup/params) | 3.9-3.10, 5.5, 1.5 |
| `ivp_event.c` | event solver: block, table `0x1007632c`, event sims (21 lazy slots on the 5 ms grid), the three search primitives, nine functors, the cases BB/BP/BK/BF and PP/PK/PF/KK | 5.2-5.4, 6 |
| `ivp_contact.c` (stage 3b) | interface to the friction and impact systems: `try_to_generate_managed_friction`, the contact-point ctor fields, `recalc_friction_s_vals` geometry, `do_impact` | 4.8-4.9 |

### 8.2 Data the dynamics side must provide (PhysBody / core)

The current `PhysBody` lacks these core and object fields. All are computed in the integrator, `0x1001e300`,
after integration:

| Field | Definition | Used by |
|---|---|---|
| `upper_limit_radius` (`core+0x04`) | `obj.extra_radius + surface radius about the mass centre` (`0x1000bc40`: `cs.upper_limit_radius + |cs.mass_center − mc|`). Ball: the radius | ranges, child exchange, OV sphere |
| `max_surface_deviation` (`core+0x08`) | `(cs.max_factor & 0xff)·cs.upper_limit_radius·0.004f + |cs.mc − mc|`. Ball: 0 | `rot_surface_speed` |
| `inv_diam` (`core+0x48`) | `0.5 / upper_limit_radius` | Voronoi thresholds in the event solver |
| `speed` (`core+0x1b8`) | `|v|` (f32, real `sqrt`) | everything |
| `rot_speed` (`core+0x1bc`) | `2·asin_series(|q.xyz|)·(1/dPSI)` of the PSI rotation quaternion: `x = |q.xyz|`, `a = x + x³·(1/6)f + x⁵·0.40414f`; 0 if `|q.xyz|² <= 1e-19` | hull, event solver |
| `rot_axis` (`core+0x1a8`) | `R_world_f_core · (q.xyz/|q.xyz|)` (**world space**, f32; (1,0,0) if no rotation). Verified at `0x1001e985-0x1001ea01`: the rows of `core+0x128` times q, i.e. `R·q` | `worst` in prediction |
| `rot_surface_speed` (`core+0x1c0`) | `rot_speed · max_surface_deviation` | hull growth, speed sums |
| `pos_at(t)` | `core+0xb8 + core+0xd8·(t − core+0x68)` (f64 + f32·f64) | hull exceeded, child exchange |
| `pose_at(t)` (`0x10009d70`) | the same interpolation the write-back uses | event solver sims |
| movement state (`obj+0x80 & 7`, `core+0x60`) | moving vs not simulated (8); frozen cores rechecked (`0x1000ce20`) | pair filtering, hull split, caches |
| `time_code` (`env+0x138`) | incremented by every `set_current_time` | cache validity, recalc dedupe |

### 8.3 Plugging into the PSI loop

1. Replace the `while (psi_time + psi <= target)` loop in `phys_frame` with `ivp_time_simulate_until(w, target)`:
   - the PSI event calls `psi_step`;
   - mindist events call `ivp_mindist_simulate_event`.

   Keep the frame-time smoothing and the write-back.

   **Change:** an event exactly at `target` waits for the next frame. The first PSI fires at t = 0, i.e. at the
   start of the first frame with a positive target. The interim loop fires PSIs at the *end* of each interval
   (`time = psi_time + psi`), so the phase shifts by one PSI. Check the write-back interpolation against
   `time_of_last_psi`.
2. In `psi_step`, remove `phys_collide()` and the contact solve. The order becomes:
   1. controllers: forces, joints, and the friction system once ported;
   2. damping, gravity, integration (existing code);
   3. refresh the 8.2 core fields;
   4. hull update per moving body;
   5. hull phase;
   6. SHORT;
   7. CRITIC.

   Hull and pair state must be updated when bodies are created or destroyed, frozen or woken:
   - create: `enable_collision_detection`;
   - destroy: delete the OV element, which deletes its pairs;
   - freeze: recheck as non-moving and zero the gradients;
   - wake: `0x100099f0`, plus `0x1001d4d0` for contacts.
3. `phys_shape_*` builds an `IvpCompactSurface` (concave → pancakes, convex → hull ledge) instead of
   `PhysShape` clouds and BVH. Spheres stay analytic: `extra_radius` plus the dummy ledge.
4. **Staged hand-over, before the friction and impact port (docs/physics.md stage 3):** `do_impact` and the
   per-PSI friction pass need an adapter until those systems are ported.
   - `do_impact` gets the contact from the mindist: world point = polygon-side closest point (or ball centre
     − n·r), normal `n` from synapse 1 toward synapse 0, distance `len`. It applies the existing
     restitution/friction impulse and calls the impact listener (`speed`, `n`, point).
   - For resting contact, after SHORT, build a `PhysContact` for every EXACT mindist with `len < 0.045` and run
     the existing sequential-impulse solve.
   - This keeps IVP's pair set, feature selection, normals and impact timing while the solver is still interim.
     The final port replaces both with `try_to_generate_managed_friction` and the impact solver.
5. **Determinism.**
   - Iterate bodies in creation order.
   - Use CK ids, not pointers, for the ball-ball synapse order and for the OV/child hash *identities*. Hash
     values only matter for collision-free lookup, never for order.
   - Preserve the "last to first" loop orders in recheck and exchange; they set mindist creation order and
     therefore tie order in the event list.

### 8.4 Validation hooks

- **Minimize:** for random poses of ball vs pancake, box vs box and ball vs box, compare `len`/`n`/features
  with brute-force closest points. The KK four-face table is the medium-confidence part.
- **Event solver:**
  - a ball dropped onto a floor must get one 0x20 event and `do_impact` at `len < 0.011`;
  - a ball rolling across a pancake seam must switch BF → BK → BF through 0x31 events, or through SHORT recalcs
    while resting.
- **Scheduling counters:** the original keeps `env+0x78/7c/80` (live, created and deleted mindists),
  `env+0x84` (watcher checks), `env+0x88` (OV rechecks) and `env+0x13c` (impacts). Expose the same counters
  and compare with a logger in the original.

## 9. Open questions

Collected from all chapters, with the ones that matter most first.

1. **Friction and impact internals** are out of scope here. Their entry points and data are specified in 4.8
   and 4.9, but the contact-point geometry functions (`0x1001f050/0x1001f160/0x1001ee70/0x1001f3b0`) and the
   impact solver need their own spec.
2. **KK core four-face table** (`0x100317a0`, E0..E3 assignment): medium confidence. Validate with brute force.
3. **PK core with `q <= 1e-19`:** the original walks the neighbours with an uninitialised vector and leaves the
   normal in object space. Recommended deterministic replacement: treat it as converged (`return 1`) with the
   object-space normal rotated to world.
4. **Template pancake ledge** (`0x100763a8`): derived from the generator, not dumped. Dump it once from a
   running original.
5. **Convex hull triangulation order** (`0x10047890/0x10047c00`) was not reproduced. It only affects tie
   breaking: which triangle a mindist starts from, and equal-distance feature choices.
6. **Tree builder infinite recursion** when a split leaves one side empty. Guard it in the port and check the
   real level meshes once.
7. **BP phase-2 threshold** `|PQ|·min(dmax², cd²)·inv_diam` and the PK `sB = rot_surface_speed·rot_speed` term
   look like original quirks. Port them literally.
8. **`s.0x10 <= 0`** (approach bound) inside the "already within distance" search gives `inv_speed = ±inf`; the
   x87 masks it. A port must reproduce IEEE behaviour (no traps) or guard it identically.
9. `obj+0xa8` (unit identity for the "same unit" pair rejection) and whether `core+0xa4` and `core+0xa8` ever
   differ in Ballance.
10. `0x10017d50` (hull-reset callback): whether its sum is f32 or f64. The decompile suggests f32.
11. Caller of the anomaly manager's `inter_penetration` path beyond `recalc_mindist`'s vtable slot 5, and the
    meaning of `0x1000a3a0(obj, {dPSI,0,0})`.
12. `env+0x9c` (0x30-byte object `0x1002db50`) and `0x10013c40` in `simulate_psi` are unidentified.
13. Whether any Ballance mesh triggers qhull's sliver retry or joggle path, which would change the hull.
14. Unidentified vtable slots: polygon surface manager slot 11 `0x10003940`, ball surface manager slot 9.

## 10. Coverage and confidence

| Subsystem | Coverage | Confidence |
|---|---|---|
| Compact layouts, ledge helpers, ledge tree query | all functions | high |
| Builders: pancake ledge, tree, pool, mass properties | all functions; template words derived | high (medium for the template words) |
| Builders: convex hull | to the level of the resulting structure; qhull and 2-D triangulation not reproduced | medium |
| Time manager, min-list, PSI event, event loop | all functions | high |
| Hull manager and hull phase | all functions | high |
| Range manager, OV tree, filters, delegator root | all functions | high |
| OO watcher, child exchange, recursive mindist | all functions | high (the recursive mindist is unused) |
| Mindist settings, layout, lists, recalc driver, scheduler, passes | all functions | high |
| Hull-exceeded halfspace shortcut | key lines verified | medium-high |
| Minimize: ball cases, BB, pierce walk, PF, PK, PP | all functions | high |
| Minimize: KK core four-face table | rebuilt from the stack layout | medium |
| Event solver: driver, root finder, functors, all 8 cases | all functions, from the disassembly | high |
| Friction hand-over (contact-point ctor fields, recalc geometry dispatch, create/keep distances) | interface level | medium-high |
| Impact system | interface only | medium |
