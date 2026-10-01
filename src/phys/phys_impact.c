/* IVP's impact path (docs/ivp_contact.md 6, "IM"): the mindist's do_impact at the exact event time, the
   impact solver (friction-cone compression in 10% steps, restitution at sqrt(e), the rescue separation), the
   impact system chaining impacts through the friction system's pairs, and the post-collision event. */
#include "phys_contact.h"

/* ---- the core sync (IM12) ---- */

/* 0x1000cfa0 synchronize_with_rot_z: the core moved to the current time (the backup at +0x22c keeps rot_speed
   and q_next); rot_speed is recomputed from the PSI's rotation q_last^-1 q_next */
static void core_sync(PhysWorld *w, IvpCore *c)
{
    c->synced = true;
    c->sync_pushed = false;
    v3cpy(c->sync_rot_speed, c->rot_speed);
    c->sync_q = c->q_next;
    IvpQuat inv = {-c->q_last.x, -c->q_last.y, -c->q_last.z, c->q_last.w}, d;   /* 0x100195f0 */
    ivp_quat_mul(&d, &inv, &c->q_next);           /* 0x10019620 */
    double t = w->current_time - c->time_of_last_psi, q;
    IvpQuat r;
    ivp_quat_slerp(&r, &c->q_last, &c->q_next, t * (double)c->i_delta_time);   /* 0x10019320 */
    c->q_next = r;
    ivp_quat_to_matrix(&c->q_next, c->m_world_f_core.r);
    for (int k = 0; k < 3; k++) c->m_world_f_core.vv[k] = (double)c->delta_pos[k] * t + c->pos_last[k];
    q = (double)c->i_delta_time + (double)c->i_delta_time;
    c->rot_speed[0] = (float)(asin(d.x) * q);
    c->rot_speed[1] = (float)(asin(d.y) * q);
    c->rot_speed[2] = (float)(asin(d.z) * q);
}

/* 0x1000d140 */
static void core_restore(IvpCore *c)
{
    v3cpy(c->rot_speed, c->sync_rot_speed);
    c->q_next = c->sync_q;
    c->synced = false;
}

/* ---- the rescue speed (IM4) ---- */

/* 0x100248a0 */
static float rot_rescue(PhysWorld *w, const IvpContactPoint *cp)
{
    float s = 0.0f;
    for (int i = 0; i < 2; i++) {
        const IvpCore *c = cp->tmp->core[i];
        if (!c || cp->syn[i].type == IVP_ST_BALL) continue;
        double w2 = ivp_dotd(c->rot_speed, c->rot_speed) * IVP_C_ROT_RESCUE;
        if (w2 > 0.25) w2 = 0.25;
        s = (float)(s + (1 - cos(sqrt(w2))) * c->upper_limit_radius * w->inv_delta_psi);
    }
    return s;
}

/* 0x10024930 */
float ivp_cp_rescue_speed(PhysWorld *w, IvpContactPoint *cp)
{
    double v;
    if (cp->gap < IVP_SET_MIN_COLL_DIST) v = ((double)IVP_SET_MIN_COLL_DIST - cp->gap) * w->inv_delta_psi;
    else {
        cp->tmp->rescue = 0.0f;
        v = 0;
    }
    double s = v + rot_rescue(w, cp);
    return (float)(s + s);
}

/* ---- the impact solver (IM5, IM6) ---- */

typedef struct {
    float rescue;             /* +0x00 */
    double m0, m1;            /* +0x08 / +0x10 */
    int allow_delaying;       /* +0x18 */
    float rot[2][3];          /* +0x38 / +0x48 */
    float speed[2][3];        /* +0x58 / +0x68 */
    float dw[2][3], dv[2][3]; /* +0x78 / +0x98: the last push */
    float rel[3];             /* +0xb8 */
    float dir[3];             /* +0xc8 */
    float dir2[3];            /* +0xd8 */
    IvpCore *core[2];         /* +0x100 */
    const float *p_cs[2];     /* +0x108 */
    float e, cos_, sin_;      /* +0x110 .. +0x118 */
    const float *n;           /* +0x11c */
    float *rel_out;           /* +0x120 */
} ImpactSolver;

static bool movable(const IvpCore *c) { return !ivp_core_fixed(c); }

/* 0x10022500: rel = v1(p1) - v0(p0) from the working copies */
static void calc_rel(ImpactSolver *S)
{
    float v0[3], v1[3];
    ivp_core_point_velocity(S->core[0], S->p_cs[0], S->speed[0], S->rot[0], v0);
    ivp_core_point_velocity(S->core[1], S->p_cs[1], S->speed[1], S->rot[1], v1);
    v3sub(S->rel, v1, v0);
}

static double ndot(const float n[3], const float v[3]) { return ((double)v[2] * n[2] + (double)v[1] * n[1]) + (double)v[0] * n[0]; }

static void normize(float v[3])                   /* 0x1000e030 */
{
    double l = ivp_dotd(v, v);
    if (l < IVP_C_1E19) return;
    double y = 1.0 / sqrt(l);
    for (int k = 0; k < 3; k++) v[k] = (float)(v[k] * y);
}

/* 0x10022580: the push direction clamped to the friction cone around -n */
static void calc_push_direction(ImpactSolver *S)
{
    v3cpy(S->dir, S->rel);
    normize(S->dir);
    double c = ndot(S->n, S->dir);
    if (c > 0.0) {
        v3cpy(S->dir, S->dir2);
        normize(S->dir);
    } else if (-S->cos_ < c) {                    /* outside the cone: sliding */
        float t[3];
        for (int k = 0; k < 3; k++) t[k] = (float)(S->dir[k] - c * S->n[k]);
        normize(t);
        for (int k = 0; k < 3; k++) S->dir[k] = (float)(-S->cos_ * S->n[k] + S->sin_ * t[k]);
    }
}

/* 0x1000ca80 for a world impulse at the core's contact point */
static void test_push(const IvpCore *c, const float p_cs[3], const float imp_ws[3], float dv[3], float dw[3])
{
    float imp_cs[3];
    ivp_rtmul_f(c->m_world_f_core.r, imp_ws, imp_cs);
    ivp_calc_push_core(c, p_cs, imp_cs, imp_ws, dv, dw);
}

/* 0x10023940 */
static void push(ImpactSolver *S, double f)
{
    float imp[3];
    v3scale(imp, S->dir, (float)f);
    for (int i = 0; i < 2; i++) {
        if (!movable(S->core[i])) continue;
        float im[3];
        for (int k = 0; k < 3; k++) im[k] = i ? (float)(imp[k] * -1.0) : imp[k];
        test_push(S->core[i], S->p_cs[i], im, S->dv[i], S->dw[i]);
        v3add(S->rot[i], S->rot[i], S->dw[i]);
        v3add(S->speed[i], S->speed[i], S->dv[i]);
    }
}

/* 0x10022950 */
static void undo_push(ImpactSolver *S)
{
    for (int i = 0; i < 2; i++) {
        if (!movable(S->core[i])) continue;
        v3sub(S->rot[i], S->rot[i], S->dw[i]);
        v3sub(S->speed[i], S->speed[i], S->dv[i]);
    }
}

/* the point-velocity change of a unit impulse imp (0x1000c2b0 without the length) */
static void unit_response(const IvpCore *c, const float p_cs[3], const float imp[3], float out[3])
{
    float dv[3], dw[3];
    test_push(c, p_cs, imp, dv, dw);
    ivp_core_point_velocity(c, p_cs, dv, dw, out);
}

/* 0x10022a20: a push along -dir_in up to the minimum separating speed */
static void ensure_min_separation(ImpactSolver *S, const float dir_in[3], int clamp)
{
    float d[3];
    for (int k = 0; k < 3; k++) d[k] = (float)(dir_in[k] * -1.0);
    float v0[3], v1[3], r[3];
    ivp_core_point_velocity(S->core[0], S->p_cs[0], S->speed[0], S->rot[0], v0);
    ivp_core_point_velocity(S->core[1], S->p_cs[1], S->speed[1], S->rot[1], v1);
    v3sub(r, v1, v0);
    double s = ndot(d, r);
    if (clamp == 1 && s > 0) s = 0;
    double diff = (double)S->rescue - s;
    if (!(diff >= 0)) return;
    double inv = 0.0;
    float md[3] = {-d[0], -d[1], -d[2]}, u[3];
    if (movable(S->core[1])) {
        unit_response(S->core[1], S->p_cs[1], d, u);
        inv += ndot(d, u);
    }
    if (movable(S->core[0])) {
        unit_response(S->core[0], S->p_cs[0], md, u);
        inv += ndot(md, u);
    }
    double k = diff / inv;
    if (!(k >= 0)) return;
    for (int i = 0; i < 2; i++) {
        if (!movable(S->core[i])) continue;
        float imp[3];
        v3scale(imp, d, (float)(i ? k : -k));
        test_push(S->core[i], S->p_cs[i], imp, S->dv[i], S->dw[i]);
        v3add(S->rot[i], S->rot[i], S->dw[i]);
        v3add(S->speed[i], S->speed[i], S->dv[i]);
    }
}

/* 0x10023b90 + 0x10023c80: 2 m0 m1 / (m0 + m1) vn with the virtual masses from the lengths of the point
   velocity changes */
static double full_elastic_impulse(ImpactSolver *S)
{
    float u[3], mn[3] = {-S->n[0], -S->n[1], -S->n[2]};
    unit_response(S->core[1], S->p_cs[1], S->n, u);
    S->m1 = 1.0 / sqrt(ivp_dotd(u, u));
    unit_response(S->core[0], S->p_cs[0], mn, u);
    S->m0 = 1.0 / sqrt(ivp_dotd(u, u));
    if (!movable(S->core[0])) S->m0 = S->m1 * IVP_C_FIXED_MASS;
    if (!movable(S->core[1])) S->m1 = S->m0 * IVP_C_FIXED_MASS;
    return (1 / (S->m0 + S->m1)) * 2 * -ndot(S->n, S->rel) * S->m1 * S->m0;
}

/* 0x10023ff0: tan(theta) = mu (1 + sqrt(e)), cos by its truncated Taylor series */
static void cos_sin_for_impact(float mu, float e, float *c, float *s)
{
    double x = (sqrt((double)e) + 1.0) * mu;
    double th = atan2(x, 1.0), t2 = th * th;
    double cc = t2 * t2 * IVP_C_TAYLOR4 + (1.0f - t2 * 0.5f);
    *c = (float)cc;
    *s = (float)(cc * x);
}

/* 0x100228c0 */
static void write_direct(ImpactSolver *S, int i)
{
    v3cpy(S->core[i]->speed, S->speed[i]);
    v3cpy(S->core[i]->rot_speed, S->rot[i]);
}

/* 0x10023680: final velocities written; the heavier body's response delayed to the next PSI when the light one
   leaves it */
static void apply_result(PhysWorld *w, ImpactSolver *S, IvpCore *pushed[2])
{
    (void)w;
    for (int i = 0; i < 2; i++)
        if (movable(S->core[i])) S->core[i]->impacts_psi++;
    if (S->allow_delaying) {
        int H = S->m0 <= S->m1 ? 1 : 0, L = 1 - H;
        if (pushed[H] && movable(pushed[H])) {
            IvpCore *cH = S->core[H];
            float vH[3], vL[3], r[3];
            ivp_core_point_velocity(cH, S->p_cs[H], cH->speed, cH->rot_speed, vH);   /* 0x1000c150 */
            ivp_core_point_velocity(S->core[L], S->p_cs[L], S->speed[L], S->rot[L], vL);
            v3sub(r, vH, vL);
            double q = ndot(S->n, r);
            if (H == 1) q *= -1.0;
            if (q < S->rescue * IVP_C_DELAY_FACTOR) {
                pushed[H] = NULL;
                for (int i = 0; i < 2; i++) v3set(S->core[i]->speed_change, 0, 0, 0), v3set(S->core[i]->rot_speed_change, 0, 0, 0);
                write_direct(S, L);
                v3sub(cH->speed_change, S->speed[H], cH->speed);       /* 0x10023870 */
                v3sub(cH->rot_speed_change, S->rot[H], cH->rot_speed);
                cH->impacts_psi--;
                return;
            }
        }
    }
    for (int i = 0; i < 2; i++) v3set(S->core[i]->speed_change, 0, 0, 0), v3set(S->core[i]->rot_speed_change, 0, 0, 0);   /* 0x100238f0 */
    write_direct(S, 0);
    write_direct(S, 1);
}

/* 0x10022ed0 */
static void impact_solve(PhysWorld *w, ImpactSolver *S, IvpCore *pushed[2], int allow_delaying, int pushes, float rescue_addon)
{
    double e_eff = 1.0f - (1.0f - S->e) * (1.0f / ((float)pushes * 0.5f + 1.0f));
    S->allow_delaying = allow_delaying;
    S->rescue = (IVP_SET_MINDIST_CHANGE_FORCE_DIST + rescue_addon) * IVP_C_RESCUE_FACTOR;
    pushed[0] = S->core[0], pushed[1] = S->core[1];
    for (int i = 0; i < 2; i++) {
        v3add(S->rot[i], S->core[i]->rot_speed, S->core[i]->rot_speed_change);
        v3add(S->speed[i], S->core[i]->speed, S->core[i]->speed_change);
    }
    calc_rel(S);
    v3cpy(S->rel_out, S->rel);
    int iter = 0;
    double f = full_elastic_impulse(S) * IVP_C_01F;
    float mn[3] = {(float)(S->n[0] * -1.0), (float)(S->n[1] * -1.0), (float)(S->n[2] * -1.0)};
    if (ndot(S->n, S->rel) <= IVP_C_APPROACH) {
        calc_push_direction(S);
        v3set(S->dir2, 0, 0, 0);
        double vn0 = -ndot(S->n, S->rel);
        double target = sqrt((double)pushes) * IVP_C_PUSHES_SPEED + sqrt(e_eff) * vn0;
        double vn = vn0;
        while (vn > 0.0 && iter < 100) {          /* compression, friction-limited */
            iter++;
            push(S, f);
            calc_rel(S);
            vn = -ndot(S->n, S->rel);
            calc_push_direction(S);
        }
        target = vn + target;
        v3cpy(S->dir, mn);
        if (target > 0.0 && iter != 100) {       /* restitution */
            push(S, 1.0);
            calc_rel(S);
            double d = ndot(S->n, S->rel) + vn;
            double k = fabs(d) <= IVP_C_DIFF_EPS ? 0.0 : target / d;
            undo_push(S);
            push(S, k);
        }
        ensure_min_separation(S, S->dir, 0);
    } else {
        ensure_min_separation(S, mn, 1);
        if (pushes > 10) S->allow_delaying = 0;
    }
    for (int i = 0; i < 2; i++) {                 /* the anomaly limits (spin clipping is never set) */
        double a = w->inv_delta_psi * w->max_angular_velocity_per_psi;
        if (ivp_dotd(S->rot[i], S->rot[i]) > a * a) ivp_max_angular_velocity_exceeded(w, S->rot[i]);   /* 0x1002f720 */
        if (ivp_dotd(S->speed[i], S->speed[i]) > (double)w->max_velocity * w->max_velocity) ivp_max_velocity_exceeded(w, S->speed[i]);
    }
    apply_result(w, S, pushed);
    /* (the max-collisions anomaly, temporarily_unmovable at +0x61, needs 70000 impacts per PSI: dropped) */
}

/* 0x10024140 do_impact_long_term */
static void impact_long_term(PhysWorld *w, IvpContactInfo *t, IvpCore *pushed[2], float rescue, IvpContactPoint *cp)
{
    ImpactSolver S;
    memset(&S, 0, sizeof S);
    float nloc[3];
    if (!t->core[1]) {                            /* obj1 fixed: the movable core becomes core 1 */
        S.core[0] = &t->obj[1]->core, S.core[1] = t->core[0];
        S.p_cs[0] = t->cp_cs[1], S.p_cs[1] = t->cp_cs[0];
        for (int k = 0; k < 3; k++) nloc[k] = (float)(t->n[k] * -1.0);
        S.n = nloc;
    } else {
        S.core[0] = t->core[0] ? t->core[0] : &t->obj[0]->core, S.core[1] = t->core[1];
        S.p_cs[0] = t->cp_cs[0], S.p_cs[1] = t->cp_cs[1];
        S.n = t->n;
    }
    S.e = t->elasticity;
    S.rel_out = t->rel;
    if (!S.core[0]->car_wheel && !S.core[1]->car_wheel) cos_sin_for_impact(cp->mu, S.e, &S.cos_, &S.sin_);
    else cos_sin_for_impact(0.0f, S.e, &S.cos_, &S.sin_);
    assert(!cp->two_friction);                    /* 0x10024740 anisotropic friction (unreachable) */
    impact_solve(w, &S, pushed, 1, t->pushes, rescue);
}

/* ---- the impact system (IM8) ---- */

typedef struct {
    PhysWorld *env;
    int iter;
    IvpVec pushed, synced, pairs;
    IvpFrictionSystem *fs;
} ImpactSystem;

/* 0x10024700: the core's contacts are predicted again */
static void reset_processed(ImpactSystem *sys, IvpCore *c)
{
    IvpFrictionInfo *fi = ivp_core_fi(c, sys->fs);
    for (uint32_t i = 0; fi && i < fi->cps.n; i++) ((IvpContactPoint *)fi->cps.v[i])->tmp->flags &= ~0xffu;
}

/* 0x100244a0 */
static void add_pushed_core(ImpactSystem *sys, IvpCore *c, IvpFrictionPair *exclude)
{
    ivp_vec_add(&sys->pushed, c);
    c->sync_pushed = true;
    IvpFrictionSystem *fs = sys->fs;
    for (uint32_t i = fs->pairs.n; i-- > 0;) {
        if (i >= fs->pairs.n) continue;
        IvpFrictionPair *q = fs->pairs.v[i];
        if ((q->c[0] == c || q->c[1] == c) && q != exclude && ivp_vec_find(&sys->pairs, q) < 0 &&
            ivp_fs_recalc_pair(sys->env, q, fs) > 0)
            ivp_vec_add(&sys->pairs, q);
    }
}

/* 0x100249b0: the gap at the next PSI from the current velocities (without the pending changes) */
static void calc_prediction(ImpactSystem *sys, IvpContactPoint *cp)
{
    PhysWorld *w = sys->env;
    IvpContactInfo *t = cp->tmp;
    t->flags = (t->flags & ~0xffu) | 1u;
    if (cp->gap > IVP_SET_MAX_DIST_FOR_IMPACT_SYSTEM) {
        t->pred_gap = IVP_C_FAR_GAP;
        return;
    }
    t->rescue = ivp_cp_rescue_speed(w, cp);
    double vc = 0.0;
    if (t->core[0]) vc = ivp_dotd(t->core[0]->rot_speed, t->cross_cs[0]) + ivp_dotd(t->core[0]->speed, t->n);
    if (t->core[1]) vc = vc - (ivp_dotd(t->core[1]->rot_speed, t->cross_cs[1]) + ivp_dotd(t->core[1]->speed, t->n));
    t->pred_gap = (float)(cp->gap - (vc + t->rescue * 0.5f) * w->delta_psi);
}

static bool fixed_or_tu(const IvpCore *c) { return ivp_core_fixed(c) || c->temporarily_unmovable; }

/* 0x10024560: the contact that would be deepest below 0.01 at the next PSI is impacted */
static int next_impact(ImpactSystem *sys)
{
    PhysWorld *w = sys->env;
    double best = IVP_SET_MAX_COLL_DIST;
    IvpContactPoint *best_cp = NULL;
    IvpFrictionPair *best_pair = NULL;
    for (uint32_t i = sys->pairs.n; i-- > 0;) {
        IvpFrictionPair *q = sys->pairs.v[i];
        if (fixed_or_tu(q->c[0]) && fixed_or_tu(q->c[1])) continue;
        for (uint32_t k = q->cps.n; k-- > 0;) {
            IvpContactPoint *cp = q->cps.v[k];
            if ((cp->tmp->flags & 0xff) != 1) calc_prediction(sys, cp);
            if (cp->tmp->pred_gap < best) best = cp->tmp->pred_gap, best_cp = cp, best_pair = q;
        }
    }
    if (!best_cp) return 0;
    IvpContactInfo *t = best_cp->tmp;
    IvpCore *cs[2] = {t->core[1], t->core[0]};
    for (int k = 0; k < 2; k++) {
        IvpCore *c = cs[k];
        if (!c || c->synced) continue;
        ivp_vec_add(&sys->synced, c);
        if (c->movement_state < IVP_MT_NOT_SIM) core_sync(w, c);
    }
    t->pushes++;
    IvpCore *pc[2];
    impact_long_term(w, t, pc, t->rescue, best_cp);
    IvpCore *ps[2] = {pc[1], pc[0]};
    for (int k = 0; k < 2; k++) {
        IvpCore *c = ps[k];
        if (!c || !movable(c)) continue;
        if (!c->sync_pushed) add_pushed_core(sys, c, best_pair);
        else reset_processed(sys, c);
    }
    return 1;
}

/* 0x100242c0 */
static void recalc_other_contacts(ImpactSystem *sys, IvpFrictionPair *p, IvpContactPoint *skip)
{
    for (uint32_t i = p->cps.n; i-- > 0;) {
        if (i >= p->cps.n) continue;
        IvpContactPoint *cp = p->cps.v[i];
        if (cp == skip) continue;
        ivp_cp_update(sys->env, cp);
        ivp_cp_read_materials(sys->env, cp);
        if (IVP_CI_LEFT_FEATURE(cp->tmp)) ivp_fs_remove_cp(sys->env, sys->fs, cp);
    }
}

/* 0x10024aa0: the synced cores not pushed are restored; the pushed ones get their next PSI from now */
static void finish(ImpactSystem *sys)
{
    PhysWorld *w = sys->env;
    for (uint32_t i = 0; i < sys->synced.n; i++) {
        IvpCore *c = sys->synced.v[i];
        if (c->synced && !c->sync_pushed) core_restore(c);
        c->synced = false;
    }
    double dt = w->time_of_next_psi - w->current_time;
    IvpEventSim es = {dt, dt <= 1e-10 ? 9999999866.485682 : 1.0 / dt, w, NULL};
    for (uint32_t i = 0; i < sys->pushed.n; i++) {
        IvpCore *c = sys->pushed.v[i];
        if (!movable(c)) continue;
        ivp_calc_next_psi_matrix(w, c, &es);      /* 0x1001e300 */
        c->synced = false, c->sync_pushed = false;
        reset_processed(sys, c);
    }
    ivp_hull_phase(w);                            /* 0x1001eb10 */
    for (uint32_t i = 0; i < sys->pushed.n; i++) {
        IvpCore *c = sys->pushed.v[i];
        if (!movable(c)) continue;
        c->impact_stamp = w->coll.impact_counter;   /* 0x1000d930 */
        ivp_object_recalc_after_impact(w, c->body);   /* 0x10009610 */
    }
}

/* 0x10024320; returns false when the mindist was deleted (5000 rounds) */
static bool impact_system(PhysWorld *w, IvpMindist *md, IvpFrictionSystem *fs, IvpFrictionPair *pair, IvpContactPoint *cp)
{
    ImpactSystem sys;
    memset(&sys, 0, sizeof sys);
    sys.env = w, sys.fs = fs;
    IvpCore *cA = ivp_cp_core(cp, 0), *cB = ivp_cp_core(cp, 1);
    if (movable(cA)) add_pushed_core(&sys, cA, pair);
    if (movable(pair->c[0])) ivp_vec_add(&sys.synced, pair->c[0]);
    if (movable(cB)) add_pushed_core(&sys, cB, pair);
    if (movable(pair->c[1])) ivp_vec_add(&sys.synced, pair->c[1]);
    ivp_vec_add(&sys.pairs, pair);
    recalc_other_contacts(&sys, pair, cp);
    bool alive = true;
    while (next_impact(&sys) == 1)
        if (++sys.iter > IVP_C_MAX_IMPACT_ROUNDS) {
            alive = false;
            break;
        }
    finish(&sys);
    ivp_vec_free(&sys.pushed);
    ivp_vec_free(&sys.synced);
    ivp_vec_free(&sys.pairs);
    if (!alive && md) ivp_mindist_delete(w, md);  /* vtable +0x10 (1) */
    return alive;
}

/* ---- the entry (IM2, IM3) ---- */

/* 0x10023cd0 do_impact_of_two_objects */
static void impact_two_objects(PhysWorld *w, IvpMindist *md, PhysBody *obj0, PhysBody *obj1)
{
    IvpCore *core0 = &obj0->core, *core1 = &obj1->core;
    IvpSimUnit *keep = obj0->object_state >= 8 ? core0->unit : core1->unit;
    IvpFrictionSystem *fs;
    bool created;
    IvpContactPoint *cp = ivp_try_generate_friction(w, md, &fs, &created, keep, true);
    IvpContactInfo *t = cp->tmp;
    float rescue = ivp_cp_rescue_speed(w, cp);
    IvpCore *pushed[2];
    impact_long_term(w, t, pushed, rescue, cp);
    float saved[3];
    v3cpy(saved, t->rel);
    if (ivp_core_fixed(core1))
        for (int k = 0; k < 3; k++) saved[k] = (float)(saved[k] * -1.0);
    IvpFrictionPair *pair = ivp_fs_find_pair(fs, core0, core1);
    double t_prev = pair->last_impact;
    pair->last_impact = w->current_time;
    bool alive = impact_system(w, md, fs, pair, cp);
    v3cpy(t->rel, saved);
    /* the post-collision event (0x10013b80; the objects' listeners 0x1000a770): obj0's, then obj1's */
    if (w->impact_fn) {
        PhysImpactEvent ev;
        ev.obj[0] = t->obj[0], ev.obj[1] = t->obj[1];
        ev.order[0] = obj0, ev.order[1] = obj1;
        v3cpy(ev.rel, t->rel);
        ev.normal = t->n;
        memcpy(ev.pos, t->cp_ws, sizeof ev.pos);
        ev.dt_last = (float)(w->current_time - t_prev);
        w->impact_fn(w->listen_user, &ev);
    }
    (void)alive;
}

/* 0x100240a0 IVP_Mindist::do_impact (vtable slot 7). The recursive mindist's slot 7 (0x10030540) reduces to it
   for real (non-virtual) features, and recursive mindists are never built in Ballance. */
void phys_contact_do_impact(PhysWorld *w, IvpMindist *md)
{
    PhysBody *obj[2] = {md->syn[0].obj, md->syn[1].obj};
    for (int i = 0; i < 2; i++) ivp_body_revive_now(w, obj[i]);   /* 0x10009670 */
    for (int i = 0; i < 2; i++) {
        IvpCore *c = &obj[i]->core;
        if (c->movement_state < IVP_MT_NOT_SIM) core_sync(w, c);
    }
    w->coll.impact_counter++;                     /* env +0x13c */
    w->coll.impacts++;
    impact_two_objects(w, md, obj[0], obj[1]);
    for (int i = 0; i < 2; i++) obj[i]->core.synced = obj[i]->core.sync_pushed = false;   /* the transaction ends */
}
