/* The OO watcher (docs/ivp_collision.md 3.5) and the child exchange (0x10016650, 3.4): for a pair where either
   surface has several ledges (every concave floor, multi-convex modules), one plain mindist per ledge pair in
   range, kept across rechecks while the pair stays in range. */
#include "phys_internal.h"
#include <stdlib.h>

/* 0x10016650 exchange(obj0, obj1, dist, children, NULL, NULL, NULL, NULL, delegator): the ledges of each
   object within the other's core radius + the extra radius + dist of the other's core position now */
static void exchange(PhysWorld *w, IvpWatcher *wt, double dist)
{
    PhysBody *o[2] = {wt->syn[0].obj, wt->syn[1].obj};
    IvpLedgeVec L[2] = {{0}, {0}};
    for (int k = 0; k < 2; k++) {
        PhysBody *self = o[k], *other = o[k ^ 1];
        if (self->coll.type == IVP_OBJ_BALL) {
            /* 0x1002fe90: the dummy ledge when the other core is within the radius */
            double c[3], d2 = 0;
            ivp_core_pos_at(&other->core, w->current_time, c);
            const IvpCache *ca = ivp_cache_get(w, self);
            double r = (double)(self->extra_radius + other->core.upper_limit_radius) + dist;
            for (int i = 0; i < 3; i++) {
                double x = ca->r[0][i] * (c[0] - ca->vv[0]) + ca->r[1][i] * (c[1] - ca->vv[1]) + ca->r[2][i] * (c[2] - ca->vv[2]);
                d2 += x * x;
            }
            if (!(r * r < d2)) {
                L[k].v = malloc(sizeof *L[k].v);
                L[k].v[0] = ivp_ball_ledge();
                L[k].n = L[k].cap = 1;
            }
            continue;
        }
        double c[3], c_os[3];
        ivp_core_pos_at(&other->core, w->current_time, c);      /* 0x10012470 */
        double r = (double)(self->extra_radius + other->core.upper_limit_radius) + dist;
        const IvpCache *ca = ivp_cache_get(w, self);
        double d[3] = {c[0] - ca->vv[0], c[1] - ca->vv[1], c[2] - ca->vv[2]};
        ivp_rtmul(ca->r, d, c_os);                               /* 0x1000f3e0 */
        ivp_surface_ledges_within_radius(self->shape->cs, c_os, r, &L[k]);
    }
    /* keep the children whose ledge pair is still found (moved into the kept prefix), create the missing ones */
    uint32_t keep = 0;
    IvpMindist **fresh = NULL;
    uint32_t nfresh = 0, capfresh = 0;
    for (uint32_t ia = L[0].n; ia-- > 0;)
        for (uint32_t ib = L[1].n; ib-- > 0;) {
            const IvpCompactLedge *a = L[0].v[ia], *b = L[1].v[ib];
            int32_t found = -1;
            for (uint32_t i = keep; i < wt->n && found < 0; i++)
                if (ivp_mindist_ledge(wt->children[i], 0) == a && ivp_mindist_ledge(wt->children[i], 1) == b) found = (int32_t)i;
            if (found >= 0) {
                if ((int32_t)keep < found) {
                    /* 0x100188c0 */
                    IvpMindist *x = wt->children[keep], *y = wt->children[found];
                    wt->children[keep] = y, wt->children[found] = x;
                    y->coll.fvec[0] = (int32_t)keep, x->coll.fvec[0] = found;
                }
                keep++;
                continue;
            }
            assert(!ivp_has_children(a) && !ivp_has_children(b));
            IvpMindist *m = ivp_mindist_create(w, wt, o[0], o[1], ivp_first_edge(a), ivp_first_edge(b));
            if (nfresh == capfresh) capfresh = capfresh ? capfresh * 2 : 16, fresh = realloc(fresh, capfresh * sizeof *fresh);
            fresh[nfresh++] = m;
        }
    while (wt->n > keep) ivp_mindist_delete(w, wt->children[wt->n - 1]);   /* child_deleted shrinks the array */
    for (uint32_t i = nfresh; i-- > 0;) {
        if (wt->n == wt->cap) wt->cap = wt->cap ? wt->cap * 2 : 8, wt->children = realloc(wt->children, wt->cap * sizeof *wt->children);
        fresh[i]->coll.fvec[0] = (int32_t)wt->n;                         /* 0x10016d60 */
        wt->children[wt->n++] = fresh[i];
    }
    free(fresh);
    free(L[0].v);
    free(L[1].v);
}

static void rearm(PhysWorld *w, IvpWatcherSynapse *s, double r)
{
    IvpHullManager *h = &s->obj->coll.hull;
    ivp_hull_remove(h, &s->hl);
    ivp_hull_insert(h, &s->hl, (float)(ivp_hull_value_at(h, w->current_time) + r));
}

/* 0x10037e30 */
void ivp_watcher_check(PhysWorld *w, IvpWatcher *wt)
{
    w->coll.watcher_checks++;
    double r0, r1;
    ivp_range_intra(w, wt->syn[0].obj, wt->syn[1].obj, &r0, &r1);
    exchange(w, wt, r0 + r1);
    rearm(w, &wt->syn[0], r0);
    rearm(w, &wt->syn[1], r1);
}

/* 0x10038000 */
IvpWatcher *ivp_watcher_create(PhysWorld *w, PhysBody *a, PhysBody *b)
{
    IvpWatcher *wt = calloc(1, sizeof *wt);
    wt->coll.kind = IVP_COLL_WATCHER;
    wt->coll.owner = NULL;
    wt->coll.fvec[0] = wt->coll.fvec[1] = -1;
    PhysBody *o[2] = {a, b};
    for (int k = 0; k < 2; k++) {
        IvpWatcherSynapse *s = &wt->syn[k];
        s->hl.kind = IVP_HL_WATCHER;
        s->obj = o[k];
        s->w = wt;
        IvpHullManager *h = &o[k]->coll.hull;   /* 0x10037fb0: "never", overwritten at once */
        ivp_hull_insert(h, &s->hl, (float)(ivp_hull_value_at(h, w->current_time) + 1.0000000200408773e20));
    }
    ivp_watcher_check(w, wt);
    return wt;
}

/* 0x100381d0 child_deleted: the last child moves into the hole */
void ivp_watcher_child_deleted(IvpWatcher *wt, IvpMindist *md)
{
    int32_t i = md->coll.fvec[0];
    if (i < 0 || (uint32_t)i >= wt->n || wt->children[i] != md) return;
    wt->n--;
    if ((uint32_t)i != wt->n) {
        wt->children[i] = wt->children[wt->n];
        wt->children[i]->coll.fvec[0] = i;
    }
    md->coll.fvec[0] = -1;
}

/* 0x10038100 */
void ivp_watcher_delete(PhysWorld *w, IvpWatcher *wt)
{
    while (wt->n) ivp_mindist_delete(w, wt->children[wt->n - 1]);
    ivp_collision_deleted(w, &wt->coll);
    free(wt->children);
    for (int k = 0; k < 2; k++) ivp_hull_remove(&wt->syn[k].obj->coll.hull, &wt->syn[k].hl);   /* 0x10037f60 */
    free(wt);
}
