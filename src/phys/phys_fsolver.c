/* The multi-contact normal force of a friction system (docs/ivp_contact.md 5, "LS"): the contacts sorted by
   their warm-start key, far ones dropped, the n x n normal-impulse matrix, a warm-started Gauss guess checked
   against the LCP conditions, else IVP_Linear_Constraint_Solver's pivoting (250 steps), the pushes applied
   asynchronously and committed or aborted by the energy test; and the core-reaction helper of the
   tangential friction (LS5).

   The incremental LU of the LCP solver (IVP_Incr_L_U_Matrix, LS1.8 / LS4.5) is replaced by fresh
   partial-pivot eliminations of the active sub-system, as the port note of LS4.5 allows: its validity is the
   decomposition's own test (no pivot below its eps, TEST_EPS * 10), every pivoting decision of solve_lc, the
   deterministic permutations, the epsilons and the step limit are kept. */
#include "phys_contact.h"

#define TEST_EPS IVP_C_TEST_EPS
#define GAUSS_EPS IVP_C_TEST_EPS
#define MAX_ERROR (IVP_C_TEST_EPS * 1000.0)
#define MAX_STEP 10000.0
#define LU_EPS (IVP_C_TEST_EPS * 10.0)

/* ---- IVP_Great_Matrix_Many_Zero::solve_great_matrix_many_zero (LS3.2): A and b are overwritten ---- */
static int gauss_solve(int n, double *A, double *b, double *x, double eps)
{
    for (int p = 0; p < n; p++) {
        int best = -1;                            /* 0x10033e80 */
        double m = fabs(A[p * n + p]);
        for (int r = n - 1; r > p; r--)
            if (fabs(A[r * n + p]) > m) m = fabs(A[r * n + p]), best = r;
        if (best >= 0) {                          /* 0x10033f00 */
            for (int c = 0; c < n; c++) {
                double t = A[p * n + c];
                A[p * n + c] = A[best * n + c], A[best * n + c] = t;
            }
            double t = b[p];
            b[p] = b[best], b[best] = t;
        }
        double d = A[p * n + p];
        if (fabs(d) < eps) continue;
        double inv = -1.0 / d;
        for (int r = p + 1; r < n; r++) {
            double f = A[r * n + p];
            if (fabs(f) > eps) {                  /* 0x10033de0 */
                double s = f * inv;
                for (int c = p; c < n; c++) A[r * n + c] += s * A[p * n + c];
                b[r] += s * b[p];
            }
        }
    }
    for (int i = n - 1; i >= 0; i--) {           /* 0x10034030 */
        double s = b[i];
        for (int c = n - 1; c > i; c--) s -= A[i * n + c] * b[c];
        double d = A[i * n + i];
        if (fabs(d) < eps) {
            if (fabs(s) < eps * 1000) {
                b[i] = 0;
                continue;
            }
            for (int k = 0; k < n; k++) x[k] = 0;
            return 0;
        }
        b[i] = s / d;
    }
    for (int i = 0; i < n; i++) x[i] = b[i];
    return 1;
}

/* the LU's decompose test (0x10035ae0): partial pivoting, a row whose pivot is below eps fails */
static int lu_decompose_ok(int n, double *U)
{
    for (int p = 0; p < n; p++) {
        int best = p;
        double m = fabs(U[p * n + p]);
        for (int r = n - 1; r > p; r--)
            if (fabs(U[r * n + p]) > m) m = fabs(U[r * n + p]), best = r;
        if (best != p)
            for (int c = 0; c < n; c++) {
                double t = U[p * n + c];
                U[p * n + c] = U[best * n + c], U[best * n + c] = t;
            }
        double d = U[p * n + p];
        if (fabs(d) < LU_EPS) return 0;
        for (int r = p + 1; r < n; r++) {
            double f = U[r * n + p] / d;
            if (f != 0)
                for (int c = p; c < n; c++) U[r * n + c] -= f * U[p * n + c];
        }
    }
    return 1;
}

/* ---- IVP_Linear_Constraint_Solver (LS4) ---- */

typedef struct {
    int n;
    const double *A, *b;      /* full_A (stride n), full_b */
    double *x, *delta_f, *accel, *delta_accel, *reset_x, *reset_accel;
    int *aii, *found;
    int r_actives, ignored_pos;
    int p1, p2, q1, q2;       /* +0x68..+0x74 */
    int status;               /* 0 LU valid, 1 invalid (Gauss), 2 failed */
    double *sub, *sb, *sx;    /* scratch for the active sub-system */
} Lcs;

#define AF(L, r, c) ((L)->A[(r) * (L)->n + (c)])

static void swap_pos(Lcs *L, int p, int q)        /* 0x10034f10 */
{
    int a = L->aii[p], b = L->aii[q];
    L->aii[p] = b, L->aii[q] = a;
    L->found[b] = p, L->found[a] = q;
}

static void active_matrix(Lcs *L, int m)
{
    for (int r = 0; r < m; r++)
        for (int c = 0; c < m; c++) L->sub[r * m + c] = AF(L, L->aii[r], L->aii[c]);
}

/* setup_l_u_solver (0x100351c0) */
static int setup_lu(Lcs *L)
{
    active_matrix(L, L->r_actives);
    return lu_decompose_ok(L->r_actives, L->sub);
}

/* lu.solve (0x10035ff0) of the active sub-system: out = A_act^-1 in */
static int lu_solve(Lcs *L, const double *in, double *out)
{
    int m = L->r_actives;
    active_matrix(L, m);
    memcpy(L->sb, in, (size_t)m * sizeof *in);
    return gauss_solve(m, L->sub, L->sb, out, LU_EPS);
}

static void lu_remove(Lcs *L)                     /* 0x10034870 (delete_row_col's own test) */
{
    if (L->status == 0 && !setup_lu(L)) L->status = 2;
}

static void lu_add(Lcs *L)                        /* 0x100348b0 (add_row_col's own test) */
{
    if (L->status == 0 && !setup_lu(L)) L->status = 2;
}

/* 0x100352c0 */
static void recompute_x_accel(Lcs *L)
{
    int n = L->n;
    for (int r = 0; r < n; r++) {                 /* 0x10034270 */
        double s = 0;
        for (int c = 0; c < n; c++) s += AF(L, r, c) * L->reset_x[c];
        L->reset_accel[r] = s;
    }
    for (int i = 0; i < n; i++) L->reset_accel[i] -= L->b[i];
    for (int m = 0; m < L->r_actives; m++) L->reset_accel[L->aii[m]] = 0;
    for (int i = n - 1; i >= 0; i--) L->accel[i] = L->reset_accel[i], L->x[i] = L->reset_x[i];
}

/* 0x10034990 */
static void little_random_permutation(Lcs *L)
{
    if (L->r_actives < 2) return;
    L->p1 = (L->p1 + 1) % L->r_actives;
    L->p2 = (L->p2 + 2) % L->r_actives;
    swap_pos(L, L->p1, L->p2);
    L->q2 += 2, L->q1 += 1;
    int m = L->n - L->ignored_pos - 1;
    if (m >= 2) {
        L->q1 %= m, L->q2 %= m;
        swap_pos(L, L->q1 + 1 + L->ignored_pos, L->q2 + 1 + L->ignored_pos);
    }
}

static void rotate_to_end(Lcs *L, int p)          /* 0x10035680 */
{
    for (int q = p + 1; q < L->n; q++) swap_pos(L, q - 1, q);
}

/* 0x100356b0 */
static int remove_negatives(Lcs *L)
{
    int count = 0;
    for (int m = 0; m < L->r_actives; m++) {
        int v = L->aii[m];
        if (!(L->x[v] < TEST_EPS)) continue;
        if (L->x[v] <= -TEST_EPS) {
            rotate_to_end(L, m);
            count++;
            L->ignored_pos--;
        } else {
            L->x[v] = 0;
            swap_pos(L, m, L->r_actives - 1);
        }
        L->r_actives--;
        L->status = 1;
        m--;
    }
    for (int p = L->r_actives; p < L->ignored_pos; p++)
        if (L->accel[L->aii[p]] < 0) {
            rotate_to_end(L, p);
            p--;
            L->ignored_pos--;
        }
    return count;
}

static void sort_actives_by_x(Lcs *L)             /* 0x10035250 */
{
    for (int i = 1; i < L->r_actives; i++)
        for (int j = i; j > 0 && L->x[L->aii[j]] > L->x[L->aii[j - 1]]; j--) swap_pos(L, j, j - 1);
}

/* 0x100354e0: restart from the current active set */
static int full_setup(Lcs *L)
{
    int k;
    do {
        memset(L->reset_x, 0, (size_t)L->n * sizeof *L->reset_x);
        little_random_permutation(L);
        int m = L->r_actives;
        for (int i = 0; i < m; i++) L->sx[i] = L->b[L->aii[i]];
        if (setup_lu(L) == 1) {
            L->status = 0;
            double *out = L->delta_accel;         /* scratch (rewritten before use) */
            lu_solve(L, L->sx, out);
            for (int i = 0; i < m; i++) L->reset_x[L->aii[i]] = out[i];
        } else {
            L->status = 2;
            sort_actives_by_x(L);
            little_random_permutation(L);
            active_matrix(L, m);
            for (int i = 0; i < m; i++) L->sb[i] = L->b[L->aii[i]];
            double *out = L->delta_accel;
            if (gauss_solve(m, L->sub, L->sb, out, GAUSS_EPS) != 1) return 0;
            for (int i = 0; i < m; i++) L->reset_x[L->aii[i]] = out[i];
        }
        recompute_x_accel(L);
        k = remove_negatives(L);
    } while (k >= 1);
    return 1;
}

/* 0x10035380: the warm start with the first nfirst variables active */
static void start_initialize(Lcs *L, int nfirst)
{
    L->r_actives = L->ignored_pos = nfirst;
    memset(L->reset_x, 0, (size_t)L->n * sizeof *L->reset_x);
    if (setup_lu(L) == 1) {
        L->status = 0;
        for (;;) {
            int m = L->r_actives;
            for (int i = 0; i < m; i++) L->sx[i] = L->b[L->aii[i]];
            double *out = L->delta_accel;
            lu_solve(L, L->sx, out);
            for (int i = 0; i < m; i++) L->reset_x[L->aii[i]] = out[i];
            recompute_x_accel(L);
            int neg = -1;
            for (int i = m - 1; i >= 0 && neg < 0; i--)
                if (L->x[L->aii[i]] < 0.0) neg = i;
            if (neg < 0) return;
            int v = L->aii[neg];
            L->x[v] = 0, L->reset_x[v] = 0;
            L->r_actives--, L->ignored_pos--;
            swap_pos(L, L->r_actives, neg);
            lu_remove(L);
            if (L->status > 0) break;
        }
        memset(L->accel, 0, (size_t)L->n * sizeof *L->accel);
        memset(L->x, 0, (size_t)L->n * sizeof *L->x);
    }
    L->r_actives = L->ignored_pos = 0;
}

/* 0x100343a0 */
static int full_test(Lcs *L)
{
    for (int p = 0; p < L->n; p++) {
        int v = L->aii[p];
        double s = 0;
        for (int c = L->n - 1; c >= 0; c--) s += AF(L, v, c) * L->x[c];   /* 0x10034340 */
        s -= L->b[v];
        if (p < L->r_actives ? fabs(s) > MAX_ERROR : fabs(s - L->accel[v]) > MAX_ERROR) return 0;
    }
    return 1;
}

/* 0x10034ff0 */
static int get_fx_dx(Lcs *L, int ig)
{
    memset(L->delta_f, 0, (size_t)L->n * sizeof *L->delta_f);
    int m = L->r_actives;
    if (m == 0) return 1;
    for (int i = 0; i < m; i++) L->sx[i] = -AF(L, L->aii[i], ig);
    double *out = L->reset_accel;                 /* scratch */
    int ret;
    if (L->status == 0) {
        ret = 1;
        if (!lu_solve(L, L->sx, out)) {
            active_matrix(L, m);
            memcpy(L->sb, L->sx, (size_t)m * sizeof *L->sx);
            ret = gauss_solve(m, L->sub, L->sb, out, GAUSS_EPS);
        }
    } else {
        active_matrix(L, m);
        memcpy(L->sb, L->sx, (size_t)m * sizeof *L->sx);
        ret = gauss_solve(m, L->sub, L->sb, out, GAUSS_EPS);
    }
    for (int i = 0; i < m; i++) L->delta_f[L->aii[i]] = out[i];
    L->delta_f[ig] = 1.0;
    return ret;
}

/* 0x100342c0 */
static void compute_delta_accel(Lcs *L)
{
    int ig = L->aii[L->ignored_pos];
    for (int p = L->r_actives; p < L->n; p++) {
        int v = L->aii[p];
        double s = 0;
        for (int m = 0; m < L->r_actives; m++) s += L->delta_f[L->aii[m]] * AF(L, v, L->aii[m]);
        L->delta_accel[v] = s + AF(L, v, ig);
    }
}

/* 0x10034f40 */
static void update_step(Lcs *L, double s)
{
    for (int i = 0; i < L->n; i++) L->accel[i] += s * L->delta_accel[i], L->x[i] += s * L->delta_f[i];
    for (int p = L->r_actives; p < L->ignored_pos; p++)
        if (L->accel[L->aii[p]] < 0) L->accel[L->aii[p]] = 0;
    for (int p = 0; p < L->r_actives; p++)
        if (L->x[L->aii[p]] < 0) L->x[L->aii[p]] = 0;
}

/* 0x10034a40 */
static void prune_actives(Lcs *L)
{
    for (int m = 0; m < L->r_actives; m++) {
        int v = L->aii[m];
        if (L->x[v] < TEST_EPS) {
            L->x[v] = 0;
            swap_pos(L, m, L->r_actives - 1);
            L->r_actives--;
            lu_remove(L);
            m--;
        }
    }
}

/* 0x10034ab0 solve_lc: 1 solved, 0 failed */
static int solve_lc(Lcs *L)
{
    int n = L->n, steps = 0, zero_steps = 0, did_real = 0, countdown = 7, jpos = 0, ig;
    double eps = TEST_EPS, step = 0;
    double *x = L->x, *accel = L->accel, *df = L->delta_f, *da = L->delta_accel;
    for (;;) {
        if (did_real) steps++;
        if (steps > IVP_C_MAX_LCP_STEPS) return 0;
        if (countdown == 0) {
            if (!full_test(L)) goto restart;
            countdown = 7;
        } else {
            countdown--;
        }
    main:
        if (L->ignored_pos >= n) return 1;
        ig = L->aii[L->ignored_pos];
        if (L->status > 0) {
            if (L->status == 1 && did_real) {
                little_random_permutation(L);
                if (setup_lu(L) == 1) L->status = 0;
            } else {
                L->status = 1;
            }
        }
        if (fabs(accel[ig]) < eps) {
            if (fabs(x[ig]) < eps) goto to_inactive;
            jpos = L->ignored_pos;
            goto ig_to_active;
        }
        if (accel[ig] >= 0) goto to_inactive;
        /* contact ig still approaching: increase x[ig] */
        if (get_fx_dx(L, ig) == 0) goto restart;
        df[ig] = 1.0;
        compute_delta_accel(L);
        for (int m = 0; m < L->r_actives; m++) da[L->aii[m]] = 0;
        if (MAX_STEP * da[ig] <= -accel[ig]) jpos = -1, step = 1e101;
        else jpos = L->ignored_pos, step = -accel[ig] / da[ig];
        for (int m = 0; m < L->r_actives; m++) {
            int v = L->aii[m];
            double d = df[v];
            if (d < -eps) {
                double t = -(x[v] / d);
                if (fabs(t) < eps && x[v] < eps) {
                    jpos = m, step = t;
                    goto step_check;
                }
                if (t < step + eps) step = t, jpos = m;
            }
        }
        for (int m = L->r_actives; m < L->ignored_pos; m++) {
            int v = L->aii[m];
            double d = da[v];
            if (d < -TEST_EPS) {
                double t = -(accel[v] / d);
                if (t < step - eps) step = t, jpos = m;
            }
        }
        if (step < 0) step = 0;
        if (jpos < 0) return 0;                   /* unbounded */
    step_check:
        if (step > MAX_STEP) goto restart;
        if (step < TEST_EPS) {
            if (++zero_steps > (n >> 1) + 2) goto restart;
        } else {
            zero_steps = 0;
        }
        did_real = 1;
        update_step(L, step);
        if (jpos < L->r_actives) {                /* an active reached x = 0: inactive */
            x[L->aii[jpos]] = 0;
            L->r_actives--;
            swap_pos(L, L->r_actives, jpos);
            lu_remove(L);
            continue;
        }
        if (jpos < L->ignored_pos) {              /* an inactive reached accel = 0: active */
            if (step > TEST_EPS) prune_actives(L);
            accel[L->aii[jpos]] = 0;
            swap_pos(L, L->r_actives, jpos);
            L->r_actives++;
            lu_add(L);
            continue;
        }
    ig_to_active:
        prune_actives(L);
        {
            int v = L->aii[jpos];
            accel[v] = 0;
            if (x[v] < 0) x[v] = 0;
        }
        swap_pos(L, L->r_actives, jpos);
        L->ignored_pos++, L->r_actives++;
        if (L->ignored_pos < n) {
            lu_add(L);
            continue;
        }
        countdown = 0;
        continue;
    to_inactive:
        did_real = 0;
        x[ig] = 0;
        if (accel[ig] < 0) accel[ig] = 0;
        L->ignored_pos++;
        if (L->ignored_pos >= n) countdown = 0;
        continue;
    restart:
        steps++;
        if (steps > IVP_C_MAX_LCP_STEPS) return 0;
        zero_steps = 0;
        if (full_setup(L) == 0) return 0;
        countdown = 7;
        goto main;
    }
}

/* 0x100346e0 init_and_solve */
static int lcs_init_and_solve(const double *A, const double *b, double *x, int n, int nfirst)
{
    Lcs L = {0};
    L.n = n, L.A = A, L.b = b, L.x = x;
    double *mem = calloc((size_t)(7 * n + n * n + 2), sizeof *mem);
    L.delta_f = mem, L.accel = mem + n, L.delta_accel = mem + 2 * n, L.reset_x = mem + 3 * n, L.reset_accel = mem + 4 * n;
    L.sb = mem + 5 * n, L.sx = mem + 6 * n, L.sub = mem + 7 * n;
    int *im = calloc((size_t)(2 * n + 1), sizeof *im);
    L.aii = im, L.found = im + n;
    for (int i = 0; i < n; i++) L.aii[i] = i, L.found[i] = i, x[i] = 0, L.accel[i] = -b[i];
    start_initialize(&L, nfirst);
    int r = solve_lc(&L);
    free(mem);
    free(im);
    return r;
}

/* ---- IVP_Friction_Solver (LS2) ---- */

/* 0x10036c60 (+ 0x10036c20): the list sorted ascending by the warm-start key, stable */
static void sort_by_key(IvpFrictionSystem *fs)
{
    uint32_t n = 0;
    for (IvpContactPoint *cp = fs->first; cp; cp = cp->next) n++;
    if (n < 2) return;
    IvpContactPoint **v = malloc(n * sizeof *v);
    n = 0;
    for (IvpContactPoint *cp = fs->first; cp; cp = cp->next) v[n++] = cp;
    for (uint32_t i = 1; i < n; i++)
        for (uint32_t j = i; j > 0 && v[j - 1]->key > v[j]->key; j--) {
            IvpContactPoint *t = v[j];
            v[j] = v[j - 1], v[j - 1] = t;
        }
    for (uint32_t i = 0; i < n; i++) v[i]->prev = i ? v[i - 1] : NULL, v[i]->next = i + 1 < n ? v[i + 1] : NULL;
    fs->first = v[0];
    free(v);
}

/* 0x10036b80 */
static void clear_far(PhysWorld *w, IvpFrictionSystem *fs)
{
    for (IvpContactPoint *cp = fs->first, *next; cp; cp = next) {
        next = cp->next;
        if (cp->gap >= IVP_SET_MAX_DIST_FOR_FRICTION || IVP_CI_LEFT_FEATURE(cp->tmp)) {
            ivp_fs_remove_cp(w, fs, cp);
        } else if (cp->gap > (float)(IVP_SET_DISTANCE_KEEPERS_SAFETY + IVP_SET_FRICTION_DIST) &&
                   ivp_cp_core(cp, 0)->fast_piling && ivp_cp_core(cp, 1)->fast_piling) {
            ivp_fs_unlink(fs, cp);                /* never in Ballance: fast piling is off */
            ivp_fs_push_front(fs, cp);
        }
    }
}

/* 0x10036d10 (with 0x10036cb0): more than 150 contacts, no forces this PSI (unreachable in Ballance) */
static void too_many_contacts(IvpFrictionSystem *fs)
{
    for (uint32_t i = fs->movable.n; i-- > 0;) {
        IvpCore *c = fs->movable.v[i];
        int k = 0;
        for (uint32_t j = 0; j < fs->pairs.n; j++) {
            IvpFrictionPair *p = fs->pairs.v[j];
            if ((p->c[0] == c || p->c[1] == c) && !ivp_core_fixed(p->c[0]) && !ivp_core_fixed(p->c[1])) k++;
        }
        if (k > 1) v3set(c->speed, 0, 0, 0), v3set(c->rot_speed, 0, 0, 0);
    }
}

/* 0x10036ec0: the kinetic energy including the pending pushes */
static double fs_energy(const IvpFrictionSystem *fs)
{
    double e = 0;
    for (uint32_t i = 0; i < fs->cores.n; i++) {
        const IvpCore *c = fs->cores.v[i];
        float v[3], w[3];
        v3add(v, c->speed, c->speed_change);
        v3add(w, c->rot_speed, c->rot_speed_change);
        e += ivp_core_energy(c, v, w);
    }
    return e;
}

/* 0x10036d70 do_friction_system */
void ivp_fs_solve(PhysWorld *w, IvpFrictionSystem *fs, const IvpEventSim *es)
{
    sort_by_key(fs);
    clear_far(w, fs);
    if (fs->n_contacts > IVP_C_MAX_CONTACTS) {
        too_many_contacts(fs);
        return;
    }
    int n = fs->n_contacts;
    IvpContactInfo **inf = malloc((size_t)(n ? n : 1) * sizeof *inf);
    /* 0x100366a0 setup_coords */
    int idx = 0;
    for (IvpContactPoint *cp = fs->first; cp; cp = cp->next) {
        IvpContactInfo *t = cp->tmp;
        t->fi[0] = ivp_core_fi(ivp_cp_core(cp, 0), fs);
        t->fi[1] = ivp_core_fi(ivp_cp_core(cp, 1), fs);
        t->gap = cp->gap;
        t->key = cp->key;
        t->index = (int16_t)idx;
        inf[idx++] = t;
    }
    double *A = calloc((size_t)(n * n + 3 * n + 1), sizeof *A), *b = A + n * n, *x = b + n, *gx = x + n;
    int *wi = malloc((size_t)(n ? n : 1) * sizeof *wi), k = 0;
    /* 0x10036760 build_matrix */
    for (int i = 0; i < n; i++) {
        IvpContactInfo *t = inf[i];
        double vrel = 0.0;
        if (t->core[0]) vrel = ivp_dotd(t->core[0]->rot_speed, t->cross_cs[0]) + ivp_dotd(t->core[0]->speed, t->n);
        if (t->core[1]) vrel -= ivp_dotd(t->core[1]->rot_speed, t->cross_cs[1]) + ivp_dotd(t->core[1]->speed, t->n);
        double d = (double)IVP_SET_FRICTION_DIST - t->gap;
        double factor = d < 0.0 ? 20.0 : 1.0;
        b[i] = factor * d + vrel;
        if (t->key != 0) wi[k++] = i;
        for (int side = 0; side < 2; side++) {
            IvpCore *c = t->core[side];
            if (!c) continue;
            float a[3], bb[3];
            if (side == 0) {
                for (int q = 0; q < 3; q++) a[q] = (t->cross_cs[0][q] * c->inv_rot_inertia[q]) * -1.0f;   /* 0x1001d020, 0x10036b00 */
                float s = (float)(c->inv_mass * -1.0f);
                v3scale(bb, t->n, s);
            } else {
                for (int q = 0; q < 3; q++) a[q] = t->cross_cs[1][q] * c->inv_rot_inertia[q];
                v3scale(bb, t->n, c->inv_mass);
            }
            const IvpFrictionInfo *fi = t->fi[side];
            for (uint32_t m = 0; fi && m < fi->cps.n; m++) {
                const IvpContactInfo *tj = ((const IvpContactPoint *)fi->cps.v[m])->tmp;
                int j = tj->index;
                if (j < 0) continue;
                double s;
                int kk;
                if (tj->core[0] == c) s = -1.0, kk = 0;
                else s = 1.0, kk = 1;
                if (!tj->core[kk]) continue;
                A[j * n + i] += (ivp_dotd(a, tj->cross_cs[kk]) + ivp_dotd(bb, tj->n)) * s;
            }
        }
    }
    /* 0x100362e0 solve_and_push: 0x10036190 scale */
    double maxd = 0.0, maxb = 0.0, scale;
    for (int i = n - 1; i >= 0; i--)
        if (A[i * n + i] > maxd) maxd = A[i * n + i];
    double sA = maxd > IVP_C_1E19 ? 1.0 / maxd : 1.0, sb;
    for (int i = n - 1; i >= 0; i--)
        if (fabs(b[i]) > maxb) maxb = fabs(b[i]);
    if (maxb > IVP_C_1E19) sb = 1.0 / maxb, scale = sA * maxb;
    else sb = 1.0, scale = sA * 1.0;
    for (int i = 0; i < n * n; i++) A[i] *= sA;
    for (int i = 0; i < n; i++) b[i] *= sb;
    /* the warm start: the contacts that pushed last PSI (0x10034120 gather, 0x10033e50 Gauss, 0x10036080) */
    double *M = malloc((size_t)(k * k + k + 1) * sizeof *M), *mb = M + k * k;
    for (int r = 0; r < k; r++) {
        for (int c = 0; c < k; c++) M[r * k + c] = A[wi[r] * n + wi[c]];
        mb[r] = b[wi[r]];
    }
    int ok = gauss_solve(k, M, mb, gx, 1e-9);     /* MATRIX_EPS of the ctor 0x10034100 */
    if (ok) {
        uint8_t *flag = calloc((size_t)(n ? n : 1), 1);
        bool bad = false;
        float g = (float)sqrt((double)w->gravity.g[0] * w->gravity.g[0] + (double)w->gravity.g[1] * w->gravity.g[1] + (double)w->gravity.g[2] * w->gravity.g[2]);
        for (int m = 0; m < k; m++) {
            int i = wi[m];
            if ((double)inf[i]->inv_virt_mass * scale * gx[m] < (double)(g * 0.01f)) bad = true;
            flag[i] = 1;
            x[i] = gx[m];
        }
        for (int i = 0; i < n && ok; i++) {
            if (flag[i]) continue;
            double s = 0;                         /* 0x10033d80 row_ok */
            for (int c = 0; c < n; c++) s += A[i * n + c] * x[c];
            if (!(fabs(b[i] * IVP_C_1E5F) + s >= b[i])) ok = 0;
        }
        if (bad) ok = 0;
        free(flag);
    }
    free(M);
    bool solved = true;
    if (!ok) {
        int nfirst = 0;                           /* 0x100362c0 */
        for (IvpContactPoint *cp = fs->first; cp && cp->key < 0; cp = cp->next) nfirst++;
        if (lcs_init_and_solve(A, b, x, n, nfirst) != 1) solved = false;   /* no impulses, keys and pressures kept */
    }
    if (solved) {
        for (int i = 0; i < n; i++) x[i] *= scale;   /* 0x10036170 */
        double E0 = fs_energy(fs);
        /* 0x100364c0 do_resulting_pushes */
        int i = 0;
        for (IvpContactPoint *cp = fs->first; cp; cp = cp->next, i++) {
            double xi = x[i], p;
            if (xi > 0) cp->key = cp->key >= 0 ? -1 : cp->key - 1;
            else if (xi == 0) {
                cp->key = 0;
                cp->pressure = 0;
                continue;
            } else {
                if (cp->key < 0) cp->key = 0;
                if (++cp->key > 9) cp->key = 0;
            }
            const IvpContactInfo *t = cp->tmp;
            IvpCore *c0 = t->core[0], *c1 = t->core[1];
            if (c0) {
                for (int q = 0; q < 3; q++) c0->rot_speed_change[q] = (float)(c0->rot_speed_change[q] + (double)(t->cross_cs[0][q] * c0->inv_rot_inertia[q]) * -xi);
                double s = -(c0->inv_mass * xi);
                for (int q = 0; q < 3; q++) c0->speed_change[q] = (float)(c0->speed_change[q] + t->n[q] * s);
            }
            if (c1) {
                for (int q = 0; q < 3; q++) c1->rot_speed_change[q] = (float)(c1->rot_speed_change[q] + (double)(t->cross_cs[1][q] * c1->inv_rot_inertia[q]) * xi);
                double s = c1->inv_mass * xi;
                for (int q = 0; q < 3; q++) c1->speed_change[q] = (float)(c1->speed_change[q] + t->n[q] * s);
            }
            p = xi < 0 ? 0.0 : xi;
            cp->pressure = (float)(p * es->i_delta_time);
        }
        double E1 = fs_energy(fs), tol = 0;
        for (uint32_t c = 0; c < fs->cores.n; c++) {   /* 0x10036fc0: |g| hard-coded as 9.81 */
            const IvpCore *core = fs->cores.v[c];
            if (!ivp_core_fixed(core)) tol += (double)(core->mass * IVP_C_981F) * 0.1;
        }
        for (uint32_t c = 0; c < fs->movable.n; c++) {
            IvpCore *core = fs->movable.v[c];
            if (tol + E0 < E1) v3set(core->speed_change, 0, 0, 0), v3set(core->rot_speed_change, 0, 0, 0);   /* 0x1000c5f0 */
            else ivp_commit_pushes(core);         /* 0x1000cbd0 */
        }
    }
    free(A);
    free(wi);
    free(inf);
}

/* ---- IVP_Solver_Core_Reaction (LS5) ---- */

/* 0x10033560 */
static void reaction_add_core(IvpCoreReaction *t, int k, IvpCore *c, const double p_ws[3], float sign)
{
    float r[3];
    for (int i = 0; i < 3; i++) r[i] = (float)(p_ws[i] - c->m_world_f_core.vv[i]);
    int nd = t->dir[1] ? 2 : 1;
    for (int d = 0; d < nd; d++) {
        const float *dir = t->dir[d];
        float x[3], *out = t->cr_out[k][d], *mult = t->cr_mult[k][d];
        v3cross(x, r, dir);
        ivp_rtmul_f(c->m_world_f_core.r, x, out);   /* 0x10033d20 */
        out[3] = 1.0f;
        for (int i = 0; i < 3; i++) mult[i] = out[i] * c->inv_rot_inertia[i];
        mult[3] = c->inv_mass;
        double d4 = (double)out[0] * mult[0] + (double)out[1] * mult[1] + (double)out[2] * mult[2] + (double)out[3] * mult[3];   /* 0x100339b0 */
        if (d == 0) t->m00 += d4;
        else {
            t->m11 += d4;
            t->m01 += ivp_dotd(mult, t->cr_out[k][0]);
        }
        t->dv[d] = (float)(t->dv[d] + sign * (ivp_dotd(c->rot_speed, out) + ivp_dotd(dir, c->speed)));
    }
}

/* 0x10033a30 */
void ivp_core_reaction_init(IvpCoreReaction *t, IvpCore *c0, IvpCore *c1, const double p_ws[3], const float *d0, const float *d1)
{
    memset(t, 0, sizeof *t);
    t->dir[0] = d0, t->dir[1] = d1;
    if (c0) reaction_add_core(t, 0, c0, p_ws, 1.0f);
    if (c1) reaction_add_core(t, 1, c1, p_ws, -1.0f);
}

/* 0x10033ad0: written directly into speed / rot_speed (not async) */
void ivp_core_reaction_push2(const IvpCoreReaction *t, IvpCore *c0, IvpCore *c1, const float imp[2])
{
    IvpCore *cs[2] = {c0, c1};
    for (int k = 0; k < 2; k++) {
        IvpCore *c = cs[k];
        if (!c) continue;
        float sg = k ? -1.0f : 1.0f;
        double im = c->inv_mass;
        for (int d = 0; d < 2; d++) {             /* 0x10010290 */
            double s = (double)(sg * imp[d]) * im;
            for (int i = 0; i < 3; i++) c->speed[i] = (float)(c->speed[i] + t->dir[d][i] * s);
        }
        for (int d = 0; d < 2; d++)
            for (int i = 0; i < 3; i++) c->rot_speed[i] = (float)(c->rot_speed[i] + (double)t->cr_mult[k][d][i] * (sg * imp[d]));
    }
}
