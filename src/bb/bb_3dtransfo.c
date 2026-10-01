/* 3DTransfo.dll Building Blocks: moving entities along curves and playing object animations
   (docs/animation.md). */
#include "bb.h"
#include "../ck/ck_anim.h"
#include "../ck/ck_curve.h"
#include <math.h>

/* ---- Position On Curve 676776d0:20d457bd (FUN_25006ca0): the target at pIn Curve's position for pIn
   Progression (CKCurve::GetPos, world). Follow (align the pIn Direction axis with the curve, FUN_2500aa80)
   and Bank are unused by the game; Follow is not ported, Bank rotates about the direction axis by the
   curve's turn in the XZ plane times Bank Amount. ---- */
static int bb_position_on_curve(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    Ck3dEntity *e = ck_entity(ctx, bb_target(ctx, b));
    CkCurve3d *c = ck_curve3d(ctx, bb_in_object(ctx, b, 0));
    if (!e || !c) return CKBR_OK;
    float p = 0.5f;
    int32_t follow = 0, bank = 0;
    bb_get_in(ctx, b, 1, &p, 4);
    bb_get_in(ctx, b, 2, &follow, 4);
    bb_get_in(ctx, b, 3, &bank, 4);
    CkVec3 pos;
    if (!ck_curve3d_get_pos(ctx, c, p, &pos, NULL)) return CKBR_OK;
    e->world[3][0] = pos.x, e->world[3][1] = pos.y, e->world[3][2] = pos.z;
    if (!bank || follow) return CKBR_OK;
    CkVec3 pp, pm;
    ck_curve3d_get_pos(ctx, c, p + 0.01f, &pp, NULL);
    ck_curve3d_get_pos(ctx, c, p - 0.01f, &pm, NULL);
    float ax = pp.x - pos.x, az = pp.z - pos.z, bx = pm.x - pos.x, bz = pm.z - pos.z;
    float la = sqrtf(ax * ax + az * az), lb = sqrtf(bx * bx + bz * bz);
    if (la == 0 || lb == 0) return CKBR_OK;
    ax /= la, az /= la, bx /= lb, bz /= lb;
    float cy = az * bx - ax * bz;             /* (A x B).y: both vectors lie in the XZ plane */
    float angle = asinf(fabsf(cy)), amount = 1;
    if (angle == 0) return CKBR_OK;
    bb_get_in(ctx, b, 4, &amount, 4);
    angle *= amount;
    float up[3] = {99, 0, 0};
    bb_get_local(ctx, b, 0, up, 12);
    if (up[1] * cy <= 0) angle = -angle;
    int32_t d = 5;
    bb_get_in(ctx, b, 5, &d, 4);
    int axis = d >= 1 && d <= 6 ? (d - 1) / 2 : 2;
    float sgn = d >= 1 && d <= 6 && !(d & 1) ? -1.0f : 1.0f;
    /* CK3dEntity::Rotate(axis, angle, ref = itself): rotate the two other axis rows about the axis row */
    int i1 = (axis + 1) % 3, i2 = (axis + 2) % 3;
    float cs = cosf(angle * sgn), sn = sinf(angle * sgn), r1[3], r2[3];
    for (int k = 0; k < 3; k++) {
        r1[k] = e->world[i1][k] * cs + e->world[i2][k] * sn;
        r2[k] = e->world[i2][k] * cs - e->world[i1][k] * sn;
    }
    for (int k = 0; k < 3; k++) e->world[i1][k] = r1[k], e->world[i2][k] = r2[k];
    return CKBR_OK;
}

/* FUN_25006c60: reset sets the saved up vector to the sentinel (99, 0, 0) */
static void cb_position_on_curve(CkContext *ctx, CkBehavior *b, int msg)
{
    if (msg == CKM_BEHAVIORRESET) {
        float up[3] = {99, 0, 0};
        bb_set_local(ctx, b, 0, up, 12);
    }
}

/* FUN_250075c0: local 0 and pOut 0 = p, then SetStep(curve(p)) */
static void play_apply(CkContext *ctx, CkBehavior *b, CkObjectAnimation *a, float p)
{
    bb_set_local(ctx, b, 0, &p, 4);
    bb_set_out(ctx, b, 0, &p, 4);
    CkParameter *cp = bb_in(ctx, b, 2);
    float y = cp && cp->value ? ck_curve_get_y((const CkCurve2d *)cp->value, p) : p;
    ck_objanim_set_step(ctx, a, y);
}

/* ---- Play Animation 3D Entity 64221225:769a143f (FUN_250073c0): plays pIn Animation over pIn Duration
   (Time, or an Integer in ms) shaped by pIn Progression Curve; On restarts, Off stops (no output); One
   Loop Played at each end, then it loops with pIn Loop, else stays at the end and stops. ---- */
static int bb_play_animation_3d_entity(CkContext *ctx, CkBehavior *b)
{
    CkObjectAnimation *a = ck_objanim(ctx, bb_in_object(ctx, b, 0));
    if (!a) return CKBR_OK;
    float duration = 5000;
    CkParameter *pin = b->pin.n > 1 ? ck_param(ctx, b->pin.v[1]) : NULL;
    if (pin && ck_guid_eq(pin->type, CKPGUID_INT)) {
        int32_t ms = 5000;
        bb_get_in(ctx, b, 1, &ms, 4);
        duration = (float)ms;
    } else {
        bb_get_in(ctx, b, 1, &duration, 4);
    }
    if (duration < 0.001f) return CKBR_OK;
    int32_t loop = 1;
    bb_get_in(ctx, b, 3, &loop, 4);
    float p = 0;
    bb_get_local(ctx, b, 0, &p, 4);
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        return CKBR_OK;
    }
    p += ctx->delta_ms / duration;
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        p = 0;
    } else if (p > 1) {
        ck_activate_output(ctx, b, 0, true);
        if (!loop) {
            play_apply(ctx, b, a, 1);
            return CKBR_OK;
        }
        p -= (float)(int32_t)p;
    }
    play_apply(ctx, b, a, p);
    return CKBR_ACTIVATENEXTFRAME;
}

/* ---- Set Position e456e78a:456789aa (FUN_25002c00): CK3dEntity::SetPosition(pIn Position, pIn Referential,
   keep children = !pIn Hierarchy) ---- */
static int bb_set_position(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    Ck3dEntity *e = ck_entity(ctx, bb_target(ctx, b));
    if (!e) return CKBR_OK;
    float pos[3] = {0, 0, 0};
    int32_t hier = 1;
    bb_get_in(ctx, b, 0, pos, 12);
    bb_get_in(ctx, b, 2, &hier, 4);
    ck_entity_set_position(ctx, e, pos, bb_in_object(ctx, b, 1), hier == 0);
    return CKBR_OK;
}

/* ---- Set World Matrix aa4aa6f0:ddefdef4 (FUN_25002fe0): SetWorldMatrix(pIn World Matrix, keep children =
   !pIn Hierarchy) ---- */
static int bb_set_world_matrix(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    Ck3dEntity *e = ck_entity(ctx, bb_target(ctx, b));
    if (!e) return CKBR_OK;
    float m[4][4];
    memcpy(m, e->world, sizeof m);
    int32_t hier = 1;
    bb_get_in(ctx, b, 0, m, 64);
    bb_get_in(ctx, b, 1, &hier, 4);
    ck_entity_set_world(ctx, e, m, hier == 0);
    return CKBR_OK;
}

/* ---- Rotate ffffffee:eeffffff (FUN_25001740): CK3dEntity::Rotate (+0x10c) by pIn Angle (default 0.01)
   around pIn Axis (default (0,1,0)) given in pIn Referential's frame, about the entity's own position;
   keep children = !Hierarchy. The rotation is Vx3DMatrixFromRotation's (row vectors, axis as given). ---- */
static int bb_rotate(CkContext *ctx, CkBehavior *b)
{
    Ck3dEntity *e = ck_entity(ctx, bb_target(ctx, b));
    if (!e) return CKBR_OK;
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    float axis[3] = {0, 1, 0}, angle = 0.01f;
    int32_t hier = 1;
    bb_get_in(ctx, b, 0, axis, 12);
    bb_get_in(ctx, b, 1, &angle, 4);
    bb_get_in(ctx, b, 3, &hier, 4);
    Ck3dEntity *ref = ck_entity(ctx, bb_in_object(ctx, b, 2));
    float a[3] = {axis[0], axis[1], axis[2]};
    if (ref) {
        /* the axis through the referential's orientation (its rows normalized) */
        for (int j = 0; j < 3; j++) a[j] = 0;
        for (int i = 0; i < 3; i++) {
            float len = sqrtf(ref->world[i][0] * ref->world[i][0] + ref->world[i][1] * ref->world[i][1] + ref->world[i][2] * ref->world[i][2]);
            if (len > 0)
                for (int j = 0; j < 3; j++) a[j] += axis[i] * ref->world[i][j] / len;
        }
    }
    float len = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (len > 0) a[0] /= len, a[1] /= len, a[2] /= len;
    float c = cosf(angle), sn = sinf(angle), t = 1 - c, x = a[0], y = a[1], z = a[2];
    float r[3][3] = {{t * x * x + c, t * x * y + sn * z, t * x * z - sn * y},
                     {t * x * y - sn * z, t * y * y + c, t * y * z + sn * x},
                     {t * x * z + sn * y, t * y * z - sn * x, t * z * z + c}};
    float m[4][4];
    memcpy(m, e->world, sizeof m);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) m[i][j] = e->world[i][0] * r[0][j] + e->world[i][1] * r[1][j] + e->world[i][2] * r[2][j];
    ck_entity_set_world(ctx, e, m, hier == 0);
    return CKBR_OK;
}

/* ---- Set Euler Orientation 0c4966d8:6c0c6d14 (FUN_25001e60, callback FUN_25002000): with three inputs (the
   game's form, "Merge Angles Params") pIn 0 is a Euler angles vector, else three floats; then Hierarchy and
   Referential. Vx3DMatrixFromEulerAngles(x, y, z), then SetOrientation (+0x12c) with dir = row 2, up = row
   1, right = row 0 in the referential's frame; keep children = !Hierarchy. The position and the axes'
   lengths (scale) are kept. ---- */
static int bb_set_euler_orientation(CkContext *ctx, CkBehavior *b)
{
    Ck3dEntity *e = ck_entity(ctx, bb_target(ctx, b));
    if (!e) return CKBR_OK;
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    int32_t merged = 0;
    bb_get_local(ctx, b, 0, &merged, 4);
    if (b->pin.n == 3) merged = 1;
    float ea[3] = {0, 0, 0};
    uint32_t k = 0;
    if (merged) bb_get_in(ctx, b, 0, ea, 12);
    else {
        bb_get_in(ctx, b, 0, &ea[0], 4);
        bb_get_in(ctx, b, 1, &ea[1], 4);
        bb_get_in(ctx, b, 2, &ea[2], 4);
        k = 2;
    }
    int32_t hier = 1;
    bb_get_in(ctx, b, k + 1, &hier, 4);
    Ck3dEntity *ref = ck_entity(ctx, bb_in_object(ctx, b, k + 2));
    float cx = cosf(ea[0]), sx = sinf(ea[0]), cy = cosf(ea[1]), sy = sinf(ea[1]), cz = cosf(ea[2]), sz = sinf(ea[2]);
    float r[3][3] = {{cy * cz, cy * sz, -sy},
                     {sx * sy * cz - cx * sz, sx * sy * sz + cx * cz, sx * cy},
                     {cx * sy * cz + sx * sz, cx * sy * sz - sx * cz, cx * cy}};
    float m[4][4];
    memcpy(m, e->world, sizeof m);
    for (int i = 0; i < 3; i++) {
        float a[3] = {r[i][0], r[i][1], r[i][2]};
        if (ref) {
            for (int j = 0; j < 3; j++) a[j] = 0;
            for (int q = 0; q < 3; q++) {
                float len = sqrtf(ref->world[q][0] * ref->world[q][0] + ref->world[q][1] * ref->world[q][1] + ref->world[q][2] * ref->world[q][2]);
                if (len > 0)
                    for (int j = 0; j < 3; j++) a[j] += r[i][q] * ref->world[q][j] / len;
            }
        }
        float scale = sqrtf(e->world[i][0] * e->world[i][0] + e->world[i][1] * e->world[i][1] + e->world[i][2] * e->world[i][2]);
        for (int j = 0; j < 3; j++) m[i][j] = a[j] * scale;
    }
    ck_entity_set_world(ctx, e, m, hier == 0);
    return CKBR_OK;
}

/* ---- Scale 41236987:a54a87a6 (FUN_250012a0): pIn 0 the scale vector; Absolute: SetScale (+0x140), else
   AddScale (+0x11c, multiplied, local); keep children = !Hierarchy. (3D sprites, class 37, scale their
   size instead; the game has none.) ---- */
static int bb_scale(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    Ck3dEntity *e = ck_entity(ctx, bb_target(ctx, b));
    if (!e) return CKBR_OK;
    float v[3] = {1, 1, 1};
    int32_t hier = 1, absolute = 0;
    bb_get_in(ctx, b, 0, v, 12);
    bb_get_in(ctx, b, 1, &hier, 4);
    bb_get_in(ctx, b, 2, &absolute, 4);
    ck_entity_scale(ctx, e, v, absolute != 0, hier == 0);
    return CKBR_OK;
}

/* ---- Set Parent 9d9d9d98:7e7a7f75 (execute 0x25002a70): CK3dEntity::SetParent(pIn Parent, keep world
   position) (CK2_3D 0x1000932b: not itself, not a descendant; NULL unparents) ---- */
static int bb_set_parent(CkContext *ctx, CkBehavior *b)
{
    Ck3dEntity *e = ck_entity(ctx, bb_target(ctx, b));
    if (!e) return CKBR_OK;
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkId p = bb_in_object(ctx, b, 0);
    if (!ck_entity(ctx, p)) p = 0;
    if (p == e->be.h.id) return CKBR_OK;
    for (Ck3dEntity *a = ck_entity(ctx, p); a; a = ck_entity(ctx, a->parent))
        if (a == e) return CKBR_OK;
    e->parent = p;
    ctx->hier_gen++;
    return CKBR_OK;
}

/* the local bounding box of the entity's current mesh (GetBoundingBox(TRUE); zero without a mesh) */
static void local_box(CkContext *ctx, const Ck3dEntity *e, float mn[3], float mx[3])
{
    CkMesh *m = ck_mesh(ctx, e->mesh);
    memset(mn, 0, 12), memset(mx, 0, 12);
    if (!m || !m->nverts) return;
    for (int k = 0; k < 3; k++) mn[k] = 1e30f, mx[k] = -1e30f;
    for (uint32_t i = 0; i < m->nverts; i++) {
        const float p[3] = {m->verts[i].pos.x, m->verts[i].pos.y, m->verts[i].pos.z};
        for (int k = 0; k < 3; k++) {
            if (p[k] < mn[k]) mn[k] = p[k];
            if (p[k] > mx[k]) mx[k] = p[k];
        }
    }
}

typedef struct {
    float c[3], ax[3][3], ext[3];
} Obb;

/* VxOBB(box, M) (Collisions 0x253141e0): the centre through M, unit axes, extents scaled by the rows */
static void obb_of(CkContext *ctx, const Ck3dEntity *e, Obb *o)
{
    float mn[3], mx[3];
    local_box(ctx, e, mn, mx);
    float lc[3];
    for (int k = 0; k < 3; k++) lc[k] = (mx[k] + mn[k]) * 0.5f;
    for (int j = 0; j < 3; j++) o->c[j] = lc[0] * e->world[0][j] + lc[1] * e->world[1][j] + lc[2] * e->world[2][j] + e->world[3][j];
    for (int i = 0; i < 3; i++) {
        float l = sqrtf(e->world[i][0] * e->world[i][0] + e->world[i][1] * e->world[i][1] + e->world[i][2] * e->world[i][2]);
        for (int j = 0; j < 3; j++) o->ax[i][j] = l > 0 ? e->world[i][j] / l : 0;
        o->ext[i] = (mx[i] - mn[i]) * 0.5f * l;
    }
}

/* VxIntersect::OBBOBB (VxMath 0x24287830): the 15-axis separating test; touching counts */
static bool obb_obb(const Obb *a, const Obb *b)
{
    float R[3][3], AR[3][3], t[3], T[3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            R[i][j] = a->ax[i][0] * b->ax[j][0] + a->ax[i][1] * b->ax[j][1] + a->ax[i][2] * b->ax[j][2];
            AR[i][j] = fabsf(R[i][j]);
        }
    for (int k = 0; k < 3; k++) t[k] = b->c[k] - a->c[k];
    for (int i = 0; i < 3; i++) T[i] = t[0] * a->ax[i][0] + t[1] * a->ax[i][1] + t[2] * a->ax[i][2];
    for (int i = 0; i < 3; i++)
        if (fabsf(T[i]) > a->ext[i] + b->ext[0] * AR[i][0] + b->ext[1] * AR[i][1] + b->ext[2] * AR[i][2]) return false;
    for (int j = 0; j < 3; j++) {
        float tb = T[0] * R[0][j] + T[1] * R[1][j] + T[2] * R[2][j];
        if (fabsf(tb) > b->ext[j] + a->ext[0] * AR[0][j] + a->ext[1] * AR[1][j] + a->ext[2] * AR[2][j]) return false;
    }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            int i1 = (i + 1) % 3, i2 = (i + 2) % 3, j1 = (j + 1) % 3, j2 = (j + 2) % 3;
            float proj = fabsf(T[i2] * R[i1][j] - T[i1] * R[i2][j]);
            float rad = a->ext[i1] * AR[i2][j] + a->ext[i2] * AR[i1][j] + b->ext[j1] * AR[i][j2] + b->ext[j2] * AR[i][j1];
            if (proj > rad) return false;
        }
    return true;
}

/* ---- Box Box Intersection 64154401:76cf37af (Collisions execute 0x25305850): the oriented local boxes of
   pIn Entity 0 and 1 (BoxBoxIntersection with local boxes -> OBBOBB) -> True / False. (The hierarchy pins,
   FALSE in the game, would use hierarchical boxes.) ---- */
static int bb_box_box_intersection(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    Ck3dEntity *e0 = ck_entity(ctx, bb_in_object(ctx, b, 0)), *e1 = ck_entity(ctx, bb_in_object(ctx, b, 1));
    if (e0 && e1) {
        Obb a, c;
        obb_of(ctx, e0, &a);
        obb_of(ctx, e1, &c);
        if (obb_obb(&a, &c)) {
            ck_activate_output(ctx, b, 0, true);
            return CKBR_OK;
        }
    }
    ck_activate_output(ctx, b, 1, true);
    return CKBR_OK;
}

BB_DECL_CB(d_position_on_curve, 676776d0, 20d457bd, "Position On Curve", bb_position_on_curve, cb_position_on_curve);
BB_DECL(d_play_animation_3d_entity, 64221225, 769a143f, "Play Animation 3D Entity", bb_play_animation_3d_entity);
BB_DECL(d_set_position, e456e78a, 456789aa, "Set Position", bb_set_position);
BB_DECL(d_set_world_matrix, aa4aa6f0, ddefdef4, "Set World Matrix", bb_set_world_matrix);
BB_DECL(d_set_euler_orientation, 0c4966d8, 6c0c6d14, "Set Euler Orientation", bb_set_euler_orientation);
BB_DECL(d_rotate, ffffffee, eeffffff, "Rotate", bb_rotate);
BB_DECL(d_scale, 41236987, a54a87a6, "Scale", bb_scale);
BB_DECL(d_set_parent, 9d9d9d98, 7e7a7f75, "Set Parent", bb_set_parent);
BB_DECL(d_box_box_intersection, 64154401, 76cf37af, "Box Box Intersection", bb_box_box_intersection);

const CkBBDecl *const bb_3dtransfo[] = {&d_position_on_curve, &d_play_animation_3d_entity, &d_set_position, &d_set_world_matrix,
                                        &d_rotate, &d_set_euler_orientation, &d_scale, &d_set_parent, &d_box_box_intersection};
const unsigned bb_3dtransfo_count = sizeof bb_3dtransfo / sizeof *bb_3dtransfo;
