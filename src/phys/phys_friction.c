/* IVP friction systems (docs/ivp_contact.md 4, "FS"): the contact points of a connected group of touching
   cores, grouped by core pair, with their three controllers (2000: contact update and energy easing; 600:
   the tangential friction spring; 0: the normal force, contact removal and the split), creation of contact
   points from mindists (0x10022180), merging and splitting, and the revive cascade (0x1001d4d0). */
#include "phys_contact.h"

#define FS_OF(ctrl, member) ((IvpFrictionSystem *)((char *)(ctrl) - offsetof(IvpFrictionSystem, member)))

/* ---- friction infos (2.8) ---- */

/* 0x1000d960: a movable core's one info when it belongs to fs; an unmovable core's info for fs */
IvpFrictionInfo *ivp_core_fi(const IvpCore *c, const IvpFrictionSystem *fs)
{
    if (ivp_core_fixed(c)) {
        for (uint32_t i = 0; i < c->nfis; i++)
            if (c->fis[i]->fs == fs) return c->fis[i];
        return NULL;
    }
    return c->fi && c->fi->fs == fs ? c->fi : NULL;
}

static IvpFrictionInfo *fi_new(IvpFrictionSystem *fs)
{
    IvpFrictionInfo *fi = calloc(1, sizeof *fi);
    fi->fs = fs;
    return fi;
}

/* 0x1000d9b0 */
static void fi_insert(IvpCore *c, IvpFrictionInfo *fi)
{
    if (ivp_core_fixed(c)) {
        if (c->nfis == c->capfis) c->capfis = c->capfis * 2 + 2, c->fis = realloc(c->fis, c->capfis * sizeof *c->fis);
        c->fis[c->nfis++] = fi;
    } else {
        c->fi = fi;
    }
}

/* 0x1000da50 */
static void fi_remove(IvpCore *c, IvpFrictionInfo *fi)
{
    if (ivp_core_fixed(c)) {
        for (uint32_t i = 0; i < c->nfis; i++)
            if (c->fis[i] == fi) {
                memmove(&c->fis[i], &c->fis[i + 1], (c->nfis - i - 1) * sizeof *c->fis);
                c->nfis--;
                break;
            }
    } else {
        c->fi = NULL;
    }
}

/* 0x1000da80 */
static void fi_delete(IvpCore *c, IvpFrictionInfo *fi)
{
    fi_remove(c, fi);
    ivp_vec_free(&fi->cps);
    free(fi);
}

/* ---- the system object (2.8) ---- */

/* 0x1000b000 */
static IvpFrictionSystem *fs_new(PhysWorld *w)
{
    IvpFrictionSystem *fs = calloc(1, sizeof *fs);
    fs->spring.vt = &ivp_fs_spring_vt;
    fs->normal.vt = &ivp_fs_normal_vt;
    fs->update.vt = &ivp_fs_update_vt;
    fs->env = w;
    fs->wnext = w->fs_list;
    if (w->fs_list) w->fs_list->wprev = fs;
    w->fs_list = fs;
    return fs;
}

/* 0x1000b100 (through vtable +0x18) */
void ivp_fs_delete(PhysWorld *w, IvpFrictionSystem *fs)
{
    if (fs->wprev) fs->wprev->wnext = fs->wnext;
    else w->fs_list = fs->wnext;
    if (fs->wnext) fs->wnext->wprev = fs->wprev;
    for (uint32_t i = 0; i < fs->pairs.n; i++) {
        IvpFrictionPair *p = fs->pairs.v[i];
        ivp_vec_free(&p->cps);
        free(p);
    }
    ivp_vec_free(&fs->cores);
    ivp_vec_free(&fs->movable);
    ivp_vec_free(&fs->pairs);
    free(fs);
}

/* 0x1000b390: a movable core gets the three controllers (fs+8, fs, fs+0x10) in its sim unit */
static void fs_add_core(IvpFrictionSystem *fs, IvpCore *c)
{
    ivp_vec_add(&fs->cores, c);
    if (!ivp_core_fixed(c)) {
        ivp_vec_add(&fs->movable, c);
        ivp_core_add_controller(c, &fs->normal);   /* 0x10011c70 */
        ivp_core_add_controller(c, &fs->spring);
        ivp_core_add_controller(c, &fs->update);
    }
    fs->n_cores++;
}

/* 0x1000b410 */
static void fs_remove_core(IvpFrictionSystem *fs, IvpCore *c)
{
    ivp_vec_remove(&fs->cores, c);
    if (!ivp_core_fixed(c)) {
        ivp_vec_remove(&fs->movable, c);
        ivp_core_remove_controller(c, &fs->update);   /* 0x10011c00 */
        ivp_core_remove_controller(c, &fs->spring);
        ivp_core_remove_controller(c, &fs->normal);
    }
    fs->n_cores--;
}

/* 0x1000b360: cp.fs = fs, at the head of the list */
void ivp_fs_push_front(IvpFrictionSystem *fs, IvpContactPoint *cp)
{
    cp->fs = fs;
    cp->prev = NULL;
    cp->next = fs->first;
    if (fs->first) fs->first->prev = cp;
    fs->first = cp;
    fs->n_contacts++;
}

/* 0x1000b290 (cp.fs is kept) */
void ivp_fs_unlink(IvpFrictionSystem *fs, IvpContactPoint *cp)
{
    if (cp->prev) cp->prev->next = cp->next;
    else fs->first = cp->next;
    if (cp->next) cp->next->prev = cp->prev;
    cp->next = cp->prev = NULL;
    fs->n_contacts--;
}

/* ---- core pairs (2.8) ---- */

/* 0x1001d380: either order, searched from the end */
IvpFrictionPair *ivp_fs_find_pair(const IvpFrictionSystem *fs, const IvpCore *a, const IvpCore *b)
{
    for (uint32_t i = fs->pairs.n; i-- > 0;) {
        IvpFrictionPair *p = fs->pairs.v[i];
        if ((p->c[0] == a && p->c[1] == b) || (p->c[0] == b && p->c[1] == a)) return p;
    }
    return NULL;
}

/* 0x1000b2c0 with 0x1001d340 */
static void fs_add_pair(IvpFrictionSystem *fs, IvpContactPoint *cp)
{
    IvpCore *a = ivp_cp_core(cp, 0), *b = ivp_cp_core(cp, 1);
    IvpFrictionPair *p = ivp_fs_find_pair(fs, a, b);
    if (!p) {
        p = calloc(1, sizeof *p);
        p->next_ease = 1;
        p->last_impact = -1000.0;
        p->c[0] = a, p->c[1] = b;
        ivp_vec_add(&fs->pairs, p);
    }
    ivp_vec_add(&p->cps, cp);
}

/* 0x1000b210: true when the pair died */
static bool fs_pair_remove(IvpFrictionSystem *fs, IvpContactPoint *cp)
{
    IvpFrictionPair *p = ivp_fs_find_pair(fs, ivp_cp_core(cp, 0), ivp_cp_core(cp, 1));
    if (!p) return false;
    ivp_vec_remove(&p->cps, cp);
    if (p->cps.n) return false;
    ivp_vec_remove(&fs->pairs, p);
    ivp_vec_free(&p->cps);
    free(p);
    return true;
}

/* 0x1001c460 delete_friction_distance */
void ivp_fs_remove_cp(PhysWorld *w, IvpFrictionSystem *fs, IvpContactPoint *cp)
{
    IvpCore *c[2] = {ivp_cp_core(cp, 0), ivp_cp_core(cp, 1)};
    ivp_reset_freeze_check(w, c[0]);              /* 0x1000cec0 */
    ivp_reset_freeze_check(w, c[1]);
    ivp_fs_unlink(fs, cp);
    if (fs_pair_remove(fs, cp)) fs->uf_needed = true;
    for (int k = 0; k < 2; k++) {
        IvpFrictionInfo *fi = ivp_core_fi(c[k], fs);
        if (!fi) continue;
        ivp_vec_remove(&fi->cps, cp);
        if (fi->cps.n) continue;
        fi_delete(c[k], fi);
        fs_remove_core(fs, c[k]);
        if (c[k]->unit) c[k]->unit->changed = true;
    }
    ivp_cp_destroy(w, cp);                        /* 0x1001c230 */
}

/* 0x1001cc20: the pair's contacts recomputed at this time (impact pre-pass), those that left their feature
   deleted; returns the count left */
int ivp_fs_recalc_pair(PhysWorld *w, IvpFrictionPair *p, IvpFrictionSystem *fs)
{
    int n = (int)p->cps.n;
    for (int i = n - 1; i >= 0; i--) {
        IvpContactPoint *cp = p->cps.v[i];
        ivp_cp_update(w, cp);
        ivp_cp_read_materials(w, cp);
        if (IVP_CI_LEFT_FEATURE(cp->tmp)) {
            n--;
            ivp_fs_remove_cp(w, fs, cp);
        }
    }
    return n;
}

/* ---- merge (FS7.1) ---- */

/* 0x1001c570: src's contacts and cores go to dst; src is deleted */
static void fs_merge(PhysWorld *w, IvpFrictionSystem *dst, IvpFrictionSystem *src)
{
    while (src->first) {
        IvpContactPoint *cp = src->first;
        ivp_fs_unlink(src, cp);
        fs_pair_remove(src, cp);
        ivp_fs_push_front(dst, cp);
        fs_add_pair(dst, cp);
    }
    for (uint32_t i = src->cores.n; i-- > 0;) {
        IvpCore *c = src->cores.v[i];
        IvpFrictionInfo *fs_i = ivp_core_fi(c, src), *fd = ivp_core_fi(c, dst);
        if (!fd) {
            fs_i->fs = dst;                       /* re-keyed */
            fs_remove_core(src, c);
            fs_add_core(dst, c);
        } else {
            for (uint32_t k = 0; k < fs_i->cps.n; k++) ivp_vec_add(&fd->cps, fs_i->cps.v[k]);
            fs_i->cps.n = 0;
            fi_delete(c, fs_i);
            fs_remove_core(src, c);
        }
    }
    ivp_fs_delete(w, src);
}

/* ---- creation (CP3.3 / IM11) ---- */

/* 0x10022180 try_to_generate_managed_friction */
IvpContactPoint *ivp_try_generate_friction(PhysWorld *w, IvpMindist *md, IvpFrictionSystem **out_fs, bool *created,
                                           IvpSimUnit *keep, bool update)
{
    PhysBody *oA = ivp_md_sorted(md, 0)->obj, *oB = ivp_md_sorted(md, 1)->obj;
    IvpCore *cM = &oA->core, *cO = &oB->core;
    if (ivp_core_fixed(cM)) {
        IvpCore *x = cM;
        cM = cO, cO = x;
    }
    IvpFrictionInfo *fiM = cM->fi;                /* 0x1000d9a0 */
    bool is_new;
    IvpContactPoint *cp = ivp_cp_find_or_create(w, md, &is_new);
    if (!is_new) {
        *out_fs = fiM ? fiM->fs : cp->fs;
        *created = false;
        if (update) {
            ivp_cp_update(w, cp);
            ivp_cp_read_materials(w, cp);
        }
        return cp;
    }
    if (update) {
        ivp_cp_update(w, cp);
        ivp_cp_read_materials(w, cp);
    }
    /* "friction created": the environment's listeners (0x10013bc0), then the objects' (0x1000a8a0) */
    if (w->contact_fn) w->contact_fn(w->listen_user, cp->syn[0].obj, cp->syn[1].obj, true);
    *created = true;
    IvpFrictionSystem *fs;
    IvpFrictionInfo *fiO;
    if (fiM) {
        fs = fiM->fs;
        fiO = ivp_core_fi(cO, fs);
        if (!fiO) {
            IvpFrictionInfo *f2 = ivp_core_fixed(cO) ? NULL : cO->fi;
            if (f2) {
                fs_merge(w, fs, f2->fs);
                fiO = f2;
            } else {
                fiO = fi_new(fs);
                fi_insert(cO, fiO);
                fs_add_core(fs, cO);
            }
        }
        ivp_fs_push_front(fs, cp);
        fs_add_pair(fs, cp);
    } else {
        fiM = fi_new(NULL);
        IvpFrictionInfo *f2 = ivp_core_fixed(cO) ? NULL : cO->fi;
        if (f2) {
            fs = f2->fs;
            fiO = f2;
            fiM->fs = fs;
            fi_insert(cM, fiM);
            fs_add_core(fs, cM);
            ivp_fs_push_front(fs, cp);
            fs_add_pair(fs, cp);
        } else {
            fs = fs_new(w);
            fiO = fi_new(fs);
            fiM->fs = fs;
            fi_insert(cO, fiO);
            fi_insert(cM, fiM);
            ivp_fs_push_front(fs, cp);
            fs_add_pair(fs, cp);
            fs_add_core(fs, cM);
            fs_add_core(fs, cO);
        }
    }
    ivp_vec_add(&fiM->cps, cp);                   /* 0x1003aee0 */
    ivp_vec_add(&fiO->cps, cp);
    *out_fs = fs;
    ivp_cp_init_constants(cp);                    /* 0x1001d3d0 */
    /* two movable cores in different units: one unit absorbs the other (0x10011770, 0x10011890) */
    if (!ivp_core_fixed(cM) && !ivp_core_fixed(cO) && cM->unit != cO->unit) {
        if (cO->unit == keep) ivp_sim_unit_merge(w, cO->unit, cM->unit);
        else ivp_sim_unit_merge(w, cM->unit, cO->unit);
    }
    return cp;
}

/* ---- removal with an object (CP6) ---- */

/* 0x10009a40: every contact of the object; unless keep_asleep, both friction cores are revived first
   (0x1000dab0); an emptied system is deleted */
void ivp_object_remove_contacts(PhysWorld *w, PhysBody *b, bool keep_asleep)
{
    while (b->fric_syn) {
        IvpContactPoint *cp = b->fric_syn->cp;
        IvpFrictionSystem *fs = cp->fs;
        if (!keep_asleep) {
            ivp_ensure_in_simulation(w, ivp_cp_core(cp, 0));
            ivp_ensure_in_simulation(w, ivp_cp_core(cp, 1));
        }
        ivp_fs_remove_cp(w, fs, cp);
        if (fs->n_contacts == 0) ivp_fs_delete(w, fs);
    }
}

/* ---- revive (FS7.4) ---- */

/* 0x1001d4d0: contacts with the resting movable neighbours that have none; 1 when one was made (the
   revive restarts: the units merged) */
int ivp_fs_revive_core(PhysWorld *w, IvpCore *c)
{
    PhysBody *o = c->body;
    for (IvpSynapse *s = o->coll.exact_syn, *next; s; s = next) {
        next = s->next;
        IvpMindist *md = s->md;
        if (md->flags & IVP_MDF_FUNCTION) continue;
        IvpSynapse *other = s == &md->syn[0] ? &md->syn[1] : &md->syn[0];
        IvpCore *oc = &other->obj->core;
        if (ivp_core_fixed(oc)) continue;         /* (a fixed reviving core would delete the mindist) */
        if (oc->fi) continue;
        ivp_recalc_mindist(w, md);                /* 0x10019950 */
        if (md->flags & IVP_MDF_RECALC) continue;
        if (!(md->len_numerator < IVP_SET_MAX_DIST_FOR_FRICTION)) continue;
        IvpFrictionSystem *fs;
        bool created;
        ivp_try_generate_friction(w, md, &fs, &created, c->unit, true);
        if (created) {
            ivp_reset_freeze_check(w, oc);
            return 1;
        }
    }
    return 0;
}

/* ---- the kinetic energy and point velocity helpers (2.3) ---- */

/* 0x1000c480 */
double ivp_core_energy(const IvpCore *c, const float v[3], const float w[3])
{
    const float *I = c->rot_inertia;
    return 0.5 * ((double)c->mass * ivp_dotd(v, v) + (double)I[0] * w[0] * w[0] + (double)I[1] * w[1] * w[1] + (double)I[2] * w[2] * w[2]);
}

/* ---- the tangential friction (FS4) ---- */

/* 0x1001b080 friction_force_local_constraint_2d: drives the tangential relative velocity to s / dt within
   mu N dt; returns the change of the spring energy */
static float friction_2d(IvpContactPoint *cp, const IvpEventSim *es)
{
    IvpContactInfo *t = cp->tmp;
    double maxI = (double)cp->mu * cp->pressure * es->delta_time;
    if (maxI < IVP_C_1E6F) return 0.0f;
    IvpCore *c0 = t->core[0], *c1 = t->core[1];
    assert(!(c0 && c0->car_wheel && !c1));        /* the car-wheel branch (unreachable) */
    IvpCoreReaction tcb;
    ivp_core_reaction_init(&tcb, c0, c1, t->cp_ws, t->span[0], t->span[1]);
    double a = cp->s[0] * es->i_delta_time - tcb.dv[0], b = cp->s[1] * es->i_delta_time - tcb.dv[1];
    double det = tcb.m00 * tcb.m11 - tcb.m01 * tcb.m01;
    if (det * det < IVP_C_1E38) return 0.0f;
    double inv = 1 / det;
    double c00 = tcb.m11 * inv, c01 = -tcb.m01 * inv, c11 = inv * tcb.m00;
    float imp[2] = {(float)(c00 * a + c01 * b), (float)(c01 * a + c11 * b)};
    double q = (double)imp[0] * imp[0] + (double)imp[1] * imp[1];
    if (q > maxI * maxI) {
        double k = (1.0 / sqrt((double)(float)q)) * maxI;   /* 0x1000dae0 */
        imp[0] = (float)(imp[0] * k), imp[1] = (float)(imp[1] * k);
    }
    ivp_core_reaction_push2(&tcb, c0, c1, imp);   /* 0x10033ad0: speeds written directly */
    double s2 = (double)cp->s[0] * cp->s[0] + (double)cp->s[1] * cp->s[1];
    double E = sqrt(es->delta_time * es->delta_time * q * s2) * 0.5;   /* q before the clamp (quirk) */
    float d = (float)(E - cp->old_energy);
    cp->old_energy = (float)E;
    return d;
}

/* the slip clamp of 0x1001d750 / 0x1001bc20: |s| beyond t slips (destroying energy) */
static void clamp_spring(IvpContactPoint *cp, float t, double th)
{
    double s2 = (double)cp->s[0] * cp->s[0] + (double)cp->s[1] * cp->s[1];
    if (!(s2 > th)) return;
    double r = 1.0 / sqrt((double)(float)s2);
    cp->destroyed = (float)((r * s2 - t) * cp->pressure * cp->mu + cp->destroyed);
    cp->recheck = 1;
    double k = r * t;
    cp->s[0] = (float)(cp->s[0] * k), cp->s[1] = (float)(cp->s[1] * k);
}

/* 0x1001bdf0: the springs of two coplanar contacts of a pair equalized along the line joining them */
static void ease_two(IvpContactPoint *A, IvpContactPoint *B, float dA[3], float dB[3], double f)
{
    float sg = ivp_cp_core(A, 0) == ivp_cp_core(B, 0) ? 1.0f : -1.0f, ff = (float)f;
    const IvpContactInfo *ta = A->tmp, *tb = B->tmp;
    float SA[3], SB[3], u[3];
    for (int k = 0; k < 3; k++) {
        SA[k] = A->s[0] * ta->span[0][k] + A->s[1] * ta->span[1][k];
        SB[k] = B->s[0] * tb->span[0][k] + B->s[1] * tb->span[1][k];
        u[k] = (float)(ta->cp_ws[k] - tb->cp_ws[k]);
    }
    double l2 = ivp_dotd(u, u);
    if (l2 >= IVP_C_1E19) {                        /* 0x1000e030 */
        double y = 1.0 / sqrt(l2);
        for (int k = 0; k < 3; k++) u[k] = (float)(u[k] * y);
    }
    float ua = ivp_dotf(u, SA), ub = ivp_dotf(u, SB) * sg;
    for (int k = 0; k < 3; k++) {
        float pA = u[k] * ua, pB = u[k] * ub, avg = (pA + pB) * 0.5f;
        dA[k] = dA[k] + (avg - pA) * ff;
        dB[k] = dB[k] + (avg - pB) * (sg * ff);
    }
}

/* 0x1001c070 */
static void ease_pair(IvpFrictionPair *p)
{
    uint32_t n = p->cps.n;
    IvpContactPoint **cps = malloc(n * sizeof *cps);
    float (*d)[3] = calloc(n, sizeof *d);
    memcpy(cps, p->cps.v, n * sizeof *cps);
    double f = 1.0 / (n + 1e-19);
    for (uint32_t i = 0; i + 1 < n; i++)
        for (uint32_t j = i + 1; j < n; j++) {
            double c = ivp_dotd(cps[i]->tmp->n, cps[j]->tmp->n);
            if (fabs(fabs(c) - 1.0) < IVP_C_001F) ease_two(cps[j], cps[i], d[j], d[i], f);
        }
    for (uint32_t k = 0; k < n; k++) {             /* 0x1001bd90 */
        cps[k]->s[0] = cps[k]->s[0] + ivp_dotf(cps[k]->tmp->span[0], d[k]);
        cps[k]->s[1] = cps[k]->s[1] + ivp_dotf(cps[k]->tmp->span[1], d[k]);
    }
    free(cps);
    free(d);
}

/* 0x1001d750: priority 600 */
static void fs_spring_simulate(IvpController *ctrl, const IvpEventSim *es, IvpCore **cores, uint32_t ncores)
{
    (void)cores, (void)ncores;
    IvpFrictionSystem *fs = FS_OF(ctrl, spring);
    double dt = es->delta_time;
    if (fs->n_contacts > 1) {
        /* 0x1001c550 = 0x1001bc20 (all pairs) + 0x1001c1f0 (spring easing) */
        for (uint32_t i = fs->pairs.n; i-- > 0;) {
            IvpFrictionPair *p = fs->pairs.v[i];
            double sum = 0;
            for (uint32_t k = p->cps.n; k-- > 0;) {
                IvpContactPoint *cp = p->cps.v[k];
                sum += (double)cp->pressure * cp->mu * cp->inv_vm_no_dir;
            }
            float t = (float)(sum * dt * dt);     /* shared by the pair's contacts */
            if (!p->cps.n) continue;
            float th = (float)((double)t * t + 1e-6f);
            float e = 0.0f;
            for (uint32_t k = p->cps.n; k-- > 0;) {
                IvpContactPoint *cp = p->cps.v[k];
                clamp_spring(cp, t, th);
                assert(!cp->two_friction);        /* 0x1001b8b0 (unreachable) */
                e = (float)(e + friction_2d(cp, es));
            }
            if (e > 0) p->anti_energy = (float)(e + p->anti_energy);
        }
        for (uint32_t i = fs->pairs.n; i-- > 0;) {
            IvpFrictionPair *p = fs->pairs.v[i];
            if (--p->next_ease == 0) {
                ease_pair(p);
                p->next_ease = 5;
            }
        }
        return;
    }
    IvpContactPoint *cp = fs->first;
    float t = (float)((double)cp->inv_vm_no_dir * cp->pressure * cp->mu * dt * dt);
    clamp_spring(cp, t, (double)t * t + 1e-6f);
    assert(!cp->two_friction);
    friction_2d(cp, es);                          /* the energy is discarded */
}

/* ---- the energy easing (FS6.2) ---- */

typedef struct {
    float dv[3];                      /* +0x00 */
    float ax0[3], ax1[3];             /* +0x10 / +0x20 */
    double vlen;                      /* +0x30 */
    double m0, m1, im0, im1;          /* +0x38 .. +0x50 */
    double wlen;                      /* +0x58 */
    double Ir0, Ir1, iIr0, iIr1;      /* +0x60 .. +0x78 */
    double Er, El, Etot;              /* +0x80 / +0x88 / +0x98 */
    IvpCore *c0, *c1;                 /* +0x90 / +0x94 */
} RelMotion;

static double fast_normize(float v[3])            /* 0x1000df30 */
{
    double n = ivp_dotd(v, v);
    if (n < IVP_C_1E19) return 0;
    double y = 1.0 / sqrt(n);
    for (int k = 0; k < 3; k++) v[k] = (float)(v[k] * y);
    return n * y;
}

static double len3(const float v[3]) { return sqrt(ivp_dotd(v, v)); }   /* 0x1000e480 */

/* 0x1001cd60 (not the textbook form) */
static double rel_energy(double v, double m0, double m1, double im0, double im1)
{
    double a = v / (im0 + im1), u = v - a * im1, U = u * u * m1, w = a * im0;
    return ((v * v * m1 - w * w * m0) - U) * 0.5;
}

/* 0x1001cd20 */
static double rel_impulse(double v, double i0, double i1, double E) { return (v - sqrt(fabs(v * v - 2 * E * (i0 + i1)))) / (i0 + i1); }

/* 0x1001cdb0 + 0x1001d060 */
static void rel_setup(RelMotion *R, IvpCore *a, IvpCore *b)
{
    if (ivp_core_fixed(b)) R->c0 = b, R->c1 = a;
    else R->c0 = a, R->c1 = b;
    IvpCore *c0 = R->c0, *c1 = R->c1;
    v3sub(R->dv, c1->speed, c0->speed);
    R->vlen = fast_normize(R->dv);
    float w1[3], w0[3], w[3], mw[3];
    ivp_rmul_f(c1->m_world_f_core.r, c1->rot_speed, w1);
    ivp_rmul_f(c0->m_world_f_core.r, c0->rot_speed, w0);
    v3sub(w, w1, w0);
    R->wlen = fast_normize(w);
    ivp_rtmul_f(c0->m_world_f_core.r, w, R->ax0);
    for (int k = 0; k < 3; k++) mw[k] = (float)(w[k] * -1.0);
    ivp_rtmul_f(c1->m_world_f_core.r, mw, R->ax1);
    float t[3];
    for (int k = 0; k < 3; k++) t[k] = c0->rot_inertia[k] * R->ax0[k];
    R->Ir0 = len3(t);
    if (R->Ir0 < IVP_C_1E19) R->Ir0 = 1.0, R->iIr0 = 1.0;
    else R->iIr0 = 1 / R->Ir0;
    for (int k = 0; k < 3; k++) t[k] = R->ax1[k] * c1->rot_inertia[k];
    R->Ir1 = len3(t);
    if (R->Ir1 < IVP_C_1E19) R->Ir1 = 1.0, R->iIr1 = 1.0;
    else R->iIr1 = 1 / R->Ir1;
    R->m1 = c1->mass, R->im1 = c1->inv_mass;
    if (ivp_core_fixed(c0)) {
        R->m0 = R->m1 * 10000.0, R->im0 = R->im1 * 1e-4;
        R->Ir0 = R->Ir1 * 10000.0, R->iIr0 = R->iIr1 * 1e-4;
    } else {
        R->m0 = c0->mass, R->im0 = c0->inv_mass;
    }
    R->Er = rel_energy(R->wlen, R->Ir0, R->Ir1, R->iIr0, R->iIr1);
    R->El = rel_energy(R->vlen, R->m0, R->m1, R->im0, R->im1);
    R->Etot = R->El + R->Er;
}

/* 0x1001d0e0: the fraction f of the relative energy removed through the pending velocity changes */
static void rel_remove(RelMotion *R, double f)
{
    double iR = rel_impulse(R->wlen, R->iIr0, R->iIr1, f * R->Er);
    double iL = rel_impulse(R->vlen, R->im0, R->im1, f * R->El);
    IvpCore *c0 = R->c0, *c1 = R->c1;
    if (!ivp_core_fixed(c0)) {
        for (int k = 0; k < 3; k++) c0->speed_change[k] = (float)(c0->speed_change[k] + R->dv[k] * (iL * R->im0));
        for (int k = 0; k < 3; k++) c0->rot_speed_change[k] = (float)(c0->rot_speed_change[k] + R->ax0[k] * (iR * R->iIr0));
    }
    for (int k = 0; k < 3; k++) R->dv[k] = (float)(R->dv[k] * -1.0);
    for (int k = 0; k < 3; k++) c1->speed_change[k] = (float)(c1->speed_change[k] + R->dv[k] * (iL * R->im1));
    for (int k = 0; k < 3; k++) c1->rot_speed_change[k] = (float)(c1->rot_speed_change[k] + R->ax1[k] * (iR * R->iIr1));
}

/* 0x1001ccd0 with 0x1001d2a0: at most 10% of the pair's relative energy per PSI */
static void ease_energy(PhysWorld *w, IvpFrictionPair *p)
{
    double e = pow(0.9, w->delta_psi) * p->anti_energy;   /* env +0x148 */
    p->anti_energy = (float)e;
    if (!(e > 0)) return;
    RelMotion R;
    rel_setup(&R, p->c[0], p->c[1]);
    if (e > R.Etot * IVP_C_01F) e = R.Etot * IVP_C_01F;
    double d = 0;
    if (!(R.Etot < IVP_C_1E19)) {
        rel_remove(&R, e / R.Etot);
        d = e;
    }
    p->anti_energy = (float)(p->anti_energy - d);
}

/* ---- the contact update (FS5.1) ---- */

/* 0x1001d610: priority 2000 */
static void fs_update_simulate(IvpController *ctrl, const IvpEventSim *es, IvpCore **cores, uint32_t ncores)
{
    (void)cores, (void)ncores;
    IvpFrictionSystem *fs = FS_OF(ctrl, update);
    PhysWorld *w = fs->env;
    if (fs->n_contacts <= 1) {
        ivp_cp_update(w, fs->first);
        return;
    }
    for (uint32_t i = fs->pairs.n; i-- > 0;) {    /* 0x1000b190 */
        IvpFrictionPair *p = fs->pairs.v[i];
        float e = 0.0f;
        for (uint32_t k = p->cps.n; k-- > 0;) {
            IvpContactPoint *cp = p->cps.v[k];
            float g_old = cp->gap;
            ivp_cp_update(w, cp);
            e = (float)(((double)g_old - cp->gap) * cp->pressure + e);
        }
        if (e > 0) p->anti_energy = (float)(e + p->anti_energy);
    }
    const IvpSimUnit *su = es->sim_unit;
    if (su->fast_prev)                            /* flags 0x3000 (0x1001ccb0) */
        for (uint32_t i = fs->pairs.n; i-- > 0;) ((IvpFrictionPair *)fs->pairs.v[i])->anti_energy = 0;
    if (!su->fast)                                /* (flags & 0xc00) == 0 (0x1001cc90) */
        for (uint32_t i = fs->pairs.n; i-- > 0;) ease_energy(w, fs->pairs.v[i]);
}

/* ---- the normal force (FS5.3-5.4) ---- */

/* 0x1001ae40: one impulse driving the approach speed to -k (desired - gap), 20 k when too far; the gap error
   is used directly as a speed (no 1 / dt) and the speeds are written directly */
static void normal_single(IvpContactPoint *cp, const IvpEventSim *es, float desired, float k)
{
    const IvpContactInfo *t = cp->tmp;
    IvpCore *c0 = t->core[0], *c1 = t->core[1];
    double vr = 0.0;
    if (c0) vr = ivp_dotd(t->cross_cs[0], c0->rot_speed) + ivp_dotd(c0->speed, t->n);
    if (c1) vr = vr - (ivp_dotd(c1->rot_speed, t->cross_cs[1]) + ivp_dotd(c1->speed, t->n));
    double x = (double)desired - cp->gap, kk = k;
    if (x < 0) kk = kk * 20.0;                    /* 0x10063388 */
    double imp = (kk * x + vr) * t->virt_mass;
    if (imp <= 0) {
        cp->pressure = 0;
        return;
    }
    cp->pressure = (float)(imp * es->i_delta_time);
    if (c0) {
        for (int i = 0; i < 3; i++) c0->rot_speed[i] = (float)(c0->rot_speed[i] + (double)(t->cross_cs[0][i] * c0->inv_rot_inertia[i]) * -imp);
        double s = -(c0->inv_mass * imp);
        for (int i = 0; i < 3; i++) c0->speed[i] = (float)(c0->speed[i] + t->n[i] * s);
    }
    if (c1) {
        for (int i = 0; i < 3; i++) c1->rot_speed[i] = (float)(c1->rot_speed[i] + (double)(t->cross_cs[1][i] * c1->inv_rot_inertia[i]) * imp);
        double s = c1->inv_mass * imp;
        for (int i = 0; i < 3; i++) c1->speed[i] = (float)(c1->speed[i] + t->n[i] * s);
    }
}

/* ---- union-find split (FS7.2-7.3) ---- */

static IvpCore *uf_root(IvpCore *c)               /* 0x1001c7d0 */
{
    while (c->uf_parent) c = c->uf_parent;
    return c;
}

/* 0x1001c7f0: a root of a component other than the lowest-indexed movable core's, or NULL */
static IvpCore *fs_union_find(IvpFrictionSystem *fs)
{
    for (uint32_t i = 0; i < fs->cores.n; i++) ((IvpCore *)fs->cores.v[i])->uf_parent = NULL;
    for (uint32_t i = fs->pairs.n; i-- > 0;) {
        IvpFrictionPair *p = fs->pairs.v[i];
        if (ivp_core_fixed(p->c[0]) || ivp_core_fixed(p->c[1])) continue;
        IvpCore *r0 = uf_root(p->c[0]), *r1 = uf_root(p->c[1]);
        if (r0 != r1) r1->uf_parent = r0;
    }
    IvpCore *ref = NULL;
    for (uint32_t i = 0; i < fs->cores.n && !ref; i++)
        if (!ivp_core_fixed(fs->cores.v[i])) ref = uf_root(fs->cores.v[i]);
    IvpCore *ret = NULL;
    for (uint32_t i = fs->movable.n; i-- > 0;) {
        IvpCore *r = uf_root(fs->movable.v[i]);
        if (r != ref) ret = r;
    }
    return ret;
}

static void fs_drop_all(PhysWorld *w, IvpFrictionSystem *fs)
{
    while (fs->cores.n) {
        IvpCore *c = fs->cores.v[0];
        IvpFrictionInfo *fi = ivp_core_fi(c, fs);
        if (fi) fi_delete(c, fi);
        fs_remove_core(fs, c);
    }
    ivp_fs_delete(w, fs);
}

/* 0x1001c8a0: the components other than the first move to new systems */
static void fs_split(PhysWorld *w, IvpFrictionSystem *fs, IvpCore *root)
{
    for (;;) {
        IvpFrictionSystem *nw = fs_new(w);
        for (uint32_t i = fs->cores.n; i-- > 0;) {
            IvpCore *c = fs->cores.v[i];
            if (!ivp_core_fixed(c)) {
                if (uf_root(c) != root) continue;
                fs_remove_core(fs, c);
                fs_add_core(nw, c);
                c->fi->fs = nw;
            } else {
                fi_insert(c, fi_new(nw));
                fs_add_core(nw, c);
            }
        }
        for (uint32_t i = fs->pairs.n; i-- > 0;) {
            IvpFrictionPair *p = fs->pairs.v[i];
            IvpCore *mc = ivp_core_fixed(p->c[0]) ? p->c[1] : p->c[0];
            if (uf_root(mc) != root) continue;
            IvpCore *fx = ivp_core_fixed(p->c[0]) ? p->c[0] : ivp_core_fixed(p->c[1]) ? p->c[1] : NULL;
            IvpFrictionInfo *fi_old = fx ? ivp_core_fi(fx, fs) : NULL, *fi_nw = fx ? ivp_core_fi(fx, nw) : NULL;
            ivp_vec_remove_at(&fs->pairs, i);
            ivp_vec_add(&nw->pairs, p);
            for (uint32_t k = 0; k < p->cps.n; k++) {
                IvpContactPoint *cp = p->cps.v[k];
                assert(cp->fs == fs);             /* else *0 = 0 */
                ivp_fs_unlink(fs, cp);
                ivp_fs_push_front(nw, cp);
                if (fx) {
                    ivp_vec_remove(&fi_old->cps, cp);
                    ivp_vec_add(&fi_nw->cps, cp);
                }
            }
        }
        for (uint32_t i = fs->cores.n; i-- > 0;) {
            IvpCore *c = fs->cores.v[i];
            if (!ivp_core_fixed(c)) continue;
            IvpFrictionInfo *fn = ivp_core_fi(c, nw), *fo = ivp_core_fi(c, fs);
            if (fn && !fn->cps.n) fi_delete(c, fn), fs_remove_core(nw, c);
            if (fo && !fo->cps.n) fi_delete(c, fo), fs_remove_core(fs, c);
        }
        if (nw->n_cores < 2) {
            fs_drop_all(w, nw);
            return;
        }
        if (fs->n_cores < 2) {
            fs_drop_all(w, fs);
            return;
        }
        root = fs_union_find(fs);
        if (!root) return;
    }
}

/* 0x1001d6c0: priority 0 */
static void fs_normal_simulate(IvpController *ctrl, const IvpEventSim *es, IvpCore **cores, uint32_t ncores)
{
    (void)cores, (void)ncores;
    IvpFrictionSystem *fs = FS_OF(ctrl, normal);
    PhysWorld *w = fs->env;
    if (fs->n_contacts <= 1) {
        /* 0x1001d660 */
        IvpContactPoint *cp = fs->first;
        normal_single(cp, es, IVP_SET_FRICTION_DIST, 1.0f);
        if (cp->gap >= IVP_SET_MAX_DIST_FOR_FRICTION || IVP_CI_LEFT_FEATURE(cp->tmp)) ivp_fs_remove_cp(w, fs, cp);
    } else {
        ivp_fs_solve(w, fs, es);                  /* 0x10036d70 */
    }
    if (fs->n_contacts == 0) {
        ivp_fs_delete(w, fs);
        es->sim_unit->changed = true;
        return;
    }
    if (fs->uf_needed) {
        fs->uf_needed = false;
        IvpCore *c = fs_union_find(fs);
        if (c) {
            fs_split(w, fs, c);
            c->unit->changed = true;
        }
    }
}

/* 0x1001d9a0 (core_is_going_to_be_deleted) is covered by ivp_object_remove_contacts: the glue deletes the
   object (0x10009ab0) before its core */

const IvpControllerVt ivp_fs_spring_vt = {600, fs_spring_simulate, true};    /* 0x100633d0: associates its movable cores */
const IvpControllerVt ivp_fs_normal_vt = {0, fs_normal_simulate, false};     /* 0x10063408 */
const IvpControllerVt ivp_fs_update_vt = {2000, fs_update_simulate, false};  /* 0x100633ec */
