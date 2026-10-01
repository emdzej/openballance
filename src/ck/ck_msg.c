/* CKMessageManager: queue, waits and delivery (CK2.dll 0x2400d0c9-0x2400e33a). */
#include "ck.h"
#include <stdlib.h>
#include <string.h>

enum { SEND_BROADCAST = 1, SEND_SINGLE = 2, SEND_GROUP = 3 };
enum { CKBF_WAITING_MESSAGE = 0x40 };

CkMessage *ck_send_message(CkContext *ctx, int32_t type, uint32_t send, uint32_t recipient, CkId sender)
{
    if (type < 0) return NULL;
    CkMessage *m = calloc(1, sizeof *m);
    m->type = type;
    m->send = send;
    m->recipient = recipient;
    m->sender = sender;
    if (ctx->nqueue == ctx->capqueue) {
        ctx->capqueue += 100;
        ctx->queue = realloc(ctx->queue, ctx->capqueue * sizeof *ctx->queue);
    }
    ctx->queue[ctx->nqueue++] = m;
    return m;
}

static void ensure_type(CkContext *ctx, int32_t type)
{
    if ((uint32_t)type < ctx->nwait_types) return;
    uint32_t n = (uint32_t)type + 1;
    ctx->waits = realloc(ctx->waits, n * sizeof *ctx->waits);
    ctx->nwaits = realloc(ctx->nwaits, n * sizeof *ctx->nwaits);
    for (uint32_t i = ctx->nwait_types; i < n; i++) {
        ctx->waits[i] = NULL;
        ctx->nwaits[i] = 0;
    }
    ctx->nwait_types = n;
}

void ck_register_wait(CkContext *ctx, int32_t type, CkBehavior *b, uint32_t output, CkId obj)
{
    if (type < 0 || !b) return;
    ensure_type(ctx, type);
    CkId io = output < b->out.n ? b->out.v[output] : 0;
    b->bflags |= CKBF_WAITING_MESSAGE;
    struct CkWait *w = ctx->waits[type];
    for (uint32_t i = 0; i < ctx->nwaits[type]; i++)
        if (w[i].beh == b->h.id && w[i].io == io) return;   /* already waiting (CKERR_ALREADYPRESENT) */
    ctx->waits[type] = w = realloc(w, (ctx->nwaits[type] + 1) * sizeof *w);
    w[ctx->nwaits[type]++] = (struct CkWait){obj, b->h.id, io};
}

static void remove_wait(CkContext *ctx, int32_t type, uint32_t i)
{
    memmove(&ctx->waits[type][i], &ctx->waits[type][i + 1], (ctx->nwaits[type] - i - 1) * sizeof(struct CkWait));
    ctx->nwaits[type]--;
}

void ck_unregister_wait(CkContext *ctx, int32_t type, CkBehavior *b, int32_t output)
{
    if (type < 0 || (uint32_t)type >= ctx->nwait_types) return;
    CkId io = b && output >= 0 && (uint32_t)output < b->out.n ? b->out.v[output] : 0;
    for (uint32_t i = 0; i < ctx->nwaits[type];) {
        struct CkWait *w = &ctx->waits[type][i];
        if (w->beh == (b ? b->h.id : 0) && (!io || w->io == io)) {
            remove_wait(ctx, type, i);
            if (b) b->bflags &= ~CKBF_WAITING_MESSAGE;
        } else {
            i++;
        }
    }
}

static void add_last_frame(CkContext *ctx, CkBeObject *be, CkMessage *m)
{
    be->last_frame = realloc(be->last_frame, (be->nlast + 1) * sizeof *be->last_frame);
    be->last_frame[be->nlast++] = m;
    if (!ck_ids_has(&ctx->received, be->h.id)) ck_ids_push(&ctx->received, be->h.id);
}

/* FUN_2400dbd3: an object that waits for messages gets the message; group sends recurse. */
static void deliver_to(CkContext *ctx, CkObj *o, CkMessage *m, bool recurse)
{
    if (!o || !ck_is_beobject_class(o->cid)) return;
    CkBeObject *be = (CkBeObject *)o;
    if (be->waiting) add_last_frame(ctx, be, m);   /* TODO: CKScene::IsObjectActive */
    if (recurse && m->send == SEND_GROUP && o->cid == CKCID_GROUP) {
        CkGroup *g = (CkGroup *)o;
        for (uint32_t i = 0; i < g->members.n; i++) deliver_to(ctx, ck_obj(ctx, g->members.v[i]), m, true);
    }
}

static bool in_group(CkContext *ctx, CkId obj, CkObj *group)
{
    if (!group || group->cid != CKCID_GROUP) return false;
    return ck_ids_has(&((CkGroup *)group)->members, obj);
}

void ck_deliver_messages(CkContext *ctx)
{
    /* last frame's messages are dropped */
    for (uint32_t i = 0; i < ctx->received.n; i++) {
        CkBeObject *be = ck_beobject(ctx, ctx->received.v[i]);
        if (be) be->nlast = 0;
    }
    ctx->received.n = 0;
    for (uint32_t q = 0; q < ctx->nqueue; q++) {
        CkMessage *m = ctx->queue[q];
        CkObj *target = NULL;
        if (m->send == SEND_BROADCAST) {
            for (uint32_t i = 0; i < ctx->nobjs; i++)
                if (ctx->objs[i] && ck_class_derives(ctx->objs[i]->cid, m->recipient)) deliver_to(ctx, ctx->objs[i], m, false);
        } else {
            target = ck_obj(ctx, m->recipient);
            deliver_to(ctx, target, m, true);
        }
        if ((uint32_t)m->type >= ctx->nwait_types) continue;
        for (uint32_t i = 0; i < ctx->nwaits[m->type];) {
            struct CkWait *w = &ctx->waits[m->type][i];
            CkObj *wo = ck_obj(ctx, w->obj);
            bool match = (target && w->obj == target->id) ||
                         (m->send == SEND_BROADCAST && wo && ck_class_derives(wo->cid, m->recipient)) ||
                         (m->send == SEND_GROUP && in_group(ctx, w->obj, target));
            if (!match) {
                i++;
                continue;
            }
            CkBeObject *be = ck_beobject(ctx, w->obj);
            if (be) add_last_frame(ctx, be, m);
            CkBehavior *b = ck_behavior(ctx, w->beh);
            CkBehaviorIO *io = ck_io(ctx, w->io);
            if (io && b && (b->bflags & CKBF_WAITING_MESSAGE)) io->active = true;
            if (b) b->bflags &= ~CKBF_WAITING_MESSAGE;
            remove_wait(ctx, m->type, i);
        }
    }
    /* messages stay referenced by last_frame lists until the next delivery; free the previous batch */
    static CkMessage **prev;
    static uint32_t nprev;
    for (uint32_t i = 0; i < nprev; i++) {
        free(prev[i]->params.v);
        free(prev[i]);
    }
    free(prev);
    prev = ctx->queue;
    nprev = ctx->nqueue;
    ctx->queue = NULL;
    ctx->nqueue = ctx->capqueue = 0;
}
