/* Parameters: pull-based reads through sources and operations (docs/ck-runtime.md, "Parameters"). */
#include <stdio.h>
#include "ck_types.h"
#include <stdlib.h>
#include <string.h>

void ck_param_set(CkParameter *p, const void *v, uint32_t size)
{
    if (size != p->size) {
        free(p->value);
        p->value = size ? malloc(size) : NULL;
        p->size = size;
    }
    if (size) memcpy(p->value, v, size);
}

static CkParameter *resolve(CkContext *ctx, CkParameter *p, int depth);

/* CKParameterOperation::DoOperation 0x240098bb: the function gets (context, output, input 1, input 2),
   inputs swapped when it was registered the other way round; without a function input 1 is copied. */
static void do_operation(CkContext *ctx, CkParameterOperation *op, int depth)
{
    CkParameter *out = ck_param(ctx, op->out);
    CkParameter *p1 = ck_param(ctx, op->in1), *p2 = ck_param(ctx, op->in2);
    if (!out || depth > 64) return;
    if (!op->resolved) {
        CkGuid t1 = p1 ? p1->type : CKPGUID_NONE, t2 = p2 ? p2->type : CKPGUID_NONE;
        op->fn = (void *)ck_op_function(op->op, out->type, t1, t2);
        if (!op->fn && (op->fn = (void *)ck_op_function(op->op, out->type, t2, t1))) op->swap = true;
        op->resolved = true;
        if (!op->fn && ctx->log) {
            char m[160];
            snprintf(m, sizeof m, "no operation %08x:%08x for %08x:%08x (%08x:%08x, %08x:%08x)", op->op.a, op->op.b, out->type.a, out->type.b, t1.a, t1.b, t2.a, t2.b);
            ctx->log(m);
        }
    }
    CkParameter *a = p1 ? resolve(ctx, p1, depth + 1) : NULL, *b = p2 ? resolve(ctx, p2, depth + 1) : NULL;
    if (op->fn) {
        CkOpFn fn = (CkOpFn)op->fn;
        if (op->swap) fn(ctx, out, b, a);
        else fn(ctx, out, a, b);
    } else if (a && a != out) {
        ck_param_set(out, a->value, a->size);
    }
}

static CkParameter *resolve(CkContext *ctx, CkParameter *p, int depth)
{
    /* ParameterIn -> its source (shared inputs chain to another input); ParameterOut written by an
       operation -> run the operation first (FUN_2400824d / FUN_2400a7b5). */
    while (p && p->kind == CKP_IN && depth < 64) {
        p = ck_param(ctx, p->source);
        depth++;
    }
    if (p && p->kind == CKP_OUT && p->op) {
        CkParameterOperation *op = (CkParameterOperation *)ck_obj(ctx, p->op);
        if (op && op->h.cid == CKCID_PARAMETEROPERATION) do_operation(ctx, op, depth + 1);
    }
    return p;
}

CkParameter *ck_param_resolve(CkContext *ctx, CkParameter *pin)
{
    return resolve(ctx, pin, 0);
}

const void *ck_input_value(CkContext *ctx, CkBehavior *b, uint32_t i, uint32_t *size)
{
    CkParameter *pin = i < b->pin.n ? ck_param(ctx, b->pin.v[i]) : NULL;
    CkParameter *p = pin ? ck_param_resolve(ctx, pin) : NULL;
    if (size) *size = p ? p->size : 0;
    return p ? p->value : NULL;
}

int32_t ck_input_int(CkContext *ctx, CkBehavior *b, uint32_t i)
{
    uint32_t n;
    const void *v = ck_input_value(ctx, b, i, &n);
    int32_t r = 0;
    if (v && n >= 4) memcpy(&r, v, 4);
    return r;
}

float ck_input_float(CkContext *ctx, CkBehavior *b, uint32_t i)
{
    uint32_t n;
    const void *v = ck_input_value(ctx, b, i, &n);
    float r = 0;
    if (v && n >= 4) memcpy(&r, v, 4);
    return r;
}

void ck_output_set(CkContext *ctx, CkBehavior *b, uint32_t i, const void *v, uint32_t size)
{
    CkParameter *p = i < b->pout.n ? ck_param(ctx, b->pout.v[i]) : NULL;
    if (!p) return;
    ck_param_set(p, v, size);
    /* CKParameterOut::DataChanged: destinations (e.g. parameters of other objects) get the new value */
    for (uint32_t k = 0; k < p->dests.n; k++) {
        CkParameter *d = ck_param(ctx, p->dests.v[k]);
        if (d && d != p && d->kind != CKP_IN) ck_param_set(d, v, size);
    }
}

CkId ck_behavior_target(CkContext *ctx, CkBehavior *b)
{
    CkParameter *t = b->target ? ck_param(ctx, b->target) : NULL;
    if (t) {
        CkParameter *p = t->kind == CKP_IN ? ck_param_resolve(ctx, t) : t;
        CkId id = 0;
        if (p && p->value && p->size >= 4) memcpy(&id, p->value, 4);
        return id;
    }
    return b->owner;
}
