/* Mindists and the mindist manager (docs/ivp_collision.md 4): the object cache, mindist creation and
   deletion, the EXACT / INVALID / HULL states, the recalc drivers, the event scheduling, the mindist's time
   event, the hull handlers and the per-PSI SHORT and CRITIC passes. */
#include "phys_contact.h"
#include <limits.h>
#include <stdlib.h>

/* the global coll-dist index decay counter (0x10075edc, shared by every mindist) */
static uint32_t coll_dist_counter;

/* ---- math helpers (0.1): the inverse square roots as the original computes them ---- */

static double rsqrt_steps(double x, int steps)
{
    uint64_t bits;
    memcpy(&bits, &x, 8);
    int32_t hi = (int32_t)(bits >> 32);
    uint64_t g = (uint64_t)(uint32_t)(((0x7ff00000 - hi) >> 1) + 0x1ff00000) << 32;
    double y;
    memcpy(&y, &g, 8);
    for (int i = 0; i < steps; i++) y = ((0.5 - (0.5 * x) * y * y) + 1.0) * y;
    return y;
}

double ivp_isqrt4(float x) { return rsqrt_steps((double)x, 4); }
double ivp_isqrt5(double x) { return rsqrt_steps(x, 5); }

int ivp_normalize4(double v[3])
{
    double n = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (n < 1e-19) return 0;
    double y = rsqrt_steps(n, 4);
    v[0] *= y, v[1] *= y, v[2] *= y;
    return 1;
}

double ivp_normalize_len(double v[3])
{
    double n = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (n < 1e-19) return 0;
    double y = rsqrt_steps(n, 5);
    v[0] *= y, v[1] *= y, v[2] *= y;
    return n * y;
}

int ivp_ftol(double x)
{
    if (!(x > -2147483649.0 && x < 2147483648.0)) return INT_MIN;   /* the x87 integer indefinite */
    return (int)x;
}

/* ---- the object cache (4.2.7, 0x10018930 / 0x10018a40) ---- */

/* 0x10012470 */
void ivp_core_pos_at(const IvpCore *c, double t, double out[3])
{
    double dt = t - c->time_of_last_psi;
    for (int k = 0; k < 3; k++) out[k] = c->pos_last[k] + (double)c->delta_pos[k] * dt;
}

/* 0x1001a190: refreshed when the object is simulated and the time code moved on */
IvpCache *ivp_cache_get(PhysWorld *w, PhysBody *b)
{
    IvpCache *c = &b->coll.cache;
    if (!c->valid || (b->object_state < 8 && c->time_code < w->time_code)) {
        IvpMatrix m;
        ivp_body_m_world_f_object_at(b, w->current_time, &m);
        memcpy(c->r, m.r, sizeof c->r);
        memcpy(c->vv, m.vv, sizeof c->vv);
        ivp_core_pos_at(&b->core, w->current_time, c->core_pos);
        c->time_code = w->time_code;
        c->valid = true;
    }
    return c;
}

/* 0x10018910 */
void ivp_cache_invalidate(PhysBody *b) { b->coll.cache.valid = false; }

/* ---- the manager's lists (4.4) ---- */

static void syn_push(IvpSynapse **head, IvpSynapse *s)
{
    s->prev = NULL;
    s->next = *head;
    if (*head) (*head)->prev = s;
    *head = s;
}

static void syn_unlink(IvpSynapse **head, IvpSynapse *s)
{
    if (s->prev) s->prev->next = s->next;
    else if (*head == s) *head = s->next;
    if (s->next) s->next->prev = s->prev;
    s->next = s->prev = NULL;
}

static void md_push(IvpMindist **head, IvpMindist *m)
{
    m->prev = NULL;
    m->next = *head;
    if (*head) (*head)->prev = m;
    *head = m;
}

static void md_unlink(IvpMindist **head, IvpMindist *m)
{
    if (m->prev) m->prev->next = m->next;
    else if (*head == m) *head = m->next;
    if (m->next) m->next->prev = m->prev;
    m->next = m->prev = NULL;
}

/* 0x10016e30 (without the recalc): a mindist of a car wheel's core (core +0x10) also joins the wheel vector
   (0x10016ea5 / 0x1001700a) */
static void link_exact(PhysWorld *w, IvpMindist *md)
{
    ivp_md_set_status(md, IVP_MD_EXACT);
    md_push(&w->coll.mm.exact, md);
    for (int k = 0; k < 2; k++) syn_push(&md->syn[k].obj->coll.exact_syn, &md->syn[k]);
    if (md->syn[0].obj->core.car_wheel || md->syn[1].obj->core.car_wheel) {
        IvpMindistManager *mm = &w->coll.mm;
        if (mm->nwheel == mm->capwheel) mm->capwheel = 2 * mm->capwheel + 1, mm->wheel = realloc(mm->wheel, mm->capwheel * sizeof *mm->wheel);
        mm->wheel[mm->nwheel++] = md;
    }
}

/* 0x10018230 */
static void remove_exact(PhysWorld *w, IvpMindist *md)
{
    if (md->ev.index != IVP_NO_INDEX) {
        ivp_time_remove(w, &md->ev);
        md->ev.index = IVP_NO_INDEX;
    }
    md_unlink(&w->coll.mm.exact, md);
    for (int k = 0; k < 2; k++) syn_unlink(&md->syn[k].obj->coll.exact_syn, &md->syn[k]);
    IvpMindistManager *mm = &w->coll.mm;   /* 0x100182cb: the last element moves into the hole */
    for (uint32_t i = mm->nwheel; i-- > 0;)
        if (mm->wheel[i] == md) {
            mm->wheel[i] = mm->wheel[--mm->nwheel];
            break;
        }
}

/* 0x10016ef0 */
static void insert_invalid(PhysWorld *w, IvpMindist *md)
{
    ivp_md_set_status(md, IVP_MD_INVALID);
    md_push(&w->coll.mm.invalid, md);
    for (int k = 0; k < 2; k++) syn_push(&md->syn[k].obj->coll.invalid_syn, &md->syn[k]);
}

/* 0x10018310 */
static void remove_invalid(PhysWorld *w, IvpMindist *md)
{
    md_unlink(&w->coll.mm.invalid, md);
    for (int k = 0; k < 2; k++) syn_unlink(&md->syn[k].obj->coll.invalid_syn, &md->syn[k]);
}

/* 0x10018390 */
static void remove_hull(IvpMindist *md)
{
    for (int k = 0; k < 2; k++) ivp_hull_remove(&md->syn[k].obj->coll.hull, &md->syn[k].hl);
}

/* 0x100183c0 */
static void hull_insert(PhysWorld *w, IvpMindist *md, float sA, float sB)
{
    ivp_md_set_status(md, IVP_MD_HULL);
    float s[2] = {sA, sB}, dt[2];
    for (int k = 0; k < 2; k++) {
        IvpHullManager *h = &md->syn[k].obj->coll.hull;
        double dte = w->current_time - h->last_time;
        dt[k] = (float)dte;
        ivp_hull_insert(h, &md->syn[k].hl, (float)(dte * h->gradient + h->hull_value + s[k]));
    }
    const IvpHullManager *hA = &md->syn[0].obj->coll.hull, *hB = &md->syn[1].obj->coll.hull;
    md->sum_angular_hull_time = (double)(((hB->gradient - hB->center_gradient) * dt[1] + (hB->hull_value - hB->center_hull_value)) +
                                         ((hA->gradient - hA->center_gradient) * dt[0] + (hA->hull_value - hA->center_hull_value)));
}

/* 0x10018480: the budget len - cd split by speed (all of it to the moving side when one side is static) */
static void insert_hull(PhysWorld *w, IvpMindist *md, float dist)
{
    PhysBody *a = md->syn[0].obj, *b = md->syn[1].obj;
    if ((a->object_state & 7) == 0) hull_insert(w, md, 0, dist);
    else if ((b->object_state & 7) == 0) hull_insert(w, md, dist, 0);
    else {
        double rA = (double)a->core.max_surface_rot_speed + a->core.current_speed + 1e-10f;
        double rB = (double)b->core.max_surface_rot_speed + b->core.current_speed + 1e-10f;
        float wA = (float)(rB * 0.1f + rA);
        double wB = rA * 0.1f + rB;
        float f = (float)(dist / (wB + wA));
        hull_insert(w, md, f * wA, (float)(f * wB));
    }
}

static bool recalc_ok(const IvpMindist *md) { return (md->flags & IVP_MDF_RECALC) == 0; }

/* exact_mindist_went_invalid (vtable slot 6, 0x10016f70) */
static void went_invalid(PhysWorld *w, IvpMindist *md)
{
    remove_exact(w, md);
    insert_invalid(w, md);
}

/* 0x10016f90 */
static void insert_and_recalc_exact(PhysWorld *w, IvpMindist *md)
{
    link_exact(w, md);
    ivp_recalc_mindist(w, md);
    if (recalc_ok(md)) {
        int8_t st = (int8_t)((uint8_t)md->syn[0].obj->core.movement_state | (uint8_t)md->syn[1].obj->core.movement_state);
        ivp_update_mindist_events(w, md, st < 0x21, 0);
    } else {
        went_invalid(w, md);
    }
}

/* ---- creation and deletion (4.3) ---- */

/* 0x10016290 + 0x10016490: a ball against a polygon is always synapse 0; of two balls the larger client data
   (here the CK id) is synapse 0 */
IvpMindist *ivp_mindist_create(PhysWorld *w, IvpWatcher *owner, PhysBody *a, PhysBody *b, const IvpEdge *ea, const IvpEdge *eb)
{
    IvpMindist *md = calloc(1, sizeof *md);
    md->coll.kind = IVP_COLL_MINDIST;
    md->coll.owner = owner;
    md->coll.fvec[0] = md->coll.fvec[1] = -1;
    md->ev.kind = IVP_EV_MINDIST;
    md->ev.index = IVP_NO_INDEX;
    md->flags = 0x0fc00000u;
    for (int k = 0; k < 2; k++) {
        md->syn[k].hl.kind = IVP_HL_SYNAPSE;
        md->syn[k].hl.index = IVP_NO_INDEX;
        md->syn[k].md = md;
    }
    w->coll.md_live++, w->coll.md_created++;
    IvpSynapse *first;
    if (b->coll.type == IVP_OBJ_POLYGON) {
        md->syn[1].obj = b, md->syn[1].edge = eb, md->syn[1].status = IVP_ST_POINT;
        first = &md->syn[0];
    } else {
        IvpSynapse *ball;
        if (a->coll.type != IVP_OBJ_BALL || a->entity < b->entity) ball = &md->syn[0], first = &md->syn[1];
        else ball = &md->syn[1], first = &md->syn[0];
        ball->obj = b, ball->status = IVP_ST_BALL, ball->edge = eb;
    }
    first->obj = a;
    first->edge = ea;
    first->status = a->coll.type == IVP_OBJ_POLYGON ? IVP_ST_POINT : IVP_ST_BALL;
    md->sum_extra_radius = a->extra_radius + b->extra_radius;
    insert_and_recalc_exact(w, md);   /* neither object has a phantom hook */
    return md;
}

/* 0x100162f0 (+ the delegator's collision_is_going_to_be_deleted) */
void ivp_mindist_delete(PhysWorld *w, IvpMindist *md)
{
    w->coll.md_live--, w->coll.md_deleted++;
    switch (ivp_md_status(md)) {
    case IVP_MD_INVALID: remove_invalid(w, md); break;
    case IVP_MD_EXACT: remove_exact(w, md); break;
    case IVP_MD_HULL_RECURSIVE:
    case IVP_MD_HULL: remove_hull(md); break;
    }
    if (md->ev.index != IVP_NO_INDEX) ivp_time_remove(w, &md->ev);
    if (md->coll.owner) ivp_watcher_child_deleted(md->coll.owner, md);
    else ivp_collision_deleted(w, &md->coll);
    free(md);
}

/* get_ledges (0x10016430) */
const IvpCompactLedge *ivp_mindist_ledge(const IvpMindist *md, int k) { return ivp_ledge_of(md->syn[k].edge); }

/* ---- recalc (4.5): 0x10019950 starts the loop countdown at 20 and fixes the backside up to twice,
   0x100197f0 starts at 0 and fixes it once ---- */
static int recalc(PhysWorld *w, IvpMindist *md, int count, int tries)
{
    if (md->recalc_time_stamp == w->time_code) return 4;
    md->recalc_time_stamp = w->time_code;
    IvpMinSolver s;
    s.md = md, s.count = count, s.nhash = 0, s.w = w;
    int rc = 2;
    for (int t = 0; t < tries; t++) {
        rc = ivp_minimize_step(&s);
        if (rc == 1) {
            md->flags &= ~IVP_MDF_RECALC;
            return 1;
        }
        md->flags = (md->flags & ~0x8000u) | 0x4000u;
        if (rc == 2) break;
        assert(rc == 3);
        ivp_fix_backside(&s);
    }
    if ((md->flags & IVP_MDF_FUNCTION) != 0x1000 && (md->flags & IVP_MDF_STATUS) != 0x100000) phys_contact_inter_penetration(w, md);
    return rc;
}

int ivp_recalc_mindist(PhysWorld *w, IvpMindist *md) { return recalc(w, md, 20, 2); }
int ivp_recalc_mindist_once(PhysWorld *w, IvpMindist *md) { return recalc(w, md, 0, 1); }

/* ---- event scheduling (4.6 / 5.2, 0x10017870) ---- */
void ivp_update_mindist_events(PhysWorld *w, IvpMindist *md, int allow_hull, int mode)
{
    IvpSynapse *s0 = ivp_md_sorted(md, 0), *s1 = ivp_md_sorted(md, 1);
    const IvpCore *A = &s0->obj->core, *B = &s1->obj->core;
    IvpEventSolver S;
    S.rot_sum = (double)(A->max_surface_rot_speed + B->max_surface_rot_speed);
    S.worst_total = (double)(B->current_speed + A->current_speed) + S.rot_sum;
    if (md->ev.index != IVP_NO_INDEX) {
        ivp_time_remove(w, &md->ev);
        md->ev.index = IVP_NO_INDEX;
    }
    double cd = IVP_COLL_DIST, len = md->len_numerator;
    if (w->delta_psi * S.worst_total * 2.0999999046325684 + cd < len) {   /* 0x100637f0 */
        if (!allow_hull) return;
        remove_exact(w, md);
        insert_hull(w, md, (float)(len - cd));
        return;
    }
    const float *n = md->normal;
    S.proj_v = ((double)n[2] * B->speed[2] + (double)n[1] * B->speed[1] + (double)n[0] * B->speed[0]) -
               ((double)n[2] * A->speed[2] + (double)n[1] * A->speed[1] + (double)n[0] * A->speed[0]);
    double dA = (double)A->rotation_axis_world[0] * n[0] + (double)A->rotation_axis_world[1] * n[1] + (double)A->rotation_axis_world[2] * n[2];
    double dB = (double)B->rotation_axis_world[0] * n[0] + (double)B->rotation_axis_world[1] * n[1] + (double)B->rotation_axis_world[2] * n[2];
    S.worst_approach = sqrt(1.0010000467300415 - dA * dA) * A->max_surface_rot_speed +
                       sqrt(1.0010000467300415 - dB * dB) * B->max_surface_rot_speed + S.proj_v;   /* 0x100637e8 */
    S.t_now = w->current_time;
    S.t_max = w->time_of_next_psi;
    if (S.worst_approach < IVP_SPEED_AFTER_KEEPER && S.worst_approach < 1e-19) return;
    if ((S.t_max - S.t_now) * S.worst_approach + cd <= len) return;
    if ((md->flags & IVP_MDF_FUNCTION) == 0x1000) return;
    uint32_t idx = (md->flags & IVP_MDF_COLL_DIST) >> IVP_MDF_COLL_DIST_SHIFT;
    if (idx != 0 && coll_dist_counter++ > 2) {   /* no numeric effect: every coll_dist is 0.01 */
        idx--;
        md->flags = (md->flags & ~IVP_MDF_COLL_DIST) | (idx << IVP_MDF_COLL_DIST_SHIFT);
        coll_dist_counter = 0;
    }
    S.md = md;
    S.env = w;
    S.type = 0;
    S.time = S.t_max;
    ivp_event_solve(&S);                          /* table 0x1007632c */
    if (!S.type) return;
    double t = S.time;
    if (t - S.t_now < 9.999999974752427e-07) {    /* 0x100637e0 */
        if (mode == 0) t = S.t_now;
        else {
            double slack = len - IVP_REAL_COLL_DIST;
            if (mode == 2 || (S.type & 0xf))
                t = slack < 1e-12 ? S.t_now + w->delta_psi * 0.001f : slack / S.worst_total + S.t_now + w->delta_psi * 1e-4f;
            else
                t = slack < 1e-12 ? S.t_now + w->delta_psi * 1e-5f : slack * 0.1 / S.worst_total + S.t_now + w->delta_psi * 1e-7f;
            if (t - w->time_of_next_psi >= 0) return;
        }
    }
    ivp_time_insert(w, &md->ev, t);
    md->flags = (md->flags & ~IVP_MDF_COLL_TYPE) | ((uint32_t)S.type & 0xff);
}

/* 0x100181b0: the time manager fired the mindist's event (the time code is new, so the recalc runs at t) */
void ivp_mindist_simulate_event(PhysWorld *w, IvpMindist *md)
{
    ivp_recalc_mindist(w, md);
    if (!recalc_ok(md)) return;                   /* inter_penetration already ran */
    if (md->flags & 0xf) ivp_update_mindist_events(w, md, 0, 1);    /* a topology change: re-predict */
    else if (md->len_numerator < IVP_COLL_DIST + IVP_MIN_FRICTION_DIST) phys_contact_do_impact(w, md);   /* vtable slot 7 */
    else ivp_update_mindist_events(w, md, 0, 2);  /* a near miss */
}

/* the angular hull value of an object now: (h - ch) + (g - cg) (t - t_h) */
static double angular_hull(const IvpHullManager *h, double t)
{
    return (double)(h->hull_value - h->center_hull_value) + (t - h->last_time) * (double)(h->gradient - h->center_gradient);
}

/* 0x10017d70: a synapse of a HULL mindist fired. The halfspace re-estimate re-arms the synapses while the gap
   still exceeds 6 PSIs of worst-case approach; else the mindist goes back to EXACT. */
void ivp_mindist_hull_exceeded(PhysWorld *w, IvpMindist *md, float intrusion)
{
    assert(ivp_md_status(md) != IVP_MD_HULL_RECURSIVE);
    IvpSynapse *s0 = ivp_md_sorted(md, 0), *s1 = ivp_md_sorted(md, 1);
    PhysBody *oA = s0->obj, *oB = s1->obj;
    const IvpCore *cA = &oA->core, *cB = &oB->core;
    double now = w->current_time, pA[3], pB[3];
    ivp_core_pos_at(cA, now, pA);
    ivp_core_pos_at(cB, now, pB);
    double d[3] = {pA[0] - pB[0], pA[1] - pB[1], pA[2] - pB[2]};
    double rA = (double)cA->max_surface_rot_speed + cA->current_speed + 1e-19;
    double rB = (double)cB->max_surface_rot_speed + cB->current_speed + 1e-19;
    double sum = rA + rB;
    if ((md->flags & 0x30000u) == 0) {
        double proj = md->normal[1] * d[1] + md->normal[2] * d[2] + md->normal[0] * d[0];
        double ang = angular_hull(&oA->coll.hull, now) + angular_hull(&oB->coll.hull, now);
        double nw = md->len_numerator - (ang - md->sum_angular_hull_time) - (md->contact_dot_diff_center - proj) + intrusion;
        if (nw > w->delta_psi * sum * 6.0) {      /* 0x100637f8 */
            md->sum_angular_hull_time = ang;
            md->contact_dot_diff_center = (float)proj;
            md->len_numerator = (float)(nw - intrusion);
            double ratio = nw / sum;
            IvpHullManager *hA = &oA->coll.hull, *hB = &oB->coll.hull;
            ivp_hull_remove(hA, &s0->hl);
            ivp_hull_insert(hA, &s0->hl, (float)(ivp_hull_value_at(hA, now) + ratio * rA));
            ivp_hull_remove(hB, &s1->hl);
            ivp_hull_insert(hB, &s1->hl, (float)(ivp_hull_value_at(hB, now) + ratio * rB));
            return;
        }
    }
    remove_hull(md);
    insert_and_recalc_exact(w, md);
}

/* 0x10017d50 */
void ivp_mindist_hull_reset(IvpMindist *md, float dh, float dc) { md->sum_angular_hull_time = (double)((dh - dc) + (float)md->sum_angular_hull_time); }

/* ---- the per-PSI passes ---- */

/* 0x10017850 -> 0x100187a0 */
void ivp_short_pass(PhysWorld *w)
{
    for (IvpMindist *md = w->coll.mm.exact, *next; md; md = next) {
        next = md->next;
        ivp_recalc_mindist(w, md);
        if (!recalc_ok(md)) went_invalid(w, md);
    }
}

/* 0x10017770 */
void ivp_critic_pass(PhysWorld *w)
{
    for (IvpMindist *md = w->coll.mm.exact, *next; md; md = next) {
        next = md->next;
        ivp_update_mindist_events(w, md, 1, 1);
    }
}

/* 0x10017790: for each wheel mindist (from the end): recalc (0x100187a0); when its face synapse changed
   triangle while the gap is below max_dist_for_friction, the ball's friction state moves over (0x10018040) */
void ivp_wheel_pass(PhysWorld *w)
{
    IvpMindistManager *mm = &w->coll.mm;
    for (uint32_t i = mm->nwheel; i-- > 0;) {
        if (i >= mm->nwheel) continue;
        IvpMindist *md = mm->wheel[i];
        ivp_recalc_mindist(w, md);
        if (!recalc_ok(md)) {
            went_invalid(w, md);
            continue;
        }
        IvpSynapse *b = ivp_md_sorted(md, 0), *t = ivp_md_sorted(md, 1);
        if (b->status != IVP_ST_BALL) {
            IvpSynapse *x = b;
            b = t, t = x;
        }
        if (t->edge == md->wheel_edge || t->status != IVP_ST_FACE || b->status != IVP_ST_BALL) continue;
        float gap = md->len_numerator;
        if (!(gap < IVP_MAX_DIST_FOR_FRICTION)) continue;
        ivp_ball_transfer(w, md, b->obj, gap);
        md->wheel_edge = t->edge;
    }
}

/* 0x10009610: after an impact, the exact mindists of obj to cores not stamped by this impact */
void ivp_object_recalc_after_impact(PhysWorld *w, PhysBody *b)
{
    for (IvpSynapse *s = b->coll.exact_syn, *next; s; s = next) {
        next = s->next;
        IvpMindist *md = s->md;
        if (md->syn[0].obj->core.impact_stamp == md->syn[1].obj->core.impact_stamp) continue;
        ivp_recalc_mindist(w, md);
        if (recalc_ok(md)) ivp_update_mindist_events(w, md, 0, 2);
    }
}

/* 0x100099a0: an invalid mindist whose recalc succeeds goes back to EXACT, unscheduled */
void ivp_object_after_controllers(PhysWorld *w, PhysBody *b)
{
    for (IvpSynapse *s = b->coll.invalid_syn, *next; s; s = next) {
        next = s->next;
        IvpMindist *md = s->md;
        if (ivp_md_status(md) != IVP_MD_INVALID) continue;
        ivp_recalc_mindist_once(w, md);
        if (!recalc_ok(md)) continue;
        if (next && next->md == md) next = next->next;
        remove_invalid(w, md);
        link_exact(w, md);
    }
}

void ivp_mm_free(PhysWorld *w)
{
    while (w->coll.mm.exact) ivp_mindist_delete(w, w->coll.mm.exact);
    while (w->coll.mm.invalid) ivp_mindist_delete(w, w->coll.mm.invalid);
    free(w->coll.mm.wheel);
}
