/* Compact surfaces (docs/ivp_collision.md 1): the triangle ("pancake") ledge, the compile of a ledge soup
   into a surface with its ledge tree and merged point pool, the mass properties, and the surface manager
   queries the collision code uses. */
#include "phys_internal.h"
#include <stdlib.h>

uint32_t ivp_tree_split_guards;

/* 0x10020100(size, 16) / 0x10020130 */
void *ivp_aligned_alloc(size_t size)
{
    char *raw = malloc(size + 16 + sizeof(void *));
    char *p = (char *)(((uintptr_t)raw + sizeof(void *) + 15) & ~(uintptr_t)15);
    memcpy(p - sizeof(void *), &raw, sizeof raw);
    return p;
}

void ivp_aligned_free(void *p)
{
    if (!p) return;
    char *raw;
    memcpy(&raw, (char *)p - sizeof(void *), sizeof raw);
    free(raw);
}

/* 0x1003ac00: the cached template (0x100763a8, two-sided triangle A = (p0, p1, p2), B = (p1, p0, p2), pierce
   partners) with points 0..2 overwritten; NULL when |(p1-p0) x (p2-p0)|^2 < 1e-12 */
IvpCompactLedge *ivp_ledge_triangle(const double p0[3], const double p1[3], const double p2[3])
{
    double u[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, v[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
    double c[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    if (c[0] * c[0] + c[1] * c[1] + c[2] * c[2] < 1e-12) return NULL;
    IvpCompactLedge *l = ivp_aligned_alloc(96);
    memset(l, 0, 96);
    l->c_point_offset = 0x30;
    l->flags = 0x604;
    l->n_triangles = 2;
    IvpCompactTriangle *t = (IvpCompactTriangle *)(l + 1);
    t[0].word = 0x00001000;
    t[0].e[0] = 0x00040000, t[0].e[1] = 0x00050001, t[0].e[2] = 0x00030002;
    t[1].word = 0x00000001;
    t[1].e[0] = 0x7ffc0001, t[1].e[1] = 0x7ffd0000, t[1].e[2] = 0x7ffb0002;
    IvpPolyPoint *pp = (IvpPolyPoint *)((char *)l + 0x30);
    const double *src[3] = {p0, p1, p2};
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) pp[i].k[k] = (float)src[i][k];
    return l;
}

/* 0x1003ae20 */
IvpCompactLedge *ivp_ledge_from_points(const double (*p)[3], uint32_t n)
{
    if (n < 3) return NULL;
    if (n == 3) return ivp_ledge_triangle(p[0], p[1], p[2]);
    /* deduplicated again by the f64 bytes (first occurrences kept) */
    double (*q)[3] = malloc(n * sizeof *q);
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        bool dup = false;
        for (uint32_t k = 0; k < m && !dup; k++) dup = !memcmp(q[k], p[i], sizeof *q);
        if (!dup) memcpy(q[m++], p[i], sizeof *q);
    }
    IvpCompactLedge *l = m == 3 ? ivp_ledge_triangle(q[0], q[1], q[2]) : m < 3 ? NULL : ivp_ledge_convex_hull((const double (*)[3])q, m);
    free(q);
    return l;
}

/* ---- compile (0x100384b0 with the default template of 0x10038490) ---- */

/* the sphere record (0x40 bytes, 0x10038750) */
typedef struct Sphere {
    double center[3], radius;
    uint8_t box[3];
    int ledge;                    /* leaf: the input ledge index, else -1 */
    struct Sphere *left, *right;
} Sphere;

typedef struct {
    Sphere *s;
    uint32_t n;
} SphereArena;

/* 0x100202e0: the AABB over every edge's start point (f32 compares, min first) */
static void ledge_aabb(const IvpCompactLedge *l, float mn[3], float mx[3])
{
    const IvpPolyPoint *pts = ivp_points(l);
    const float *p0 = pts[ivp_ledge_tri(l, 0)->e[0] & 0xffff].k;
    for (int k = 0; k < 3; k++) mn[k] = mx[k] = p0[k];
    for (int t = 0; t < l->n_triangles; t++)
        for (int e = 0; e < 3; e++) {
            const float *p = pts[ivp_ledge_tri(l, t)->e[e] & 0xffff].k;
            for (int k = 0; k < 3; k++) {
                if (p[k] < mn[k]) mn[k] = p[k];
                else if (p[k] > mx[k]) mx[k] = p[k];
            }
        }
}

/* member box (FUN_10038b40): [(f32)(c - box*(0.004 r)), (f32)(c + box*(0.004 r))] */
static void union_boxes(Sphere **list, uint32_t n, float bmin[3], float bmax[3])
{
    for (int k = 0; k < 3; k++) bmin[k] = 1e6f, bmax[k] = -1e6f;
    for (uint32_t i = 0; i < n; i++) {
        const Sphere *s = list[i];
        for (int k = 0; k < 3; k++) {
            double h = s->box[k] * (0.004 * s->radius);
            float lo = (float)(s->center[k] - h), hi = (float)(s->center[k] + h);
            if (lo < bmin[k]) bmin[k] = lo;
            if (hi > bmax[k]) bmax[k] = hi;
        }
    }
}

static float box_volume(Sphere **list, uint32_t n)
{
    float a[3], b[3];
    union_boxes(list, n, a, b);
    return fabsf(b[0] - a[0]) * fabsf(b[1] - a[1]) * fabsf(b[2] - a[2]);
}

/* 0x10038c90: top-down; per axis the members split at the box midpoint (centres within +-1e-6 of it follow
   their list neighbour, or alternate), the axis with the smallest summed box volume wins */
static Sphere *build(SphereArena *ar, Sphere **list, uint32_t n)
{
    if (n == 1) return list[0];
    float bmin[3], bmax[3];
    union_boxes(list, n, bmin, bmax);
    Sphere *node = &ar->s[ar->n++];
    node->ledge = -1;
    double r2 = 0;
    for (int k = 0; k < 3; k++) {
        node->center[k] = 0.5 * (double)bmin[k] + 0.5 * (double)bmax[k];
        float d = (float)(bmax[k] - (float)node->center[k]);
        r2 += (double)d * d;
    }
    node->radius = sqrt(r2);
    for (int k = 0; k < 3; k++) node->box[k] = (uint8_t)((int32_t)((bmax[k] - node->center[k]) / (0.004 * node->radius)) + 1);
    if (n == 2) {
        node->left = list[0];
        node->right = list[1];
        return node;
    }
    Sphere **low[3], **high[3];
    uint32_t nlow[3], nhigh[3];
    float cost[3];
    double eps = (double)1e-6f;
    for (int k = 0; k < 3; k++) {
        low[k] = malloc(n * sizeof **low), high[k] = malloc(n * sizeof **high);
        nlow[k] = nhigh[k] = 0;
        double split = bmin[k] + ((double)bmax[k] - bmin[k]) * 0.5;
        int toggle = 1;
        for (uint32_t j = 0; j < n; j++) {
            double c = list[j]->center[k];
            if (c < split - eps) low[k][nlow[k]++] = list[j];
            else if (c > split + eps) high[k][nhigh[k]++] = list[j];
            else {
                double nb = j < n - 1 ? list[j + 1]->center[k] : list[j - 1]->center[k];
                if (nb < split - eps) high[k][nhigh[k]++] = list[j];
                else if (nb > split + eps) low[k][nlow[k]++] = list[j];
                else if (toggle == 1) low[k][nlow[k]++] = list[j], toggle = 0;
                else high[k][nhigh[k]++] = list[j], toggle = 1;
            }
        }
        cost[k] = box_volume(low[k], nlow[k]) + box_volume(high[k], nhigh[k]);
    }
    int ks = cost[1] <= cost[0] ? (cost[1] < cost[2] ? 1 : 2) : (cost[0] < cost[2] ? 0 : 2);
    if (!nlow[ks] || !nhigh[ks]) {
        /* the original recurses forever here (9.6); split in list order instead */
        ivp_tree_split_guards++;
        nlow[ks] = nhigh[ks] = 0;
        for (uint32_t j = 0; j < n; j++) {
            if (j < n / 2) low[ks][nlow[ks]++] = list[j];
            else high[ks][nhigh[ks]++] = list[j];
        }
    }
    node->left = build(ar, low[ks], nlow[ks]);
    node->right = build(ar, high[ks], nhigh[ks]);
    for (int k = 0; k < 3; k++) free(low[k]), free(high[k]);
    return node;
}

typedef struct {
    IvpLedgeNode *nodes;
    uint32_t next;
    IvpCompactLedge **placed;     /* the copied ledges by input index */
} TreeWriter;

/* 0x10039a70: preorder, left subtree at node + 0x1c */
static IvpLedgeNode *write_tree(TreeWriter *tw, const Sphere *s)
{
    IvpLedgeNode *n = &tw->nodes[tw->next++];
    for (int k = 0; k < 3; k++) n->center[k] = (float)s->center[k];
    n->radius = (float)s->radius;
    memcpy(n->box_sizes, s->box, 3);
    n->free_0 = 0;
    if (s->left) {
        n->offset_compact_ledge = 0;
        write_tree(tw, s->left);
        IvpLedgeNode *r = write_tree(tw, s->right);
        n->offset_right_node = (int32_t)((char *)r - (char *)n);
    } else {
        IvpCompactLedge *l = tw->placed[s->ledge];
        l->flags &= ~3u;
        n->offset_right_node = 0;
        n->offset_compact_ledge = (int32_t)((char *)l - (char *)n);
    }
    return n;
}

/* the terminal ledges in left-to-right DFS order (0x10020480) */
static void collect_ledges(const IvpLedgeNode *node, IvpLedgeVec *out)
{
    while (node->offset_right_node) {
        collect_ledges(node + 1, out);
        node = (const IvpLedgeNode *)((const char *)node + node->offset_right_node);
    }
    if (out->n == out->cap) out->cap = out->cap ? out->cap * 2 : 16, out->v = realloc(out->v, out->cap * sizeof *out->v);
    out->v[out->n++] = (const IvpCompactLedge *)((const char *)node + node->offset_compact_ledge);
}

/* the second moments about mc of the closed surface: V and the integrals of x^2, y^2, z^2 (the result of
   FUN_1003bb20's edge integrals; computed here over the tetrahedra to mc) */
static void second_moments(const IvpLedgeVec *lv, const double mc[3], double m[3])
{
    double V = 0, I[3] = {0, 0, 0};
    for (uint32_t i = 0; i < lv->n; i++) {
        const IvpCompactLedge *l = lv->v[i];
        for (int t = 0; t < l->n_triangles; t++) {
            const IvpCompactTriangle *tr = ivp_ledge_tri(l, t);
            double a[3], b[3], c[3];
            for (int k = 0; k < 3; k++) {
                a[k] = (double)ivp_P(l, &tr->e[0])[k] - mc[k];
                b[k] = (double)ivp_P(l, &tr->e[1])[k] - mc[k];
                c[k] = (double)ivp_P(l, &tr->e[2])[k] - mc[k];
            }
            double v6 = a[0] * (b[1] * c[2] - b[2] * c[1]) + a[1] * (b[2] * c[0] - b[0] * c[2]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
            V += v6 / 6;
            for (int k = 0; k < 3; k++)
                I[k] += v6 / 60 * (a[k] * a[k] + b[k] * b[k] + c[k] * c[k] + a[k] * b[k] + a[k] * c[k] + b[k] * c[k]);
        }
    }
    for (int k = 0; k < 3; k++) m[k] = V < 1e-19 ? 1.0 : I[k] / V;
}

/* 0x10039b30: mass centre and unit inertia (0x1003bbf0), radius and deviation (0x10020650) */
static void mass_props(IvpCompactSurface *cs)
{
    IvpLedgeVec lv = {0};
    collect_ledges(ivp_surface_root(cs), &lv);
    double S2 = 0, S6 = 0, mc[3], I[3];
    float C[3] = {0, 0, 0};
    for (uint32_t i = 0; i < lv.n; i++) {
        const IvpCompactLedge *l = lv.v[i];
        for (int t = 0; t < l->n_triangles; t++) {
            const IvpCompactTriangle *tr = ivp_ledge_tri(l, t);
            const float *p0 = ivp_P(l, &tr->e[0]), *p1 = ivp_P(l, &tr->e[1]), *p2 = ivp_P(l, &tr->e[2]);
            float u[3], v[3], n[3];
            v3sub(u, p1, p0), v3sub(v, p2, p0);
            v3cross(n, u, v);                             /* 0x1003b8e0 */
            S2 += (double)n[0] * n[0] + (double)n[1] * n[1] + (double)n[2] * n[2];
            double d = (double)n[0] * p0[0] + (double)n[1] * p0[1] + (double)n[2] * p0[2];
            S6 += d;
            float f = (float)(d * 0.25f);
            for (int k = 0; k < 3; k++) C[k] += f * (p0[k] + p1[k] + p2[k]);
        }
    }
    double sq = sqrt(S2);
    if (S6 <= sq * sq * sq * 1e-9) {
        /* no volume (every concave soup: front and back cancel): the union AABB's centre */
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        for (uint32_t i = 0; i < lv.n; i++) {
            float a[3], b[3];
            ledge_aabb(lv.v[i], a, b);
            for (int k = 0; k < 3; k++) {
                if (a[k] < mn[k]) mn[k] = a[k];
                if (b[k] > mx[k]) mx[k] = b[k];
            }
        }
        double d2 = 0;
        for (int k = 0; k < 3; k++) {
            mc[k] = 0.5 * (double)mn[k] + 0.5 * (double)mx[k];
            d2 += ((double)mx[k] - mn[k]) * ((double)mx[k] - mn[k]);
        }
        double h = 0.5 * sqrt(d2);
        for (int k = 0; k < 3; k++) I[k] = h * h * 0.5;
    } else {
        float inv = (float)(1.0 / S6);
        for (int k = 0; k < 3; k++) mc[k] = (double)(C[k] * inv);    /* 0x1003bf40 */
        double m[3];
        second_moments(&lv, mc, m);
        /* the stored unit inertia is sqrt(m_y^2 + m_z^2), not m_y + m_z (the quirk ported boxes need) */
        I[0] = sqrt(m[1] * m[1] + m[2] * m[2]);
        I[1] = sqrt(m[0] * m[0] + m[2] * m[2]);
        I[2] = sqrt(m[0] * m[0] + m[1] * m[1]);
    }
    for (int k = 0; k < 3; k++) cs->mass_center[k] = (float)mc[k], cs->rotation_inertia[k] = (float)I[k];
    /* 0x10020650 */
    double R = 0, dev = 0;
    for (uint32_t i = 0; i < lv.n; i++) {
        const IvpCompactLedge *l = lv.v[i];
        for (int t = 0; t < l->n_triangles; t++) {
            const IvpCompactTriangle *tr = ivp_ledge_tri(l, t);
            double n[3];
            {
                const float *p0 = ivp_P(l, &tr->e[0]), *p1 = ivp_P(l, &tr->e[1]), *p2 = ivp_P(l, &tr->e[2]);
                double u[3] = {(double)p1[0] - p0[0], (double)p1[1] - p0[1], (double)p1[2] - p0[2]};
                double v[3] = {(double)p2[0] - p0[0], (double)p2[1] - p0[1], (double)p2[2] - p0[2]};
                n[0] = u[1] * v[2] - u[2] * v[1], n[1] = u[2] * v[0] - u[0] * v[2], n[2] = u[0] * v[1] - u[1] * v[0];
            }
            double nn = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
            for (int e = 0; e < 3; e++) {
                const float *p = ivp_P(l, &tr->e[e]);
                double d[3] = {p[0] - mc[0], p[1] - mc[1], p[2] - mc[2]};
                double r = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (r > R) R = r;
                double x[3] = {n[1] * d[2] - n[2] * d[1], n[2] * d[0] - n[0] * d[2], n[0] * d[1] - n[1] * d[0]};
                if (nn > 0) {
                    double a = sqrt((x[0] * x[0] + x[1] * x[1] + x[2] * x[2]) / nn);
                    if (a > dev) dev = a;
                }
            }
        }
    }
    cs->upper_limit_radius = (float)R;
    uint32_t f = R > 0 ? (uint32_t)(uint8_t)(int32_t)(dev / (R * 0.004) + 1.0) : 1;
    cs->dev_and_size = (cs->dev_and_size & ~0xffu) | f;
    free(lv.v);
}

/* the point pool's hash (FUN_1003b4b0 is a CRC32 of the 12 bytes; any hash works for the lookup) */
static uint32_t point_hash(const float k[3])
{
    uint32_t h = 2166136261u, w[3];
    memcpy(w, k, 12);
    for (int i = 0; i < 3; i++) h = (h ^ w[i]) * 16777619u;
    return h;
}

/* 0x100384b0 */
IvpCompactSurface *ivp_surface_compile(IvpCompactLedge **ledges, uint32_t n)
{
    if (!n) return NULL;
    /* 0x10038750: a sphere per ledge, in insertion order */
    SphereArena ar = {calloc(2 * n, sizeof(Sphere)), 0};
    Sphere **list = malloc(n * sizeof *list);
    for (uint32_t i = 0; i < n; i++) {
        Sphere *s = &ar.s[ar.n++];
        float mn[3], mx[3];
        ledge_aabb(ledges[i], mn, mx);
        double r2 = 0;
        for (int k = 0; k < 3; k++) {
            s->center[k] = 0.5 * (double)mn[k] + 0.5 * (double)mx[k];
            double d = (double)mx[k] - s->center[k];
            r2 += d * d;
        }
        s->radius = sqrt(r2);
        for (int k = 0; k < 3; k++) s->box[k] = (uint8_t)((int32_t)(((double)mx[k] - s->center[k]) / (s->radius * 0.004)) + 1);
        s->ledge = (int)i;
        list[i] = s;
    }
    const Sphere *root = build(&ar, list, n);
    free(list);
    /* 0x10039600: one ledge is copied whole; several share a merged point pool */
    uint32_t ntri = 0;
    for (uint32_t i = 0; i < n; i++) ntri += (uint32_t)ledges[i]->n_triangles;
    uint32_t nnodes = 2 * n - 1, npool = 0, root_off;
    float (*pool)[3] = NULL;
    uint32_t *remap = NULL;
    if (n == 1) {
        root_off = 0x30 + (ledges[0]->flags >> 8) * 16;
    } else {
        /* every edge's point by exact xyz equality, first appearance order */
        uint32_t cap = 1024;
        while (cap < 4 * ntri) cap *= 2;
        int32_t *tab = malloc(cap * sizeof *tab);
        for (uint32_t i = 0; i < cap; i++) tab[i] = -1;
        pool = malloc(3 * ntri * sizeof *pool);
        remap = malloc(3 * ntri * sizeof *remap);
        uint32_t ei = 0;
        for (uint32_t i = 0; i < n; i++)
            for (int t = 0; t < ledges[i]->n_triangles; t++)
                for (int e = 0; e < 3; e++) {
                    const float *p = ivp_P(ledges[i], &ivp_ledge_tri(ledges[i], t)->e[e]);
                    uint32_t h = point_hash(p) & (cap - 1);
                    while (tab[h] >= 0 && memcmp(pool[tab[h]], p, 12)) h = (h + 1) & (cap - 1);
                    if (tab[h] < 0) {
                        memcpy(pool[npool], p, 12);
                        tab[h] = (int32_t)npool++;
                    }
                    remap[ei++] = (uint32_t)tab[h];
                }
        free(tab);
        root_off = 0x30 + 16 * (npool + n + ntri);
    }
    uint32_t byte_size = root_off + 0x1c * nnodes;
    IvpCompactSurface *cs = ivp_aligned_alloc(byte_size);
    memset(cs, 0, byte_size);
    cs->offset_ledgetree_root = (int32_t)root_off;
    cs->dev_and_size = byte_size << 8;
    IvpCompactLedge **placed = malloc(n * sizeof *placed);
    char *dst = (char *)cs + 0x30;
    if (n == 1) {
        size_t sz = (ledges[0]->flags >> 8) * 16;
        memcpy(dst, ledges[0], sz);
        placed[0] = (IvpCompactLedge *)dst;
    } else {
        IvpPolyPoint *pp = (IvpPolyPoint *)((char *)cs + 0x30 + 16 * (n + ntri));
        for (uint32_t i = 0; i < npool; i++) memcpy(pp[i].k, pool[i], 12), pp[i].hesse = 0;
        uint32_t ei = 0;
        for (uint32_t i = 0; i < n; i++) {
            size_t sz = ((size_t)ledges[i]->n_triangles + 1) * 16;
            memcpy(dst, ledges[i], sz);
            IvpCompactLedge *l = (IvpCompactLedge *)dst;
            IvpCompactTriangle *tr = (IvpCompactTriangle *)(l + 1);
            for (int t = 0; t < l->n_triangles; t++)
                for (int e = 0; e < 3; e++) tr[t].e[e] = (tr[t].e[e] & 0xffff0000u) | (remap[ei++] & 0xffff);
            l->c_point_offset = (int32_t)((char *)pp - (char *)l);   /* size_div_16 stays stale */
            placed[i] = l;
            dst += sz;
        }
    }
    free(pool);
    free(remap);
    TreeWriter tw = {(IvpLedgeNode *)((char *)cs + root_off), 0, placed};
    write_tree(&tw, root);
    free(placed);
    free(ar.s);
    mass_props(cs);
    for (uint32_t i = 0; i < n; i++) ivp_aligned_free(ledges[i]);   /* t[1]: free the input ledges */
    return cs;
}

/* ---- the polygon surface manager (vtable 0x100631e0) ---- */

/* 0x1000bcd0 */
const IvpCompactLedge *ivp_surface_single_convex(const IvpCompactSurface *cs)
{
    const IvpLedgeNode *root = ivp_surface_root(cs);
    if (!root->offset_right_node || root->offset_compact_ledge)
        return (const IvpCompactLedge *)((const char *)root + root->offset_compact_ledge);
    return NULL;
}

/* 0x1000bc40 */
void ivp_surface_radius_dev(const IvpCompactSurface *cs, const float c[3], float *radius, float *dev)
{
    double d2 = 0;
    for (int k = 0; k < 3; k++) d2 += ((double)cs->mass_center[k] - c[k]) * ((double)cs->mass_center[k] - c[k]);
    float d = (float)sqrt(d2);
    *radius = cs->upper_limit_radius + d;
    *dev = (float)(cs->dev_and_size & 0xff) * cs->upper_limit_radius * 0.004f + d;
}

/* 0x1000b950: the sphere and box descent */
static void ledges_walk(const IvpLedgeNode *node, const double c[3], double r, IvpLedgeVec *out)
{
    for (;;) {
        double d[3] = {node->center[0] - c[0], node->center[1] - c[1], node->center[2] - c[2]};
        double R = node->radius + r;
        if (R * R < d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) return;
        float s = (float)(node->radius * 0.004);
        for (int k = 0; k < 3; k++)
            if (!(fabs(d[k]) < node->box_sizes[k] * (double)s + r)) return;
        if (node->offset_compact_ledge) {
            if (out->n == out->cap) out->cap = out->cap ? out->cap * 2 : 16, out->v = realloc(out->v, out->cap * sizeof *out->v);
            out->v[out->n++] = (const IvpCompactLedge *)((const char *)node + node->offset_compact_ledge);
            return;
        }
        if (!node->offset_right_node) return;     /* both offsets 0: never in valid data */
        ledges_walk(node + 1, c, r, out);
        node = (const IvpLedgeNode *)((const char *)node + node->offset_right_node);
    }
}

/* 0x1000bb30 with root_ledge NULL (the only use: no node has a hull) */
void ivp_surface_ledges_within_radius(const IvpCompactSurface *cs, const double c[3], double r, IvpLedgeVec *out)
{
    ledges_walk(ivp_surface_root(cs), c, r, out);
}

/* the ball surface manager (0x10076380, 0x1002fdf0): a 32-byte all-zero ledge */
const IvpCompactLedge *ivp_ball_ledge(void)
{
    static _Alignas(16) uint8_t dummy[32];
    return (const IvpCompactLedge *)dummy;
}
