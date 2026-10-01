/* The broadphase (docs/ivp_collision.md 2.4, 2.6): the range manager, the OV tree with its elements, the
   recheck that reconciles an object's pairs with the overlapping elements, the collision filter and the
   collision delegator root that creates the pairs. */
#include "phys_internal.h"
#include <stdlib.h>

#define VGROW(arr, n, cap)                                                  \
    do {                                                                    \
        if ((n) == (cap)) {                                                 \
            (cap) = (cap) ? (cap) * 2 : 4;                                  \
            (arr) = realloc((arr), (cap) * sizeof *(arr));                  \
        }                                                                   \
    } while (0)

static bool moving(const PhysBody *b) { return (b->object_state & 7) != 0; }

/* ---- the range manager (0x1002d8e0, env +0x1c) ---- */

/* 0x1002dab0 get_coll_range_in_world (slot 1) */
static double range_in_world(PhysWorld *w, PhysBody *b)
{
    const IvpCore *c = &b->core;
    double s = (double)(c->max_surface_rot_speed + c->current_speed) + 1e-19;
    double x = s * 1.0;
    if (c->upper_limit_radius * 5.0 < x) x = c->upper_limit_radius * 5.0;
    if (x < 0.5) x = 0.5;
    if (x > 15.0) x = 15.0;
    x = x - w->delta_psi * s;
    double floor = s * 0.1 + c->upper_limit_radius;   /* the floor includes the radius (quirk) */
    return x > floor ? x : floor;
}

/* 0x1002d990 get_coll_range_intra_objects (slot 0) */
void ivp_range_intra(PhysWorld *w, PhysBody *a, PhysBody *b, double *ra, double *rb)
{
    const IvpCore *ca = &a->core, *cb = &b->core;
    double sA = (double)ca->max_surface_rot_speed + ca->current_speed + 1e-19;
    double sB = (double)cb->max_surface_rot_speed + cb->current_speed + 1e-19;
    double S = sA + sB;
    double rmin = ca->upper_limit_radius < cb->upper_limit_radius ? ca->upper_limit_radius : cb->upper_limit_radius;
    double x = S * 0.5, y = rmin * 0.89999997615814209;          /* 0x3feccccc_c0000000 */
    if (y < x) x = y;
    if (x < 0.80000001192092896) x = 0.80000001192092896;    /* 0x3fe99999_a0000000 */
    if (x > 10.0) x = 10.0;
    x = x - w->delta_psi * S;
    if (x < S * 0.10000000149011612) x = S * 0.10000000149011612;   /* 0x3fb99999_a0000000 */
    double a0 = sB * 0.2000000029802322 + sA;
    double a1 = a0 * 0.1800000071525574 + sB;                 /* uses the updated a0 (quirk) */
    double f = (1.0 / (a0 + a1)) * x;
    *ra = f * a0;
    *rb = f * a1;
}

/* ---- the OV tree (0x1002dff0) ---- */

void ivp_ov_init(IvpOvTree *ov)
{
    memset(ov, 0, sizeof *ov);
    for (int j = 0; j < 81; j++) ov->powerlist[j] = ldexp(1.0, j - 40);
}

static void node_free(IvpOvNode *n)
{
    free(n->ch);
    free(n->el);
    free(n);
}

void ivp_ov_free(IvpOvTree *ov)
{
    for (int i = 0; i < 256; i++)
        for (IvpOvNode *n = ov->hash[i], *x; n; n = x) {
            x = n->hnext;
            node_free(n);
        }
    memset(ov->hash, 0, sizeof ov->hash);
    ov->root = NULL;
}

/* the node hash: an identity lookup by (x, y, z, L) */
static uint32_t node_hash(int32_t x, int32_t y, int32_t z, int32_t L)
{
    uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u ^ (uint32_t)L * 2654435761u;
    return (h ^ (h >> 16)) & 255;
}

static IvpOvNode *hash_find(IvpOvTree *ov, int32_t x, int32_t y, int32_t z, int32_t L)
{
    for (IvpOvNode *n = ov->hash[node_hash(x, y, z, L)]; n; n = n->hnext)
        if (n->L == L && n->x == x && n->y == y && n->z == z) return n;
    return NULL;
}

static void hash_add(IvpOvTree *ov, IvpOvNode *n)
{
    uint32_t h = node_hash(n->x, n->y, n->z, n->L);
    n->hnext = ov->hash[h];
    ov->hash[h] = n;
}

static void hash_remove(IvpOvTree *ov, IvpOvNode *n)
{
    for (IvpOvNode **p = &ov->hash[node_hash(n->x, n->y, n->z, n->L)]; *p; p = &(*p)->hnext)
        if (*p == n) {
            *p = n->hnext;
            return;
        }
}

static IvpOvNode *node_new(int32_t x, int32_t y, int32_t z, int32_t L, int32_t raster)
{
    IvpOvNode *n = calloc(1, sizeof *n);
    n->x = x, n->y = y, n->z = z, n->L = L, n->raster = raster;
    return n;
}

static void add_child(IvpOvNode *p, IvpOvNode *c)
{
    VGROW(p->ch, p->nch, p->capch);
    p->ch[p->nch++] = c;
}

/* 0x1002e150: the binary exponent */
static int exponent(double x)
{
    uint64_t bits;
    memcpy(&bits, &x, 8);
    return (int)((bits >> 52) & 0x7ff) - 0x3ff;
}

/* 0x1002e210 (r == R in Ballance) */
static double calc_optimal_box(IvpOvTree *ov, const IvpOvElement *e, double r, double R)
{
    int lvl = exponent(2 * r) + 1;
    if (lvl < -39) lvl = -39;     /* -40 in the original; powerlist[41 - lvl] would overrun at -40 (radii < 2^-41) */
    for (;; lvl++) {
        double s = ov->powerlist[41 - lvl];
        int32_t lo[3];
        bool fit = true;
        for (int a = 0; a < 3 && fit; a++) {
            double c = e->center[a];
            lo[a] = (int32_t)floor((c - r) * s);
            int32_t hi = (int32_t)ceil((c + r) * s);
            fit = hi <= lo[a] + 2;
        }
        if (!fit) continue;
        ov->sx = lo[0], ov->sy = lo[1], ov->sz = lo[2], ov->sL = lvl - 1, ov->sraster = lvl;
        if (R <= r) return r;
        double s2 = ov->powerlist[40 - lvl];
        int32_t lo2[3];
        bool fit2 = true;
        for (int a = 0; a < 3 && fit2; a++) {
            double c = e->center[a];
            lo2[a] = (int32_t)floor((c - R) * s2);
            fit2 = (int32_t)ceil((c + R) * s2) <= lo2[a] + 2;
        }
        if (fit2) {
            ov->sx = lo2[0], ov->sy = lo2[1], ov->sz = lo2[2], ov->sL = lvl, ov->sraster = lvl + 1;
            return R;
        }
        return r;
    }
}

/* 0x1002e180: A coarser than B by s levels */
static bool contains(const IvpOvNode *A, const IvpOvNode *B, int s)
{
    const int32_t a[3] = {A->x, A->y, A->z}, b[3] = {B->x, B->y, B->z};
    for (int k = 0; k < 3; k++)
        if (!(a[k] * (1 << s) <= b[k] && b[k] <= a[k] * (1 << s) + (2 << s) - 2)) return false;
    return true;
}

/* 0x1002e4b0: A coarser by s levels; touching boxes do not overlap */
static bool overlaps(const IvpOvNode *A, const IvpOvNode *B, int s)
{
    const int32_t a[3] = {A->x, A->y, A->z}, b[3] = {B->x, B->y, B->z};
    for (int k = 0; k < 3; k++)
        if (!(b[k] + 2 > a[k] * (1 << s) && (a[k] + 2) * (1 << s) > b[k])) return false;
    return true;
}

/* 0x1002e560 */
static IvpOvNode *find_node(IvpOvNode *root, const IvpOvNode *n)
{
    IvpOvNode *cur = root;
    for (;;) {
        IvpOvNode *next = NULL;
        for (uint32_t i = 0; i < cur->nch && !next; i++)
            if (cur->ch[i]->L >= n->L && contains(cur->ch[i], n, cur->ch[i]->L - n->L)) next = cur->ch[i];
        if (!next) return cur;
        cur = next;
    }
}

/* 0x1002e5b0 */
static void connect_boxes(IvpOvTree *ov, IvpOvNode *p, IvpOvNode *n)
{
    while (p->raster - n->raster != 1) {
        int d = p->raster - n->raster;
        int32_t pc[3] = {p->x, p->y, p->z}, nc[3] = {n->x, n->y, n->z}, cc[3];
        for (int k = 0; k < 3; k++) {
            if (nc[k] < (2 * pc[k] + 1) * (1 << (d - 1))) cc[k] = 2 * pc[k];
            else if (nc[k] < (pc[k] + 1) * (1 << d)) cc[k] = 2 * pc[k] + 1;
            else cc[k] = 2 * pc[k] + 2;
        }
        IvpOvNode *c = node_new(cc[0], cc[1], cc[2], p->L - 1, p->raster - 1);
        c->parent = p;
        add_child(p, c);
        hash_add(ov, c);
        p = c;
    }
    n->parent = p;
    add_child(p, n);
}

/* 0x1002e7b0 */
static void expand_root(IvpOvTree *ov, const IvpOvNode *n)
{
    IvpOvNode *r = ov->root;
    int32_t rc[3] = {r->x, r->y, r->z}, nc[3] = {n->x, n->y, n->z}, pc[3];
    for (int k = 0; k < 3; k++) {
        pc[k] = rc[k] / 2;
        int32_t rem = rc[k] % 2;
        if (rem == -1) pc[k] -= 1;
        else if (rem == 0 && (double)nc[k] * ldexp(1.0, n->L) < (double)pc[k] * ldexp(1.0, r->L + 1)) pc[k] -= 1;
    }
    IvpOvNode *P = node_new(pc[0], pc[1], pc[2], r->L + 1, r->raster + 1);
    add_child(P, r);
    r->parent = P;
    hash_add(ov, P);
    ov->root = P;
}

static void cand_add(IvpOvTree *ov, IvpOvElement *x)
{
    VGROW(*ov->cand, *ov->ncand, *ov->capcand);
    (*ov->cand)[(*ov->ncand)++] = x;
}

static bool sphere_hit(const IvpOvElement *x, const IvpOvElement *e)
{
    float d[3] = {x->center[0] - e->center[0], x->center[1] - e->center[1], x->center[2] - e->center[2]};
    float r = x->radius + e->radius;
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= r * r;
}

/* 0x1002e9b0 */
static void collect_all(IvpOvTree *ov, IvpOvElement *e, IvpOvNode *node)
{
    for (uint32_t i = node->nel; i-- > 0;)
        if (sphere_hit(node->el[i], e)) cand_add(ov, node->el[i]);
    for (uint32_t i = node->nch; i-- > 0;) collect_all(ov, e, node->ch[i]);
}

/* 0x1002ea70 */
static void collect(IvpOvTree *ov, IvpOvElement *e, IvpOvNode *cur, IvpOvNode *target)
{
    for (uint32_t i = cur->nel; i-- > 0;)
        if (sphere_hit(cur->el[i], e)) cand_add(ov, cur->el[i]);
    if (target->raster < cur->raster - 1) {
        for (uint32_t i = 0; i < cur->nch; i++) {
            IvpOvNode *ch = cur->ch[i];
            if (ch == target) collect_all(ov, e, ch);
            else if (overlaps(ch, target, ch->L - target->L)) collect(ov, e, ch, target);
        }
    } else {
        for (uint32_t i = 0; i < cur->nch; i++) {
            IvpOvNode *ch = cur->ch[i];
            if (ch == target) collect_all(ov, e, ch);
            else if (overlaps(target, ch, target->L - ch->L)) collect(ov, e, ch, target);
        }
    }
}

/* 0x1002ec10 */
static double insert_ov_element(IvpOvTree *ov, IvpOvElement *e, double rmin, double rmax, IvpOvElement ***cand, uint32_t *ncand,
                                uint32_t *capcand)
{
    double r = calc_optimal_box(ov, e, rmin, rmax);
    e->radius = (float)r;
    IvpOvNode *n = hash_find(ov, ov->sx, ov->sy, ov->sz, ov->sL);
    if (!n) {
        n = node_new(ov->sx, ov->sy, ov->sz, ov->sL, ov->sraster);
        VGROW(n->el, n->nel, n->capel);
        n->el[n->nel++] = e;
        e->node = n;
        if (!ov->root) {
            ov->root = n;
            hash_add(ov, n);
            return r;
        }
        while (!(n->L <= ov->root->L && contains(ov->root, n, ov->root->L - n->L))) expand_root(ov, n);
        if (ov->root->L == n->L) {
            IvpOvNode *root = ov->root;
            VGROW(root->el, root->nel, root->capel);
            root->el[root->nel++] = e;
            e->node = root;
            node_free(n);
            n = root;
        } else {
            hash_add(ov, n);
            connect_boxes(ov, find_node(ov->root, n), n);
        }
    } else {
        VGROW(n->el, n->nel, n->capel);
        n->el[n->nel++] = e;
        e->node = n;
    }
    if (cand) {
        ov->cand = cand, ov->ncand = ncand, ov->capcand = capcand;
        collect(ov, e, ov->root, n);
        ov->cand = NULL;
    }
    return r;
}

/* 0x1002eec0 (with 0x1002ee50 / the node dtor 0x1002def0) */
static void remove_ov_element(IvpOvTree *ov, IvpOvElement *e)
{
    IvpOvNode *n = e->node;
    if (!n) return;
    e->node = NULL;
    for (uint32_t i = 0; i < n->nel; i++)
        if (n->el[i] == e) {
            memmove(&n->el[i], &n->el[i + 1], (n->nel - i - 1) * sizeof *n->el);
            n->nel--;
            break;
        }
    while (n && !n->nel && !n->nch) {
        IvpOvNode *p = n->parent;
        hash_remove(ov, n);
        if (!p) ov->root = NULL;
        else
            for (uint32_t i = 0; i < p->nch; i++)
                if (p->ch[i] == n) {
                    memmove(&p->ch[i], &p->ch[i + 1], (p->nch - i - 1) * sizeof *p->ch);
                    p->nch--;
                    break;
                }
        node_free(n);
        n = p;
    }
}

/* 0x1002dd00: listen on the object's hull, budget delta beyond its current value */
static void ov_hull_add(PhysWorld *w, IvpOvElement *e, IvpHullManager *hm, double delta)
{
    if (e->hull) ivp_hull_remove(e->hull, &e->hl);
    else e->hull = hm;
    IvpHullManager *h = e->hull;
    ivp_hull_insert(h, &e->hl, (float)((w->current_time - h->last_time) * h->gradient + h->hull_value + delta));
}

/* ---- collisions in the elements' vectors (0x1002dde0 / 0x1002de30): slot fvec[0] for the first element a
   collision was added to, fvec[1] for the second ---- */

void ivp_ov_add_collision(IvpOvElement *e, IvpCollision *c)
{
    VGROW(e->coll, e->ncoll, e->capcoll);
    int slot = c->fvec[0] < 0 ? 0 : 1;
    c->fvec[slot] = (int32_t)e->ncoll;
    e->coll[e->ncoll++] = c;
}

/* the collision at index i of e: which of its slots holds i */
static int slot_of(const IvpOvElement *e, const IvpCollision *c, uint32_t i)
{
    (void)e;
    return c->fvec[0] == (int32_t)i ? 0 : 1;
}

static void ov_remove_collision(IvpOvElement *e, IvpCollision *c)
{
    for (uint32_t i = 0; i < e->ncoll; i++) {
        if (e->coll[i] != c) continue;
        int s = slot_of(e, c, i);
        c->fvec[s] = -1;
        e->ncoll--;
        if (i != e->ncoll) {
            IvpCollision *m = e->coll[e->ncoll];
            m->fvec[slot_of(e, m, e->ncoll)] = (int32_t)i;
            e->coll[i] = m;
        }
        return;
    }
}

/* 0x100188c0: swap two collisions of e, fixing their slots */
static void ov_swap(IvpOvElement *e, uint32_t i, uint32_t j)
{
    IvpCollision *a = e->coll[i], *b = e->coll[j];
    int sa = slot_of(e, a, i), sb = slot_of(e, b, j);
    a->fvec[sa] = (int32_t)j;
    b->fvec[sb] = (int32_t)i;
    e->coll[i] = b;
    e->coll[j] = a;
}

static void collision_objects(IvpCollision *c, PhysBody **o0, PhysBody **o1)
{
    if (c->kind == IVP_COLL_MINDIST) {
        IvpMindist *m = (IvpMindist *)c;
        *o0 = m->syn[0].obj, *o1 = m->syn[1].obj;
    } else {
        IvpWatcher *wt = (IvpWatcher *)c;
        *o0 = wt->syn[0].obj, *o1 = wt->syn[1].obj;
    }
}

/* the root delegator's collision_is_going_to_be_deleted (slot 0, 0x1002f3f0) */
void ivp_collision_deleted(PhysWorld *w, IvpCollision *c)
{
    (void)w;
    PhysBody *o0, *o1;
    collision_objects(c, &o0, &o1);
    if (o0->coll.ov) ov_remove_collision(o0->coll.ov, c);
    if (o1->coll.ov && o1 != o0) ov_remove_collision(o1->coll.ov, c);
}

/* vtable slot 4 (the deleting dtor) of a root collision */
void ivp_collision_delete(PhysWorld *w, IvpCollision *c)
{
    if (c->kind == IVP_COLL_MINDIST) ivp_mindist_delete(w, (IvpMindist *)c);
    else ivp_watcher_delete(w, (IvpWatcher *)c);
}

/* ---- the collision filter (env +0x18): the glue's chain of the group filter (0x10014900) and the exclusive
   pair filter (0x10014b30; nothing registers pairs in Ballance) ---- */
static bool filter_check(const PhysBody *a, const PhysBody *b)
{
    if (!a->group[0] || !b->group[0]) return true;
    return strncmp(a->group, b->group, 8) != 0;
}

/* 0x1002f430 object_pairs_may_collide: a plain mindist when both surfaces are one ledge, else an OO watcher
   (the recursive mindist needs a hull ledge, which the builder never makes) */
static void object_pairs_may_collide(PhysWorld *w, PhysBody *a, PhysBody *b)
{
    const IvpCompactLedge *la = a->coll.type == IVP_OBJ_BALL ? ivp_ball_ledge() : ivp_surface_single_convex(a->shape->cs);
    const IvpCompactLedge *lb = b->coll.type == IVP_OBJ_BALL ? ivp_ball_ledge() : ivp_surface_single_convex(b->shape->cs);
    IvpCollision *c;
    if (!la || !lb) {
        c = &ivp_watcher_create(w, a, b)->coll;
    } else {
        assert(!ivp_has_children(la) && !ivp_has_children(lb));
        c = &ivp_mindist_create(w, NULL, a, b, ivp_first_edge(la), ivp_first_edge(lb))->coll;
    }
    ivp_ov_add_collision(a->coll.ov, c);
    ivp_ov_add_collision(b->coll.ov, c);
}

/* 0x10017140 */
void ivp_recheck_ov_element(PhysWorld *w, PhysBody *obj)
{
    IvpOvElement *e = obj->coll.ov;
    if (!e) return;
    IvpCollWorld *cw = &w->coll;
    remove_ov_element(&cw->ov, e);
    cw->ov_rechecks++;
    const IvpCore *core = &obj->core;
    for (int k = 0; k < 3; k++) e->center[k] = (float)core->m_world_f_core.vv[k];
    double R = core->upper_limit_radius;
    double r = (double)core->upper_limit_radius + range_in_world(w, obj);
    IvpOvElement **cand = NULL;
    uint32_t ncand = 0, capcand = 0;
    double r_used = insert_ov_element(&cw->ov, e, r, r, &cand, &ncand, &capcand);
    ov_hull_add(w, e, &obj->coll.hull, r_used - R);
    /* reconcile the existing collisions with the candidates, last to first */
    uint32_t keep = 0;
    PhysBody **fresh = NULL;
    uint32_t nfresh = 0, capfresh = 0;
    for (uint32_t ci = ncand; ci-- > 0;) {
        PhysBody *other = cand[ci]->obj;
        if (!(moving(obj) || moving(other))) continue;
        if (other == obj) continue;               /* obj+0xa8 equal: the same unit */
        if (!filter_check(obj, other)) continue;
        int32_t found = -1;
        for (uint32_t i = 0; i < e->ncoll && found < 0; i++) {
            PhysBody *o0, *o1;
            collision_objects(e->coll[i], &o0, &o1);
            if ((o0 == obj && o1 == other) || (o1 == obj && o0 == other)) found = (int32_t)i;
        }
        if (found >= 0) {
            if ((int32_t)keep < found) ov_swap(e, keep, (uint32_t)found);
            if ((int32_t)keep <= found) keep++;
        } else {
            VGROW(fresh, nfresh, capfresh);
            fresh[nfresh++] = other;
        }
    }
    free(cand);
    while (e->ncoll > keep) ivp_collision_delete(w, e->coll[e->ncoll - 1]);
    for (uint32_t i = nfresh; i-- > 0;) object_pairs_may_collide(w, obj, fresh[i]);   /* the one delegator root */
    free(fresh);
}

/* 0x100099f0: the recheck with the core's state forced to 0x21, so new mindists don't go to the hull */
void ivp_recheck_ov_element_forced(PhysWorld *w, PhysBody *b)
{
    int8_t s = b->core.movement_state;
    b->core.movement_state = 0x21;
    ivp_recheck_ov_element(w, b);
    b->core.movement_state = s;
}

/* 0x100176e0 */
void ivp_enable_collision_detection(PhysWorld *w, PhysBody *b)
{
    if (b->coll.ov) ivp_disable_collision_detection(w, b);
    IvpOvElement *e = calloc(1, sizeof *e);
    e->hl.kind = IVP_HL_OV;
    e->hl.index = IVP_NO_INDEX;
    e->radius = -1.0f;
    e->obj = b;
    b->coll.ov = e;
    ivp_recheck_ov_element(w, b);
}

/* 0x1002f3b0 object_is_removed + the element dtor 0x1002dc70 */
void ivp_disable_collision_detection(PhysWorld *w, PhysBody *b)
{
    IvpOvElement *e = b->coll.ov;
    if (!e) return;
    while (e->ncoll) ivp_collision_delete(w, e->coll[e->ncoll - 1]);
    if (e->hull) ivp_hull_remove(e->hull, &e->hl);
    remove_ov_element(&w->coll.ov, e);
    free(e->coll);
    free(e);
    b->coll.ov = NULL;
}
