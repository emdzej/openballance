/* The minimize solver (docs/ivp_collision.md 3.9-3.10, 3.13, 5.5): the closest-feature walk over two compact
   ledges (or a ball and a ledge) that writes the mindist's distance, normal and synapse features. Table
   0x10075ee0 (0x1001a6b0), the poly driver 0x10019ff0, the ball driver 0x1001a220, the cases PP 0x10032350,
   PK 0x10032da0 / 0x10032e60, PF 0x10030c60 / 0x10030f80, KK 0x10031440 / 0x100317a0, FF 0x10019aa0, BP
   0x10032050, BK 0x10032860 / 0x10032910, BF 0x10030d50, BB 0x1001a370, and the pierce walk 0x1001a5c0.

   Every converged case leaves n pointing from the second info's object toward the first's, len = the
   distance minus the extra radii, and dot_diff_center = (core A - core B) . n. Return codes: 1 converged,
   2 failed (a loop, more than 256 steps, coincident points), 3 backside (status 5 and solver.pos mark it). */
#include "phys_internal.h"
#include <stdlib.h>

/* the "cache ledge point" info (3.9.1): {points, ledge, cache, object, synapse} */
typedef struct {
    const IvpPolyPoint *points;
    const IvpCompactLedge *ledge;
    IvpCache *cache;
    PhysBody *obj;
    IvpSynapse *syn;
} Info;

static const float *PT(const Info *I, const IvpEdge *e) { return I->points[*e & 0xffff].k; }

/* ---- transforms (3.1.5, 1.5.3) ---- */

static void obj_to_world(const IvpCache *c, const double p[3], double o[3])   /* 0x10018d10 */
{
    double x[3];
    ivp_rmul(c->r, p, x);
    for (int k = 0; k < 3; k++) o[k] = x[k] + c->vv[k];
}

static void fobj_to_world(const IvpCache *c, const float p[3], double o[3])
{
    double d[3] = {p[0], p[1], p[2]};
    obj_to_world(c, d, o);
}

static void world_to_obj(const IvpCache *c, const double w[3], double o[3])   /* 0x10018ca0 */
{
    double d[3] = {w[0] - c->vv[0], w[1] - c->vv[1], w[2] - c->vv[2]};
    ivp_rtmul(c->r, d, o);
}

/* 0x10020710: the f32 matrix entries and an f32 row sum, then the f64 inverse */
static void pos_other_space(const IvpEdge *e, const Info *A, const Info *B, double o[3])
{
    const float *p = PT(A, e);
    double w[3];
    for (int k = 0; k < 3; k++) {
        float s = (float)A->cache->r[k][0] * p[0] + (float)A->cache->r[k][1] * p[1] + (float)A->cache->r[k][2] * p[2];
        w[k] = (double)s + A->cache->vv[k];
    }
    world_to_obj(B->cache, w, o);
}

/* 0x100208f0: the same in f64 */
static void fpos_other_space(const float p[3], const Info *A, const Info *B, double o[3])
{
    double w[3];
    fobj_to_world(A->cache, p, w);
    world_to_obj(B->cache, w, o);
}

/* 0x10020810: R_B^T R_A v */
static void dir_other_space(const double v[3], const Info *A, const Info *B, double o[3])
{
    double w[3];
    ivp_rmul(A->cache->r, v, w);
    ivp_rtmul(B->cache->r, w, o);
}

static double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const double a[3], const double b[3], double o[3])
{
    double x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x, o[1] = y, o[2] = z;
}
/* a - b of two f32 points: the x87 subtracts them exactly (fld dword / fsub dword) and stores f64 */
static void fsub_d(const float a[3], const float b[3], double o[3])
{
    for (int k = 0; k < 3; k++) o[k] = (double)a[k] - b[k];
}

/* |q - p|^2 of two f32 points, summed on the x87 and stored f32: the argument of 0x1000dae0 in the ring
   walks (e.g. 0x10032246 .. 0x1003226d) */
static float flen2(const float q[3], const float p[3])
{
    double d[3];
    fsub_d(q, p, d);
    return (float)(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

/* ---- ledge helpers (1.5.2) ---- */

/* 0x10020a10: (X - P).d and (N - X).d, d = N - P. The differences and the dot products are formed on the
   x87 from the f32 points and the f64 X (0x10020a4e .. 0x10020abc); only the two results are stored f32. */
static void kvals(const Info *I, const IvpEdge *e, const double X[3], float out[2])
{
    const float *P = PT(I, e), *N = PT(I, ivp_next(e));
    double d[3], a[3] = {X[0] - P[0], X[1] - P[1], X[2] - P[2]}, b[3] = {N[0] - X[0], N[1] - X[1], N[2] - X[2]};
    fsub_d(N, P, d);
    out[0] = (float)dot3(a, d);
    out[1] = (float)dot3(b, d);
}

/* 0x10020ad0: the unscaled barycentric weights of X's projection, out[k] for the edge e + k (the weight of
   the vertex opposite it), out[3] = det. The edge vectors, X - q1, the dot products and the weights are f64
   (qword temporaries, 0x10020b38 .. 0x10020c22); only the four outputs are stored f32 (0x10020c28 ..
   0x10020c52). In f32 the weight of a point on an edge is rounding noise of the size of det * 2^-24, which
   sends a ball centred over the shared edge of a large pancake to its backside (the pierce walk then
   crosses the zero edge, 0x1001a5c0) and fails the recalc. */
void ivp_tri_qr_vals(const IvpCompactLedge *l, const IvpEdge *e, const double X[3], float out[4])
{
    const IvpCompactTriangle *t = ivp_tri(e);
    const float *q0 = ivp_P(l, &t->e[0]), *q1 = ivp_P(l, &t->e[1]), *q2 = ivp_P(l, &t->e[2]);
    double a[3], b[3], d[3] = {X[0] - q1[0], X[1] - q1[1], X[2] - q1[2]};
    fsub_d(q0, q1, a), fsub_d(q2, q1, b);
    double aa = dot3(a, a), bb = dot3(b, b), ab = dot3(a, b), det = aa * bb - ab * ab;
    double da = dot3(d, a), db = dot3(d, b);
    double w[3];
    w[0] = da * bb - db * ab;                     /* q0 */
    w[2] = db * aa - da * ab;                     /* q2 */
    w[1] = det - w[0] - w[2];                     /* q1 */
    int u = (int)(((uintptr_t)e >> 2) & 3) - 1;   /* e's slot: edge u runs q_u -> q_u+1 */
    for (int k = 0; k < 3; k++) out[k] = (float)w[(u + k + 2) % 3];
    out[3] = (float)det;
}

static void fvals(const Info *I, const IvpEdge *e, const double X[3], float out[4]) { ivp_tri_qr_vals(I->ledge, e, X, out); }

static bool all_inside(const float w[4]) { return !signbit(w[0]) && !signbit(w[1]) && !signbit(w[2]); }

/* 0x10021280: (N - P) x (prev - P), the differences and the cross product in f64 (0x100212e7 ..
   0x10021343; 0x10021350 is the same with f32 outputs) */
static void hesse(const Info *I, const IvpEdge *e, double out[3])
{
    const float *P = PT(I, e), *N = PT(I, ivp_next(e)), *R = PT(I, ivp_prev(e));
    double a[3], b[3];
    fsub_d(R, P, a), fsub_d(N, P, b);
    cross3(b, a, out);
}

/* 0x10021420: to the infinite line */
static double line_dist2(const Info *I, const IvpEdge *e, const double X[3])
{
    const float *P = PT(I, e), *N = PT(I, ivp_next(e));
    double a[3] = {X[0] - P[0], X[1] - P[1], X[2] - P[2]}, d[3], c[3];
    fsub_d(N, P, d);
    cross3(a, d, c);
    return dot3(c, c) * (1.0 / dot3(d, d));
}

/* 0x100216f0 */
static double point_dist2(const Info *I, const IvpEdge *e, const double X[3])
{
    const float *P = PT(I, e);
    double a[3] = {X[0] - P[0], X[1] - P[1], X[2] - P[2]};
    return dot3(a, a);
}

/* 0x10021740: to the segment */
static double seg_dist2(const double X[3], const Info *I, const IvpEdge *e)
{
    float s[2];
    kvals(I, e, X, s);
    if (s[0] >= 0 && s[1] >= 0) return line_dist2(I, e, X);
    if (s[0] < 0) return point_dist2(I, e, X);
    return point_dist2(I, ivp_next(e), X);
}

/* ---- edge-edge (0x10020c60, 0x10020e60, 0x10020da0, 0x10021800): everything in L's object space ---- */
typedef struct {
    const float *L0, *L1;
    double K0[3], K1[3], Kdir[3], Ldir[3];
    const IvpEdge *K, *L;
    const Info *IK, *IL;
    double cross[3];
} KKSetup;

static void kk_setup(KKSetup *kk, const IvpEdge *K, const IvpEdge *L, const Info *IK, const Info *IL)
{
    kk->K = K, kk->L = L, kk->IK = IK, kk->IL = IL;
    kk->L0 = PT(IL, L), kk->L1 = PT(IL, ivp_next(L));
    pos_other_space(K, IK, IL, kk->K0);
    pos_other_space(ivp_next(K), IK, IL, kk->K1);
    double kd[3];
    fsub_d(PT(IK, ivp_next(K)), PT(IK, K), kd);
    dir_other_space(kd, IK, IL, kk->Kdir);
    fsub_d(kk->L1, kk->L0, kk->Ldir);
    cross3(kk->Kdir, kk->Ldir, kk->cross);
}

static void lerp3(const double a[3], const double b[3], double t, double o[3])   /* 0x1000dc40 */
{
    for (int k = 0; k < 3; k++) o[k] = a[k] * (1 - t) + b[k] * t;
}

/* r[0..1] = (s, 1-s) along K from K0, r[2..3] = (u, 1-u) along L, unscaled. 0 in the parallel case (sampled). */
static int kk_params(const KKSetup *kk, float r[4])
{
    if (dot3(kk->cross, kk->cross) <= 1e-18) {    /* 0x10063880 */
        static const float tbl[11] = {-1, .5f, 2, 0, 1, -.001f, .001f, .999f, 1.001f, -1e-6f, 1e-6f};
        double best = 1e101;
        for (int i = 0; i < 11; i++) {
            double x[3];
            lerp3(kk->K0, kk->K1, tbl[i], x);
            double d = line_dist2(kk->IL, kk->L, x);
            if (d < best) {
                best = d;
                r[0] = tbl[i], r[1] = 1.0f - tbl[i];
                kvals(kk->IL, kk->L, x, &r[2]);
            }
        }
        double A0[3], A1[3];
        fpos_other_space(kk->L0, kk->IL, kk->IK, A0);
        fpos_other_space(kk->L1, kk->IL, kk->IK, A1);
        for (int i = 0; i < 9; i++) {
            double x[3];
            lerp3(A0, A1, tbl[i], x);
            double d = line_dist2(kk->IK, kk->K, x);
            if (d < best) {
                best = d;
                r[2] = tbl[i], r[3] = 1.0f - tbl[i];
                kvals(kk->IK, kk->K, x, &r[0]);
            }
        }
        return 0;
    }
    double nK[3], nL[3], L0[3] = {kk->L0[0], kk->L0[1], kk->L0[2]}, L1[3] = {kk->L1[0], kk->L1[1], kk->L1[2]};
    cross3(kk->Kdir, kk->cross, nK);
    cross3(kk->Ldir, kk->cross, nL);
    /* the projections and their differences are f64 temporaries; only r[] is stored f32 (0x10020ecd ..
       0x10020fbe) */
    double a = dot3(L0, nK), b = dot3(L1, nK), c = dot3(kk->K0, nK);
    r[2] = (float)((a - c) * (a - b));
    r[3] = (float)((c - b) * (a - b));
    double f8 = dot3(nL, kk->K0), f9 = dot3(nL, kk->K1), f3 = dot3(L0, nL), den = f8 - f9;
    r[0] = (float)((f8 - f3) * den);
    r[1] = (float)((f3 - f9) * den);
    return 1;
}

static double kk_qlen(const KKSetup *kk)
{
    double c2 = dot3(kk->cross, kk->cross);
    if (c2 > 1e-24) {
        double L0[3] = {kk->L0[0], kk->L0[1], kk->L0[2]};
        double d = dot3(kk->K0, kk->cross) - dot3(L0, kk->cross);
        return d * d / c2;
    }
    return seg_dist2(kk->K0, kk->IL, kk->L);
}

static double qlen_kk(const IvpEdge *K, const IvpEdge *L, const Info *IK, const Info *IL)
{
    KKSetup kk;
    float r[4];
    kk_setup(&kk, K, L, IK, IL);
    kk_params(&kk, r);
    double L0k[3], L1k[3];
    if (signbit(r[2]) || signbit(r[3])) {
        pos_other_space(L, IL, IK, L0k);
        pos_other_space(ivp_next(L), IL, IK, L1k);
        if (!signbit(r[0]) && !signbit(r[1])) {
            if (r[2] <= 0) return seg_dist2(L0k, IK, K);
            return seg_dist2(L1k, IK, K);
        }
        double m = seg_dist2(L0k, IK, K), x;
        if ((x = seg_dist2(kk.K0, IL, L)) < m) m = x;
        if ((x = seg_dist2(L1k, IK, K)) < m) m = x;
        if ((x = seg_dist2(kk.K1, IL, L)) < m) m = x;
        return m;
    }
    if (r[0] <= 0) return seg_dist2(kk.K0, IL, L);
    if (r[1] > 0) return kk_qlen(&kk);
    return seg_dist2(kk.K1, IL, L);
}

/* ---- solver plumbing ---- */

/* 0x100196f0 */
static int check_loop(IvpMinSolver *s, int sa, const IvpEdge *ea, int sb, const IvpEdge *eb)
{
    intptr_t a = (intptr_t)((uintptr_t)sa | (uintptr_t)ea), b = (intptr_t)((uintptr_t)sb | (uintptr_t)eb);
    uintptr_t hi = (uintptr_t)(a > b ? a : b), lo = (uintptr_t)(a > b ? b : a);
    for (int i = s->nhash; i-- > 0;)
        if (s->hash[i][0] == hi && s->hash[i][1] == lo) return 1;
    if (s->nhash > 0xff) return 1;
    s->hash[s->nhash][0] = hi, s->hash[s->nhash][1] = lo;
    s->nhash++;
    return 0;
}

#define LOOP_CHECK(s, sa, ea, sb, eb)                                       \
    do {                                                                    \
        if (--(s)->count < 0 && check_loop((s), (sa), (ea), (sb), (eb))) return 2; \
    } while (0)

static void set(const Info *I, const IvpEdge *e, int st)
{
    I->syn->edge = e;
    I->syn->status = (int16_t)st;
}

/* make I the first sorted synapse */
static void make_first(IvpMinSolver *s, const Info *I)
{
    if (I->syn != &s->md->syn[ivp_md_sel(s->md)]) s->md->flags ^= IVP_MDF_SORT;
}

static void store_n(IvpMindist *md, const double n[3])
{
    for (int k = 0; k < 3; k++) md->normal[k] = (float)n[k];
}

static void dot_diff(IvpMindist *md, const IvpCache *A, const IvpCache *B)
{
    double d[3] = {A->core_pos[0] - B->core_pos[0], A->core_pos[1] - B->core_pos[1], A->core_pos[2] - B->core_pos[2]};
    md->contact_dot_diff_center = (float)(d[0] * md->normal[0] + d[1] * md->normal[1] + d[2] * md->normal[2]);
}

/* the ring of edges into the point P = start(e): each starts at a neighbour Q and ends at P */
#define FOR_RING(g, e) for (const IvpEdge *g = ivp_prev(ivp_opp(ivp_prev(e)));; g = ivp_prev(ivp_opp(g)))
#define RING_END(g, e) if (g == ivp_prev(e)) break

static int PP(IvpMinSolver *s, const IvpEdge *PA, const IvpEdge *PB, const Info *A, const Info *B);
static int PK(IvpMinSolver *s, const IvpEdge *P, const IvpEdge *K, const Info *A, const Info *B);
static int PK_core(IvpMinSolver *s, const IvpEdge *P, const IvpEdge *K, const Info *A, const Info *B);
static int PF(IvpMinSolver *s, const IvpEdge *P, const IvpEdge *F, const Info *A, const Info *B);
static int PF_core(IvpMinSolver *s, const IvpEdge *P, const double X[3], const IvpEdge *F, const Info *A, const Info *B);
static int KK(IvpMinSolver *s, const IvpEdge *KA, const IvpEdge *KB, const Info *A, const Info *B);

/* the edge of tri(F) (in walk order from F) nearest to X */
static const IvpEdge *nearest_edge(const Info *I, const IvpEdge *F, const double X[3])
{
    const IvpEdge *best = F, *e = F;
    double bq = 1e101;
    for (int k = 0; k < 3; k++, e = ivp_next(e)) {
        double q = seg_dist2(X, I, e);
        if (q < bq) bq = q, best = e;
    }
    return best;
}

/* PF entry 0x10030c60 */
static int PF(IvpMinSolver *s, const IvpEdge *P, const IvpEdge *F, const Info *A, const Info *B)
{
    double X[3];
    float w[4];
    pos_other_space(P, A, B, X);
    fvals(B, F, X, w);
    if (!all_inside(w)) return PK(s, P, nearest_edge(B, F, X), A, B);
    return PF_core(s, P, X, F, A, B);
}

/* PF core 0x10030f80 (X = P in B space) */
static int PF_core(IvpMinSolver *s, const IvpEdge *P, const double X[3], const IvpEdge *F, const Info *A, const Info *B)
{
    IvpMindist *md = s->md;
    LOOP_CHECK(s, 0, P, 2, F);
    double h[3], nW[3], nA[3];
    hesse(B, F, h);
    ivp_normalize4(h);                            /* 0x1000dd50 */
    ivp_rmul(B->cache->r, h, nW);
    ivp_rtmul(A->cache->r, nW, nA);
    const float *F0 = PT(B, F);
    double dist = dot3(h, X) - (h[0] * F0[0] + h[1] * F0[1] + h[2] * F0[2]);
    md->len_numerator = (float)dist;
    store_n(md, nW);
    if (md->len_numerator < 0)
        for (int k = 0; k < 3; k++) nA[k] *= -1.0;
    md->len_numerator = (float)(md->len_numerator - md->sum_extra_radius);
    dot_diff(md, A->cache, B->cache);
    const float *Pp = PT(A, P);
    double best = 0;
    const IvpEdge *bestE = NULL;
    FOR_RING(g, P) {
        const float *Q = PT(A, g);
        double qp[3];
        fsub_d(Q, Pp, qp);
        double proj = qp[0] * nA[0] + qp[1] * nA[1] + qp[2] * nA[2];
        if (proj < 0) {
            double val = ivp_isqrt4(flen2(Q, Pp)) * proj;   /* 0x100311d4 */
            if (val < best) best = val, bestE = g;
        }
        RING_END(g, P);
    }
    if (!bestE) {
        set(A, P, IVP_ST_POINT);
        set(B, F, IVP_ST_FACE);
        if ((double)md->len_numerator + md->sum_extra_radius >= 0) return 1;
        memcpy(s->pos, X, sizeof s->pos);
        set(B, F, IVP_ST_BACKSIDE);
        return 3;                                 /* P is behind F */
    }
    double XQ[3];
    float w[4];
    pos_other_space(bestE, A, B, XQ);
    fvals(B, F, XQ, w);
    if (all_inside(w)) return PF_core(s, bestE, XQ, F, A, B);
    const IvpEdge *Fk[3] = {F, ivp_next(F), ivp_next(ivp_next(F))};
    int nneg = 0, kneg = 0;
    for (int k = 0; k < 3; k++)
        if (w[k] < 0) nneg++, kneg = k;
    if (nneg == 1) return KK(s, bestE, Fk[kneg], A, B);
    const IvpEdge *pick = NULL;
    double bq = 1e101;
    for (int k = 0; k < 3; k++) {
        if (!(w[k] <= 0)) continue;
        double q = qlen_kk(bestE, Fk[k], A, B);
        if (q < bq) bq = q, pick = Fk[k];
    }
    return KK(s, bestE, pick ? pick : Fk[kneg], A, B);
}

/* PK entry 0x10032da0 */
static int PK(IvpMinSolver *s, const IvpEdge *P, const IvpEdge *K, const Info *A, const Info *B)
{
    double X[3];
    float uv[2];
    pos_other_space(P, A, B, X);
    kvals(B, K, X, uv);
    if (uv[0] < 0) return PP(s, P, K, A, B);
    if (uv[1] < 0) return PP(s, P, ivp_next(K), A, B);
    return PK_core(s, P, K, A, B);
}

/* any vector perpendicular to d (0x1000f870) */
static void any_perpendicular(const double d[3], double o[3])
{
    double a[3] = {fabs(d[0]), fabs(d[1]), fabs(d[2])};
    double ax[3] = {0, 0, 0};
    ax[a[0] <= a[1] && a[0] <= a[2] ? 0 : a[1] <= a[2] ? 1 : 2] = 1;
    cross3(d, ax, o);
}

/* PK core 0x10032e60 */
static int PK_core(IvpMinSolver *s, const IvpEdge *P, const IvpEdge *K, const Info *A, const Info *B)
{
    IvpMindist *md = s->md;
    LOOP_CHECK(s, 0, P, 1, K);
    double X[3];
    pos_other_space(P, A, B, X);
    const float *K0 = PT(B, K);
    double d[3], t1[3], t2[3], n1[3], n2[3], x[3] = {X[0] - K0[0], X[1] - K0[1], X[2] - K0[2]};
    fsub_d(PT(B, ivp_next(K)), K0, d);
    fsub_d(PT(B, ivp_prev(K)), K0, t1);
    fsub_d(PT(B, ivp_prev(ivp_opp(K))), K0, t2);
    cross3(d, t1, n1);
    cross3(t2, d, n2);
    double s1 = dot3(x, n1), s2 = dot3(x, n2);
    float w1[4], w2[4];
    fvals(B, K, X, w1);
    fvals(B, ivp_opp(K), X, w2);
    if (w1[0] > 0) {
        if (w2[0] <= 0 || s2 <= 0) return PF(s, P, K, A, B);
        return PF(s, P, ivp_opp(K), A, B);
    }
    if (w2[0] > 0) return PF(s, P, ivp_opp(K), A, B);
    if (s1 < 0 && s2 < 0) {
        set(A, P, IVP_ST_POINT);
        set(B, K, IVP_ST_BACKSIDE);
        memcpy(s->pos, X, sizeof s->pos);
        return 3;
    }
    double c[3];
    cross3(d, x, c);
    double id = 1 / dot3(d, d), q = dot3(c, c) * id;
    if (q <= 1e-19) {
        /* exact contact on the edge line: the original stores an object-space normal and walks the
           neighbours with it (3.13.R2.2); treated as converged, the normal rotated to world (9.3) */
        double p[3], nW[3];
        any_perpendicular(d, p);
        ivp_normalize_d(p);
        ivp_rmul(B->cache->r, p, nW);
        md->len_numerator = -md->sum_extra_radius;
        store_n(md, nW);
        dot_diff(md, A->cache, B->cache);
        set(A, P, IVP_ST_POINT);
        set(B, K, IVP_ST_EDGE);
        return 1;
    }
    double si = ivp_isqrt5(q), dc[3], nW[3], nA[3];
    md->len_numerator = (float)(si * q - md->sum_extra_radius);
    cross3(d, c, dc);
    ivp_rmul(B->cache->r, dc, nW);
    double n[3] = {nW[0] * (-si * id), nW[1] * (-si * id), nW[2] * (-si * id)};
    store_n(md, n);
    dot_diff(md, A->cache, B->cache);
    /* is a neighbour of P closer (nW, unscaled, points from P toward the edge)? */
    ivp_rtmul(A->cache->r, nW, nA);
    double best = q * 1e-12;
    const IvpEdge *bestE = NULL;
    const float *Pp = PT(A, P);
    FOR_RING(g, P) {
        const float *Q = PT(A, g);
        double qp[3];
        fsub_d(Q, Pp, qp);
        double proj = qp[0] * nA[0] + qp[1] * nA[1] + qp[2] * nA[2];
        if (proj > 0) {
            double val = ivp_isqrt4(flen2(Q, Pp)) * proj;
            if (val > best) best = val, bestE = g;
        }
        RING_END(g, P);
    }
    if (!bestE) {
        set(A, P, IVP_ST_POINT);
        set(B, K, IVP_ST_EDGE);
        return 1;
    }
    if (best < 1e-8) {                            /* nearly parallel: avoid flip-flopping */
        KKSetup kk;
        float r[4];
        kk_setup(&kk, K, bestE, B, A);
        kk_params(&kk, r);
        if (r[2] < 0) {
            set(A, P, IVP_ST_POINT);
            set(B, K, IVP_ST_EDGE);
            return 1;
        }
    }
    return KK(s, bestE, K, A, B);
}

/* PP 0x10032350 */
static int PP(IvpMinSolver *s, const IvpEdge *PA, const IvpEdge *PB, const Info *A, const Info *B)
{
    IvpMindist *md = s->md;
    LOOP_CHECK(s, 0, PA, 0, PB);
    double WA[3], WB[3], XA_B[3], XB_A[3];
    fobj_to_world(A->cache, PT(A, PA), WA);
    fobj_to_world(B->cache, PT(B, PB), WB);
    world_to_obj(B->cache, WA, XA_B);
    world_to_obj(A->cache, WB, XB_A);
    double d[3] = {WA[0] - WB[0], WA[1] - WB[1], WA[2] - WB[2]}, q = dot3(d, d);
    if (q <= 1e-12) return 2;
    double si = ivp_isqrt5(q);
    md->len_numerator = (float)(si * q - md->sum_extra_radius);
    double n[3] = {d[0] * si, d[1] * si, d[2] * si};
    store_n(md, n);
    dot_diff(md, A->cache, B->cache);
    double best = 0;
    const IvpEdge *bestE = NULL;
    int side = 0;
    const IvpEdge *Ps[2] = {PA, PB};
    const Info *Is[2] = {A, B};
    const double *other[2] = {XB_A, XA_B};
    for (int k = 0; k < 2; k++) {
        const float *P = PT(Is[k], Ps[k]);
        double v[3] = {other[k][0] - P[0], other[k][1] - P[1], other[k][2] - P[2]};
        double base = P[0] * v[0] + P[1] * v[1] + P[2] * v[2];
        FOR_RING(g, Ps[k]) {
            const float *Q = PT(Is[k], g);
            double proj = (Q[0] * v[0] + Q[1] * v[1] + Q[2] * v[2]) - base;
            if (proj > 0) {
                double val = ivp_isqrt4(flen2(Q, P)) * proj;   /* 0x100326a7 */
                if (val > best) best = val, bestE = g, side = k;
            }
            RING_END(g, Ps[k]);
        }
    }
    if (bestE) {
        const Info *si_ = Is[side], *oi = Is[side ^ 1];
        float uv[2];
        kvals(si_, bestE, other[side], uv);
        if (uv[1] > 0) {
            if (uv[0] >= 0) {
                make_first(s, oi);
                return PK_core(s, Ps[side ^ 1], bestE, oi, si_);
            }
            make_first(s, si_);
            return PP(s, bestE, Ps[side ^ 1], si_, oi);
        }
    }
    set(A, PA, IVP_ST_POINT);
    set(B, PB, IVP_ST_POINT);
    return 1;
}

static int KK_core(IvpMinSolver *s, const IvpEdge *KA, const IvpEdge *KB, const KKSetup *kk, const float r[4], const Info *A,
                   const Info *B);

/* KK entry 0x10031440 (the parallel case's sampled r used as if real) */
static int KK(IvpMinSolver *s, const IvpEdge *KA, const IvpEdge *KB, const Info *A, const Info *B)
{
    KKSetup kk;
    float r[4];
    kk_setup(&kk, KA, KB, A, B);
    kk_params(&kk, r);
    if (r[2] >= 0 && r[3] >= 0) {
        if (r[0] < 0) return PK(s, KA, KB, A, B);
        if (r[1] >= 0) return KK_core(s, KA, KB, &kk, r, A, B);
        return PK(s, ivp_next(KA), KB, A, B);
    }
    if (r[0] >= 0 && r[1] >= 0) {
        make_first(s, B);
        return PK(s, r[2] >= 0 ? ivp_next(KB) : KB, KA, B, A);
    }
    /* both out of range */
    const IvpEdge *pa = r[0] < r[1] ? KA : ivp_next(KA), *qa = pa == KA ? ivp_next(KA) : KA;
    const IvpEdge *pb = r[2] < r[3] ? KB : ivp_next(KB), *qb = pb == KB ? ivp_next(KB) : KB;
    double X[3];
    float uv[2], uv2[2];
    pos_other_space(pa, A, B, X);
    kvals(B, KB, X, uv);
    if (uv[0] >= 0 && uv[1] >= 0) return PK(s, pa, KB, A, B);
    pos_other_space(pb, B, A, X);
    kvals(A, KA, X, uv2);
    if (uv2[0] >= 0 && uv2[1] >= 0) {
        make_first(s, B);
        return PK(s, pb, KA, B, A);
    }
    if (uv[0] * r[2] < 0) return PP(s, pa, qb, A, B);   /* r[2], not r[3] (3.13.R2.1) */
    if (uv2[0] * r[0] < 0) {
        make_first(s, B);
        return PP(s, pb, qa, B, A);
    }
    return PP(s, pa, pb, A, B);
}

/* a face of the KK core's four (3.13.R2.8) */
typedef struct {
    const IvpEdge *tri, *other;
    const double *point;          /* the other edge's start in the triangle owner's space */
    const Info *other_info, *owner;
    const double *vec;
    double h[3];
} KKFace;

/* KK core 0x100317a0 */
static int KK_core(IvpMinSolver *s, const IvpEdge *KA, const IvpEdge *KB, const KKSetup *kk, const float r[4], const Info *A,
                   const Info *B)
{
    IvpMindist *md = s->md;
    LOOP_CHECK(s, 1, KA, 1, KB);
    const double *c = kk->cross;
    float sf = (float)((kk->L0[0] - kk->K0[0]) * c[0] + (kk->L0[1] - kk->K0[1]) * c[1] + (kk->L0[2] - kk->K0[2]) * c[2]);
    int neg = signbit(sf) ? 1 : 0;
    double inv = ivp_isqrt5(dot3(c, c)), nW[3], nA[3];
    ivp_rmul(B->cache->r, c, nW);
    ivp_rtmul(A->cache->r, nW, nA);
    md->len_numerator = (float)(fabs(sf * inv) - md->sum_extra_radius);
    double k = (neg - 0.5f) * 2 * inv;
    double n[3] = {nW[0] * k, nW[1] * k, nW[2] * k};
    store_n(md, n);
    dot_diff(md, A->cache, B->cache);
    double KB0_A[3], KB1_A[3];
    fpos_other_space(kk->L0, B, A, KB0_A);
    fpos_other_space(kk->L1, B, A, KB1_A);
    KKFace E[4] = {
        {ivp_opp(KA), KB, KB0_A, B, A, nA, {0}},
        {KA, ivp_opp(KB), KB1_A, B, A, nA, {0}},
        {ivp_opp(KB), KA, kk->K0, A, B, c, {0}},
        {KB, ivp_opp(KA), kk->K1, A, B, c, {0}},
    };
    int flag[4];
    for (int i = 0; i < 4; i++) {
        hesse(E[i].owner, E[i].tri, E[i].h);
        flag[i] = (signbit((float)dot3(E[i].h, E[i].vec)) ? 1 : 0) ^ (i >> 1) ^ neg;
    }
    double best = -4e-12;
    int pick = -1, pickj = 0;
    float pw[4] = {0};
    for (int i = 0; i < 4; i++) {
        int j = flag[i] ^ i ^ neg, kx = i ^ neg;
        double v[3] = {E[kx].point[0] - E[kx ^ 1].point[0], E[kx].point[1] - E[kx ^ 1].point[1], E[kx].point[2] - E[kx ^ 1].point[2]};
        double dot = dot3(v, E[i].h);
        if (dot < 0) {
            double hf = ivp_isqrt4((float)dot3(E[i].h, E[i].h));
            double cc = ivp_isqrt4((float)dot3(v, v)) * dot * hf;
            float w[4];
            if (cc < best) {
                fvals(E[i].owner, E[i].tri, E[j].point, w);
                if (w[0] > 0) best = cc, pick = i, pickj = j, memcpy(pw, w, sizeof w);
            }
        }
    }
    if (pick < 0) {
        set(A, KA, IVP_ST_EDGE);
        set(B, KB, IVP_ST_EDGE);
        if (flag[0] + flag[1] == 2) {
            set(A, KA, IVP_ST_BACKSIDE);
            set(B, KB, IVP_ST_FACE);
            lerp3(KB0_A, KB1_A, (double)r[2] / ((double)r[2] + r[3]), s->pos);
            return 3;
        }
        if (flag[2] + flag[3] == 2) {
            set(A, KA, IVP_ST_FACE);
            set(B, KB, IVP_ST_BACKSIDE);
            lerp3(kk->K0, kk->K1, (double)r[0] / ((double)r[0] + r[1]), s->pos);
            return 3;
        }
        return 1;
    }
    const IvpEdge *tri = E[pick].tri, *other = E[pickj].other;
    const Info *oi = E[pickj].other_info, *ow = E[pick].owner;
    make_first(s, oi);
    if (all_inside(pw)) return PF_core(s, other, E[pickj].point, tri, oi, ow);
    if (pw[2] >= 0) return KK(s, other, ivp_next(tri), oi, ow);
    if (pw[1] >= 0) return KK(s, other, ivp_prev(tri), oi, ow);
    /* the original passes a stack address as the second call's first edge (3.13.R2.4); `other` is meant */
    double q1 = qlen_kk(other, ivp_next(tri), oi, ow), q2 = qlen_kk(other, ivp_opp(ivp_prev(tri)), oi, ow);
    return KK(s, other, q2 < q1 ? ivp_prev(tri) : ivp_next(tri), oi, ow);
}

/* 0x10021210: the normalized plane of a triangle, n . X + d = signed distance */
static void hesse_plane(const Info *I, const IvpEdge *e, double n[3], double *d)
{
    hesse(I, e, n);
    ivp_normalize_d(n);
    const float *P = PT(I, e);
    *d = -(n[0] * P[0] + n[1] * P[1] + n[2] * P[2]);
}

/* FF init 0x10019aa0: the brute-force restart for face-face */
static int FF(IvpMinSolver *s, const IvpEdge *FA, const IvpEdge *FB, const Info *A, const Info *B)
{
    const IvpCompactTriangle *ta = ivp_tri(FA), *tb = ivp_tri(FB);
    double best = 1e101;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double a[3], b[3];
            fobj_to_world(A->cache, PT(A, &ta->e[i]), a);
            fobj_to_world(B->cache, PT(B, &tb->e[j]), b);
            double d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
            if (dot3(d, d) < best) {
                best = dot3(d, d);
                set(A, &ta->e[i], IVP_ST_POINT);
                set(B, &tb->e[j], IVP_ST_POINT);
            }
        }
    const Info *faces[2] = {B, A}, *pts[2] = {A, B};
    const IvpCompactTriangle *ft[2] = {tb, ta}, *pt[2] = {ta, tb};
    for (int f = 0; f < 2; f++) {
        double n[3], dd;
        hesse_plane(faces[f], &ft[f]->e[0], n, &dd);
        for (int i = 0; i < 3; i++) {
            double X[3];
            float w[4];
            pos_other_space(&pt[f]->e[i], pts[f], faces[f], X);
            fvals(faces[f], &ft[f]->e[0], X, w);
            if (!all_inside(w)) continue;
            double q = dot3(n, X) + dd;
            q *= q;
            if (q * 1.000000000001 < best) {      /* 0x10063820 */
                best = q;
                set(pts[f], &pt[f]->e[i], IVP_ST_POINT);
                set(faces[f], &ft[f]->e[0], IVP_ST_FACE);
            }
        }
    }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            KKSetup kk;
            float r[4];
            kk_setup(&kk, &ta->e[i], &tb->e[j], A, B);
            kk_params(&kk, r);
            if (signbit(r[0]) || signbit(r[1]) || signbit(r[2]) || signbit(r[3])) continue;
            double q = kk_qlen(&kk);
            if (q * 1.000000000001 < best) {
                best = q;
                set(A, &ta->e[i], IVP_ST_EDGE);
                set(B, &tb->e[j], IVP_ST_EDGE);
            }
        }
    const Info *first = A, *second = B;
    if (second->syn->status == IVP_ST_POINT && first->syn->status != IVP_ST_POINT) first = B, second = A;
    make_first(s, first);
    int sa = first->syn->status, sb = second->syn->status;
    const IvpEdge *ea = first->syn->edge, *eb = second->syn->edge;
    if (sa == IVP_ST_POINT && sb == IVP_ST_POINT) return PP(s, ea, eb, first, second);
    if (sa == IVP_ST_POINT && sb == IVP_ST_EDGE) return PK(s, ea, eb, first, second);
    if (sa == IVP_ST_POINT && sb == IVP_ST_FACE) return PF(s, ea, eb, first, second);
    if (sa == IVP_ST_EDGE && sb == IVP_ST_EDGE) return KK(s, ea, eb, first, second);
    assert(0);
    return 2;
}

/* ---- the ball cases (5.5), ball = synapse 0 ---- */

typedef struct {
    IvpCache *cache;
    PhysBody *obj;
    IvpSynapse *syn;
} BallInfo;

static int BP(IvpMinSolver *s, const BallInfo *bi, const IvpEdge *e, const Info *I);
static int BK(IvpMinSolver *s, const BallInfo *bi, const IvpEdge *e, const Info *I);

/* BF 0x10030d50 (no loop check) */
static int BF(IvpMinSolver *s, const BallInfo *bi, const IvpEdge *e, const Info *I)
{
    IvpMindist *md = s->md;
    double X[3];
    float w[4];
    world_to_obj(I->cache, bi->cache->vv, X);
    fvals(I, e, X, w);
    if (!all_inside(w)) return BK(s, bi, nearest_edge(I, e, X), I);
    double n[3], nW[3];
    hesse(I, e, n);
    ivp_normalize_d(n);                           /* 0x1000e120 */
    ivp_rmul(I->cache->r, n, nW);
    store_n(md, nW);
    set(I, e, IVP_ST_FACE);
    const float *P = PT(I, e);
    double h = dot3(n, X) - (n[0] * P[0] + n[1] * P[1] + n[2] * P[2]);
    if (h >= 0) {
        md->len_numerator = (float)(h - md->sum_extra_radius);
        dot_diff(md, bi->cache, I->cache);
        return 1;
    }
    set(I, e, IVP_ST_BACKSIDE);
    memcpy(s->pos, X, sizeof s->pos);
    return 3;                                     /* the centre behind the face */
}

/* BK core 0x10032910 */
static int BK_core(IvpMinSolver *s, const BallInfo *bi, const IvpEdge *e, const Info *I)
{
    IvpMindist *md = s->md;
    LOOP_CHECK(s, 3, NULL, 1, e);
    double X[3];
    world_to_obj(I->cache, bi->cache->vv, X);
    const float *P = PT(I, e);
    double d[3], r[3], sv[3], n1[3], n2[3], x[3] = {X[0] - P[0], X[1] - P[1], X[2] - P[2]};
    fsub_d(PT(I, ivp_next(e)), P, d);
    fsub_d(PT(I, ivp_prev(e)), P, r);
    fsub_d(PT(I, ivp_prev(ivp_opp(e))), P, sv);
    cross3(d, r, n1);
    cross3(sv, d, n2);
    double h1 = dot3(x, n1), h2 = dot3(x, n2);
    float w1[4], w2[4];
    fvals(I, e, X, w1);
    fvals(I, ivp_opp(e), X, w2);
    if (w1[0] <= 0) {
        if (w2[0] <= 0) {                         /* in the edge's region */
            if (h1 < -1e-19 && h2 < -1e-19) {     /* 0x10063af0: behind both faces */
                memcpy(s->pos, X, sizeof s->pos);
                set(I, e, IVP_ST_BACKSIDE);
                return 3;
            }
            double c[3], dc[3], wv[3];
            cross3(d, x, c);
            double id2 = 1 / dot3(d, d), q = dot3(c, c) * id2, is = ivp_isqrt5(q);
            md->len_numerator = (float)(is * q - md->sum_extra_radius);
            cross3(d, c, dc);
            ivp_rmul(I->cache->r, dc, wv);
            double n[3] = {-(is * id2) * wv[0], -(is * id2) * wv[1], -(is * id2) * wv[2]};
            store_n(md, n);
            dot_diff(md, bi->cache, I->cache);
            set(I, e, IVP_ST_EDGE);
            return 1;
        }
        return BF(s, bi, ivp_opp(e), I);
    }
    if (w2[0] <= 0 || h2 <= 0.0) return BF(s, bi, e, I);
    return BF(s, bi, ivp_opp(e), I);
}

/* BK 0x10032860 */
static int BK(IvpMinSolver *s, const BallInfo *bi, const IvpEdge *e, const Info *I)
{
    double X[3];
    float ab[2];
    world_to_obj(I->cache, bi->cache->vv, X);
    kvals(I, e, X, ab);
    if (ab[0] < 0) return BP(s, bi, e, I);
    if (ab[1] < 0) return BP(s, bi, ivp_next(e), I);
    return BK_core(s, bi, e, I);
}

/* BP 0x10032050 */
static int BP(IvpMinSolver *s, const BallInfo *bi, const IvpEdge *e, const Info *I)
{
    IvpMindist *md = s->md;
    LOOP_CHECK(s, 3, NULL, 0, e);
    double Pw[3], X[3];
    fobj_to_world(I->cache, PT(I, e), Pw);        /* 0x100217d0 */
    world_to_obj(I->cache, bi->cache->vv, X);
    double v[3] = {bi->cache->vv[0] - Pw[0], bi->cache->vv[1] - Pw[1], bi->cache->vv[2] - Pw[2]};
    double L = ivp_normalize_len(v);              /* 0x1000de30 */
    store_n(md, v);
    md->len_numerator = (float)(L - md->sum_extra_radius);
    dot_diff(md, bi->cache, I->cache);
    /* X - P, P.(X - P) and N.(X - P) - P.(X - P) in f64 (0x100321a1 .. 0x10032235) */
    const float *P = PT(I, e);
    double xd[3] = {X[0] - P[0], X[1] - P[1], X[2] - P[2]};
    double c0 = P[0] * xd[0] + P[1] * xd[1] + P[2] * xd[2];
    const IvpEdge *best = NULL;
    double bestv = 0;
    FOR_RING(g, e) {
        const float *N = PT(I, g);
        double f = (N[0] * xd[0] + N[1] * xd[1] + N[2] * xd[2]) - c0;
        if (f > 0) {
            double wv = ivp_isqrt4(flen2(N, P)) * f;   /* 0x10032270 */
            if (wv > bestv) bestv = wv, best = g;
        }
        RING_END(g, e);
    }
    if (best) {
        float ab[2];
        kvals(I, best, X, ab);
        if (ab[1] > 0) return ab[0] >= 0 ? BK(s, bi, best, I) : BP(s, bi, best, I);
    }
    set(I, e, IVP_ST_POINT);
    return 1;
}

/* ---- drivers ---- */

static void make_info(PhysWorld *w, IvpSynapse *syn, Info *I)
{
    I->ledge = ivp_ledge_of(syn->edge);
    I->points = ivp_points(I->ledge);
    I->cache = ivp_cache_get(w, syn->obj);
    I->obj = syn->obj;
    I->syn = syn;
}

/* 0x1001a370 */
static int BB(IvpMinSolver *s)
{
    IvpMindist *md = s->md;
    IvpCache *ca = ivp_cache_get(s->w, md->syn[0].obj), *cb = ivp_cache_get(s->w, md->syn[1].obj);
    float v[3] = {(float)(ca->vv[0] - cb->vv[0]), (float)(ca->vv[1] - cb->vv[1]), (float)(ca->vv[2] - cb->vv[2])};
    double q = (double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2];
    double inv = fabs(q) <= (double)1e-19f ? 1.0 : ivp_isqrt5(q);
    md->len_numerator = (float)(inv * q - md->sum_extra_radius);
    for (int k = 0; k < 3; k++) md->normal[k] = (float)(v[k] * inv);
    dot_diff(md, ca, cb);
    return 1;
}

/* the table 0x10075ee0 (0x1001a6b0), index status(syn[sel]) * 4 + status(syn[sel ^ 1]) */
int ivp_minimize_step(IvpMinSolver *s)
{
    IvpMindist *md = s->md;
    int sa = ivp_md_sorted(md, 0)->status, sb = ivp_md_sorted(md, 1)->status;
    assert(sa <= 3 && sb <= 3);
    if (sa == IVP_ST_BALL && sb == IVP_ST_BALL) return BB(s);
    if (sb == IVP_ST_BALL) {
        /* 0x1001a1f0 (PB, KB, FB): an assert in the original, then the flip */
        assert(0);
        md->flags ^= IVP_MDF_SORT;
        sa = IVP_ST_BALL;
    }
    if (sa == IVP_ST_BALL) {
        /* 0x1001a220: the ball is synapse 0 */
        BallInfo bi = {ivp_cache_get(s->w, md->syn[0].obj), md->syn[0].obj, &md->syn[0]};
        Info I;
        make_info(s->w, &md->syn[1], &I);
        switch (md->syn[1].status) {
        case IVP_ST_POINT: return BP(s, &bi, md->syn[1].edge, &I);
        case IVP_ST_EDGE: return BK(s, &bi, md->syn[1].edge, &I);
        case IVP_ST_FACE: return BF(s, &bi, md->syn[1].edge, &I);
        }
        assert(0);
        return 2;
    }
    if ((sa == IVP_ST_EDGE && sb == IVP_ST_FACE) || (sa == IVP_ST_FACE && sb == IVP_ST_EDGE)) {
        assert(0);                                /* 0x1001a6a0 */
        return 5;
    }
    if (sa > sb) md->flags ^= IVP_MDF_SORT;       /* 0x1001a350 (KP, FP) */
    /* 0x10019ff0 */
    IvpSynapse *A = ivp_md_sorted(md, 0), *B = ivp_md_sorted(md, 1);
    Info IA, IB;
    make_info(s->w, A, &IA);
    make_info(s->w, B, &IB);
    switch (A->status * 4 + B->status) {
    case 0: return PP(s, A->edge, B->edge, &IA, &IB);
    case 1: return PK(s, A->edge, B->edge, &IA, &IB);
    case 2: return PF(s, A->edge, B->edge, &IA, &IB);
    case 5: return KK(s, A->edge, B->edge, &IA, &IB);
    default: return FF(s, A->edge, B->edge, &IA, &IB);
    }
}

/* 0x1001a5c0: from the pierce triangle to the triangle "facing" pos */
static const IvpEdge *pierce_walk(PhysWorld *w, IvpSynapse *syn, const double pos[3])
{
    Info I;
    make_info(w, syn, &I);
    int n = I.ledge->n_triangles;
    uint8_t vis_small[64], *visited = n <= 64 ? vis_small : malloc((size_t)n);
    memset(visited, 0, (size_t)n);
    const IvpEdge *e = ivp_ledge_tri(I.ledge, ivp_pierce_index(ivp_tri(syn->edge)))->e;
    for (;;) {
        visited[ivp_tri_index(ivp_tri(e))] = 1;
        float wv[4];
        fvals(&I, e, pos, wv);
        const IvpEdge *ek = e, *next = NULL;
        for (int k = 0; k < 3 && !next; k++, ek = ivp_next(ek))
            if (wv[k] <= 0 && !visited[ivp_tri_index(ivp_tri(ivp_opp(ek)))]) next = ivp_opp(ek);
        if (!next) break;
        e = next;
    }
    if (visited != vis_small) free(visited);
    return e;
}

/* 0x10019790 */
void ivp_fix_backside(IvpMinSolver *s)
{
    IvpSynapse *a = ivp_md_sorted(s->md, 0), *syn = a->status == IVP_ST_BACKSIDE ? a : ivp_md_sorted(s->md, 1);
    syn->edge = pierce_walk(s->w, syn, s->pos);
    syn->status = IVP_ST_FACE;
}
