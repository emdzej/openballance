/* IVP contact points (docs/ivp_contact.md 3, "CP"): the persistent feature pairs a mindist leaves behind,
   their per-update geometry (0x1001f860 with the four case functions), materials, the creation constants,
   the destructor with its "friction deleted" event and the ball triangle transfer; plus the collision seam
   (docs/ivp_collision.md 8.3) that the PSI loop calls. */
#include "phys_contact.h"

/* ---- synapses and the constructor (CP2.4) ---- */

static void fsyn_link(IvpFricSyn *s)
{
    PhysBody *o = s->obj;
    s->prev = NULL;
    s->next = o->fric_syn;
    if (o->fric_syn) o->fric_syn->prev = s;
    o->fric_syn = s;
}

static void fsyn_unlink(IvpFricSyn *s)
{
    if (s->prev) s->prev->next = s->next;
    else s->obj->fric_syn = s->next;
    if (s->next) s->next->prev = s->prev;
    s->next = s->prev = NULL;
}

/* the triangle normal (P1 - P0) x (P2 - P0) of g's triangle, P0 = start(g) (0x10021280), object space */
static void tri_normal(const IvpEdge *g, double N[3])
{
    const IvpCompactLedge *l = ivp_ledge_of(g);
    const float *p0 = ivp_P(l, g), *p1 = ivp_P(l, ivp_next(g)), *p2 = ivp_P(l, ivp_prev(g));
    double a[3], b[3];
    for (int k = 0; k < 3; k++) a[k] = (double)p1[k] - p0[k], b[k] = (double)p2[k] - p0[k];
    N[0] = a[1] * b[2] - a[2] * b[1];
    N[1] = a[2] * b[0] - a[0] * b[2];
    N[2] = a[0] * b[1] - a[1] * b[0];
}

/* 0x1001a8b0: the sorted synapses (syn[(flags >> 8 ^ k) & 3]) become syn0 / syn1 */
static IvpContactPoint *cp_new(PhysWorld *w, IvpMindist *md)
{
    IvpContactPoint *cp = calloc(1, sizeof *cp);
    for (int k = 0; k < 2; k++) {
        const IvpSynapse *s = ivp_md_sorted(md, k);
        IvpFricSyn *fs = &cp->syn[k];
        fs->obj = s->obj;
        fs->g = s->edge;
        fs->type = (uint8_t)s->status;
        fs->cp = cp;
        fsyn_link(fs);
    }
    cp->time = w->current_time;
    if (cp->syn[1].type == IVP_ST_FACE) {
        double N[3];
        tri_normal(cp->syn[1].g, N);
        cp->inv_tri_det = (float)(1.0 / sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]));   /* 0x1000dd20 */
    }
    cp->recheck = 1;
    cp->gap = IVP_SET_FRICTION_DIST;
    cp->keeper = 20;
    cp->tmp = &cp->info;
    return cp;
}

/* 0x1001d840 */
static bool syn_match(const IvpFricSyn *cs, const IvpSynapse *ms)
{
    if (cs->type != ms->status) return false;
    switch (cs->type) {
    case IVP_ST_POINT: return ivp_ledge_of(cs->g) == ivp_ledge_of(ms->edge) && ((*cs->g ^ *ms->edge) & 0xffff) == 0;
    case IVP_ST_EDGE: return ms->edge == cs->g || ms->edge == ivp_opp(cs->g);
    case IVP_ST_FACE: return ivp_tri(cs->g) == ivp_tri(ms->edge);
    case IVP_ST_BALL: return true;
    default: assert(!"0x1001d840: feature type"); return false;
    }
}

/* 0x1001d910: the mindist's features, either way round */
static bool cp_matches(const IvpContactPoint *cp, const IvpMindist *md)
{
    const IvpSynapse *m0 = &md->syn[0], *m1 = &md->syn[1];
    if (cp->syn[0].obj == m0->obj && cp->syn[1].obj == m1->obj && syn_match(&cp->syn[0], m0) && syn_match(&cp->syn[1], m1)) return true;
    return cp->syn[0].obj == m1->obj && cp->syn[1].obj == m0->obj && syn_match(&cp->syn[0], m1) && syn_match(&cp->syn[1], m0);
}

/* 0x10020030 -> 0x1001ffe0: walk md.obj0's friction synapses for a contact with md.obj1 and matching features */
IvpContactPoint *ivp_cp_find_or_create(PhysWorld *w, IvpMindist *md, bool *created)
{
    PhysBody *o1 = md->syn[1].obj;
    for (IvpFricSyn *s = md->syn[0].obj->fric_syn; s; s = s->next) {
        IvpContactPoint *cp = s->cp;
        if ((cp->syn[0].obj == o1 || cp->syn[1].obj == o1) && cp_matches(cp, md)) {
            *created = false;
            return cp;
        }
    }
    *created = true;
    return cp_new(w, md);
}

/* ---- the per-update geometry (CP4) ---- */

/* 0x100217d0: the world position of start(g), M p + t in f64 */
static void world_point(const IvpCache *c, const IvpEdge *g, double o[3])
{
    const float *p = ivp_P(ivp_ledge_of(g), g);
    double d[3] = {p[0], p[1], p[2]};
    ivp_rmul(c->r, d, o);
    for (int k = 0; k < 3; k++) o[k] += c->vv[k];
}

/* 0x1000e030: unchanged below 1e-19 */
static void normize_f(float v[3])
{
    double n = ivp_dotd(v, v);
    if (n < IVP_C_1E19) return;
    double y = ivp_rsqrt(n);
    v[0] = (float)(v[0] * y), v[1] = (float)(v[1] * y), v[2] = (float)(v[2] * y);
}

static void left_feature(IvpContactInfo *t) { t->flags = (t->flags & ~0x200u) | 0x100u; }

/* 0x1001f050: POINT / BALL against POINT / BALL */
static void point_point(IvpContactPoint *cp, const double p0[3], const double p1[3], IvpContactInfo *t)
{
    double d[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
    double len = ivp_normalize_len(d);           /* 0x1000de30 */
    cp->gap = (float)len;
    for (int k = 0; k < 3; k++) t->n[k] = (float)d[k];
    const float *n = t->n;
    float ax, az;
    if (n[0] * n[0] < 0.9f) ax = 1, az = 0;       /* 0x10063878 */
    else ax = 0, az = 1;
    v3set(t->span[0], n[1] * az, n[2] * ax - n[0] * az, 0 - n[1] * ax);
    normize_f(t->span[0]);
    memcpy(t->cp_ws, p0, sizeof t->cp_ws);
}

/* 0x1001f160: POINT / BALL against EDGE */
static void point_edge(IvpContactPoint *cp, const double p0[3], const IvpEdge *g1, const IvpCache *c1, IvpContactInfo *t)
{
    double A[3], B[3];
    world_point(c1, g1, A);
    world_point(c1, ivp_next(g1), B);
    float e[3], r[3];
    for (int k = 0; k < 3; k++) e[k] = (float)(B[k] - A[k]), r[k] = (float)(p0[k] - A[k]);
    double inv_len = ivp_isqrt5(((double)e[2] * e[2] + (double)e[1] * e[1]) + (double)e[0] * e[0]);   /* 0x1000db80 */
    float c[3] = {r[2] * e[1] - e[2] * r[1], e[2] * r[0] - r[2] * e[0], r[1] * e[0] - e[1] * r[0]};
    double dist = sqrt(ivp_dotd(c, c)) * inv_len;     /* 0x1000e480 */
    cp->gap = (float)dist;
    if (dist * dist <= IVP_C_1E19) v3set(c, 1, 0, 0);
    else {
        double k = inv_len / dist;
        for (int i = 0; i < 3; i++) c[i] = (float)(c[i] * k);
    }
    v3set(t->n, c[2] * e[1] - c[1] * e[2], e[2] * c[0] - c[2] * e[0], c[1] * e[0] - e[1] * c[0]);
    normize_f(t->n);
    memcpy(t->cp_ws, p0, sizeof t->cp_ws);
    v3cpy(t->span[0], c);
    if (cp->recheck == 1) {
        cp->recheck = 0;
        double u = (((double)e[2] * r[2] + (double)r[1] * e[1]) + (double)r[0] * e[0]) * inv_len * inv_len;
        if (u < 0.0 || !(u <= 1.0)) left_feature(t);
    }
}

/* 0x1001eedd -> 0x10020ad0: the signs of the unnormalized barycentric weights of p (object space; f64
   inside, f32 results) */
static bool outside_triangle(const IvpEdge *g, const double p_os[3])
{
    float w[4];
    ivp_tri_qr_vals(ivp_ledge_of(g), g, p_os, w);
    return signbit(w[0]) || signbit(w[1]) || signbit(w[2]);   /* -0.0 counts */
}

/* 0x1001ee70: POINT / BALL against TRIANGLE */
static void point_triangle(IvpContactPoint *cp, const double p0[3], const IvpEdge *g1, const IvpCache *c1, IvpContactInfo *t)
{
    memcpy(t->cp_ws, p0, sizeof t->cp_ws);
    double d[3] = {p0[0] - c1->vv[0], p0[1] - c1->vv[1], p0[2] - c1->vv[2]}, p_os[3];
    ivp_rtmul(c1->r, d, p_os);                    /* 0x10018ca0 */
    if (cp->recheck == 1) {
        cp->recheck = 0;
        if (outside_triangle(g1, p_os)) left_feature(t);
    }
    double N[3], n_os[3], n_ws[3];
    tri_normal(g1, N);
    for (int k = 0; k < 3; k++) n_os[k] = N[k] * cp->inv_tri_det;
    ivp_rmul(c1->r, n_os, n_ws);                  /* 0x10018ea0 */
    const IvpCompactLedge *l = ivp_ledge_of(g1);
    const float *P0 = ivp_P(l, g1), *P1 = ivp_P(l, ivp_next(g1));
    cp->gap = (float)(((p_os[2] * n_os[2] + p_os[1] * n_os[1]) + p_os[0] * n_os[0]) -
                      ((P0[1] * n_os[1] + P0[0] * n_os[0]) + P0[2] * n_os[2]));
    for (int k = 0; k < 3; k++) t->n[k] = (float)(n_ws[k] * -1.0);
    float e[3] = {P1[0] - P0[0], P1[1] - P0[1], P1[2] - P0[2]};
    ivp_rmul_f(c1->r, e, t->span[0]);             /* 0x10018f10 */
}

/* 0x1001f3b0: EDGE against EDGE */
static void edge_edge(IvpContactPoint *cp, const IvpEdge *g0, const IvpEdge *g1, const IvpCache *c0, const IvpCache *c1,
                      IvpContactInfo *t)
{
    double A0[3], A1[3], B0[3], B1[3];
    world_point(c0, g0, A0), world_point(c0, ivp_next(g0), A1);
    world_point(c1, g1, B0), world_point(c1, ivp_next(g1), B1);
    double eb[3], ea[3];
    for (int k = 0; k < 3; k++) eb[k] = B1[k] - B0[k], ea[k] = A1[k] - A0[k];
    ivp_normalize4(eb);                           /* 0x1000dd50 */
    ivp_normalize4(ea);
    double c[3] = {ea[1] * eb[2] - ea[2] * eb[1], ea[2] * eb[0] - ea[0] * eb[2], ea[0] * eb[1] - ea[1] * eb[0]};
    double cc = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
    if (cc <= 1e-24) {                            /* 0x100634e8: parallel */
        left_feature(t);
        cp->gap = IVP_SET_KEEPER_DIST;
        memcpy(t->cp_ws, A0, sizeof t->cp_ws);
        v3set(t->n, 1, 0, 0);
        v3set(t->span[0], 0, 1, 0);
        return;
    }
    double na[3] = {ea[1] * c[2] - ea[2] * c[1], ea[2] * c[0] - ea[0] * c[2], ea[0] * c[1] - ea[1] * c[0]};
    double nb[3] = {eb[1] * c[2] - eb[2] * c[1], eb[2] * c[0] - eb[0] * c[2], eb[0] * c[1] - eb[1] * c[0]};
#define D3(a, b) ((a)[0] * (b)[0] + (a)[1] * (b)[1] + (a)[2] * (b)[2])
    double tB = (D3(na, B0) - D3(na, A0)) * (1.0 / (D3(na, B0) - D3(na, B1)));
    double tA = (D3(nb, A0) - D3(nb, B0)) * (1.0 / (D3(nb, A0) - D3(nb, A1)));
#undef D3
    double PA[3], PB[3], dd[3];
    for (int k = 0; k < 3; k++) {
        PA[k] = (1 - tA) * A0[k] + tA * A1[k];   /* 0x1000dc40 */
        PB[k] = (1 - tB) * B0[k] + tB * B1[k];
        dd[k] = PB[k] - PA[k];
    }
    double len = sqrt(dd[0] * dd[0] + dd[1] * dd[1] + dd[2] * dd[2]);
    if (len <= IVP_C_1E19) {
        cp->gap = 0;
        double k = 1 / sqrt(cc);
        for (int i = 0; i < 3; i++) t->n[i] = (float)(c[i] * k);
    } else {
        cp->gap = (float)len;
        double k = 1 / len;
        for (int i = 0; i < 3; i++) t->n[i] = (float)(dd[i] * k);
    }
    for (int i = 0; i < 3; i++) t->span[0][i] = (float)ea[i];
    memcpy(t->cp_ws, PA, sizeof t->cp_ws);
    if (cp->recheck == 1) {
        cp->recheck = 0;
        if (tB < 0 || tB > 1 || tA < 0 || tA > 1) left_feature(t);
    }
}

/* 0x1000bf90: R (w x p_cs) + v, the cross product f32, the rotation f64, the result f32 */
void ivp_core_point_velocity(const IvpCore *c, const float p_cs[3], const float v[3], const float w[3], float out[3])
{
    float x[3] = {w[1] * p_cs[2] - w[2] * p_cs[1], w[2] * p_cs[0] - w[0] * p_cs[2], w[0] * p_cs[1] - w[1] * p_cs[0]};
    double d[3] = {x[0], x[1], x[2]};
    ivp_rmul(c->m_world_f_core.r, d, d);
    for (int k = 0; k < 3; k++) out[k] = (float)(d[k] + v[k]);
}

/* 0x1001f860: normal, point, gap, arms and virtual mass from the current poses; the spring integrated over the
   time since the last update. Nothing is written to the cores. */
void ivp_cp_update(PhysWorld *w, IvpContactPoint *cp)
{
    IvpContactInfo *t = &cp->info;
    cp->tmp = t;
    t->pushes = 0;
    t->flags &= ~0x3ffu;
    PhysBody *o0 = cp->syn[0].obj, *o1 = cp->syn[1].obj;
    const IvpCache *c1 = ivp_cache_get(w, o1), *c0 = ivp_cache_get(w, o0);
    double p0[3], p1[3];
    switch (cp->syn[0].type) {
    case IVP_ST_POINT: world_point(c0, cp->syn[0].g, p0); break;
    case IVP_ST_BALL:
        memcpy(p0, c0->vv, sizeof p0);
        cp->recheck = 1;
        break;
    case IVP_ST_EDGE: edge_edge(cp, cp->syn[0].g, cp->syn[1].g, c0, c1, t); goto tail;
    default: assert(!"0x1001f860: syn0 is a triangle"); return;
    }
    switch (cp->syn[1].type) {
    case IVP_ST_POINT:
        world_point(c1, cp->syn[1].g, p1);
        point_point(cp, p0, p1, t);
        break;
    case IVP_ST_EDGE: point_edge(cp, p0, cp->syn[1].g, c1, t); break;
    case IVP_ST_FACE: point_triangle(cp, p0, cp->syn[1].g, c1, t); break;
    case IVP_ST_BALL: point_point(cp, p0, c1->vv, t); break;
    default: assert(!"0x1001f860: syn1 type"); return;
    }
tail:;
    float r0 = o0->extra_radius;
    for (int k = 0; k < 3; k++) t->cp_ws[k] = (double)t->n[k] * r0 + t->cp_ws[k];
    cp->gap = cp->gap - (r0 + o1->extra_radius);
    if (cp->gap < 0.0f) cp->gap = 0;              /* 0x10063370: the penetration depth is dropped */
    normize_f(t->span[0]);
    const float *n = t->n, *v0 = t->span[0];
    v3set(t->span[1], n[1] * v0[2] - v0[1] * n[2], v0[0] * n[2] - n[0] * v0[2], n[0] * v0[1] - n[1] * v0[0]);
    float vel[2][3] = {{0, 0, 0}, {0, 0, 0}};
    float inv[2] = {0, 0};
    for (int k = 0; k < 2; k++) {
        IvpCore *c = ivp_cp_core(cp, k);
        if (ivp_core_fixed(c)) {
            v3set(t->cp_cs[k], 0, 0, 0);
            v3set(t->cross_cs[k], 0, 0, 0);
            t->core[k] = NULL;
            continue;
        }
        const double (*R)[3] = c->m_world_f_core.r;
        double d[3] = {t->cp_ws[0] - c->m_world_f_core.vv[0], t->cp_ws[1] - c->m_world_f_core.vv[1], t->cp_ws[2] - c->m_world_f_core.vv[2]};
        float *cs = t->cp_cs[k], ncs[3];
        for (int i = 0; i < 3; i++) cs[i] = (float)((R[1][i] * d[1] + R[2][i] * d[2]) + R[0][i] * d[0]);
        for (int i = 0; i < 3; i++) ncs[i] = (float)((R[1][i] * n[1] + R[2][i] * n[2]) + R[0][i] * n[0]);
        float *cr = t->cross_cs[k];
        v3set(cr, cs[1] * ncs[2] - cs[2] * ncs[1], cs[2] * ncs[0] - cs[0] * ncs[2], ncs[1] * cs[0] - ncs[0] * cs[1]);
        ivp_core_point_velocity(c, cs, c->speed, c->rot_speed, vel[k]);
        const float *iI = c->inv_rot_inertia;
        inv[k] = (float)((((double)(cr[2] * iI[2]) * cr[2] + (double)(cr[1] * iI[1]) * cr[1]) + (double)(cr[0] * iI[0]) * cr[0]) + c->inv_mass);
        t->core[k] = c;
    }
    t->inv_virt_mass = t->core[1] ? inv[1] + inv[0] : inv[0];
    float vrel[3];
    if (t->core[1]) v3sub(vrel, vel[0], vel[1]);  /* 0x100102d0 */
    else v3cpy(vrel, vel[0]);
    t->virt_mass = 1.0f / t->inv_virt_mass;
    double dt = w->current_time - cp->time;
    cp->time = w->current_time;
    cp->s[0] = (float)(cp->s[0] - ivp_dotd(vrel, t->span[0]) * dt);
    cp->s[1] = (float)(cp->s[1] - ivp_dotd(vrel, t->span[1]) * dt);
}

/* ---- materials (CP5) ---- */

/* 0x10023fa0 with 0x1001d830: the ledge triangle's material index (always 0 in Ballance: the object's
   material); a nonzero index means the manager's default material (0x1000be30: 0.5 / 0.5) */
static void syn_material(const IvpFricSyn *s, double *friction, double *elasticity)
{
    uint32_t idx = (ivp_tri(s->g)->word >> 24) & 0x7f;
    if (idx == 0) *friction = (double)s->obj->friction, *elasticity = (double)s->obj->elasticity;
    else *friction = 0.5, *elasticity = 0.5;
}

/* 0x10024040: the products of the material manager (0x1000bd10 / 0x1000bd40) */
void ivp_cp_read_materials(PhysWorld *w, IvpContactPoint *cp)
{
    (void)w;
    IvpContactInfo *t = cp->tmp;
    double f[2], e[2];
    for (int k = 0; k < 2; k++) {
        syn_material(&cp->syn[k], &f[k], &e[k]);
        t->mat_friction[k] = (float)f[k], t->mat_elasticity[k] = (float)e[k];
        t->obj[k] = cp->syn[k].obj;
        t->g[k] = cp->syn[k].g;
    }
    t->elasticity = (float)(e[0] * e[1]);
    cp->mu = (float)(f[1] * f[0]);
}

/* 0x1000c200: the worst-case virtual mass at p (core space) */
double ivp_worst_vm(const IvpCore *c, const float p[3])
{
    const float *iI = c->inv_rot_inertia;
    float A = (p[1] * p[1] + p[2] * p[2]) * iI[0], B = (p[0] * p[0] + p[2] * p[2]) * iI[1], C = (p[0] * p[0] + p[1] * p[1]) * iI[2];
    float m = A;
    if (B > m) m = B;
    if (C > m) m = C;
    return 1.0 / ((double)m + c->inv_mass);
}

/* 0x1001d3d0 (creation only, after the update) */
void ivp_cp_init_constants(IvpContactPoint *cp)
{
    const IvpContactInfo *t = cp->tmp;
    IvpCore *c0 = ivp_cp_core(cp, 0), *c1 = ivp_cp_core(cp, 1);
    if (ivp_core_fixed(c0)) cp->inv_vm_no_dir = (float)(1.0 / ivp_worst_vm(c1, t->cp_cs[1]));
    else if (ivp_core_fixed(c1)) cp->inv_vm_no_dir = (float)(1.0 / ivp_worst_vm(c0, t->cp_cs[0]));
    else {
        double m0 = ivp_worst_vm(c0, t->cp_cs[0]), m1 = ivp_worst_vm(c1, t->cp_cs[1]);
        cp->inv_vm_no_dir = (float)(1.0 / ((m1 * m0) / (m1 + m0)));
    }
}

/* 0x1001c230 + the caller's free: "friction deleted" to the listeners (0x10013c00, 0x1000a900), then the
   synapses leave their objects */
void ivp_cp_destroy(PhysWorld *w, IvpContactPoint *cp)
{
    if (w->contact_fn) w->contact_fn(w->listen_user, cp->syn[0].obj, cp->syn[1].obj, false);
    fsyn_unlink(&cp->syn[0]);
    fsyn_unlink(&cp->syn[1]);
    free(cp);
}

/* 0x10018040: a ball with a contact rolls onto a new triangle; the new contact inherits the pressure and the
   spring displacement of the ball's newest one (the old one is left to the removal tests). Reached only for
   car wheels (the wheel vector of 0x10017790; see phys_mindist.c), so never in Ballance. */
void ivp_ball_transfer(PhysWorld *w, IvpMindist *md, PhysBody *ball, float gap)
{
    if (!ball->fric_syn) return;
    IvpFrictionSystem *fs;
    bool created;
    IvpContactPoint *nw = ivp_try_generate_friction(w, md, &fs, &created, NULL, true);
    if (!created) return;
    IvpContactPoint *old = ball->fric_syn->cp;
    if (old == nw) return;
    double t_saved = old->time;
    old->time = w->current_time;
    ivp_cp_update(w, old);                        /* dt = 0: refreshes old's tmp only */
    nw->pressure = old->pressure;
    old->pressure = 0;
    nw->time = t_saved;
    nw->gap = gap;
    old->time = t_saved;
    float sv[3];
    for (int k = 0; k < 3; k++) sv[k] = old->s[0] * old->info.span[0][k] + old->s[1] * old->info.span[1][k];
    nw->s[0] = ivp_dotf(sv, nw->info.span[0]);
    nw->s[1] = ivp_dotf(sv, nw->info.span[1]);
}

/* ---- the collision seam ---- */

/* PSI start: the PSI listeners (none) and the mindist manager's wheel pass (0x10017790) */
void phys_collision_psi_start(PhysWorld *w) { ivp_wheel_pass(w); }

/* 0x100099a0 for the unit's objects, cores and objects n-1..0 */
void phys_collision_after_controllers(PhysWorld *w, IvpSimUnit *su)
{
    for (uint32_t i = su->ncores; i-- > 0;) ivp_object_after_controllers(w, su->cores[i]->body);
}

int phys_contact_revive_core(PhysWorld *w, IvpCore *c) { return ivp_fs_revive_core(w, c); }

/* in 0x1001e300: the object's hull grows with the core's speeds (0x1001e750); objects whose first listener
   falls below the next-PSI value are checked (0x1001ecd0) */
void phys_collision_core_integrated(PhysWorld *w, IvpCore *c, double dt)
{
    IvpHullManager *h = &c->body->coll.hull;
    ivp_hull_update(h, w->current_time, dt, c->max_surface_rot_speed + c->current_speed, c->current_speed);
    if (h->list.min_value - h->hull_value_next_psi < 0.0f) {
        IvpCollWorld *cw = &w->coll;
        if (cw->ncheck == cw->capcheck) cw->capcheck = cw->capcheck ? cw->capcheck * 2 : 32, cw->check = realloc(cw->check, cw->capcheck * sizeof *cw->check);
        cw->check[cw->ncheck++] = h;
    }
}

/* 0x10013cb0 after integration */
void phys_collision_psi_end(PhysWorld *w)
{
    ivp_hull_phase(w);                            /* 0x1001eb10 */
    w->state = 3;
    ivp_short_pass(w);                            /* 0x10017850 */
    w->state = 4;
    ivp_critic_pass(w);                           /* 0x10017770 */
}

/* 0x1000ce20, per object: recheck as non-moving, the hull frozen at its value now, the cache dropped */
void phys_collision_object_frozen(PhysWorld *w, PhysBody *b)
{
    ivp_recheck_ov_element(w, b);
    IvpHullManager *h = &b->coll.hull;
    double dt = w->current_time - h->last_time;
    h->hull_value = (float)(h->hull_value + dt * h->gradient);
    h->gradient = 0;
    h->center_hull_value = (float)(h->center_hull_value + dt * h->center_gradient);
    h->center_gradient = 0;
    ivp_hull_reset(h);
    ivp_cache_invalidate(b);
}

/* 0x1000cf20 -> 0x100099f0 */
void phys_collision_object_revived(PhysWorld *w, PhysBody *b) { ivp_recheck_ov_element_forced(w, b); }

/* 0x10009ab0: the object is deleted, its contacts go without reviving anything */
void phys_contacts_forget(PhysWorld *w, PhysBody *b) { ivp_object_remove_contacts(w, b, true); }

/* the anomaly manager's inter_penetration (0x1002f8f0): push the objects apart at 2 |g| dPSI (0x1002fc80) */
void phys_contact_inter_penetration(PhysWorld *w, IvpMindist *md)
{
    PhysBody *A = md->syn[0].obj, *B = md->syn[1].obj;
    const float *g = w->gravity.g;
    float push = (float)(2 * sqrt((double)g[0] * g[0] + (double)g[1] * g[1] + (double)g[2] * g[2]) * w->delta_psi);
    bool fa = A->core.unmovable, fb = B->core.unmovable;
    if (fa != fb) {
        IvpSynapse *fs = fa ? &md->syn[0] : &md->syn[1];
        PhysBody *mov = fa ? B : A;
        if (fs->status < IVP_ST_BALL) {
            /* the plane of the fixed ledge's triangles farthest out from the moving core */
            const IvpCache *c = ivp_cache_get(w, fs->obj);
            double X[3], best = -1e101, n[3] = {1, 0, 0};
            const double *cp = mov->core.m_world_f_core.vv;
            double d[3] = {cp[0] - c->vv[0], cp[1] - c->vv[1], cp[2] - c->vv[2]};
            ivp_rtmul(c->r, d, X);
            const IvpCompactLedge *l = ivp_ledge_of(fs->edge);
            for (int t = 0; t < l->n_triangles; t++) {
                double m[3];
                tri_normal(ivp_ledge_tri(l, t)->e, m);
                if (!ivp_normalize_d(m)) continue;
                const float *q0 = ivp_P(l, &ivp_ledge_tri(l, t)->e[0]);
                double dd = (X[0] - q0[0]) * m[0] + (X[1] - q0[1]) * m[1] + (X[2] - q0[2]) * m[2];
                if (dd > best) best = dd, memcpy(n, m, sizeof n);
            }
            double nw[3];
            ivp_rmul(c->r, n, nw);
            if (!mov->fixed && mov->object_state < IVP_MT_STATIC) {
                for (int k = 0; k < 3; k++) mov->core.speed_change[k] += (float)(nw[k] * push);   /* 0x1000a350 */
                ivp_body_wake(w, mov);
            }
            return;
        }
    }
    /* 0x1002f780 */
    const double *pa = A->core.m_world_f_core.vv, *pb = B->core.m_world_f_core.vv;
    float dir[3] = {(float)(pb[0] - pa[0]), (float)(pb[1] - pa[1]), (float)(pb[2] - pa[2])};
    if (ivp_normalize_f(dir) == 0) return;
    PhysBody *ob[2] = {A, B};
    for (int k = 0; k < 2; k++) {
        PhysBody *o = ob[k];
        if (o->fixed || o->core.movement_state >= 8) continue;
        float imp[3];
        v3scale(imp, dir, (k ? 1.0f : -1.0f) * o->core.mass * push);
        ivp_body_async_push_ws(w, o, pa, imp);    /* 0x1000a3e0 */
    }
}

/* ---- debugging ---- */

void phys_debug(const PhysWorld *w, uint32_t *bodies, uint32_t *awake, uint32_t *contacts)
{
    *bodies = *awake = *contacts = 0;
    for (const PhysBody *b = w->bodies; b; b = b->next) (*bodies)++, *awake += b->object_state == IVP_MT_MOVING;
    for (const IvpFrictionSystem *fs = w->fs_list; fs; fs = fs->wnext) *contacts += (uint32_t)fs->n_contacts;
}

bool phys_debug_contact(const PhysWorld *w, uint32_t i, uint32_t *ea, uint32_t *eb, int types[2], float *gap, float n[3], float p[3])
{
    for (const IvpFrictionSystem *fs = w->fs_list; fs; fs = fs->wnext)
        for (const IvpContactPoint *cp = fs->first; cp; cp = cp->next) {
            if (i--) continue;
            *ea = cp->syn[0].obj->entity, *eb = cp->syn[1].obj->entity;
            types[0] = cp->syn[0].type, types[1] = cp->syn[1].type;
            *gap = cp->gap;
            v3cpy(n, cp->info.n);
            for (int k = 0; k < 3; k++) p[k] = (float)cp->info.cp_ws[k];
            return true;
        }
    return false;
}
