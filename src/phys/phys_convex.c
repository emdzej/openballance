/* Convex hull ledges (docs/ivp_collision.md 1.6.3) and the ledge generator (0x10048b60 / 0x10048ed0).

   The original runs qhull ("qhull Qs Pp C-0 W1e-14 E1.0e-18", 0x10046c30), drops sliver facets by removing a
   vertex and retrying (0x1003a330), builds a polygon template (0x10039cf0), triangulates every polygon
   (0x10048320), assigns the pierce partners (0x10048a60) and writes the ledge. This is a replacement hull
   builder that produces the same structure: an incremental hull in f64, coplanar facets merged into convex
   polygons (collinear boundary vertices dropped), each polygon fanned into outward-wound triangles, twins
   linked, pierce = the most anti-parallel triangle. Only the triangle and point order (tie breaking) can differ
   from qhull's (1.9). */
#include "phys_internal.h"
#include <stdlib.h>

typedef struct {
    int v[3];
    double n[3], d;
    bool alive;
} HFace;

typedef struct {
    HFace *f;
    int n, cap;
} FaceVec;

static void face_plane(const double (*p)[3], HFace *f)
{
    const double *a = p[f->v[0]], *b = p[f->v[1]], *c = p[f->v[2]];
    double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    double n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l > 0) n[0] /= l, n[1] /= l, n[2] /= l;
    memcpy(f->n, n, sizeof n);
    f->d = n[0] * a[0] + n[1] * a[1] + n[2] * a[2];
}

static double face_dist(const HFace *f, const double *q) { return f->n[0] * q[0] + f->n[1] * q[1] + f->n[2] * q[2] - f->d; }

static void add_face(FaceVec *fv, const double (*p)[3], int a, int b, int c)
{
    if (fv->n == fv->cap) {
        fv->cap = fv->cap ? fv->cap * 2 : 32;
        fv->f = realloc(fv->f, (size_t)fv->cap * sizeof *fv->f);
    }
    HFace *f = &fv->f[fv->n++];
    f->v[0] = a, f->v[1] = b, f->v[2] = c;
    f->alive = true;
    face_plane(p, f);
}

static double dist2(const double *a, const double *b)
{
    double d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
}

/* every directed edge of the alive faces has exactly one reverse */
static bool manifold(const FaceVec *fv)
{
    for (int i = 0; i < fv->n; i++) {
        if (!fv->f[i].alive) continue;
        for (int k = 0; k < 3; k++) {
            int a = fv->f[i].v[k], b = fv->f[i].v[(k + 1) % 3], twins = 0;
            for (int j = 0; j < fv->n; j++) {
                if (!fv->f[j].alive) continue;
                for (int m = 0; m < 3; m++) twins += fv->f[j].v[m] == b && fv->f[j].v[(m + 1) % 3] == a;
            }
            if (twins != 1) return false;
        }
    }
    return true;
}

/* incremental hull: faces with outward normals. 0 for flat or degenerate input. */
static int hull3d(const double (*p)[3], int n, double eps, FaceVec *fv)
{
    int i0 = 0;
    for (int i = 1; i < n; i++)
        if (p[i][0] < p[i0][0]) i0 = i;
    int i1 = -1;
    double best = 0;
    for (int i = 0; i < n; i++)
        if (dist2(p[i], p[i0]) > best) best = dist2(p[i], p[i0]), i1 = i;
    if (i1 < 0 || best <= eps * eps) return 0;
    int i2 = -1;
    best = 0;
    double e[3] = {p[i1][0] - p[i0][0], p[i1][1] - p[i0][1], p[i1][2] - p[i0][2]}, el = sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    for (int i = 0; i < n; i++) {
        double d[3] = {p[i][0] - p[i0][0], p[i][1] - p[i0][1], p[i][2] - p[i0][2]};
        double c[3] = {d[1] * e[2] - d[2] * e[1], d[2] * e[0] - d[0] * e[2], d[0] * e[1] - d[1] * e[0]};
        double l = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]) / el;
        if (l > best) best = l, i2 = i;
    }
    if (i2 < 0 || best <= eps) return 0;
    HFace base = {{i0, i1, i2}, {0}, 0, true};
    face_plane(p, &base);
    int i3 = -1;
    best = 0;
    for (int i = 0; i < n; i++)
        if (fabs(face_dist(&base, p[i])) > best) best = fabs(face_dist(&base, p[i])), i3 = i;
    if (i3 < 0 || best <= eps) return 0;
    fv->n = 0;
    if (face_dist(&base, p[i3]) > 0) {
        add_face(fv, p, i0, i2, i1);
        add_face(fv, p, i0, i1, i3);
        add_face(fv, p, i1, i2, i3);
        add_face(fv, p, i2, i0, i3);
    } else {
        add_face(fv, p, i0, i1, i2);
        add_face(fv, p, i0, i3, i1);
        add_face(fv, p, i1, i3, i2);
        add_face(fv, p, i2, i3, i0);
    }
    int *hz = NULL, nhz = 0, caphz = 0;
    for (int i = 0; i < n; i++) {
        if (i == i0 || i == i1 || i == i2 || i == i3) continue;
        int nvis = 0;
        for (int f = 0; f < fv->n; f++)
            if (fv->f[f].alive && face_dist(&fv->f[f], p[i]) > eps) nvis++;
        if (!nvis) continue;
        /* the horizon: edges of visible faces whose reverse is not on a visible face */
        nhz = 0;
        for (int f = 0; f < fv->n; f++) {
            HFace *F = &fv->f[f];
            if (!F->alive || !(face_dist(F, p[i]) > eps)) continue;
            for (int k = 0; k < 3; k++) {
                int a = F->v[k], b = F->v[(k + 1) % 3];
                bool inner = false;
                for (int g = 0; g < fv->n && !inner; g++) {
                    HFace *G = &fv->f[g];
                    if (g == f || !G->alive || !(face_dist(G, p[i]) > eps)) continue;
                    for (int m = 0; m < 3; m++) inner |= G->v[m] == b && G->v[(m + 1) % 3] == a;
                }
                if (inner) continue;
                if (nhz + 2 > caphz) caphz = caphz ? caphz * 2 : 64, hz = realloc(hz, (size_t)caphz * sizeof *hz);
                hz[nhz++] = a, hz[nhz++] = b;
            }
        }
        for (int f = 0; f < fv->n; f++)
            if (fv->f[f].alive && face_dist(&fv->f[f], p[i]) > eps) fv->f[f].alive = false;
        for (int k = 0; k < nhz; k += 2) add_face(fv, p, hz[k], hz[k + 1], i);
        /* compact the dead faces away now and then */
        if (fv->n > 4 * n + 64) {
            int m = 0;
            for (int f = 0; f < fv->n; f++)
                if (fv->f[f].alive) fv->f[m++] = fv->f[f];
            fv->n = m;
        }
    }
    free(hz);
    int m = 0;
    for (int f = 0; f < fv->n; f++)
        if (fv->f[f].alive) fv->f[m++] = fv->f[f];
    fv->n = m;
    return manifold(fv);
}

/* a convex polygon of merged coplanar facets: its boundary loop (outward winding) */
typedef struct {
    int *v;
    int n;
    double nrm[3];
} Poly;

/* the facets grouped into polygons (union of edge-adjacent faces within the tolerance of the group's first
   face's plane), the boundary loops extracted. 0 when a loop is not simple. */
static int merge_polys(const double (*p)[3], const FaceVec *fv, double tol, Poly **out, int *npoly)
{
    int nf = fv->n;
    int *grp = malloc((size_t)nf * sizeof *grp);
    for (int i = 0; i < nf; i++) grp[i] = -1;
    int ng = 0;
    int *stack = malloc((size_t)nf * sizeof *stack);
    for (int s = 0; s < nf; s++) {
        if (grp[s] >= 0) continue;
        const HFace *seed = &fv->f[s];
        int sp = 0;
        stack[sp++] = s;
        grp[s] = ng;
        while (sp) {
            const HFace *F = &fv->f[stack[--sp]];
            for (int k = 0; k < 3; k++) {
                int a = F->v[k], b = F->v[(k + 1) % 3];
                for (int g = 0; g < nf; g++) {
                    if (grp[g] >= 0) continue;
                    const HFace *G = &fv->f[g];
                    bool adj = false;
                    for (int m = 0; m < 3; m++) adj |= G->v[m] == b && G->v[(m + 1) % 3] == a;
                    if (!adj) continue;
                    double c = G->n[0] * seed->n[0] + G->n[1] * seed->n[1] + G->n[2] * seed->n[2];
                    bool cop = c > 1 - 1e-9;
                    for (int m = 0; m < 3 && cop; m++) cop = fabs(face_dist(seed, p[G->v[m]])) <= tol;
                    if (!cop) continue;
                    grp[g] = ng;
                    stack[sp++] = g;
                }
            }
        }
        ng++;
    }
    free(stack);
    Poly *polys = calloc((size_t)ng, sizeof *polys);
    int ok = 1;
    int *be = malloc((size_t)nf * 3 * 2 * sizeof *be);
    for (int g = 0; g < ng && ok; g++) {
        /* boundary edges of the group, in the faces' winding */
        int nbe = 0, seed = -1;
        for (int f = 0; f < nf; f++) {
            if (grp[f] != g) continue;
            if (seed < 0) seed = f;
            for (int k = 0; k < 3; k++) {
                int a = fv->f[f].v[k], b = fv->f[f].v[(k + 1) % 3];
                bool inner = false;
                for (int h = 0; h < nf && !inner; h++) {
                    if (grp[h] != g || h == f) continue;
                    for (int m = 0; m < 3; m++) inner |= fv->f[h].v[m] == b && fv->f[h].v[(m + 1) % 3] == a;
                }
                if (!inner) be[nbe * 2] = a, be[nbe * 2 + 1] = b, nbe++;
            }
        }
        memcpy(polys[g].nrm, fv->f[seed].n, sizeof polys[g].nrm);
        polys[g].v = malloc((size_t)(nbe + 1) * sizeof(int));
        /* chain them from the first one */
        int cur = be[0], start = cur, cnt = 0;
        do {
            int nxt = -1, found = 0;
            for (int k = 0; k < nbe; k++)
                if (be[k * 2] == cur) nxt = be[k * 2 + 1], found++;
            if (found != 1 || cnt >= nbe) {
                ok = 0;
                break;
            }
            polys[g].v[cnt++] = cur;
            cur = nxt;
        } while (cur != start);
        if (ok && cnt != nbe) ok = 0;
        polys[g].n = cnt;
    }
    free(be);
    free(grp);
    *out = polys;
    *npoly = ng;
    return ok;
}

static void free_polys(Poly *pl, int n)
{
    for (int i = 0; i < n; i++) free(pl[i].v);
    free(pl);
}

/* is vertex v of the loop a corner (not collinear with its loop neighbours)? */
static bool loop_corner(const double (*p)[3], const Poly *pl, int i, double tol)
{
    const double *a = p[pl->v[(i + pl->n - 1) % pl->n]], *b = p[pl->v[i]], *c = p[pl->v[(i + 1) % pl->n]];
    double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    double x[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
    double lw = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    /* the distance of b from the line a-c */
    return lw > 0 && sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]) / lw > tol;
}

/* 0x1003a1e0 / 0x1003a2d0: A = sum n . ((v_k+1 - v_k) x (v_k - v_0)), L2 = sum |v_k+1 - v_k|^2; a sliver when
   |A| < sqrt(L2) * 0.005 (0x10063828) */
static bool sliver(const double (*p)[3], const Poly *pl)
{
    double A = 0, L2 = 0;
    const double *v0 = p[pl->v[0]];
    for (int k = 0; k < pl->n; k++) {
        const double *a = p[pl->v[k]], *b = p[pl->v[(k + 1) % pl->n]];
        double d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, r[3] = {a[0] - v0[0], a[1] - v0[1], a[2] - v0[2]};
        double c[3] = {d[1] * r[2] - d[2] * r[1], d[2] * r[0] - d[0] * r[2], d[0] * r[1] - d[1] * r[0]};
        A += pl->nrm[0] * c[0] + pl->nrm[1] * c[1] + pl->nrm[2] * c[2];
        L2 += d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    }
    if (A < 0) A = -A;
    return A < sqrt(L2) * 0.005;
}

/* ---- the ledge generator (0x10048b60 sizes, 0x10048ed0 writes) ----
   tri[i] = vertex indices into pts in ring order (p0 -> p1, p1 -> p2, p2 -> p0), pierce[i] = the pierce
   partner's index. Points get indices in first-appearance order; opposite = the twin edge's slot minus this
   edge's slot (4i + k + 1). NULL when an edge has no twin. */
IvpCompactLedge *ivp_ledge_generate(const float (*pts)[3], const int (*tri)[3], const int *pierce, uint32_t ntri)
{
    int maxv = 0;
    for (uint32_t i = 0; i < ntri; i++)
        for (int k = 0; k < 3; k++)
            if (tri[i][k] > maxv) maxv = tri[i][k];
    int *idx = malloc((size_t)(maxv + 1) * sizeof *idx), npts = 0;
    for (int i = 0; i <= maxv; i++) idx[i] = -1;
    int *order = malloc((size_t)(maxv + 1) * sizeof *order);
    for (uint32_t i = 0; i < ntri; i++)
        for (int k = 0; k < 3; k++)
            if (idx[tri[i][k]] < 0) order[npts] = tri[i][k], idx[tri[i][k]] = npts++;
    size_t size = (size_t)(npts + 1 + (int)ntri) * 16;
    IvpCompactLedge *l = ivp_aligned_alloc(size);
    memset(l, 0, size);
    l->c_point_offset = (int32_t)(0x10 + 16 * ntri);
    l->ledgetree_node_offset = 0;
    l->flags = ((uint32_t)(1 + ntri + (uint32_t)npts) << 8) | 4;
    l->n_triangles = (int16_t)ntri;
    IvpCompactTriangle *t = (IvpCompactTriangle *)(l + 1);
    bool ok = true;
    for (uint32_t i = 0; i < ntri && ok; i++) {
        t[i].word = (i & 0xfff) | ((uint32_t)(pierce[i] & 0xfff) << 12);
        for (int k = 0; k < 3; k++) {
            int a = tri[i][k], b = tri[i][(k + 1) % 3];
            int slot = -1;
            for (uint32_t j = 0; j < ntri && slot < 0; j++)
                for (int m = 0; m < 3; m++)
                    if (tri[j][m] == b && tri[j][(m + 1) % 3] == a) slot = (int)(4 * j) + m + 1;
            if (slot < 0) {
                ok = false;
                break;
            }
            int self = (int)(4 * i) + k + 1;
            t[i].e[k] = (uint32_t)idx[a] | ((uint32_t)((slot - self) & 0x7fff) << 16);
        }
    }
    IvpPolyPoint *pp = (IvpPolyPoint *)((char *)l + l->c_point_offset);
    for (int i = 0; i < npts; i++) {
        memcpy(pp[i].k, pts[order[i]], 12);
        pp[i].hesse = 0;
    }
    free(idx);
    free(order);
    if (!ok) {
        ivp_aligned_free(l);
        return NULL;
    }
    return l;
}

/* 0x10048a60: for each triangle in list order without a partner, the triangle with the most negative
   n_T . n_U below -1e-6 (the first on ties); both are set, overwriting U's old partner */
static void assign_pierce(const double (*nrm)[3], int n, int *pierce)
{
    for (int i = 0; i < n; i++) pierce[i] = -1;
    for (int i = 0; i < n; i++) {
        if (pierce[i] >= 0) continue;
        int best = -1;
        double bv = -1e-6;
        for (int j = 0; j < n; j++) {
            double d = nrm[i][0] * nrm[j][0] + nrm[i][1] * nrm[j][1] + nrm[i][2] * nrm[j][2];
            if (d < bv) bv = d, best = j;
        }
        if (best < 0) best = i;      /* "no valid pierce index" */
        pierce[i] = best;
        pierce[best] = i;
    }
}

/* 0x1003a750: the hull ledge of a point soup (deduplicated by the caller) */
IvpCompactLedge *ivp_ledge_convex_hull(const double (*p)[3], uint32_t n)
{
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (uint32_t i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) {
            if (p[i][k] < lo[k]) lo[k] = p[i][k];
            if (p[i][k] > hi[k]) hi[k] = p[i][k];
        }
    double ext = 0, mag = 0;
    for (int k = 0; k < 3; k++) {
        if (hi[k] - lo[k] > ext) ext = hi[k] - lo[k];
        if (fabs(lo[k]) > mag) mag = fabs(lo[k]);
        if (fabs(hi[k]) > mag) mag = fabs(hi[k]);
    }
    /* tolerances: the f32 resolution of the input, relative to its size */
    double eps = (ext + mag) * 1e-9, tol = (ext + mag) * 2e-7;
    FaceVec fv = {0};
    if (!hull3d(p, (int)n, eps, &fv)) {
        free(fv.f);
        return NULL;
    }
    Poly *pl;
    int np;
    IvpCompactLedge *l = NULL;
    if (!merge_polys(p, &fv, tol, &pl, &np)) {
        /* not simple: keep the hull's own triangles, one polygon each */
        free_polys(pl, np);
        np = fv.n;
        pl = calloc((size_t)np, sizeof *pl);
        for (int i = 0; i < np; i++) {
            pl[i].v = malloc(3 * sizeof(int));
            memcpy(pl[i].v, fv.f[i].v, 3 * sizeof(int));
            pl[i].n = 3;
            memcpy(pl[i].nrm, fv.f[i].n, sizeof pl[i].nrm);
        }
    }
    free(fv.f);
    /* 0x1003a330: a sliver polygon drops one vertex (neither of the two farthest apart) and the hull is
       rebuilt without it */
    bool *corner = calloc(n, sizeof *corner);
    for (int g = 0; g < np; g++)
        for (int i = 0; i < pl[g].n; i++)
            if (loop_corner(p, &pl[g], i, tol)) corner[pl[g].v[i]] = true;
    for (int g = 0; g < np; g++) {
        Poly *q = &pl[g];
        int m = 0;
        for (int i = 0; i < q->n; i++)
            if (corner[q->v[i]]) q->v[m++] = q->v[i];
        q->n = m;
    }
    for (int g = 0; g < np; g++) {
        if (pl[g].n < 3 || !sliver(p, &pl[g])) continue;
        int a = 0, b = 0;
        double best = -1;
        for (int i = 0; i < pl[g].n; i++)
            if (dist2(p[pl[g].v[i]], p[pl[g].v[0]]) > best) best = dist2(p[pl[g].v[i]], p[pl[g].v[0]]), a = i;
        best = -1;
        for (int i = 0; i < pl[g].n; i++)
            if (dist2(p[pl[g].v[i]], p[pl[g].v[a]]) > best) best = dist2(p[pl[g].v[i]], p[pl[g].v[a]]), b = i;
        int drop = -1;
        for (int i = 0; i < pl[g].n && drop < 0; i++)
            if (i != a && i != b) drop = pl[g].v[i];
        if (drop < 0) continue;
        double (*q)[3] = malloc(n * sizeof *q);
        uint32_t m = 0;
        for (uint32_t i = 0; i < n; i++)
            if ((int)i != drop) memcpy(q[m++], p[i], sizeof *q);
        IvpCompactLedge *r = m == 3 ? ivp_ledge_triangle(q[0], q[1], q[2]) : m < 3 ? NULL : ivp_ledge_convex_hull((const double (*)[3])q, m);
        free(q);
        free(corner);
        free_polys(pl, np);
        return r;
    }
    /* fan every polygon (outward winding follows the loop), list in reverse creation order */
    int ntri = 0;
    for (int g = 0; g < np; g++) ntri += pl[g].n >= 3 ? pl[g].n - 2 : 0;
    int (*tri)[3] = malloc((size_t)(ntri ? ntri : 1) * sizeof *tri);
    int t = ntri;
    for (int g = 0; g < np; g++)
        for (int i = 1; i + 1 < pl[g].n; i++) {
            t--;
            tri[t][0] = pl[g].v[0], tri[t][1] = pl[g].v[i], tri[t][2] = pl[g].v[i + 1];
        }
    if (ntri >= 4 && ntri <= 4095) {
        double (*nrm)[3] = malloc((size_t)ntri * sizeof *nrm);
        for (int i = 0; i < ntri; i++) {
            HFace f = {{tri[i][0], tri[i][1], tri[i][2]}, {0}, 0, true};
            face_plane(p, &f);
            memcpy(nrm[i], f.n, sizeof nrm[i]);
        }
        int *pierce = malloc((size_t)ntri * sizeof *pierce);
        assign_pierce((const double (*)[3])nrm, ntri, pierce);
        float (*fp)[3] = malloc(n * sizeof *fp);
        for (uint32_t i = 0; i < n; i++)
            for (int k = 0; k < 3; k++) fp[i][k] = (float)p[i][k];
        l = ivp_ledge_generate((const float (*)[3])fp, (const int (*)[3])tri, pierce, (uint32_t)ntri);
        free(fp);
        free(pierce);
        free(nrm);
    }
    free(tri);
    free(corner);
    free_polys(pl, np);
    return l;
}

