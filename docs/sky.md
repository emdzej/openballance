# TT Sky (Building Block)

`TT Sky`, GUID `36691920:3b261630`, `TT_Gravity_RT.dll` (image base `0x10000000`, Ghidra project
`ballance`, program `TT_Gravity_RT.dll`). Category `TT_Gravity`, author Terratools, version `0x10000`,
compatible class `0x13` (BeObject). The German description in the DLL says: "creates a sky object with any
number of side faces and changeable material transparency".

| Function | Address |
|---|---|
| declaration | `0x10004f30` |
| prototype creation | `0x10004fc0` |
| execute | `0x10005180` |
| behavior callback | `0x10005f40` (all messages, mask `0xffffffff`) |
| pre-render callback (entity) | `0x10006220` (a label, not a Ghidra function) |
| post-render callback (entity) | `0x10006360` (a label) |

The Ghidra decompile of `0x10005180` is unreliable: it confuses stack slots and registers. Everything
below comes from the disassembly, with stack slots tracked by hand.

In short, TT Sky builds a procedural prism ("sky box") around the origin: N textured side walls, plus an
optional top cap and bottom cap. It puts that mesh on a new, hidden, unsaved 3D entity that is drawn
first, with no z-test and no z-write. Every frame, while the BB stays active, it moves the entity to the
active camera's position and gives it the orientation of the "Orientation Object". Around the entity's
own draw, a pre-render callback swaps in a projection with a wider FOV ("Distortion") and near=1,
far=200. A post-render callback puts the camera projection back.

## 1. Prototype (`0x10004fc0`)

Inputs: `0 On`, `1 Off`. Outputs: `0 Exit On`, `1 Exit Off`.

Input parameters (fixed part, indices 0..6):

| # | Name (exact) | Type | Default string | Default in code | Meaning |
|---|---|---|---|---|---|
| 0 | `Distortion` | Percentage (`f3c84b4e:0ffacc34`, float 0..1) | `30` | 0.3 | FOV widening. If > 1.0 it is clamped to 1.0 (`0x10005269`). There is no lower clamp. |
| 1 | `Vertex Color` | Color (`57d42fee:7cbb3b91`, 4 floats RGBA) | `1, 1, 1, 0` | (1,1,1,0) | Prelit colour written to every vertex, every frame. |
| 2 | `Orientation Object` | 3D Entity (`5b8a05d5:31ea28d4`) | none | NULL | Whose world matrix gives the sky its orientation. |
| 3 | `Radius` | Float | `70.0f` | 70.0 | Circumradius of the N-gon, in the XZ plane. |
| 4 | `Quadratic SideFaces?` | Boolean | `TRUE` | 1 | If set, the wall height is the wall width (square walls). |
| 5 | `or SideFace-Heigth` | Float | `10` | 0 (read only if pin 4 is false) | Wall height H if not quadratic. |
| 6 | `Y-Position of Sky` | Float | `0` | 0.0 | Y of the walls' vertical centre. |

Settings (local parameters 0..2):

| # | Name | Type | Default | Meaning |
|---|---|---|---|---|
| 0 | `Side Materials` | Integer | `4` | N = number of sides (the callback clamps it to >= 4) |
| 1 | `Top Material` | Boolean | `True` | build a top cap |
| 2 | `Bottom Material` | Boolean | `True` | build a bottom cap |

Local 3 is `Skyaround`, type `4d082c90:0c8339a2` (a raw buffer), 80 bytes. It is the runtime state (§6).

Dynamic material inputs are created by the callback (§7) after the 7 fixed ones, all of type Material
(`55ab12cd:22ae6a8b`):
- `7 .. 7+N-1`: `"%d.Side-Mat"` with 1-based numbers: `1.Side-Mat`, `2.Side-Mat`, ...
- then `Top Mat` if Top is set;
- then `Bottom Mat` if Bottom is set.
- So the top material is input `N+7`. The bottom material is input `N+8` if Top is set, otherwise `N+7`.

`SetFlags(1)` on the prototype.

## 2. Game usage

The game uses TT Sky twice. Both uses were probed with `ck_load` + `ck_param_resolve`:

| File | Behavior | Distortion | Vertex Color | Orient. Obj | Radius | Quadratic | Height pin | Y | N | Top | Bottom |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `3D Entities/MenuLevel.nmo` (BB #325) | id 6437 | 0.10 | (1,1,1,1) | NULL | 70 | TRUE | 10 (unused) | 0 | 4 | 0 | 1 |
| `3D Entities/Gameplay.nmo` (BB #3658) | id 9770 | 0.15 | (1,1,1,1) | NULL | 100 | TRUE | 10 (unused) | 0 | 4 | 0 | 1 |

In both files the material pins are `1.Side-Mat`, `2.Side-Mat`, `3.Side-Mat`, `4.Side-Mat` and
`Bottom Mat`. They come from the outputs `Back`, `Right`, `Front`, `Left` and `Down` of a `Get Row` BB
(a sky-materials data array). These are NULL in the file and filled at runtime. `Get Row.Found -> TT
Sky.On`. In MenuLevel, `TT Sky.Exit On -> Set Fog.In`. Nothing drives `Off` in either file. The saved
`Skyaround` is all zeros, except the unused padding bytes after +0x4c.

What the game gets: V = 7 vertices per side, F = 3 faces per side, 28 vertices, 12 faces. The prism is an
axis-aligned cube with side `R*sqrt(2)`, centred on the camera: half-extent 49.50 in the menu and 70.71 in
gameplay. It has 4 walls plus a floor of 4 triangles, and no ceiling.

## 3. Execute (`0x10005180`), per call

Unconditional prologue (every call):
1. Reads `Vertex Color` (pin 1) into r,g,b,a (default 1,1,1,0).
   `color = RGBAFTOCOLOR(r,g,b,a)` (VxMath import, gives ARGB `A<<24|R<<16|G<<8|B` with each value x255).
2. Reads N (local 0, default 0), Top (local 1, byte, default 0) and Bottom (local 2, byte, default 0).
3. If `On` (input 0) is active, it builds the sky (§4) and then falls through to 4.
4. Per-frame update (§5). This also runs on the On frame.

## 4. On: building the sky (`0x1000523d`..`0x10005c9b`)

1. `ActivateInput(0, FALSE)`, then `ActivateOutput(0 "Exit On", TRUE)`. The output fires before anything
   is built.
2. Reads the parameters:
   - `d` = pin 0 (default 0.3; if `d > 1.0` then `d = 1.0`);
   - `orient` = `GetInputParameterObject(2)`;
   - `R` = pin 3 (default 70);
   - `quad` = pin 4 (byte, default 1);
   - `H` = 0, and if `!quad`, `H` = pin 5;
   - `Y` = pin 6 (default 0).
3. Creates the objects. The creation option is `CK_OBJECTCREATION_DYNAMIC` (0x10) if the behavior's own
   object flags contain all of `CK_OBJECT_DYNAMIC` (`(flags & 0x108) == 0x108`), otherwise 0.
   - `mesh = CreateObject(CKCID_MESH 0x20, "TT_Sky_Mesh", opt)`; if NULL, it returns
     `CKBR_PARAMETERERROR` (0xA008).
   - `mesh->m_ObjectFlags |= 0x23` (`CK_OBJECT_NOTTOBELISTEDANDSAVED`: interface-only, private, not saved).
   - `ent = CreateObject(CKCID_3DENTITY 0x21, "TT_Sky_Entity", opt)`; if NULL, it returns 0xA008.
   - `ent->m_ObjectFlags |= 0x23`.
   - If mesh creation fails, the entity is never created. If entity creation fails, the mesh leaks.
4. Per-side counts:
   - `V = 4 + (Top ? 3 : 0) + (Bottom ? 3 : 0)` vertices per side;
   - `F = 2 + Top + Bottom` faces per side.
5. If `quad`, the wall width becomes the height: `step = 2*pi/N`, `cx = R*cos(step)`, `sy = R*sin(step)`,
   `H = sqrt((R - cx)^2 + sy^2)`. That is the N-gon edge length, `2*R*sin(pi/N)`.
6. `mesh->SetVertexCount(V*N)`.

### 4.1 Vertices

Loop over sides `i = 0..N-1`. The vertex base is `b = i*V`. Constants: `step = 2*pi/N` (float
6.2831855 / N), `A0 = 3.9269910` (5*pi/4), `K = 0.7071` (float `0x3f350481`).

```
a0 = i*step + A0      x0 = R*cos(a0)   z0 = R*sin(a0)
a1 = (i+1)*step + A0  x1 = R*cos(a1)   z1 = R*sin(a1)
lo = Y - 0.5*H        hi = Y + 0.5*H
capUV(x,z): n = 1/sqrt(x*x + z*z);  u = 0.5 + K*(x*n);  v = 0.5 + K*(-z*n)
```

`SetVertexPosition` (+0xa0) and `SetVertexTextureCoordinates(idx, u, v, -1)` (+0xa4) for each vertex:

| Vertex | Position | UV | Condition |
|---|---|---|---|
| b+0 | (x0, lo, z0) | (1, 1) | always |
| b+1 | (x1, lo, z1) | (0, 1) | always |
| b+2 | (x1, hi, z1) | (0, 0) | always |
| b+3 | (x0, hi, z0) | (1, 0) | always |
| b+4 | (x0, hi, z0) | capUV(x0,z0) | Top |
| b+5 | (x1, hi, z1) | capUV(x1,z1) | Top |
| b+6 | (0, hi, 0) | (0.5, 0.5) | Top |
| k+0 | (x0, lo, z0) | capUV(x0,z0) | Bottom; k = b+7 if Top, else b+4 |
| k+1 | (0, lo, 0) | (0.5, 0.5) | Bottom |
| k+2 | (x1, lo, z1) | capUV(x1,z1) | Bottom |

Notes:
- Vertex order on the caps differs: top is (corner0, corner1, centre), bottom is (corner0, centre, corner1).
- Since `|(x,z)| = R`, capUV is `(0.5 + K*cos a, 0.5 - K*sin a)`. With the 5*pi/4 start angle and N=4,
  the four corners land exactly on the texture corners:
  - a=5pi/4, position (-,-), UV (0,1);
  - 7pi/4, (+,-), UV (1,1);
  - pi/4, (+,+), UV (1,0);
  - 3pi/4, (-,+), UV (0,0).
- Each side maps its whole texture on its wall. u runs from 1 at corner 0 to 0 at corner 1, and v=0 is
  at the top.
- With N=4, the sides are:
  - i=0: the -Z wall (x -a..+a), material `1.Side-Mat` = "Back";
  - i=1: the +X wall, "Right";
  - i=2: the +Z wall, "Front";
  - i=3: the -X wall, "Left".
  - The walls are at `a = R/sqrt(2)`.

### 4.2 Faces

`mesh->SetFaceCount(F*N)` (+0xf4). Loop over sides `i`. The face base is `f = i*F`. Vertex indices use
`b` as above. `SetFaceVertexIndex` (+0x114), `SetFaceMaterial` (+0x11c):

| Face | Vertices | Material |
|---|---|---|
| f+0 | (b+2, b+0, b+1) | input `7+i` (i-th Side-Mat) |
| f+1 | (b+2, b+3, b+0) | input `7+i` |
| f+2 (Top only) | (b+4, b+5, b+6) | input `N+7` (Top Mat) |
| f+2 or f+3 (Bottom; f+3 if Top) | (k, k+1, k+2) | input `N+8` if Top, else `N+7` (Bottom Mat) |

Winding: with the face normal `(v1-v0) x (v2-v0)`, every face normal points into the prism (toward the
axis, down for the top, up for the bottom). This is the same convention as the file meshes, so the sky is
seen from inside. Worked out for i=0, N=4: the wall normal is +Z, the top normal is -Y, the bottom normal
is +Y. The floor is N separate triangles that all share the centre, each with duplicated vertices.

### 4.3 Entity setup (`0x10005ba7`..`0x10005c9b`), in this order

1. `mesh->BuildFaceNormals()` (+0x1b4).
2. `ent->SetMoveableFlags(ent->GetMoveableFlags() | 0x100000)`, i.e. `VX_MOVEABLE_RENDERFIRST`.
3. `behctx->CurrentLevel->AddObject(ent)` (`CKLevel::AddObject`, level from `CKBehaviorContext+0xc`).
4. `ent->Show(CKHIDE)` (vtable +0x00, argument 0).
5. `ent->AddMesh(mesh)` (+0xfc), then `ent->SetCurrentMesh(mesh, TRUE)` (+0xf0).
6. `ent->SetPickable(FALSE)` (+0xb4).
7. Writes `Skyaround` (local 3, 80 bytes) with `SetLocalParameterValue(3, buf, 0x50)`:
   - `+0x00` = d;
   - `+0x04..+0x43` = 0;
   - `+0x44` = ent;
   - `+0x48` = orient;
   - `+0x4c` byte = 1.
8. `ent->SetMoveableFlags(GetMoveableFlags() | 0x280000)`, i.e. `VX_MOVEABLE_NOZBUFFERTEST (0x200000) |
   VX_MOVEABLE_NOZBUFFERWRITE (0x80000)`.
9. `p = GetLocalParameterReadDataPtr(3)`. Then `ent->AddPreRenderCallBack(0x10006220, p, FALSE)` (+0x6c)
   and `ent->AddPostRenderCallBack(0x10006360, p, FALSE)` (+0x7c). The callback argument is the local's
   storage itself.

What TT Sky does not touch: fog, lighting (beyond the prelit mode below), ZOrder, render channels or
material states. In MenuLevel, fog is handled separately by the following `Set Fog` BB.

Distortion and the orientation object are captured at On time. Changes to pins 0 and 2 afterwards have
no effect until the next On. Radius, height, Y, N, Top and Bottom are also only applied at On.

## 5. Per-frame update (`0x10005c9e`..`0x10005f36`)

1. `p = GetLocalParameterReadDataPtr(3)`, `ent = p+0x44`. If `ent == NULL`, it returns 0xA008. This
   happens before Off is checked, so Off before any On does nothing: `Exit Off` does not fire and Off is
   not consumed.
2. `mesh = ent->GetCurrentMesh()` (+0xec). `nf = mesh->GetFaceCount()` (+0xf0). `F = 2 + Top + Bottom`
   (from the current settings).
3. It reassigns every face's material from the current inputs, every frame. For side `i`:
   - faces `f+0`, `f+1` get input `7+i`;
   - face `f+2` gets input `N+7` if Top;
   - the next face gets input `N+8`/`N+7` if Bottom.

   So materials can change while the sky is active (the game fills them via Get Row before On). The
   original also calls `sideMat->GetDiffuse()` (material +0x64, result ignored, a no-op) and would crash
   on a NULL side material. A port should just skip NULLs.
4. Colours: `ptr = mesh->GetColorsPtr(&stride)` (+0xa8), `n = mesh->GetVertexCount()` (+0x8c). It writes
   `color` to `n+1` entries. The original has an off-by-one and writes one colour past the end; write `n`.
   Then `mesh->ColorChanged()` (+0xec) and `mesh->SetLitMode(VX_PRELITMESH = 0)` (+0x64).
   - The sky is prelit: vertex colour x texture, no lighting.
   - The alpha comes from `Vertex Color` (the game uses 1.0; the prototype default alpha is 0).
5. If `Off` (input 1) is active:
   - `ActivateInput(1, FALSE)`, `ActivateOutput(1 "Exit Off", TRUE)`;
   - `ent->Show(CKHIDE)`;
   - return `CKBR_OK`.
   - Nothing is destroyed: the entity, mesh and callbacks stay, and the entity is just hidden.
6. Otherwise, the camera follow:
   - `ent->Show(CKSHOW)`.
   - `mesh->GetMaterial(0)->GetDiffuse()` (+0x1dc, then +0x64): a no-op, but it crashes if face 0 has no
     material.
   - `cam = rc->GetAttachedCamera()` (+0x194); if NULL, `cam = rc->GetViewpoint()` (+0x198); if still
     NULL, return 0xA008. `rc` is `CKBehaviorContext+0x18`, the current render context.
   - `cam->GetPosition(&tmp, NULL)` (+0x128). The result is unused.
   - `M = orient ? orient->GetWorldMatrix() : VxMatrix::Identity()` (`orient` = `p+0x48`).
     `ent->SetWorldMatrix(M, FALSE)` (+0x170). This copies the orient object's rotation and also its
     scale.
   - `ent->SetPosition3f(0, 0, 0, cam, FALSE)` (+0x120): the entity origin goes to the camera position.
   - return `CKBR_ACTIVATENEXTFRAME` (1). The BB keeps itself running every frame until Off.

So the sky follows the camera twice:
- in this per-frame execution (orientation + position);
- in the pre-render callback (position only, §6).

## 6. Render callbacks and the `Skyaround` state

`Skyaround` layout (80 bytes):

| Offset | Content |
|---|---|
| +0x00 | float d (Distortion, clamped to <= 1) |
| +0x04 | VxMatrix (64 bytes): the camera projection saved by the pre-render callback |
| +0x44 | CK3dEntity* sky entity |
| +0x48 | CK3dEntity* orientation object |
| +0x4c | byte "callbacks installed" (1 after On) |

Callbacks have the signature `CKBOOL cb(CKRenderContext *dev, CKRenderObject *ent, void *arg)` (cdecl),
with `arg` = the Skyaround storage.

Pre-render callback (`0x10006220`), called just before the sky entity is drawn:
1. If `arg == NULL`, it returns 0xA008.
2. `dev->GetViewRect(&r)` (+0xe0, r pre-zeroed). `w = r.right - r.left`, `h = r.bottom - r.top`.
3. `P = dev->GetProjectionTransformationMatrix()` (+0x170). The callback copies it to `arg+0x04`.
4. `fov = |2*atan(1/P[0][0])|`. This is the camera's horizontal FOV, taken from the live projection.
5. `half = 0.5*(fov + (pi - fov)*d)`, with pi taken as the double 3.141592654. Distortion interpolates
   the FOV from the camera's (d=0) toward 180 degrees (d=1; cot = 0, degenerate).
6. It builds a new all-zero matrix Q, then sets:
   - `Q[0][0] = cot(half)`
   - `Q[1][1] = cot(half) * w / h`
   - `Q[2][2] = 1.0050251` (`0x3f80a4aa`)
   - `Q[2][3] = 1.0`
   - `Q[3][2] = -1.0050251`
   - `Q[3][3] = 0`

   This is a D3D-style perspective with near = 1 and far = 200 (`f/(f-n) = 200/199`). The matrix is
   row-major VxMatrix `[row][col]`, with row 3 = translation.
7. `dev->SetProjectionTransformationMatrix(Q)` (+0x164).
8. `cam = dev->GetAttachedCamera()`, or `dev->GetViewpoint()`. If either is found:
   `ent->SetPosition(&VxVector::axis0() /*(0,0,0)*/, cam, FALSE)` (entity +0x124). This snaps the sky to
   the camera again at draw time.
9. Returns 0.

Post-render callback (`0x10006360`): `dev->SetProjectionTransformationMatrix(arg+0x04)`, which restores
the camera projection. Returns 0.

Effect: the sky is drawn first (`RENDERFIRST`), with no z-test and no z-write. It uses its own wider
projection and is always centred on the eye, so it never gets closer and never clips the scene.

## 7. Behavior callback (`0x10005f40`)

Switch on `CKBehaviorContext.CallbackMessage` (+0x2c):

- **2 DELETE, 4 DETACH, 9 RESET, 17 DEACTIVATESCRIPT** (`0x10005fa5`): if `p+0x44` (the entity) is set:
  - `ent->RemovePreRenderCallBack(0x10006220, p)`;
  - `ent->RemovePreRenderCallBack(0x10006360, p)`. This really is +0x70 both times: the original never
    removes the post-render callback, which is harmless.
  - `CKDestroyObject(ent, 0, NULL)`. The mesh is not destroyed and leaks.

  Then, even without an entity, Skyaround is set to 80 zero bytes.
- **3 ATTACH, 13 SETTINGSEDITED** (`0x10006021`): this is editor or attach-time pin generation.
  - Skyaround is set to zeros.
  - It reads Top, Bottom and N (default 4), clamps `N >= 4` and writes N back to local 0.
  - It removes all input parameters with index > 6 (destroying them).
  - It creates `"%d.Side-Mat"` for 1..N, then `Top Mat` if Top, then `Bottom Mat` if Bottom.

  A port whose pins come from the file should not rebuild them. Just zero the state.
- **5 PAUSE** (`0x10006169`): if an entity is set, the same two `RemovePreRenderCallBack` calls as above.
- **6 RESUME** (`0x1000619d`): if an entity is set:
  - if byte `p+0x4c` is set: `AddPreRenderCallBack(0x10006220, p, FALSE)`;
  - always: `AddPostRenderCallBack(0x10006360, p, FALSE)`. In the original this adds a duplicate post
    callback, which is harmless because restoring twice is idempotent.
- **11 LOAD**: Skyaround is set to zeros.
- Other messages: nothing. All cases return 0.

## 8. Quirks worth keeping in mind

- A second `On` while a sky exists builds another mesh and entity and overwrites the pointers. The old
  sky stays in the level with its callbacks, visible, and is never destroyed. The game only fires On once
  per script activation. A port can destroy the old sky first.
- `Off` only hides the sky. RESET, DEACTIVATESCRIPT, DETACH and DELETE destroy it.
- Distortion/orientation are frozen at On; vertex colour and materials are re-read every frame.
- The per-frame SetWorldMatrix uses the orientation object's full world matrix, scale included. Only the
  translation is then replaced by the camera position.

## 9. Vtable identification

Slots come from the Virtools SDK 2.1 headers (doyaGu/Virtools-SDK-2.1: `CKObject.h`,
`CKSceneObject.h`, `CKBeObject.h`, `CKRenderObject.h`, `CK3dEntity.h`, `CKMesh.h`, `CKMaterial.h`,
`CKRenderContext.h`), counting new virtuals down the inheritance chain. Each slot's argument count
matches the pushes in the disassembly. Consistency check: `src/ck/ck_3d.c` already uses CKMesh
`BuildNormals` at +0x1b0, the slot just before `BuildFaceNormals` +0x1b4. I did not open the CK2_3D.dll
vtables for each entry.

| Class | Offset | Method | Confidence |
|---|---|---|---|
| CKObject | +0x00 | Show(option) (0 hide, 1 show) | high |
| CKMesh | +0x64 | SetLitMode(VX_PRELITMESH) | high |
| CKMesh | +0x8c / +0x90 | GetVertexCount / SetVertexCount | high |
| CKMesh | +0xa0 | SetVertexPosition(int, const VxVector*) | high |
| CKMesh | +0xa4 | SetVertexTextureCoordinates(int, u, v, channel=-1) | high |
| CKMesh | +0xa8 | GetColorsPtr(DWORD *stride) | high |
| CKMesh | +0xec | ColorChanged() | high |
| CKMesh | +0xf0 / +0xf4 | GetFaceCount / SetFaceCount | high |
| CKMesh | +0x114 | SetFaceVertexIndex(face, v1, v2, v3) | high |
| CKMesh | +0x11c | SetFaceMaterial(face, CKMaterial*) | high |
| CKMesh | +0x1b4 | BuildFaceNormals() | high |
| CKMesh | +0x1dc | GetMaterial(int) (per-material list, not per-face) | medium-high |
| CKMaterial | +0x64 | GetDiffuse() (result unused) | medium (any getter would be equally inert) |
| CKRenderObject | +0x6c / +0x70 | AddPreRenderCallBack / RemovePreRenderCallBack | high |
| CKRenderObject | +0x7c | AddPostRenderCallBack | high |
| CK3dEntity | +0xb4 | SetPickable | high |
| CK3dEntity | +0xe0 / +0xe4 | GetMoveableFlags / SetMoveableFlags | high |
| CK3dEntity | +0xec / +0xf0 / +0xfc | GetCurrentMesh / SetCurrentMesh(mesh, add) / AddMesh | high |
| CK3dEntity | +0x120 | SetPosition3f(x, y, z, ref, keepChildren) | high |
| CK3dEntity | +0x124 | SetPosition(const VxVector*, ref, keepChildren) | high |
| CK3dEntity | +0x128 | GetPosition(VxVector*, ref) | high |
| CK3dEntity | +0x170 / +0x174 | SetWorldMatrix(mat, keepChildren) / GetWorldMatrix | high |
| CKRenderContext | +0xe0 | GetViewRect(VxRect&) | high |
| CKRenderContext | +0x164 / +0x170 | Set / GetProjectionTransformationMatrix | high |
| CKRenderContext | +0x194 / +0x198 | GetAttachedCamera / GetViewpoint | high |

Imports: `0x100101e8` `VxVector::axis0()`, `0x100101ec` `RGBAFTOCOLOR(float,float,float,float)`,
`0x100101f0` `VxMatrix::Identity()`.

Constants:

| Address | Value |
|---|---|
| `0x10010218` | 1.0 |
| `0x10010250` | 0.5 |
| `0x10010258` | 0.7071 |
| `0x1001025c` | 3.9269910 (5*pi/4) |
| `0x10010260` | 6.2831855 (2*pi) |
| `0x10010268` | double 3.141592654 |

Strings:

| Address | String |
|---|---|
| `0x100129a0` | `TT_Sky_Mesh` |
| `0x10012990` | `TT_Sky_Entity` |
| `0x100129c0` | `%d.Side-Mat` |
| `0x100129b8` | `Top Mat` |
| `0x100129ac` | `Bottom Mat` |
