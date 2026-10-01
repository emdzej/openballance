/* Building Blocks the game uses once or twice, from several DLLs: TT_Toolbox_RT (counter, text input, paths,
   environment, texture info, mipmapping), TT_Gravity_RT (proximity volume), Controllers (Input String),
   3DTransfo (Play Global Animation), Cameras (Set Clipping Planes), Materials (Set MipMap Level), Visuals
   (Statistics) and Interface (Create System Font). Each cites the original function (re/bb_map.txt; the
   TT_Toolbox_RT addresses are the jump thunks' targets). */
#include "bb.h"
#include "../ck/ck_3d.h"
#include "../ck/ck_anim.h"
#include "../ck/ck_curve.h"
#include "../ck/ck_font.h"
#include "../ck/ck_sound.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- TT_Counter 750f20fc:3741186d (thunk 0x1000114a -> 0x1000f0a0): local Count; a Start outside
   [Min, Max] is replaced by Min (console "TT_Counter: Start value invalid!"). Initialize -> Count = Start;
   Count Up -> Count + 1, past Max wraps to Min; Count Down -> Count - 1, below Min wraps to Max. Every
   active input fires its output; the count goes to the local and pOut current count. ---- */
static int bb_tt_counter(CkContext *ctx, CkBehavior *b)
{
    int32_t start = 0, lo = 0, hi = 0, count = 0;
    bb_get_in(ctx, b, 0, &start, 4);
    bb_get_in(ctx, b, 1, &lo, 4);
    bb_get_in(ctx, b, 2, &hi, 4);
    bb_get_local(ctx, b, 0, &count, 4);
    if (start < lo || hi < start) {
        start = lo;
        if (ctx->log) ctx->log("TT_Counter: Start value invalid!");
    }
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        count = start;
        ck_activate_output(ctx, b, 0, true);
    }
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        if (hi < ++count) count = lo;
        ck_activate_output(ctx, b, 1, true);
    }
    if (ck_input_active(ctx, b, 2)) {
        ck_activate_input(ctx, b, 2, false);
        if (--count < lo) count = hi;
        ck_activate_output(ctx, b, 2, true);
    }
    bb_set_local(ctx, b, 0, &count, 4);
    bb_set_out(ctx, b, 0, &count, 4);
    return CKBR_OK;
}

/* ---- TT ProximityVolumeControl 38571b61:64cd2174 (TT_Gravity_RT 0x100031a0): local Status. On -> On,
   Status TRUE; Off clears Status. Every frame d^2 = |Object #2's position in Object #1's frame|^2;
   g = 0 beyond Far, 1 within Near, else 1 - (d^2 - near^2) / (far^2 - near^2). The sound's gain is
   0x10003150(g): 1 above 1, 0 below 0, else 0.02 * 50^g (so out of range it is 0.02, not silent). Then
   Status FALSE -> Off output and return 0, else stay active. A missing object or sound returns 0xa008. ---- */
static float proximity_gain(float g)
{
    if (g > 1) return 1;
    if (g < 0) return 0;
    return (float)(pow(50.0, g) * 0.02);
}

static int bb_tt_proximity_volume_control(CkContext *ctx, CkBehavior *b)
{
    int32_t status = 0;
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        bb_set_local(ctx, b, 0, &status, 4);
    } else if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        ck_activate_output(ctx, b, 0, true);
        status = 1;
        bb_set_local(ctx, b, 0, &status, 4);
    } else {
        bb_get_local(ctx, b, 0, &status, 4);
    }
    CkId o1 = bb_in_object(ctx, b, 0);
    Ck3dEntity *e1 = ck_entity(ctx, o1), *e2 = ck_entity(ctx, bb_in_object(ctx, b, 1));
    if (!e1 || !e2) return 0xa008;
    float p[3];
    ck_entity_get_position(ctx, e2, o1, p);
    float d2 = p[0] * p[0] + p[1] * p[1] + p[2] * p[2], near = 0, far = 0, g;
    bb_get_in(ctx, b, 2, &near, 4);
    bb_get_in(ctx, b, 3, &far, 4);
    near *= near, far *= far;
    if (!(d2 < far)) g = 0;
    else if (d2 <= near) g = 1;
    else g = 1 - (d2 - near) / (far - near);
    CkWaveSound *s = ck_wavesound(ctx, bb_in_object(ctx, b, 4));
    if (!s) return 0xa008;
    ck_sound_set_gain(s, proximity_gain(g));
    if (!(status & 0xff)) {   /* the low byte: the original stores a bool in a 4-byte local */
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    return CKBR_ACTIVATENEXTFRAME;
}

/* ---- Text input: Input String 693e7e4b:100104c4 (Controllers FUN_25202c70, callback FUN_252039e0) and
   TT InputString 52fd6294:612f51a5 (thunks 0x100011b3 -> 0x1001fa80, callback 0x1000155f -> 0x100208c0).

   Locals: 0 Max Size (32), 1 Keyboard Repetition, 2 Multiline, 3 Disable Keyboard Section (bit 0 the
   numeric keypad, bit 1 the cursor block), 4 Use Caret, 5 the caret index, 6 the working buffer: the text
   with a '\b' at the caret (when full, the caret takes the last cell and the last character sits after it),
   Input String 7 "on". Each frame the input manager's key buffer is walked (pressed keys only): pOut Key
   = the scan code; Backspace / Delete edit, Left / Right move (unless section bit 1), keypad keys insert
   their character (unless bit 0), others go through VxScanCodeToAscii (nothing for 0 and Esc). Input String
   ends on Return / keypad Enter (or the End Key when multiline): pOut String = the text, Off output, return
   0. TT InputString inserts '\n' for Return when multiline and only ends on the End Key; its pOut 1 is the
   text without caret. After an edit the caret is stored, pOut String gets the text (with the '\b' caret
   when Use Caret) and Updated String fires.

   The key buffer is this frame's newly pressed keys in scan code order (Keyboard Repetition, DirectInput's
   auto-repeat, has no source here); VxScanCodeToAscii uses a US layout with Shift. Home / End / Page Up /
   Page Down (the multi-line caret moves, 0x252029e0..0x25202b10) need section bit 1 clear, which no game
   instance has (both use 3); they are not ported. ---- */

/* VxScanCodeToAscii (US layout): 0 for keys without a character */
static char scan_to_ascii(const CkContext *ctx, uint32_t k)
{
    static const char lower[0x3a] = {
        0, 0x1b, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',
        '[', ']', '\r', 0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.',
        '/', 0, '*', 0, ' '};
    static const char upper[0x3a] = {
        0, 0x1b, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',
        '{', '}', '\r', 0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>',
        '?', 0, '*', 0, ' '};
    if (k >= sizeof lower) return 0;
    return (ctx->keys[0x2a] || ctx->keys[0x36]) ? upper[k] : lower[k];
}

/* the keypad characters (jump table 0x252038a8 / 0x100204a4) */
static char keypad_char(uint32_t k)
{
    switch (k) {
    case 0x37: return '*';
    case 0x47: return '7';
    case 0x48: return '8';
    case 0x49: return '9';
    case 0x4a: return '-';
    case 0x4b: return '4';
    case 0x4c: return '5';
    case 0x4d: return '6';
    case 0x4e: return '+';
    case 0x4f: return '1';
    case 0x50: return '2';
    case 0x51: return '3';
    case 0x52: return '0';
    case 0x53: return '.';
    case 0x8d: return '=';
    case 0x9c: return '\n';
    case 0xb3: return ',';
    case 0xb5: return '/';
    }
    return 0;
}

/* FUN_25202bb0 / 0x1001f950: insert ch at the caret. Full (len - 1 == max) with the caret before the last
   character: that character is replaced. One cell left with the caret at the end: ch goes after the caret.
   Otherwise the tail moves right (cut to max when full) and the caret advances. */
static bool text_insert(char ch, char *buf, int32_t *caret, int32_t *len, int32_t max)
{
    int32_t n = *len;
    if (n - 1 == max && *caret == n - 2) {
        if (ch == '\n') return false;
        buf[*caret + 1] = ch;
        return true;
    }
    if (n == max && *caret == n - 1) {
        if (ch == '\n') return false;
        buf[*caret + 1] = ch;
        buf[*caret + 2] = 0;
    } else {
        int32_t end = n - 1 == max ? max : n;
        memmove(buf + *caret + 1, buf + *caret, (size_t)(end - *caret));
        buf[*caret] = ch;
        (*caret)++;
        buf[end + 1] = 0;
    }
    if (*len <= max) (*len)++;
    return true;
}

/* FUN_25202940 / 0x1001f6a0: remove the character at the caret (the '\b'); the caret becomes 0 */
static void text_remove_caret(char *buf, int32_t *caret, int32_t len)
{
    if (*caret >= 0 && *caret < len) memmove(buf + *caret, buf + *caret + 1, (size_t)(len - *caret));
    if (len > 0) buf[len - 1] = 0;
    *caret = 0;
}

/* FUN_25202980: every '\b' removed in place */
static void text_strip_carets(char *s)
{
    char *d = s;
    for (; *s; s++)
        if (*s != '\b') *d++ = *s;
    *d = 0;
}

static void set_string_out(CkContext *ctx, CkBehavior *b, uint32_t i, const char *s)
{
    bb_set_out(ctx, b, i, s, (uint32_t)strlen(s) + 1);
}

/* pOut i = the buffer without its caret (a copy) */
static void set_string_out_nocaret(CkContext *ctx, CkBehavior *b, uint32_t i, const char *buf, int32_t caret)
{
    size_t n = strlen(buf);
    char *c = malloc(n + 1);
    memcpy(c, buf, n + 1);
    text_remove_caret(c, &caret, (int32_t)n);
    set_string_out(ctx, b, i, c);
    free(c);
}

static int text_input(CkContext *ctx, CkBehavior *b, bool tt)
{
    CkParameter *lp = bb_local(ctx, b, 6);
    if (!lp) return CKBR_OK;   /* the original creates missing locals (old files); none in the game */
    int32_t max = 0x20, caret = 0;
    bb_get_local(ctx, b, 0, &max, 4);
    bb_get_local(ctx, b, 5, &caret, 4);
    if (max < 1) max = 1;
    uint32_t size = lp->size > (uint32_t)max + 2 ? lp->size : (uint32_t)max + 2;
    char *buf = calloc(size + 4, 1);
    if (lp->value) memcpy(buf, lp->value, lp->size);
    buf[size - 1] = 0;
    int32_t len = (int32_t)strlen(buf);
    if (caret < 0 || caret > len) caret = len;
    const uint32_t key_out = tt ? 2 : 1;
    int ret = CKBR_ACTIVATENEXTFRAME;
    int32_t on = 0, rep = 0, use_caret = 0;
    bb_get_local(ctx, b, 4, &use_caret, 4);

    if (ck_input_active(ctx, b, 1)) {
        /* Off: auto-repeat off (input manager +0x78); Input String outputs the text if it was on */
        bb_get_local(ctx, b, 1, &rep, 4);
        if (tt) {
            text_remove_caret(buf, &caret, len);
            bb_set_local(ctx, b, 5, &caret, 4);
            set_string_out(ctx, b, 0, buf);
        } else {
            bb_get_local(ctx, b, 7, &on, 4);
            if (on) {
                set_string_out_nocaret(ctx, b, 0, buf, caret);
                on = 0;
                bb_set_local(ctx, b, 7, &on, 4);
            }
        }
        ck_activate_input(ctx, b, 1, false);
        ck_activate_output(ctx, b, 1, true);
        ret = CKBR_OK;
    } else if (ck_input_active(ctx, b, 0)) {
        /* On: the buffer from Reset String, the caret at its end (or on the last cell when it is too long) */
        if (!tt) {
            on = 1;
            bb_set_local(ctx, b, 7, &on, 4);
        }
        ck_activate_input(ctx, b, 0, false);
        CkParameter *rp = bb_in(ctx, b, 0);
        char *reset = rp && rp->value && rp->size ? (char *)rp->value : NULL;
        if (reset && !tt) text_strip_carets(reset);   /* in the source parameter, as the original */
        if (tt) set_string_out(ctx, b, 1, reset ? reset : "");
        const char *r = reset ? reset : "";
        int32_t n = (int32_t)strnlen(r, rp ? rp->size : 0);
        if (n < max) {
            memcpy(buf, r, (size_t)n);
            buf[n] = '\b';
            buf[n + 1] = 0;
            caret = n;
        } else {
            strncpy(buf, r, (size_t)max);
            buf[max] = buf[max - 1];
            buf[max - 1] = '\b';
            caret = max - 1;
            buf[max + 1] = 0;
        }
        bb_set_local(ctx, b, 5, &caret, 4);
        if (!use_caret) set_string_out_nocaret(ctx, b, 0, buf, caret);
        else set_string_out(ctx, b, 0, buf);
        ck_activate_output(ctx, b, 0, true);
    } else {
        int32_t multiline = 0, section = 0;
        uint32_t end_key = 0;
        bb_get_local(ctx, b, 2, &multiline, 4);
        if (multiline) bb_get_in(ctx, b, 1, &end_key, 4);
        bb_get_local(ctx, b, 3, &section, 4);
        bool changed = false, done = false;
        for (uint32_t k = 1; k < 256 && !done; k++) {
            if (!ctx->keys[k] || ctx->keys_prev[k]) continue;
            bb_set_out(ctx, b, key_out, &k, 4);
            len = (int32_t)strlen(buf);
            if (multiline ? (end_key && k == end_key) : (!tt && (k == 0x1c || k == 0x9c))) {
                if (tt) {
                    text_remove_caret(buf, &caret, len);
                    bb_set_local(ctx, b, 5, &caret, 4);
                    set_string_out(ctx, b, 0, buf);
                } else {
                    set_string_out_nocaret(ctx, b, 0, buf, caret);
                    bb_set_local(ctx, b, 5, &caret, 4);
                }
                ck_activate_output(ctx, b, 1, true);
                ret = CKBR_OK;
                done = true;
                break;
            }
            char ch = 0;
            if (k == 0x0e) {   /* Backspace */
                if (caret > 0) {
                    if (len - 1 == max && caret == len - 2) buf[caret + 1] = 0;
                    else {
                        memmove(buf + caret - 1, buf + caret, (size_t)(len - caret));
                        caret--;
                        buf[len - 1] = 0;
                    }
                    changed = true;
                }
            } else if (k == 0xd3) {   /* Delete */
                if (len > 1) {
                    if (len - 1 == max && caret == len - 2) {
                        buf[caret + 1] = 0;
                        changed = true;
                    } else if (caret < len - 1) {
                        memmove(buf + caret + 1, buf + caret + 2, (size_t)(len - caret - 2));
                        buf[len - 1] = 0;
                        changed = true;
                    }
                }
            } else if (k == 0xcb) {   /* Left */
                if (!(section & 2) && caret > 0) {
                    buf[caret] = buf[caret - 1];
                    buf[--caret] = '\b';
                    changed = true;
                }
            } else if (k == 0xcd) {   /* Right */
                if (!(section & 2) && caret < len - 1 && caret + 1 < max) {
                    buf[caret] = buf[caret + 1];
                    buf[++caret] = '\b';
                    changed = true;
                }
            } else if (k == 0xc7 || k == 0xc9 || k == 0xcf || k == 0xd1) {
                /* Home, Page Up, End, Page Down: see above */
            } else if (k == 0x1c) {   /* Return: a new line (Input String gets here only when multiline) */
                if (!tt || multiline) ch = '\n';
            } else if ((ch = keypad_char(k)) != 0) {
                if (section & 1) ch = 0;
            } else {
                ch = scan_to_ascii(ctx, k);
                if (ch == 0x1b) ch = 0;
            }
            if (ch) {
                text_insert(ch, buf, &caret, &len, max);
                changed = true;
            }
        }
        if (!done && changed) {
            bb_set_local(ctx, b, 5, &caret, 4);
            if (use_caret) set_string_out(ctx, b, 0, buf);
            else set_string_out_nocaret(ctx, b, 0, buf, caret);
            if (tt) set_string_out_nocaret(ctx, b, 1, buf, caret);
            ck_activate_output(ctx, b, 2, true);
        }
    }
    ck_param_set(lp, buf, size);
    free(buf);
    return ret;
}

static int bb_input_string(CkContext *ctx, CkBehavior *b) { return text_input(ctx, b, false); }
static int bb_tt_input_string(CkContext *ctx, CkBehavior *b) { return text_input(ctx, b, true); }

/* Input String's callback (FUN_252039e0): except on settings edits, "on" off, the buffer just the caret
   ("\b", Max Size + 2 bytes), caret 0 */
static void cb_input_string(CkContext *ctx, CkBehavior *b, int msg)
{
    int32_t z = 0, max = 0x20;
    bb_set_local(ctx, b, 7, &z, 4);
    bb_get_local(ctx, b, 0, &max, 4);
    if (max < 1) max = 1;
    char *buf = calloc((size_t)max + 2, 1);
    buf[0] = '\b';
    CkParameter *lp = bb_local(ctx, b, 6);
    if (lp) ck_param_set(lp, buf, (uint32_t)max + 2);
    free(buf);
    bb_set_local(ctx, b, 5, &z, 4);
    (void)msg;
}

/* TT InputString's callback (0x100208c0): on attach and load, pOut String = Max Size + 2 zero bytes, caret 0 */
static void cb_tt_input_string(CkContext *ctx, CkBehavior *b, int msg)
{
    if (msg != CKM_BEHAVIORATTACH && msg != CKM_BEHAVIORLOAD) return;
    int32_t z = 0, max = 0x20;
    bb_get_local(ctx, b, 0, &max, 4);
    if (max < 1) max = 1;
    char *buf = calloc((size_t)max + 2, 1);
    bb_set_out(ctx, b, 0, buf, (uint32_t)max + 2);
    free(buf);
    bb_set_local(ctx, b, 5, &z, 4);
}

/* ---- Play Global Animation 1c9236e1:42f40996 (3DTransfo FUN_250077b0): pIn Animation (a keyed animation),
   Duration (ms; an Int pin is read as an integer; default 5000; below 0.001 the BB does nothing and
   returns 0), Progression Curve, Loop (TRUE); local 0 the progression. Off: return 0. Each frame progression
   += dt / duration; On restarts it at 0 (and resets character roots: no characters here). Past 1: One Loop
   Played; without Loop the animation is set to 1 and the BB stops, with Loop the integer part is dropped.
   FUN_25007a20: local and pOut Progression = p, then SetStep (+0x64) at curve(p). ---- */
static int bb_play_global_animation(CkContext *ctx, CkBehavior *b)
{
    CkKeyedAnimation *k = ck_keyedanim(ctx, bb_in_object(ctx, b, 0));
    float duration = 5000;
    CkParameter *dp = b->pin.n > 1 ? ck_param(ctx, b->pin.v[1]) : NULL;
    if (dp && ck_guid_eq(dp->type, CKPGUID_INT)) {
        int32_t ms = 5000;
        bb_get_in(ctx, b, 1, &ms, 4);
        duration = (float)ms;
    } else {
        bb_get_in(ctx, b, 1, &duration, 4);
    }
    if (duration < 0.001f) return CKBR_OK;
    CkParameter *cp = bb_in(ctx, b, 2);
    const CkCurve2d *curve = cp && cp->value ? (const CkCurve2d *)cp->value : NULL;
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
    }
    if (p > 1) {
        ck_activate_output(ctx, b, 0, true);
        if (!loop) {
            p = 1;
            bb_set_local(ctx, b, 0, &p, 4);
            bb_set_out(ctx, b, 0, &p, 4);
            if (k) ck_keyed_set_step(ctx, k, curve ? ck_curve_get_y(curve, p) : p);
            return CKBR_OK;
        }
        p -= (float)(int32_t)p;
    }
    if (k) {
        bb_set_local(ctx, b, 0, &p, 4);
        bb_set_out(ctx, b, 0, &p, 4);
        ck_keyed_set_step(ctx, k, curve ? ck_curve_get_y(curve, p) : p);
    }
    return CKBR_ACTIVATENEXTFRAME;
}

/* ---- TT GetEnvironmentVariable 4c6513e9:20e11177 (thunk 0x1000146a -> 0x1001d220): getenv(pIn Variablename)
   -> pOut, Found; no name or no variable -> Not Found, console message, 0xa008. The game asks for the
   developers' "Gravity" variable; players don't have it. ---- */
static int bb_tt_get_environment_variable(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    const char *name = bb_in_string(ctx, b, 0);
    if (!name) {
        ck_activate_output(ctx, b, 1, true);
        if (ctx->log) ctx->log("No Variablename defined!");
        return 0xa008;
    }
    const char *v = getenv(name);
    if (!v) {
        ck_activate_output(ctx, b, 1, true);
        if (ctx->log) ctx->log("No Variable with the specified name found!");
        return 0xa008;
    }
    set_string_out(ctx, b, 0, v);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- TT_ReplacePath 524a6bcb:66f67774 (thunk 0x100013ac -> 0x1001ee40): PathIDX outside 0..2 -> Failed.
   Otherwise old = dir + Old, new = dir + New, and the first path of the category whose name differs from
   old (strcmp != 0, so in practice the first path) is renamed to new -> Ok; no path -> Failed. Only
   low-memory machines get here (Sounds -> Sounds_low). The VFS has no path categories: Ok, nothing
   renamed. ---- */
static int bb_tt_replace_path(CkContext *ctx, CkBehavior *b)
{
    int32_t idx = 0;
    bb_get_in(ctx, b, 3, &idx, 4);
    ck_activate_output(ctx, b, idx < 0 || idx > 2 ? 1 : 0, true);
    return CKBR_OK;
}

/* ---- TT_GetCurrentDirectory 556e6df3:24207d37 (thunk 0x10001749 -> 0x1001e4a0): GetCurrentDirectoryA ->
   pOut, OK (Fail when empty). The player runs from the game's Bin directory; a fixed path stands in. ---- */
static int bb_tt_get_current_directory(CkContext *ctx, CkBehavior *b)
{
    set_string_out(ctx, b, 0, "C:\\Ballance\\Bin");
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- TT_TextureInfo 7c663b40:2f4b347e (thunk 0x10001203 -> 0x100179b0): pOut MaxVideoMemory (MB, from the
   render driver), NumberOfTextures, UsedSystemMemory (MB: height * bytes per line of every system copy),
   TexturesInVideoMemory, UsedVideoTextureMemory (MB). There is no video memory here: those stay 0, and the
   system copies are RGBA8. Debug display only. ---- */
static int bb_tt_texture_info(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    int32_t zero = 0, count = 0;
    double bytes = 0;
    for (uint32_t i = 0; i < ctx->nobjs; i++) {
        CkObj *o = ctx->objs[i];
        if (!o || o->cid != CKCID_TEXTURE) continue;
        CkTexture *t = (CkTexture *)o;
        count++;
        bytes += (double)t->height * t->width * 4;
    }
    float mb = (float)bytes * 9.5367431640625e-07f, fz = 0;
    bb_set_out(ctx, b, 0, &zero, 4);
    bb_set_out(ctx, b, 1, &count, 4);
    bb_set_out(ctx, b, 2, &mb, 4);
    bb_set_out(ctx, b, 3, &zero, 4);
    bb_set_out(ctx, b, 4, &fz, 4);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Set Clipping Planes 652316a2:01aa09a9 (Cameras FUN_25181670): the target camera's SetFrontPlane
   (+0x1d8, default 0.1) and SetBackPlane (+0x1e0, default 200); no target -> 0xa004 ---- */
static int bb_set_clipping_planes(CkContext *ctx, CkBehavior *b)
{
    CkCamera *k = ck_camera(ctx, bb_target(ctx, b));
    if (!k) return 0xa004;
    float near = 0.1f, far = 200;
    bb_get_in(ctx, b, 0, &near, 4);
    bb_get_in(ctx, b, 1, &far, 4);
    k->znear = near, k->zfar = far;
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- TT_SetMipMapping 38c34dab:5dd03bdf (thunk 0x10001861 -> 0x100176f0): if the target material's texture
   (+0x7c GetTexture) accepts UseMipmap(MipMap Count) (+0x78), SetTextureMinMode (+0x90, Filter Min) and
   SetTextureMagMode (+0x98, Filter Mag). The game: Levelinit "set MipMap", min 6 (linear mipmap linear),
   mag 2 (linear). UseMipmap(count) turns the texture's mip levels on (any nonzero count). ---- */
static int bb_tt_set_mipmapping(CkContext *ctx, CkBehavior *b)
{
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        ck_activate_output(ctx, b, 0, true);
    }
    CkMaterial *m = ck_material(ctx, bb_target(ctx, b));
    int32_t fmin = 0, fmag = 0, count = 1;
    bb_get_in(ctx, b, 0, &fmin, 4);
    bb_get_in(ctx, b, 1, &fmag, 4);
    bb_get_in(ctx, b, 2, &count, 4);
    CkTexture *t = m ? ck_texture(ctx, m->texture) : NULL;
    if (t) {
        if (t->mipmap != (count != 0)) t->mipmap = count != 0, t->version++;
        m->min_filter = (uint8_t)fmin, m->mag_filter = (uint8_t)fmag;
    }
    return CKBR_OK;
}

/* ---- Set MipMap Level 3ebe40f2:3fa41377 (Materials FUN_255058a0): the target texture's UseMipmap(pIn Use
   MipMapping, default TRUE) (+0x78) ---- */
static int bb_set_mipmap_level(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkTexture *t = ck_texture(ctx, bb_target(ctx, b));
    int32_t use = 1;
    bb_get_in(ctx, b, 0, &use, 4);
    if (t && t->mipmap != (use != 0)) t->mipmap = use != 0, t->version++;
    return CKBR_OK;
}

/* ---- Statistics 5fb70201:65595af3 (Visuals FUN_257943b0): On -> Exit On, profiling on; Off -> Exit Off,
   profiling off, return 0. While active: pOut Frame Rate = 1000 / frame time when local 0 says so; the
   other outputs (profiler times and counters, chosen by the bits of locals 1-3) aren't measured here and
   keep their values. Stays active. ---- */
static int bb_statistics(CkContext *ctx, CkBehavior *b)
{
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        ck_activate_output(ctx, b, 0, true);
    } else if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    int32_t fps = 0;
    bb_get_local(ctx, b, 0, &fps, 4);
    if (fps && ctx->delta_ms > 0) {
        float r = 1000.0f / ctx->delta_ms;
        bb_set_out(ctx, b, 0, &r, 4);
    }
    return CKBR_ACTIVATENEXTFRAME;
}

/* ---- Create System Font 936334fc:f243684f (Interface FUN_25389a60, docs/fonts.md 6.3): renders a Windows
   font into a texture with GDI and makes a texture font of it -> pOut Font Created, Success; failures return
   0xa008 without Error. No GDI here: the font is an alias of the first built font (its texture and glyph
   table under the new name). Only the debug script "create DebugFont" uses it. ---- */
static int bb_create_system_font(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    const char *name = bb_in_string(ctx, b, 0);
    int32_t sys = -1;
    bb_get_in(ctx, b, 1, &sys, 4);
    if (!name || sys == -1) return 0xa008;
    CkFont *src = NULL;
    for (uint32_t i = 1; i <= ctx->nfonts && !src; i++) src = ck_font_get(ctx, (int32_t)i);
    if (!src) return 0xa008;
    CkFont copy = *src;
    int32_t idx = ck_font_create(ctx, name, copy.texture, copy.rect, copy.counts, (copy.flags & 1) != 0, copy.first, 0.3f);
    CkFont *f = ck_font_get(ctx, idx);
    if (!f) return 0xa008;
    memcpy(f->g, copy.g, sizeof f->g);
    bb_set_out(ctx, b, 0, &idx, 4);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

BB_DECL(d_tt_counter, 750f20fc, 3741186d, "TT_Counter", bb_tt_counter);
BB_DECL(d_tt_proximity_volume_control, 38571b61, 64cd2174, "TT ProximityVolumeControl", bb_tt_proximity_volume_control);
BB_DECL_CB(d_input_string, 693e7e4b, 100104c4, "Input String", bb_input_string, cb_input_string);
BB_DECL_CB(d_tt_input_string, 52fd6294, 612f51a5, "TT InputString", bb_tt_input_string, cb_tt_input_string);
BB_DECL(d_play_global_animation, 1c9236e1, 42f40996, "Play Global Animation", bb_play_global_animation);
BB_DECL(d_tt_get_environment_variable, 4c6513e9, 20e11177, "TT GetEnvironmentVariable", bb_tt_get_environment_variable);
BB_DECL(d_tt_replace_path, 524a6bcb, 66f67774, "TT_ReplacePath", bb_tt_replace_path);
BB_DECL(d_tt_get_current_directory, 556e6df3, 24207d37, "TT_GetCurrentDirectory", bb_tt_get_current_directory);
BB_DECL(d_tt_texture_info, 7c663b40, 2f4b347e, "TT_TextureInfo", bb_tt_texture_info);
BB_DECL(d_set_clipping_planes, 652316a2, 01aa09a9, "Set Clipping Planes", bb_set_clipping_planes);
BB_DECL(d_tt_set_mipmapping, 38c34dab, 5dd03bdf, "TT_SetMipMapping", bb_tt_set_mipmapping);
BB_DECL(d_set_mipmap_level, 3ebe40f2, 3fa41377, "Set MipMap Level", bb_set_mipmap_level);
BB_DECL(d_statistics, 5fb70201, 65595af3, "Statistics", bb_statistics);
BB_DECL(d_create_system_font, 936334fc, f243684f, "Create System Font", bb_create_system_font);

const CkBBDecl *const bb_tt_misc[] = {
    &d_tt_counter, &d_tt_proximity_volume_control, &d_input_string, &d_tt_input_string, &d_play_global_animation,
    &d_tt_get_environment_variable, &d_tt_replace_path, &d_tt_get_current_directory, &d_tt_texture_info,
    &d_set_clipping_planes, &d_tt_set_mipmapping, &d_set_mipmap_level, &d_statistics, &d_create_system_font};
const unsigned bb_tt_misc_count = sizeof bb_tt_misc / sizeof *bb_tt_misc;
