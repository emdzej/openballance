/* TT_Gravity_RT.dll Building Blocks (Terratools' game-specific blocks). TT Sky: docs/sky.md; TT Extra and
   TT Simple Shadow: docs/gameplay_bbs.md. */
#include "bb.h"
#include "../ck/ck_3d.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* TT Sky's runtime state (local 3 "Skyaround" in the original: distortion, the saved projection, the
   entity, the orientation object, "callbacks installed") */
typedef struct {
    float distortion;
    CkId entity, mesh, orient;
} SkyState;

static uint32_t rgbaf_to_color(const float c[4])
{
    uint32_t k = 0;
    for (int i = 0; i < 4; i++) k |= ((uint32_t)(int32_t)(c[i] * 255.0f) & 0xff) << (i == 3 ? 24 : 16 - 8 * i);
    return k;
}

/* Material inputs: 7.. the N sides, then Top Mat if Top, then Bottom Mat if Bottom */
static CkId sky_material(CkContext *ctx, CkBehavior *b, uint32_t pin)
{
    CkId m = bb_in_object(ctx, b, pin);
    return ck_material(ctx, m) ? m : 0;
}

static void sky_face_materials(CkContext *ctx, CkBehavior *b, CkMesh *m, int32_t n, bool top, bool bottom)
{
    uint32_t per = 2 + top + bottom;
    for (int32_t i = 0; i < n; i++) {
        uint32_t f = (uint32_t)i * per;
        CkId side = sky_material(ctx, b, 7 + (uint32_t)i);
        if (side) ck_mesh_set_face_material(m, f, side), ck_mesh_set_face_material(m, f + 1, side);
        if (top) {
            CkId t = sky_material(ctx, b, (uint32_t)n + 7);
            if (t) ck_mesh_set_face_material(m, f + 2, t);
        }
        if (bottom) {
            CkId bm = sky_material(ctx, b, (uint32_t)n + (top ? 8 : 7));
            if (bm) ck_mesh_set_face_material(m, f + 2 + top, bm);
        }
    }
}

/* ---- TT Sky 36691920:3b261630 (FUN_10005180): On builds an N-sided prism of radius R around the origin
   (quadratic walls, or pIn 5 high, centred at pIn 6) on a hidden, unpickable entity drawn first without
   the z-buffer, with a projection widened by the distortion (the pre/post-render callbacks
   0x10006220/0x10006360, done by the renderer). Every frame it re-applies the materials and the prelit
   vertex colour and puts the sky at the camera with the orientation object's matrix; Off hides it.
   Stays active. ---- */
static int bb_tt_sky(CkContext *ctx, CkBehavior *b)
{
    if (!b->bb_state) b->bb_state = calloc(1, sizeof(SkyState));
    SkyState *st = b->bb_state;
    float vc[4] = {1, 1, 1, 0};
    bb_get_in(ctx, b, 1, vc, 16);
    uint32_t color = rgbaf_to_color(vc);
    int32_t n = 0, top = 0, bottom = 0;
    bb_get_local(ctx, b, 0, &n, 4);
    bb_get_local(ctx, b, 1, &top, 1);
    bb_get_local(ctx, b, 2, &bottom, 1);
    top &= 0xff, bottom &= 0xff;
    if (ck_input_active(ctx, b, 0) && n > 0) {
        ck_activate_input(ctx, b, 0, false);
        ck_activate_output(ctx, b, 0, true);
        float d = 0.3f, r = 70, h = 0, y = 0;
        int32_t quad = 1;
        bb_get_in(ctx, b, 0, &d, 4);
        if (d > 1) d = 1;
        CkId orient = bb_in_object(ctx, b, 2);
        bb_get_in(ctx, b, 3, &r, 4);
        bb_get_in(ctx, b, 4, &quad, 1);
        if (!(quad & 0xff)) bb_get_in(ctx, b, 5, &h, 4);
        bb_get_in(ctx, b, 6, &y, 4);
        if (st->entity) ck_destroy(ctx, st->entity);   /* the original leaks a second sky */
        CkMesh *m = (CkMesh *)ck_create(ctx, CKCID_MESH, "TT_Sky_Mesh");
        Ck3dEntity *e = (Ck3dEntity *)ck_create(ctx, CKCID_3DENTITY, "TT_Sky_Entity");
        uint32_t per_v = 4 + (top ? 3 : 0) + (bottom ? 3 : 0), per_f = 2 + (top != 0) + (bottom != 0);
        float step = 6.2831855f / (float)n, a0 = 3.9269910f, k = 0.7071f;
        if (quad & 0xff) {
            float cx = r * cosf(step), sy = r * sinf(step);
            h = sqrtf((r - cx) * (r - cx) + sy * sy);
        }
        ck_mesh_resize(m, per_v * (uint32_t)n, per_f * (uint32_t)n);
        float lo = y - 0.5f * h, hi = y + 0.5f * h;
        for (int32_t i = 0; i < n; i++) {
            float x0 = r * cosf((float)i * step + a0), z0 = r * sinf((float)i * step + a0);
            float x1 = r * cosf((float)(i + 1) * step + a0), z1 = r * sinf((float)(i + 1) * step + a0);
            CkVertex *v = &m->verts[(uint32_t)i * per_v];
            float n0 = 1 / sqrtf(x0 * x0 + z0 * z0), n1 = 1 / sqrtf(x1 * x1 + z1 * z1);
            float cu0 = 0.5f + k * (x0 * n0), cv0 = 0.5f + k * (-z0 * n0), cu1 = 0.5f + k * (x1 * n1), cv1 = 0.5f + k * (-z1 * n1);
            v[0] = (CkVertex){{x0, lo, z0}, {0, 0, 0}, 1, 1, 0, 0};
            v[1] = (CkVertex){{x1, lo, z1}, {0, 0, 0}, 0, 1, 0, 0};
            v[2] = (CkVertex){{x1, hi, z1}, {0, 0, 0}, 0, 0, 0, 0};
            v[3] = (CkVertex){{x0, hi, z0}, {0, 0, 0}, 1, 0, 0, 0};
            uint32_t kv = 4;
            if (top) {
                v[4] = (CkVertex){{x0, hi, z0}, {0, 0, 0}, cu0, cv0, 0, 0};
                v[5] = (CkVertex){{x1, hi, z1}, {0, 0, 0}, cu1, cv1, 0, 0};
                v[6] = (CkVertex){{0, hi, 0}, {0, 0, 0}, 0.5f, 0.5f, 0, 0};
                kv = 7;
            }
            if (bottom) {
                v[kv] = (CkVertex){{x0, lo, z0}, {0, 0, 0}, cu0, cv0, 0, 0};
                v[kv + 1] = (CkVertex){{0, lo, 0}, {0, 0, 0}, 0.5f, 0.5f, 0, 0};
                v[kv + 2] = (CkVertex){{x1, lo, z1}, {0, 0, 0}, cu1, cv1, 0, 0};
            }
            uint16_t bv = (uint16_t)((uint32_t)i * per_v);
            CkFace *f = &m->faces[(uint32_t)i * per_f];
            f[0] = (CkFace){{(uint16_t)(bv + 2), bv, (uint16_t)(bv + 1)}, 0};
            f[1] = (CkFace){{(uint16_t)(bv + 2), (uint16_t)(bv + 3), bv}, 0};
            uint32_t kf = 2;
            if (top) f[kf++] = (CkFace){{(uint16_t)(bv + 4), (uint16_t)(bv + 5), (uint16_t)(bv + 6)}, 0};
            if (bottom) f[kf] = (CkFace){{(uint16_t)(bv + kv), (uint16_t)(bv + kv + 1), (uint16_t)(bv + kv + 2)}, 0};
        }
        ck_mesh_build_normals(m);
        e->moveable |= VX_MOVEABLE_RENDERFIRST | VX_MOVEABLE_NOZBUFFERTEST | VX_MOVEABLE_NOZBUFFERWRITE;
        ck_level_add_object(ctx, e->be.h.id);
        e->be.h.flags &= ~(uint32_t)CK_OBJECT_VISIBLE;
        e->mesh = m->be.h.id;
        ck_ids_push(&e->meshes, m->be.h.id);
        st->distortion = d;
        st->entity = e->be.h.id;
        st->mesh = m->be.h.id;
        st->orient = orient;
        e->sky_distortion = d > 0 ? d : 1e-6f;
    }
    Ck3dEntity *e = ck_entity(ctx, st->entity);
    CkMesh *m = e ? ck_mesh(ctx, e->mesh) : NULL;
    if (!e || !m) return CKBR_OK;
    sky_face_materials(ctx, b, m, n, top != 0, bottom != 0);
    bool recolor = false;
    for (uint32_t i = 0; i < m->nverts; i++) {
        recolor |= m->verts[i].diffuse != color;
        m->verts[i].diffuse = color;
    }
    if (recolor || !(m->flags & VXMESH_PRELITMODE)) m->version++;
    m->flags |= VXMESH_PRELITMODE;
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        ck_activate_output(ctx, b, 1, true);
        e->be.h.flags &= ~(uint32_t)CK_OBJECT_VISIBLE;
        return CKBR_OK;
    }
    e->be.h.flags |= CK_OBJECT_VISIBLE;
    CkCamera *cam = ck_camera(ctx, ctx->camera);
    if (!cam) return CKBR_OK;
    Ck3dEntity *o = ck_entity(ctx, st->orient);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) e->world[i][j] = o ? o->world[i][j] : (i == j ? 1.0f : 0.0f);
    for (int j = 0; j < 3; j++) e->world[3][j] = cam->e.world[3][j];
    return CKBR_ACTIVATENEXTFRAME;
}

/* 0x10005f40: delete, detach, reset and script deactivation destroy the sky entity (not the mesh) */
static void cb_tt_sky(CkContext *ctx, CkBehavior *b, int msg)
{
    SkyState *st = b->bb_state;
    if (msg == CKM_BEHAVIORDELETE || msg == CKM_BEHAVIORDETACH || msg == CKM_BEHAVIORRESET || msg == CKM_BEHAVIORDEACTIVATESCRIPT ||
        msg == CKM_BEHAVIORATTACH || msg == CKM_BEHAVIORLOAD) {
        if (st && st->entity && msg != CKM_BEHAVIORATTACH && msg != CKM_BEHAVIORLOAD) ck_destroy(ctx, st->entity);
        if (st) memset(st, 0, sizeof *st);
    }
}

/* ---- TT SpeedOMeter 51bd5521:672c67bf (FUN_100064e0): pOut Absolute Speed = |position - last position|
   * 1000 / dt of the target (dt times setting 1 "frames" when > 1); with setting 0, pOut 1 = (speed -
   pIn 0) / (pIn 1 - pIn 0) clamped to 0..1. Local 0 keeps the position. ---- */
static int bb_tt_speedometer(CkContext *ctx, CkBehavior *b)
{
    Ck3dEntity *e = ck_entity(ctx, bb_target(ctx, b));
    if (!e) return CKBR_OK;
    int32_t norm = 0, frames = 0;
    bb_get_local(ctx, b, 1, &norm, 1);
    bb_get_local(ctx, b, 2, &frames, 4);
    float dt = ctx->delta_ms;
    if (frames > 1) dt = (float)frames * dt;
    float last[3] = {0, 0, 0}, pos[3] = {e->world[3][0], e->world[3][1], e->world[3][2]};
    bb_get_local(ctx, b, 0, last, 12);
    float dx = pos[0] - last[0], dy = pos[1] - last[1], dz = pos[2] - last[2];
    float speed = sqrtf(dz * dz + dy * dy + dx * dx) * 1000.0f / dt;
    bb_set_out(ctx, b, 0, &speed, 4);
    if (norm & 0xff) {
        float lo = 0, hi = 0;
        bb_get_in(ctx, b, 0, &lo, 4);
        bb_get_in(ctx, b, 1, &hi, 4);
        float v = (speed - lo) / (hi - lo);
        if (v > 1) v = 1;
        else if (v < 0) v = 0;
        bb_set_out(ctx, b, 1, &v, 4);
    }
    bb_set_local(ctx, b, 0, pos, 12);
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- TT_TextureSine 009c1208:3a8d779e (execute 0x10006910): wobbles the target mesh's UVs around (0.5, 0.5)
   from the reference UVs in local 0 (the base set, 8 bytes per vertex): angle 4 (i/n - 0.5) + phase *
   pIn Velocity, u += (0.5 - u) cos * pIn X Amplitude, v += (0.5 - v) sin * Y Amplitude; the phase (local 1,
   seconds) advances by dt and wraps at 2 pi / Velocity ---- */
static int bb_tt_texture_sine(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkMesh *m = ck_mesh(ctx, bb_target(ctx, b));
    CkParameter *ref = bb_local(ctx, b, 0);
    if (!m || !ref || !ref->value) return CKBR_OK;
    float xa = 0.1f, ya = 0.1f, vel = 1, phase = 0;
    bb_get_in(ctx, b, 0, &xa, 4);
    bb_get_in(ctx, b, 1, &ya, 4);
    bb_get_in(ctx, b, 2, &vel, 4);
    bb_get_local(ctx, b, 1, &phase, 4);
    const float *uv = (const float *)ref->value;
    uint32_t n = m->nverts < ref->size / 8 ? m->nverts : ref->size / 8;
    for (uint32_t i = 0; i < n; i++) {
        float ang = 4 * ((float)i / (float)m->nverts - 0.5f) + phase * vel;
        m->verts[i].u = uv[i * 2] + (0.5f - uv[i * 2]) * cosf(ang) * xa;
        m->verts[i].v = uv[i * 2 + 1] + (0.5f - uv[i * 2 + 1]) * sinf(ang) * ya;
    }
    m->version++;
    phase += ctx->delta_ms * 0.001f;
    if (phase * vel > 6.2831855f) phase -= 6.2831855f / vel;
    bb_set_local(ctx, b, 1, &phase, 4);
    return CKBR_OK;
}

/* Return values of the original error paths (bit 0 clear: the BB is not kept active) */
enum { BR_OWNERERROR = 0xa004, BR_GENERICERROR = 0xa008 };

static void log_msg(CkContext *ctx, const char *m)
{
    if (ctx->log) ctx->log(m);
}

/* CK3dEntity GetChildrenCount (+0x88) / GetChild (+0x8c): the entities parented to e, in object order */
static CkId entity_child(const CkContext *ctx, CkId e, uint32_t index)
{
    for (uint32_t i = 0; i < ctx->nobjs; i++) {
        CkObj *o = ctx->objs[i];
        if (o && ck_is_3dentity_class(o->cid) && ((Ck3dEntity *)o)->parent == e && !index--) return o->id;
    }
    return 0;
}

/* CKBeObject::GetScript(0) */
static CkId first_script(const CkContext *ctx, CkId obj)
{
    CkBeObject *be = ck_beobject(ctx, obj);
    return be && be->scripts.n ? be->scripts.v[0] : 0;
}

/* CKObject::Show(CKSHOW / CKHIDE) on the object alone */
static void show_object(CkContext *ctx, CkId id, bool on)
{
    CkObj *o = ck_obj(ctx, id);
    if (!o) return;
    if (on) o->flags = (o->flags | CK_OBJECT_VISIBLE) & ~CK_OBJECT_HIERARCHICALHIDE;
    else o->flags &= ~(CK_OBJECT_VISIBLE | CK_OBJECT_HIERARCHICALHIDE);
}

static void entity_pos(CkContext *ctx, CkId id, CkId ref, float p[3])
{
    Ck3dEntity *e = ck_entity(ctx, id);
    p[0] = p[1] = p[2] = 0;
    if (e) ck_entity_get_position(ctx, e, ref, p);
}

static void entity_set_pos(CkContext *ctx, CkId id, const float p[3], CkId ref)
{
    Ck3dEntity *e = ck_entity(ctx, id);
    if (e) ck_entity_set_position(ctx, e, p, ref, false);
}

/* ======== TT Extra ======== */

/* The small-ball list node (new(0x1c)): sprite, saved position (velocity memory), homing gain, alive */
typedef struct {
    CkId sprite;
    float saved[3];
    float gain;
    bool alive;
} ExtraNode;

/* Local 1 "Smallballlist" (a raw pointer in the original) */
typedef struct {
    ExtraNode *nodes;
    uint32_t n;
} ExtraList;

enum { EX_ACTIVE = 0, EX_LIST = 1, EX_SHADOW = 2, EX_CENTER = 3, EX_STATUS = 4, EX_INITIALIZED = 5, EX_TIMECOUNTER = 6,
       EX_BALLCOUNTER = 7, EX_TIMEVALUE = 8, EX_NEXTCHECK = 9, EX_AWAYPOS = 10, EX_EXACTNESS = 11 };

static int32_t local_int(CkContext *ctx, CkBehavior *b, uint32_t i, int32_t def)
{
    int32_t v = def;
    bb_get_local(ctx, b, i, &v, 4);
    return v;
}
static float local_float(CkContext *ctx, CkBehavior *b, uint32_t i, float def)
{
    float v = def;
    bb_get_local(ctx, b, i, &v, 4);
    return v;
}
static void set_local_int(CkContext *ctx, CkBehavior *b, uint32_t i, int32_t v) { bb_set_local(ctx, b, i, &v, 4); }
static void set_local_float(CkContext *ctx, CkBehavior *b, uint32_t i, float v) { bb_set_local(ctx, b, i, &v, 4); }
static float in_float(CkContext *ctx, CkBehavior *b, uint32_t i, float def)
{
    float v = def;
    bb_get_in(ctx, b, i, &v, 4);
    return v;
}
static int32_t in_int(CkContext *ctx, CkBehavior *b, uint32_t i, int32_t def)
{
    int32_t v = def;
    bb_get_in(ctx, b, i, &v, 4);
    return v;
}

/* 0x100019d0: the first child whose name, from character `offset` on, starts with key (a copy of strlen(key)
   characters compared with strcmp) */
static CkId extra_find(CkContext *ctx, size_t offset, const char *key, CkId entity)
{
    size_t klen = strlen(key);
    for (uint32_t i = 0;; i++) {
        CkId c = entity_child(ctx, entity, i);
        if (!c) return 0;
        const char *name = ck_obj(ctx, c)->name;
        if (name && strlen(name) >= offset && !strncmp(name + offset, key, klen) && strlen(name + offset) >= klen) return c;
    }
}

static CkId frame_script(CkContext *ctx, CkId sprite) { return first_script(ctx, entity_child(ctx, sprite, 0)); }

/* release (0x10001ec0 -> 0x10001f10): the frame scripts deactivated, the sprites hidden, the list freed */
static void extra_release(CkContext *ctx, CkBehavior *b)
{
    ExtraList *l = b->bb_state;
    if (!l) return;
    for (uint32_t i = 0; i < l->n; i++) {
        CkId s = frame_script(ctx, l->nodes[i].sprite);
        if (s) ck_scene_deactivate(ctx, s);
        show_object(ctx, l->nodes[i].sprite, false);
    }
    free(l->nodes);
    free(l);
    b->bb_state = NULL;
}

/* initialize (0x10001ac0) */
static int extra_initialize(CkContext *ctx, CkBehavior *b)
{
    int32_t status = local_int(ctx, b, EX_STATUS, -1);
    if (status == -1) {
        b->bb_state = NULL;                               /* a stale list is dropped (leaked) */
    } else if (status == 1) {
        extra_release(ctx, b);
    } else if (b->bb_state) {
        return 0;                                         /* already built */
    }
    int32_t n = in_int(ctx, b, 1, 6);
    if (n < 1) n = 1;
    set_local_int(ctx, b, EX_BALLCOUNTER, n);
    CkId t = bb_target(ctx, b);
    if (!ck_entity(ctx, t)) return BR_OWNERERROR;
    int32_t flying = in_int(ctx, b, 12, 0);
    CkId floor = extra_find(ctx, 14, "Floor", t), center = extra_find(ctx, 14, "Ball0", t);
    if (!flying && !floor) {
        log_msg(ctx, "There is no Floorglowobject in hirachy!");
        return BR_GENERICERROR;
    }
    if (!center) {
        log_msg(ctx, "There is no Centerbillboard in hirachy!");
        return BR_GENERICERROR;
    }
    set_local_int(ctx, b, EX_SHADOW, (int32_t)floor);
    set_local_int(ctx, b, EX_CENTER, (int32_t)center);
    if (floor) {
        ck_scene_deactivate(ctx, floor);
        show_object(ctx, floor, false);
    }
    CkId cs = frame_script(ctx, center);
    if (cs) {
        ck_scene_deactivate(ctx, cs);
    } else {
        char m[160];
        snprintf(m, sizeof m, "1Script not found %s", ck_obj(ctx, center)->name ? ck_obj(ctx, center)->name : "");
        log_msg(ctx, m);
    }
    show_object(ctx, center, false);
    float step = in_float(ctx, b, 9, 0.06f) / (float)n, acc = in_float(ctx, b, 7, 0.06f) - step;
    ExtraList *l = calloc(1, sizeof *l);
    l->nodes = calloc((size_t)n, sizeof *l->nodes);
    for (int32_t i = 0; i < n; i++) {
        char key[16];
        snprintf(key, sizeof key, "%d", i + 1);
        CkId s = extra_find(ctx, 18, key, t);
        if (!s) {
            log_msg(ctx, "Missing one or more Billboards.");
            return BR_GENERICERROR;                       /* the nodes built so far leak, the list isn't stored */
        }
        CkId fs = frame_script(ctx, s);
        if (fs) ck_scene_deactivate(ctx, fs);
        show_object(ctx, s, false);
        ExtraNode *nd = &l->nodes[i];
        nd->sprite = s;
        entity_pos(ctx, s, 0, nd->saved);
        nd->alive = true;
        nd->gain = (acc += step);
        if (!entity_child(ctx, s, 0)) {
            log_msg(ctx, "There is no Frame at one of the Billboards.");
            return BR_GENERICERROR;
        }
        if (!fs) {
            log_msg(ctx, "There is no Script at one of the Billboardframes.");
            return BR_GENERICERROR;
        }
        CkBehavior *sb = ck_behavior(ctx, fs);
        if (sb) ck_behavior_activate(ctx, sb, false, false);
        l->n = (uint32_t)i + 1;
    }
    b->bb_state = l;
    set_local_float(ctx, b, EX_TIMECOUNTER, in_float(ctx, b, 10, 1000.0f));
    set_local_int(ctx, b, EX_INITIALIZED, 1);
    set_local_int(ctx, b, EX_STATUS, 0);
    return 0;
}

/* show (0x10002030) */
static int extra_show(CkContext *ctx, CkBehavior *b)
{
    ExtraList *l = b->bb_state;
    if (!l) return BR_GENERICERROR;
    CkId center = (CkId)local_int(ctx, b, EX_CENTER, 0), floor = (CkId)local_int(ctx, b, EX_SHADOW, 0);
    show_object(ctx, center, true);
    if (floor && !in_int(ctx, b, 12, 0)) show_object(ctx, floor, true);
    for (uint32_t i = 0; i < l->n; i++) {
        CkId s = frame_script(ctx, l->nodes[i].sprite);
        if (s) ck_scene_activate(ctx, s, true);
        else log_msg(ctx, "8Script not found");
        show_object(ctx, l->nodes[i].sprite, true);
        if (s) ck_scene_activate(ctx, s, true);          /* (the original does it twice) */
    }
    return 0;
}

/* hide (0x10002160); Status is left as it is */
static int extra_hide(CkContext *ctx, CkBehavior *b)
{
    ExtraList *l = b->bb_state;
    if (!l) return BR_GENERICERROR;
    CkId center = (CkId)local_int(ctx, b, EX_CENTER, 0), floor = (CkId)local_int(ctx, b, EX_SHADOW, 0);
    CkId cs = frame_script(ctx, center);
    if (cs) ck_scene_deactivate(ctx, cs);
    show_object(ctx, center, false);
    if (floor && !in_int(ctx, b, 12, 0)) show_object(ctx, floor, false);
    for (uint32_t i = 0; i < l->n; i++) {
        CkId s = frame_script(ctx, l->nodes[i].sprite);
        if (s) ck_scene_deactivate(ctx, s);
        show_object(ctx, l->nodes[i].sprite, false);
    }
    return 0;
}

/* Vx3DMatrixFromRotation (the axis as given, not normalized) then Vx3DRotateVector (row vector times the 3x3) */
static void rotate_about(const float axis[3], float angle, const float p[3], float out[3])
{
    float c = cosf(angle), s = sinf(angle), t = 1 - c, x = axis[0], y = axis[1], z = axis[2];
    float m[3][3] = {{t * x * x + c, t * x * y + s * z, t * x * z - s * y},
                     {t * x * y - s * z, t * y * y + c, t * y * z + s * x},
                     {t * x * z + s * y, t * y * z - s * x, t * z * z + c}};
    for (int j = 0; j < 3; j++) out[j] = p[0] * m[0][j] + p[1] * m[1][j] + p[2] * m[2][j];
}

static float dist2(const float a[3], const float b[3])
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

/* state 0 (0x10002400): orbit around the target until the ball comes within the activation distance */
static int extra_orbit(CkContext *ctx, CkBehavior *b, ExtraList *l)
{
    CkId t = bb_target(ctx, b), ball = bb_in_object(ctx, b, 0);
    if (!ck_entity(ctx, t)) return BR_OWNERERROR;
    if (!ck_entity(ctx, ball)) return BR_GENERICERROR;
    float bp[3], tp[3];
    entity_pos(ctx, ball, 0, bp);
    entity_pos(ctx, t, 0, tp);
    float ad = in_float(ctx, b, 2, 2.0f);
    if (!(dist2(tp, bp) > ad * ad)) {                     /* x87 compare: NaN counts as inside */
        set_local_int(ctx, b, EX_STATUS, 1);
        CkId floor = (CkId)local_int(ctx, b, EX_SHADOW, 0), center = (CkId)local_int(ctx, b, EX_CENTER, 0);
        if (!in_int(ctx, b, 12, 0) && floor) show_object(ctx, floor, false);
        CkId cs = frame_script(ctx, center);
        if (cs) ck_scene_deactivate(ctx, cs);
        show_object(ctx, center, false);
        CkId f = entity_child(ctx, center, 0);
        show_object(ctx, f, true);
        if (first_script(ctx, f)) ck_scene_activate(ctx, first_script(ctx, f), true);
        for (uint32_t i = 0; i < l->n; i++) entity_pos(ctx, l->nodes[i].sprite, 0, l->nodes[i].saved);
        int32_t hits = 1;
        bb_set_out(ctx, b, 0, &hits, 4);
        ck_activate_output(ctx, b, 0, true);
        float away[3] = {bp[0], bp[1] - 2.1f, bp[2]};
        bb_set_local(ctx, b, EX_AWAYPOS, away, 12);
        return 0;
    }
    /* the axes of the jump table 0x10002854 (0.707 = 0x3f34fdf4) */
    static const float axes[6][3] = {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}, {0.5f, 0.5f, 0.70700002f}, {0.70700002f, -0.70700002f, 0},
                                     {-0.5f, -0.5f, 0.70700002f}};
    float angle = in_float(ctx, b, 4, 5.0f) * local_float(ctx, b, EX_TIMEVALUE, 0);
    for (uint32_t i = 0; i < l->n; i++) {
        float p[3], q[3];
        entity_pos(ctx, l->nodes[i].sprite, t, p);
        rotate_about(axes[i < 5 ? i : 5], angle, p, q);
        float len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
        if (len != 2.0f)
            for (int k = 0; k < 3; k++) q[k] *= 2.0f / len;   /* radius forced to 2 (MF-local units) */
        entity_set_pos(ctx, l->nodes[i].sprite, q, t);
    }
    return 0;
}

/* state 1 (0x10002870): fly away from below the ball's firing position for Flyawaytime ms (once per Initialize) */
static int extra_fly_away(CkContext *ctx, CkBehavior *b, ExtraList *l)
{
    float a[3] = {0, 0, 0};
    bb_get_local(ctx, b, EX_AWAYPOS, a, 12);
    a[1] -= 3.0f;
    float tc = local_float(ctx, b, EX_TIMECOUNTER, 0) - ctx->delta_ms;
    if (tc <= 0) {
        set_local_int(ctx, b, EX_STATUS, 2);                /* (Timecounter not stored) */
        return 0;
    }
    set_local_float(ctx, b, EX_TIMECOUNTER, tc);
    float k = in_float(ctx, b, 5, 50.0f) * local_float(ctx, b, EX_TIMEVALUE, 0), d = in_float(ctx, b, 6, 0.5f);
    for (uint32_t i = 0; i < l->n; i++) {
        ExtraNode *nd = &l->nodes[i];
        float o[3] = {nd->saved[0], nd->saved[1], nd->saved[2]}, p[3], q[3];
        entity_pos(ctx, nd->sprite, 0, p);
        memcpy(nd->saved, p, 12);
        for (int j = 0; j < 3; j++) q[j] = p[j] + d * (p[j] - o[j]) + k * (p[j] - a[j]);
        entity_set_pos(ctx, nd->sprite, q, 0);
    }
    return 0;
}

/* state 2 (0x10002a50): home in on the ball; each small ball within CollDistance (compared with the squared
   distance) is a hit and fires its hit frame's particle script */
static int extra_home_in(CkContext *ctx, CkBehavior *b, ExtraList *l)
{
    int32_t left = local_int(ctx, b, EX_BALLCOUNTER, 0);
    if (left <= 0) {
        set_local_int(ctx, b, EX_STATUS, 3);
        ck_activate_output(ctx, b, 2, true);
        return 0;
    }
    float bp[3];
    entity_pos(ctx, bb_in_object(ctx, b, 0), 0, bp);
    float damp = in_float(ctx, b, 8, 0.9f), coll = in_float(ctx, b, 3, 2.0f), tv = local_float(ctx, b, EX_TIMEVALUE, 0);
    CkObj *go = ck_obj(ctx, bb_in_object(ctx, b, 11));
    CkGroup *hitframes = go && go->cid == CKCID_GROUP ? (CkGroup *)go : NULL;
    int32_t hits = 0;
    for (uint32_t i = 0; i < l->n; i++) {
        ExtraNode *nd = &l->nodes[i];
        if (!nd->alive) continue;
        float p[3];
        entity_pos(ctx, nd->sprite, 0, p);
        if (dist2(bp, p) <= coll) {
            hits++;
            nd->alive = false;
            set_local_int(ctx, b, EX_BALLCOUNTER, --left);
            CkId s = frame_script(ctx, nd->sprite);
            if (s) ck_scene_deactivate(ctx, s);
            show_object(ctx, nd->sprite, false);
            CkId hf = hitframes && i < hitframes->members.n ? hitframes->members.v[i] : 0;
            if (ck_entity(ctx, hf)) {
                entity_pos(ctx, nd->sprite, 0, p);
                entity_set_pos(ctx, hf, p, 0);
                if (first_script(ctx, hf)) ck_scene_activate(ctx, first_script(ctx, hf), true);
            }
        } else {
            float o[3] = {nd->saved[0], nd->saved[1], nd->saved[2]}, q[3];
            memcpy(nd->saved, p, 12);
            for (int j = 0; j < 3; j++) q[j] = p[j] + (bp[j] - p[j]) * nd->gain * tv + (p[j] - o[j]) * damp;
            entity_set_pos(ctx, nd->sprite, q, 0);
        }
    }
    if (hits > 0) {
        bb_set_out(ctx, b, 0, &hits, 4);
        ck_activate_output(ctx, b, 1, true);
    }
    return 0;
}

/* ---- TT Extra 36106bd9:51813906 (execute 0x10001640, docs/gameplay_bbs.md): the extra-points pickup. The
   small balls orbit the centre billboard (state 0); when the ball comes close they burst (Activated), fly
   away for Flyawaytime (state 1), then home in on the ball, each touch a Hit (state 2), Ready when all
   are taken. Initialize builds the list from the target's hierarchy; On shows, Off hides. ---- */
static int bb_tt_extra(CkContext *ctx, CkBehavior *b)
{
    float dt = ctx->delta_ms;
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        set_local_int(ctx, b, EX_ACTIVE, 0);
        extra_hide(ctx, b);
        return CKBR_OK;
    }
    bool initialized = local_int(ctx, b, EX_INITIALIZED, 0) != 0, active;
    int32_t exact = local_int(ctx, b, EX_EXACTNESS, 1);
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        if (!initialized) {
            log_msg(ctx, "Extra is'nt intialized yet, this could slow all down.");
            if (extra_initialize(ctx, b)) {
                log_msg(ctx, "Could'nt intialize the Extra.");
                return BR_GENERICERROR;
            }
            initialized = true;
        }
        set_local_int(ctx, b, EX_ACTIVE, 1);
        set_local_float(ctx, b, EX_TIMEVALUE, dt * 0.001f);
        if (exact) set_local_int(ctx, b, EX_NEXTCHECK, in_int(ctx, b, 13, 2));
        if (extra_show(ctx, b)) {
            log_msg(ctx, "Could'nt activate the Extra.");
            return BR_GENERICERROR;
        }
        active = true;
    } else if (ck_input_active(ctx, b, 2)) {
        ck_activate_input(ctx, b, 2, false);
        if (initialized) {
            active = false;
        } else {
            if (!extra_initialize(ctx, b)) return CKBR_OK;
            log_msg(ctx, "Could'nt intialize the Extra.");
            return BR_GENERICERROR;
        }
    } else {
        active = local_int(ctx, b, EX_ACTIVE, 0) != 0;
    }
    if (active && initialized) {
        if (exact) {
            /* the frame time sampled once every Framedelay frames and held in between */
            int32_t next = local_int(ctx, b, EX_NEXTCHECK, 0) - 1;
            if (next == 0) {
                set_local_float(ctx, b, EX_TIMEVALUE, dt * 0.001f);
                next = in_int(ctx, b, 13, 2);
            }
            set_local_int(ctx, b, EX_NEXTCHECK, next);
        }
        ExtraList *l = b->bb_state;
        int r = 0;
        switch (local_int(ctx, b, EX_STATUS, -1)) {
        case 0: r = l ? extra_orbit(ctx, b, l) : 0; break;
        case 1: r = l ? extra_fly_away(ctx, b, l) : 0; break;
        case 2: r = l ? extra_home_in(ctx, b, l) : 0; break;
        default: break;                                   /* state 3: nothing */
        }
        if (r) {
            log_msg(ctx, "Could'nt execute the Extra.");
            return BR_GENERICERROR;
        }
    }
    return CKBR_ACTIVATENEXTFRAME;
}

/* 0x10001890: delete, detach, reset and script deactivation release the list; attach and load clear it */
static void cb_tt_extra(CkContext *ctx, CkBehavior *b, int msg)
{
    if (msg == CKM_BEHAVIORDELETE || msg == CKM_BEHAVIORDETACH || msg == CKM_BEHAVIORRESET || msg == CKM_BEHAVIORDEACTIVATESCRIPT) {
        if (local_int(ctx, b, EX_INITIALIZED, 0)) {
            extra_release(ctx, b);
            set_local_int(ctx, b, EX_INITIALIZED, 0);
            set_local_int(ctx, b, EX_STATUS, -1);
        }
    } else if (msg == CKM_BEHAVIORATTACH || msg == CKM_BEHAVIORLOAD) {
        b->bb_state = NULL;
        set_local_int(ctx, b, EX_INITIALIZED, 0);
        set_local_int(ctx, b, EX_ACTIVE, 0);
    }
}

/* ======== TT Simple Shadow ======== */

enum { SHADOW_MAX_RECEIVERS = 50 };

/* Local 0 "Data" (new(0xd8) in the original) */
typedef struct {
    bool valid;
    CkId mat, tex;
    float scale;
    CkId receivers[SHADOW_MAX_RECEIVERS];
    uint32_t count;
} ShadowData;

static const char SHADOW_MATERIAL[] = "TT_SimpleShadow Material";

static CkMesh *entity_mesh(CkContext *ctx, CkId id)
{
    Ck3dEntity *e = ck_entity(ctx, id);
    return e ? ck_mesh(ctx, e->mesh) : NULL;
}

/* remove channels (0x10004eb0): the channel of mat from every receiver's current mesh */
static void shadow_remove_channels(CkContext *ctx, const ShadowData *d, CkId mat)
{
    for (uint32_t i = 0; i < d->count; i++) {
        CkMesh *m = entity_mesh(ctx, d->receivers[i]);
        if (m) ck_mesh_remove_channel(m, ck_mesh_channel_by_material(m, mat));
    }
}

/* receiver search (0x10004c10): the visible Floor-attribute objects whose world box overlaps the ball's in
   XZ, whose top is at most H below the ball's bottom, and whose local box meets the shadow square */
static void shadow_receivers(CkContext *ctx, ShadowData *d, const Ck3dEntity *t, float pos[][3], float scl[][3], float h)
{
    float tb[6];
    ck_entity_world_box(ctx, t, tb);
    float m = 1.0f / ((tb[3] - tb[0]) * d->scale);
    const CkAttributeType *at = ck_attribute_info(ctx, ck_attribute_type(ctx, "Floor"));
    uint32_t count = 0;
    for (uint32_t i = 0; at && i < at->objects.n; i++) {
        CkObj *oo = ck_obj(ctx, at->objects.v[i]);
        if (!oo || !ck_is_3dentity_class(oo->cid) || !(oo->flags & CK_OBJECT_VISIBLE)) continue;
        /* (IsAllOutsideFrustrum, last frame's culling result, is not tracked: every visible floor counts) */
        const Ck3dEntity *o = (const Ck3dEntity *)oo;
        float fb[6];
        ck_entity_world_box(ctx, o, fb);
        if (!(fb[0] <= tb[3] && fb[2] <= tb[5] && fb[3] >= tb[0] && fb[5] >= tb[2])) continue;
        if (!(fb[1] <= tb[4] && tb[1] - fb[4] <= h)) continue;
        float p[3], l[6], s[3];
        ck_entity_get_position(ctx, t, oo->id, p);
        ck_entity_local_box(ctx, o, l);
        ck_entity_local_scale(ctx, o, s);
        float sx = s[0] * m, sz = s[2] * m;
        if (!((l[3] - p[0]) * sx >= -0.5f && (l[0] - p[0]) * sx <= 0.5f)) continue;
        if (!((l[5] - p[2]) * sz >= -0.5f && (l[2] - p[2]) * sz <= 0.5f)) continue;
        if (count >= SHADOW_MAX_RECEIVERS) continue;
        memcpy(pos[count], p, 12);
        memcpy(scl[count], s, 12);
        d->receivers[count++] = oo->id;
    }
    d->count = count;
}

/* the pre-render callback (0x10004780): the receivers' shadow channel UVs, a square of ball width * Size Scale
   centred under the ball, projected along the receiver's local Y */
static void shadow_pre_render(CkContext *ctx, CkBehavior *b)
{
    ShadowData *d = b->bb_state;
    Ck3dEntity *t = ck_entity(ctx, bb_target(ctx, b));
    if (!d || !d->valid || !t) return;
    CkMaterial *mat = ck_material(ctx, d->mat);
    CkId old[SHADOW_MAX_RECEIVERS];
    uint32_t nold = d->count;
    memcpy(old, d->receivers, nold * sizeof *old);
    CkId tex = bb_in_object(ctx, b, 0);
    if (tex != d->tex) {
        d->tex = tex;
        if (mat) mat->texture = ck_obj(ctx, tex) ? tex : 0;
    }
    d->scale = in_float(ctx, b, 1, 2.0f);
    float h = in_float(ctx, b, 2, 100.0f);
    float lt[6];
    ck_entity_local_box(ctx, t, lt);
    float row0 = sqrtf(t->world[0][0] * t->world[0][0] + t->world[0][1] * t->world[0][1] + t->world[0][2] * t->world[0][2]);
    float k = 1.0f / ((lt[3] - lt[0]) * row0 * d->scale);
    float pos[SHADOW_MAX_RECEIVERS][3], scl[SHADOW_MAX_RECEIVERS][3];
    shadow_receivers(ctx, d, t, pos, scl, h);
    for (uint32_t i = 0; i < nold; i++) {                 /* left the shadow */
        bool still = false;
        for (uint32_t j = 0; j < d->count && !still; j++) still = d->receivers[j] == old[i];
        CkMesh *m = still ? NULL : entity_mesh(ctx, old[i]);
        if (m) ck_mesh_remove_channel(m, ck_mesh_channel_by_material(m, d->mat));
    }
    for (uint32_t i = 0; i < d->count; i++) {
        CkMesh *m = entity_mesh(ctx, d->receivers[i]);
        if (!m) continue;
        int32_t c = ck_mesh_channel_by_material(m, d->mat);
        if (c < 0) {
            c = ck_mesh_add_channel(m, d->mat, false);
            m->channels[c].src_blend = VXBLEND_ZERO;
            m->channels[c].dst_blend = VXBLEND_SRCCOLOR;
        }
        float *uv = ck_mesh_channel_uv(m, c);
        float a = k * scl[i][0], bz = k * scl[i][2];
        for (uint32_t v = 0; v < m->nverts; v++) {
            uv[v * 2] = 0.5f + a * (pos[i][0] - m->verts[v].pos.x);
            uv[v * 2 + 1] = 0.5f + bz * (pos[i][2] - m->verts[v].pos.z);
        }
        m->channels[c].version++;                         /* UVChanged */
    }
}

/* ---- TT Simple Shadow 7f0517c2:52cc76bb (execute 0x10004450, docs/gameplay_bbs.md): the ball's blob
   shadow as an extra material channel on the Floor objects under it (src ZERO, dst SRCCOLOR: the floor
   times the shadow texture). On (re)creates the material, Off removes the channels and destroys it; the
   pre-render callback is re-armed every frame while active. ---- */
static int bb_tt_simple_shadow(CkContext *ctx, CkBehavior *b)
{
    if (!ck_entity(ctx, bb_target(ctx, b))) return BR_OWNERERROR;
    ShadowData *d = b->bb_state;
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        ck_activate_output(ctx, b, 1, true);
        if (d && d->valid) {
            CkId mat = ck_find(ctx, SHADOW_MATERIAL, CKCID_MATERIAL);
            if (!ck_material(ctx, d->mat)) return BR_GENERICERROR;
            if (mat == d->mat) {
                shadow_remove_channels(ctx, d, mat);
                ck_material(ctx, mat)->texture = 0;
                ck_destroy(ctx, mat);
            }
            d->valid = false;                             /* Data = NULL (the block leaks) */
        }
        return CKBR_OK;
    }
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        ck_activate_output(ctx, b, 0, true);
        CkId old = ck_find(ctx, SHADOW_MATERIAL, CKCID_MATERIAL);
        if (old) {
            ck_material(ctx, old)->texture = 0;
            ck_destroy(ctx, old);
        }
        /* (created dynamic when the behavior is; the runtime makes no difference) */
        CkMaterial *m = (CkMaterial *)ck_create(ctx, CKCID_MATERIAL, SHADOW_MATERIAL);
        m->emissive = (CkColor){1, 1, 1, 1};
        m->diffuse = (CkColor){1, 1, 1, 1};
        m->specular = (CkColor){0, 0, 0, 1};
        m->ambient = (CkColor){0, 0, 0, 1};
        m->address = 3;                                   /* VXTEXTURE_ADDRESSCLAMP */
        m->texture_blend = 7;                             /* VXTEXTUREBLEND_COPY */
        if (!d) d = b->bb_state = calloc(1, sizeof *d);
        /* the old receivers' channels are removed with the NEW material: nothing is found (original bug) */
        if (d->valid) shadow_remove_channels(ctx, d, m->be.h.id);
        memset(d, 0, sizeof *d);
        *d = (ShadowData){true, m->be.h.id, (CkId)-1, 2.0f, {0}, 0};
    }
    ck_add_pre_render(ctx, shadow_pre_render, b->h.id);
    return CKBR_ACTIVATENEXTFRAME;
}

/* 0x10004bd0: pause / resume remove / re-add the pre-render callback */
static void cb_tt_simple_shadow(CkContext *ctx, CkBehavior *b, int msg)
{
    if (msg == CKM_BEHAVIORPAUSE) ck_remove_pre_render(ctx, shadow_pre_render, b->h.id);
    else if (msg == CKM_BEHAVIORRESUME) ck_add_pre_render(ctx, shadow_pre_render, b->h.id);
}

BB_DECL_CB(d_tt_sky, 36691920, 3b261630, "TT Sky", bb_tt_sky, cb_tt_sky);

BB_DECL(d_tt_speedometer, 51bd5521, 672c67bf, "TT SpeedOMeter", bb_tt_speedometer);

BB_DECL(d_tt_texture_sine, 009c1208, 3a8d779e, "TT_TextureSine", bb_tt_texture_sine);

BB_DECL_CB(d_tt_extra, 36106bd9, 51813906, "TT Extra", bb_tt_extra, cb_tt_extra);

BB_DECL_CB(d_tt_simple_shadow, 7f0517c2, 52cc76bb, "TT Simple Shadow", bb_tt_simple_shadow, cb_tt_simple_shadow);

const CkBBDecl *const bb_tt_gravity[] = {&d_tt_sky, &d_tt_speedometer, &d_tt_texture_sine, &d_tt_extra,
                                         &d_tt_simple_shadow};
const unsigned bb_tt_gravity_count = sizeof bb_tt_gravity / sizeof *bb_tt_gravity;
