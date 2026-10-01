#include "ck_anim.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

size_t ck_anim_obj_size(uint32_t cid)
{
    if (cid == 36) return sizeof(Ck3dCurvePoint);
    if (cid == 43) return sizeof(CkCurve3d);
    if (cid == 15) return sizeof(CkObjectAnimation);
    if (cid == 18) return sizeof(CkKeyedAnimation);
    return 0;
}

void ck_anim_free(CkObj *o)
{
    if (o->cid == 43) free(((CkCurve3d *)o)->points.v);
    if (o->cid == 18) free(((CkKeyedAnimation *)o)->anims.v);
    if (o->cid == 15) {
        CkObjectAnimation *a = (CkObjectAnimation *)o;
        free(a->pos), free(a->rot), free(a->scale);
    }
}

/* Controller key blocks: count, then the keys (4 dwords time + vector, 5 time + quaternion) */
static void *read_keys(CkReader *r, uint32_t dwords, uint32_t *n)
{
    uint32_t count = ck_read_dword(r);
    if (count > 1u << 20) count = 0;
    float *k = malloc((count ? count : 1) * dwords * sizeof(float));
    for (uint32_t i = 0; i < count * dwords; i++) k[i] = ck_read_float(r);
    *n = count;
    return k;
}

void ck_anim_load(CkObj *o, const CkChunk *c, CkId (*remap)(const void *, uint32_t), const void *L)
{
    CkReader r;
    ck_reader_init(&r, c);
    if (o->cid == 36 && c->data_version >= 5 && ck_seek(&r, 0x10000000)) {
        /* CKCurvePoint load FUN_1001701d: curve, not_tcb, linear, tension, continuity, bias, in, out */
        Ck3dCurvePoint *p = (Ck3dCurvePoint *)o;
        p->curve = remap(L, ck_read_object(&r));
        p->user_tangents = ck_read_int(&r) != 0;
        p->linear = ck_read_int(&r) != 0;
        p->tension = ck_read_float(&r), p->continuity = ck_read_float(&r), p->bias = ck_read_float(&r);
        p->in.x = ck_read_float(&r), p->in.y = ck_read_float(&r), p->in.z = ck_read_float(&r);
        p->out.x = ck_read_float(&r), p->out.y = ck_read_float(&r), p->out.z = ck_read_float(&r);
    }
    if (o->cid == 18) {
        /* CKAnimation load FUN_10047adc: 0x40 length, 0x80 an object array then the root entity; CKKeyedAnimation
           FUN_10049e10: 0x1000 the object animations, 0x100000 root animation index and merge factor */
        CkKeyedAnimation *k = (CkKeyedAnimation *)o;
        if (ck_seek(&r, 0x40)) k->length = ck_read_float(&r);
        if (ck_seek(&r, 0x80)) {
            uint32_t n = ck_read_dword(&r);
            for (uint32_t i = 0; i < n && !r.error; i++) ck_read_object(&r);
            k->root_entity = remap(L, ck_read_object(&r));
        }
        if (ck_seek(&r, 0x1000)) {
            uint32_t n = ck_read_dword(&r);
            for (uint32_t i = 0; i < n && i < 4096 && !r.error; i++) {
                CkId id = remap(L, ck_read_object(&r));
                if (id) ck_ids_push(&k->anims, id);
            }
        }
        if (ck_seek(&r, 0x100000)) k->root_index = ck_read_int(&r), k->merge = ck_read_float(&r);
    }
    if (o->cid == 43) {
        /* CKCurve load FUN_10016342: control points, fitting coefficient, step count, open */
        CkCurve3d *cv = (CkCurve3d *)o;
        cv->open = true;
        cv->steps = 100;
        cv->dirty = true;
        if (c->data_version >= 5 && ck_seek(&r, 0xffc00000)) {
            uint32_t n = ck_read_dword(&r);
            for (uint32_t i = 0; i < n && i < 4096 && !r.error; i++) {
                CkId id = remap(L, ck_read_object(&r));
                if (id) ck_ids_push(&cv->points, id);
            }
            cv->fitting = ck_read_float(&r);
            cv->steps = ck_read_dword(&r);
            cv->open = ck_read_dword(&r) != 0;
        }
    }
    if (o->cid == 15 && c->data_version >= 1 && ck_seek(&r, 0x4000000)) {
        /* CKObjectAnimation load FUN_10058c51: vector, 4 floats, flags, entity, length, [merge], then the
           controller list {type, size, keys} ending with type 0; the linear position (0x637c4301),
           rotation (0x49ed4002) and scale (0x654a3a04) controllers are kept, the others skipped */
        CkObjectAnimation *a = (CkObjectAnimation *)o;
        a->offset.x = ck_read_float(&r), a->offset.y = ck_read_float(&r), a->offset.z = ck_read_float(&r);
        ck_skip(&r, 4);
        a->flags = ck_read_dword(&r);
        a->entity = remap(L, ck_read_object(&r));
        a->length = ck_read_float(&r);
        if (a->flags & 0x80) ck_skip(&r, 3);
        while (!r.error) {
            uint32_t type = ck_read_dword(&r);
            if (!type || r.error) break;
            uint32_t size = ck_read_dword(&r), at = r.pos;
            if (type == 0x637c4301 || type == 1) free(a->pos), a->pos = read_keys(&r, 4, &a->npos);
            else if (type == 0x49ed4002 || type == 2) free(a->rot), a->rot = read_keys(&r, 5, &a->nrot);
            else if (type == 0x654a3a04 || type == 4) free(a->scale), a->scale = read_keys(&r, 4, &a->nscale);
            r.pos = at;
            ck_skip(&r, size);
        }
    }
}

/* ---- curves ---- */

static CkVec3 v3(float x, float y, float z) { return (CkVec3){x, y, z}; }
static CkVec3 vadd(CkVec3 a, CkVec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static CkVec3 vsub(CkVec3 a, CkVec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static CkVec3 vmul(CkVec3 a, float k) { return v3(a.x * k, a.y * k, a.z * k); }
static float vlen(CkVec3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }

static CkVec3 hermite(CkVec3 p0, CkVec3 t0, CkVec3 p1, CkVec3 t1, float s)
{
    float s2 = s * s, s3 = s2 * s;
    return vadd(vadd(vmul(p0, 2 * s3 - 3 * s2 + 1), vmul(p1, -2 * s3 + 3 * s2)), vadd(vmul(t0, s3 - 2 * s2 + s), vmul(t1, s3 - s2)));
}

/* FUN_1001603e */
static int32_t wrap(const CkCurve3d *c, int32_t i)
{
    int32_t n = (int32_t)c->points.n;
    if (!c->open) return (i % n + n) % n;
    return i < 0 ? 0 : i >= n ? n - 1 : i;
}

static Ck3dCurvePoint *point(CkContext *ctx, const CkCurve3d *c, int32_t i)
{
    CkObj *o = ck_obj(ctx, c->points.v[wrap(c, i)]);
    return o && o->cid == 36 ? (Ck3dCurvePoint *)o : NULL;
}

/* p in the frame of the matrix m (rows: axes, translation): (p - t) * inverse(axes) */
static CkVec3 to_local(const float m[4][4], CkVec3 p)
{
    float a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], k = m[2][2];
    float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
    if (det == 0) return p;
    float inv[3][3] = {{(e * k - f * h) / det, (c * h - b * k) / det, (b * f - c * e) / det},
                       {(f * g - d * k) / det, (a * k - c * g) / det, (c * d - a * f) / det},
                       {(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det}};
    float q[3] = {p.x - m[3][0], p.y - m[3][1], p.z - m[3][2]};
    return v3(q[0] * inv[0][0] + q[1] * inv[1][0] + q[2] * inv[2][0], q[0] * inv[0][1] + q[1] * inv[1][1] + q[2] * inv[2][1],
              q[0] * inv[0][2] + q[1] * inv[1][2] + q[2] * inv[2][2]);
}

/* CKCurve::Update FUN_1001586d: curve-local positions, TCB tangents (FUN_10013bdc, Kochanek-Bartels,
   not rescaled), fitting, arc lengths (100 chords per Hermite segment) */
static void curve_update(CkContext *ctx, CkCurve3d *c)
{
    int32_t n = (int32_t)c->points.n;
    for (int32_t i = 0; i < n; i++) {
        Ck3dCurvePoint *p = point(ctx, c, i);
        if (p) p->raw = p->pos = to_local(c->e.world, v3(p->e.world[3][0], p->e.world[3][1], p->e.world[3][2]));
    }
    for (int32_t i = 0; i < n; i++) {
        Ck3dCurvePoint *p = point(ctx, c, i), *pp = point(ctx, c, i - 1), *pn = point(ctx, c, i + 1);
        if (!p || !pp || !pn || p->user_tangents) continue;
        CkVec3 dp = (c->open && i == 0) ? v3(0, 0, 0) : vsub(p->raw, pp->raw);
        CkVec3 dn = (c->open && i == n - 1) ? v3(0, 0, 0) : vsub(pn->raw, p->raw);
        float t = p->tension, cc = p->continuity, b = p->bias;
        p->out = vmul(vadd(vmul(dn, (1 - t) * (1 - cc) * (1 - b)), vmul(dp, (1 - t) * (1 + cc) * (1 + b))), 0.5f);
        p->in = vmul(vadd(vmul(dn, (1 - t) * (1 + cc) * (1 - b)), vmul(dp, (1 - t) * (1 - cc) * (1 + b))), 0.5f);
    }
    if (c->fitting > 0)
        for (int32_t i = 0; i < n; i++) {
            Ck3dCurvePoint *p = point(ctx, c, i), *pp = point(ctx, c, i - 1), *pn = point(ctx, c, i + 1);
            if (p && pp && pn) p->pos = vadd(p->raw, vmul(vsub(vmul(vadd(pp->raw, pn->raw), 0.5f), p->raw), c->fitting));
        }
    float len = 0;
    int32_t segs = c->open ? n - 1 : n;
    for (int32_t i = 0; i < segs; i++) {
        Ck3dCurvePoint *p = point(ctx, c, i), *q = point(ctx, c, i + 1);
        if (!p || !q) continue;
        p->length = len;
        if (p->linear) {
            len += vlen(vsub(q->pos, p->pos));
        } else {
            CkVec3 prev = p->pos;
            for (int k = 1; k <= 100; k++) {
                CkVec3 s = hermite(p->pos, p->out, q->pos, q->in, (float)k / 100);
                len += vlen(vsub(s, prev));
                prev = s;
            }
        }
        if (i != n - 1) q->length = len;
    }
    c->length = len;
    c->dirty = false;
}

bool ck_curve3d_get_pos(CkContext *ctx, CkCurve3d *c, float step, CkVec3 *pos, CkVec3 *dir)
{
    if (c->dirty) curve_update(ctx, c);
    if (!c->open) {
        if (step > 1) step -= (float)(int32_t)step;
        while (step < 0) step += 1;
    } else {
        step = step < 0 ? 0 : step > 1 ? 1 : step;
    }
    int32_t n = (int32_t)c->points.n;
    if (n < 2 || !pos) return false;
    float L = step * c->length;
    int32_t next = 0;
    while (next < n && !(point(ctx, c, next) && point(ctx, c, next)->length > L)) next++;
    if (next == n) next = c->open ? n - 1 : 0;
    int32_t prev = next - 1;
    if (prev < 0) prev = c->open ? 0 : n - 1;
    Ck3dCurvePoint *a = point(ctx, c, prev), *b = point(ctx, c, next);
    if (!a || !b) return false;
    float L0 = a->length, L1 = next == 0 ? c->length : b->length, s = L1 == L0 ? 0 : (L - L0) / (L1 - L0);
    CkVec3 p, d;
    if (a->linear) {
        p = vadd(a->pos, vmul(vsub(b->pos, a->pos), s));
        d = vsub(b->pos, a->pos);
    } else {
        p = hermite(a->pos, a->out, b->pos, b->in, s);
        d = vsub(hermite(a->pos, a->out, b->pos, b->in, s + 0.01f), p);
    }
    const float (*m)[4] = c->e.world;
    *pos = v3(p.x * m[0][0] + p.y * m[1][0] + p.z * m[2][0] + m[3][0], p.x * m[0][1] + p.y * m[1][1] + p.z * m[2][1] + m[3][1],
              p.x * m[0][2] + p.y * m[1][2] + p.z * m[2][2] + m[3][2]);
    if (dir) {
        CkVec3 w = v3(d.x * m[0][0] + d.y * m[1][0] + d.z * m[2][0], d.x * m[0][1] + d.y * m[1][1] + d.z * m[2][1],
                      d.x * m[0][2] + d.y * m[1][2] + d.z * m[2][2]);
        float l = vlen(w);
        *dir = l > 0 ? vmul(w, 1 / l) : w;
    }
    return true;
}

/* ---- object animations ---- */

/* linear controllers (FUN_1004b8df / FUN_1004c150): the segment around t by binary search */
static bool find_segment(const float *keys, uint32_t n, uint32_t stride, float t, uint32_t *i0, uint32_t *i1, float *f)
{
    if (!n) return false;
    if (t <= keys[0]) return *i0 = *i1 = 0, *f = 0, true;
    if (t >= keys[(n - 1) * stride]) return *i0 = *i1 = n - 1, *f = 0, true;
    uint32_t lo = 0, hi = n - 1;
    while (lo < hi - 1) {
        uint32_t mid = (lo + hi) >> 1;
        if (t <= keys[mid * stride]) hi = mid;
        else lo = mid;
    }
    float t0 = keys[(hi - 1) * stride], t1 = keys[hi * stride];
    *i0 = hi - 1, *i1 = hi, *f = (t - t0) / (t1 - t0);
    return true;
}

static bool eval_vec(const CkPosKey *k, uint32_t n, float t, CkVec3 *out)
{
    uint32_t a, b;
    float f;
    if (!find_segment((const float *)k, n, 4, t, &a, &b, &f)) return false;
    *out = vadd(k[a].v, vmul(vsub(k[b].v, k[a].v), f));
    return true;
}

/* VxMath Slerp (0x2429c8f0), not normalized */
static bool eval_rot(const CkRotKey *k, uint32_t n, float t, float q[4])
{
    uint32_t a, b;
    float f;
    if (!find_segment((const float *)k, n, 5, t, &a, &b, &f)) return false;
    const float *q0 = k[a].q, *q1 = k[b].q;
    float d = q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3], sign = 1;
    if (d < 0) d = -d, sign = -1;
    float k0, k1;
    if (1 - d > 0.01f) {
        float th = acosf(d), s = sinf(th);
        k0 = sinf((1 - f) * th) / s, k1 = sign * sinf(f * th) / s;
    } else {
        k0 = 1 - f, k1 = sign * f;
    }
    for (int i = 0; i < 4; i++) q[i] = k0 * q0[i] + k1 * q1[i];
    return true;
}

/* VxQuaternion::ToMatrix (0x242836e0): rows are the entity axes */
static void quat_to_rows(const float q[4], float m[3][3])
{
    float x = q[0], y = q[1], z = q[2], w = q[3], n = x * x + y * y + z * z + w * w, s = n > 0 ? 2 / n : 0;
    m[0][0] = 1 - s * (y * y + z * z), m[0][1] = s * (x * y - w * z), m[0][2] = s * (x * z + w * y);
    m[1][0] = s * (x * y + w * z), m[1][1] = 1 - s * (x * x + z * z), m[1][2] = s * (y * z - w * x);
    m[2][0] = s * (x * z - w * y), m[2][1] = s * (y * z + w * x), m[2][2] = 1 - s * (x * x + y * y);
}

/* the inverse of quat_to_rows for an orthonormal matrix (VxQuaternion::FromMatrix) */
static void rows_to_quat(const float m[3][3], float q[4])
{
    float tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0) {
        float s = sqrtf(tr + 1) * 2;
        q[3] = 0.25f * s, q[0] = (m[2][1] - m[1][2]) / s, q[1] = (m[0][2] - m[2][0]) / s, q[2] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = sqrtf(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q[3] = (m[2][1] - m[1][2]) / s, q[0] = 0.25f * s, q[1] = (m[0][1] + m[1][0]) / s, q[2] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        float s = sqrtf(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q[3] = (m[0][2] - m[2][0]) / s, q[0] = (m[0][1] + m[1][0]) / s, q[1] = 0.25f * s, q[2] = (m[1][2] + m[2][1]) / s;
    } else {
        float s = sqrtf(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q[3] = (m[1][0] - m[0][1]) / s, q[0] = (m[0][2] + m[2][0]) / s, q[1] = (m[1][2] + m[2][1]) / s, q[2] = 0.25f * s;
    }
}

/* SetStep steps 3 and 5 (FUN_100578ca): the matrix m (the entity's current one) rebuilt from the keys at
   frame; pos += add when given (the keyed root offset). False when nothing is animated. */
static bool eval_matrix(const CkObjectAnimation *a, float frame, const CkVec3 *add, float w[4][4])
{
    CkVec3 pos, scale;
    float q[4];
    bool hp = !(a->flags & 4) && eval_vec(a->pos, a->npos, frame, &pos);
    bool hr = !(a->flags & 8) && eval_rot(a->rot, a->nrot, frame, q);
    bool hs = eval_vec(a->scale, a->nscale, frame, &scale);
    if (!hp && !hr && !hs) return false;
    if (hp && add) pos = vadd(pos, *add);
    if (hp && !hr && !hs) {
        w[3][0] = pos.x, w[3][1] = pos.y, w[3][2] = pos.z;
        return true;
    }
    /* the missing parts from the current matrix (FUN_1005695c) */
    float cur[3][3];
    float sc[3];
    for (int i = 0; i < 3; i++) {
        sc[i] = sqrtf(w[i][0] * w[i][0] + w[i][1] * w[i][1] + w[i][2] * w[i][2]);
        for (int j = 0; j < 3; j++) cur[i][j] = sc[i] > 0 ? w[i][j] / sc[i] : 0;
    }
    if (!hp) pos = v3(w[3][0], w[3][1], w[3][2]);
    if (!hs) scale = v3(sc[0], sc[1], sc[2]);
    if (!hr) rows_to_quat(cur, q);
    float r[3][3];
    quat_to_rows(q, r);
    float s[3] = {scale.x, scale.y, scale.z};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) w[i][j] = r[i][j] * s[i];
        w[i][3] = 0;
    }
    w[3][0] = pos.x, w[3][1] = pos.y, w[3][2] = pos.z, w[3][3] = 1;
    return true;
}

void ck_objanim_set_step(CkContext *ctx, CkObjectAnimation *a, float step)
{
    a->step = step;
    Ck3dEntity *e = ck_entity(ctx, a->entity);
    if (!e) return;
    eval_matrix(a, step * a->length, NULL, e->world);
}

/* affine row-vector matrices */
static void mat_mul(const float a[4][4], const float b[4][4], float out[4][4])
{
    float t[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(out, t, sizeof t);
}
static void mat_inv(const float m[4][4], float out[4][4])
{
    float a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], k = m[2][2];
    float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
    float inv[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    if (det != 0) {
        inv[0][0] = (e * k - f * h) / det, inv[0][1] = (c * h - b * k) / det, inv[0][2] = (b * f - c * e) / det;
        inv[1][0] = (f * g - d * k) / det, inv[1][1] = (a * k - c * g) / det, inv[1][2] = (c * d - a * f) / det;
        inv[2][0] = (d * h - e * g) / det, inv[2][1] = (b * g - a * h) / det, inv[2][2] = (a * e - b * d) / det;
    }
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) out[i][j] = inv[i][j];
        out[i][3] = 0;
    }
    for (int j = 0; j < 3; j++) out[3][j] = -(m[3][0] * inv[0][j] + m[3][1] * inv[1][j] + m[3][2] * inv[2][j]);
    out[3][3] = 1;
}

static uint32_t depth_of(const CkContext *ctx, CkId id)
{
    uint32_t d = 0;
    for (const Ck3dEntity *e = ck_entity(ctx, id); e && e->parent && d < 64; e = ck_entity(ctx, e->parent)) d++;
    return d;
}

void ck_keyed_set_step(CkContext *ctx, CkKeyedAnimation *k, float step)
{
    if (step > 1) step = 1;
    if (step < 0) step = 0;
    if (!k->linked) {
        /* load (FUN_10049e10): each object animation's saved vector becomes the keyed offset (the last wins) */
        for (uint32_t i = 0; i < k->anims.n; i++) {
            CkObjectAnimation *a = ck_objanim(ctx, k->anims.v[i]);
            if (a && (a->offset.x != 0 || a->offset.y != 0 || a->offset.z != 0)) k->offset = a->offset;
        }
        /* FUN_100499c1: without a root entity, the shallowest animated entity */
        if (!k->root_entity) {
            uint32_t best = UINT32_MAX;
            for (uint32_t i = 0; i < k->anims.n; i++) {
                CkObjectAnimation *a = ck_objanim(ctx, k->anims.v[i]);
                uint32_t d = a ? depth_of(ctx, a->entity) : UINT32_MAX;
                if (a && d < best) best = d, k->root_entity = a->entity;
            }
        }
        /* FUN_10048282: parents before children */
        for (uint32_t i = 1; i < k->anims.n; i++)
            for (uint32_t j = i; j > 0; j--) {
                CkObjectAnimation *x = ck_objanim(ctx, k->anims.v[j - 1]), *y = ck_objanim(ctx, k->anims.v[j]);
                if (!x || !y || depth_of(ctx, x->entity) <= depth_of(ctx, y->entity)) break;
                CkId t = k->anims.v[j - 1];
                k->anims.v[j - 1] = k->anims.v[j], k->anims.v[j] = t;
            }
        k->linked = true;
    }
    for (uint32_t i = 0; i < k->anims.n; i++) {
        CkObjectAnimation *a = ck_objanim(ctx, k->anims.v[i]);
        Ck3dEntity *e = a ? ck_entity(ctx, a->entity) : NULL;
        if (!a) continue;
        a->step = step;
        if (!e) continue;
        Ck3dEntity *p = ck_entity(ctx, e->parent);
        float local[4][4], inv[4][4];
        if (p) {
            mat_inv(p->world, inv);
            mat_mul(e->world, inv, local);
        } else memcpy(local, e->world, sizeof local);
        if (!eval_matrix(a, step * a->length, a->entity == k->root_entity ? &k->offset : NULL, local)) continue;
        if (p) mat_mul(local, p->world, local);
        ck_entity_set_world(ctx, e, local, false);
    }
    k->step = step;
}
