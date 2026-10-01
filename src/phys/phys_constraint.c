/* The spring actuator and the constraint solver (docs/ivp_core.md 12): Set Physics Spring, Hinge, Slider and
   Ball Joint. Both are controllers of the sim unit their bodies are merged into (0x10011b30). */
#include "phys_internal.h"
#include <stdlib.h>

static void joint_link(PhysWorld *w, PhysJoint *j)
{
    j->next = w->joints;
    w->joints = j;
}

/* the movable cores of the joint's bodies, the second skipped if equal */
static void joint_cores(PhysJoint *j)
{
    j->ncores = 0;
    if (j->a && !j->a->core.unmovable) j->cores[j->ncores++] = &j->a->core;
    if (j->b && !j->b->core.unmovable && (!j->a || j->b != j->a)) j->cores[j->ncores++] = &j->b->core;
}

/* 0x10009c40 (get_m_world_f_object_AT at the current time), identity for the world */
static void object_world(const PhysWorld *w, const PhysBody *b, IvpMatrix *m)
{
    if (b) ivp_body_m_world_f_object_at(b, w->current_time, m);
    else ivp_mat_identity(m);
}

/* 0x10009be0: m_core_f_object (rotation q_core_f_object, identity here; vv = shift_core_f_object) */
static void core_f_object(const PhysBody *b, IvpMatrix *m)
{
    ivp_mat_identity(m);
    if (b)
        for (int k = 0; k < 3; k++) m->vv[k] = b->shift_core_f_object[k];
}

/* R v + vv, f32 v (0x1000f5f0) */
static void mat_point(const IvpMatrix *m, const float v[3], double out[3])
{
    double d[3] = {v[0], v[1], v[2]};
    ivp_rmul(m->r, d, out);
    for (int k = 0; k < 3; k++) out[k] += m->vv[k];
}

/* ---- spring (12.1-12.3) ---- */

/* 0x10013ea0: an anchor at a world position, kept in core space */
static void anchor_init(const PhysWorld *w, const PhysBody *b, const float pos_ws[3], float p_core[3])
{
    if (!b) {
        v3cpy(p_core, pos_ws);
        return;
    }
    IvpMatrix m, mi;
    object_world(w, b, &m);
    ivp_mat_inverse(&mi, &m);                 /* 0x10018ca0 */
    double p[3];
    float p_object[3];
    mat_point(&mi, pos_ws, p);
    for (int k = 0; k < 3; k++) p_object[k] = (float)p[k];
    for (int k = 0; k < 3; k++) p_core[k] = p_object[k] + b->shift_core_f_object[k];
}

/* 0x1000c0b0 operand: the surface speed, 0 for the world or an unmovable core */
static void vsurf(const PhysBody *b, const float p_cs[3], float out[3])
{
    if (!b || b->core.unmovable) v3set(out, 0, 0, 0);
    else ivp_surface_speed(&b->core, p_cs, out);
}

/* 0x100146d0 */
static void spring_simulate(IvpController *ctrl, const IvpEventSim *ev, IvpCore **cores, uint32_t n)
{
    PhysJoint *j = (PhysJoint *)ctrl;
    double p0[3], p1[3];
    if (j->a) mat_point(&j->a->core.m_world_f_core, j->anchor_core[0], p0);
    else for (int k = 0; k < 3; k++) p0[k] = j->anchor_core[0][k];
    if (j->b) mat_point(&j->b->core.m_world_f_core, j->anchor_core[1], p1);
    else for (int k = 0; k < 3; k++) p1[k] = j->anchor_core[1][k];
    float d[3] = {(float)(p0[0] - p1[0]), (float)(p0[1] - p1[1]), (float)(p0[2] - p1[2])};
    float len = ivp_normalize_f(d);
    if (len < 1e-10f) return;
    /* (break length / listeners: never enabled by the glue) */
    double F = ((double)len - j->length) * j->k;
    float u0[3], u1[3], u[3];
    vsurf(j->b, j->anchor_core[1], u1);
    vsurf(j->a, j->anchor_core[0], u0);
    v3sub(u, u1, u0);
    double s = (F - j->c_lin * ((double)d[0] * u[0] + (double)d[1] * u[1] + (double)d[2] * u[2])) * ev->delta_time;
    double cr = j->c_rel * ev->delta_time;
    float I[3], mI[3];
    for (int k = 0; k < 3; k++) I[k] = (float)(d[k] * s - cr * u[k]);
    v3scale(mI, I, -1.0f);
    if (j->b && j->b->core.movement_state < 8) ivp_async_push_core_ws(&j->b->core, p1, I);
    if (j->a && j->a->core.movement_state < 8) ivp_async_push_core_ws(&j->a->core, p0, mI);
}

static const IvpControllerVt spring_vt = {1500, spring_simulate, true};

/* Set Physics Spring (0x10006490): template 0x10014200 with [3] Length, [4] 0 (absolute values, f = 1),
   [5] Constant, [6] Linear Dampening (along the spring), [7] Global Dampening (the full relative velocity);
   create 0x10013700 / ctor 0x10014250 */
PhysJoint *phys_spring_create(PhysWorld *w, PhysBody *a, const float pa[3], PhysBody *b, const float pb[3], float length,
                              float constant, float lin_damp, float glob_damp)
{
    PhysJoint *j = calloc(1, sizeof *j);
    j->ctrl.vt = &spring_vt;
    j->kind = PHYS_SPRING;
    j->a = a, j->b = b;
    anchor_init(w, a, pa, j->anchor_core[0]);
    anchor_init(w, b, pb, j->anchor_core[1]);
    float f = 1.0f;
    j->length = length;
    j->k = constant * f, j->c_lin = lin_damp * f, j->c_rel = glob_damp * f;
    joint_cores(j);
    joint_link(w, j);
    ivp_controller_attach_merge(w, &j->ctrl, j->cores, j->ncores);
    return j;
}

/* ---- constraints (12.4-12.6) ---- */

/* 0x10033e50: Gaussian elimination with partial pivoting (0x10033f80) and back substitution (0x10034030) */
static bool lin_solve(double A[6][6], double b[6], int n, double x[6])
{
    const double eps = (double)1e-9f;
    for (int i = 0; i < n; i++) {
        int p = i;
        double best = fabs(A[i][i]);
        for (int r = n - 1; r > i; r--)
            if (fabs(A[r][i]) > best) best = fabs(A[r][i]), p = r;
        if (p != i) {
            for (int k = 0; k < n; k++) {
                double t = A[i][k];
                A[i][k] = A[p][k], A[p][k] = t;
            }
            double t = b[i];
            b[i] = b[p], b[p] = t;
        }
        double piv = A[i][i];
        if (fabs(piv) < eps) continue;
        for (int r = i + 1; r < n; r++) {
            if (!(fabs(A[r][i]) > eps)) continue;
            double f = A[r][i] * (-1.0 / piv);
            for (int k = i; k < n; k++) A[r][k] += f * A[i][k];
            b[r] += f * b[i];
        }
    }
    for (int i = n - 1; i >= 0; i--) {
        double s = b[i];
        for (int k = i + 1; k < n; k++) s -= A[i][k] * x[k];
        if (fabs(A[i][i]) >= eps) x[i] = s / A[i][i];
        else if (fabs(s) < eps * 1000) x[i] = 0;
        else {
            for (int k = 0; k < n; k++) x[k] = 0;
            return false;
        }
    }
    return true;
}

static void rtmul_fd(const double r[3][3], const double v[3], float o[3])
{
    double t[3];
    ivp_rtmul(r, v, t);
    o[0] = (float)t[0], o[1] = (float)t[1], o[2] = (float)t[2];
}

static void rmul_ff(const double r[3][3], const float v[3], double o[3])
{
    double d[3] = {v[0], v[1], v[2]};
    ivp_rmul(r, d, o);
}

/* 0x10028960: one exact velocity solve of all active axes per PSI */
static void constraint_simulate(IvpController *ctrl, const IvpEventSim *ev, IvpCore **cores, uint32_t ncores)
{
    PhysJoint *j = (PhysJoint *)ctrl;
    IvpCore *cR = j->a ? &j->a->core : NULL, *cA = j->b ? &j->b->core : NULL;
    bool mR = cR && !cR->unmovable, mA = cA && !cA->unmovable;
    IvpMatrix ident;
    ivp_mat_identity(&ident);
    const IvpMatrix *WR = cR ? &cR->m_world_f_core : &ident, *WA = cA ? &cA->m_world_f_core : &ident;
    float dt = (float)ev->delta_time;
    double idt = ev->i_delta_time;
    const double (*Rr)[3] = j->m_rcs_f_rcore.r, (*Ra)[3] = j->m_acs_f_acore.r;
    double CsW[3][3], CsA[3][3];
    ivp_mat_rrt(CsW, Rr, WR->r);              /* Rcs_f_world */
    ivp_mat_rr(CsA, CsW, WA->r);              /* Rcs_f_Acore (= CsA_rot) */

    /* angular error */
    double erot[3] = {0, 0, 0};
    int nrot = j->nfixed_rot + j->nlimited_rot;
    if (nrot == 3 || nrot == 2) {
        double Q[3][3];
        ivp_mat_rrt(Q, CsA, Ra);
        if (nrot == 3) {
            IvpQuat q;
            ivp_matrix_to_quat(Q, &q);
            double qv[3] = {(float)q.x, q.y, q.z};
            for (int i = 0; i < 3; i++) {
                double x = qv[i], x3 = x * x * x;
                erot[i] = 2 * x + 0.24f * x3 + 0.58f * x3 * x * x;
            }
            if (q.w < 0)
                for (int i = 0; i < 3; i++) erot[i] = -erot[i];
        } else {
            int o0 = j->rot_order[0], o1 = j->rot_order[1], o2 = j->rot_order[2];
            double b[3] = {Q[0][o2], Q[1][o2], Q[2][o2]};    /* A's free axis in Rcs */
            erot[o0] = -atan2(b[o1], b[o2]);
            erot[o1] = atan2(b[o0], b[o2]);
            erot[o2] = 0;
        }
    }
    /* (one locked rotation axis: not reachable from the content, not ported) */

    /* linear part */
    float aR[3], aA[3];
    double t[3];
    for (int k = 0; k < 3; k++) t[k] = -j->m_rcs_f_rcore.vv[k];
    rtmul_fd(Rr, t, aR);
    for (int k = 0; k < 3; k++) t[k] = -j->m_acs_f_acore.vv[k];
    rtmul_fd(Ra, t, aA);
    double pR[3], pA[3];
    mat_point(WR, aR, pR);
    mat_point(WA, aA, pA);
    float vR[3] = {0, 0, 0}, vA[3] = {0, 0, 0};
    if (cR) ivp_surface_speed(cR, aR, vR);
    if (cA) ivp_surface_speed(cA, aA, vA);
    double d[3] = {pA[0] - pR[0], pA[1] - pR[1], pA[2] - pR[2]}, err[3], relv[3], cvA[3], cvR[3];
    ivp_rmul(CsW, d, err);
    rmul_ff(CsW, vA, cvA);
    rmul_ff(CsW, vR, cvR);
    for (int k = 0; k < 3; k++) relv[k] = cvA[k] - cvR[k];
    double kd = (double)dt * j->damp_factor, g = -(double)j->force_factor * (float)idt, rhs[6];
    for (int i = 0; i < 3; i++) {
        double b = err[i] + kd * relv[i];
        if (j->type[i] & 2) {
            if (b < j->lo[i]) j->type[i] |= 1, b = dt * relv[i] + (err[i] - j->lo[i]) * j->limit_factor;
            else if (b > j->hi[i]) j->type[i] |= 1, b = dt * relv[i] + (err[i] - j->hi[i]) * j->limit_factor;
            else j->type[i] &= ~1u;
        }
        rhs[i] = (float)b * g;
    }
    /* angular part */
    double wA[3] = {0, 0, 0}, wR[3] = {0, 0, 0}, relw[3];
    if (cA) rmul_ff(CsA, cA->rot_speed, wA);
    if (cR) rmul_ff(Rr, cR->rot_speed, wR);
    for (int k = 0; k < 3; k++) relw[k] = wA[k] - wR[k];
    for (int i = 0; i < 3; i++) {
        double c = erot[i] + kd * relw[i];
        if (j->type[3 + i] & 2) {
            if (c < j->lo[3 + i]) j->type[3 + i] |= 1, c = dt * relw[i] + (erot[i] - j->lo[3 + i]) * j->limit_factor;
            else if (c > j->hi[3 + i]) j->type[3 + i] |= 1, c = dt * relw[i] + (erot[i] - j->hi[3 + i]) * j->limit_factor;
            else j->type[3 + i] &= ~1u;
        }
        rhs[3 + i] = (float)c * g;
    }
    int act[6], na = 0;
    for (int i = 0; i < 6; i++)
        if (j->type[i] & 1) act[na++] = i;
    j->nactive = (uint8_t)na;
    if (!na) return;

    /* the response matrix: column a = the measured axes' response to a unit impulse on active axis a */
    double M[6][6], B[6], x[6];
    for (int ci = 0; ci < na; ci++) {
        int a = act[ci];
        double e[3] = {0, 0, 0}, ne[3];
        e[a % 3] = 1;
        for (int k = 0; k < 3; k++) ne[k] = -e[k];
        double T[3] = {0, 0, 0}, Rm[3] = {0, 0, 0}, tv[3];
        if (a < 3) {
            float dc[3], dw_[3], dv[3], dw[3], c[3], cw[3];
            if (mR) {
                rtmul_fd(Rr, e, dc);
                rtmul_fd(CsW, e, dw_);
                ivp_calc_push_core(cR, aR, dc, dw_, dv, dw);
                v3cross(c, dw, aR);
                ivp_rmul_f(WR->r, c, cw);
                v3add(cw, cw, dv);              /* 0x1000bf90 */
                rmul_ff(CsW, cw, tv);
                for (int k = 0; k < 3; k++) T[k] -= tv[k];
                rmul_ff(Rr, dw, tv);
                for (int k = 0; k < 3; k++) Rm[k] -= tv[k];
            }
            if (mA) {
                rtmul_fd(CsA, ne, dc);
                rtmul_fd(CsW, ne, dw_);
                ivp_calc_push_core(cA, aA, dc, dw_, dv, dw);
                v3cross(c, dw, aA);
                ivp_rmul_f(WA->r, c, cw);
                v3add(cw, cw, dv);
                rmul_ff(CsW, cw, tv);
                for (int k = 0; k < 3; k++) T[k] += tv[k];
                rmul_ff(CsA, dw, tv);
                for (int k = 0; k < 3; k++) Rm[k] += tv[k];
            }
        } else {
            float tc[3], dw[3], dv[3];
            if (mR) {
                rtmul_fd(Rr, e, tc);
                for (int k = 0; k < 3; k++) dw[k] = cR->inv_rot_inertia[k] * tc[k];   /* 0x1000cb20, k = 1 */
                v3cross(dv, dw, aR);
                rmul_ff(Rr, dv, tv);
                for (int k = 0; k < 3; k++) T[k] -= tv[k];
                rmul_ff(Rr, dw, tv);
                for (int k = 0; k < 3; k++) Rm[k] -= tv[k];
            }
            if (mA) {
                rtmul_fd(CsA, ne, tc);
                for (int k = 0; k < 3; k++) dw[k] = cA->inv_rot_inertia[k] * tc[k];
                v3cross(dv, dw, aA);
                rmul_ff(CsA, dv, tv);           /* A side by CsA (local_760), R side by +0x70 */
                for (int k = 0; k < 3; k++) T[k] += tv[k];
                rmul_ff(CsA, dw, tv);
                for (int k = 0; k < 3; k++) Rm[k] += tv[k];
            }
        }
        for (int ri = 0; ri < na; ri++) {
            int r = act[ri];
            M[ri][ci] = r < 3 ? T[r] : Rm[r - 3];
        }
    }
    for (int ri = 0; ri < na; ri++) B[ri] = rhs[act[ri]];
    if (!lin_solve(M, B, na, x)) return;     /* the constraint applies nothing this PSI */
    double lt[3] = {0, 0, 0}, lr[3] = {0, 0, 0}, nlt[3], nlr[3];
    for (int ci = 0; ci < na; ci++) {
        if (act[ci] < 3) lt[act[ci]] = x[ci];
        else lr[act[ci] - 3] = x[ci];
    }
    for (int k = 0; k < 3; k++) nlt[k] = -lt[k], nlr[k] = -lr[k];
    /* apply, immediately */
    if (mR) {
        float ic[3], iw[3], tc[3];
        rtmul_fd(Rr, lt, ic);
        ivp_rmul_f(WR->r, ic, iw);
        ivp_push_core(cR, aR, ic, iw);
        rtmul_fd(Rr, lr, tc);
        ivp_rot_push_core_cs(cR, tc);
    }
    if (mA) {
        float ic[3], iw[3], tc[3];
        rtmul_fd(CsA, nlt, ic);
        ivp_rmul_f(WA->r, ic, iw);
        ivp_push_core(cA, aA, ic, iw);
        rtmul_fd(CsA, nlr, tc);
        ivp_rot_push_core_cs(cA, tc);
    }
    (void)cores, (void)ncores;
}

static const IvpControllerVt constraint_vt = {405, constraint_simulate, true};

/* 0x10012870 (set_constraint_ws) + 0x10012690 + create 0x100129c0 / init 0x100284d0. R = the BB target, A =
   Object2; the constraint axis is constraint-space z */
static PhysJoint *constraint_create(PhysWorld *w, int kind, PhysBody *R, PhysBody *A, const float anchor[3], const double *axis,
                                    int ntrans, int nrot)
{
    if (!R && !A) return NULL;
    PhysJoint *j = calloc(1, sizeof *j);
    j->ctrl.vt = &constraint_vt;
    j->kind = kind;
    j->a = R, j->b = A;
    /* the template (0x10012570) */
    IvpMatrix MR, MRi, rfs_f_rcs;
    object_world(w, R, &MR);
    ivp_mat_inverse(&MRi, &MR);
    double anc[3] = {anchor[0], anchor[1], anchor[2]};
    ivp_mat_identity(&rfs_f_rcs);
    ivp_rmul(MRi.r, anc, rfs_f_rcs.vv);
    for (int k = 0; k < 3; k++) rfs_f_rcs.vv[k] += MRi.vv[k];
    if (axis) {
        double ax[3];
        ivp_rtmul(MR.r, axis, ax);
        if (nrot == 2 || ntrans == 2) ivp_basis(rfs_f_rcs.r, ax, 2);
        else if (nrot == 1 || ntrans == 1) ivp_basis(rfs_f_rcs.r, ax, 0);
    }
    for (int i = 0; i < 3; i++) j->type[i] = ntrans > i, j->type[3 + i] = nrot > i;
    j->force_factor = 1.0f;
    j->damp_factor = 1.0f / 1.0f;        /* damp_factor / force_factor */
    j->limit_factor = 0.3f;
    /* the constraint object */
    IvpMatrix WlR, WlA, WlRi, rfs_f_afs, rcore_f_rfs, acore_f_afs, afs_f_acore, rcore_f_acore, t;
    WlR = MR;
    object_world(w, A, &WlA);
    ivp_mat_inverse(&WlRi, &WlR);
    ivp_mat_mul(&rfs_f_afs, &WlRi, &WlA);
    core_f_object(R, &rcore_f_rfs);
    core_f_object(A, &acore_f_afs);
    ivp_mat_inverse(&afs_f_acore, &acore_f_afs);
    ivp_mat_mul(&t, &rcore_f_rfs, &rfs_f_afs);
    ivp_mat_mul(&rcore_f_acore, &t, &afs_f_acore);
    ivp_mat_mul(&t, &rcore_f_rfs, &rfs_f_rcs);
    if (!ivp_mat_inverse_general(&j->m_rcs_f_rcore, &t, 1e-19)) ivp_mat_identity(&j->m_rcs_f_rcore);
    ivp_mat_mul(&j->m_acs_f_acore, &j->m_rcs_f_rcore, &rcore_f_acore);   /* A's frame coincides with R's */
    joint_cores(j);
    joint_link(w, j);
    return j;
}

/* the rotation order bytes (0x100283f0): fixed, then limited, then free */
static void constraint_register(PhysWorld *w, PhysJoint *j)
{
    int o = 0;
    j->nfixed_rot = j->nlimited_rot = 0;
    for (int i = 0; i < 3; i++)
        if (j->type[3 + i] == 1) j->rot_order[o++] = (uint8_t)i, j->nfixed_rot++;
    for (int i = 0; i < 3; i++)
        if (j->type[3 + i] & 2) j->rot_order[o++] = (uint8_t)i, j->nlimited_rot++;
    for (int i = 0; i < 3; i++)
        if (j->type[3 + i] == 0) j->rot_order[o++] = (uint8_t)i;
    ivp_controller_attach_merge(w, &j->ctrl, j->cores, j->ncores);   /* 0x10037620 -> 0x10011b30 */
}

/* Set Physics Hinge (0x100059d0): 3 translations and 2 rotations locked about the axis */
PhysJoint *phys_hinge_create(PhysWorld *w, PhysBody *a, PhysBody *b, const float anchor[3], const float axis[3])
{
    double ax[3] = {axis[0], axis[1], axis[2]};
    PhysJoint *j = constraint_create(w, PHYS_HINGE, a, b, anchor, ax, 3, 2);
    if (j) constraint_register(w, j);
    return j;
}

/* Set Physics Slider (0x10005f10): axis = p1 - p0 (unnormalized), 2 translations and 3 rotations locked;
   limits on translation axis z (0x10012960) */
PhysJoint *phys_slider_create(PhysWorld *w, PhysBody *a, PhysBody *b, const float p0[3], const float p1[3], bool limits,
                              float lo, float hi)
{
    double ax[3] = {(double)p1[0] - p0[0], (double)p1[1] - p0[1], (double)p1[2] - p0[2]};
    PhysJoint *j = constraint_create(w, PHYS_SLIDER, a, b, p0, ax, 2, 3);
    if (!j) return NULL;
    if (limits) j->type[2] = 2, j->lo[2] = lo, j->hi[2] = hi;
    constraint_register(w, j);
    return j;
}

/* Set Physics Ball Joint (0x100052f0): the 3 translations */
PhysJoint *phys_balljoint_create(PhysWorld *w, PhysBody *a, PhysBody *b, const float anchor[3])
{
    PhysJoint *j = constraint_create(w, PHYS_BALLJOINT, a, b, anchor, NULL, 3, 0);
    if (j) constraint_register(w, j);
    return j;
}

/* detach from the cores (0x10011a60), unlink, free */
void ivp_joint_free(PhysWorld *w, PhysJoint *j)
{
    ivp_controller_detach(w, &j->ctrl, j->cores, j->ncores);
    for (PhysJoint **p = &w->joints; *p; p = &(*p)->next)
        if (*p == j) {
            *p = j->next;
            break;
        }
    free(j);
}

void phys_joint_destroy(PhysWorld *w, PhysJoint *j)
{
    for (PhysJoint *p = w->joints; p; p = p->next)
        if (p == j) {
            ivp_joint_free(w, j);
            return;
        }
}
