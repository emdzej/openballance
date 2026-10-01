/* Logics.dll Building Blocks (flow control, messages, arrays, interpolators, iterators). */
#include "bb.h"
#include "../ck/ck_curve.h"
#include "../ck/ck_3d.h"
#include "../vfs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- Op 2d5d6d01:6a353eb0 (execute FUN_2548c460, callback FUN_2548c4f0) ----
   Local 0 holds the operation function (+ swap flag) that the callback resolves from the operation GUID in
   locals 1-2 and the types of pIn 0, pIn 1 and pOut 0; the retry swaps the inputs. */
typedef struct {
    CkOpFn fn;
    bool swap, resolved;
} OpState;

static int bb_op(CkContext *ctx, CkBehavior *b)
{
    OpState *st = b->bb_state;
    if (!st) st = b->bb_state = calloc(1, sizeof *st);
    CkParameter *pin0 = b->pin.n > 0 ? ck_param(ctx, b->pin.v[0]) : NULL;
    CkParameter *pin1 = b->pin.n > 1 ? ck_param(ctx, b->pin.v[1]) : NULL;
    CkParameter *out = bb_out(ctx, b, 0);
    if (!st->resolved) {
        int32_t g[2] = {0, 0};
        bb_get_local(ctx, b, 1, &g[0], 4);
        bb_get_local(ctx, b, 2, &g[1], 4);
        CkGuid op = {(uint32_t)g[0], (uint32_t)g[1]};
        CkGuid t0 = pin0 ? pin0->type : CKPGUID_NONE, t1 = pin1 ? pin1->type : CKPGUID_NONE;
        CkGuid to = out ? out->type : CKPGUID_NONE;
        st->fn = ck_op_function(op, to, t0, t1);
        if (!st->fn && (st->fn = ck_op_function(op, to, t1, t0))) st->swap = true;
        st->resolved = true;
        if (!st->fn && ctx->log) {
            char m[160];
            snprintf(m, sizeof m, "no operation %08x:%08x for %08x:%08x (%08x:%08x, %08x:%08x)", op.a, op.b, to.a, to.b, t0.a, t0.b, t1.a, t1.b);
            ctx->log(m);
        }
    }
    if (st->fn && out) {
        CkParameter *a = bb_in(ctx, b, 0), *c = bb_in(ctx, b, 1);
        if (st->swap) st->fn(ctx, out, c, a);
        else st->fn(ctx, out, a, c);
        ck_output_set(ctx, b, 0, out->value, out->size);   /* DataChanged */
    }
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}
BB_DECL(d_op, 2d5d6d01, 6a353eb0, "Op", bb_op);

/* ---- Binary Switch eb506901:984afccc (FUN_254846d0) ---- */
static int bb_binary_switch(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    int32_t cond = 1;
    bb_get_in(ctx, b, 0, &cond, 4);
    ck_activate_output(ctx, b, cond ? 0 : 1, true);
    return CKBR_OK;
}
BB_DECL(d_binary_switch, eb506901, 984afccc, "Binary Switch", bb_binary_switch);

/* ---- Identity (FUN_2548dab0): every pIn copied to the pOut of the same index ---- */
static int bb_identity(CkContext *ctx, CkBehavior *b)
{
    if (b->pin.n == b->pout.n) {
        for (uint32_t i = 0; i < b->pin.n; i++) {
            CkParameter *p = bb_in(ctx, b, i);
            if (p) ck_output_set(ctx, b, i, p->value, p->size);
        }
        ck_activate_input(ctx, b, 0, false);
        ck_activate_output(ctx, b, 0, true);
    }
    /* the original throws "Wrong number of Input/Output parameters" otherwise */
    return CKBR_OK;
}

/* ---- Nop (FUN_25485790): all inputs off, all outputs on ---- */
static int bb_nop(CkContext *ctx, CkBehavior *b)
{
    for (uint32_t i = 0; i < b->in.n; i++) ck_activate_input(ctx, b, i, false);
    for (uint32_t i = 0; i < b->out.n; i++) ck_activate_output(ctx, b, i, true);
    return CKBR_OK;
}

/* ---- Delayer (FUN_2549e180): Elapsed Time accumulates DeltaTime until Time to Wait ---- */
static int bb_delayer(CkContext *ctx, CkBehavior *b)
{
    float zero = 0;
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        bb_set_out(ctx, b, 0, &zero, 4);
    }
    float wait = 0, elapsed = 0;
    bb_get_in(ctx, b, 0, &wait, 4);
    bb_get_out(ctx, b, 0, &elapsed, 4);
    elapsed += ctx->delta_ms;
    bb_set_out(ctx, b, 0, &elapsed, 4);
    if (wait <= elapsed) {
        ck_activate_output(ctx, b, 0, true);
        return CKBR_OK;
    }
    return CKBR_ACTIVATENEXTFRAME;
}

/* ---- Sequencer, version 2 (FUN_254864c0): inputs Reset, In; outputs Exit Reset, Out 1..n;
   pOut Current (-1 after reset) ---- */
static int bb_sequencer(CkContext *ctx, CkBehavior *b)
{
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        int32_t cur = 0;
        bb_get_out(ctx, b, 0, &cur, 4);
        cur++;
        if ((int32_t)b->out.n - 1 <= cur) cur = 0;
        bb_set_out(ctx, b, 0, &cur, 4);
        ck_activate_output(ctx, b, (uint32_t)cur + 1, true);
    }
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        int32_t cur = -1;
        bb_set_out(ctx, b, 0, &cur, 4);
        ck_activate_output(ctx, b, 0, true);
    }
    return CKBR_OK;
}

/* ---- Test (FUN_25483a80): Comparison Operator (Equal=1 .. Greater or equal=6) on A and B, as floats,
   or as strings if either is a string; integers and booleans compare as floats ---- */
static bool cmp_float(int op, float a, float c)
{
    switch (op) {
    case 1: return a == c;
    case 2: return a != c;
    case 3: return a < c;
    case 4: return a <= c;
    case 5: return a > c;
    case 6: return a >= c;
    }
    return false;
}
static bool cmp_string(int op, const char *a, const char *c)
{
    int r = strcmp(a ? a : "", c ? c : "");
    switch (op) {
    case 1: return r == 0;
    case 2: return r != 0;
    case 3: return r < 0;
    case 4: return r <= 0;
    case 5: return r > 0;
    case 6: return r >= 0;
    }
    return false;
}
static int bb_test(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    int32_t op = 1;
    bb_get_in(ctx, b, 0, &op, 4);
    float f[2] = {0, 0};
    const char *s[2] = {NULL, NULL};
    bool strings = false;
    for (int k = 0; k < 2; k++) {
        CkParameter *pin = (uint32_t)(k + 1) < b->pin.n ? ck_param(ctx, b->pin.v[k + 1]) : NULL;
        CkGuid t = pin ? pin->type : CKPGUID_NONE;
        CkParameter *p = bb_in(ctx, b, (uint32_t)k + 1);
        if (ck_type_derives(t, CKPGUID_FLOAT)) {
            if (p && p->value && p->size >= 4) memcpy(&f[k], p->value, 4);
        } else if (ck_guid_eq(t, CKPGUID_STRING)) {
            s[k] = p && p->value ? (const char *)p->value : NULL;
            strings = true;
        } else {
            int32_t v = 0;
            if (p && p->value && p->size >= 4) memcpy(&v, p->value, 4);
            f[k] = ck_guid_eq(t, CKPGUID_BOOL) ? (v ? 1.0f : 0.0f) : (float)v;
        }
    }
    if (op > 0) {
        bool r = strings ? cmp_string(op, s[0], s[1]) : cmp_float(op, f[0], f[1]);
        ck_activate_output(ctx, b, r ? 0 : 1, true);
    }
    return CKBR_OK;
}

/* ---- Binary Memory (FUN_25484550): input 0 = True, 1 = False; remembers it in pOut 0 ---- */
static int bb_binary_memory(CkContext *ctx, CkBehavior *b)
{
    bool on = ck_input_active(ctx, b, 0);
    int32_t v = on ? 1 : 0;
    ck_activate_input(ctx, b, on ? 0 : 1, false);
    ck_activate_output(ctx, b, on ? 0 : 1, true);
    bb_set_out(ctx, b, 0, &v, 4);
    return CKBR_OK;
}

/* ---- Parameter Selector (FUN_25485be0): the first active input selects pIn i -> pOut 0 ---- */
static int bb_parameter_selector(CkContext *ctx, CkBehavior *b)
{
    for (uint32_t i = 0; i < b->in.n; i++) {
        if (!ck_input_active(ctx, b, i)) continue;
        ck_activate_input(ctx, b, i, false);
        CkParameter *p = bb_in(ctx, b, i);
        if (p) bb_set_out(ctx, b, 0, p->value, p->size);
        break;
    }
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* Cell -> output parameter, only when the types agree (Get Cell FUN_25497330, row helper FUN_2549ac10). */
static void cell_to_param(CkContext *ctx, CkDataArray *a, uint32_t col, const CkCell *c, CkBehavior *b, uint32_t out)
{
    CkParameter *o = bb_out(ctx, b, out);
    if (!o) return;
    switch (a->cols[col].type) {
    case CKARRAYTYPE_INT:
        if (ck_guid_eq(o->type, CKPGUID_INT)) bb_set_out(ctx, b, out, &c->i, 4);
        break;
    case CKARRAYTYPE_FLOAT:
        if (ck_guid_eq(o->type, CKPGUID_FLOAT)) bb_set_out(ctx, b, out, &c->f, 4);
        break;
    case CKARRAYTYPE_STRING:
        if (ck_guid_eq(o->type, CKPGUID_STRING)) {
            const char *s = c->s ? c->s : "";
            bb_set_out(ctx, b, out, s, (uint32_t)strlen(s) + 1);
        }
        break;
    case CKARRAYTYPE_OBJECT: {
        uint32_t cid = ck_type_class(o->type);
        if (cid) {
            CkObj *x = ck_obj(ctx, c->obj);
            CkId v = x && ck_class_derives(x->cid, cid) ? c->obj : 0;
            bb_set_out(ctx, b, out, &v, 4);
        }
        break;
    }
    case CKARRAYTYPE_PARAMETER: {
        CkParameter *cp = ck_param(ctx, c->obj);
        if (cp) bb_set_out(ctx, b, out, cp->value, cp->size);
        break;
    }
    }
}

static void row_to_outputs(CkContext *ctx, CkDataArray *a, uint32_t row, CkBehavior *b, uint32_t first)
{
    for (uint32_t col = 0; col < a->ncols; col++) {
        CkCell *c = ck_array_cell(a, row, col);
        if (c) cell_to_param(ctx, a, col, c, b, first + col);
    }
}

/* FUN_2549add0: input parameters -> row cells, only when the input type matches the column type */
static void inputs_to_row(CkContext *ctx, CkDataArray *a, uint32_t row, CkBehavior *b, uint32_t first)
{
    for (uint32_t col = 0; col < a->ncols; col++) {
        uint32_t i = first + col;
        CkParameter *pin = i < b->pin.n ? ck_param(ctx, b->pin.v[i]) : NULL;
        CkParameter *src = bb_in(ctx, b, i);
        CkCell *c = ck_array_cell(a, row, col);
        if (!pin || !src || !c) continue;
        switch (a->cols[col].type) {
        case CKARRAYTYPE_INT:
            if (ck_guid_eq(pin->type, CKPGUID_INT)) { c->i = 0; if (src->value && src->size >= 4) memcpy(&c->i, src->value, 4); }
            break;
        case CKARRAYTYPE_FLOAT:
            if (ck_guid_eq(pin->type, CKPGUID_FLOAT)) { c->f = 0; if (src->value && src->size >= 4) memcpy(&c->f, src->value, 4); }
            break;
        case CKARRAYTYPE_STRING:
            if (ck_guid_eq(pin->type, CKPGUID_STRING)) {
                free(c->s);
                c->s = src->value ? strdup((const char *)src->value) : NULL;
            }
            break;
        case CKARRAYTYPE_OBJECT: {
            CkGuid be = {0x71d80779u, 0x402f42f3u};
            if (ck_guid_eq(pin->type, be)) { c->obj = 0; if (src->value && src->size >= 4) memcpy(&c->obj, src->value, 4); }
            break;
        }
        case CKARRAYTYPE_PARAMETER:
            if (ck_guid_eq(pin->type, a->cols[col].param_type)) {
                CkParameter *cp = ck_param(ctx, c->obj);
                if (cp) ck_param_set(cp, src->value, src->size);
            }
            break;
        }
    }
}

/* FUN_2549bc00: key value for FindRowIndex from input parameter i, typed by the column. Returns the size
   (strings: length + 1) or 0xffffffff when the input type doesn't fit the column. */
static uint32_t key_from_input(CkContext *ctx, CkDataArray *a, CkBehavior *b, uint32_t col, uint32_t i, uint32_t *key, const void **keyptr)
{
    CkParameter *pin = i < b->pin.n ? ck_param(ctx, b->pin.v[i]) : NULL;
    if (!pin) return 0;
    *key = 0;
    *keyptr = NULL;
    CkParameter *src = bb_in(ctx, b, i);
    if (col >= a->ncols) return 0xffffffff;
    switch (a->cols[col].type) {
    case CKARRAYTYPE_INT:
        if (!ck_guid_eq(pin->type, CKPGUID_INT)) return 0xffffffff;
        break;
    case CKARRAYTYPE_FLOAT:
        if (!ck_guid_eq(pin->type, CKPGUID_FLOAT)) return 0xffffffff;
        break;
    case CKARRAYTYPE_STRING:
        if (!ck_guid_eq(pin->type, CKPGUID_STRING) || !src || !src->value) return 0xffffffff;
        *keyptr = src->value;
        return (uint32_t)strlen((const char *)src->value) + 1;
    case CKARRAYTYPE_OBJECT: {
        CkGuid be = {0x71d80779u, 0x402f42f3u};
        if (!ck_guid_eq(pin->type, be)) return 0xffffffff;
        break;
    }
    case CKARRAYTYPE_PARAMETER:
        if (!ck_guid_eq(pin->type, a->cols[col].param_type)) return 0xffffffff;
        *keyptr = src ? src->value : NULL;
        return src ? src->size : 0;
    default:
        return 0xffffffff;
    }
    if (src && src->value && src->size >= 4) memcpy(key, src->value, 4);
    return 0xffffffff;
}

/* ---- Get Cell (FUN_25497330) ---- */
static int bb_get_cell(CkContext *ctx, CkBehavior *b)
{
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a) return CKBR_OK;   /* original: 0xa004 (CKBR_OWNERERROR) */
    int32_t row = 0, col = 0;
    bb_get_in(ctx, b, 0, &row, 4);
    bb_get_in(ctx, b, 1, &col, 4);
    CkCell *c = row >= 0 && col >= 0 ? ck_array_cell(a, (uint32_t)row, (uint32_t)col) : NULL;
    ck_activate_input(ctx, b, 0, false);
    if (!c) {
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    cell_to_param(ctx, a, (uint32_t)col, c, b, 0);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Set Cell (FUN_25498470) ---- */
static int bb_set_cell(CkContext *ctx, CkBehavior *b)
{
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a) return CKBR_OK;
    int32_t row = 0, col = 0;
    bb_get_in(ctx, b, 0, &row, 4);
    bb_get_in(ctx, b, 1, &col, 4);
    bool ok = row >= 0 && col >= 0 && ck_array_set_from_param(ctx, a, (uint32_t)row, (uint32_t)col, bb_in(ctx, b, 2));
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, ok ? 0 : 1, true);
    return CKBR_OK;
}

/* ---- Get Row (FUN_254978a0) ---- */
static int bb_get_row(CkContext *ctx, CkBehavior *b)
{
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a) return CKBR_OK;
    int32_t row = 0;
    bb_get_in(ctx, b, 0, &row, 4);
    ck_activate_input(ctx, b, 0, false);
    if (row < 0 || (uint32_t)row >= a->nrows) {
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    row_to_outputs(ctx, a, (uint32_t)row, b, 0);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Set Row (FUN_254986d0): setting 0 "unique key" rejects a key already used by another row ---- */
static int bb_set_row(CkContext *ctx, CkBehavior *b)
{
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a) return CKBR_OK;
    int32_t row = 0, unique = 1;
    bb_get_in(ctx, b, 0, &row, 4);
    bb_get_local(ctx, b, 0, &unique, 4);
    if (unique && a->key_column >= 0) {
        uint32_t key;
        const void *kp;
        uint32_t size = key_from_input(ctx, a, b, (uint32_t)a->key_column, (uint32_t)a->key_column + 1, &key, &kp);
        int32_t found = ck_array_find_row(ctx, a, (uint32_t)a->key_column, 1, key, kp, size, 0);
        if (found != -1 && found != row) {
            ck_activate_input(ctx, b, 0, false);
            ck_activate_output(ctx, b, 1, true);
            return CKBR_OK;
        }
    }
    if (row >= 0 && (uint32_t)row < a->nrows) inputs_to_row(ctx, a, (uint32_t)row, b, 1);
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Add Row (FUN_25495990): setting 0 "Unique Key Column" ---- */
static int bb_add_row(CkContext *ctx, CkBehavior *b)
{
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a) return CKBR_OK;
    int32_t unique = 1;
    bb_get_local(ctx, b, 0, &unique, 4);
    if (unique && a->key_column >= 0) {
        uint32_t key;
        const void *kp;
        uint32_t size = key_from_input(ctx, a, b, (uint32_t)a->key_column, (uint32_t)a->key_column, &key, &kp);
        if (ck_array_find_row(ctx, a, (uint32_t)a->key_column, 1, key, kp, size, 0) != -1) {
            ck_activate_input(ctx, b, 0, false);
            ck_activate_output(ctx, b, 1, true);
            return CKBR_OK;
        }
    }
    uint32_t row = ck_array_add_row(ctx, a);
    inputs_to_row(ctx, a, row, b, 0);
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Remove Row If (FUN_25496400): pIn Column, Operation, Value; pOut count removed ---- */
static int bb_remove_row_if(CkContext *ctx, CkBehavior *b)
{
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a) return CKBR_OK;
    int32_t col = 0, op = 1, removed = 0, r = 0;
    bb_get_in(ctx, b, 0, &col, 4);
    bb_get_in(ctx, b, 1, &op, 4);
    uint32_t key;
    const void *kp;
    uint32_t size = key_from_input(ctx, a, b, (uint32_t)col, 2, &key, &kp);
    for (;;) {
        r = ck_array_find_row(ctx, a, (uint32_t)col, op, key, kp, size, r);
        if (r == -1) break;
        ck_array_remove_row(ctx, a, (uint32_t)r);
        removed++;
        if ((uint32_t)r >= a->nrows) break;
    }
    bb_set_out(ctx, b, 0, &removed, 4);
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, removed ? 0 : 1, true);
    return CKBR_OK;
}

/* ---- Row Search 78863443:45af59b6 (FUN_25497fb0; the callback FUN_254980a0 retypes pIn 3 to the column's
   type on attach / load, already so in the files): CKDataArray::FindRowIndex(pIn Column, pIn Operation
   (default 1 equal), pIn 3 Reference Value, from pIn First Search Row) -> First Row Found and Found, else
   Not Found (pOut untouched) ---- */
static int bb_row_search(CkContext *ctx, CkBehavior *b)
{
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a) return CKBR_OK;
    int32_t col = 0, first = 0, op = 1;
    bb_get_in(ctx, b, 0, &col, 4);
    bb_get_in(ctx, b, 1, &first, 4);
    bb_get_in(ctx, b, 2, &op, 4);
    uint32_t key;
    const void *kp;
    uint32_t size = key_from_input(ctx, a, b, (uint32_t)col, 3, &key, &kp);
    int32_t r = ck_array_find_row(ctx, a, (uint32_t)col, op, key, kp, size, first);
    ck_activate_input(ctx, b, 0, false);
    if (r < 0) {
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    bb_set_out(ctx, b, 0, &r, 4);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Clear Array (FUN_25495bd0) ---- */
static int bb_clear_array(CkContext *ctx, CkBehavior *b)
{
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a) return CKBR_OK;
    ck_array_clear(ctx, a);
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Iterator (FUN_25499440): rows Starting Row .. Ending Row (-1 = last), optionally backward;
   Loop Out fills the outputs from the row (FUN_2549ac10, from pOut 1) ---- */
static int bb_iterator(CkContext *ctx, CkBehavior *b)
{
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a) return CKBR_OK;
    int32_t n = (int32_t)a->nrows, start = 0, end = -1, back = 0, first = 0, cur;
    bb_get_in(ctx, b, 0, &start, 4);
    if (start >= n) {
        ck_activate_output(ctx, b, 0, true);
        return CKBR_OK;
    }
    bb_get_in(ctx, b, 1, &end, 4);
    if (end == -1) end = n - 1;
    bb_get_in(ctx, b, 2, &back, 4);
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        cur = back ? end : start;
        bb_set_out(ctx, b, 0, &cur, 4);
        first = 1;
    } else {
        ck_activate_input(ctx, b, 1, false);
        cur = 0;
        bb_get_out(ctx, b, 0, &cur, 4);
        cur = back ? cur - 1 : cur + 1;
        if (cur < 0) cur = n - 1;              /* unsigned compare in the original */
        else if (cur >= n) cur = 0;
        bb_set_out(ctx, b, 0, &cur, 4);
    }
    int32_t stop;
    if (!back) {
        stop = end + 1;
        if (stop >= n) stop = 0;
    } else {
        stop = start - 1;
        if (stop == -1) stop = n - 1;
    }
    if ((!first || n == 0) && cur == stop) {
        ck_activate_output(ctx, b, 0, true);
        return CKBR_OK;
    }
    row_to_outputs(ctx, a, (uint32_t)cur, b, 1);
    ck_activate_output(ctx, b, 1, true);
    return CKBR_OK;
}

/* ---- Group Iterator (FUN_25489340): pOut Element, Index (or local 0 when there is no Index output) ---- */
static int bb_group_iterator(CkContext *ctx, CkBehavior *b)
{
    CkObj *g = ck_obj(ctx, bb_in_object(ctx, b, 0));
    if (!g || g->cid != CKCID_GROUP) {
        ck_activate_output(ctx, b, 0, true);
        return CKBR_OK;   /* original: 0xa008 */
    }
    int32_t i = 0;
    if (!bb_get_out(ctx, b, 1, &i, 4)) bb_get_local(ctx, b, 0, &i, 4);
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        i = 0;
    } else {
        ck_activate_input(ctx, b, 1, false);
        i++;
    }
    CkGroup *grp = (CkGroup *)g;
    if (i >= (int32_t)grp->members.n) {
        ck_activate_output(ctx, b, 0, true);
        return CKBR_OK;
    }
    CkId e = grp->members.v[i];
    bb_set_out(ctx, b, 0, &e, 4);
    if (b->pout.n > 1) bb_set_out(ctx, b, 1, &i, 4);
    else bb_set_local(ctx, b, 0, &i, 4);
    ck_activate_output(ctx, b, 1, true);
    return CKBR_OK;
}

/* ---- Wait For All (FUN_25487570): output once every input is active ---- */
static int bb_wait_for_all(CkContext *ctx, CkBehavior *b)
{
    for (uint32_t i = 0; i < b->in.n; i++)
        if (!ck_input_active(ctx, b, i)) return CKBR_ACTIVATENEXTFRAME;
    for (uint32_t i = 0; i < b->in.n; i++) ck_activate_input(ctx, b, i, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Counter (FUN_2549d100): Count, Start Index, Step; pOut Value ---- */
static int bb_counter(CkContext *ctx, CkBehavior *b)
{
    int32_t count = 10, start = 1, step = 1, v = 0;
    bb_get_in(ctx, b, 0, &count, 4);
    bb_get_in(ctx, b, 1, &start, 4);
    bb_get_in(ctx, b, 2, &step, 4);
    if (!step) step = 1;
    bb_get_out(ctx, b, 0, &v, 4);
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        v = start - step;
    } else {
        ck_activate_input(ctx, b, 1, false);
    }
    v += step;
    bb_set_out(ctx, b, 0, &v, 4);
    ck_activate_output(ctx, b, count <= (v - start) / step ? 0 : 1, true);
    return CKBR_OK;
}

/* ---- Send Message (FUN_25488060): pIn Message, Dest; extra pIns become message parameters ---- */
static int bb_send_message(CkContext *ctx, CkBehavior *b)
{
    CkId sender = bb_target(ctx, b);
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    int32_t type = -1;
    bb_get_in(ctx, b, 0, &type, 4);
    CkMessage *m = ck_send_message(ctx, type, 2, bb_in_object(ctx, b, 1), sender);
    if (!m) return CKBR_OK;   /* original throws "Message wasn't send normally" */
    for (uint32_t i = 2; i < b->pin.n; i++) {
        CkParameter *p = bb_in(ctx, b, i);
        ck_ids_push(&m->params, p ? p->h.id : 0);
    }
    return CKBR_OK;
}

/* ---- Send Message To Group (FUN_25488270): SendMessageGroup(pIn Message, pIn Group) from the target, the
   remaining inputs (from 2; from 1 before version 2.0) as the message's parameters. No group -> 0xa008
   (Out still activated). ---- */
static int bb_send_message_to_group(CkContext *ctx, CkBehavior *b)
{
    CkId sender = bb_target(ctx, b);
    if (!ck_obj(ctx, sender)) return CKBR_OK;
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    int32_t type = -1;
    bb_get_in(ctx, b, 0, &type, 4);
    CkObj *g = ck_obj(ctx, bb_in_object(ctx, b, 1));
    if (!g) return CKBR_OK;
    CkMessage *m = ck_send_message(ctx, type, 3, g->id, sender);
    if (!m) return CKBR_OK;   /* original throws "Message wasn't send normally" */
    for (uint32_t i = b->proto_version < 0x20000 ? 1 : 2; i < b->pin.n; i++) {
        CkParameter *p = bb_in(ctx, b, i);
        ck_ids_push(&m->params, p ? p->h.id : 0);
    }
    return CKBR_OK;
}

/* ---- Wait Message (FUN_254887a0): In registers a wait on the target, Stop (input 1) unregisters ---- */
static int bb_wait_message(CkContext *ctx, CkBehavior *b)
{
    CkId target = bb_target(ctx, b);
    int32_t type = -1;
    bb_get_in(ctx, b, 0, &type, 4);
    if (type >= 0) {
        if (ck_input_active(ctx, b, 0)) {
            ck_activate_input(ctx, b, 0, false);
            ck_register_wait(ctx, type, b, 0, target);
        }
        if (ck_input_active(ctx, b, 1)) {
            ck_activate_input(ctx, b, 1, false);
            ck_unregister_wait(ctx, type, b, 0);
        }
    }
    return CKBR_OK;
}

/* ---- Iterator If (FUN_25499770): rows whose Column compares (Operator) with Reference Value (pIn 2);
   pOut Index + the row's cells from pOut 1 ---- */
static int bb_iterator_if(CkContext *ctx, CkBehavior *b)
{
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a) return CKBR_OK;
    int32_t start;
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        start = 0;
    } else {
        ck_activate_input(ctx, b, 1, false);
        start = 0;
        bb_get_out(ctx, b, 0, &start, 4);
        start++;
    }
    if (start >= (int32_t)a->nrows) {
        ck_activate_output(ctx, b, 0, true);
        return CKBR_OK;
    }
    int32_t col = 0, op = 1;
    bb_get_in(ctx, b, 0, &col, 4);
    bb_get_in(ctx, b, 1, &op, 4);
    uint32_t key;
    const void *kp;
    uint32_t size = key_from_input(ctx, a, b, (uint32_t)col, 2, &key, &kp);
    int32_t row = ck_array_find_row(ctx, a, (uint32_t)col, op, key, kp, size, start);
    if (row < 0) {
        ck_activate_output(ctx, b, 0, true);
        return CKBR_OK;
    }
    row_to_outputs(ctx, a, (uint32_t)row, b, 1);
    bb_set_out(ctx, b, 0, &row, 4);
    ck_activate_output(ctx, b, 1, true);
    return CKBR_OK;
}

/* ---- Insert Column (FUN_25495d80): pIn Index (-1 = append), Name, Column Type, Parameter Type ---- */
static int bb_insert_column(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a) return CKBR_OK;
    int32_t index = -1, type = 1;
    bb_get_in(ctx, b, 0, &index, 4);
    const char *name = bb_in_string(ctx, b, 1);
    bb_get_in(ctx, b, 2, &type, 4);
    CkGuid ptype = {0, 0};
    if (type == CKARRAYTYPE_PARAMETER) {
        CkParameter *p = bb_in(ctx, b, 3);
        if (p && p->value && p->size >= 8) memcpy(&ptype, p->value, 8);   /* a parameter-type value holds its GUID */
    }
    ck_array_insert_column(ctx, a, index, (uint32_t)type, name, ptype);
    return CKBR_OK;
}

/* ---- Remove Column (FUN_25496240) ---- */
static int bb_remove_column(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a) return CKBR_OK;
    int32_t index = -1;
    bb_get_in(ctx, b, 0, &index, 4);
    ck_array_remove_column(ctx, a, index);
    return CKBR_OK;
}

/* ---- Get SubString (FUN_2549ee70): pIn String, Start, Length (0 = to the end) ---- */
static int bb_get_substring(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    const char *str = bb_in_string(ctx, b, 0);
    if (!str) return CKBR_OK;
    int32_t n = (int32_t)strlen(str), start = 1, len = 0;
    bb_get_in(ctx, b, 1, &start, 4);
    bb_get_in(ctx, b, 2, &len, 4);
    if (len == 0 || n < len + start) len = n - start;
    if (start < 0) {
        len += start;
        start = 0;
    }
    char *out = malloc((len > 0 ? (size_t)len : 0) + 1);
    if (len > 0 && start < n) {
        strncpy(out, str + start, (size_t)len);
        out[len] = 0;
    } else {
        out[0] = 0;
    }
    bb_set_out(ctx, b, 0, out, (uint32_t)strlen(out) + 1);
    free(out);
    return CKBR_OK;
}

/* ---- Broadcast Message (FUN_25487890): pIn Message, Class (version 2; default Behavioral Object);
   further pIns become message parameters ---- */
static int bb_broadcast_message(CkContext *ctx, CkBehavior *b)
{
    CkId sender = bb_target(ctx, b);
    if (!sender) return CKBR_OK;
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    int32_t type = -1, cls = CKCID_BEOBJECT;
    uint32_t first = 1;
    bb_get_in(ctx, b, 0, &type, 4);
    if (b->proto_version > 0x10000) {
        bb_get_in(ctx, b, 1, &cls, 4);
        first = 2;
    }
    CkMessage *m = ck_send_message(ctx, type, 1, (uint32_t)cls, sender);
    if (!m) return CKBR_OK;
    for (uint32_t i = first; i < b->pin.n; i++) {
        CkParameter *p = bb_in(ctx, b, i);
        ck_ids_push(&m->params, p ? p->h.id : 0);
    }
    return CKBR_OK;
}

/* ---- Mini Calculator (FUN_2548ddd0): pIn Operator (a string: plus, minus, times or slash sign; or enum 1-4), a, b; pOut x ---- */
static int bb_mini_calculator(CkContext *ctx, CkBehavior *b)
{
    float a = 0, c = 0, x = 0;
    bb_get_in(ctx, b, 1, &a, 4);
    bb_get_in(ctx, b, 2, &c, 4);
    CkParameter *pin = b->pin.n ? ck_param(ctx, b->pin.v[0]) : NULL;
    int op = 0;
    if (pin && ck_guid_eq(pin->type, CKPGUID_STRING)) {
        const char *s = bb_in_string(ctx, b, 0);
        char ch = s ? s[0] : 0;
        op = ch == '+' ? 1 : ch == '-' ? 2 : ch == '*' ? 3 : ch == '/' ? 4 : 0;
    } else {
        int32_t v = 1;
        bb_get_in(ctx, b, 0, &v, 4);
        op = v;
    }
    switch (op) {
    case 1: x = c + a; break;
    case 2: x = a - c; break;
    case 3: x = c * a; break;
    case 4: x = a / c; break;
    }
    bb_set_out(ctx, b, 0, &x, 4);
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Threshold (FUN_254926b0): outputs X < MIN, X > MAX, MIN < X < MAX; pOut the clamped X ---- */
static int bb_threshold(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    float x = 0, lo = 0, hi = 0, out;
    int which;
    bb_get_in(ctx, b, 0, &x, 4);
    bb_get_in(ctx, b, 1, &lo, 4);
    if (lo <= x) {
        bb_get_in(ctx, b, 2, &hi, 4);
        if (x <= hi) which = 2, out = x;
        else which = 1, out = hi;
    } else {
        which = 0, out = lo;
    }
    ck_activate_output(ctx, b, (uint32_t)which, true);
    bb_set_out(ctx, b, 0, &out, 4);
    return CKBR_OK;
}

/* ---- Switch On Parameter (FUN_25486ef0): pIn Test, Pin 1..n; setting Float epsilon. Out i for the first
   pin equal to Test (same type only): floats within +-epsilon (exact if epsilon is 0), strings by strcmp,
   booleans by truth, other types byte for byte; None otherwise ---- */
static int bb_switch_on_parameter(CkContext *ctx, CkBehavior *b)
{
    CkParameter *t0 = b->pin.n ? ck_param(ctx, b->pin.v[0]) : NULL;
    CkParameter *test = bb_in(ctx, b, 0);
    float eps = 0;
    bb_get_local(ctx, b, 0, &eps, 4);
    if (t0 && test && test->value) {
        bool is_float = ck_type_derives(t0->type, CKPGUID_FLOAT);
        for (uint32_t i = 1; i < b->pin.n; i++) {
            CkParameter *pi = ck_param(ctx, b->pin.v[i]);
            if (!pi || !ck_guid_eq(pi->type, t0->type)) continue;
            CkParameter *v = bb_in(ctx, b, i);
            if (!v || !v->value) continue;
            bool hit;
            if (is_float && eps != 0.0f) {
                float a, c;
                memcpy(&a, test->value, 4);
                memcpy(&c, v->value, 4);
                hit = a - eps <= c && c < a + eps;
            } else if (ck_guid_eq(t0->type, CKPGUID_STRING)) {
                hit = !strcmp((const char *)v->value, (const char *)test->value);
            } else if (ck_guid_eq(t0->type, CKPGUID_BOOL)) {
                int32_t a, c;
                memcpy(&a, test->value, 4);
                memcpy(&c, v->value, 4);
                hit = (a != 0) == (c != 0);
            } else {
                hit = test->size <= v->size && !memcmp(test->value, v->value, test->size);
            }
            if (hit) {
                ck_activate_input(ctx, b, 0, false);
                ck_activate_output(ctx, b, i, true);
                return CKBR_OK;
            }
        }
    }
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Switch On Message (FUN_254884c0, callback FUN_25488580): inputs On, Off; pIn Message 0..n. Each
   frame, Received i for every last-frame message of the owner matching Message i. The callback makes
   the owner wait for messages on attach and load. ---- */
static int bb_switch_on_message(CkContext *ctx, CkBehavior *b)
{
    CkBeObject *owner = ck_beobject(ctx, b->owner);
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        return CKBR_OK;
    }
    if (ck_input_active(ctx, b, 0)) ck_activate_input(ctx, b, 0, false);
    if (owner && owner->nlast)
        for (uint32_t i = 0; i < b->pin.n; i++) {
            int32_t type = -1;
            bb_get_in(ctx, b, i, &type, 4);
            for (uint32_t k = 0; k < owner->nlast; k++)
                if (owner->last_frame[k]->type == type) ck_activate_output(ctx, b, i, true);
        }
    return CKBR_ACTIVATENEXTFRAME;
}
static void cb_switch_on_message(CkContext *ctx, CkBehavior *b, int msg)
{
    CkBeObject *owner = ck_beobject(ctx, b->owner);
    if (!owner) return;
    if (msg == CKM_BEHAVIORATTACH || msg == CKM_BEHAVIORLOAD) owner->waiting = true;
    else if (msg == CKM_BEHAVIORDETACH) owner->waiting = false;
}

/* ---- Linear Progression, time based (FUN_2549d850): pIn Duration, A, B; pOut Elapsed Time, Value,
   Delta, Progression (not clamped after the end). Loop Out while running, Out at the end. The frame
   based variant (setting 0, LAB_2549d720) is not used by the game. ---- */
static int bb_linear_progression(CkContext *ctx, CkBehavior *b)
{
    float a = 0, c = 0, elapsed = 0, duration = 0, prev = 0;
    bb_get_in(ctx, b, 1, &a, 4);
    bb_get_in(ctx, b, 2, &c, 4);
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        bb_set_out(ctx, b, 1, &a, 4);
    } else {
        ck_activate_input(ctx, b, 1, false);
        bb_get_out(ctx, b, 0, &elapsed, 4);
    }
    elapsed += ctx->delta_ms;
    bb_set_out(ctx, b, 0, &elapsed, 4);
    bb_get_in(ctx, b, 0, &duration, 4);
    float progress = elapsed / duration, value = c;
    if (elapsed <= duration) value = (c - a) * progress + a;
    ck_activate_output(ctx, b, elapsed <= duration ? 1 : 0, true);
    bb_get_out(ctx, b, 1, &prev, 4);
    float delta = value - prev;
    bb_set_out(ctx, b, 1, &value, 4);
    bb_set_out(ctx, b, 2, &delta, 4);
    bb_set_out(ctx, b, 3, &progress, 4);
    return CKBR_OK;
}

/* ---- Bezier Progression (FUN_2549c340; callback FUN_2549c8c0 picks it for the time based setting with a
   Time duration): pIn Duration, A, B, Progression Curve; pOut Elapsed Time, Value = A + (B - A) *
   curve(elapsed / duration clamped to 1), Delta, Progression. Loop Out while running, Out once elapsed
   exceeds the duration. Nothing happens without a curve. ---- */
static int bb_bezier_progression(CkContext *ctx, CkBehavior *b)
{
    float duration = 0, a = 0, c = 0, elapsed = 0, zero = 0;
    bb_get_in(ctx, b, 0, &duration, 4);
    CkParameter *cp = bb_in(ctx, b, 3);
    const CkCurve2d *curve = cp && cp->value ? (const CkCurve2d *)cp->value : NULL;
    if (!curve) return CKBR_OK;
    bb_get_in(ctx, b, 1, &a, 4);
    bb_get_in(ctx, b, 2, &c, 4);
    if (!ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 1, false);
        bb_get_out(ctx, b, 0, &elapsed, 4);
    } else {
        ck_activate_input(ctx, b, 0, false);
        bb_set_out(ctx, b, 0, &zero, 4);
        float v0 = (c - a) * ck_curve_get_y(curve, 0) + a;
        bb_set_out(ctx, b, 1, &v0, 4);
        bb_set_out(ctx, b, 2, &zero, 4);
        bb_set_out(ctx, b, 3, &zero, 4);
    }
    elapsed += ctx->delta_ms;
    bb_set_out(ctx, b, 0, &elapsed, 4);
    float progress = elapsed / duration;
    if (progress > 1) progress = 1;
    float value = (c - a) * ck_curve_get_y(curve, progress) + a, prev = 0;
    bb_get_out(ctx, b, 1, &prev, 4);
    float delta = value - prev;
    bb_set_out(ctx, b, 1, &value, 4);
    bb_set_out(ctx, b, 2, &delta, 4);
    bb_set_out(ctx, b, 3, &progress, 4);
    ck_activate_output(ctx, b, duration < elapsed ? 0 : 1, true);
    return CKBR_OK;
}

/* ---- Random (FUN_25490e90): Rand of the output's type from Min and Max, both of exactly that type.
   Float and derived types (the game uses Float and Time): (Max - Min) * rand() / 32767 + Min; Integer:
   rand() * (Max - Min) / 0x7fff + Min; Boolean: rand() & 1. (Vector, 2D vector, colour and rect
   variants are not used by the game.) ---- */
static int bb_random(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkParameter *out = bb_out(ctx, b, 0);
    if (!out) return CKBR_OK;
    CkGuid t = out->type;
    if (ck_guid_eq(t, CKPGUID_BOOL)) {
        int32_t v = ck_rand(ctx) & 1;
        bb_set_out(ctx, b, 0, &v, 4);
        return CKBR_OK;
    }
    bool is_float = ck_type_derives(t, CKPGUID_FLOAT);
    if (!is_float && !ck_guid_eq(t, CKPGUID_INT)) return CKBR_OK;
    for (uint32_t i = 0; i < 2; i++) {
        CkParameter *pin = i < b->pin.n ? ck_param(ctx, b->pin.v[i]) : NULL;
        if (!pin || !ck_guid_eq(pin->type, t)) return CKBR_OK;
    }
    if (is_float) {
        float lo = 0, hi = 0;
        bb_get_in(ctx, b, 0, &lo, 4);
        bb_get_in(ctx, b, 1, &hi, 4);
        float v = (hi - lo) * (float)ck_rand(ctx) * 3.0518509e-05f + lo;
        bb_set_out(ctx, b, 0, &v, 4);
    } else {
        int32_t lo = 0, hi = 0;
        bb_get_in(ctx, b, 0, &lo, 4);
        bb_get_in(ctx, b, 1, &hi, 4);
        int32_t v = ck_rand(ctx) * (hi - lo) / 0x7fff + lo;
        bb_set_out(ctx, b, 0, &v, 4);
    }
    return CKBR_OK;
}

/* ---- Get Highest (FUN_254976b0): the row with the highest value in column pIn 0 of the target array
   (CKDataArray::GetHighest 0x2402834d with the column compare: the first of equal values), pOut 0 its index (-1 if none), the
   row's cells to pOut 1... Found / Not Found. (The original answers from the sort order when the array
   was sorted on that column; the game doesn't sort arrays.) ---- */
static int bb_get_highest(CkContext *ctx, CkBehavior *b)
{
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a) return CKBR_OK;
    int32_t col = 0, row = -1;
    bb_get_in(ctx, b, 0, &col, 4);
    if (col >= 0 && (uint32_t)col < a->ncols && a->nrows) {
        row = 0;
        for (uint32_t r = 1; r < a->nrows; r++) {
            if (ck_array_compare_rows(ctx, a, (uint32_t)col, (uint32_t)row, r) > 0) row = (int32_t)r;
        }
    }
    bb_set_out(ctx, b, 0, &row, 4);
    ck_activate_input(ctx, b, 0, false);
    if (row < 0) {
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    row_to_outputs(ctx, a, (uint32_t)row, b, 1);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Remove Key Row (FUN_25496b20): removes the first row whose key column equals pIn 0 ---- */
static int bb_remove_key_row(CkContext *ctx, CkBehavior *b)
{
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a || !b->pin.n) return CKBR_OK;
    if (a->key_column < 0) {
        if (ctx->log) ctx->log("No column criterion defined");
        return CKBR_OK;
    }
    uint32_t key;
    const void *kp;
    uint32_t size = key_from_input(ctx, a, b, (uint32_t)a->key_column, 0, &key, &kp);
    int32_t row = ck_array_find_row(ctx, a, (uint32_t)a->key_column, 1, key, kp, size, 0);
    ck_activate_input(ctx, b, 0, false);
    if (row < 0) {
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    ck_array_remove_row(ctx, a, (uint32_t)row);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Add To Group (FUN_25488ac0): the target into group pIn 0 (CKGroup::AddObject 0x2402b86f: BeObjects
   only, not the group itself, not twice) ---- */
static int bb_add_to_group(CkContext *ctx, CkBehavior *b)
{
    CkObj *g = ck_obj(ctx, bb_in_object(ctx, b, 0));
    if (!g || g->cid != CKCID_GROUP) return CKBR_OK;
    CkObj *o = ck_obj(ctx, bb_target(ctx, b));
    CkGroup *grp = (CkGroup *)g;
    if (o && o != g && ck_is_beobject_class(o->cid) && !ck_ids_has(&grp->members, o->id)) ck_ids_push(&grp->members, o->id);
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Create String (FUN_2549e740, version 2): pIn 0 delimiter, pIn 1... the parts, each formatted with
   the matching local parameter (a printf format; empty = the part's text). Integer and Float sources
   are formatted as numbers, anything else as text (with "%s" when the format doesn't end in 's').
   Create: Text = the parts joined by the delimiter. Add Line: Text += "\n" + parts (no newline when
   Text is empty). Clear: Text = "" (then Create / Add Line in the same frame still run). ---- */
static void append(char **s, size_t *len, const char *t)
{
    size_t n = strlen(t);
    *s = realloc(*s, *len + n + 1);
    memcpy(*s + *len, t, n + 1);
    *len += n;
}

static int bb_create_string(CkContext *ctx, CkBehavior *b)
{
    const char *delim = bb_in_string(ctx, b, 0);
    if (!delim) delim = "";
    if (ck_input_active(ctx, b, 2)) {
        ck_activate_input(ctx, b, 2, false);
        ck_activate_output(ctx, b, 2, true);
        bb_set_out(ctx, b, 0, "", 1);
        if (!ck_input_active(ctx, b, 0) && !ck_input_active(ctx, b, 1)) return CKBR_OK;
    }
    char *text = NULL;
    size_t len = 0;
    append(&text, &len, "");
    for (uint32_t i = 1; i < b->pin.n; i++) {
        CkParameter *src = bb_in(ctx, b, i);
        if (!src) continue;
        CkParameter *fp = bb_local(ctx, b, i - 1);
        const char *fmt = fp && fp->value && fp->size ? (const char *)fp->value : "";
        char part[1024];
        if (!*fmt) {
            ck_param_to_string(ctx, src, part, sizeof part);
        } else if (ck_guid_eq(src->type, CKPGUID_INT)) {
            int32_t v = 0;
            if (src->value && src->size >= 4) memcpy(&v, src->value, 4);
            snprintf(part, sizeof part, fmt, v);
        } else if (ck_guid_eq(src->type, CKPGUID_FLOAT)) {
            float v = 0;
            if (src->value && src->size >= 4) memcpy(&v, src->value, 4);
            snprintf(part, sizeof part, fmt, (double)v);
        } else {
            char str[1024];
            ck_param_to_string(ctx, src, str, sizeof str);
            snprintf(part, sizeof part, fmt[strlen(fmt) - 1] != 's' ? "%s" : fmt, str);
        }
        if (i != 1) append(&text, &len, delim);
        append(&text, &len, part);
    }
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        ck_activate_output(ctx, b, 0, true);
        bb_set_out(ctx, b, 0, text, (uint32_t)len + 1);
    } else if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        ck_activate_output(ctx, b, 1, true);
        CkParameter *o = bb_out(ctx, b, 0);
        const char *prev = o && o->value && o->size ? (const char *)o->value : "";
        char *r = NULL;
        size_t rl = 0;
        append(&r, &rl, prev);
        if (!bb_local(ctx, b, 0) || *prev) append(&r, &rl, "\n");
        append(&r, &rl, text);
        bb_set_out(ctx, b, 0, r, (uint32_t)rl + 1);
        free(r);
    }
    free(text);
    return CKBR_OK;
}

/* ---- Test Cell (FUN_25499a90): CKDataArray::TestRow(pIn Row, pIn Column, pIn Operation, pIn 3 the
   reference value typed by the column) -> True / False ---- */
static int bb_test_cell(CkContext *ctx, CkBehavior *b)
{
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a) return CKBR_OK;
    int32_t row = 0, col = 0, op = 1;
    bb_get_in(ctx, b, 0, &row, 4);
    bb_get_in(ctx, b, 1, &col, 4);
    bb_get_in(ctx, b, 2, &op, 4);
    uint32_t key = 0;
    const void *kp = NULL;
    uint32_t size = col >= 0 ? key_from_input(ctx, a, b, (uint32_t)col, 3, &key, &kp) : 0;
    ck_activate_output(ctx, b, ck_array_test_cell(ctx, a, row, col, op, key, kp, size) ? 0 : 1, true);
    return CKBR_OK;
}

/* ---- Get Key Row (FUN_25498960): the first row whose key column equals pIn 0; its cells to pOut 0... ---- */
static int bb_get_key_row(CkContext *ctx, CkBehavior *b)
{
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a) return CKBR_OK;
    if (a->key_column < 0) {
        if (ctx->log) ctx->log("No column criterion defined");
        return CKBR_OK;
    }
    uint32_t key = 0;
    const void *kp = NULL;
    uint32_t size = key_from_input(ctx, a, b, (uint32_t)a->key_column, 0, &key, &kp);
    int32_t row = ck_array_find_row(ctx, a, (uint32_t)a->key_column, 1, key, kp, size, 0);
    ck_activate_input(ctx, b, 0, false);
    if (row < 0) {
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    row_to_outputs(ctx, a, (uint32_t)row, b, 0);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Change Value If (FUN_25496dd0): every row whose pIn Column cell satisfies pIn Operation against pIn 2
   gets the cell set from pIn 3 (CKDataArray::SetElementValueFromParameter); pOut Number Changed; Found
   when any changed ---- */
static int bb_change_value_if(CkContext *ctx, CkBehavior *b)
{
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a) return CKBR_OK;
    int32_t col = 0, op = 1, n = 0;
    bb_get_in(ctx, b, 0, &col, 4);
    bb_get_in(ctx, b, 1, &op, 4);
    uint32_t key = 0;
    const void *kp = NULL;
    uint32_t size = col >= 0 ? key_from_input(ctx, a, b, (uint32_t)col, 2, &key, &kp) : 0;
    CkParameter *src = bb_in(ctx, b, 3);
    for (int32_t r = 0; col >= 0 && (r = ck_array_find_row(ctx, a, (uint32_t)col, op, key, kp, size, r)) != -1; r++) {
        if (src) ck_array_set_from_param(ctx, a, (uint32_t)r, (uint32_t)col, src);
        n++;
    }
    bb_set_out(ctx, b, 0, &n, 4);
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, n ? 0 : 1, true);
    return CKBR_OK;
}

/* ---- Value Count (FUN_25499d20): pOut Count = rows whose pIn Column cell satisfies pIn Operator against
   pIn 2 (CKDataArray::GetCount) ---- */
static int bb_value_count(CkContext *ctx, CkBehavior *b)
{
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a) return CKBR_OK;
    int32_t col = 1, op = 1;
    bb_get_in(ctx, b, 0, &col, 4);
    uint32_t key = 0;
    const void *kp = NULL;
    uint32_t size = col >= 0 ? key_from_input(ctx, a, b, (uint32_t)col, 2, &key, &kp) : 0;
    bb_get_in(ctx, b, 1, &op, 4);
    int32_t n = ck_array_count(ctx, a, col, op, key, kp, size);
    bb_set_out(ctx, b, 0, &n, 4);
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Sort Rows (FUN_2549a840): CKDataArray::Sort(pIn Sorting Column, pIn Ascending) ---- */
static int bb_sort_rows(CkContext *ctx, CkBehavior *b)
{
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a) return CKBR_OK;
    int32_t col = 0, asc = 1;
    bb_get_in(ctx, b, 0, &col, 4);
    bb_get_in(ctx, b, 1, &asc, 4);
    ck_array_sort(ctx, a, col, asc != 0);
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Remove Attribute (FUN_254951a0): CKBeObject::RemoveAttribute(pIn Attribute, default -1) on the target ---- */
static int bb_remove_attribute(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkId t = bb_target(ctx, b);
    if (!ck_obj(ctx, t)) return CKBR_OK;
    int32_t type = -1;
    bb_get_in(ctx, b, 0, &type, 4);
    ck_remove_attribute(ctx, t, type);
    return CKBR_OK;
}

/* ---- Set Attribute (FUN_254954a0): CKBeObject::SetAttribute(pIn Attribute) on the target, then the
   attribute's parameter copies pIn 1 (the callback FUN_25495540 creates it with the attribute's type) ---- */
static int bb_set_attribute(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkId t = bb_target(ctx, b);
    if (!ck_obj(ctx, t)) return CKBR_OK;
    int32_t type = -1;
    bb_get_in(ctx, b, 0, &type, 4);
    ck_set_attribute(ctx, t, type, 0);
    CkParameter *ap = ck_param(ctx, ck_attribute_parameter(ctx, t, type)), *src = bb_in(ctx, b, 1);
    if (ap && src && src->value) ck_param_set(ap, src->value, src->size);
    return CKBR_OK;
}

/* ---- Has Attribute (FUN_25494a20): True with pOut the attribute's value when the target has pIn Attribute,
   else False ---- */
static int bb_has_attribute(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    CkId t = bb_target(ctx, b);
    int32_t type = -1;
    bb_get_in(ctx, b, 0, &type, 4);
    if (ck_obj(ctx, t) && ck_has_attribute(ctx, t, type)) {
        ck_activate_output(ctx, b, 0, true);
        CkParameter *ap = ck_param(ctx, ck_attribute_parameter(ctx, t, type));
        if (ap && ap->value && bb_out(ctx, b, 0)) bb_set_out(ctx, b, 0, ap->value, ap->size);
        return CKBR_OK;
    }
    ck_activate_output(ctx, b, 1, true);
    return CKBR_OK;
}

/* ---- Set Component (FUN_25491a20): pOut Variable (colour 4, vector / euler angles 3, 2D vector 2, rect 4
   floats) from the float inputs ---- */
static int bb_set_component(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkParameter *o = bb_out(ctx, b, 0);
    if (!o) return CKBR_OK;
    uint32_t n = ck_guid_eq(o->type, (CkGuid){0x57d42fee, 0x7cbb3b91}) || ck_guid_eq(o->type, (CkGuid){0x7ab20d20, 0x693044a9}) ? 4
                 : ck_guid_eq(o->type, (CkGuid){0x4efcb34a, 0x6079e42f}) ? 2
                 : ck_guid_eq(o->type, (CkGuid){0x48824eae, 0x2fe47960}) || ck_guid_eq(o->type, (CkGuid){0x13b01b3c, 0x1942583e}) ? 3 : 0;
    float v[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < n; i++) bb_get_in(ctx, b, i, &v[i], 4);
    if (n) bb_set_out(ctx, b, 0, v, n * 4);
    return CKBR_OK;
}

/* ---- Calculator (FUN_2548ad70; compile FUN_2548b110, evaluate FUN_2548b750): pIn expression, then the
   variables: a single letter A..Z is input parameter (letter - 'A' + 1); pOut x. The expression is upper-
   cased and compiled when it changes (local 0 keeps the source, "#####" after a syntax error, which is
   reported and leaves x alone). The game's expressions only use numbers, variables, + - * / and
   parentheses; PI and unary minus are also read here. ---- */
typedef struct {
    const char *p;
    CkContext *ctx;
    CkBehavior *b;
    bool err;
} Calc;

static double calc_expr(Calc *c);

static void calc_space(Calc *c)
{
    while (*c->p == ' ' || *c->p == '\t') c->p++;
}

static double calc_primary(Calc *c)
{
    calc_space(c);
    char ch = *c->p;
    if (ch == '(') {
        c->p++;
        double v = calc_expr(c);
        calc_space(c);
        if (*c->p == ')') c->p++;
        else c->err = true;
        return v;
    }
    if (ch == '-') {
        c->p++;
        return -calc_primary(c);
    }
    if (ch == '+') {
        c->p++;
        return calc_primary(c);
    }
    if (c->p[0] == 'P' && c->p[1] == 'I') {
        c->p += 2;
        return 3.1415927f;
    }
    if (ch >= 'A' && ch <= 'Z' && !(c->p[1] >= 'A' && c->p[1] <= 'Z')) {
        c->p++;
        float v = 0;
        CkParameter *pin = (uint32_t)(ch - 'A' + 1) < c->b->pin.n ? ck_param(c->ctx, c->b->pin.v[ch - 'A' + 1]) : NULL;
        if (!pin) {
            c->err = true;
            return 0;
        }
        if (ck_guid_eq(pin->type, CKPGUID_INT)) {
            int32_t i = 0;
            bb_get_in(c->ctx, c->b, (uint32_t)(ch - 'A' + 1), &i, 4);
            v = (float)i;
        } else {
            bb_get_in(c->ctx, c->b, (uint32_t)(ch - 'A' + 1), &v, 4);
        }
        return v;
    }
    char *end;
    double v = strtod(c->p, &end);
    if (end == c->p) c->err = true;
    c->p = end;
    return (float)v;
}

static double calc_term(Calc *c)
{
    double v = calc_primary(c);
    for (;;) {
        calc_space(c);
        if (*c->p == '*') c->p++, v = (float)(v * calc_primary(c));
        else if (*c->p == '/') c->p++, v = (float)(v / calc_primary(c));
        else return v;
    }
}

static double calc_expr(Calc *c)
{
    double v = calc_term(c);
    for (;;) {
        calc_space(c);
        if (*c->p == '+') c->p++, v = (float)(v + calc_term(c));
        else if (*c->p == '-') c->p++, v = (float)(v - calc_term(c));
        else return v;
    }
}

static int bb_calculator(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    const char *src = bb_in_string(ctx, b, 0);
    char up[512];
    snprintf(up, sizeof up, "%s", src ? src : "");
    for (char *q = up; *q; q++)
        if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 'a' + 'A');
    Calc c = {up, ctx, b, false};
    float x = (float)calc_expr(&c);
    calc_space(&c);
    if (c.err || *c.p || !*up) {
        if (ctx->log) ctx->log("Syntax Error in CALCULATOR");
        CkParameter *l = bb_local(ctx, b, 0);
        if (l) ck_param_set(l, "#####", 6);
        return CKBR_OK;
    }
    bb_set_out(ctx, b, 0, &x, 4);
    return CKBR_OK;
}

/* ---- Collection Iterator (FUN_2549cd70): over pIn Collection (an object array: its ids), pOut Element and
   Index (kept in local 0 when the Index output is missing); Out at the end ---- */
static int bb_collection_iterator(CkContext *ctx, CkBehavior *b)
{
    CkParameter *c = bb_in(ctx, b, 0);
    if (!c || !c->value) {
        ck_activate_output(ctx, b, 0, true);
        return CKBR_OK;
    }
    int32_t i = 0;
    if (!bb_get_out(ctx, b, 1, &i, 4)) bb_get_local(ctx, b, 0, &i, 4);
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        i = 0;
    } else {
        ck_activate_input(ctx, b, 1, false);
        i++;
    }
    uint32_t n = c->size / 4;
    if (i < 0 || (uint32_t)i >= n) {
        ck_activate_output(ctx, b, 0, true);
        return CKBR_OK;
    }
    CkId id = ((const CkId *)c->value)[i];
    if (!ck_obj(ctx, id)) id = 0;
    bb_set_out(ctx, b, 0, &id, 4);
    if (bb_out(ctx, b, 1)) bb_set_out(ctx, b, 1, &i, 4);
    else bb_set_local(ctx, b, 0, &i, 4);
    ck_activate_output(ctx, b, 1, true);
    return CKBR_OK;
}

/* ---- Scan String (FUN_2549f4b0): pIn Text split at pIn Delimiter (default " "); pOut 1... the pieces
   (converted from text to their types; empty pieces between delimiters are skipped), pOut 0 their count.
   When the outputs run out, the count isn't written. ---- */
static void scan_out(CkContext *ctx, CkBehavior *b, uint32_t i, const char *piece)
{
    CkParameter *o = bb_out(ctx, b, i);
    if (!o || !*piece) return;
    CkParameter tmp = *o;
    tmp.value = NULL, tmp.size = 0;
    ck_param_from_string(ctx, &tmp, piece);
    bb_set_out(ctx, b, i, tmp.value, tmp.size);
    free(tmp.value);
}

static int bb_scan_string(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    const char *text = bb_in_string(ctx, b, 0);
    if (!text) return CKBR_OK;
    const char *delim = bb_in_string(ctx, b, 1);
    if (!delim || !*delim) delim = " ";
    size_t dl = strlen(delim);
    int32_t count = 0;
    char piece[256];
    const char *cur = text, *p;
    while ((p = strstr(cur, delim)) != NULL) {
        if (p != cur) {
            count++;
            if (!bb_out(ctx, b, (uint32_t)count)) break;
            snprintf(piece, sizeof piece, "%.*s", (int)(p - cur), cur);
            scan_out(ctx, b, (uint32_t)count, piece);
        }
        cur = p + dl;
    }
    if (*cur) {
        count++;
        if (!bb_out(ctx, b, (uint32_t)count)) return CKBR_OK;
        snprintf(piece, sizeof piece, "%s", cur);
        scan_out(ctx, b, (uint32_t)count, piece);
    }
    bb_set_out(ctx, b, 0, &count, 4);
    return CKBR_OK;
}

/* ---- Streaming Event 1f0b52bf:4c3342dd (v2 execute 0x25486aa0): an output fires on its input's rising edge,
   measured between executions (local 0: the inputs active last time); pOut 0 the last output fired ---- */
static int bb_streaming_event(CkContext *ctx, CkBehavior *b)
{
    int32_t mask = 0;
    bb_get_local(ctx, b, 0, &mask, 4);
    for (uint32_t i = 0; i < b->in.n; i++) {
        if (ck_input_active(ctx, b, i)) {
            ck_activate_input(ctx, b, i, false);
            if (!(mask & (1 << (i & 31)))) {
                mask |= 1 << i;
                int32_t idx = (int32_t)i;
                bb_set_out(ctx, b, 0, &idx, 4);
                ck_activate_output(ctx, b, i, true);
            }
        } else {
            mask &= ~(1 << i);
        }
    }
    bb_set_local(ctx, b, 0, &mask, 4);
    return CKBR_OK;
}

/* 0x25486b70: attach, reset, load: pOut 0 = -1, local 0 = 0 */
static void cb_streaming_event(CkContext *ctx, CkBehavior *b, int msg)
{
    if (msg == CKM_BEHAVIORATTACH || msg == CKM_BEHAVIORRESET || msg == CKM_BEHAVIORLOAD) {
        int32_t m1 = -1, z = 0;
        CkParameter *o = bb_out(ctx, b, 0);
        if (o) ck_param_set(o, &m1, 4);
        bb_set_local(ctx, b, 0, &z, 4);
    }
}

/* ---- Per Second 448e54ce:75a655c5 (execute 0x25490b50): pOut i = pIn i (every float of it) * dt in seconds ---- */
static int bb_per_second(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    float s = ctx->delta_ms * 0.001f;
    for (uint32_t i = 0; i < b->pout.n; i++) {
        CkParameter *src = bb_in(ctx, b, i), *o = bb_out(ctx, b, i);
        if (!src || !src->value || !o) continue;
        float v[16] = {0};
        uint32_t n = o->size / 4;
        if (n > 16) n = 16;
        memcpy(v, src->value, (src->size < n * 4 ? src->size : n * 4));
        for (uint32_t k = 0; k < n; k++) v[k] *= s;
        bb_set_out(ctx, b, i, v, n * 4);
    }
    return CKBR_OK;
}

/* ---- Timer a2a5a63a:e4e7e8e5 (execute 0x2549e390): In restarts, Loop In continues; elapsed += dt; Out once
   elapsed >= pIn Duration (ms), else Loop Out ---- */
static int bb_timer(CkContext *ctx, CkBehavior *b)
{
    float elapsed = 0, duration = 3000;
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        bb_set_out(ctx, b, 0, &elapsed, 4);
    } else {
        ck_activate_input(ctx, b, 1, false);
        bb_get_out(ctx, b, 0, &elapsed, 4);
    }
    elapsed = elapsed + ctx->delta_ms;
    bb_set_out(ctx, b, 0, &elapsed, 4);
    bb_set_out(ctx, b, 1, &ctx->delta_ms, 4);
    bb_get_in(ctx, b, 0, &duration, 4);
    ck_activate_output(ctx, b, !(elapsed < duration) ? 0 : 1, true);
    return CKBR_OK;
}

/* ---- Random Switch 79d72fde:2e9d0912 (execute 0x254861b0): one output by the pIn Coef weights (one rand());
   setting 0 forbids repeating local 1 ---- */
static int bb_random_switch(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    int32_t forbid = 0, last = 0;
    bb_get_local(ctx, b, 0, &forbid, 4);
    bb_get_local(ctx, b, 1, &last, 4);
    int32_t excl = forbid ? last : -1;
    float w[32], sum = 0;
    uint32_t n = b->out.n < 32 ? b->out.n : 32;
    for (uint32_t i = 0; i < n; i++) {
        w[i] = 1;
        if ((int32_t)i == excl) continue;
        bb_get_in(ctx, b, i, &w[i], 4);
        sum += w[i];
    }
    float r = (float)ck_rand(ctx) * sum * (1.0f / 32767.0f), acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        if ((int32_t)i == excl) continue;
        acc += w[i];
        if (r <= acc) {
            ck_activate_output(ctx, b, i, true);
            if (forbid) {
                int32_t li = (int32_t)i;
                bb_set_local(ctx, b, 1, &li, 4);
            }
            break;
        }
    }
    return CKBR_OK;
}

/* ---- Remove Row 1fa57136:14310857 (execute 0x25496680) ---- */
static int bb_remove_row(CkContext *ctx, CkBehavior *b)
{
    CkDataArray *a = ck_array(ctx, bb_target(ctx, b));
    if (!a) return CKBR_OK;
    int32_t row = 0;
    bb_get_in(ctx, b, 0, &row, 4);
    ck_activate_input(ctx, b, 0, false);
    if (row >= 0 && (uint32_t)row < a->nrows) {
        ck_array_remove_row(ctx, a, (uint32_t)row);
        ck_activate_output(ctx, b, 0, true);
    } else {
        ck_activate_output(ctx, b, 1, true);
    }
    return CKBR_OK;
}

/* ---- Objects With Attribute Iterator 6bc1494c:0c816ad3 (execute 0x25494f10): over the objects having pIn
   Attribute, in the order it was set; pOut Object and the attribute's value ---- */
static int bb_objects_with_attribute_iterator(CkContext *ctx, CkBehavior *b)
{
    int32_t type = -1, idx = 0;
    bb_get_in(ctx, b, 0, &type, 4);
    const CkAttributeType *t = ck_attribute_info(ctx, type);
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        idx = 0;
    } else {
        ck_activate_input(ctx, b, 1, false);
        bb_get_local(ctx, b, 0, &idx, 4);
        idx++;
    }
    if (!t || idx < 0 || (uint32_t)idx >= t->objects.n) {
        ck_activate_output(ctx, b, 0, true);
        return CKBR_OK;
    }
    CkId o = t->objects.v[idx];
    bb_set_out(ctx, b, 0, &o, 4);
    bb_set_local(ctx, b, 0, &idx, 4);
    CkParameter *ap = ck_param(ctx, ck_attribute_parameter(ctx, o, type));
    if (ap && ap->value && bb_out(ctx, b, 1)) bb_set_out(ctx, b, 1, ap->value, ap->size);
    ck_activate_output(ctx, b, 1, true);
    return CKBR_OK;
}

/* ---- Fill Group By Class 4445257b:70016c57 (execute 0x25488c60): the target group gets every object of pIn
   Class (with Derived: any derived class; without: classes c and c + 1 that derive from c, an original
   off-by-one), in the current scene when pIn 2 ---- */
static int bb_fill_group_by_class(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkObj *g = ck_obj(ctx, bb_target(ctx, b));
    if (!g || g->cid != CKCID_GROUP) return CKBR_OK;
    int32_t c = 0, derived = 0, scene = 1;
    bb_get_in(ctx, b, 0, &c, 4);
    bb_get_in(ctx, b, 1, &derived, 4);
    bb_get_in(ctx, b, 2, &scene, 4);
    int32_t lo = derived ? 0 : c, hi = derived ? 0x37 : c + 1;
    CkGroup *grp = (CkGroup *)g;
    for (int32_t cid = lo; cid <= hi; cid++) {
        if (!ck_class_derives((uint32_t)cid, (uint32_t)c)) continue;
        for (uint32_t i = 0; i < ctx->nobjs; i++) {
            CkObj *o = ctx->objs[i];
            if (!o || o->cid != (uint32_t)cid || o == g || !ck_is_beobject_class(o->cid)) continue;
            if (scene && !ck_scene_entry(ctx, o->id)) continue;
            if (!ck_ids_has(&grp->members, o->id)) ck_ids_push(&grp->members, o->id);
        }
    }
    return CKBR_OK;
}

/* ---- Load String 391555d6:42f2500e (execute 0x2549f0c0): the text file (resolved by the path manager, CRLF
   -> LF) -> pOut String, Out; File Error if it can't be read ---- */
static int bb_load_string(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    const char *name = bb_in_string(ctx, b, 0);
    size_t n = 0;
    uint8_t *d = name ? vfs_read_all(name, &n) : NULL;
    if (!d) {
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    ck_activate_output(ctx, b, 0, true);
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (!(d[i] == '\r' && i + 1 < n && d[i + 1] == '\n')) d[k++] = d[i];
    char *t = malloc(k + 1);
    memcpy(t, d, k);
    t[k] = 0;
    bb_set_out(ctx, b, 0, t, (uint32_t)k + 1);
    free(t);
    free(d);
    return CKBR_OK;
}

/* ---- Get Nearest In Group 085207eb:584950d8 (execute 0x25488f40): the 3D entity of pIn Group nearest to pIn
   Position (through Referential; the referential itself excluded); Out even on errors ---- */
static int bb_get_nearest_in_group(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkObj *g = ck_obj(ctx, bb_in_object(ctx, b, 0));
    if (!g || g->cid != CKCID_GROUP) return CKBR_OK;
    float p[3] = {0, 0, 0}, w[3];
    bb_get_in(ctx, b, 1, p, 12);
    CkId refid = bb_in_object(ctx, b, 2);
    Ck3dEntity *ref = ck_entity(ctx, refid);
    if (ref)
        for (int j = 0; j < 3; j++) w[j] = p[0] * ref->world[0][j] + p[1] * ref->world[1][j] + p[2] * ref->world[2][j] + ref->world[3][j];
    else
        memcpy(w, p, 12);
    CkId none = 0, best_o = 0;
    bb_set_out(ctx, b, 0, &none, 4);
    float best = 3.4028235e38f;
    CkGroup *grp = (CkGroup *)g;
    for (uint32_t i = 0; i < grp->members.n; i++) {
        CkId o = grp->members.v[i];
        Ck3dEntity *e = ck_entity(ctx, o);
        if (!e || o == refid) continue;
        float dx = e->world[3][0] - w[0], dy = e->world[3][1] - w[1], dz = e->world[3][2] - w[2], d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best) best = d2, best_o = o;
    }
    float dist = sqrtf(best);
    bb_set_out(ctx, b, 0, &best_o, 4);
    bb_set_out(ctx, b, 1, &dist, 4);
    return CKBR_OK;
}

/* CK3dEntity::RayIntersection over the current mesh's faces in world space (VxIntersect::RayFace, culled
   unless the face's material is two-sided; faces without a material are culled): the nearest hit's distance
   along the unit ray, face index and point */
static bool ray_mesh(CkContext *ctx, const Ck3dEntity *e, const float o[3], const float d[3], float *best_t, int32_t *face,
                     float hit[3])
{
    CkMesh *m = ck_mesh(ctx, e->mesh);
    if (!m || !m->nverts || !m->nfaces) return false;
    bool found = false;
    const float eps = 1.1920929e-7f;
    for (uint32_t f = 0; f < m->nfaces; f++) {
        float v[3][3];
        for (int k = 0; k < 3; k++) {
            const CkVertex *vx = &m->verts[m->faces[f].i[k] < m->nverts ? m->faces[f].i[k] : 0];
            const float l[3] = {vx->pos.x, vx->pos.y, vx->pos.z};
            for (int j = 0; j < 3; j++) v[k][j] = l[0] * e->world[0][j] + l[1] * e->world[1][j] + l[2] * e->world[2][j] + e->world[3][j];
        }
        float e1[3] = {v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2]};
        float e2[3] = {v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        float ln = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (ln == 0) continue;
        for (int k = 0; k < 3; k++) n[k] /= ln;
        CkId mid = m->faces[f].material < m->materials.n ? m->materials.v[m->faces[f].material] : 0;
        CkMaterial *mat = ck_material(ctx, mid);
        bool two = mat && (mat->flags & CKMAT_TWOSIDED);
        float den = n[0] * d[0] + n[1] * d[1] + n[2] * d[2];
        if (two ? fabsf(den) < eps : !(den <= -eps)) continue;
        float dd = -(n[0] * v[0][0] + n[1] * v[0][1] + n[2] * v[0][2]);
        float t = -(n[0] * o[0] + n[1] * o[1] + n[2] * o[2] + dd) / den;
        if (t < -eps || t >= *best_t) continue;
        float p[3] = {o[0] + t * d[0], o[1] + t * d[1], o[2] + t * d[2]};
        /* PointInFace: drop the dominant normal axis; inside when the three edge tests agree */
        int ax = 0;
        if (fabsf(n[1]) > fabsf(n[ax])) ax = 1;
        if (fabsf(n[2]) > fabsf(n[ax])) ax = 2;
        int iu = ax == 0 ? 1 : 0, iv = ax == 2 ? 1 : 2;
        float sgn[3];
        for (int k = 0; k < 3; k++) {
            const float *a = v[k], *b2 = v[(k + 1) % 3];
            float eu = p[iu] - a[iu], ev = p[iv] - a[iv], fu = b2[iu] - a[iu], fv = b2[iv] - a[iv];
            sgn[k] = eu * fv - ev * fu;
        }
        bool inside = (sgn[0] < 0 && sgn[1] < 0 && sgn[2] < 0) || (sgn[0] >= 0 && sgn[1] >= 0 && sgn[2] >= 0);
        if (!inside) continue;
        *best_t = t, *face = (int32_t)f;
        memcpy(hit, p, 12);
        found = true;
    }
    return found;
}

/* ---- Ray Intersection 671e4a87:383b2912 (execute 0x25483090, test 0x25482cf0): the ray pIn Ray Origin /
   Direction (through Referential; the direction normalised) against the mesh triangles of pIn Filter's
   members (else every 3D object) in the scene, the owner skipped (setting 1); True when the nearest hit is
   within pIn Depth: pOut 0 the object, with setting 0 also face, nearest vertex, point, normal, distance ---- */
static int bb_ray_intersection(CkContext *ctx, CkBehavior *b)
{
    CkId tgt = bb_target(ctx, b);
    if (!ck_obj(ctx, tgt)) return CKBR_OK;
    ck_activate_input(ctx, b, 0, false);
    int32_t skip_owner = 1, skip_invis = 0, upd = 1;
    bb_get_local(ctx, b, 1, &skip_owner, 4);
    bb_get_local(ctx, b, 2, &skip_invis, 4);
    if (!skip_owner) tgt = 0;
    float o[3] = {0, 0, 0}, d[3] = {0, 0, 1}, wo[3], wd[3], depth = 100;
    bb_get_in(ctx, b, 0, o, 12);
    bb_get_in(ctx, b, 1, d, 12);
    Ck3dEntity *ref = ck_entity(ctx, bb_in_object(ctx, b, 2));
    for (int j = 0; j < 3; j++) {
        wo[j] = ref ? o[0] * ref->world[0][j] + o[1] * ref->world[1][j] + o[2] * ref->world[2][j] + ref->world[3][j] : o[j];
        wd[j] = ref ? d[0] * ref->world[0][j] + d[1] * ref->world[1][j] + d[2] * ref->world[2][j] : d[j];
    }
    float l = sqrtf(wd[0] * wd[0] + wd[1] * wd[1] + wd[2] * wd[2]);
    if (l > 0) wd[0] /= l, wd[1] /= l, wd[2] /= l;
    bb_get_in(ctx, b, 3, &depth, 4);
    CkObj *g = ck_obj(ctx, bb_in_object(ctx, b, 4));
    float best_t = depth, hit[3] = {0, 0, 0};
    int32_t face = -1;
    CkId best = 0;
    uint32_t n = g && g->cid == CKCID_GROUP ? ((CkGroup *)g)->members.n : ctx->nobjs;
    for (uint32_t i = 0; i < n; i++) {
        CkId id = g && g->cid == CKCID_GROUP ? ((CkGroup *)g)->members.v[i] : i + 1;
        CkObj *c = ck_obj(ctx, id);
        if (!c || id == tgt || !ck_is_3dentity_class(c->cid)) continue;
        if (!g && c->cid != CKCID_3DOBJECT) continue;
        if (!ck_scene_entry(ctx, id)) continue;
        if (skip_invis && !(c->flags & CK_OBJECT_VISIBLE)) continue;
        if (ray_mesh(ctx, (Ck3dEntity *)c, wo, wd, &best_t, &face, hit)) best = id;
    }
    if (best) {
        bb_set_out(ctx, b, 0, &best, 4);
        bb_get_local(ctx, b, 0, &upd, 4);
        if (upd) {
            bb_set_out(ctx, b, 1, &face, 4);
            bb_set_out(ctx, b, 3, hit, 12);
            bb_set_out(ctx, b, 5, &best_t, 4);
        }
        ck_activate_output(ctx, b, 0, true);
        return CKBR_OK;
    }
    ck_activate_output(ctx, b, 1, true);
    return CKBR_OK;
}

/* ---- Interpolator (FUN_25493530 and the typed variants the callback FUN_25493c00 selects by the output
   type: float, int, vector/euler, 2D vector, colour (RGB; setting 0 = HSV, unused by the game), matrix,
   quaternion, rect). Every variant the game uses is C = (B - A) * Value + A per float component. ---- */
static int bb_interpolator(CkContext *ctx, CkBehavior *b)
{
    CkParameter *out = bb_out(ctx, b, 0);
    float a[16] = {0}, c[16] = {0}, v = 0.5f, r[16];
    uint32_t n = out && out->size ? out->size / 4 : 1;
    if (n > 16) n = 16;
    bb_get_in(ctx, b, 0, a, n * 4);
    bb_get_in(ctx, b, 1, c, n * 4);
    bb_get_in(ctx, b, 2, &v, 4);
    if (out && ck_guid_eq(out->type, CKPGUID_INT)) {
        int32_t ia, ic;
        memcpy(&ia, a, 4);
        memcpy(&ic, c, 4);
        int32_t x = (int32_t)((float)(ic - ia) * v + (float)ia);   /* FUN_254935b0: ftol */
        bb_set_out(ctx, b, 0, &x, 4);
    } else {
        for (uint32_t i = 0; i < n; i++) r[i] = (c[i] - a[i]) * v + a[i];
        bb_set_out(ctx, b, 0, r, n * 4);
    }
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

BB_DECL(d_identity, 15151652, aeefffd5, "Identity", bb_identity);
BB_DECL(d_nop, 302561c4, 0d282980, "Nop", bb_nop);
BB_DECL(d_delayer, 15d472a5, 3bea409f, "Delayer", bb_delayer);
BB_DECL(d_sequencer, 42530844, 257b6053, "Sequencer", bb_sequencer);
BB_DECL(d_test, 17d66d26, 726b7dec, "Test", bb_test);
BB_DECL(d_binary_memory, d02d67dd, 10211fdd, "Binary Memory", bb_binary_memory);
BB_DECL(d_parameter_selector, 63eb2b0c, 27e2767e, "Parameter Selector", bb_parameter_selector);
BB_DECL(d_get_cell, 33b99f51, 07d95c45, "Get Cell", bb_get_cell);
BB_DECL(d_set_cell, 30ed1c6d, 4a3b7067, "Set Cell", bb_set_cell);
BB_DECL(d_get_row, 33b77f41, 07b95c45, "Get Row", bb_get_row);
BB_DECL(d_set_row, 62e87901, 2df007dd, "Set Row", bb_set_row);
BB_DECL(d_add_row, 1c7e5dc6, 3f6423c2, "Add Row", bb_add_row);
BB_DECL(d_remove_row_if, 57865622, 662d2fee, "Remove Row If", bb_remove_row_if);
BB_DECL(d_row_search, 78863443, 45af59b6, "Row Search", bb_row_search);
BB_DECL(d_clear_array, 35c9352f, 7b1a193b, "Clear Array", bb_clear_array);
BB_DECL(d_iterator, 198f0af9, 0268249f, "Iterator", bb_iterator);
BB_DECL(d_group_iterator, 6050252f, 3aa82d40, "Group Iterator", bb_group_iterator);
BB_DECL(d_wait_for_all, c044a999, fdfefaf7, "Wait For All", bb_wait_for_all);
BB_DECL(d_counter, 998f000f, f000f899, "Counter", bb_counter);
BB_DECL(d_send_message, a20e8d5b, df002150, "Send Message", bb_send_message);
BB_DECL(d_send_message_to_group, 5f906952, 6df11649, "Send Message To Group", bb_send_message_to_group);
BB_DECL(d_wait_message, 4587ffee, 4587ffdd, "Wait Message", bb_wait_message);
BB_DECL(d_iterator_if, 6bec4be6, 12d64c7c, "Iterator If", bb_iterator_if);
BB_DECL(d_insert_column, 05cb7802, 04a64a58, "Insert Column", bb_insert_column);
BB_DECL(d_remove_column, 3f377888, 128a7767, "Remove Column", bb_remove_column);
BB_DECL(d_get_substring, 10b16051, 26173b39, "Get SubString", bb_get_substring);
BB_DECL(d_broadcast_message, 3d6c4ae1, 72ae2cd6, "Broadcast Message", bb_broadcast_message);
BB_DECL(d_mini_calculator, 55bc3115, 1dfe1e40, "Mini Calculator", bb_mini_calculator);
BB_DECL(d_threshold, 655e6af4, 08c0596f, "Threshold", bb_threshold);
BB_DECL(d_switch_on_parameter, 4c42aace, 1da45635, "Switch On Parameter", bb_switch_on_parameter);
BB_DECL_CB(d_switch_on_message, 1bb23f1d, 17ff14b9, "Switch On Message", bb_switch_on_message, cb_switch_on_message);
BB_DECL(d_linear_progression, fff45680, aa512a39, "Linear Progression", bb_linear_progression);
BB_DECL(d_bezier_progression, 6bb8699a, 29fb6a4b, "Bezier Progression", bb_bezier_progression);
BB_DECL(d_random, 0c622386, 1c3054f7, "Random", bb_random);
BB_DECL(d_get_highest, 46f71e13, 13d26f61, "Get Highest", bb_get_highest);
BB_DECL(d_remove_key_row, 0f8334ea, 279a40cd, "Remove Key Row", bb_remove_key_row);
BB_DECL(d_add_to_group, 00024125, 785420ab, "Add To Group", bb_add_to_group);
BB_DECL(d_create_string, 4bcd2f9d, 382652e2, "Create String", bb_create_string);
BB_DECL(d_test_cell, 4e6c6da8, 636904fc, "Test Cell", bb_test_cell);
BB_DECL(d_get_key_row, 49064205, 10e72f7a, "Get Key Row", bb_get_key_row);
BB_DECL(d_change_value_if, 07253edb, 4d1237ed, "Change Value If", bb_change_value_if);
BB_DECL(d_value_count, 534377de, 75fd478a, "Value Count", bb_value_count);
BB_DECL(d_sort_rows, 6f623e68, 62bb5a98, "Sort Rows", bb_sort_rows);
BB_DECL(d_remove_attribute, 6b6340c4, 61e94a41, "Remove Attribute", bb_remove_attribute);
BB_DECL(d_set_attribute, 373040f2, 05e01b34, "Set Attribute", bb_set_attribute);
BB_DECL(d_has_attribute, 25b54079, 6ff90545, "Has Attribute", bb_has_attribute);
BB_DECL(d_set_component, 6e800755, 57b64acb, "Set Component", bb_set_component);
BB_DECL(d_calculator, 4bc209b8, 5b3679b0, "Calculator", bb_calculator);
BB_DECL(d_collection_iterator, 419602ec, 7822779e, "Collection Iterator", bb_collection_iterator);
BB_DECL(d_scan_string, 4afa2f11, 034872e2, "Scan String", bb_scan_string);
BB_DECL_CB(d_streaming_event, 1f0b52bf, 4c3342dd, "Streaming Event", bb_streaming_event, cb_streaming_event);
BB_DECL(d_per_second, 448e54ce, 75a655c5, "Per Second", bb_per_second);
BB_DECL(d_timer, a2a5a63a, e4e7e8e5, "Timer", bb_timer);
BB_DECL(d_random_switch, 79d72fde, 2e9d0912, "Random Switch", bb_random_switch);
BB_DECL(d_remove_row, 1fa57136, 14310857, "Remove Row", bb_remove_row);
BB_DECL(d_objects_with_attribute_iterator, 6bc1494c, 0c816ad3, "Objects With Attribute Iterator", bb_objects_with_attribute_iterator);
BB_DECL(d_fill_group_by_class, 4445257b, 70016c57, "Fill Group By Class", bb_fill_group_by_class);
BB_DECL(d_load_string, 391555d6, 42f2500e, "Load String", bb_load_string);
BB_DECL(d_get_nearest_in_group, 085207eb, 584950d8, "Get Nearest In Group", bb_get_nearest_in_group);
BB_DECL(d_ray_intersection, 671e4a87, 383b2912, "Ray Intersection", bb_ray_intersection);
BB_DECL(d_interpolator, 35503950, 2dde7a65, "Interpolator", bb_interpolator);

const CkBBDecl *const bb_logics[] = {
    &d_remove_attribute, &d_row_search, &d_send_message_to_group, &d_op, &d_binary_switch, &d_identity, &d_nop, &d_delayer, &d_sequencer, &d_test, &d_binary_memory,
    &d_parameter_selector, &d_get_cell, &d_set_cell, &d_get_row, &d_set_row, &d_add_row, &d_remove_row_if,
    &d_clear_array, &d_iterator, &d_group_iterator, &d_wait_for_all, &d_counter, &d_send_message, &d_wait_message,
    &d_iterator_if, &d_insert_column, &d_remove_column, &d_get_substring, &d_broadcast_message, &d_mini_calculator,
    &d_threshold, &d_switch_on_parameter, &d_switch_on_message, &d_linear_progression,
    &d_interpolator, &d_bezier_progression, &d_random, &d_get_highest, &d_remove_key_row, &d_add_to_group,
    &d_create_string, &d_test_cell, &d_get_key_row, &d_change_value_if, &d_value_count, &d_sort_rows,
    &d_set_attribute, &d_has_attribute, &d_set_component, &d_calculator, &d_collection_iterator, &d_scan_string, &d_streaming_event, &d_per_second, &d_timer,
    &d_random_switch, &d_remove_row, &d_objects_with_attribute_iterator, &d_fill_group_by_class, &d_load_string,
    &d_get_nearest_in_group, &d_ray_intersection,
};
const unsigned bb_logics_count = sizeof bb_logics / sizeof *bb_logics;
