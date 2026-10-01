/* The min list, the time manager and its event loop, and the per-object hull manager
   (docs/ivp_collision.md 2.1.4-2.3, 7.1). */
#include "phys_internal.h"
#include <stdlib.h>

/* ---- IVP_U_Min_List (0x100300f0 / 0x10030180 / 0x10030470): a sorted doubly linked list. A new element
   goes before the first one with a value >= its own, so ties keep the newest first; a binary heap would not
   (2.1.5). The original's skip links only speed up the search. ---- */

void ivp_minlist_init(IvpMinList *l)
{
    memset(l, 0, sizeof *l);
    l->free_head = l->first = l->last = IVP_NO_INDEX;
    l->min_value = 1e10f;
}

void ivp_minlist_free(IvpMinList *l)
{
    free(l->e);
    ivp_minlist_init(l);
}

uint32_t ivp_minlist_add(IvpMinList *l, void *payload, float value)
{
    if (l->free_head == IVP_NO_INDEX) {
        uint32_t old = l->cap;
        l->cap = old * 2 + 1;     /* grow to 2n + 1 */
        l->e = realloc(l->e, l->cap * sizeof *l->e);
        for (uint32_t i = l->cap; i-- > old;) l->e[i].next = l->free_head, l->free_head = i;
    }
    uint32_t i = l->free_head;
    l->free_head = l->e[i].next;
    IvpMinListElem *x = &l->e[i];
    x->value = value;
    x->payload = payload;
    if (!l->n || value <= l->min_value) {
        x->prev = IVP_NO_INDEX;
        x->next = l->first;
        if (l->first != IVP_NO_INDEX) l->e[l->first].prev = i;
        else l->last = i;
        l->first = i;
        l->min_value = value;
    } else {
        /* after the last element with a smaller value (= before the first one >= value) */
        uint32_t p = l->last;
        while (l->e[p].value >= value) p = l->e[p].prev;
        x->prev = p;
        x->next = l->e[p].next;
        if (x->next != IVP_NO_INDEX) l->e[x->next].prev = i;
        else l->last = i;
        l->e[p].next = i;
    }
    l->n++;
    return i;
}

void ivp_minlist_remove(IvpMinList *l, uint32_t i)
{
    IvpMinListElem *x = &l->e[i];
    if (x->prev != IVP_NO_INDEX) l->e[x->prev].next = x->next;
    else l->first = x->next;
    if (x->next != IVP_NO_INDEX) l->e[x->next].prev = x->prev;
    else l->last = x->prev;
    l->n--;
    l->min_value = l->first != IVP_NO_INDEX ? l->e[l->first].value : 1e10f;
    x->next = l->free_head;
    l->free_head = i;
}

/* ---- the time manager (0x1002f0b0) ---- */

void ivp_time_init(PhysWorld *w)
{
    IvpTimeManager *tm = &w->coll.tm;
    ivp_minlist_init(&tm->list);
    tm->base = 0;
    tm->last_rel = 0;
    tm->psi_event.kind = IVP_EV_PSI;
    tm->psi_event.index = IVP_NO_INDEX;
    ivp_time_insert(w, &tm->psi_event, 0.0);      /* the first PSI at t = 0 */
}

/* 0x1002f1d0: the key is (f32)(t - base) */
void ivp_time_insert(PhysWorld *w, IvpTimeEvent *ev, double t)
{
    IvpTimeManager *tm = &w->coll.tm;
    ev->index = ivp_minlist_add(&tm->list, ev, (float)(t - tm->base));
}

/* 0x1002f200 (the caller resets the index) */
void ivp_time_remove(PhysWorld *w, IvpTimeEvent *ev) { ivp_minlist_remove(&w->coll.tm.list, ev->index); }

/* 0x100138f0 */
static void set_current_time(PhysWorld *w, double t)
{
    w->current_time = t;
    w->time_code++;
}

/* 0x1002f2c0: the PSI event. The rebase subtracts T - old base (the original subtracts T; identical while the
   list is empty, which it is when the PSI fires, 2.2.5). */
static void psi_event(PhysWorld *w)
{
    IvpTimeManager *tm = &w->coll.tm;
    w->time_of_last_psi = w->current_time;
    w->time_of_next_psi = w->time_of_last_psi + w->delta_psi;
    double T = w->time_of_last_psi, d = T - tm->base;
    for (uint32_t i = tm->list.first; i != IVP_NO_INDEX; i = tm->list.e[i].next) tm->list.e[i].value = (float)(tm->list.e[i].value - d);
    tm->list.min_value = tm->list.n ? (float)(tm->list.min_value - d) : 1e10f;
    tm->base = T;
    tm->last_rel = 0.0;
    ivp_simulate_psi(w);                          /* 0x10013cb0 */
    ivp_time_insert(w, &tm->psi_event, w->time_of_next_psi);
}

/* 0x1002f250 -> 0x1002ef70: every event strictly before the target, in list order */
void ivp_time_simulate_until(PhysWorld *w, double target)
{
    IvpTimeManager *tm = &w->coll.tm;
    for (;;) {
        float m = tm->list.min_value;
        if (!(target - tm->base > (double)m)) break;
        IvpTimeEvent *ev = ivp_minlist_first(&tm->list);
        ivp_minlist_remove(&tm->list, ev->index);
        ev->index = IVP_NO_INDEX;
        tm->last_rel = (double)m;
        set_current_time(w, (double)m + tm->base);
        if (ev->kind == IVP_EV_PSI) psi_event(w);
        else ivp_mindist_simulate_event(w, (IvpMindist *)((char *)ev - offsetof(IvpMindist, ev)));   /* 0x100181b0 */
    }
    set_current_time(w, target);
}

/* ---- the hull manager (2.1.6, 2.3) ---- */

void ivp_hull_init(IvpHullManager *h)
{
    memset(h, 0, sizeof *h);
    ivp_minlist_init(&h->list);
}

/* the hull value at t (the linear growth since last_time) */
float ivp_hull_value_at(const IvpHullManager *h, double t) { return (float)((t - h->last_time) * h->gradient + h->hull_value); }

void ivp_hull_insert(IvpHullManager *h, IvpHullListener *l, float key) { l->index = ivp_minlist_add(&h->list, l, key); }

void ivp_hull_remove(IvpHullManager *h, IvpHullListener *l)
{
    if (l->index == IVP_NO_INDEX) return;
    ivp_minlist_remove(&h->list, l->index);
    l->index = IVP_NO_INDEX;
}

/* 0x1001e750 */
void ivp_hull_update(IvpHullManager *h, double now, double dt, float speed, float center_speed)
{
    double d = now - h->last_time;
    h->last_time = now;
    h->hull_value = (float)(h->hull_value + d * h->gradient);
    h->center_hull_value = (float)(h->center_hull_value + d * h->center_gradient);
    h->center_gradient = center_speed;
    h->gradient = speed * 1.00001f;                /* 0x10063868 */
    h->hull_value_next_psi = (float)(h->gradient * (double)(float)dt + h->hull_value);
}

/* 0x1001a820: every ~10 s, the values rebased to 0 */
void ivp_hull_reset(IvpHullManager *h)
{
    float dh = -h->hull_value, dc = -h->center_hull_value;
    for (uint32_t i = h->list.first; i != IVP_NO_INDEX; i = h->list.e[i].next) {
        h->list.e[i].value += dh;
        IvpHullListener *l = h->list.e[i].payload;
        if (l->kind == IVP_HL_SYNAPSE) ivp_mindist_hull_reset(((IvpSynapse *)l)->md, dh, dc);   /* 0x10017cf0 */
    }
    if (h->list.n) h->list.min_value += dh;
    h->hull_value = 0;
    h->center_hull_value = 0;
    h->hull_value_next_psi += dh;
}

/* a listener's hull_limit_exceeded_event (vtable slot 1) */
static void listener_exceeded(PhysWorld *w, IvpHullListener *l, float intrusion)
{
    switch (l->kind) {
    case IVP_HL_SYNAPSE: ivp_mindist_hull_exceeded(w, ((IvpSynapse *)l)->md, intrusion); break;   /* 0x10017d10 */
    case IVP_HL_OV: ivp_recheck_ov_element(w, ((IvpOvElement *)l)->obj); break;                    /* 0x1002ddc0 */
    case IVP_HL_WATCHER: ivp_watcher_check(w, ((IvpWatcherSynapse *)l)->w); break;                 /* 0x10037e20 */
    }
}

/* 0x1001eb10: the hull managers collected after integration, last to first; each fires at most 102 events */
void ivp_hull_phase(PhysWorld *w)
{
    IvpCollWorld *cw = &w->coll;
    while (cw->ncheck) {
        IvpHullManager *h = cw->check[--cw->ncheck];
        int k = 100;
        float d;
        while ((d = h->list.min_value - h->hull_value_next_psi) < 0.0f) {
            listener_exceeded(w, ivp_minlist_first(&h->list), d);
            if (k-- < 0) break;
        }
        if ((double)h->reset_time < h->last_time) {
            ivp_hull_reset(h);
            h->reset_time = ivp_ftol(h->last_time + 10.0);
        }
    }
}
