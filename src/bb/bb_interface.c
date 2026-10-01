/* Text and menu buttons: Interface.dll's font Building Blocks (Set Font Properties, 2D Text) and
   TT_Toolbox_RT's TT CreateFontEx, TT PushButton2 and TT Scaleable Proximity. The font manager is src/ck/ck_font.c; docs/fonts.md
   has the original functions and their quirks. */
#include "bb.h"
#include "../ck/ck_font.h"
#include <math.h>

/* VxMath RGBAFTOCOLOR: channels * 255 truncated */
static uint32_t rgbaf_to_color(const float c[4])
{
    uint32_t k = 0;
    for (int i = 0; i < 4; i++) k |= ((uint32_t)(int32_t)(c[i] * 255.0f) & 0xff) << (i == 3 ? 24 : 16 - 8 * i);
    return k;
}

/* ---- TT CreateFontEx 260e4eb0:0e256b90 (TT_Toolbox_RT FUN_10026540): CreateTextureFont(pIn Font Name, pIn
   Font Texture, pIn 4 Font Bounds (zero = whole texture), pIn 2 character counts (16, 8), fixed = !pIn 3
   Proportional, pIn 5 First Character, 0.3) -> pOut Font; then glyphs 0..254 from the FontCoordinatesData
   array (pIn 6): row = glyph, columns u, v, width, pre, post, height. A missing row repeats the previous
   values. ---- */
static int bb_tt_create_font_ex(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    const char *name = bb_in_string(ctx, b, 0);
    if (!name) return CKBR_OK;
    CkId tex = bb_in_object(ctx, b, 1);
    if (!ck_texture(ctx, tex)) return CKBR_OK;
    float counts[2] = {16, 8}, bounds[4] = {0, 0, 0, 0};
    int32_t prop = 1, first = 0;
    bb_get_in(ctx, b, 2, counts, 8);
    bb_get_in(ctx, b, 3, &prop, 4);
    bb_get_in(ctx, b, 4, bounds, 16);
    bb_get_in(ctx, b, 5, &first, 4);
    int32_t idx = ck_font_create(ctx, name, tex, bounds, counts, prop == 0, first, 0.3f);
    bb_set_out(ctx, b, 0, &idx, 4);
    CkFont *f = ck_font_get(ctx, idx);
    CkDataArray *a = ck_array(ctx, bb_in_object(ctx, b, 6));
    if (!f) return CKBR_OK;
    float v[6] = {0, 0, 0, 0, 0, 0};
    for (uint32_t row = 0; row < 255; row++) {
        for (uint32_t col = 0; col < 6; col++) {
            CkCell *c = a ? ck_array_cell(a, row, col) : NULL;
            if (c) v[col] = c->f;
        }
        memcpy(f->g[row], v, sizeof v);
    }
    return CKBR_OK;
}

/* ---- Set Font Properties dacfbd61:7a6e65e7 (Interface FUN_2538a760): pIn Font, Space, Scale, Italic Offset,
   Color, End Color, Shadow Color, Shadow Angle, Shadow Distance, Shadow Size, Lit Material; setting the Font
   Properties flags. Colours only when readable; the shadow offset (-cos a, sin a) * distance when the angle
   is. The pins are read by index, as the original (some game instances lost Shadow Distance). ---- */
static int bb_set_font_properties(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    int32_t idx = 0;
    bb_get_in(ctx, b, 0, &idx, 4);
    CkFont *f = ck_font_get(ctx, idx);
    if (!f) return CKBR_OK;
    bb_get_in(ctx, b, 1, f->spacing, 8);
    bb_get_in(ctx, b, 2, f->scale, 8);
    bb_get_in(ctx, b, 3, &f->italic, 4);
    float c[4];
    if (bb_get_in(ctx, b, 4, c, 16)) f->start_color = rgbaf_to_color(c);
    if (bb_get_in(ctx, b, 5, c, 16)) f->end_color = rgbaf_to_color(c);
    if (bb_get_in(ctx, b, 6, c, 16)) f->shadow_color = rgbaf_to_color(c);
    float angle = 1.0f, dist = 4;
    if (bb_get_in(ctx, b, 7, &angle, 4)) {
        bb_get_in(ctx, b, 8, &dist, 4);
        f->shadow_offset[0] = -cosf(angle) * dist;
        f->shadow_offset[1] = sinf(angle) * dist;
    }
    bb_get_in(ctx, b, 9, f->shadow_scale, 8);
    bb_get_in(ctx, b, 10, &f->lit_material, 4);
    int32_t props = 0;
    bb_get_local(ctx, b, 0, &props, 4);
    f->props = (uint32_t)props;
    return CKBR_OK;
}

/* ---- 2D Text 055b29fe:662d5ca0 (Interface FUN_253868e0): while active, registers the text draw on the
   target 2D entity for this frame (AddPostRenderCallBack, temporary); the drawing and the outputs happen
   at render time (ck_text_render). On -> Exit On, Off -> Exit Off (stops). ---- */
static int bb_2d_text(CkContext *ctx, CkBehavior *b)
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
    if (ck_2dentity(ctx, bb_target(ctx, b))) ck_ids_push(&ctx->text_draws, b->h.id);
    return CKBR_ACTIVATENEXTFRAME;
}

/* ---- TT PushButton2 14d325d1:6748654e (TT_Toolbox_RT FUN_10021130): the target 2D entity as a mouse
   button. Local 0 = mouse over, 1 = left button pressed. Over = the topmost pickable 2D entity under the
   mouse is the target and the cursor is shown. Output 0 "Released" fires on roll-out (sic), 1 Roll Over
   on roll-in, 2 Mouse Down on the press edge while over. On resets, Off stops. ---- */
static int bb_tt_push_button2(CkContext *ctx, CkBehavior *b)
{
    int32_t state = 0, pressed = 0;
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        bb_set_local(ctx, b, 0, &state, 4);
        bb_set_local(ctx, b, 1, &pressed, 4);
    }
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        return CKBR_OK;
    }
    bb_get_local(ctx, b, 0, &state, 4);
    bb_get_local(ctx, b, 1, &pressed, 4);
    CkId t = bb_target(ctx, b);
    int32_t over = t && ck_pick_2d(ctx, ctx->mouse[0], ctx->mouse[1]) == t && ctx->cursor_visible;
    bool down = (ctx->mouse_buttons & 1) != 0, changed = false;
    if (down && !pressed) pressed = 1, changed = true;
    else if (!down && pressed) pressed = 0, changed = true;
    bb_set_local(ctx, b, 1, &pressed, 4);
    if (over != (state & 1)) {
        state = over;
        bb_set_local(ctx, b, 0, &state, 4);
        ck_activate_output(ctx, b, over ? 1 : 0, true);
    }
    if (changed && over && down) ck_activate_output(ctx, b, 2, true);
    return CKBR_ACTIVATENEXTFRAME;
}

/* CK3dEntity::GetBaryCenter (+0x1cc): the mean of the mesh's vertices in world space (the position
   without a mesh) */
static void barycenter(CkContext *ctx, Ck3dEntity *e, float out[3])
{
    CkMesh *m = ck_mesh(ctx, e->mesh);
    float c[3] = {0, 0, 0};
    if (m && m->nverts) {
        for (uint32_t i = 0; i < m->nverts; i++) c[0] += m->verts[i].pos.x, c[1] += m->verts[i].pos.y, c[2] += m->verts[i].pos.z;
        for (int k = 0; k < 3; k++) c[k] /= (float)m->nverts;
    }
    for (int j = 0; j < 3; j++) out[j] = c[0] * e->world[0][j] + c[1] * e->world[1][j] + c[2] * e->world[2][j] + e->world[3][j];
}

/* ---- TT Scaleable Proximity 2a2a63ca:00826175 (TT_Toolbox_RT FUN_1001bb70): the distance between pIn
   ObjectA and ObjectB (their positions, or with pIn Barycenter? their barycenters) over the axes of setting
   3 (bits 4 z, 2 y, 1 x), squared with setting 4 (then pIn Distance, 4 and 5 are squared too). pOut 0 =
   that distance, pOut 1 = (dx, dy, dz). The test runs every N frames (local 1 counts down): N goes
   linearly from pIn 6 at pIn 4 to pIn 7 at pIn 5, at least 1. State (local 0: 1 in, 0 out, 2 after On)
   gives the outputs of the setting 2 mask (1 In Range, 2 Out Range, 4 Enter Range, 8 Exit Range; numbered
   among the existing ones): in range: Enter after 0 or 2, else In; out: Exit after 1 or 2, else Out.
   Stays active; Off stops. ---- */
static int bb_tt_scaleable_proximity(CkContext *ctx, CkBehavior *b)
{
    int32_t mask = 0x11, axes = 0, squared = 1;
    bb_get_local(ctx, b, 2, &mask, 4);
    bb_get_local(ctx, b, 3, &axes, 4);
    bb_get_local(ctx, b, 4, &squared, 1);
    squared &= 0xff;
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        return CKBR_OK;
    }
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        int32_t two = 2;
        bb_set_local(ctx, b, 0, &two, 4);
    }
    int32_t countdown = 1;
    bb_get_local(ctx, b, 1, &countdown, 4);
    if (--countdown > 0) {
        bb_set_local(ctx, b, 1, &countdown, 4);
        return CKBR_ACTIVATENEXTFRAME;
    }
    float dist = 0;
    int32_t bary = 0;
    bb_get_in(ctx, b, 0, &dist, 4);
    bb_get_in(ctx, b, 3, &bary, 4);
    Ck3dEntity *a = ck_entity(ctx, bb_in_object(ctx, b, 1)), *c = ck_entity(ctx, bb_in_object(ctx, b, 2));
    if (!a || !c) return CKBR_OK;
    float pa[3], pb[3];
    if (bary) {
        barycenter(ctx, a, pa);
        barycenter(ctx, c, pb);
    } else {
        for (int j = 0; j < 3; j++) pa[j] = a->world[3][j], pb[j] = c->world[3][j];
    }
    float d[3] = {0, 0, 0}, d2 = 0;
    if (axes > 3) d[2] = pb[2] - pa[2], axes -= 4, d2 = d[2] * d[2];
    if (axes > 1) d[1] = pb[1] - pa[1], axes -= 2, d2 += d[1] * d[1];
    if (axes == 1) d[0] = pb[0] - pa[0], d2 += d[0] * d[0];
    float near = 0, far = 0;
    int32_t fnear = 0, ffar = 0;
    bb_get_in(ctx, b, 4, &near, 4);
    bb_get_in(ctx, b, 5, &far, 4);
    bb_get_in(ctx, b, 6, &fnear, 4);
    bb_get_in(ctx, b, 7, &ffar, 4);
    if (squared) dist *= dist, far *= far, near *= near;
    else d2 = sqrtf(d2);
    bb_set_out(ctx, b, 0, &d2, 4);
    bb_set_out(ctx, b, 1, d, 12);
    int32_t frames = d2 >= far ? ffar : d2 <= near ? fnear : (int32_t)((d2 - near) / (far - near) * (float)(ffar - fnear)) + fnear;
    if (frames < 1) frames = 1;
    bb_set_local(ctx, b, 1, &frames, 4);
    int32_t state = 0, fire;
    bb_get_local(ctx, b, 0, &state, 4);
    if (d2 < dist) {
        fire = state == 0 || state == 2 ? 4 : 1;
        state = 1;
    } else {
        fire = state == 1 || state == 2 ? 8 : 2;
        state = 0;
    }
    bb_set_local(ctx, b, 0, &state, 4);
    for (uint32_t bit = 0, out = 0; bit < 4; bit++)
        if (mask & (1 << bit)) {
            if (fire & (1 << bit)) ck_activate_output(ctx, b, out, true);
            out++;
        }
    return CKBR_ACTIVATENEXTFRAME;
}

/* ---- Text Display f22d010a:f2cd010a (FUN_25385d90, callback FUN_25386080; docs/fonts.md 6.5): Off -> Exit
   Off and the BB stops; On -> Exit On, then it stays active. The original draws the pins' text with a GDI
   CKSpriteText; only debug scripts use it (FPS, face and texture counters), so the text isn't drawn. ---- */
static int bb_text_display(CkContext *ctx, CkBehavior *b)
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
    return CKBR_ACTIVATENEXTFRAME;
}

BB_DECL(d_text_display, f22d010a, f2cd010a, "Text Display", bb_text_display);
BB_DECL(d_tt_create_font_ex, 260e4eb0, 0e256b90, "TT CreateFontEx", bb_tt_create_font_ex);
BB_DECL(d_set_font_properties, dacfbd61, 7a6e65e7, "Set Font Properties", bb_set_font_properties);
BB_DECL(d_2d_text, 055b29fe, 662d5ca0, "2D Text", bb_2d_text);
BB_DECL(d_tt_push_button2, 14d325d1, 6748654e, "TT PushButton2", bb_tt_push_button2);
BB_DECL(d_tt_scaleable_proximity, 2a2a63ca, 00826175, "TT Scaleable Proximity", bb_tt_scaleable_proximity);

const CkBBDecl *const bb_interface[] = {&d_text_display, &d_tt_create_font_ex, &d_set_font_properties, &d_2d_text, &d_tt_push_button2,
                                        &d_tt_scaleable_proximity};
const unsigned bb_interface_count = sizeof bb_interface / sizeof *bb_interface;
