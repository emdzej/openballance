/* The behavior scheduler: CKBehaviorManager::Execute, CKBehavior::Execute / Activate and the graph
   helpers, ported from CK2.dll. See docs/ck-runtime.md for the rules in prose. */
#include "ck_3d.h"
#include "ck_sound.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LINK_QUEUED = 1, LINK_FIRED = 2 };

static void logf_(CkContext *ctx, const char *fmt, const char *a, const char *b)
{
    if (!ctx->log) return;
    char buf[256];
    snprintf(buf, sizeof buf, fmt, a, b);
    ctx->log(buf);
}

/* ---- IO ---- */

bool ck_input_active(CkContext *ctx, CkBehavior *b, uint32_t i)
{
    CkBehaviorIO *io = i < b->in.n ? ck_io(ctx, b->in.v[i]) : NULL;
    return io && io->active;
}

/* CKBehavior::ActivateInput 0x240028b8 */
void ck_activate_input(CkContext *ctx, CkBehavior *b, uint32_t i, bool on)
{
    CkBehaviorIO *io = i < b->in.n ? ck_io(ctx, b->in.v[i]) : NULL;
    if (io) io->active = on;
}

/* CKBehavior::ActivateOutput 0x2400270c */
void ck_activate_output(CkContext *ctx, CkBehavior *b, uint32_t i, bool on)
{
    CkBehaviorIO *io = i < b->out.n ? ck_io(ctx, b->out.v[i]) : NULL;
    if (io) io->active = on;
}

/* ---- Activate / reset ---- */

/* FUN_24005dc8 */
static void behavior_reset(CkContext *ctx, CkBehavior *b)
{
    for (uint32_t i = 0; i < b->links.n; i++) {
        CkBehaviorLink *l = ck_link(ctx, b->links.v[i]);
        if (l) l->cur_delay = l->init_delay;
    }
    for (uint32_t i = 0; i < b->delayed.n; i++) {
        CkBehaviorLink *l = ck_link(ctx, b->delayed.v[i]);
        if (l) l->lflags &= ~LINK_QUEUED;
    }
    b->delayed.n = 0;
    for (uint32_t i = 0; i < b->sub.n; i++) {
        CkBehavior *s = ck_behavior(ctx, b->sub.v[i]);
        if (!s) continue;
        s->bflags &= ~CKBF_ACTIVE;
        behavior_reset(ctx, s);
    }
    for (uint32_t i = 0; i < b->in.n; i++) ck_activate_input(ctx, b, i, false);
    for (uint32_t i = 0; i < b->out.n; i++) ck_activate_output(ctx, b, i, false);
    uint32_t f = b->bflags;
    b->bflags = (f & 0xef3fffbf) | CKBF_RESET_DONE;
    if (f & CKBF_SCRIPT) ck_activate_input(ctx, b, 0, true);
}

/* CKBehavior::Activate 0x24003957 */
void ck_behavior_activate(CkContext *ctx, CkBehavior *b, bool active, bool reset)
{
    if (reset || b->bflags == 0) behavior_reset(ctx, b);
    if (active) b->bflags |= CKBF_ACTIVE;
    else b->bflags &= ~CKBF_ACTIVE;
}

/* ---- graph execution ---- */

static void queue_link(CkBehavior *g, CkBehaviorLink *l)
{
    if (l->cur_delay == 0) l->cur_delay = l->init_delay;
    if (!(l->lflags & LINK_QUEUED)) {
        l->lflags |= LINK_QUEUED;
        ck_ids_push(&g->delayed, l->h.id);
    }
}

/* FUN_240044ec: consume active IOs through the links, then build the execution stack. */
static void graph_prepare(CkContext *ctx, CkBehavior *g)
{
    CkIds sources = {0}, dests = {0};
    for (uint32_t i = 0; i < g->links.n; i++) {
        CkBehaviorLink *l = ck_link(ctx, g->links.v[i]);
        if (!l) continue;
        l->lflags &= ~LINK_FIRED;
        CkBehaviorIO *src = ck_io(ctx, l->src);
        if (!src || !src->active) continue;
        ck_ids_push(&sources, src->h.id);
        l->lflags |= LINK_FIRED;
        if (l->init_delay == 0) ck_ids_push(&dests, l->dst);
        else queue_link(g, l);
    }
    for (uint32_t i = 0; i < sources.n; i++) ck_io(ctx, sources.v[i])->active = false;
    for (uint32_t i = 0; i < dests.n; i++) {
        CkBehaviorIO *d = ck_io(ctx, dests.v[i]);
        if (!d) continue;
        d->active = true;
        CkBehavior *owner = d->input ? ck_behavior(ctx, d->owner) : NULL;
        if (owner) owner->bflags |= CKBF_ACTIVE;
    }
    free(sources.v);
    free(dests.v);
    g->stack.n = 0;
    for (uint32_t i = g->sub.n; i-- > 0;) {
        CkBehavior *s = ck_behavior(ctx, g->sub.v[i]);
        if (s && s != g && (s->bflags & CKBF_ACTIVE)) {
            s->bflags |= CKBF_STACKED;
            ck_ids_push(&g->stack, s->h.id);
        }
    }
}

/* FUN_2400477f: propagate the active outputs of s (just executed) inside graph g. */
static void graph_propagate(CkContext *ctx, CkBehavior *g, CkBehavior *s)
{
    for (uint32_t i = 0; i < s->out.n; i++) {
        CkBehaviorIO *out = ck_io(ctx, s->out.v[i]);
        if (!out || !out->active) continue;
        out->active = false;
        for (uint32_t k = 0; k < out->links.n; k++) {
            CkBehaviorLink *l = ck_link(ctx, out->links.v[k]);
            if (!l) continue;
            l->lflags |= LINK_FIRED;
            if (l->init_delay != 0) {
                queue_link(g, l);
                continue;
            }
            CkBehaviorIO *d = ck_io(ctx, l->dst);
            if (!d) continue;
            d->active = true;
            CkBehavior *owner = ck_behavior(ctx, d->owner);
            if (!d->input || !owner || owner == g) continue;
            owner->bflags |= CKBF_ACTIVE;
            if (owner->bflags & CKBF_STACKED) continue;
            owner->bflags |= CKBF_STACKED;
            /* insert by priority: above every entry whose priority is <= the owner's */
            int j = (int)g->stack.n - 1;
            while (j >= 0) {
                CkBehavior *t = ck_behavior(ctx, g->stack.v[j]);
                if (t->priority <= owner->priority) break;
                j--;
            }
            ck_ids_push(&g->stack, 0);
            memmove(&g->stack.v[j + 2], &g->stack.v[j + 1], (g->stack.n - 2 - (uint32_t)j) * sizeof(CkId));
            g->stack.v[j + 1] = owner->h.id;
        }
    }
}

/* FUN_2400465b: count down queued links; decide whether the graph stays active. */
static void graph_end(CkContext *ctx, CkBehavior *g)
{
    CkIds keep = {0};
    for (uint32_t i = 0; i < g->delayed.n; i++) {
        CkBehaviorLink *l = ck_link(ctx, g->delayed.v[i]);
        if (!l) continue;
        l->lflags |= LINK_FIRED;
        l->cur_delay--;
        if (l->cur_delay < 1) {
            l->cur_delay = 0;
            l->lflags &= ~LINK_QUEUED;
            CkBehaviorIO *d = ck_io(ctx, l->dst);
            if (d) {
                CkBehavior *owner = ck_behavior(ctx, d->owner);
                if (owner) owner->bflags |= CKBF_ACTIVE;
                d->active = true;
            }
        } else {
            ck_ids_push(&keep, l->h.id);
        }
    }
    free(g->delayed.v);
    g->delayed = keep;
    g->bflags &= ~CKBF_ACTIVE;
    bool stay = g->delayed.n > 0;
    for (uint32_t i = 0; !stay && i < g->sub.n; i++) {
        CkBehavior *s = ck_behavior(ctx, g->sub.v[i]);
        if (s && (s->bflags & 0x41)) stay = true;
    }
    if (stay) g->bflags |= CKBF_ACTIVE;
}

/* A Building Block without an implementation: trace it once, pass the flow through output 0. */
static int bb_missing(CkContext *ctx, CkBehavior *b)
{
    if (ctx->trace) ctx->trace(ctx, b, "missing");
    for (uint32_t i = 0; i < b->in.n; i++) ck_activate_input(ctx, b, i, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* CKBehavior::Execute 0x24003806 */
bool ck_behavior_execute(CkContext *ctx, CkBehavior *b)
{
    bool ok = true;
    if (b->has_proto || (b->bflags & CKBF_BUILDINGBLOCK)) {
        /* FUN_240048c4 + FUN_2400498d */
        if (ctx->trace) ctx->trace(ctx, b, "execute");
        int r = b->bb && b->bb->fn ? b->bb->fn(ctx, b) : bb_missing(ctx, b);
        if (!(r & 1)) b->bflags &= ~CKBF_ACTIVE;
        return true;
    }
    graph_prepare(ctx, b);
    int iterations = 0;
    while (b->stack.n > 0) {
        if (iterations > ctx->max_iterations) {
            /* FUN_24005e7c: the original logs and keeps going, once per extra iteration */
            logf_(ctx, "ERROR : Infinite Loop Detected (%s / %s)", ck_obj(ctx, b->owner) ? ck_obj(ctx, b->owner)->name : "?", b->h.name);
            ok = false;
            break;
        }
        CkBehavior *s = ck_behavior(ctx, b->stack.v[--b->stack.n]);
        s->bflags &= ~CKBF_STACKED;
        iterations++;
        ck_behavior_execute(ctx, s);
        graph_propagate(ctx, b, s);
    }
    graph_end(ctx, b);
    return ok;
}

/* ---- frames ---- */

static void manage(CkContext *ctx, CkId be)
{
    if (!ck_ids_has(&ctx->managed, be)) ck_ids_push(&ctx->managed, be);
}

/* CKBehaviorManager::Execute 0x2400c40c + CKBeObject::ExecuteBehaviors 0x2401af67 */
void ck_process(CkContext *ctx, float delta_ms)
{
    ctx->delta_ms = delta_ms;
    ctx->text_draws.n = 0;          /* 2D Text re-registers its post-render callback every frame */
    for (uint32_t i = 0; i < ctx->managed.n; i++) {
        CkBeObject *be = ck_beobject(ctx, ctx->managed.v[i]);
        for (uint32_t k = 0; be && k < be->scripts.n; k++) {
            CkBehavior *s = ck_behavior(ctx, be->scripts.v[k]);
            if (s && (s->bflags & (CKBF_REQ_ACTIVATE | CKBF_REQ_DEACTIVATE))) {
                ck_behavior_activate(ctx, s, (s->bflags & CKBF_REQ_ACTIVATE) != 0, (s->bflags & CKBF_REQ_RESET) != 0);
                s->bflags &= ~(CKBF_REQ_ACTIVATE | CKBF_REQ_DEACTIVATE | CKBF_REQ_RESET);
            }
        }
    }
    for (uint32_t i = 0; i < ctx->managed.n; i++) {
        CkBeObject *be = ck_beobject(ctx, ctx->managed.v[i]);
        for (uint32_t k = 0; be && k < be->scripts.n; k++) {
            CkBehavior *s = ck_behavior(ctx, be->scripts.v[k]);
            if (s && (s->bflags & CKBF_ACTIVE)) ck_behavior_execute(ctx, s);
        }
    }
    /* managers' PostProcess: the message manager delivers this frame's messages; the physics steps and
       writes back; sounds fade */
    ck_deliver_messages(ctx);
    if (ctx->physics_frame) ctx->physics_frame(ctx);
    ck_sound_update(ctx);
    ctx->frame++;
}

/* CKLevel::LaunchScene 0x2402c168 with the level scene and default flags -> FUN_2402e096 */
void ck_launch_level_scene(CkContext *ctx)
{
    CkBeObject *level = ck_beobject(ctx, ctx->level);
    if (!level) return;
    /* CKContext::Reset before Play: every Building Block gets CKM_BEHAVIORRESET */
    for (uint32_t i = 0; i < ctx->nobjs; i++) {
        CkBehavior *b = ck_behavior(ctx, i + 1);
        if (b && b->bb && b->bb->callback) b->bb->callback(ctx, b, CKM_BEHAVIORRESET);
    }
    if (level->scripts.n) manage(ctx, level->h.id);
    if (ctx->scene_flags & 2) {            /* CKScene::ApplyEnvironmentSettings 0x2402d736 */
        if (ck_obj(ctx, ctx->scene_env.camera)) ctx->camera = ctx->scene_env.camera;
        ctx->background = ctx->scene_env.background;
        ctx->fog_color = ctx->scene_env.fog_color;
        ctx->fog_mode = ctx->scene_env.fog_mode;
        ctx->fog_start = ctx->scene_env.fog_start;
        ctx->fog_end = ctx->scene_env.fog_end;
        ctx->fog_density = ctx->scene_env.fog_density;
        ctx->ambient = ctx->scene_env.ambient;
    }
    for (uint32_t i = 0; i < level->nscene; i++) {
        CkSceneEntry *e = &level->scene[i];
        CkObj *o = ck_obj(ctx, e->obj);
        if (!o) continue;
        bool activate = e->flags & 1, leave = e->flags & 0x20, reset = e->flags & 0x40;
        if (reset && (e->has_initial || e->snap)) ck_restore_initial_state(ctx, o->id);
        if (leave) {
            reset = false;
            if (o->cid == CKCID_BEHAVIOR) activate = (((CkBehavior *)o)->bflags & CKBF_ACTIVE) != 0;
        }
        if (activate) e->flags |= 8;
        else e->flags &= ~8u;
        if (o->cid == CKCID_BEHAVIOR) {
            CkBehavior *b = (CkBehavior *)o;
            b->bflags &= ~(CKBF_REQ_ACTIVATE | CKBF_REQ_DEACTIVATE | CKBF_REQ_RESET);
            ck_behavior_activate(ctx, b, (e->flags & 8) != 0, reset);
        }
        if ((e->flags & 8) && ck_is_beobject_class(o->cid) && ((CkBeObject *)o)->scripts.n) manage(ctx, o->id);
    }
}

/* ---- scene activation ---- */

static CkSceneEntry *scene_entry(const CkContext *ctx, CkId obj)
{
    CkBeObject *level = ck_beobject(ctx, ctx->level);
    for (uint32_t i = 0; level && i < level->nscene; i++)
        if (level->scene[i].obj == obj) return &level->scene[i];
    return NULL;
}

const CkSceneEntry *ck_scene_entry(const CkContext *ctx, CkId id) { return scene_entry(ctx, id); }

static CkSceneEntry *scene_add_entry(CkContext *ctx, CkId obj);

void ck_save_initial_state(CkContext *ctx, CkId id)
{
    CkSceneEntry *e = scene_entry(ctx, id);
    Ck3dEntity *en = ck_entity(ctx, id);
    if (!e || !en) return;
    if (!e->snap) e->snap = calloc(1, sizeof *e->snap);
    memcpy(e->snap->world, en->world, sizeof en->world);
    e->snap->objflags = en->be.h.flags, e->snap->flags = en->flags, e->snap->moveable = en->moveable;
    e->snap->mesh = en->mesh, e->snap->parent = en->parent;
}

void ck_restore_initial_state(CkContext *ctx, CkId id)
{
    CkSceneEntry *e = scene_entry(ctx, id);
    if (!e) return;
    Ck3dEntity *en = ck_entity(ctx, id);
    if (e->snap && en) {
        memcpy(en->world, e->snap->world, sizeof en->world);
        en->be.h.flags = e->snap->objflags, en->flags = e->snap->flags, en->moveable = e->snap->moveable;
        en->mesh = e->snap->mesh, en->parent = e->snap->parent;
        ctx->hier_gen++;
    } else if (e->has_initial) {
        ck_read_object_state(ctx, id, &e->initial, e->file);
    }
}

void ck_scene_add_copy(CkContext *ctx, CkId id, uint32_t flags)
{
    if (scene_entry(ctx, id)) return;
    CkSceneEntry *e = scene_add_entry(ctx, id);
    if (e) e->flags = flags;
}

bool ck_scene_is_active(const CkContext *ctx, CkId obj)
{
    if (obj == ctx->level) return true;
    CkSceneEntry *e = scene_entry(ctx, obj);
    return e && (e->flags & 8);
}

/* CKBehavior::CallSubBehaviorsCallbackFunction: the message to every Building Block of the script's tree */
void ck_behavior_callback_tree(CkContext *ctx, CkBehavior *b, uint32_t msg)
{
    if (b->bb && b->bb->callback) b->bb->callback(ctx, b, msg);
    for (uint32_t i = 0; i < b->sub.n; i++) {
        CkBehavior *s = ck_behavior(ctx, b->sub.v[i]);
        if (s) ck_behavior_callback_tree(ctx, s, msg);
    }
}

static void scripts_callback(CkContext *ctx, CkBeObject *be, uint32_t msg)
{
    for (uint32_t i = 0; i < be->scripts.n; i++) {
        CkBehavior *s = ck_behavior(ctx, be->scripts.v[i]);
        if (s) ck_behavior_callback_tree(ctx, s, msg);
    }
}

void ck_add_pre_render(CkContext *ctx, void (*fn)(CkContext *ctx, CkBehavior *b), CkId beh)
{
    for (uint32_t i = 0; i < ctx->npre_render; i++)
        if (ctx->pre_render[i].fn == fn && ctx->pre_render[i].beh == beh) return;
    ctx->pre_render = realloc(ctx->pre_render, (ctx->npre_render + 1) * sizeof *ctx->pre_render);
    ctx->pre_render[ctx->npre_render++] = (struct CkPreRender){fn, beh};
}

void ck_remove_pre_render(CkContext *ctx, void (*fn)(CkContext *ctx, CkBehavior *b), CkId beh)
{
    for (uint32_t i = 0; i < ctx->npre_render; i++)
        if (ctx->pre_render[i].fn == fn && ctx->pre_render[i].beh == beh) {
            memmove(&ctx->pre_render[i], &ctx->pre_render[i + 1], (ctx->npre_render - i - 1) * sizeof *ctx->pre_render);
            ctx->npre_render--;
            return;
        }
}

void ck_run_pre_render(CkContext *ctx)
{
    uint32_t n = ctx->npre_render;
    struct CkPreRender *list = ctx->pre_render;
    ctx->pre_render = NULL, ctx->npre_render = 0;
    for (uint32_t i = 0; i < n; i++) {
        CkBehavior *b = ck_behavior(ctx, list[i].beh);
        if (b) list[i].fn(ctx, b);
    }
    free(list);
}

/* FUN_2402eeb0 (behavior) / FUN_2402ed90 (BeObject), activate = 1 */
void ck_scene_activate(CkContext *ctx, CkId id, bool reset)
{
    CkObj *o = ck_obj(ctx, id);
    if (!o) return;
    CkSceneEntry *e = scene_entry(ctx, id);
    if (o->cid == CKCID_BEHAVIOR) {
        CkBehavior *b = (CkBehavior *)o;
        if (e) e->flags |= 8;
        CkObj *owner = ck_obj(ctx, b->owner);
        if (owner && (ck_scene_is_active(ctx, owner->id) || owner->cid == CKCID_LEVEL)) manage(ctx, owner->id);
        b->bflags = (b->bflags & ~CKBF_REQ_DEACTIVATE) | CKBF_REQ_ACTIVATE | (reset ? CKBF_REQ_RESET : 0);
    } else if (ck_is_beobject_class(o->cid)) {
        if (e && !(e->flags & 8)) {
            e->flags |= 8;
            if (((CkBeObject *)o)->scripts.n) manage(ctx, id);
            if (reset && (e->flags & 0x40) && (e->has_initial || e->snap)) ck_restore_initial_state(ctx, id);
            scripts_callback(ctx, (CkBeObject *)o, CKM_BEHAVIORACTIVATESCRIPT);
        } else if (!e && ((CkBeObject *)o)->scripts.n) {
            manage(ctx, id);
        }
    }
}

void ck_scene_deactivate(CkContext *ctx, CkId id)
{
    CkObj *o = ck_obj(ctx, id);
    if (!o) return;
    CkSceneEntry *e = scene_entry(ctx, id);
    if (o->cid == CKCID_BEHAVIOR) {
        CkBehavior *b = (CkBehavior *)o;
        b->bflags = (b->bflags & ~(CKBF_REQ_ACTIVATE | CKBF_REQ_RESET)) | CKBF_REQ_DEACTIVATE;
        if (e) e->flags &= ~8u;
    } else if (e) {
        bool was_active = (e->flags & 8) != 0;
        e->flags &= ~8u;
        /* FUN_2400c672: the manager stops executing the object's scripts */
        for (uint32_t i = 0; i < ctx->managed.n; i++)
            if (ctx->managed.v[i] == id) {
                memmove(&ctx->managed.v[i], &ctx->managed.v[i + 1], (ctx->managed.n - i - 1) * sizeof(CkId));
                ctx->managed.n--;
                break;
            }
        if (was_active && ck_is_beobject_class(o->cid)) scripts_callback(ctx, (CkBeObject *)o, CKM_BEHAVIORDEACTIVATESCRIPT);
    }
}

static CkSceneEntry *scene_add_entry(CkContext *ctx, CkId obj)
{
    CkBeObject *level = ck_beobject(ctx, ctx->level);
    if (!level) return NULL;
    level->scene = realloc(level->scene, (level->nscene + 1) * sizeof *level->scene);
    CkSceneEntry *e = &level->scene[level->nscene++];
    memset(e, 0, sizeof *e);
    e->obj = obj;
    return e;
}

/* CKScene::AddObject 0x2402dc8a */
static void scene_add_object(CkContext *ctx, CkObj *o)
{
    if (o->cid == CKCID_SCENE || scene_entry(ctx, o->id)) return;
    CkSceneEntry *e = scene_add_entry(ctx, o->id);
    if (!e || !o->has_activity) return;
    uint32_t f = o->activity;
    if (f & 0x80) {                       /* initial value: the state as loaded */
        f &= ~0x80u;
        if (o->chunk) {
            e->initial = *o->chunk;
            e->has_initial = true;
            e->file = o->file;
        }
    }
    e->flags = f & ~8u;
    if (f & 1) ck_scene_activate(ctx, o->id, (f & 0x40) != 0);
    else if (f & 0x10) ck_scene_deactivate(ctx, o->id);
}

void ck_level_add_object(CkContext *ctx, CkId id)
{
    CkObj *o = ck_obj(ctx, id);
    if (!o || !ck_class_derives(o->cid, CKCID_SCENEOBJECT) || o->cid == CKCID_LEVEL) return;
    scene_add_object(ctx, o);
    if (ck_is_beobject_class(o->cid)) {   /* CKBeObject::AddToScene with dependencies: the scripts too */
        CkBeObject *be = (CkBeObject *)o;
        for (uint32_t i = 0; i < be->scripts.n; i++) {
            CkBehavior *s = ck_behavior(ctx, be->scripts.v[i]);
            if (s && (s->bflags & CKBF_SCRIPT)) scene_add_object(ctx, &s->h);
        }
    }
}
