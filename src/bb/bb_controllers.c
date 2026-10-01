/* Controllers.dll Building Blocks: the keyboard and the mouse through the input manager (CKInputManager,
   GUID f787c904:0). The platform fills ctx->keys (DirectInput key codes) and the mouse each frame. */
#include "bb.h"

/* ---- Key Event 1af2274b:6b8c1524 (FUN_252043b0): pIn Key Waited... (one or more keys); pOut Key State.
   While active: Pressed once all keys are down (state TRUE), Released once one of them is up again (state
   FALSE). On resets the state; Off stops. Stays active. ---- */
static int bb_key_event(CkContext *ctx, CkBehavior *b)
{
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        return CKBR_OK;
    }
    int32_t state = 0;
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        bb_set_out(ctx, b, 0, &state, 4);
    }
    bb_get_out(ctx, b, 0, &state, 4);
    uint32_t i = 0;
    for (; i < b->pin.n; i++) {
        uint32_t key = 0;
        bb_get_in(ctx, b, i, &key, 4);
        if (!ctx->keys[key & 0xff]) break;
    }
    if (i == b->pin.n) {
        if (!state) {
            state = 1;
            ck_activate_output(ctx, b, 0, true);
        }
    } else if (state) {
        state = 0;
        ck_activate_output(ctx, b, 1, true);
    }
    bb_set_out(ctx, b, 0, &state, 4);
    return CKBR_ACTIVATENEXTFRAME;
}

/* ---- Mouse Waiter 014d010b:015d010b (FUN_25205240): settings 0 "Off input exists" and 1 the outputs that
   exist (bits: 1 Move, 2/4 left button down/up, 8/0x10 right, 0x20/0x40 middle, 0x80 roll; the outputs are
   numbered in that order). pOut Mouse Position, Wheel Direction, Left, Middle, Right Button: each button
   output fires on the edge against the previous value of its pOut; Move when the position changed. Stays
   active. ---- */
static int bb_mouse_waiter(CkContext *ctx, CkBehavior *b)
{
    int32_t has_off = 1, mask = 0xffff;
    bb_get_local(ctx, b, 0, &has_off, 4);
    bb_get_local(ctx, b, 1, &mask, 4);
    if (ck_input_active(ctx, b, 0)) ck_activate_input(ctx, b, 0, false);
    if (has_off && ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        return CKBR_OK;
    }
    float prev[2] = {0, 0}, wheel = 0;
    bb_get_out(ctx, b, 0, prev, 8);
    bb_set_out(ctx, b, 0, ctx->mouse, 8);
    bb_set_out(ctx, b, 1, &wheel, 4);
    int32_t was[3] = {0, 0, 0}, now[3];
    static const uint32_t pout_of[3] = {2, 3, 4}, bit_of[3] = {1, 4, 2};   /* left, middle, right */
    for (int k = 0; k < 3; k++) {
        bb_get_out(ctx, b, pout_of[k], &was[k], 4);
        now[k] = (ctx->mouse_buttons & bit_of[k]) != 0;
        bb_set_out(ctx, b, pout_of[k], &now[k], 4);
    }
    uint32_t out = 0;
    if (mask & 1) {
        if (prev[0] != ctx->mouse[0] || prev[1] != ctx->mouse[1]) ck_activate_output(ctx, b, out, true);
        out++;
    }
    static const int order[3] = {0, 2, 1};   /* left, right, middle outputs */
    for (int j = 0; j < 3; j++) {
        int k = order[j];
        if (mask & (2 << (2 * j))) {
            if (!was[k] && now[k]) ck_activate_output(ctx, b, out, true);
            out++;
        }
        if (mask & (4 << (2 * j))) {
            if (was[k] && !now[k]) ck_activate_output(ctx, b, out, true);
            out++;
        }
    }
    return CKBR_ACTIVATENEXTFRAME;
}

/* ---- Key Waiter 016d010b:017d010b (FUN_25204790 / any key FUN_25204820, callback FUN_252046c0 picks the
   function from local 0 "Wait For Any Key"): Out once the key (pIn 0) is down (IsKeyDown, +0x80), else
   stays active. The game uses it with a key (Exit Player). Any key: the first buffered key event, when a
   press (GetKeyFromBuffer(0), +0xa0), goes to pOut Key. ---- */
static int bb_key_waiter(CkContext *ctx, CkBehavior *b)
{
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

BB_DECL(d_key_waiter, 016d010b, 017d010b, "Key Waiter", bb_key_waiter);
BB_DECL(d_key_event, 1af2274b, 6b8c1524, "Key Event", bb_key_event);
BB_DECL(d_mouse_waiter, 014d010b, 015d010b, "Mouse Waiter", bb_mouse_waiter);

const CkBBDecl *const bb_controllers[] = {&d_key_event, &d_mouse_waiter, &d_key_waiter};
const unsigned bb_controllers_count = sizeof bb_controllers / sizeof *bb_controllers;
