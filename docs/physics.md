# Physics (physics_RT.dll: Terratools' Virtools glue over the Ipion "IVP" SDK)

This document collects what we need to decide and plan the physics port. It covers the Building Blocks Ballance
uses and the values the game gives them, the physics manager that couples Virtools to IVP, a map of the
IVP engine inside the DLL with sizes, and the constants that shape the feel. It is not a description of all of IVP.

- **Addresses.** They are function entry points in `data/BuildingBlocks/physics_RT.dll`, image base
  `0x10000000`, Ghidra program `physics_RT.dll` in project `ballance`.
- **Offsets.** Offsets such as `mgr+0xd0` are fields of the original C++ objects. They are given only for
  cross-checking.
- **Missing functions.** `re/physics_RT.dll.c` lacks about 180 functions that `objdump` finds. Most are small or
  vtable methods, for example the manager's PostProcess `0x10007ce0`, OnCKReset `0x10007b00` and several listener
  methods. Use `objdump -d --x86-asm-syntax=intel` for those.
- **No symbols.** The DLL has no RTTI and no source or assert strings. The only IVP names in it are:
  - `IVP_Handle`, `IVP_SurfaceBuilder_Ledge_Soup`, `IVP_SurfaceBuilder_Pointsoup`;
  - the constraint error messages;
  - the German OV-tree errors ("Mehr als 27 Kinder");
  - the profiler line `TOT ... COLL DYN UNIV CONTR INTEGR HULL SHORT CRITIC`;
  - the qhull build tag `ipion 99/07/10`.

  Subsystem names below are therefore inferred from call graphs, address clustering and constants.

## 1. The shape of the problem in one paragraph

Every physics call goes through the manager `6bed328b:141f5148`.

- **Bodies.** Physicalize creates an IVP body from one of two sources:
  - a ball of radius 2 (the player ball and loose balls);
  - a compact surface built from meshes and cached by name. Convex meshes become qhull hulls. Concave meshes
    become one triangle "ledge" per face.
- **Frame step.** Once per frame, `PostProcess` advances the IVP environment by the smoothed frame time ×
  0.001 × *Physic Time Factor*. The game sets the factor to **2**, so the simulation runs at twice real time.
  IVP steps at a fixed **1/66 s** ("PSI").
- **Write-back.** The poses of awake objects are written back to their entities (interpolated between PSIs).
  Entities are never read back after creation.
- **Game settings.** The game sets gravity to **(0, -20, 0)**. One Virtools unit is one IVP metre. Friction and
  elasticity combine by multiplication.
- **Player control.** The ball is steered by `SetPhysicsForce` controllers. They add a fixed impulse every PSI
  along world axes.

## 2. Building Blocks the game uses

There are 13 BBs and 194 instances. Counts come from `re/bb_map.txt` and were confirmed by a probe over
`base.cmo`, `3D Entities/*.nmo`, `Level/*.NMO` and `PH/*.nmo`.

| BB | GUID | Uses | Execute | Callback |
|---|---|---:|---|---|
| Physicalize | `7522370e:37ec15ec` | 64 | `FUN_100026b0` | `FUN_10002ad0` |
| SetPhysicsForce | `56e20c57:0b926068` | 37 | `FUN_10004800` | `FUN_10005ee0` |
| Set Physics Hinge | `41cd3653:0de60c1d` | 34 | `FUN_10005850` | `FUN_10005ee0` |
| PhysicsCollDetection | `7435488d:201d1188` | 17 | `FUN_10004080` | `FUN_10005ee0` |
| Physics WakeUp | `38b851b5:72ca74ac` | 14 | `FUN_10004d50` | none |
| Set Physics Globals | `72af347c:03da71e1` | 12 | `FUN_100055a0` | none |
| Set Physics Slider | `2973360e:23d31aa7` | 4 | `FUN_10005d90` | `FUN_10005ee0` |
| Physics Impulse | `0c7e39bb:16db20d5` | 3 | `FUN_10002f90` | `FUN_10003370` |
| Set Physics Spring | `24a06a3a:07100fce` | 3 | `FUN_10006360` | `FUN_10005ee0` |
| PhysicsContinuousContact | `199e4cf1:545a78fe` | 2 | `FUN_100011e0` | `FUN_10001380` |
| Set Physics Ball Joint | `5e624f0a:35160450` | 2 | `FUN_10005120` | `FUN_10005250` |
| Get Profiler Values | `1c8e61d1:32723c6f` | 1 | `FUN_10001fd0` | none |
| DeleteCollisionSurfaces | `53bf75aa:770c7021` | 1 | `FUN_10001d30` | none |

Parameter types used by these BBs:

| Type | GUID |
|---|---|
| float | `47884c3f:432c2c20` |
| bool | `1ad52a8e:5e741920` |
| int | `5a5716fd:44e276d7` |
| vector | `48824eae:2fe47960` |
| string | `6bd010e2:115617ea` |
| 3D entity | `5b8a05d5:31ea28d4` |
| mesh | `24535345:65d15229` |
| `IVP_Handle` (local) | `50766059:159d4bde` |

### 2.0 Common mechanism: deferred commands

The BBs for constraints, forces, listeners, wake-up and surface deletion do not act directly.

1. The BB allocates a 16-byte command `{vtable, mgr, 2, behavior}` and passes it to the installer queue
   `FUN_10008160(mgr+0x4c)`.
2. The queue runs the command's `Execute` (slot 0) immediately.
3. If `Execute` returns 0 ("not ready", typically because a body is not physicalized yet), the command stays in
   the queue. The manager retries it every frame in PostProcess (`FUN_100081d0`). A non-zero return deletes the
   command.

Because of this, a script can create a hinge before or after physicalizing both bodies.

The IVP object a BB creates is stored in the BB's local 0 (`IVP_Handle`):

- The second input (Destroy/Stop) deletes the object through the handle.
- Create is a no-op while the handle is set.
- The shared callback `FUN_10005ee0` clears the handle on `CKM_BEHAVIORRESET` without deleting the object.

`FUN_10007800(mgr, entity, warn)` is the registry lookup (section 3.3). With `warn` set it prints
`"You must Physicalize %s !!!"`.

### 2.1 Physicalize

Inputs are Physicalize and Unphysicalize, outputs Out1 and Out2. Behavior flags are `0x4040000`.

| # | Pin | Type | Prototype default | Execute fallback |
|---|---|---|---|---|
| 0 | Fixed ? | bool | FALSE | |
| 1 | Friction | float | 0.7 | 0.4 |
| 2 | Elasticity | float | 0.4 | 0.5 |
| 3 | Mass | float | 1.0 | 1.0 |
| 4 | Collision Group | string (8 chars significant) | "" | |
| 5 | Start Frozen | bool | FALSE | |
| 6 | Enable Collision | bool | TRUE | |
| 7 | Automatic Calculate Mass Center | bool | TRUE | |
| 8 | Linear Speed Dampening | float | 0.1 | |
| 9 | Rot Speed Dampening | float | 0.1 | |
| 10 | Collision Surface | string | "" | |
| 11.. | `convex 1..N` (mesh), then `ball position i` (vector) / `ball radius i` (float), then `concave 1..M` (mesh) | | | |

Settings (locals 0..3): Convex Count (int, 1), Ball Count (int, 0), Concave Count (int, 0), Shift Mass Center
(vector, 0).

**Callback.**

- On attach: if the owner is a 3D object, it sets `convex 1` to the owner's current mesh and *Collision Surface*
  to that mesh's name.
- On settings edit: it rebuilds the dynamic pins.

**Unphysicalize input.**

1. Increment `mgr+0xe4`.
2. If the target is registered, destroy its IVP object (`FUN_10009990`) and remove it from the registry
   (`FUN_100086f0`).
3. Fire Out2.

**Physicalize input.**

1. Increment `mgr+0xe0`.
2. If the target is already registered, fire Out1 only. Parameters are **not** updated.
3. Otherwise read the pins:
   - All convex and concave meshes are collected.
   - For balls, only the radius pins are read, into one variable, so the **last radius wins**. **Ball positions
     are ignored.**
   - The shift is passed only when Auto Mass Center is FALSE.
4. Create a new material `FUN_1000bf00(friction, elasticity)` of 0x30 bytes, holding doubles at `+0x10` and
   `+0x20`. Append it to `mgr+0x3c`. Materials are never shared.
5. Call the creation core `FUN_10002380`, with arguments in this order (read from the disassembly; the decompiler
   has them wrong):

   `(mgr, ent, nConvex, convex[], nBall, nConcave, concave[], radius, surfName, shift|NULL, fixed, material, mass, group, startFrozen, enableColl, autoMass, linDamp, rotDamp)`

**Inside `FUN_10002380`.**

1. Read the entity scale (`GetScale`, vtable `+0x144`). The scale is baked into the geometry.
2. **Ball** (`FUN_100075d0`, then IVP create-ball `FUN_10013a10`): chosen if `nBall != 0` or the surface name is
   empty.
   - Fork B found that in the empty-name case collision is not enabled. Every ball the game creates has
     Ball Count = 1.
3. **Polygon body** (`FUN_100076f0`, then create-polygon `FUN_100139a0`) in all other cases:
   1. Look up the compact surface **by the Collision Surface name** in the cache `mgr+0xbc`
      (`FUN_10006f90`/`FUN_1000b820`).
   2. On a miss, build a ledge soup (`FUN_10038290`):
      - Each convex mesh (`FUN_10007260`): vertices with exact duplicates removed, × scale, qhull hull
        (`FUN_1003ae20`), inserted as one convex ledge.
      - Each concave mesh (`FUN_10006fb0`): **every face becomes its own 3-point ledge**.
   3. Compile the soup (`FUN_100384b0`) and cache it under the name.
   4. With no usable mesh, print `"Error: incorrect mesh for %s"`.
   5. **Bodies with the same surface name share geometry, including the scale of the first one built.**
4. Build the template (`FUN_10007460`):

   | Field | Value |
   |---|---|
   | name | entity name |
   | `+0xc` | unmovable = Fixed |
   | `+0x14` | material |
   | `+0x18` | mass |
   | `+4` | nocoll group, `strncpy` 8 chars |
   | `+0x38` | linear damping |
   | `+0x40/48/50` | rotational damping on each axis |
   | `+0x64` | mass-centre override, only when not automatic |

5. Set the pose from the entity's world matrix (`+0x174`): the rotation goes through
   `VxQuaternion::FromMatrix`, the translation is used as-is.
6. After creation:
   - If not Start Frozen, wake the body (`FUN_1000a460`).
   - If Enable Collision, insert it into the collision system (`FUN_10009350`: object flag `0x100`, broadphase
     `FUN_1002dc00`).
   - Set `obj+0xb0` = entity and register the object (`FUN_10008530`).

**Core setup (`FUN_10009f00`).**

- A mass below 1e-8 becomes 1.0.
- Ball inertia is 0.4·m·r².
- Polygon inertia is the surface's unit inertia × mass.

**Values used by the game.** Unphysicalize-only instances (19) leave everything at its default. Pins 7 onward
list Auto Mass Center, Linear Damp and Rot Damp.

- **Player ball** (Gameplay `BallManager/New Ball/physicalize new Ball` and `Trafo Manager/physicalize new Ball`)
  - Ball Count 1, Convex 0, radius **2**.
  - The other pins are fed from array `Physicalize_GameBall` in `Balls.nmo`:

  | Ball | Friction | Elasticity | Mass | Group | Lin damp | Rot damp | *Force* column |
  |---|---|---|---|---|---|---|---|
  | Ball_Paper | 0.5 | 0.4 | 0.2 | Ball | 1.5 | 0.1 | 0.065 |
  | Ball_Stone | 0.5 | 0.1 | 10 | Ball | 0.3 | 0.1 | 0.92 |
  | Ball_Wood | 0.8 | 0.2 | 1.9 | Ball | 0.9 | 0.1 | 0.43 |

- **Level floors** (Levelinit `init Level/Phys. Floors`, array `Physicalize_Floors`)
  - Fixed, 1 concave, 0 convex.
  - `Phys_Floors` and `Phys_FloorRails`: 0.7 / 0.3 / mass 1 / group **Floor**.
  - `Phys_FloorStopper`: same values, group **Ball**. Stoppers therefore do not collide with the player ball
    (section 3.4).
- **Sector objects** (`Activate Sector/activate Type 2` convex, `Type 3` ball). Values come from arrays via
  Get Key Row. In the table, "Fixed" is yes only where noted and Auto Mass Center is always 0:

  | Object | Fixed | Friction / Elasticity | Mass | Lin / Rot damp | Radius |
  |---|---|---|---|---|---|
  | P_Box | | 0.7 / 0.3 | 1 | 0.1 / 0.1 | |
  | P_Ball_Paper | | 0.5 / 0.4 | 0.2 | 1.5 / 0.1 | |
  | P_Dome | **yes** | 0.2 / 0.8 | 0.2 | 0.1 / 0.1 | |
  | P_Ball_Wood | | 0.6 / 0.2 | 2 | 0.6 / 0.1 | 2 |
  | P_Ball_Stone | | 0.7 / 0.1 | 10 | 0.2 / 0.1 | 2 |

- **FixCube** (Levelinit `init Placeholders`): fixed, Enable Collision 0, surface `FixCube_Mesh`. It is the
  static anchor for single-body joints.
- **Ball pieces** (explosions in `Balls.nmo`): convex, group Ball, friction 2, elasticity 1, mass 0.2 (stone
  0.8), damping 0.3 / 0.2. Paper pieces instead use friction Random(1..5), mass Random(0.02..0.09) and damping
  6 / 0.5.
- **PH modules**. Blank Shift means none.

  | Object | Fixed | Fric | Elast | Mass | Group | Frozen | Coll | LinD | RotD | #convex | Shift |
  |---|---|---|---|---|---|---|---|---|---|---|---|
  | P_Modul_01 Filler | 1 | .7 | .4 | 1 | Floor | 0 | 1 | .1 | .1 | 1 | |
  | P_Modul_01 Rinne | 1 | .7 | .4 | 1 | Ball | 0 | 1 | .1 | .1 | 3 | |
  | P_Modul_01 Pusher | 0 | .6 | .4 | 3 | Floor | 1 | 1 | .1 | 1 | 3 | |
  | P_Modul_03 Wall01-07 | 0 | .4 | .01 | 2 | "" | 1 | 1 | .5 | 1.5 | 1 (shared surface `P_Modul_03_Wall_Coll_Mesh`) | |
  | P_Modul_03 Gate | 0 | .4 | 0 | 2 | "" | 1 | 1 | .5 | 1 | 3 | |
  | P_Modul_03 Floor | 0 | .7 | 0 | 3 | "" | 1 | 1 | 1 | 3 | 1 | |
  | P_Modul_08 Schaukel | 0 | .7 | .4 | 10 | Floor | 1 | 1 | .4 | .1 | 6 | |
  | P_Modul_17 Dreharme | 0 | .7 | .4 | 3 | Floor | 0 | 1 | 3 | .005 | 3 | |
  | P_Modul_19 Flaps | 0 | .7 | .4 | 3 | Floor | 1 | 1 | 1 | .05 | 4 | (1,1,0) |
  | P_Modul_25 Bridge | 0 | .7 | 1 | 3 | "" | 1 | 1 | 1 | .05 | 2 | (-2.5,.2,0) |
  | P_Modul_26 Rope | 0 | .7 | .4 | 1 | "" | 0 | **0** | .1 | .1 | 1 | |
  | P_Modul_26 Sack | 0 | .7 | .4 | 10 | Floor | 0 | 1 | .1 | .1 | 1 | |
  | P_Modul_29 Platte01-09 | 0 | .7 | .4 | .5 (09: 1) | Modul29 | 1 | 1 | .1 | .3 | 1 | |
  | P_Modul_30 Wippe | 0 | .7 | .4 | 3 | Floor | 1 | 1 | 1 | 1 | 2 | (0,4,0) |
  | P_Modul_34 Kiste | 0 | .8 | .4 | 1.4 | "" | 1 | 1 | .1 | .1 | 1 | |
  | P_Modul_34 Schiebestein | 0 | .5 | .4 | 1.6 | "" | 1 | 1 | .1 | .1 | 1 | |
  | P_Modul_37 Bridge | 0 | .7 | 1 | 3 | "" | 1 | 1 | 1 | .05 | 3 | (-7.5,0,0) |
  | P_Modul_41 | 0 | .7 | .4 | 1 | Floor | 0 | 1 | .1 | .1 | 2 | |
  | PE_Balloon Platform | 0 | .7 | .4 | 4 | Floor | 1 | 1 | .1 | .1 | 6 | (0,2,0) |
  | PE_Balloon Platte08 | 0 | .7 | .4 | .5 | Floor | 1 | 1 | .1 | .1 | 1 | |
  | PE_Balloon Ballon04 | 0 | .7 | .4 | .2 | "" | 1 | **0** | 1 | 1 | 1 | |
  | PE_Box_slide | 0 | .7 | .4 | 4 | "" | 1 | **0** | 1 | 1 | 1 | |

Features the content never uses:

- Auto Mass Center = TRUE on a creating instance. The mass centre is always the object origin plus Shift.
- Ball Count > 1.
- Balls mixed with meshes.
- Concave meshes on dynamic bodies.

### 2.2 SetPhysicsForce

Inputs are Create and Destroy. The command's `Execute` is `FUN_10004930` (vtable `0x10063278`).

| Pin | Type | Default |
|---|---|---|
| Position | vector | 0,0,0 |
| Pos Referential | 3D entity | |
| Direction | vector | 0,0,1 |
| Direction Ref | 3D entity | |
| Force Value | float | 10 |

**Execute.** It needs the target physicalized, otherwise the command stays queued.

- **Direction.** Transformed to world by Direction Ref (`TransformVector`) and normalized. A squared length
  ≤ 1e-4 gives (1,0,0). The result is multiplied by Force Value.
- **Position.** If Pos Referential is the target, it is kept object-local. Otherwise it goes to world, then into
  core space.
- **Controller.** A 0x40-byte IVP controller is created (vtable `0x10063240`, priority `0x5dc`) and attached to
  the core (`FUN_10011b10`).

**Every PSI** the controller rotates the stored world force into object space and calls the core's async push
`FUN_1000c830`:

- Δv += F / m
- Δω += I⁻¹ (r × F)

**There is no multiplication by the time step. Force Value is an impulse per PSI.** The equivalent continuous
force is F × 66 per simulated second.

Uses:

- **Ball Navigation** (Gameplay): four instances with world directions (1,0,0), (-1,0,0), (0,0,-1) and (0,0,1),
  no referentials. Force comes from a graph input, presumably the *Force* column in 2.1. A fifth instance pushes
  along (0,1,0) with a force computed by an Op.
- **Paper wind**: 18 pieces, (-1,0,1), force 0.03.
- **PH modules**:
  - P_Modul_08: ±Z relative to `P_Modul_08_Fix`, 1.1.
  - P_Modul_18: (0,1,0), 0.1.
  - P_Modul_26 Sack: ±Z relative to Halter, 0.25.
  - PE_Box_slide: (-1,0,1) in its own frame, 0.2.
  - PE_Balloon: (0,1,0) at hinge_b, 0.3; ropes (-0.2,0,0), 0.3.

### 2.3 Constraints: Hinge, Slider, Ball Joint

All three are built from a constraint template:

1. `FUN_10012570` creates the template.
2. `FUN_10012870(target, anchor, axis, nTransFixed, nRotFixed, obj2, 0)` sets what is locked.
3. Optional limits: `FUN_10012990` for angular, `FUN_10012960` for linear.
4. `FUN_100129c0` creates the constraint on the environment.

**Object2.** If Object2 is NULL, the Hinge `Execute` returns "done" and creates nothing. In practice the scripts
write `FixCube` into Object2 with an Op before the BB runs. This was verified for P_Modul_08 and is presumed for
the other single-body joints.

- **Hinge** (`Execute` `FUN_100059d0`)
  - Pins: Object2, Joint Referential, Limitations (bool, FALSE), Lower Limit (-45), Upper Limit (45).
  - Anchor = the referential's world position. Axis = its world Z (Dir).
  - Locks 3 translations and 2 rotations.
  - The game **never enables limits**.
  - Uses:
    - 13 single-body hinges to FixCube: P_Modul_08, 17, 19, 25, 29 Platte01/09, 30, 37, 41, PE_Balloon_Platte08.
    - The P_Modul_29 plate chain.
    - The PE_Balloon bridge chain (Hinge01..09) and the balloon, rope and platform.
- **Slider** (`Execute` `FUN_10005f10`)
  - Pins: Object2, Axis first Point, Axis second Point, Limitations (meter), Lower Limit (-1), Upper Limit (1).
  - The axis runs from the first point to the second.
  - Locks all rotations and the 2 perpendicular translations.
  - Uses:
    - P_Modul_03 Floor and P_Modul_34 Schiebestein: no limits.
    - PE_Box_slide: limits **on, -30000..0**.
    - PE_Balloon_Platform on PE_Box_slide: no limits.
- **Ball Joint** (`Execute` `FUN_100052f0`)
  - Pins: Object2, Position 1, Referential 1. The setting "Specify 2 Points" is unused.
  - Locks the 3 translations.
  - Uses: P_Modul_26, rope to FixCube at `Balljoint_oben` and rope to sack at `Balljoint_unten`.

### 2.4 Set Physics Spring

The command's `Execute` is `FUN_10006490`.

| Pin | Type | Default |
|---|---|---|
| Object2 | 3D entity | |
| Position 1 | vector | |
| Referential 1 | 3D entity | |
| Position 2 | vector | |
| Referential 2 | 3D entity | |
| Length | float | 1 |
| Constant | float | 1 |
| Linear Dampening | float | 0.1 |
| Global Dampening | float | 0.1 |

- Both anchors are converted to world coordinates and attached to their bodies (`FUN_10013e80`).
- A spring template is built (`FUN_10014200`) and the spring actuator is created (`FUN_10013700`).
- The mapping of the four floats onto IVP template fields is **not yet verified**.

Uses (Length / Constant / Lin / Global):

| Object | Anchors | Values |
|---|---|---|
| P_Modul_03 Floor | frame_low to frame_high | 0 / 15 / 0.1 / 1 |
| P_Modul_17 Dreharme | (0,4,0)@HingeFrame to (0,0,-4)@self | 0 / 0.32 / 0.1 / 0.1 |
| PE_Balloon | platform to box_slide | 15 / 20 / 0.6 / 1 |

### 2.5 Physics Impulse

`FUN_10002f90` acts directly, without the command queue.

| Pin | Type | Default |
|---|---|---|
| Position | vector | |
| Referential | 3D entity | |
| Direction | vector | 0,0,1 |
| Direction Ref | 3D entity | |
| Impulse | float | 10 |

| Setting | Type | Effect |
|---|---|---|
| 2 pos instead of dir ? | bool | direction = point − position |
| Constant Force ? | bool | impulse × `mgr+0xcc`, the simulation dt |

Steps:

1. Skip bodies with core flags `0xc` (fixed).
2. Wake the body.
3. Transform the direction, normalize it and multiply by Impulse.
4. Apply it:
   - If Referential is the target, apply it locally (`FUN_1000c830`).
   - Otherwise apply it at the world position (`FUN_1000a3e0`).

Uses: only ball explosions. Referential is each piece, both settings are 0.

| Ball | Position | Impulse |
|---|---|---|
| Wood | (0,1,0) | Random 1.5..3 |
| Paper | (-0.03,1,0.02) | Random 0.5..1.3 |
| Stone | (-0.05,1,0.05) | Random 4..9 |

### 2.6 PhysicsCollDetection

| | Name | Type | Default |
|---|---|---|---|
| Input | Create | | |
| Input | Stop | | |
| Output | Collision | | |
| Pin | Min Speed m/s | float | 0.3 |
| Pin | Max Speed m/s | float | 10 |
| Pin | Sleep afterwards | float | 0.5 |
| Pin | Collision ID | int | 1 |
| Setting | Use Collision ID | bool | FALSE |
| Out pin | Entity | 3D entity | |
| Out pin | Speed (0-1) | float | |
| Out pin | Collision Normal World | vector | |
| Out pin | Position World | vector | |

Create adds a 0x30-byte listener `{lastTime, min, max, sleep, obj, mgr, beh, id}` to the object (`FUN_10009570`).
Stop removes it (`FUN_10009590`). The BB stays active while it has pending work.

On IVP's post-collision event (`FUN_100042b0`):

1. The other body must be physicalized.
2. Its ID is the value of the attribute **"Coll Detection ID"**, or -1 if it has none.
   - The attribute type is resolved once, at world creation (`FUN_10008240`), by scanning 3D objects. If no
     object has it at that moment, every ID reads -1.
3. If Use ID is set and the IDs differ, ignore the event.
4. Ignore the event if `now - lastTime < Sleep`, or if the output is still active.
5. Ignore the event if `|impact speed| ≤ Min`.
6. Set the outputs:
   - Speed = `min(speed/Max, 1)`. It is 1 if Max < 1e-4, and a tiny constant if speed ≤ 1e-4 (fork analyses
     read it as 2.5e-5 and 1e-4; recheck).
   - The normal is negated when the target is the second object of the contact.
   - **Position World has z = 0.** y is written twice, which is an original bug. Keep it: sound scripts may not
     care.
7. Set lastTime and fire Collision.

Uses (Min / Max / Sleep / ID):

| Script | Use ID | Values |
|---|---|---|
| Ball piece sounds, 12 instances | 0 | 2 / 25 / 0.5 / 1 |
| Sound.nmo hit sounds | 1 | (2,14,2,3), (1,15,1,4), (2,14,1,2), (2,30,1,1) |
| HitSound Woodenflaps | 0 | 0.3 / 10 / 0.5 / 1 |

### 2.7 PhysicsContinuousContact

`Execute` of the command is `FUN_10001470`; the listener vtable is `0x100631b8`.

- Pins: Time Delay Start (0.1), Time Delay End (0.1).
- Setting: Number Group Output (5). It creates output pairs `contact on %d` / `contact off %d`.
- **Only partly traced.** Contacts are grouped by the other body's attribute **"Continuous Contact ID"**:
  - "on N" fires once contact has lasted Time Delay Start;
  - "off N" fires after Time Delay End without contact.
- The manager drives the timers every frame (`FUN_100017a0`). It keeps a contact count and a first-contact time
  in each registry record, maintained by the global collision listener (vtable `0x10063360`, friction-contact
  created/deleted).
- There is a 0.5 constant at `0x100631d0` in the timing.
- Uses: `Sound.nmo` "Roll Paper" and "Roll Wood/Stone", both 0.3 / 0.3 with 3 groups. **These drive the ball
  rolling sounds** (floor type by ID).

### 2.8 The rest

- **Physics WakeUp** (`Execute` `FUN_10004de0`, no pins) calls `FUN_1000a460`:
  - if the object state `+0x80` is not 8, it re-activates the core (`FUN_1000cec0`);
  - otherwise it calls `FUN_1000b540`.

  It is used after every new ball and by Ball Navigation.
- **Set Physics Globals**
  - Pins: Gravity (vector, 0,-9.81,0), Physic Time Factor (float, 1).
  - *Set Values*: `mgr+0xd0 = factor × 0.001` and `env->set_gravity` (`FUN_10013680`).
  - *Clean Physics World*: destroys and recreates the environment (`FUN_10006b70`, `FUN_10006bb0`). This resets
    gravity to -9.81. The object registry is **not** cleared, so check for dangling entries.
  - **The game always uses gravity (0,-20,0)**. The time factor is **2** while playing (reset, unpause, Init
    Ingame, tutorial) and **0** to pause.
- **Get Profiler Values** returns counters and QPC times; it is used once, for diagnostics.
- **DeleteCollisionSurfaces**:
  - If any object is still registered, it prints "Please dephysicalize all objects" and does nothing.
  - Otherwise it frees the surface-name cache and makes a new one with 64 buckets.

  It is used once, from base.cmo's Event_handler.

## 3. The physics manager (`6bed328b:141f5148`, "Physics Manager")

The factory `FUN_10009070` allocates 0x2cf0 bytes. The constructor is `FUN_10006730` and the vtable
`0x100632d0`. The valid-function mask is `0x43ec` and the function priority is 0.

| Callback | Function | Does |
|---|---|---|
| PreProcess `+0x14` | no-op | |
| **PostProcess `+0x18`** | `FUN_10007ce0` | the whole frame step (3.1) |
| PostClearAll `+0x10` | `FUN_10007c30` | frees the surface cache, recreates it with 64 buckets, frees `+0x44` |
| OnCKInit `+0x2c` | `FUN_10007930` | surface cache with 4 buckets, env = NULL, `+0xd0 = 0.001` |
| OnCKEnd `+0x30` | `FUN_10007ae0` | destroys the world and the cache |
| OnCKReset `+0x34` | `FUN_10007b00` | destroys the world (`FUN_100079b0`), `+0xd0 = 0.001`, empties the registry |
| OnCKPause `+0x3c` | no-op | |
| OnCKPlay `+0x40` | `FUN_10007910` | if the context was reset, creates a new world (`FUN_10006bb0`) |
| extra `+0x78` | `FUN_10007ea0` | world reset, registry clear, recreate |

Fields:

| Offset | Meaning |
|---|---|
| `+0x28` | array of awake IVP objects (u16 count `+0x2a`, pointer `+0x2c`): the write-back set |
| `+0x34` | CK objects destroyed on reset |
| `+0x3c` | materials created by Physicalize |
| `+0x4c` | installer queue, 3 buckets |
| `+0x50` | deferred deletes |
| `+0x54` | continuous-contact timers |
| `+0x58/+0x5c` | global IVP listeners |
| `+0x60` | attribute type of "Coll Detection ID" |
| `+0x64` | exclusive-pair collision filter |
| `+0x78..+0x8c` | profiler sums |
| `+0xbc` | collision-surface cache (hash by name) |
| `+0xc0` | IVP environment |
| `+0xc8` | smoothed frame time (ms) |
| `+0xcc` | simulation dt (s) |
| `+0xd0` | 0.001 × time factor |
| `+0xdc..+0xec` | profiler counters |
| `+0x2cd8..+0x2cec` | object registry |

### 3.1 Per frame (PostProcess `FUN_10007ce0`)

1. `smoothed = (last_delta_ms + 3·smoothed) / 4`, starting from 0.
   - `last_delta_ms` is TimeManager `+0x38`, which CK clamps to its min/max delta.
   - The constants are 3.0 at `0x10063378` and 0.25 at `0x10063374`.
2. `dt = smoothed × mgr[+0xd0]`, which is `smoothed_ms × 0.001 × TimeFactor`. With factor 2 that is **2 × the
   frame time**.
3. Retry queued installers (`FUN_100081d0`).
4. `env->simulate(dt)` (`FUN_100138c0`): the target time is `env.time + dt`.
   - The time manager (`FUN_1002ef70` / event loop `FUN_1002f250`) runs every due event, including each PSI at
     1/66 s, then sets `env.time = target`.
   - There is **no cap** on PSIs per frame.
   - The FPU is forced to 64-bit mantissa precision around the loop.
5. Fire continuous-contact timers (`FUN_100017a0`), destroy deferred deletes, accumulate the profiler.
6. For each object in `+0x28`, call `FUN_10006ec0`:
   1. Get the IVP pose at `env.time`, interpolated between PSIs by `FUN_10009d70`:
      - position = `pos_psi + v·(t − t_psi)`;
      - rotation = quaternion interpolation between the core's previous and current PSI quaternions;
      - then the mass-centre and object-to-core transforms are applied.
   2. Transpose it into a VxMatrix.
   3. Call `SetWorldMatrix(M, FALSE)` (vtable `+0x170`).

Consequences:

- **1 Virtools unit = 1 IVP metre.** There is no scaling anywhere.
- The written matrix is orthonormal. Entity scale lives only in the collision geometry.
- **Entities are never read back.** `GetWorldMatrix` is called only at creation, so script moves of a
  physicalized entity are overwritten on the next frame (while the object is awake) and are invisible to IVP.
  Ballance resets by unphysicalizing, moving and re-physicalizing.
- Write-back set: a global object listener (vtable `0x10063350`) adds an object on revive (`FUN_10008030`) and
  removes it on freeze (`FUN_10007fd0`) or delete (`FUN_10007f20`). Fixed and sleeping objects are never written.

### 3.2 World creation (`FUN_10006bb0`)

- Application environment:
  - material manager `FUN_1000bda0`;
  - collision filter chain: the exclusive-pair filter (`FUN_10014b80`, kept at `+0x64`), then the group filter
    (`FUN_10014970`);
  - range manager and profiler.
- `create_environment("NeMo")`. The environment constructor `FUN_10012a60` builds an object of 0x178 bytes:
  - PSI = 1/66 s (double `0x3f8f07c1f07c1f08`, `FUN_10013240`);
  - IVP's built-in gravity is (0, 9.83, 0), immediately replaced by (0, -9.81, 0) (`0xc0239eb851eb851f`);
  - anomaly limits object `FUN_1002f5e0` (env `+0x24`): see 5.
- Two listeners are installed, the deferred lists are created, and the "Coll Detection ID" attribute type is
  resolved.

### 3.3 Object registry

`FUN_10007800(mgr, ent, warn)` looks up an XHashTable keyed by CK_ID:

- 16 initial buckets, load factor 0.75, node pool of 200 × 0x38 bytes at `+0x118`. Insert is `FUN_10008530`,
  remove `FUN_100086f0`.
- It times itself with QueryPerformanceCounter into the profiler fields.

The value is a 0x30-byte record:

| Index | Meaning |
|---|---|
| `[0]` | surface wrapper |
| `[1]` | IVP real object: `+0xa4` core, `+0xb0` entity, `+0x80` state |
| `[2]` | material |
| `[3..5]` | entity scale |
| `[6]` | number of friction contacts |
| `[8..9]` | time the first contact started (double) |
| `[0xb]` | continuous-contact handler |

### 3.4 Materials, groups, geometry

- **Materials** (`FUN_1000bda0` manager): the pair friction is **f0 × f1** (`FUN_1000bd10`) and the pair
  elasticity is **e0 × e1** (`FUN_1000bd40`). Adhesion is a sum and zero in practice. The IVP default material
  is 0.5 / 0.5.
  - Example: stone ball (0.5) on floor (0.7) gives friction 0.35 and elasticity 0.1 × 0.3 = 0.03.
- **Collision groups** (group filter `FUN_10014900`): two objects whose **non-empty** group strings are equal
  (first 8 characters) do **not** collide. "" collides with everything. Groups in use:
  - **Ball**: player ball, ball pieces, floor stoppers, P_Modul_01 Rinne. The player passes through stoppers,
    which only stop boxes and loose balls.
  - **Floor**: level floors and several PH modules, which may therefore intersect the floor.
  - **Modul29**: the plate chain.
- **Enable Collision FALSE** keeps the object out of the collision system altogether. This applies to FixCube,
  the rope, the balloon and box_slide.
- **Geometry**:
  - Balls are analytic spheres, radius 2 for every ball in the game.
  - Convex hulls come from qhull over the scaled, deduplicated mesh vertices.
  - Concave meshes are a soup of triangle ledges. The builder (`FUN_10038290`/`FUN_100384b0`) arranges the
    ledges in a ledge tree (a bounding-sphere hierarchy) for the recursive mindist search.
  - Surfaces are cached **by name**.

## 4. The IVP engine inside the DLL

`.text` is 0x614b0 (398,512 B), with about 2,032 function starts; Ghidra has 1,848. The table maps address ranges
to subsystems. Names are inferred and sizes are ±20%.

| Range | Fns | Bytes | Subsystem |
|---|---:|---:|---|
| `10001000-10009200` | 155 | 33 K | **Virtools glue**: 13 used BBs + 3 others, manager, own force controller and listeners |
| `10009340-1000b000` | 68 | 7.4 K | real object base, create helpers, clusters (`0x10009690` builds the 0x238-byte core via `0x1000d400`) |
| `1000b000-1000c000` | 54 | 4.3 K | containers, listeners, materials |
| `1000c000-1000e000` | 53 | 8.1 K | IVP core: push and impulse (`0x1000c510`, `0x1000c830`), mass and inertia, damping (`0x1000c6a0`), sleep test (`0x1000d680`) |
| `1000e000-10010300` | 54 | 8.9 K | vector, matrix and quaternion math |
| `10010300-100112b0` | 38 | 4.0 K | buoyancy controller (**unused**) |
| `100112b0-10012a60` | 52 | 6.1 K | sim-unit and controller manager, constraint templates |
| `10012a60-10015fc0` | 147 | 13.7 K | environment, managers, perf counters, collision filters, range manager, spring actuators |
| `10015fc0-10018000` | 42 | 8.3 K | mindist objects and manager (recalc `0x10017870`) |
| `10018000-1001a000` | 46 | 8.5 K | core state sync and interpolation, quaternion to matrix |
| `1001a000-1001da00` | 60 | 14.5 K | **friction system**: forces `0x1001b080`, pair setup `0x1001cdb0`, controller |
| `1001da00-1001e000` | 16 | 1.6 K | hash tables, debug messages |
| `1001e000-10020000` | 26 | 8.1 K | core integration, anomaly clamps and movement check (`0x1001e300`), contact-point creation (`0x1001f860`) |
| `10020000-10022000` | 39 | 8.2 K | memory pools, compact-ledge triangle and edge geometry |
| `10022000-10024e80` | 41 | 11.9 K | **impact solver**, i.e. elastic collision response (`0x10022ed0`, uses asin) |
| `10024e80-10028270` | 17 | 13.3 K | buoyancy solver (**unused**) |
| `10028270-1002a540` | 14 | 8.9 K | **constraint solver** (0x190-byte constraint `0x10028270`, solver `0x10028960` is 7 K) |
| `1002a540-1002d8e0` | 29 | 13.2 K | **mindist case solvers**, a 4×4 dispatch table at `0x1007632c` (filled by `0x1002d860`) |
| `1002d8e0-1002f0b0` | 35 | 6.1 K | collision delegator, OV-tree broadphase |
| `1002f0b0-10030860` | 58 | 6.1 K | time manager and event heap, ball and polygon classes, pair creation (`0x1002f430`) |
| `10030860-10033500` | 20 | 11.4 K | **narrowphase on compact ledges**: recursive mindist over ledge trees (`0x10030860`) |
| `10033500-10037000` | 78 | 15.1 K | **friction linear solver** (matrix setup `0x10036760`, iterative loop `0x10034ab0`, up to 250 iterations) |
| `10037000-10038000` | 42 | 4.1 K | constraint and mindist helpers |
| `10038000-1003c990` | 73 | 18.8 K | surface builders (ledge soup, point soup to qhull) |
| `10047170-100490d0` | 35 | 8.0 K | polygon triangulation and tetrahedralization |
| `1003c990-10047170`, `100490d0-10060750` | 332 | 139 K | **qhull** |
| `10060750-100624b0` | 354 | 7.5 K | CRT thunks, exception-unwind funclets |

Grouped by subsystem:

| Subsystem | Fns | Code | Exercised by Ballance |
|---|---:|---:|---|
| Collision detection (mindist objects and cases, narrowphase, OV tree, ledge geometry, time and event manager) | ~200 | ~55 K | **core**: ball on concave triangle ledges, convex against convex and triangles, ball against ball |
| Friction, contact and impact (friction system, linear solver, impact solver) | ~180 | ~42 K | **core**: rolling contact, bounces |
| Core integration (core, damping, sleep, anomaly clamps, math) | ~135 | ~25 K | yes |
| Environment, managers, objects, materials, filters | ~380 | ~37 K | yes; mostly small accessors |
| Controllers (constraint solver and templates, spring, glue force controller) | ~60 | ~18 K | hinge, slider (with limits once), ball joint, spring, force |
| Surface builders and triangulation | ~108 | ~27 K | load time only |
| qhull | 332 | 139 K | load time only; any correct 3D hull builder replaces it |
| Buoyancy | ~55 | ~17-21 K | **no** |

**The per-PSI path is about 180 KB (roughly 950 functions).** It is all live, with little dead weight besides
buoyancy. After following vtables, only about 5 KB of the engine is unreachable from the glue.

**Step order** (`FUN_10013cb0`, matching the profiler's phase order):

1. step listeners;
2. controllers (`FUN_100124c0`: forces, springs, constraints, friction);
3. integration (`FUN_1001ea50`, `FUN_1001eb10`);
4. mindist passes: hull, short and critical checks (`FUN_10017850`, `FUN_10017770`).

IVP's collision detection is **event-driven**, not per-step discrete:

- Each close pair has a mindist object whose separation and "hull" sensitivity schedule when it must be checked
  next. Impacts are resolved at the predicted time of impact.
- Resting and rolling contact is handled by the friction system: persistent contact points with a linear solve,
  not impulses.

This is what makes Ballance's ball roll smoothly over triangle seams. A per-step discrete collider has to
reproduce that deliberately.

**Precision.** The event loop and hull build set the x87 control word to 64-bit mantissa. Storage mixes types:

- double: time, core matrices;
- float: velocities, ledge vertices (float4), contact and mindist data, event times as float offsets from a
  double base.

The functions use `asin` and `acos`. A port using SSE doubles and floats will be deterministic but not bit-exact.
Trajectories will diverge slowly from the original, which no design can avoid.

## 5. Constants that determine the feel

| Quantity | Value | Source |
|---|---|---|
| Units | 1 Virtools unit = 1 m, no scaling | PostProcess, create path |
| Gravity | game: **(0,-20,0)** via Set Physics Globals; manager default (0,-9.81,0); IVP default (0,9.83,0) | `FUN_100055a0`, `FUN_10006bb0` |
| Time scale | sim dt = smoothed frame time × **2** (paused: × 0) | `mgr+0xd0` |
| Frame-time smoothing | `s = (Δ + 3s)/4` | `FUN_10007ce0` |
| PSI | fixed **1/66 s** of sim time, i.e. 132 PSIs per real second while playing | env ctor |
| Friction / elasticity | product of the two materials | `FUN_1000bd10/40` |
| Damping (per PSI) | `v *= exp(-d_lin·dt)` and `ω_i *= exp(-d_rot·dt)`, using 1−x when the argument is small; **+0.1** on every factor when the core is "calm" (`core+0x60` ≥ 2) | `FUN_1000c6a0` |
| Gravity application | velocity change after damping | `FUN_10012010` |
| Ball inertia | 0.4·m·r² (solid sphere) | `FUN_10009f00` |
| Control force | impulse per PSI along a world axis; Δv per PSI = F/m | 2.2 |
| Sleep test | every 10–15 PSIs (random counter `FUN_10013610`). Short window: \|Δpos\|² ≤ 1e-4 and rotation measure ≤ 2.5e-5 → calm; calm for more than 0.3 s → freeze candidate. Long window: \|Δpos\|² ≤ 0.01 and rotation ≤ 0.04 for more than 4 s → freeze candidate. A unit freezes only when all its cores agree; \|ω\|² > 1 skips the test | `FUN_1000d680` |
| Anomaly limits | max speed 2000 m/s; max angular speed π/2 per PSI (≈104 rad/s); `70000` field of unknown use. Never reached in play | `FUN_1002f5e0`, checked in `FUN_1001e300` |
| Contact distances | 0.001, 0.01, 0.02, 0.023, 0.045, 0.22 m (base 0.01) | `FUN_10015fe0` |
| Penetration recovery | 0.5, 0.9, 0.8, 10, 0.1, 1, 5, 0.5, 15, 0.1 (meanings not traced) | env `+0x1c`, `FUN_1002d8e0` |
| Misc | env `+0x148` = 0.9^(1/66) | env ctor |
| Rolling resistance | no explicit term found. Rolling slows through the damping above and through friction-system losses | |

A rough check of the ball feel on flat ground in simulation time. This ignores rotational damping and uses
a = F·66 / (1.4·m) for a sphere rolling without slip (0.4 m r² inertia):

| Ball | Acceleration | Terminal speed ≈ a / d_lin |
|---|---|---|
| Stone | ≈ 4.3 u/s² | ≈ 14 u/s |
| Wood | ≈ 10.7 u/s² | ≈ 12 u/s |
| Paper | ≈ 15.3 u/s² | ≈ 10 u/s |

In real time, speeds double and accelerations quadruple. These are estimates to validate a port against, not
measurements.

## 6. Port options

The feel depends on three behaviors:

- rolling contact of a radius-2 sphere on triangle soups: seams, edges, rails, where the persistent friction
  contacts matter;
- low-elasticity impacts;
- the exact integration, damping and force scheme.

Hinged, slider and spring PH modules come second. Stacks of boxes do not occur beyond a few loose boxes.

**(a) Faithful port of the IVP subset.** Port the per-PSI path (section 4, about 180 KB and 950 functions)
function by function from our decompilation. That covers:

- core, damping, sleep and integration;
- time and event manager;
- mindist objects and the case solvers the game hits (ball against point, edge and triangle; convex against
  convex; ball against ball);
- ledge-tree narrowphase;
- friction system and its linear solver;
- impact solver;
- constraint solver;
- spring;
- materials and filters.

Drop buoyancy. Replace qhull with a small hull builder and write our own compact-ledge builder that produces the
same structures (triangle ledges plus a sphere tree).

- **Pros**: closest feel, including seam and edge behavior and sleep timing. Every constant already has a home.
  Validation is function-level.
- **Cons**: the largest effort, with dense float code and an event scheduler. Fields have to be identified in a
  0x238-byte core and in mindist, friction and constraint objects.
- **Effort**: about 8–12 weeks for one engineer, plus tuning against recorded trajectories.

**(b) Custom engine tuned to observed behavior.** A small rigid-body engine: sphere, convex hull and triangle
mesh; a sequential-impulse contact solver; hinge, slider, ball-joint and spring constraints. Use the same
integration, damping, gravity and force scheme and the same constants.

- **Pros**: 3–5 weeks to playable. The code is small and clear.
- **Cons**: the feel will differ where it matters most:
  - rolling over triangle edges ("bumps" at seams unless handled specially);
  - resting jitter and sleep;
  - bounce thresholds;
  - rail riding.

  Tuning without the original running is guesswork, so it may take as long again.
- **Effort**: 3–5 weeks, plus an open-ended tuning phase.

**(c) Hybrid.** Port the dynamics faithfully: core, damping and sleep, controllers and force, the friction
system with its solver, the impact solver, constraints and materials. Replace collision detection with our own
per-PSI discrete closest-point queries (sphere against triangle and convex, convex against convex via GJK), and
feed the results into the ported friction and impact code in its native form: contact points, normals and
distances.

- **Pros**: about 40% less code to port (the event-driven mindist machinery and OV tree are the hardest and most
  pointer-heavy part), with the solver semantics intact.
- **Cons**: IVP's contact creation timing (event-driven time of impact, hull sensitivity) is what the friction
  system expects. The adapter is the risky seam, and a mismatch shows exactly at edges and impacts.
- **Effort**: about 6–8 weeks.

**Recommendation: (a), staged, with the load-time parts replaced.** It follows the project's existing plan
(a clean-room Ipion port) and the "ball feel is the game" risk.

Order the stages so each one is playable:

1. **Glue**: manager, registry, surface cache, time scaling, write-back, BB commands and queue.
2. **Core and integration** (damping, gravity, force controller, sleep) with a stub collider.
3. **Ball on triangle ledges**: mindist, time manager, friction system and solver, impact. This is the critical
   milestone, and levels become playable.
4. **Convex hulls and convex-convex**: boxes and PH modules.
5. **Constraints and spring.**
6. **Collision and continuous-contact listeners**: sounds.

If stage 3 proves too costly, (c) is the fallback. It reuses everything from stages 1, 2, 5 and 6.

**Validation.** Record ball trajectories in the original (Wine or a VM, with a small in-process logger), for
example a straight run on a flat floor with each ball type, a drop and bounce, and rolling over a rail. Compare
the port's sim-time curves against them.

## 7. Open questions

- How Set Physics Spring's Length, Constant, Linear and Global damping map onto IVP's spring template.
- Whether every NULL-Object2 hinge is really redirected to FixCube (verified for P_Modul_08 only).
- The full state machine of PhysicsContinuousContact and the role of the 0.5 constant.
- CollDetection's tiny-speed constant (1e-4 or 2.5e-5); `mgr+0xcc` versus the IVP PSI length.
- Clean Physics World leaves registry entries pointing at destroyed objects. Does the game only call it when
  nothing is physicalized?
- Meanings of the penetration-recovery constants and the anomaly field `70000`.
- The ball-creation branch for an empty surface name: collision off, per fork B. It is harmless, because game
  balls always use Ball Count 1, but confirm that branch before porting.
