/* TT_Toolbox_RT.dll Building Blocks for levels: following, looking at, timing, strings, textures
   (docs/gameplay_bbs.md part 2). */
#include "bb.h"
#include "../ck/ck_3d.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void entity_pos(const Ck3dEntity *e, const Ck3dEntity *ref, float out[3])
{
    /* GetPosition(&p, ref): in ref's frame */
    float w[3] = {e->world[3][0], e->world[3][1], e->world[3][2]};
    if (!ref) {
        memcpy(out, w, 12);
        return;
    }
    const float (*a)[4] = ref->world;
    float d[3] = {w[0] - a[3][0], w[1] - a[3][1], w[2] - a[3][2]};
    float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (det == 0) {
        memcpy(out, d, 12);
        return;
    }
    for (int k = 0; k < 3; k++) {
        float m[3][3];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) m[i][j] = i == k ? d[j] : a[i][j];
        out[k] = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                  m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) / det;
    }
}

/* ---- TT Set Dynamic Position 0fd4755f:7de22dc8 (execute 0x10004a80): a damped spring toward pIn Object (minus
   the offset), positions in pIn Coordinate System. On records the position (output 0 now, stays active);
   each frame N = X + V * Damping + (T - X - Offset) * Force * dt(s); Off does one last step, then Off.
   Output 0 gets the new position. (The MaxDistance clamp, buggy in the original, is 0 in every game
   instance and left out.) ---- */
static int bb_tt_set_dynamic_position(CkContext *ctx, CkBehavior *b)
{
    int32_t status = 0;
    Ck3dEntity *e = ck_entity(ctx, bb_target(ctx, b)), *ref = ck_entity(ctx, bb_in_object(ctx, b, 10));
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        bb_set_local(ctx, b, 0, &status, 1);
    } else if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        ck_activate_output(ctx, b, 0, true);
        status = 1;
        bb_set_local(ctx, b, 0, &status, 1);
        if (!e) return CKBR_OK;
        float old[3];
        entity_pos(e, ref, old);
        bb_set_local(ctx, b, 1, old, 12);
        return CKBR_ACTIVATENEXTFRAME;
    } else {
        bb_get_local(ctx, b, 0, &status, 1);
        status &= 0xff;
    }
    Ck3dEntity *obj = ck_entity(ctx, bb_in_object(ctx, b, 0));
    if (!e || !obj) return CKBR_OK;
    float t[3], p0[3] = {0, 0, 0}, x[3];
    entity_pos(obj, ref, t);
    bb_get_local(ctx, b, 1, p0, 12);
    entity_pos(e, ref, x);
    bb_set_local(ctx, b, 1, x, 12);
    float force[3] = {1, 1, 1}, damp[3] = {0, 0, 0}, off[3] = {0, 0, 0}, n[3];
    for (int k = 0; k < 3; k++) {
        bb_get_in(ctx, b, 1 + (uint32_t)k, &force[k], 4);
        bb_get_in(ctx, b, 4 + (uint32_t)k, &damp[k], 4);
        bb_get_in(ctx, b, 7 + (uint32_t)k, &off[k], 4);
    }
    float kdt = ctx->delta_ms * 0.001f;
    for (int k = 0; k < 3; k++) n[k] = x[k] + (x[k] - p0[k]) * damp[k] + (t[k] - x[k] - off[k]) * force[k] * kdt;
    /* SetPosition(&N, ref, FALSE) */
    float w[3];
    if (ref)
        for (int j = 0; j < 3; j++) w[j] = n[0] * ref->world[0][j] + n[1] * ref->world[1][j] + n[2] * ref->world[2][j] + ref->world[3][j];
    else
        memcpy(w, n, 12);
    ck_entity_set_position(ctx, e, w, 0, false);
    bb_set_out(ctx, b, 0, n, 12);
    if (status) return CKBR_ACTIVATENEXTFRAME;
    ck_activate_output(ctx, b, 1, true);
    return CKBR_OK;
}

/* ---- TT_LinearVolume 09b335b3:12d17cdc (execute 0x10021600): pIn 0 (a 0..1 level) -> 1 above 1, 0 at or
   below 0.01, else 50^v * 0.02 ---- */
static int bb_tt_linear_volume(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    float v = 0;
    bb_get_in(ctx, b, 0, &v, 4);
    float r = v > 1 ? 1.0f : v <= 0.01 ? 0.0f : (float)(pow(50.0, (double)v) * 0.02);
    bb_set_out(ctx, b, 0, &r, 4);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- TT_Timer 6ac67901:7d2a6059 (execute 0x10022800): the chronometer of the global TT_Sceneanager.
   On/Reset (then continues), Pause, Play, Off (resets); otherwise pOut Elapsed Time = elapsed + dt, then it
   adds dt unless paused. ---- */
static int bb_tt_timer(CkContext *ctx, CkBehavior *b)
{
    if (ck_input_active(ctx, b, 0)) {
        ctx->tt_timer_paused = false, ctx->tt_timer_ms = 0;
        ck_activate_input(ctx, b, 0, false);
        ck_activate_output(ctx, b, 0, true);
    }
    if (ck_input_active(ctx, b, 1)) {
        ctx->tt_timer_paused = true;
        ck_activate_input(ctx, b, 1, false);
        ck_activate_output(ctx, b, 1, true);
        return CKBR_ACTIVATENEXTFRAME;
    }
    if (ck_input_active(ctx, b, 2)) {
        ctx->tt_timer_paused = false;
        ck_activate_input(ctx, b, 2, false);
        ck_activate_output(ctx, b, 2, true);
        return CKBR_ACTIVATENEXTFRAME;
    }
    if (ck_input_active(ctx, b, 3)) {
        ctx->tt_timer_paused = false, ctx->tt_timer_ms = 0;
        ck_activate_input(ctx, b, 3, false);
        ck_activate_output(ctx, b, 3, true);
        return CKBR_OK;
    }
    float out = ctx->tt_timer_ms + ctx->delta_ms;
    if (!ctx->tt_timer_paused) ctx->tt_timer_ms += ctx->delta_ms;
    bb_set_out(ctx, b, 0, &out, 4);
    return CKBR_ACTIVATENEXTFRAME;
}

/* CK3dEntity::GetOrientation / SetOrientation (+0x130 / +0x12c): the axes rows (unit) */
static void set_orientation(CkContext *ctx, Ck3dEntity *e, const float dir[3], const float up[3], const float right[3])
{
    float m[4][4];
    memcpy(m, e->world, sizeof m);
    for (int k = 0; k < 3; k++) m[0][k] = right[k], m[1][k] = up[k], m[2][k] = dir[k];
    ck_entity_set_world(ctx, e, m, false);
}

static void norm3(float v[3])
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) v[0] /= l, v[1] /= l, v[2] /= l;
}

static void cross3(const float a[3], const float b[3], float o[3])
{
    float r[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    memcpy(o, r, 12);
}

/* ---- TT LookAt 3d4861f8:2861703d (execute 0x10010e00, helper 0x10011190): turns the target's setting-1 axis
   (-Z, Y, ...) toward pIn Position (through pIn Referential), ignoring the axes flagged by pins 4..6, by pIn
   Following Speed (times dt * 0.07 when Time Based, at most 1); the scale is kept; roll (setting 2) about
   the axis ---- */
static int bb_tt_lookat(CkContext *ctx, CkBehavior *b)
{
    Ck3dEntity *e = ck_entity(ctx, bb_target(ctx, b));
    if (!e) return CKBR_OK;
    float pos[3] = {0, 0, 0}, w[3], speed = 1;
    bb_get_in(ctx, b, 0, pos, 12);
    Ck3dEntity *ref = ck_entity(ctx, bb_in_object(ctx, b, 1));
    if (ref)
        for (int j = 0; j < 3; j++) w[j] = pos[0] * ref->world[0][j] + pos[1] * ref->world[1][j] + pos[2] * ref->world[2][j] + ref->world[3][j];
    else
        memcpy(w, pos, 12);
    bb_get_in(ctx, b, 2, &speed, 4);
    int32_t time_based = 1, dir = 5;
    float roll = 0;
    bb_get_local(ctx, b, 0, &time_based, 4);
    bb_get_local(ctx, b, 1, &dir, 4);
    bb_get_local(ctx, b, 2, &roll, 4);
    if (time_based) speed *= ctx->delta_ms * 0.07f;
    if (speed > 1) speed = 1;
    float d[3] = {w[0] - e->world[3][0], w[1] - e->world[3][1], w[2] - e->world[3][2]};
    if (d[0] != 0 || d[1] != 0 || d[2] != 0) {
        for (int k = 0; k < 3; k++) {
            int32_t ignore = 1;
            bb_get_in(ctx, b, 4 + (uint32_t)k, &ignore, 4);
            if (ignore) d[k] = 0;
        }
        norm3(d);
        float scale[3];
        for (int i = 0; i < 3; i++) scale[i] = sqrtf(e->world[i][0] * e->world[i][0] + e->world[i][1] * e->world[i][1] + e->world[i][2] * e->world[i][2]);
        if (!(dir & 1)) d[0] = -d[0], d[1] = -d[1], d[2] = -d[2];
        float up[3] = {0, 1, 0}, n[3], r[3], u[3];
        if (dir >= 5) {
            float c[3] = {e->world[2][0], e->world[2][1], e->world[2][2]};
            norm3(c);
            for (int k = 0; k < 3; k++) n[k] = c[k] + (d[k] - c[k]) * speed;
            cross3(up, n, r), cross3(n, r, u);
            norm3(n), norm3(r), norm3(u);
            set_orientation(ctx, e, n, u, r);
        } else if (dir >= 3) {
            float c[3] = {e->world[1][0], e->world[1][1], e->world[1][2]}, a[3], bb[3];
            norm3(c);
            for (int k = 0; k < 3; k++) n[k] = c[k] + (d[k] - c[k]) * speed;
            cross3(n, up, a), cross3(a, n, bb);
            norm3(n), norm3(a), norm3(bb);
            set_orientation(ctx, e, a, n, bb);
        }
        if (roll != 0) {
            /* Rotate3f about the local Z (or Y) axis, keeping the other rows orthogonal */
            int ax = dir >= 5 ? 2 : 1, i1 = (ax + 1) % 3, i2 = (ax + 2) % 3;
            float m[4][4], cs = cosf(roll), sn = sinf(roll);
            memcpy(m, e->world, sizeof m);
            for (int k = 0; k < 3; k++) {
                float p1 = e->world[i1][k], p2 = e->world[i2][k];
                m[i1][k] = p1 * cs + p2 * sn, m[i2][k] = p2 * cs - p1 * sn;
            }
            ck_entity_set_world(ctx, e, m, false);
        }
        ck_entity_scale(ctx, e, scale, true, false);
    }
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- TT ConvertPixel-Homogen 18f96977:18e20f83 (execute 0x1000e9e0): pIn Position between pixels and
   fractions of the render context (setting 0 TRUE: pixel -> homogeneous; FALSE: truncated pixels) ---- */
static int bb_tt_convert_pixel_homogen(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    float pos[2] = {0, 0}, r[2];
    int32_t to_h = 0;
    bb_get_in(ctx, b, 0, pos, 8);
    bb_get_local(ctx, b, 0, &to_h, 4);
    if (to_h) r[0] = pos[0] / CK_SCREEN_W, r[1] = pos[1] / CK_SCREEN_H;
    else r[0] = (float)(int32_t)(pos[0] * CK_SCREEN_W), r[1] = (float)(int32_t)(pos[1] * CK_SCREEN_H);
    bb_set_out(ctx, b, 0, &r[0], 4);
    bb_set_out(ctx, b, 1, &r[1], 4);
    bb_set_out(ctx, b, 2, r, 8);
    return CKBR_OK;
}

/* ---- TT_SplitString 5ae74e2f:799c49b5 (execute 0x10012220): as Scan String (pieces as strings) ---- */
static int bb_tt_split_string(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    const char *text = bb_in_string(ctx, b, 0);
    if (!text) return CKBR_OK;
    const char *delim = bb_in_string(ctx, b, 1);
    if (!delim || !*delim) delim = " ";
    size_t dl = strlen(delim);
    int32_t count = 0;
    char piece[512];
    const char *cur = text, *p;
    while ((p = strstr(cur, delim)) != NULL) {
        if (p != cur) {
            count++;
            if (!bb_out(ctx, b, (uint32_t)count)) break;
            snprintf(piece, sizeof piece, "%.*s", (int)(p - cur), cur);
            bb_set_out(ctx, b, (uint32_t)count, piece, (uint32_t)strlen(piece) + 1);
        }
        cur = p + dl;
    }
    if (*cur) {
        count++;
        if (!bb_out(ctx, b, (uint32_t)count)) return CKBR_OK;
        snprintf(piece, sizeof piece, "%s", cur);
        bb_set_out(ctx, b, (uint32_t)count, piece, (uint32_t)strlen(piece) + 1);
    }
    bb_set_out(ctx, b, 0, &count, 4);
    return CKBR_OK;
}

/* ---- TT_PreloadTextures 416b4f0e:192d339a / TT_FlushTextures 734f73b4:01fc79ac: video memory management;
   the renderer uploads every texture anyway (the count reports the textures) ---- */
static int bb_tt_preload_textures(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    int32_t n = 0;
    for (uint32_t i = 0; i < ctx->nobjs; i++) n += ctx->objs[i] && ctx->objs[i]->cid == CKCID_TEXTURE;
    bb_set_out(ctx, b, 0, &n, 4);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

static int bb_tt_flush_textures(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

BB_DECL(d_tt_set_dynamic_position, 0fd4755f, 7de22dc8, "TT Set Dynamic Position", bb_tt_set_dynamic_position);
BB_DECL(d_tt_linear_volume, 09b335b3, 12d17cdc, "TT_LinearVolume", bb_tt_linear_volume);
BB_DECL(d_tt_timer, 6ac67901, 7d2a6059, "TT_Timer", bb_tt_timer);
BB_DECL(d_tt_lookat, 3d4861f8, 2861703d, "TT LookAt", bb_tt_lookat);
/* ---- TT_Key Waiter 2ffb3ef0:21807ae3 (execute 0x10024d90 / any key 0x10024e50, callback 0x10024c80 picks by
   local 0 "Wait For Any Key"): Controllers' Key Waiter plus an Off input (input 1: cleared, the BB stops,
   no output). The game (Menu, the key configuration) waits for any key: the first buffered key event,
   when a press (GetKeyFromBuffer(0)), is written to pOut Key and Out fires. ---- */
static int bb_tt_key_waiter(CkContext *ctx, CkBehavior *b)
{
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        return CKBR_OK;
    }
    int32_t any = 0;
    bb_get_local(ctx, b, 0, &any, 4);
    if (any) {
        for (uint32_t k = 1; k < 256; k++)
            if (ctx->keys[k] && !ctx->keys_prev[k]) {
                ck_activate_input(ctx, b, 0, false);
                ck_activate_output(ctx, b, 0, true);
                int32_t key = (int32_t)k;
                bb_set_out(ctx, b, 0, &key, 4);
                return CKBR_OK;
            }
        return CKBR_ACTIVATENEXTFRAME;
    }
    int32_t key = 0;
    bb_get_in(ctx, b, 0, &key, 4);
    if (ctx->keys[key & 0xff]) {
        ck_activate_input(ctx, b, 0, false);
        ck_activate_output(ctx, b, 0, true);
        return CKBR_OK;
    }
    return CKBR_ACTIVATENEXTFRAME;
}

BB_DECL(d_tt_key_waiter, 2ffb3ef0, 21807ae3, "TT_Key Waiter", bb_tt_key_waiter);
BB_DECL(d_tt_convert_pixel_homogen, 18f96977, 18e20f83, "TT ConvertPixel-Homogen", bb_tt_convert_pixel_homogen);
BB_DECL(d_tt_split_string, 5ae74e2f, 799c49b5, "TT_SplitString", bb_tt_split_string);
BB_DECL(d_tt_preload_textures, 416b4f0e, 192d339a, "TT_PreloadTextures", bb_tt_preload_textures);
BB_DECL(d_tt_flush_textures, 734f73b4, 01fc79ac, "TT_FlushTextures", bb_tt_flush_textures);

const CkBBDecl *const bb_tt_toolbox[] = {&d_tt_key_waiter, &d_tt_set_dynamic_position, &d_tt_linear_volume, &d_tt_timer, &d_tt_lookat,
                                         &d_tt_convert_pixel_homogen, &d_tt_split_string, &d_tt_preload_textures,
                                         &d_tt_flush_textures};
const unsigned bb_tt_toolbox_count = sizeof bb_tt_toolbox / sizeof *bb_tt_toolbox;
