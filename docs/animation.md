# 3D curves and object animations (CK2 2.1)

What OpenBallance needs for `CKCurve` / `CKCurvePoint`, `CKObjectAnimation` (with its controllers) and the
3DTransfo BBs **Position On Curve** and **Play Animation 3D Entity**. The addresses are function entry
points in the Ghidra project `ballance`: CK2_3D.dll (image base `0x10000000`), 3DTransfo.dll
(`0x25000000`), VxMath.dll (`0x24280000`). Offsets such as `+0x1b4` are fields of the original C++
objects and are given only so the code can be cross-checked.

## 1. What the game actually uses

Scan of `base.cmo`, `3D Entities/*.nmo`, `Level/*.NMO` and `PH/*.nmo` (class ids from the file object
table):

| File | Objects | Used by |
|---|---|---|
| `MenuLevel.nmo` | Curve (43) `#502 I_MenuLevel_Curve`, 4 CurvePoints (36) `#498..#501` | Position On Curve `#20` in `MenuLevel_Init`: target `Cam_MenuLevel` (`#497`, a target camera), Progression from Bezier Progression (Duration 44000 ms, loops), Follow 0, Bank 0, Bank Amount 1, Direction 5 (Z), Hierarchy 1, Roll 0 |
| `MenuLevel.nmo` | ObjectAnimation (15) `#1064 Record Anim`, entity `I_Ball_Stone` (`#1065`, 3D object, no parent) | Play Animation 3D Entity `#1058` in `Ball_Stone Script`: Duration 59246 ms (Time), linear progression curve, Loop 1 |
| `MenuLevel.nmo` | ObjectAnimation `#496 Kamera02`, entity `Cam_MenuLevel` | only in the camera's animation list (3D entity identifier `0x2000`); no BB plays it |
| `PE_Balloon.nmo` | KeyedAnimation (18) `#1842 UFO_Animation` with 8 ObjectAnimations `PE_UFO_Arm_*` | Play Global Animation `#1488` (not covered here; section 5.6) |

No other file has classes 15, 16, 18, 36 or 43. The `Animation` pin's type `43476550:11e52c0e` is the
ObjectAnimation parameter type: it is registered by the ObjectAnimation class (`FUN_1005c210`).

Feature use:

- **Curve:** closed, 4 points, all TCB (tension -0.6, continuity 0, bias 0), not linear, fitting
  coefficient 0, step count 100 (used only by the line mesh), the curve is hidden. Skip these: user
  tangents, linear segments, fitting, open curves, and the mesh. They are cheap though, so the formulas
  below cover them.
- **Object animations played by a ported BB:** only `Record Anim`. It has linear position, linear
  rotation and linear scale, 4445 keys each (one per frame, times 0..4444), length 4445, flags 0, not
  merged, no morph, no scale axis. TCB, Bezier, scale-axis and morph controllers can be skipped. `Kamera02`
  also has a linear scale-axis controller. The UFO arms have **TCB rotation** (`0x45b52a02`) and linear
  scale.
- **Position On Curve:** only the position path is used (Follow and Bank are FALSE).
- **Play Animation 3D Entity:** the Time duration and the Loop path are used; the curve is linear, so
  y = x.

Class tree for `ck_load.c` (`class_parent` has no entries for these yet): 15 ObjectAnimation, 16
Animation and 36 CurvePoint derive as 15 → 11 SceneObject, 16 → 11, 18 KeyedAnimation → 16,
36 → 33 3dEntity. 43 Curve → 33 is already listed. ObjectAnimation and Animation are **not**
BeObjects. `tools/ck.py summary` crashes on files containing class ids that are missing from its `CLASS`
table (15, 36, 37).

## 2. CKCurvePoint (class 36)

A CKCurvePoint is a full 3D entity whose parent is the curve. Its position comes from its entity world
matrix (see 3.3).

### Load (`FUN_1001701d`, vtable `0x10084488` slot `+0x20`)

First the CK3dEntity load (`FUN_1000a7b9`). Then:

| Data version | Identifier | Fields |
|---|---|---|
| >= 5 (game: 10) | `0x10000000` | curve (object), `not_tcb` (int, `+0x1ec`), `linear` (int, `+0x1f0`), tension `+0x1ac`, continuity `+0x1b0`, bias `+0x1b4` (floats), in tangent (vector, `+0x1c8`), out tangent (vector, `+0x1d4`) |
| < 5 | `0x10000000` | curve, not_tcb, linear, position (3 floats, applied as SetPosition) |
|  | `0x20000000` | tension, continuity, bias |
|  | `0x80000000` | vector at `+0x1bc` |
|  | `0x40000000` | in and out tangents |

The saved tangents are a cache: for TCB points (`not_tcb == 0`) `Update` recomputes them. In the game
they are identical to the recomputed ones; this was checked with a Python model to 3 decimals.

The CK3dEntity part of the game's curve points is identifier `0x100000`, 15 dwords: entity flags
`0x20000`, moveable flags, the 4 world-matrix rows (12 floats), then the **parent** object (= the curve)
because flag `0x20000` is set. In `FUN_1000a7b9` (current format), after the matrix: if flag `0x10000`,
skip an object ID; if `0x20000`, read the parent; if `0x100000`, read an int (priority). Our
`load_entity` does not read the parent yet. The curve does not need it, because the evaluation uses world
matrices.

### Fields and accessors (vtable from `+0x1d4`)

| Field | Offset | Accessor |
|---|---|---|
| curve | `+0x1a8` | GetCurve `FUN_10016c95` |
| tension / continuity / bias | `+0x1ac` / `+0x1b0` / `+0x1b4` | `FUN_10016cde` / `FUN_10016d13` / `FUN_10016ca9` (setters mark the curve dirty, `FUN_10016dff`) |
| length from curve start | `+0x1b8` | GetLength `FUN_10016db8`; set by `Update` |
| raw position (curve-local) | `+0x1bc` | `FUN_10016ad0` / `FUN_10016b00` |
| in / out tangent (curve-local) | `+0x1c8` / `+0x1d4` | GetTangents `FUN_10016e7c`, SetTangents `FUN_10016e2b` |
| fitted position (curve-local) | `+0x1e0` | `FUN_10016b30` / `FUN_10016b60` |
| not_tcb (user tangents) | `+0x1ec` | UseTCB(b) sets `!b` (`FUN_10016d7d`); IsTCB = `not_tcb == 0` (`FUN_10016d9c`) |
| linear (the segment leaving this point) | `+0x1f0` | IsLinear `FUN_10016d48` |

Suggested runtime struct (the name `CkCurvePoint` is already taken by the 2D curve in `ck_curve.h`):

```c
typedef struct {
    Ck3dEntity e;
    CkId curve;
    float tension, continuity, bias;
    bool user_tangents, linear;      /* not_tcb, linear */
    CkVec3 in, out;                  /* curve-local tangents */
    /* computed by the curve update */
    CkVec3 raw, pos;                 /* curve-local position, fitted position */
    float length;                    /* arc length from the first point */
} Ck3dCurvePoint;
```

## 3. CKCurve (class 43)

### Load (`FUN_10016342`, vtable `0x10084238` slot `+0x20`)

First the CK3dEntity load (`FUN_1000a7b9`). In the game the curve's chunk also has `0x4` (hidden) and
`0x400` (1 word, base class) before `0x100000`.

| Data version | Identifier | Fields |
|---|---|---|
| >= 5 (game: 10) | `0xffc00000` | control points: object array (count, then object refs, in curve order), fitting coefficient (float, `+0x1c0`), step count (dword, `+0x1bc`), **open** flag (dword, `+0x1b4`) |
|  | `0xff000000` | only when not loading from a file: per point an object ID and a sub-chunk (point state) |
| < 5 | `0x800000` points, `0x400000` fitting, `0x1000000` step count, `0x2000000` open, `0xff000000` points and sub-chunks | |

Game values: points `#499, #500, #501, #498` (Point0000..0003), fitting 0, steps 100, open 0.
At the end the load clears the up-to-date flag (object flag `0x400`, through `FUN_1000ced0(this, 0,
0x400)`), so the first `GetPos`/`GetLength` runs `Update`.

Fields: points `+0x1a8`, open `+0x1b4` (constructor `FUN_10013610` sets 1; `Open` `FUN_10014f1b` sets 1,
`Close` `FUN_10014f50` sets 0, `IsOpen` `FUN_10014f85`), total length `+0x1b8`, step count `+0x1bc`
(default 100), fitting `+0x1c0`, colour `+0x1c4` (default white), "loading" guard `+0x1c8`.

Suggested runtime struct:

```c
typedef struct {
    Ck3dEntity e;
    CkIds points;            /* Ck3dCurvePoint ids in curve order */
    bool open;
    float fitting;
    uint32_t steps;          /* line mesh only */
    float length;            /* computed */
    bool dirty;              /* set by load, cleared by update */
} CkCurve3d;
```

### CKCurve virtuals (CK3dEntity ends at `+0x1d0`)

| Slot | Function | Method |
|---|---|---|
| `+0x1d4` | `FUN_10014e8a` | GetLength (Update if dirty, return `+0x1b8`) |
| `+0x1d8` / `+0x1dc` / `+0x1e0` | `FUN_10014f1b` / `FUN_10014f50` / `FUN_10014f85` | Open / Close / IsOpen |
| `+0x1e4` | `FUN_10014195` | **GetPos(step, pos, dir)** |
| `+0x1e8` | `FUN_1001483b` | GetLocalPos |
| `+0x1ec` / `+0x1f0` | `FUN_10013b93` / `FUN_10013bdc` | GetTangents(point) / **GetTangents(index)** |
| `+0x1f4` / `+0x1f8` | `FUN_10013b4a` / `FUN_10013719` | SetTangents(point) / SetTangents(index) |
| `+0x1fc` / `+0x200` | `FUN_10014eb8` / `FUN_10014ee0` | Set/GetFittingCoeff |
| `+0x204..+0x218` | | Remove/Insert/AddControlPoint (`FUN_10013fcd`), GetControlPointCount (`FUN_10013f7c`), GetControlPoint (`FUN_10013f95`), RemoveAll |
| `+0x21c` / `+0x220` | `FUN_10013ef0` / `FUN_10013f1a` | Set/GetStepCount |
| `+0x224` / `+0x228` | `FUN_10014f99` / `FUN_1001506d` | CreateLineMesh / UpdateMesh |
| `+0x22c` / `+0x230` | | Get/SetColor |
| `+0x234` | `FUN_1001586d` | **Update** |

### 3.1 Index wrap (`FUN_1001603e`)

`wrap(i)`: for a closed curve, `i` modulo n (with `(i + n) % n` for negative i). For an open curve,
clamp to `[0, n-1]`.

### 3.2 Tangents (`GetTangents(index)` `FUN_10013bdc`)

If the point is not TCB (`not_tcb`), return its stored in and out tangents. Otherwise, with `P` = the
raw curve-local positions (`+0x1bc`), t/c/b the point's tension, continuity and bias,
`dp = P[i] - P[wrap(i-1)]` and `dn = P[wrap(i+1)] - P[i]` (at the ends of an open curve one of them is 0):

```
out = ((1-t)(1-c)(1-b) * dn + (1-t)(1+c)(1+b) * dp) / 2
in  = ((1-t)(1+c)(1-b) * dn + (1-t)(1-c)(1+b) * dp) / 2
```

This is plain Kochanek-Bartels. Unlike the 2D curve, the tangents are **not** rescaled.

### 3.3 Update (`FUN_1001586d`)

This does nothing while `+0x1c8` (loading) is set. Steps:

1. For every point: `raw = pos = point.GetPosition(ref = curve)`, i.e. the point's world position in
   the curve's frame: `inverse(curve.world)` applied to `point.world` row 3. The curve points' matrices
   are not otherwise used.
2. For every point: `GetTangents(i)`, then store the result in the point (TCB points get their tangents
   recomputed from the raw positions).
3. If `fitting > 0`: for every point, `pos = raw + ((raw[wrap(i-1)] + raw[wrap(i+1)]) / 2 - raw) *
   fitting`. The tangents from step 2 are kept, i.e. they come from the unfitted positions.
4. Arc lengths. `len = 0`. There are `n-1` segments if open, `n` if closed. For segment i (points i and
   `j = i+1`, or 0 for the closing segment): `point[i].length = len`. If `point[i].linear`, add
   `|pos[j] - pos[i]|`. Otherwise sample the Hermite segment `H(pos[i], out[i], pos[j], in[j], s)` at
   `s = k/100` for k = 1..100 and add the chord lengths from the previous sample, starting at `pos[i]`.
   Unless `i == n-1`, set `point[j].length = len`. `curve.length = len`.
5. UpdateMesh (skip), then mark the curve up to date (flag `0x400`).

Hermite (used by Update and GetPos): `H(P0,T0,P1,T1,s) = P0(2s³-3s²+1) + P1(-2s³+3s²) + T0(s³-2s²+s) +
T1(s³-s²)`.

Game curve (Python model): curve-local points ≈ (94.98,0,0.48), (-0.95,0,95.46), (-95.46,0,-0.95),
(1.43,0,-94.98). Point lengths 0, 149.06, 298.27, 447.34; total 595.27. The world path is a
radius-95 loop at y = 40 that starts at (0.477, 40, -94.955).

### 3.4 GetPos(step, pos, dir) (`FUN_10014195`)

1. If dirty, run Update.
2. Step: for a closed curve, if `step > 1` then `step -= trunc(step)`, and while `step < 0`,
   `step += 1`. For an open curve, clamp to [0, 1].
3. If there are fewer than 2 points or `pos` is NULL, return an error and leave `pos` untouched.
4. `L = step * length`. `next` = the first index k with `point[k].length > L`. If none, `next` is
   `n-1` (open) or `0` (closed). `prev = next - 1`; if that is < 0 it becomes `n-1` (closed) or 0 (open).
5. `L0 = point[prev].length`, `L1 = (next == 0) ? length : point[next].length`,
   `s = (L1 == L0) ? 0 : (L - L0) / (L1 - L0)`.
6. If `point[prev].linear`: `p = pos[prev] + (pos[next] - pos[prev]) * s`, `d = pos[next] -
   pos[prev]`. Otherwise `p = H(pos[prev], out[prev], pos[next], in[next], s)` and, if `dir` is wanted,
   `d = H(..., s + 0.01) - p` (it can extrapolate past the segment end).
7. `pos = curve.world * p` (point transform, `Vx3DMultiplyMatrixVector`). `dir = normalize(rotate(curve.world,
   d))` (`Vx3DRotateVector`).

The evaluation is in curve-local space. It is equivalent to evaluating in world space only for a
uniformly scaled curve matrix, so do it in local space. `GetLength` (`+0x1d4`) returns `length` after a
lazy Update. There is no Bezier variant for 3D curves: segments are Hermite (TCB or user tangents) or
linear.

## 4. Position On Curve (3DTransfo `676776d0:20d457bd`, execute `FUN_25006ca0`, callback `FUN_25006c60`)

Pins:

| Index | Pin | Type | Default |
|---|---|---|---|
| in 0 | In | | |
| out 0 | Out | | |
| pIn 0 | Curve | Curve object | |
| pIn 1 | Progression | Percentage (float 0..1) | 0.5 |
| pIn 2 | Follow | Bool | FALSE |
| pIn 3 | Bank | Bool | FALSE |
| pIn 4 | Bank Amount | Float | 1.0 |
| pIn 5 | Direction | Direction enum `0286652d:5ea709c2` (1 X, 2 -X, 3 Y, 4 -Y, 5 Z, 6 -Z), or a Vector | Z (5) |
| pIn 6 | Hierarchy | Bool | TRUE |
| local 0 | (unnamed) | Vector: the previous up vector, sentinel (99,0,0) | |
| local 1 | Roll (setting) | Angle | 0 |

It targets a 3D entity (behaviour flag `0x40000`). The **callback** (`FUN_25006c60`) runs on
`CKM_BEHAVIORRESET` (9) and sets local 0 to (99, 0, 0).

**Execute:**

1. Deactivate `In` and activate `Out` first. If there is no target, return `0xa004`; if there is no
   curve, return `0xa008`. In our port, return `CKBR_OK` in both cases, after the activations.
2. Read Progression `p` (0.5), Follow, Bank, Hierarchy (1), then `keepChildren = !Hierarchy`, then Roll
   (0).
3. `curve.GetPos(p, &P, NULL)`, then `target.SetPosition(&P, ref = NULL, keepChildren)` (CK3dEntity
   vtable `+0x124`, world position). If neither Follow nor Bank is set, return `CKBR_OK` (0). **This is
   the only path the game uses.**
4. Direction index `D`. If pIn 5 is a Vector (`48824eae:2fe47960`), `D = 2*i + 1` for the first nonzero
   component i: the sign is ignored. Otherwise `D` is the enum value (default 5). The axis table at
   `0x2500eff8` is built on first use: `D` → ±X/±Y/±Z unit vectors, in the enum order above.
5. `P+ = GetPos(p + 0.01)`, `P- = GetPos(p - 0.01)` (wrapped or clamped by the curve).
   `up = local 0`.
6. **Follow:** `dir = normalize(P+ - P-)`, then `up = FUN_2500aa80(target, dir, D, Roll, up, 1.0,
   keepChildren, 0)`, stored in local 0. The helper orients the entity so that its axis D points along
   `dir`. It builds the frame from the entity's orientation (`GetOrientation` `+0x130`) and the previous up
   vector, then calls `SetOrientation` (`+0x12c`) and a `Rotate` of `Roll` about the axis (`+0x108`). Its
   decompile is garbled and it has not been ported.
7. **Bank** (only the XZ plane counts): `a = (P+ - P)` and `b = (P- - P)`, both with y = 0. If either
   has zero length, return. `A = a/|a|`, `B = b/|b|`, `C = A × B`, `angle = asin(|C|)`. If
   `angle == 0`, return. `angle *= Bank Amount`, negated when `dot(up, C) <= 0`. Then
   `target.Rotate(axis[D], angle, ref = target, keepChildren)` (vtable `+0x10c`), in the entity's own
   frame. Nothing resets the orientation between frames, so with Bank on and Follow off the roll
   accumulates.
8. Return `CKBR_OK` (0). It is called again only when `In` is activated again (in the game, by Bezier
   Progression's `Out`/`Loop Out`).

## 5. Object animations

### 5.1 Objects

- **CKObjectAnimation (15)**, 0x40 bytes, vtable `0x10086160`, constructor `FUN_10056270`: keyframe data
  `+0x1c`, flags `+0x20`, entity `+0x24`, current step `+0x28`, merge factor `+0x2c`, merged animations
  `+0x30`/`+0x34`, parent keyed animation `+0x3c`.
- **Keyframe data** (0x20 bytes): controllers position `+0`, scale `+4`, rotation `+8`, scale axis `+0xc`,
  morph `+0x10`; length (frames, float) `+0x14`; owner `+0x1c`. `FUN_1004a5b2` sets the length and copies
  it to every controller. `FUN_1004a48a` deletes all controllers.
- **Controller**: type `+4`, key count `+8`, length `+0xc`, keys `+0x10`.
- Flags (tested by the Evaluate* functions): `4` ignore position, `8` ignore rotation, `0x80` merged.
  0x10/0x20/0x40 are presumably ignore scale / scale axis / morph (not verified, and all 0 in the game).

Suggested runtime structs:

```c
typedef struct { float t; CkVec3 v; } CkPosKey;              /* 16 bytes */
typedef struct { float t; float q[4]; } CkRotKey;            /* 20 bytes, q = x, y, z, w */
typedef struct {
    CkObj h;                  /* SceneObject: no BeObject part */
    uint32_t flags;
    CkId entity;
    float length, step;
    CkPosKey *pos, *scale;  uint32_t npos, nscale;
    CkRotKey *rot, *scale_axis;  uint32_t nrot, nscale_axis;
} CkObjectAnimation;
```

### 5.2 Load (`FUN_10058c51`, slot `+0x20`)

`CKObject::Load`, then `Clear` (`+0xc0`). With data version < 1 there is a legacy per-identifier format
(`0x200000` morph, `0x4000` pos, `0x8000` rot and scale axis, `0x20000` scale, `0x40000` flags,
`0x80000` entity, `0x2000` length, `0x100000` merge, `0x1000` vector). It is not present in the game.
Data version >= 1 (the game: 10):

| Identifier | Layout |
|---|---|
| `0x2000000` | shared: source animation (object), 3 floats, 4 unused floats, flags, entity, [if flags & 0x80: merge factor, anim A, anim B], then `ShareDataFrom(source)` (`+0xb0`). Not in the game. |
| **`0x4000000`** (the game) | 3 floats (a vector; stored as app data when nonzero, and taken over by a parent keyed animation as its `+0x48`), 4 unused floats, **flags**, **entity** (object), **length** (float, frames), [if flags & 0x80: merge factor, anim A, anim B], then a **controller list**: repeat { `type` (dword; 0 ends the list), `size` (dwords that follow), key block }. Each block is handed to `CreateController(type)` (`+0x50` → `FUN_1004a922`), then `ReadKeysFrom` (controller slot `+0x18`), and `size` dwords are skipped. |
| `0x1000` (only when neither of the above) | older single-block format: 3 + 4 floats, morph key count helper ints, flags, entity, length, merge, then fixed-order pos / scale / rot / scale-axis blocks (each: byte size, key count, keys). Not in the game. |
| `0x1000000`, `0x800000` | morph normals / compressed morph vertices. Not in the game. |

Key block (`ReadKeysFrom`): `count` (dword), then `count` keys packed back to back. The `size` field is
`1 + count * key_dwords`.

| Type | Controller | Slot | Key (dwords) | ReadKeysFrom / Evaluate |
|---|---|---|---|---|
| `0x637c4301` (also 1) | linear position | `+0` | 4: time, x, y, z | `FUN_1004bc0e` / `FUN_1004b8df` |
| `0x347e4a01` | TCB position | `+0` | 9: time, x, y, z, tension, continuity, bias, ease to, ease from (field order per the SDK) | `FUN_1004cf6d` / `FUN_1004ca20` |
| `0x921ab801` | Bezier position | `+0` | variable: time, x, y, z, flags; + in tangent (3) if in-mode & 0x20; + out tangent (3) if out-mode & 0x20 | `FUN_1004f65a` / `FUN_1004efc5` |
| `0x49ed4002` (also 2) | linear rotation | `+8` | 5: time, qx, qy, qz, qw | `FUN_1004c464` / `FUN_1004c150` |
| `0x45b52a02` | TCB rotation | `+8` | 10: time, quat, T, C, B, ease to, ease from | `FUN_1004da6c` / `FUN_1004d54e` |
| `0x654a3a04` (also 4) | linear scale (same code as linear position) | `+4` | 4 | as linear position |
| `0x1b545904` / `0x18ab4404` | TCB / Bezier scale | `+4` | as position | |
| `0x2f200b08` | linear scale axis (same code as linear rotation) | `+0xc` | 5 | as linear rotation |
| `0x32595908` | TCB scale axis | `+0xc` | 10 | as TCB rotation |
| `0x73847810` (also 0x10) | morph | `+0x10` | complex | `FUN_10050744` |

Game data (dword layout of `Record Anim`): `[0x4000000, 0]`, `0 0 0`, `0 0 0 0`, flags `0`, entity
`#1065`, length `4445.0`, then `0x637c4301 17781 4445 {t x y z}...`, `0x49ed4002 22226 4445 {t qx qy qz
qw}...`, `0x654a3a04 17781 4445 {t sx sy sz}...`, `0`. Key 0 is at (-19.664, 8.446, -39.114), which is
exactly `I_Ball_Stone`'s saved position. `PE_UFO_Arm_A_03`: flags 0, length 100, `0x45b52a02 61 6`
(keys at 0, 35, 59, 70, 75, 100, with T/C/B/ease all 0), `0x654a3a04 5 1`.

### 5.3 Linear evaluation (`FUN_1004b8df`, `FUN_1004c150`)

With no keys the controller returns FALSE (the component counts as absent). If `t <= key[0].t`, return
key 0. If `t >= key[n-1].t`, return the last key. Otherwise binary search: `lo = 0, hi = n-1; while lo <
hi-1 { mid = (lo+hi)>>1; if t <= key[mid].t then hi = mid else lo = mid }`. The segment is
`(hi-1, hi)` and `f = (t - t0)/(t1 - t0)`.

- Vectors: `v0 + (v1 - v0) * f`.
- Quaternions: `Slerp(f, q0, q1)` (VxMath `0x2429c8f0`). `d = q0·q1`. If `d < 0`, use `-d` and
  negate the q1 weight. If `1 - |d| > 0.01`: `θ = acos(|d|)`, `k0 = sin((1-f)θ)/sinθ`,
  `k1 = ±sin(fθ)/sinθ`. Otherwise `k0 = 1-f`, `k1 = ±f`. Result `k0*q0 + k1*q1`, not normalized.

TCB rotation (needed only for the UFO / Play Global Animation) is at `FUN_1004d54e`. It is not analysed
here.

### 5.4 Evaluate* and SetStep

`EvaluatePosition(frame, &v)` (`+0x6c`, `FUN_10056d02`): if flags & 4, v = 0 and return FALSE. If not
merged, return the position controller's result (FALSE without a controller). If merged: with factor 0
use anim A, with factor 1 anim B, otherwise blend A and B by the factor at the same relative time. Rotation
`+0x74` (`FUN_10056f38`, flag 8), scale `+0x70` (`FUN_100571a0`) and scale axis `+0x78`
(`FUN_10057333`) follow the same pattern.

**SetStep(step, keyed = NULL)** (slot `+0xe4`, `FUN_100578ca`):

1. `this.step = step`; `frame = step * length`. Return an error if there is no entity.
2. If `keyed` is not the "-1" sentinel and the entity has flag `0x400` (entity flags `+0x74`), return
   (the entity is driven elsewhere). If the entity is a body part (class 42) belonging to a character other
   than `keyed`, return. Neither case applies to the game's entities.
3. Evaluate the position (bit 4), rotation (bit 8), scale (bit 1) and scale axis (bit 2) at `frame`.
4. If the animation belongs to a keyed animation whose root animation (`+0x44`) is this one:
   `pos += keyed.+0x48`.
5. If nothing was evaluated, skip to the morph step. If only the position was evaluated, write
   `local[3] = pos`. Otherwise, if some of pos/rot/scale are missing, take them from the current local
   matrix (`GetLocalMatrix` `+0x16c`, decomposed by `FUN_1005695c`: pos = row 3, rot =
   `VxQuaternion::FromMatrix`, scale = row lengths).
   - If there is no scale axis, or its `w` is within 1.19e-7 of 1: `local = compose(pos, scale, rot)`
     (`FUN_1000c230`): `R = QuatToMatrix(rot)`, row i of R scaled by scale[i], row 3 = pos.
   - Otherwise `FUN_1000c2e0`: `R(rot) * (U · diag(scale) · Uᵀ)` with `U = R(scale axis)`, then row 3 =
     pos. Only `Kamera02` needs this, and it is unused.
6. Unless `keyed` is set and reports that it handles the update (`keyed+0x6c`), call
   `LocalMatrixChanged` (`FUN_100064a4`). This sets `world = local` without a parent, or
   `world = parent.world ∘ local` with one (`Vx3DMultiplyMatrix(world, parent.world, local)`), and then
   recomputes the children's world matrices from their local matrices. The animation therefore drives the
   **local** matrix.
7. Morph (`HasMorphInfo` `+0x88`) updates mesh vertices. It is not used.

`QuatToMatrix` (VxMath `VxQuaternion::ToMatrix` `0x242836e0`, x87 path `0x242837d3`): with `s = 2/(x²+y²+z²+w²)`:

```
row0 = (1 - s(yy+zz),  s(xy - wz),     s(xz + wy))
row1 = (s(xy + wz),    1 - s(xx+zz),   s(yz - wx))
row2 = (s(xz - wy),    s(yz + wx),     1 - s(xx+yy))
row3 = (0, 0, 0, 1)
```

Rows are the entity axes, the same convention as `Ck3dEntity.world`. This is the transpose of D3DX's
`MatrixRotationQuaternion`. The UFO arm key 0 reproduces the arm's saved world matrix (parent at identity),
but that rotation is symmetric, so the check does not settle transposition; the disassembly does.

For `Record Anim` in our runtime (no parent, all three components present), SetStep reduces to:
`e->world = compose(lerp pos, slerp rot, lerp scale)` at `frame = step * 4445`.

### 5.5 Play Animation 3D Entity (3DTransfo `64221225:769a143f`, execute `FUN_250073c0`, helper `FUN_250075c0`)

| Index | Pin | Type | Default |
|---|---|---|---|
| in 0 / in 1 | On / Off | | |
| out 0 | One Loop Played | | |
| pIn 0 | Animation | ObjectAnimation `43476550:11e52c0e` | |
| pIn 1 | Duration | Time `54b4422b:730f0f4f` (float ms), or an Integer `5a5716fd:44e276d7` source (ms) | 5000 |
| pIn 2 | Progression Curve | 2D curve `20ad345d:1afb25b1` | |
| pIn 3 | Loop | Bool | TRUE |
| pOut 0 | Progression | Percentage | |
| local 0 | progression (float) | | 0 |

Execute (the behaviour's owner is fetched but not used; the animation's own entity moves):

1. No animation: return `0xa008` (in the port, `CKBR_OK`).
2. `duration`: if pIn 1's type is Integer, read the int value of its real source (following shared
   inputs, `FUN_25004340`) and convert it to float. Otherwise read it as float (default 5000). If
   `duration < 0.001`, return `CKBR_OK`.
3. Read the curve, Loop (1), and local 0 `p`. There is also a read of a nonexistent pIn 4 (result
   unused).
4. If `Off` is active: deactivate it and return `CKBR_OK`. Nothing is written and `On` is not
   touched.
5. `p += dt / duration` (dt = `ctx->delta_ms`).
6. If `On` is active: deactivate it, set `p = 0`, apply, and return `CKBR_ACTIVATENEXTFRAME`.
7. Else if `p > 1`: activate `One Loop Played`. With Loop, `p -= trunc(p)`, apply, and return
   `CKBR_ACTIVATENEXTFRAME`. Without Loop, `p = 1`, apply, and return `CKBR_OK` (the behaviour stops).
8. Else apply and return `CKBR_ACTIVATENEXTFRAME`.

Apply (`FUN_250075c0`): write `p` to local 0 and pOut 0 (the unshaped progression), `y =
curve.GetY(p)` (`ck_curve_get_y`), then `anim.SetStep(y, NULL)` (`+0xe4`). The original dereferences the
curve without checking it. The game always has one (linear, y = p); in the port, use y = p when it is
missing.

Game run: `Ball_Stone Script.Start → On`. The stone replays its 4445-frame recording over 59.246 s and
loops (about 75 frames/s). `One Loop Played` is not linked.

### 5.6 Keyed animation (for reference: Play Global Animation)

CKAnimation load `FUN_10047adc`: `0x40` length, `0x10` either 12 bytes (int, int → SetFrameRate
`+0x70`, float frame rate `+0x30`, flags via `+0x8c`) or 8 bytes (flags `+0x2c`, frame rate `+0x30`),
`0x80` (an object array, then the root entity `+0x28`), `0x100` character `+0x1c`, `0x200` float
`+0x24`. CKKeyedAnimation load `FUN_10049e10`: `0x1000` object animations (`+0x34`), `0x100000`
(root animation `+0x3c`, merge factor `+0x40`). After loading it links each object animation to itself
(`FUN_1004a2d0`) and takes a child's app-data vector into `+0x48`. `UFO_Animation`: flags 5, 30 fps,
length 100, root entity `PE_UFO_Body`, 8 arm animations.

## 6. Port checklist

1. Loader: classes 15/16/18/36 in `class_parent`/sizes. Load CurvePoint `0x10000000` and Curve
   `0xffc00000`, and ObjectAnimation `0x4000000` with linear pos/rot/scale (store scale axis and TCB keys
   if convenient, and skip unknown types by `size`). Reading the 3D-entity parent (flag `0x20000`) is
   needed only for hierarchy-aware transforms.
2. `ck_curve3d_get_pos(ctx, curve, step, pos, dir)`: lazy Update (3.3) on the first call (after the
   whole file has loaded, because point matrices are needed), then 3.4.
3. `ck_objanim_set_step(ctx, anim, step)`: 5.3 and 5.4 without merge, morph or keyed handling; write the
   entity world matrix (these entities have no parent).
4. BBs in a new `src/bb/bb_3dtransfo.c`: Position On Curve (step 3 only, plus the reset callback) and
   Play Animation 3D Entity (5.5).
