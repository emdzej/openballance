/* The mindist event solver (docs/ivp_collision.md 5.2-6): from the closest features the minimizer found, the
   earliest time within [t_now, t_next_psi] at which the pair reaches the collision distance (codes 0xN0) or
   the features stop being valid (0xN1, 0xN2). Table 0x1007632c (0x1002d860): ball entry 0x1002d4d0 (BP
   0x1002c260, BK 0x1002d000, BF 0x1002b370), BB 0x1002d5f0 / 0x1002c670, the polygon entry 0x1002d6c0 (PP
   0x1002bcf0, PK 0x1002c820, PF 0x1002aed0, KK 0x1002b690); the search primitives 0x10037790 / 0x10037910 /
   0x10037b30 and the distance functors (vtables 0x100639a8..0x100639dc). */
#include "phys_internal.h"

/* ---- event sims (6.2, 0x1002b300): the object's pose on the 5 ms grid from t_now, filled lazily ---- */
typedef struct {
    PhysBody *obj;
    IvpCore *core;
    const IvpMatrix *slot[21];
    IvpMatrix store[21];
    IvpMatrix m0;                 /* the cache matrix at t_now */
} Sim;

static void sim_init(Sim *s, PhysWorld *w, PhysBody *obj)
{
    const IvpCache *c = ivp_cache_get(w, obj);
    s->obj = obj;
    s->core = &obj->core;
    memcpy(s->m0.r, c->r, sizeof s->m0.r);
    memcpy(s->m0.vv, c->vv, sizeof s->m0.vv);
    for (int i = 0; i < 21; i++) s->slot[i] = obj->object_state >= 8 || i == 0 ? &s->m0 : NULL;
}

static const IvpMatrix *sim_slot(Sim *s, int i, double t)
{
    if (!s->slot[i]) {
        ivp_body_m_world_f_object_at(s->obj, t, &s->store[i]);   /* 0x10009d70 */
        s->slot[i] = &s->store[i];
    }
    return s->slot[i];
}

/* ---- functors ---- */
typedef struct Functor Functor;
struct Functor {
    double (*eval)(const Functor *f, const IvpMatrix *mA, const IvpMatrix *mB);
    double speed, inv_speed;      /* +0x08, +0x10 */
    double a[3], b[3], c[3], d[3];
    double h, sign;
    float pf[3], qf[3];
};

static double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const double a[3], const double b[3], double o[3])
{
    double x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x, o[1] = y, o[2] = z;
}
static void mpoint(const IvpMatrix *m, const double p[3], double o[3])   /* 0x1000f550 */
{
    ivp_rmul(m->r, p, o);
    for (int k = 0; k < 3; k++) o[k] += m->vv[k];
}
static void fmpoint(const IvpMatrix *m, const float p[3], double o[3])   /* 0x1000f5f0 */
{
    double d[3] = {p[0], p[1], p[2]};
    mpoint(m, d, o);
}

static void set_speed(Functor *f, double speed)
{
    f->speed = speed;
    f->inv_speed = 1.0 / speed;
}

/* 0x1002a9c0 point-plane: (MA p - MB q) . (RB n); a = p (A), b = n (B), c = q (B) */
static double ev_point_plane(const Functor *f, const IvpMatrix *mA, const IvpMatrix *mB)
{
    double pa[3], qb[3], n[3];
    mpoint(mA, f->a, pa);
    mpoint(mB, f->c, qb);
    ivp_rmul(mB->r, f->b, n);
    double d[3] = {pa[0] - qb[0], pa[1] - qb[1], pa[2] - qb[2]};
    return dot3(d, n);
}

/* 0x1002ae80 / 0x1002aae0 direction-direction: (R1 d) . (R2 n); a = d, b = n */
static double ev_dir_dir(const Functor *f, const IvpMatrix *mA, const IvpMatrix *mB)
{
    double d[3], n[3];
    ivp_rmul(mA->r, f->a, d);
    ivp_rmul(mB->r, f->b, n);
    return dot3(d, n);
}

/* 0x1002b530 line-line: sign ((MA pA - MB pB) . c) isqrt_f32(|c|^2), c = (RA dA) x (RB dB);
   a = pA, b = dA, c = pB, d = dB */
static double ev_line_line(const Functor *f, const IvpMatrix *mA, const IvpMatrix *mB)
{
    double pa[3], pb[3], da[3], db[3], c[3];
    mpoint(mA, f->a, pa);
    mpoint(mB, f->c, pb);
    ivp_rmul(mA->r, f->b, da);
    ivp_rmul(mB->r, f->d, db);
    cross3(da, db, c);
    double d[3] = {pa[0] - pb[0], pa[1] - pb[1], pa[2] - pb[2]};
    return f->sign * dot3(d, c) * ivp_isqrt4((float)dot3(c, c));
}

/* 0x1002b620 parallel: |(RA dA) x (RB dB)|^2; a = dA, b = dB */
static double ev_parallel(const Functor *f, const IvpMatrix *mA, const IvpMatrix *mB)
{
    double da[3], db[3], c[3];
    ivp_rmul(mA->r, f->a, da);
    ivp_rmul(mB->r, f->b, db);
    cross3(da, db, c);
    return dot3(c, c);
}

/* 0x1002ab30 point-point: d = MB pB - MA pA, s = 1.2 (d . u) (0x100639a0); s if s|s| < |d|^2 else |d|;
   a = pA, b = pB, c = u (world) */
static double ev_point_point(const Functor *f, const IvpMatrix *mA, const IvpMatrix *mB)
{
    double pa[3], pb[3];
    mpoint(mA, f->a, pa);
    mpoint(mB, f->b, pb);
    double d[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
    double s = 1.2000000476837158 * dot3(d, f->c), d2 = dot3(d, d);
    return s * fabs(s) < d2 ? s : sqrt(d2);
}

/* 0x1002abc0 plane through a point of object 2: (M2 p2 - M1 p1) . (R2 v); a = p1, b = p2, c = v */
static double ev_plane_through(const Functor *f, const IvpMatrix *m1, const IvpMatrix *m2)
{
    double p1[3], p2[3], v[3];
    mpoint(m1, f->a, p1);
    mpoint(m2, f->b, p2);
    ivp_rmul(m2->r, f->c, v);
    double d[3] = {p2[0] - p1[0], p2[1] - p1[1], p2[2] - p1[2]};
    return dot3(d, v);
}

/* 0x1002ad90 point-line: w = (MB q - MA p) x (RB e), min(|w|, w . (RB c) + h); pf = p, qf = q, a = e, b = c */
static double ev_point_line(const Functor *f, const IvpMatrix *mA, const IvpMatrix *mB)
{
    double p[3], q[3], e[3], c[3], w[3];
    fmpoint(mA, f->pf, p);
    fmpoint(mB, f->qf, q);
    ivp_rmul(mB->r, f->a, e);
    ivp_rmul(mB->r, f->b, c);
    double d[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]};
    cross3(d, e, w);
    double len = sqrt(dot3(w, w)), x = dot3(w, c) + f->h;   /* 0x1000dd20 */
    return len < x ? len : x;
}

/* 0x1002ac40 edge turn: d = MA p - MB q, f = ((RB e) x d) x (RB e) normalized, (RA v) . f;
   a = p, b = v (A), c = q, d = e (B) */
static double ev_edge_turn(const Functor *f, const IvpMatrix *mA, const IvpMatrix *mB)
{
    double p[3], q[3], e[3], v[3], x[3], y[3];
    mpoint(mA, f->a, p);
    mpoint(mB, f->c, q);
    ivp_rmul(mB->r, f->d, e);
    ivp_rmul(mA->r, f->b, v);
    double d[3] = {p[0] - q[0], p[1] - q[1], p[2] - q[2]};
    cross3(e, d, x);
    cross3(x, e, y);
    ivp_normalize_d(y);                           /* 0x1000e120 */
    return dot3(v, y);
}

/* ---- the search primitives (5.3 / 6.4) ---- */

static double fx(const Functor *F, const IvpMatrix *a, const IvpMatrix *b) { return F->eval(F, a, b); }

static int steps_of(double step)
{
    int n = ivp_ftol(step * 200.0);               /* 0x10063b88, truncation */
    return n < 1 ? 1 : n;
}

/* 0x10037790 regula falsi (f_lo > target >= f_hi), with poses computed fresh */
static double regula_falsi(const Functor *F, double t_lo, double t_hi, double target, double f_lo, double f_hi, PhysBody *oa,
                           PhysBody *ob)
{
    for (int count = 0;;) {
        double t = t_lo + (t_hi - t_lo) * (target - f_lo) / (f_hi - f_lo);
        if ((count & 3) == 3) {
            if (count > 0x40) return t_lo;        /* the conservative exit */
            t = t + ((t_lo - t) + (t_hi - t)) * 0.375;   /* 0x10063b80: toward the midpoint */
        }
        IvpMatrix ma, mb;
        ivp_body_m_world_f_object_at(oa, t, &ma);
        ivp_body_m_world_f_object_at(ob, t, &mb);
        double f = fx(F, &ma, &mb);
        if (fabs(f - target) < 1e-8) return t;   /* 0x100633b8 */
        count++;
        if (f < target) t_hi = t, f_hi = f;
        else t_lo = t, f_lo = f;
    }
}

/* 0x10037910: the first t with f(t) <= target, by conservative advancement on the 5 ms grid */
static int search_below(const Functor *F, double target, double t, double t_max, int i, Sim *sa, Sim *sb, const double *f_start,
                        double *out)
{
    double f = f_start ? *f_start : fx(F, sa->slot[0], sb->slot[0]);
    if (f <= target) {
        *out = t;
        return 1;
    }
    for (;;) {
        double dt_safe = (f - target) * F->inv_speed, d = t - t_max;
        if (d + dt_safe > 0) return 0;            /* cannot reach the target before t_max */
        double step = 2 * dt_safe;
        if ((float)d + step > 0) step = (t_max - t) + 1e-8;   /* d rounded to f32 for this test only */
        int n = steps_of(step);
        i += n;
        if (i > 20) return 0;                     /* (unreachable within one PSI) */
        double t_new = t + n * (double)0.005f;    /* 0x10063a30 */
        double f_new = fx(F, sim_slot(sa, i, t_new), sim_slot(sb, i, t_new));
        if (f_new > target) {
            if (i == 20) return 0;
            t = t_new, f = f_new;
            continue;
        }
        double hit = regula_falsi(F, t, t_new, target, f, f_new, sa->obj, sb->obj);
        if (hit - t_max > 0) return 0;
        *out = hit;
        return 1;
    }
}

/* 0x10037b30: as search_below, with the "already within the target" branch (v2 the close-in distance) */
static int find_event(const Functor *F, double target, double v2, double t_start, double t_max, Sim *sa, Sim *sb,
                      const double *f_start, double *out)
{
    double f0 = f_start ? *f_start : fx(F, sa->slot[0], sb->slot[0]);
    if (f0 > target) return search_below(F, target, t_start, t_max, 0, sa, sb, &f0, out);
    double inv = 1.0 / F->speed;
    if (!(t_start - t_max < 0)) return 0;
    double t = t_start, f = f0;
    int i = 0;
    for (;;) {
        double step = (f - v2) * inv;
        if ((t - t_max) + step > 0) step = (t_max - t) + 1e-8;
        else if (step < 0) step = 0;
        int n = steps_of(step);
        i += n;
        if (i > 20) return 0;
        double t_new = t + n * (double)0.005f;
        double f_new = fx(F, sim_slot(sa, i, t_new), sim_slot(sb, i, t_new));
        if (f_new > target) {                     /* out of the hit zone: look for re-entry */
            if (t_new - t_max <= 0) return search_below(F, target, t_new, t_max, i, sa, sb, &f_new, out);
            return 0;
        }
        if (f_new <= f0) {                        /* not separating against the start (quirk: f0, not f) */
            *out = t;
            return 1;
        }
        t = t_new, f = f_new;
        if (!(t_new - t_max < 0)) return 0;
    }
}

/* ---- case plumbing ---- */
typedef struct {
    const IvpPolyPoint *points;
    const IvpCompactLedge *ledge;
    PhysBody *obj;
} Ctx;

static const float *PT(const Ctx *c, const IvpEdge *e) { return c->points[*e & 0xffff].k; }
static void ctx_init(Ctx *c, const IvpSynapse *s)
{
    c->ledge = ivp_ledge_of(s->edge);
    c->points = ivp_points(c->ledge);
    c->obj = s->obj;
}
static void fd(const float a[3], double o[3]) { o[0] = a[0], o[1] = a[1], o[2] = a[2]; }
/* a - b of two f32 points, exact: the x87 subtracts the f32 operands and stores f64 (e.g. 0x1002d0fb) */
static void fsub_d(const float a[3], const float b[3], double o[3])
{
    for (int k = 0; k < 3; k++) o[k] = (double)a[k] - b[k];
}

/* 0x10021280 */
static void tri_normal(const Ctx *c, const IvpEdge *e, double n[3])
{
    double a[3], b[3];
    fsub_d(PT(c, ivp_next(e)), PT(c, e), a);
    fsub_d(PT(c, ivp_prev(e)), PT(c, e), b);
    cross3(a, b, n);
}

#define FOR_OUT_RING(g, e) for (const IvpEdge *g = ivp_opp(ivp_prev(e));; g = ivp_opp(ivp_prev(g)))

/* ---- the ball cases (5.4), ball = synapse 0 ---- */

/* 0x1002b370 BF */
static void ball_face(IvpEventSolver *S, Sim *sb, Sim *sp, const Ctx *P, const IvpEdge *e)
{
    IvpMindist *md = S->md;
    double r = md->sum_extra_radius, len = md->len_numerator;
    Functor F = {ev_point_plane, 0, 0, {0}, {0}, {0}, {0}, 0, 0, {0}, {0}};
    set_speed(&F, S->worst_approach);
    tri_normal(P, e, F.b);
    ivp_normalize4(F.b);
    fd(PT(P, e), F.c);
    double f0 = r + len;
    if (find_event(&F, r + IVP_COLL_DIST, 0.5f * r + IVP_REAL_COLL_DIST, S->t_now, S->t_max, sb, sp, &f0, &S->time)) S->type = 0x20;
}

/* 0x1002c260 BP */
static void ball_point(IvpEventSolver *S, Sim *sb, Sim *sp, const Ctx *P, const IvpEdge *e)
{
    IvpMindist *md = S->md;
    double r = md->sum_extra_radius, len = md->len_numerator, cd = IVP_COLL_DIST;
    const float *Pf = PT(P, e);
    Functor F1 = {ev_point_point, 0, 0, {0}, {0}, {0}, {0}, 0, 0, {0}, {0}};
    set_speed(&F1, S->worst_approach);
    fd(Pf, F1.b);
    for (int k = 0; k < 3; k++) F1.c[k] = (double)md->normal[k] * -1.0;
    if (find_event(&F1, r + cd, 0.5f * r + IVP_REAL_COLL_DIST, S->t_now, S->t_max, sb, sp, NULL, &S->time)) S->type = 0x10;
    /* leaving the point's region along an incident edge */
    double dmax = (S->time - S->t_now) * S->worst_total + len;
    const IvpCore *pc = sp->core, *bcore = sb->core;
    double plen = sqrt((double)Pf[0] * Pf[0] + (double)Pf[1] * Pf[1] + (double)Pf[2] * Pf[2]);   /* 0x1000e480 */
    double speed = plen * pc->abs_omega + pc->current_speed + pc->abs_omega * dmax + bcore->current_speed;
    double m2 = dmax * dmax < cd * cd ? dmax * dmax : cd * cd;
    double k = -m2 * pc->inv_object_diameter;
    Functor F2 = {ev_plane_through, 0, 0, {0}, {0}, {0}, {0}, 0, 0, {0}, {0}};
    set_speed(&F2, speed);
    fd(Pf, F2.b);
    FOR_OUT_RING(g, e) {
        double d[3];
        fsub_d(PT(P, ivp_next(g)), Pf, d);
        double l2 = dot3(d, d), is = ivp_isqrt4((float)l2);
        for (int j = 0; j < 3; j++) F2.c[j] = d[j] * is;
        double a = is * l2 * k;
        if (search_below(&F2, a, S->t_now, S->time, 0, sb, sp, NULL, &S->time)) S->type = 0x11;
        if (g == e) break;
    }
}

/* 0x1002d000 BK */
static void ball_edge(IvpEventSolver *S, Sim *sb, Sim *sp, const Ctx *P, const IvpEdge *e)
{
    IvpMindist *md = S->md;
    double r = md->sum_extra_radius, cd = IVP_COLL_DIST;
    const float *Pf = PT(P, e), *Qf = PT(P, ivp_next(e));
    Functor F1 = {ev_point_line, 0, 0, {0}, {0}, {0}, {0}, 0, 0, {0}, {0}};
    set_speed(&F1, S->worst_approach);
    F1.h = 0.5 * (r + cd);                        /* 0x100631d0 */
    memcpy(F1.qf, Pf, 12);
    fsub_d(Qf, Pf, F1.a);
    ivp_normalize4(F1.a);
    double Pw[3], ew[3], x[3];
    fmpoint(&sp->m0, Pf, Pw);
    ivp_rmul(sp->m0.r, F1.a, ew);
    double d[3] = {Pw[0] - sb->m0.vv[0], Pw[1] - sb->m0.vv[1], Pw[2] - sb->m0.vv[2]};
    cross3(d, ew, x);
    ivp_rtmul(sp->m0.r, x, F1.b);
    ivp_normalize_d(F1.b);
    if (find_event(&F1, r + cd, 0.9f * r + IVP_REAL_COLL_DIST, S->t_now, S->t_max, sb, sp, NULL, &S->time)) S->type = 0x30;
    /* entering either adjacent face */
    Functor F2 = {ev_point_plane, 0, 0, {0}, {0}, {0}, {0}, 0, 0, {0}, {0}};
    set_speed(&F2, S->worst_total);
    const IvpEdge *ee[2] = {e, ivp_opp(e)};
    for (int i = 0; i < 2; i++) {
        double qp[3], rp[3], nt[3];
        fsub_d(PT(P, ivp_next(ee[i])), PT(P, ee[i]), qp);
        fsub_d(PT(P, ivp_prev(ee[i])), PT(P, ee[i]), rp);
        cross3(qp, rp, nt);
        cross3(qp, nt, F2.b);
        ivp_normalize_d(F2.b);
        fd(PT(P, ee[i]), F2.c);
        if (search_below(&F2, -IVP_MIN_FRICTION_DIST, S->t_now, S->time, 0, sb, sp, NULL, &S->time)) S->type = 0x31;
    }
}

/* 0x1002d4d0 */
static void ball_poly(IvpEventSolver *S)
{
    IvpMindist *md = S->md;
    Sim sb, sp;
    sim_init(&sb, S->env, md->syn[0].obj);
    sim_init(&sp, S->env, md->syn[1].obj);
    Ctx P;
    ctx_init(&P, &md->syn[1]);
    S->type = 0;
    S->time = S->t_max;
    switch (md->syn[1].status) {
    case IVP_ST_POINT: ball_point(S, &sb, &sp, &P, md->syn[1].edge); break;
    case IVP_ST_EDGE: ball_edge(S, &sb, &sp, &P, md->syn[1].edge); break;
    case IVP_ST_FACE: ball_face(S, &sb, &sp, &P, md->syn[1].edge); break;
    default: assert(0);
    }
}

/* 0x1002d5f0 -> 0x1002c670 BB */
static void ball_ball(IvpEventSolver *S)
{
    IvpMindist *md = S->md;
    Sim sa, sb;
    sim_init(&sa, S->env, md->syn[0].obj);
    sim_init(&sb, S->env, md->syn[1].obj);
    S->type = 0;
    S->time = S->t_max;
    double r = md->sum_extra_radius;
    Functor F = {ev_point_point, 0, 0, {0}, {0}, {0}, {0}, 0, 0, {0}, {0}};
    set_speed(&F, S->worst_approach);
    for (int k = 0; k < 3; k++) F.c[k] = sb.m0.vv[k] - sa.m0.vv[k];
    ivp_normalize_d(F.c);
    if (find_event(&F, r + IVP_COLL_DIST, 0.5f * r + IVP_REAL_COLL_DIST, S->t_now, S->t_max, &sa, &sb, NULL, &S->time)) S->type = 0x10;
}

/* ---- the convex cases (6.6-6.10) ---- */
typedef struct {
    Sim *sa, *sb;
    const Ctx *A, *B;
    const IvpCore *ca, *cb;
    double rotsum, cd, rcd, eps, xr, len;
} Conv;

/* 0x1002aed0 PF */
static void conv_pf(IvpEventSolver *S, const Conv *c, const IvpEdge *eA, const IvpEdge *eB)
{
    const float *P = PT(c->A, eA);
    Functor F = {ev_point_plane, 0, 0, {0}, {0}, {0}, {0}, 0, 0, {0}, {0}};
    set_speed(&F, S->worst_approach);
    fd(P, F.a);
    tri_normal(c->B, eB, F.b);
    ivp_normalize4(F.b);
    fd(PT(c->B, eB), F.c);
    double f_now = c->xr + c->len;
    if (find_event(&F, c->xr + c->cd, 0.5 * c->xr + c->rcd, S->t_now, S->time, c->sa, c->sb, &f_now, &S->time)) S->type = 0x20;
    /* an edge leaving P dips below the face plane */
    Functor G = {ev_dir_dir, 0, 0, {0}, {0}, {0}, {0}, 0, 0, {0}, {0}};
    set_speed(&G, c->rotsum + 1e-19);
    memcpy(G.b, F.b, sizeof G.b);
    double mn = c->len < c->cd ? c->len : c->cd;
    double lim = (-(c->eps * c->cb->inv_object_diameter)) * (c->xr * 0.1f + mn) / c->cd;
    double nw[3], nA[3];
    ivp_rmul(c->sb->m0.r, F.b, nw);
    ivp_rtmul(c->sa->m0.r, nw, nA);
    double maxdev = (S->time - S->t_now) * (c->rotsum + 1e-19);
    FOR_OUT_RING(g, eA) {
        double v[3];
        fsub_d(PT(c->A, ivp_next(g)), P, v);
        double k = ivp_isqrt4((float)dot3(v, v));
        double c0 = dot3(nA, v) * k;
        if (c0 < maxdev) {
            for (int j = 0; j < 3; j++) G.a[j] = v[j] * k;
            if (search_below(&G, lim, S->t_now, S->time, 0, c->sa, c->sb, &c0, &S->time)) S->type = 0x21;
        }
        if (g == eA) break;
    }
}

/* 0x1002bcf0 PP */
static void conv_pp(IvpEventSolver *S, const Conv *c, const IvpEdge *eA, const IvpEdge *eB)
{
    IvpMindist *md = S->md;
    const float *PA = PT(c->A, eA), *PB = PT(c->B, eB);
    Functor Pf = {ev_point_point, 0, 0, {0}, {0}, {0}, {0}, 0, 0, {0}, {0}};
    set_speed(&Pf, S->worst_approach);
    fd(PA, Pf.a);
    fd(PB, Pf.b);
    for (int k = 0; k < 3; k++) Pf.c[k] = -(double)md->normal[k];
    if (find_event(&Pf, c->cd + c->xr, 0.5 * c->xr + c->rcd, S->t_now, S->t_max, c->sa, c->sb, NULL, &S->time)) S->type = 0x10;
    double maxd = (S->time - S->t_now) * S->worst_total + c->len;
    double lA = sqrt((double)PA[0] * PA[0] + (double)PA[1] * PA[1] + (double)PA[2] * PA[2]);
    double lB = sqrt((double)PB[0] * PB[0] + (double)PB[1] * PB[1] + (double)PB[2] * PB[2]);
    double sA = lA * c->ca->abs_omega + c->ca->current_speed, sB = lB * c->cb->abs_omega + c->cb->current_speed;
    double spdB = c->cb->abs_omega * maxd + sB + sA, spdA = c->ca->abs_omega * maxd + sB + sA;
    double R = c->ca->upper_limit_radius > c->cb->upper_limit_radius ? c->ca->upper_limit_radius : c->cb->upper_limit_radius;
    double m2 = maxd * maxd < c->cd * c->cd ? maxd * maxd : c->cd * c->cd;
    double k = -0.5 * m2 / R;
    struct {
        const IvpEdge *e0;
        const Ctx *ctx;
        const float *P0, *Po;
        Sim *se, *so;
        double spd;
    } sides[2] = {{eB, c->B, PB, PA, c->sb, c->sa, spdB}, {eA, c->A, PA, PB, c->sa, c->sb, spdA}};
    for (int s = 0; s < 2; s++) {
        Functor Q = {ev_plane_through, 0, 0, {0}, {0}, {0}, {0}, 0, 0, {0}, {0}};
        set_speed(&Q, sides[s].spd);
        fd(sides[s].Po, Q.a);
        fd(sides[s].P0, Q.b);
        const IvpEdge *e0 = sides[s].e0;
        FOR_OUT_RING(g, e0) {
            double v[3];
            fsub_d(PT(sides[s].ctx, ivp_next(g)), sides[s].P0, v);
            double l2 = dot3(v, v), ki = ivp_isqrt4((float)l2);
            for (int j = 0; j < 3; j++) Q.c[j] = v[j] * ki;
            double target = (l2 * ki) * k;
            if (search_below(&Q, target, S->t_now, S->time, 0, sides[s].so, sides[s].se, NULL, &S->time)) S->type = 0x11;
            if (g == e0) break;
        }
    }
}

/* 0x1002c820 PK */
static void conv_pk(IvpEventSolver *S, const Conv *c, const IvpEdge *eA, const IvpEdge *eB)
{
    const float *P = PT(c->A, eA), *E0 = PT(c->B, eB), *E1 = PT(c->B, ivp_next(eB));
    double e[3];
    fsub_d(E1, E0, e);
    ivp_normalize_d(e);
    double wP[3], wE0[3], we[3], x[3];
    fmpoint(&c->sa->m0, P, wP);
    fmpoint(&c->sb->m0, E0, wE0);
    ivp_rmul(c->sb->m0.r, e, we);
    double d[3] = {wE0[0] - wP[0], wE0[1] - wP[1], wE0[2] - wP[2]};
    cross3(d, we, x);
    Functor R = {ev_point_line, 0, 0, {0}, {0}, {0}, {0}, 0, 0, {0}, {0}};
    set_speed(&R, S->worst_approach);
    R.h = 0.5 * (c->cd + c->xr);
    memcpy(R.pf, P, 12);
    memcpy(R.qf, E0, 12);
    memcpy(R.a, e, sizeof e);
    ivp_rtmul(c->sb->m0.r, x, R.b);
    ivp_normalize_d(R.b);
    if (find_event(&R, c->cd + c->xr, 0.9f * c->xr + c->rcd, S->t_now, S->t_max, c->sa, c->sb, NULL, &S->time)) S->type = 0x30;
    /* P enters one of the two faces at the edge */
    Functor F = {ev_point_plane, 0, 0, {0}, {0}, {0}, {0}, 0, 0, {0}, {0}};
    set_speed(&F, S->worst_total);
    fd(P, F.a);
    const IvpEdge *ce[2] = {eB, ivp_opp(eB)};
    for (int i = 0; i < 2; i++) {
        double pn[3], pp[3], nrm[3];
        fsub_d(PT(c->B, ivp_next(ce[i])), PT(c->B, ce[i]), pn);
        fsub_d(PT(c->B, ivp_prev(ce[i])), PT(c->B, ce[i]), pp);
        cross3(pn, pp, nrm);
        cross3(pn, nrm, F.b);
        ivp_normalize_d(F.b);
        fd(PT(c->B, ce[i]), F.c);
        if (search_below(&F, -c->eps, S->t_now, S->time, 0, c->sa, c->sb, NULL, &S->time)) S->type = 0x31;
    }
    /* an edge leaving P turns toward the edge */
    double dmin = c->len - (S->time - S->t_now) * S->worst_total;
    if (dmin < 1e-8) dmin = 1e-8;
    double lP = sqrt((double)P[0] * P[0] + (double)P[1] * P[1] + (double)P[2] * P[2]);
    double sA = lP * c->ca->abs_omega + c->ca->current_speed;
    double sB = (double)c->cb->max_surface_rot_speed * c->cb->abs_omega + c->cb->current_speed;   /* quirk, ported as is */
    Functor T = {ev_edge_turn, 0, 0, {0}, {0}, {0}, {0}, 0, 0, {0}, {0}};
    set_speed(&T, (sB + sA) / dmin + c->rotsum);
    fd(P, T.a);
    fd(E0, T.c);
    memcpy(T.d, e, sizeof e);
    double mn = c->len < c->cd ? c->len : c->cd;
    double target = 2 * c->cb->inv_object_diameter * mn * -0.30000001192092896;   /* 0x100639d0 */
    FOR_OUT_RING(g, eA) {
        fsub_d(PT(c->A, ivp_next(g)), P, T.b);
        ivp_normalize_d(T.b);
        if (search_below(&T, target, S->t_now, S->time, 0, c->sa, c->sb, NULL, &S->time)) S->type = 0x32;
        if (g == eA) break;
    }
}

/* 0x1002b690 KK */
static void conv_kk(IvpEventSolver *S, const Conv *c, const IvpEdge *eA, const IvpEdge *eB)
{
    IvpMindist *md = S->md;
    double dA[3], dB[3], wa[3], wb[3], cr[3];
    fsub_d(PT(c->A, ivp_next(eA)), PT(c->A, eA), dA);
    ivp_normalize4(dA);
    fsub_d(PT(c->B, ivp_next(eB)), PT(c->B, eB), dB);
    ivp_normalize4(dB);
    ivp_rmul(c->sa->m0.r, dA, wa);
    ivp_rmul(c->sb->m0.r, dB, wb);
    cross3(wa, wb, cr);
    double nc = md->normal[0] * cr[0] + md->normal[1] * cr[1] + md->normal[2] * cr[2];
    int flag;
    double sign;
    if (-nc > 0) flag = 0, sign = -1.0;
    else flag = 1, sign = 1.0;
    Functor H = {ev_line_line, 0, 0, {0}, {0}, {0}, {0}, 0, 0, {0}, {0}};
    set_speed(&H, S->worst_approach);
    fd(PT(c->A, eA), H.a);
    memcpy(H.b, dA, sizeof dA);
    fd(PT(c->B, eB), H.c);
    memcpy(H.d, dB, sizeof dB);
    H.sign = sign;
    if (find_event(&H, c->cd, c->rcd, S->t_now, S->time, c->sa, c->sb, NULL, &S->time)) S->type = 0x40;
    Functor J = {ev_parallel, 0, 0, {0}, {0}, {0}, {0}, 0, 0, {0}, {0}};
    set_speed(&J, 2 * c->rotsum + 1e-19);
    memcpy(J.a, dA, sizeof dA);
    memcpy(J.b, dB, sizeof dB);
    if (search_below(&J, 1e-19, S->t_now, S->time, 0, c->sa, c->sb, NULL, &S->time)) S->type = 0x41;
    Functor K = {ev_dir_dir, 0, 0, {0}, {0}, {0}, {0}, 0, 0, {0}, {0}};
    set_speed(&K, c->rotsum + 1e-19);
    struct {
        const IvpEdge *face;
        const double *dir;
        double sgn;
        Sim *sd, *sf;
        const Ctx *ctx;
    } cases[4] = {
        {ivp_opp(eA), dB, flag ? 1.0 : -1.0, c->sb, c->sa, c->A},
        {eA, dB, flag ? -1.0 : 1.0, c->sb, c->sa, c->A},
        {ivp_opp(eB), dA, flag ? 1.0 : -1.0, c->sa, c->sb, c->B},
        {eB, dA, flag ? -1.0 : 1.0, c->sa, c->sb, c->B},
    };
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 3; j++) K.a[j] = cases[i].dir[j] * cases[i].sgn;   /* 0x10063230 */
        tri_normal(cases[i].ctx, cases[i].face, K.b);
        ivp_normalize4(K.b);
        double target = -(c->eps * cases[i].sd->core->inv_object_diameter);
        if (search_below(&K, target, S->t_now, S->time, 0, cases[i].sd, cases[i].sf, NULL, &S->time)) S->type = 0x42;
    }
}

/* 0x1002d6c0 */
static void poly_poly(IvpEventSolver *S)
{
    IvpMindist *md = S->md;
    IvpSynapse *A = ivp_md_sorted(md, 0), *B = ivp_md_sorted(md, 1);
    Ctx ca, cb;
    ctx_init(&ca, A);
    ctx_init(&cb, B);
    Sim sa, sb;
    sim_init(&sa, S->env, A->obj);
    sim_init(&sb, S->env, B->obj);
    S->type = 0;
    S->time = S->t_max;
    Conv c = {&sa, &sb, &ca, &cb, &A->obj->core, &B->obj->core, 0, IVP_COLL_DIST, IVP_REAL_COLL_DIST, IVP_MIN_FRICTION_DIST,
              md->sum_extra_radius, md->len_numerator};
    c.rotsum = (double)c.ca->abs_omega + c.cb->abs_omega;
    switch (A->status * 4 + B->status) {
    case 0: conv_pp(S, &c, A->edge, B->edge); break;
    case 1: conv_pk(S, &c, A->edge, B->edge); break;
    case 2: conv_pf(S, &c, A->edge, B->edge); break;
    case 5: conv_kk(S, &c, A->edge, B->edge); break;
    default: assert(0);                           /* 0x1002d850 */
    }
}

/* the table 0x1007632c */
void ivp_event_solve(IvpEventSolver *S)
{
    IvpMindist *md = S->md;
    int sa = ivp_md_sorted(md, 0)->status, sb = ivp_md_sorted(md, 1)->status;
    if (sa == IVP_ST_BALL && sb == IVP_ST_BALL) ball_ball(S);
    else if (sa == IVP_ST_BALL && sb <= IVP_ST_FACE) ball_poly(S);
    else if (sa <= sb && sb <= IVP_ST_FACE && !(sa == IVP_ST_EDGE && sb == IVP_ST_FACE)) poly_poly(S);
    else assert(0);                               /* 0x1002d850 */
}
