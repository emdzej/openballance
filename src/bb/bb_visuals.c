/* Visuals.dll, Materials.dll, Cameras.dll and Lights.dll Building Blocks (visibility, 2D materials, material
   properties, textures, the viewpoint, light colours). */
#include "bb.h"
#include "../ck/ck_3d.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* CKObject::Show (vtable +0): CKGroup's shows or hides each object in the group as well */
static void show_object(CkContext *ctx, CkObj *o, bool on, int depth)
{
    if (on) o->flags = (o->flags | CK_OBJECT_VISIBLE) & ~CK_OBJECT_HIERARCHICALHIDE;
    else o->flags &= ~(CK_OBJECT_VISIBLE | CK_OBJECT_HIERARCHICALHIDE);
    if (o->cid != CKCID_GROUP || depth > 8) return;
    CkGroup *g = (CkGroup *)o;
    for (uint32_t i = 0; i < g->members.n; i++) {
        CkObj *m = ck_obj(ctx, g->members.v[i]);
        if (m && m != o) show_object(ctx, m, on, depth + 1);
    }
}

/* FUN_25781110 (Hierarchy): Show, then the same for the children of a 3D entity (+0x88 / +0x8c) or of a 2D
   entity (class 0x1b: +0xec / +0xf0), recursively */
static void show(CkContext *ctx, CkId id, bool on, bool hierarchy)
{
    CkObj *o = ck_obj(ctx, id);
    if (!o) return;
    show_object(ctx, o, on, 0);
    if (!hierarchy) return;
    bool e3 = ck_is_3dentity_class(o->cid), e2 = ck_is_2dentity_class(o->cid);
    if (!e3 && !e2) return;
    for (uint32_t i = 0; i < ctx->nobjs; i++) {
        CkObj *c = ctx->objs[i];
        if (!c || c == o) continue;
        if (e3 && ck_is_3dentity_class(c->cid) && ((Ck3dEntity *)c)->parent == id) show(ctx, c->id, on, true);
        if (e2 && ck_is_2dentity_class(c->cid) && ((Ck2dEntity *)c)->parent == id) show(ctx, c->id, on, true);
    }
}

/* ---- Hide 31d97d82:78d54d98 (FUN_257811b0) / Show a85a213a:ef78d52a (FUN_257815c0): pIn Hierarchy ---- */
static int bb_show_hide(CkContext *ctx, CkBehavior *b, bool on)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    int32_t hierarchy = 0;
    bb_get_in(ctx, b, 0, &hierarchy, 4);
    CkId t = bb_target(ctx, b);
    if (t) show(ctx, t, on, hierarchy != 0);
    return CKBR_OK;
}
static int bb_hide(CkContext *ctx, CkBehavior *b) { return bb_show_hide(ctx, b, false); }
static int bb_show(CkContext *ctx, CkBehavior *b) { return bb_show_hide(ctx, b, true); }

/* ---- Set Diffuse e1e1e1e1:1e1e1e1e (Materials FUN_25501c00): pIn Diffuse Color, Keep Alpha. Version 1
   blocks also switched blending on for alpha < 1; the game's are version 2. ---- */
static int bb_set_diffuse(CkContext *ctx, CkBehavior *b)
{
    CkMaterial *m = ck_material(ctx, bb_target(ctx, b));
    if (!m) return CKBR_OK;
    CkColor c = {1, 1, 1, 1};
    int32_t keep_alpha = 0;
    bb_get_in(ctx, b, 0, &c, 16);
    bb_get_in(ctx, b, 1, &keep_alpha, 4);
    if (keep_alpha) {
        c.a = m->diffuse.a;
    } else if (b->proto_version < 0x20000) {
        if (c.a < 1.0f) {
            m->flags |= CKMAT_ALPHABLEND;
            m->src_blend = VXBLEND_SRCALPHA;
            m->dst_blend = VXBLEND_INVSRCALPHA;
        }
        if (c.a == 1.0f) m->flags &= ~CKMAT_ALPHABLEND;
    }
    m->diffuse = c;
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Movie Load 798e26a9:34e4169e (BuildingBlocksAddons1 FUN_2510f920): the target texture plays a movie
   file (CKTexture::LoadMovie); outputs Loaded, Error ---- */
static int bb_movie_load(CkContext *ctx, CkBehavior *b)
{
    CkTexture *t = ck_texture(ctx, bb_target(ctx, b));
    if (!t) {
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    ck_activate_input(ctx, b, 0, false);
    const char *file = bb_in_string(ctx, b, 0);
    char path[512];
    bool ok = false;
    if (file) {
        ok = ck_texture_load_movie(t, file);
        if (!ok) {   /* CKPathManager: bitmap paths */
            snprintf(path, sizeof path, "Textures/%s", file);
            ok = ck_texture_load_movie(t, path);
        }
    }
    if (ok) {
        CkId id = t->be.h.id;
        bb_set_out(ctx, b, 0, &id, 4);
    } else if (ctx->log) {
        snprintf(path, sizeof path, "Movie Load: can't load %s", file ? file : "(null)");
        ctx->log(path);
    }
    ck_activate_output(ctx, b, ok ? 0 : 1, true);
    return CKBR_OK;
}

/* ---- Movie Player 778d16d4:1dd60060 (Materials FUN_25504630): inputs On, Off; outputs Synchro Out (each
   slot change), One Loop Played; pIn Duration (Time, or Integer frames), Loop, Starting Slot, Ending
   Slot; pOut Current Slot; local 0 elapsed time ---- */
static int bb_movie_player(CkContext *ctx, CkBehavior *b)
{
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        return CKBR_OK;
    }
    CkTexture *t = ck_texture(ctx, bb_target(ctx, b));
    if (!t) return CKBR_OK;
    float duration = 5000;
    CkParameter *p0 = b->pin.n ? ck_param(ctx, b->pin.v[0]) : NULL;
    if (p0 && ck_guid_eq(p0->type, CKPGUID_INT)) {
        int32_t frames = 5;
        bb_get_in(ctx, b, 0, &frames, 4);
        duration = (float)frames;
    } else {
        bb_get_in(ctx, b, 0, &duration, 4);
    }
    if (duration == 0) return CKBR_OK;
    int32_t count = (int32_t)ck_texture_slot_count(t);
    if (!count) return CKBR_OK;
    int32_t first = 0, last = 0;
    bb_get_in(ctx, b, 2, &first, 4);
    if (first < 0) first = 0;
    if (count - 1 < first) first = count;
    bb_get_in(ctx, b, 3, &last, 4);
    if (last < 0) last = 0;
    if (count - 1 < last) last = count;
    int32_t span = first == 0 && last == 0 ? count : abs(last - first);
    float rate = (float)span / duration;
    float elapsed;
    int32_t before;
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        before = -1;
        elapsed = 0;
    } else {
        elapsed = 0;
        bb_get_local(ctx, b, 0, &elapsed, 4);
        before = (int32_t)(elapsed * rate);
        elapsed += ctx->delta_ms;
    }
    if (duration <= elapsed) {
        ck_activate_output(ctx, b, 1, true);
        int32_t loop = 1;
        bb_get_in(ctx, b, 1, &loop, 4);
        if (!loop) return CKBR_OK;
        while (duration <= elapsed) elapsed -= duration;
    }
    bb_set_local(ctx, b, 0, &elapsed, 4);
    int32_t now = (int32_t)(elapsed * rate);
    if (now != before) {
        int32_t slot = last < first ? first - now - 1 : first + now;
        ck_texture_set_slot(t, slot);
        bb_set_out(ctx, b, 0, &slot, 4);
        ck_activate_output(ctx, b, 0, true);
    }
    return CKBR_ACTIVATENEXTFRAME;
}

/* ---- Set 2D Material 3f7e5bff:02326a71 (FUN_2578e160): CK2dEntity::SetMaterial(pIn Material) ---- */
static int bb_set_2d_material(CkContext *ctx, CkBehavior *b)
{
    Ck2dEntity *e = ck_2dentity(ctx, bb_target(ctx, b));
    if (!e) return CKBR_OK;
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkId m = bb_in_object(ctx, b, 0);
    e->material = ck_material(ctx, m) ? m : 0;
    return CKBR_OK;
}

/* ---- TT Set_2DSprite 3c392558:419d2680 (TT_Toolbox_RT FUN_100254c0): the setting (bits 4 Position,
   2 Size, 1 UV-Rect) says which inputs exist; missing ones keep the entity's homogeneous position / size.
   The UV rect becomes the source rect (not switched on), then the entity is made homogeneous with the
   rectangle (x, y, x + w, y + h). Calls to the CK2dEntity vtable: 0x88 GetPosition, 0x90 GetSize,
   0xa8 SetSourceRect, 0x100 SetHomogeneousCoordinates, 0xa0 SetHomogeneousRect. ---- */
static int bb_tt_set_2dsprite(CkContext *ctx, CkBehavior *b)
{
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        ck_activate_output(ctx, b, 0, true);
    }
    Ck2dEntity *e = ck_2dentity(ctx, bb_target(ctx, b));
    if (!e) return CKBR_OK;
    int32_t set = 0;
    bb_get_local(ctx, b, 0, &set, 4);
    float r[4];
    ck_2d_get_homogeneous_rect(e, r);
    float pos[2] = {r[0], r[1]}, size[2] = {r[2] - r[0], r[3] - r[1]};
    uint32_t in = 0;
    if (set >= 4) {
        set -= 4;
        bb_get_in(ctx, b, in++, pos, 8);
    }
    if (set >= 2) {
        set -= 2;
        bb_get_in(ctx, b, in++, size, 8);
    }
    if (set > 0) {
        float uv[4] = {0, 0, 0, 0};
        bb_get_in(ctx, b, in, uv, 16);
        memcpy(e->src, uv, sizeof uv);
    }
    e->flags |= CK2D_HOMOGENEOUS;
    e->rect[0] = pos[0], e->rect[1] = pos[1], e->rect[2] = pos[0] + size[0], e->rect[3] = pos[1] + size[1];
    return CKBR_OK;
}

/* CKMaterial vtable slots used below (Virtools SDK 2.1 order, anchored by SetEmissive +0x78 and
   SetTexture0 +0x84): +0x88 SetTextureBlendMode, +0xa0 SetTextureAddressMode, +0xb0 / +0xb4 SetSource /
   DestBlend, +0xcc EnableZWrite, +0xd4 EnableAlphaBlend, +0xdc SetZFunc, +0xe4 EnablePerspectiveCorrection,
   +0x104 EnableAlphaTest, +0x10c SetAlphaFunc, +0x114 SetAlphaRef. */

/* ---- Set Texture eb123eb5:5be321be (Materials FUN_25502ff0): the target material's texture from pIn 0
   when it has a source, then (when readable) pIn 1 perspective correction (+0xe4) and pIn 2 the address
   mode (+0xa0) ---- */
static int bb_set_texture(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkMaterial *m = ck_material(ctx, bb_target(ctx, b));
    if (!m) return CKBR_OK;
    if (bb_in(ctx, b, 0)) {
        CkId t = bb_in_object(ctx, b, 0);
        m->texture = ck_texture(ctx, t) ? t : 0;
    }
    int32_t v = 1;
    if (bb_get_in(ctx, b, 1, &v, 4)) m->flags = v ? (m->flags | CKMAT_PERSPECTIVE) : (m->flags & ~CKMAT_PERSPECTIVE);
    v = 1;
    if (bb_get_in(ctx, b, 2, &v, 4)) m->address = (uint8_t)v;
    return CKBR_OK;
}

/* ---- Change Texture Video Format f56d92ef:41eb20c1 (Materials FUN_25505350): converts the target texture's
   video memory copy to pIn 0's pixel format; Success when it already has it or the device supports it.
   Our textures are always RGBA8 on the GPU, so it always succeeds. ---- */
static int bb_change_texture_video_format(CkContext *ctx, CkBehavior *b)
{
    if (!ck_input_active(ctx, b, 0)) {
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, ck_texture(ctx, bb_target(ctx, b)) ? 0 : 1, true);
    return CKBR_OK;
}

/* ---- Set As Active Camera 368f0ab1:2d8957e4 (Cameras FUN_25181d90): AttachViewpointToCamera(target) ---- */
static int bb_set_as_active_camera(CkContext *ctx, CkBehavior *b)
{
    CkId t = bb_target(ctx, b);
    if (!ck_obj(ctx, t)) return CKBR_OK;
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    if (ck_camera(ctx, t)) ctx->camera = t;
    return CKBR_OK;
}

/* ---- Set Light Color 32115590:78951230 (Lights FUN_25401110): CKLight::SetColor(pIn 0) ---- */
static int bb_set_light_color(CkContext *ctx, CkBehavior *b)
{
    CkLight *l = ck_light(ctx, bb_target(ctx, b));
    if (!l) return CKBR_OK;
    float c[4] = {0, 0, 0, 0};
    bb_get_in(ctx, b, 0, c, 16);
    l->color = (CkColor){c[0], c[1], c[2], c[3]};
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Set Emissive d1d1d1d1:30303030 (Materials FUN_25501e30): CKMaterial::SetEmissive(pIn 0) ---- */
static int bb_set_emissive(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkMaterial *m = ck_material(ctx, bb_target(ctx, b));
    if (!m) return CKBR_OK;
    float c[4] = {0, 0, 0, 1};
    bb_get_in(ctx, b, 0, c, 16);
    m->emissive = (CkColor){c[0], c[1], c[2], c[3]};
    return CKBR_OK;
}

/* ---- TT_ReflectionMapping 7e212b2f:24db67c6 (TT_Toolbox_RT FUN_10016600): sphere-map UVs on the target's
   current mesh from pIn Camera's position in the target's frame: per vertex v = normalize(camera - p),
   r = normalize(2 (v.n) n - v), uv = ((r.x + 1) / 2, (r.z + 1) / 2). The base UVs are written
   (GetTextureCoordinatesPtr(-1); pIn MatChannel is read but unused). Stays active; Off stops. ---- */
static int bb_tt_reflection_mapping(CkContext *ctx, CkBehavior *b)
{
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        ck_activate_output(ctx, b, 0, true);
    }
    Ck3dEntity *e = ck_entity(ctx, bb_target(ctx, b));
    if (!e) return CKBR_OK;
    Ck3dEntity *cam = ck_entity(ctx, bb_in_object(ctx, b, 1));
    CkMesh *m = ck_mesh(ctx, e->mesh);
    if (!m || !cam) return CKBR_OK;
    /* the camera position in the target's frame */
    float w[3] = {cam->world[3][0] - e->world[3][0], cam->world[3][1] - e->world[3][1], cam->world[3][2] - e->world[3][2]}, c[3];
    const float (*a)[4] = e->world;
    float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (det == 0) return CKBR_ACTIVATENEXTFRAME;
    /* solve c * A = w (row vector times the axes) by Cramer's rule */
    for (int k = 0; k < 3; k++) {
        float m3[3][3];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) m3[i][j] = i == k ? w[j] : a[i][j];
        c[k] = (m3[0][0] * (m3[1][1] * m3[2][2] - m3[1][2] * m3[2][1]) - m3[0][1] * (m3[1][0] * m3[2][2] - m3[1][2] * m3[2][0]) +
                m3[0][2] * (m3[1][0] * m3[2][1] - m3[1][1] * m3[2][0])) / det;
    }
    for (uint32_t i = 0; i < m->nverts; i++) {
        CkVertex *v = &m->verts[i];
        float d[3] = {c[0] - v->pos.x, c[1] - v->pos.y, c[2] - v->pos.z};
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l > 0) d[0] /= l, d[1] /= l, d[2] /= l;
        float n[3] = {v->normal.x, v->normal.y, v->normal.z};
        float k = 2 * (d[2] * n[2] + d[0] * n[0] + d[1] * n[1]);
        float r[3] = {k * n[0] - d[0], k * n[1] - d[1], k * n[2] - d[2]};
        l = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (l > 0) r[0] /= l, r[2] /= l;
        v->u = (r[0] + 1) * 0.5f;
        v->v = (r[2] + 1) * 0.5f;
    }
    m->version++;
    return CKBR_ACTIVATENEXTFRAME;
}

/* ---- Set Blend Modes 2f572a66:6a9a0088 (Materials FUN_25501680): each readable input: pIn 0 alpha blending,
   1 source blend (5), 2 dest blend (6), 3 texture blend mode (2) ---- */
static int bb_set_blend_modes(CkContext *ctx, CkBehavior *b)
{
    CkMaterial *m = ck_material(ctx, bb_target(ctx, b));
    if (!m) return CKBR_OK;
    int32_t v = 1;
    if (bb_get_in(ctx, b, 0, &v, 4)) m->flags = v ? (m->flags | CKMAT_ALPHABLEND) : (m->flags & ~CKMAT_ALPHABLEND);
    v = 5;
    if (bb_get_in(ctx, b, 1, &v, 4)) m->src_blend = (uint8_t)v;
    v = 6;
    if (bb_get_in(ctx, b, 2, &v, 4)) m->dst_blend = (uint8_t)v;
    v = 2;
    if (bb_get_in(ctx, b, 3, &v, 4)) m->texture_blend = (uint8_t)v;
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Set Alpha Test 1cb5661e:5b5d3fda (Materials FUN_255012c0): readable inputs: pIn 0 alpha test, 1 function
   (7 greater-equal), 2 reference (1) ---- */
static int bb_set_alpha_test(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkMaterial *m = ck_material(ctx, bb_target(ctx, b));
    if (!m) return CKBR_OK;
    int32_t v = 1;
    if (bb_get_in(ctx, b, 0, &v, 4)) m->flags = v ? (m->flags | CKMAT_ALPHATEST) : (m->flags & ~CKMAT_ALPHATEST);
    v = 7;
    if (bb_get_in(ctx, b, 1, &v, 4)) m->alpha_func = (uint8_t)v;
    v = 1;
    if (bb_get_in(ctx, b, 2, &v, 4)) m->alpha_ref = (uint8_t)v;
    return CKBR_OK;
}

/* ---- Set Material Z Buffer 1144022f:68fd055c (Materials FUN_25503940): pIn 0 z write, pIn 1 z compare (4) ---- */
static int bb_set_material_z_buffer(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkMaterial *m = ck_material(ctx, bb_target(ctx, b));
    if (!m) return CKBR_OK;
    int32_t write = 1, func = 4;
    bb_get_in(ctx, b, 0, &write, 4);
    bb_get_in(ctx, b, 1, &func, 4);
    m->flags = write ? (m->flags | CKMAT_ZWRITE) : (m->flags & ~CKMAT_ZWRITE);
    m->z_func = (uint8_t)func;
    return CKBR_OK;
}

/* ---- Set Prelit Color 60415d44:0d0174ea (Materials FUN_25502770): the target's current mesh becomes prelit
   (SetLitMode 0) with every vertex coloured pIn 0 (default 1, 1, 1, 0.5) and specular pIn 1 (3D sprites:
   their material's diffuse and specular instead; the game has none) ---- */
static int bb_set_prelit_color(CkContext *ctx, CkBehavior *b)
{
    Ck3dEntity *e = ck_entity(ctx, bb_target(ctx, b));
    CkMesh *m = e ? ck_mesh(ctx, e->mesh) : NULL;
    if (!m) return CKBR_OK;
    float c[4] = {1, 1, 1, 0.5f}, sp[4] = {0, 0, 0, 0};
    bb_get_in(ctx, b, 0, c, 16);
    bb_get_in(ctx, b, 1, sp, 16);
    uint32_t col = 0, spec = 0;
    for (int i = 0; i < 4; i++) {
        col |= ((uint32_t)(int32_t)(c[i] * 255.0f) & 0xff) << (i == 3 ? 24 : 16 - 8 * i);
        spec |= ((uint32_t)(int32_t)(sp[i] * 255.0f) & 0xff) << (i == 3 ? 24 : 16 - 8 * i);
    }
    m->flags |= VXMESH_PRELITMODE;
    for (uint32_t i = 0; i < m->nverts; i++) m->verts[i].diffuse = col, m->verts[i].specular = spec;
    m->version++;
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Texture Scroller f11d010a:fb1d010a (Materials execute 0x255049f0): the target mesh's base UVs (pIn
   Channel -1; the game has no others) shifted by pIn Scroll Vector, less the integer part of vertex 0's
   coordinates so they stay near 0 ---- */
static int bb_texture_scroller(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkMesh *m = ck_mesh(ctx, bb_target(ctx, b));
    if (!m || !m->nverts) return CKBR_OK;
    float sv[2] = {0, 0};
    bb_get_in(ctx, b, 0, sv, 8);
    sv[0] -= (float)(int32_t)m->verts[0].u;
    sv[1] -= (float)(int32_t)m->verts[0].v;
    for (uint32_t i = 0; i < m->nverts; i++) m->verts[i].u += sv[0], m->verts[i].v += sv[1];
    m->version++;
    return CKBR_OK;
}

/* ---- Set Current Slot aaaa213a:eaa8d52a (Materials execute 0x25501110): CKBitmapData::SetCurrentSlot ---- */
static int bb_set_current_slot(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkTexture *t = ck_texture(ctx, bb_target(ctx, b));
    if (!t) return CKBR_OK;
    int32_t slot = 0;
    bb_get_in(ctx, b, 0, &slot, 4);
    if (t->movie) {
        ck_texture_set_slot(t, slot);
    } else if (slot != t->current_slot) {
        t->current_slot = slot;
        t->version++;
    }
    return CKBR_OK;
}

/* ---- Planar Filter 00cd320b:32ed010b (Visuals execute 0x25787b50, render callback 0x25787cd0): On registers
   a post-sprite render callback on the render context drawing a full-viewport quad of pIn Filtering Color
   (+ Additional Color) times pIn Texture, blended pIn Source / Dest Blend (the renderer does it); Off
   removes it. Stays active until Off. ---- */
static int bb_planar_filter(CkContext *ctx, CkBehavior *b)
{
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        ck_activate_output(ctx, b, 1, true);
        for (uint32_t i = 0; i < ctx->planar_filters.n; i++)
            if (ctx->planar_filters.v[i] == b->h.id) ctx->planar_filters.v[i--] = ctx->planar_filters.v[--ctx->planar_filters.n];
        return CKBR_OK;
    }
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        ck_activate_output(ctx, b, 0, true);
        if (!ck_ids_has(&ctx->planar_filters, b->h.id)) ck_ids_push(&ctx->planar_filters, b->h.id);
    }
    return CKBR_ACTIVATENEXTFRAME;
}

static void cb_planar_filter(CkContext *ctx, CkBehavior *b, int msg)
{
    if (msg == CKM_BEHAVIORDETACH || msg == CKM_BEHAVIORPAUSE || msg == CKM_BEHAVIORRESET || msg == CKM_BEHAVIORDEACTIVATESCRIPT)
        for (uint32_t i = 0; i < ctx->planar_filters.n; i++)
            if (ctx->planar_filters.v[i] == b->h.id) ctx->planar_filters.v[i--] = ctx->planar_filters.v[--ctx->planar_filters.n];
}

BB_DECL(d_hide, 31d97d82, 78d54d98, "Hide", bb_hide);
BB_DECL(d_show, a85a213a, ef78d52a, "Show", bb_show);
BB_DECL(d_set_diffuse, e1e1e1e1, 1e1e1e1e, "Set Diffuse", bb_set_diffuse);
BB_DECL(d_movie_load, 798e26a9, 34e4169e, "Movie Load", bb_movie_load);
BB_DECL(d_movie_player, 778d16d4, 1dd60060, "Movie Player", bb_movie_player);
BB_DECL(d_set_2d_material, 3f7e5bff, 02326a71, "Set 2D Material", bb_set_2d_material);
BB_DECL(d_tt_set_2dsprite, 3c392558, 419d2680, "TT Set_2DSprite", bb_tt_set_2dsprite);
BB_DECL(d_set_texture, eb123eb5, 5be321be, "Set Texture", bb_set_texture);
BB_DECL(d_change_texture_video_format, f56d92ef, 41eb20c1, "Change Texture Video Format", bb_change_texture_video_format);
BB_DECL(d_set_as_active_camera, 368f0ab1, 2d8957e4, "Set As Active Camera", bb_set_as_active_camera);
BB_DECL(d_set_light_color, 32115590, 78951230, "Set Light Color", bb_set_light_color);
BB_DECL(d_set_emissive, d1d1d1d1, 30303030, "Set Emissive", bb_set_emissive);
BB_DECL(d_tt_reflection_mapping, 7e212b2f, 24db67c6, "TT_ReflectionMapping", bb_tt_reflection_mapping);
BB_DECL(d_set_blend_modes, 2f572a66, 6a9a0088, "Set Blend Modes", bb_set_blend_modes);
BB_DECL(d_set_alpha_test, 1cb5661e, 5b5d3fda, "Set Alpha Test", bb_set_alpha_test);
BB_DECL(d_set_material_z_buffer, 1144022f, 68fd055c, "Set Material Z Buffer", bb_set_material_z_buffer);
BB_DECL(d_set_prelit_color, 60415d44, 0d0174ea, "Set Prelit Color", bb_set_prelit_color);
BB_DECL(d_texture_scroller, f11d010a, fb1d010a, "Texture Scroller", bb_texture_scroller);
BB_DECL(d_set_current_slot, aaaa213a, eaa8d52a, "Set Current Slot", bb_set_current_slot);
BB_DECL_CB(d_planar_filter, 00cd320b, 32ed010b, "Planar Filter", bb_planar_filter, cb_planar_filter);

const CkBBDecl *const bb_visuals[] = {&d_hide, &d_show, &d_set_diffuse, &d_movie_load, &d_movie_player,
                                     &d_set_2d_material, &d_tt_set_2dsprite, &d_set_texture,
                                     &d_change_texture_video_format, &d_set_as_active_camera, &d_set_light_color,
                                     &d_set_emissive, &d_tt_reflection_mapping, &d_set_blend_modes, &d_set_alpha_test,
                                     &d_set_material_z_buffer, &d_set_prelit_color, &d_texture_scroller, &d_set_current_slot,
                                     &d_planar_filter};
const unsigned bb_visuals_count = sizeof bb_visuals / sizeof *bb_visuals;
