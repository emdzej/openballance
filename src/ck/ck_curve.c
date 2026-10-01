#include "ck_curve.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int by_x(const void *a, const void *b)
{
    float x = ((const CkCurvePoint *)a)->pos[0], y = ((const CkCurvePoint *)b)->pos[0];
    return x < y ? -1 : x > y;
}

/* Tangents of point i from the chords to its neighbours (TCB). The in tangent is then rescaled so its x
   is the chord to the previous point, the out tangent so its x is the chord to the next one; the ends
   use 1e-4 (first in) and 1e-3 (last out). Zero-length chords count as 1e-4. */
static void tcb(CkCurvePoint *p, const float *dp, const float *dn)
{
    float t = p->tension, c = p->continuity, b = p->bias;
    if (!(p->flags & CK_CURVEPOINT_USER_TANGENTS)) {
        float oa = (1 + c) * (1 + b) * (1 - t), ob = (1 - t) * (1 - c) * (1 - b);   /* out: prev, next */
        float ia = (1 + b) * (1 - t) * (1 - c), ib = (1 + c) * (1 - t) * (1 - b);   /* in: prev, next */
        for (int k = 0; k < 2; k++) {
            p->out[k] = (oa * dp[k] + ob * dn[k]) * 0.5f;
            p->in[k] = (ia * dp[k] + ib * dn[k]) * 0.5f;
        }
    }
}

static void rescale(float *tan, float x)
{
    if (tan[0] != 0) {
        float s = x / tan[0];
        tan[0] *= s;
        tan[1] *= s;
    }
}

/* CK2dCurve::UpdatePointsAndTangents 0x24001600 */
static void update_tangents(CkCurve2d *c)
{
    uint32_t n = c->n;
    if (n < 2) return;
    CkCurvePoint *p = c->p;
    float zero[2] = {0, 0};
    for (uint32_t i = 0; i < n; i++) {
        float dp[2] = {0, 0}, dn[2] = {0, 0};
        if (i > 0) dp[0] = p[i].pos[0] - p[i - 1].pos[0], dp[1] = p[i].pos[1] - p[i - 1].pos[1];
        if (i + 1 < n) dn[0] = p[i + 1].pos[0] - p[i].pos[0], dn[1] = p[i + 1].pos[1] - p[i].pos[1];
        tcb(&p[i], i > 0 ? dp : zero, i + 1 < n ? dn : zero);
        float xp = dp[0] == 0 ? 1e-4f : dp[0], xn = dn[0] == 0 ? 1e-4f : dn[0];
        rescale(p[i].in, i == 0 ? 1e-4f : xp);
        rescale(p[i].out, i + 1 == n ? 1e-3f : xn);
    }
}

/* CK2dCurve::Read 0x240013e4. Data version 0: identifier 0x40 fitting coefficient, 0x3f800 point count
   then per point an object ID and a CK2dCurvePoint sub-chunk (CK2dCurvePoint::Read 0x24002646:
   0x4000 id, user tangents, linear, position; 0x800 TCB; 0x1000 in and out tangents). Current format,
   identifier 0x100: an object array whose size is the point count, the fitting coefficient, then per
   point its flags; only points with flag 0x10000000 (cleared) carry data and are kept: position, TCB,
   2 unused floats, in and out tangents. Update (0x24001084) then sorts the points by x and computes the
   tangents. */
CkCurve2d *ck_curve_read(const CkChunk *ch, uint32_t *size)
{
    CkReader r;
    ck_reader_init(&r, ch);
    uint32_t n = 0;
    float fitting = 0;
    CkCurvePoint *pts = NULL;
    if (ch->data_version < 1) {
        if (ck_seek(&r, 0x40)) fitting = ck_read_float(&r);
        if (ck_seek(&r, 0x3f800)) {
            uint32_t count = ck_read_dword(&r);
            if (count > 4096) count = 0;
            pts = calloc(count ? count : 1, sizeof *pts);
            for (uint32_t i = 0; i < count; i++) {
                ck_read_object(&r);
                CkChunk sub;
                if (!ck_read_subchunk(&r, &sub)) continue;
                CkCurvePoint *p = &pts[i];
                CkReader s;
                ck_reader_init(&s, &sub);
                if (ck_seek(&s, 0x4000)) {
                    ck_read_object(&s);
                    if (ck_read_dword(&s)) p->flags |= CK_CURVEPOINT_USER_TANGENTS;
                    if (ck_read_dword(&s)) p->flags |= CK_CURVEPOINT_LINEAR;
                    p->pos[0] = ck_read_float(&s), p->pos[1] = ck_read_float(&s);
                }
                if (ck_seek(&s, 0x800))
                    p->tension = ck_read_float(&s), p->continuity = ck_read_float(&s), p->bias = ck_read_float(&s);
                if (ck_seek(&s, 0x1000)) {
                    p->in[0] = ck_read_float(&s), p->in[1] = ck_read_float(&s);
                    p->out[0] = ck_read_float(&s), p->out[1] = ck_read_float(&s);
                }
            }
            n = count;
        }
    }
    if (ck_seek(&r, 0x100)) {
        uint32_t count = ck_read_dword(&r);
        if (count > 4096) count = 0;
        ck_skip(&r, count);
        fitting = ck_read_float(&r);
        free(pts);
        pts = calloc(count ? count : 1, sizeof *pts);
        n = 0;
        for (uint32_t i = 0; i < count; i++) {
            CkCurvePoint *p = &pts[n];
            p->flags = ck_read_dword(&r);
            if (!(p->flags & 0x10000000u)) continue;
            p->flags &= ~0x10000000u;
            p->pos[0] = ck_read_float(&r), p->pos[1] = ck_read_float(&r);
            p->tension = ck_read_float(&r), p->continuity = ck_read_float(&r), p->bias = ck_read_float(&r);
            ck_read_float(&r), ck_read_float(&r);
            p->in[0] = ck_read_float(&r), p->in[1] = ck_read_float(&r);
            p->out[0] = ck_read_float(&r), p->out[1] = ck_read_float(&r);
            n++;
        }
    }
    if (!pts) return NULL;
    CkCurve2d *c = malloc(sizeof *c + n * sizeof *pts);
    c->n = n;
    c->fitting = fitting;
    memcpy(c->p, pts, n * sizeof *pts);
    free(pts);
    qsort(c->p, n, sizeof *c->p, by_x);
    update_tangents(c);
    *size = (uint32_t)(sizeof *c + n * sizeof *pts);
    return c;
}

/* CK2dCurve::GetY 0x24001b36: the segment from the point before the first point with x >= the given x.
   On a Hermite segment the parameter is found by bisection (at most 1000 steps, until x is within 1e-5);
   the first step evaluates at the linear estimate, every later one at the midpoint of the bracket, which
   is updated from the previous evaluation. x beyond the last point gives 0. */
float ck_curve_get_y(const CkCurve2d *c, float x)
{
    if (x > 1) x = 1;
    if (x < 0) x = 0;
    if (!c || c->n < 2) return 0;
    uint32_t j = 0;
    while (j < c->n && !(x <= c->p[j].pos[0])) j++;
    if (j == c->n) return 0;
    const CkCurvePoint *a = &c->p[j ? j - 1 : 0], *b = &c->p[j];
    float y = 0;
    if (a->pos[0] == b->pos[0]) {
        y = a->pos[1];
    } else if (!(a->flags & CK_CURVEPOINT_LINEAR)) {
        float s = (x - a->pos[0]) / (b->pos[0] - a->pos[0]), lo = 0, hi = 1, px;
        int it = 0;
        do {
            if (it > 999) break;
            it++;
            float h3 = (s - 2) * s + 1, h2 = 3 - (s + s), h1 = ((s + s) - 3) * s * s + 1;
            px = h1 * a->pos[0] + h2 * b->pos[0] * s * s + h3 * a->out[0] * s + (s - 1) * b->in[0] * s * s;
            y = h1 * a->pos[1] + h2 * b->pos[1] * s * s + h3 * a->out[1] * s + (s - 1) * b->in[1] * s * s;
            s = (hi + lo) * 0.5f;
            if (px < x) lo = s;
            if (x < px) hi = s;
        } while (fabs((double)(px - x)) > 1e-5);
    } else {
        y = (x - a->pos[0]) * (b->pos[1] - a->pos[1]) / (b->pos[0] - a->pos[0]) + a->pos[1];
    }
    if (y > 1) return 1;
    if (y < 0) return 0;
    return y;
}
